#include "War3MapCore/Services/LuaTestEval.h"
#include "War3MapCore/Lua/LuaSyntax.h"
#include "War3MapCore/Runtime/LuaTestMailbox.h"
#include "War3MapCore/Trigger/TriggerGraphEditing.h"

#include <algorithm>
#include <cctype>

namespace War3Map::Script::lua { extern const std::string_view kLuaTriggerRuntimeSource; }

namespace War3Map::Services {
namespace {
template<class T> Result<T> Fail(const char* code, std::string text) {
    return Result<T>::Failure("Lua.TestEval", code, std::move(text));
}
constexpr std::size_t kActionBytes = 64 * 1024;
constexpr std::size_t kReads = 32;
constexpr std::string_view kCategoryKey = "agent-eval/category", kTriggerKey = "agent-eval/trigger";

bool Identifier(std::string_view text) {
    return !text.empty() && text.size() <= 128 && !std::isdigit(static_cast<unsigned char>(text.front()))
        && std::all_of(text.begin(), text.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; });
}
}

Result<LuaTestEvalPlan> AddLuaTestEvalTrigger(Trigger::TriggerDocument& copy, const Trigger::UiCatalog& catalog,
    const LuaTestEvalRequest& request, const Trigger::MapObjectReferenceSnapshot* mapObjects) {
    using namespace Trigger;
    if (request.actions.empty() || request.actions.size() > kActionBytes)
        return Fail<LuaTestEvalPlan>("LUA_TEST_EVAL_ACTIONS", "actions 须是 1..65536 字节的紧凑 T 文本。");
    if (request.reads.size() > kReads) return Fail<LuaTestEvalPlan>("LUA_TEST_EVAL_READS", "reads 最多 32 项。");
    LuaTestEvalPlan plan;
    for (const auto& read : request.reads) {
        std::string_view text = read;
        if (text.starts_with("udg_")) text.remove_prefix(4);
        std::string_view name = text, index;
        if (const auto open = text.find('['); open != std::string_view::npos) {
            if (!text.ends_with("]")) return Fail<LuaTestEvalPlan>("LUA_TEST_EVAL_READS", "读取项写法应为 名字 或 名字[下标]：" + read);
            name = text.substr(0, open); index = text.substr(open + 1, text.size() - open - 2);
            if (index.empty() || index.size() > 5 || !std::all_of(index.begin(), index.end(), [](char c) { return c >= '0' && c <= '9'; }))
                return Fail<LuaTestEvalPlan>("LUA_TEST_EVAL_READS", "数组下标须是非负整数：" + read);
        }
        const auto variable = std::find_if(copy.variables.begin(), copy.variables.end(),
            [&](const auto& row) { return row.name.text == name; });
        if (!Identifier(name) || variable == copy.variables.end())
            return Fail<LuaTestEvalPlan>("LUA_TEST_EVAL_READ_UNKNOWN", "没有全局变量 " + std::string(name) + "。");
        if ((variable->isArray != 0) != !index.empty())
            return Fail<LuaTestEvalPlan>("LUA_TEST_EVAL_READS", variable->isArray != 0 ? read + " 是数组，须写下标。" : read + " 不是数组，不能写下标。");
        if (!index.empty() && std::stoi(std::string(index)) > variable->size)
            return Fail<LuaTestEvalPlan>("LUA_TEST_EVAL_READS", read + " 的下标超出数组长度。");
        plan.reads.emplace_back(read, "udg_" + std::string(name) + (index.empty() ? "" : "[" + std::string(index) + "]"));
    }
    const auto clash = [](const auto& rows, std::string_view name) {
        return std::any_of(rows.begin(), rows.end(), [&](const auto& row) { return row.name.text == name; });
    };
    if (clash(copy.categories, kLuaTestEvalCategory) || clash(copy.triggers, kLuaTestEvalTrigger))
        return Fail<LuaTestEvalPlan>("LUA_TEST_EVAL_NAME_TAKEN", "地图里已有名为 " + std::string(kLuaTestEvalTrigger) + " 的触发器或同名类别。");

    const auto binding = CaptureTriggerGraphBinding(copy, catalog, mapObjects);
    if (!binding.Succeeded() || !binding.value) return {std::nullopt, binding.diagnostics};
    GraphParentExpectation categories{GraphParentKind::Categories, {}, {}, std::nullopt};
    for (const auto& row : copy.categories) categories.orderedChildren.push_back({GraphRecordKind::Category, row.id, {}});
    const GraphRecordRef category{GraphRecordKind::Category, 0, std::string(kCategoryKey)};
    const GraphRecordRef trigger{GraphRecordKind::Trigger, 0, std::string(kTriggerKey)};
    TriggerGraphEditBatch batch{*binding.value, {}};
    batch.operations.emplace_back(GraphCreate{std::string(kCategoryKey), GraphCategoryValue{std::string(kLuaTestEvalCategory), false},
        categories, std::nullopt});
    batch.operations.emplace_back(GraphCreate{std::string(kTriggerKey),
        GraphTriggerValue{std::string(kLuaTestEvalTrigger), {}, true, true, false, false},
        GraphParentExpectation{GraphParentKind::CategoryTriggers, category, {}, std::nullopt}, std::nullopt});
    batch.operations.emplace_back(GraphReplaceTriggerText{trigger, request.actions});
    const auto applied = ApplyTriggerGraphEditBatch(copy, catalog, batch, {}, mapObjects);
    if (!applied.Succeeded() || !applied.value) return {std::nullopt, applied.diagnostics};
    plan.triggerId = applied.value->created.at(std::string(kTriggerKey)).id;
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
