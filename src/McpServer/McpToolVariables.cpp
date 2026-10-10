#include "McpVariableTools.h"
#include "McpToolInternal.h"
#include "McpTriggerGraphJson.h"

namespace War3Map::Mcp {

void RegisterVariableTools(McpServer& server) {
    for (auto definition : VariableToolDefinitions()) {
        const auto name = definition.name;
        definition.handler = [&server, name](const QJsonObject& arguments) -> QJsonValue {
            QJsonObject error;
            auto* workspace = RequireWorkspace(server, error);
            if (!workspace) return error;
            VariableToolHost host;
            host.identity = [&server, workspace] {
                QJsonObject identity{{"project_identity", QString::fromStdString(server.DocumentId())},
                    {"revision", QString::fromStdString(workspace->ObjectRevision())},
                    {"transaction_active", workspace->HasActiveTransaction()},
                    {"transaction_revision", RevisionJson(workspace->Revision())}};
                if (workspace->HasActiveTransaction())
                    identity.insert("transaction_id", QString::number(workspace->ActiveTransactionId()));
                return identity;
            };
            host.context = [workspace, readNames = name == "variables_list" || name == "variables_get"]() -> Result<VariableToolContext> {
                const auto environment = workspace->EnvironmentStatus();
                if (!environment.Succeeded()) {
                    Result<VariableToolContext> failure; failure.diagnostics = environment.diagnostics; return failure;
                }
                VariableToolContext context;
                context.document = &workspace->VisibleDocument();
                context.catalog = &workspace->Catalog();
                const auto references = workspace->MapObjectReferences();
                if (references.Succeeded() && references.value) context.mapObjects = *references.value;
                if (readNames) context.nameResolver = CaptureWorkspaceNameResolver(*workspace, *context.document, context.mapObjects.get());
                return Result<VariableToolContext>::Success(std::move(context));
            };
            host.apply = [workspace](const Trigger::TriggerGraphEditBatch& batch,
                const VariablePrepublishValidator& validate) {
                GraphEditPublishValidator guard;
                if (validate) guard = [&validate](const Trigger::TriggerDocument& before,
                    const Trigger::TriggerDocument& after, const Trigger::TriggerGraphEditReceipt& receipt) {
                    return validate(before, after, receipt, {});
                };
                return workspace->ApplyGraphEdit(workspace->ActiveTransactionId(), workspace->Revision(), batch, guard);
            };
            return InvokeVariableTool(name, arguments, host);
        };
        server.RegisterTool(std::move(definition));
    }
}

} // namespace War3Map::Mcp
