#pragma once

namespace War3Map {

// WarVK 专用功能是否对作者显示：地图设置的 WarVK 分组、物编装饰物的“远景”勾选、Agent 的 map_warvk_* 工具，
// 以及模型几何体超出游戏上限的提醒。预览版与开发构建显示；正式版构建关闭（CMake AUDASTRA_WARVK_FEATURES=OFF）。
// 只藏入口，不改数据通路：地图里已有的 WarVK 设置照常读取、保存并写出初始化段。
[[nodiscard]] bool WarVKFeaturesShown() noexcept;

} // namespace War3Map
