#include "McpTerrainEditTools.h"
#include "McpToolInternal.h"

namespace War3Map::Mcp {

void RegisterTerrainEditTools(McpServer& server) {
    for (auto definition : TerrainEditToolDefinitions()) {
        const auto name = definition.name;
        definition.handler = [&server, name](const QJsonObject& arguments) -> QJsonValue {
            QJsonObject error;
            auto* workspace = RequireWorkspace(server, error);
            if (!workspace) return error;
            TerrainEditToolHost host;
            host.identity = [&server, workspace] {
                QJsonObject identity{{"project_identity", QString::fromStdString(server.DocumentId())},
                    {"revision", QString::fromStdString(workspace->ObjectRevision())},
                    {"transaction_active", workspace->HasActiveTransaction()},
                    {"transaction_revision", RevisionJson(workspace->Revision())}};
                if (workspace->HasActiveTransaction()) identity.insert("transaction_id", QString::number(workspace->ActiveTransactionId()));
                return identity;
            };
            host.context = [&server, workspace]() -> Result<TerrainEditToolContext> {
                const auto current = workspace->TerrainContext(server.DocumentId(), server.DocumentGeneration());
                if (!current.Succeeded()) return {{}, current.diagnostics};
                return Result<TerrainEditToolContext>::Success({current.value->session, current.value->document,
                    current.value->includesActiveTransactionOverlay, false});
            };
            host.apply = [workspace](const auto& plan, const auto& validate) { return workspace->ApplyTerrainPlan(plan, validate); };
            return InvokeTerrainEditTool(name, arguments, host);
        };
        server.RegisterTool(std::move(definition));
    }
}

} // namespace War3Map::Mcp
