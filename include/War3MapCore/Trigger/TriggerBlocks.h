#pragma once

#include "War3MapCore/Trigger/UiCatalog.h"

#include <cstdint>
#include <string_view>
#include <vector>

namespace War3Map::Trigger {

// 块语句的分支（WTG 只存子节点的分支号，分支的种类与名称由编辑器按函数名决定）。
// 原版块语句与 YDWE 的块语句（计时器 / 注册触发器 / 运行触发器 / 选取单位 / 区域 / 循环）的分支表
// 参照两者的公开行为；其余 …Multiple 视为单个“循环 - 动作”。界面、紧凑 T 写法共用这一份表。
struct TriggerBlockBranch {
    std::int32_t branch = 0;
    std::int32_t type = 2;          // 子节点 ECA 种类：0 事件、1 条件、2 动作
    const char* label = nullptr;    // UTF-8 显示名
    const char* key = nullptr;      // 紧凑 T 写法里的分支标签（ASCII）
};

[[nodiscard]] bool IsIfThenElseBlock(std::string_view name) noexcept;
// 不是块语句时返回空指针。catalog 给出时，函数库声明多一个末尾 code 参数的动作（回调块）也有一个“动作”分支。
[[nodiscard]] const std::vector<TriggerBlockBranch>* FindTriggerBlockBranches(
    std::string_view name, const UiCatalog* catalog = nullptr);

} // namespace War3Map::Trigger
