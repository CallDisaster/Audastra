#include "McpProjectSearchTools.h"
#include "McpToolInternal.h"

#include <QJsonDocument>

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace War3Map::Mcp {
namespace {
using namespace Services;
constexpr std::size_t kRequestBytes = 16U * 1024U, kBodyBytes = 252U * 1024U;
class SearchJsonError final : public std::runtime_error {
public:
    SearchJsonError(const char* code, std::string message) : std::runtime_error(std::move(message)), code(code) {}
    const char* code;
};
QJsonObject TextSchema(int maximum = 512) { return {{"type", "string"}, {"maxLength", maximum}}; }
QJsonObject NumberSchema(int maximum) { return {{"type", "integer"}, {"minimum", 1}, {"maximum", maximum}}; }
void Keys(const QJsonObject& object, const QJsonObject& schema) {
    for (const auto key : schema.value("required").toArray()) if (!object.contains(key.toString()))
        throw SearchJsonError("PROJECT_SEARCH_ARGUMENT_REQUIRED", "缺少搜索参数：" + key.toString().toStdString());
    const auto allowed = schema.value("properties").toObject();
    for (auto it = object.begin(); it != object.end(); ++it) if (!allowed.contains(it.key()))
        throw SearchJsonError("PROJECT_SEARCH_ARGUMENT_UNKNOWN", "未知搜索参数：" + it.key().toStdString());
}
std::string Text(const QJsonObject& object, const char* key, std::size_t maximum = 512) {
    const auto raw = object.value(QLatin1String(key));
    if (!raw.isString() || raw.toString().contains(QChar::Null))
        throw SearchJsonError("PROJECT_SEARCH_ARGUMENT_TYPE", std::string(key) + "必须为无NUL字符串。");
    const auto bytes = raw.toString().toUtf8();
    if (QString::fromUtf8(bytes) != raw.toString() || static_cast<std::size_t>(bytes.size()) > maximum)
        throw SearchJsonError("PROJECT_SEARCH_ARGUMENT_BUDGET", std::string(key) + "不是有界无损UTF8字符串。");
    return bytes.toStdString();
}
std::size_t Number(const QJsonObject& object, const char* key, std::size_t maximum) {
    const auto value = ParseInt32(object, key);
    if (!value.Succeeded() || !value.value || *value.value < 1 || static_cast<std::size_t>(*value.value) > maximum)
        throw SearchJsonError("PROJECT_SEARCH_ARGUMENT_BUDGET", std::string(key) + "必须为预算范围内的真整数。");
    return static_cast<std::size_t>(*value.value);
}
QJsonObject CursorSchema() {
    return BasicSchema({{"context_revision", TextSchema()}, {"query", TextSchema(256)},
        {"window_sha256", QJsonObject{{"type", "string"}, {"pattern", "^[0-9a-f]{64}$"}, {"maxLength", 64}}},
        {"next_index", QJsonObject{{"type", "string"}, {"pattern", "^[1-9][0-9]*$"}, {"maxLength", 3}}}},
        {"context_revision", "query", "window_sha256", "next_index"});
}
ProjectSearchCursor ReadCursor(const QJsonObject& object) {
    Keys(object, CursorSchema());
    const auto next = ParseUnsignedId(object, "next_index");
    const auto canonical = Text(object, "next_index", 3);
    const auto sha = Text(object, "window_sha256", 64);
    if (!next.Succeeded() || !next.value || *next.value > 127 || canonical != std::to_string(*next.value)
        || !IsLowerHexSha256(sha))
        throw SearchJsonError("PROJECT_SEARCH_CURSOR_INVALID", "游标必须含规范SHA及窗口内十进制索引。");
    return {Text(object, "context_revision"), Text(object, "query", 256), sha, static_cast<std::size_t>(*next.value)};
}
QJsonObject CursorJson(const ProjectSearchCursor& cursor) {
    return {{"context_revision", QString::fromStdString(cursor.contextRevision)}, {"query", QString::fromStdString(cursor.queryUtf8)},
        {"window_sha256", QString::fromStdString(cursor.windowSha256)}, {"next_index", QString::number(cursor.nextIndex)}};
}
QJsonValue Id(const std::optional<std::uint64_t>& value) {
    return value ? QJsonValue(QString::number(*value)) : QJsonValue(QJsonValue::Null);
}
QJsonValue OptionalText(const std::optional<std::string>& value) {
    return value ? QJsonValue(QString::fromStdString(*value)) : QJsonValue(QJsonValue::Null);
}
std::string_view Category(ProjectSearchCategory category) {
    constexpr std::array<std::string_view, 11> keys{"trigger", "variable", "map_object", "standard_object", "sound", "imported_file",
        "unit_placement", "doodad_placement", "region", "camera", "code_symbol"};
    const auto index = static_cast<std::size_t>(category); return index < keys.size() ? keys[index] : "invalid";
}
std::string_view State(ProjectSearchState state) {
    switch (state) {
    case ProjectSearchState::Ready: return "ready";
    case ProjectSearchState::Partial: return "partial";
    case ProjectSearchState::Missing: return "missing";
    case ProjectSearchState::Invalid: return "invalid";
    case ProjectSearchState::Ambiguous: return "ambiguous";
    case ProjectSearchState::QueryTooShort: return "query_too_short";
    default: return "unavailable";
    }
}
QString String(std::string_view value) { return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size())); }
QJsonObject RowJson(const ProjectSearchRow& row) {
    QJsonObject body{{"category", String(Category(row.category))}, {"stable_id", QString::fromStdString(row.stableId)},
        {"title", QString::fromStdString(row.title)}, {"detail", QString::fromStdString(row.detail)},
        {"context_revision", QString::fromStdString(row.contextRevision)}, {"score", QString::number(row.score)},
        {"source_id", Id(row.sourceId)}, {"eca_id", Id(row.ecaId)}, {"argument_id", Id(row.argumentId)},
        {"raw_value", OptionalText(row.rawValue)}, {"source_index", Id(row.sourceIndex)},
        {"row_key", QString::fromStdString(row.rowKey)}, {"text_truncated", row.textTruncated},
        {"code_source", OptionalText(row.codeSource)}, {"code_line", QString::number(row.codeLine)},
        {"code_column", QString::number(row.codeColumn)}, {"code_binding", QString::fromStdString(row.codeBinding)},
        {"map_kind", row.mapKind ? QJsonValue(static_cast<int>(*row.mapKind)) : QJsonValue(QJsonValue::Null)},
        {"object", QJsonValue(QJsonValue::Null)}};
    if (row.object) {
        const auto raw = QByteArray::fromStdString(row.object->rawcode); const auto display = QString::fromUtf8(raw);
        body.insert("object", QJsonObject{{"type", String(ObjectData::ToString(row.object->type))},
            {"rawcode", display.toUtf8() == raw ? QJsonValue(display) : QJsonValue(QJsonValue::Null)},
            {"rawcode_hex", QString::fromLatin1(raw.toHex())}});
    }
    return body;
}
QJsonObject HostIdentity(const ProjectSearchToolHost& host) {
    const auto value = host.identity();
    const auto project = Text(value, "project_identity"), revision = Text(value, "context_revision");
    if (project.empty() || revision.empty()) throw SearchJsonError("PROJECT_SEARCH_HOST_IDENTITY", "搜索宿主没有当前工程身份及复合修订。");
    return {{"project_identity", QString::fromStdString(project)}, {"context_revision", QString::fromStdString(revision)}};
}
QJsonObject Bounded(QJsonObject body) {
    if (static_cast<std::size_t>(QJsonDocument(body).toJson(QJsonDocument::Compact).size()) > kBodyBytes)
        return Failure("PROJECT_SEARCH_RESPONSE_BUDGET", "搜索正文超过252KiB；请缩小页长或结果字节预算。未修改文档。");
    return body;
}
} // namespace

std::vector<ToolDefinition> ProjectSearchToolDefinitions() {
    ToolDefinition tool; tool.name = "project_search";
    tool.description = "只读当前工程全局搜索，与界面共用SearchProjectPage：物体名称/ID、触发器名称/参数文本、变量、声音、资源、摆放、区域/镜头及已生成的代码声明。UTF8模糊匹配；11类coverage分别报告unknown/partial及预算截断。分页只访问最多128条排序窗口，window_has_more提示缩小查询；不冒称无限全图索引。cursor绑定当前复合修订、原查询、所有预算、页长与实际窗口SHA。参数返回所属触发器/ECA/Argument稳定ID及有界原值；不导航、不读资源正文、不执行脚本、不编辑或保存。";
    tool.inputSchema = BasicSchema({{"project_identity", TextSchema()}, {"expected_context_revision", TextSchema()},
        {"query", TextSchema(256)}, {"page", BasicSchema({{"limit", NumberSchema(128)}, {"cursor", CursorSchema()}})},
        {"budget", BasicSchema({{"window_limit", NumberSchema(128)}, {"max_examined_per_category", NumberSchema(32768)},
            {"max_result_bytes", NumberSchema(256 * 1024)}, {"max_trigger_nodes", NumberSchema(65536)},
            {"max_trigger_text_bytes", NumberSchema(8 * 1024 * 1024)}})}});
    tool.permission = ToolPermission::Query; tool.maxResponseBytes = 256U * 1024U; tool.maxRequestIdBytes = 1024;
    return {std::move(tool)};
}
QJsonObject InvokeProjectSearchTool(const QString& name, const QJsonObject& arguments, const ProjectSearchToolHost& host) {
    try {
        const auto definitions = ProjectSearchToolDefinitions();
        if (name != definitions.front().name) return Failure("PROJECT_SEARCH_TOOL_UNSUPPORTED", "未注册该搜索工具。");
        if (!host.identity || !host.query) return Failure("PROJECT_SEARCH_HOST_UNAVAILABLE", "没有绑定当前工程搜索宿主。");
        if (static_cast<std::size_t>(QJsonDocument(arguments).toJson(QJsonDocument::Compact).size()) > kRequestBytes)
            return Failure("PROJECT_SEARCH_REQUEST_BUDGET", "搜索请求超过16KiB。");
        Keys(arguments, definitions.front().inputSchema);
        if (arguments.contains("project_identity") != arguments.contains("expected_context_revision"))
            return Failure("PROJECT_SEARCH_IDENTITY_REQUIRED", "project_identity及expected_context_revision必须同时提供或同时省略。");
        const auto identity = HostIdentity(host);
        const auto revision = identity.value("context_revision").toString().toStdString();
        if (arguments.contains("project_identity") && (String(Text(arguments, "project_identity")) != identity.value("project_identity").toString()
            || Text(arguments, "expected_context_revision") != revision))
            return Failure("PROJECT_SEARCH_STALE", "请求与当前工程搜索修订不符。");
        const auto query = arguments.contains("query") ? Text(arguments, "query", 256) : std::string{};
        ProjectSearchOptions options; options.limit = 128; std::size_t limit = 40;
        std::optional<ProjectSearchCursor> cursor;
        const auto properties = definitions.front().inputSchema.value("properties").toObject();
        if (arguments.contains("budget")) {
            if (!arguments.value("budget").isObject()) return Failure("PROJECT_SEARCH_ARGUMENT_TYPE", "budget必须为对象。");
            const auto budget = arguments.value("budget").toObject(); Keys(budget, properties.value("budget").toObject());
            if (budget.contains("window_limit")) options.limit = Number(budget, "window_limit", 128);
            if (budget.contains("max_examined_per_category")) options.maxExaminedPerCategory = Number(budget, "max_examined_per_category", 32768);
            if (budget.contains("max_result_bytes")) options.maxResultBytes = Number(budget, "max_result_bytes", 256 * 1024);
            if (budget.contains("max_trigger_nodes")) options.maxTriggerNodes = Number(budget, "max_trigger_nodes", 65536);
            if (budget.contains("max_trigger_text_bytes")) options.maxTriggerTextBytes = Number(budget, "max_trigger_text_bytes", 8 * 1024 * 1024);
        }
        if (arguments.contains("page")) {
            if (!arguments.value("page").isObject()) return Failure("PROJECT_SEARCH_ARGUMENT_TYPE", "page必须为对象。");
            const auto page = arguments.value("page").toObject(); Keys(page, properties.value("page").toObject());
            if (page.contains("limit")) limit = Number(page, "limit", 128);
            if (page.contains("cursor")) {
                if (!page.value("cursor").isObject()) return Failure("PROJECT_SEARCH_CURSOR_INVALID", "cursor必须为对象。");
                cursor = ReadCursor(page.value("cursor").toObject());
            }
        }
        const auto queried = host.query(revision, query, options, limit, cursor ? &*cursor : nullptr);
        if (!queried.Succeeded()) return Bounded(Failure(queried));
        if (!queried.value || queried.value->contextRevision != revision || queried.value->queryUtf8 != query
            || queried.value->rows.size() > limit || queried.value->totalWindowRows > options.limit
            || !IsLowerHexSha256(queried.value->windowSha256)
            || std::any_of(queried.value->rows.begin(), queried.value->rows.end(), [&](const auto& row) { return row.contextRevision != revision; }))
            return Failure("PROJECT_SEARCH_HOST_IDENTITY", "搜索宿主返回了未绑定当前修订的结果。");
        if (HostIdentity(host) != identity) return Failure("PROJECT_SEARCH_STALE", "搜索期间工程修订变化；请重新查询。");
        const auto& page = *queried.value;
        QJsonArray rows; for (const auto& row : page.rows) rows.append(RowJson(row));
        QJsonArray coverage;
        for (const auto& entry : page.coverage) coverage.append(QJsonObject{{"category", String(Category(entry.category))},
            {"state", String(State(entry.state))}, {"examined", QString::number(entry.examined)}, {"matches", QString::number(entry.matches)},
            {"total_rows", entry.totalRows ? QJsonValue(QString::number(*entry.totalRows)) : QJsonValue(QJsonValue::Null)},
            {"scan_truncated", entry.scanTruncated}, {"results_truncated", entry.resultsTruncated},
            {"graph_nodes_examined", QString::number(entry.graphNodesExamined)}, {"text_bytes_examined", QString::number(entry.textBytesExamined)}});
        QJsonObject body{{"ok", true}, {"schema_version", 1}, {"read_only", true}, {"project_identity", identity.value("project_identity")},
            {"context_revision", String(page.contextRevision)}, {"query", String(page.queryUtf8)},
            {"window_sha256", String(page.windowSha256)}, {"total_window_rows", QString::number(page.totalWindowRows)},
            {"coverage", coverage}, {"complete", page.complete}, {"window_has_more", page.windowHasMore},
            {"next_cursor", page.next ? QJsonValue(CursorJson(*page.next)) : QJsonValue(QJsonValue::Null)}, {"rows", rows}};
        if (!queried.diagnostics.empty()) body.insert("diagnostics", DiagnosticsJson(queried.diagnostics).value("diagnostics"));
        return Bounded(std::move(body));
    } catch (const SearchJsonError& error) { return Bounded(Failure(error.code, error.what())); }
    catch (const std::exception&) { return Failure("PROJECT_SEARCH_QUERY_FAILED", "搜索未完成；没有修改文档或保存地图。"); }
}
} // namespace War3Map::Mcp
