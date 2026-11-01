#pragma once

namespace TerrainSaveCorpus {
struct Point { std::uint32_t x, y; std::string texture; };

// 固定语料规则：距完整W3E网格中心最近的普通平地顶点，距离相同按y/x。
// 只换既有表中首个不同的ASCII字母数字rawcode，不创建贴图槽或修复来源。
std::optional<Point> Select(const War3Map::MapData::TerrainGridSnapshot& grid) {
    if (grid.header.version != 11 || grid.header.width < 3 || grid.header.height < 3
        || grid.header.width > 2049 || grid.header.height > 2049
        || grid.vertices.size() != std::size_t(grid.header.width) * grid.header.height
        || grid.header.groundTilesets.size() < 2 || grid.header.groundTilesets.size() > 16) return {};
    std::optional<Point> best;
    std::int64_t distance = INT64_MAX;
    for (std::uint32_t y = 1; y + 1 < grid.header.height; ++y) {
        for (std::uint32_t x = 1; x + 1 < grid.header.width; ++x) {
            const auto& center = grid.vertices[std::size_t(y) * grid.header.width + x];
            if (center.GroundTextureIndex() >= grid.header.groundTilesets.size()
                || !center.CliffLevel() || center.CliffLevel() > 14) continue;
            bool ordinary = true;
            for (int dy = -1; dy <= 1 && ordinary; ++dy) for (int dx = -1; dx <= 1; ++dx) {
                const auto& vertex = grid.vertices[std::size_t(int(y) + dy) * grid.header.width + int(x) + dx];
                if (vertex.extendedTextureLayout || vertex.textureFlagsExtensionRaw || vertex.UnknownWaterFlags()
                    || vertex.HasWater() || vertex.IsRamp() || vertex.IsBlight() || vertex.IsBoundary() || vertex.IsMapEdge()
                    || vertex.groundHeightRaw > 0x3fff || vertex.groundHeightRaw != center.groundHeightRaw
                    || vertex.CliffLevel() != center.CliffLevel()) { ordinary = false; break; }
            }
            if (!ordinary) continue;
            const auto selected = std::find_if(grid.header.groundTilesets.begin(), grid.header.groundTilesets.end(),
                [&](const auto& id) {
                    return id != grid.header.groundTilesets[center.GroundTextureIndex()] && id.size() == 4
                        && std::all_of(id.begin(), id.end(), [](unsigned char c) {
                            return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
                        });
                });
            if (selected == grid.header.groundTilesets.end()) continue;
            const auto dx = std::int64_t(x) * 2 - (grid.header.width - 1);
            const auto dy = std::int64_t(y) * 2 - (grid.header.height - 1);
            const auto next = dx * dx + dy * dy;
            if (!best || next < distance) { distance = next; best = Point{x, y, *selected}; }
        }
    }
    return best;
}

void TestSelection() {
    War3Map::MapData::TerrainGridSnapshot grid;
    grid.header.width = grid.header.height = 7;
    grid.header.groundTilesets = {"Ldrt", "Lgrs"};
    grid.vertices.resize(49);
    for (auto& vertex : grid.vertices) { vertex.groundHeightRaw = 8192; vertex.cliffAndLayerRaw = 2; }
    const auto center = Select(grid);
    if (!center || center->x != 3 || center->y != 3 || center->texture != "Lgrs")
        throw std::runtime_error("corpus selection must choose center and existing different texture");
    grid.vertices[3 * 7 + 3].textureAndFlagsRaw = 0x40;
    const auto tie = Select(grid);
    if (!tie || tie->x != 3 || tie->y != 1) throw std::runtime_error("corpus selection must avoid wet halo and break ties by y/x");
    for (auto& vertex : grid.vertices) vertex.textureAndFlagsRaw = 0x10;
    if (Select(grid)) throw std::runtime_error("corpus selection must not repair ramp maps");
    for (auto& vertex : grid.vertices) vertex.textureAndFlagsRaw = 0;
    grid.header.groundTilesets = {"Ldrt", "Ldrt"};
    if (Select(grid)) throw std::runtime_error("duplicate slots do not provide a different texture");
    grid.header.version = 12;
    if (Select(grid)) throw std::runtime_error("corpus selection does not assume classic layout for v12");
}
} // namespace TerrainSaveCorpus
