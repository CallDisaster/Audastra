#pragma once

void DeadBridgeRequiresItsActualDeathDeclaration() {
    const auto declare = [](Fixture& fixture, std::string death) {
        // 正常位图来自Original覆盖；死亡声明只控制注册门，其位图不参与WPM导出。
        fixture.Member("Units/DestructableData.slk") = Slk(
            {"destructableID", "code", "walkable", "pathTex", "fixedRot", "pathTexDeath"},
            {{"Br01", "Br01", "0", std::string(kBridgeTexture), "0", std::move(death)}});
    };
    for (const bool custom : {false, true}) for (const std::uint8_t flags : {0, 2, 6}) {
        Fixture fixture; PatternedBridgeOriginal(fixture, 90.f, NativeValueType::Unreal, custom);
        declare(fixture, "PathTextures/death-not-consumed-by-wpm.tga");
        auto placements = Take(ParseDoodadPlacements(fixture.Member("war3map.doo")));
        placements.doodads.front().flags = flags;
        placements.doodads.front().lifePercent = 0;
        placements.doodads.front().position[2] = 321.25f;
        const auto wire = Take(placements.Serialize()); fixture.Member("war3map.doo") = wire;
        fixture.Open(); fixture.Paint();
        const auto shallow = BridgeOriginalExpectedPathing(fixture, false, true);
        const auto deep = BridgeOriginalExpectedPathing(fixture, true, true);
        fixture.SaveExpected("dead-bridge-deep.w3x", fixture.ExpectedTerrain(true), deep, &wire, true);
        Require(fixture.edits->Undo(), "死亡桥地形保存后仍可撤销");
        fixture.SaveExpected("dead-bridge-undone.w3x", fixture.ExpectedTerrain(false), shallow, &wire, true);
        Require(fixture.edits->Redo(), "死亡桥地形保存后仍可重做");
        fixture.SaveExpected("dead-bridge-redone.w3x", fixture.ExpectedTerrain(true), deep, &wire, true);
    }
    for (const std::string& death : std::vector<std::string>{"", "_", "none", "nOnE", std::string(260, 'x'), std::string("x\0y", 3)}) {
        Fixture fixture; PatternedBridgeOriginal(fixture, 90.f); declare(fixture, death);
        auto placements = Take(ParseDoodadPlacements(fixture.Member("war3map.doo")));
        placements.doodads.front().lifePercent = 0;
        fixture.Member("war3map.doo") = Take(placements.Serialize()); fixture.Open(); fixture.Paint();
        const auto source = Take(Sha256File(fixture.archive->SourcePath()));
        const auto plan = fixture.Plan();
        Require(!plan.SaveGate().Succeeded() && HasCode(plan.SaveGate(), "TERRAIN_WALKABLE_DESTRUCTABLE_STATE"),
            "死亡路径明确禁用时不以存活占地代替，仍在状态门拒绝");
        Require(Take(Sha256File(fixture.archive->SourcePath())) == source && fixture.edits->IsDirty(),
            "死亡声明拒绝保留原图和地形草稿");
    }
}
