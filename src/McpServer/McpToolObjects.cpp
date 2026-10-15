#include "McpObjectTools.h"
#include "McpToolInternal.h"
#include "War3MapCore/ObjectData/EffectiveObjectCatalog.h"

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
            auto result = InvokeObjectTool(name, arguments, host);
            if (result.value("ok").toBool() && (name == "objects_list" || name == "objects_get")) {
                const auto captured = workspace->EffectiveObjectSources();
                if (captured) {
                    QJsonArray sources;
                    for (const auto& input : captured->Inputs()) {
                        QJsonObject source{{"logical_path", QString::fromStdString(input.logicalPath)}, {"present", input.present}};
                        if (input.present) {
                            const auto layer = input.location.layer;
                            const auto layerName = layer == Resources::ResourceLayer::MapImport ? "map"
                                : layer == Resources::ResourceLayer::SessionOverride ? "session"
                                : layer == Resources::ResourceLayer::Campaign ? "campaign"
                                : layer == Resources::ResourceLayer::Game ? "game" : "other";
                            source.insert("layer", QString::fromLatin1(layerName));
                            source.insert("source_id", QString::fromStdString(input.location.sourceId));
                            source.insert("member", QString::fromStdString(input.location.sourcePath));
                            source.insert("container_identity", QString::fromStdString(input.containerIdentity));
                            source.insert("sha256", QString::fromStdString(input.sha256));
                            source.insert("byte_size", static_cast<qint64>(input.byteSize));
                        }
                        sources.push_back(std::move(source));
                    }
                    result.insert("catalog_sources", std::move(sources));
                }
            }
            return result;
        };
        server.RegisterTool(std::move(definition));
    }
}

} // namespace War3Map::Mcp
