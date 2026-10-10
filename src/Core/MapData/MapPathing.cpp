#include "War3MapCore/MapData/MapPathing.h"

#include <algorithm>
#include <array>
#include <new>

namespace War3Map::MapData {
namespace {

Result<MapPathing> Fail(std::string code, std::string message, std::size_t offset) {
    auto result = Result<MapPathing>::Failure("MapPathing", std::move(code), std::move(message));
    result.diagnostics.front().origin = OriginSpan{"war3map.wpm", offset, 0, {}};
    return result;
}

std::uint32_t U32(std::span<const std::byte> bytes, std::size_t offset) {
    std::uint32_t value = 0;
    for (unsigned shift = 0; shift < 32; shift += 8) {
        value |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + shift / 8])) << shift;
    }
    return value;
}

} // namespace

std::optional<std::uint8_t> PathingGridSnapshot::Cell(std::uint32_t x, std::uint32_t y) const noexcept {
    if (x >= width || y >= height) return std::nullopt;
    const auto index = static_cast<std::size_t>(y) * width + x;
    if (index >= cells.size()) return std::nullopt;
    return cells[index];
}

Result<MapPathing> ParseMapPathing(std::span<const std::byte> bytes,
    const TerrainHeader* terrain, const PathingReadLimits& limits) {
    const PathingReadLimits hard;
    if (limits.maxBytes == 0 || limits.maxDimension == 0 || limits.maxCells == 0
        || limits.maxBytes > hard.maxBytes || limits.maxDimension > hard.maxDimension
        || limits.maxCells > hard.maxCells) {
        return Fail("PATHING_LIMITS", "调用方只能收紧非零 WPM 读取预算", 0);
    }
    if (bytes.size() > limits.maxBytes) return Fail("PATHING_BYTE_LIMIT", "WPM 超过读取字节预算", 0);
    if (bytes.size() < 16) return Fail("PATHING_TRUNCATED", "WPM 头部不足 16 字节", bytes.size());
    constexpr std::array magic{std::byte{'M'}, std::byte{'P'}, std::byte{'3'}, std::byte{'W'}};
    if (!std::equal(magic.begin(), magic.end(), bytes.begin())) {
        return Fail("PATHING_MAGIC", "WPM 签名不是 MP3W", 0);
    }
    const auto version = U32(bytes, 4);
    if (version != 0) return Fail("PATHING_VERSION_UNSUPPORTED", "只读取经典 MP3W v0；未知版本不降级试读", 4);
    const auto width = U32(bytes, 8), height = U32(bytes, 12);
    if (width == 0 || height == 0) return Fail("PATHING_DIMENSIONS", "WPM 网格宽高必须为正", 8);
    if (width > limits.maxDimension || height > limits.maxDimension
        || static_cast<std::size_t>(height) > limits.maxCells / width) {
        return Fail("PATHING_GRID_LIMIT", "WPM 网格尺寸或格数超过预算", 8);
    }
    const auto count = static_cast<std::size_t>(width) * height;
    if (count > bytes.size() - 16) return Fail("PATHING_TRUNCATED", "WPM 格子数据被截断", bytes.size());
    if (count < bytes.size() - 16) return Fail("PATHING_TRAILING_DATA", "WPM 格子数据后有未识别字节", 16 + count);
    if (terrain != nullptr) {
        if (terrain->width < 2 || terrain->height < 2
            || static_cast<std::uint64_t>(terrain->width - 1) * 4 != width
            || static_cast<std::uint64_t>(terrain->height - 1) * 4 != height) {
            return Fail("PATHING_TERRAIN_DIMENSIONS", "WPM 格子尺寸与 W3E tile 尺寸不匹配", 8);
        }
    }
    try {
        auto snapshot = std::make_shared<PathingGridSnapshot>();
        snapshot->version = version;
        snapshot->width = width;
        snapshot->height = height;
        snapshot->cells.reserve(count);
        for (std::size_t i = 16; i < bytes.size(); ++i) {
            snapshot->cells.push_back(std::to_integer<std::uint8_t>(bytes[i]));
        }
        MapPathing pathing;
        pathing.original_.assign(bytes.begin(), bytes.end());
        pathing.snapshot_ = std::move(snapshot);
        return Result<MapPathing>::Success(std::move(pathing));
    } catch (const std::bad_alloc&) {
        return Fail("PATHING_ALLOCATION_FAILED", "无法分配 WPM 只读快照内存", 0);
    }
}

} // namespace War3Map::MapData
