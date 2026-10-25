Bytes StreamingTerrain(std::uint32_t tiles) {
    const auto width = tiles + 1;
    auto bytes = W3e(); bytes.resize(45 + static_cast<std::size_t>(width) * width * 7);
    Put32(bytes, 29, width); Put32(bytes, 33, width);
    for (std::size_t at = 45; at < bytes.size(); at += 7) {
        Put16(bytes, at, 8192); Put16(bytes, at + 2, 8192);
        bytes[at + 4] = bytes[at + 5] = std::byte{0}; bytes[at + 6] = std::byte{0xf2};
    }
    return bytes;
}

ClassicMinimapCandidateInput StreamingInput(std::uint32_t tiles) {
    auto input = Input(StreamingTerrain(tiles));
    MapData::MapSettings settings;
    settings.formatVersion = 25; settings.custom.emplace(); settings.flags = 0x400;
    settings.cameraMargins = {8, 8, 8, 8};
    settings.playableWidth = settings.playableHeight = static_cast<std::int32_t>(tiles - 16);
    input.terrain.sourceW3i = Take(settings.Serialize());
    input.terrain.rangePolicy = TerrainMinimapRangePolicy::ClassicPlayable;
    return input;
}

void StreamingLargePlayableAndBudgets() {
    auto input = StreamingInput(288);
    const auto output = Take(BuildClassicMinimapCandidate(input));
    Require(output.raw.width == 272 && output.raw.height == 272 && output.manifest.sourceTiles == 82944
        && output.manifest.sourceRectangle == std::array<std::uint32_t, 4>{8, 8, 280, 280},
        "288平方源与超过65536格的真实playable应生成完整逐格小地图");
    const auto& facts = output.manifest.terrainScan;
    Require(facts.inputBytes == input.terrain.currentW3e.size() && facts.vertexDecodes == 83521
        && facts.tileAnchors == 82944 && facts.maximumResidentRows == 7
        && facts.maximumResidentBytes < MapData::TerrainRowScanLimits{}.maxResidentBytes
        && facts.consumerWorkUnits == 272u * 272u * 1024u,
        "全图含域外末行列应完整解码，邻域与七行驻留须有实际累计事实");
    Require(RgbaAt(output.raw, 0, 0) == std::array<unsigned, 4>{100, 120, 140, 255}
        && RgbaAt(output.raw, 271, 271) == std::array<unsigned, 4>{100, 120, 140, 255},
        "大图首末锚点均按原代表色输出");
    ClassicMinimapCandidateLimits limits;
    limits.terrainScan.maxTileAnchors = 65536;
    RequireRejected(BuildClassicMinimapCandidate(input, limits), "TERRAIN_TILE_LIMIT");
    limits = {}; limits.terrainScan.maxWorkUnits = facts.workUnits;
    Require(BuildClassicMinimapCandidate(input, limits).Succeeded(), "跨所有行的工作精确累计边界应通过");
    --limits.terrainScan.maxWorkUnits;
    RequireRejected(BuildClassicMinimapCandidate(input, limits), "TERRAIN_ROW_WORK_BUDGET");
    limits = {}; limits.maxRawRgbaBytes = output.raw.rgba.size() - 1;
    RequireRejected(BuildClassicMinimapCandidate(input, limits), "CLASSIC_MINIMAP_BUDGET");
    limits = {}; limits.terrainScan.maxResidentBytes = facts.maximumResidentBytes - 1;
    RequireRejected(BuildClassicMinimapCandidate(input, limits), "TERRAIN_ROW_RESIDENT_BUDGET");
    auto legacy = input; legacy.terrain.rangePolicy = TerrainMinimapRangePolicy::FullW3e; legacy.terrain.sourceW3i.reset();
    RequireRejected(BuildClassicMinimapCandidate(legacy), "TERRAIN_TILE_LIMIT");
    // 终端东北角完全在裁剪区域之外；仍必须由同一vertex解码器发现非法原字。
    Put16(input.terrain.currentW3e, input.terrain.currentW3e.size() - 7, 0x8000);
    input.terrain.binding.terrainSha256 = Take(Sha256(input.terrain.currentW3e));
    RequireRejected(BuildClassicMinimapCandidate(input), "TERRAIN_GROUND_HEIGHT_AMBIGUOUS");
}

void StreamingStableNoiseAndSmallGoldenOutput() {
    auto input = StreamingInput(288);
    input.postprocess.medianRadius = 2; input.postprocess.blurRadius = 0.5;
    input.postprocess.noisePercent = 10; input.postprocess.noiseSeed = 1234567;
    const auto first = Take(BuildClassicMinimapCandidate(input));
    const auto second = Take(BuildClassicMinimapCandidate(input));
    Require(first.raw.rgba == second.raw.rgba && first.preNoise.rgba == second.preNoise.rgba
        && first.finalImage.rgba == second.finalImage.rgba && first.encoded.bytes == second.encoded.bytes,
        "行窗口不能改变同内容固定seed的四层输出确定性");
    // 既有微型黄金例的未滤波raw和上下翻转结果是逐像素手写值，防止复用同算法自证。
    const auto small = Take(BuildClassicMinimapCandidate(Input(W3e(true))));
    for (std::uint32_t y = 0; y < 2; ++y) for (std::uint32_t x = 0; x < 2; ++x)
        Require(RgbaAt(small.raw, x, y) == std::array<unsigned, 4>{9, 11, 13, 255}, "流式路径保留原边界黄金像素");
    for (std::uint32_t y = 0; y < 256; ++y) for (std::uint32_t x = 0; x < 256; ++x)
        Require(RgbaAt(small.preNoise, x, y) == std::array<unsigned, 4>{9, 11, 13, 255}, "16.16缩放与翻转保留原边界黄金像素");
}
