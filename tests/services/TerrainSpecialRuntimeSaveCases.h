#pragma once

// 在TerrainOrdinaryDoodadSaveCases.h之后由同一私有namespace包含；
// 真实类型/贴图捕获、typed命令、正式保存与回读均复用产品服务。

void MixedSpecialRuntimeFixture(Fixture& f, bool duplicateActive) {
    SpecialWaterDeleteFixture(f);
    f.Member("Doodads/Doodads.slk") = Slk({"doodID", "code", "pathTex", "category", "fixedRot"},
        {{"Doo0", "Doo0", std::string(kUniformDoodadTexture), "C", "45"},
         {"Doo1", "Doo1", std::string(kUniformDoodadTexture), "C", "45"},
         {"Doo2", "Doo2", std::string(kUniformDoodadTexture), "C", "45"},
         {"Doo3", "Doo3", std::string(kUniformDoodadTexture), "C", "45"},
         {"Emp0", "Emp0", "_", "C", "45"},
         {"Emp1", "Emp1", "none", "C", "45"},
         {"Emp2", "Emp2", "", "C", "45"},
         {"Ord0", "Ord0", "_", "E", "-1"}});
    auto placed = Take(ParseDoodadPlacements(f.Member("war3map.doo")));
    const auto active = placed.specialDoodads;
    Require(active.size() == 3, "original mixed fixture has three known active special records");
    // 空项与将删active具有相同XY，且分布在运行时原始记录前、中、后。
    placed.specialDoodads = {{"Emp0", 37, {2, 2}}, active[0], {"Emp1", -91, {2, 2}}, active[1], active[2]};
    if (duplicateActive) placed.specialDoodads.push_back({"Doo3", 0, {2, 2}});
    placed.specialDoodads.push_back({"Emp2", 122, {2, 2}});
    f.Member("war3map.doo") = Take(placed.Serialize());
    // inactive从不写静态通行；同XY的新增active贴图相同，首覆盖亦不改变原字面WPM。
    f.Open();
}

void RequireMixedRuntimeCapture(const TerrainPathingResources& rules, bool afterDelete, bool duplicateActive) {
    const auto shapes = rules.WaterSpecialShapes();
    if (afterDelete) {
        Require(shapes.size() == 5
            && shapes[0].sourceRecordIndex == 0 && shapes[0].rawcode == "Emp0" && !shapes[0].hasFootprint
            && shapes[1].sourceRecordIndex == 1 && shapes[1].rawcode == "Doo2" && shapes[1].hasFootprint
            && shapes[2].sourceRecordIndex == 2 && shapes[2].rawcode == "Emp1" && !shapes[2].hasFootprint
            && shapes[3].sourceRecordIndex == 3 && shapes[3].rawcode == "Doo1" && shapes[3].hasFootprint
            && shapes[4].sourceRecordIndex == 4 && shapes[4].rawcode == "Emp2" && !shapes[4].hasFootprint,
            "real recapture retains complete raw bindings and runtime active subsequence [Doo2,Doo1]");
    } else {
        Require(shapes.size() == (duplicateActive ? 7u : 6u)
            && shapes[0].rawcode == "Emp0" && !shapes[0].hasFootprint
            && shapes[1].rawcode == "Doo0" && shapes[1].hasFootprint
            && shapes[2].rawcode == "Emp1" && !shapes[2].hasFootprint
            && shapes[3].rawcode == "Doo1" && shapes[3].hasFootprint
            && shapes[4].rawcode == "Doo2" && shapes[4].hasFootprint
            && shapes.back().rawcode == "Emp2" && !shapes.back().hasFootprint,
            "real pathTex resolution distinguishes _, none and empty cells from actual active textures");
        if (duplicateActive) Require(shapes[5].rawcode == "Doo3" && shapes[5].hasFootprint,
            "duplicate active XY is retained independently of adjacent disabled records");
    }
}

void SpecialRuntimeMixedRowsUseFormalSavingAndHistory() {
    for (const bool duplicateActive : {false, true}) {
        Fixture f; MixedSpecialRuntimeFixture(f, duplicateActive);
        const auto originalTerrain = f.Member("war3map.w3e"), originalPathing = f.Member("war3map.wpm");
        const auto originalWire = f.Member("war3map.doo");
        const auto sourcePath = f.archive->SourcePath();
        const auto sourceSha = f.archive->SourceSha256();
        const auto original = OrdinaryCurrent(f);
        auto deleted = original;
        deleted.specialDoodads = {original.specialDoodads[0], original.specialDoodads[4],
            original.specialDoodads[2], original.specialDoodads[3], original.specialDoodads.back()};
        const auto deletedWire = Take(deleted.Serialize());
        const auto rules = BridgeSpecialWaterRules(f);
        RequireMixedRuntimeCapture(rules, false, duplicateActive);
        PaintBridgeSpecialWater(f);
        Require(Take(OrdinaryCurrent(f).Serialize()) == deletedWire && f.edits->Revision() == 1,
            "one typed water command maps only active swap-last rows and preserves all disabled and ordinary fields");
        const auto waterTerrain = Take(SerializeMapTerrain(*f.edits->Terrain()));
        Require(f.edits->Terrain()->vertices[2 * 9 + 2].HasWater()
            && f.edits->Terrain()->vertices[2 * 9 + 2].CliffLevel() == 1,
            "mixed runtime record mapping belongs to a real shallow-water structural edit");
        auto waterPathing = originalPathing;
        // 完整独立字面G1；远区[Doo2,Doo1]和普通零占地记录保持不变。
        for (unsigned y = 4; y < 12; ++y) for (unsigned x = 4; x < 12; ++x)
            waterPathing[16 + y * 32 + x] = std::byte{0xdb};
        for (unsigned y = 6; y < 10; ++y) for (unsigned x = 6; x < 10; ++x)
            waterPathing[16 + y * 32 + x] = std::byte{0x1b};
        RequireMixedRuntimeCapture(BridgeSpecialWaterRules(f), true, duplicateActive);
        const auto plan = f.Plan(); auto output = f.Output(plan);
        auto tampered = deleted; tampered.specialDoodads.front().variation += 1;
        const auto member = std::find_if(output.begin(), output.end(),
            [](const auto& item) { return item.name == "war3map.doo"; });
        Require(member != output.end(), "formal mixed save includes its complete coupled DOO");
        member->data = Take(tampered.Serialize());
        Require(!plan.VerifyForSave(*f.edits, f.archive->SourceSha256(), output).Succeeded(),
            "frozen output binds disabled records too, including their opaque extra DWORD");

        f.SaveExpected("runtime-mixed-water.w3x", waterTerrain, waterPathing, &deletedWire, true);
        const auto saved = OrdinaryCurrent(f);
        const auto savedRules = Take(TerrainPathingSave::CaptureWaterBrushRules(*f.archive, *f.edits->Terrain(),
            *f.edits->Terrain(), saved, f.gameRoot, true, kBudget));
        RequireMixedRuntimeCapture(savedRules, true, duplicateActive);
        Require(f.edits->Undo() && Take(OrdinaryCurrent(f).Serialize()) == originalWire
            && Take(SerializeMapTerrain(*f.edits->Terrain())) == originalTerrain,
            "Undo across formal native save restores all raw records, nested ordinary data and the original terrain");
        f.SaveExpected("runtime-mixed-undo.w3x", originalTerrain, originalPathing, &originalWire, true);
        Require(f.edits->Redo() && Take(OrdinaryCurrent(f).Serialize()) == deletedWire
            && Take(SerializeMapTerrain(*f.edits->Terrain())) == waterTerrain,
            "Redo restores the exact mapped runtime order without re-running a different deletion algorithm");
        f.SaveExpected("runtime-mixed-redo.w3x", waterTerrain, waterPathing, &deletedWire, true);
        Require(Take(Sha256File(sourcePath)) == sourceSha,
            "mixed runtime regression uses only owned copies and leaves the original source file unchanged");
    }
}
