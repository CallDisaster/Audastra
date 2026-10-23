#include "War3MapCore/GameUi/MapUiFiles.h"

#include "War3MapCore/Text/TextUtil.h"
#include "../Jass/JassLexer.h"
#include "../Lua/LuaLexer.h"

#include <algorithm>
#include <unordered_set>

namespace War3Map::GameUi {
namespace {

constexpr const char* kSubsystem = "GameUi";

std::string NormalizePath(std::string_view path) {
    std::string normalized = Text::LowerAscii(path);
    std::replace(normalized.begin(), normalized.end(), '/', '\\');
    return normalized;
}

bool IsTocLoadFunction(std::string_view name) {
    const auto& functions = TocLoadFunctions();
    return std::find(functions.begin(), functions.end(), name) != functions.end();
}

std::vector<TocLoadCall> FindJassCalls(const MapScriptText& script) {
    std::vector<TocLoadCall> calls;
    const auto tokens = Jass::detail::LexJass(script.text);
    for (std::size_t i = 0; i + 1 < tokens.size(); ++i) {
        const auto& token = tokens[i];
        if (token.kind != Jass::detail::TokenKind::Identifier || !IsTocLoadFunction(token.text)) continue;
        if (!tokens[i + 1].IsOperator("(")) continue;
        TocLoadCall call{std::string(token.text), {}, script.path, token.line};
        // 只认“字面量后紧跟右括号”；拼接或变量参数无法静态确定。
        if (i + 3 < tokens.size() && tokens[i + 2].kind == Jass::detail::TokenKind::String && tokens[i + 3].IsOperator(")")) {
            call.path = Jass::detail::StringLiteralValue(tokens[i + 2].text).value_or(std::string());
        }
        calls.push_back(std::move(call));
    }
    return calls;
}

std::vector<TocLoadCall> FindLuaCalls(const MapScriptText& script) {
    using Lua::Detail::TokenKind;
    std::vector<TocLoadCall> calls;
    const std::string_view source = script.text;
    const auto tokens = Lua::Detail::Tokenize(source);
    for (std::size_t i = 0; i + 1 < tokens.size(); ++i) {
        const auto& token = tokens[i];
        if (token.kind != TokenKind::Name || !IsTocLoadFunction(token.Text(source))) continue;
        // 方法或字段访问（x.DzLoadToc、x:DzLoadToc）不是全局函数调用。
        if (i > 0 && (tokens[i - 1].Is(source, ".") || tokens[i - 1].Is(source, ":"))) continue;
        const auto& next = tokens[i + 1];
        const auto literal = [&](const Lua::Detail::Token& candidate) {
            return candidate.kind == TokenKind::String && !candidate.unterminated;
        };
        TocLoadCall call{std::string(token.Text(source)), {}, script.path, token.line};
        if (literal(next)) {
            // Lua 允许 f "字符串" 的调用写法。
            call.path = Lua::Detail::StringValue(next.Text(source));
        } else if (next.Is(source, "(")) {
            if (i + 3 < tokens.size() && literal(tokens[i + 2]) && tokens[i + 3].Is(source, ")")) {
                call.path = Lua::Detail::StringValue(tokens[i + 2].Text(source));
            }
        } else {
            continue;  // 只是引用函数名，不是调用
        }
        calls.push_back(std::move(call));
    }
    return calls;
}

} // namespace

const std::vector<std::string_view>& TocLoadFunctions() {
    static const std::vector<std::string_view> functions{"DzLoadToc", "BlzLoadTOCFile"};
    return functions;
}

const std::vector<MapScriptPath>& MapScriptPaths() {
    static const std::vector<MapScriptPath> paths{
        {"war3map.j", ScriptLanguage::Jass}, {"scripts\\war3map.j", ScriptLanguage::Jass}, {"war3map.lua", ScriptLanguage::Lua}};
    return paths;
}

std::vector<TocLoadCall> FindTocLoadCalls(const MapScriptText& script) {
    return script.language == ScriptLanguage::Lua ? FindLuaCalls(script) : FindJassCalls(script);
}

MapTocPlan PlanMapTocs(const std::vector<std::string>& mapFiles, const std::vector<MapScriptText>& scripts) {
    MapTocPlan plan;
    std::unordered_set<std::string> planned;
    const auto add = [&](const std::string& path) {
        if (planned.insert(NormalizePath(path)).second) plan.tocs.push_back(path);
    };
    for (const auto& script : scripts) {
        for (auto& call : FindTocLoadCalls(script)) {
            if (call.path.empty()) {
                plan.diagnostics.push_back(Diagnostic{DiagnosticSeverity::Info, kSubsystem, "GAMEUI_MAP_TOC_DYNAMIC",
                    call.scriptFile + " 第 " + std::to_string(call.line) + " 行的 " + call.function +
                        " 参数不是字面量，无法确定加载的界面目录",
                    std::nullopt});
            } else {
                add(call.path);
            }
            plan.calls.push_back(std::move(call));
        }
    }
    for (const auto& file : mapFiles) {
        const std::string normalized = NormalizePath(file);
        if (normalized.size() < 4 || normalized.compare(normalized.size() - 4, 4, ".toc") != 0) continue;
        if (planned.contains(normalized)) continue;
        plan.diagnostics.push_back(Diagnostic{DiagnosticSeverity::Info, kSubsystem, "GAMEUI_MAP_TOC_UNREFERENCED",
            "地图包含界面目录 " + file + "，脚本里没有找到加载它的调用；仍读入以便预览", std::nullopt});
        add(file);
    }
    return plan;
}

} // namespace War3Map::GameUi
