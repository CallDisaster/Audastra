#pragma once

#include "War3MapCore/Script/LuaTriggerUnit.h"
#include "War3MapCore/Trigger/MapObjectReferences.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace War3Map::Services {

// Agent 进游戏测试：一段紧凑 T 动作作为临时触发器加入文档副本，随 Lua 测试单元一起生成，
// 再取出它的函数正文拼成一段发给测试游戏的代码。只接受 T 写法，不接受任意 Lua 文本；
// 副本只用于本次生成，不进入作者文档、撤销栈或地图。
struct LuaTestEvalRequest {
    std::string actions; // 紧凑 T 动作，规则同 replace_trigger_text；不写段落时就是动作
    // 执行后按顺序打印的全局变量：名字或 名字[下标]，可带 udg_ 前缀；最多 32 个。
    std::vector<std::string> reads;
};
struct LuaTestEvalPlan {
    StableId triggerId = 0;
    std::vector<std::pair<std::string, std::string>> reads; // 请求原文、Lua 表达式
};

inline constexpr std::string_view kLuaTestEvalCategory = "Audastra Agent Eval";
inline constexpr std::string_view kLuaTestEvalTrigger = "AudastraAgentEval";
// 测试游戏打印每个读取值时使用的行首标记，宿主据此从控制台输出里分出读取结果。
inline constexpr std::string_view kLuaTestEvalReadMarker = "@audastra-read\t";

// 错误行列是 actions 文本自身的行列；变量不存在、数组下标不符时拒绝。
[[nodiscard]] Result<LuaTestEvalPlan> AddLuaTestEvalTrigger(Trigger::TriggerDocument& copy,
    const Trigger::UiCatalog& catalog, const LuaTestEvalRequest& request,
    const Trigger::MapObjectReferenceSnapshot* mapObjects = nullptr);
// unit 须是含临时触发器的文档以测试热重载选项生成的结果。
[[nodiscard]] Result<std::string> BuildLuaTestEvalChunk(const Script::LuaTriggerUnit& unit, const LuaTestEvalPlan& plan);

} // namespace War3Map::Services
