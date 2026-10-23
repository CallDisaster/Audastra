#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
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
    std::optional<float> fixedRotationDegrees; // 实际fixedRot；缺失时只支持旋转无关的同色正方形。
};

struct TerrainNativeObjectPathing {
    std::size_t workBytes = 0;
    std::vector<TerrainNativeDoodadPathingType> footprints;
    // 特殊DOO按地格坐标加载，不使用普通对象的旋转/吸附。空pathTexture也列出，
    // 仅表示实际getter明确禁用；调用方须与完整special记录一一绑定。
    std::vector<TerrainNativeDoodadPathingType> specialFootprints;
};

} // namespace War3Map::MapData
