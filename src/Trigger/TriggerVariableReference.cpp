#include "War3MapCore/Trigger/TriggerVariableReference.h"

#include "War3MapCore/Trigger/TriggerDirectory.h"

namespace War3Map::Trigger {

Result<VariableReference> ResolveVariableReference(const TriggerDocument& document,
    std::string_view name, const MapObjectReferenceSnapshot* mapObjects) {
    using Output = Result<VariableReference>;
    std::optional<VariableReference> selected;
    const auto ambiguous = [&] {
        return Output::Failure("Trigger.GraphEdit", "TRIGGER_GRAPH_VARIABLE_AMBIGUOUS",
            "变量原名、udg_别名、触发器生成名或预放对象命中多个真实来源：" + std::string(name));
    };
    for (const auto& variable : document.variables) {
        if (variable.name.text != name && (!name.starts_with("udg_") || variable.name.text != name.substr(4))) continue;
        if (selected) return ambiguous();
        if (variable.typeName.text.empty() || (variable.isArray != 0 && variable.isArray != 1))
            return Output::Failure("Trigger.GraphEdit", "TRIGGER_GRAPH_VARIABLE_INVALID",
                "变量声明的类型或数组标志无效：" + std::string(name));
        selected = VariableReference{VariableReferenceKind::Declared, variable.typeName.text,
            variable.isArray != 0, true, {}};
    }
    for (const auto& trigger : document.triggers) {
        // 与生成器一致：注释与禁用的T不声明生成全局；初始关闭仍是实际生成的trigger。
        if (trigger.type != 0 || trigger.enabled == 0
            || name != "gg_trg_" + TriggerDirectoryEditor::ScriptName(trigger.name.text)) continue;
        if (selected) return ambiguous();
        selected = VariableReference{VariableReferenceKind::GeneratedTrigger, "trigger", false, false, {}};
    }
    // 密封快照的精确查找是唯一预放资格入口；udg_前缀不转发给外部对象查找。
    if (mapObjects != nullptr) {
        if (const auto* object = mapObjects->Find(name)) {
            if (selected) return ambiguous();
            selected = VariableReference{VariableReferenceKind::PreplacedObject, object->type,
                false, false, mapObjects->Identity()};
        }
    }
    if (!selected) return Output::Failure("Trigger.GraphEdit", "TRIGGER_GRAPH_VARIABLE_UNKNOWN",
        "参数变量未有真实声明/类型资格：" + std::string(name));
    return Output::Success(std::move(*selected));
}

} // namespace War3Map::Trigger
