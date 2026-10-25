// 使用本目标的原创MPQ/BLP/W3E夹具；不加载作者地图或本机游戏资源。

void TestCurrentEncodedClosureAndTighterBudgets() {
    Fixture fixture; auto source = fixture.Source("current-encoded.w3x");
    const auto captured = Take(CaptureTerrainMinimapAssets(source, fixture.edits, {}));
    Require(VerifyTerrainMinimapAssetsCurrent(captured, source, fixture.edits, {}).Succeeded()
        && VerifyTerrainMinimapAssetsCurrent(captured, source, fixture.edits, {}).Succeeded(),
        "同来源同修订重复复核应保留完整原始mip身份");

    TerrainMinimapAssetCaptureLimits limits;
    limits.maxTiles = 64;
    Require(VerifyTerrainMinimapAssetsCurrent(captured, source, fixture.edits, {}, limits).Succeeded(),
        "收紧到实际64地格仍通过原捕获检查");
    --limits.maxTiles;
    RequireRejected(VerifyTerrainMinimapAssetsCurrent(captured, source, fixture.edits, {}, limits), "TERRAIN_ASSETS_BUDGET");
    limits = {}; limits.atlas.maxTotalMipPixels = 340;
    RequireRejected(VerifyTerrainMinimapAssetsCurrent(captured, source, fixture.edits, {}, limits));
    limits.atlas.maxTotalMipPixels = 341;
    Require(VerifyTerrainMinimapAssetsCurrent(captured, source, fixture.edits, {}, limits).Succeeded(),
        "收紧mip预算不能丢弃旧链，也不能拒绝恰好341像素的原链");
    limits = {}; limits.atlas.maxDimension = 8;
    RequireRejected(VerifyTerrainMinimapAssetsCurrent(captured, source, fixture.edits, {}, limits));
    limits = {}; limits.tables.maxRows = 1;
    RequireRejected(VerifyTerrainMinimapAssetsCurrent(captured, source, fixture.edits, {}, limits));
    limits = {}; limits.atlas.maxWorkingBytes = 1024;
    RequireRejected(VerifyTerrainMinimapAssetsCurrent(captured, source, fixture.edits, {}, limits));
    limits = {}; limits.atlas.maxTotalMipPixels = 340;
    const auto boundedChain = Take(CaptureTerrainMinimapAssets(source, fixture.edits, {}, limits));
    Require(boundedChain.Dependencies().at(boundedChain.GroundAssets().front().dependencyIndex).decodedMips.size() == 4,
        "340像素限额只验证16/8/4/2四级原mip");
    Require(VerifyTerrainMinimapAssetsCurrent(boundedChain, source, fixture.edits, {}, limits).Succeeded(),
        "同限额保留原来实际验证的mip前缀");
    RequireRejected(VerifyTerrainMinimapAssetsCurrent(boundedChain, source, fixture.edits, {}), "TERRAIN_ASSETS_STALE");

    // 编码复核多计一次current-W3E散列时，精确旧边界仍可回到原有捕获路径。
    limits = {}; limits.maxSourceBytes = captured.Facts().cumulativeSourceBytes;
    const auto exact = Take(CaptureTerrainMinimapAssets(source, fixture.edits, {}, limits));
    Require(VerifyTerrainMinimapAssetsCurrent(exact, source, fixture.edits, {}, limits).Succeeded(),
        "原捕获精确累计字节边界不能因优化的额外散列被误拒绝");
    --limits.maxSourceBytes;
    RequireRejected(VerifyTerrainMinimapAssetsCurrent(exact, source, fixture.edits, {}, limits));
}

void TestCurrentFinalTerrainAndEncodedMipChanges() {
    Fixture fixture; auto source = fixture.Source("current-final-terrain.w3x");
    TerrainBrush brush; brush.kind = TerrainBrushKind::Raise; brush.radius = 0; brush.strength = 0.25;
    const TerrainStrokePoint point{256, 256};
    Require(fixture.edits.EditTerrain(brush, std::span(&point, 1), false).Succeeded(), "建立当前地形修订");
    const auto captured = Take(CaptureTerrainMinimapAssets(source, fixture.edits, {}));
    Require(VerifyTerrainMinimapAssetsCurrent(captured, source, fixture.edits, {}).Succeeded(),
        "预览阶段没有显式W3E输出时仍可复核live地形");
    const std::vector<MemberReplacement> current{{"war3map.w3e", captured.Input().currentW3e}};
    const std::vector<MemberReplacement> stale{{"war3map.w3e", fixture.w3e}};
    Require(VerifyTerrainMinimapAssetsCurrent(captured, source, fixture.edits, {}, {}, current).Succeeded(),
        "同次最终W3E须精确对应当前修订");
    RequireRejected(VerifyTerrainMinimapAssetsCurrent(captured, source, fixture.edits, {}, {}, stale), "TERRAIN_ASSETS_FINAL_BYTES");
    TerrainMinimapAssetCaptureLimits tight; tight.maxTiles = 64;
    RequireRejected(VerifyTerrainMinimapAssetsCurrent(captured, source, fixture.edits, {}, tight, stale), "TERRAIN_ASSETS_FINAL_BYTES");

    std::vector<MemberReplacement> overlay{{"Fixture\\Ground.blp", Blp()}};
    const auto overlaid = Take(CaptureTerrainMinimapAssets(source, fixture.edits, {}, {},
        TerrainMinimapRangePolicy::FullW3e, overlay));
    Require(VerifyTerrainMinimapAssetsCurrent(overlaid, source, fixture.edits, {}, {}, overlay).Succeeded(),
        "复核须使用同次真实overlay而非原地图同名资源");
    // 只改最后一级mip的索引，mip0和可见代表色完全保持。
    overlay.front().data.back() = std::byte{1};
    RequireRejected(VerifyTerrainMinimapAssetsCurrent(overlaid, source, fixture.edits, {}, {}, overlay), "TERRAIN_ASSETS_FINAL_BYTES");
}

void TestCurrentWinnerLayersAndSettingsOutput() {
    Fixture fixture(false, W3i()); auto source = fixture.Source("current-winner-layers.w3x");
    const auto captured = Take(CaptureTerrainMinimapAssets(source, fixture.edits, {}, {},
        TerrainMinimapRangePolicy::DeclaredW3iMargins));
    const std::vector<MemberReplacement> sameAtlas{{"Fixture\\Ground.blp", Blp()}};
    RequireRejected(VerifyTerrainMinimapAssetsCurrent(captured, source, fixture.edits, {}, {}, sameAtlas), "TERRAIN_ASSETS_STALE");
    const std::vector<MemberReplacement> sameSettings{{"war3map.w3i", *fixture.w3i}};
    Require(VerifyTerrainMinimapAssetsCurrent(captured, source, fixture.edits, {}, {}, sameSettings).Succeeded(),
        "W3I同字显式回写继续保留捕获时的原map语义");
    const std::vector<MemberReplacement> duplicates{{"unused.blp", {}}, {"UNUSED.BLP", {}}};
    RequireRejected(VerifyTerrainMinimapAssetsCurrent(captured, source, fixture.edits, {}, {}, duplicates),
        "TERRAIN_ASSETS_FINAL_DUPLICATE");

    fixture.TextureVariation();
    RequireRejected(VerifyTerrainMinimapAssetsCurrent(captured, source, fixture.edits, {}), "TERRAIN_ASSETS_STALE");
    Require(fixture.edits.Undo(), "撤销当前夹具笔划");
    RequireRejected(VerifyTerrainMinimapAssetsCurrent(captured, source, fixture.edits, {}), "TERRAIN_ASSETS_STALE");
}

void TestCurrentPreservesCapturedWarnings() {
    // 原创JPEG在完整EOI后多一个字节：合法图像仍可用，但来源异常须继续对外报告。
    const auto jpeg = TestFixtures::MakeTinyGrayJpeg(16, 16);
    Bytes blp(160);
    std::copy_n(Text("BLP1").begin(), 4, blp.begin());
    Put32(blp, 12, 16); Put32(blp, 16, 16);
    Put32(blp, 28, 160); Put32(blp, 92, static_cast<std::uint32_t>(jpeg.bytes.size() + 1));
    for (const auto byte : jpeg.bytes) blp.push_back(static_cast<std::byte>(byte));
    blp.push_back(std::byte{0x41});
    Fixture fixture; auto source = fixture.Source("current-warning.w3x", Materials(std::move(blp)));
    auto captured = CaptureTerrainMinimapAssets(source, fixture.edits, {});
    Require(captured.Succeeded() && captured.diagnostics.size() == 1
        && captured.diagnostics.front().severity == DiagnosticSeverity::Warning
        && captured.diagnostics.front().code == "TERRAIN_ATLAS_JPEG_TRAILING_DATA",
        "原创完整JPEG尾数据夹具须产生可用快照和明确警告");
    const auto current = VerifyTerrainMinimapAssetsCurrent(*captured.value, source, fixture.edits, {});
    Require(current.Succeeded() && current.diagnostics.size() == captured.diagnostics.size(),
        "同字复核不能因省去解码而丢失原有警告");
    const auto& before = captured.diagnostics.front(); const auto& after = current.diagnostics.front();
    Require(before.severity == after.severity && before.subsystem == after.subsystem
        && before.code == after.code && before.message == after.message,
        "同字快路须原样保留原解码诊断级别、来源、代码和文本");
}
