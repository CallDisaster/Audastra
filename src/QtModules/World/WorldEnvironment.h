#pragma once

#include "War3MapCore/MapData/MapSettings.h"

#include <optional>

namespace War3Map::Modules {

// 地图设置中影响世界环境的输入：W3I 环境字段与“使用地形迷雾”选项。
struct WorldEnvironmentInputs {
    MapData::MapEnvironmentSettings settings;
    bool terrainFog = false;
};
[[nodiscard]] std::optional<WorldEnvironmentInputs> CaptureWorldEnvironment(const MapData::MapSettings* settings);
// 两次输入只有雾不同时可只重算雾；光照环境不同须重新读取。
[[nodiscard]] bool SameWorldLighting(const WorldEnvironmentInputs& left, const WorldEnvironmentInputs& right);
[[nodiscard]] bool SameWorldFog(const WorldEnvironmentInputs& left, const WorldEnvironmentInputs& right);

} // namespace War3Map::Modules
