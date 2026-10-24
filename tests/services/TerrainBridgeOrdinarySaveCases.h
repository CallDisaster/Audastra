#pragma once

// 在 TerrainOrdinaryDoodadSaveCases.h / TerrainOrdinarySpecialSaveCases.h 后包含；
// 复用同一正式保存、完整归档读回及 CommitSaved/Undo/Redo 夹具。
void BridgeOrdinaryFixture(Fixture& f) {
    WalkableBridge(f, false, true);
    f.Member("Doodads/Doodads.slk") = Slk({"doodID", "code", "pathTex", "category", "fixedRot"},
        {{"Bl01", "Bl01", std::string(kBridgeBlockerTexture), "E", "-1"},
         {"NewD", "NewD", std::string(kBridgeBlockerTexture), "E", "-1"}});
    auto placements = Take(ParseDoodadPlacements(f.Member("war3map.doo")));
    for (auto& row : placements.doodads) if (row.rawcode == "Br01") {
        row.droppedItemTable = 17;
        PlacementItemSet drops; drops.items = {{"gold", 70}, {"lmbr", 30}};
        row.itemSets.push_back(std::move(drops));
    }
    f.Member("war3map.doo") = Take(placements.Serialize()); f.Open();
}

Bytes BridgeOrdinaryExpectedPathing(Fixture& f, bool deep, unsigned operation = 8) {
    auto expected = f.ExpectedPathing(deep);
    // 两桥原字固定：覆盖x16..19/y14..17，抑制terrain但不输出普通RGB位。
    for (unsigned y = 14; y < 18; ++y) for (unsigned x = 16; x < 20; ++x)
        expected[16 + y * 32 + x] = std::byte{0x11};
    const auto green = [&](unsigned start) {
        for (unsigned y = 14; y < 18; ++y) for (unsigned x = start; x < start + 4; ++x)
            expected[16 + y * 32 + x] |= std::byte{4};
    };
    if (operation != 4) green(operation == 0 ? 18 : 14);
    if (operation == 3) green(6);
    return expected;
}

void BridgeAllowsOnlyUnrelatedOrdinaryMutationsThroughFormalHistory() {
    for (unsigned operation = 0; operation < 8; ++operation) {
        Fixture f; BridgeOrdinaryFixture(f);
        const auto originalWire = f.Member("war3map.doo");
        auto changed = OrdinaryCurrent(f);
        auto& ordinary = changed.doodads[1];
        switch (operation) {
        case 0: ordinary.position[0] = 640; break;
        case 1: ordinary.position[2] = 64; break;
        case 2: ordinary.scale = {2, 0.5f, 3}; break;
        case 3: {
            auto created = ordinary; created.position = {256, 512, 37}; created.creationId = 101;
            changed.doodads.insert(changed.doodads.begin(), std::move(created)); break;
        }
        case 4: changed.doodads.erase(changed.doodads.begin() + 1); break;
        case 5: std::swap(changed.doodads[0], changed.doodads[1]); break;
        case 6: ordinary.rawcode = "NewD"; break;
        case 7: {
            ordinary.droppedItemTable = 23;
            PlacementItemSet drops; drops.items = {{"drop", 100}};
            ordinary.itemSets.push_back(std::move(drops)); break;
        }
        }
        const auto finalWire = Take(changed.Serialize());
        f.Paint(); Check(f.edits->EditDoodads(changed));
        const auto owner = Take(TerrainPathingSave::CaptureNative(*f.archive, f.original,
            *f.edits->Terrain(), f.gameRoot, kBudget, &finalWire));
        const auto g0 = Take(owner.GenerateNative(f.original, TerrainNativeStage::Before, kBudget));
        const auto g1 = Take(owner.GenerateNative(*f.edits->Terrain(), TerrainNativeStage::After, kBudget));
        const auto expectedBefore = BridgeOrdinaryExpectedPathing(f, false);
        const auto expectedAfter = BridgeOrdinaryExpectedPathing(f, true, operation);
        for (std::size_t i = 16; i < expectedAfter.size(); ++i) {
            Require(g0[i - 16] == (expectedBefore[i] & std::byte{0xee}),
                "complete G0 retains original bridge suppression and old ordinary contribution");
            Require(g1[i - 16] == (expectedAfter[i] & std::byte{0xee}),
                "complete G1 independently reflects final ordinary records around frozen bridge records");
        }
        f.SaveExpected("bridge-ordinary-final.w3x", f.ExpectedTerrain(true), expectedAfter, &finalWire, true);
        Require(f.edits->Undo(), "unrelated ordinary mutation remains undoable after bridge native save");
        // 首次保存已建立地形基线；仅普通物Z的Undo/Redo精确保留原WPM。
        // 其它字段即使产生相同WPM，也必须重新完成G0/G1证明。
        f.SaveExpected("bridge-ordinary-undone.w3x", f.ExpectedTerrain(true),
            BridgeOrdinaryExpectedPathing(f, true), &originalWire, true, operation != 1);
        Require(f.edits->Redo(), "unrelated ordinary mutation remains redoable after native source rebase");
        f.SaveExpected("bridge-ordinary-redone.w3x", f.ExpectedTerrain(true), expectedAfter, &finalWire, true, operation != 1);
    }
    // ROC没有掉落字段；同一writer长度投影仍须精确定位42字节记录。
    Fixture roc; WalkableBridge(roc);
    auto source = Take(ParseDoodadPlacements(roc.Member("war3map.doo")));
    source.version = 7; source.subversion = 9;
    roc.Member("war3map.doo") = Take(source.Serialize()); roc.Open(); roc.Paint();
    auto moved = OrdinaryCurrent(roc); moved.doodads[1].position[0] = 640;
    Check(roc.edits->EditDoodads(moved)); const auto movedWire = Take(moved.Serialize());
    roc.SaveExpected("bridge-ordinary-roc.w3x", roc.ExpectedTerrain(true),
        BridgeOrdinaryExpectedPathing(roc, true, 0), &movedWire, true);
}

void BridgeRecordBytesOrderAndClassificationRemainMandatory() {
    for (unsigned mutation = 0; mutation < 21; ++mutation) {
        Fixture f; BridgeOrdinaryFixture(f); f.Paint();
        const auto sourceSha = f.archive->SourceSha256();
        auto changed = OrdinaryCurrent(f); auto& bridge = changed.doodads.front();
        switch (mutation) {
        case 0: bridge.position[0] += 1; break;
        case 1: bridge.position[1] += 1; break;
        case 2: bridge.position[2] = 16; break;
        case 3: bridge.rotation = -0.0f; break;
        case 4: bridge.scale[0] = 2; break;
        case 5: bridge.variation += 1; break;
        case 6: bridge.droppedItemTable += 1; break;
        case 7: bridge.itemSets.front().items.front().chance += 1; break;
        case 8: bridge.itemSets.front().items.front().rawcode = "drop"; break;
        case 9: bridge.creationId += 100; break;
        case 10: std::swap(changed.doodads[0], changed.doodads[2]); break;
        case 11: changed.doodads.erase(changed.doodads.begin()); break;
        case 12: {
            auto created = bridge; created.creationId = 102; changed.doodads.push_back(std::move(created)); break;
        }
        case 13: bridge.rawcode = "Bl01"; break;
        case 14: changed.doodads[1].rawcode = "Br01"; break;
        case 15: changed.doodads[1].rawcode = "Gone"; break;
        case 16: bridge.itemSets.emplace_back(); break;
        case 17: bridge.itemSets.front().items.pop_back(); break;
        case 18: std::reverse(bridge.itemSets.front().items.begin(), bridge.itemSets.front().items.end()); break;
        case 19: bridge.flags ^= 1; break;
        case 20: bridge.lifePercent = 99; break;
        }
        Check(f.edits->EditDoodads(changed));
        const auto wire = Take(changed.Serialize()); const auto gate = f.Plan().SaveGate();
        Require(!gate.Succeeded(), "bridge record mutation or unknown ordinary classification cannot acquire a save gate");
        if (mutation != 15) Require(HasCode(gate, "TERRAIN_WALKABLE_DESTRUCTABLE_DOODADS_CHANGED"),
            "bridge complete fields, insert/delete/type changes and relative order stay frozen");
        Require(f.edits->IsDirty() && f.edits->CanUndo() && Take(OrdinaryCurrent(f).Serialize()) == wire
            && Take(Sha256File(f.archive->SourcePath())) == sourceSha,
            "refusal preserves pending DOO, history and the actual original archive");
    }
}

void BridgeOrdinaryFrozenOutputSourcesAndBudgetRemainBound() {
    Fixture f; BridgeOrdinaryFixture(f); f.Paint();
    auto changed = OrdinaryCurrent(f); changed.doodads[1].position[0] = 640;
    Check(f.edits->EditDoodads(changed)); const auto wire = Take(changed.Serialize());
    const auto owner = Take(TerrainPathingSave::CaptureNative(*f.archive, f.original,
        *f.edits->Terrain(), f.gameRoot, kBudget, &wire));
    Take(TerrainPathingSave::CaptureNative(*f.archive, f.original, *f.edits->Terrain(),
        f.gameRoot, owner.CaptureBudgetBytes(), &wire));
    Require(!TerrainPathingSave::CaptureNative(*f.archive, f.original, *f.edits->Terrain(),
        f.gameRoot, owner.CaptureBudgetBytes() - 1, &wire).Succeeded(),
        "mixed ordinary bridge capture still requires its exact cumulative resource budget");
    const auto plan = f.Plan(); const auto output = f.Output(plan);
    auto substituted = output;
    const auto doo = std::find_if(substituted.begin(), substituted.end(),
        [](const auto& member) { return member.name == "war3map.doo"; });
    Require(doo != substituted.end(), "mixed bridge save carries its complete final DOO");
    doo->data = f.Member("war3map.doo");
    Require(!plan.VerifyForSave(*f.edits, f.archive->SourceSha256(), substituted).Succeeded(),
        "a later output cannot silently restore unrelated ordinary fields after qualification");
    auto drift = output; auto texture = f.Member(kBridgeTexture); texture[21] ^= std::byte{1};
    drift.push_back({std::string(kBridgeTexture), std::move(texture)});
    Require(!plan.VerifyForSave(*f.edits, f.archive->SourceSha256(), drift).Succeeded(),
        "unrelated ordinary support never releases bridge resource identity");
    const auto saved = Take(MapArchiveSession::Open(Take(f.archive->SaveCandidate(f.root / "bridge-ordinary-frozen.w3x", output)).path));
    Check(plan.VerifyReadback(saved));
    const std::array<MemberReplacement, 1> replacement{{{"war3map.doo", f.Member("war3map.doo")}}};
    const auto wrong = Take(MapArchiveSession::Open(Take(saved.SaveCandidate(f.root / "bridge-ordinary-drift.w3x", replacement)).path));
    Require(!plan.VerifyReadback(wrong).Succeeded(), "readback independently binds every final ordinary and bridge byte");
}
