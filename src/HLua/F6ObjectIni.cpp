#include "War3MapCore/HLua/ObjectMapping.h"
#include "War3MapCore/Text/LniData.h"
#include "War3MapCore/Runtime/LuaTestMailboxValidation.h"
#include "War3MapCore/IO/FileBytes.h"
#include <charconv>
#include <cmath>
#include <algorithm>

namespace War3Map::HLua {
Result<std::vector<HslkObject>> ParseF6ObjectIni(ObjectData::ObjectType type, std::string_view text,
    std::string_view source, const HslkLimits& limits) {
    using Output = Result<std::vector<HslkObject>>;
    if (!Audastra::Runtime32::LuaMailboxUtf8(text)) return Output::Failure("HLua", "HLUA_LNI_ENCODING", "F6 INI 必须是 UTF-8：" + std::string(source));
    auto parsed = Text::ParseLniData({reinterpret_cast<const std::byte*>(text.data()), text.size()});
    if (!parsed.Succeeded()) {
        for (auto& d : parsed.diagnostics) { d.message = std::string(source) + "：" + d.message; if (d.origin) d.origin->source = IO::PathFromUtf8(source); }
        return {std::nullopt, std::move(parsed.diagnostics)};
    }
    if (parsed.value->fields.size() > limits.objects) return Output::Failure("HLua", "HLUA_OBJECT_BUDGET", "F6 物体数超过预算。");
    std::vector<HslkObject> output;
    const auto atom = [](const Text::LniDataValue& value) -> Result<FieldAtom> {
        if (value.kind == Text::LniDataValue::Kind::String) return Result<FieldAtom>::Success(value.text);
        if (value.kind != Text::LniDataValue::Kind::Number)
            return Result<FieldAtom>::Failure("HLua", "HLUA_LNI_NESTED", "F6 字段不接受嵌套表。");
        double number = 0;
        const auto end = std::from_chars(value.text.data(), value.text.data() + value.text.size(), number);
        if (end.ec != std::errc{} || end.ptr != value.text.data() + value.text.size() || !std::isfinite(number))
            return Result<FieldAtom>::Failure("HLua", "HLUA_FIELD_VALUE", "F6 字段不是有限数字。");
        return Result<FieldAtom>::Success(number);
    };
    std::size_t bytes = 0;
    for (const auto& [id, section] : parsed.value->fields) {
        if (id.size() != 4 || section.kind != Text::LniDataValue::Kind::Table)
            return Output::Failure("HLua", "HLUA_LNI_OBJECT", "F6 节名不是四字节物体 ID：" + id);
        HslkObject object; object.id = id; object.parent = id;
        object.objectClass = ObjectData::ToString(type); object.source = source;
        for (const auto& [name, value] : section.fields) {
            if (name == "_parent") {
                if (value.kind != Text::LniDataValue::Kind::String || value.text.size() != 4)
                    return Output::Failure("HLua", "HLUA_PARENT_INVALID", "F6 的 _parent 需要四字节文本。");
                object.parent = value.text; continue;
            }
            if (name.starts_with('_')) continue;
            HslkField field; field.name = name;
            field.source = std::string(source) + ':' + std::to_string(1 + std::count(text.begin(),
                text.begin() + std::min(value.offset, text.size()), '\n'));
            field.array = value.kind == Text::LniDataValue::Kind::Table;
            if (field.array) {
                for (std::size_t i = 1; i <= value.fields.size(); ++i) {
                    const auto found = value.fields.find(std::to_string(i));
                    if (found == value.fields.end()) return Output::Failure("HLua", "HLUA_LNI_ARRAY", "F6 数组必须连续，从1开始。");
                    auto item = atom(found->second);
                    if (!item.Succeeded()) return {std::nullopt, std::move(item.diagnostics)};
                    field.values.push_back(std::move(*item.value));
                }
            } else {
                auto item = atom(value);
                if (!item.Succeeded()) return {std::nullopt, std::move(item.diagnostics)};
                field.values.push_back(std::move(*item.value));
            }
            bytes += sizeof(HslkField) + name.size();
            for (const auto& item : field.values) bytes += sizeof(FieldAtom) + (std::holds_alternative<std::string>(item) ? std::get<std::string>(item).size() : 0);
            if (bytes > limits.fieldBytes) return Output::Failure("HLua", "HLUA_FIELD_BUDGET", "F6 字段超过字节预算。");
            object.fields.push_back(std::move(field));
        }
        output.push_back(std::move(object));
    }
    return Output::Success(std::move(output));
}
} // namespace War3Map::HLua
