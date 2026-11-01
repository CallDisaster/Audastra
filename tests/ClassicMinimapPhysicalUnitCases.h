#pragma once

void TestDuplicateUnitIdsOnlyGrantPhysicalMinimapReads() {
    UnitPlacements placements;
    for (int i = 0; i < 2; ++i) {
        UnitPlacement unit; unit.rawcode = "hfoo"; unit.creationId = 7; unit.scale = {1, 1, 1};
        unit.position = {128.f + i * 128.f, 256.f, 0.f}; placements.units.push_back(unit);
    }
    const auto repeated = Take(placements.Serialize());
    Fixture fixture(repeated, MapDataFixtures::EmptyDoodadsV8Sub11(), repeated);
    auto source = fixture.Source("duplicate-unit-ids.w3x");
    const auto before = Take(fixture.edits.Snapshot(MapDataKind::Units));
    Require(before.Member(MapDataKind::Units).state == MapDataState::Partial && before.Units()
        && before.Units()->units.size() == 2, "重复ID是已完整解析的Partial，不伪造Ready");
    auto assets = Take(CaptureClassicMinimapObjectAssets(CaptureTerrain(fixture, source), source, fixture.edits));
    Require(assets.Units().units.size() == 2 && assets.Units().units[0].creationId == 7
        && assets.Units().units[1].creationId == 7 && !fixture.edits.IsDirty(), "读取保留全部物理行、重复ID与干净会话");
    const auto candidate = Take(BuildClassicMinimapCandidate(assets, FlatPostprocess()));
    placements.units[1].creationId = 8;
    const auto unique = Take(placements.Serialize());
    Fixture control(unique, MapDataFixtures::EmptyDoodadsV8Sub11(), unique);
    auto controlSource = control.Source("unique-unit-ids.w3x");
    auto controlAssets = Take(CaptureClassicMinimapObjectAssets(CaptureTerrain(control, controlSource), controlSource, control.edits));
    const auto expected = Take(BuildClassicMinimapCandidate(controlAssets, FlatPostprocess()));
    Require(candidate.raw.rgba == expected.raw.rgba && candidate.manifest.blpSha256 == expected.manifest.blpSha256,
        "初始化ID不参与0x08保存像素，重复与唯一ID的实际图像逐字节相同");
    Require(VerifyClassicMinimapObjectAssetsCurrent(assets, source, fixture.edits, {}).Succeeded()
        && VerifyClassicMinimapObjectAssetsFrozenOutput(assets, source, {}).Succeeded(), "物理行捕获仍通过真实当前/冻结身份复核");
    auto changed = Take(ParseUnitPlacements(repeated)); changed.units[0].owner = 1;
    Require(!fixture.edits.EditUnits(changed).Succeeded(), "只读小地图资格不授予重复ID单位的编辑定位");
    const auto after = Take(fixture.edits.Snapshot(MapDataKind::Units));
    Require(after.Member(MapDataKind::Units).state == MapDataState::Partial
        && after.Member(MapDataKind::Units).sources.front().data == repeated, "编辑拒绝后不改成员原字或Partial状态");
    const std::array<MemberReplacement, 1> wrong{{{"war3mapUnits.doo", unique}}};
    Require(!VerifyClassicMinimapObjectAssetsFrozenOutput(assets, source, wrong).Succeeded(),
        "即使像素相同也不能将重复ID源偷换为唯一ID输出");
    auto truncated = repeated; truncated.pop_back();
    Fixture invalid(truncated, MapDataFixtures::EmptyDoodadsV8Sub11(), truncated);
    auto invalidSource = invalid.Source("truncated-units.w3x");
    Rejected(CaptureClassicMinimapObjectAssets(CaptureTerrain(invalid, invalidSource), invalidSource, invalid.edits));
    auto doodads = Take(ParseDoodadPlacements(DoodadBytes(true, false)));
    doodads.doodads.push_back(doodads.doodads.front());
    const auto badDoodads = Take(doodads.Serialize());
    Fixture other(MapDataFixtures::EmptyUnitsV8Sub11(), badDoodads, MapDataFixtures::EmptyUnitsV8Sub11(), badDoodads);
    auto otherSource = other.Source("duplicate-doodad-ids.w3x", false, false, true);
    Rejected(CaptureClassicMinimapObjectAssets(CaptureTerrain(other, otherSource), otherSource, other.edits),
        "CLASSIC_OBJECT_CAPTURE_CURRENT_LAYOUT");
}
