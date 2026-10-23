#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace War3Map::MapData {

// 严格物编解析器从同次捕获的实际来源解释出的普通装饰物类型。
// 不包含外部“已验证”标记，也不授予保存资格；owner仍须捕获实际占地贴图，
// 复现实例注册、解释旧WPM并核对最终来源。只列有占地的唯一rawcode。
struct TerrainNativeDoodadPathingType {
    std::string rawcode;
    std::string pathTexture;
    std::uint8_t snapCells = 2; // 实际category为C时为4，其余明确类别为2。
};

struct TerrainNativeObjectPathing {
    std::size_t workBytes = 0;
    std::vector<TerrainNativeDoodadPathingType> footprints;
};

} // namespace War3Map::MapData
