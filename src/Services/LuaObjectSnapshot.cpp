#include "War3MapCore/Services/LuaObjectSnapshot.h"
#include "War3MapCore/Services/ObjectCommands.h"
#include "War3MapCore/ObjectData/ProfileList.h"
#include "War3MapCore/Runtime/LuaObjectSnapshot.h"
#include "War3MapCore/Text/TextUtil.h"
#include <algorithm>
#include <set>
#include <stdexcept>

namespace War3Map::Services {
namespace {
using namespace ObjectData;
std::string Quote(std::string_view text) {
    std::string output = "\"";
    for (unsigned char c : text) {
        if (c == '"' || c == '\\') { output += '\\'; output += static_cast<char>(c); }
        else if (c >= 32 && c < 127) output += static_cast<char>(c);
        else { output += '\\'; output += static_cast<char>('0' + c / 100); output += static_cast<char>('0' + (c / 10) % 10); output += static_cast<char>('0' + c % 10); }
    }
    return output + '"';
}
std::string FieldName(const ObjectFieldMatch& match) {
    const auto& field = *match.definition;
    std::string name = field.sourceField;
    if (Text::LowerAscii(field.sourceTable) == "profile") {
        const auto level = match.modification.level.value_or(0);
        if (field.repeat > 0 && field.appendIndex && level > 1) name += std::to_string(level - 1);
    } else {
        if (field.data > 0) name += static_cast<char>('A' + field.data - 1);
        const auto level = match.modification.level.value_or(match.modification.variation.value_or(0));
        if (field.repeat > 0) { if (field.objectType == ObjectType::Doodad && level < 10) name += '0'; name += std::to_string(level); }
    }
    return name;
}
}
Result<std::vector<std::byte>> BuildLuaObjectSnapshot(const LuaObjectSnapshotSources& sources) {
    using Output = Result<std::vector<std::byte>>;
    if (!sources.schema || !sources.standard) return Output::Failure("Lua.ObjectSnapshot", "LUA_SLK_SOURCE_MISSING", "缺少冻结的物编字段和有效目录。");
    try {
        std::string output = "return {\n";
        const auto append = [&](std::string_view value) {
            if (value.size() > Audastra::Runtime32::kLuaObjectSnapshotBytes - std::min(output.size(), Audastra::Runtime32::kLuaObjectSnapshotBytes))
                throw std::length_error("只读物编资料超过16 MiB。");
            output += value;
        };
        for (unsigned index = 0; index < 7; ++index) {
            const auto type = static_cast<ObjectType>(index);
            std::set<std::string> ids;
            for (const auto& object : sources.standard->Entries()) if (object.key.type == type) ids.insert(object.key.rawcode);
            if (sources.documents[index]) for (const auto& object : sources.documents[index]->Objects()) ids.insert(object.key.rawcode);
            append("[" + Quote(ToString(type)) + "]={\n");
            ObjectCommandContext context; context.type = type; context.schema = sources.schema; context.standard = sources.standard;
            context.document = sources.documents[index]; context.documents = sources.documents;
            std::map<std::string, std::string> caseNames;
            for (const auto& field : sources.schema->Definitions()) if (field.objectType == type) caseNames[Text::LowerAscii(field.sourceField)] = field.sourceField;
            for (const auto& id : ids) {
                std::string base = id; std::set<std::string> visited;
                for (;;) {
                    if (!visited.insert(base).second || visited.size() > 128) throw std::runtime_error("只读物编父链循环或过长：" + id);
                    const auto* record = context.document ? context.document->FindObject({type, base}) : nullptr;
                    if (!record || !record->parentRawcode) break;
                    base = *record->parentRawcode;
                }
                const auto* parent = sources.standard->Find(type, base);
                if (!parent) throw std::runtime_error("只读物编缺少标准父对象：" + base);
                ObjectFieldContext fieldContext; fieldContext.rawcode = base; fieldContext.baseCode = std::string(parent->Field("code"));
                fieldContext.item = type == ObjectType::Item;
                if (type == ObjectType::Unit) { fieldContext.hero = parent->category == "hero"; fieldContext.building = parent->category == "building"; }
                std::map<std::string, std::string> values;
                for (const auto& [key, value] : parent->fields) {
                    const auto match = sources.schema->MatchSourceField(type, key, fieldContext);
                    values[match.definition ? FieldName(match) : caseNames.contains(key) ? caseNames.at(key) : key] = value;
                }
                auto fields = ReadObjectFields(context, id);
                if (!fields.Succeeded()) return {std::nullopt, std::move(fields.diagnostics)};
                for (const auto& row : *fields.value) {
                    if (!row.value) continue;
                    if (!row.definition || std::holds_alternative<UnknownBytesValue>(*row.value)) throw std::runtime_error("只读物编含未解释字段：" + id + '/' + row.key.fieldId);
                    const auto match = sources.schema->MatchNativeField(type, row.key, fieldContext);
                    if (!match.definition) throw std::runtime_error("只读物编字段地址未确认：" + id + '/' + row.key.fieldId);
                    auto name = FieldName(match);
                    auto value = ValueToDisplayString(*row.value);
                    const bool grouped = row.definition->repeat > 0 || std::any_of(sources.schema->Definitions().begin(), sources.schema->Definitions().end(), [&](const auto& definition) {
                        return definition.objectType == type && definition.sourceField == row.definition->sourceField && definition.index > 0;
                    });
                    if (match.sourceIndex && grouped) {
                        auto list = SlkText::SplitProfileList(values[name]);
                        if (*match.sourceIndex >= list.size()) {
                            if (*match.sourceIndex != list.size()) throw std::runtime_error("只读 profile 缺少分量来源：" + id + '/' + name);
                            list.resize(list.size() + 1);
                        }
                        auto token = SlkText::ProfileItemToken(value);
                        if (!token) throw std::runtime_error("只读 profile 值无法编码：" + id + '/' + name);
                        list[*match.sourceIndex] = *token; value = SlkText::JoinProfileList(list);
                    }
                    values[std::move(name)] = std::move(value);
                }
                append("[" + Quote(id) + "]={");
                for (const auto& [name, value] : values) append("[" + Quote(name) + "]=" + Quote(value) + ',');
                append("},\n");
            }
            append("},\n");
        }
        append("misc={\n");
        for (const auto& [section, fields] : sources.misc) {
            append("[" + Quote(section) + "]={");
            for (const auto& [key, value] : fields) append("[" + Quote(key) + "]=" + Quote(value) + ',');
            append("},\n");
        }
        append("}}\n");
        return Output::Success({reinterpret_cast<const std::byte*>(output.data()), reinterpret_cast<const std::byte*>(output.data() + output.size())});
    } catch (const std::exception& e) { return Output::Failure("Lua.ObjectSnapshot", "LUA_SLK_SNAPSHOT_INCOMPLETE", e.what()); }
}
} // namespace War3Map::Services
