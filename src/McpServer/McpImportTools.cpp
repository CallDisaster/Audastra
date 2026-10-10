#include "McpImportTools.h"
#include "McpToolInternal.h"

#include <QJsonDocument>

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace War3Map::Mcp {
namespace {
using namespace Services;
constexpr std::size_t kRequestBytes = 64U * 1024U, kBodyBytes = 252U * 1024U;
class ImportJsonError final : public std::runtime_error {
public:
    ImportJsonError(const char* code, std::string message) : std::runtime_error(std::move(message)), code(code) {}
    const char* code;
};
QJsonObject TextSchema(int maximum = 4096) { return {{"type", "string"}, {"maxLength", maximum}}; }
QJsonObject ShaSchema() { return {{"type", "string"}, {"pattern", "^[0-9a-f]{64}$"}, {"maxLength", 64}}; }
QJsonObject IndexSchema() { return {{"type", "string"}, {"pattern", "^(0|[1-9][0-9]*)$"}, {"maxLength", 20}}; }
void Keys(const QJsonObject& object, const QJsonObject& schema) {
    for (const auto key : schema.value("required").toArray()) if (!object.contains(key.toString()))
        throw ImportJsonError("IMPORT_ARGUMENT_REQUIRED", "缺少导入参数：" + key.toString().toStdString());
    const auto allowed = schema.value("properties").toObject();
    for (auto it = object.begin(); it != object.end(); ++it) if (!allowed.contains(it.key()))
        throw ImportJsonError("IMPORT_ARGUMENT_UNKNOWN", "未知导入参数：" + it.key().toStdString());
}
std::string Text(const QJsonObject& object, const char* key, std::size_t maximum = 4096) {
    const auto raw = object.value(QLatin1String(key));
    if (!raw.isString() || raw.toString().contains(QChar::Null))
        throw ImportJsonError("IMPORT_ARGUMENT_TYPE", std::string(key) + "必须是无NUL字符串。");
    const auto bytes = raw.toString().toUtf8();
    if (QString::fromUtf8(bytes) != raw.toString() || static_cast<std::size_t>(bytes.size()) > maximum)
        throw ImportJsonError("IMPORT_ARGUMENT_BUDGET", std::string(key) + "不是有界无损UTF-8字符串。");
    return bytes.toStdString();
}
std::uint64_t Index(const QJsonObject& object, const char* key) {
    const auto parsed = ParseUnsignedId(object, key, true);
    if (!parsed.Succeeded() || !parsed.value || std::to_string(*parsed.value) != Text(object, key, 20))
        throw ImportJsonError("IMPORT_CURSOR_INVALID", "游标索引必须是规范uint64十进制字符串。");
    return *parsed.value;
}
QJsonValue OptionalText(const std::optional<std::string>& value) {
    return value ? QJsonValue(QString::fromStdString(*value)) : QJsonValue(QJsonValue::Null);
}
QJsonValue OptionalNumber(const std::optional<std::uint64_t>& value) {
    return value ? QJsonValue(QString::number(*value)) : QJsonValue(QJsonValue::Null);
}
QJsonObject BindingSchema() {
    return BasicSchema({{"project_identity", TextSchema()}, {"document_generation", IndexSchema()},
        {"revision", TextSchema()}, {"source_sha256", ShaSchema()}, {"asset_revision", TextSchema()},
        {"content_scope", TextSchema()}},
        {"project_identity", "document_generation", "revision", "source_sha256", "asset_revision", "content_scope"});
}
QJsonObject BindingJson(const ImportInventoryBinding& binding) {
    return {{"project_identity", QString::fromStdString(binding.documentId)},
        {"document_generation", QString::number(binding.generation)}, {"revision", QString::fromStdString(binding.revision)},
        {"source_sha256", QString::fromStdString(binding.sourceSha256)},
        {"asset_revision", QString::fromStdString(binding.assetRevision)}, {"content_scope", QString::fromStdString(binding.contentScope)}};
}
ImportInventoryBinding ReadBinding(const QJsonObject& object) {
    Keys(object, BindingSchema());
    ImportInventoryBinding binding{Text(object, "project_identity"), Index(object, "document_generation"),
        Text(object, "revision"), Text(object, "source_sha256", 64), Text(object, "asset_revision"), Text(object, "content_scope")};
    if (!IsLowerHexSha256(binding.sourceSha256)) throw ImportJsonError("IMPORT_CURSOR_INVALID", "游标来源SHA无效。");
    return binding;
}
QJsonObject CursorSchema() {
    return BasicSchema({{"binding", BindingSchema()}, {"inventory_sha256", ShaSchema()},
        {"query", TextSchema()}, {"next_index", IndexSchema()}}, {"binding", "inventory_sha256", "query", "next_index"});
}
QJsonObject CursorJson(const ImportInventoryCursor& cursor) {
    return {{"binding", BindingJson(cursor.binding)}, {"inventory_sha256", QString::fromStdString(cursor.inventorySha256)},
        {"query", QString::fromStdString(cursor.query)}, {"next_index", QString::number(cursor.nextIndex)}};
}
ImportInventoryCursor ReadCursor(const QJsonObject& object) {
    Keys(object, CursorSchema());
    if (!object.value("binding").isObject()) throw ImportJsonError("IMPORT_CURSOR_INVALID", "游标binding必须为对象。");
    return {ReadBinding(object.value("binding").toObject()), Text(object, "inventory_sha256", 64),
        Text(object, "query"), Index(object, "next_index")};
}
QJsonObject DescriptionJson(const ImportInventoryDescription& description) {
    return {{"ok", true}, {"schema_version", 1}, {"binding", BindingJson(description.binding)},
        {"inventory_sha256", QString::fromStdString(description.inventorySha256)},
        {"source_kind", QString::fromStdString(description.sourceKind)}, {"imp_state", QString::fromStdString(description.impState)},
        {"imp_source_path", OptionalText(description.impSourcePath)}, {"imp_sha256", OptionalText(description.impSha256)},
        {"imp_record_count", QString::number(description.impRecordCount)},
        {"physical_member_count", QString::number(description.physicalMemberCount)}, {"queued_count", QString::number(description.queuedCount)},
        {"excluded_reserved_input_rows", QString::number(description.excludedArchiveMetadataCount)},
        {"archive_internal_metadata_enumerated", false},
        {"coverage", description.hasOpaqueArchiveMembers ? "partial" : "named-source-complete"},
        {"has_opaque_archive_members", description.hasOpaqueArchiveMembers},
        {"includes_unsaved_changes", description.includesUnsavedChanges}, {"read_only", true}, {"payloads_opaque", true},
        {"text_available", false}, {"lower_layer_coverage", "unknown"}, {"metadata_scope", "source-member-bytes-and-current-path-overlay"}};
}
QJsonObject IdentityJson(const ArchiveExactMemberIdentity& member) {
    return {{"path", QString::fromStdString(member.name)}, {"state", member.present ? "present" : "missing"},
        {"bytes", OptionalNumber(member.byteSize)}, {"sha256", OptionalText(member.sha256)}};
}
QJsonObject RowJson(const ImportInventoryRow& row) {
    const auto raw = QByteArray::fromStdString(row.rawPath);
    const auto display = QString::fromUtf8(raw);
    QJsonArray candidates;
    for (const auto& member : row.candidates) candidates.append(IdentityJson(member));
    QJsonObject body{{"import_id", QString::fromStdString(row.importId)}, {"kind", QString::fromStdString(row.kind)},
        {"raw_path", display.toUtf8() == raw ? QJsonValue(display) : QJsonValue(QJsonValue::Null)},
        {"raw_path_hex", QString::fromLatin1(raw.toHex())},
        {"flags", row.flags ? QJsonValue(static_cast<int>(*row.flags)) : QJsonValue(QJsonValue::Null)},
        {"source_path", OptionalText(row.sourcePath)}, {"archive_path", OptionalText(row.archivePath)},
        {"source_layer", QString::fromStdString(row.sourceLayer)}, {"state", QString::fromStdString(row.state)},
        {"overlay_state", QString::fromStdString(row.overlayState)}, {"bytes", OptionalNumber(row.bytes)},
        {"sha256", OptionalText(row.sha256)}, {"reported_bytes", OptionalNumber(row.reportedBytes)},
        {"override", "unknown"}, {"payload_opaque", true}, {"candidates", candidates}};
    if (!row.diagnostics.empty()) body.insert("diagnostics", DiagnosticsJson(row.diagnostics).value("diagnostics"));
    return body;
}
QJsonObject HostIdentity(const ImportToolHost& host) {
    const auto raw = host.identity();
    const auto project = Text(raw, "project_identity"), revision = Text(raw, "revision");
    if (project.empty() || revision.empty()) throw ImportJsonError("IMPORT_HOST_IDENTITY", "导入宿主没有真实工程身份与修订。");
    return {{"project_identity", QString::fromStdString(project)}, {"revision", QString::fromStdString(revision)}};
}
QJsonObject Bounded(QJsonObject body) {
    if (static_cast<std::size_t>(QJsonDocument(body).toJson(QJsonDocument::Compact).size()) > kBodyBytes)
        return Failure("IMPORT_RESPONSE_BUDGET", "完整导入元数据回执超过252KiB正文预算，请缩小页长；未修改任何状态。");
    return body;
}
} // namespace

std::vector<ToolDefinition> ImportToolDefinitions() {
    auto properties = QJsonObject{{"project_identity", TextSchema()}, {"expected_revision", TextSchema()},
        {"expected_inventory_sha256", ShaSchema()}};
    auto listProperties = properties;
    listProperties.insert("query", TextSchema());
    listProperties.insert("page", BasicSchema({{"cursor", CursorSchema()},
        {"limit", QJsonObject{{"type", "integer"}, {"minimum", 1}, {"maximum", 128}}}}));
    properties.insert("import_id", TextSchema(256));
    ToolDefinition list;
    list.name = "imports_list";
    list.description = "只读导入元数据：分别列IMP原记录（保留flags/raw path与歧义）、物理命名成员、queued-unfrozen队列。query按原路径UTF-8字节区分大小写包含匹配，默认64/最多128行，每页谓词最多4096次；unknown总数为null，opaque覆盖为partial。cursor绑定文档/源SHA/IMP/资产修订及query。字节数与SHA只证明原source_path内容，路径覆盖单列。所有载荷不透明，不读正文、不编辑或保存；正文最多252KiB，身份散列最多256名/64MiB。";
    list.inputSchema = BasicSchema(listProperties); list.permission = ToolPermission::Query;
    list.maxResponseBytes = 256U * 1024U; list.maxRequestIdBytes = 1024;
    ToolDefinition get;
    get.name = "imports_get";
    get.description = "以imports_list返回的import_id与expected_inventory_sha256读取同一快照的单项元数据。返回原flags/raw path、唯一路径或双候选歧义、来源层、存在状态、bytes/SHA及未保存路径状态；pending字节为reported_bytes且SHA未知。所有导入载荷保持opaque，绝不返回text/data/Base64或按扩展名解析；不修改、删除或保存。";
    get.inputSchema = BasicSchema(properties, {"import_id", "expected_inventory_sha256"});
    get.permission = ToolPermission::Query; get.maxResponseBytes = 256U * 1024U; get.maxRequestIdBytes = 1024;
    return {std::move(list), std::move(get)};
}

QJsonObject InvokeImportTool(const QString& name, const QJsonObject& arguments, const ImportToolHost& host) {
    try {
        const auto definitions = ImportToolDefinitions();
        const auto definition = std::find_if(definitions.begin(), definitions.end(), [&](const auto& value) { return value.name == name; });
        if (definition == definitions.end()) return Failure("IMPORT_TOOL_UNSUPPORTED", "未注册该导入工具；本单元只开放list/get元数据。");
        if (!host.identity || !host.context) return Failure("IMPORT_HOST_UNAVAILABLE", "没有绑定导入只读宿主。");
        if (static_cast<std::size_t>(QJsonDocument(arguments).toJson(QJsonDocument::Compact).size()) > kRequestBytes)
            return Failure("IMPORT_REQUEST_BUDGET", "导入查询请求超过64KiB预算。");
        Keys(arguments, definition->inputSchema);
        if (arguments.contains("project_identity") != arguments.contains("expected_revision"))
            return Failure("IMPORT_IDENTITY_REQUIRED", "project_identity与expected_revision必须同时提供或同时省略。");
        const auto identity = HostIdentity(host);
        if (arguments.contains("project_identity") && (QString::fromStdString(Text(arguments, "project_identity")) != identity.value("project_identity").toString()
            || QString::fromStdString(Text(arguments, "expected_revision")) != identity.value("revision").toString()))
            return Failure("IMPORT_REVISION_CONFLICT", "请求与当前工程修订不符。");
        const auto captured = host.context();
        if (!captured.Succeeded()) return Bounded(Failure(captured));
        if (!captured.value || !*captured.value) return Failure("IMPORT_HOST_UNAVAILABLE", "导入宿主未返回快照。");
        const auto& snapshot = **captured.value;
        const auto& description = snapshot.Description();
        if (QString::fromStdString(description.binding.documentId) != identity.value("project_identity").toString()
            || QString::fromStdString(description.binding.revision) != identity.value("revision").toString())
            return Failure("IMPORT_HOST_IDENTITY", "导入快照与宿主身份不一致。");
        if (arguments.contains("expected_inventory_sha256")) {
            const auto sha = Text(arguments, "expected_inventory_sha256", 64);
            if (!IsLowerHexSha256(sha)) return Failure("IMPORT_ARGUMENT_SHA", "expected_inventory_sha256必须为规范SHA256。");
            if (sha != description.inventorySha256) return Failure("IMPORT_REVISION_CONFLICT", "导入来源/IMP/资产覆盖快照已变化。");
        }
        QJsonObject body;
        if (name == "imports_get") {
            const auto row = snapshot.Get(description.binding, Text(arguments, "expected_inventory_sha256", 64), Text(arguments, "import_id", 256));
            if (!row.Succeeded()) return Bounded(Failure(row));
            body = DescriptionJson(description); body.insert("row", RowJson(*row.value));
        } else {
            const auto query = arguments.contains("query") ? Text(arguments, "query") : std::string{};
            std::optional<ImportInventoryCursor> cursor;
            std::size_t limit = 64;
            if (arguments.contains("page")) {
                if (!arguments.value("page").isObject()) return Failure("IMPORT_ARGUMENT_TYPE", "page必须为对象。");
                const auto page = arguments.value("page").toObject();
                Keys(page, definition->inputSchema.value("properties").toObject().value("page").toObject());
                if (page.contains("limit")) {
                    const auto number = ParseInt32(page, "limit");
                    if (!number.Succeeded() || !number.value || *number.value < 1 || *number.value > 128)
                        return Failure("IMPORT_QUERY_BUDGET", "limit必须是真整数1..128。");
                    limit = static_cast<std::size_t>(*number.value);
                }
                if (page.contains("cursor")) {
                    if (!page.value("cursor").isObject()) return Failure("IMPORT_CURSOR_INVALID", "cursor必须为完整对象。");
                    cursor = ReadCursor(page.value("cursor").toObject());
                }
            }
            const auto page = snapshot.Query(description.binding, query, cursor, limit);
            if (!page.Succeeded()) return Bounded(Failure(page));
            body = DescriptionJson(page.value->description);
            QJsonArray rows; for (const auto& row : page.value->rows) rows.append(RowJson(row));
            body.insert("rows", rows); body.insert("known_row_count", QString::number(page.value->knownRowCount));
            body.insert("matched_total", OptionalNumber(page.value->matchedTotal));
            body.insert("examined", static_cast<int>(page.value->examined));
            body.insert("scan_budget_exhausted", page.value->scanBudgetExhausted);
            body.insert("truncated", page.value->next.has_value());
            body.insert("next_cursor", page.value->next ? QJsonValue(CursorJson(*page.value->next)) : QJsonValue(QJsonValue::Null));
        }
        const auto current = snapshot.VerifyCurrent(); if (!current.Succeeded()) return Bounded(Failure(current));
        if (HostIdentity(host) != identity) return Failure("IMPORT_REVISION_CONFLICT", "读取导入元数据期间宿主修订变化。");
        for (auto it = identity.begin(); it != identity.end(); ++it) body.insert(it.key(), it.value());
        return Bounded(std::move(body));
    } catch (const ImportJsonError& error) {
        return Bounded(Failure(error.code, error.what()));
    } catch (const std::exception&) {
        return Failure("IMPORT_QUERY_FAILED", "导入元数据查询未完成，未修改任何状态。");
    }
}

} // namespace War3Map::Mcp
