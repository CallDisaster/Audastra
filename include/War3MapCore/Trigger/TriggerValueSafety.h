#pragma once
#include "War3MapCore/Trigger/TriggerTypes.h"
#include <span>
#include <map>
#include <memory>
#include <functional>

namespace War3Map::Script { struct ScriptFrameworkInputs; }
namespace War3Map::Trigger { class TriggerStorageCapabilities; }
namespace War3Map::Script {
[[nodiscard]] Result<std::shared_ptr<const Trigger::TriggerStorageCapabilities>> CaptureTriggerStorageCapabilities(
    const ScriptFrameworkInputs& inputs, std::span<const std::string> storageTypes);
}

namespace War3Map::Trigger {
struct TriggerValueIssue {
    Diagnostic diagnostic;
    StableId triggerId = 0, ecaId = 0, argumentId = 0;
    std::size_t position = 0;
    bool enabled = true;
    std::string storageType;
};
// 扩展存取能力必须来自宿主冻结的实际预处理上下文，不能由请求提供放行标记。
class TriggerStorageCapabilities final {
public:
    [[nodiscard]] std::optional<bool> Supports(std::string_view catalogSha256, std::string_view type) const;
    [[nodiscard]] const std::string& Binding() const noexcept { return binding_; }
    [[nodiscard]] Status Validate() const;
private:
    std::string catalogSha256_, binding_;
    std::map<std::string, bool, std::less<>> supported_;
    std::function<Status()> verifySources_;
    TriggerStorageCapabilities() = default;
    friend Result<std::shared_ptr<const TriggerStorageCapabilities>> Script::CaptureTriggerStorageCapabilities(
        const Script::ScriptFrameworkInputs&, std::span<const std::string>);
};
[[nodiscard]] std::vector<std::string> RequiredTriggerStorageTypes(const TriggerDocument& document, const UiCatalog& catalog);
struct TriggerValueSafetyReport { std::vector<TriggerValueIssue> issues; };
[[nodiscard]] Result<TriggerValueSafetyReport> AnalyzeTriggerValues(const TriggerDocument& document,
    const UiCatalog& catalog, std::span<const StableId> affectedArguments = {},
    const TriggerStorageCapabilities* capabilities = nullptr);
}
