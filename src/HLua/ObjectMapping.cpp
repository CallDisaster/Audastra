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
std::string LniName(const ObjectFieldDefinition& field, const ObjectFieldSchema& schema) {
    std::string name = field.sourceField;
    if (field.data > 0 && field.data <= 26) name += static_cast<char>('A' + field.data - 1);
    const bool indexed = std::any_of(schema.Definitions().begin(), schema.Definitions().end(), [&](const auto& other) {
        return other.objectType == field.objectType && other.sourceField == field.sourceField && other.index >= 1;
    });
    if (indexed) name += '_' + std::to_string(field.index + 1);
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
        auto end = std::from_chars(text.data(), text.data() + text.size(), number);
        if (end.ec != std::errc{} || end.ptr != text.data() + text.size()) return Fail<Value>("HLUA_FIELD_VALUE", "数值字段含非数字文本。");
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
}
Result<ObjectData::LniObjectDocument> ApplyHslkObjects(ObjectType type, const LniObjectDocument& baseline,
    std::span<const HslkObject> objects, const ObjectFieldSchema& schema, const StandardObjectCatalog& standard) {
    LniObjectDocument output = baseline;
    std::map<std::string, std::vector<const ObjectFieldDefinition*>> fieldsByName;
    for (const auto& definition : schema.Definitions()) if (definition.objectType == type)
        fieldsByName[LniName(definition, schema)].push_back(&definition);
    Services::ObjectCommandContext source; source.type = type; source.document = &output; source.schema = &schema; source.standard = &standard;
    for (const auto& object : objects) {
        if (object.objectClass != ToString(type)) continue;
        const ObjectKey key{type, object.id};
        const bool original = object.parent == object.id;
        if (output.FindObject(key)) return Fail<LniObjectDocument>("HLUA_ID_CONFLICT", "物体已经存在：" + object.id);
        const auto* parent = standard.Find(type, object.parent);
        // 自定义父链由共用事务验证；字段适用性必须追到确定的标准终点。
        std::string baseId = object.parent;
        std::set<std::string> seen;
        while (!parent) {
            if (!seen.insert(baseId).second || seen.size() > 128)
                return Fail<LniObjectDocument>("HLUA_PARENT_INVALID", "父链循环或超过128层：" + object.id);
            const auto* record = output.FindObject({type, baseId});
            if (!record || !record->parentRawcode) return Fail<LniObjectDocument>("HLUA_PARENT_MISSING", "标准父对象不可用：" + baseId);
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
        for (const auto& field : object.fields) {
            std::vector<const ObjectFieldDefinition*> candidates;
            const auto known = fieldsByName.find(Text::LowerAscii(field.name));
            if (known != fieldsByName.end()) for (const auto* definition : known->second)
                if (schema.AppliesTo(*definition, context)) candidates.push_back(definition);
            if (candidates.size() != 1 || !candidates.front()->nativeValueType)
                return Fail<LniObjectDocument>("HLUA_FIELD_UNMAPPED", object.id + "." + field.name + "：字段未知、歧义或没有可靠原生类型。");
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
                if (!value.Succeeded()) return {std::nullopt, std::move(value.diagnostics)};
                const auto applied = output.SetModification(key, address, std::move(*value.value));
                if (!applied.Succeeded()) return {std::nullopt, applied.diagnostics};
            }
        }
    }
    return Result<LniObjectDocument>::Success(std::move(output));
}
} // namespace War3Map::HLua
