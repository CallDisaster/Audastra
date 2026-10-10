#include "War3MapCore/MapData/MapTerrain.h"
#include "War3MapCore/MapData/MapPathing.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/MapArchiveSession.h"

#include <bit>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace {

using namespace War3Map;
using namespace War3Map::MapData;

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Fixture {
    std::vector<std::byte> bytes;
    std::size_t dimensions = 0, offsets = 0, vertices = 0;
    void U8(std::uint8_t value) { bytes.push_back(static_cast<std::byte>(value)); }
    void U16(std::uint16_t value) { U8(static_cast<std::uint8_t>(value & 0xff)); U8(static_cast<std::uint8_t>(value >> 8)); }
    void U32(std::uint32_t value) {
        for (unsigned shift = 0; shift < 32; shift += 8) U8(static_cast<std::uint8_t>((value >> shift) & 0xff));
    }
    void F32(float value) { U32(std::bit_cast<std::uint32_t>(value)); }
    void Raw(std::string_view value) { for (const auto c : value) U8(static_cast<std::uint8_t>(c)); }
    void Patch32(std::size_t offset, std::uint32_t value) {
        for (unsigned shift = 0; shift < 32; shift += 8) bytes[offset + shift / 8] = static_cast<std::byte>((value >> shift) & 0xff);
    }
    void Vertex(std::uint16_t ground, std::uint16_t water, std::uint8_t flags,
        std::uint8_t variation, std::uint8_t cliff) {
        U16(ground); U16(water); U8(flags); if (bytes[4] == std::byte{12}) U8(0xff);
        U8(variation); U8(cliff);
    }
    Fixture(std::uint32_t version = 11) {
        Raw("W3E!"); U32(version); U8('L'); U32(1);
        U32(2); Raw("Tst1"); Raw("Tst2");
        U32(1); Raw("Clf1");
        dimensions = bytes.size();
        U32(3); U32(2);
        offsets = bytes.size();
        F32(-128.0f); F32(64.0f);
        vertices = bytes.size();
        Vertex(8192, 8192, 0, 0, 2);
        Vertex(8196, 8196 | 0x4000, 0x71, 0xb3, 3);
        Vertex(8188, 8192, 0x80, 0, 1);
        Vertex(0, 0, 0, 0, 0);
        Vertex(0x7fff, 0x3fff, 1, 0, 2);
        Vertex(8192, 8192 | 0x8000, 0x40, 0, 0xf2);
    }
};

void Rejected(std::span<const std::byte> bytes, std::string_view code, const TerrainReadLimits& limits = {}) {
    const auto result = ParseMapTerrain(bytes, limits);
    Require(!result.Succeeded() && !result.value, "失败不能返回半成品地形");
    Require(result.diagnostics.size() == 1, "失败必须有一条明确错误");
    if (result.diagnostics.front().code != code) throw std::runtime_error("错误代码不符：" + result.diagnostics.front().code + "，预期 " + std::string(code));
    Require(result.diagnostics.front().origin.has_value(), "格式错误必须定位原成员字节");
    Require(result.diagnostics.front().origin->offset <= bytes.size(), "错误定位不能超出输入");
}

void TestCompleteGrid() {
    Fixture fixture;
    auto result = ParseMapTerrain(fixture.bytes);
    Require(result.Succeeded(), "完整原创 W3E 应可解析");
    const auto snapshot = result.value->Snapshot();
    static_assert(std::is_same_v<decltype(result.value->Snapshot()), std::shared_ptr<const TerrainGridSnapshot>>);
    Require(snapshot && snapshot->header.version == 11 && snapshot->header.mainTileset == 'L'
        && snapshot->header.usesCustomTileset, "header 字段错误");
    Require(snapshot->header.groundTilesets == std::vector<std::string>{"Tst1", "Tst2"}
        && snapshot->header.cliffTilesets == std::vector<std::string>{"Clf1"}, "纹理表原始顺序或 ID 改变");
    Require(snapshot->header.width == 3 && snapshot->header.height == 2
        && snapshot->vertices.size() == 6 && snapshot->tiles.size() == 2, "必须解析完整 vertex/tile 网格");
    Require(snapshot->vertices[0].position == std::array<float, 3>{-128, 64, 0}, "西南顶点原始偏移错误");
    Require(snapshot->vertices[1].position == std::array<float, 3>{0, 64, 129}, "ground /4 与 cliff 128 层高错误");
    Require(snapshot->vertices[2].position == std::array<float, 3>{128, 64, -129}, "负 ground 偏移或低 cliff 层错误");
    Require(snapshot->vertices[3].position == std::array<float, 3>{-128, 192, -2304}, "行优先 Y 方向或 ground=0 边界错误");
    Require(snapshot->vertices[4].position[2] == 6143.75f, "共同域 ground=0x7fff 不应截成 14 位");
    Require(snapshot->vertices[1].waterBaseHeight == 1 && snapshot->vertices[1].IsMapEdge(), "water 0x4000 必须与水位分开");
    Require(snapshot->vertices[4].waterBaseHeight == 2047.75f && snapshot->vertices[5].waterBaseHeight == 0,
        "water 基础高度不能加固定材质偏移或计入 bit15");
    Require(snapshot->vertices[5].UnknownWaterFlags() == 0x8000 && snapshot->vertices[5].CliffTextureIndex() == 15,
        "未识别水标志与悬崖哨兵必须保留");
    const auto& detail = snapshot->vertices[1];
    Require(detail.GroundTextureIndex() == 1 && detail.GroundVariation() == 19 && detail.CliffVariation() == 5,
        "经典 variation 必须按 5+3 分位");
    Require(detail.IsRamp() && detail.IsBlight() && detail.HasWater() && !detail.IsBoundary()
        && snapshot->vertices[2].IsBoundary(), "packed flags 错误");
    Require(snapshot->tiles[0].vertices == std::array<std::uint32_t, 4>{0, 1, 3, 4}
        && snapshot->tiles[1].vertices == std::array<std::uint32_t, 4>{1, 2, 4, 5}, "tile SW/SE/NW/NE 索引错误");
    Require(snapshot->minimum == std::array<float, 3>{-128, 64, -2304}
        && snapshot->maximum == std::array<float, 3>{128, 192, 6143.75f}, "完整地形包围盒错误");
    Require(result.value->OriginalBytes() == fixture.bytes, "只读数据必须保留完整输入原字节");
    Require(result.diagnostics.size() == 1 && result.diagnostics.front().code == "TERRAIN_WATER_FLAG_UNKNOWN", "未知标志须有准确诊断");
    fixture.bytes[0] = std::byte{0};
    result.value.reset();
    Require(snapshot->vertices[1].position[2] == 129, "不可变快照不能依赖输入 span 或原文档的寿命");
}

void TestRejectMalformed() {
    const Fixture valid;
    for (std::size_t length = 0; length < valid.bytes.size(); ++length) {
        const auto truncated = std::span<const std::byte>(valid.bytes).first(length);
        const auto result = ParseMapTerrain(truncated);
        Require(!result.Succeeded() && !result.value, "任一截断前缀都不能被当作完整地形");
    }
    auto fixture = valid; fixture.bytes[0] = std::byte{0}; Rejected(fixture.bytes, "TERRAIN_MAGIC");
    fixture = valid; fixture.Patch32(4, 13); Rejected(fixture.bytes, "TERRAIN_VERSION_UNSUPPORTED");
    fixture = valid; fixture.Patch32(9, 2); Rejected(fixture.bytes, "TERRAIN_CUSTOM_TILESET_FLAG");
    fixture = valid; fixture.Patch32(13, 0xffffffff); Rejected(fixture.bytes, "TERRAIN_NEGATIVE_COUNT");
    fixture = valid; fixture.Patch32(13, 257); Rejected(fixture.bytes, "TERRAIN_TILESET_LIMIT");
    fixture = valid; fixture.Patch32(fixture.dimensions, 0); Rejected(fixture.bytes, "TERRAIN_DIMENSIONS");
    fixture = valid; fixture.Patch32(fixture.dimensions + 4, 1); Rejected(fixture.bytes, "TERRAIN_DIMENSIONS");
    fixture = valid; fixture.Patch32(fixture.dimensions, 0x7fffffff); Rejected(fixture.bytes, "TERRAIN_DIMENSION_LIMIT");
    fixture = valid; fixture.Patch32(fixture.offsets, std::bit_cast<std::uint32_t>(std::numeric_limits<float>::quiet_NaN())); Rejected(fixture.bytes, "TERRAIN_NONFINITE");
    fixture = valid; fixture.Patch32(fixture.offsets + 4, std::bit_cast<std::uint32_t>(std::numeric_limits<float>::infinity())); Rejected(fixture.bytes, "TERRAIN_NONFINITE");
    fixture = valid; fixture.Patch32(fixture.offsets, std::bit_cast<std::uint32_t>(16777216.0f)); Rejected(fixture.bytes, "TERRAIN_BOUNDS");
    fixture = valid; fixture.bytes.push_back(std::byte{0}); Rejected(fixture.bytes, "TERRAIN_TRAILING_DATA");
    for (const auto high : {std::uint16_t{0x8000}, std::uint16_t{0xffff}}) {
        fixture = valid;
        fixture.bytes[fixture.vertices] = static_cast<std::byte>(high & 0xff);
        fixture.bytes[fixture.vertices + 1] = static_cast<std::byte>(high >> 8);
        Rejected(fixture.bytes, "TERRAIN_GROUND_HEIGHT_AMBIGUOUS");
        const auto result = ParseMapTerrain(fixture.bytes);
        Require(result.diagnostics.front().origin->offset == fixture.vertices, "歧义 ground 必须定位具体顶点");
    }
}

void TestUnknownIndicesAndLimits() {
    Fixture fixture;
    fixture.bytes[fixture.vertices + 4] = std::byte{0x0e};
    fixture.bytes[fixture.vertices + 6] = std::byte{0xe2};
    auto result = ParseMapTerrain(fixture.bytes);
    Require(result.Succeeded() && result.value->Snapshot()->unresolvedGroundTextures == 1
        && result.value->Snapshot()->unresolvedCliffTextures == 1, "索引缺表必须保留数据并诊断，不能猜纹理");
    Require(result.value->OriginalBytes() == fixture.bytes, "未解析索引原字节改变");
    TerrainReadLimits limits;
    limits.maxBytes = 1; Rejected(fixture.bytes, "TERRAIN_BYTE_LIMIT", limits);
    limits = {}; limits.maxDimension = 2; Rejected(fixture.bytes, "TERRAIN_DIMENSION_LIMIT", limits);
    limits = {}; limits.maxVertices = 5; Rejected(fixture.bytes, "TERRAIN_VERTEX_LIMIT", limits);
    limits = {}; limits.maxTiles = 1; Rejected(fixture.bytes, "TERRAIN_TILE_LIMIT", limits);
    limits = {}; limits.maxTilesetIds = 1; Rejected(fixture.bytes, "TERRAIN_TILESET_LIMIT", limits);
    limits = {}; limits.maxDecodedBytes = fixture.bytes.size(); Rejected(fixture.bytes, "TERRAIN_MEMORY_LIMIT", limits);
    limits = {}; limits.maxAbsoluteCoordinate = 127; Rejected(fixture.bytes, "TERRAIN_BOUNDS", limits);
    limits = {}; limits.maxAbsoluteHeight = 128; Rejected(fixture.bytes, "TERRAIN_BOUNDS", limits);
    limits = {}; limits.maxAbsoluteHeight = std::numeric_limits<double>::quiet_NaN(); Rejected(fixture.bytes, "TERRAIN_LIMITS", limits);
    limits = {}; ++limits.maxDimension; Rejected(fixture.bytes, "TERRAIN_LIMITS", limits);
}

void TestVersion12ReadOnly() {
    Fixture fixture(12); fixture.bytes[fixture.vertices + 4] = std::byte{0xff};
    auto parsed = ParseMapTerrain(fixture.bytes); Require(parsed.Succeeded() && parsed.value->OriginalBytes() == fixture.bytes, "v12 原字节必须完整保留");
    auto grid = *parsed.value->Snapshot(); const auto& vertex = grid.vertices[0];
    Require(grid.header.VertexRecordBytes() == 8 && vertex.GroundTextureIndex() == 63 && vertex.IsRamp()
        && vertex.IsBlight() && vertex.HasWater() && vertex.IsBoundary() && vertex.UnknownTextureFlags() == 252,
        "v12 索引、标志与未知位不能按七字节布局解释");
    Require(grid.vertices[1].variationRaw == 0xb3 && grid.vertices.back().cliffAndLayerRaw == 0xf2, "v12 记录步长错位");
    Rejected(std::span<const std::byte>(fixture.bytes).first(fixture.bytes.size() - 1), "TERRAIN_TRUNCATED");
    MapDataEditSession session(MapDocument::FromMembers({}));
    Require(!session.AttachTerrain(fixture.bytes).Succeeded() && session.AttachTerrain(Fixture{}.bytes).Succeeded()
        && !session.SetTerrain(fixture.bytes).Succeeded(), "拒绝新版附入/覆盖必须保持经典事务可用");
    const TerrainStrokePoint point{-128, 64}; TerrainBrush brush; brush.kind = TerrainBrushKind::Raise;
    for (const auto version : {12, 11}) {
        grid.header.version = version;
        Require(!SerializeMapTerrain(grid).Succeeded() && !ApplyTerrainStroke(grid, brush, {&point, 1}).Succeeded()
            && grid.vertices[0].groundHeightRaw == 8192, "新版或伪造降版不能写出或被笔刷改动");
    }
    fixture.bytes.push_back(std::byte{0}); Rejected(fixture.bytes, "TERRAIN_TRAILING_DATA");
}

void TestPathingReadOnly() {
    const auto terrain = ParseMapTerrain(Fixture{}.bytes);
    Require(terrain.Succeeded(), "通行图测试地形损坏");
    const auto& header = terrain.value->Snapshot()->header;
    std::vector<std::byte> bytes;
    for (const auto c : std::string_view("MP3W")) bytes.push_back(static_cast<std::byte>(c));
    for (const std::uint32_t value : {0u, 8u, 4u}) {
        for (unsigned shift = 0; shift < 32; shift += 8) bytes.push_back(static_cast<std::byte>(value >> shift));
    }
    for (std::uint8_t i = 0; i < 32; ++i) bytes.push_back(static_cast<std::byte>(i % 2 ? 0xee : 0x40));
    const auto parsed = ParseMapPathing(bytes, &header);
    Require(parsed.Succeeded() && parsed.value->OriginalBytes() == bytes, "WPM 原始位没有逐字节保留");
    const auto grid = parsed.value->Snapshot();
    Require(grid->width == 8 && grid->height == 4 && grid->Cell(0, 0) == 0x40
        && grid->Cell(1, 0) == 0xee && !grid->Cell(8, 0) && !grid->Cell(0, 4),
        "WPM 西南原点行优先读取或边界有误");
    auto mismatch = header;
    ++mismatch.width;
    auto rejected = ParseMapPathing(bytes, &mismatch);
    Require(!rejected.Succeeded() && rejected.diagnostics.front().code == "PATHING_TERRAIN_DIMENSIONS",
        "WPM 与 W3E 尺寸不符仍被接受");
    bytes.pop_back();
    rejected = ParseMapPathing(bytes, &header);
    Require(!rejected.Succeeded() && rejected.diagnostics.front().code == "PATHING_TRUNCATED",
        "截断 WPM 返回半成品");
}

void InspectArchive(const std::filesystem::path& path) {
    const auto archive = MapArchiveSession::Open(path);
    Require(archive.Succeeded() && archive.value, "真实地图无法只读打开");
    const auto member = archive.value->ReadMember("war3map.w3e");
    Require(member.Succeeded() && member.value, "真实地图缺少可读取 W3E");
    const auto terrain = ParseMapTerrain(*member.value);
    for (const auto& diagnostic : terrain.diagnostics) std::cout << diagnostic.code << ": " << diagnostic.message << '\n';
    Require(terrain.Succeeded(), "真实 W3E 不在当前只读解析支持范围");
    const auto grid = terrain.value->Snapshot();
    Require(terrain.value->OriginalBytes() == *member.value, "真实 W3E 原字节未保留");
    const auto pathingMember = archive.value->ReadMember("war3map.wpm");
    Require(pathingMember.Succeeded() && pathingMember.value, "真实地图缺少可读取 WPM");
    const auto pathing = ParseMapPathing(*pathingMember.value, &grid->header);
    Require(pathing.Succeeded() && pathing.value, "真实 WPM 与 W3E 的尺寸/版本不兼容");
    Require(pathing.value->OriginalBytes() == *pathingMember.value, "真实 WPM 原字节未保留");
    Require(archive.value->VerifySourceUnchanged().Succeeded(), "只读验收期间源地图身份变化");
    std::cout << "terrain sourceSha=" << archive.value->SourceSha256() << " width=" << grid->header.width
        << " height=" << grid->header.height << " vertices=" << grid->vertices.size() << " tiles=" << grid->tiles.size()
        << " boundsMin=" << grid->minimum[0] << ',' << grid->minimum[1] << ',' << grid->minimum[2]
        << " boundsMax=" << grid->maximum[0] << ',' << grid->maximum[1] << ',' << grid->maximum[2]
        << " pathing=" << pathing.value->Snapshot()->width << 'x' << pathing.value->Snapshot()->height << '\n';
}

} // namespace

int main(int argc, char** argv) {
    try {
        Require(argc == 1 || argc == 2, "用法：terrain-tests [独立地图归档路径]");
        TestCompleteGrid();
        TestRejectMalformed();
        TestUnknownIndicesAndLimits();
        TestPathingReadOnly();
        TestVersion12ReadOnly();
        if (argc == 2) InspectArchive(std::filesystem::path(argv[1]));
        std::cout << "MapTerrain tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
