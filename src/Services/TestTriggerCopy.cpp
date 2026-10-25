#include "TestTriggerCopy.h"

#include "War3MapCore/Trigger/TriggerGraphEditing.h"

#include <algorithm>
#include <cctype>

namespace War3Map::Services::detail {
namespace {
constexpr std::size_t kReads = 32;

bool Identifier(std::string_view text) {
    return !text.empty() && text.size() <= 128 && !std::isdigit(static_cast<unsigned char>(text.front()))
        && std::all_of(text.begin(), text.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; });
}
template<class T> Result<T> Fail(std::string_view subsystem, std::string_view prefix, std::string_view suffix, std::string text) {
    return Result<T>::Failure(std::string(subsystem), std::string(prefix) + std::string(suffix), std::move(text));
}
}

Result<std::vector<TestRead>> ResolveTestReads(const Trigger::TriggerDocument& copy,
    std::span<const std::string> reads, std::string_view subsystem, std::string_view codePrefix) {
    using Output = std::vector<TestRead>;
    if (reads.size() > kReads) return Fail<Output>(subsystem, codePrefix, "_READS", "reads 最多 32 项。");
    Output output;
    for (const auto& read : reads) {
        std::string_view text = read;
        if (text.starts_with("udg_")) text.remove_prefix(4);
        std::string_view name = text, index;
        if (const auto open = text.find('['); open != std::string_view::npos) {
            if (!text.ends_with("]")) return Fail<Output>(subsystem, codePrefix, "_READS", "读取项写法应为 名字 或 名字[下标]：" + read);
            name = text.substr(0, open); index = text.substr(open + 1, text.size() - open - 2);
            if (index.empty() || index.size() > 5 || !std::all_of(index.begin(), index.end(), [](char c) { return c >= '0' && c <= '9'; }))
                return Fail<Output>(subsystem, codePrefix, "_READS", "数组下标须是非负整数：" + read);
        }
        const auto variable = std::find_if(copy.variables.begin(), copy.variables.end(),
            [&](const auto& row) { return row.name.text == name; });
        if (!Identifier(name) || variable == copy.variables.end())
            return Fail<Output>(subsystem, codePrefix, "_READ_UNKNOWN", "没有全局变量 " + std::string(name) + "。");
        if ((variable->isArray != 0) != !index.empty())
            return Fail<Output>(subsystem, codePrefix, "_READS", variable->isArray != 0 ? read + " 是数组，须写下标。" : read + " 不是数组，不能写下标。");
        // 变量的“大小”只是开局初始化的个数；JASS 数组都有 8192 格（经典版上限，新版更大），下标按它检查。
        if (!index.empty() && std::stoi(std::string(index)) >= 8192)
            return Fail<Output>(subsystem, codePrefix, "_READS", read + " 的下标超出 JASS 数组范围 0..8191。");
        output.push_back({read, std::string(name), std::string(index), &*variable});
    }
    return Result<Output>::Success(std::move(output));
}

Result<StableId> AddTestTrigger(Trigger::TriggerDocument& copy, const Trigger::UiCatalog& catalog,
    std::string_view category, std::string_view trigger, std::string_view keyPrefix, std::string_view text,
    std::string_view subsystem, std::string_view codePrefix, const Trigger::MapObjectReferenceSnapshot* mapObjects) {
    using namespace Trigger;
    const auto clash = [](const auto& rows, std::string_view name) {
        return std::any_of(rows.begin(), rows.end(), [&](const auto& row) { return row.name.text == name; });
    };
    if (clash(copy.categories, category) || clash(copy.triggers, trigger))
        return Fail<StableId>(subsystem, codePrefix, "_NAME_TAKEN", "地图里已有名为 " + std::string(trigger) + " 的触发器或同名类别。");
    const auto binding = CaptureTriggerGraphBinding(copy, catalog, mapObjects);
    if (!binding.Succeeded() || !binding.value) return {std::nullopt, binding.diagnostics};
    const auto categoryKey = std::string(keyPrefix) + "/category", triggerKey = std::string(keyPrefix) + "/trigger";
    GraphParentExpectation categories{GraphParentKind::Categories, {}, {}, std::nullopt};
    for (const auto& row : copy.categories) categories.orderedChildren.push_back({GraphRecordKind::Category, row.id, {}});
    const GraphRecordRef categoryRef{GraphRecordKind::Category, 0, categoryKey};
    const GraphRecordRef triggerRef{GraphRecordKind::Trigger, 0, triggerKey};
    TriggerGraphEditBatch batch{*binding.value, {}};
    batch.operations.emplace_back(GraphCreate{categoryKey, GraphCategoryValue{std::string(category), false}, categories, std::nullopt});
    batch.operations.emplace_back(GraphCreate{triggerKey, GraphTriggerValue{std::string(trigger), {}, true, true, false, false},
        GraphParentExpectation{GraphParentKind::CategoryTriggers, categoryRef, {}, std::nullopt}, std::nullopt});
    batch.operations.emplace_back(GraphReplaceTriggerText{triggerRef, std::string(text)});
    const auto applied = ApplyTriggerGraphEditBatch(copy, catalog, batch, {}, mapObjects);
    if (!applied.Succeeded() || !applied.value) return {std::nullopt, applied.diagnostics};
    return Result<StableId>::Success(applied.value->created.at(triggerKey).id);
}

} // namespace War3Map::Services::detail
