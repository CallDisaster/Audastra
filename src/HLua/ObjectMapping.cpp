#include "War3MapCore/HLua/ObjectMapping.h"
#include "War3MapCore/Services/ObjectCommands.h"
#include "War3MapCore/Text/TextUtil.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <set>

namespace War3Map::HLua {
namespace {
using namespace ObjectData;
template<class T> Result<T> Fail(const char* code, std::string message) { return Result<T>::Failure("HLua", code, std::move(message)); }
std::string LniName(const ObjectFieldDefinition& field, const ObjectFieldSchema& schema, bool withIndex = true) {
    std::string name = field.sourceField;
    if (field.data > 0 && field.data <= 26) name += static_cast<char>('A' + field.data - 1);
    const bool indexed = std::any_of(schema.Definitions().begin(), schema.Definitions().end(), [&](const auto& other) {
        return other.objectType == field.objectType && other.sourceField == field.sourceField && other.index >= 1;
    });
    if (indexed && withIndex) name += '_' + std::to_string(field.index + 1);
    return Text::LowerAscii(name);
}
Result<Value> ValueFor(const FieldAtom& atom, NativeValueType type) {
    if (type == NativeValueType::String) {
        if (const auto* text = std::get_if<std::string>(&atom)) {
            if (text->find('\0') != text->npos) return Fail<Value>("HLUA_FIELD_VALUE", "原生文本字段不能含 NUL。");
            return Result<Value>::Success(*text);
        }
        char text[64]; const auto number = std::get<double>(atom);
        auto end = std::to_chars(text, text + sizeof(text), number, std::chars_format::general);
        if (end.ec != std::errc{}) return Fail<Value>("HLUA_FIELD_VALUE", "无法编码文本字段的数字。");
        return Result<Value>::Success(std::string(text, end.ptr));
    }
    double number = 0;
    if (const auto* value = std::get_if<double>(&atom)) number = *value;
    else {
        const auto& text = std::get<std::string>(atom);
        // LNI 的显式空数值表示零；不把其它非法文本或缺失字段当成零。
        if (!text.empty()) {
            auto end = std::from_chars(text.data(), text.data() + text.size(), number);
            if (end.ec != std::errc{} || end.ptr != text.data() + text.size()) return Fail<Value>("HLUA_FIELD_VALUE", "数值字段含非数字文本。");
        }
    }
    if (!std::isfinite(number)) return Fail<Value>("HLUA_FIELD_VALUE", "数值字段必须有限。");
    if (type == NativeValueType::Int32) {
        if (std::trunc(number) != number || number < INT32_MIN || number > INT32_MAX)
            return Fail<Value>("HLUA_FIELD_VALUE", "整数字段超界或含小数，未截断。");
        return Result<Value>::Success(static_cast<std::int32_t>(number));
    }
    const auto real = static_cast<float>(number);
    if (!std::isfinite(real)) return Fail<Value>("HLUA_FIELD_VALUE", "实数字段超出原生 float32 范围。");
    return Result<Value>::Success(real);
}
std::string AtomText(const FieldAtom& atom) {
    auto value = ValueFor(atom, NativeValueType::String);
    return value.Succeeded() ? std::get<std::string>(*value.value) : "[无法表示]";
}
std::string IndexedHints(const HslkObject& object, ObjectType type, const ObjectFieldSchema& schema) {
    std::set<std::string> keys;
    for (const auto& field : object.fields) for (const auto& definition : schema.Definitions())
        if (definition.objectType == type && LniName(definition, schema, false) == Text::LowerAscii(field.name)
            && LniName(definition, schema) != Text::LowerAscii(field.name)) keys.insert(LniName(definition, schema));
    std::string text;
    for (const auto& key : keys) { if (!text.empty()) text += ", "; text += key; }
    return text.empty() ? "" : "；未带下标的声明不能代替缺失父值，请改用带下标的键：" + text;
}
Result<IndexedFieldAudit> RedundantField(const HslkObject& object, const HslkField& field,
    std::span<const ObjectFieldDefinition* const> definitions,
    const Services::ObjectCommandContext& source) {
    std::string keys;
    for (const auto* definition : definitions) {
        if (!keys.empty()) keys += ", ";
        keys += LniName(*definition, *source.schema);
    }
    const auto rejected = [&](std::string why) {
        return Fail<IndexedFieldAudit>("HLUA_FIELD_UNMAPPED", (field.source.empty() ? object.source : field.source) + "：" + object.id + "." + field.name
            + "：" + why + "；请改用带下标的键：" + keys + "。未选择下标、广播或丢弃字段。");
    };
    if (field.array || field.values.size() != 1) return rejected("冗余声明只接受单个文本或数字");
    auto rows = Services::ReadObjectFields(source, object.id);
    if (!rows.Succeeded()) return rejected("无法确认完整父链与有效字段值");
    IndexedFieldAudit audit{object.objectClass, object.id, field.source.empty() ? object.source : field.source,
        field.name, AtomText(field.values.front()), {}};
    std::set<std::int32_t> seenIndices;
    for (const auto* definition : definitions) {
        if (!definition->nativeValueType || definition->index < 0 || definition->repeat != 0)
            return rejected("下标或等级范围无法完整确认");
        if (!seenIndices.insert(definition->index).second) return rejected("同一下标有多个适用定义，无法唯一确认");
        const auto found = std::find_if(rows.value->begin(), rows.value->end(), [&](const auto& row) {
            return row.key.fieldId == definition->fieldId && row.definition == definition;
        });
        const auto indexName = LniName(*definition, *source.schema) + " (" + definition->fieldId + ")";
        std::optional<Value> effective;
        std::string valueSource;
        bool defaulted = false;
        if (found == rows.value->end()) {
            const auto* parent = source.standard->Find(source.type, object.parent);
            bool profileKeyPresent = false;
            if (parent) {
                const auto origin = parent->fieldSourceFiles.find(Text::LowerAscii(definition->sourceField));
                profileKeyPresent = origin != parent->fieldSourceFiles.end()
                    && std::find(parent->profileSectionFiles.begin(), parent->profileSectionFiles.end(), origin->second) != parent->profileSectionFiles.end();
            }
            const bool unknownOverride = std::any_of(rows.value->begin(), rows.value->end(), [&](const auto& row) {
                return row.key.fieldId == definition->fieldId;
            });
            if (!parent || !parent->nativeOverridesSupported || !parent->completeProfileAndSlk || profileKeyPresent || unknownOverride
                || Text::LowerAscii(definition->sourceTable) != "profile" || *definition->nativeValueType != NativeValueType::String
                || definition->isList || !definition->referenceTypes.empty())
                return rejected(indexName + " 没有可确认的实际有效值；父物体=" + object.parent
                    + "；未满足完整标准父物体的缺省文本条件（数字、列表和不完整分量不适用）");
            effective = std::string{}; defaulted = true;
            valueSource = "standard-profile-default-empty:" + object.parent;
            for (const auto& path : parent->profileSectionFiles) valueSource += ';' + path;
        } else {
            if (!found->editable || !found->value) return rejected(indexName + " 的实际有效值不可靠；来源="
                + found->valueSource + "；原因=" + found->reason);
            effective = found->value; valueSource = found->valueSource;
        }
        const auto expected = ValueFor(field.values.front(), *definition->nativeValueType);
        if (!expected.Succeeded() || *expected.value != *effective)
            return rejected("与 " + LniName(*definition, *source.schema) + " 的实际有效值不同");
        // 数字相等使用精确比较；不能靠写成 float32 后的舍入碰巧相同放行。
        if (*definition->nativeValueType != NativeValueType::String) {
            double declared = 0;
            if (const auto* number = std::get_if<double>(&field.values.front())) declared = *number;
            else {
                const auto& text = std::get<std::string>(field.values.front());
                const auto parsed = std::from_chars(text.data(), text.data() + text.size(), declared);
                if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
                    return rejected("冗余数值声明不能借空文本或解析回退证明相等");
            }
            const double actual = std::holds_alternative<float>(*effective)
                ? static_cast<double>(std::get<float>(*effective)) : static_cast<double>(std::get<std::int32_t>(*effective));
            if (declared != actual) return rejected("原始数值与实际有效值不完全相同");
        }
        audit.indices.push_back({LniName(*definition, *source.schema), definition->fieldId,
            ValueToDisplayString(*effective), std::move(valueSource), defaulted,
            defaulted ? "docs/HLUA_PROJECTS.md#profile-text-default-evidence" : ""});
    }
    return Result<IndexedFieldAudit>::Success(std::move(audit));
}
Result<ObjectData::LniObjectDocument> ApplyHslkObjectsImpl(ObjectType type, const LniObjectDocument& baseline,
    std::span<const HslkObject> objects, const ObjectFieldSchema& schema, const StandardObjectCatalog& standard,
    std::vector<IndexedFieldAudit>* redundantFields, std::vector<UnwrittenField>& unwritten) {
    LniObjectDocument output = baseline;
    std::vector<IndexedFieldAudit> audits;
    std::map<std::string, std::vector<const ObjectFieldDefinition*>> fieldsByName;
    std::set<std::string> baseNames;
    std::set<std::string> indexedFamilies;
    std::map<std::string, std::set<std::string>> otherClasses;
    for (const auto& definition : schema.Definitions()) {
        const auto base = LniName(definition, schema, false);
        if (definition.objectType == type) {
            baseNames.insert(base);
            if (LniName(definition, schema) != base) indexedFamilies.insert(base);
        }
        else {
            otherClasses[base].insert(std::string(ToString(definition.objectType)));
            otherClasses[LniName(definition, schema)].insert(std::string(ToString(definition.objectType)));
        }
    }
    for (const auto& definition : schema.Definitions()) if (definition.objectType == type)
        fieldsByName[LniName(definition, schema)].push_back(&definition);
    Services::ObjectCommandContext source; source.type = type; source.document = &output; source.schema = &schema; source.standard = &standard;
    for (const auto& object : objects) {
        if (object.objectClass != ToString(type)) continue;
        const ObjectKey key{type, object.id};
        const bool original = object.parent == object.id;
        if (output.FindObject(key)) return Fail<LniObjectDocument>("HLUA_ID_CONFLICT", "物体已经存在：" + object.id);
        if (!original && standard.Find(type,object.id)) return Fail<LniObjectDocument>("HLUA_ID_CONFLICT", "新物体 ID 已被标准物体占用：" + object.id);
        const auto* parent = standard.Find(type, object.parent);
        // 自定义父链由共用事务验证；字段适用性必须追到确定的标准终点。
        std::string baseId = object.parent;
        std::set<std::string> seen;
        while (!parent) {
            if (!seen.insert(baseId).second || seen.size() > 128)
                return Fail<LniObjectDocument>("HLUA_PARENT_INVALID", "父链循环或超过128层：" + object.id);
            const auto* record = output.FindObject({type, baseId});
            if (!record || !record->parentRawcode) return Fail<LniObjectDocument>("HLUA_PARENT_MISSING",
                object.source + "：标准父对象不可用：" + baseId + IndexedHints(object, type, schema));
            baseId = *record->parentRawcode; parent = standard.Find(type, baseId);
        }
        auto writable = Services::VerifyObjectWriteSource(source, object.parent);
        if (!writable.Succeeded()) return {std::nullopt, std::move(writable.diagnostics)};
        auto added = original ? output.AddOriginalObject(object.id) : output.AddObject(object.id, object.parent);
        if (!added.Succeeded()) return {std::nullopt, std::move(added.diagnostics)};
        ObjectFieldContext context;
        context.rawcode = parent->key.rawcode; context.baseCode = std::string(parent->Field("code"));
        context.item = type == ObjectType::Item;
        if (type == ObjectType::Unit) { context.hero = parent->category == "hero"; context.building = parent->category == "building"; }
        if (parent->Field("creep") == "0" || parent->Field("creep") == "1") context.creep = parent->Field("creep") == "1";
        std::set<std::string> addresses;
        std::vector<std::pair<const HslkField*, std::vector<const ObjectFieldDefinition*>>> pending;
        for (const auto& field : object.fields) {
            std::vector<const ObjectFieldDefinition*> candidates;
            const auto known = fieldsByName.find(Text::LowerAscii(field.name));
            if (known != fieldsByName.end()) for (const auto* definition : known->second)
                if (schema.AppliesTo(*definition, context)) candidates.push_back(definition);
            if (candidates.size() != 1 || !candidates.front()->nativeValueType) {
                if (known == fieldsByName.end()) {
                    std::vector<const ObjectFieldDefinition*> indexed;
                    for (const auto& definition : schema.Definitions())
                        if (definition.objectType == type && LniName(definition, schema, false) == Text::LowerAscii(field.name)
                            && LniName(definition, schema) != Text::LowerAscii(field.name) && schema.AppliesTo(definition, context))
                            indexed.push_back(&definition);
                    if (!indexed.empty()) {
                        std::sort(indexed.begin(), indexed.end(), [](const auto* a, const auto* b) { return a->index < b->index; });
                        pending.emplace_back(&field, std::move(indexed));
                        continue;
                    }
                    const auto name = Text::LowerAscii(field.name);
                    const auto separator = name.find_last_of('_');
                    if (separator != name.npos && indexedFamilies.contains(name.substr(0, separator))
                        && separator + 1 < name.size() && std::all_of(name.begin() + separator + 1, name.end(),
                            [](char c) { return c >= '0' && c <= '9'; }))
                        return Fail<LniObjectDocument>("HLUA_FIELD_UNMAPPED", (field.source.empty() ? object.source : field.source)
                            + "：" + object.id + "." + field.name + "：已知下标字段族没有此下标，未按未知字段跳过。");
                    if (!baseNames.contains(name) && !schema.MetadataComplete(type))
                        return Fail<LniObjectDocument>("HLUA_METADATA_INCOMPLETE", (field.source.empty() ? object.source : field.source)
                            + "：" + object.id + "." + field.name + "：本类型metadata不完整或有重复，无法证明字段不存在，未跳过。");
                    if (!baseNames.contains(name) && schema.MetadataComplete(type)) {
                        if (field.values.empty()) return Fail<LniObjectDocument>("HLUA_FIELD_ARRAY", "未写入字段也不能使用无值数组：" + field.name);
                        for (const auto& atom : field.values) {
                            if (const auto* number = std::get_if<double>(&atom); number && !std::isfinite(*number))
                                return Fail<LniObjectDocument>("HLUA_FIELD_VALUE", "未写入字段的数字必须有限：" + field.name);
                            if (const auto* text = std::get_if<std::string>(&atom); text && text->find('\0') != text->npos)
                                return Fail<LniObjectDocument>("HLUA_FIELD_VALUE", "未写入字段的文本不能含NUL：" + field.name);
                        }
                        const auto& classes = otherClasses[name];
                        unwritten.push_back({object.objectClass, object.id, field.source.empty() ? object.source : field.source,
                            field.name, {classes.begin(), classes.end()}});
                        continue;
                    }
                }
                std::string hints;
                const auto wanted=Text::LowerAscii(field.name);
                unsigned count=0;
                for(const auto& [name,definitions]:fieldsByName) if(name.starts_with(wanted) && name!=wanted && count++<8) {
                    if(!hints.empty()) hints+=", "; hints+=name;
                }
                return Fail<LniObjectDocument>("HLUA_FIELD_UNMAPPED", object.source+"："+object.id + "." + field.name
                    + "：字段未知、歧义或没有可靠原生类型。"+(hints.empty() ? "" : "本机 metadata 中的相近键："+hints+"；未自动替换。"));
            }
            const auto& definition = *candidates.front();
            std::vector<FieldAtom> values = field.values;
            if (definition.repeat == 0 && field.array) {
                if (definition.isList && *definition.nativeValueType == NativeValueType::String) {
                    std::string text;
                    for (const auto& atom : values) {
                        auto part = ValueFor(atom, NativeValueType::String);
                        if (!part.Succeeded()) return {std::nullopt, std::move(part.diagnostics)};
                        if (!text.empty()) text += ',';
                        text += std::get<std::string>(*part.value);
                    }
                    values = {std::move(text)};
                } else if (values.size() != 1) return Fail<LniObjectDocument>("HLUA_FIELD_ARRAY", object.id + "." + field.name + "：非重复字段不接受多项数组。");
            }
            if (values.empty()) return Fail<LniObjectDocument>("HLUA_FIELD_ARRAY", object.id + "." + field.name + "：空数组没有确定的原生值。");
            for (std::size_t i = 0; i < values.size(); ++i) {
                ModificationKey address; address.fieldId = definition.fieldId;
                if (type == ObjectType::Ability || type == ObjectType::Upgrade || type == ObjectType::Doodad) {
                    address.dataIndex = static_cast<std::uint32_t>(std::max(0, definition.data));
                    if (type == ObjectType::Doodad) address.variation = definition.repeat > 0 ? static_cast<std::uint32_t>(i + 1) : 0;
                    else address.level = definition.repeat > 0 ? static_cast<std::uint32_t>(i + 1) : 0;
                }
                const auto match = schema.MatchNativeField(type, address, context);
                if (!match.safeToEdit) return Fail<LniObjectDocument>("HLUA_FIELD_UNSUPPORTED", object.id + "." + field.name + "：" + match.reason);
                const auto identity = address.fieldId + ':' + std::to_string(address.level.value_or(0)) + ':'
                    + std::to_string(address.dataIndex.value_or(0)) + ':' + std::to_string(address.variation.value_or(0));
                if (!addresses.insert(identity).second) return Fail<LniObjectDocument>("HLUA_FIELD_DUPLICATE", "字段地址重复：" + object.id + "." + field.name);
                auto value = ValueFor(values[i], *definition.nativeValueType);
                if (!value.Succeeded()) {
                    for (auto& diagnostic : value.diagnostics) diagnostic.message = object.source + "：" + object.id + "." + field.name
                        + '[' + std::to_string(i + 1) + "] -> " + definition.fieldId + "：" + diagnostic.message;
                    return {std::nullopt, std::move(value.diagnostics)};
                }
                const auto applied = output.SetModification(key, address, std::move(*value.value));
                if (!applied.Succeeded()) return {std::nullopt, applied.diagnostics};
            }
        }
        // 先完成所有显式字段，再用共用读取服务解析最终继承值，避免输入顺序影响判定。
        for (const auto& [field, definitions] : pending) {
            auto audit = RedundantField(object, *field, definitions, source);
            if (!audit.Succeeded()) return {std::nullopt, std::move(audit.diagnostics)};
            audits.push_back(std::move(*audit.value));
        }
    }
    auto result = Result<LniObjectDocument>::Success(std::move(output));
    for (const auto& audit : audits) {
        std::string values;
        for (const auto& index : audit.indices) {
            if (!values.empty()) values += ", ";
            values += index.field + "=[" + index.effectiveValue + "] (" + index.valueSource + ")";
            if (index.defaultedTextEmpty) values += "；缺省按空串；原版核对证据：" + index.defaultEvidence;
        }
        result.diagnostics.push_back({DiagnosticSeverity::Warning, "HLua", "HLUA_REDUNDANT_INDEXED_FIELD",
            audit.source + "：" + audit.id + "." + audit.field + "=[" + audit.declaredValue
            + "] 与每个下标实际有效值完全相同，按本方兼容策略保留结果、不写冗余声明：" + values, {}});
    }
    if (redundantFields) redundantFields->insert(redundantFields->end(), audits.begin(), audits.end());
    return result;
}
std::string ClassLabel(std::string_view type) {
    if (type == "unit") return "单位";
    if (type == "item") return "物品";
    if (type == "ability") return "技能";
    if (type == "buff") return "魔法效果";
    if (type == "upgrade") return "科技";
    if (type == "doodad") return "装饰物";
    return "可破坏物";
}
}
Result<ObjectData::LniObjectDocument> ApplyHslkObjects(ObjectType type, const LniObjectDocument& baseline,
    std::span<const HslkObject> objects, const ObjectFieldSchema& schema, const StandardObjectCatalog& standard,
    std::vector<IndexedFieldAudit>* redundantFields, std::vector<UnwrittenField>* unwrittenFields) {
    std::vector<UnwrittenField> unwritten;
    auto result = ApplyHslkObjectsImpl(type, baseline, objects, schema, standard, redundantFields, unwritten);
    std::vector<Diagnostic> warnings;
    for (const auto& field : unwritten) {
        std::string message = field.source + "：" + field.id + "." + field.field + "：该类型没有这个字段，未写入物编。";
        for (const auto& other : field.otherObjectClasses) message += ClassLabel(other) + "有此字段，" + ClassLabel(field.objectClass) + "没有。";
        warnings.push_back({DiagnosticSeverity::Warning, "HLua", "HLUA_FIELD_NOT_WRITTEN", std::move(message), {}});
    }
    result.diagnostics.insert(result.diagnostics.begin(), warnings.begin(), warnings.end());
    if (result.Succeeded() && unwrittenFields) unwrittenFields->insert(unwrittenFields->end(), unwritten.begin(), unwritten.end());
    return result;
}
} // namespace War3Map::HLua
