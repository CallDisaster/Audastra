#pragma once

// 由TerrainNativeObjectSaveTests.cpp在其私有namespace中包含，复用同一原创
// Fixture和正式MapSaveService回读/CommitSaved检查，不另建弱化的保存替身。

void AddOrdinaryPatternExpected(Bytes& wpm, unsigned x, unsigned y, bool rotated = false) {
    // 独立字面WPM矩阵：原TGA为(黑蓝绿/红青紫)，角0画2x3，角90画3x2。
    // x/y为已确认的文件格点；不调用待测投影或原版通行生成器造expected。
    constexpr std::array<unsigned, 6> at0{0, 0xc2, 8, 0x0c, 4, 0xca};
    constexpr std::array<unsigned, 6> at90{4, 8, 0, 0xca, 0x0c, 0xc2};
    const auto width = rotated ? 3u : 2u;
    const auto& masks = rotated ? at90 : at0;
    for (unsigned i = 0; i < masks.size(); ++i)
        wpm[16 + (y + i / width) * 32 + x + i % width] |= static_cast<std::byte>(masks[i]);
}

void OrdinaryMixedFixture(Fixture& f, bool empty = false, bool overlap = false) {
    StandardDoodad(f);
    f.Member("Doodads/Doodads.slk") = Slk({"doodID", "code", "pathTex", "category", "fixedRot"},
        {{"Doo0", "Doo0", std::string(kUniformDoodadTexture), "E", "-1"},
         {"NewD", "NewD", std::string(kUniformDoodadTexture), "E", "-1"},
         {"Spc0", "Spc0", "_", "C", "-1"}});
    Bytes texture(18); texture[2] = std::byte{2}; texture[12] = std::byte{3}; texture[14] = std::byte{2};
    texture[16] = std::byte{24}; texture[17] = std::byte{32};
    constexpr std::array<std::uint8_t, 18> pixels{0,0,0, 1,0,0, 0,1,0, 0,0,1, 1,1,0, 1,0,1};
    for (const auto pixel : pixels) texture.push_back(static_cast<std::byte>(pixel));
    f.members.push_back({std::string(kUniformDoodadTexture), std::move(texture)});
    auto placements = Take(ParseDoodadPlacements(f.Member("war3map.doo")));
    if (empty) placements.doodads.clear();
    else {
        placements.doodads.front().rotation = 0;
        PlacementItemSet drops; drops.items = {{"gold", 70}, {"lmbr", 30}};
        placements.doodads.front().itemSets.push_back(std::move(drops));
        if (overlap) {
            auto copied = placements.doodads.front(); copied.creationId = 73;
            placements.doodads.push_back(std::move(copied));
        }
        AddOrdinaryPatternExpected(f.Member("war3map.wpm"), 16, 15);
    }
    placements.specialDoodads = {{"Spc0", 3, {1, 1}}, {"Spc0", 7, {6, 6}}};
    f.Member("war3map.doo") = Take(placements.Serialize());
    f.Open();
}

DoodadPlacements OrdinaryCurrent(const Fixture& f) {
    const auto current = Take(f.edits->Snapshot(MapDataKind::Doodads, kBudget));
    Require(current.Doodads().has_value(), "bounded actual DOO snapshot exists");
    return *current.Doodads();
}

void OrdinaryDoodadMutationsUseBothSaveStages() {
    for (unsigned operation = 0; operation < 7; ++operation) {
        Fixture f; OrdinaryMixedFixture(f, false, operation == 6);
        const auto oldDoodads = f.Member("war3map.doo");
        const auto oldPlacements = OrdinaryCurrent(f);
        auto changed = oldPlacements;
        // 每一种修改前后都有完整普通实例和原字特殊尾，包含嵌套掉落表。
        switch (operation) {
        case 0: changed.doodads.front().position[0] = 640; break;
        case 1: changed.doodads.front().rotation = 1.57079637f; break;
        case 2: changed.doodads.front().scale = {2, 0.5f, 3}; break;
        case 3:
        case 4: {
            auto created = changed.doodads.front(); created.position = {256, 256, 37}; created.creationId = 91;
            if (operation == 4) { created.rawcode = "NewD"; created.itemSets.clear(); }
            changed.doodads.push_back(std::move(created)); break;
        }
        case 5: changed.doodads.clear(); break;
        case 6: changed.doodads.erase(changed.doodads.begin()); break;
        }
        const auto newDoodads = Take(changed.Serialize());
        constexpr std::size_t tailBytes = 8 + 2 * 16;
        Require(std::equal(oldDoodads.end() - tailBytes, oldDoodads.end(), newDoodads.end() - tailBytes),
            "fixture keeps every byte of the original special version/count/records");
        f.Paint(); Check(f.edits->EditDoodads(changed));
        auto expected = f.ExpectedPathing(true);
        if (operation == 0) AddOrdinaryPatternExpected(expected, 20, 15);
        else if (operation == 1) AddOrdinaryPatternExpected(expected, 15, 16, true);
        else if (operation != 5) AddOrdinaryPatternExpected(expected, 16, 15);
        if (operation == 3 || operation == 4) AddOrdinaryPatternExpected(expected, 8, 7);
        const auto expectedTerrain = f.ExpectedTerrain(true);
        f.SaveExpected("ordinary-mixed.w3x", expectedTerrain, expected, &newDoodads, true);
        Require(f.edits->Undo(), "ordinary mutation remains undoable after a formal native save");
        auto previous = f.ExpectedPathing(true); AddOrdinaryPatternExpected(previous, 16, 15);
        f.SaveExpected("ordinary-undone.w3x", expectedTerrain, previous, &oldDoodads, true);
        Require(f.edits->Redo(), "ordinary mutation remains redoable after rebasing the saved undo endpoint");
        f.SaveExpected("ordinary-redone.w3x", expectedTerrain, expected, &newDoodads, true);
    }
    // 旧普通段完全为空；新增类型只在After出现，不能跳过分类或复用旧空层。
    Fixture empty; OrdinaryMixedFixture(empty, true); empty.Paint();
    auto created = OrdinaryCurrent(empty);
    DoodadPlacement placed; placed.rawcode = "NewD"; placed.position = {256, 256, 37};
    placed.scale = {1, 1, 1}; placed.flags = 2; placed.creationId = 811;
    created.doodads.push_back(placed); Check(empty.edits->EditDoodads(created));
    const auto wire = Take(created.Serialize());
    auto expected = empty.ExpectedPathing(true); AddOrdinaryPatternExpected(expected, 8, 7);
    empty.SaveExpected("ordinary-created-from-empty.w3x", empty.ExpectedTerrain(true), expected, &wire, true);
}

void OrdinaryDoodadMoveSurvivesZeroNetTerrainAndHistory() {
    Fixture f; OrdinaryMixedFixture(f);
    const auto originalWire = f.Member("war3map.doo");
    auto moved = OrdinaryCurrent(f); moved.doodads.front().position[0] = 640;
    const auto movedWire = Take(moved.Serialize()); Check(f.edits->EditDoodads(moved));
    f.Paint(); Require(f.edits->Undo(), "Undo the last terrain brush while retaining the preceding ordinary move");
    Require(Take(SerializeMapTerrain(*f.edits->Terrain())) == f.ExpectedTerrain(false)
        && f.edits->IsDirty() && TerrainSavePlan::HasTerrainChanges(*f.edits),
        "zero-net terrain still enters the actual pathing save chain for a changed complete DOO");
    auto expected = f.ExpectedPathing(false); AddOrdinaryPatternExpected(expected, 20, 15);
    f.SaveExpected("ordinary-after-terrain-undo.w3x", f.ExpectedTerrain(false), expected, &movedWire, true);
    Require(f.edits->Undo(), "Undo the saved ordinary move after zero-net terrain save");
    f.SaveExpected("ordinary-zero-net-undone.w3x", f.ExpectedTerrain(false), f.Member("war3map.wpm"), &originalWire, true);
    Require(f.edits->Redo(), "Redo restores the precise ordinary move after the new baseline");
    f.SaveExpected("ordinary-zero-net-redone.w3x", f.ExpectedTerrain(false), expected, &movedWire, true);

    Fixture blight; OrdinaryMixedFixture(blight);
    TerrainBrush brush; brush.kind = TerrainBrushKind::Blight; brush.shape = TerrainBrushShape::Square; brush.radius = 8;
    Take(blight.edits->EditTerrain(brush, std::span(&blight.point, 1), false));
    auto relocated = OrdinaryCurrent(blight); relocated.doodads.front().position[0] = 640;
    Check(blight.edits->EditDoodads(relocated)); const auto relocatedWire = Take(relocated.Serialize());
    auto blightedTerrain = blight.original;
    for (auto& vertex : blightedTerrain.vertices) vertex.textureAndFlagsRaw |= 0x20;
    auto blightedPathing = blight.ExpectedPathing(false);
    for (auto it = blightedPathing.begin() + 16; it != blightedPathing.end(); ++it) *it |= std::byte{0x20};
    AddOrdinaryPatternExpected(blightedPathing, 20, 15);
    blight.SaveExpected("ordinary-mixed-blight.w3x", Take(SerializeMapTerrain(blightedTerrain)), blightedPathing, &relocatedWire, true);
}

void OrdinaryDoodadSaveKeepsActualDependenciesAndBudgets() {
    Fixture f; OrdinaryMixedFixture(f); f.Paint();
    auto moved = OrdinaryCurrent(f); moved.doodads.front().position[0] = 640;
    Check(f.edits->EditDoodads(moved)); const auto wire = Take(moved.Serialize());
    const auto owner = Take(TerrainPathingSave::CaptureNative(*f.archive, f.original, *f.edits->Terrain(),
        f.gameRoot, kBudget, &wire));
    const auto exact = owner.CaptureBudgetBytes();
    const auto bounded = Take(TerrainPathingSave::CaptureNative(*f.archive, f.original, *f.edits->Terrain(), f.gameRoot, exact, &wire));
    Require(bounded.CaptureBudgetBytes() == exact && !TerrainPathingSave::CaptureNative(*f.archive,
        f.original, *f.edits->Terrain(), f.gameRoot, exact - 1, &wire).Succeeded(),
        "both complete instance lists, both static planes and one shared decoded texture obey the exact owner budget");
    auto oldExpected = f.ExpectedPathing(false), newExpected = f.ExpectedPathing(true);
    AddOrdinaryPatternExpected(oldExpected, 16, 15); AddOrdinaryPatternExpected(newExpected, 20, 15);
    const auto before = Take(owner.GenerateNative(f.original, TerrainNativeStage::Before, kBudget));
    const auto after = Take(owner.GenerateNative(*f.edits->Terrain(), TerrainNativeStage::After, kBudget));
    for (std::size_t i = 0; i < before.size(); ++i) {
        Require(before[i] == (oldExpected[16 + i] & std::byte{0xee}), "G0 independently explains the complete old WPM");
        Require(after[i] == (newExpected[16 + i] & std::byte{0xee}), "G1 independently matches the complete final DOO projection");
    }
    Require(!owner.GenerateNative(f.original, kBudget).Succeeded(), "changed ordinary DOO requires explicit stage selection");
    const auto plan = f.Plan(); const auto output = f.Output(plan);
    auto substituted = output;
    auto doo = std::find_if(substituted.begin(), substituted.end(), [](const auto& value) { return value.name == "war3map.doo"; });
    Require(doo != substituted.end(), "mixed output carries a whole DOO replacement");
    doo->data = f.Member("war3map.doo");
    Require(!plan.VerifyForSave(*f.edits, f.archive->SourceSha256(), substituted).Succeeded(),
        "the frozen final output cannot restore the old ordinary pose");
    const auto saved = Take(MapArchiveSession::Open(Take(f.archive->SaveCandidate(f.root / "ordinary-frozen.w3x", output)).path));
    Check(plan.VerifyReadback(saved));
    const std::array<MemberReplacement, 1> replacement{{{"war3map.doo", f.Member("war3map.doo")}}};
    auto changed = Take(MapArchiveSession::Open(Take(saved.SaveCandidate(f.root / "ordinary-readback-drift.w3x", replacement)).path));
    Require(!plan.VerifyReadback(changed).Succeeded(), "readback independently binds the exact final ordinary wire");
    auto late = output; late.push_back({std::string(kUniformDoodadTexture), Text("different source")});
    Require(!plan.VerifyForSave(*f.edits, f.archive->SourceSha256(), late).Succeeded(),
        "same-name texture replacement cannot inherit the captured winner");
    Check(f.edits->EditDoodads(OrdinaryCurrent(f))); // equal input is not a new command
    moved.doodads.front().position[1] += 128; Check(f.edits->EditDoodads(moved));
    Require(!plan.VerifyForSave(*f.edits, f.archive->SourceSha256(), output).Succeeded(), "session revision drift rejects old native qualification");

    Fixture unexplained; OrdinaryMixedFixture(unexplained);
    unexplained.Member("war3map.wpm")[16] |= std::byte{0x04}; unexplained.Open(); unexplained.Paint();
    auto changedPose = OrdinaryCurrent(unexplained); changedPose.doodads.front().position[0] = 640;
    Check(unexplained.edits->EditDoodads(changedPose));
    Require(HasCode(unexplained.Plan().SaveGate(), "TERRAIN_PATHING_SOURCE_UNEXPLAINED"),
        "new ordinary mutation support keeps full old-WPM explanation mandatory");
    TerrainSavePlanLimits low; low.maximumBytes = 4096;
    const auto refused = TerrainPathingSave::PrepareForSave(*f.archive, *f.edits, f.gameRoot, low);
    Require(!refused.Succeeded() || !refused.value || !refused.value->SaveGate().Succeeded(),
        "tight whole-plan budget rejects instead of returning partial two-stage pathing");
}

void OrdinaryMutationsDoNotAcquireSpecialOrBridgeAuthority() {
    for (unsigned mutation = 0; mutation < 3; ++mutation) {
        Fixture f; OrdinaryMixedFixture(f); f.Paint();
        auto changed = OrdinaryCurrent(f); changed.doodads.front().position[0] += 128;
        if (mutation == 0) changed.specialDoodads.pop_back();
        else if (mutation == 1) std::reverse(changed.specialDoodads.begin(), changed.specialDoodads.end());
        else changed.specialDoodads.front().variation += 1;
        Check(f.edits->EditDoodads(changed));
        Require(!f.Plan().SaveGate().Succeeded(), "ordinary proof never authorizes raw special deletion, order or variation changes");
        Require(f.edits->IsDirty() && f.edits->CanUndo(), "special refusal preserves pending edits and Undo");
    }
    // 复用已有原创桥来源；桥记录不动，无关普通物的Z变化仍须完整保存读回。
    Fixture bridge; WalkableBridge(bridge); bridge.Open(); bridge.Paint();
    auto changed = OrdinaryCurrent(bridge); changed.doodads.back().position[2] += 128;
    Check(bridge.edits->EditDoodads(changed));
    const auto wire = Take(changed.Serialize());
    bridge.SaveExpected("ordinary-z-alongside-fixed-bridge.w3x", bridge.ExpectedTerrain(true),
        BridgeExpectedPathing(bridge, true, false), &wire, true);
}

void OrdinaryMoveThenNativeWaterUsesTheCurrentDoodadOwner() {
    Fixture f; OrdinaryMixedFixture(f);
    for (auto& vertex : f.original.vertices) vertex.textureAndFlagsRaw = 0;
    RefreshTerrainDerived(f.original, 0, 0, 9, 9);
    f.Member("war3map.w3e") = Take(SerializeMapTerrain(f.original));
    std::fill(f.Member("war3map.wpm").begin() + 16, f.Member("war3map.wpm").end(), std::byte{0x51});
    AddOrdinaryPatternExpected(f.Member("war3map.wpm"), 16, 15);
    f.Member("UI/MiscData.txt") = Text("[Terrain]\nMaxSlope=90\nMinHeight=-256\nMaxHeight=1536\n"
        "[Water]\nDeepLevelPathing=64\n[CameraMargins]\nLeft=0\nRight=0\nBottom=0\nTop=0\n");
    f.members.push_back({"TerrainArt/CliffTypes.slk", Slk({"cliffID"}, {{"Cl01"}})});
    f.gameRoot = f.root / "water-game"; Require(std::filesystem::create_directory(f.gameRoot), "owned missing-profile game directory");
    const std::array<MapArchiveMember, 1> gameMembers{{{"marker.txt", Text("original fixture")}}};
    Take(WriteVerifiedArchive(f.gameRoot / "war3.mpq", gameMembers)); f.Open();
    auto moved = OrdinaryCurrent(f); moved.doodads.front().position = {896, 512, 0};
    Check(f.edits->EditDoodads(moved)); const auto movedWire = Take(moved.Serialize());
    const auto rules = Take(TerrainPathingSave::CaptureWaterBrushRules(*f.archive, f.original,
        *f.edits->Terrain(), moved, f.gameRoot, true, kBudget));
    const Services::MapDataQueryBinding document{"ordinary-native-water", 1, 0, f.archive->SourceSha256()};
    const Services::TerrainEditBinding binding{document, f.edits->SessionIdentity(), f.edits->Revision(), f.edits->TerrainSourceRevision()};
    TerrainWaterStrokeOptions options; options.shape = TerrainBrushShape::Square; options.randomSeed = 1;
    const TerrainStrokePoint point{256, 256};
    const auto prepared = Take(Services::PrepareTerrainWaterEdit(*f.edits, document, binding, rules,
        options, std::span(&point, 1), false));
    Take(Services::ApplyTerrainEdit(*f.edits, document, prepared));
    Require(Take(OrdinaryCurrent(f).Serialize()) == movedWire, "native water preserves all ordinary bytes and the original zero-footprint special tail");
    auto expected = f.Member("war3map.wpm");
    std::fill(expected.begin() + 16, expected.end(), std::byte{0x51});
    AddOrdinaryPatternExpected(expected, 28, 15);
    for (unsigned y = 4; y < 12; ++y) for (unsigned x = 4; x < 12; ++x) expected[16 + y * 32 + x] = std::byte{0xdb};
    for (unsigned y = 6; y < 10; ++y) for (unsigned x = 6; x < 10; ++x) expected[16 + y * 32 + x] = std::byte{0x1b};
    const auto terrain = Take(SerializeMapTerrain(*f.edits->Terrain()));
    const auto journal = f.edits->ExportTerrainStrokeJournal();
    Require(journal.Succeeded() && journal.value && journal.value->strokes.size() == 2
        && journal.value->strokes[0].frozenDoodads && journal.value->strokes[1].intent.waterBrush
        && f.edits->IsDirty() && Take(OrdinaryCurrent(f).Serialize()) == movedWire,
        "ordinary edit keeps its frozen full command before the water step rather than impersonating a terrain brush");
    f.SaveExpected("ordinary-move-before-native-water.w3x", terrain, expected, &movedWire, true);
}
