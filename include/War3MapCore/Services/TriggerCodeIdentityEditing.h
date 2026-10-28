#pragma once
#include "War3MapCore/Trigger/TriggerGraphEditing.h"
#include "War3MapCore/Trigger/TriggerCodeIdentityChange.h"

namespace War3Map::Services {
enum class TriggerCodeIdentityAction { Rename, Copy };
enum class TriggerCodeScriptTarget { Unknown, Jass, Lua };
struct TriggerCodeIdentityContext {
    const Trigger::TriggerDocument* document = nullptr;
    const Trigger::UiCatalog* catalog = nullptr;
    const Trigger::MapObjectReferenceSnapshot* mapObjects = nullptr;
    TriggerCodeScriptTarget target = TriggerCodeScriptTarget::Unknown;
    bool completeScriptSources = false;
    std::vector<std::string> opaqueScriptSources;
    std::function<Status()> verify;
};
struct TriggerCodeIdentityRequest {
    TriggerCodeIdentityAction action = TriggerCodeIdentityAction::Rename;
    Trigger::TriggerGraphBinding expected;
    StableId triggerId = 0;
    Trigger::RawText expectedName;
    std::string expectedCodeSha256, name;
    std::int32_t categoryId = 0;
    StableId afterTriggerId = 0;
    std::vector<StableId> expectedCategoryOrder;
    StableId categoryRecordId = 0; // 协议使用分类ref.id；原生categoryId仅供可信C++调用方。
};
struct TriggerCodeIdentitySummary {
    StableId triggerId = 0, resultId = 0;
    bool copied = false, changed = false;
    std::string oldName, newName, oldSymbol, newSymbol;
    std::size_t changedBlocks = 0, changedReferences = 0;
    std::vector<std::string> locations;
};
class PreparedTriggerCodeIdentityEdit final {
public:
    [[nodiscard]] const TriggerCodeIdentitySummary& Summary() const noexcept { return summary_; }
    [[nodiscard]] Trigger::GraphCodeIdentityChange Operation() const { return {change_}; }
private:
    std::shared_ptr<const Trigger::PreparedTriggerCodeIdentityChange> change_;
    TriggerCodeIdentitySummary summary_;
    Trigger::TriggerGraphBinding binding_;
    friend class TriggerCodeIdentityEditing;
};
class TriggerCodeIdentityEditing final {
public:
    [[nodiscard]] static Status VerifyContext(const TriggerCodeIdentityContext& context);
    [[nodiscard]] static Result<PreparedTriggerCodeIdentityEdit> Prepare(
        const TriggerCodeIdentityContext& context, const TriggerCodeIdentityRequest& request);
    [[nodiscard]] static Result<Trigger::TriggerGraphEditReceipt> Apply(
        const TriggerCodeIdentityContext& context, Trigger::TriggerDocument& document,
        const PreparedTriggerCodeIdentityEdit& prepared);
};
}
