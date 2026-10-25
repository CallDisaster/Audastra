Bytes LargeClassicSettings() {
    auto settings = W3i();
    for (const auto offset : {48u, 52u, 56u, 60u}) Put32(settings, offset, 8);
    Put32(settings, 64, 272); Put32(settings, 68, 272); Put32(settings, 72, 0x400);
    return settings;
}

Bytes LargeClassicDoodads() {
    DoodadPlacements placements;
    for (const auto* rawcode : {"D001", "D002"}) {
        DoodadPlacement placement;
        placement.rawcode = rawcode;
        placement.position = {8 * 128.0f, 8 * 128.0f, 0};
        placement.scale = {1, 1, 1}; placement.flags = 2;
        placement.creationId = static_cast<std::int32_t>(placements.doodads.size() + 1);
        placements.doodads.push_back(std::move(placement));
    }
    return Take(placements.Serialize());
}

std::vector<MapArchiveMember> LargeClassicMaterials() {
    auto resources = Materials();
    resources.push_back({"UI\\MiscData.txt", Text("; original fixture\n")});
    resources.push_back({"UI\\WorldEditData.txt", Text(
        "[CameraMargins]\nLeft=4\nRight=4\nBottom=2\nTop=2\n"
        "[WorldEditArt]\nInvalidModel=Fixture/Invalid.mdx\n")});
    resources.push_back({"Doodads\\Doodads.slk", Slk(
        {"doodID", "code", "showInMM", "useMMColor", "MMRed", "MMGreen", "MMBlue", "file", "pathTex", "numVar"},
        {{"D001", "D001", "1", "1", "220", "40", "10", "", "", "1"},
            {"D002", "D002", "1", "1", "10", "180", "230", "", "", "1"}})});
    resources.push_back({"Units\\DestructableData.slk", Slk({"destructableID", "code"}, {{"Dst0", "Dst0"}})});
    resources.push_back({"Doodads\\DoodadMetaData.slk", Slk(
        {"ID", "field", "slk", "type", "repeat", "data", "index"},
        {{"dptx", "pathTex", "DoodadData", "pathingTexture", "0", "0", "0"}})});
    resources.push_back({"Units\\DestructableMetaData.slk", Slk(
        {"ID", "field", "slk", "type", "repeat", "data", "index"},
        {{"bwal", "walkable", "DestructableData", "bool", "0", "0", "-1"}})});
    return resources;
}

void TestLargeClassicCaptureAndTypedObjectOrder() {
    constexpr auto policy = TerrainMinimapRangePolicy::ClassicPlayable;
    Fixture fixture(false, LargeClassicSettings(), false, 289, LargeClassicDoodads());
    auto source = fixture.Source("large-classic.w3x", LargeClassicMaterials());
    const auto originalSha = source.SourceSha256();
    Rejected(CaptureTerrainMinimapAssets(source, fixture.edits, {}, {}, TerrainMinimapRangePolicy::FullW3e));
    Rejected(CaptureTerrainMinimapAssets(source, fixture.edits, {}, {}, TerrainMinimapRangePolicy::DeclaredW3iMargins));
    TerrainBrush boundary; boundary.kind = TerrainBrushKind::Boundary; boundary.radius = 0; boundary.value = true;
    const TerrainStrokePoint point{8 * 128, 8 * 128};
    Require(fixture.edits.EditTerrain(boundary, std::span(&point, 1), false).Succeeded(),
        "在真实会话建立对象所在格的boundary修改");
    auto terrain = Take(CaptureTerrainMinimapAssets(source, fixture.edits, {}, {}, policy));
    Require(terrain.Input().currentW3e.size() == fixture.w3e.size()
        && terrain.Binding().currentTerrainSha256 != terrain.Binding().originalTerrainSha256,
        "大图捕获必须冻结实际当前W3E，同时保留来源原字身份");
    const std::vector<MemberReplacement> output{{"war3map.w3e", terrain.Input().currentW3e}};
    Require(VerifyTerrainMinimapAssetsCurrent(terrain, source, fixture.edits, {}, {}, output).Succeeded()
        && VerifyTerrainMinimapAssetsFrozenOutput(terrain, source, output).Succeeded(),
        "超过旧65k的大图依赖应接通当前及最终正文核验");
    TerrainMinimapAssetCaptureLimits smaller;
    smaller.classicScan.maxTileAnchors = 65536;
    RequireRejected(VerifyTerrainMinimapAssetsCurrent(terrain, source, fixture.edits, {}, smaller, output),
        "TERRAIN_ASSETS_BUDGET");
    smaller = {}; smaller.classicScan.maxInputBytes = fixture.w3e.size() - 1;
    RequireRejected(VerifyTerrainMinimapAssetsCurrent(terrain, source, fixture.edits, {}, smaller, output),
        "TERRAIN_ASSETS_BUDGET");
    smaller = {}; smaller.classicScan.maxWorkUnits = 83521 + 4 * 82944 - 1;
    RequireRejected(VerifyTerrainMinimapAssetsCurrent(terrain, source, fixture.edits, {}, smaller, output),
        "TERRAIN_ASSETS_BUDGET");
    auto saved = Saved(fixture, source, "large-classic-readback.w3x", output);
    Require(VerifyTerrainMinimapAssetsReadback(terrain, saved).Succeeded(), "大图依赖应独立回读真实归档");
    auto altered = output; altered[0].data[altered[0].data.size() - 6] |= std::byte{0x80};
    RequireRejected(VerifyTerrainMinimapAssetsFrozenOutput(terrain, source, altered), "TERRAIN_ASSETS_FINAL_BYTES");
    auto wrongSaved = Saved(fixture, source, "large-classic-wrong.w3x", altered);
    RequireRejected(VerifyTerrainMinimapAssetsReadback(terrain, wrongSaved), "TERRAIN_ASSETS_READBACK_TERRAIN");
    auto objects = Take(CaptureClassicMinimapObjectAssets(std::move(terrain), source, fixture.edits, {}, output));
    Require(objects.CapturedMembers()[1].ordinaryPlacements == 2 && objects.ObjectLayer().pixels.size() == 2
        && objects.ObjectLayer().pixels[0].placementIndex == 0 && objects.ObjectLayer().pixels[1].placementIndex == 1,
        "真实DOO的同格对象覆盖顺序必须原样进入sealed图层");
    TerrainMinimapPostprocessSettings filters; filters.noisePercent = 0;
    const auto candidate = Take(BuildClassicMinimapCandidate(objects, filters));
    Require(candidate.raw.width == 272 && candidate.raw.height == 272 && candidate.manifest.sourceTiles == 82944
        && candidate.manifest.sourceRectangle == std::array<std::uint32_t, 4>{8, 8, 280, 280}
        && candidate.manifest.terrainScan.vertexDecodes == 83521 && candidate.manifest.terrainScan.tileAnchors == 82944
        && candidate.manifest.terrainScan.maximumResidentRows == 7,
        "真实来源与typed对象入口必须生成超过65k的完整可玩区，而非仅纯算法入口通过");
    Require(candidate.manifest.boundaryTiles == 1 && candidate.manifest.objectPixels == 2
        && candidate.raw.rgba[0] == std::byte{10} && candidate.raw.rgba[1] == std::byte{180}
        && candidate.raw.rgba[2] == std::byte{230} && candidate.raw.rgba[3] == std::byte{255},
        "七行扫描完成地形与boundary后，第二个同格对象仍最后覆盖，不能逐行改变顺序");
    Require(VerifyClassicMinimapObjectAssetsCurrent(objects, source, fixture.edits, {}, {}, {}, output).Succeeded()
        && VerifyClassicMinimapObjectAssetsFrozenOutput(objects, source, output).Succeeded()
        && VerifyClassicMinimapObjectAssetsReadback(objects, saved).Succeeded(),
        "大图组合资产的当前、冻结和回读生命周期仍须逐项验证");
    Require(Take(Sha256File(source.SourcePath())) == originalSha
        && Take(source.ReadMember("war3map.w3e")) == fixture.w3e,
        "大图测试只写原创副本，源归档与源W3E须逐字保持");
}
