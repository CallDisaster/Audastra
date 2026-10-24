#pragma once

// 包含在TerrainWaterEditServiceTests.cpp的匿名namespace末尾，复用其原创Fixture。
// 预期WPM是字面浅/深水与干地生成位，不用待测生成器制造比较值。
void InstallRemoveWaterWetSource(Fixture& fixture, bool deep) {
    for (auto& vertex : fixture.grid.vertices) {
        vertex.textureAndFlagsRaw |= 0x40;
        vertex.waterHeightAndFlagsRaw = static_cast<std::uint16_t>(8192 + (deep ? 200 : 80) * 4);
    }
    RefreshTerrainDerived(fixture.grid, 0, 0, 33, 33);
    fixture.members[0].data = Take(SerializeMapTerrain(fixture.grid));
    for (std::size_t index = 16; index < fixture.members[1].data.size(); ++index)
        fixture.members[1].data[index] = (fixture.members[1].data[index] & std::byte{0x11})
            | (deep ? std::byte{0x0a} : std::byte{0x08});
    fixture.source = std::make_unique<MapArchiveSession>(Take(MapArchiveSession::Open(
        Take(WriteVerifiedArchive(fixture.root / "remove-wet-source.w3x", fixture.members)).path)));
    fixture.edits = Session(fixture);
}

void RemoveWaterReplayAndNativeSaveKeepOldContributionsBound() {
    for (const bool deep : {false, true}) {
        Fixture fixture(true, true); InstallRemoveWaterWetSource(fixture, deep);
        const auto sourceSha = fixture.source->SourceSha256();
        const auto originalTerrain = fixture.members[0].data, originalPathing = fixture.members[1].data;
        const auto originalDoodads = Take(fixture.source->ReadMember("war3map.doo"));
        const auto originalUnits = Take(fixture.source->ReadMember("war3mapUnits.doo"));
        TerrainStrokeIntent restriction; restriction.brush.kind = TerrainBrushKind::PathingRestriction;
        restriction.brush.radius = 0; restriction.brush.pathingRestrictions = 0x04;
        restriction.points = {{48, 48}};
        TerrainStrokeIntent removal; removal.brush.kind = TerrainBrushKind::RemoveWater;
        removal.brush.shape = TerrainBrushShape::Square; removal.brush.radius = 16;
        removal.points = {fixture.point}; removal.followDoodads = true;
        const std::array intents{restriction, removal};
        auto replayed = Take(ReplayFixture(fixture, *fixture.source, intents));
        Require(replayed.waterOwners.empty() && replayed.receipts.size() == 2,
            "remove-water reuses ordinary replay without new water owners or special-deletion proof");
        fixture.edits = std::move(replayed.session);
        const auto dryTerrain = Take(SerializeMapTerrain(*fixture.edits->Terrain()));
        Require(std::none_of(fixture.edits->Terrain()->vertices.begin(), fixture.edits->Terrain()->vertices.end(),
            [](const auto& vertex) { return vertex.HasWater(); }), "the fixture removes the last water surface");
        const auto vertexStart = originalTerrain.size() - fixture.grid.vertices.size() * 7;
        for (std::size_t i = 0; i < dryTerrain.size(); ++i)
            Require(dryTerrain[i] == ((i >= vertexStart && (i - vertexStart) % 7 == 4)
                ? originalTerrain[i] & std::byte{0xbf} : originalTerrain[i]),
                "native save input changes only HasWater and retains original water/ground raw values");
        const auto placements = fixture.edits->Snapshot();
        Require(Take(placements.Doodads()->Serialize()) == originalDoodads
            && Take(placements.Units()->Serialize()) == originalUnits,
            "ordinary replay neither moves nor deletes ordinary/special placements or units");

        const auto journal = Take(TerrainPathingSave::CaptureJournal(*fixture.source, *fixture.edits));
        const auto encoded = Take(EncodeTerrainStrokeJournal(journal));
        Require(encoded[3] == std::byte{'1'}, "restriction and remove-water keep ordinary FTJ1");
        auto restored = Take(TerrainPathingSave::RestoreJournal(*fixture.source, MapDocument::FromMembers(fixture.members),
            originalTerrain, originalPathing, Take(DecodeTerrainStrokeJournal(encoded)), *fixture.edits, fixture.game));
        Require(restored.waterOwners.empty() && restored.receipts.size() == 2
            && Take(SerializeMapTerrain(*restored.session->Terrain())) == dryTerrain,
            "source-bound journal cold replay reproduces the exact dry candidate");

        auto expectedDryPathing = originalPathing;
        for (std::size_t index = 16; index < expectedDryPathing.size(); ++index)
            expectedDryPathing[index] = (expectedDryPathing[index] & std::byte{0x11}) | std::byte{0x40};
        // 原创special的全黑alpha0足迹只撤回unfloat；去水不删除special，也不能丢掉其贡献。
        for (const auto& special : placements.Doodads()->specialDoodads)
            for (unsigned y = 0; y < 4; ++y) for (unsigned x = 0; x < 4; ++x)
                expectedDryPathing[16 + std::size_t(special.position[1] * 4 + y) * 128
                    + special.position[0] * 4 + x] &= std::byte{0xbf};
        expectedDryPathing[16 + 128 + 1] |= std::byte{0x04};
        auto expectedWetPathing = originalPathing; expectedWetPathing[16 + 128 + 1] |= std::byte{0x04};
        const auto sessionIdentity = fixture.edits->SessionIdentity();
        for (unsigned phase = 0; phase < 3; ++phase) {
            const bool dry = phase != 1;
            if (phase == 1) Require(fixture.edits->Undo(), "saved removal remains undoable");
            if (phase == 2) Require(fixture.edits->Redo(), "saved water restoration remains redoable");
            const auto plan = Take(TerrainPathingSave::PrepareForSave(*fixture.source, *fixture.edits, fixture.game));
            Require(plan.SaveGate().Succeeded() && plan.RebuiltPathing(),
                "dry/wet saved history always reaches actual G0/G1 native WPM reconstruction");
            auto replacements = fixture.edits->Replacements();
            Require(plan.InstallPathingOutput(*fixture.edits, replacements).Succeeded()
                && plan.VerifyForSave(*fixture.edits, fixture.source->SourceSha256(), replacements).Succeeded(),
                "the full pathing output remains bound to the frozen terrain and old WPM");
            const auto written = Take(fixture.source->SaveCandidate(fixture.root / ("remove-phase-" + std::to_string(phase) + ".w3x"), replacements));
            auto readback = Take(MapArchiveSession::Open(written.path));
            Require(plan.VerifyReadback(readback).Succeeded()
                && Take(readback.ReadMember("war3map.w3e")) == (dry ? dryTerrain : originalTerrain)
                && Take(readback.ReadMember("war3map.wpm")) == (dry ? expectedDryPathing : expectedWetPathing),
                "fresh archive readback withdraws only the old water contribution, retaining APR/protection/special footprints");
            Require(Take(readback.ReadMember("war3map.doo")) == originalDoodads
                && Take(readback.ReadMember("war3mapUnits.doo")) == originalUnits,
                "real archive publication keeps the original ordinary/special and unit bytes");
            Require(plan.CommitSaved(*fixture.edits, MapDocument::FromArchive(readback)).Succeeded()
                && !fixture.edits->IsDirty() && fixture.edits->SessionIdentity() == sessionIdentity,
                "successful readback rebases the same live undo session");
            fixture.source = std::make_unique<MapArchiveSession>(std::move(readback));
        }
        Require(Take(Sha256File(fixture.root / "remove-wet-source.w3x")) == sourceSha,
            "all save candidates leave the wet source map unchanged");
    }
}

void RemoveWaterStillRequiresOldWaterRulesAndExplainedPathing() {
    for (const bool missingWaterRules : {true, false}) {
        Fixture fixture; InstallRemoveWaterWetSource(fixture, true);
        if (missingWaterRules) {
            std::erase_if(fixture.members, [](const auto& member) { return member.name == "TerrainArt/Water.slk"; });
        } else {
            fixture.members[1].data[16 + 17 * 128 + 17] |= std::byte{0x04};
        }
        fixture.source = std::make_unique<MapArchiveSession>(Take(MapArchiveSession::Open(
            Take(WriteVerifiedArchive(fixture.root / "remove-invalid-source.w3x", fixture.members)).path)));
        fixture.edits = Session(fixture);
        TerrainBrush brush; brush.kind = TerrainBrushKind::RemoveWater;
        brush.shape = TerrainBrushShape::Square; brush.radius = 16;
        Require(fixture.edits->EditTerrain(brush, std::span(&fixture.point, 1), false).Succeeded(),
            "a raw remove-water draft remains editable before native-save qualification");
        const auto candidate = Take(SerializeMapTerrain(*fixture.edits->Terrain()));
        const auto revision = fixture.edits->Revision();
        const auto plan = TerrainPathingSave::PrepareForSave(*fixture.source, *fixture.edits, fixture.game);
        Require((!plan.Succeeded() || (plan.value && !plan.value->SaveGate().Succeeded() && !plan.value->RebuiltPathing()))
            && fixture.edits->Revision() == revision
            && Take(SerializeMapTerrain(*fixture.edits->Terrain())) == candidate,
            "removing the final water does not waive original water-rule sources or unexplained old WPM bits");
    }
}

void RemoveWaterDirectSaveKeepsTheExistingScriptGate() {
    Fixture fixture; InstallRemoveWaterWetSource(fixture, true);
    auto request = fixture.Request();
    auto& removal = request.terrainStrokes.front(); removal.waterBrush.reset();
    removal.brush.kind = TerrainBrushKind::RemoveWater; removal.brush.shape = TerrainBrushShape::Square;
    removal.brush.radius = 16; removal.points = {{16, 16}};
    const auto result = SaveMapEditCandidate(request);
    Require(!result.Succeeded() && !result.value && HasCode(result, "SCRIPT_TOOLCHAIN_MISSING")
        && !HasCode(result, "SAVE_STROKE_KIND") && !std::filesystem::exists(request.targetCandidate),
        "public removal save passes shared terrain validation and retains the unchanged real script-compiler gate");
    Require(Take(Sha256File(fixture.source->SourcePath())) == fixture.source->SourceSha256(),
        "unavailable script tooling cannot publish or modify the removal source");
}
