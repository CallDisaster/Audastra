#include "McpImportTools.h"
#include "McpToolInternal.h"

namespace War3Map::Mcp {

void RegisterImportTools(McpServer& server) {
    for (auto definition : ImportToolDefinitions()) {
        const auto name = definition.name;
        definition.handler = [&server, name](const QJsonObject& arguments) -> QJsonValue {
            QJsonObject error;
            auto* workspace = RequireWorkspace(server, error);
            if (!workspace) return error;
            const auto environment = workspace->EnvironmentStatus();
            if (!environment.Succeeded()) return Failure(environment);
            ImportToolHost host;
            host.identity = [&server, workspace] {
                return QJsonObject{{"project_identity", QString::fromStdString(server.DocumentId())},
                    {"revision", QString::fromStdString(workspace->ObjectRevision())}};
            };
            host.context = [&server, workspace]() -> Result<std::shared_ptr<const Services::ImportInventorySnapshot>> {
                using Snapshot = std::shared_ptr<const Services::ImportInventorySnapshot>;
                const auto source = DirectoryWorkspace::Open(workspace->SourceRoot());
                if (!source.Succeeded()) return {{}, source.diagnostics};
                const auto revision = workspace->Revision();
                if (source.value->Snapshot().aggregateSha256 != revision.sourceSha256)
                    return Result<Snapshot>::Failure("Mcp.Imports", "IMPORT_SOURCE_CHANGED",
                        "目录来源与当前工程快照不一致；重新打开工程后再查询。");
                Services::ImportInventoryBinding binding{server.DocumentId(), server.DocumentGeneration(),
                    workspace->ObjectRevision(), revision.sourceSha256, workspace->ObjectRevision(), "committed-source"};
                // 独立宿主尚无导入写事务；物理字节身份只描述原目录，不冒充其它领域的未保存覆盖。
                return Services::CaptureImportInventory(*source.value, std::move(binding));
            };
            const auto result = InvokeImportTool(name, arguments, host);
            const auto after = workspace->EnvironmentStatus();
            return after.Succeeded() ? result : Failure(after);
        };
        server.RegisterTool(std::move(definition));
    }
}

} // namespace War3Map::Mcp
