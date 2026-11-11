#include "War3MapCore/Lua/LuaModuleScan.h"
#include "LuaCodeModelInternal.h"
#include <stdexcept>

namespace War3Map::Lua {
JapiLookupScan ScanJapiNativeLookups(std::string_view source, std::size_t maxTokens) {
    static const auto catalog = BuildOwnedRuntimeCatalog({});
    JapiLookupScan output;
    Detail::ParseOptions options;
    options.catalog = catalog.get();
    options.maxTokens = maxTokens;
    options.includeComments = false;
    options.japiLookup = [&](auto name, auto line) { output.lookups.push_back({std::string(name), line}); };
    try {
        const auto model = Detail::ParseModel(source, options);
        output.complete = !model->recovered;
    } catch (const std::length_error&) {
        output.complete = false;
    }
    return output;
}
std::vector<ExternalCapability> ScanLiteralMemberLookups(std::string_view source,std::string_view table) {
    const auto tokens=Detail::Tokenize(source,false,4*1024*1024);
    std::vector<ExternalCapability> output;
    for(std::size_t i=0;i+2<tokens.size();++i) if(tokens[i].kind==Detail::TokenKind::Name && tokens[i].Text(source)==table) {
        if(tokens[i+1].Text(source)=="." && tokens[i+2].kind==Detail::TokenKind::Name) output.push_back({std::string(tokens[i+2].Text(source)),tokens[i].line});
        else if(i+3<tokens.size() && tokens[i+1].Text(source)=="[" && tokens[i+2].kind==Detail::TokenKind::String && !tokens[i+2].unterminated
            && tokens[i+3].Text(source)=="]") output.push_back({Detail::StringValue(tokens[i+2].Text(source)),tokens[i].line});
    }
    return output;
}
std::vector<ExternalCapability> ScanLiteralCallArguments(std::string_view source,std::string_view table,std::string_view method) {
    const auto tokens=Detail::Tokenize(source,false,4*1024*1024);
    std::vector<ExternalCapability> output;
    for(std::size_t i=0;i+4<tokens.size();++i) if(tokens[i].kind==Detail::TokenKind::Name && tokens[i].Text(source)==table
        && tokens[i+1].Text(source)=="." && tokens[i+2].Text(source)==method && tokens[i+3].Text(source)=="("
        && tokens[i+4].kind==Detail::TokenKind::String && !tokens[i+4].unterminated)
        output.push_back({Detail::StringValue(tokens[i+4].Text(source)),tokens[i].line});
    return output;
}
std::vector<ExternalCapability> ScanExternalCapabilities(std::string_view source) {
    const auto tokens = Detail::Tokenize(source, false, 4 * 1024 * 1024);
    std::vector<ExternalCapability> output;
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        if (tokens[i].kind != Detail::TokenKind::Name) continue;
        const auto name = tokens[i].Text(source);
        if (name == "io") output.push_back({"io", tokens[i].line});
        if ((name != "os" && name != "package") || i + 2 >= tokens.size()) continue;
        const auto separator = tokens[i + 1].Text(source);
        const auto member = tokens[i + 2].kind == Detail::TokenKind::String
            ? Detail::StringValue(tokens[i + 2].Text(source)) : std::string(tokens[i + 2].Text(source));
        if ((separator == "." || separator == "[") && ((name == "os" && member == "execute") || (name == "package" && member == "loadlib")))
            output.push_back({std::string(name) + '.' + member, tokens[i].line});
    }
    return output;
}
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
