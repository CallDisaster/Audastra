#pragma once

#include "War3MapCore/Types.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace War3Map::MapData {

struct MinimapMarkerLimits {
    std::size_t maxBytes = 16u * 1024u * 1024u;
    std::size_t maxMarkers = 65536;
};

struct MinimapMarkerRecord {
    // 记录是完整的四个 32 位字段；未知类型、坐标和颜色均原样保留。
    // 此处不把原始坐标换算为世界坐标，也不从单位或开始位置推导记录。
    std::int32_t typeRaw = 0;
    std::array<std::int32_t, 2> positionRaw{};
    std::array<std::uint8_t, 4> colorBgra{};
};

struct MinimapMarkersSnapshot {
    std::int32_t version = 0;
    std::vector<MinimapMarkerRecord> records;
};

// 严格读取 MMP v0 的 8 字节头和 16 字节记录；未知版本、截断、尾字节均拒绝。
// 只证明格式完整和可逐字往返，不证明标记类型、坐标映射或原版生成时机。
[[nodiscard]] Result<MinimapMarkersSnapshot> ParseMapMinimapMarkers(std::span<const std::byte> bytes,
    const MinimapMarkerLimits& limits = {});
[[nodiscard]] Result<std::vector<std::byte>> SerializeMapMinimapMarkers(const MinimapMarkersSnapshot& markers,
    const MinimapMarkerLimits& limits = {});

} // namespace War3Map::MapData
