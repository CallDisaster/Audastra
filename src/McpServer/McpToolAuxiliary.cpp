#include "McpAuxiliaryTools.h"
#include "McpToolInternal.h"

namespace War3Map::Mcp {

void RegisterAuxiliaryTools(McpServer& server) {
    for (auto definition : AuxiliaryToolDefinitions()) {
        const auto name = definition.name;
        definition.handler = [&server, name](const QJsonObject& arguments) -> QJsonValue {
            QJsonObject error;
            auto* workspace = RequireWorkspace(server, error);
            if (!workspace) return error;
            AuxiliaryToolHost host;
            host.identity = [&server, workspace] {
                QJsonObject identity{{"project_identity", QString::fromStdString(server.DocumentId())},
                    {"revision", QString::fromStdString(workspace->ObjectRevision())},
                    {"transaction_active", workspace->HasActiveTransaction()}, {"transaction_revision", RevisionJson(workspace->Revision())}};
                if (workspace->HasActiveTransaction()) identity.insert("transaction_id", QString::number(workspace->ActiveTransactionId()));
                return identity;
            };
            host.context = [&server, workspace]() -> Result<MapDataToolContext> {
                const auto source = workspace->MapDataContext(server.DocumentId(), server.DocumentGeneration());
                if (!source.Succeeded()) return {{}, source.diagnostics};
                MapDataToolContext context;
                context.original = source.value->original; context.document = source.value->document; context.binding = source.value->binding;
                context.includesActiveTransactionOverlay = source.value->includesActiveTransactionOverlay;
                return Result<MapDataToolContext>::Success(std::move(context));
            };
            host.references = [workspace] { return workspace->AuxiliaryReferences(); };
            host.apply = [workspace](const Services::PreparedAuxiliaryEdit& plan, const MapDataPrepublishValidator& validate) {
                return workspace->ApplyAuxiliaryPlan(plan, [&](const auto& before, const auto& after, const auto& receipt) {
                    return validate ? validate(before, after, receipt, {}) : Status::Ok();
                });
            };
            return InvokeAuxiliaryTool(name, arguments, host);
        };
        server.RegisterTool(std::move(definition));
    }
}

} // namespace War3Map::Mcp
