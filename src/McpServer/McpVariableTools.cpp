#include "McpVariableTools.h"
#include "McpToolInternal.h"
#include "McpTriggerGraphJson.h"

#include <QJsonDocument>

#include <algorithm>
#include <initializer_list>
#include <stdexcept>
#include <utility>

namespace War3Map::Mcp {
namespace {
using namespace Trigger;
constexpr std::size_t kBodyBytes = 256U * 1024U;
constexpr std::size_t kIdentityBytes = 4096;
constexpr std::size_t kRequestBytes = 4U * 1024U * 1024U;
constexpr const char* kCreateKey = "variable";

class VariableJsonError final : public std::runtime_error {
public:
    VariableJsonError(const char* code, std::string message) : std::runtime_error(std::move(message)), code(code) {}
    const char* code;
};

QJsonObject StringSchema() { return {{"type", "string"}, {"maxLength", 65536}}; }
QJsonObject IdSchema() { return {{"type", "string"}, {"pattern", "^[1-9][0-9]*$"}, {"maxLength", 20}}; }
QJsonObject VariableProperties() {
    return {{"type_name", StringSchema()}, {"is_array", QJsonObject{{"type", "boolean"}}},
        {"array_size", QJsonObject{{"type", "integer"}, {"minimum", 1}, {"maximum", 8192}}},
        {"initialized", QJsonObject{{"type", "boolean"}}}, {"initial_value", StringSchema()}};
}
void Keys(const QJsonObject& object, const QJsonObject& properties, const QJsonArray& required = {}) {
    for (const auto& key : required) if (!object.contains(key.toString()))
        throw VariableJsonError("VARIABLE_ARGUMENT_REQUIRED", "缺少变量参数：" + key.toString().toStdString());
    for (auto it = object.begin(); it != object.end(); ++it) if (!properties.contains(it.key()))
        throw VariableJsonError("VARIABLE_ARGUMENT_UNKNOWN", "未知变量参数：" + it.key().toStdString());
}
std::string String(const QJsonObject& object, const char* key) {
    const auto value = object.value(QLatin1String(key));
    if (!value.isString() || value.toString().contains(QChar::Null))
        throw VariableJsonError("VARIABLE_ARGUMENT_TYPE", std::string(key) + " 必须是无 NUL 的字符串。");
    const auto bytes = value.toString().toUtf8();
    if (bytes.size() > 65536) throw VariableJsonError("VARIABLE_ARGUMENT_BUDGET", "变量文本最多 65536 UTF-8 字节。");
    return bytes.toStdString();
}
bool Boolean(const QJsonObject& object, const char* key) {
    if (!object.value(QLatin1String(key)).isBool())
        throw VariableJsonError("VARIABLE_ARGUMENT_TYPE", std::string(key) + " 必须是布尔值。");
    return object.value(QLatin1String(key)).toBool();
}
std::int32_t Integer(const QJsonObject& object, const char* key) {
    const auto value = ParseInt32(object, key);
    if (!value.Succeeded()) throw VariableJsonError("VARIABLE_ARGUMENT_TYPE", std::string(key) + " 必须是 int32 整数。");
    return *value.value;
}
StableId Id(const QJsonObject& object) {
    if (!object.value("id").isString()) throw VariableJsonError("VARIABLE_ID_INVALID", "变量 ID 必须是非零规范十进制字符串。");
    const auto id = ParseUnsignedId(object, "id");
    if (!id.Succeeded() || std::to_string(*id.value) != object.value("id").toString().toStdString())
        throw VariableJsonError("VARIABLE_ID_INVALID", "变量 ID 无效、溢出或不是规范十进制字符串。");
    return *id.value;
}
const VariableRecord* FindVariable(const TriggerDocument& document, StableId id) {
    const VariableRecord* found = nullptr;
    for (const auto& variable : document.variables) if (variable.id == id) {
        if (found) throw VariableJsonError("MCP_TRIGGER_GRAPH_INVALID", "变量 ID 在当前文档中有歧义。");
        found = &variable;
    }
    return found;
}
QJsonObject Identity(const VariableToolHost& host) {
    const auto identity = host.identity();
    QJsonObject result;
    for (const auto* key : {"project_identity", "revision", "transaction_id", "transaction_revision", "transaction_active",
        "document_generation", "scope", "content_scope"}) if (identity.contains(key)) result.insert(key, identity.value(key));
    if (!result.value("project_identity").isString() || !result.value("revision").isString()
        || result.value("project_identity").toString().isEmpty() || result.value("revision").toString().isEmpty()
        || static_cast<std::size_t>(QJsonDocument(result).toJson(QJsonDocument::Compact).size()) > kIdentityBytes)
        throw VariableJsonError("VARIABLE_HOST_IDENTITY", "变量宿主必须提供有界的真实工程身份和修订。");
    return result;
}
bool SameIdentity(const QJsonObject& first, const QJsonObject& second) {
    return first.value("project_identity") == second.value("project_identity") && first.value("revision") == second.value("revision");
}
QJsonObject Stamp(QJsonObject result, const QJsonObject& identity) {
    for (auto it = identity.begin(); it != identity.end(); ++it) result.insert(it.key(), it.value());
    return result;
}
Status BodyBudget(const QJsonObject& body) {
    // 最多另加 4 KiB 可信身份；最终 RPC 还有二次转义和已前置限额的 request id。
    if (static_cast<std::size_t>(QJsonDocument(body).toJson(QJsonDocument::Compact).size()) > kBodyBytes - kIdentityBytes)
        return Status::Failure("MCP.Variable", "VARIABLE_RESPONSE_BUDGET", "完整变量回执超过 256 KiB 预算，未发布候选。");
    return Status::Ok();
}
Status RecordBudget(const VariableRecord* record) {
    if (!record) return Status::Ok();
    std::size_t bytes = 0;
    constexpr auto limit = kBodyBytes - kIdentityBytes;
    for (const auto* field : {&record->name, &record->typeName, &record->value}) {
        // 先看真实长度下界，避免为明显超额的历史 RawText 构造巨大 hex/JSON。
        if (field->text.size() > limit - bytes)
            return Status::Failure("MCP.Variable", "VARIABLE_RESPONSE_BUDGET", "完整变量文本超出回执预算，未发布候选。");
        bytes += field->text.size();
        if (field->originalBytes.size() > (limit - bytes) / 2)
            return Status::Failure("MCP.Variable", "VARIABLE_RESPONSE_BUDGET", "完整变量原字节超出回执预算，未发布候选。");
        bytes += 2 * field->originalBytes.size();
    }
    return Status::Ok();
}
GraphParentExpectation Parent(const TriggerDocument& document) {
    GraphParentExpectation parent; parent.kind = GraphParentKind::Variables;
    parent.orderedChildren.reserve(document.variables.size());
    for (const auto& variable : document.variables) parent.orderedChildren.push_back({GraphRecordKind::Variable, variable.id, {}});
    return parent;
}
QJsonArray RenameJson(const TriggerGraphEditReceipt& receipt) {
    QJsonArray renames;
    for (const auto& rename : receipt.renames) {
        QJsonArray ids;
        for (const auto id : rename.changedArgumentIds) ids.append(QString::number(id));
        renames.append(QJsonObject{{"id", QString::number(rename.declarationId)},
            {"old_name", QString::fromStdString(rename.oldName)}, {"new_name", QString::fromStdString(rename.newName)},
            {"old_symbol", QString::fromStdString(rename.oldSymbol)}, {"new_symbol", QString::fromStdString(rename.newSymbol)},
            {"changed_argument_ids", ids}, {"changed_argument_count", QString::number(rename.changedArgumentIds.size())},
            {"code_audit_count", QString::number(rename.externallyAuditedCode.size())},
            {"code_coverage", rename.codeCoverage == AtomicRenameCodeCoverage::NoOpaqueCode
                ? "no-opaque-code" : "caller-asserted-not-runtime-proven"}, {"external_dependencies_verified", false}});
    }
    return renames;
}
} // namespace

std::vector<ToolDefinition> VariableToolDefinitions() {
    QJsonObject identity{{"project_identity", QJsonObject{{"type", "string"}, {"maxLength", 4096}}},
        {"expected_revision", QJsonObject{{"type", "string"}, {"maxLength", 4096}}}};
    std::vector<ToolDefinition> result;
    const auto add = [&](const char* name, const char* description, QJsonObject properties, QJsonArray required, ToolPermission permission) {
        ToolDefinition tool; tool.name = QString::fromUtf8(name); tool.description = QString::fromUtf8(description);
        tool.inputSchema = BasicSchema(std::move(properties), std::move(required)); tool.permission = permission;
        tool.maxResponseBytes = 1024U * 1024U; tool.maxRequestIdBytes = 4096;
        result.push_back(std::move(tool));
    };
    auto list = identity;
    list.insert("page", BasicSchema({{"offset", QJsonObject{{"type", "integer"}, {"minimum", 0}}},
        {"limit", QJsonObject{{"type", "integer"}, {"minimum", 1}, {"maximum", 128}}}}));
    add("variables_list", "按文档顺序分页读取变量完整记录；page.limit=1..128，默认64。返回真实身份、修订和图 binding，不保存。", list, {}, ToolPermission::Query);
    auto get = identity; get.insert("id", IdSchema());
    add("variables_get", "以非零规范十进制 StableId 精确读取一个变量；缺失或歧义拒绝，不按名称或行号定位。", get, {"id"}, ToolPermission::Query);
    auto create = identity;
    const auto fields = VariableProperties();
    for (auto it = fields.begin(); it != fields.end(); ++it) create.insert(it.key(), it.value());
    create.insert("name", StringSchema());
    add("variables_create", "创建变量并进入原撤销/事务。默认非数组、长度1、未初始化、空初值；initial_value是WTG文本，initialized=true时须为类型可验证的字面值、真实预设或null，不执行表达式。", create,
        {"project_identity", "expected_revision", "name", "type_name"}, ToolPermission::Edit);
    auto update = identity; update.insert("id", IdSchema());
    update.insert("changes", BasicSchema(fields));
    add("variables_update", "原子修改实际提供的类型、数组、长度、初始化flag或初值；changes不能为空，未提供值保留，不自动清空初值或修正长度。改名使用variables_rename。", update,
        {"project_identity", "expected_revision", "id", "changes"}, ToolPermission::Edit);
    auto rename = identity; rename.insert("id", IdSchema()); rename.insert("name", StringSchema());
    rename.insert("code_audits", QJsonObject{{"type", "array"}, {"maxItems", 16384}, {"items", GraphRenameCodeAuditSchema()}});
    add("variables_rename", "使用原RawText原子改名并更新明确typed引用。不透明代码默认拒绝；code_audits仅接受既有逐块SHA/编码/符号绑定审计，不能声称外部依赖或运行已验证。", rename,
        {"project_identity", "expected_revision", "id", "name"}, ToolPermission::Edit);
    auto remove = identity; remove.insert("id", IdSchema());
    add("variables_delete", "按精确StableId删除变量；typed引用或可能引用的不透明代码按原领域门禁拒绝，不自动保存。", remove,
        {"project_identity", "expected_revision", "id"}, ToolPermission::Edit);
    return result;
}

QJsonObject InvokeVariableTool(const QString& name, const QJsonObject& arguments, const VariableToolHost& host) {
    try {
        if (!host.identity || !host.context) return Failure("VARIABLE_HOST_UNAVAILABLE", "变量宿主上下文未接通。");
        const auto definitions = VariableToolDefinitions();
        const auto definition = std::find_if(definitions.begin(), definitions.end(), [&](const auto& tool) { return tool.name == name; });
        if (definition == definitions.end()) return Failure("VARIABLE_OPERATION_UNSUPPORTED", "没有此变量工具。");
        if (static_cast<std::size_t>(QJsonDocument(arguments).toJson(QJsonDocument::Compact).size()) > kRequestBytes)
            return Failure("VARIABLE_REQUEST_BUDGET", "变量请求超过 4 MiB 预算。");
        Keys(arguments, definition->inputSchema.value("properties").toObject(), definition->inputSchema.value("required").toArray());
        const bool write = definition->permission == ToolPermission::Edit;
        const auto identity = Identity(host);
        if (write || arguments.contains("project_identity") || arguments.contains("expected_revision")) {
            if (!arguments.value("project_identity").isString() || !arguments.value("expected_revision").isString()
                || arguments.value("project_identity") != identity.value("project_identity")
                || arguments.value("expected_revision") != identity.value("revision"))
                return Failure("VARIABLE_REVISION_CONFLICT", "工程或变量修订已变化；重新查询后再操作。");
        }
        const auto context = host.context();
        if (!context.Succeeded()) return Failure(context);
        if (!context.value || !context.value->document || !context.value->catalog)
            return Failure("VARIABLE_HOST_UNAVAILABLE", "当前地图未加载完整触发器文档与真实类型目录。");
        const auto& document = *context.value->document;
        const auto& catalog = *context.value->catalog;
        const auto* mapObjects = context.value->mapObjects.get();
        if (!write) {
            QJsonObject request{{"table", "variables"}};
            int start = 0, limit = 64;
            StableId id = 0;
            if (name == "variables_get") {
                id = Id(arguments); request.insert("variable_ids", QJsonArray{QString::number(id)});
            } else {
                if (arguments.contains("page")) {
                    if (!arguments.value("page").isObject()) throw VariableJsonError("VARIABLE_ARGUMENT_TYPE", "page 必须是对象。");
                    const auto page = arguments.value("page").toObject(); Keys(page, QJsonObject{{"offset", QJsonValue{}}, {"limit", QJsonValue{}}});
                    if (page.contains("offset")) start = Integer(page, "offset");
                    if (page.contains("limit")) limit = Integer(page, "limit");
                }
                if (start < 0 || limit < 1 || limit > 128) throw VariableJsonError("VARIABLE_PAGE_BUDGET", "offset>=0，limit=1..128。");
                request.insert("start", start); request.insert("limit", limit);
            }
            if (id) {
                const auto budget = RecordBudget(FindVariable(document, id)); if (!budget.Succeeded()) return Failure(budget);
            } else {
                const auto end = std::min(document.variables.size(), static_cast<std::size_t>(start) + static_cast<std::size_t>(limit));
                for (auto index = static_cast<std::size_t>(start); index < end; ++index) {
                    const auto budget = RecordBudget(&document.variables[index]); if (!budget.Succeeded()) return Failure(budget);
                }
            }
            auto result = QueryGraphDocument(document, catalog, mapObjects, request, context.value->nameResolver);
            if (!result.value("ok").toBool()) return result;
            if (id) {
                const auto rows = result.value("rows").toArray();
                if (rows.size() != 1) return Failure("MCP_TRIGGER_GRAPH_INVALID", "精确变量查询没有唯一记录。");
                result.insert("variable", rows.first()); result.insert("id", QString::number(id));
                result.remove("rows");
            } else {
                result.insert("limit", limit); result.insert("truncated", !result.value("eof").toBool());
            }
            const auto budget = BodyBudget(result); if (!budget.Succeeded()) return Failure(budget);
            const auto current = Identity(host);
            if (!SameIdentity(identity, current)) return Failure("VARIABLE_REVISION_CONFLICT", "查询期间工程修订变化，未返回混合记录。");
            return Stamp(std::move(result), current);
        }
        if (!host.apply) return Failure("VARIABLE_OPERATION_UNSUPPORTED", "当前宿主没有变量编辑接缝。");
        const auto binding = CaptureTriggerGraphBinding(document, catalog, mapObjects);
        if (!binding.Succeeded()) return Failure(binding);
        TriggerGraphEditBatch batch; batch.expected = *binding.value;
        StableId subject = name == "variables_create" ? 0 : Id(arguments);
        const auto* variable = subject ? FindVariable(document, subject) : nullptr;
        if (subject && !variable) return Failure("MCP_TRIGGER_GRAPH_VARIABLE_MISSING", "变量 ID 已不存在；重新查询。");
        if (name == "variables_create") {
            GraphVariableValue value; value.name = String(arguments, "name"); value.typeName = String(arguments, "type_name");
            if (arguments.contains("is_array")) value.isArray = Boolean(arguments, "is_array");
            if (arguments.contains("array_size")) value.arraySize = Integer(arguments, "array_size");
            if (arguments.contains("initialized")) value.initialized = Boolean(arguments, "initialized");
            if (arguments.contains("initial_value")) value.initialValue = String(arguments, "initial_value");
            GraphCreate create; create.clientKey = kCreateKey; create.value = std::move(value); create.parent = Parent(document);
            batch.operations.emplace_back(std::move(create));
        } else if (name == "variables_update") {
            if (!arguments.value("changes").isObject()) throw VariableJsonError("VARIABLE_ARGUMENT_TYPE", "changes 必须是对象。");
            const auto changes = arguments.value("changes").toObject(); Keys(changes, VariableProperties());
            if (changes.isEmpty()) throw VariableJsonError("VARIABLE_CHANGES_EMPTY", "changes 至少提供一个字段。");
            const GraphRecordRef target{GraphRecordKind::Variable, subject, {}};
            const auto set = [&](const char* key, GraphField field, GraphFieldValue expected, GraphFieldValue value) {
                if (changes.contains(key)) batch.operations.emplace_back(GraphSet{target, field, std::move(expected), std::move(value)});
            };
            if (changes.contains("type_name")) set("type_name", GraphField::VariableType, variable->typeName.text, String(changes, "type_name"));
            if (changes.contains("is_array")) set("is_array", GraphField::VariableArray, variable->isArray != 0, Boolean(changes, "is_array"));
            if (changes.contains("array_size")) set("array_size", GraphField::VariableArraySize, variable->size, Integer(changes, "array_size"));
            if (changes.contains("initialized")) set("initialized", GraphField::VariableInitialized, variable->hasDefault != 0, Boolean(changes, "initialized"));
            if (changes.contains("initial_value")) set("initial_value", GraphField::VariableInitialValue, variable->value.text, String(changes, "initial_value"));
        } else if (name == "variables_rename") {
            GraphRename rename; rename.target = {GraphRecordKind::Variable, subject, {}};
            rename.expectedName = variable->name; rename.name = String(arguments, "name");
            if (arguments.contains("code_audits")) {
                if (!arguments.value("code_audits").isArray()) throw VariableJsonError("VARIABLE_ARGUMENT_TYPE", "code_audits 必须是逐块审计数组。");
                const auto audits = DecodeGraphRenameCodeAudits(arguments.value("code_audits").toArray());
                if (!audits.Succeeded()) return Failure(audits);
                rename.codeAudits = *audits.value;
            }
            batch.operations.emplace_back(std::move(rename));
        } else {
            batch.operations.emplace_back(GraphRemove{{GraphRecordKind::Variable, subject, {}}, variable->name.text, Parent(document)});
        }
        if (!SameIdentity(identity, Identity(host))) return Failure("VARIABLE_REVISION_CONFLICT", "变量计划期间修订变化，未执行候选。");
        QJsonObject prepared;
        bool previewed = false;
        const auto validate = [&](const TriggerDocument& before, const TriggerDocument& after,
            const TriggerGraphEditReceipt& receipt, const QJsonObject& changes) -> Status {
            if (previewed) return Status::Failure("MCP.Variable", "VARIABLE_HOST_PROTOCOL", "宿主重复调用变量发布前门禁。");
            previewed = true;
            if (!SameIdentity(identity, Identity(host))) return Status::Failure("MCP.Variable", "VARIABLE_REVISION_CONFLICT", "发布候选前修订已变化。");
            if (!subject) {
                const auto found = receipt.created.find(kCreateKey);
                if (found == receipt.created.end() || found->second.kind != GraphRecordKind::Variable || !found->second.id)
                    return Status::Failure("MCP.Variable", "VARIABLE_HOST_PROTOCOL", "变量创建回执缺少真实 StableId。");
                subject = found->second.id;
            }
            const auto* oldRecord = FindVariable(before, subject);
            const auto* newRecord = FindVariable(after, subject);
            const auto oldBudget = RecordBudget(oldRecord); if (!oldBudget.Succeeded()) return oldBudget;
            const auto newBudget = RecordBudget(newRecord); if (!newBudget.Succeeded()) return newBudget;
            for (const auto& rename : receipt.renames) if (rename.changedArgumentIds.size() > (kBodyBytes - kIdentityBytes) / 3)
                return Status::Failure("MCP.Variable", "VARIABLE_RESPONSE_BUDGET", "完整改名引用 ID 超出回执预算，未发布候选。");
            const QJsonValue oldRow = oldRecord ? QJsonValue(GraphVariableRowJson(*oldRecord)) : QJsonValue(QJsonValue::Null);
            const QJsonValue newRow = newRecord ? QJsonValue(GraphVariableRowJson(*newRecord)) : QJsonValue(QJsonValue::Null);
            const bool changed = receipt.before.currentWtgSha256 != receipt.after.currentWtgSha256
                || receipt.before.currentWctSha256 != receipt.after.currentWctSha256;
            auto panel = changes;
            if (panel.isEmpty()) panel = {{"items", QJsonArray{QJsonObject{{"kind", "variable"}, {"object_id", QString::number(subject)},
                {"before", oldRow}, {"after", newRow}}}}, {"changed_records", changed ? 1 : 0}};
            prepared = {{"ok", true}, {"id", QString::number(subject)}, {"changed", changed}, {"before", oldRow}, {"after", newRow},
                {"before_binding", GraphBindingJson(receipt.before)}, {"binding", GraphBindingJson(receipt.after)},
                {"applied_operations", QString::number(receipt.appliedOperations)}, {"renames", RenameJson(receipt)},
                {"external_dependencies_verified", false}, {"saved", false}, {"runtime_verified", false}, {"changes", panel}};
            return BodyBudget(prepared);
        };
        const auto applied = host.apply(batch, validate);
        if (!applied.Succeeded()) return Failure(applied);
        if (!previewed || prepared.isEmpty()) return Failure("VARIABLE_HOST_PROTOCOL", "宿主没有执行发布前回执门禁。");
        return Stamp(std::move(prepared), Identity(host));
    } catch (const VariableJsonError& failure) {
        return Failure(failure.code, failure.what());
    }
}

} // namespace War3Map::Mcp
