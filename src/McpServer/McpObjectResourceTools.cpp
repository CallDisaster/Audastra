#include "McpObjectResourceTools.h"
#include "McpToolInternal.h"
#include <QJsonDocument>
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace War3Map::Mcp {
namespace {
using namespace Services;
class ArgumentError : public std::runtime_error { public: using std::runtime_error::runtime_error; };
QJsonObject TextSchema(int maximum = 4096) { return {{"type", "string"}, {"maxLength", maximum}}; }
QJsonObject EnumSchema(QJsonArray values) { return {{"type", "string"}, {"enum", values}}; }
QJsonObject IntegerSchema(int maximum) { return {{"type", "integer"}, {"minimum", 1}, {"maximum", maximum}}; }
QJsonObject CursorSchema() {
    return BasicSchema({{"catalog_identity", TextSchema()}, {"query_identity", TextSchema()},
        {"next_index", QJsonObject{{"type", "string"}, {"pattern", "^(0|[1-9][0-9]*)$"}, {"maxLength", 20}}}},
        {"catalog_identity", "query_identity", "next_index"});
}
void Keys(const QJsonObject& object, const QJsonObject& schema) {
    for (const auto& key : schema.value("required").toArray())
        if (!object.contains(key.toString())) throw ArgumentError("缺少参数：" + key.toString().toStdString());
    const auto properties = schema.value("properties").toObject();
    for (auto i = object.begin(); i != object.end(); ++i)
        if (!properties.contains(i.key())) throw ArgumentError("未知参数：" + i.key().toStdString());
}
std::string Text(const QJsonObject& object, const char* key, std::size_t limit = 4096) {
    const auto value = object.value(QLatin1String(key));
    if (!value.isString() || value.toString().contains(QChar::Null)) throw ArgumentError(std::string(key) + "须为无NUL字符串。");
    const auto bytes = value.toString().toUtf8();
    if (QString::fromUtf8(bytes) != value.toString() || static_cast<std::size_t>(bytes.size()) > limit)
        throw ArgumentError(std::string(key) + "超过UTF-8预算或编码无效。");
    return bytes.toStdString();
}
std::size_t Number(const QJsonObject& object, const char* key, std::size_t defaultValue, std::size_t maximum) {
    if (!object.contains(key)) return defaultValue;
    const auto value = object.value(QLatin1String(key)); const auto number = value.toDouble(-1);
    if (!value.isDouble() || !std::isfinite(number) || number < 1 || number > maximum || std::floor(number) != number)
        throw ArgumentError(std::string(key) + "超出整数预算。");
    return static_cast<std::size_t>(number);
}
ObjectResourceKind Kind(std::string_view name) {
    for (auto kind : {ObjectResourceKind::Model, ObjectResourceKind::Icon, ObjectResourceKind::Texture,
        ObjectResourceKind::SoundFile, ObjectResourceKind::SoundLabel, ObjectResourceKind::UnitSoundSet})
        if (ObjectResourceKindName(kind) == name) return kind;
    throw ArgumentError("未知资源类型。");
}
ObjectResourceOrigin Origin(std::string_view name) {
    for (auto origin : {ObjectResourceOrigin::Object, ObjectResourceOrigin::Import, ObjectResourceOrigin::Game})
        if (ObjectResourceOriginName(origin) == name) return origin;
    throw ArgumentError("未知资源来源。");
}
ObjectData::ObjectType ObjectType(std::string_view name) {
    for (auto type : {ObjectData::ObjectType::Unit, ObjectData::ObjectType::Item, ObjectData::ObjectType::Ability,
        ObjectData::ObjectType::Buff, ObjectData::ObjectType::Destructable, ObjectData::ObjectType::Doodad, ObjectData::ObjectType::Upgrade})
        if (ObjectData::ToString(type) == name) return type;
    throw ArgumentError("未知物编类别。");
}
QJsonObject Identity(const ObjectResourceToolHost& host) {
    auto identity = host.identity();
    QJsonObject result;
    for (const auto* key : {"project_identity", "revision", "resource_revision"}) {
        const auto text = Text(identity, key);
        if (text.empty()) throw ArgumentError("宿主没有可信资源身份。");
        result.insert(QLatin1String(key), QString::fromStdString(text));
    }
    return result;
}
QJsonObject CursorJson(const ObjectResourceCursor& cursor) {
    return {{"catalog_identity", QString::fromStdString(cursor.catalogIdentity)},
        {"query_identity", QString::fromStdString(cursor.queryIdentity)}, {"next_index", QString::number(cursor.nextIndex)}};
}
QJsonObject Row(const ObjectResourceCandidate& row) {
    QJsonObject result{{"id", QString::fromStdString(row.id)}, {"title", QString::fromStdString(row.title)},
        {"value", QString::fromStdString(row.value)}, {"preview_path", QString::fromStdString(row.previewPath)},
        {"kind", QString::fromUtf8(ObjectResourceKindName(row.kind))}, {"origin", QString::fromUtf8(ObjectResourceOriginName(row.origin))},
        {"source_id", QString::fromStdString(row.sourceId)}, {"state", QString::fromStdString(row.state)},
        {"directory", row.directory}, {"source_verified", row.sourceVerified}, {"custom_object", row.customObject},
        {"field_id", QString::fromStdString(row.fieldId)}};
    if (row.object) result.insert("object", QJsonObject{{"type", QString::fromUtf8(ObjectData::ToString(row.object->type))},
        {"rawcode", QString::fromStdString(row.object->rawcode)}});
    return result;
}
QJsonObject Info(const ObjectResourceInfo& info) {
    QJsonArray sequences;
    for (const auto& sequence : info.sequences) sequences.append(QJsonObject{{"name", QString::fromStdString(sequence.name)},
        {"start_ms", static_cast<double>(sequence.startMs)}, {"end_ms", static_cast<double>(sequence.endMs)},
        {"loops", sequence.loops}, {"usable", sequence.usable}});
    return {{"ok", true}, {"value", QString::fromStdString(info.requestedValue)}, {"resource_path", QString::fromStdString(info.resourcePath)},
        {"kind", QString::fromUtf8(ObjectResourceKindName(info.kind))}, {"source_id", QString::fromStdString(info.source.sourceId)},
        {"source_path", QString::fromStdString(info.source.sourcePath)}, {"source_priority", info.source.priority},
        {"source_verified", true}, {"bytes", QString::number(info.bytes)},
        {"width", info.width ? QJsonValue(static_cast<double>(*info.width)) : QJsonValue(QJsonValue::Null)},
        {"height", info.height ? QJsonValue(static_cast<double>(*info.height)) : QJsonValue(QJsonValue::Null)},
        {"sequences", sequences}, {"diagnostics", DiagnosticsJson(info.diagnostics).value("diagnostics")}, {"payload_included", false}};
}
}
std::vector<ToolDefinition> ObjectResourceToolDefinitions() {
    auto properties = QJsonObject{{"project_identity", TextSchema()}, {"expected_revision", TextSchema()}, {"resource_revision", TextSchema()}};
    const QJsonArray required{"project_identity", "expected_revision", "resource_revision"};
    auto query = properties;
    query.insert("origin", EnumSchema({"object", "import", "game"}));
    query.insert("kind", EnumSchema({"model", "icon", "texture", "sound-file", "sound-label", "unit-sound-set"}));
    query.insert("object_type", TextSchema(32)); query.insert("field_id", TextSchema(128));
    query.insert("search", TextSchema(1024)); query.insert("directory", TextSchema());
    query.insert("limit", IntegerSchema(128)); query.insert("max_examined", IntegerSchema(8192)); query.insert("cursor", CursorSchema());
    auto inspect = properties;
    inspect.insert("kind", query.value("kind")); inspect.insert("value", TextSchema());
    auto queryRequired = required; queryRequired.append("origin");
    auto inspectRequired = required; inspectRequired.append("kind"); inspectRequired.append("value");
    std::vector<ToolDefinition> result{
        {"object_resource_state", "读取当前资源身份。后续查询与资源检查须带此身份；字段写入使用objects_set_fields。", BasicSchema()},
        {"object_resource_query", "查询物编字段资源候选，支持物体、导入、游戏文件与有界分页；候选来源未经实际读取确认。", BasicSchema(query, queryRequired)},
        {"object_resource_inspect", "读取实际覆盖来源、图片尺寸或模型动作；不返回文件正文、不修改字段。", BasicSchema(inspect, inspectRequired)}};
    for (auto& definition : result) { definition.permission = ToolPermission::Query; definition.maxResponseBytes = 256 * 1024; definition.maxRequestIdBytes = 256; }
    return result;
}
QJsonObject InvokeObjectResourceTool(const QString& name, const QJsonObject& arguments, const ObjectResourceToolHost& host) {
    try {
        const auto definitions = ObjectResourceToolDefinitions();
        const auto found = std::find_if(definitions.begin(), definitions.end(), [&](const auto& definition) { return definition.name == name; });
        if (found == definitions.end()) return Failure("OBJECT_RESOURCE_TOOL_UNKNOWN", "未知资源工具。");
        if (QJsonDocument(arguments).toJson(QJsonDocument::Compact).size() > 64 * 1024) throw ArgumentError("请求超过64KiB预算。");
        Keys(arguments, found->inputSchema);
        if (!host.identity) return Failure("OBJECT_RESOURCE_HOST_MISSING", "宿主没有资源接口。");
        const auto before = Identity(host);
        if (name == "object_resource_state") { auto body = before; body.insert("ok", true); body.insert("read_only", true); return body; }
        if (Text(arguments, "project_identity") != Text(before, "project_identity") ||
            Text(arguments, "expected_revision") != Text(before, "revision") ||
            Text(arguments, "resource_revision") != Text(before, "resource_revision"))
            return Failure("OBJECT_RESOURCE_STALE", "工程或资源修订已变化，请重新查询状态。");
        QJsonObject body;
        if (name == "object_resource_query") {
            if (!host.load) return Failure("OBJECT_RESOURCE_HOST_MISSING", "资源候选接口未接通。");
            ObjectResourceQuery query; query.expectedRevision = Text(before, "resource_revision");
            if (arguments.contains("object_type")) query.objectType = ObjectType(Text(arguments, "object_type", 32));
            if (arguments.contains("field_id")) {
                if (!query.objectType || !host.field) throw ArgumentError("按字段查询必须指定物编类别且宿主支持字段元数据。");
                auto field = host.field(*query.objectType, Text(arguments, "field_id", 128));
                if (!field.Succeeded()) return Failure(field); query.kind = field.value->kind;
                if (arguments.contains("kind") && Kind(Text(arguments, "kind")) != query.kind) throw ArgumentError("指定kind与实际字段元数据不一致。");
                body.insert("ordered_list", field.value->orderedList); body.insert("stores_path", field.value->storesPath);
            } else {
                if (!arguments.contains("kind")) throw ArgumentError("须指定kind或object_type与field_id。");
                query.kind = Kind(Text(arguments, "kind"));
            }
            if (arguments.contains("search")) query.search = Text(arguments, "search", 1024);
            if (arguments.contains("directory")) query.directory = Text(arguments, "directory");
            query.limit = Number(arguments, "limit", 64, 128); query.maxExamined = Number(arguments, "max_examined", 4096, 8192);
            std::optional<ObjectResourceCursor> cursor;
            if (arguments.contains("cursor")) {
                if (!arguments.value("cursor").isObject()) throw ArgumentError("cursor必须为对象。");
                const auto raw = arguments.value("cursor").toObject(); Keys(raw, CursorSchema());
                const auto index = ParseUnsignedId(raw, "next_index", true);
                if (!index.Succeeded() || !index.value || std::to_string(*index.value) != Text(raw, "next_index", 20) || *index.value > SIZE_MAX)
                    throw ArgumentError("游标索引不是规范size_t字符串。");
                cursor = ObjectResourceCursor{Text(raw, "catalog_identity"), Text(raw, "query_identity"), static_cast<std::size_t>(*index.value)};
            }
            auto catalog = host.load(Origin(Text(arguments, "origin")));
            if (!catalog.Succeeded()) return Failure(catalog);
            auto page = QueryObjectResources(**catalog.value, query, cursor);
            if (!page.Succeeded()) return Failure(page);
            QJsonArray rows; for (const auto& row : page.value->rows) rows.append(Row(row));
            body.insert("ok", true); body.insert("rows", rows); body.insert("complete", page.value->complete);
            body.insert("examined", static_cast<double>(page.value->examined)); body.insert("known_rows", QString::number(page.value->knownRows));
            body.insert("scan_budget_exhausted", page.value->scanBudgetExhausted);
            body.insert("cursor", page.value->next ? QJsonValue(CursorJson(*page.value->next)) : QJsonValue(QJsonValue::Null));
            body.insert("diagnostics", DiagnosticsJson(page.value->diagnostics).value("diagnostics"));
        } else {
            if (!host.inspect) return Failure("OBJECT_RESOURCE_INSPECT_UNAVAILABLE", "宿主没有可验证的资源读取接口。");
            auto info = host.inspect(Text(arguments, "value"), Kind(Text(arguments, "kind")));
            if (!info.Succeeded()) return Failure(info); body = Info(*info.value);
        }
        if (Identity(host) != before) return Failure("OBJECT_RESOURCE_HOST_CHANGED", "查询期间工程或资源来源已变化，结果已丢弃。");
        for (auto i = before.begin(); i != before.end(); ++i) body.insert(i.key(), i.value());
        body.insert("read_only", true);
        if (QJsonDocument(body).toJson(QJsonDocument::Compact).size() > 252 * 1024)
            return Failure("OBJECT_RESOURCE_RESPONSE_BUDGET", "完整资源信息超过252KiB预算，请缩小查询；未修改地图。");
        return body;
    } catch (const ArgumentError& error) { return Failure("OBJECT_RESOURCE_ARGUMENT_INVALID", error.what()); }
      catch (const std::exception& error) { return Failure("OBJECT_RESOURCE_QUERY_FAILED", error.what()); }
}
}
