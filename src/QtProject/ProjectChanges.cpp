#include "EditorProject.h"
#include "Commands/MapDataCommands.h"
#include "Commands/ObjectSnapshotCommand.h"
#include "Commands/TriggerCommands.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/Trigger/TriggerCodec.h"
#include <QPointer>
#include <QUndoStack>

namespace War3Map::QtProject {

Status EditorProject::RevertProjectChange(const Services::ProjectChangeRow& row,
    const Services::MapDataQueryBinding& currentBinding) {
    if (row.domain != Services::ProjectChangeDomain::MapData) {
        if (savingMap_ || row.binding != currentBinding || row.editorRevision != editRevision_.Revision()
            || (mapEdits_ && mapEdits_->Revision() != currentBinding.revision))
            return Status::Failure("ProjectChanges", "CHANGE_CONTEXT_STALE", "工程或内容修订已变化，请刷新修改列表");
        const auto context = CaptureContentChangesContext(currentBinding);
        if (!context.Succeeded()) return {context.diagnostics};
        Services::ProjectChangesOptions options; options.maxRecordsPerMember = 32768;
        const auto planned = Services::BuildProjectContentReversePlan(*context.value, row, options);
        if (!planned.Succeeded()) return {planned.diagnostics};
        const auto& plan = *planned.value;
        if (plan.objectDocument && plan.objectType) {
            const auto index = static_cast<std::size_t>(*plan.objectType);
            auto* target = objectDocuments_[index].get(); auto* stack = objectUndoStacks_[index];
            if (!target || !stack) return Status::Failure("ProjectChanges", "OBJECT_UNDO_UNAVAILABLE", "物编文档或原有撤销栈未就绪");
            const QPointer<EditorProject> guarded(this);
            stack->push(new ObjectSnapshotCommand(*target, *plan.objectDocument,
                [guarded, target, index](const auto& snapshot) {
                    if (!guarded || guarded->objectDocuments_[index].get() != target) return;
                    *target = snapshot; guarded->ObserveObjectEdit(static_cast<int>(index));
                    emit guarded->ObjectDocumentsReplaced();
                }, tr("撤销物编字段修改")));
        } else if (!plan.arguments.empty() && triggerDocument_ && triggerUndoStack_) {
            if (!uiCatalog_) return Status::Failure("ProjectChanges", "TRIGGER_CATALOG_UNAVAILABLE", "触发器定义未就绪");
            std::vector<TriggerArgumentChange> edits; edits.reserve(plan.arguments.size());
            for (const auto& edit : plan.arguments) {
                auto* argument = Trigger::TriggerDocumentEditor::FindArgument(*triggerDocument_, edit.argumentId);
                if (!argument) return Status::Failure("ProjectChanges", "TRIGGER_ARGUMENT_STALE", "触发器参数已变化");
                edits.push_back({edit.argumentId, edit.before, edit.after});
            }
            auto candidate = *triggerDocument_;
            for (const auto& edit : edits)
                *Trigger::TriggerDocumentEditor::FindArgument(candidate, edit.argumentId) = edit.after;
            const auto valid = Trigger::WtgWctCodec{}.Validate(candidate, *uiCatalog_);
            if (!valid.Succeeded()) return valid;
            // 与参数编辑器一致：先应用，再交给 firstRedo 不重复应用的既有命令。
            for (const auto& edit : edits)
                *Trigger::TriggerDocumentEditor::FindArgument(*triggerDocument_, edit.argumentId) = edit.after;
            triggerUndoStack_->push(new EditTriggerArgumentsUndoCommand(triggerDocument_.get(), std::move(edits), tr("撤销触发器参数修改")));
            ObserveTriggerEdit();
        } else return Status::Failure("ProjectChanges", "CHANGE_PLAN_EMPTY", "没有可执行的内容逆操作");
        return Status::Ok();
    }
    if (savingMap_ || !mapDocument_ || !mapEdits_ || row.binding != currentBinding
        || mapEdits_->Revision() != currentBinding.revision)
        return Status::Failure("ProjectChanges", "CHANGE_CONTEXT_STALE", "工程或保存状态已变化，请刷新修改列表");
    const auto current = mapEdits_->Snapshot();
    const Services::ProjectChangesContext context{mapDocument_.get(), &current, currentBinding};
    const auto planned = Services::BuildProjectChangeReversePlan(context, row);
    if (!planned.Succeeded()) { Status failed; failed.diagnostics = planned.diagnostics; return failed; }
    const auto& plan = *planned.value;
    Status applied;
    if (plan.fieldTables) {
        const auto encoded = MapData::SerializeMapFieldTable(*plan.fieldTables, plan.member);
        if (!encoded.Succeeded()) { applied.diagnostics = encoded.diagnostics; return applied; }
        applied = mapEdits_->EditSerialized(plan.member, *encoded.value);
    } else if (plan.units || plan.doodads) {
        applied = mapEdits_->EditPlacements(plan.units ? &*plan.units : nullptr, plan.doodads ? &*plan.doodads : nullptr);
    } else return Status::Failure("ProjectChanges", "CHANGE_PLAN_EMPTY", "所选项目没有可执行的反向编辑");
    if (!applied.Succeeded()) return applied;
    PushMapDataEdit(*this, tr("撤销这项修改"), "从修改面板反向编辑");
    (void)ObserveEditState("从修改面板反向编辑");
    emit MapDocumentChanged();
    return applied;
}

} // namespace War3Map::QtProject
