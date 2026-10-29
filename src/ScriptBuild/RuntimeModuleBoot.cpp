#include "RuntimeModuleBoot.h"
#include "War3MapCore/Jass/JassChecker.h"
#include "../Jass/JassLexer.h"
#include <algorithm>

namespace War3Map::Script::Detail {
namespace {
using namespace Jass::detail;
using Tokens = std::vector<Token>;
constexpr std::string_view Entry = "AudastraRuntimeModuleLoad", Boot = "AjStartRuntimeModule";
Status Fail(std::string_view message) {
    return Status::Failure("MapRuntime", "RUNTIME_MODULE_BOOT_INVALID", std::string(message));
}
Result<Tokens> Lex(std::string_view script) {
    if (script.size() > 16 * 1024 * 1024) return {{}, Fail("主脚本超过16MiB结构检查预算。").diagnostics};
    auto tokens = LexJassBounded(script, {}, Jass::JassCheckOptions{}.maxTokens);
    if (tokens.Succeeded() && std::any_of(tokens.value->begin(), tokens.value->end(), [](const auto& t) {
        return t.kind == TokenKind::Invalid;
    })) return {{}, Fail("主脚本词法不完整，未安装模块启动。").diagnostics};
    return tokens;
}
std::string Shim(const ScriptProfileModuleLoader& loader) {
    return "function " + std::string(Entry) + " takes nothing returns nothing\r\n    call AjRuntimeModuleLoaded(" +
        loader.functionName + "(\"warvk_runtime_module.dll\", " + (loader.anonymous ? "true" : "false") + "))\r\nendfunction\r\n";
}
bool Signature(const Tokens& t, std::size_t i) {
    // 参数名称由作者定义，类型、数量和返回值固定。
    return i + 10 < t.size() && t[i + 1].Is(Keyword::Takes) && t[i + 2].text == "string" &&
        t[i + 3].kind == TokenKind::Identifier && t[i + 4].IsOperator(",") && t[i + 5].text == "boolean" &&
        t[i + 6].kind == TokenKind::Identifier && t[i + 7].Is(Keyword::Returns) &&
        t[i + 8].text == "boolean" && t[i + 9].EndsLine();
}
struct Layout {
    std::size_t mainEnd = 0, callBegin = 0, callEnd = 0, shimBegin = 0, shimEnd = 0;
    std::size_t diagnosticBegin = 0, diagnosticEnd = 0;
};
std::size_t LineBegin(std::string_view text, std::size_t offset) {
    while (offset && text[offset - 1] != '\r' && text[offset - 1] != '\n') --offset;
    return offset;
}
std::size_t LineEnd(std::string_view text, std::size_t offset) {
    while (offset < text.size() && text[offset] != '\r' && text[offset] != '\n') ++offset;
    if (offset < text.size() && text[offset] == '\r') ++offset;
    if (offset < text.size() && text[offset] == '\n') ++offset;
    return offset;
}
Result<Layout> Inspect(std::string_view script, const std::optional<ScriptProfileModuleLoader>& loader, bool completion = false) {
    auto lexed = Lex(script); if (!lexed.Succeeded()) return {{}, lexed.diagnostics};
    const auto& t = *lexed.value;
    Layout out; bool main = false; std::size_t last = 0, bootTokenEnd = 0;
    for (std::size_t i = 0; i + 1 < t.size(); ++i) {
        if (i && t[i - 1].Is(Keyword::Function) && t[i].text == "main" && t[i + 1].Is(Keyword::Takes)) main = true;
        if (main && t[i].Is(Keyword::EndFunction)) {
            out.mainEnd = t[i].offset;
            // 仅允许末尾启动及紧随的可选测试提示；其他作者语句不可搬动。
            if (out.callEnd && last != bootTokenEnd && !out.diagnosticEnd) return {{}, Fail("模块启动必须是main末尾固定挂点。").diagnostics};
            main = false;
        }
        if (completion && main && t[i].text == "return") return {{}, Fail("main含提前返回，不能证明初始化完成后一定到达模块挂点。").diagnostics};
        if (main && t[i].kind != TokenKind::Newline && t[i].kind != TokenKind::Pragma) last = i;
        if (t[i].text == Entry) {
            if (!loader || out.shimEnd || !i || !t[i - 1].Is(Keyword::Function) || !t[i + 1].Is(Keyword::Takes))
                return {{}, Fail("发现无所有权、重复或被其他代码引用的模块引导函数。").diagnostics};
            auto end = i + 1;
            while (end < t.size() && !t[end].Is(Keyword::EndFunction)) ++end;
            if (end + 1 >= t.size() || !t[end + 1].EndsLine()) return {{}, Fail("模块引导函数边界不完整。").diagnostics};
            const auto expectedText = Shim(*loader);
            const auto expected = Lex(expectedText);
            std::vector<std::string_view> actualTokens, expectedTokens;
            for (auto n = i - 1; n <= end; ++n) if (!t[n].EndsLine()) actualTokens.push_back(t[n].text);
            for (const auto& token : *expected.value) if (!token.EndsLine() && token.kind != TokenKind::End) expectedTokens.push_back(token.text);
            if (actualTokens != expectedTokens) return {{}, Fail("模块引导函数与受管声明不符，未删除作者代码。").diagnostics};
            out.shimBegin = LineBegin(script, t[i - 1].offset); out.shimEnd = LineEnd(script, t[end].offset);
            if (script.substr(out.shimBegin, out.shimEnd - out.shimBegin).find("//") != std::string_view::npos)
                return {{}, Fail("受管模块函数新增了作者注释，不能在重建中删除。").diagnostics};
        }
        if (t[i].text != Boot) continue;
        if (i && t[i - 1].Is(Keyword::Function) && t[i + 1].Is(Keyword::Takes)) continue; // SDK定义。
        if (!loader || !main || out.callEnd || !i || !t[i - 1].Is(Keyword::Call) || i + 3 >= t.size()
            || !t[i + 1].IsOperator("(") || !t[i + 2].IsOperator(")") || !t[i + 3].EndsLine())
            return {{}, Fail("模块启动不是唯一的main独立调用。").diagnostics};
        const auto begin = t[i - 1].offset;
        if (script.substr(LineBegin(script, begin), begin - LineBegin(script, begin)).find_first_not_of(" \t") != std::string_view::npos)
            return {{}, Fail("模块启动必须独占一行。").diagnostics};
        out.callBegin = LineBegin(script, begin); out.callEnd = LineEnd(script, t[i + 2].offset); bootTokenEnd = i + 2;
        if (script.substr(out.callBegin, out.callEnd - out.callBegin).find("//") != std::string_view::npos)
            return {{}, Fail("模块启动行新增了作者注释，不能在重建中删除。").diagnostics};
        if (i + 8 < t.size() && t[i + 4].Is(Keyword::Call) && t[i + 5].text == "AjStartTestDiagnostics" &&
            t[i + 6].IsOperator("(") && t[i + 7].IsOperator(")") && t[i + 8].EndsLine()) {
            out.diagnosticBegin = LineBegin(script, t[i + 4].offset); out.diagnosticEnd = LineEnd(script, t[i + 7].offset);
            if (script.substr(out.diagnosticBegin, out.diagnosticEnd - out.diagnosticBegin).find("//") != std::string_view::npos)
                return {{}, Fail("模块测试调用行新增了作者注释，不能在重建中删除。").diagnostics};
            bootTokenEnd = i + 7;
        }
    }
    if (!out.mainEnd || ((out.callEnd != 0) != (out.shimEnd != 0))) return {{}, Fail("main或配对模块引导片段不完整。").diagnostics};
    // 挂点后不得有别的语句，即使曾出现过测试诊断。
    if (out.callEnd) {
        const auto tail = script.substr(out.diagnosticEnd ? out.diagnosticEnd : out.callEnd,
            out.mainEnd - (out.diagnosticEnd ? out.diagnosticEnd : out.callEnd));
        const auto remaining = Lex(tail);
        if (!remaining.Succeeded() || std::any_of(remaining.value->begin(), remaining.value->end(), [](const auto& token) {
            return !token.EndsLine() && token.kind != TokenKind::End;
        })) return {{}, Fail("模块挂点之后仍有作者语句，未重排初始化。").diagnostics};
    }
    return Result<Layout>::Success(out);
}
Result<Token> ExpectedBinding(std::string_view script) {
    auto lexed = Lex(script); if (!lexed.Succeeded()) return {{}, lexed.diagnostics};
    const auto& t = *lexed.value; bool globals = false; std::optional<Token> found;
    for (std::size_t i = 0; i < t.size(); ++i) {
        if (t[i].Is(Keyword::Globals)) globals = true;
        if (t[i].Is(Keyword::EndGlobals)) globals = false;
        if (t[i].text != "AjRuntimeModuleExpected" || !i) continue;
        if (t[i - 1].Is(Keyword::Set)) return {{}, Fail("模块启动策略不能在执行期重新赋值。").diagnostics};
        if (t[i - 1].text != "boolean") continue;
        if (!globals || found || i + 3 >= t.size() || !t[i + 1].IsOperator("=") ||
            (t[i + 2].text != "true" && t[i + 2].text != "false") || !t[i + 3].EndsLine())
            return {{}, Fail("模块策略须为唯一全局布尔字面量，不能遮蔽。").diagnostics};
        found = t[i + 2];
    }
    return found ? Result<Token>::Success(*found) : Result<Token>::Success({});
}
}

Result<std::optional<ScriptProfileModuleLoader>> SelectModuleLoader(std::string_view script,
    std::span<const ScriptProfileModuleLoader> loaders) {
    if (loaders.size() > 8) return {{}, Fail("模块加载档案超过预算。").diagnostics};
    auto lexed = Lex(script); if (!lexed.Succeeded()) return {{}, lexed.diagnostics};
    const auto& t = *lexed.value; std::optional<ScriptProfileModuleLoader> selected;
    for (const auto& loader : loaders) {
        if (!ScriptModuleLoaderIdentity(loader).Succeeded()) return {{}, Fail("模块加载档案声明无效。").diagnostics};
        unsigned declarations = 0; bool valid = false;
        for (std::size_t i = 1; i + 1 < t.size(); ++i) if (t[i].text == loader.functionName &&
            (t[i - 1].Is(Keyword::Function) || t[i - 1].text == "native") && t[i + 1].Is(Keyword::Takes)) {
            ++declarations; valid = Signature(t, i);
        }
        if (!declarations) continue;
        if (declarations != 1 || !valid || selected)
            return Result<std::optional<ScriptProfileModuleLoader>>::Failure("MapRuntime", "RUNTIME_COEXISTENCE_UNVERIFIED",
                "公开模块加载接口声明重复、签名不符或存在多种可用接口，未改变原初始化。");
        selected = loader;
    }
    return Result<std::optional<ScriptProfileModuleLoader>>::Success(std::move(selected));
}
Result<std::string> BindModuleExpected(std::string_view script, bool expected) {
    const auto binding = ExpectedBinding(script); if (!binding.Succeeded()) return {{}, binding.diagnostics};
    if (binding.value->text.empty()) return expected ? Result<std::string>{{}, Fail("模块启动需要更新配套客户端。").diagnostics}
        : Result<std::string>::Success(std::string(script));
    std::string out(script); out.replace(binding.value->offset, binding.value->text.size(), expected ? "true" : "false");
    return Result<std::string>::Success(std::move(out));
}
Result<std::string> RemoveModuleBoot(std::string_view script, const std::optional<ScriptProfileModuleLoader>& owned) {
    const auto layout = Inspect(script, owned); if (!layout.Succeeded()) return {{}, layout.diagnostics};
    const auto& l = *layout.value; std::string out(script);
    if (l.shimEnd) {
        // helper位于main之后，按倒序删除，保持所有作者字节及相对顺序。
        if (l.shimBegin < l.mainEnd) return {{}, Fail("受管模块引导函数被移动。").diagnostics};
        out.erase(l.shimBegin, l.shimEnd - l.shimBegin);
        if (l.diagnosticEnd) out.erase(l.diagnosticBegin, l.diagnosticEnd - l.diagnosticBegin);
        out.erase(l.callBegin, l.callEnd - l.callBegin);
    }
    return Result<std::string>::Success(std::move(out));
}
Result<std::string> AddModuleBoot(std::string_view script, const ScriptProfileModuleLoader& loader) {
    const auto layout = Inspect(script, {}, true); if (!layout.Succeeded()) return {{}, layout.diagnostics};
    std::string out(script);
    const auto begin = LineBegin(script, layout.value->mainEnd);
    if (script.substr(begin, layout.value->mainEnd - begin).find_first_not_of(" \t") != std::string_view::npos)
        return {{}, Fail("main结束不独占一行，未加入模块启动。").diagnostics};
    out.insert(begin, "    call AjStartRuntimeModule()\r\n");
    if (!out.ends_with('\n') && !out.ends_with('\r')) out += "\r\n";
    out += Shim(loader);
    return Result<std::string>::Success(std::move(out));
}
Status VerifyModuleBoot(std::string_view script, const std::optional<ScriptProfileModuleLoader>& loader) {
    const auto layout = Inspect(script, loader, loader.has_value()); if (!layout.Succeeded()) return {layout.diagnostics};
    if (loader.has_value() != (layout.value->callEnd != 0)) return Fail("最终模块启动与选择的载体不符。");
    const auto binding = ExpectedBinding(script); if (!binding.Succeeded()) return {binding.diagnostics};
    if ((loader && binding.value->text != "true") || (!loader && binding.value->text == "true")) return Fail("最终客户端的模块策略不符。");
    if (loader) {
        const auto selected = SelectModuleLoader(script, std::span(&*loader, 1));
        if (!selected.Succeeded()) return {selected.diagnostics};
        if (!*selected.value) return Fail("最终脚本缺少工具链声明的模块加载接口。");
    }
    return Status::Ok();
}
}
