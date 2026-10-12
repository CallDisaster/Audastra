#include "McpToolInternal.h"
#include "McpMapDataJson.h"

#include "War3MapCore/Hash.h"
#include "War3MapCore/MapData/MapFields.h"
#include "War3MapCore/Services/MapDataQuery.h"
#include "War3MapCore/Workspace.h"

#include <QJsonDocument>
#include <QByteArray>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
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
        tool.description = "只读描述当前展开目录会话的七类地图数据、成员 SHA 与字段目录（字段标识、类型、是否可写及约束）；仅已提交来源，活动事务和未保存 UI 修订不可见";
        tool.maxResponseBytes = kMapDataMaxJsonBytes;
        tool.inputSchema = BasicSchema({}, {"expected_revision"});
        tool.handler = [&server, mapData](const QJsonObject& parameters) -> QJsonValue {
            auto expected = ParseRevision(parameters);
            if (!expected.Succeeded()) return Failure(expected);
            QJsonObject error;
            McpWorkspace* workspace = RequireWorkspace(server, error);
            if (!workspace) return error;
            const Status loaded = EnsureSnapshot(server, *workspace, *expected.value, *mapData);
            if (!loaded.Succeeded()) return Failure(loaded);
            auto description = DescribeMapData(*mapData->snapshot, CurrentBinding(server, *expected.value));
            if (!description.Succeeded()) return Failure(description);
            QJsonArray members;
            for (const auto& member : description.value->members) members.append(MemberJson(member));
            QJsonObject result{{"ok", true}, {"schema_version", 1},
                {"workspace_kind", "expanded-directory"}, {"content_scope", "committed-source"},
                {"includes_active_transaction_overlay", false}, {"includes_unsaved_ui_revision", false},
                {"binding", BindingJson(description.value->binding)},
                {"revision", RevisionJson(*expected.value)},
                {"has_opaque_archive_members", description.value->hasOpaqueArchiveMembers}, {"members", members},
                {"field_catalog", FieldCatalogJson()}};
            const Status after = workspace->EnvironmentStatus();
            if (!after.Succeeded()) return Failure(after);
            return BoundedJson(std::move(result));
        };
        server.RegisterTool(std::move(tool));
    }
    {
        ToolDefinition tool;
        tool.name = "query_map_data_page";
        tool.description = "按会话绑定和成员 SHA 分页读取七类地图数据；最多 128 行/256 KiB 逻辑数据/1 MiB JSON，续页索引仅在同一修订有效";
        tool.maxResponseBytes = kMapDataMaxJsonBytes;
        tool.inputSchema = BasicSchema({
            {"expected_map_data_binding", bindingSchema},
            {"kind", QJsonObject{{"type", "string"}, {"enum", QJsonArray{
                "settings", "units", "doodads", "regions", "cameras", "sounds", "strings"}}}},
            {"expected_member_snapshot", QJsonObject{{"type", "string"},
                {"pattern", "^([0-9a-f]{64}|missing)$"}}},
            {"start_index", unsignedSchema},
            {"limit", QJsonObject{{"type", "integer"}, {"minimum", 1}, {"maximum", 128}}},
            {"max_logical_bytes", QJsonObject{{"type", "integer"}, {"minimum", 1}, {"maximum", 262144}}},
        }, {"expected_revision", "expected_map_data_binding", "kind", "expected_member_snapshot",
            "start_index", "limit"});
        tool.handler = [&server, mapData](const QJsonObject& parameters) -> QJsonValue {
            auto expected = ParseRevision(parameters);
            if (!expected.Succeeded()) return Failure(expected);
            QJsonObject error;
            McpWorkspace* workspace = RequireWorkspace(server, error);
            if (!workspace) return error;
            const Status loaded = EnsureSnapshot(server, *workspace, *expected.value, *mapData);
            if (!loaded.Succeeded()) return Failure(loaded);
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
            if (binding != CurrentBinding(server, *expected.value))
                return Failure("MAP_DATA_QUERY_STALE", "地图数据文档身份、世代、修订或来源 SHA-256 已变化。");
            auto described = DescribeMapData(*mapData->snapshot, binding);
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
            auto page = QueryMapDataPage(*mapData->snapshot, binding, *kind,
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
                {"workspace_kind", "expanded-directory"}, {"content_scope", "committed-source"},
                {"includes_active_transaction_overlay", false}, {"includes_unsaved_ui_revision", false},
                {"binding", BindingJson(page.value->binding)}, {"revision", RevisionJson(*expected.value)},
                {"member", MemberJson(page.value->member)},
                {"logical_bytes", QString::number(page.value->logicalBytes)},
                {"rows", rows}, {"next_cursor", next}};
            const Status after = workspace->EnvironmentStatus();
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
