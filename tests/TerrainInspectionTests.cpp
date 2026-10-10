#include "War3MapCore/Render/TerrainInspection.h"

#include <bit>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace {

using namespace War3Map;
using namespace War3Map::MapData;
using namespace War3Map::Render;

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

// 原创 W3E 二进制：非居中偏移和可手算坡面，不绕过生产解析器伪造快照。
struct Fixture {
    std::vector<std::byte> bytes;
    void U8(std::uint8_t value) { bytes.push_back(static_cast<std::byte>(value)); }
    void U16(std::uint16_t value) { U8(static_cast<std::uint8_t>(value)); U8(static_cast<std::uint8_t>(value >> 8)); }
    void U32(std::uint32_t value) {
        for (unsigned shift = 0; shift < 32; shift += 8) U8(static_cast<std::uint8_t>(value >> shift));
    }
    void Raw(std::string_view value) { for (const auto c : value) U8(static_cast<std::uint8_t>(c)); }
    Fixture(std::uint32_t width = 6, std::uint32_t height = 5) {
        Raw("W3E!"); U32(11); U8('L'); U32(1);
        U32(2); Raw("TstA"); Raw("tsTB"); U32(1); Raw("ClfZ");
        U32(width); U32(height);
        U32(std::bit_cast<std::uint32_t>(-192.25f)); U32(std::bit_cast<std::uint32_t>(64.5f));
        for (std::uint32_t y = 0; y < height; ++y) {
            for (std::uint32_t x = 0; x < width; ++x) {
                const bool unknownWater = x == 2 && y == 2;
                const bool missingTexture = x == 3 && y == 2;
                U16(static_cast<std::uint16_t>(8192 + 4 * x + 8 * y));
                U16(static_cast<std::uint16_t>(unknownWater ? 0xe008 : 8192));
                U8(static_cast<std::uint8_t>(unknownWater ? 0xf1 : (missingTexture ? 0x0f : 0)));
                U8(static_cast<std::uint8_t>(unknownWater ? 0xbf : 0));
                U8(static_cast<std::uint8_t>(unknownWater ? 0xf2 : (missingTexture ? 0x32 : 2)));
            }
        }
    }
    MapTerrain Parse() const {
        auto result = ParseMapTerrain(bytes);
        Require(result.Succeeded(), "原创 W3E fixture 应完整解析");
        return std::move(*result.value);
    }
};

TerrainInspectionBinding Binding() {
    return {"test-document-opaque", 7, 0, std::string(64, 'a'), std::string(64, 'b')};
}

TerrainInspectionBlock Success(const MapTerrain& terrain, TerrainTileRectangle rectangle,
    const TerrainInspectionLimits& limits = {}) {
    const auto identity = Binding();
    auto result = QueryTerrainBlock(terrain, identity, {identity, rectangle}, limits);
    if (!result.Succeeded()) throw std::runtime_error("区块意外失败：" + result.diagnostics.front().code);
    Require(result.diagnostics.empty(), "原始未知位通过数据和计数表达，不应伪装为查询失败");
    return std::move(*result.value);
}

void Rejected(const MapTerrain& terrain, const TerrainInspectionBinding& current,
    const TerrainInspectionRequest& request, std::string_view code, const TerrainInspectionLimits& limits = {}) {
    const auto result = QueryTerrainBlock(terrain, current, request, limits);
    Require(!result.Succeeded() && !result.value, "拒绝不能返回半个区块");
    Require(result.diagnostics.size() == 1 && result.diagnostics.front().severity == DiagnosticSeverity::Error,
        "拒绝须有一条明确错误");
    if (result.diagnostics.front().code != code) {
        throw std::runtime_error("诊断错误：" + result.diagnostics.front().code + " 预期 " + std::string(code));
    }
}

void TestTileAndVertexCoordinates() {
    const auto terrain = Fixture().Parse();
    const auto block = Success(terrain, {1, 1, 3, 2});
    Require(block.binding == Binding() && block.binding.revision == 0, "身份必须完整保留，首次修订 0 合法");
    Require(block.sourceHeader.width == 6 && block.sourceHeader.height == 5 && block.sourceHeader.usesCustomTileset,
        "header 是全图顶点数，不是区块格子数");
    Require(block.sourceHeader.groundTilesets == std::vector<std::string>{"TstA", "tsTB"}
        && block.sourceHeader.cliffTilesets == std::vector<std::string>{"ClfZ"}, "材质 rawcode 顺序及大小写不能改变");
    Require(block.vertices.size() == 12 && block.cells.size() == 6, "3×2 格子应带 4×3 顶点，不能遗漏北/东边界");
    Require(block.vertices.front().x == 1 && block.vertices.front().y == 1
        && block.vertices.front().sourceIndex == 7, "局部顶点 0 必须映射原始全局坐标");
    Require(block.vertices.back().x == 4 && block.vertices.back().y == 3
        && block.vertices.back().sourceIndex == 22, "最后顶点包含区块东北边界");
    Require(block.vertices.front().sample.position == std::array<float, 3>{-64.25f, 192.5f, 3},
        "世界坐标必须使用原始偏移及 128 世界单位步长");
    Require(block.cells.front().sourceIndex == 6
        && block.cells.front().source.vertices == std::array<std::uint32_t, 4>{7, 8, 13, 14}
        && block.cells.front().localVertices == std::array<std::uint32_t, 4>{0, 1, 4, 5},
        "第一格 SW/SE/NW/NE 全局及局部索引错误");
    Require(block.cells.back().sourceIndex == 13
        && block.cells.back().source.vertices == std::array<std::uint32_t, 4>{15, 16, 21, 22}
        && block.cells.back().localVertices == std::array<std::uint32_t, 4>{6, 7, 10, 11},
        "第二行最后一格不能误用全图宽度作为局部步长");
    Require(block.minimum == std::array<float, 3>{-64.25f, 192.5f, 3}
        && block.maximum == std::array<float, 3>{319.75f, 448.5f, 10}, "区块 AABB 不能返回整图范围");
    for (const auto& cell : block.cells) {
        for (std::size_t corner = 0; corner < 4; ++corner) {
            Require(block.vertices[cell.localVertices[corner]].sourceIndex == cell.source.vertices[corner],
                "每格所有局部角都应唯一映射同一个原始顶点");
        }
    }
}

void TestUnknownBytesAndReadonlyLifetime() {
    Fixture fixture;
    auto terrain = fixture.Parse();
    const auto snapshot = terrain.Snapshot();
    const auto originalBytes = terrain.OriginalBytes();
    const auto before = snapshot->vertices[14];
    auto block = Success(terrain, {1, 1, 3, 2});
    const auto& sample = block.vertices[5].sample;
    Require(sample.groundHeightRaw == 8216 && sample.waterHeightAndFlagsRaw == 0xe008
        && sample.textureAndFlagsRaw == 0xf1 && sample.variationRaw == 0xbf && sample.cliffAndLayerRaw == 0xf2,
        "查询不得过滤未知位或把 packed 原字段重编码");
    Require(sample.UnknownWaterFlags() == 0x8000 && sample.IsMapEdge() && sample.IsRamp()
        && sample.IsBlight() && sample.HasWater() && sample.IsBoundary(), "各原始标志应同时保留");
    Require(sample.GroundVariation() == 31 && sample.CliffVariation() == 5 && sample.waterBaseHeight == 2
        && sample.position == std::array<float, 3>{63.75f, 320.5f, 6}, "查询不能应用水材质偏移或坡道插值");
    Require(block.unknownWaterFlagVertices == 1 && block.unresolvedGroundTextureVertices == 1
        && block.unresolvedCliffTextureVertices == 1, "未知标志和缺失纹理须显式计数，cliff15 不得算越界");
    for (int i = 0; i < 3; ++i) (void)Success(terrain, {0, 0, 1, 1});
    Require(terrain.OriginalBytes() == originalBytes && terrain.Snapshot() == snapshot
        && snapshot->vertices[14].position == before.position
        && snapshot->vertices[14].waterHeightAndFlagsRaw == before.waterHeightAndFlagsRaw, "重复查询必须只读");
    terrain = MapTerrain{};
    fixture.bytes.clear();
    Require(block.vertices[5].sample.groundHeightRaw == 8216, "区块不能借用输入或原文档寿命");
    block.vertices[5].sample.groundHeightRaw = 0;
    Require(snapshot->vertices[14].groundHeightRaw == 8216, "修改结果副本不能污染源快照");
}

void TestEveryIdentityComponentAndInvalidBinding() {
    const auto terrain = Fixture().Parse();
    const auto binding = Binding();
    for (unsigned field = 0; field < 5; ++field) {
        auto stale = binding;
        if (field == 0) stale.documentId = "another-document";
        if (field == 1) ++stale.documentGeneration;
        if (field == 2) ++stale.revision;
        if (field == 3) stale.sourceSha256[0] = 'c';
        if (field == 4) stale.terrainSha256[0] = 'd';
        Rejected(terrain, binding, {stale, {0, 0, 1, 1}}, "TERRAIN_INSPECT_STALE");
    }
    for (unsigned field = 0; field < 6; ++field) {
        auto invalid = binding;
        if (field == 0) invalid.documentId.clear();
        if (field == 1) invalid.documentId.assign(129, 'x');
        if (field == 2) invalid.documentId = std::string("map\0id", 6);
        if (field == 3) invalid.sourceSha256.pop_back();
        if (field == 4) invalid.sourceSha256[0] = 'A';
        if (field == 5) invalid.terrainSha256[0] = 'g';
        Rejected(terrain, invalid, {invalid, {0, 0, 1, 1}}, "TERRAIN_INSPECT_BINDING_INVALID");
        Rejected(terrain, binding, {invalid, {0, 0, 1, 1}}, "TERRAIN_INSPECT_BINDING_INVALID");
    }
    auto large = binding;
    large.documentId.assign(128, 'x');
    large.documentGeneration = large.revision = std::numeric_limits<std::uint64_t>::max();
    const auto result = QueryTerrainBlock(terrain, large, {large, {0, 0, 1, 1}});
    Require(result.Succeeded() && result.value->binding == large, "64 位身份不能经过 double 丢失精度");
}

void TestBoundsAndUnavailable() {
    const auto terrain = Fixture().Parse();
    const auto binding = Binding();
    const auto max = std::numeric_limits<std::uint32_t>::max();
    for (const TerrainTileRectangle rectangle : {TerrainTileRectangle{0, 0, 0, 1}, {0, 0, 1, 0},
        {5, 0, 1, 1}, {0, 4, 1, 1}, {4, 3, 2, 1}, {4, 3, 1, 2}, {max, 0, 1, 1},
        {0, max, 1, 1}, {1, 1, max, 1}, {1, 1, 1, max}}) {
        Rejected(terrain, binding, {binding, rectangle}, "TERRAIN_INSPECT_BOUNDS");
    }
    const auto edge = Success(terrain, {4, 3, 1, 1});
    Require(edge.cells.size() == 1 && edge.vertices.size() == 4 && edge.cells.front().sourceIndex == 19
        && edge.cells.front().source.vertices == std::array<std::uint32_t, 4>{22, 23, 28, 29},
        "最后一格必须可查，最北/东顶点不能被当成 tile 起点");
    Rejected(MapTerrain{}, binding, {binding, {0, 0, 1, 1}}, "TERRAIN_INSPECT_UNAVAILABLE");
}

void TestExactBudgetsAndLargeMapSlice() {
    const auto terrain = Fixture().Parse();
    const auto binding = Binding();
    const TerrainInspectionRequest request{binding, {1, 1, 3, 2}};
    const auto reference = Success(terrain, request.tiles);
    const std::size_t exactBytes = sizeof(TerrainInspectionBlock)
        + binding.documentId.size() + 1 + 65 + 65 + 3 * (sizeof(std::string) + 5)
        + 6 * sizeof(TerrainInspectionTile) + 12 * sizeof(TerrainInspectionVertex);
    Require(reference.outputBytes == exactBytes, "预算须包含元数据表、身份字符串和全部结果元素");
    TerrainInspectionLimits limits{6, 12, exactBytes};
    Require(QueryTerrainBlock(terrain, binding, request, limits).Succeeded(), "精确预算边界应可用");
    --limits.maxOutputBytes;
    Rejected(terrain, binding, request, "TERRAIN_INSPECT_BUDGET", limits);
    limits = {5, 12, exactBytes}; Rejected(terrain, binding, request, "TERRAIN_INSPECT_BUDGET", limits);
    limits = {6, 11, exactBytes}; Rejected(terrain, binding, request, "TERRAIN_INSPECT_BUDGET", limits);
    for (const auto invalid : {TerrainInspectionLimits{0, 8192, 2097152}, {4097, 8192, 2097152},
        {4096, 8193, 2097152}, {4096, 8192, 2097153}, {4096, 8192, 0}}) {
        Rejected(terrain, binding, request, "TERRAIN_INSPECT_LIMITS_INVALID", invalid);
    }
    const auto large = Fixture(257, 129).Parse();
    const auto smallSlice = Success(large, {254, 125, 2, 3}, {6, 12, 4096});
    Require(smallSlice.cells.size() == 6 && smallSlice.vertices.size() == 12
        && smallSlice.vertices.back().sourceIndex == 128 * 257 + 256,
        "远角小查询只返回请求顶点，不复制整图或重新编号原始索引");
    const auto exactTiles = Success(large, {0, 0, 64, 64});
    Require(exactTiles.cells.size() == 4096 && exactTiles.vertices.size() == 4225, "硬上限内完整区块应可用");
    Rejected(large, binding, {binding, {0, 0, 65, 65}}, "TERRAIN_INSPECT_BUDGET");
}

} // namespace

int main() {
    try {
        TestTileAndVertexCoordinates();
        TestUnknownBytesAndReadonlyLifetime();
        TestEveryIdentityComponentAndInvalidBinding();
        TestBoundsAndUnavailable();
        TestExactBudgetsAndLargeMapSlice();
        std::cout << "TerrainInspectionTests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
