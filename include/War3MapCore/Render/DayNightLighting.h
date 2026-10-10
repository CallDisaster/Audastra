#pragma once

#include "War3MapCore/Render/MdxModel.h"
#include "War3MapCore/Text/TextUtil.h"

#include <array>
#include <optional>
#include <string>

namespace War3Map::Render {

// 某一时刻的场景光：指向光源的单位方向，直射光与环境光（颜色 × 强度，分量可大于 1）。
struct SceneLight {
    std::array<float, 3> towardLight{0, 0, 1};
    std::array<float, 3> diffuse{};
    std::array<float, 3> ambient{};
};

// 编辑器预览的默认时刻（中午）。
constexpr float kDefaultDayNightHours = 12.0f;

// 原版昼夜光照取 DNC 模型的第一个平行光。Stand 动作区间线性对应一天 0–24 时；
// 颜色与强度轨迹按线性插值（文件中的 Hermite 切线未使用）；光沿节点旋转后的 −Z 方向射出。
// 没有平行光或没有 Stand 动作时返回空。
[[nodiscard]] std::optional<SceneLight> SampleDayNight(const MdxModel& model, float hours);

// 地形/单位 DNC 模型路径：查 WorldEditData.txt 的 [TerrainLights] / [UnitLights]（键为 tileset 字母），
// 没有条目时用游戏内置默认 Lordaeron。lightEnvironment（W3I）非 0 时代替主 tileset。
// 表中写 .mdl，返回实际文件 .mdx。
[[nodiscard]] std::string DayNightModelPath(const Text::ProfileSections& worldEditData, char mainTileset,
    std::uint8_t lightEnvironment, bool unit);

} // namespace War3Map::Render
