#include "McpObjectTools.h"
#include "McpToolInternal.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <type_traits>

namespace War3Map::Mcp {
namespace {
using namespace ObjectData;
using namespace Services;
QJsonObject String() { return {{"type", "string"}}; }
QJsonObject Id() { return {{"type", "string"}, {"minLength", 4}, {"maxLength", 4}, {"pattern", "^[ -~]{4}$"}}; }
QJsonObject Page() {
    return BasicSchema({{"offset", QJsonObject{{"type", "integer"}, {"minimum", 0}}},
        {"limit", QJsonObject{{"type", "integer"}, {"minimum", 1}, {"maximum", 128}}}});
}
std::optional<ObjectType> Kind(const QString& name) {
    for (const auto type : {ObjectType::Unit, ObjectType::Item, ObjectType::Ability, ObjectType::Buff,
        ObjectType::Destructable, ObjectType::Doodad, ObjectType::Upgrade})
        if (name == QString::fromUtf8(ToString(type))) return type;
    return {};
}
struct Pagination { int offset = 0; int limit = 64; };
Result<Pagination> DecodePage(const QJsonObject& arguments) {
    Pagination result;
    if (!arguments.contains("page")) return Result<Pagination>::Success(result);
    if (!arguments.value("page").isObject())
        return Result<Pagination>::Failure("MCP", "OBJECT_PAGE_INVALID", "page 必须是 offset/limit 对象。");
    const auto page = arguments.value("page").toObject();
    for (auto it = page.begin(); it != page.end(); ++it)
        if (it.key() != "offset" && it.key() != "limit")
            return Result<Pagination>::Failure("MCP", "OBJECT_PAGE_INVALID", "page 含未知字段。");
    for (const auto& key : {"offset", "limit"}) {
        if (!page.contains(key)) continue;
        const auto value = ParseInt32(page, key);
        if (!value.Succeeded()) { Result<Pagination> failed; failed.diagnostics = value.diagnostics; return failed; }
        if (std::string_view(key) == "offset") result.offset = *value.value;
        else result.limit = *value.value;
    }
    if (result.offset < 0 || result.limit < 1 || result.limit > 128)
        return Result<Pagination>::Failure("MCP", "OBJECT_PAGE_BUDGET", "offset>=0、limit=1..128。");
    return Result<Pagination>::Success(result);
}
Result<Value> DecodeValue(const QJsonValue& value) {
    if (value.isBool()) return Result<Value>::Success(Value{value.toBool()});
    if (value.isString()) {
        const auto text = value.toString().toUtf8();
        if (text.size() > 65536 || text.contains('\0'))
            return Result<Value>::Failure("MCP", "OBJECT_VALUE_BUDGET", "字符串最多 65536 UTF-8 字节，不能含 NUL。");
        return Result<Value>::Success(Value{text.toStdString()});
    }
    if (value.isDouble()) {
        const double number = value.toDouble();
        if (!std::isfinite(number) || std::abs(number) > static_cast<double>((std::numeric_limits<float>::max)()))
            return Result<Value>::Failure("MCP", "OBJECT_NUMBER_INVALID", "数值必须有限且在 float 范围内。");
        if (std::floor(number) == number && number >= (std::numeric_limits<std::int32_t>::min)() &&
            number <= (std::numeric_limits<std::int32_t>::max)())
            return Result<Value>::Success(Value{static_cast<std::int32_t>(number)});
        return Result<Value>::Success(Value{static_cast<float>(number)});
    }
    if (value.isArray()) {
        const auto array = value.toArray();
        if (array.size() > 256) return Result<Value>::Failure("MCP", "OBJECT_LIST_BUDGET", "列表最多 256 项。");
        RawcodeListValue result;
        for (const auto& item : array) {
            if (!item.isString() || item.toString().size() > 256)
                return Result<Value>::Failure("MCP", "OBJECT_LIST_ITEM_INVALID", "列表项必须是最多 256 字符的字符串。");
            result.values.push_back(item.toString().toStdString());
        }
        return Result<Value>::Success(Value{std::move(result)});
    }
    return Result<Value>::Failure("MCP", "OBJECT_VALUE_TYPE", "字段值只接受真实数值、布尔、字符串或有序字符串数组。");
}
QJsonObject FieldJson(const ObjectFieldRow& row) {
    const auto* field = row.definition;
    QJsonObject result{{"field", QString::fromStdString(row.key.fieldId)}, {"raw_key", QString::fromStdString(row.sourceKey)},
        {"display_name", field ? QString::fromStdString(field->displayName) : QString::fromStdString(row.sourceKey)},
        {"type", field ? QString::fromStdString(field->valueType) : QStringLiteral("unknown")},
        {"value", row.value ? ObjectValueJson(*row.value, field) : QJsonValue(QJsonValue::Null)},
        {"default_value", row.defaultValue ? ObjectValueJson(*row.defaultValue, field) : QJsonValue(QJsonValue::Null)},
        {"overridden", row.overridden}, {"editable", row.editable}, {"value_source", QString::fromStdString(row.valueSource)},
        {"reason", QString::fromStdString(row.reason)}};
    if (row.key.level && *row.key.level > 0) result.insert("level", static_cast<qint64>(*row.key.level));
    if (row.key.variation && *row.key.variation > 0) result.insert("variation", static_cast<qint64>(*row.key.variation));
    if (row.key.dataIndex) result.insert("data_index", static_cast<qint64>(*row.key.dataIndex));
    if (field) {
        result.insert("minimum", QString::fromStdString(field->minimum));
        result.insert("maximum", QString::fromStdString(field->maximum));
        result.insert("ordered_list", field->isList);
    }
    return result;
}
QJsonObject AuditJson(const ObjectDeletionAssessment& assessment) {
    QJsonArray reasons, definite, possible, coverage;
    for (const auto& row : assessment.reasons) {
        if (reasons.size() >= 128) break;
        reasons.append(QJsonObject{{"code", QString::fromStdString(row.code)},
        {"source", QString::fromStdString(row.source)}, {"message", QString::fromStdString(row.message)}});
    }
    const auto appendReferences = [](QJsonArray& output, const std::vector<IndexedObjectReference>& input, bool uncertain) {
        for (const auto& row : input) {
            if (output.size() >= 128) break;
            output.append(QJsonObject{{"source", QString::fromStdString(row.memberName)},
                {"location", QString::fromStdString(row.location)}, {"field", QString::fromStdString(row.fieldId)},
                {"possible", uncertain}});
        }
    };
    appendReferences(definite, assessment.objectReferences.definite, false);
    appendReferences(possible, assessment.objectReferences.possible, true);
    for (const auto& row : assessment.untypedGuiReferences) {
        if (possible.size() >= 128) break;
        possible.append(QJsonObject{
        {"owner", QString::fromStdString(row.owner)}, {"owner_id", QString::fromStdString(row.ownerId)},
        {"detail", QString::fromStdString(row.detail)}, {"possible", true}});
    }
    for (const auto& row : assessment.scriptHints) {
        if (possible.size() >= 128) break;
        possible.append(QJsonObject{{"source", QString::fromStdString(row.source)},
        {"line", static_cast<qint64>(row.line)}, {"byte_offset", QString::number(row.byteOffset)}, {"possible", true}});
    }
    // 全部审计诊断保留；引用细节和覆盖由同一服务取得，不能由协议声称已全覆盖。
    for (const auto& row : assessment.coverage) {
        if (coverage.size() >= 128) break;
        QString state;
        switch (row.state) {
        case ReferenceInputState::Missing: state = "missing"; break;
        case ReferenceInputState::Ready: state = "ready"; break;
        case ReferenceInputState::Partial: state = "partial"; break;
        case ReferenceInputState::Invalid: state = "invalid"; break;
        case ReferenceInputState::Ambiguous: state = "ambiguous"; break;
        case ReferenceInputState::Unreadable: state = "unreadable"; break;
        }
        coverage.append(QJsonObject{{"member", QString::fromStdString(row.memberName)}, {"state", state},
            {"detail", QString::fromStdString(row.detail)}});
    }
    const auto possibleCount = assessment.objectReferences.possible.size() + assessment.untypedGuiReferences.size() + assessment.scriptHintCount;
    return {{"reasons", reasons}, {"definite_references", definite}, {"possible_references", possible}, {"coverage", coverage},
        {"definite_count", QString::number(assessment.objectReferences.definite.size())},
        {"possible_count", QString::number(possibleCount)}, {"coverage_count", QString::number(assessment.coverage.size())},
        {"truncated", assessment.objectReferences.definite.size() > 128 || possibleCount > 128 || assessment.coverage.size() > 128},
        {"dynamic_value_count", QString::number(assessment.dynamicValueCount)}, {"complete", false}};
}
QJsonObject ChangesJson(const ObjectCommandPlan& plan, const ObjectCommandContext& context) {
    QJsonArray items;
    auto beforeContext = context; beforeContext.document = &plan.before;
    auto afterContext = context; afterContext.document = &plan.after;
    const auto beforeFields = ReadObjectFields(beforeContext, plan.subject.rawcode);
    const auto afterFields = ReadObjectFields(afterContext, plan.subject.rawcode);
    const auto valueAt = [&](const auto& fields, const ModificationKey& key, const std::optional<Value>& fallback) {
        if (fields.Succeeded()) for (const auto& row : *fields.value)
            if (row.key == key && row.value) return ObjectValueJson(*row.value, row.definition);
        return fallback ? ObjectValueJson(*fallback) : QJsonValue(QJsonValue::Null);
    };
    const auto objectAt = [&](const ObjectCommandContext& current, const auto& fields) -> QJsonValue {
        const auto* record = current.document ? current.document->FindObject(plan.subject) : nullptr;
        const auto* standard = current.standard ? current.standard->Find(plan.subject.type, plan.subject.rawcode) : nullptr;
        if (!record && !standard) return QJsonValue(QJsonValue::Null);
        QJsonArray values;
        if (fields.Succeeded()) for (const auto& field : *fields.value) {
            if (values.size() == 128) break;
            values.append(FieldJson(field));
        }
        return QJsonObject{{"id", QString::fromStdString(plan.subject.rawcode)},
            {"parent_id", record && record->parentRawcode ? QJsonValue(QString::fromStdString(*record->parentRawcode)) : QJsonValue(QJsonValue::Null)},
            {"fields", values}, {"total", fields.Succeeded() ? static_cast<qint64>(fields.value->size()) : 0},
            {"truncated", fields.Succeeded() && fields.value->size() > 128}};
    };
    for (const auto& change : plan.changes.changes) {
        const ObjectFieldDefinition* definition = nullptr;
        if (change.modification) {
            const auto match = ResolveObjectField(afterContext, change.object.rawcode, change.modification->fieldId,
                change.modification->level, change.modification->variation);
            if (match.Succeeded()) definition = match.value->definition;
        }
        QJsonObject item{{"kind", "object"}, {"object_id", QString::fromStdString(change.object.rawcode)},
            {"object_kind", QString::fromUtf8(ToString(change.object.type))},
            {"title", QString::fromStdString(change.object.rawcode)}, {"target", QString::fromStdString(change.object.rawcode)},
            {"before", change.before ? ObjectValueJson(*change.before, definition) : QJsonValue(QJsonValue::Null)},
            {"after", change.after ? ObjectValueJson(*change.after, definition) : QJsonValue(QJsonValue::Null)}};
        if (change.modification) {
            item.insert("field", QString::fromStdString(change.modification->fieldId));
            const auto address = ResolveObjectField(afterContext, change.object.rawcode, change.modification->fieldId,
                change.modification->level, change.modification->variation);
            const auto& key = address.Succeeded() ? address.value->modification : *change.modification;
            item.insert("before", valueAt(beforeFields, key, change.before));
            item.insert("after", valueAt(afterFields, key, change.after));
            if (key.level && *key.level > 0) item.insert("level", static_cast<qint64>(*key.level));
            if (key.variation && *key.variation > 0) item.insert("variation", static_cast<qint64>(*key.variation));
        } else {
            item.insert("before", objectAt(beforeContext, beforeFields));
            item.insert("after", objectAt(afterContext, afterFields));
        }
        items.append(item);
    }
    return {{"objects", plan.changes.Empty() ? 0 : 1}, {"items", items}};
}
} // namespace

QJsonValue ObjectValueJson(const ObjectData::Value& value, const ObjectData::ObjectFieldDefinition* definition) {
    if (definition && (definition->valueType == "bool" || definition->valueType == "boolean"))
        if (const auto* integer = std::get_if<std::int32_t>(&value)) return *integer != 0;
    if (definition && definition->isList) if (const auto* text = std::get_if<std::string>(&value)) {
        QJsonArray result;
        if (text->empty() || *text == "_") return result;
        for (const auto& part : QString::fromStdString(*text).split(',')) result.append(part);
        return result;
    }
    return std::visit([](const auto& item) -> QJsonValue {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, std::int32_t> || std::is_same_v<T, float> || std::is_same_v<T, bool>) return item;
        else if constexpr (std::is_same_v<T, std::string>) return QString::fromStdString(item);
        else if constexpr (std::is_same_v<T, RawcodeListValue>) {
            QJsonArray result; for (const auto& id : item.values) result.append(QString::fromStdString(id)); return result;
        } else if constexpr (std::is_same_v<T, UnknownBytesValue>) return QJsonObject{{"opaque", true}, {"bytes", QString::number(item.bytes.size())}};
        else return QString::fromStdString(item.value);
    }, value);
}

std::vector<ToolDefinition> ObjectToolDefinitions() {
    const auto kind = QJsonObject{{"type", "string"}, {"enum", QJsonArray{"unit", "item", "ability", "buff", "destructable", "doodad", "upgrade"}}};
    auto properties = QJsonObject{{"kind", kind}, {"project_identity", String()},
        {"expected_revision", QJsonObject{{"type", "string"}}}, {"page", Page()}};
    std::vector<ToolDefinition> result;
    const auto add = [&](const char* name, const char* description, QJsonObject fields, QJsonArray required, ToolPermission permission) {
        ToolDefinition tool;
        tool.name = QString::fromUtf8(name); tool.description = QString::fromUtf8(description);
        tool.inputSchema = BasicSchema(std::move(fields), std::move(required));
        tool.permission = permission; tool.maxResponseBytes = 1024 * 1024;
        result.push_back(std::move(tool));
    };
    auto list = properties;
    list.insert("scope", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"standard", "custom", "all"}}, {"default", "all"}});
    list.insert("query", QJsonObject{{"type", "string"}, {"maxLength", 256}});
    add("objects_list", "分页读取七类物编的 id、名称、来源及父对象；page.limit 上限128。返回当前 project_identity/revision供后续修改使用。",
        list, {"kind"}, ToolPermission::Query);
    auto get = properties; get.insert("id", Id());
    get.insert("fields", QJsonObject{{"type", "array"}, {"maxItems", 128}, {"items", String()}});
    add("objects_get", "读取对象当前值、继承默认值、中文字段名、原始键、类型和覆盖状态。fields可筛选原生四字节键或LNI键，字段结果有界分页。",
        get, {"kind", "id"}, ToolPermission::Query);
    auto set = properties; set.remove("page"); set.insert("id", Id());
    set.insert("changes", QJsonObject{{"type", "array"}, {"minItems", 1}, {"maxItems", 128},
        {"items", BasicSchema({{"field", String()}, {"level", QJsonObject{{"type", "integer"}, {"minimum", 1}, {"maximum", 1024}}},
            {"variation", QJsonObject{{"type", "integer"}, {"minimum", 1}, {"maximum", 1024}}}, {"value", QJsonObject{}}},
            {"field", "value"})}});
    add("objects_set_fields", "原子修改1..128个字段并进入共用事务/撤销栈。数值按类型/范围校验，布尔用true/false，列表用有序字符串数组，引用必须实际存在。返回前后值和新修订，不保存。",
        set, {"kind", "id", "changes", "project_identity", "expected_revision"}, ToolPermission::Edit);
    auto create = properties; create.remove("page"); create.insert("base_id", Id()); create.insert("new_id", Id()); create.insert("name", String());
    add("objects_create", "复制已有标准或自定义对象；new_id省略时分配不冲突的四字节id，name按真实名称字段元数据修改。创建也必须绑定修订，进入原事务/撤销栈，不保存。",
        create, {"kind", "base_id", "project_identity", "expected_revision"}, ToolPermission::Edit);
    auto remove = properties; remove.remove("page"); remove.insert("id", Id());
    add("objects_delete", "先用共用引用审计检查确定/可能引用和覆盖缺口；未证明完整无引用时拒绝删除并返回位置，不用静态零命中放行。",
        remove, {"kind", "id", "project_identity", "expected_revision"}, ToolPermission::Edit);
    auto options = properties; options.insert("field", String()); options.insert("id", Id());
    add("objects_field_options", "分页查询字段真实枚举或可选引用对象。受对象/技能限制的字段可提供id；不会要求猜rawcode。布尔返回真/假，列表选项仅为条目。",
        options, {"kind", "field"}, ToolPermission::Query);
    return result;
}

QJsonObject InvokeObjectTool(const QString& name, const QJsonObject& arguments, const ObjectToolHost& host) {
    if (!host.identity || !host.context) return Failure("OBJECT_HOST_UNAVAILABLE", "物编宿主上下文未接通。");
    const auto definitions = ObjectToolDefinitions();
    const auto definition = std::find_if(definitions.begin(), definitions.end(), [&](const ToolDefinition& tool) { return tool.name == name; });
    if (definition == definitions.end()) return Failure("OBJECT_OPERATION_UNSUPPORTED", "没有此物编工具。");
    const auto properties = definition->inputSchema.value("properties").toObject();
    for (const auto& required : definition->inputSchema.value("required").toArray())
        if (!arguments.contains(required.toString())) return Failure("OBJECT_ARGUMENT_REQUIRED", "缺少参数：" + required.toString().toStdString());
    for (auto it = arguments.begin(); it != arguments.end(); ++it) {
        if (!properties.contains(it.key())) return Failure("OBJECT_ARGUMENT_UNKNOWN", "未知参数：" + it.key().toStdString());
        const auto type = properties.value(it.key()).toObject().value("type").toString();
        if ((type == "string" && !it.value().isString()) || (type == "array" && !it.value().isArray()) ||
            (type == "object" && !it.value().isObject())) return Failure("OBJECT_ARGUMENT_TYPE", "参数类型不符：" + it.key().toStdString());
    }
    const auto type = Kind(arguments.value("kind").toString());
    if (!type) return Failure("OBJECT_KIND_INVALID", "kind 必须是七类物编之一。");
    const bool write = name == "objects_set_fields" || name == "objects_create" || name == "objects_delete";
    const auto identity = host.identity();
    if (write || arguments.contains("expected_revision") || arguments.contains("project_identity")) {
        if (!arguments.value("project_identity").isString() || !arguments.value("expected_revision").isString() ||
            arguments.value("project_identity") != identity.value("project_identity") || arguments.value("expected_revision") != identity.value("revision"))
            return Failure("OBJECT_REVISION_CONFLICT", "地图身份或修订已变化；重新查询物编后再操作。");
    }
    const auto context = host.context(*type);
    if (!context.Succeeded()) return Failure(context);
    const auto page = DecodePage(arguments);
    if (!page.Succeeded()) return Failure(page);
    const auto finish = [&](QJsonObject result) {
        const auto current = host.identity();
        result.insert("project_identity", current.value("project_identity"));
        result.insert("revision", current.value("revision"));
        for (const auto* key : {"transaction_id", "transaction_revision", "transaction_active"})
            if (current.contains(key)) result.insert(key, current.value(key));
        result.insert("kind", QString::fromUtf8(ToString(*type)));
        return result;
    };
    if (name == "objects_list") {
        const auto scope = arguments.value("scope").toString("all");
        if (scope != "standard" && scope != "custom" && scope != "all") return Failure("OBJECT_SCOPE_INVALID", "scope须为standard/custom/all。");
        auto rows = ListObjects(*context.value, scope == "standard" ? ObjectListScope::Standard : scope == "custom" ? ObjectListScope::Custom : ObjectListScope::All);
        if (!rows.Succeeded()) return Failure(rows);
        const auto query = arguments.value("query").toString();
        if (query.size() > 256) return Failure("OBJECT_QUERY_BUDGET", "query最多256字符。");
        QJsonArray items; qint64 total = 0;
        for (const auto& row : *rows.value) {
            if (!QString::fromStdString(row.key.rawcode).contains(query, Qt::CaseInsensitive) &&
                !QString::fromStdString(row.name).contains(query, Qt::CaseInsensitive)) continue;
            if (total >= page.value->offset && items.size() < page.value->limit) items.append(QJsonObject{
                {"id", QString::fromStdString(row.key.rawcode)}, {"stable_id", QString::number(row.stableId)},
                {"name", QString::fromStdString(row.name)}, {"display_name", QString::fromStdString(row.nameResolution.displayName)},
                {"raw_value", QString::fromStdString(row.nameResolution.rawValue)},
                {"name_state", QString::fromUtf8(NameResolutionStateName(row.nameResolution.state))},
                {"name_reason", QString::fromStdString(row.nameResolution.reason)},
                {"name_reference", QString::fromStdString(row.nameResolution.reference)},
                {"icon_path", QString::fromStdString(row.nameResolution.iconPath)}, {"source", QString::fromStdString(row.source)},
                {"parent_id", row.parent ? QJsonValue(QString::fromStdString(*row.parent)) : QJsonValue(QJsonValue::Null)}});
            ++total;
        }
        return finish({{"ok", true}, {"items", items}, {"total", total}, {"offset", page.value->offset}, {"limit", page.value->limit},
            {"truncated", total > static_cast<qint64>(page.value->offset) + items.size()}});
    }
    if (name == "objects_get") {
        auto fields = ReadObjectFields(*context.value, arguments.value("id").toString().toStdString());
        if (!fields.Succeeded()) return Failure(fields);
        const auto filter = arguments.value("fields").toArray();
        if (filter.size() > 128) return Failure("OBJECT_FIELDS_BUDGET", "fields最多128项。");
        for (const auto& field : filter) if (!field.isString() || field.toString().isEmpty() || field.toString().size() > 128)
            return Failure("OBJECT_FIELD_ADDRESS_INVALID", "fields各项必须是1..128字符的字段键。");
        QJsonArray items; qint64 total = 0;
        for (const auto& row : *fields.value) {
            if (!filter.isEmpty() && std::none_of(filter.begin(), filter.end(), [&](const QJsonValue& field) {
                return field.toString() == QString::fromStdString(row.key.fieldId) ||
                    field.toString().compare(QString::fromStdString(row.sourceKey), Qt::CaseInsensitive) == 0;
            })) continue;
            if (total >= page.value->offset && items.size() < page.value->limit) {
                auto item = FieldJson(row);
                if (row.value) {
                    NameResolutionContext names;
                    names.mapDocument = context.value->nameMap;
                    names.standardObjects = context.value->standard;
                    const auto raw = ValueToDisplayString(*row.value);
                    const auto resolved = ResolveName(names, raw);
                    item.insert("raw_value", QString::fromStdString(raw));
                    item.insert("display_value", QString::fromStdString(resolved.displayName));
                    item.insert("value_name_state", QString::fromStdString(std::string(NameResolutionStateName(resolved.state))));
                    item.insert("value_name_reason", QString::fromStdString(resolved.reason));
                }
                items.append(item);
            }
            ++total;
        }
        return finish({{"ok", true}, {"id", arguments.value("id")}, {"fields", items}, {"total", total},
            {"offset", page.value->offset}, {"limit", page.value->limit}, {"truncated", total > page.value->offset + items.size()}});
    }
    if (name == "objects_field_options") {
        const auto fieldName = arguments.value("field").toString().toStdString();
        const ObjectFieldDefinition* field = nullptr;
        if (arguments.contains("id")) {
            const auto match = ResolveObjectField(*context.value, arguments.value("id").toString().toStdString(), fieldName);
            if (!match.Succeeded()) return Failure(match);
            field = match.value->definition;
        } else {
            field = context.value->schema ? context.value->schema->FindNative(*type, fieldName) : nullptr;
            if (!field && context.value->schema) {
                const auto match = context.value->schema->MatchSourceField(*type, fieldName);
                if (match.safeToEdit) field = match.definition;
            }
        }
        if (!field) return Failure("OBJECT_FIELD_UNSUPPORTED", "字段定义不明确；提供实际对象id或从objects_get取得field。");
        if (!field->nativeValueType) return Failure("OBJECT_FIELD_TYPE_UNSUPPORTED", "字段存储类型未确认，不能提供可写的自由值选项。");
        if (!arguments.contains("id") && (!field->useSpecific.empty() || !field->notSpecific.empty()))
            return Failure("OBJECT_FIELD_CONTEXT_REQUIRED", "此字段受实际对象类型限制；请提供id以检查适用性。");
        QJsonArray options;
        const bool boolean = field->valueType == "bool" || field->valueType == "boolean";
        if (boolean) {
            options.append(QJsonObject{{"value", true}, {"name", "真"}}); options.append(QJsonObject{{"value", false}, {"name", "假"}});
        }
        if (!boolean) for (const auto& choice : field->choices) {
            QJsonValue value(QString::fromStdString(choice.value));
            if (field->nativeValueType == NativeValueType::Int32) {
                bool ok = false; const auto number = QString::fromStdString(choice.value).toInt(&ok); if (ok) value = number;
            }
            options.append(QJsonObject{{"value", value}, {"name", QString::fromStdString(choice.displayName)}});
        }
        for (const auto targetType : field->referenceTypes) {
            auto target = host.context(targetType);
            if (!target.Succeeded()) return Failure(target);
            const auto rows = ListObjects(*target.value, ObjectListScope::All);
            if (!rows.Succeeded()) return Failure(rows);
            for (const auto& row : *rows.value) options.append(QJsonObject{{"value", QString::fromStdString(row.key.rawcode)},
                {"id", QString::fromStdString(row.key.rawcode)}, {"kind", QString::fromUtf8(ToString(targetType))},
                {"name", QString::fromStdString(row.name)}, {"source", QString::fromStdString(row.source)}});
        }
        QJsonArray items;
        for (qsizetype i = page.value->offset; i < options.size() && items.size() < page.value->limit; ++i) items.append(options[i]);
        return finish({{"ok", true}, {"field", QString::fromStdString(field->fieldId)}, {"ordered_list", field->isList},
            {"items", items}, {"total", options.size()}, {"offset", page.value->offset}, {"limit", page.value->limit},
            {"truncated", options.size() > page.value->offset + items.size()},
            {"free_value", options.isEmpty() && field->referenceTypes.empty() && field->choices.empty() && !boolean}});
    }
    if (!write || !host.commit) return Failure("OBJECT_OPERATION_UNSUPPORTED", "当前宿主没有此物编编辑操作。");
    Result<ObjectCommandPlan> plan;
    QJsonObject audit;
    if (name == "objects_set_fields") {
        const auto requested = arguments.value("changes").toArray();
        if (requested.isEmpty() || requested.size() > 128) return Failure("OBJECT_CHANGE_BUDGET", "一批需要1..128项。");
        std::vector<ObjectFieldChange> changes;
        for (const auto& value : requested) {
            if (!value.isObject()) return Failure("OBJECT_CHANGE_INVALID", "每项须为field/level/value对象。");
            const auto change = value.toObject();
            for (auto it = change.begin(); it != change.end(); ++it)
                if (it.key() != "field" && it.key() != "level" && it.key() != "variation" && it.key() != "value")
                    return Failure("OBJECT_CHANGE_INVALID", "字段修改含未知参数：" + it.key().toStdString());
            if (!change.value("field").isString() || !change.contains("value"))
                return Failure("OBJECT_CHANGE_INVALID", "字段修改必须提供字符串field和value。");
            auto decoded = DecodeValue(change.value("value"));
            if (!decoded.Succeeded()) return Failure(decoded);
            ObjectFieldChange row{change.value("field").toString().toStdString(), {}, {}, std::move(*decoded.value)};
            for (const auto& key : {"level", "variation"}) if (change.contains(key)) {
                const auto index = ParseInt32(change, key);
                if (!index.Succeeded()) return Failure(index);
                if (*index.value < 1 || *index.value > 1024) return Failure("OBJECT_FIELD_ADDRESS_INVALID", "等级/变体需1..1024。");
                if (std::string_view(key) == "level") row.level = static_cast<std::uint32_t>(*index.value);
                else row.variation = static_cast<std::uint32_t>(*index.value);
            }
            changes.push_back(std::move(row));
        }
        plan = PlanObjectFieldChanges(*context.value, arguments.value("id").toString().toStdString(), changes);
    } else if (name == "objects_create") {
        plan = PlanObjectCreation(*context.value, arguments.value("base_id").toString().toStdString(),
            arguments.contains("new_id") ? std::optional{arguments.value("new_id").toString().toStdString()} : std::nullopt,
            arguments.contains("name") ? std::optional{arguments.value("name").toString().toStdString()} : std::nullopt);
    } else {
        if (!host.audit) return Failure("OBJECT_DELETE_AUDIT_UNAVAILABLE", "未接通工程引用审计，删除被拒绝。");
        const auto assessment = host.audit({*type, arguments.value("id").toString().toStdString()});
        audit = AuditJson(assessment);
        plan = PlanObjectDeletion(*context.value, arguments.value("id").toString().toStdString(), assessment);
    }
    if (!plan.Succeeded()) {
        auto failed = Failure(plan); if (!audit.isEmpty()) failed.insert("reference_audit", audit); return failed;
    }
    const auto changes = ChangesJson(*plan.value, *context.value);
    const auto latest = host.identity();
    if (latest.value("project_identity") != identity.value("project_identity") || latest.value("revision") != identity.value("revision"))
        return Failure("OBJECT_REVISION_CONFLICT", "物编校验期间地图身份或修订变化；整批未发布。");
    if (!plan.value->changes.Empty()) {
        const auto committed = host.commit(*plan.value);
        if (!committed.Succeeded()) return Failure(committed);
    }
    return finish({{"ok", true}, {"id", QString::fromStdString(plan.value->subject.rawcode)},
        {"stable_id", QString::number(MakeObjectStableId(plan.value->subject))}, {"changed", !plan.value->changes.Empty()},
        {"saved", false}, {"changes", changes}});
}

} // namespace War3Map::Mcp
