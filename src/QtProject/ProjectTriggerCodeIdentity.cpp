#include "ProjectTriggerCodeIdentity.h"
#include "EditorProject.h"
#include "ProjectAuxiliaryEdits.h"
#include "ProjectObjectCatalog.h"
#include "Commands/TriggerCommands.h"
#include "War3MapCore/Services/MapObjectReferenceCapture.h"
#include <QPointer>

namespace War3Map::QtProject {
Result<Services::TriggerCodeIdentityContext> CaptureProjectTriggerCodeIdentityContext(EditorProject& project) {
    using R = Result<Services::TriggerCodeIdentityContext>;
    if (!project.triggerDocument_ || !project.uiCatalog_) return R::Failure("QtProject.Trigger", "TRIGGER_CODE_SOURCE_STALE", "工程触发器或函数目录尚不可用。");
    auto objects = project.CaptureTriggerMapObjectReferences(); if (!objects.Succeeded()) return {{}, objects.diagnostics};
    Services::TriggerCodeIdentityContext context; context.document = project.triggerDocument_.get(); context.catalog = project.uiCatalog_.get();
    context.mapObjects = (*objects.value)->References().empty() ? nullptr : objects.value->get();
    const auto* settings = project.mapEdits_ && project.mapEdits_->Snapshot().Settings()
        ? &*project.mapEdits_->Snapshot().Settings() : nullptr;
    if (project.luaMapTarget_) context.target = Services::TriggerCodeScriptTarget::Lua;
    else if (settings) context.target = settings->scriptLanguage == 0 ? Services::TriggerCodeScriptTarget::Jass : Services::TriggerCodeScriptTarget::Lua;
    auto references = CaptureAuxiliaryReferences(project);
    context.completeScriptSources = references.completeTriggerSource; context.opaqueScriptSources = std::move(references.opaqueScriptSources);
    const auto generation = project.editRevision_.Generation(), revision = project.editRevision_.Revision();
    const auto targetGeneration = project.luaMapTargetGeneration_; QPointer<EditorProject> guard(&project);
    context.verify = [guard, generation, revision, targetGeneration, owner = *objects.value] {
        if (!guard || guard->editRevision_.Generation() != generation || guard->editRevision_.Revision() != revision
            || guard->luaMapTargetGeneration_ != targetGeneration || guard->sourceChangeSuspected_ || guard->sourceChangeConfirmed_)
            return Status::Failure("QtProject.Trigger", "TRIGGER_CODE_SOURCE_STALE", "工程来源、修订或实际脚本目标已变化。");
        const std::array<std::string, 2> names{"Units/UnitData.slk", "Units/ItemData.slk"};
        auto sources = CaptureProjectObjectCatalogSources(*guard, names);
        if (!sources.Succeeded()) return Status{sources.diagnostics};
        return Services::VerifyMapObjectReferenceClassificationSources(*owner, sources.value->Sources());
    };
    return R::Success(std::move(context));
}
Result<Trigger::TriggerGraphEditReceipt> ApplyProjectTriggerCodeIdentity(EditorProject& project,
    const Services::PreparedTriggerCodeIdentityEdit& prepared, const QString& undoLabel) {
    return ApplyProjectTriggerCodeIdentity(project, prepared, undoLabel, {});
}
Result<Trigger::TriggerGraphEditReceipt> ApplyProjectTriggerCodeIdentity(EditorProject& project,
    const Services::PreparedTriggerCodeIdentityEdit& prepared, const QString& undoLabel,
    std::vector<Trigger::GraphEditOperation> suffix) {
    auto context = CaptureProjectTriggerCodeIdentityContext(project); if (!context.Succeeded()) return {{}, context.diagnostics};
    const auto checked = Services::TriggerCodeIdentityEditing::VerifyContext(*context.value);
    if (!checked.Succeeded()) return {{}, checked.diagnostics};
    std::vector<Trigger::GraphEditOperation> operations{prepared.Operation()};
    operations.insert(operations.end(), std::make_move_iterator(suffix.begin()), std::make_move_iterator(suffix.end()));
    return PublishTriggerGraphOperations(project, std::move(operations), undoLabel, [context = *context.value] {
        return Services::TriggerCodeIdentityEditing::VerifyContext(context);
    });
}
}
