#include "McpPlacementTools.h"
#include "McpToolInternal.h"

namespace War3Map::Mcp {

void RegisterPlacementTools(McpServer& server) {
    for (auto definition : PlacementToolDefinitions()) {
        const auto name = definition.name;
        definition.handler = [&server, name](const QJsonObject& arguments) -> QJsonValue {
            QJsonObject error;
            auto* workspace = RequireWorkspace(server, error);
            if (!workspace) return error;
            PlacementToolHost host;
            host.identity = [&server, workspace] {
                QJsonObject identity{{"project_identity", QString::fromStdString(server.DocumentId())},
                    {"revision", QString::fromStdString(workspace->ObjectRevision())},
                    {"transaction_active", workspace->HasActiveTransaction()},
                    {"transaction_revision", RevisionJson(workspace->Revision())}};
                if (workspace->HasActiveTransaction())
                    identity.insert("transaction_id", QString::number(workspace->ActiveTransactionId()));
                return identity;
            };
            host.context = [&server, workspace]() -> Result<MapDataToolContext> {
                const auto current = workspace->MapDataContext(server.DocumentId(), server.DocumentGeneration());
                if (!current.Succeeded()) return {{}, current.diagnostics};
                MapDataToolContext context;
                context.original = current.value->original;
                context.document = current.value->document;
                context.binding = current.value->binding;
                context.includesActiveTransactionOverlay = current.value->includesActiveTransactionOverlay;
                context.preparedSnapshot = current.value->preparedSnapshot;
                context.acceptSnapshot = [workspace, binding = context.binding](auto snapshot) {
                    return workspace->PrepareProjectSearchMapCache(binding, std::move(snapshot));
                };
                return Result<MapDataToolContext>::Success(std::move(context));
            };
            host.apply = [workspace](const MapData::PreparedMapDataPlacement& plan,
                const PlacementPrepublishValidator& validate) {
                return workspace->ApplyPlacementPlan(plan, [&](const auto& before, const auto& after, const auto& receipt) {
                    return validate ? validate(before, after, receipt, {}) : Status::Ok();
                });
            };
            return InvokePlacementTool(name, arguments, host);
        };
        server.RegisterTool(std::move(definition));
    }
}

} // namespace War3Map::Mcp
