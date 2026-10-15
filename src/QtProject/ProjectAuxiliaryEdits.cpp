#include "ProjectAuxiliaryEdits.h"
#include "EditorProject.h"
#include "Commands/MapDataCommands.h"
#include "War3MapCore/Text/TextUtil.h"
#include "War3MapCore/Trigger/TriggerDocument.h"
#include "War3MapCore/Services/AuxiliaryTransaction.h"

#include <algorithm>

namespace War3Map::QtProject {

Services::AuxiliaryReferenceContext CaptureAuxiliaryReferences(EditorProject& project) {
    Services::AuxiliaryReferenceContext result;
    result.triggers = project.triggerDocument_.get();
    result.completeTriggerSource = result.triggers != nullptr;
    const auto inspect = [&](std::string name) {
        Services::ObserveAuxiliaryScriptSource(result, std::move(name));
    };
    if (project.archiveSession_) {
        for (const auto& member : project.archiveSession_->Members()) inspect(member.name);
        if (project.archiveSession_->HasOpaqueMembers()) result.opaqueScriptSources.push_back("未命名归档成员");
    } else if (project.directorySnapshot_) {
        for (const auto& file : project.directorySnapshot_->Snapshot().files) inspect(file.relativePath.generic_string());
    } else if (project.readOnlyArchive_) result.opaqueScriptSources.push_back("只读归档未建立完整成员清单");
    for (const auto& pending : project.pendingImports_) inspect(pending.archivePath.toStdString());
    return result;
}

Result<AuxiliaryEditBinding> CaptureAuxiliaryEditBinding(const EditorProject& project) {
    if (!project.mapEdits_ || !project.mapDataUndoStack_ || project.savingMap_
        || project.sourceChangeSuspected_ || project.sourceChangeConfirmed_)
        return Result<AuxiliaryEditBinding>::Failure("MapData.Auxiliary", "AUX_EDIT_UNAVAILABLE",
            "当前地图数据或撤销栈不可用、正在保存，或来源文件已变化待核对。");
    return Result<AuxiliaryEditBinding>::Success({project.projectRoot_, project.sourceArchivePath_,
        project.mapEdits_->SessionIdentity(), project.editRevision_.Generation(), project.mapEdits_->Revision()});
}

bool AuxiliaryEditBindingCurrent(const EditorProject& project, const AuxiliaryEditBinding& expected) {
    return project.mapEdits_ && project.mapDataUndoStack_ && !project.savingMap_
        && !project.sourceChangeSuspected_ && !project.sourceChangeConfirmed_
        && project.projectRoot_ == expected.projectRoot && project.sourceArchivePath_ == expected.sourceArchive
        && project.mapEdits_->SessionIdentity() == expected.sessionIdentity
        && project.editRevision_.Generation() == expected.generation && project.mapEdits_->Revision() == expected.revision;
}

Result<AuxiliaryEditResult> ApplyAuxiliaryEditWithResult(EditorProject& project,
    const AuxiliaryEditBinding& expected, Services::AuxiliaryEditRequest request, const QString& label,
    const MapData::MapDataPublishValidator& validator) {
    if (!AuxiliaryEditBindingCurrent(project, expected))
        return Result<AuxiliaryEditResult>::Failure("MapData.Auxiliary", "AUX_PROJECT_STALE",
            "地图来源、编辑会话或修订已变化，或正在保存；请重新打开属性后编辑。");
    auto current = std::make_shared<const MapDocument>(project.mapEdits_->Snapshot());
    const auto plan = Services::AuxiliaryEditing::Prepare(std::move(current), std::move(request), CaptureAuxiliaryReferences(project));
    if (!plan.Succeeded()) return {std::nullopt, plan.diagnostics};
    AuxiliaryEditResult result;
    result.binding = expected; result.changed = plan.value->HasChanges();
    // 所有可能失败的结果身份派生都在发布前完成；不能提交后才因行键/响应失败报未修改。
    const auto member = Services::AuxiliaryEditing::MemberIdentity(plan.value->After(), plan.value->Kind());
    if (!member.Succeeded()) return {std::nullopt, member.diagnostics};
    result.memberSha256 = *member.value;
    const auto& appliedRequest = plan.value->Request();
    if (appliedRequest.action != Services::AuxiliaryEditAction::Delete) {
        const auto size = appliedRequest.kind == MapDataKind::Regions
            ? plan.value->After().Regions()->entries.size() : plan.value->After().Cameras()->entries.size();
        const auto index = appliedRequest.action == Services::AuxiliaryEditAction::Create ? size - 1 : appliedRequest.sourceIndex;
        const auto key = MapData::MapFieldRowKey(plan.value->After(), appliedRequest.kind, index);
        if (!key.Succeeded()) return {std::nullopt, key.diagnostics};
        result.rowKey = *key.value;
        if (appliedRequest.kind == MapDataKind::Regions)
            result.object = MapData::MakeRegionId(static_cast<std::uint32_t>(index), plan.value->After().Regions()->entries[index]);
        else result.object = MapData::MakeCameraId(static_cast<std::uint32_t>(index), plan.value->After().Cameras()->entries[index]);
    }
    if (!result.changed) return Result<AuxiliaryEditResult>::Success(std::move(result));
    const auto applied = Services::AuxiliaryEditing::Apply(*project.mapEdits_, *plan.value,
        CaptureAuxiliaryReferences(project), [&](const auto& before, const auto& after, const auto& receipt) {
            if (!AuxiliaryEditBindingCurrent(project, expected))
                return Status::Failure("MapData.Auxiliary", "AUX_PROJECT_STALE", "区域/镜头发布前工程身份已变化。");
            if (validator) {
                const auto checked = validator(before, after, receipt);
                if (!checked.Succeeded()) return checked;
            }
            if (!AuxiliaryEditBindingCurrent(project, expected))
                return Status::Failure("MapData.Auxiliary", "AUX_PROJECT_STALE", "区域/镜头回执预检期间工程身份已变化。");
            result.receipt = receipt;
            return Status::Ok();
        });
    if (!applied.Succeeded()) return {std::nullopt, applied.diagnostics};
    result.binding.revision = project.mapEdits_->Revision();
    PushMapDataEdit(project, label, "编辑区域或镜头");
    (void)project.ObserveMapDataEdit("编辑区域或镜头");
    emit project.MapDocumentChanged();
    return Result<AuxiliaryEditResult>::Success(std::move(result));
}

Status ApplyAuxiliaryEdit(EditorProject& project, Services::AuxiliaryEditRequest request, const QString& label) {
    const auto binding = CaptureAuxiliaryEditBinding(project);
    if (!binding.Succeeded()) return {binding.diagnostics};
    const auto result = ApplyAuxiliaryEditWithResult(project, *binding.value, std::move(request), label);
    return {result.diagnostics};
}
} // namespace War3Map::QtProject
