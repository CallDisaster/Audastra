#include "McpObjectResourceTools.h"
#include "McpToolInternal.h"
#include "War3MapCore/Game/GameDataSource.h"
#include "War3MapCore/ObjectData/LniObjectCodec.h"
#include "War3MapCore/Services/ImportInventoryQuery.h"
#include "War3MapCore/Services/ObjectResourceDirectory.h"

namespace War3Map::Mcp {
namespace {
using namespace Services;
using CatalogResult = Result<std::shared_ptr<const ObjectResourceCatalog>>;
CatalogResult Load(McpServer& server, McpWorkspace& workspace, ObjectResourceOrigin origin) {
    const auto revision = workspace.ObjectRevision();
    if (origin == ObjectResourceOrigin::Object) {
        auto objects = workspace.ObjectContext(ObjectData::ObjectType::Unit);
        if (!objects.Succeeded()) return {{}, objects.diagnostics};
        std::array<ProjectSearchObjectSource, 7> sources;
        for (std::size_t i = 0; i < sources.size(); ++i) {
            auto& source = sources[i]; source.type = static_cast<ObjectData::ObjectType>(i);
            const auto* document = objects.value->documents[i];
            source.state = document ? (document->IsStructurallySafe() ? ProjectSearchState::Ready : ProjectSearchState::Partial)
                : ProjectSearchState::Unavailable;
            if (document) source.records = document->Objects();
        }
        ObjectResourceContext context; context.revision = revision; context.schema = objects.value->schema;
        context.names.objects = sources; context.names.standardObjects = objects.value->standard;
        context.names.mapDocument = objects.value->nameMap;
        return CaptureObjectResourceCatalog(context);
    }
    if (origin == ObjectResourceOrigin::Game) {
        auto game = Game::GameDataSource::MountStrict(workspace.GameRoot());
        if (!game.Succeeded()) return {{}, game.diagnostics};
        return CaptureGameResourceCatalog(*game.value, revision);
    }
    const auto source = DirectoryWorkspace::Open(workspace.SourceRoot());
    if (!source.Succeeded()) return {{}, source.diagnostics};
    const auto sourceSha = workspace.Revision().sourceSha256;
    if (source.value->Snapshot().aggregateSha256 != sourceSha)
        return CatalogResult::Failure("Object.Resource", "OBJECT_RESOURCE_SOURCE_CHANGED", "目录来源已变化，请重新打开工程。");
    ImportInventoryBinding binding{server.DocumentId(), server.DocumentGeneration(), revision, sourceSha, revision, "committed-source"};
    auto inventory = CaptureImportInventory(*source.value, binding);
    if (!inventory.Succeeded()) return {{}, inventory.diagnostics};
    return CaptureImportResourceCatalog(**inventory.value, binding);
}
Result<ObjectResourceInfo> Inspect(McpWorkspace& workspace, std::string_view value, ObjectResourceKind kind) {
    using Return = Result<ObjectResourceInfo>;
    auto directory = DirectoryWorkspace::Open(workspace.SourceRoot());
    if (!directory.Succeeded()) return {{}, directory.diagnostics};
    if (directory.value->Snapshot().aggregateSha256 != workspace.Revision().sourceSha256)
        return Return::Failure("Object.Resource", "OBJECT_RESOURCE_SOURCE_CHANGED", "目录来源已变化。");
    ImportInventoryBinding binding{"resource-inspect", 1, workspace.ObjectRevision(),
        workspace.Revision().sourceSha256, workspace.ObjectRevision(), "committed-source"};
    return InspectDirectoryObjectResource(*directory.value, workspace.GameRoot(), binding, {}, value, kind);
}
}
void RegisterObjectResourceTools(McpServer& server) {
    for (auto definition : ObjectResourceToolDefinitions()) {
        const auto name = definition.name;
        definition.handler = [&server, name](const QJsonObject& arguments) -> QJsonValue {
            QJsonObject error; auto* workspace = RequireWorkspace(server, error); if (!workspace) return error;
            const auto before = workspace->EnvironmentStatus(); if (!before.Succeeded()) return Failure(before);
            ObjectResourceToolHost host;
            host.identity = [&] { return QJsonObject{{"project_identity", QString::fromStdString(server.DocumentId())},
                {"revision", QString::fromStdString(workspace->ObjectRevision())}, {"resource_revision", QString::fromStdString(workspace->ObjectRevision())}}; };
            host.load = [&](auto origin) { return Load(server, *workspace, origin); };
            host.inspect = [&](auto value, auto kind) { return Inspect(*workspace, value, kind); };
            host.field = [&](auto type, auto id) -> Result<ObjectResourceField> {
                auto context = workspace->ObjectContext(type); if (!context.Succeeded()) return {{}, context.diagnostics};
                const auto* definition = context.value->schema->FindNative(type, id);
                auto field = definition ? DescribeObjectResourceField(*definition) : std::optional<ObjectResourceField>{};
                if (!field) return Result<ObjectResourceField>::Failure("Object.Resource", "OBJECT_RESOURCE_FIELD_UNSUPPORTED", "字段不存在或不是资源字段。");
                return Result<ObjectResourceField>::Success(*field);
            };
            auto result = InvokeObjectResourceTool(name, arguments, host);
            const auto after = workspace->EnvironmentStatus(); return after.Succeeded() ? result : Failure(after);
        };
        server.RegisterTool(std::move(definition));
    }
}
}
