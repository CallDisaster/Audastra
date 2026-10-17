#include "War3MapCore/Render/TerrainGroundMesh.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using namespace War3Map;
using namespace War3Map::MapData;
using namespace War3Map::Render;
using Atlas = std::shared_ptr<const TerrainAtlasSnapshot>;

void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

void Near(float actual, float expected, const char* message) {
    if (!std::isfinite(actual) || std::abs(actual - expected) > 1e-6f) throw std::runtime_error(message);
}

TerrainGridSnapshot Grid(std::uint32_t width = 2, std::uint32_t height = 2,
    const std::function<float(std::uint32_t, std::uint32_t)>& elevation = {}) {
    TerrainGridSnapshot terrain;
    terrain.header.width = width; terrain.header.height = height;
    terrain.header.groundTilesets = {"Ldrt", "Lgrs", "Lrok"};
    terrain.header.offset = {-448, -1216};
    terrain.minimum.fill(std::numeric_limits<float>::infinity());
    terrain.maximum.fill(-std::numeric_limits<float>::infinity());
    for (std::uint32_t y = 0; y < height; ++y) for (std::uint32_t x = 0; x < width; ++x) {
        TerrainVertex vertex;
        vertex.groundHeightRaw = 8192; vertex.cliffAndLayerRaw = 2;
        vertex.position = {terrain.header.offset[0] + static_cast<float>(x) * 128,
            terrain.header.offset[1] + static_cast<float>(y) * 128, elevation ? elevation(x, y) : 0};
        for (std::size_t axis = 0; axis < 3; ++axis) {
            terrain.minimum[axis] = std::min(terrain.minimum[axis], vertex.position[axis]);
            terrain.maximum[axis] = std::max(terrain.maximum[axis], vertex.position[axis]);
        }
        terrain.vertices.push_back(vertex);
        if (x + 1 < width && y + 1 < height) {
            const auto sw = y * width + x;
            terrain.tiles.push_back({{sw, sw + 1, sw + width, sw + width + 1}});
        }
    }
    return terrain;
}

std::shared_ptr<TerrainAtlasSnapshot> MakeAtlas(std::uint32_t columns = 4, std::uint32_t cellSize = 16) {
    auto atlas = std::make_shared<TerrainAtlasSnapshot>();
    atlas->columns = columns;
    atlas->sourceWidth = atlas->image.width = columns * cellSize;
    atlas->sourceHeight = atlas->image.height = 4 * cellSize;
    atlas->image.rgba.assign(static_cast<std::size_t>(atlas->image.width) * atlas->image.height * 4, std::byte{255});
    for (std::uint32_t y = 0; y < 4; ++y) for (std::uint32_t x = 0; x < columns; ++x)
        atlas->cells.push_back({x, y, x * cellSize, y * cellSize, cellSize, cellSize});
    return atlas;
}

std::vector<Atlas> Atlases() { return {MakeAtlas(), MakeAtlas(8), MakeAtlas(4, 32)}; }

// 既有用例核对未加光照的颜色语义；光照单独在 TestLighting 覆盖。
constexpr TerrainLighting kUnlit{false};

TerrainGroundMesh Success(const TerrainGridSnapshot& terrain, const std::vector<Atlas>& atlases,
    const TerrainGroundMeshLimits& limits = {}, const TerrainLighting& lighting = kUnlit,
    const TerrainWaterStyle* water = nullptr, Atlas blight = {}) {
    auto result = BuildTerrainGroundMesh(terrain, atlases, limits, lighting, water, {}, std::move(blight));
    if (!result.Succeeded()) throw std::runtime_error("构建意外失败：" + result.diagnostics.front().code);
    return std::move(*result.value);
}

void Rejected(const TerrainGridSnapshot& terrain, const std::vector<Atlas>& atlases,
    std::string_view code, const TerrainGroundMeshLimits& limits = {}) {
    const auto result = BuildTerrainGroundMesh(terrain, atlases, limits);
    Require(!result.Succeeded() && !result.value, "失败不得发布部分网格或纹理列表");
    Require(result.diagnostics.size() == 1 && result.diagnostics.front().code == code,
        "必须给出稳定的错误类别");
}

void CheckGeometry(const TerrainGridSnapshot& terrain, const TerrainGroundMesh& mesh) {
    const auto tiles = terrain.tiles.size(), base = tiles - mesh.hiddenTileCount;
    const auto layered = base + mesh.overlayTiles.size();
    const auto quads = layered + mesh.waterTiles.size();
    Require(mesh.vertices.size() == quads * 4 && mesh.indices.size() == quads * 6,
        "每个可见原格及过渡层必须使用四顶点及六索引，不得降采样");
    for (std::size_t quad = 0; quad < quads; ++quad) {
        const auto tile = quad < base ? (mesh.hiddenTileCount ? mesh.baseTiles[quad] : quad)
            : quad < layered ? mesh.overlayTiles[quad - base] : mesh.waterTiles[quad - layered];
        Require(tile < tiles, "过渡层/水面必须指向原 tile");
        for (std::size_t corner = 0; corner < 4; ++corner) {
            const auto& original = terrain.vertices[terrain.tiles[tile].vertices[corner]].position;
            const auto& copied = mesh.vertices[quad * 4 + corner];
            if (quad < layered)
                Require(original == std::array<float, 3>{copied.x, copied.y, copied.z}, "UV 拆点不能修改任何原始 XYZ 位值");
            else
                Require(original[0] == copied.x && original[1] == copied.y, "水面 XY 必须与原格一致");
        }
    }
    std::vector<bool> seen(quads);
    std::uint32_t nextIndex = 0;
    bool blending = false;
    for (const auto& batch : mesh.batches) {
        Require(batch.firstIndex == nextIndex && batch.indexCount != 0 && batch.indexCount % 6 == 0,
            "批次必须无空隙覆盖全部索引");
        Require(batch.textureIndex == TerrainGroundHeightColor || batch.textureIndex < mesh.textures.size(),
            "批次纹理索引必须有效");
        Require(!(blending && !batch.alphaBlend) && !(batch.alphaBlend && batch.textureIndex == TerrainGroundHeightColor),
            "过渡层批次必须在全部基础层之后且带贴图");
        blending |= batch.alphaBlend;
        for (auto index = batch.firstIndex; index < batch.firstIndex + batch.indexCount; index += 6) {
            const auto sw = mesh.indices[index], tile = sw / 4;
            Require(sw % 4 == 0 && tile < seen.size() && !seen[tile], "tile 不能重复或混用邻格顶点");
            seen[tile] = true;
            const std::array<std::uint32_t, 6> expected{sw, sw + 1, sw + 2, sw + 1, sw + 3, sw + 2};
            Require(std::equal(expected.begin(), expected.end(), mesh.indices.begin() + index), "必须保留 NW-SE 对角线及绕序");
        }
        nextIndex += batch.indexCount;
    }
    Require(nextIndex == mesh.indices.size() && std::all_of(seen.begin(), seen.end(), [](bool value) { return value; }),
        "texture 归组不能丢格子");
}

void CheckCell(const TerrainGroundMesh& mesh, std::uint32_t columns, std::uint32_t cellSize,
    std::uint32_t column, std::uint32_t row) {
    const auto width = static_cast<float>(columns * cellSize), height = static_cast<float>(4 * cellSize);
    const auto left = (static_cast<float>(column * cellSize) + 0.5f) / width;
    const auto right = (static_cast<float>((column + 1) * cellSize) - 0.5f) / width;
    const auto top = (static_cast<float>(row * cellSize) + 0.5f) / height;
    const auto bottom = (static_cast<float>((row + 1) * cellSize) - 0.5f) / height;
    const std::array<std::array<float, 2>, 4> expected{{{left, bottom}, {right, bottom}, {left, top}, {right, top}}};
    for (std::size_t i = 0; i < expected.size(); ++i) {
        Near(mesh.vertices[i].u, expected[i][0], "U 必须使用真实像素宽度和半 texel 内缩");
        Near(mesh.vertices[i].v, expected[i][1], "V 方向必须 SW 下、NW 上且按真实高度内缩");
        Near(mesh.vertices[i].red, 1, "基础层不能被高度色染色");
    }
}

void TestGeometryAndBlocks() {
    const auto terrain = Grid(35, 20, [](auto x, auto y) { return static_cast<float>((x * y) % 17) * 9.25f; });
    const auto mesh = Success(terrain, Atlases());
    CheckGeometry(terrain, mesh);
    Require(mesh.texturedTiles == 34 * 19 && mesh.fallback.totalTiles == 0 && mesh.blocks.size() == 6,
        "原格数量与 16 格块划分错误");
    const auto& block = mesh.blocks.back();
    Require(block.tileX == 32 && block.tileY == 16 && block.tileWidth == 2 && block.tileHeight == 3,
        "末块必须包含末行末列而不延展地图");
    Near(block.maximum[0], terrain.maximum[0], "块 AABB 东界错误");
    Near(block.maximum[1], terrain.maximum[1], "块 AABB 北界错误");
    float low = std::numeric_limits<float>::infinity(), high = -low;
    for (std::uint32_t y = 16; y < 20; ++y) for (std::uint32_t x = 32; x < 35; ++x) {
        low = std::min(low, terrain.vertices[y * 35 + x].position[2]);
        high = std::max(high, terrain.vertices[y * 35 + x].position[2]);
    }
    Near(block.minimum[2], low, "块必须包含内部最低高度");
    Near(block.maximum[2], high, "块必须包含内部最高高度");
    auto nonPlanar = Grid(2, 2, [](auto x, auto y) { return x && y ? 128.0f : 0.0f; });
    const auto original = nonPlanar.vertices;
    const auto exact = Success(nonPlanar, Atlases());
    CheckGeometry(nonPlanar, exact);
    Require(exact.vertices[0].z == 0 && exact.vertices[1].z == 0 && exact.vertices[2].z == 0
        && exact.vertices[3].z == 128, "不得为非平面四边形重新插值高度");
    for (std::size_t i = 0; i < original.size(); ++i)
        Require(original[i].position == nonPlanar.vertices[i].position, "构建不得修改源网格");
}

void TestVariationAndActualUv() {
    auto terrain = Grid();
    auto atlases = Atlases();
    // 每个用例的期望是显式行列，不复刻被测代码的判断过程。
    const std::array<std::array<std::uint32_t, 3>, 4> square{{{0, 0, 0}, {1, 3, 3}, {16, 3, 3}, {31, 3, 3}}};
    for (const auto& sample : square) {
        terrain.vertices[0].variationRaw = static_cast<std::uint8_t>(0xa0u | sample[0]);
        CheckCell(Success(terrain, atlases), 4, 16, sample[1], sample[2]);
    }
    atlases[0] = MakeAtlas(8, 32);
    const std::array<std::array<std::uint32_t, 3>, 8> extended{{
        {0, 4, 0}, {1, 5, 0}, {3, 7, 0}, {4, 4, 1}, {15, 7, 3}, {16, 3, 3}, {17, 0, 0}, {31, 0, 0}}};
    for (const auto& sample : extended) {
        terrain.vertices[0].variationRaw = static_cast<std::uint8_t>(sample[0]);
        terrain.vertices[3].variationRaw = 7;
        CheckCell(Success(terrain, atlases), 8, 32, sample[1], sample[2]);
    }
    atlases[0] = MakeAtlas(4, 1); terrain.vertices[0].variationRaw = 0;
    const auto tiny = Success(terrain, atlases);
    CheckCell(tiny, 4, 1, 0, 0);
    Near(tiny.vertices[0].u, tiny.vertices[1].u, "单像素单元应夹到唯一像素中心");
}

void TestBaseTextureBatchesAndLifetime() {
    auto terrain = Grid(7, 2);
    const std::array<std::uint8_t, 7> textures{1, 1, 0, 0, 2, 2, 2};
    for (std::size_t i = 0; i < terrain.vertices.size(); ++i) terrain.vertices[i].textureAndFlagsRaw = textures[i % 7];
    terrain.vertices[6].textureAndFlagsRaw |= 0x20;
    auto mesh = Success(terrain, Atlases());
    CheckGeometry(terrain, mesh);
    Require(mesh.texturedTiles == 6 && mesh.fallback.totalTiles == 0 && mesh.fallback.blightTiles == 1
        && mesh.blightApproximationTiles == 1 && mesh.batches.size() == 5,
        "荒芜格保留原地表纹理，过渡层按材质各一批");
    Require(mesh.batches[0].textureIndex == 0 && mesh.batches[0].indexCount == 18
        && mesh.indices[0] == 4 && mesh.indices[6] == 8 && mesh.indices[12] == 12,
        "应选数值最小 ground 索引，并在同纹理批内保持原 tile 顺序");
    Require(mesh.batches[1].textureIndex == 1 && mesh.indices[mesh.batches[1].firstIndex] == 0,
        "批次顺序不得改变顶点到原格的映射");
    Require(mesh.batches[2].textureIndex == 2 && mesh.batches[2].indexCount == 12
        && mesh.mixedGroundTextureTiles == 2, "荒芜不能制造高度色批次，混合材质格须可统计");
    Require(mesh.overlayTiles == std::vector<std::uint32_t>{1, 3} && mesh.batches[3].alphaBlend && mesh.batches[4].alphaBlend
        && mesh.batches[3].textureIndex == 1 && mesh.batches[4].textureIndex == 2,
        "过渡层按材质索引升序、在基础层之后");
    auto shared = MakeAtlas();
    std::weak_ptr<const TerrainAtlasSnapshot> witness = shared;
    std::vector<Atlas> atlases{shared, shared, {}};
    auto adjacent = Grid(3, 2);
    for (auto& vertex : adjacent.vertices) vertex.textureAndFlagsRaw = 1;
    adjacent.vertices[0].textureAndFlagsRaw = 0;
    mesh = Success(adjacent, atlases);
    Require(mesh.textures.size() == 1 && mesh.batches.size() == 2 && mesh.batches[1].alphaBlend
        && mesh.batches[1].textureIndex == 0, "相同 atlas 应去重保活而非复制 RGBA");
    shared.reset(); atlases.clear();
    Require(!witness.expired(), "返回 mesh 必须独立保活贴图");
    mesh.textures.clear();
    Require(witness.expired(), "释放 mesh 后不得有隐藏全局 atlas 缓存");
}

void TestFallbackReasonsAndWater() {
    auto terrain = Grid(); const auto atlases = Atlases();
    terrain.vertices[3].textureAndFlagsRaw = 0x20;
    auto mesh = Success(terrain, atlases);
    Require(mesh.texturedTiles == 1 && mesh.fallback.totalTiles == 0 && mesh.fallback.blightTiles == 1
        && mesh.blightApproximationTiles == 1 && mesh.textures.size() == 1
        && mesh.batches[0].textureIndex == 0, "荒芜保留原地表而不是平坦高度色");
    Near(mesh.vertices[0].red, 1.0f, "未标记角保留原有贴图颜色");
    Near(mesh.vertices[3].red, 0.87f, "荒芜角轻度调色且保留贴图");
    Near(mesh.vertices[3].green, 0.78f, "荒芜角不是统一灰度高度色");
    CheckGeometry(terrain, mesh);
    terrain.vertices[3].textureAndFlagsRaw = 0x10;
    mesh = Success(terrain, atlases);
    Require(mesh.texturedTiles == 1 && mesh.fallback.totalTiles == 0, "坡道标记本身不回退：坡道面按地表贴图显示");
    terrain.vertices[3].textureAndFlagsRaw = 0x40;
    mesh = Success(terrain, atlases);
    Require(mesh.texturedTiles == 1 && mesh.waterFlaggedTiles == 1 && mesh.fallback.totalTiles == 0,
        "水标志仅统计，地面可显示但不生造水面");
    CheckGeometry(terrain, mesh);
    TerrainWaterStyle water{0, {100, 120, 140, 160}, {100, 120, 140, 160},
        {100, 120, 140, 160}, {100, 120, 140, 160}};
    const auto waterOnly = Success(terrain, atlases, {}, kUnlit, &water);
    terrain.vertices[3].textureAndFlagsRaw = 0x60; // 荒芜与水标记并存
    const auto blightedWater = Success(terrain, atlases, {}, kUnlit, &water);
    Require(blightedWater.texturedTiles == 1 && blightedWater.blightApproximationTiles == 1
        && blightedWater.waterTiles == waterOnly.waterTiles && blightedWater.waterTiles.size() == 1,
        "荒芜近似不得吞掉水面");
    for (std::size_t corner = 4; corner < 8; ++corner) {
        Near(blightedWater.vertices[corner].red, waterOnly.vertices[corner].red, "荒芜不得染色水面");
        Near(blightedWater.vertices[corner].green, waterOnly.vertices[corner].green, "荒芜不得改变水面颜色");
        Near(blightedWater.vertices[corner].u, waterOnly.vertices[corner].u, "荒芜不得改变水面透明度 UV");
    }
    auto rampSurface = terrain;
    for (auto& vertex : rampSurface.vertices) vertex.textureAndFlagsRaw = 0x10;
    rampSurface.vertices[3].cliffAndLayerRaw = 3;
    mesh = Success(rampSurface, atlases);
    Require(mesh.texturedTiles == 1 && mesh.fallback.mixedCliffLayerTiles == 0, "四角都是坡道的层级过渡格按坡道面贴图");
    auto edge = Grid(4, 4);
    edge.vertices[0].cliffAndLayerRaw = 3;
    mesh = Success(edge, atlases);
    Require(mesh.fallback.mixedCliffLayerTiles == 1 && mesh.fallback.neighboringCliffTiles == 3
        && mesh.fallback.totalTiles == 4 && mesh.texturedTiles == 5, "地图边缘 cliff 必须有界影响三个邻格，不越界或全图染色");
    CheckGeometry(edge, mesh);
    edge.vertices[0].textureAndFlagsRaw |= 0x20;
    const auto blightedCliff = Success(edge, atlases);
    Require(blightedCliff.fallback.totalTiles == 4 && blightedCliff.fallback.mixedCliffLayerTiles == 1
        && blightedCliff.blightApproximationTiles == 0,
        "荒芜标记不得覆盖原有悬崖回退规则");
    auto middle = Grid(9, 5);
    middle.vertices[2 * 9 + 2].cliffAndLayerRaw = 3;
    mesh = Success(middle, atlases);
    Require(mesh.fallback.mixedCliffLayerTiles == 4 && mesh.fallback.neighboringCliffTiles == 16
        && mesh.fallback.totalTiles == 16 && mesh.texturedTiles == 16, "八邻 cliff 回退计数可重叠，但 total 不重复计数");
    auto flat = Grid(); auto missing = atlases; missing[0].reset();
    mesh = Success(flat, missing);
    Require(mesh.fallback.missingAtlasTiles == 1 && mesh.batches[0].textureIndex == TerrainGroundHeightColor,
        "缺贴图保留高度色，不借用另一张纹理");
    Near(mesh.vertices[0].red, 0.37f, "高度色必须复用现有预览色阶");
    Near(mesh.vertices[0].u, 0, "高度色顶点不得带无效 UV");
    auto bad = MakeAtlas(); bad->cells[0].width += 1; missing[0] = bad;
    mesh = Success(flat, missing);
    Require(mesh.fallback.invalidAtlasTiles == 1, "畸形 atlas 单元不能被悄悄修正");
    flat.vertices[3].textureAndFlagsRaw = 7;
    mesh = Success(flat, atlases);
    Require(mesh.fallback.invalidGroundIndexTiles == 1 && mesh.texturedTiles == 0,
        "任一角无效索引不能被最小合法索引掩盖");
}

void TestOriginalBlightAtlasAndFallback() {
    auto terrain = Grid();
    terrain.vertices[3].textureAndFlagsRaw = 0x20; // NE：原版 mask 4 = (0,1)
    auto blight = MakeAtlas(8, 16);
    const auto actual = Success(terrain, Atlases(), {}, kUnlit, nullptr, blight);
    CheckGeometry(terrain, actual);
    Require(actual.texturedTiles == 1 && actual.blightTexturedTiles == 1
        && actual.blightApproximationTiles == 0 && actual.overlayTiles == std::vector<std::uint32_t>{0}
        && actual.textures.size() == 2 && actual.batches.size() == 2,
        "原版荒芜必须在保留基础地表的同时绘制独立遮罩层");
    Require(actual.batches[0].textureIndex == 0 && !actual.batches[0].alphaBlend
        && actual.batches[1].textureIndex == 1 && actual.batches[1].alphaBlend,
        "荒芜遮罩必须在基础层之后透明混合");
    Near(actual.vertices[3].red, 1.0f, "使用原版荒芜时底图不能再叠加近似色调");
    Near(actual.vertices[4].u, 0.5f / 128.0f, "NE 遮罩必须使用第 4 格的左界");
    Near(actual.vertices[4].v, 31.5f / 64.0f, "NE 遮罩必须使用第 4 格的南界");
    for (auto& vertex : terrain.vertices) vertex.textureAndFlagsRaw |= 0x20;
    terrain.vertices[0].variationRaw = 1;
    const auto full = Success(terrain, Atlases(), {}, kUnlit, nullptr, blight);
    Require(full.blightTexturedTiles == 1 && full.overlayTiles.size() == 1,
        "四角荒芜也须使用原版贴图");
    Near(full.vertices[4].u, 80.5f / 128.0f, "全荒芜格应从右侧基础变体区取第 1 变体");
    Near(full.vertices[4].v, 15.5f / 64.0f, "全荒芜基础变体须保持 SW 朝下的 UV");

    TerrainGroundMeshLimits oneTexture;
    oneTexture.maxTextures = 1;
    const auto limited = Success(terrain, Atlases(), oneTexture, kUnlit, nullptr, blight);
    Require(limited.blightTexturedTiles == 0 && limited.blightApproximationTiles == 1
        && limited.textures.size() == 1 && limited.overlayTiles.empty(),
        "荒芜 atlas 无纹理槽时应退化为既有地表近似而不让整图消失");
    Near(limited.vertices[0].red, 0.87f, "荒芜预算回退必须保留逐角色调");
    const auto malformed = Success(terrain, Atlases(), {}, kUnlit, nullptr, MakeAtlas(4));
    Require(malformed.blightTexturedTiles == 0 && malformed.blightApproximationTiles == 1,
        "4x4 atlas 没有已验证的基础变体区，不得冒充原版荒芜材质");

    terrain.vertices[3].textureAndFlagsRaw |= 0x40;
    TerrainWaterStyle water{0, {100, 120, 140, 160}, {100, 120, 140, 160},
        {100, 120, 140, 160}, {100, 120, 140, 160}};
    const auto layered = Success(terrain, Atlases(), {}, kUnlit, &water, blight);
    CheckGeometry(terrain, layered);
    Require(layered.blightTexturedTiles == 1 && layered.waterTiles.size() == 1
        && layered.batches.size() == 3 && layered.batches[1].alphaBlend
        && !layered.batches[1].translucent && layered.batches[2].translucent,
        "真实荒芜必须在水面之前绘制且不吞掉水面");
    TerrainGroundMeshLimits twoTextures;
    twoTextures.maxTextures = 2;
    const auto limitedWater = Success(terrain, Atlases(), twoTextures, kUnlit, &water, blight);
    Require(limitedWater.blightTexturedTiles == 0 && limitedWater.blightApproximationTiles == 1
        && limitedWater.waterTiles.size() == 1 && !limitedWater.waterOmitted,
        "槽位不足时优先保留已有水面，并明确退化荒芜材质");
}

void TestRetainedMipBudgets() {
    auto atlas = MakeAtlas(4, 2);
    atlas->additionalMips.push_back({4, 4, std::vector<std::byte>(64, std::byte{93})});
    const std::vector<Atlas> atlases{atlas, nullptr, nullptr};
    const auto terrain = Grid();
    TerrainGroundMeshLimits limits;
    limits.maxRgbaBytes = 319;
    Rejected(terrain, atlases, "TERRAIN_GROUND_BUDGET", limits);
    limits.maxRgbaBytes = 320;
    Require(Success(terrain, atlases, limits).textures.front()->additionalMips.front().rgba.front() == std::byte{93},
        "保留的原始 mip 被遗漏或改写");
    limits = {}; limits.maxTextureChainBytes = 319;
    Rejected(terrain, atlases, "TERRAIN_GROUND_BUDGET", limits);
    atlas->additionalMips.front().width = 3;
    const auto malformed = Success(terrain, atlases);
    Require(malformed.texturedTiles == 0 && malformed.fallback.invalidAtlasTiles == 1,
        "畸形 mip 链没有使相关 tile 回退高度色");
}

void TestBudgetsAndInvalidInputs() {
    const auto terrain = Grid(); const auto atlases = Atlases();
    TerrainGroundMeshLimits limits;
    for (int budget = 0; budget < 8; ++budget) {
        limits = {};
        switch (budget) {
        case 0: limits.maxVertices = 3; break;
        case 1: limits.maxIndices = 5; break;
        case 2: limits.maxTextures = 0; break;
        case 3: limits.maxTextureDimension = 63; break;
        case 4: limits.maxTextureBytes = atlases[0]->image.rgba.size() - 1; break;
        case 5: limits.maxRgbaBytes = atlases[0]->image.rgba.size() - 1; break;
        case 6: limits.maxBatches = 0; break;
        default: limits.maxWorkingBytes = 1; break;
        }
        Rejected(terrain, atlases, "TERRAIN_GROUND_BUDGET", limits);
    }
    limits = {}; limits.maxVertices += 1;
    Rejected(terrain, atlases, "TERRAIN_GROUND_LIMITS", limits);
    Rejected(Grid(514, 514), atlases, "TERRAIN_GROUND_BUDGET");
    auto invalid = terrain;
    invalid.tiles[0].vertices[2] = 3;
    Rejected(invalid, atlases, "TERRAIN_GROUND_GRID");
    invalid = terrain; invalid.vertices[0].position[2] = std::numeric_limits<float>::quiet_NaN();
    Rejected(invalid, atlases, "TERRAIN_GROUND_GRID");
    invalid = terrain; invalid.vertices[0].position[0] += 0.25f;
    Rejected(invalid, atlases, "TERRAIN_GROUND_GRID");
    invalid = terrain; invalid.minimum[2] = -1;
    Rejected(invalid, atlases, "TERRAIN_GROUND_GRID");
    auto wrongOrderSize = atlases; wrongOrderSize.pop_back();
    Rejected(terrain, wrongOrderSize, "TERRAIN_GROUND_GRID");
    limits = {}; limits.maxTextures = 0;
    Require(Success(terrain, std::vector<Atlas>(3), limits).fallback.totalTiles == 1,
        "没有真正使用纹理时，零纹理预算仍允许原格高度色");

    // 单格没有 RGBA 或保活槽：原格载荷与批次 213B，固定材质/分组 scratch 4812B（含过渡层分组表）。
    // 旧预算只计前者，213B 即会错误成功；临界点必须包含全部固定表。
    const std::vector<Atlas> noAtlases(3);
    limits = {}; limits.maxWorkingBytes = 213;
    Rejected(terrain, noAtlases, "TERRAIN_GROUND_BUDGET", limits);
    limits.maxWorkingBytes = 5024;
    Rejected(terrain, noAtlases, "TERRAIN_GROUND_BUDGET", limits);
    limits.maxWorkingBytes = 5025;
    const auto exactBudget = Success(terrain, noAtlases, limits);
    Require(exactBudget.fallback.totalTiles == 1 && exactBudget.textures.empty(),
        "恰好容纳固定 scratch 的预算应保留完整高度格");
    CheckGeometry(terrain, exactBudget);
}

void TestBlendLayers() {
    // SW=0、SE=1、NW=2、NE=1：基础层 0；过渡层 1 在东南+东北（遮罩 1|4=5），过渡层 2 在西北（遮罩 8）。
    auto terrain = Grid();
    const std::array<std::uint8_t, 4> grounds{0, 1, 2, 1};
    for (std::size_t i = 0; i < 4; ++i) terrain.vertices[i].textureAndFlagsRaw = grounds[i];
    const auto atlases = Atlases();
    const auto mesh = Success(terrain, atlases);
    CheckGeometry(terrain, mesh);
    Require(mesh.texturedTiles == 1 && mesh.mixedGroundTextureTiles == 1 && mesh.overlayTiles.size() == 2
        && mesh.batches.size() == 3 && mesh.batches[1].textureIndex == 1 && mesh.batches[2].textureIndex == 2,
        "三种材质应得到基础层加两层过渡层");
    const auto checkMask = [&](std::size_t quad, const TerrainAtlasSnapshot& atlas, std::uint32_t mask) {
        const auto cellSize = atlas.image.height / 4;
        const auto width = static_cast<float>(atlas.image.width), height = static_cast<float>(atlas.image.height);
        const auto column = mask % 4, row = mask / 4;
        Near(mesh.vertices[quad * 4].u, (static_cast<float>(column * cellSize) + 0.5f) / width, "遮罩格列号错误");
        Near(mesh.vertices[quad * 4].v, (static_cast<float>((row + 1) * cellSize) - 0.5f) / height, "遮罩格行号错误");
        Near(mesh.vertices[quad * 4 + 3].u, (static_cast<float>((column + 1) * cellSize) - 0.5f) / width, "遮罩格右界错误");
    };
    checkMask(1, *atlases[1], 5);
    checkMask(2, *atlases[2], 8);
    // 同一已验证混合布局改由派生索引提供，原始四角仍全为0；基础/过渡UV必须一致。
    const auto original = Grid();
    const auto selected = BuildTerrainGroundMesh(original, atlases, {}, kUnlit, nullptr, {}, {}, grounds);
    Require(selected.Succeeded() && selected.value->vertices.size() == mesh.vertices.size()
        && selected.value->overlayTiles == mesh.overlayTiles && selected.value->mixedGroundTextureTiles == 1,
        "派生索引必须驱动基础及全部过渡层");
    for (std::size_t i = 0; i < mesh.vertices.size(); ++i) {
        Near(selected.value->vertices[i].u, mesh.vertices[i].u, "派生遮罩U");
        Near(selected.value->vertices[i].v, mesh.vertices[i].v, "派生遮罩V");
    }
    for (const auto& vertex : original.vertices) Require(vertex.GroundTextureIndex() == 0, "原始地面索引未被改写");
    const auto truncated = std::span<const std::uint8_t>(grounds).first(3);
    Require(!BuildTerrainGroundMesh(original, atlases, {}, kUnlit, nullptr, {}, {}, truncated).Succeeded(),
        "不接受长度不完整的派生索引");
    auto invalid = grounds; invalid[1] = 99;
    Require(!BuildTerrainGroundMesh(original, atlases, {}, kUnlit, nullptr, {}, {}, invalid).Succeeded(),
        "派生索引不得越过原palette");
    auto unknown = original; unknown.vertices[0].textureAndFlagsRaw = 15;
    const std::array<std::uint8_t, 4> retainedRaw{15, 0, 0, 0};
    const auto rawFallback = Success(unknown, atlases);
    const auto retained = BuildTerrainGroundMesh(unknown, atlases, {}, kUnlit, nullptr, {}, {}, retainedRaw);
    Require(retained.Succeeded() && retained.value->fallback.invalidGroundIndexTiles == 1
        && retained.value->fallback.totalTiles == rawFallback.fallback.totalTiles
        && retained.value->texturedTiles == rawFallback.texturedTiles,
        "未知原raw原样透传必须保持逐tile高度色回退，不能拒绝整个地面");
    // 荒芜角不吞掉已有的地表混合；基础层和过渡层使用同一轻度调色。
    terrain.vertices[3].textureAndFlagsRaw |= 0x20;
    const auto blighted = Success(terrain, atlases);
    CheckGeometry(terrain, blighted);
    Require(blighted.texturedTiles == 1 && blighted.blightApproximationTiles == 1
        && blighted.fallback.totalTiles == 0 && blighted.overlayTiles == mesh.overlayTiles,
        "混合地表上的荒芜仍保留原过渡层");
    Near(blighted.vertices[3].red, 0.87f, "基础层荒芜角轻度调色");
    Near(blighted.vertices[7].red, 0.87f, "过渡层荒芜角轻度调色");
    // 过渡层所用材质缺图：整格保留高度色，不只画基础层。
    auto missing = atlases; missing[2].reset();
    const auto fallback = Success(terrain, missing);
    Require(fallback.fallback.missingAtlasTiles == 1 && fallback.texturedTiles == 0
        && fallback.blightApproximationTiles == 0 && fallback.overlayTiles.empty(),
        "过渡层缺图必须整格回退");
    // 四角同材质没有过渡层。
    Require(Success(Grid(), atlases).overlayTiles.empty(), "单一材质不应产生过渡层");
}

void TestEveryTransitionMask() {
    // 独立列举物理角的材料与 atlas 格坐标，不在 oracle 内重新计算 CornerMask。
    // 每个原创 atlas 只在该格写入带方向的四角 alpha，其余格为黑色不透明毒值。
    struct Case { std::array<std::uint8_t, 4> swSeNwNe; std::uint32_t column, row; };
    constexpr std::array<Case, 14> cases{{
        {{{0, 1, 0, 0}}, 1, 0}, {{{1, 0, 0, 0}}, 2, 0}, {{{1, 1, 0, 0}}, 3, 0},
        {{{0, 0, 0, 1}}, 0, 1}, {{{0, 1, 0, 1}}, 1, 1}, {{{1, 0, 0, 1}}, 2, 1},
        {{{1, 1, 0, 1}}, 3, 1}, {{{0, 0, 1, 0}}, 0, 2}, {{{0, 1, 1, 0}}, 1, 2},
        {{{1, 0, 1, 0}}, 2, 2}, {{{1, 1, 1, 0}}, 3, 2}, {{{0, 0, 1, 1}}, 0, 3},
        {{{0, 1, 1, 1}}, 1, 3}, {{{1, 0, 1, 1}}, 2, 3}}};
    for (const auto columns : {4u, 8u}) for (const auto& item : cases) {
        auto terrain = Grid();
        for (std::size_t corner = 0; corner < 4; ++corner)
            terrain.vertices[corner].textureAndFlagsRaw = item.swSeNwNe[corner];
        auto atlas = MakeAtlas(columns, 8);
        for (std::size_t pixel = 0; pixel < atlas->image.rgba.size(); pixel += 4) {
            atlas->image.rgba[pixel] = atlas->image.rgba[pixel + 1] = atlas->image.rgba[pixel + 2] = std::byte{0};
        }
        const auto& cell = *atlas->Cell(item.column, item.row);
        for (std::uint32_t y = 0; y < cell.height; ++y) for (std::uint32_t x = 0; x < cell.width; ++x) {
            const float east = static_cast<float>(x) / static_cast<float>(cell.width - 1);
            const float south = static_cast<float>(y) / static_cast<float>(cell.height - 1);
            const auto northAlpha = std::lerp(static_cast<float>(item.swSeNwNe[2]), static_cast<float>(item.swSeNwNe[3]), east);
            const auto southAlpha = std::lerp(static_cast<float>(item.swSeNwNe[0]), static_cast<float>(item.swSeNwNe[1]), east);
            const auto pixel = (static_cast<std::size_t>(cell.y + y) * atlas->image.width + cell.x + x) * 4;
            atlas->image.rgba[pixel] = std::byte{47}; atlas->image.rgba[pixel + 1] = std::byte{139};
            atlas->image.rgba[pixel + 2] = std::byte{211};
            atlas->image.rgba[pixel + 3] = static_cast<std::byte>(std::lround(std::lerp(northAlpha, southAlpha, south) * 255));
        }
        const auto mesh = Success(terrain, {MakeAtlas(), atlas, MakeAtlas()});
        CheckGeometry(terrain, mesh);
        Require(mesh.overlayTiles == std::vector<std::uint32_t>{0} && mesh.batches.size() == 2
            && mesh.batches.back().alphaBlend && mesh.batches.back().textureIndex == 1,
            "每类局部蒙版都应提交一个有 alpha 混合标志的过渡层");
        for (std::size_t corner = 0; corner < 4; ++corner) {
            const auto& vertex = mesh.vertices[4 + corner];
            const auto x = static_cast<std::uint32_t>(std::lround(vertex.u * static_cast<float>(atlas->image.width) - 0.5f));
            const auto y = static_cast<std::uint32_t>(std::lround(vertex.v * static_cast<float>(atlas->image.height) - 0.5f));
            Require(x < atlas->image.width && y < atlas->image.height, "蒙版角 UV 不得越界");
            const auto pixel = (static_cast<std::size_t>(y) * atlas->image.width + x) * 4;
            Require(atlas->image.rgba[pixel] == std::byte{47} && atlas->image.rgba[pixel + 1] == std::byte{139}
                && atlas->image.rgba[pixel + 2] == std::byte{211}, "蒙版选格错误或采到了邻格");
            Require(std::to_integer<unsigned>(atlas->image.rgba[pixel + 3]) == item.swSeNwNe[corner] * 255u,
                "蒙版物理角 alpha 方向错误（SW/SE/NW/NE），不可翻转或旋转");
        }
    }
}

void TestCoveredBaseCompactionPreservesTransitions() {
    auto terrain = Grid(4, 2);
    terrain.vertices[1].textureAndFlagsRaw = 1;
    terrain.vertices[7].textureAndFlagsRaw = 1;
    const auto atlases = Atlases();
    const std::array<std::uint8_t, 3> covered{0, 1, 0};
    TerrainGroundMeshLimits limits;
    limits.maxVertices = 16; limits.maxIndices = 24; // 两个可见基础格 + 两个过渡层，恰好容纳。
    auto actual = BuildTerrainGroundMesh(terrain, atlases, limits, kUnlit, nullptr, covered, {}, {},
        TerrainGroundBudgetPolicy::PreserveBasePreview);
    Require(actual.Succeeded(), "悬崖覆盖格不应继续占用不可见基础顶点预算");
    const auto& mesh = *actual.value;
    CheckGeometry(terrain, mesh);
    Require(mesh.baseTiles == std::vector<std::uint32_t>{0, 2} && mesh.hiddenTiles == std::vector<std::uint8_t>{0, 1, 0}
        && mesh.hiddenTileCount == 1, "紧缩基础层必须保留原格身份并完整跳过悬崖覆盖格");
    Require(mesh.vertices.size() == 16 && mesh.indices.size() == 24 && mesh.overlayTiles == std::vector<std::uint32_t>{0, 2}
        && mesh.budget.base.requested == 2 && mesh.budget.base.displayed == 2 && mesh.budget.base.omitted == 0
        && mesh.budget.transitions.requested == 2 && mesh.budget.transitions.displayed == 2
        && mesh.budget.transitions.omitted == 0 && !mesh.budget.reduced,
        "同一预算必须保留完整两层过渡，不能只恢复部分地图");
    const auto strict = BuildTerrainGroundMesh(terrain, atlases, limits, kUnlit, nullptr, covered);
    Require(strict.Succeeded() && strict.value->indices == mesh.indices && strict.value->vertices.size() == mesh.vertices.size(),
        "紧缩是相同几何表示，不应只在允许降级的预览策略中生效");
    const auto noCoverage = BuildTerrainGroundMesh(terrain, atlases, limits, kUnlit, nullptr, {}, {}, {},
        TerrainGroundBudgetPolicy::PreserveBasePreview);
    Require(noCoverage.Succeeded() && noCoverage.value->baseTiles.empty() && noCoverage.value->hiddenTiles.empty()
        && noCoverage.value->budget.transitions.displayed == 0 && noCoverage.value->budget.transitions.omitted == 3
        && noCoverage.value->budget.reduced, "没有已准备覆盖时不能擅自隐藏基础格以通过预算");
    limits.maxVertices = 8;
    const auto stillLimited = BuildTerrainGroundMesh(terrain, atlases, limits, kUnlit, nullptr, covered, {}, {},
        TerrainGroundBudgetPolicy::PreserveBasePreview);
    Require(stillLimited.Succeeded() && stillLimited.value->budget.transitions.omitted == 2
        && stillLimited.value->budget.reduced, "紧缩后仍超预算时必须保留明确的整类降级统计");
}

void TestWaterColorBounds() {
    // 两端相同的满强度通道应保持 1；旧的双乘加在部分浅水深度会得到 1+1ULP，
    // 从而使渲染插件拒绝包含水面的整份地表。使用原创颜色，不依赖游戏资源。
    TerrainWaterStyle style{0, {255, 255, 255, 0}, {255, 255, 255, 255},
        {255, 255, 255, 255}, {255, 255, 255, 0}};
    for (int quarter = 0; quarter <= 320; ++quarter) {
        const auto color = TerrainWaterColor(style, static_cast<float>(quarter) / 4.0f);
        for (std::size_t channel = 0; channel < 3; ++channel)
            Require(color[channel] == 1.0f, "相同的水面颜色端点必须保持原值，不能越过渲染颜色范围");
        Require(std::isfinite(color[3]) && color[3] >= 0.0f && color[3] <= 1.0f,
            "浅水及深水插值的透明度必须保持在归一化范围内");
    }
    auto terrain = Grid(3, 3);
    for (auto& vertex : terrain.vertices) {
        vertex.textureAndFlagsRaw |= 0x40;
        vertex.waterBaseHeight = 12.0f; // alpha 量化为第 1 格，保持此颜色回归实际覆盖水面输出。
    }
    const auto mesh = Success(terrain, Atlases(), {}, kUnlit, &style);
    Require(mesh.waterTiles.size() == terrain.tiles.size() && !mesh.waterOmitted,
        "颜色边界回归必须实际产生全部水面格，不能通过省略水面绕过");
    for (const auto& vertex : mesh.vertices)
        Require(std::isfinite(vertex.red) && vertex.red >= 0.0f && vertex.red <= 1.0f
            && std::isfinite(vertex.green) && vertex.green >= 0.0f && vertex.green <= 1.0f
            && std::isfinite(vertex.blue) && vertex.blue >= 0.0f && vertex.blue <= 1.0f,
            "有水面时地表输出仍须满足渲染插件的顶点颜色要求");
}

void TestWater() {
    // Water.slk 洛丹伦夏季一行的颜色与高度偏移（原版数值，写成原创测试常量）。
    TerrainWaterStyle style{-89.6f, {255, 255, 255, 10}, {117, 117, 200, 219}, {117, 117, 200, 219}, {96, 96, 192, 250}};
    const auto near = [](const std::array<float, 4>& color, const std::array<std::uint8_t, 4>& expected, const char* message) {
        for (std::size_t channel = 0; channel < 4; ++channel) Near(color[channel], expected[channel] / 255.0f, message);
    };
    near(TerrainWaterColor(style, 0), style.shallowMinimum, "岸边取浅水最小色");
    near(TerrainWaterColor(style, 10), style.shallowMinimum, "10 以内仍为浅水最小色");
    near(TerrainWaterColor(style, 64), style.shallowMaximum, "64 为浅水最大色");
    near(TerrainWaterColor(style, 200), style.deepMaximum, "72 以上为深水最大色");
    Near(TerrainWaterColor(style, 68)[3], (219.0f + 250.0f) / 2 / 255.0f, "深水区间线性插值");

    // 3×3 平地，西南角一个顶点有水：只有 tile 0 生成水面 quad。
    auto terrain = Grid(3, 3);
    terrain.vertices[0].textureAndFlagsRaw |= 0x40;
    for (auto& vertex : terrain.vertices) vertex.waterBaseHeight = 89.6f + 40.0f; // 水面 40
    const auto atlases = Atlases();
    const auto mesh = Success(terrain, atlases, {}, kUnlit, &style);
    CheckGeometry(terrain, mesh);
    Require(mesh.waterTiles == std::vector<std::uint32_t>{0} && !mesh.waterOmitted && mesh.waterFlaggedTiles == 1,
        "有水角的格子才生成水面");
    const auto& batch = mesh.batches.back();
    Require(batch.alphaBlend && batch.translucent && batch.textureIndex == mesh.textures.size() - 1 && batch.indexCount == 6,
        "水面批次最后绘制并按透明度混合");
    const auto& water = *mesh.textures.back();
    Require(water.columns == 4 && water.image.width == 128 && water.cells.size() == 16, "合成水面 atlas 为 4×4 格");
    // 无贴图时为白色；第 15 格不透明，第 0 格全透明。
    Require(water.image.rgba[0] == std::byte{255} && water.image.rgba[3] == std::byte{0}
        && water.image.rgba.back() == std::byte{255}, "合成 atlas 透明度分级错误");
    const auto first = mesh.vertices.size() - 4;
    const auto expected = TerrainWaterColor(style, 40);
    for (std::size_t corner = 0; corner < 4; ++corner) {
        Require(std::abs(mesh.vertices[first + corner].z - 40.0f) < 1e-3f, "水面高度 = waterBaseHeight + 样式偏移");
        Near(mesh.vertices[first + corner].blue, expected[2], "水面颜色按深度");
    }
    // 透明度 = 四角平均（均为深度 40）量化到 16 级，UV 落在对应格内。
    const auto level = static_cast<std::uint32_t>(std::lround(expected[3] * 15.0f));
    const auto& cell = *water.Cell(level % 4, level / 4);
    Near(mesh.vertices[first].u, (static_cast<float>(cell.x) + 0.5f) / 128.0f, "水面透明度格列号错误");
    Near(mesh.vertices[first].v, (static_cast<float>(cell.y + cell.height) - 0.5f) / 128.0f, "水面透明度格行号错误");
    // 有水面贴图时颜色取自贴图，透明度 = 贴图 alpha × 级别。
    auto textured = style;
    textured.texture = std::make_shared<const Image::DecodedImage>(Image::DecodedImage{2, 2,
        std::vector<std::byte>{std::byte{10}, std::byte{20}, std::byte{30}, std::byte{255}, std::byte{10}, std::byte{20}, std::byte{30}, std::byte{255},
            std::byte{10}, std::byte{20}, std::byte{30}, std::byte{255}, std::byte{10}, std::byte{20}, std::byte{30}, std::byte{255}}});
    textured.frameIndex = 1; textured.frameCount = 2;
    textured.frameSourcePath = "WATER\\FLOW01.BLP"; textured.frameSourceSha256 = "frame-1-fixture";
    const auto withTexture = Success(terrain, atlases, {}, kUnlit, &textured);
    const auto& texturedAtlas = *withTexture.textures.back();
    Require(texturedAtlas.image.rgba[0] == std::byte{10} && texturedAtlas.image.rgba[2] == std::byte{30}
        && texturedAtlas.image.rgba.back() == std::byte{255}
        && withTexture.waterFrameIndex == 1 && withTexture.waterFrameCount == 2
        && withTexture.waterFrameSourcePath == "WATER\\FLOW01.BLP"
        && withTexture.waterFrameSourceSha256 == "frame-1-fixture", "合成 atlas 必须取所选水面帧颜色并保留身份");
    textured.frameIndex = 2;
    const auto invalidFrame = BuildTerrainGroundMesh(terrain, atlases, {}, kUnlit, &textured);
    Require(!invalidFrame.Succeeded() && !invalidFrame.value
        && invalidFrame.diagnostics.front().code == "TERRAIN_GROUND_WATER_FRAME", "越界帧不得发布部分水面网格");

    // 没有样式：不画水面但如实标记；纹理槽满：同样标记。
    Require(Success(terrain, atlases).waterOmitted && Success(terrain, atlases).waterTiles.empty(), "缺样式必须标记未显示");
    TerrainGroundMeshLimits oneTexture; oneTexture.maxTextures = 1;
    const auto full = Success(terrain, atlases, oneTexture, kUnlit, &style);
    Require(full.waterOmitted && full.waterTiles.empty() && full.textures.size() == 1, "纹理槽满时不画水面");
}

void TestWaterZeroContribution() {
    auto terrain = Grid();
    for (auto& vertex : terrain.vertices) { vertex.textureAndFlagsRaw |= 0x40; vertex.waterBaseHeight = 40; }
    const auto original = terrain.vertices;
    const auto atlases = Atlases();
    const auto styleFor = [](std::uint8_t alpha) {
        const std::array<std::uint8_t, 4> color{73, 151, 219, alpha};
        return TerrainWaterStyle{0, color, color, color, color};
    };
    const auto sourceFor = [](std::uint32_t width, std::uint32_t height, std::uint8_t alpha) {
        auto source = std::make_shared<Image::DecodedImage>(Image::DecodedImage{width, height,
            std::vector<std::byte>(static_cast<std::size_t>(width) * height * 4, std::byte{231})});
        for (std::size_t at = 3; at < source->rgba.size(); at += 4) source->rgba[at] = static_cast<std::byte>(alpha);
        return source;
    };
    const auto counts = [&terrain](const TerrainGroundMesh& mesh, std::size_t zero, std::size_t shown, std::size_t omitted) {
        const auto& water = mesh.budget.water;
        Require(water.noContribution == zero && water.displayed == shown && water.omitted == omitted
            && water.requested == zero + shown + omitted && water.requested == mesh.waterFlaggedTiles
            && water.displayed == mesh.waterTiles.size() && mesh.waterOmitted == (omitted != 0),
            "水面源请求必须守恒，零贡献不能冒充显示或质量省略");
        CheckGeometry(terrain, mesh);
    };
    auto style = styleFor(255); style.texture = sourceFor(3, 5, 0);
    auto mesh = Success(terrain, atlases, {}, kUnlit, &style);
    counts(mesh, 1, 0, 0);
    Require(mesh.vertices.size() == 4 && mesh.indices.size() == 6 && mesh.textures.size() == 1
        && !mesh.waterTextureIndex && mesh.waterFrameCount == 0 && !mesh.budget.reduced,
        "全零水面不分配atlas或几何，也不产生质量降级");
    TerrainGroundMeshLimits occupiedSlot; occupiedSlot.maxTextures = 1;
    counts(Success(terrain, atlases, occupiedSlot, kUnlit, &style), 1, 0, 0);
    // 8/255×15 仍不到半级；9/255 则选第1格。白色 fallback 的非零邻 cell 不允许剔除。
    for (const std::uint8_t alpha : {std::uint8_t{8}, std::uint8_t{9}, std::uint8_t{255}}) {
        style = styleFor(alpha);
        mesh = Success(terrain, atlases, {}, kUnlit, &style);
        counts(mesh, 0, 1, 0);
        const auto at = (alpha == 8 ? 0u : alpha == 9 ? 32u : 96u * 128u + 96u) * 4 + 3;
        Require(mesh.textures[*mesh.waterTextureIndex]->image.rgba[at] == static_cast<std::byte>(alpha == 8 ? 0 : alpha == 9 ? 17 : 255),
            "selected cell 的量化零值不能许可 legacy 删除仍有非零邻 cell 的水 draw");
    }
    // 原整数舍入边界不变；即使 selected cell 为零，源有非零样本仍必须保留 draw。
    for (const auto [styleAlpha, sourceAlpha, selectedLevel, cellZero] : std::array<std::array<std::uint8_t, 4>, 4>{{
            {17, 7, 1, 1}, {17, 8, 1, 0}, {119, 1, 7, 1}, {136, 1, 8, 0}}}) {
        style = styleFor(styleAlpha); style.texture = sourceFor(3, 5, sourceAlpha);
        mesh = Success(terrain, atlases, {}, kUnlit, &style);
        counts(mesh, 0, 1, 0);
        const auto& atlas = *mesh.textures[*mesh.waterTextureIndex];
        Require(atlas.additionalMips.empty() && atlas.validatedMipCount == 1,
            "共享合成 atlas 仍保持原单 mip 字节");
        for (std::uint32_t y = 0; y < 128; ++y) for (std::uint32_t x = 0; x < 128; ++x) {
            const auto level = (y / 32) * 4 + x / 32;
            const auto at = (static_cast<std::size_t>(y) * 128 + x) * 4;
            Require(atlas.image.rgba[at] == std::byte{231}
                && std::to_integer<std::uint32_t>(atlas.image.rgba[at + 3]) == (level * sourceAlpha + 7) / 15,
                "共享helper不得改动合成atlas字节或整数alpha舍入");
        }
        const auto selectedAt = ((selectedLevel / 4 * 32) * 128 + selectedLevel % 4 * 32) * 4 + 3;
        Require((atlas.image.rgba[selectedAt] == std::byte{0}) == (cellZero != 0),
            "量化边界负控须确实覆盖 selected cell 零与 alpha1，不能放宽到近零阈值");
    }
    style = styleFor(255);
    auto sparse = sourceFor(64, 64, 0);
    sparse->rgba[(1 * 64 + 1) * 4 + 3] = std::byte{255}; // 32²最近邻从64²只取偶数坐标。
    const auto sparseOriginal = sparse->rgba;
    style.texture = sparse;
    counts(Success(terrain, atlases, {}, kUnlit, &style), 1, 0, 0);
    Require(sparse->rgba == sparseOriginal, "分类不得修改原始水面帧");
    sparse->rgba[(2 * 64 + 2) * 4 + 3] = std::byte{1};
    counts(Success(terrain, atlases, {}, kUnlit, &style), 0, 1, 0);
    // 缺style保持质量省略；非法source即使style alpha0仍按旧输入合同拒绝。
    counts(Success(terrain, atlases), 0, 0, 1);
    style = styleFor(0); style.texture = std::make_shared<Image::DecodedImage>(Image::DecodedImage{2, 2, {}});
    auto rejected = BuildTerrainGroundMesh(terrain, atlases, {}, kUnlit, &style);
    Require(!rejected.Succeeded() && !rejected.value && rejected.diagnostics.front().code == "TERRAIN_GROUND_WATER_FRAME",
        "零alpha不能绕过无效水源布局/字节拒绝");
    style = styleFor(0); style.texture = sourceFor(3, 5, 0);
    for (const auto invalidHeight : {std::numeric_limits<float>::quiet_NaN(), 10000001.0f, -10000001.0f}) {
        auto invalid = terrain; invalid.vertices[0].waterBaseHeight = invalidHeight;
        for (const auto covered : {std::uint8_t{0}, std::uint8_t{1}}) {
            const std::array<std::uint8_t, 1> coverage{covered};
            rejected = BuildTerrainGroundMesh(invalid, atlases, {}, kUnlit, &style, coverage);
            Require(!rejected.Succeeded() && !rejected.value
                && rejected.diagnostics.front().code == "TERRAIN_GROUND_WATER_HEIGHT",
                "零贡献/悬崖覆盖仍必须校验每个有水原格的全部水面高度");
        }
    }
    const std::array<std::uint8_t, 1> covered{1};
    const auto waterOnly = BuildTerrainGroundMesh(terrain, atlases, {}, kUnlit, &style, covered);
    Require(waterOnly.Succeeded() && waterOnly.value->budget.water.noContribution == 0
        && waterOnly.value->waterTiles.size() == 1 && waterOnly.value->indices.size() == 6
        && waterOnly.value->waterTextureIndex && !waterOnly.value->waterOmitted,
        "全部基础格被覆盖时保持原零alpha水draw，不能变成默认网格空场景");
    // 同一非零源帧的深度零/非零格都保留，不能按 selected cell 分别删除。
    auto pair = Grid(3, 2);
    for (std::size_t i = 0; i < pair.vertices.size(); ++i) {
        pair.vertices[i].textureAndFlagsRaw |= 0x40; pair.vertices[i].waterBaseHeight = i % 3 == 2 ? 64.0f : 0.0f;
    }
    style = TerrainWaterStyle{0, {255,255,255,0}, {255,255,255,255}, {255,255,255,255}, {255,255,255,255}};
    const auto partial = Success(pair, atlases, {}, kUnlit, &style);
    Require(partial.budget.water.requested == 2 && partial.budget.water.noContribution == 0
        && partial.budget.water.displayed == 2 && partial.budget.water.omitted == 0
        && partial.waterTiles == std::vector<std::uint32_t>{0,1} && !partial.waterOmitted,
        "非零源帧的深度零/非零格都保留原 draw、原格身份及相对顺序");
    CheckGeometry(pair, partial);
    TerrainGroundMeshLimits oneSlot; oneSlot.maxTextures = 1;
    const auto full = Success(pair, atlases, oneSlot, kUnlit, &style);
    Require(full.budget.water.requested == 2 && full.budget.water.noContribution == 0
        && full.budget.water.displayed == 0 && full.budget.water.omitted == 2 && full.waterOmitted
        && full.budget.water.omissionReasons == static_cast<std::uint32_t>(TerrainGroundOmissionReason::TextureSlots),
        "槽满也不能把仅 selected cell 为零的真实省略混入零贡献");
    TerrainGroundMeshLimits baseOnly; baseOnly.maxVertices = 8;
    const auto limited = BuildTerrainGroundMesh(pair, atlases, baseOnly, kUnlit, &style, {}, {}, {},
        TerrainGroundBudgetPolicy::PreserveBasePreview);
    Require(limited.Succeeded() && limited.value->budget.water.requested == 2
        && limited.value->budget.water.noContribution == 0 && limited.value->budget.water.omitted == 2
        && limited.value->budget.water.displayed == 0 && limited.value->waterOmitted && limited.value->budget.reduced
        && (limited.value->budget.water.omissionReasons & static_cast<std::uint32_t>(TerrainGroundOmissionReason::Vertices)),
        "几何预算仍整类省略两个未证明全 atlas 为零的原格");
    style.texture = sourceFor(3, 5, 0);
    const auto wholeZero = Success(pair, atlases, oneSlot, kUnlit, &style);
    Require(wholeZero.budget.water.requested == 2 && wholeZero.budget.water.noContribution == 2
        && wholeZero.budget.water.displayed == 0 && wholeZero.budget.water.omitted == 0
        && wholeZero.waterTiles.empty() && !wholeZero.waterTextureIndex && !wholeZero.waterOmitted,
        "实际源样本全零时两个深度格都无需纹理槽，不能伪造质量省略或 rendered 身份");
    CheckGeometry(pair, wholeZero);
    auto blighted = terrain;
    for (auto& vertex : blighted.vertices) vertex.textureAndFlagsRaw |= 0x20;
    TerrainGroundMeshLimits twoSlots; twoSlots.maxTextures = 2;
    style = styleFor(0); style.texture = sourceFor(3, 5, 0);
    const auto reclaimed = Success(blighted, atlases, twoSlots, kUnlit, &style, MakeAtlas(8));
    Require(reclaimed.budget.water.noContribution == 1 && reclaimed.blightTexturedTiles == 1
        && reclaimed.textures.size() == 2, "零贡献不占槽，按原优先级让精确荒芜准入");
    style = styleFor(255);
    const auto priority = Success(blighted, atlases, twoSlots, kUnlit, &style, MakeAtlas(8));
    Require(priority.waterTiles.size() == 1 && priority.blightApproximationTiles == 1,
        "有贡献的水面仍优先于荒芜，不能改变原准入顺序");
    for (std::size_t i = 0; i < original.size(); ++i)
        Require(terrain.vertices[i].position == original[i].position
            && terrain.vertices[i].waterBaseHeight == original[i].waterBaseHeight
            && terrain.vertices[i].textureAndFlagsRaw == original[i].textureAndFlagsRaw,
            "水面剔除只能省绘制，不能回写地形源数据");
}

void TestLighting() {
    const TerrainLighting lighting;
    constexpr float flatShade = 0.9f, ambient = 0.35f;
    // 平地：默认中性光下所有顶点亮度 0.9，贴图格是灰度（不染色）。
    const auto flat = Success(Grid(4, 4), Atlases(), {}, lighting);
    Require(flat.lit && flat.texturedTiles == 9, "平地应全部贴图并标记已加光照");
    for (const auto& vertex : flat.vertices) {
        Near(vertex.red, flatShade, "平地亮度必须等于 ambient + diffuse × 平地朝光度");
        Near(vertex.green, vertex.red, "贴图格明暗必须是灰度");
        Near(vertex.blue, vertex.red, "贴图格明暗必须是灰度");
    }
    // 向东下降的坡（法线朝东）背离西侧光源，比平地暗；向西下降的坡朝光，比平地亮且不超过 1。
    const auto east = Grid(5, 5, [](auto x, auto) { return 400.0f - static_cast<float>(x) * 96.0f; });
    const auto west = Grid(5, 5, [](auto x, auto) { return static_cast<float>(x) * 96.0f; });
    const auto away = TerrainVertexShade(east, 2, 2, lighting)[0];
    const auto toward = TerrainVertexShade(west, 2, 2, lighting)[0];
    Require(away < flatShade && away >= ambient, "背光坡必须变暗但不低于环境光");
    Require(toward > flatShade && toward <= 1.0f, "朝光坡必须变亮且不超过 1");
    // 边缘单侧差分与中心差分在匀坡上一致。
    Near(TerrainVertexShade(east, 0, 2, lighting)[0], away, "边缘法线必须用单侧差分");
    // 彩色光：环境光与直射光逐分量相加并夹到 1（原版 DNC 的蓝色环境光、暖色直射光）。
    TerrainLighting dnc;
    dnc.towardLight = {0, 0, 1};
    dnc.ambient = {0.1f, 0.2f, 0.3f};
    dnc.diffuse = {1.2f, 0.5f, 0.25f};
    const auto colored = TerrainVertexShade(Grid(4, 4), 1, 1, dnc);
    Near(colored[0], 1.0f, "红分量夹到 1");
    Near(colored[1], 0.7f, "绿分量 = 环境 + 直射");
    Near(colored[2], 0.55f, "蓝分量 = 环境 + 直射");
    // 荒芜近似调色叠加于真实地表贴图；光照关闭时保留每角的近似色。
    auto blight = Grid(3, 3, [](auto x, auto) { return static_cast<float>(x) * 96.0f; });
    for (auto& vertex : blight.vertices) vertex.textureAndFlagsRaw |= 0x20;
    const auto lit = Success(blight, Atlases(), {}, lighting);
    const auto unlit = Success(blight, Atlases());
    Require(lit.fallback.totalTiles == 0 && lit.texturedTiles == 4 && lit.blightApproximationTiles == 4,
        "荒芜不应回退高度色");
    Near(unlit.vertices[0].red, 0.87f, "荒芜贴图红色调");
    Near(unlit.vertices[0].green, 0.78f, "荒芜贴图绿色调");
    Near(unlit.vertices[0].blue, 0.91f, "荒芜贴图蓝色调");
    for (std::size_t i = 0; i < lit.vertices.size(); ++i) {
        Require(lit.vertices[i].red < unlit.vertices[i].red + 1e-6f && lit.vertices[i].red > 0,
            "荒芜调色仍须乘地形光照");
        Near(lit.vertices[i].x, unlit.vertices[i].x, "光照不得改变坐标");
        Near(lit.vertices[i].z, unlit.vertices[i].z, "光照不得改变高度");
    }
    // 无效参数整体拒绝，不发布部分网格。
    TerrainLighting below = lighting; below.towardLight = {0, 0, -1};
    Require(!BuildTerrainGroundMesh(Grid(), Atlases(), {}, below).Succeeded(), "地平线以下的光源必须拒绝");
    TerrainLighting bright = lighting; bright.diffuse[1] = 17.0f;
    Require(!BuildTerrainGroundMesh(Grid(), Atlases(), {}, bright).Succeeded(), "超出范围的光照颜色必须拒绝");
    TerrainLighting negative = lighting; negative.ambient[2] = -0.1f;
    Require(!BuildTerrainGroundMesh(Grid(), Atlases(), {}, negative).Succeeded(), "负的环境光必须拒绝");
}

void TestOmissionReasonsAndResourceStates() {
    const auto bit = [](TerrainGroundOmissionReason reason) { return static_cast<std::uint32_t>(reason); };
    constexpr auto preview = TerrainGroundBudgetPolicy::PreserveBasePreview;
    auto terrain = Grid();
    terrain.vertices[0].textureAndFlagsRaw |= 0x60;
    const std::vector<Atlas> atlases{MakeAtlas(), MakeAtlas(), MakeAtlas()};
    const auto blight = MakeAtlas(8);
    TerrainWaterStyle water{0, {90, 130, 170, 210}, {90, 130, 170, 210},
        {90, 130, 170, 210}, {90, 130, 170, 210}};
    const auto build = [&](const TerrainWaterStyle* style, Atlas exactBlight, const TerrainGroundMeshLimits& limits = {}) {
        auto result = BuildTerrainGroundMesh(terrain, atlases, limits, kUnlit, style, {}, exactBlight, {}, preview);
        Require(result.Succeeded(), "原因夹具必须保留合法基础层");
        CheckGeometry(terrain, *result.value);
        return std::move(*result.value);
    };
    const auto full = build(&water, blight);
    Require(full.budget.water.omissionReasons == 0 && full.budget.blight.omissionReasons == 0
        && full.budget.water.displayed == 1 && full.budget.blight.displayed == 1 && !full.budget.reduced,
        "成功显示的层不能带有省略原因");
    Require(full.budget.waterStyleAvailable && !full.budget.waterSourceTextureAvailable
        && full.budget.blightAtlasAvailable && full.budget.blightAtlasValidationPerformed && full.budget.blightAtlasValid
        && full.budget.groundTextureSlots == 1 && full.budget.finalTextureSlots == 3 && full.budget.textureSlotLimit == 16
        && full.budget.water.textureSlotsBeforeAdmission == 1 && full.budget.blight.textureSlotsBeforeAdmission == 2,
        "白色回退水样式仍可用，来源纹理缺失与样式缺失必须区分");
    auto texturedWater = water;
    texturedWater.texture = std::make_shared<const Image::DecodedImage>(Image::DecodedImage{1, 1,
        {std::byte{35}, std::byte{70}, std::byte{105}, std::byte{210}}});
    Require(build(&texturedWater, blight).budget.waterSourceTextureAvailable,
        "实际水源纹理可用状态不能沿用白色回退值");
    const auto missingStyle = build(nullptr, blight);
    Require(!missingStyle.budget.waterStyleAvailable && missingStyle.budget.water.requested == 1
        && missingStyle.budget.water.omitted == 1 && missingStyle.budget.water.displayed == 0
        && missingStyle.budget.water.omissionReasons == bit(TerrainGroundOmissionReason::MissingWaterStyle)
        && !missingStyle.budget.reduced, "缺样式是资源省略，不能伪报为几何预览降级");
    const auto missingBlight = build(&water, {});
    Require(!missingBlight.budget.blightAtlasAvailable && !missingBlight.budget.blightAtlasValidationPerformed
        && missingBlight.budget.blight.omissionReasons == bit(TerrainGroundOmissionReason::MissingBlightAtlas)
        && missingBlight.blightApproximationTiles == 1, "缺荒芜图集原因丢失");
    const auto invalidBlight = build(&water, MakeAtlas(4));
    Require(invalidBlight.budget.blightAtlasAvailable && invalidBlight.budget.blightAtlasValidationPerformed
        && !invalidBlight.budget.blightAtlasValid
        && invalidBlight.budget.blight.omissionReasons == bit(TerrainGroundOmissionReason::InvalidBlightAtlas),
        "存在但无效的荒芜图集不能伪报为未提供");
    TerrainGroundMeshLimits limits; limits.maxVertices = 4;
    const auto vertexLimited = build(&water, blight, limits);
    Require(vertexLimited.budget.water.omissionReasons == bit(TerrainGroundOmissionReason::Vertices)
        && vertexLimited.budget.blight.omissionReasons == bit(TerrainGroundOmissionReason::Vertices)
        && vertexLimited.budget.reduced, "资源足够时实际顶点失败域必须与资源省略分开");
    limits = {}; limits.maxIndices = 6;
    const auto indexLimited = build(&water, blight, limits);
    Require(indexLimited.budget.water.omissionReasons == bit(TerrainGroundOmissionReason::Indices)
        && indexLimited.budget.blight.omissionReasons == bit(TerrainGroundOmissionReason::Indices), "独立索引失败域未记录");
    limits = {}; limits.maxBatches = 1;
    const auto batchLimited = build(&water, blight, limits);
    Require(batchLimited.budget.water.omissionReasons == bit(TerrainGroundOmissionReason::Batches)
        && batchLimited.budget.blight.omissionReasons == bit(TerrainGroundOmissionReason::Batches), "独立批次失败域未记录");
    limits = {}; limits.maxRgbaBytes = 16 * 1024;
    const auto rgbaLimited = build(&water, blight, limits);
    Require(rgbaLimited.budget.water.omissionReasons == bit(TerrainGroundOmissionReason::RgbaBytes)
        && rgbaLimited.budget.blight.omissionReasons == bit(TerrainGroundOmissionReason::RgbaBytes)
        && !rgbaLimited.budget.reduced, "RGBA资源域省略不能伪报为整体几何预算");

    // 原创16种实际使用地表；每列两角相同，邻列之间恰有15个普通过渡。
    auto sixteen = Grid(17, 2);
    sixteen.header.groundTilesets.clear();
    std::vector<Atlas> distinct;
    for (std::size_t i = 0; i < 16; ++i) {
        sixteen.header.groundTilesets.push_back("g" + std::to_string(i));
        distinct.push_back(MakeAtlas());
    }
    for (std::uint32_t y = 0; y < 2; ++y) for (std::uint32_t x = 0; x < 17; ++x)
        sixteen.vertices[y * 17 + x].textureAndFlagsRaw = static_cast<std::uint8_t>(std::min(x, 15u) | 0x60);
    const auto fullSlotsResult = BuildTerrainGroundMesh(sixteen, distinct, {}, kUnlit, &water, {}, blight, {}, preview);
    Require(fullSlotsResult.Succeeded(), "16槽基础/过渡仍应完整显示");
    const auto& fullSlots = *fullSlotsResult.value;
    CheckGeometry(sixteen, fullSlots);
    Require(fullSlots.budget.transitions.displayed == 15 && fullSlots.budget.groundTextureSlots == 16
        && fullSlots.budget.finalTextureSlots == 16 && fullSlots.budget.water.omitted == 16
        && fullSlots.budget.blight.omitted == 16 && !fullSlots.budget.reduced
        && fullSlots.budget.water.textureSlotsBeforeAdmission == 16 && fullSlots.budget.blight.textureSlotsBeforeAdmission == 16
        && fullSlots.budget.water.omissionReasons == bit(TerrainGroundOmissionReason::TextureSlots)
        && fullSlots.budget.blight.omissionReasons == bit(TerrainGroundOmissionReason::TextureSlots),
        "槽满导致省略且reduced=false必须直接记录实际原因，不猜未来几何失败");
    limits = {}; limits.maxVertices = (16 + 15) * 4;
    const auto simultaneous = BuildTerrainGroundMesh(sixteen, distinct, limits, kUnlit, &water, {}, blight, {}, preview);
    Require(simultaneous.Succeeded()
        && simultaneous.value->budget.water.omissionReasons == bit(TerrainGroundOmissionReason::TextureSlots)
        && simultaneous.value->budget.blight.omissionReasons
            == (bit(TerrainGroundOmissionReason::TextureSlots) | bit(TerrainGroundOmissionReason::Vertices))
        && !simultaneous.value->budget.reduced, "只记录实际评估的域；水面资源拒绝后不虚构几何检查，荒芜双重失败仍要报告");
    const auto missingStyleFullSlots = BuildTerrainGroundMesh(sixteen, distinct, {}, kUnlit, nullptr, {}, blight, {}, preview);
    Require(missingStyleFullSlots.Succeeded() && missingStyleFullSlots.value->budget.water.omissionReasons
        == bit(TerrainGroundOmissionReason::MissingWaterStyle), "缺样式没有进入纹理分配，不得猜测实际槽满分支");
    auto shared = distinct;
    for (auto& atlas : shared) atlas = distinct.front();
    const auto sharedSlots = BuildTerrainGroundMesh(sixteen, shared, {}, kUnlit, &water, {}, blight, {}, preview);
    Require(sharedSlots.Succeeded() && sharedSlots.value->budget.groundTextureSlots == 1
        && sharedSlots.value->budget.water.displayed == 16 && sharedSlots.value->budget.blight.displayed == 16
        && sharedSlots.value->budget.water.omissionReasons == 0 && sharedSlots.value->budget.blight.omissionReasons == 0,
        "逻辑palette数量不能冒充实际去重后的纹理槽数");
    const auto noRequests = BuildTerrainGroundMesh(Grid(), atlases, {}, kUnlit, nullptr, {}, {}, {}, preview);
    Require(noRequests.Succeeded() && noRequests.value->budget.water.requested == 0
        && noRequests.value->budget.water.omissionReasons == 0 && noRequests.value->budget.blight.omissionReasons == 0
        && !noRequests.value->budget.water.resourceAdmissionChecked && !noRequests.value->budget.blight.resourceAdmissionChecked,
        "无请求时不能因为缺省资源产生虚假省略原因");
}

void TestPreserveBasePreviewBudget() {
    // 原创单格：三种地表、一个荒芜角和水面，明确请求 base=1、transition=2、blight=1、water=1。
    auto terrain = Grid(2, 2, [](std::uint32_t x, std::uint32_t y) { return 7.0f * x + 11.0f * y; });
    const std::array<std::uint8_t, 4> grounds{0, 1, 2, 1};
    for (std::size_t i = 0; i < grounds.size(); ++i) terrain.vertices[i].textureAndFlagsRaw = grounds[i];
    terrain.vertices[0].variationRaw = 3;
    terrain.vertices[3].textureAndFlagsRaw |= 0x60;
    const auto atlases = Atlases();
    const auto blight = MakeAtlas(8);
    TerrainWaterStyle water{0, {90, 130, 170, 210}, {90, 130, 170, 210},
        {90, 130, 170, 210}, {90, 130, 170, 210}};
    water.frameIndex = 1; water.frameCount = 2;
    water.frameSourcePath = "fixture/water-1"; water.frameSourceSha256 = "original-fixture-frame";
    constexpr auto previewPolicy = TerrainGroundBudgetPolicy::PreserveBasePreview;
    const auto attempt = [&](const TerrainGridSnapshot& grid, const TerrainGroundMeshLimits& limits,
        TerrainGroundBudgetPolicy policy) {
        return BuildTerrainGroundMesh(grid, atlases, limits, kUnlit, &water, {}, blight, {}, policy);
    };
    const auto preview = [&](const TerrainGridSnapshot& grid, const TerrainGroundMeshLimits& limits) {
        auto result = attempt(grid, limits, previewPolicy);
        Require(result.Succeeded() && result.value.has_value(), "显式预览应在基础层预算内成功");
        return std::move(*result.value);
    };
    const auto failure = [](const auto& result, std::string_view code) {
        Require(!result.Succeeded() && !result.value && result.diagnostics.size() == 1
            && result.diagnostics.front().code == code, "预览必须保留真实失败类别且不发布部分结果");
    };
    const auto counts = [](const TerrainGroundMesh& mesh) {
        for (const auto* layer : {&mesh.budget.base, &mesh.budget.transitions, &mesh.budget.blight, &mesh.budget.water})
            Require(layer->requested == layer->noContribution + layer->displayed + layer->omitted,
                "每类原请求数必须等于零贡献、显示和质量省略之和");
        Require(mesh.budget.base.omitted == 0 && mesh.budget.base.displayed * 4 <= mesh.vertices.size(),
            "预览不能省略基础层顶点");
        Require(mesh.budget.transitions.displayed + mesh.budget.blight.displayed == mesh.overlayTiles.size()
            && mesh.budget.blight.displayed == mesh.blightTexturedTiles
            && mesh.budget.water.displayed == mesh.waterTiles.size(), "统计必须与实际输出映射一致");
    };
    const auto strict = Success(terrain, atlases, {}, kUnlit, &water, blight);
    const auto full = preview(terrain, {});
    CheckGeometry(terrain, full); counts(full);
    Require(!full.budget.reduced && full.budget.base.requested == 1 && full.budget.transitions.requested == 2
        && full.budget.blight.requested == 1 && full.budget.water.requested == 1
        && full.vertices.size() == strict.vertices.size() && full.indices == strict.indices
        && full.overlayTiles == strict.overlayTiles && full.waterTiles == strict.waterTiles,
        "预算足够时预览应保持严格模式的完整几何及各类请求数");
    const auto sameBase = [&](const TerrainGroundMesh& mesh, const TerrainGroundMesh& reference, std::size_t tiles) {
        for (std::size_t i = 0; i < tiles * 4; ++i) {
            const auto& a = mesh.vertices[i]; const auto& b = reference.vertices[i];
            Require(std::array<float, 5>{a.x, a.y, a.z, a.u, a.v} == std::array<float, 5>{b.x, b.y, b.z, b.u, b.v},
                "预算取舍必须逐位保留每个基础层顶点 XYZ 和实际图集 UV");
        }
    };
    sameBase(full, strict, 1);

    TerrainGroundMeshLimits limits;
    limits.maxVertices = 8; // 仅容纳基础层与全图水面，不能只留两个过渡层中的第一个。
    const auto waterFirst = preview(terrain, limits);
    CheckGeometry(terrain, waterFirst); counts(waterFirst); sameBase(waterFirst, strict, 1);
    Require(waterFirst.budget.reduced && waterFirst.waterTiles == std::vector<std::uint32_t>{0}
        && waterFirst.overlayTiles.empty() && waterFirst.budget.transitions.omitted == 2
        && waterFirst.budget.blight.omitted == 1 && waterFirst.blightApproximationTiles == 1
        && !waterFirst.waterOmitted && waterFirst.waterFrameIndex == 1, "全图水面优先于整类普通过渡和精确荒芜");
    Require(waterFirst.budget.transitions.omissionReasons == static_cast<std::uint32_t>(TerrainGroundOmissionReason::Vertices),
        "普通过渡的实际顶点省略原因不能只写在水面或荒芜上");
    limits.maxVertices = 12;
    const auto retryLowerClass = preview(terrain, limits);
    counts(retryLowerClass); CheckGeometry(terrain, retryLowerClass);
    Require(retryLowerClass.budget.transitions.displayed == 0 && retryLowerClass.budget.blight.displayed == 1
        && retryLowerClass.budget.water.displayed == 1, "普通过渡整类装不下后仍可保留完整荒芜类");
    limits.maxVertices = 16;
    const auto transitions = preview(terrain, limits);
    counts(transitions); CheckGeometry(terrain, transitions);
    Require(transitions.budget.transitions.displayed == 2 && transitions.budget.water.displayed == 1
        && transitions.budget.blight.displayed == 0, "普通过渡优先于精确荒芜，不能颠倒保留顺序");

    limits.maxVertices = 4;
    const auto baseOnly = preview(terrain, limits);
    counts(baseOnly); CheckGeometry(terrain, baseOnly); sameBase(baseOnly, strict, 1);
    Require(baseOnly.vertices.size() == 4 && baseOnly.texturedTiles == 1 && baseOnly.waterOmitted
        && !baseOnly.waterTextureIndex && baseOnly.waterFrameCount == 0 && baseOnly.waterFrameSourcePath.empty()
        && baseOnly.waterFrameSourceSha256.empty() && baseOnly.budget.water.omitted == 1,
        "低预算应保留完整基础贴图；省略水面必须清空已显示帧身份");
    // 默认实参仍严格拒绝同一输入；显式 Strict 也不能静默采用预览。
    failure(BuildTerrainGroundMesh(terrain, atlases, limits, kUnlit, &water, {}, blight), "TERRAIN_GROUND_BUDGET");
    failure(attempt(terrain, limits, TerrainGroundBudgetPolicy::Strict), "TERRAIN_GROUND_BUDGET");
    limits.maxVertices = 3;
    failure(attempt(terrain, limits, previewPolicy), "TERRAIN_GROUND_BUDGET");
    limits = {}; limits.maxIndices = 5;
    failure(attempt(terrain, limits, previewPolicy), "TERRAIN_GROUND_BUDGET");
    limits = {}; limits.maxWorkingBytes = 1;
    failure(attempt(terrain, limits, previewPolicy), "TERRAIN_GROUND_BUDGET");
    limits = {}; limits.maxIndices = 12;
    const auto indexLimited = preview(terrain, limits);
    counts(indexLimited);
    Require(indexLimited.waterTiles.size() == 1 && indexLimited.overlayTiles.empty(), "索引预算也必须整类保留水面");
    limits = {}; limits.maxBatches = 2;
    const auto batchLimited = preview(terrain, limits);
    counts(batchLimited);
    Require(batchLimited.batches.size() == 2 && batchLimited.waterTiles.size() == 1
        && batchLimited.overlayTiles.empty(), "基础批次只计实际可见分组，批次预算仍按水面优先");
    limits = {}; limits.maxWorkingBytes = 120 * 1024;
    const auto workingLimited = preview(terrain, limits);
    counts(workingLimited); CheckGeometry(terrain, workingLimited); sameBase(workingLimited, strict, 1);
    Require(workingLimited.waterTiles.empty() && workingLimited.budget.transitions.displayed == 2
        && workingLimited.budget.blight.displayed == 0, "整体载荷不足以保留水面时仍须保留完整基础层和装得下的过渡类");
    Require((workingLimited.budget.water.omissionReasons & static_cast<std::uint32_t>(TerrainGroundOmissionReason::WorkingBytes)) != 0,
        "工作载荷造成的实际水面省略必须保留独立原因");
    failure(attempt(terrain, limits, TerrainGroundBudgetPolicy::Strict), "TERRAIN_GROUND_BUDGET");

    // x64：完整五类载荷按 5 个保活槽计为 218817B；push_back 自增到 6/8 槽会越过此边界。
    limits = {}; limits.maxTextures = 5; limits.maxWorkingBytes = 218825;
    const auto capacityLimited = preview(terrain, limits);
    counts(capacityLimited); CheckGeometry(terrain, capacityLimited); sameBase(capacityLimited, strict, 1);
    Require(!capacityLimited.budget.reduced && capacityLimited.budget.transitions.displayed == 2
        && capacityLimited.budget.blight.displayed == 1 && capacityLimited.budget.water.displayed == 1,
        "已计费的纹理槽不能因容器扩容额外开销导致完整可容纳预览整包失败");

    // 全图九个水格装不下时整类省略，再尝试唯一的过渡层；不能残留地图前部的若干水格。
    auto wide = Grid(4, 4);
    for (auto& vertex : wide.vertices) vertex.textureAndFlagsRaw |= 0x40;
    wide.vertices[0].textureAndFlagsRaw |= 1;
    limits = {}; limits.maxVertices = (wide.tiles.size() + 1) * 4;
    const auto wideMesh = preview(wide, limits);
    counts(wideMesh); CheckGeometry(wide, wideMesh);
    Require(wideMesh.waterTiles.empty() && wideMesh.budget.water.requested == 9 && wideMesh.budget.water.omitted == 9
        && wideMesh.overlayTiles == std::vector<std::uint32_t>{0} && wideMesh.budget.transitions.displayed == 1,
        "整类水面放不下时应允许完整低优先级过渡，不能按 tile 前缀截断");
    sameBase(wideMesh, Success(wide, atlases, {}, kUnlit, &water, blight), wide.tiles.size());

    // 资源上限与结构错误不能因低几何预算被省略成成功；已有逐格资源回退保持原样。
    limits = {}; limits.maxVertices = 4; limits.maxTextures = 1;
    failure(attempt(terrain, limits, previewPolicy), "TERRAIN_GROUND_BUDGET");
    limits = {}; limits.maxVertices = 4; limits.maxTextureDimension = 63;
    failure(attempt(terrain, limits, previewPolicy), "TERRAIN_GROUND_BUDGET");
    limits = {}; limits.maxVertices = 4; limits.maxRgbaBytes = 100 * 1024;
    failure(attempt(terrain, limits, previewPolicy), "TERRAIN_GROUND_BUDGET");
    limits = {}; limits.maxVertices = 4;
    auto invalid = terrain; invalid.tiles[0].vertices[2] = 3;
    failure(attempt(invalid, limits, previewPolicy), "TERRAIN_GROUND_GRID");
    invalid = terrain; invalid.vertices[0].position[2] = std::numeric_limits<float>::quiet_NaN();
    failure(attempt(invalid, limits, previewPolicy), "TERRAIN_GROUND_GRID");
    invalid = terrain; invalid.minimum[2] -= 1;
    failure(attempt(invalid, limits, previewPolicy), "TERRAIN_GROUND_GRID");
    const std::array<std::uint8_t, 1> shortSelection{0};
    failure(BuildTerrainGroundMesh(terrain, atlases, limits, kUnlit, &water, {}, blight, shortSelection, previewPolicy),
        "TERRAIN_GROUND_SELECTION");
    TerrainLighting invalidLight; invalidLight.towardLight[2] = -1;
    failure(BuildTerrainGroundMesh(terrain, atlases, limits, invalidLight, &water, {}, blight, {}, previewPolicy),
        "TERRAIN_GROUND_LIGHTING");
    auto invalidWater = water; invalidWater.frameIndex = invalidWater.frameCount;
    failure(BuildTerrainGroundMesh(terrain, atlases, limits, kUnlit, &invalidWater, {}, blight, {}, previewPolicy),
        "TERRAIN_GROUND_WATER_FRAME");
    invalidWater = water; invalidWater.heightOffset = std::numeric_limits<float>::quiet_NaN();
    failure(BuildTerrainGroundMesh(terrain, atlases, limits, kUnlit, &invalidWater, {}, blight, {}, previewPolicy),
        "TERRAIN_GROUND_WATER_HEIGHT");
    invalid = terrain; invalid.vertices[0].waterBaseHeight = std::numeric_limits<float>::infinity();
    failure(attempt(invalid, limits, previewPolicy), "TERRAIN_GROUND_WATER_HEIGHT");
    failure(attempt(invalid, {}, TerrainGroundBudgetPolicy::Strict), "TERRAIN_GROUND_WATER_HEIGHT");
    failure(attempt(terrain, {}, static_cast<TerrainGroundBudgetPolicy>(99)), "TERRAIN_GROUND_POLICY");
    auto missing = atlases; missing[2].reset();
    const auto fallback = BuildTerrainGroundMesh(terrain, missing, limits, kUnlit, &water, {}, blight, {}, previewPolicy);
    Require(fallback.Succeeded() && fallback.value->texturedTiles == 0 && fallback.value->fallback.missingAtlasTiles == 1
        && fallback.value->budget.transitions.requested == 0, "缺过渡材质仍须整格高度色回退，不能以基础贴图伪装完整资源");
    counts(*fallback.value);
    const std::array<std::uint8_t, 1> covered{1};
    const auto hidden = BuildTerrainGroundMesh(terrain, atlases, {}, kUnlit, &water, covered, blight, {}, previewPolicy);
    Require(hidden.Succeeded() && hidden.value->hiddenTileCount == 1 && hidden.value->budget.base.displayed == 0
        && hidden.value->baseTiles.empty() && hidden.value->vertices.size() == 4 && hidden.value->indices.size() == 6,
        "悬崖覆盖不分配基础原格顶点，覆盖格上的水面仍独立保留");
    counts(*hidden.value);
}

void TestOmittedTransitionAtlasReAdmission() {
    constexpr auto preview = TerrainGroundBudgetPolicy::PreserveBasePreview;
    const auto bit = [](TerrainGroundOmissionReason reason) { return static_cast<std::uint32_t>(reason); };
    TerrainWaterStyle water{0, {90, 130, 170, 210}, {90, 130, 170, 210},
        {90, 130, 170, 210}, {90, 130, 170, 210}};
    water.frameIndex = 1; water.frameCount = 2;
    water.frameSourcePath = "original/retained-water-1"; water.frameSourceSha256 = "retained-water-fixture";
    auto single = Grid();
    const std::array<std::uint8_t, 4> grounds{0, 1, 2, 1};
    for (std::size_t i = 0; i < grounds.size(); ++i)
        single.vertices[i].textureAndFlagsRaw = static_cast<std::uint8_t>(grounds[i] | 0x40);
    std::vector<Atlas> atlases{MakeAtlas(), MakeAtlas(8), MakeAtlas()};
    const auto build = [&](const TerrainGridSnapshot& terrain, const std::vector<Atlas>& sources,
        const TerrainGroundMeshLimits& limits, const TerrainWaterStyle* style, Atlas blight = {}) {
        auto result = BuildTerrainGroundMesh(terrain, sources, limits, kUnlit, style, {}, blight, {}, preview);
        Require(result.Succeeded(), "原创重准入夹具必须保留完整基础层");
        CheckGeometry(terrain, *result.value);
        for (std::size_t slot = 0; slot < result.value->textures.size(); ++slot)
            Require(std::any_of(result.value->batches.begin(), result.value->batches.end(), [&](const auto& batch) {
                return batch.indexCount != 0 && batch.textureIndex == slot;
            }), "回收后的每个纹理槽必须有真实非空draw依赖");
        for (const auto* layer : {&result.value->budget.base, &result.value->budget.transitions,
                &result.value->budget.blight, &result.value->budget.water})
            Require(layer->requested == layer->noContribution + layer->displayed + layer->omitted, "重准入不破坏层计数守恒");
        return std::move(*result.value);
    };
    TerrainGroundMeshLimits limits; limits.maxTextures = 3; limits.maxVertices = 8;
    const auto small = build(single, atlases, limits, &water);
    Require(small.budget.requestedGroundTextureSlots == 3 && small.budget.groundTextureSlots == 1
        && small.budget.finalTextureSlots == 2 && small.textures[0] == atlases[0]
        && small.vertices.size() == 8 && small.waterTiles.size() == 1 && small.overlayTiles.empty()
        && small.budget.water.omissionReasons == 0 && small.budget.reduced
        && small.budget.transitions.omissionReasons == bit(TerrainGroundOmissionReason::Vertices)
        && small.budget.water.textureSlotsBeforeAdmission == 1,
        "原槽拒水且原顶点省过渡后，回收unused再完整重试水，保留原过渡省略原因");
    Require(small.waterTextureIndex == 1 && small.waterFrameIndex == 1 && small.waterFrameCount == 2
        && small.waterFrameSourceSha256 == water.frameSourceSha256, "重准入成功须写实际水面索引和来源身份");
    Require(!BuildTerrainGroundMesh(single, atlases, limits, kUnlit, &water).Succeeded(), "Strict不采用资源回收预览");
    limits.maxTextures = 4;
    const auto retainedWater = build(single, atlases, limits, &water);
    Require(retainedWater.budget.water.displayed == 1 && retainedWater.waterTextureIndex == 1
        && retainedWater.textures.size() == 2 && retainedWater.batches.back().textureIndex == 1
        && retainedWater.waterFrameSourceSha256 == water.frameSourceSha256,
        "原已准入水面须稳定保留，过渡槽压紧后同步remap水batch/索引而不改变来源");
    limits.maxTextures = 3;
    limits.maxVertices = 4;
    const auto noRoom = build(single, atlases, limits, &water);
    Require(noRoom.waterTiles.empty() && noRoom.budget.water.omissionReasons == bit(TerrainGroundOmissionReason::Vertices)
        && noRoom.budget.finalTextureSlots == 1 && !noRoom.waterTextureIndex && noRoom.waterFrameCount == 0,
        "资源重试通过但几何仍失败时只记录实际最终原因，不残留旧槽拒或水帧身份");
    limits = {}; limits.maxRgbaBytes = 96 * 1024; limits.maxVertices = 8;
    const auto rgba = build(single, atlases, limits, &water);
    Require(rgba.budget.water.displayed == 1 && rgba.budget.water.omissionReasons == 0 && rgba.budget.finalTextureSlots == 2,
        "A全部ground64KiB通过但加水128KiB被拒，过渡省略后base水80KiB可重新准入");
    limits.maxRgbaBytes = 80 * 1024 - 1;
    const auto rgbaBelow = build(single, atlases, limits, &water);
    Require(rgbaBelow.budget.water.omissionReasons == bit(TerrainGroundOmissionReason::RgbaBytes)
        && rgbaBelow.budget.finalTextureSlots == 1, "重试真实RGBA并集少一字节仍省水，不伪造几何原因");
    limits.maxRgbaBytes = 80 * 1024;
    Require(build(single, atlases, limits, &water).budget.water.displayed == 1, "重试RGBA恰好达到上限时应显示");

    // 16来源、唯一base：前14格各2过渡，最后1格1过渡，共29；不使用地图资源。
    auto sixteen = Grid(16, 2); sixteen.header.groundTilesets.clear(); std::vector<Atlas> distinct;
    for (std::uint32_t i = 0; i < 16; ++i) { sixteen.header.groundTilesets.push_back("r" + std::to_string(i)); distinct.push_back(MakeAtlas()); }
    for (std::uint32_t x = 0; x < 16; ++x) {
        sixteen.vertices[x].textureAndFlagsRaw = 0x40;
        sixteen.vertices[16 + x].textureAndFlagsRaw = static_cast<std::uint8_t>(std::min(x + 1, 15u) | 0x40);
    }
    limits = {}; limits.maxVertices = 120;
    const auto narrow = build(sixteen, distinct, limits, &water);
    Require(narrow.budget.requestedGroundTextureSlots == 16 && narrow.budget.groundTextureSlots == 1
        && narrow.budget.finalTextureSlots == 2 && narrow.budget.transitions.requested == 29
        && narrow.budget.transitions.omissionReasons == bit(TerrainGroundOmissionReason::Vertices)
        && narrow.budget.water.displayed == 15 && narrow.vertices.size() == 120 && narrow.indices.size() == 180
        && narrow.batches.size() == 2 && narrow.textures.capacity() >= 16,
        "16槽反例须回收15个unused并保全15水，同时保留并计费原capacity");
    limits = {};
    const auto roomy = build(sixteen, distinct, limits, &water);
    Require(roomy.budget.groundTextureSlots == 16 && roomy.budget.finalTextureSlots == 16
        && roomy.budget.transitions.displayed == 29 && roomy.budget.water.displayed == 0
        && roomy.budget.water.omissionReasons == bit(TerrainGroundOmissionReason::TextureSlots) && !roomy.budget.reduced,
        "宽松几何全部trans可显示时没有unused，不能为了17槽水并集主动丢掉原成功过渡");
    auto blighted = sixteen;
    for (auto& vertex : blighted.vertices)
        vertex.textureAndFlagsRaw = static_cast<std::uint8_t>((vertex.textureAndFlagsRaw & 0x0f) | 0x20);
    limits.maxVertices = 120; const auto exactBlight = MakeAtlas(8);
    const auto recovered = build(blighted, distinct, limits, nullptr, exactBlight);
    Require(recovered.budget.groundTextureSlots == 1 && recovered.budget.finalTextureSlots == 2
        && recovered.blightTexturedTiles == 15 && recovered.vertices.size() == 120 && recovered.textures[1] == exactBlight,
        "原过渡几何省略后荒芜按回收后的实际表准入");
    auto smallBlight = single;
    for (auto& vertex : smallBlight.vertices)
        vertex.textureAndFlagsRaw = static_cast<std::uint8_t>((vertex.textureAndFlagsRaw & 0x0f) | 0x20);
    limits = {}; limits.maxVertices = 8;
    const auto aliasBlight = build(smallBlight, atlases, limits, nullptr, atlases[1]);
    Require(aliasBlight.textures.size() == 2 && aliasBlight.textures[1] == atlases[1] && aliasBlight.blightTexturedTiles == 1
        && aliasBlight.batches.back().textureIndex == 1, "blight复用已省略transition来源须重新保活并remap");
    auto shared = atlases; shared[1] = shared[0]; shared[2] = shared[0];
    const auto aliasBase = build(single, shared, limits, &water);
    Require(aliasBase.budget.requestedGroundTextureSlots == 1 && aliasBase.budget.groundTextureSlots == 1
        && aliasBase.textures[0] == shared[0] && aliasBase.waterTextureIndex == 1,
        "base仍引用的同pointer atlas不能因其transition省略被回收");
    limits = {};
    const auto aliasFull = build(single, shared, limits, &water);
    Require(aliasFull.budget.transitions.displayed == 2 && aliasFull.batches.size() == 4
        && aliasFull.batches[1].textureIndex == 0 && aliasFull.batches[2].textureIndex == 0,
        "共享atlas的原palette透明分组不合批、不重排");

    for (int domain = 0; domain < 5; ++domain) {
        limits = {}; limits.maxVertices = 8;
        switch (domain) {
        case 0: limits.maxTextures = 2; break;
        case 1: limits.maxTextureDimension = 127; break;
        case 2: limits.maxTextureBytes = 32767; break;
        case 3: limits.maxTextureChainBytes = 32767; break;
        default: limits.maxRgbaBytes = 65535; break;
        }
        const auto rejected = BuildTerrainGroundMesh(single, atlases, limits, kUnlit, &water, {}, {}, {}, preview);
        Require(!rejected.Succeeded() && rejected.diagnostics.front().code == "TERRAIN_GROUND_BUDGET", "最终不用的过渡资源也不能绕过A原硬帽");
    }
    limits = {}; limits.maxVertices = 8; limits.maxWorkingBytes = 60 * 1024;
    const auto requiredBudget = BuildTerrainGroundMesh(single, atlases, limits, kUnlit, &water, {}, {}, {}, preview);
    Require(!requiredBudget.Succeeded() && requiredBudget.diagnostics.front().code == "TERRAIN_GROUND_BUDGET",
        "原base加全部所需ground工作集检查失败时不得靠后置回收改成成功");
    auto missing = atlases; missing[1].reset(); limits = {}; limits.maxVertices = 8;
    const auto fallback = build(single, missing, limits, &water);
    Require(fallback.fallback.missingAtlasTiles == 1 && fallback.texturedTiles == 0
        && fallback.budget.transitions.requested == 0, "缺transition源仍整格高度色fallback");
    auto zero = water;
    zero.texture = std::make_shared<const Image::DecodedImage>(Image::DecodedImage{1, 1,
        {std::byte{90}, std::byte{130}, std::byte{170}, std::byte{0}}}); // 整个source为零，非仅选中cell0。
    limits = {}; limits.maxVertices = 4;
    const auto noWater = build(single, atlases, limits, &zero);
    Require(noWater.budget.water.noContribution == 1 && noWater.budget.water.omissionReasons == 0
        && !noWater.waterTextureIndex && noWater.textures.size() == 1,
        "969零贡献候选不因重试生成水atlas或质量省略原因");
}

void TestSharedBaseBlightBudgetsAndMapping() {
    constexpr auto preview = TerrainGroundBudgetPolicy::PreserveBasePreview;
    constexpr auto shared = TerrainGroundBlightPolicy::SharedBase;
    auto terrain = Grid(17, 2);
    terrain.header.groundTilesets.resize(16, "Ldrt");
    std::vector<Atlas> atlases;
    for (std::uint32_t ground = 0; ground < 16; ++ground) atlases.push_back(MakeAtlas(4, 1));
    for (std::uint32_t y = 0; y < 2; ++y) for (std::uint32_t x = 0; x < 17; ++x)
        terrain.vertices[y * 17 + x].textureAndFlagsRaw = static_cast<std::uint8_t>(std::min(x, 15u) | 0x20);
    const auto blight = MakeAtlas(8, 1);
    TerrainGroundMeshLimits limits; limits.maxVertices = 124;
    const auto build = [&](const TerrainGroundMeshLimits& cap) {
        return BuildTerrainGroundMesh(terrain, atlases, cap, kUnlit, nullptr, {}, blight, {}, preview, shared);
    };
    const auto result = build(limits);
    Require(result.Succeeded(), "16个实际基础材质与独立第17荒芜资源应在原顶点帽内成功");
    const auto& mesh = *result.value;
    CheckGeometry(terrain, mesh);
    Require(mesh.textures.size() == 16 && mesh.vertices.size() == 124 && mesh.indices.size() == 186
        && mesh.overlayTiles.size() == 15 && mesh.blightTexture == blight && mesh.blightIndices.size() == 96
        && mesh.blightBatches.size() == 1 && mesh.baseTiles.empty() && mesh.budget.base.displayed == 16
        && mesh.blightTexturedTiles == 16 && mesh.blightApproximationTiles == 0,
        "共享pass必须只加96索引，保16个基础格和15个普通过渡，不复制顶点或占第17个scene槽");
    for (const auto& vertex : mesh.vertices) Near(vertex.red, 1, "精确荒芜准入后base/过渡不能残留近似色调");
    for (std::uint32_t quad = 0; quad < 16; ++quad) {
        const std::array<std::uint32_t, 6> face{quad * 4, quad * 4 + 1, quad * 4 + 2, quad * 4 + 1, quad * 4 + 3, quad * 4 + 2};
        Require(std::equal(face.begin(), face.end(), mesh.blightIndices.begin() + quad * 6),
            "无hidden时空baseTiles是identity；每个基础格只能按标准拓扑引用一次");
    }
    Require(mesh.blightBatches[0].uvLeft == mesh.blightBatches[0].uvRight
        && mesh.blightBatches[0].uvTop == mesh.blightBatches[0].uvBottom,
        "1像素canonical cell两端相等仍然合法");
    const auto legacy = BuildTerrainGroundMesh(terrain, atlases, limits, kUnlit, nullptr, {}, blight, {}, preview);
    Require(legacy.Succeeded() && legacy.value->blightPolicy == TerrainGroundBlightPolicy::Legacy
        && legacy.value->blightTexture == nullptr && legacy.value->blightIndices.empty()
        && legacy.value->blightTexturedTiles == 0 && legacy.value->blightApproximationTiles == 16,
        "缺少显式能力选择的旧入口必须保留原纹理槽/顶点拒绝和近似色调");
    const auto omitted = [&](TerrainGroundMeshLimits cap, TerrainGroundOmissionReason reason) {
        const auto rejected = build(cap);
        Require(rejected.Succeeded() && rejected.value->blightTexture == nullptr
            && rejected.value->blightIndices.empty() && rejected.value->blightBatches.empty()
            && rejected.value->blightApproximationTiles == 16
            && (rejected.value->budget.blight.omissionReasons & static_cast<std::uint32_t>(reason)),
            "新pass预算拒绝必须整类回退，不能发布去近似色的部分pass");
        Near(rejected.value->vertices[0].red, 0.87f, "拒绝精确pass后必须保留base近似色调");
    };
    auto cap = limits; cap.maxIndices = 186 + 96 - 1; omitted(cap, TerrainGroundOmissionReason::Indices);
    cap = limits; cap.maxBatches = 31; omitted(cap, TerrainGroundOmissionReason::Batches);
    cap = limits; cap.maxRgbaBytes = 16 * 64 + 128 - 1; omitted(cap, TerrainGroundOmissionReason::RgbaBytes);
    cap.maxRgbaBytes += 1; Require(build(cap).value->blightTexturedTiles == 16, "RGBA边界必须聚合两份资源表后准入");
    // 必须同时保留完整普通层，排除低预算省过渡后反而能装荒芜的另一准入区间。
    const auto completeBlight = [](const Result<TerrainGroundMesh>& built) {
        return built.Succeeded() && built.value->blightTexturedTiles == 16
            && built.value->overlayTiles.size() == 15 && built.value->vertices.size() == 124;
    };
    // 搜索实际working门槛，核对边界前后完整场景准入；不把分配器开销或进程RSS混入该口径。
    std::size_t low = 0, high = 128 * 1024;
    while (low < high) {
        const auto middle = low + (high - low) / 2;
        cap = limits; cap.maxWorkingBytes = middle;
        const auto bounded = build(cap);
        if (completeBlight(bounded)) high = middle;
        else low = middle + 1;
    }
    cap = limits; cap.maxWorkingBytes = low;
    Require(low > 0 && completeBlight(build(cap)), "working门槛处应完整准入");
    cap.maxWorkingBytes -= 1;
    const auto below = build(cap);
    Require(!below.Succeeded() || (below.value->blightIndices.empty() && below.value->blightApproximationTiles == 16),
        "working边界以下不能保留部分索引或无pass的去近似色base");

    TerrainWaterStyle refusedWater{0, {90, 130, 170, 210}, {90, 130, 170, 210},
        {90, 130, 170, 210}, {90, 130, 170, 210}};
    auto wet = terrain;
    for (auto& vertex : wet.vertices) { vertex.textureAndFlagsRaw |= 0x40; vertex.waterBaseHeight = 40; }
    const auto minimumAcceptedBytes = [](const std::function<bool(std::size_t)>& accepts) {
        std::size_t first = 0, last = 256 * 1024;
        Require(accepts(last), "容量边界夹具必须在宽松硬预算内完整准入");
        while (first < last) {
            const auto middle = first + (last - first) / 2;
            if (accepts(middle)) last = middle;
            else first = middle + 1;
        }
        return first;
    };
    // 16个真实ground槽使全部水候选因资源拒绝。公开结果搜索阈值，不复制builder私有预算公式。
    // Strict资源拒绝clear与Preview最终clear均须保留实际candidate capacity的账。
    for (const auto policy : {TerrainGroundBudgetPolicy::Strict, preview}) {
        const auto attempt = [&](bool hasWater, std::size_t bytes) {
            auto bounded = limits; bounded.maxWorkingBytes = bytes;
            return BuildTerrainGroundMesh(hasWater ? wet : terrain, atlases, bounded, kUnlit,
                hasWater ? &refusedWater : nullptr, {}, blight, {}, policy, shared);
        };
        const auto dryMinimum = minimumAcceptedBytes([&](std::size_t bytes) {
            return completeBlight(attempt(false, bytes));
        });
        const auto wetMinimum = minimumAcceptedBytes([&](std::size_t bytes) {
            return completeBlight(attempt(true, bytes));
        });
        const auto accepted = attempt(true, wetMinimum);
        Require(accepted.Succeeded() && accepted.value->waterFlaggedTiles == 16
            && accepted.value->budget.water.omitted == 16 && accepted.value->waterTiles.empty()
            && accepted.value->budget.water.omissionReasons == static_cast<std::uint32_t>(TerrainGroundOmissionReason::TextureSlots),
            "水候选资源拒绝必须保留完整源请求与拒绝原因");
        Require(wetMinimum >= dryMinimum + accepted.value->waterFlaggedTiles * sizeof(std::uint32_t),
            "clear保留的水候选capacity至少容纳全部请求，必须体现在公开working准入门槛");
        const auto rejected = attempt(true, wetMinimum - 1);
        if (!(rejected.Succeeded() && rejected.value->blightIndices.empty() && rejected.value->blightApproximationTiles == 16
            && (rejected.value->budget.blight.omissionReasons & static_cast<std::uint32_t>(TerrainGroundOmissionReason::WorkingBytes)))) {
            std::cerr << "shared-water-boundary policy=" << (policy == TerrainGroundBudgetPolicy::Strict ? "strict" : "preview")
                << " dry_min=" << dryMinimum << " wet_min=" << wetMinimum << " succeeded=" << rejected.Succeeded()
                << " error=" << (rejected.diagnostics.empty() ? "none" : rejected.diagnostics.front().code);
            if (rejected.value) std::cerr << " vertices=" << rejected.value->vertices.size()
                << " overlays=" << rejected.value->overlayTiles.size() << " blight_indices=" << rejected.value->blightIndices.size()
                << " blight_batches=" << rejected.value->blightBatches.size() << " blight_displayed=" << rejected.value->blightTexturedTiles
                << " blight_approx=" << rejected.value->blightApproximationTiles << " blight_reasons=" << rejected.value->budget.blight.omissionReasons;
            std::cerr << '\n';
        }
        Require(rejected.Succeeded() && rejected.value->blightIndices.empty() && rejected.value->blightApproximationTiles == 16
            && (rejected.value->budget.blight.omissionReasons & static_cast<std::uint32_t>(TerrainGroundOmissionReason::WorkingBytes)),
            "真实拒水候选门槛以下1B必须整类拒绝精确荒芜，不能漏账准入");
    }
    // 水面保留时，独立候选账不能再叠加到原逐quad账；比较公开增量而非硬编码私有字节数。
    auto waterOnly = Grid(); waterOnly.header.groundTilesets = {"Ldrt"};
    auto wetOnly = waterOnly;
    for (auto& vertex : wetOnly.vertices) { vertex.textureAndFlagsRaw |= 0x40; vertex.waterBaseHeight = 40; }
    const std::vector<Atlas> oneAtlas{MakeAtlas(4, 1)};
    const auto selectedWaterCost = [&](TerrainGroundBlightPolicy mode) {
        const auto minimum = [&](bool hasWater) {
            return minimumAcceptedBytes([&](std::size_t bytes) {
                TerrainGroundMeshLimits bounded; bounded.maxWorkingBytes = bytes;
                const auto built = BuildTerrainGroundMesh(hasWater ? wetOnly : waterOnly, oneAtlas, bounded, kUnlit,
                    hasWater ? &refusedWater : nullptr, {}, {}, {}, preview, mode);
                return built.Succeeded() && (!hasWater || built.value->waterTiles.size() == 1);
            });
        };
        return minimum(true) - minimum(false);
    };
    Require(selectedWaterCost(shared) == selectedWaterCost(TerrainGroundBlightPolicy::Legacy),
        "已选水面的公开working增量必须维持原口径，不能重复计候选4B");

    auto compact = Grid(5, 2);
    for (auto& vertex : compact.vertices) vertex.textureAndFlagsRaw = 0x20;
    compact.vertices[4].textureAndFlagsRaw = compact.vertices[9].textureAndFlagsRaw = 0x2f;
    const std::vector<std::uint8_t> covered{0, 1, 0, 0};
    const auto mapped = BuildTerrainGroundMesh(compact, Atlases(), {}, kUnlit, nullptr, covered, blight, {}, preview, shared);
    Require(mapped.Succeeded() && mapped.value->baseTiles == std::vector<std::uint32_t>{0, 2, 3}
        && mapped.value->budget.base.displayed == 3 && mapped.value->fallback.totalTiles == 1
        && mapped.value->blightIndices == std::vector<std::uint32_t>{0, 1, 2, 1, 3, 2, 4, 5, 6, 5, 7, 6},
        "base前缀含fallback；荒芜只引用紧缩后的0/2原格，排除hidden和fallback格");
    CheckGeometry(compact, *mapped.value);

    auto grouped = Grid(5, 2);
    for (auto& vertex : grouped.vertices) vertex.textureAndFlagsRaw = 0x20;
    grouped.vertices[1].variationRaw = 1; grouped.vertices[3].variationRaw = 16;
    const auto groups = BuildTerrainGroundMesh(grouped, Atlases(), {}, kUnlit, nullptr, {}, blight, {}, preview, shared);
    Require(groups.Succeeded() && groups.value->blightBatches.size() == 3
        && groups.value->blightBatches[0].indexCount == 12 && groups.value->blightBatches[1].firstIndex == 12
        && groups.value->blightIndices[0] == 0 && groups.value->blightIndices[6] == 8
        && groups.value->blightIndices[12] == 4 && groups.value->blightIndices[18] == 12,
        "不同cell按规范顺序唯一分组；组内原tile顺序稳定，不能重复quad");

    for (std::uint32_t mask = 1; mask < 16; ++mask) for (std::uint32_t variation = 0; variation < (mask == 15 ? 18u : 1u); ++variation) {
        auto one = Grid();
        const std::array<std::uint32_t, 4> bits{2, 1, 8, 4};
        for (std::size_t corner = 0; corner < 4; ++corner)
            one.vertices[corner].textureAndFlagsRaw = mask & bits[corner] ? 0x20 : 0;
        one.vertices[0].variationRaw = static_cast<std::uint8_t>(variation);
        const auto expanded = Success(one, Atlases(), {}, kUnlit, nullptr, blight);
        const auto precise = BuildTerrainGroundMesh(one, Atlases(), {}, kUnlit, nullptr, {}, blight, {}, preview, shared);
        Require(precise.Succeeded() && precise.value->blightBatches.size() == 1, "每种原版mask/variation都应保留");
        const auto& rect = precise.value->blightBatches.front();
        Require(rect.uvLeft == expanded.vertices[4].u && rect.uvRight == expanded.vertices[5].u
            && rect.uvTop == expanded.vertices[6].v && rect.uvBottom == expanded.vertices[4].v,
            "新pass必须逐位复用旧CellUv和mask/variation，不能重新猜测半texel约定");
    }
    auto source = Grid();
    Require(!TerrainBlightCell(source, source.tiles[0].vertices), "零mask不能产生荒芜cell");
    auto invalidCorners = source.tiles[0].vertices; invalidCorners[3] = static_cast<std::uint32_t>(source.vertices.size());
    Require(!TerrainBlightCell(source, invalidCorners), "公开纯函数应有界拒绝非法corner");
    for (auto& vertex : source.vertices) vertex.textureAndFlagsRaw = 0x20;
    const std::array<std::array<std::uint32_t, 3>, 5> boundaries{{{0, 4, 0}, {15, 7, 3}, {16, 3, 3}, {17, 0, 0}, {31, 0, 0}}};
    for (const auto& entry : boundaries) {
        source.vertices[0].variationRaw = static_cast<std::uint8_t>(entry[0]);
        const auto cell = TerrainBlightCell(source, source.tiles[0].vertices);
        Require(cell && (*cell)[0] == entry[1] && (*cell)[1] == entry[2], "共享纯函数必须保留原版variation全部边界");
    }
}

} // namespace

int main() {
    try {
        TestGeometryAndBlocks();
        TestVariationAndActualUv();
        TestBaseTextureBatchesAndLifetime();
        TestFallbackReasonsAndWater();
        TestOriginalBlightAtlasAndFallback();
        TestSharedBaseBlightBudgetsAndMapping();
        TestRetainedMipBudgets();
        TestBudgetsAndInvalidInputs();
        TestBlendLayers();
        TestEveryTransitionMask();
        TestCoveredBaseCompactionPreservesTransitions();
        TestWaterColorBounds();
        TestWater();
        TestWaterZeroContribution();
        TestLighting();
        TestOmissionReasonsAndResourceStates();
        TestPreserveBasePreviewBudget();
        TestOmittedTransitionAtlasReAdmission();
        std::cout << "TerrainGroundMeshTests: all tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "TerrainGroundMeshTests: " << exception.what() << '\n';
        return 1;
    }
}
