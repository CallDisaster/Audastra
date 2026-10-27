#include "ProjectMapCustomSettings.h"
#include "EditorProject.h"
#include "ProjectObjectCatalog.h"
#include "Commands/MapDataCommands.h"
#include "War3MapCore/Services/MapCustomSettingsSources.h"

#include <QPointer>

namespace War3Map::QtProject {

Result<Services::MapCustomSettingsValidation> CaptureProjectMapCustomSettingsValidation(EditorProject& project) {
    using R = Result<Services::MapCustomSettingsValidation>;
    auto names = Services::MapCustomSettingsInputNames();
    if (!names.Succeeded()) return {{}, names.diagnostics};
    auto captured = CaptureProjectObjectCatalogSources(project, *names.value);
    if (!captured.Succeeded()) return {{}, captured.diagnostics};
    auto input = captured.value->Sources();
    for (const auto& replacement : input.replacements) if (ObjectData::ObjectTypeFromNativeFileName(replacement.name))
        return R::Failure("Map.CustomSettings", "SETTINGS_OBJECT_IMPORT_PENDING", "原生物编导入尚未进入当前编辑文档，请先完成导入后重新查询。");
    for (const auto& removal : input.removals) if (ObjectData::ObjectTypeFromNativeFileName(removal))
        return R::Failure("Map.CustomSettings", "SETTINGS_OBJECT_IMPORT_PENDING", "原生物编删除尚未进入当前编辑文档，请先完成保存后重新查询。");
    Services::MapCustomSettingsObjectOverlay overlay;
    for (std::size_t i = 0; i < overlay.size(); ++i) if (project.objectDocuments_[i])
        overlay[i] = std::make_shared<const ObjectData::LniObjectDocument>(*project.objectDocuments_[i]);
    auto validation = Services::CaptureMapCustomSettingsValidation(input, std::move(overlay));
    if (!validation.Succeeded()) return validation;
    const auto generation = project.editRevision_.Generation(), revision = project.editRevision_.Revision();
    const auto gameRoot = project.war3GameRoot_;
    QPointer<EditorProject> guard(&project);
    auto previous = std::move(validation.value->verify);
    validation.value->verify = [guard, generation, revision, gameRoot, previous] {
        if (!guard || guard->editRevision_.Generation() != generation || guard->editRevision_.Revision() != revision || guard->war3GameRoot_ != gameRoot)
            return Status::Failure("Map.CustomSettings", "SETTINGS_PROJECT_STALE", "工程物编或资源修订已变化。");
        return previous ? previous() : Status::Ok();
    };
    return validation;
}

Result<MapData::MapDataEditSession::EditReceipt> ApplyProjectMapCustomSettings(EditorProject& project,
    const Services::PreparedMapCustomSettingsEdit& plan, const Services::MapCustomSettingsValidation& validation,
    const QString& undoLabel, const std::string& description, const MapData::MapDataPublishValidator& validator) {
    using R = Result<MapData::MapDataEditSession::EditReceipt>;
    if (!project.mapEdits_ || !project.mapDataUndoStack_) return R::Failure("Map.CustomSettings", "SETTINGS_UNDO_UNAVAILABLE", "当前地图没有共用撤销会话。");
    const auto applied = Services::MapCustomSettingsEditing::Apply(*project.mapEdits_, plan, validation, validator);
    if (!applied.Succeeded()) return {{}, applied.diagnostics};
    if (!plan.HasChanges()) return R::Success({});
    const auto receipt = project.mapEdits_->LastEdit();
    PushMapDataEdit(project, undoLabel, description);
    (void)project.ObserveMapDataEdit(description);
    emit project.MapDocumentChanged();
    return R::Success(receipt);
}

} // namespace War3Map::QtProject
