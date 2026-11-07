#include "War3MapCore/HLua/Project.h"
#include "War3MapCore/Jass/ByteLexical.h"
#include "War3MapCore/Lua/LuaModuleScan.h"
#include "War3MapCore/Runtime/LuaModuleNames.h"
#include <algorithm>
#include <set>

namespace War3Map::HLua {
Result<ProjectSnapshot> HLuaProjectScanner::Capture(const std::filesystem::path& root) {
    auto files = Services::CaptureLuaFolder(root, true, true);
    if (!files.Succeeded()) return {std::nullopt, std::move(files.diagnostics)};
    if (files.value->missing || files.value->skippedLinks)
        return Result<ProjectSnapshot>::Failure("HLua", "HLUA_PROJECT_UNSAFE", "工程不存在或包含链接，无法冻结完整来源。");
    if (!FindLuaFile(*files.value, "main.lua"))
        return Result<ProjectSnapshot>::Failure("HLua", "HLUA_MAIN_MISSING", "工程根目录没有 main.lua。");
    ProjectSnapshot result; result.source = std::move(*files.value);
    std::set<std::string> modules;
    for (std::size_t i = 0; i < result.source.files.size(); ++i) {
        const auto& file = result.source.files[i];
        auto key = file.path;
        for (auto& c : key) { if (c == '\\') c = '/'; else if (c >= 'A' && c <= 'Z') c += 'a' - 'A'; }
        if (key.starts_with("map/")) continue;
        if (key.starts_with("hslk/")) result.hslkFiles.push_back(i);
        else {
            if (key == "h-lua.lua" || key.starts_with("h-lua-") || key.starts_with("project-slk/")
                || key.starts_with("const/") || key.starts_with("foundation/") || key.starts_with("builtin/")
                || key.starts_with("lib/"))
                return Result<ProjectSnapshot>::Failure("HLua", "HLUA_MODULE_COLLISION", "作者模块占用了框架成员名：" + file.path);
            result.runtimeFiles.push_back(i);
        }
        for (const auto& item : Lua::ScanModuleRequires(LuaText(file))) {
            if (!item.dynamic && Audastra::Runtime32::LuaModuleMember(item.name).empty())
                return Result<ProjectSnapshot>::Failure("HLua","HLUA_MODULE_NAME",file.path+':'+std::to_string(item.line)+"：require 模块名不是安全的 UTF-8 相对名字。");
            if (!item.dynamic && item.name.starts_with("jass.")) modules.insert(item.name);
        }
    }
    result.jassModules.assign(modules.begin(), modules.end());
    return Result<ProjectSnapshot>::Success(std::move(result));
}

Result<std::string> InsertLuaEntry(std::string_view source, std::string_view module) {
    if (Audastra::Runtime32::LuaModuleMember(module).empty() || module.find_first_of("\\\"") != module.npos)
        return Result<std::string>::Failure("HLua", "HLUA_ENTRY_INVALID", "入口模块名无效。");
    if (source.size() > 64u * 1024u * 1024u)
        return Result<std::string>::Failure("HLua", "HLUA_SCRIPT_BUDGET", "地图脚本超过64 MiB。");
    struct Token { std::size_t begin, end; };
    std::vector<Token> tokens;
    for (std::size_t at = 0; at < source.size();) {
        const auto c = source[at];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { ++at; continue; }
        if (Jass::IsSourceControlByte(c))
            return Result<std::string>::Failure("HLua", "HLUA_SCRIPT_LEXICAL", "地图脚本含控制字节。");
        if (const auto opaque = Jass::ScanByteOpaque(source, at)) {
            if (!opaque->closed) return Result<std::string>::Failure("HLua", "HLUA_SCRIPT_LEXICAL", "地图脚本的注释或文字没有闭合。");
            if (c == '"' || c == '\'') tokens.push_back({at, opaque->end});
            at = opaque->end; continue;
        }
        const auto start = at++;
        const auto name = [](unsigned char v) { return (v >= 'A' && v <= 'Z') || (v >= 'a' && v <= 'z') || (v >= '0' && v <= '9') || v == '_'; };
        if (name(c)) while (at < source.size() && name(source[at])) ++at;
        tokens.push_back({start, at});
        if (tokens.size() > 16u * 1024u * 1024u)
            return Result<std::string>::Failure("HLua", "HLUA_SCRIPT_BUDGET", "地图脚本词法记号超过预算。");
    }
    const auto text = [&](std::size_t at) { return source.substr(tokens[at].begin, tokens[at].end - tokens[at].begin); };
    std::vector<std::size_t> insertions;
    std::size_t calls = 0;
    for (std::size_t i = 0; i + 3 < tokens.size(); ++i) {
        if (text(i) != "call" || text(i + 1) != "InitGlobals" || text(i + 2) != "(" || text(i + 3) != ")") continue;
        ++calls;
        bool existing = false;
        if (i + 8 < tokens.size() && text(i + 4) == "call" && text(i + 5) == "Cheat" && text(i + 6) == "(" && text(i + 8) == ")") {
            const auto decoded = Jass::DecodeStringBytes(text(i + 7));
            if (decoded) {
                const auto entry = Audastra::Runtime32::ParseLuaEntryCommand(*decoded);
                existing = entry.recognized && entry.error.empty() && entry.module == module;
            }
        }
        if (!existing) insertions.push_back(tokens[i + 3].end);
    }
    if (!calls) return Result<std::string>::Failure("HLua", "HLUA_INIT_GLOBALS_MISSING", "地图脚本没有 call InitGlobals()，未写出不执行的入口。");
    std::string result(source);
    const std::string line = "\n    call Cheat(\"exec-lua: \\\"" + std::string(module) + "\\\"\")\n";
    for (auto it = insertions.rbegin(); it != insertions.rend(); ++it) result.insert(*it, line);
    return Result<std::string>::Success(std::move(result));
}
} // namespace War3Map::HLua
