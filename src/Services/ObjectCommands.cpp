#include "War3MapCore/Services/ObjectCommands.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <tuple>
#include <type_traits>

namespace War3Map::Services {
namespace {
using namespace ObjectData;

template<class T, class U> Result<T> CopyFailure(const Result<U>& source) {
    Result<T> result;
    result.diagnostics = source.diagnostics;
    return result;
}
template<class T> Result<T> CopyFailure(const Status& source) {
    Result<T> result;
    result.diagnostics = source.diagnostics;
    return result;
}
template<class T> Result<T> Fail(const char* code, std::string message) {
    return Result<T>::Failure("Object.Commands", code, std::move(message));
}
bool ValidId(std::string_view value) {
    return value.size() == 4 && std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return c >= 0x20 && c <= 0x7e;
    });
}
std::string Lower(std::string_view value) {
    std::string result(value);
    for (auto& c : result) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return result;
}
std::vector<std::string> Components(std::string_view input) {
    std::vector<std::string> result;
    std::string token;
    bool quoted = false;
    for (std::size_t i = 0; i < input.size(); ++i) {
        const char c = input[i];
        if (c == '"') {
            if (quoted && i + 1 < input.size() && input[i + 1] == '"') { token += c; ++i; }
            else quoted = !quoted;
        } else if (c == ',' && !quoted) {
            result.push_back(std::move(token)); token.clear();
        } else token += c;
    }
    if (quoted) return {};
    result.push_back(std::move(token));
    return result;
}
template<class T> std::optional<T> Number(std::string_view text) {
    T value{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) return {};
    if constexpr (std::is_floating_point_v<T>) if (!std::isfinite(value)) return {};
    return value;
}
std::optional<Value> SourceValue(const ObjectFieldDefinition& field, std::string_view text) {
    if (!field.nativeValueType) return {};
    switch (*field.nativeValueType) {
    case NativeValueType::Int32:
        if (const auto value = Number<std::int32_t>(text)) return Value{*value};
        return {};
    case NativeValueType::Real:
    case NativeValueType::Unreal:
        if (const auto value = Number<float>(text)) return Value{*value};
        return {};
    case NativeValueType::String: return Value{std::string(text)};
    }
    return {};
}
struct Chain {
    std::vector<const ObjectRecord*> records; // 当前对象到远父
    const StandardObjectEntry* base = nullptr;
    ObjectFieldContext field;
};
Result<Chain> Inheritance(const ObjectCommandContext& context, const std::string& id, bool requireSchema = true) {
    if (!ValidId(id)) return Fail<Chain>("OBJECT_ID_INVALID", "对象 id 必须是四字节可打印 ASCII。");
    if (!context.standard || (requireSchema && !context.schema))
        return Fail<Chain>("OBJECT_METADATA_UNAVAILABLE", "尚未加载同一来源的标准物编和字段元数据，不能猜测字段。");
    Chain result;
    std::set<std::string> seen;
    std::string current = id;
    for (std::size_t depth = 0; depth < 128; ++depth) {
        if (!seen.insert(current).second)
            return Fail<Chain>("OBJECT_PARENT_CYCLE", "物编父链存在循环：" + current);
        const auto* record = context.document ? context.document->FindObject({context.type, current}) : nullptr;
        if (record) result.records.push_back(record);
        if (record && record->parentRawcode) { current = *record->parentRawcode; continue; }
        result.base = context.standard->Find(context.type, current);
        if (!result.base)
            return Fail<Chain>("OBJECT_BASE_UNAVAILABLE", "对象或其标准父对象不存在：" + current);
        result.field.rawcode = result.base->key.rawcode;
        result.field.baseCode = std::string(result.base->Field("code"));
        result.field.item = context.type == ObjectType::Item;
        if (context.type == ObjectType::Unit) {
            result.field.hero = result.base->category == "hero";
            result.field.building = result.base->category == "building";
        }
        const auto creep = result.base->Field("creep");
        if (creep == "0" || creep == "1") result.field.creep = creep == "1";
        return Result<Chain>::Success(std::move(result));
    }
    return Fail<Chain>("OBJECT_PARENT_BUDGET", "对象父链超过 128 层，未修改。");
}
ObjectFieldMatch MatchModification(const ObjectCommandContext& context, const Chain& chain,
    const ModificationKey& key) {
    auto match = context.schema->MatchNativeField(context.type, key, chain.field);
    if (!match.definition) {
        const auto index = key.level && *key.level > 0 ? std::optional<std::uint32_t>{*key.level - 1} : std::nullopt;
        match = context.schema->MatchSourceField(context.type, key.fieldId, chain.field, index);
    }
    return match;
}
Result<LniObjectDocument> EmptyOrCopy(const ObjectCommandContext& context) {
    if (context.document) {
        if (!context.document->IsStructurallySafe())
            return Fail<LniObjectDocument>("OBJECT_DOCUMENT_UNSAFE", "此类物编结构不安全，原文保持不变。");
        return Result<LniObjectDocument>::Success(*context.document);
    }
    const auto parsed = LniObjectCodec(context.type).Parse({});
    const auto* empty = parsed.Succeeded() ? dynamic_cast<LniObjectDocument*>(parsed.document.get()) : nullptr;
    if (!empty) return Fail<LniObjectDocument>("OBJECT_EMPTY_DOCUMENT_FAILED", "无法建立经典空物编文档。");
    return Result<LniObjectDocument>::Success(*empty);
}
Result<ObjectCommandPlan> Seal(ObjectCommandPlan plan) {
    LniObjectCodec codec(plan.subject.type);
    const auto valid = codec.Validate(plan.after);
    if (!valid.Succeeded()) return CopyFailure<ObjectCommandPlan>(valid);
    const auto encoded = codec.Serialize(plan.after);
    if (!encoded.Succeeded()) return CopyFailure<ObjectCommandPlan>(encoded);
    const auto parsed = codec.Parse(*encoded.value);
    const auto* decoded = parsed.Succeeded() ? dynamic_cast<LniObjectDocument*>(parsed.document.get()) : nullptr;
    if (!decoded || !DiffObjects(plan.after.Objects(), decoded->Objects()).Empty())
        return Fail<ObjectCommandPlan>("OBJECT_PLAN_ROUNDTRIP_FAILED", "修改后的物编未能无损回读，整批未发布。");
    plan.changes = DiffObjects(plan.before.Objects(), plan.after.Objects());
    return Result<ObjectCommandPlan>::Success(std::move(plan));
}
bool Exists(const ObjectCommandContext& context, ObjectType type, const std::string& id) {
    const auto* document = context.documents[static_cast<std::size_t>(type)];
    if (type == context.type && context.document) document = context.document;
    return (document && document->FindObject({type, id})) || (context.standard && context.standard->Find(type, id));
}
Result<Value> Normalize(const ObjectCommandContext& context, const ObjectFieldDefinition& field, const Value& input) {
    if (!field.nativeValueType) return Fail<Value>("OBJECT_FIELD_TYPE_UNSUPPORTED", field.fieldId + " 的存储类型未确认。");
    const bool boolean = field.valueType == "bool" || field.valueType == "boolean";
    if (boolean && !std::holds_alternative<bool>(input))
        return Fail<Value>("OBJECT_BOOLEAN_REQUIRED", field.fieldId + " 需要 true/false。");
    std::string text;
    if (field.isList) {
        const auto* list = std::get_if<RawcodeListValue>(&input);
        if (!list) return Fail<Value>("OBJECT_ORDERED_LIST_REQUIRED", field.fieldId + " 需要有序字符串数组，不能用逗号文字代替。");
        if (list->values.size() > 256) return Fail<Value>("OBJECT_LIST_BUDGET", "一个字段最多提供 256 个有序条目。");
        for (const auto& value : list->values) {
            if (value.empty() || value.find_first_of(",\r\n") != std::string::npos || value.find('\0') != std::string::npos)
                return Fail<Value>("OBJECT_LIST_ITEM_INVALID", field.fieldId + " 的列表项为空或含分隔符。");
            if (!text.empty()) text += ',';
            text += value;
        }
    } else if (const auto* stringValue = std::get_if<std::string>(&input)) text = *stringValue;
    else if (const auto* rawcodeValue = std::get_if<RawcodeValue>(&input)) text = rawcodeValue->value;
    else if (const auto* enumValue = std::get_if<EnumValue>(&input)) text = enumValue->value;
    else if (const auto* pathValue = std::get_if<AssetPathValue>(&input)) text = pathValue->value;
    else if (const auto* localizedValue = std::get_if<LocalizedStringValue>(&input)) text = localizedValue->value;
    else if (std::holds_alternative<RawcodeListValue>(input))
        return Fail<Value>("OBJECT_SCALAR_REQUIRED", field.fieldId + " 不是列表字段。");
    else text = ValueToDisplayString(input);
    if (boolean) text = std::get<bool>(input) ? "1" : "0";
    if (text.size() > 65536 || text.find('\0') != std::string::npos || std::holds_alternative<UnknownBytesValue>(input))
        return Fail<Value>("OBJECT_VALUE_INVALID", field.fieldId + " 的值超出预算、含 NUL 或是未知字节。");
    if (!field.referenceTypes.empty()) {
        const auto values = field.isList ? std::get<RawcodeListValue>(input).values : std::vector<std::string>{text};
        for (const auto& id : values) {
            if ((id.empty() || id == "_") && field.canBeEmpty) continue;
            if (!ValidId(id) || std::none_of(field.referenceTypes.begin(), field.referenceTypes.end(),
                [&](ObjectType type) { return Exists(context, type, id); }))
                return Fail<Value>("OBJECT_REFERENCE_NOT_FOUND", field.fieldId + " 引用不存在或未确认的对象：" + id +
                    "；先用 objects_field_options 查询该字段可选对象。");
        }
    }
    if (!field.choices.empty()) {
        const auto values = field.isList ? std::get<RawcodeListValue>(input).values : std::vector<std::string>{text};
        for (const auto& choice : values) {
            if (choice.empty() && field.canBeEmpty) continue;
            if (std::none_of(field.choices.begin(), field.choices.end(), [&](const ObjectFieldChoice& item) { return item.value == choice; }))
                return Fail<Value>("OBJECT_ENUM_INVALID", field.fieldId + " 不接受选项：" + choice);
        }
    }
    std::optional<double> numeric;
    Value value;
    if (boolean) { value = std::int32_t{std::get<bool>(input) ? 1 : 0}; numeric = std::get<bool>(input) ? 1.0 : 0.0; }
    else switch (*field.nativeValueType) {
    case NativeValueType::Int32: {
        const auto* integer = std::get_if<std::int32_t>(&input);
        if (!integer) return Fail<Value>("OBJECT_INTEGER_REQUIRED", field.fieldId + " 需要 int32 整数。");
        value = *integer; numeric = static_cast<double>(*integer); break;
    }
    case NativeValueType::Real:
    case NativeValueType::Unreal:
        if (const auto* real = std::get_if<float>(&input); real && std::isfinite(*real)) { value = *real; numeric = *real; }
        else if (const auto* integer = std::get_if<std::int32_t>(&input)) { value = static_cast<float>(*integer); numeric = static_cast<float>(*integer); }
        else return Fail<Value>("OBJECT_REAL_REQUIRED", field.fieldId + " 需要有限的 real 数值。");
        break;
    case NativeValueType::String:
        if (!field.isList && !std::holds_alternative<std::string>(input) && !std::holds_alternative<RawcodeValue>(input) &&
            !std::holds_alternative<EnumValue>(input) && !std::holds_alternative<AssetPathValue>(input) &&
            !std::holds_alternative<LocalizedStringValue>(input))
            return Fail<Value>("OBJECT_STRING_REQUIRED", field.fieldId + " 需要字符串。");
        value = std::move(text);
        break;
    }
    if (numeric) {
        for (const auto& [bound, minimum] : {std::pair{field.minimum, true}, std::pair{field.maximum, false}}) {
            if (bound.empty() || bound == "_") continue;
            const auto parsed = Number<double>(bound);
            if (!parsed) return Fail<Value>("OBJECT_RANGE_UNSUPPORTED", field.fieldId + " 的范围元数据无法确认：" + bound);
            if ((minimum && *numeric < *parsed) || (!minimum && *numeric > *parsed))
                return Fail<Value>("OBJECT_VALUE_OUT_OF_RANGE", field.fieldId + " 超出字段范围 [" + field.minimum + ", " + field.maximum + "]。");
        }
    }
    return Result<Value>::Success(std::move(value));
}
} // namespace

Status VerifyObjectWriteSource(const ObjectCommandContext& context, const std::string& id) {
    const auto chain = Inheritance(context, id, false);
    if (!chain.Succeeded()) return {chain.diagnostics};
    if (!chain.value->base->nativeOverridesSupported)
        return Status::Failure("Object.Commands", "OBJECT_CATALOG_READ_ONLY",
            "基础对象来自地图或战役 SLK/profile，当前只支持读取，尚未支持此来源的原生覆盖写回。");
    return Status::Ok();
}

Status VerifyObjectDocumentWriteSources(const ObjectCommandContext& context,
    const LniObjectDocument& before, const LniObjectDocument& after) {
    if (!context.standard) return Status::Failure("Object.Command", "OBJECT_METADATA_UNAVAILABLE", "没有当前有效物编目录，未授予文档写入资格。");
    std::set<std::string> changed;
    for (const auto& change : DiffObjects(before.Objects(), after.Objects()).changes) {
        if (change.object.type != context.type)
            return Status::Failure("Object.Command", "OBJECT_PLAN_SCOPE_MISMATCH", "物编文档差异包含其他类别，未授予写入资格。");
        changed.insert(change.object.rawcode);
    }
    for (const auto& id : changed) {
        const ObjectKey key{context.type, id};
        const bool existed = before.FindObject(key) || context.standard->Find(context.type, id);
        const bool exists = after.FindObject(key) || context.standard->Find(context.type, id);
        if (!existed && !exists)
            return Status::Failure("Object.Command", "OBJECT_PLAN_SUBJECT_MISSING", "差异对象不在当前目录或文档中，未授予写入资格。");
        auto current = context;
        if (existed) {
            current.document = &before;
            const auto checked = VerifyObjectWriteSource(current, id);
            if (!checked.Succeeded()) return checked;
        }
        if (exists) {
            current.document = &after;
            const auto checked = VerifyObjectWriteSource(current, id);
            if (!checked.Succeeded()) return checked;
        }
    }
    return Status::Ok();
}

Result<std::vector<ObjectListRow>> ListObjects(const ObjectCommandContext& context, ObjectListScope scope) {
    if (!context.standard) return Fail<std::vector<ObjectListRow>>("OBJECT_CATALOG_UNAVAILABLE", "未加载标准物编目录，不能把缺少来源当作空目录。");
    std::map<std::string, ObjectListRow> rows;
    if (scope != ObjectListScope::Custom) for (const auto& item : context.standard->Entries()) {
        if (item.key.type != context.type) continue;
        rows.emplace(item.key.rawcode, ObjectListRow{item.key, MakeObjectStableId(item.key), item.displayName, "standard", {}});
    }
    if (context.document) for (const auto& item : context.document->Objects()) {
        const bool custom = item.parentRawcode.has_value();
        if ((scope == ObjectListScope::Custom && !custom) || (scope == ObjectListScope::Standard && custom)) continue;
        rows.insert_or_assign(item.key.rawcode, ObjectListRow{item.key, item.stableId, item.key.rawcode,
            custom ? "custom" : "map_override", item.parentRawcode});
    }
    std::vector<ObjectListRow> result;
    result.reserve(rows.size());
    ProjectSearchObjectSource source;
    source.type = context.type;
    source.state = context.document ? ProjectSearchState::Ready : ProjectSearchState::Missing;
    if (context.document) source.records = context.document->Objects();
    NameResolutionContext names;
    names.objects = std::span(&source, 1); names.standardObjects = context.standard; names.mapDocument = context.nameMap;
    for (auto& [id, row] : rows) {
        (void)id;
        row.nameResolution = ResolveObjectName(names, row.key);
        row.name = row.nameResolution.displayName;
        result.push_back(std::move(row));
    }
    return Result<std::vector<ObjectListRow>>::Success(std::move(result));
}

Result<ObjectFieldMatch> ResolveObjectField(const ObjectCommandContext& context, const std::string& id,
    const std::string& field, std::optional<std::uint32_t> level, std::optional<std::uint32_t> variation) {
    const auto chain = Inheritance(context, id);
    if (!chain.Succeeded()) return CopyFailure<ObjectFieldMatch>(chain);
    if (field.empty() || field.size() > 128 || (level && *level > 1024) || (variation && *variation > 1024))
        return Fail<ObjectFieldMatch>("OBJECT_FIELD_ADDRESS_INVALID", "字段键或等级/变体超出预算。");
    if (level && variation) return Fail<ObjectFieldMatch>("OBJECT_FIELD_ADDRESS_INVALID", "等级与变体不能同时提供。");
    ObjectFieldMatch match;
    if (const auto* definition = context.schema->FindNative(context.type, field)) {
        if ((level && context.type != ObjectType::Ability && context.type != ObjectType::Upgrade) ||
            (variation && context.type != ObjectType::Doodad) ||
            (definition->repeat == 0 && (level || variation)) ||
            (definition->repeat > 0 && ((level && *level == 0) || (variation && *variation == 0))))
            return Fail<ObjectFieldMatch>("OBJECT_FIELD_ADDRESS_INVALID", "字段不接受此等级/变体；重复字段使用从 1 开始的等级。 ");
        ModificationKey key{definition->fieldId};
        if (context.type == ObjectType::Doodad) key.variation = variation.value_or(definition->repeat > 0 ? 1u : 0u);
        else if (definition->repeat > 0) key.level = level.value_or(1u);
        key.dataIndex = static_cast<std::uint32_t>(std::max(0, definition->data));
        match = context.schema->MatchNativeField(context.type, key, chain.value->field);
    } else {
        if ((level && context.type != ObjectType::Ability && context.type != ObjectType::Upgrade) ||
            (variation && context.type != ObjectType::Doodad) || (level && *level == 0) || (variation && *variation == 0))
            return Fail<ObjectFieldMatch>("OBJECT_FIELD_ADDRESS_INVALID", "此物编类别不接受该等级/变体地址。");
        const auto index = level && *level > 0 ? std::optional<std::uint32_t>{*level - 1} : std::nullopt;
        match = context.schema->MatchSourceField(context.type, field, chain.value->field, index);
        if (level && match.definition && match.definition->repeat == 0)
            match.safeToEdit = false;
        if (level && match.modification.level != level) match.safeToEdit = false;
        if (variation && match.modification.variation != variation) match.safeToEdit = false;
    }
    if (!match.definition || !match.safeToEdit)
        return Fail<ObjectFieldMatch>("OBJECT_FIELD_UNSUPPORTED", id + "/" + field + "：" + match.reason);
    return Result<ObjectFieldMatch>::Success(std::move(match));
}

Result<std::vector<ObjectFieldRow>> ReadObjectFields(const ObjectCommandContext& context, const std::string& id) {
    const auto chain = Inheritance(context, id);
    if (!chain.Succeeded()) return CopyFailure<std::vector<ObjectFieldRow>>(chain);
    std::vector<ObjectFieldRow> rows;
    const auto find = [&](const ModificationKey& key) {
        return std::find_if(rows.begin(), rows.end(), [&](const ObjectFieldRow& row) { return row.key == key; });
    };
    for (const auto& [sourceKey, sourceValue] : chain.value->base->fields) {
        const auto components = Components(sourceValue);
        std::vector<ObjectFieldMatch> matches;
        auto match = context.schema->MatchSourceField(context.type, sourceKey, chain.value->field);
        if (match.definition && match.safeToEdit) matches.push_back(match);
        else for (std::size_t part = 0; part < components.size() && part < 1024; ++part) {
            auto indexed = context.schema->MatchSourceField(context.type, sourceKey, chain.value->field, static_cast<std::uint32_t>(part));
            if (indexed.definition && indexed.safeToEdit) matches.push_back(std::move(indexed));
        }
        for (const auto& field : matches) {
            std::string text(sourceValue);
            if (field.sourceIndex) {
                if (*field.sourceIndex >= components.size()) continue;
                text = components[*field.sourceIndex];
            } else if (Lower(field.definition->sourceTable) == "profile") {
                if (components.empty()) continue;
                text.clear();
                for (const auto& component : components) { if (!text.empty()) text += ','; text += component; }
            }
            auto value = SourceValue(*field.definition, text);
            if (!value) continue;
            const auto existing = find(field.modification);
            if (existing != rows.end()) {
                if (existing->defaultValue != value) { existing->editable = false; existing->reason = "多个标准来源值冲突。"; }
            } else rows.push_back({field.modification, sourceKey, field.definition, value, value, false,
                context.document ? context.document->IsStructurallySafe() : true, "standard:" + chain.value->base->key.rawcode, {}});
        }
    }
    for (std::size_t position = chain.value->records.size(); position > 0; --position) {
        const auto& record = *chain.value->records[position - 1];
        for (const auto& modification : record.modifications) {
            const auto match = MatchModification(context, *chain.value, modification.key);
            const auto key = match.definition ? match.modification : modification.key;
            auto existing = find(key);
            const bool own = record.key.rawcode == id;
            if (existing == rows.end()) {
                rows.push_back({key, modification.key.fieldId, match.definition, modification.value, {}, own,
                    match.safeToEdit && !modification.valueWasOpaque && !std::holds_alternative<UnknownBytesValue>(modification.value),
                    "map:" + record.key.rawcode, match.reason});
            } else {
                existing->defaultValue = existing->value;
                existing->value = modification.value;
                existing->overridden = own;
                existing->valueSource = "map:" + record.key.rawcode;
                existing->editable = existing->editable && match.safeToEdit && !modification.valueWasOpaque &&
                    !std::holds_alternative<UnknownBytesValue>(modification.value);
            }
        }
    }
    std::sort(rows.begin(), rows.end(), [](const ObjectFieldRow& a, const ObjectFieldRow& b) {
        return std::tuple{a.key.fieldId, a.key.level, a.key.dataIndex, a.key.variation} <
            std::tuple{b.key.fieldId, b.key.level, b.key.dataIndex, b.key.variation};
    });
    if (!chain.value->base->nativeOverridesSupported) for (auto& row : rows) {
        row.editable = false;
        row.reason = "基础对象使用地图或战役 SLK/profile；当前只支持读取，尚未支持此来源的原生覆盖写回。";
    }
    return Result<std::vector<ObjectFieldRow>>::Success(std::move(rows));
}

Result<ObjectCommandPlan> PlanObjectFieldChanges(const ObjectCommandContext& context, const std::string& id,
    std::span<const ObjectFieldChange> changes) {
    const auto writable = VerifyObjectWriteSource(context, id);
    if (!writable.Succeeded()) return CopyFailure<ObjectCommandPlan>(writable);
    if (changes.empty() || changes.size() > 128)
        return Fail<ObjectCommandPlan>("OBJECT_CHANGE_BUDGET", "一批需要 1..128 个字段。");
    const auto fields = ReadObjectFields(context, id);
    if (!fields.Succeeded()) return CopyFailure<ObjectCommandPlan>(fields);
    auto before = EmptyOrCopy(context);
    if (!before.Succeeded()) return CopyFailure<ObjectCommandPlan>(before);
    ObjectCommandPlan plan{{context.type, id}, *before.value, *before.value, {}, "Agent 修改物编 " + id};
    std::vector<ModificationKey> seen;
    if (!plan.after.FindObject(plan.subject)) {
        const auto added = plan.after.AddOriginalObject(id);
        if (!added.Succeeded()) return CopyFailure<ObjectCommandPlan>(added);
    }
    for (const auto& change : changes) {
        const auto match = ResolveObjectField(context, id, change.field, change.level, change.variation);
        if (!match.Succeeded()) return CopyFailure<ObjectCommandPlan>(match);
        const auto& canonical = match.value->modification;
        if (std::find(seen.begin(), seen.end(), canonical) != seen.end())
            return Fail<ObjectCommandPlan>("OBJECT_DUPLICATE_FIELD", "同一批重复修改精确字段地址：" + change.field);
        seen.push_back(canonical);
        auto normalized = Normalize(context, *match.value->definition, change.value);
        if (!normalized.Succeeded()) return CopyFailure<ObjectCommandPlan>(normalized);
        const auto old = std::find_if(fields.value->begin(), fields.value->end(), [&](const ObjectFieldRow& row) { return row.key == canonical; });
        if (old != fields.value->end() && !old->editable)
            return Fail<ObjectCommandPlan>("OBJECT_FIELD_READ_ONLY", change.field + "：" + old->reason);
        ModificationKey target = canonical;
        if (context.document) if (const auto* record = context.document->FindObject(plan.subject)) {
            for (const auto& modification : record->modifications) {
                const auto chain = Inheritance(context, id);
                const auto existing = MatchModification(context, *chain.value, modification.key);
                if (existing.definition && existing.modification == canonical) { target = modification.key; break; }
            }
        }
        const auto applied = plan.after.SetModification(plan.subject, target, std::move(*normalized.value));
        if (!applied.Succeeded()) return CopyFailure<ObjectCommandPlan>(applied);
    }
    return Seal(std::move(plan));
}

Result<ObjectCommandPlan> PlanObjectCreation(const ObjectCommandContext& context, const std::string& baseId,
    const std::optional<std::string>& requestedId, const std::optional<std::string>& name) {
    const auto base = Inheritance(context, baseId);
    if (!base.Succeeded()) return CopyFailure<ObjectCommandPlan>(base);
    if (!base.value->base->nativeOverridesSupported)
        return Fail<ObjectCommandPlan>("OBJECT_CATALOG_READ_ONLY", "基础对象来自地图或战役 SLK/profile，当前只支持读取，未创建或复制对象。");
    std::string id = requestedId.value_or("");
    if (!requestedId) {
        constexpr std::string_view alphabet = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
        for (std::uint32_t counter = 0; counter < 46656; ++counter) {
            std::string candidate(4, '0'); candidate[0] = baseId.front();
            auto value = counter;
            for (std::size_t i = 4; i > 1; --i) { candidate[i - 1] = alphabet[value % 36]; value /= 36; }
            if (!Exists(context, context.type, candidate)) { id = std::move(candidate); break; }
        }
    }
    if (!ValidId(id) || !std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
    })) return Fail<ObjectCommandPlan>("OBJECT_NEW_ID_INVALID", "新 id 必须是四位英文字母或数字；自动分配空间可能已满。");
    if (Exists(context, context.type, id)) return Fail<ObjectCommandPlan>("OBJECT_ID_EXISTS", "对象 id 已存在：" + id);
    auto before = EmptyOrCopy(context);
    if (!before.Succeeded()) return CopyFailure<ObjectCommandPlan>(before);
    ObjectCommandPlan plan{{context.type, id}, *before.value, *before.value, {}, "Agent 创建物编 " + id};
    const auto* source = context.document ? context.document->FindObject({context.type, baseId}) : nullptr;
    const auto added = source && source->parentRawcode ? plan.after.CloneCustomObject(source->key, id) :
        plan.after.AddObject(id, baseId);
    if (!added.Succeeded()) return CopyFailure<ObjectCommandPlan>(added);
    if (source && !source->parentRawcode) for (const auto& field : source->modifications) {
        const auto applied = plan.after.SetModification(plan.subject, field.key, field.value);
        if (!applied.Succeeded()) return CopyFailure<ObjectCommandPlan>(applied);
    }
    if (name) {
        auto targetContext = context; targetContext.document = &plan.after;
        const std::array<ObjectFieldChange, 1> changes{{{"name", {}, {}, *name}}};
        auto renamed = PlanObjectFieldChanges(targetContext, id, changes);
        if (!renamed.Succeeded()) return renamed;
        plan.after = std::move(renamed.value->after);
    }
    return Seal(std::move(plan));
}

Result<ObjectCommandPlan> PlanObjectDeletion(const ObjectCommandContext& context, const std::string& id,
    const ObjectDeletionAssessment& assessment) {
    const ObjectKey key{context.type, id};
    if (!context.document || !context.document->FindObject(key))
        return Fail<ObjectCommandPlan>("OBJECT_NOT_IN_MAP", "标准游戏对象不能删除；当前地图中没有该自定义对象或覆盖。");
    if (assessment.subject != key) return Fail<ObjectCommandPlan>("OBJECT_DELETE_AUDIT_MISMATCH", "引用审计不属于当前对象。");
    return Fail<ObjectCommandPlan>(assessment.disposition == ObjectDeletionDisposition::BlockedReferences ?
        "OBJECT_DELETE_REFERENCED" : "OBJECT_DELETE_COVERAGE_INCOMPLETE",
        "删除未执行：引用审计尚未证明脚本和动态引用全覆盖；请查看返回的引用位置与覆盖范围。");
}

} // namespace War3Map::Services
