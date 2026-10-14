#pragma once

#include <filesystem>

// 只读体检：地表/水面贴图，以及物编里改过模型的装饰物、可破坏物、单位、物品的模型和模型贴图。
// 与世界页同样按“地图（含清单外成员）→ 游戏”查找，逐项写出来源与失败原因，最后按原因汇总。
int RunResourceCheck(const std::filesystem::path& mapPath, const std::filesystem::path& gameRoot);
