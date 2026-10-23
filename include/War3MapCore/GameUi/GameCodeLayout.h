#pragma once

#include "War3MapCore/GameUi/FrameTree.h"

#include <optional>
#include <string_view>
#include <vector>

// 游戏代码决定的界面位置（1.27a 经典版）：fdf 里没有、由游戏程序在运行时摆放或直接创建的部分。
// 数据来自对游戏程序的只读分析，只记录事实（锚点、偏移、尺寸），坐标为 0.8×0.6 屏幕单位。
namespace War3Map::GameUi {

// 游戏代码区域的根节点名；界面树里以此名挂出全部代码区域。
inline constexpr std::string_view kGameCodeRoot = "GameUI";

// 游戏代码直接创建、没有 fdf 模板的区域（小地图、头像、命令面板等）。
struct GameCodeArea {
    std::string_view id;         // 稳定标识（界面树中的节点名），例如 Minimap
    std::string_view label;      // 中文显示名
    std::string_view className;  // 游戏内部类名；未确认时为空
    // relativeName 为空表示相对屏幕，否则为另一区域的 id。
    std::vector<FrameAnchor> anchors;
    std::optional<float> width;
    std::optional<float> height;
};

[[nodiscard]] const std::vector<GameCodeArea>& GameCodeAreas();

// fdf 根框架由游戏代码定位时的摆放；未列出的根框架位置尚未取得。
[[nodiscard]] std::optional<RootPlacement> KnownRootPlacement(std::string_view templateName);

} // namespace War3Map::GameUi
