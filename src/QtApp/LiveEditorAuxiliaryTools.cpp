#include "LiveEditorAuxiliaryTools.h"

#include "EditorProject.h"
#include "McpAuxiliaryTools.h"
#include "McpToolInternal.h"
#include "ProjectAuxiliaryEdits.h"

namespace War3Map::QtApp {

QJsonObject InvokeLiveAuxiliaryTool(QtProject::EditorProject& project,
    const QString& name, const QJsonObject& arguments,
    const std::function<QJsonObject()>& identity,
    const std::function<Result<Mcp::MapDataToolContext>()>& context) {
    const auto binding = QtProject::CaptureAuxiliaryEditBinding(project);
    if (!binding.Succeeded()) return Mcp::Failure(binding);
    Mcp::AuxiliaryToolHost host;
    host.identity = identity;
    host.context = context;
    host.references = [&project] { return QtProject::CaptureAuxiliaryReferences(project); };
    host.apply = [&project, expected = *binding.value](const Services::PreparedAuxiliaryEdit& plan,
        const Mcp::MapDataPrepublishValidator& validate) -> Result<MapData::MapDataEditSession::EditReceipt> {
        const auto result = QtProject::ApplyAuxiliaryEditWithResult(project, expected, plan.Request(),
            QObject::tr("AI 编辑区域或镜头"), [&](const auto& before, const auto& after, const auto& receipt) {
                // Qt 适配重新 Prepare 后仍须是 facade 的完整计划，不能只比待写成员。
                if (!Services::AuxiliaryEditing::AreEquivalentMapSources(before, plan.Before())
                    || !Services::AuxiliaryEditing::AreEquivalentMapSources(after, plan.After()))
                    return Status::Failure("QtApp.Live", "AUX_PLAN_STALE", "活动工程的区域/镜头计划已变化。");
                return validate ? validate(before, after, receipt, {}) : Status::Ok();
            });
        if (!result.Succeeded()) return {{}, result.diagnostics};
        return Result<MapData::MapDataEditSession::EditReceipt>::Success(result.value->receipt);
    };
    return Mcp::InvokeAuxiliaryTool(name, arguments, host);
}

} // namespace War3Map::QtApp
