#pragma once

#include "War3MapCore/MapData/PlacementEditing.h"
#include <vector>

namespace War3Map::Modules {
struct WorldStartLocationMarker {
    MapData::PlacementId id;
    std::int32_t owner = 0;
    std::array<float, 3> position{};
};
struct WorldStartLocationCapture {
    std::vector<WorldStartLocationMarker> markers;
    std::size_t sourceCount = 0, omittedCount = 0;
};
// 在文档来源换代时调用；鼠标/动画路径只使用有界标记，不重新扫描单位表。
[[nodiscard]] Result<WorldStartLocationCapture> CaptureWorldStartLocations(const MapDocument& document,
    std::size_t maxMarkers = 256);
} // namespace War3Map::Modules
