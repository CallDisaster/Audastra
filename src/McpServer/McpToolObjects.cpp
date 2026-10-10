#include "McpObjectTools.h"
#include "McpToolInternal.h"

namespace War3Map::Mcp {

void RegisterObjectTools(McpServer& server) {
    for (auto definition : ObjectToolDefinitions()) {
        const auto name = definition.name;
        definition.handler = [&server, name](const QJsonObject& arguments) -> QJsonValue {
            QJsonObject error;
            auto* workspace = RequireWorkspace(server, error);
            if (!workspace) return error;
            ObjectToolHost host;
            host.identity = [&server, workspace] {
                QJsonObject identity{{"project_identity", QString::fromStdString(server.DocumentId())},
                    {"revision", QString::fromStdString(workspace->ObjectRevision())},
                    {"transaction_active", workspace->HasActiveTransaction()},
                    {"transaction_revision", RevisionJson(workspace->Revision())}};
                if (workspace->HasActiveTransaction()) identity.insert("transaction_id", QString::number(workspace->ActiveTransactionId()));
                return identity;
            };
            host.context = [workspace](ObjectData::ObjectType type) { return workspace->ObjectContext(type); };
            host.commit = [workspace](const Services::ObjectCommandPlan& plan) { return workspace->ApplyObjectPlan(plan); };
            host.audit = [workspace](const ObjectData::ObjectKey& key) { return workspace->AssessObjectDeletion(key); };
            return InvokeObjectTool(name, arguments, host);
        };
        server.RegisterTool(std::move(definition));
    }
}

} // namespace War3Map::Mcp
