#pragma once

// 在TerrainOrdinaryDoodadSaveCases.h之后由同一私有namespace包含；复用原创
// special水刷、非对称普通占地和正式MapSaveService回读，不另造保存替身。

constexpr std::string_view kOrdinarySpecialTexture = "PathTextures/synthetic-ordinary-special.tga";

void OrdinarySpecialWaterFixture(Fixture& f) {
    SpecialWaterDeleteFixture(f);
    f.Member("Doodads/Doodads.slk") = Slk({"doodID", "code", "pathTex", "category", "fixedRot"},
        {{"Doo0", "Doo0", std::string(kUniformDoodadTexture), "C", "45"},
         {"Doo1", "Doo1", std::string(kUniformDoodadTexture), "C", "45"},
         {"Doo2", "Doo2", std::string(kUniformDoodadTexture), "C", "45"},
         {"Ord0", "Ord0", std::string(kOrdinarySpecialTexture), "E", "-1"}});
    Bytes texture(18); texture[2] = std::byte{2}; texture[12] = std::byte{3}; texture[14] = std::byte{2};
    texture[16] = std::byte{24}; texture[17] = std::byte{32};
    constexpr std::array<std::uint8_t, 18> pixels{0,0,0, 1,0,0, 0,1,0, 0,0,1, 1,1,0, 1,0,1};
    for (const auto pixel : pixels) texture.push_back(static_cast<std::byte>(pixel));
    f.members.push_back({std::string(kOrdinarySpecialTexture), std::move(texture)});
    auto placed = Take(ParseDoodadPlacements(f.Member("war3map.doo")));
    Require(placed.doodads.size() == 1 && placed.specialDoodads.size() == 3,
        "mixed fixture has one real ordinary footprint and three ordered special records");
    placed.doodads.front().rotation = 0;
    f.Member("war3map.doo") = Take(placed.Serialize());
    AddOrdinaryPatternExpected(f.Member("war3map.wpm"), 16, 15);
    f.Open();
}

Bytes OrdinarySpecialExpectedPathing(Fixture& f, bool water, bool moved) {
    auto expected = f.Member("war3map.wpm");
    // 普通物旧位置不与任何special或本笔水区重叠；先恢复已知干地51再投影。
    // 两个位置与非对称2x3矩阵均为独立字面预期，不调用待测G0/G1。
    for (unsigned y = 15; y < 18; ++y) for (unsigned x = 16; x < 18; ++x)
        expected[16 + y * 32 + x] = std::byte{0x51};
    AddOrdinaryPatternExpected(expected, moved ? 20 : 16, 15);
    if (water) {
        // source0被真实水刷删除；两个远区special仍按原四列贡献保留。
        for (unsigned y = 4; y < 12; ++y) for (unsigned x = 4; x < 12; ++x)
            expected[16 + y * 32 + x] = std::byte{0xdb};
        for (unsigned y = 6; y < 10; ++y) for (unsigned x = 6; x < 10; ++x)
            expected[16 + y * 32 + x] = std::byte{0x1b};
    }
    return expected;
}

void MoveOrdinarySpecialInstance(Fixture& f) {
    auto placed = OrdinaryCurrent(f);
    Require(placed.doodads.size() == 1, "move targets the sole ordinary instance");
    placed.doodads.front().position[0] = 640;
    Check(f.edits->EditDoodads(placed));
}

void OrdinarySpecialMutationsUseBothStagesAndSavedHistory() {
    for (const bool moveFirst : {false, true}) {
        Fixture f; OrdinarySpecialWaterFixture(f);
        const auto sourceSha = f.archive->SourceSha256();
        const auto sourcePath = f.archive->SourcePath();
        const auto originalTerrain = f.Member("war3map.w3e");
        const auto original = OrdinaryCurrent(f);
        auto moved = original; moved.doodads.front().position[0] = 640;
        auto deleted = original;
        deleted.specialDoodads = {original.specialDoodads[2], original.specialDoodads[1]};
        auto combined = moved; combined.specialDoodads = deleted.specialDoodads;
        const auto originalWire = Take(original.Serialize()), movedWire = Take(moved.Serialize());
        const auto deletedWire = Take(deleted.Serialize()), combinedWire = Take(combined.Serialize());
        if (moveFirst) MoveOrdinarySpecialInstance(f);
        // 复用相同typed原生水刷服务；此helper不要求地图中存在桥。
        PaintBridgeSpecialWater(f);
        const auto waterTerrain = Take(SerializeMapTerrain(*f.edits->Terrain()));
        if (!moveFirst) MoveOrdinarySpecialInstance(f);
        Require(Take(OrdinaryCurrent(f).Serialize()) == combinedWire && f.edits->Revision() == 2,
            "both operation orders retain full ordinary nested fields and the trusted swap-last special order");
        Require(f.edits->Terrain()->vertices[2 * 9 + 2].HasWater()
            && f.edits->Terrain()->vertices[2 * 9 + 2].CliffLevel() == 1,
            "mixed qualification belongs to an actual structural water command");
        Require(!f.edits->VerifyTerrainPathingForSave().Succeeded(),
            "the public invariant gate is not expanded to authorize mixed static contributions");
        const auto journal = f.edits->ExportTerrainStrokeJournal();
        Require(!journal.Succeeded() && HasCode(journal, "TERRAIN_JOURNAL_HISTORY_UNEXPRESSIBLE"),
            "ordinary XY history remains a full raw project draft rather than a fabricated terrain-only replay");
        f.SaveExpected("ordinary-special-final.w3x", waterTerrain,
            OrdinarySpecialExpectedPathing(f, true, true), &combinedWire, true);
        Require(f.edits->Undo(), "first mixed command remains undoable across native save");
        const bool intermediateWater = !moveFirst, intermediateMoved = moveFirst;
        const auto& intermediateWire = moveFirst ? movedWire : deletedWire;
        Require(Take(OrdinaryCurrent(f).Serialize()) == intermediateWire,
            "first Undo independently reverses only the most recent ordinary or trusted special command");
        f.SaveExpected("ordinary-special-undo-one.w3x", intermediateWater ? waterTerrain : originalTerrain,
            OrdinarySpecialExpectedPathing(f, intermediateWater, intermediateMoved), &intermediateWire, true);
        Require(f.edits->Undo(), "preceding mixed command remains undoable after rebasing its later endpoint");
        Require(Take(OrdinaryCurrent(f).Serialize()) == originalWire, "second Undo restores the original complete DOO");
        f.SaveExpected("ordinary-special-undo-both.w3x", originalTerrain,
            OrdinarySpecialExpectedPathing(f, false, false), &originalWire, true);
        Require(f.edits->Redo(), "first mixed command remains redoable after saving the original endpoint");
        f.SaveExpected("ordinary-special-redo-one.w3x", intermediateWater ? waterTerrain : originalTerrain,
            OrdinarySpecialExpectedPathing(f, intermediateWater, intermediateMoved), &intermediateWire, true);
        Require(f.edits->Redo(), "second mixed command remains redoable after a second native source rebase");
        f.SaveExpected("ordinary-special-redo-both.w3x", waterTerrain,
            OrdinarySpecialExpectedPathing(f, true, true), &combinedWire, true);
        Require(Take(Sha256File(sourcePath)) == sourceSha,
            "formal mixed saves never rewrite the original owned map source");
    }
}

void OrdinarySpecialProofKeepsPrivateSourcesAndFrozenOutput() {
    Fixture f; OrdinarySpecialWaterFixture(f);
    const auto original = OrdinaryCurrent(f);
    MoveOrdinarySpecialInstance(f); PaintBridgeSpecialWater(f);
    const auto combined = OrdinaryCurrent(f);
    const auto combinedWire = Take(combined.Serialize());
    const auto plan = f.Plan(); const auto output = f.Output(plan);
    auto valid = Take(MapArchiveSession::Open(Take(f.archive->SaveCandidate(f.root / "mixed-special-valid.w3x", output)).path));
    Check(plan.VerifyReadback(valid));
    for (unsigned change = 0; change < 3; ++change) {
        auto altered = combined;
        if (change == 0) altered.doodads = original.doodads;
        else if (change == 1) std::reverse(altered.specialDoodads.begin(), altered.specialDoodads.end());
        else altered.specialDoodads.front().variation += 1;
        const auto wire = Take(altered.Serialize());
        auto substituted = output;
        const auto member = std::find_if(substituted.begin(), substituted.end(),
            [](const auto& item) { return item.name == "war3map.doo"; });
        Require(member != substituted.end(), "final mixed output contains the complete changed DOO");
        member->data = wire;
        Require(!plan.VerifyForSave(*f.edits, f.archive->SourceSha256(), substituted).Succeeded(),
            "frozen output binds both ordinary pose and exact surviving special order/variation");
        const std::array<MemberReplacement, 1> replacement{{{"war3map.doo", wire}}};
        const auto path = f.root / ("mixed-special-output-drift-" + std::to_string(change) + ".w3x");
        const auto wrong = Take(MapArchiveSession::Open(Take(valid.SaveCandidate(path, replacement)).path));
        Require(!plan.VerifyReadback(wrong).Succeeded(),
            "independent readback cannot accept an altered ordinary or special endpoint");
        const auto revision = f.edits->Revision();
        Require(!plan.CommitSaved(*f.edits, MapDocument::FromArchive(wrong)).Succeeded()
            && f.edits->Revision() == revision && Take(OrdinaryCurrent(f).Serialize()) == combinedWire,
            "wrong readback cannot rebase or modify the current mixed transaction");
    }
    for (const auto name : {kOrdinarySpecialTexture, kUniformDoodadTexture, std::string_view("Doodads/Doodads.slk")}) {
        auto bytes = f.Member(name); bytes.push_back(std::byte{0});
        auto substituted = output; substituted.push_back({std::string(name), bytes});
        Require(!plan.VerifyForSave(*f.edits, f.archive->SourceSha256(), substituted).Succeeded(),
            "mixed qualification cannot change any actual ordinary/special texture or classification winner");
        const std::array<MemberReplacement, 1> replacement{{{std::string(name), bytes}}};
        const auto path = f.root / (name == kOrdinarySpecialTexture ? "mixed-ordinary-resource-drift.w3x"
            : name == kUniformDoodadTexture ? "mixed-special-resource-drift.w3x" : "mixed-classification-drift.w3x");
        const auto wrong = Take(MapArchiveSession::Open(Take(valid.SaveCandidate(path, replacement)).path));
        Require(!plan.VerifyReadback(wrong).Succeeded(), "readback independently checks both static source layers");
    }
    Require(!plan.VerifyForSave(*f.edits, std::string(64, '0'), output).Succeeded(),
        "whole-map source identity remains part of the native qualification");
    for (unsigned mutation = 0; mutation < 4; ++mutation) {
        auto changed = OrdinaryCurrent(f); changed.doodads.front().position[1] += 128;
        if (mutation == 0) changed.specialDoodads.pop_back();
        else if (mutation == 1) std::reverse(changed.specialDoodads.begin(), changed.specialDoodads.end());
        else if (mutation == 2) changed.specialDoodads.front().variation += 1;
        else changed.specialDoodads.push_back(original.specialDoodads.front());
        Check(f.edits->EditDoodads(changed));
        const auto refused = f.Plan().SaveGate();
        Require(!refused.Succeeded() && HasCode(refused, "TERRAIN_SPECIAL_PROVENANCE_REQUIRED"),
            "ordinary XY plus raw special deletion/order/variation/reinsertion never acquires private provenance");
        Require(!plan.VerifyForSave(*f.edits, f.archive->SourceSha256(), output).Succeeded(),
            "every additional command invalidates the already frozen complete mixed plan");
        Require(f.edits->Undo() && Take(OrdinaryCurrent(f).Serialize()) == combinedWire,
            "rejected raw special changes leave exact trusted mixed endpoint available through Undo");
        Check(f.Plan().SaveGate());
    }
    Fixture unexplained; OrdinarySpecialWaterFixture(unexplained);
    unexplained.Member("war3map.wpm")[16] ^= std::byte{4}; unexplained.Open();
    MoveOrdinarySpecialInstance(unexplained); PaintBridgeSpecialWater(unexplained);
    const auto refused = unexplained.Plan().SaveGate();
    Require(!refused.Succeeded() && HasCode(refused, "TERRAIN_PATHING_SOURCE_UNEXPLAINED"),
        "combined provenance never bypasses even one unexplained original WPM bit");
}

void OrdinarySpecialProofKeepsCompleteBridgeFreeze() {
    for (const bool moveFirst : {false, true}) {
        Fixture f; BridgeSpecialWaterFixture(f);
        const auto sourceSha = f.archive->SourceSha256();
        const auto moveNonBridge = [&] {
            auto placed = OrdinaryCurrent(f);
            const auto ordinary = std::find_if(placed.doodads.begin(), placed.doodads.end(),
                [](const auto& record) { return record.rawcode == "Ord0"; });
            Require(ordinary != placed.doodads.end(), "bridge fixture contains an unrelated nonbridge instance");
            ordinary->position[0] += 128; Check(f.edits->EditDoodads(placed));
        };
        if (moveFirst) moveNonBridge();
        PaintBridgeSpecialWater(f);
        if (!moveFirst) moveNonBridge();
        Require(OrdinaryCurrent(f).specialDoodads.size() == 2, "bridge refusal is tested after a real trusted deletion");
        const auto draft = Take(OrdinaryCurrent(f).Serialize());
        const auto refused = f.Plan().SaveGate();
        Require(!refused.Succeeded() && HasCode(refused, "TERRAIN_WALKABLE_DESTRUCTABLE_DOODADS_CHANGED"),
            "a bridge still freezes the entire ordinary wire including nonbridge siblings in either command order");
        Require(f.edits->CanUndo() && f.edits->IsDirty() && Take(OrdinaryCurrent(f).Serialize()) == draft
            && Take(Sha256File(f.archive->SourcePath())) == sourceSha,
            "bridge refusal preserves complete pending edits, history and original map bytes");
    }
}
