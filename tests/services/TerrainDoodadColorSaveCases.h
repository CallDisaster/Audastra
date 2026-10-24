#pragma once

// 在TerrainNativeObjectSaveTests.cpp的TerrainOrdinaryDoodadSaveCases.h之后包含，
// 复用完整原创资源、独立字面占地、正式MapSaveService和CommitSaved。
void OrdinaryDoodadColorsUseBothNativeSaveStages() {
    for (const bool custom : {false, true}) {
        Fixture f; OrdinaryMixedFixture(f);
        const std::string id = custom ? "Clr0" : "Doo0";
        auto placements = Take(ParseDoodadPlacements(f.Member("war3map.doo")));
        placements.doodads.front().rawcode = id;
        f.Member("war3map.doo") = Take(placements.Serialize());
        auto document = Take(NativeObjectDocument::CreateEmpty(ObjectType::Doodad));
        if (custom) Check(document.AddCustomObject(id, "Doo0"));
        else Check(document.AddOriginalObject(id));
        for (const std::string field : {"ClrR", "ClrG", "ClrB"}) for (std::uint32_t variation = 1; variation <= 10; ++variation) {
            ModificationKey key{field}; key.variation = variation; key.dataIndex = 0;
            Check(document.AddModification({ObjectType::Doodad, id}, key,
                NativeValueType::Int32, std::int32_t(variation * 17)));
        }
        const auto originalW3d = Take(NativeObjectCodec(ObjectType::Doodad).Serialize(document));
        f.members.push_back({"war3map.w3d", originalW3d});
        f.members.push_back({"Doodads/DoodadMetaData.slk", Slk({"ID", "field", "slk", "type", "repeat", "index"},
            {{"Pth0", "pathTex", "DoodadData", "pathingTexture", "0", "0"},
             {"ClrR", "vertR", "DoodadData", "int", "10", "0"},
             {"ClrG", "vertG", "DoodadData", "int", "10", "0"},
             {"ClrB", "vertB", "DoodadData", "int", "10", "0"}})});
        f.Open(); f.Paint();
        const auto originalDoo = f.Member("war3map.doo");
        auto moved = OrdinaryCurrent(f); moved.doodads.front().position[0] = 640;
        Check(f.edits->EditDoodads(moved)); const auto wire = Take(moved.Serialize());
        const auto owner = Take(TerrainPathingSave::CaptureNative(*f.archive, f.original,
            *f.edits->Terrain(), f.gameRoot, kBudget, &wire));
        auto oldExpected = f.ExpectedPathing(false), newExpected = f.ExpectedPathing(true);
        AddOrdinaryPatternExpected(oldExpected, 16, 15); AddOrdinaryPatternExpected(newExpected, 20, 15);
        const auto before = Take(owner.GenerateNative(f.original, TerrainNativeStage::Before, kBudget));
        const auto after = Take(owner.GenerateNative(*f.edits->Terrain(), TerrainNativeStage::After, kBudget));
        Require(before.size() == 32u * 32u && after.size() == before.size(), "RGB保存完整生成两阶段通行");
        for (std::size_t i = 0; i < before.size(); ++i) {
            Require(before[i] == (oldExpected[16 + i] & std::byte{0xee}), "RGB原生G0解释完整旧WPM");
            Require(after[i] == (newExpected[16 + i] & std::byte{0xee}), "RGB原生G1匹配完整新摆放");
        }
        const auto plan = f.Plan(); const auto output = f.Output(plan);
        auto changedW3d = originalW3d;
        // 只变第一个颜色值，视觉语义不参与占地也不能继承原owner的来源身份。
        Require(changedW3d.size() > 40, "RGB原生来源含完整记录");
        changedW3d[custom ? 40 : 36] ^= std::byte{1};
        auto substituted = output; substituted.push_back({"war3map.w3d", changedW3d});
        Require(!owner.VerifyFrozenOutput(substituted, {}).Succeeded(), "无关颜色字节变化仍令冻结来源失效");
        const auto budget = owner.CaptureBudgetBytes();
        Require(Take(TerrainPathingSave::CaptureNative(*f.archive, f.original, *f.edits->Terrain(),
            f.gameRoot, budget, &wire)).CaptureBudgetBytes() == budget, "两阶段RGB来源遵守精确累计预算");
        Require(!TerrainPathingSave::CaptureNative(*f.archive, f.original, *f.edits->Terrain(),
            f.gameRoot, budget - 1, &wire).Succeeded(), "两阶段RGB来源少一字节预算仍拒绝");
        const auto expectedTerrain = f.ExpectedTerrain(true);
        f.SaveExpected("rgb-native-moved.w3x", expectedTerrain, newExpected, &wire, true);
        Require(Take(f.archive->ReadMember("war3map.w3d")) == originalW3d, "正式保存逐字保留完整W3D颜色变体");
        Require(f.edits->Undo(), "RGB保存后的普通摆放仍可撤销");
        auto undone = f.ExpectedPathing(true); AddOrdinaryPatternExpected(undone, 16, 15);
        f.SaveExpected("rgb-native-undone.w3x", expectedTerrain, undone, &originalDoo, true);
        Require(f.edits->Redo(), "RGB保存后的普通摆放仍可重做");
        f.SaveExpected("rgb-native-redone.w3x", expectedTerrain, newExpected, &wire, true);
    }
}
