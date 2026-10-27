#include "War3MapCore/Trigger/LegacyTriggerReferences.h"

#include "War3MapCore/Trigger/TriggerDirectory.h"

#include <algorithm>
#include <set>
#include <unordered_set>

namespace War3Map::Trigger {

std::map<std::string, std::vector<StableId>> LegacyTriggerSymbols(const TriggerDocument& document) {
    constexpr std::string_view prefix = "gg_trg_";
    std::unordered_set<std::string> current;
    for (const auto& trigger : document.triggers) {
        if (trigger.type == 0) current.insert(TriggerDirectoryEditor::TriggerSymbol(trigger.name));
    }
    // 旧形式只在不和任何 WE 写法撞名时才认。
    std::map<std::string, std::vector<StableId>> legacy;
    for (const auto& trigger : document.triggers) {
        if (trigger.type != 0) continue;
        for (const auto& key : {std::string(prefix) + TriggerDirectoryEditor::ScriptName(trigger.name.text),
                 std::string(prefix) + trigger.name.text}) {
            if (current.contains(key)) continue;
            auto& targets = legacy[key];
            if (targets.empty() || targets.back() != trigger.id) targets.push_back(trigger.id);
        }
    }
    return legacy;
}

std::vector<LegacyTriggerReference> FindLegacyTriggerReferences(const TriggerDocument& document,
    std::optional<StableId> ownerTrigger) {
    std::vector<LegacyTriggerReference> result;
    const auto legacy = LegacyTriggerSymbols(document);
    if (legacy.empty()) return result;
    for (const auto& owner : document.triggers) {
        if (owner.type != 0 || (ownerTrigger && owner.id != *ownerTrigger)) continue;
        // 显式栈走完该触发器的 ECA、参数与嵌套调用；坏文档里的重复指向只走一次。
        std::vector<std::uint32_t> ecas(owner.ecas.rbegin(), owner.ecas.rend()), arguments;
        std::set<std::uint32_t> seenEcas, seenArguments;
        while (!ecas.empty() || !arguments.empty()) {
            if (!arguments.empty()) {
                const auto index = arguments.back();
                arguments.pop_back();
                if (index >= document.argumentNodes.size() || !seenArguments.insert(index).second) continue;
                const auto& argument = document.argumentNodes[index];
                if (argument.indexArgument) arguments.push_back(*argument.indexArgument);
                if (argument.callEca) ecas.push_back(*argument.callEca);
                if (argument.type != 1) continue;
                const auto found = legacy.find(argument.value.text);
                if (found == legacy.end() || found->second.size() != 1) continue;
                const auto target = std::find_if(document.triggers.begin(), document.triggers.end(),
                    [&](const TriggerRecord& trigger) { return trigger.id == found->second.front(); });
                result.push_back({argument.id, owner.id, target->id, argument.value.text, TriggerDirectoryEditor::TriggerSymbol(target->name)});
                continue;
            }
            const auto index = ecas.back();
            ecas.pop_back();
            if (index >= document.ecaNodes.size() || !seenEcas.insert(index).second) continue;
            const auto& eca = document.ecaNodes[index];
            ecas.insert(ecas.end(), eca.children.rbegin(), eca.children.rend());
            arguments.insert(arguments.end(), eca.arguments.rbegin(), eca.arguments.rend());
        }
    }
    return result;
}

std::vector<GraphEditOperation> LegacyTriggerReferenceFixes(std::span<const LegacyTriggerReference> references) {
    std::vector<GraphEditOperation> operations;
    operations.reserve(references.size());
    for (const auto& reference : references) {
        operations.emplace_back(GraphSet{{GraphRecordKind::Argument, reference.argumentId, {}}, GraphField::ArgumentScalar,
            GraphScalarValue{1, reference.value}, GraphScalarValue{1, reference.symbol}});
    }
    return operations;
}

} // namespace War3Map::Trigger
