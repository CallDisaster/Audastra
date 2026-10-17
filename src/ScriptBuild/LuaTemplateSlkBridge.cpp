#include "LuaTemplateSlkBridge.h"

#include "War3MapCore/MapData/MapAuxiliary.h"
#include "War3MapCore/ObjectData/NativeObjectCodec.h"
#include "War3MapCore/Script/SlkDataset.h"
#include "War3MapCore/Text/SlkCells.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <set>

namespace War3Map::Script::detail {
namespace {

using ObjectData::ObjectType;
constexpr std::array<const char*, 7> kClasses{"unit", "ability", "item", "buff", "upgrade", "doodad", "destructable"};
constexpr std::size_t kStrings = 7;
// 这些是 WE 的界面/闪电/命令卡来源，不在 w3x2lni core/info.lua 的 slk/txt/misc
// 读取清单里，目标模板 slk 库不会读取它们。旧图常带这些成员；把它们当成
// “未知物编来源”拒绝会让整张图无法保存，而跳过不会改变 slk 的结果。
constexpr std::array<std::string_view, 3> kSlkUnreadSources{
    "SPLATS\\LIGHTNINGDATA.SLK", "UNITS\\COMMANDFUNC.TXT", "WAR3MAPSKIN.TXT"};
bool SlkUnreadSource(std::string_view name) {
    return std::find(kSlkUnreadSources.begin(), kSlkUnreadSources.end(), name) != kSlkUnreadSources.end();
}

struct NativeMod {
    std::string id;
    lua_Integer level = 0;
    ObjectData::Value value;
};
struct NativeRecord {
    std::string id;
    std::optional<std::string> parent, w2l;
    std::vector<NativeMod> mods;
};

std::string FieldId(std::string id) {
    while (!id.empty() && id.back() == '\0') id.pop_back();
    return id;
}

void PushScalar(lua_State* lua, const SlkSessionScalar& scalar) {
    std::visit([&](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, std::int64_t>) lua_pushinteger(lua, static_cast<lua_Integer>(value));
        else if constexpr (std::is_same_v<T, double>) lua_pushnumber(lua, value);
        else if constexpr (std::is_same_v<T, std::string>) lua_pushlstring(lua, value.data(), value.size());
        else lua_pushboolean(lua, value ? 1 : 0);
    }, scalar);
}

void PushFields(lua_State* lua, const SlkDatasetFields& fields) {
    lua_createtable(lua, 0, static_cast<int>(std::min<std::size_t>(fields.size(), 4096)));
    for (const auto& [key, field] : fields) {
        if (const auto* scalar = std::get_if<SlkSessionScalar>(&field.value)) {
            PushScalar(lua, *scalar);
        } else {
            const auto& list = std::get<std::vector<SlkSessionScalar>>(field.value);
            lua_createtable(lua, static_cast<int>(list.size()), 0);
            for (std::size_t i = 0; i < list.size(); ++i) {
                PushScalar(lua, list[i]);
                lua_rawseti(lua, -2, static_cast<lua_Integer>(i + 1));
            }
        }
        lua_setfield(lua, -2, key.c_str());
    }
}

void PushTable(lua_State* lua, const SlkDatasetTable& table) {
    lua_createtable(lua, 0, static_cast<int>(std::min<std::size_t>(table.size(), 65536)));
    for (const auto& [id, record] : table) {
        PushFields(lua, record.fields);
        lua_setfield(lua, -2, id.c_str());
    }
}

void PushScope(lua_State* lua, const SlkDatasetMetadataScope& scope) {
    lua_createtable(lua, 0, static_cast<int>(std::min<std::size_t>(scope.fields.size(), 4096)));
    for (const auto& [key, record] : scope.fields) {
        PushFields(lua, record.fields);
        lua_setfield(lua, -2, key.c_str());
    }
}

void PushValue(lua_State* lua, const ObjectData::Value& value) {
    if (const auto* integer = std::get_if<std::int32_t>(&value)) lua_pushinteger(lua, *integer);
    else if (const auto* real = std::get_if<float>(&value)) lua_pushnumber(lua, static_cast<lua_Number>(*real));
    else if (const auto* text = std::get_if<std::string>(&value)) lua_pushlstring(lua, text->data(), text->size());
    else lua_pushnil(lua);
}

std::unique_ptr<ObjectData::NativeObjectDocument> ParseNative(ObjectType type, const SlkMapMemberSnapshot& member,
    std::string& error) {
    if (member.identity.presence != SlkSessionSourcePresence::Present) {
        auto empty = ObjectData::NativeObjectDocument::CreateEmpty(type);
        if (!empty.Succeeded() || !empty.value) { error = "无法建立空物编表。"; return nullptr; }
        return std::make_unique<ObjectData::NativeObjectDocument>(std::move(*empty.value));
    }
    auto parsed = ObjectData::NativeObjectCodec(type).Parse(member.bytes);
    auto* native = parsed.Succeeded() ? dynamic_cast<ObjectData::NativeObjectDocument*>(parsed.document.get()) : nullptr;
    if (!native || !native->IsStructurallySafe()) {
        error = "地图物编 " + member.name + " 无法完整读取，模板 slk 库不能使用它。";
        return nullptr;
    }
    parsed.document.release();
    return std::unique_ptr<ObjectData::NativeObjectDocument>(native);
}

// 读取地图物编记录；字符串中的整串 TRIGSTR 引用按 WTS 解析，与 w3x2lni frontend 读取的值一致。
bool ReadNative(ObjectType type, const SlkMapMemberSnapshot& member, const MapData::StringTableDocument* strings,
    std::vector<NativeRecord>& records, std::string& error) {
    auto native = ParseNative(type, member, error);
    if (!native) return false;
    for (const auto& object : native->Objects()) {
        NativeRecord record{object.key.rawcode, object.parentRawcode, std::nullopt, {}};
        for (const auto& modification : object.modifications) {
            NativeMod mod{FieldId(modification.key.fieldId),
                static_cast<lua_Integer>(modification.key.level.value_or(modification.key.variation.value_or(0))),
                modification.value};
            if (auto* text = std::get_if<std::string>(&mod.value); text && strings && text->starts_with("TRIGSTR_")) {
                auto resolved = strings->Resolve(*text);
                if (resolved.Succeeded() && resolved.value) *text = std::move(*resolved.value);
            }
            if (!std::holds_alternative<std::int32_t>(mod.value) && !std::holds_alternative<float>(mod.value)
                && !std::holds_alternative<std::string>(mod.value)) {
                error = "地图物编 " + member.name + " 中对象 " + record.id + " 的字段 " + mod.id + " 值类型无法读取。";
                return false;
            }
            if (mod.id == "W2lo") {
                if (const auto* text = std::get_if<std::string>(&mod.value)) record.w2l = *text;
            }
            record.mods.push_back(std::move(mod));
        }
        records.push_back(std::move(record));
    }
    return true;
}

// war3mapMisc.txt：[节] 键=值，键按 w3x2lni 规则转小写；值保留原文，由 slk 库按 misc metadata 转换。
std::map<std::string, std::map<std::string, std::string>> ParseMiscText(std::span<const std::byte> bytes) {
    std::map<std::string, std::map<std::string, std::string>> sections;
    const std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    std::map<std::string, std::string>* current = nullptr;
    std::size_t at = 0;
    const auto trim = [](std::string_view value) {
        const auto first = value.find_first_not_of(" \t\r");
        if (first == std::string_view::npos) return std::string_view{};
        return value.substr(first, value.find_last_not_of(" \t\r") - first + 1);
    };
    while (at < text.size()) {
        auto end = text.find('\n', at);
        if (end == std::string_view::npos) end = text.size();
        const auto line = trim(text.substr(at, end - at));
        at = end + 1;
        if (line.empty() || line.starts_with("//") || line.starts_with(";")) continue;
        if (line.front() == '[' && line.back() == ']') {
            current = &sections[std::string(trim(line.substr(1, line.size() - 2)))];
            continue;
        }
        const auto equals = line.find('=');
        if (!current || equals == std::string_view::npos) continue;
        std::string key(trim(line.substr(0, equals)));
        for (auto& ch : key) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        auto value = trim(line.substr(equals + 1));
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') value = value.substr(1, value.size() - 2);
        (*current)[key] = std::string(value);
    }
    return sections;
}

// 地图自带的 SLK 表所属类别（w3x2lni frontend 读取的固定表名）。
// 游戏字段存储类型：w3x2lni 由游戏元数据生成的 prebuilt/metadata.ini（[作用域] / [.字段] / type = N）。
// canonical defined/metadata.ini 有少数字段类型与游戏不同（如 AIde 的 Idef 记为 unreal，游戏为 int）；
// 写 native 时按游戏类型，与原版编辑器一致。只取 type，其余定义仍用 canonical。
using GameTypes = std::map<std::string, std::map<std::string, int>>;
std::optional<GameTypes> ReadGameTypes(const std::filesystem::path& path) {
    constexpr std::uintmax_t kMaxBytes = 16u << 20;
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size > kMaxBytes) return std::nullopt;
    std::ifstream file(path, std::ios::binary);
    if (!file) return std::nullopt;
    GameTypes types;
    std::string line, scope, key;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.size() > 2 && line.front() == '[' && line.back() == ']') {
            if (line[1] == '.') key = line.substr(2, line.size() - 3);
            else { scope = line.substr(1, line.size() - 2); key.clear(); }
            continue;
        }
        if (scope.empty() || key.empty() || !line.starts_with("type = ")) continue;
        const auto value = line.substr(7);
        if (value.size() == 1 && value[0] >= '0' && value[0] <= '3') types[scope][key] = value[0] - '0';
    }
    if (file.bad() || types.empty()) return std::nullopt;
    return types;
}

std::optional<std::size_t> SlkClass(std::string_view name) {
    constexpr std::pair<std::string_view, std::size_t> tables[] = {
        {"UNITS\\UNITDATA.SLK", 0}, {"UNITS\\UNITBALANCE.SLK", 0}, {"UNITS\\UNITUI.SLK", 0},
        {"UNITS\\UNITABILITIES.SLK", 0}, {"UNITS\\UNITWEAPONS.SLK", 0}, {"UNITS\\ABILITYDATA.SLK", 1},
        {"UNITS\\ITEMDATA.SLK", 2}, {"UNITS\\ABILITYBUFFDATA.SLK", 3}, {"UNITS\\UPGRADEDATA.SLK", 4},
        {"DOODADS\\DOODADS.SLK", 5}, {"UNITS\\DESTRUCTABLEDATA.SLK", 6}};
    for (const auto& [table, index] : tables) {
        if (table == name) return index;
    }
    return std::nullopt;
}

// 地图 SLK 第一列（标题行之后）的对象 ID。
std::optional<std::vector<std::string>> SlkIds(std::span<const std::byte> bytes) {
    const std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    auto cells = Text::ParseSlkCells(text, Text::SlkCellMode::CatalogStrict);
    if (!cells.Succeeded() || !cells.value) return std::nullopt;
    std::vector<std::string> ids;
    for (const auto& [row, columns] : *cells.value) {
        if (row <= 1) continue;
        const auto first = columns.find(1);
        if (first != columns.end() && !first->second.empty()) ids.push_back(first->second);
    }
    return ids;
}

// units\*.txt 的节名即对象 ID。
std::vector<std::string> TxtIds(std::span<const std::byte> bytes) {
    std::vector<std::string> ids;
    for (const auto& [section, fields] : ParseMiscText(bytes)) ids.push_back(section);
    return ids;
}

std::string Field(lua_State* lua, int index, const char* name) {
    lua_getfield(lua, index, name);
    std::size_t size = 0;
    const char* text = lua_type(lua, -1) == LUA_TSTRING ? lua_tolstring(lua, -1, &size) : nullptr;
    std::string value = text ? std::string(text, size) : std::string{};
    lua_pop(lua, 1);
    return value;
}

lua_Integer Integer(lua_State* lua, int index, const char* name) {
    lua_getfield(lua, index, name);
    const auto value = lua_isinteger(lua, -1) ? lua_tointeger(lua, -1) : 0;
    lua_pop(lua, 1);
    return value;
}

struct GeneratedMod {
    std::string id;
    lua_Integer level = 0, data = 0, type = 0;
    ObjectData::Value value;
};
struct GeneratedObject {
    std::string id, parent;
    std::vector<GeneratedMod> mods;
};
struct ClassWrite {
    bool present = false;
    std::vector<std::string> remove;
    std::vector<GeneratedObject> add;
};

// 只读取 Lua 结果表；不在这里做会抛 C++ 异常的工作以外的事情。
std::string ReadWrites(lua_State* lua, int index, std::array<ClassWrite, 7>& writes) {
    lua_getfield(lua, index, "write");
    const int write = lua_gettop(lua);
    for (std::size_t c = 0; c < kClasses.size(); ++c) {
        lua_getfield(lua, write, kClasses[c]);
        if (lua_istable(lua, -1)) {
            auto& target = writes[c];
            target.present = true;
            const int entry = lua_gettop(lua);
            lua_getfield(lua, entry, "remove");
            for (lua_Integer i = 1; lua_rawgeti(lua, -1, i) == LUA_TSTRING; ++i) {
                target.remove.emplace_back(lua_tostring(lua, -1));
                lua_pop(lua, 1);
            }
            lua_pop(lua, 2);
            lua_getfield(lua, entry, "add");
            for (lua_Integer i = 1; lua_rawgeti(lua, -1, i) == LUA_TTABLE; ++i) {
                const int object = lua_gettop(lua);
                GeneratedObject generated{Field(lua, object, "id"), Field(lua, object, "parent"), {}};
                lua_getfield(lua, object, "mods");
                for (lua_Integer m = 1; lua_rawgeti(lua, -1, m) == LUA_TTABLE; ++m) {
                    const int mod = lua_gettop(lua);
                    GeneratedMod item{Field(lua, mod, "id"), Integer(lua, mod, "level"), Integer(lua, mod, "data"),
                        Integer(lua, mod, "type"), std::int32_t{0}};
                    lua_getfield(lua, mod, "value");
                    if (item.type == 0 && lua_isinteger(lua, -1)) {
                        item.value = static_cast<std::int32_t>(lua_tointeger(lua, -1));
                    } else if ((item.type == 1 || item.type == 2) && lua_type(lua, -1) == LUA_TNUMBER) {
                        item.value = static_cast<float>(lua_tonumber(lua, -1));
                    } else if (item.type == 3 && lua_type(lua, -1) == LUA_TSTRING) {
                        std::size_t size = 0;
                        const char* text = lua_tolstring(lua, -1, &size);
                        item.value = std::string(text, size);
                    } else {
                        lua_pop(lua, 4);
                        return "模板生成的对象 " + generated.id + " 字段 " + item.id + " 的值类型无效。";
                    }
                    lua_pop(lua, 2); // value、mod
                    generated.mods.push_back(std::move(item));
                }
                lua_pop(lua, 3); // 结束标记、mods、object
                target.add.push_back(std::move(generated));
            }
            lua_pop(lua, 3); // 结束标记、add、类别表
        } else {
            lua_pop(lua, 1);
        }
    }
    lua_pop(lua, 1);
    return {};
}

} // namespace

std::string PushSlkData(lua_State* lua, const LuaTemplateSlkSources& sources,
    std::vector<Diagnostic>* diagnostics) {
    if (!sources.map) return "缺少本次保存的地图来源，模板 slk 库不能读取物编。";
    const auto members = sources.map->Members();
    if (members.size() != 9) return "地图来源快照不完整，模板 slk 库不能读取物编。";
    SlkTypedDataset dataset;
    std::array<std::vector<NativeRecord>, 7> natives;
    std::array<std::string, 7> broken; // 读不了的类别：模板用到该类别时才报错
    std::string unsupported;
    std::map<std::string, std::map<std::string, std::string>> misc;
    // 地图自带 SLK/TXT 覆盖：ID 参与占用与重复检查；读取被覆盖对象的字段时由 slk 库报错。
    std::array<std::string, 7> slkOverride;
    std::array<std::vector<std::string>, 7> slkIds;
    std::vector<std::pair<std::string, std::string>> txtOverride; // ID -> 来源成员
    GameTypes gameTypes;
    try {
        const auto& w3i = members[8];
        if (w3i.identity.presence != SlkSessionSourcePresence::Present) return "地图缺少 war3map.w3i，模板 slk 库无法确定数据集。";
        SlkDatasetRequest request;
        request.coreRoot = sources.w3x2lniRoot / "plugin" / "w3x2lni" / "script" / "core";
        request.installedYdweRoot = sources.w3x2lniRoot;
        request.finalW3i = w3i.bytes;
        request.documentRevision = sources.map->Revision();
        auto read = ReadSlkTypedDataset(request);
        if (!read.Succeeded() || !read.value) {
            std::string message = "无法读取 w3x2lni 物编默认数据";
            if (!read.diagnostics.empty()) message += "：" + read.diagnostics.front().message;
            return message;
        }
        dataset = std::move(*read.value);
        const auto gameTypesPath = sources.w3x2lniRoot / "plugin" / "w3x2lni" / "data" / dataset.dataSetId / "prebuilt" / "metadata.ini";
        auto readTypes = ReadGameTypes(gameTypesPath);
        if (!readTypes) return "无法读取游戏字段类型表 " + gameTypesPath.generic_string() + "，模板不能确定物编字段的存储类型。";
        gameTypes = std::move(*readTypes);
        std::optional<MapData::StringTableDocument> strings;
        if (members[kStrings].identity.presence == SlkSessionSourcePresence::Present) {
            auto parsed = MapData::ParseStringTable(members[kStrings].bytes);
            if (!parsed.Succeeded() || !parsed.value) return "地图字符串表 war3map.wts 无法读取。";
            strings = std::move(*parsed.value);
        }
        for (std::size_t c = 0; c < natives.size(); ++c) {
            std::string error;
            if (ReadNative(static_cast<ObjectType>(c), members[c], strings ? &*strings : nullptr, natives[c], error)) continue;
            // 没有 W2lo 字段 ID 的字节就不可能有上次模板生成的对象，未用到该类别的模板可以继续。
            const std::string_view raw(reinterpret_cast<const char*>(members[c].bytes.data()), members[c].bytes.size());
            if (raw.find("W2lo") != std::string_view::npos) return error;
            natives[c].clear();
            broken[c] = error;
        }
        std::vector<std::string> others;
        for (const auto& name : sources.map->UnsupportedSources()) {
            if (SlkUnreadSource(name)) {
                if (diagnostics) diagnostics->push_back({DiagnosticSeverity::Warning, "Script.LuaTemplate",
                    "TEMPLATE_SLK_SOURCE_IGNORED", "模板 slk 库不读取该来源，已跳过：" + std::string(name), {}});
                continue;
            }
            const auto slkClass = SlkClass(name);
            const bool txt = name.starts_with("UNITS\\") && name.ends_with(".TXT");
            if (name != "WAR3MAPMISC.TXT" && !slkClass && !txt) { others.push_back(name); continue; }
            const auto found = sources.map->LookupMember(name);
            if (!found.Succeeded() || !found.value || found.value->state != SlkMapSourceLookupState::Captured || !found.value->member) {
                others.push_back(name);
                continue;
            }
            const auto& bytes = found.value->member->bytes;
            if (name == "WAR3MAPMISC.TXT") {
                misc = ParseMiscText(bytes);
            } else if (slkClass) {
                auto ids = SlkIds(bytes);
                if (!ids) { others.push_back(name); continue; }
                slkOverride[*slkClass] = name;
                slkIds[*slkClass].insert(slkIds[*slkClass].end(), ids->begin(), ids->end());
            } else {
                for (auto& id : TxtIds(bytes)) txtOverride.emplace_back(std::move(id), name);
            }
        }
        if (!others.empty()) {
            unsupported = "地图里有模板 slk 库尚未支持的物编来源（";
            for (std::size_t i = 0; i < others.size() && i < 5; ++i) unsupported += (i ? "、" : "") + others[i];
            unsupported += "），不能保证读到的值与游戏一致，已停止。";
        }
        // 未列名成员不影响：物编来源只按固定文件名读取，快照已逐个精确查询这些名字。
    } catch (const std::exception&) {
        return "读取模板 slk 数据时资源不足。";
    }

    lua_createtable(lua, 0, 5);
    lua_createtable(lua, 0, 8);
    for (std::size_t c = 0; c < kClasses.size(); ++c) {
        PushScope(lua, dataset.metadata.common[c]);
        lua_setfield(lua, -2, kClasses[c]);
    }
    PushScope(lua, dataset.metadata.misc);
    lua_setfield(lua, -2, "misc");
    lua_setfield(lua, -2, "meta");
    lua_createtable(lua, 0, static_cast<int>(std::min<std::size_t>(dataset.metadata.byCode.size(), 65536)));
    for (const auto& [code, scope] : dataset.metadata.byCode) {
        PushScope(lua, scope);
        lua_setfield(lua, -2, code.c_str());
    }
    lua_setfield(lua, -2, "codemeta");
    lua_createtable(lua, 0, static_cast<int>(std::min<std::size_t>(gameTypes.size(), 65536)));
    for (const auto& [scope, fields] : gameTypes) {
        lua_createtable(lua, 0, static_cast<int>(std::min<std::size_t>(fields.size(), 65536)));
        for (const auto& [field, type] : fields) {
            lua_pushinteger(lua, type);
            lua_setfield(lua, -2, field.c_str());
        }
        lua_setfield(lua, -2, scope.c_str());
    }
    lua_setfield(lua, -2, "nativetype");
    lua_createtable(lua, 0, 8);
    for (std::size_t c = 0; c < kClasses.size(); ++c) {
        PushTable(lua, dataset.defaults[c]);
        lua_setfield(lua, -2, kClasses[c]);
    }
    PushTable(lua, dataset.misc);
    lua_setfield(lua, -2, "misc");
    lua_setfield(lua, -2, "default");
    lua_createtable(lua, 0, 7);
    for (std::size_t c = 0; c < kClasses.size(); ++c) {
        lua_createtable(lua, static_cast<int>(natives[c].size()), 0);
        for (std::size_t r = 0; r < natives[c].size(); ++r) {
            const auto& record = natives[c][r];
            lua_createtable(lua, 0, 4);
            lua_pushlstring(lua, record.id.data(), record.id.size());
            lua_setfield(lua, -2, "id");
            if (record.parent) {
                lua_pushlstring(lua, record.parent->data(), record.parent->size());
                lua_setfield(lua, -2, "parent");
            }
            if (record.w2l) {
                lua_pushlstring(lua, record.w2l->data(), record.w2l->size());
                lua_setfield(lua, -2, "w2l");
            }
            lua_createtable(lua, static_cast<int>(record.mods.size()), 0);
            for (std::size_t m = 0; m < record.mods.size(); ++m) {
                const auto& mod = record.mods[m];
                lua_createtable(lua, 0, 3);
                lua_pushlstring(lua, mod.id.data(), mod.id.size());
                lua_setfield(lua, -2, "id");
                lua_pushinteger(lua, mod.level);
                lua_setfield(lua, -2, "level");
                PushValue(lua, mod.value);
                lua_setfield(lua, -2, "value");
                lua_rawseti(lua, -2, static_cast<lua_Integer>(m + 1));
            }
            lua_setfield(lua, -2, "mods");
            lua_rawseti(lua, -2, static_cast<lua_Integer>(r + 1));
        }
        lua_setfield(lua, -2, kClasses[c]);
    }
    lua_setfield(lua, -2, "native");
    lua_createtable(lua, 0, 0);
    for (std::size_t c = 0; c < kClasses.size(); ++c) {
        if (broken[c].empty()) continue;
        lua_pushlstring(lua, broken[c].data(), broken[c].size());
        lua_setfield(lua, -2, kClasses[c]);
    }
    lua_setfield(lua, -2, "broken");
    lua_createtable(lua, 0, 0);
    for (std::size_t c = 0; c < kClasses.size(); ++c) {
        if (slkOverride[c].empty()) continue;
        lua_pushlstring(lua, slkOverride[c].data(), slkOverride[c].size());
        lua_setfield(lua, -2, kClasses[c]);
    }
    lua_setfield(lua, -2, "slkoverride");
    lua_createtable(lua, 0, 0);
    for (std::size_t c = 0; c < kClasses.size(); ++c) {
        lua_createtable(lua, 0, static_cast<int>(std::min<std::size_t>(slkIds[c].size(), 65536)));
        for (const auto& id : slkIds[c]) {
            lua_pushboolean(lua, 1);
            lua_setfield(lua, -2, id.c_str());
        }
        lua_setfield(lua, -2, kClasses[c]);
    }
    lua_setfield(lua, -2, "slkids");
    lua_createtable(lua, 0, static_cast<int>(std::min<std::size_t>(txtOverride.size(), 65536)));
    for (const auto& [id, member] : txtOverride) {
        lua_pushlstring(lua, member.data(), member.size());
        lua_setfield(lua, -2, id.c_str());
    }
    lua_setfield(lua, -2, "txtoverride");
    lua_createtable(lua, 0, static_cast<int>(misc.size()));
    for (const auto& [section, fields] : misc) {
        lua_createtable(lua, 0, static_cast<int>(fields.size()));
        for (const auto& [key, value] : fields) {
            lua_pushlstring(lua, value.data(), value.size());
            lua_setfield(lua, -2, key.c_str());
        }
        lua_setfield(lua, -2, section.c_str());
    }
    lua_setfield(lua, -2, "miscmap");
    if (!unsupported.empty()) {
        lua_pushlstring(lua, unsupported.data(), unsupported.size());
        lua_setfield(lua, -2, "unsupported");
    }
    return {};
}

std::string ApplySlkResult(lua_State* lua, int index, const LuaTemplateSlkSources& sources,
    TemplateTransactionOutput& output) {
    std::array<ClassWrite, 7> writes;
    if (auto error = ReadWrites(lua, index, writes); !error.empty()) return error;
    try {
        const auto members = sources.map->Members();
        for (std::size_t c = 0; c < writes.size(); ++c) {
            const auto& write = writes[c];
            if (!write.present) continue;
            const auto type = static_cast<ObjectType>(c);
            std::string error;
            auto native = ParseNative(type, members[c], error);
            if (!native) return error;
            if (!native->Supports(ObjectData::NativeObjectOperation::EditStructure))
                return "地图物编 " + members[c].name + " 的格式不能写入新对象。";
            const auto keyOf = [type](const auto& mod) {
                ObjectData::ModificationKey key;
                key.fieldId = mod.id;
                key.fieldId.resize(4, '\0');
                if (type == ObjectType::Ability || type == ObjectType::Upgrade) {
                    key.level = static_cast<std::uint32_t>(mod.level);
                    key.dataIndex = static_cast<std::uint32_t>(mod.data);
                } else if (type == ObjectType::Doodad) {
                    key.variation = static_cast<std::uint32_t>(mod.level);
                    key.dataIndex = static_cast<std::uint32_t>(mod.data);
                }
                return key;
            };
            const auto failed = [](std::string message, const Status& status) {
                if (!status.diagnostics.empty()) message += "：" + status.diagnostics.front().message;
                return message;
            };
            // 上次生成、本次仍以同一 ID 和父对象生成的对象就地更新：未变字段保留原字节（含结束标记），
            // 与编辑器把差异应用到原成员的方式一致。其余旧对象删除，新对象追加。
            std::set<std::string> updated;
            for (const auto& object : write.add) {
                if (std::find(write.remove.begin(), write.remove.end(), object.id) == write.remove.end()) continue;
                const auto* existing = native->FindObject({type, object.id});
                if (!existing || existing->parentRawcode != object.parent) continue;
                updated.insert(object.id);
            }
            for (const auto& id : write.remove) {
                if (updated.contains(id)) continue;
                const auto status = native->DeleteCustomObject({type, id});
                if (!status.Succeeded()) return "无法移除上次模板生成的对象 " + id + "。";
            }
            for (const auto& object : write.add) {
                for (const auto& mod : object.mods) {
                    if (mod.id.empty() || mod.id.size() > 4) return "模板对象 " + object.id + " 的字段 ID 无效：" + mod.id;
                }
                const ObjectData::ObjectKey objectKey{type, object.id};
                if (updated.contains(object.id)) {
                    std::vector<ObjectData::ModificationKey> stale;
                    for (const auto& old : native->FindObject(objectKey)->modifications) {
                        if (std::none_of(object.mods.begin(), object.mods.end(),
                                [&](const auto& mod) { return keyOf(mod) == old.key; }))
                            stale.push_back(old.key);
                    }
                    for (const auto& key : stale) {
                        const auto status = native->RemoveModification(objectKey, key);
                        if (!status.Succeeded()) return failed("无法移除模板对象 " + object.id + " 的旧字段 " + key.fieldId, status);
                    }
                } else if (!native->AddCustomObject(object.id, object.parent).Succeeded()) {
                    return "无法新建模板对象 " + object.id + "（父对象 " + object.parent + "）。";
                }
                for (const auto& mod : object.mods) {
                    const auto key = keyOf(mod);
                    const auto valueType = static_cast<ObjectData::NativeValueType>(mod.type);
                    const auto& current = native->FindObject(objectKey)->modifications;
                    const auto old = std::find_if(current.begin(), current.end(), [&](const auto& m) { return m.key == key; });
                    Status status;
                    if (old == current.end()) {
                        status = native->AddModification(objectKey, key, valueType, mod.value);
                    } else if (old->value != mod.value) {
                        status = native->SetModification(objectKey, key, mod.value);
                        // 存储类型不同（值类型改变）时只能替换该字段。
                        if (!status.Succeeded()) {
                            status = native->RemoveModification(objectKey, key);
                            if (status.Succeeded()) status = native->AddModification(objectKey, key, valueType, mod.value);
                        }
                    }
                    if (!status.Succeeded()) return failed("无法写入模板对象 " + object.id + " 的字段 " + mod.id, status);
                }
            }
            auto bytes = ObjectData::NativeObjectCodec(type).Serialize(*native);
            if (!bytes.Succeeded() || !bytes.value) return "无法写出模板改动后的 " + members[c].name + "。";
            output.objectMembers.push_back({std::string(ObjectData::NativeFileName(type)), std::move(*bytes.value)});
        }
    } catch (const std::exception&) {
        return "写出模板物编时资源不足。";
    }
    return {};
}

} // namespace War3Map::Script::detail
