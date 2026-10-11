#include "TriggerCodeCommands.h"

#include "EditorProject.h"

#include <QUndoStack>

namespace War3Map::QtProject {

TriggerCodeEditCommand::TriggerCodeEditCommand(EditorProject& project,
    Trigger::TriggerCodeEditReceipt receipt, QString label)
    : QUndoCommand(std::move(label)), project_(project), receipt_(std::move(receipt)) {}

void TriggerCodeEditCommand::redo() {
    if (firstRedo_) { firstRedo_ = false; return; }
    Apply(true);
}

void TriggerCodeEditCommand::PublishSnapshot() noexcept {
    project_.guiConversionSnapshots_.Swap(snapshot_);
}

void TriggerCodeEditCommand::Apply(bool forward) {
    if (!project_.triggerDocument_ || !project_.uiCatalog_) return;
    // 离开的状态即上次观察到的状态；撤销栈后进先出，到达的状态与此前观察到的同一位置内容相同。
    const auto leaving = project_.ObservedTriggerFingerprint();
    const auto status = Trigger::TriggerCodeEditor::Replay(
        *project_.triggerDocument_, *project_.uiCatalog_, receipt_, forward);
    if (!status.Succeeded()) { project_.AddDiagnostics(status.diagnostics); return; }
    PublishSnapshot();
    if (leaving) (forward ? beforeFingerprint_ : afterFingerprint_) = *leaving;
    const auto& arriving = forward ? afterFingerprint_ : beforeFingerprint_;
    // QUndoStack 在命令返回后才移动位置并发出 indexChanged。
    if (project_.triggerUndoStack_ && !arriving.empty())
        project_.OfferKnownTriggerFingerprint(project_.triggerUndoStack_->index() + (forward ? 1 : -1), arriving);
}

} // namespace War3Map::QtProject
