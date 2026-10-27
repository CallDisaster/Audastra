#include "McpMapCustomSettingsTools.h"
#include "McpToolInternal.h"
#include "War3MapCore/Services/MapCustomSettingsQuery.h"
#include <QJsonDocument>

namespace War3Map::Mcp {
namespace {
QJsonObject Object(QJsonObject properties, QJsonArray required) { return {{"type", "object"}, {"properties", properties}, {"required", required}, {"additionalProperties", false}}; }
QJsonObject String() { return {{"type", "string"}}; }
QJsonObject Integer(int minimum = 0) { return {{"type", "integer"}, {"minimum", minimum}, {"maximum", 2147483647}}; }
QJsonObject Array(QJsonObject items, int maximum) { return {{"type", "array"}, {"items", items}, {"maxItems", maximum}}; }
QJsonObject Body(const std::string& text) { return QJsonDocument::fromJson(QByteArray::fromStdString(text)).object(); }
QJsonObject QueryProperties() {
    return {{"section", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"summary", "upgrades", "tech_restrictions", "random_units", "random_items"}}}},
        {"settings_binding", QJsonObject{{"type", "object"}}}, {"part", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"table_headers", "columns", "rows", "sets", "items"}}}},
        {"number", Integer(-2147483647 - 1)}, {"set_index", Integer()}, {"offset", Integer()}, {"limit", QJsonObject{{"type", "integer"}, {"minimum", 1}, {"maximum", 256}}}};
}
QJsonObject Stamp(QJsonObject result, const QJsonObject& identity) {
    for (const auto* key : {"project_identity", "revision", "document_generation", "scope", "dirty"}) if (identity.contains(key)) result.insert(key, identity.value(key));
    return result;
}
} // namespace

QJsonObject MapCustomSettingsOperationSchema() {
    const QJsonObject raw{{"type", "string"}, {"pattern", "^[0-9a-f]{8}$"}};
    const auto choice = QJsonObject{{"oneOf", QJsonArray{Object({{"kind", QJsonObject{{"const", "none"}}}}, {"kind"}),
        Object({{"kind", QJsonObject{{"const", "object"}}}, {"id_hex", raw}}, {"kind", "id_hex"})}}};
    const QJsonObject chance{{"type", "integer"}, {"minimum", 0}, {"maximum", 100}};
    const auto name = QJsonObject{{"oneOf", QJsonArray{Object({{"keep", QJsonObject{{"const", true}}}}, {"keep"}),
        Object({{"utf8", QJsonObject{{"type", "string"}, {"maxLength", 4096}}}}, {"utf8"})}}};
    QJsonArray alternatives;
    const QStringList sections{"upgrades", "tech_restrictions", "random_units", "random_items"};
    for (int s = 0; s < sections.size(); ++s) {
        const bool table = s >= 2;
        QJsonObject value;
        const QJsonObject mask{{"type", "integer"}, {"minimum", 0}, {"maximum", 4294967295.0}};
        if (s == 0) value = Object({{"player_mask", mask}, {"id_hex", raw}, {"level", Integer()}, {"availability", QJsonObject{{"type", "integer"}, {"minimum", 0}, {"maximum", 2}}}}, {"player_mask", "id_hex", "level", "availability"});
        else if (s == 1) value = Object({{"player_mask", mask}, {"id_hex", raw}}, {"player_mask", "id_hex"});
        else if (s == 2) value = Object({{"name", name}, {"column_types", Array(QJsonObject{{"type", "integer"}, {"minimum", 0}, {"maximum", 2}}, 64)},
            {"rows", Array(Object({{"chance", chance}, {"ids", Array(choice, 64)}}, {"chance", "ids"}), 4096)}}, {"name", "column_types", "rows"});
        else value = Object({{"name", name}, {"sets", Array(Array(Object({{"chance", chance}, {"id", choice}}, {"chance", "id"}), 16384), 4096)}}, {"name", "sets"});
        for (const auto& op : QStringList{"append", table ? "replace" : "update", "delete"}) {
            QJsonObject properties{{"section", QJsonObject{{"const", sections[s]}}}, {"op", QJsonObject{{"const", op}}}};
            QJsonArray required{"section", "op"};
            if (table) { properties.insert("number", Integer()); required.append("number"); }
            if (op == "append") { properties.insert("expected_count", Integer()); required.append("expected_count"); }
            else { if (!table) { properties.insert("index", Integer()); required.append("index"); }
                const auto key = table ? "expected_table_key" : "expected_row_key";
                properties.insert(key, QJsonObject{{"type", "string"}, {"pattern", "^[0-9a-f]{64}$"}}); required.append(key); }
            if (op != "delete") { properties.insert("value", value); required.append("value"); }
            alternatives.append(Object(properties, required));
        }
    }
    return {{"oneOf", alternatives}};
}
std::vector<ToolDefinition> MapCustomSettingsToolDefinitions() {
    std::vector<ToolDefinition> result;
    for (bool write : {false, true}) {
        ToolDefinition tool; tool.name = write ? "map_custom_settings_edit" : "map_custom_settings_get";
        tool.permission = write ? ToolPermission::Edit : ToolPermission::Query;
        tool.description = write ? "原生 v25 科技逐行 append/update/delete，随机表整表预览；逐条 CAS，失败不发布前缀。dry_run 必填，随机提交尚未开放，一个批次走同一撤销事务。"
            : "查询活动地图升级、科技限制和随机单位/物品表。先 summary 取得 settings_binding，后续精确回传；offset 从0起，每页最多256项，名称保留 raw_hex。";
        auto properties = write ? QJsonObject{{"settings_binding", QJsonObject{{"type", "object"}}},
            {"operations", Array(MapCustomSettingsOperationSchema(), 128)}, {"dry_run", QJsonObject{{"type", "boolean"}}}} : QueryProperties();
        properties.insert("project_identity", String()); properties.insert("expected_revision", String());
        auto required = write ? QJsonArray{"settings_binding", "operations", "dry_run"} : QJsonArray{"section"};
        required.append("project_identity"); required.append("expected_revision"); tool.inputSchema = Object(properties, required);
        tool.maxRequestIdBytes = 128; tool.maxResponseBytes = 1024 * 1024 + 4096; result.push_back(std::move(tool));
    }
    return result;
}
QJsonObject InvokeMapCustomSettingsTool(const QString& name, const QJsonObject& arguments, const MapCustomSettingsToolHost& host) {
    if (!host.identity || !host.context) return Failure("SETTINGS_HOST_UNAVAILABLE", "当前宿主没有地图数据来源。");
    const auto identity = host.identity();
    if (!arguments.value("project_identity").isString() || !arguments.value("expected_revision").isString()
        || arguments.value("project_identity") != identity.value("project_identity") || arguments.value("expected_revision") != identity.value("revision"))
        return Failure("SETTINGS_PROJECT_STALE", "工程或修订已变化，请重新查询。");
    auto context = host.context(); if (!context.Succeeded()) return Failure(context);
    if (!context.value->document) return Failure("SETTINGS_HOST_UNAVAILABLE", "当前地图文档不可用。");
    auto payload = arguments; payload.remove("project_identity"); payload.remove("expected_revision");
    if (name == "map_custom_settings_get") {
        auto query = Services::QueryMapCustomSettings(*context.value->document, context.value->binding, QJsonDocument(payload).toJson(QJsonDocument::Compact).toStdString());
        if (!query.Succeeded()) return Failure(query);
        auto result = Body(*query.value); result.insert("ok", true); return Stamp(result, identity);
    }
    if (name != "map_custom_settings_edit") return Failure("SETTINGS_TOOL_UNKNOWN", "未知自定义表入口。");
    if (!payload.value("dry_run").isBool() || payload.size() != 3 || !payload.contains("operations") || !payload.value("settings_binding").isObject())
        return Failure("SETTINGS_EDIT_SHAPE_INVALID", "修改须只携带 settings_binding、operations 与必填 dry_run。");
    const auto summary = Services::QueryMapCustomSettings(*context.value->document, context.value->binding, R"({"section":"summary"})");
    if (!summary.Succeeded()) return Failure(summary);
    if (payload.value("settings_binding") != Body(*summary.value).value("settings_binding")) return Failure("SETTINGS_QUERY_STALE", "自定义表 binding 已变化。");
    const QJsonObject edit{{"expected_member_sha256", payload.value("settings_binding").toObject().value("member_sha256")}, {"operations", payload.value("operations")}};
    auto parsed = Services::ParseMapCustomSettingsEdit(QJsonDocument(edit).toJson(QJsonDocument::Compact).toStdString());
    if (!parsed.Succeeded()) return Failure(parsed);
    if (!host.validation) return Failure("SETTINGS_VALIDATION_UNAVAILABLE", "宿主没有实际物编验证入口。");
    auto validation = host.validation(); if (!validation.Succeeded()) return Failure(validation);
    auto plan = Services::MapCustomSettingsEditing::Prepare(context.value->document, *parsed.value, *validation.value);
    if (!plan.Succeeded()) return Failure(plan);
    auto result = Body(Services::MapCustomSettingsEditReceipt(*plan.value)); result.insert("ok", true); result.insert("dry_run", payload.value("dry_run"));
    if (payload.value("dry_run").toBool() || !plan.value->HasChanges()) return Stamp(result, identity);
    if (!host.apply) return Failure("SETTINGS_COMMIT_HOST_UNAVAILABLE", "当前宿主只提供预览；请使用独立候选保存。");
    bool checked = false;
    const auto applied = host.apply(*plan.value, *validation.value, [&](const auto&, const auto&, const auto& receipt, const auto&) {
        checked = true; result.insert("session_identity", QString::number(receipt.SessionIdentity())); result.insert("command_identity", QString::number(receipt.CommandIdentity()));
        result.insert("dirty", true); return Status::Ok();
    });
    if (!applied.Succeeded()) return Failure(applied);
    if (!checked) return Failure("SETTINGS_HOST_PROTOCOL", "宿主未执行发布前回执检查。");
    return Stamp(result, host.identity());
}
void RegisterMapCustomSettingsTools(McpServer& server) {
    for (auto definition : MapCustomSettingsToolDefinitions()) {
        const auto name = definition.name;
        definition.handler = [&server, name](const QJsonObject& arguments) -> QJsonValue {
            QJsonObject error; auto* workspace = RequireWorkspace(server, error); if (!workspace) return error;
            MapCustomSettingsToolHost host;
            host.identity = [&server, workspace] { return QJsonObject{{"project_identity", QString::fromStdString(server.DocumentId())}, {"revision", QString::fromStdString(workspace->ObjectRevision())}}; };
            host.context = [&server, workspace]() -> Result<MapDataToolContext> { auto current = workspace->MapDataContext(server.DocumentId(), server.DocumentGeneration()); if (!current.Succeeded()) return {{}, current.diagnostics};
                MapDataToolContext value; value.original = current.value->original; value.document = current.value->document; value.binding = current.value->binding; return Result<MapDataToolContext>::Success(std::move(value)); };
            host.validation = [] { return Result<Services::MapCustomSettingsValidation>::Failure("Map.CustomSettings", "SETTINGS_DIRECTORY_WRITE_UNSUPPORTED", "独立目录事务暂不接自定义表写入；请对原生 MPQ 使用 save_map_edit_candidate。"); };
            return InvokeMapCustomSettingsTool(name, arguments, host);
        };
        server.RegisterTool(std::move(definition));
    }
    ToolDefinition query; query.name = "query_map_custom_settings"; query.permission = ToolPermission::Query;
    query.description = "从独立 MPQ 只读查询科技及随机表；源 SHA 必填。先 summary，后续分页回传 settings_binding。不会读取或执行地图脚本。";
    auto properties = QueryProperties(); properties.insert("source_map_path", String()); properties.insert("expected_source_sha256", String());
    query.inputSchema = Object(properties, {"source_map_path", "expected_source_sha256", "section"}); query.maxResponseBytes = 1024 * 1024 + 4096;
    query.handler = [](const QJsonObject& arguments) -> QJsonValue {
        const auto path = ParseAbsolutePath(arguments, "source_map_path"); if (!path.Succeeded()) return Failure(path);
        auto payload = arguments; payload.remove("source_map_path"); payload.remove("expected_source_sha256");
        auto result = Services::QueryArchiveMapCustomSettings(*path.value, arguments.value("expected_source_sha256").toString().toStdString(), QJsonDocument(payload).toJson(QJsonDocument::Compact).toStdString());
        if (!result.Succeeded()) return Failure(result); auto body = Body(*result.value); body.insert("ok", true); return body;
    };
    server.RegisterTool(std::move(query));
}
} // namespace War3Map::Mcp
