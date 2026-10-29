#include "YDTriggerStorage.h"

#include "War3MapCore/Script/ScriptPipeline.h"

#include <array>
#include <cstdio>

namespace War3Map::Script::detail {
namespace {


bool IdentifierChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

} // namespace


std::string StorageKey(std::string_view name) {
    // 名称写在宏的字符串参数里，YDWE 由模板按 Lua 字符串求 StringHash：先按同样规则处理转义。
    std::string text;
    text.reserve(name.size());
    for (std::size_t i = 0; i < name.size(); ++i) {
        char c = name[i];
        if (c == '\\' && i + 1 < name.size()) {
            switch (name[++i]) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case '0': c = '\0'; break;
            default: c = name[i]; break;
            }
        }
        text.push_back(c);
    }
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "0x%08X", StormStringHash(text));
    return buffer;
}

std::string LocalPage(int page) {
    switch (page) {
    case 1: return "GetHandleId(GetTriggeringTrigger())*ydl_localvar_step";
    case 2: return "GetHandleId(GetTriggeringTrigger())*LoadInteger(YDLOC, GetHandleId(GetTriggeringTrigger()), 0xECE825E7)";
    case 3: return "GetHandleId(GetExpiredTimer())";
    case 4: return "GetHandleId(GetTriggeringTrigger())";
    default: return "ydl_triggerstep";
    }
}

std::string HandlePage(std::string_view handle) {
    return "GetHandleId(" + std::string(handle) + ")";
}

TableWrap UserDataTableWrap(std::string_view type) {
    // 布尔值、角度与文件路径类型在 YDWE 头文件中没有可用的转换（展开为未定义名称），按未知类型处理。
    const auto storage = StorageFor(type, false);
    if (!storage.known || type == "boolean" || type == "radian" || type == "degree" || type == "imagefile" ||
        type == "modelfile" || type == "StringExt") {
        return {"YDTRIGGER_ANY2I_" + std::string(type) + "(", ")", false};
    }
    if (storage.have != "HaveSavedHandle") {
        if (type == "real") return {"R2I(", ")", true};
        if (type == "string") return {"StringHash(", ")", true};
        return {"", "", true};
    }
    return {"GetHandleId(", ")", true};
}

std::span<const std::string_view> LocalInitializeLines() {
    static constexpr std::string_view kLines[] = {
        "local integer ydl_localvar_step = LoadInteger(YDLOC, GetHandleId(GetTriggeringTrigger()), 0xCFDE6C76)",
        "set ydl_localvar_step = ydl_localvar_step + 3",
        "call SaveInteger(YDLOC, GetHandleId(GetTriggeringTrigger()), 0xCFDE6C76, ydl_localvar_step)",
        "call SaveInteger(YDLOC, GetHandleId(GetTriggeringTrigger()), 0xECE825E7, ydl_localvar_step)",
    };
    return kLines;
}

std::string LocalResetCall() {
    return "call SaveInteger(YDLOC, GetHandleId(GetTriggeringTrigger()), 0xECE825E7, ydl_localvar_step)";
}

std::string LocalExecuteTrigger(std::string_view trigger) {
    const std::string id = HandlePage(trigger);
    return "set ydl_triggerstep = " + id + "*(LoadInteger(YDLOC, " + id + ", 0xCFDE6C76) + 3)";
}

std::string_view ObjectPropertyTable(std::string_view objectType) {
    static constexpr std::pair<std::string_view, std::string_view> kTables[] = {
        {"YDWE_OBJECT_TYPE_ABILITY", "ability"}, {"YDWE_OBJECT_TYPE_BUFF", "buff"},
        {"YDWE_OBJECT_TYPE_UNIT", "unit"}, {"YDWE_OBJECT_TYPE_ITEM", "item"},
        {"YDWE_OBJECT_TYPE_UPGRADE", "upgrade"}, {"YDWE_OBJECT_TYPE_DOODAD", "doodad"},
        {"YDWE_OBJECT_TYPE_DESTRUCTABLE", "destructable"},
    };
    for (const auto& [constant, table] : kTables) {
        if (constant == objectType) return table;
    }
    return {};
}

std::string ReplaceIdentifier(std::string_view line, std::string_view name, std::string_view value) {
    std::string result;
    result.reserve(line.size());
    std::size_t i = 0;
    while (i < line.size()) {
        const char c = line[i];
        if (c == '"' || c == '\'') {
            // 字符串与四字符码：原样复制到配对的引号（反斜杠转义下一个字符）。
            const std::size_t start = i++;
            while (i < line.size() && line[i] != c) i += line[i] == '\\' && i + 1 < line.size() ? 2 : 1;
            i = i < line.size() ? i + 1 : i;
            result.append(line.substr(start, i - start));
        } else if (c == '/' && i + 1 < line.size() && line[i + 1] == '/') {
            result.append(line.substr(i));
            break;
        } else if (IdentifierChar(c)) {
            const std::size_t start = i;
            while (i < line.size() && IdentifierChar(line[i])) ++i;
            const auto word = line.substr(start, i - start);
            const bool identifier = !(word[0] >= '0' && word[0] <= '9');
            result.append(identifier && word == name ? value : word);
        } else {
            result += c;
            ++i;
        }
    }
    return result;
}

} // namespace War3Map::Script::detail
