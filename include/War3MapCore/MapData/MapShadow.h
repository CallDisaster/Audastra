#pragma once

#include "War3MapCore/Types.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace War3Map::MapData {

struct ShadowReadLimits {
    std::size_t maxBytes = 64u * 1024u * 1024u;
    std::size_t maxDimension = 8192;
    std::size_t maxCells = 64u * 1024u * 1024u;
};

struct ShadowGridSnapshot {
    std::uint32_t width = 0, height = 0;
    // 原始强度字节，不能按是否为零归一成 0/255；SHD 自身不存宽高。
    std::vector<std::uint8_t> intensities;
};

// 调用方提供已证明的网格尺寸；这里只验证长度、预算和逐字节布局。
// 不从文件长度猜尺寸，不推导坐标方向，不执行地形/地物阴影烘焙。
[[nodiscard]] Result<ShadowGridSnapshot> ParseMapShadow(std::span<const std::byte> bytes,
    std::uint32_t width, std::uint32_t height, const ShadowReadLimits& limits = {});
[[nodiscard]] Result<std::vector<std::byte>> SerializeMapShadow(const ShadowGridSnapshot& shadow,
    const ShadowReadLimits& limits = {});

} // namespace War3Map::MapData
