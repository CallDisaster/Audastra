#pragma once
#include "War3MapCore/Services/TriggerCodeIdentityEditing.h"
#include <QString>
namespace War3Map::QtProject {
class EditorProject;
[[nodiscard]] Result<Services::TriggerCodeIdentityContext> CaptureProjectTriggerCodeIdentityContext(EditorProject& project);
[[nodiscard]] Result<Trigger::TriggerGraphEditReceipt> ApplyProjectTriggerCodeIdentity(EditorProject& project,
    const Services::PreparedTriggerCodeIdentityEdit& prepared, const QString& undoLabel);
[[nodiscard]] Result<Trigger::TriggerGraphEditReceipt> ApplyProjectTriggerCodeIdentity(EditorProject& project,
    const Services::PreparedTriggerCodeIdentityEdit& prepared, const QString& undoLabel,
    std::vector<Trigger::GraphEditOperation> suffix);
}
