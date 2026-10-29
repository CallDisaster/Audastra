#include "War3MapCore/Trigger/TriggerValueSafety.h"
#include "War3MapCore/Trigger/TriggerReal.h"
#include "War3MapCore/Trigger/TriggerStorage.h"
#include <algorithm>
#include <set>

namespace War3Map::Trigger {
namespace {
std::string StorageType(const TriggerTypeSystem& types, std::string expected) {
    if (StorageFor(expected).known) return {};
    if (const auto* type = types.Find(expected); type && !type->baseType.empty() && StorageFor(type->baseType).known) return {};
    return expected;
}
}
std::optional<bool> TriggerStorageCapabilities::Supports(std::string_view catalogSha256, std::string_view type) const {
    if (catalogSha256_ != catalogSha256 || binding_.empty()) return {};
    const auto found = supported_.find(type);
    return found == supported_.end() ? std::nullopt : std::optional(found->second);
}
Status TriggerStorageCapabilities::Validate() const {
    if (!verifySources_) return Status::Failure("Trigger.Value", "TRIGGER_VALUE_CHECK_CONTEXT_INCOMPLETE", "存取能力快照没有来源核验。");
    return verifySources_();
}
std::vector<std::string> RequiredTriggerStorageTypes(const TriggerDocument& document, const UiCatalog& catalog) {
    const TriggerTypeSystem types(catalog);
    std::set<std::string> required;
    for (const auto& eca : document.ecaNodes) for (std::size_t i = 0; i < eca.arguments.size(); ++i) {
        if (eca.arguments[i] >= document.argumentNodes.size()) continue;
        const auto& argument = document.argumentNodes[eca.arguments[i]];
        if (argument.type != 2 || !argument.callEca || *argument.callEca >= document.ecaNodes.size()) continue;
        const auto& call = document.ecaNodes[*argument.callEca];
        if (call.name.text != kLocalGet && call.name.text != kLocalGetArray) continue;
        const auto storage = StorageType(types, types.ExpectedType(document, eca, i));
        if (!storage.empty()) required.insert(storage);
    }
    return {required.begin(), required.end()};
}
Result<TriggerValueSafetyReport> AnalyzeTriggerValues(const TriggerDocument& document, const UiCatalog& catalog,
    std::span<const StableId> affectedArguments, const TriggerStorageCapabilities* capabilities) {
    using Output = Result<TriggerValueSafetyReport>;
    if (capabilities) if (const auto valid = capabilities->Validate(); !valid.Succeeded()) return {std::nullopt, valid.diagnostics};
    if (document.argumentNodes.size() > 524288 || document.ecaNodes.size() > 524288)
        return Output::Failure("Trigger.Value", "TRIGGER_VALUE_CHECK_BUDGET", "参数安全检查超出图规模预算。");
    const TriggerTypeSystem types(catalog);
    const std::set<StableId> scope(affectedArguments.begin(), affectedArguments.end());
    std::vector<char> visited(document.ecaNodes.size());
    TriggerValueSafetyReport report;
    const auto visit = [&](const auto& self, std::uint32_t index, bool enabled, std::size_t depth) -> Status {
        if (index >= document.ecaNodes.size() || depth > 256 || visited[index])
            return Status::Failure("Trigger.Value", "TRIGGER_VALUE_CHECK_STRUCTURE", "参数安全检查遇到越界、重复挂载或过深的图。");
        visited[index] = 1;
        const auto& eca = document.ecaNodes[index]; enabled = enabled && eca.enabled != 0;
        for (std::size_t i = 0; i < eca.arguments.size(); ++i) {
            if (eca.arguments[i] >= document.argumentNodes.size())
                return Status::Failure("Trigger.Value", "TRIGGER_VALUE_CHECK_STRUCTURE", "参数安全检查遇到越界参数。");
            const auto& argument = document.argumentNodes[eca.arguments[i]];
            if (scope.empty() || scope.contains(argument.id)) {
                const auto expected = types.ExpectedType(document, eca, i);
                std::optional<Diagnostic> issue;
                std::string requiredStorage;
                if (argument.type == 3 && types.LiteralKind(expected) == TriggerTypeSystem::Literal::Real)
                    issue = CheckTriggerRealLiteral(argument.value.text);
                if (argument.type == 2 && argument.callEca && *argument.callEca < document.ecaNodes.size()) {
                    const auto& call = document.ecaNodes[*argument.callEca];
                    if (call.name.text == kLocalGet || call.name.text == kLocalGetArray) {
                        const auto storage = StorageType(types, expected);
                        requiredStorage = storage;
                        const auto supported = capabilities ? capabilities->Supports(catalog.Sha256(), storage) : std::nullopt;
                        if (!storage.empty() && (!supported || !*supported)) issue = Diagnostic{enabled ? DiagnosticSeverity::Error : DiagnosticSeverity::Warning,
                            "Trigger.Value", supported ? "TRIGGER_LOCAL_HANDLE_STORAGE_UNSUPPORTED" : "TRIGGER_VALUE_CHECK_CONTEXT_INCOMPLETE",
                            "泛型局部读取将按父槽“" + expected + "”生成存取宏；" + (supported ? std::string("当前工具链没有此类型的存取宏。")
                            : std::string("缺少绑定当前目录和工具链的扩展存取能力快照，不能猜测支持。"))
                            + "请先读入unit等具体类型变量，再传给handle参数。"};
                    }
                }
                if (issue) {
                    issue->message += "（trigger_id=" + std::to_string(eca.sourceTriggerId) + "，eca_id="
                        + std::to_string(eca.id) + "，argument_id=" + std::to_string(argument.id) + "，参数=" + std::to_string(i + 1) + "）";
                    report.issues.push_back({std::move(*issue), eca.sourceTriggerId, eca.id, argument.id, i, enabled, requiredStorage});
                    if (report.issues.size() > 4096) return Status::Failure("Trigger.Value", "TRIGGER_VALUE_CHECK_BUDGET", "参数安全诊断超过4096项，未截断放行。");
                }
            }
            if (argument.callEca) if (const auto status = self(self, *argument.callEca, enabled, depth + 1); !status.Succeeded()) return status;
            auto indexArgument = argument.indexArgument;
            for (std::size_t indexDepth = depth; indexArgument; ++indexDepth) {
                if (*indexArgument >= document.argumentNodes.size() || indexDepth > 256)
                    return Status::Failure("Trigger.Value", "TRIGGER_VALUE_CHECK_STRUCTURE", "数组下标图越界或过深。");
                const auto& nested = document.argumentNodes[*indexArgument];
                if (nested.callEca) if (const auto status = self(self, *nested.callEca, enabled, indexDepth + 1); !status.Succeeded()) return status;
                indexArgument = nested.indexArgument;
            }
        }
        for (const auto child : eca.children) if (const auto status = self(self, child, enabled, depth + 1); !status.Succeeded()) return status;
        return Status::Ok();
    };
    for (const auto& trigger : document.triggers) for (const auto root : trigger.ecas)
        if (const auto status = visit(visit, root, trigger.enabled != 0, 0); !status.Succeeded()) return {std::nullopt, status.diagnostics};
    return Output::Success(std::move(report));
}
}
