#pragma once

void AbsoluteHeightBridgeKeepsNativePathingAndWireState() {
    for (const bool custom : {false, true}) for (const std::uint8_t flags : {0, 4, 6}) {
        Fixture fixture; PatternedBridgeOriginal(fixture, 90.f, NativeValueType::Unreal, custom);
        auto placements = Take(ParseDoodadPlacements(fixture.Member("war3map.doo")));
        placements.doodads.front().flags = flags;
        placements.doodads.front().position[2] = 321.25f;
        const auto wire = Take(placements.Serialize()); fixture.Member("war3map.doo") = wire;
        fixture.Open(); fixture.Paint();
        const auto shallow = BridgeOriginalExpectedPathing(fixture, false, true);
        const auto deep = BridgeOriginalExpectedPathing(fixture, true, true);
        Require(shallow == fixture.Member("war3map.wpm"), "保留Z不改变原版二维桥注册的完整旧WPM");
        fixture.SaveExpected("absolute-bridge-deep.w3x", fixture.ExpectedTerrain(true), deep, &wire, true);
        Require(fixture.edits->Undo(), "保留Z桥的地形修改仍可撤销");
        fixture.SaveExpected("absolute-bridge-undone.w3x", fixture.ExpectedTerrain(false), shallow, &wire, true);
        Require(fixture.edits->Redo(), "保留Z桥的地形修改仍可重做");
        fixture.SaveExpected("absolute-bridge-redone.w3x", fixture.ExpectedTerrain(true), deep, &wire, true);
    }
    for (const unsigned state : {1u,3u,5u,7u,8u,16u,31u,130u,255u,256u,306u,355u,357u,511u}) {
        Fixture fixture; WalkableBridge(fixture);
        auto placements = Take(ParseDoodadPlacements(fixture.Member("war3map.doo")));
        auto& bridge = placements.doodads.front();
        if (state < 256) bridge.flags = static_cast<std::uint8_t>(state);
        else { bridge.flags = 6; bridge.lifePercent = static_cast<std::uint8_t>(state - 256); }
        const auto serialized = placements.Serialize();
        if (bridge.flags >= 0x20) {
            Require(!serialized.Succeeded(), "布局不支持的高位标志在序列化边界拒绝");
            continue;
        }
        fixture.Member("war3map.doo") = Take(serialized); fixture.Open(); fixture.Paint();
        const auto source = Take(Sha256File(fixture.archive->SourcePath()));
        const auto plan = fixture.Plan();
        Require(!plan.SaveGate().Succeeded() && HasCode(plan.SaveGate(), "TERRAIN_WALKABLE_DESTRUCTABLE_STATE"),
            "仅增加已证标志，未知标志和无死亡声明/受损/越范围生命值不放开");
        Require(Take(Sha256File(fixture.archive->SourcePath())) == source && fixture.edits->IsDirty(),
            "状态拒绝不写原图或抹掉未保存地形");
    }
}
