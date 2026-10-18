#include "McpToolInternal.h"
#include "McpMapDataJson.h"

#include "War3MapCore/Hash.h"
#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/MapDocument.h"
#include "War3MapCore/MapData/MapFields.h"
#include "War3MapCore/Services/MapDataQuery.h"
#include "War3MapCore/Workspace.h"

#include <QJsonDocument>
#include <QByteArray>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

namespace War3Map::Mcp {
namespace {

using namespace Services;
using namespace MapDataJson;
constexpr std::size_t kMapDataMaxJsonBytes = 1024 * 1024;

struct MapDataReadState {
    std::optional<MapDataQuerySnapshot> snapshot;
    // 地图归档只读模式：与 CLI map-data-query 同一绑定（archive-readonly-<SHA>），按路径与 SHA 复用快照。
    std::optional<MapDataQuerySnapshot> archiveSnapshot;
    std::filesystem::path archivePath;
};

std::optional<MapDataKind> MapKind(const QString& text) {
    return MapData::ParseMapDataKindId(text.toStdString());
}


MapDataQueryBinding CurrentBinding(const McpServer& server, const ProjectRevision& revision) {
    return {server.DocumentId(), server.DocumentGeneration(), revision.generation, revision.sourceSha256};
}

Status EnsureSnapshot(McpServer& server, McpWorkspace& workspace,
    const ProjectRevision& expected, MapDataReadState& state) {
    if (workspace.Revision() != expected) return Status::Failure("MCP", "MCP_REVISION_CONFLICT",
        "地图数据请求的 expected_revision 已过期。");
    if (workspace.HasActiveTransaction()) return Status::Failure("MCP", "MCP_MAP_DATA_TRANSACTION_ACTIVE",
        "地图数据只读服务仅包含已提交的目录来源；活动事务 overlay 与未保存 UI 修订不可查询。");
    const Status current = workspace.EnvironmentStatus();
    if (!current.Succeeded()) return current;
    const auto binding = CurrentBinding(server, expected);
    if (state.snapshot && state.snapshot->Binding() == binding) return Status::Ok();
    state.snapshot.reset();
    auto directory = DirectoryWorkspace::Open(workspace.SourceRoot());
    if (!directory.Succeeded()) return {std::move(directory.diagnostics)};
    if (directory.value->Snapshot().aggregateSha256 != expected.sourceSha256) return Status::Failure(
        "MCP", "MCP_MAP_DATA_SOURCE_CHANGED", "地图数据目录快照与当前会话来源身份不一致。");
    auto document = MapDocument::FromDirectory(*directory.value);
    if (!document.Succeeded()) return {std::move(document.diagnostics)};
    const Status verified = directory.value->VerifyUnchanged();
    if (!verified.Succeeded()) return verified;
    auto captured = CaptureMapDataSnapshot(std::move(*document.value), binding, false);
    if (!captured.Succeeded()) return {std::move(captured.diagnostics)};
    const Status after = workspace.EnvironmentStatus();
    if (!after.Succeeded()) return after;
    state.snapshot.emplace(std::move(*captured.value));
    return Status::Ok();
}

MapDataQueryBinding ArchiveBinding(const std::string& sourceSha256) {
    return {"archive-readonly-" + sourceSha256, 1, 0, sourceSha256};
}

// 归档模式：只读打开 .w3x/.w3m，核对来源 SHA（未给出时采用实际 SHA），每次请求都重新确认来源未变。
// 返回的行标识与成员 SHA 可直接用于 save_map_edit_candidate 的 field_edits。
Status EnsureArchiveSnapshot(const QJsonObject& parameters, bool requireSha, MapDataReadState& state) {
    auto path = ParseAbsolutePath(parameters, "source_map_path");
    if (!path.Succeeded()) return {std::move(path.diagnostics)};
    const auto expectedSha = parameters.value("expected_source_sha256").toString().toStdString();
    if ((requireSha || parameters.contains("expected_source_sha256")) && !IsLowerHexSha256(expectedSha))
        return Status::Failure("MCP", "MCP_MAP_DATA_SOURCE_SHA_INVALID", "expected_source_sha256 必须是 64 位小写十六进制。");
    auto archive = MapArchiveSession::Open(*path.value);
    if (!archive.Succeeded() || !archive.value) return {std::move(archive.diagnostics)};
    const auto& sourceSha = archive.value->SourceSha256();
    if (!expectedSha.empty() && sourceSha != expectedSha)
        return Status::Failure("MCP", "MAP_DATA_QUERY_STALE", "地图归档的 SHA-256 与 expected_source_sha256 不一致。");
    const auto binding = ArchiveBinding(sourceSha);
    if (!(state.archiveSnapshot && state.archiveSnapshot->Binding() == binding && state.archivePath == *path.value)) {
        state.archiveSnapshot.reset();
        auto captured = CaptureMapDataSnapshot(MapDocument::FromArchive(*archive.value), binding,
            archive.value->HasOpaqueMembers());
        if (!captured.Succeeded() || !captured.value) return {std::move(captured.diagnostics)};
        state.archiveSnapshot.emplace(std::move(*captured.value));
        state.archivePath = *path.value;
    }
    return archive.value->VerifySourceUnchanged();
}

// 两种来源互斥：展开目录会话用 expected_revision，地图归档用 source_map_path。
std::optional<bool> ArchiveMode(const QJsonObject& parameters, QJsonObject& error) {
    const bool archive = parameters.contains("source_map_path");
    const bool workspace = parameters.contains("expected_revision");
    if (archive == workspace) {
        error = Failure("MCP_MAP_DATA_SOURCE_SCOPE",
            "只能二选一：展开目录会话给 expected_revision，地图归档给 source_map_path（可附 expected_source_sha256）。");
        return std::nullopt;
    }
    if (!archive && parameters.contains("expected_source_sha256")) {
        error = Failure("MCP_MAP_DATA_SOURCE_SCOPE", "expected_source_sha256 只用于地图归档模式。");
        return std::nullopt;
    }
    return archive;
}

// 当前请求可用的快照与绑定；归档模式的修订为占位（generation 1 / revision 0）。
struct MapDataSource {
    const MapDataQuerySnapshot* snapshot = nullptr;
    MapDataQueryBinding binding;
    McpWorkspace* workspace = nullptr;
    std::optional<ProjectRevision> revision;
};

Result<MapDataSource> ResolveMapDataSource(McpServer& server, const QJsonObject& parameters, bool requireSha,
    MapDataReadState& state, QJsonObject& error) {
    const auto archive = ArchiveMode(parameters, error);
    if (!archive) return Result<MapDataSource>::Failure("MCP", "MCP_MAP_DATA_SOURCE_SCOPE", "来源参数无效。");
    MapDataSource source;
    if (*archive) {
        const Status loaded = EnsureArchiveSnapshot(parameters, requireSha, state);
        if (!loaded.Succeeded()) {
            error = Failure(loaded);
            return {std::nullopt, loaded.diagnostics};
        }
        source.snapshot = &*state.archiveSnapshot;
        source.binding = state.archiveSnapshot->Binding();
        return Result<MapDataSource>::Success(std::move(source));
    }
    auto expected = ParseRevision(parameters);
    if (!expected.Succeeded()) {
        error = Failure(expected);
        return {std::nullopt, expected.diagnostics};
    }
    source.workspace = RequireWorkspace(server, error);
    if (!source.workspace) return Result<MapDataSource>::Failure("MCP", "MCP_WORKSPACE_REQUIRED", "需要展开目录会话。");
    const Status loaded = EnsureSnapshot(server, *source.workspace, *expected.value, state);
    if (!loaded.Succeeded()) {
        error = Failure(loaded);
        return {std::nullopt, loaded.diagnostics};
    }
    source.snapshot = &*state.snapshot;
    source.binding = CurrentBinding(server, *expected.value);
    source.revision = *expected.value;
    return Result<MapDataSource>::Success(std::move(source));
}

QJsonObject SourceProperties() {
    auto properties = BasicSchema({}, {"expected_revision"}).value("properties").toObject();
    properties["source_map_path"] = QJsonObject{{"type", "string"}};
    properties["expected_source_sha256"] = QJsonObject{{"type", "string"}, {"pattern", "^[0-9a-f]{64}$"}};
    return properties;
}

QJsonObject MergeProperties(QJsonObject base, const QJsonObject& extra) {
    for (auto it = extra.begin(); it != extra.end(); ++it) base[it.key()] = it.value();
    return base;
}

QString WorkspaceKind(const MapDataSource& source) {
    return source.workspace ? QStringLiteral("expanded-directory") : QStringLiteral("map-archive-readonly");
}

QJsonValue RevisionValue(const MapDataSource& source) {
    return source.revision ? QJsonValue(RevisionJson(*source.revision)) : QJsonValue(QJsonValue::Null);
}

Status AfterStatus(const MapDataSource& source) {
    return source.workspace ? source.workspace->EnvironmentStatus() : Status::Ok();
}

QJsonObject BoundedJson(QJsonObject result) {
    if (static_cast<std::size_t>(QJsonDocument(result).toJson(QJsonDocument::Compact).size()) > kMapDataMaxJsonBytes)
        return Failure("MCP_MAP_DATA_JSON_BUDGET", "完整地图数据响应超过 1 MiB；请缩小页长。");
    return result;
}

} // namespace

void RegisterReadOnlyTools(McpServer& server) {
    const auto mapData = std::make_shared<MapDataReadState>();
    const QJsonObject unsignedSchema{{"type", "string"}, {"pattern", "^(0|[1-9][0-9]*)$"}};
    const QJsonObject shaSchema{{"type", "string"}, {"pattern", "^[0-9a-f]{64}$"}};
    const auto bindingSchema = BasicSchema({
        {"document_id", QJsonObject{{"type", "string"}}},
        {"document_generation", unsignedSchema}, {"revision", unsignedSchema},
        {"source_sha256", shaSchema}},
        {"document_id", "document_generation", "revision", "source_sha256"});
    {
        ToolDefinition tool;
        tool.name = "describe_map_data";
        tool.description = "只读描述七类地图数据、成员 SHA 与字段目录（字段标识、类型、是否可写及约束）。来源二选一："
            "展开目录会话给 expected_revision；地图归档（.w3x/.w3m）给 source_map_path，可附 expected_source_sha256，"
            "返回的 binding.source_sha256 与成员 SHA 可直接用于 save_map_edit_candidate 的 field_edits。仅已提交来源";
        tool.maxResponseBytes = kMapDataMaxJsonBytes;
        tool.inputSchema = BasicSchema(SourceProperties());
        tool.handler = [&server, mapData](const QJsonObject& parameters) -> QJsonValue {
            QJsonObject error;
            auto source = ResolveMapDataSource(server, parameters, false, *mapData, error);
            if (!source.Succeeded()) return error;
            auto description = DescribeMapData(*source.value->snapshot, source.value->binding);
            if (!description.Succeeded()) return Failure(description);
            QJsonArray members;
            for (const auto& member : description.value->members) members.append(MemberJson(member));
            QJsonObject result{{"ok", true}, {"schema_version", 1},
                {"workspace_kind", WorkspaceKind(*source.value)}, {"content_scope", "committed-source"},
                {"includes_active_transaction_overlay", false}, {"includes_unsaved_ui_revision", false},
                {"binding", BindingJson(description.value->binding)},
                {"revision", RevisionValue(*source.value)},
                {"has_opaque_archive_members", description.value->hasOpaqueArchiveMembers}, {"members", members},
                {"field_catalog", FieldCatalogJson()}};
            const Status after = AfterStatus(*source.value);
            if (!after.Succeeded()) return Failure(after);
            return BoundedJson(std::move(result));
        };
        server.RegisterTool(std::move(tool));
    }
    {
        ToolDefinition tool;
        tool.name = "query_map_data_page";
        tool.description = "按绑定和成员 SHA 分页读取七类地图数据；最多 128 行/256 KiB 逻辑数据/1 MiB JSON，续页索引仅在同一修订有效。"
            "来源与 describe_map_data 相同二选一；地图归档模式必须给 expected_source_sha256";
        tool.maxResponseBytes = kMapDataMaxJsonBytes;
        auto queryProperties = SourceProperties();
        tool.inputSchema = BasicSchema(MergeProperties(queryProperties, {
            {"expected_map_data_binding", bindingSchema},
            {"kind", QJsonObject{{"type", "string"}, {"enum", QJsonArray{
                "settings", "units", "doodads", "regions", "cameras", "sounds", "strings"}}}},
            {"expected_member_snapshot", QJsonObject{{"type", "string"},
                {"pattern", "^([0-9a-f]{64}|missing)$"}}},
            {"start_index", unsignedSchema},
            {"limit", QJsonObject{{"type", "integer"}, {"minimum", 1}, {"maximum", 128}}},
            {"max_logical_bytes", QJsonObject{{"type", "integer"}, {"minimum", 1}, {"maximum", 262144}}},
        }), {"expected_map_data_binding", "kind", "expected_member_snapshot", "start_index", "limit"});
        tool.handler = [&server, mapData](const QJsonObject& parameters) -> QJsonValue {
            QJsonObject error;
            auto source = ResolveMapDataSource(server, parameters, true, *mapData, error);
            if (!source.Succeeded()) return error;
            const auto rawBinding = parameters.value("expected_map_data_binding").toObject();
            const std::string documentId = rawBinding.value("document_id").toString().toStdString();
            if (documentId.empty() || documentId.size() > 128 || documentId.find('\0') != std::string::npos)
                return Failure("MCP_MAP_DATA_BINDING_INVALID", "document_id 必须为 1..128 字节且不含 NUL。");
            auto generation = ParseUnsignedId(rawBinding, "document_generation");
            auto revision = ParseUnsignedId(rawBinding, "revision", true);
            auto start = ParseUnsignedId(parameters, "start_index", true);
            if (!generation.Succeeded()) return Failure(generation);
            if (!revision.Succeeded()) return Failure(revision);
            if (!start.Succeeded()) return Failure(start);
            const MapDataQueryBinding binding{documentId, *generation.value, *revision.value,
                rawBinding.value("source_sha256").toString().toStdString()};
            const auto kind = MapKind(parameters.value("kind").toString());
            if (!kind) return Failure("MCP_MAP_DATA_KIND_INVALID", "未知地图数据类别。");
            if (binding != source.value->binding)
                return Failure("MAP_DATA_QUERY_STALE", "地图数据文档身份、世代、修订或来源 SHA-256 已变化。");
            auto described = DescribeMapData(*source.value->snapshot, binding);
            if (!described.Succeeded()) return Failure(described);
            const auto& member = described.value->members[static_cast<std::size_t>(*kind)];
            const QString memberSha = member.memberSha256 ? QString::fromStdString(*member.memberSha256) : "missing";
            if (parameters.value("expected_member_snapshot").toString() != memberSha)
                return Failure("MCP_MAP_DATA_MEMBER_STALE", "成员 SHA-256 或缺失状态与当前快照不一致。");
            const int limit = parameters.value("limit").toInt();
            const int maxLogicalBytes = parameters.contains("max_logical_bytes")
                ? parameters.value("max_logical_bytes").toInt() : 256 * 1024;
            if (limit < 1 || limit > 128 || maxLogicalBytes < 1 || maxLogicalBytes > 256 * 1024)
                return Failure("MCP_MAP_DATA_LIMIT", "单页预算只能为 1..128 行、1..262144 逻辑字节。");
            auto page = QueryMapDataPage(*source.value->snapshot, binding, *kind,
                MapDataQueryCursor{binding, *kind, *start.value},
                static_cast<std::size_t>(limit), static_cast<std::size_t>(maxLogicalBytes));
            if (!page.Succeeded()) return Failure(page);
            QJsonArray rows;
            for (const auto& row : page.value->rows) rows.append(RowJson(row));
            QJsonValue next(QJsonValue::Null);
            if (page.value->next) next = QJsonObject{{"binding", BindingJson(page.value->next->binding)},
                {"kind", MapKindName(page.value->next->kind)},
                {"next_index", QString::number(page.value->next->nextIndex)}};
            QJsonObject result{{"ok", true}, {"schema_version", 1},
                {"workspace_kind", WorkspaceKind(*source.value)}, {"content_scope", "committed-source"},
                {"includes_active_transaction_overlay", false}, {"includes_unsaved_ui_revision", false},
                {"binding", BindingJson(page.value->binding)}, {"revision", RevisionValue(*source.value)},
                {"member", MemberJson(page.value->member)},
                {"logical_bytes", QString::number(page.value->logicalBytes)},
                {"rows", rows}, {"next_cursor", next}};
            const Status after = AfterStatus(*source.value);
            if (!after.Succeeded()) return Failure(after);
            return BoundedJson(std::move(result));
        };
        server.RegisterTool(std::move(tool));
    }
    {
        ToolDefinition tool;
        tool.name = "list_triggers";
        tool.description = "只读列出分类与触发器稳定 ID；64 位 ID 始终以十进制字符串返回";
        tool.inputSchema = BasicSchema();
        tool.handler = [&server](const QJsonObject&) -> QJsonValue {
            QJsonObject error;
            McpWorkspace* workspace = RequireWorkspace(server, error);
            if (workspace == nullptr) return error;
            const auto& document = workspace->VisibleDocument();
            QJsonArray categories;
            for (const auto& category : document.categories) {
                categories.append(QJsonObject{
                    {"id", QString::number(category.id)},
                    {"category_id", category.categoryId},
                    {"name", QString::fromUtf8(category.name.text)},
                });
            }
            QJsonArray triggers;
            for (const auto& trigger : document.triggers) {
                triggers.append(QJsonObject{
                    {"id", QString::number(trigger.id)},
                    {"name", QString::fromUtf8(trigger.name.text)},
                    {"category_id", trigger.categoryId},
                    {"eca_count", static_cast<qint64>(trigger.ecas.size())},
                });
            }
            return QJsonObject{
                {"ok", true}, {"categories", categories}, {"triggers", triggers}};
        };
        server.RegisterTool(std::move(tool));
    }

    {
        ToolDefinition tool;
        tool.name = "get_trigger";
        tool.description = "按十进制稳定 ID 只读查看触发器根 ECA 与参数 ID";
        tool.inputSchema = BasicSchema(
            {{"trigger_id", QJsonObject{{"type", "string"}}}},
            {"trigger_id"});
        tool.handler = [&server](const QJsonObject& parameters) -> QJsonValue {
            QJsonObject error;
            McpWorkspace* workspace = RequireWorkspace(server, error);
            if (workspace == nullptr) return error;
            auto triggerId = ParseUnsignedId(parameters, "trigger_id");
            if (!triggerId.Succeeded()) return Failure(triggerId);
            const auto& document = workspace->VisibleDocument();
            const auto found = std::find_if(
                document.triggers.begin(), document.triggers.end(),
                [&](const Trigger::TriggerRecord& value) {
                    return value.id == *triggerId.value;
                });
            if (found == document.triggers.end()) {
                return Failure("MCP_TRIGGER_NOT_FOUND", "找不到指定 trigger_id。");
            }
            QJsonArray roots;
            for (std::uint32_t index : found->ecas) {
                if (index >= document.ecaNodes.size()) {
                    return Failure("MCP_TRIGGER_GRAPH_INVALID", "触发器根 ECA 索引越界。");
                }
                const Trigger::EcaNode& eca = document.ecaNodes[index];
                // WTG原始0/1/2/3和UiDefinitionKind枚举编号不同，必须用共用显式转换。
                const auto kind = Trigger::UiDefinitionKindFromEcaType(eca.type);
                if (kind == Trigger::UiDefinitionKind::Unknown)
                    return Failure("MCP_TRIGGER_ECA_KIND_INVALID", "触发器根 ECA 类型未知，未返回猜测的类型。");
                QJsonArray arguments;
                for (std::uint32_t argumentIndex : eca.arguments) {
                    if (argumentIndex >= document.argumentNodes.size()) {
                        return Failure("MCP_TRIGGER_GRAPH_INVALID", "ECA 参数索引越界。");
                    }
                    arguments.append(QString::number(
                        document.argumentNodes[argumentIndex].id));
                }
                roots.append(QJsonObject{
                    {"id", QString::number(eca.id)},
                    {"kind", KindName(kind)},
                    {"function", QString::fromUtf8(eca.name.text)},
                    {"argument_ids", arguments},
                    {"child_count", static_cast<qint64>(eca.children.size())},
                });
            }
            const auto customTextHash = Sha256Text(found->customText.text);
            return QJsonObject{
                {"ok", true},
                {"id", QString::number(found->id)},
                {"name", QString::fromUtf8(found->name.text)},
                {"category_id", found->categoryId},
                {"custom_text", found->customTextFlag != 0},
                {"custom_text_bytes",
                    static_cast<qint64>(found->customText.text.size())},
                {"custom_text_sha256", customTextHash.Succeeded()
                    ? QString::fromUtf8(*customTextHash.value)
                    : QString{}},
                {"run_on_init", found->runOnInit != 0},
                {"root_ecas", roots},
            };
        };
        server.RegisterTool(std::move(tool));
    }

    {
        ToolDefinition tool;
        tool.name = "get_ui_catalog";
        tool.description = "只读搜索 UI Catalog 函数签名";
        tool.inputSchema = BasicSchema(
            {{"query", QJsonObject{{"type", "string"}}}});
        tool.handler = [&server](const QJsonObject& parameters) -> QJsonValue {
            QJsonObject error;
            McpWorkspace* workspace = RequireWorkspace(server, error);
            if (workspace == nullptr) return error;
            const std::string query = parameters.value("query").toString().toStdString();
            QJsonArray definitions;
            for (const auto kind : {
                    Trigger::UiDefinitionKind::Event,
                    Trigger::UiDefinitionKind::Condition,
                    Trigger::UiDefinitionKind::Action,
                    Trigger::UiDefinitionKind::Call}) {
                for (const Trigger::UiDefinition* definition :
                     workspace->Catalog().Definitions(kind)) {
                    if (!query.empty() && definition->name.find(query) == std::string::npos) {
                        continue;
                    }
                    QJsonArray arguments;
                    for (const auto& argument : definition->Active().arguments) {
                        arguments.append(QJsonObject{
                            {"type", QString::fromUtf8(argument.type)},
                            {"default", QString::fromUtf8(argument.defaultValue)},
                        });
                    }
                    QString returns;
                    for (const auto& [field, value] : definition->Active().fields) {
                        if (field == "returns") returns = QString::fromUtf8(value);
                    }
                    QJsonObject item{
                        {"kind", KindName(kind)},
                        {"name", QString::fromUtf8(definition->name)},
                        {"returns", returns},
                        {"arguments", arguments},
                    };
                    // 回调块：真实声明末尾多一个 code 参数，块内动作放在 child_branch 0，生成为独立函数传入。
                    if (const auto* callback = kind == Trigger::UiDefinitionKind::Action
                            ? workspace->Catalog().CallbackActions().Find(definition->name) : nullptr) {
                        item.insert("callback_block", QJsonObject{{"child_branch", 0}, {"child_kind", "action"},
                            {"shares_outer_locals", callback->sharesLocals}});
                    }
                    definitions.append(item);
                    if (definitions.size() >= 100) break;
                }
                if (definitions.size() >= 100) break;
            }
            return QJsonObject{{"ok", true}, {"definitions", definitions}};
        };
        server.RegisterTool(std::move(tool));
    }
}

} // namespace War3Map::Mcp
