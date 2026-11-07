#include "War3MapCore/Lua/LuaModuleScan.h"
#include "LuaLexer.h"

namespace War3Map::Lua {
std::vector<EmptyTableInitializer> FindEmptyTableInitializers(std::string_view source, std::string_view variable) {
    const auto tokens = Detail::Tokenize(source, false, 4 * 1024 * 1024);
    std::vector<EmptyTableInitializer> output;
    for (std::size_t i = 0; i + 3 < tokens.size(); ++i) {
        if (tokens[i].kind != Detail::TokenKind::Name || tokens[i].Text(source) != variable
            || tokens[i + 1].Text(source) != "=" || tokens[i + 2].Text(source) != "{"
            || tokens[i + 3].Text(source) != "}") continue;
        if (i && (tokens[i - 1].Text(source) == "." || tokens[i - 1].Text(source) == ":")) continue;
        output.push_back({tokens[i + 2].begin, tokens[i + 3].end});
    }
    return output;
}
std::vector<ModuleRequire> ScanModuleRequires(std::string_view source, std::size_t maxTokens) {
    const auto tokens = Detail::Tokenize(source, false, maxTokens);
    std::vector<ModuleRequire> output;
    for (std::size_t i = 0; i + 1 < tokens.size(); ++i) {
        if (tokens[i].kind != Detail::TokenKind::Name || tokens[i].Text(source) != "require") continue;
        if (i && (tokens[i - 1].Text(source) == "." || tokens[i - 1].Text(source) == ":")) continue;
        auto next = i + 1;
        if (tokens[next].Text(source) == "(" && next + 1 < tokens.size()) ++next;
        const bool literal = tokens[next].kind == Detail::TokenKind::String && !tokens[next].unterminated;
        output.push_back({literal ? Detail::StringValue(tokens[next].Text(source)) : std::string{},
            tokens[i].line, !literal});
    }
    return output;
}
} // namespace War3Map::Lua
