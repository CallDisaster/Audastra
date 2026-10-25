#include "War3MapCore/Services/LuaTestEval.h"
#include "War3MapCore/Lua/LuaSyntax.h"
#include "War3MapCore/Runtime/LuaTestMailbox.h"
#include "TestTriggerCopy.h"

#include <algorithm>

namespace War3Map::Script::lua { extern const std::string_view kLuaTriggerRuntimeSource; }

namespace War3Map::Services {
namespace {
template<class T> Result<T> Fail(const char* code, std::string text) {
    return Result<T>::Failure("Lua.TestEval", code, std::move(text));
}
constexpr std::size_t kActionBytes = 64 * 1024;
}

Result<LuaTestEvalPlan> AddLuaTestEvalTrigger(Trigger::TriggerDocument& copy, const Trigger::UiCatalog& catalog,
    const LuaTestEvalRequest& request, const Trigger::MapObjectReferenceSnapshot* mapObjects) {
    if (request.actions.empty() || request.actions.size() > kActionBytes)
        return Fail<LuaTestEvalPlan>("LUA_TEST_EVAL_ACTIONS", "actions 须是 1..65536 字节的紧凑 T 文本。");
    const auto reads = detail::ResolveTestReads(copy, request.reads, "Lua.TestEval", "LUA_TEST_EVAL");
    if (!reads.Succeeded() || !reads.value) return {std::nullopt, reads.diagnostics};
    LuaTestEvalPlan plan;
    for (const auto& read : *reads.value)
        plan.reads.emplace_back(read.request, "udg_" + read.name + (read.index.empty() ? "" : "[" + read.index + "]"));
    const auto trigger = detail::AddTestTrigger(copy, catalog, kLuaTestEvalCategory, kLuaTestEvalTrigger, "agent-eval",
        request.actions, "Lua.TestEval", "LUA_TEST_EVAL", mapObjects);
    if (!trigger.Succeeded() || !trigger.value) return {std::nullopt, trigger.diagnostics};
    plan.triggerId = *trigger.value;
    return Result<LuaTestEvalPlan>::Success(std::move(plan));
}

Result<std::string> BuildLuaTestEvalChunk(const Script::LuaTriggerUnit& unit, const LuaTestEvalPlan& plan) {
    if (!unit.Succeeded() || !unit.testHotReload)
        return Fail<std::string>("LUA_TEST_EVAL_UNIT", "没有以测试选项成功生成的 Lua 单元。");
    const auto chunk = std::find_if(unit.testTriggers.begin(), unit.testTriggers.end(),
        [&](const auto& row) { return row.symbol.triggerId == plan.triggerId; });
    if (chunk == unit.testTriggers.end() || chunk->customCode)
        return Fail<std::string>("LUA_TEST_EVAL_UNIT", "生成结果里没有测试步骤的函数。");
    // 只调用动作函数：测试步骤不注册事件、不经过条件，读取值在动作之后打印。
    // 生成的函数用到的运行时辅助函数是各代码块内的局部函数，与热重载补丁一样先带上。
    std::string source = "-- Audastra agent test steps\n" + std::string(Script::lua::kLuaTriggerRuntimeSource) + "\n"
        + chunk->functions + chunk->symbol.actions + "()\n";
    for (std::size_t index = 0; index < plan.reads.size(); ++index)
        source += "print(\"" + std::string(kLuaTestEvalReadMarker.substr(0, kLuaTestEvalReadMarker.size() - 1)) + "\\t"
            + std::to_string(index) + "\\t\" .. tostring(" + plan.reads[index].second + "))\n";
    Lua::SyntaxOptions options; options.maxSourceBytes = Audastra::Runtime32::kLuaReloadSourceCapacity - 1;
    const auto checked = Lua::CheckSyntax(source, "@Audastra/agent-steps.lua", options);
    if (!checked.Succeeded())
        return Fail<std::string>("LUA_TEST_EVAL_SYNTAX", checked.diagnostics.empty() ? "测试步骤生成的代码过大或无法编译。"
            : "测试步骤生成的代码无法编译：" + checked.diagnostics.front().message);
    return Result<std::string>::Success(std::move(source));
}

} // namespace War3Map::Services
