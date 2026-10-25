#pragma once

#include "War3MapCore/MapData/MapTerrain.h"

#include <bit>
#include <cmath>
#include <limits>
#include <utility>

// 整图解析和有界行扫描共用唯一W3E字段解码；不在消费者复制格式实现。
namespace War3Map::MapData::TerrainDecode {

struct TerrainError { Diagnostic diagnostic; };

class Reader {
public:
    explicit Reader(std::span<const std::byte> bytes, std::size_t offset = 0) : bytes_(bytes), offset_(offset) {}
    [[noreturn]] void Fail(const char* code, std::string message) const {
        throw TerrainError{{DiagnosticSeverity::Error, "MapTerrain", code, std::move(message),
            OriginSpan{"war3map.w3e", offset_, offset_ < bytes_.size() ? 1u : 0u, {}}}};
    }
    [[noreturn]] void FailAt(const char* code, std::string message, std::size_t offset) const {
        throw TerrainError{{DiagnosticSeverity::Error, "MapTerrain", code, std::move(message),
            OriginSpan{"war3map.w3e", offset, offset < bytes_.size() ? 1u : 0u, {}}}};
    }
    std::size_t Offset() const noexcept { return offset_; }
    std::size_t Remaining() const noexcept { return bytes_.size() - offset_; }
    void Need(std::size_t length) const {
        if (length > Remaining()) Fail("TERRAIN_TRUNCATED", "地形成员在字段或顶点记录中间截断");
    }
    std::uint8_t U8() { Need(1); return std::to_integer<std::uint8_t>(bytes_[offset_++]); }
    std::uint16_t U16() {
        const auto low = U8();
        const auto high = U8();
        return static_cast<std::uint16_t>(static_cast<std::uint16_t>(low) | (static_cast<std::uint16_t>(high) << 8));
    }
    std::uint32_t U32() {
        Need(4);
        std::uint32_t value = 0;
        for (unsigned shift = 0; shift < 32; shift += 8) value |= static_cast<std::uint32_t>(U8()) << shift;
        return value;
    }
    std::int32_t I32() { return std::bit_cast<std::int32_t>(U32()); }
    float F32() {
        const auto offset = offset_;
        const auto value = std::bit_cast<float>(U32());
        if (!std::isfinite(value)) FailAt("TERRAIN_NONFINITE", "地形坐标不是有限数", offset);
        return value;
    }
    std::string Rawcode() {
        Need(4);
        std::string result;
        result.reserve(4);
        for (int i = 0; i < 4; ++i) result.push_back(static_cast<char>(U8()));
        return result;
    }
private:
    std::span<const std::byte> bytes_;
    std::size_t offset_ = 0;
};

inline void SpendBytes(Reader& reader, std::size_t& used, std::size_t count, std::size_t size, std::size_t limit) {
    if (used > limit || count > (limit - used) / size) reader.Fail("TERRAIN_MEMORY_LIMIT", "地形解码内存超过预算");
    used += count * size;
}

inline bool ValidLimits(const TerrainReadLimits& limits) {
    const TerrainReadLimits hard;
    return limits.maxBytes <= hard.maxBytes && limits.maxDimension <= hard.maxDimension
        && limits.maxVertices <= hard.maxVertices && limits.maxTiles <= hard.maxTiles
        && limits.maxTilesetIds <= hard.maxTilesetIds && limits.maxDecodedBytes <= hard.maxDecodedBytes
        && std::isfinite(limits.maxAbsoluteCoordinate) && limits.maxAbsoluteCoordinate >= 0
        && limits.maxAbsoluteCoordinate <= hard.maxAbsoluteCoordinate
        && std::isfinite(limits.maxAbsoluteHeight) && limits.maxAbsoluteHeight >= 0
        && limits.maxAbsoluteHeight <= hard.maxAbsoluteHeight;
}

inline void ReadTilesets(Reader& reader, std::vector<std::string>& output, const TerrainReadLimits& limits,
    std::size_t& decodedBytes) {
    const auto count = reader.I32();
    if (count < 0) reader.Fail("TERRAIN_NEGATIVE_COUNT", "地形纹理表数量不能为负数");
    const auto size = static_cast<std::size_t>(count);
    if (size > limits.maxTilesetIds) reader.Fail("TERRAIN_TILESET_LIMIT", "地形纹理表超过数量预算");
    if (size > reader.Remaining() / 4) reader.Fail("TERRAIN_TRUNCATED", "纹理表数量超出剩余字节");
    SpendBytes(reader, decodedBytes, size, sizeof(std::string) + 4, limits.maxDecodedBytes);
    output.reserve(size);
    for (std::size_t index = 0; index < size; ++index) output.push_back(reader.Rawcode());
}

inline void CheckBound(Reader& reader, double value, double limit, const char* field) {
    if (!std::isfinite(value) || std::abs(value) > limit) {
        reader.Fail("TERRAIN_BOUNDS", std::string(field) + "超过有限世界坐标/高度预算");
    }
}

inline void AddWarning(std::vector<Diagnostic>& output, const char* code, std::string message,
    std::size_t offset, std::size_t recordBytes) {
    output.push_back({DiagnosticSeverity::Warning, "MapTerrain", code, std::move(message),
        OriginSpan{"war3map.w3e", offset, recordBytes, {}}});
}

struct TerrainLayout {
    TerrainHeader header;
    std::size_t vertexCount = 0, tileCount = 0;
};

inline TerrainLayout ReadLayout(Reader& reader, const TerrainReadLimits& limits, std::size_t& decodedBytes) {
    if (reader.Rawcode() != "W3E!") reader.FailAt("TERRAIN_MAGIC", "地形成员签名不是 W3E!", 0);
    TerrainLayout layout;
    auto& header = layout.header;
    header.version = reader.I32();
    const auto recordBytes = header.VertexRecordBytes();
    if (recordBytes == 0) reader.FailAt("TERRAIN_VERSION_UNSUPPORTED", "当前只读取 W3E v11/v12；未知版本不降级试读", 4);
    header.mainTileset = reader.U8();
    const auto custom = reader.I32();
    if (custom != 0 && custom != 1) reader.FailAt("TERRAIN_CUSTOM_TILESET_FLAG", "自定义地形标志必须为 0 或 1", 9);
    header.usesCustomTileset = custom != 0;
    ReadTilesets(reader, header.groundTilesets, limits, decodedBytes);
    ReadTilesets(reader, header.cliffTilesets, limits, decodedBytes);
    const auto width = reader.I32();
    const auto height = reader.I32();
    if (width < 2 || height < 2) reader.Fail("TERRAIN_DIMENSIONS", "地形宽高必须至少含两个顶点（一个 tile）");
    if (static_cast<std::size_t>(width) > limits.maxDimension || static_cast<std::size_t>(height) > limits.maxDimension) {
        reader.Fail("TERRAIN_DIMENSION_LIMIT", "地形单边顶点数超过预算");
    }
    header.width = static_cast<std::uint32_t>(width);
    header.height = static_cast<std::uint32_t>(height);
    // 先做除法界限检查，再相乘、分配；同时保证所有 tile 索引都能放入 uint32。
    const auto columns = static_cast<std::size_t>(width);
    const auto rows = static_cast<std::size_t>(height);
    if (rows > limits.maxVertices / columns) reader.Fail("TERRAIN_VERTEX_LIMIT", "地形顶点总数超过预算");
    const auto vertexCount = columns * rows;
    layout.vertexCount = vertexCount;
    if (vertexCount > std::numeric_limits<std::uint32_t>::max()) reader.Fail("TERRAIN_VERTEX_LIMIT", "地形顶点索引超过 uint32 范围");
    if (rows - 1 > limits.maxTiles / (columns - 1)) reader.Fail("TERRAIN_TILE_LIMIT", "地形 tile 总数超过预算");
    layout.tileCount = (columns - 1) * (rows - 1);
    for (auto& coordinate : header.offset) coordinate = reader.F32();
    CheckBound(reader, header.offset[0], limits.maxAbsoluteCoordinate, "地形西边界");
    CheckBound(reader, header.offset[1], limits.maxAbsoluteCoordinate, "地形南边界");
    CheckBound(reader, static_cast<double>(header.offset[0]) + static_cast<double>(columns - 1) * 128.0, limits.maxAbsoluteCoordinate, "地形东边界");
    CheckBound(reader, static_cast<double>(header.offset[1]) + static_cast<double>(rows - 1) * 128.0, limits.maxAbsoluteCoordinate, "地形北边界");
    if (vertexCount > reader.Remaining() / recordBytes) reader.Fail("TERRAIN_TRUNCATED", "地形顶点数量超出该版本的剩余记录");
    if (reader.Remaining() != vertexCount * recordBytes) reader.Fail("TERRAIN_TRAILING_DATA", "地形结构之后有未识别字节，拒绝不完整解释");
    return layout;
}

inline TerrainVertex ReadVertex(Reader& reader, const TerrainHeader& header, std::size_t x, std::size_t y,
    const TerrainReadLimits& limits) {
    const auto recordOffset = reader.Offset();
    TerrainVertex vertex;
    vertex.groundHeightRaw = reader.U16();
    vertex.waterHeightAndFlagsRaw = reader.U16();
    vertex.textureAndFlagsRaw = reader.U8();
    // v12 的八字节布局依据研究 §8 的公开格式证据；没有据此开放原版保存。
    // byte B 必须原样保留，不能仿照某些写入器清掉其高六位。
    vertex.extendedTextureLayout = header.version == 12;
    if (vertex.extendedTextureLayout) vertex.textureFlagsExtensionRaw = reader.U8();
    vertex.variationRaw = reader.U8();
    vertex.cliffAndLayerRaw = reader.U8();
    if ((vertex.groundHeightRaw & 0x8000) != 0) {
        reader.FailAt("TERRAIN_GROUND_HEIGHT_AMBIGUOUS", "ground raw bit15 置位，有符号解释尚无一致证据；原成员保留，不生成猜测预览", recordOffset);
    }
    const double z = (static_cast<double>(vertex.groundHeightRaw) - 8192.0) / 4.0
        + 128.0 * (static_cast<int>(vertex.CliffLevel()) - 2);
    const double water = (static_cast<double>(vertex.waterHeightAndFlagsRaw & 0x3fff) - 8192.0) / 4.0;
    CheckBound(reader, z, limits.maxAbsoluteHeight, "地形高度");
    CheckBound(reader, water, limits.maxAbsoluteHeight, "水基础高度");
    vertex.position = {static_cast<float>(static_cast<double>(header.offset[0]) + static_cast<double>(x) * 128.0),
        static_cast<float>(static_cast<double>(header.offset[1]) + static_cast<double>(y) * 128.0), static_cast<float>(z)};
    vertex.waterBaseHeight = static_cast<float>(water);
    return vertex;
}

} // namespace War3Map::MapData::TerrainDecode
