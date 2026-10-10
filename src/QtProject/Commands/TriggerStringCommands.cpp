#include "TriggerStringCommands.h"

#include "EditorProject.h"

namespace War3Map::QtProject {

TriggerStringEditCommand::TriggerStringEditCommand(EditorProject& project,
    Services::TriggerStringEditReceipt receipt, QString text)
    : QUndoCommand(std::move(text)), project_(project), receipt_(std::move(receipt)) {}

void TriggerStringEditCommand::redo() {
    if (firstRedo_) { firstRedo_ = false; return; }
    Apply(true);
}

void TriggerStringEditCommand::Apply(bool forward) {
    if (!project_.triggerDocument_ || !project_.mapEdits_) return;
    const auto status = Services::ReplayTriggerStringEdit(*project_.triggerDocument_, *project_.mapEdits_, receipt_, forward);
    if (!status.Succeeded()) { project_.AddDiagnostics(status.diagnostics); return; }
    (void)project_.ObserveMapDataEdit("编辑触发器共享字符串");
    emit project_.MapDocumentChanged();
}

} // namespace War3Map::QtProject
