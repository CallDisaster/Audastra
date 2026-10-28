#pragma once
#include "War3MapCore/Trigger/TriggerCodeEditing.h"
#include <functional>

namespace War3Map::Services { class TriggerCodeIdentityEditing; }
namespace War3Map::Trigger {
// 服务拥有的精确候选，不接受协议中的自报审计、任意WCT或修改后的plan。
class PreparedTriggerCodeIdentityChange final {
public:
    [[nodiscard]] Status ApplyTo(TriggerDocument& document, const UiCatalog& catalog) const;
private:
    PreparedTriggerCodeIdentityChange() = default;
    TriggerGraphReplayIdentity before_;
    std::shared_ptr<const TriggerDocument> after_;
    std::function<Status()> verify_;
    friend class War3Map::Services::TriggerCodeIdentityEditing;
};
}
