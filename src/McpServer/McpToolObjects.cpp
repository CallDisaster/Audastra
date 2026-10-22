#include "McpObjectTools.h"
#include "McpObjectSourceCapture.h"
#include "McpToolInternal.h"
#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/MapDocument.h"
#include "War3MapCore/ObjectData/EffectiveObjectCatalog.h"

#include <QRegularExpression>

#include <algorithm>
#include <filesystem>
#include <memory>

namespace War3Map::Mcp {
namespace {

// 独立服务直接只读查询 .w3x 的来源缓存：与 describe_map_data 相同的 source_map_path /
// expected_source_sha256 绑定，同一路径与 SHA 复用快照，每次调用都复核源图未变化。
struct ArchiveObjectCache {
    std::filesystem::path path;
    std::string sha256;
    std::shared_ptr<const MapArchiveSession> archive;
    std::unique_ptr<McpObjectWorkspaceState> state;
};

bool IsArchiveQueryTool(const QString& name) {
    return name == "objects_list" || name == "objects_get" || name == "objects_text" || name == "objects_field_options";
}

QJsonArray CatalogSourcesJson(const ObjectData::EffectiveObjectCatalog& catalog) {
    QJsonArray sources;
    for (const auto& input : catalog.Inputs()) {
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
    return sources;
}

Result<const McpObjectWorkspaceState*> EnsureArchiveObjects(const QJsonObject& arguments, ArchiveObjectCache& cache) {
    using StateResult = Result<const McpObjectWorkspaceState*>;
    const auto pathText = arguments.value("source_map_path").toString();
    const auto shaText = arguments.value("expected_source_sha256").toString();
    if (pathText.isEmpty() || pathText.size() > 1024 || pathText.contains(QChar::Null))
        return StateResult::Failure("MCP", "MCP_OBJECT_SOURCE_PATH_INVALID", "source_map_path 必须是本机 .w3x/.w3m 的绝对路径。");
    if (!QRegularExpression(QStringLiteral("^[0-9a-f]{64}$")).match(shaText).hasMatch())
        return StateResult::Failure("MCP", "MCP_OBJECT_SOURCE_SHA_INVALID", "按归档查询物编时须给出 64 位小写 expected_source_sha256。");
    const std::filesystem::path path(pathText.toStdWString());
    if (!path.is_absolute())
        return StateResult::Failure("MCP", "MCP_OBJECT_SOURCE_PATH_INVALID", "source_map_path 必须是绝对路径。");
    const auto sha = shaText.toStdString();
    if (cache.state && cache.path == path && cache.sha256 == sha) {
        const auto unchanged = cache.archive->VerifySourceUnchanged();
        if (!unchanged.Succeeded()) {
            cache = {};
            return {{}, unchanged.diagnostics};
        }
        return StateResult::Success(cache.state.get());
    }
    cache = {};
    auto gameRoot = HostConfiguredSaveRoot("FORGE_MCP_WAR3_ROOT");
    if (!gameRoot.Succeeded()) return {{}, gameRoot.diagnostics};
    auto opened = MapArchiveSession::Open(path);
    if (!opened.Succeeded()) return {{}, opened.diagnostics};
    if (opened.value->SourceSha256() != sha)
        return StateResult::Failure("MCP", "MAP_OBJECT_SOURCE_STALE", "源地图 SHA 与 expected_source_sha256 不一致；重新取得 SHA 后再查询。");
    auto archive = std::make_shared<const MapArchiveSession>(std::move(*opened.value));
    Services::ObjectCatalogSources sources;
    sources.archive = archive.get();
    sources.gameRoot = *gameRoot.value;
    ObjectMemberSource members;
    std::vector<std::string> names;
    for (const auto& member : archive->Members()) {
        // 非 UTF-8 名字不可能是七类物编或主脚本成员，只是不参与按名字匹配。
        try {
            auto generic = member.name;
            std::replace(generic.begin(), generic.end(), '\\', '/');
            members.files.emplace_back(std::u8string(generic.begin(), generic.end()));
            names.push_back(member.name);
        } catch (const std::exception&) {
        }
    }
    members.read = [archive, files = members.files, names](const std::filesystem::path& file, std::size_t maximumBytes)
        -> Result<std::vector<std::byte>> {
        const auto found = std::find(files.begin(), files.end(), file);
        if (found == files.end())
            return Result<std::vector<std::byte>>::Failure("MCP", "MCP_OBJECT_MEMBER_MISSING", "归档成员已不在命名清单中。");
        auto bytes = archive->ReadMember(names[static_cast<std::size_t>(found - files.begin())]);
        if (bytes.Succeeded() && bytes.value->size() > maximumBytes)
            return Result<std::vector<std::byte>>::Failure("MCP", "MCP_OBJECT_MEMBER_BUDGET", "物编成员超过单成员读取预算。");
        return bytes;
    };
    auto state = CaptureObjectWorkspaceState(sources, members, MapDocument::FromArchive(*archive));
    if (!state.Succeeded()) return {{}, state.diagnostics};
    const auto unchanged = archive->VerifySourceUnchanged();
    if (!unchanged.Succeeded()) return {{}, unchanged.diagnostics};
    cache.path = path;
    cache.sha256 = sha;
    cache.archive = std::move(archive);
    cache.state = std::move(*state.value);
    return StateResult::Success(cache.state.get());
}

QJsonObject InvokeArchiveObjectTool(const QString& name, const QJsonObject& arguments, ArchiveObjectCache& cache) {
    const auto state = EnsureArchiveObjects(arguments, cache);
    if (!state.Succeeded()) return Failure(state);
    const auto sha = QString::fromStdString(cache.sha256);
    ObjectToolHost host;
    host.identity = [sha] {
        return QJsonObject{{"project_identity", "archive-readonly-" + sha}, {"revision", sha}, {"transaction_active", false}};
    };
    host.context = [state = *state.value](ObjectData::ObjectType type) { return CommittedObjectContext(*state, type); };
    auto forwarded = arguments;
    forwarded.remove("source_map_path");
    forwarded.remove("expected_source_sha256");
    auto result = InvokeObjectTool(name, forwarded, host);
    if (result.value("ok").toBool()) {
        result.insert("workspace_kind", "map-archive-readonly");
        result.insert("source_sha256", sha);
        if (name == "objects_list" || name == "objects_get")
            result.insert("catalog_sources", CatalogSourcesJson(*(*state.value)->effectiveCatalog));
    }
    return result;
}

} // namespace

void RegisterObjectTools(McpServer& server) {
    const auto cache = std::make_shared<ArchiveObjectCache>();
    for (auto definition : ObjectToolDefinitions()) {
        const auto name = definition.name;
        if (IsArchiveQueryTool(name)) {
            auto schema = definition.inputSchema;
            auto properties = schema.value("properties").toObject();
            properties.insert("source_map_path", QJsonObject{{"type", "string"}, {"maxLength", 1024},
                {"description", "独立服务只读查询 .w3x/.w3m 的绝对路径；与 expected_source_sha256 一起给出，不需要展开工作区"}});
            properties.insert("expected_source_sha256", QJsonObject{{"type", "string"}, {"pattern", "^[0-9a-f]{64}$"}});
            schema.insert("properties", properties);
            definition.inputSchema = schema;
            definition.description += QStringLiteral("独立服务可用 source_map_path + expected_source_sha256 直接只读查询地图归档。");
        }
        definition.handler = [&server, name, cache](const QJsonObject& arguments) -> QJsonValue {
            if (IsArchiveQueryTool(name) && (arguments.contains("source_map_path") || arguments.contains("expected_source_sha256")))
                return InvokeArchiveObjectTool(name, arguments, *cache);
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
                if (captured) result.insert("catalog_sources", CatalogSourcesJson(*captured));
            }
            return result;
        };
        server.RegisterTool(std::move(definition));
    }
}

} // namespace War3Map::Mcp
