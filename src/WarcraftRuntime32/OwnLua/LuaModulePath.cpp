#include "LuaModulePath.h"
#include "War3MapCore/Runtime/LuaTestMailboxValidation.h"

namespace Audastra::Runtime32::OwnLua {
std::string LuaModuleMember(std::string_view name) {
    if (name.empty() || name.size() > 240 || !LuaMailboxUtf8(name)) return {};
    std::string result;
    bool separator = true;
    for (const auto ch : name) {
        if (ch == '.' || ch == '/' || ch == '\\') {
            if (separator) return {};
            result += '\\'; separator = true;
        } else {
            if (static_cast<unsigned char>(ch) < 0x20 || ch == 0x7f || ch == ':' || ch == '*' || ch == '?'
                || ch == '"' || ch == '<' || ch == '>' || ch == '|') return {};
            result += ch; separator = false;
        }
    }
    if (separator) return {};
    result += ".lua";
    return result;
}
}
