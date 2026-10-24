#pragma once

#include "War3MapCore/MapData/MapMinimapMarkers.h"
#include "War3MapCore/MapData/MapSettings.h"
#include "War3MapCore/MapData/MapTerrain.h"

namespace War3Map::MapData {

// 坐标矩形均为西、南、东、北；候选已由实际单位/能力目录解释，保持原版枚举顺序。
// 纯计算器不读取地图、不认证来源，也不授予保存资格。
struct NativeMinimapMarkerCandidate {
    std::int32_t type = 0;
    std::array<float, 2> position{};
    std::array<std::uint8_t, 4> colorBgra{};
};

struct NativeMinimapMarkerFrame {
    std::array<float, 4> worldRectangle{};
    // 西、南、东、北边界，均为0..256；Y在写出时按原版翻转。
    std::array<std::int32_t, 4> pixelRectangle{0, 0, 256, 256};
};

// BLP与MMP同用5E7140的可玩区：CameraMargins按左、右、下、上排列，单位为地块。
// 0x400补足边距模式不需要配置；自定义相机模式必须提供实际读取的四项配置。
[[nodiscard]] Result<std::array<std::uint32_t, 4>> NativeMinimapPlayableRectangle(
    const TerrainGridSnapshot& terrain, const MapSettings& settings,
    const std::optional<std::array<double, 4>>& cameraMargins = {});

// 583530按可玩格数保持纵横比，采用原版整数截断和两侧同宽黑边。
[[nodiscard]] Result<NativeMinimapMarkerFrame> NativeMinimapFrame(
    const TerrainGridSnapshot& terrain, const std::array<std::uint32_t, 4>& playable);

// 原版没有把域外单位夹回边缘；结果可能为负或超过256。无法安全转换为I32时拒绝。
[[nodiscard]] Result<MinimapMarkersSnapshot> BuildNativeMinimapMarkers(
    const NativeMinimapMarkerFrame& frame, std::span<const NativeMinimapMarkerCandidate> candidates,
    const MinimapMarkerLimits& limits = {});

// 原版开始位置用实际TeamColor贴图的整图RGB整数均色，忽略源alpha，输出alpha固定FF。
// 输入是完整RGBA像素；不内置玩家颜色表，不把颜色采样冒充成来源认证。
[[nodiscard]] Result<std::array<std::uint8_t, 4>> AverageNativeMinimapTeamColor(
    std::span<const std::byte> rgba, std::size_t maximumPixels = 1024u * 1024u);

} // namespace War3Map::MapData
