#include "LuaCustomReloadAnalysis.h"
#include "LuaDeclarationScan.h"
#include <algorithm>
#include <chrono>
#include <map>
#include <new>

namespace War3Map::Script::lua {
namespace {
constexpr std::size_t kBytes = 8 * 1024 * 1024;
[[noreturn]] void Reject(std::string code, std::string message, std::size_t offset = 0) {
    throw Diagnostic{DiagnosticSeverity::Error, "Script.LuaReload", std::move(code), std::move(message),
        OriginSpan{{}, offset, 0, {}}};
}
Lua::SyntaxOptions Limited(Lua::SyntaxOptions options) {
    options.maxSourceBytes = (std::min)(options.maxSourceBytes, kBytes);
    options.deadline = (std::min)(options.deadline, std::chrono::steady_clock::now()
        + (std::clamp)(options.maxDuration, std::chrono::milliseconds::zero(), std::chrono::milliseconds{30000}));
    return options;
}
void Compile(std::string_view text, const Lua::SyntaxOptions& options) {
    if (text.size() > kBytes) Reject("LUA_RELOAD_CUSTOM_BUDGET", "作者代码超过 8 MiB 分析预算。");
    const auto result = Lua::CheckSyntax(text, "@Audastra/custom-reload-analysis.lua", options);
    if (!result.Succeeded()) Reject("LUA_RELOAD_CUSTOM_SYNTAX", result.diagnostics.empty()
        ? "作者代码语法检查未完成。" : result.diagnostics.front().message);
}
bool Named(std::string_view value) {
    return !value.empty() && ((value.front() >= 'a' && value.front() <= 'z')
        || (value.front() >= 'A' && value.front() <= 'Z') || value.front() == '_');
}
bool Registration(std::string_view name) {
    return name == "TriggerAddAction" || name == "TriggerAddCondition" || name == "Condition" || name == "TimerStart";
}
bool DynamicEnvironment(std::string_view name) {
    return name == "_G" || name == "_ENV" || name == "load" || name == "loadfile" || name == "dofile"
        || name == "require" || name == "jass" || name == "debug" || name == "getmetatable" || name == "setmetatable";
}
struct Function {
    std::string name;
    std::size_t nameToken, body, bodyEnd, end;
};
std::vector<Function> Functions(std::string_view text, const std::vector<DeclarationToken>& tokens) {
    std::vector<Function> result;
    std::size_t i = 0;
    while (i < tokens.size()) {
        if (tokens[i].text == ";") { ++i; continue; }
        if (i + 3 >= tokens.size() || tokens[i].text != "function" || !Named(tokens[i + 1].text) || tokens[i + 2].text != "(")
            Reject("LUA_RELOAD_CUSTOM_TOP_LEVEL", "自定义块只允许具名顶层 function 声明；顶层赋值、局部捕获、模块写法和副作用需要重新测试。", tokens[i].offset);
        Function function{std::string(tokens[i + 1].text), i + 1, 0, 0, 0};
        i += 3;
        while (i < tokens.size() && tokens[i].text != ")") ++i;
        if (i == tokens.size()) Reject("LUA_RELOAD_CUSTOM_SHAPE", "无法确定作者函数参数边界。");
        function.body = tokens[i++].offset + 1;
        std::size_t depth = 1;
        for (; i < tokens.size(); ++i) {
            const auto value = tokens[i].text;
            if (value == "function" || value == "if" || value == "do" || value == "repeat") {
                if (++depth > 128) Reject("LUA_RELOAD_CUSTOM_DEPTH", "作者函数块超过 128 层预算。", tokens[i].offset);
            } else if (value == "end" || value == "until") {
                if (--depth == 0) {
                    function.bodyEnd = tokens[i].offset; function.end = tokens[i].offset + tokens[i].text.size(); ++i; break;
                }
            }
        }
        if (!function.end || function.end > text.size()) Reject("LUA_RELOAD_CUSTOM_SHAPE", "无法确定作者函数正文边界。");
        if (result.size() >= 8192) Reject("LUA_RELOAD_CUSTOM_FUNCTION_LIMIT", "作者函数数目超过 8192 项预算。");
        result.push_back(std::move(function));
    }
    return result;
}
using Argument = std::pair<std::size_t, std::size_t>;
std::vector<Argument> Arguments(const std::vector<DeclarationToken>& tokens, std::size_t call) {
    std::vector<Argument> result;
    if (call + 1 >= tokens.size() || tokens[call + 1].text != "(") return result;
    std::size_t first = call + 2, depth = 0;
    for (auto i = first; i < tokens.size(); ++i) {
        const auto value = tokens[i].text;
        if (value == "(" || value == "[" || value == "{") { ++depth; continue; }
        if (value == ")" && depth == 0) { result.push_back({first, i}); return result; }
        if (value == ")" || value == "]" || value == "}") { if (!depth) return {}; --depth; continue; }
        if (value == "," && depth == 0) {
            if (result.size() >= 8) return {};
            result.push_back({first, i}); first = i + 1;
        }
    }
    return {};
}
void NoShadowedNames(const std::vector<DeclarationToken>& tokens, const CustomReloadContext& context,
    const std::set<std::size_t>& declarations) {
    const auto reserved = [&](std::string_view name) { return context.functions.contains(name) || Registration(name) || name == "_G" || name == "_ENV"; };
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        if (DynamicEnvironment(tokens[i].text))
            Reject("LUA_RELOAD_CUSTOM_ENVIRONMENT", "动态加载、模块别名或显式修改全局环境的作者块尚不支持热重载。", tokens[i].offset);
        if (tokens[i].text == "local" || tokens[i].text == "for") {
            auto at = i + 1; if (at < tokens.size() && tokens[at].text == "function") ++at;
            while (at < tokens.size() && Named(tokens[at].text)) {
                if (reserved(tokens[at].text)) Reject("LUA_RELOAD_CUSTOM_SHADOW", "局部名称遮蔽函数或回调接口，不能证明全局转发身份。", tokens[at].offset);
                if (at + 1 >= tokens.size() || tokens[at + 1].text != ",") break;
                at += 2;
            }
        }
        if (tokens[i].text == "function") {
            auto at = i + 1;
            if (at < tokens.size() && reserved(tokens[at].text) && !declarations.contains(at))
                Reject("LUA_RELOAD_CUSTOM_SHADOW", "嵌套函数重定义了具名函数或回调接口，不能证明全局转发身份。", tokens[at].offset);
            while (at < tokens.size() && tokens[at].text != "(") ++at;
            for (++at; at < tokens.size() && tokens[at].text != ")"; ++at)
                if (reserved(tokens[at].text)) Reject("LUA_RELOAD_CUSTOM_SHADOW", "函数参数遮蔽函数或回调接口，不能证明全局转发身份。", tokens[at].offset);
        }
    }
}
void NoEscapedReferences(const std::vector<DeclarationToken>& tokens, const CustomReloadContext& context,
    const std::set<std::size_t>& declarations, const std::set<std::size_t>& allowed = {}) {
    for (std::size_t i = 0; i < tokens.size(); ++i) if (context.functions.contains(tokens[i].text)
        && !declarations.contains(i) && !allowed.contains(i)) {
        if (i + 1 >= tokens.size() || tokens[i + 1].text != "(" ||
            (i && (tokens[i - 1].text == "." || tokens[i - 1].text == ":")))
            Reject("LUA_RELOAD_CUSTOM_FUNCTION_ESCAPE", "具名函数通过别名、表或未识别的回调位置传递，不能保证已有注册更新。", tokens[i].offset);
    }
}
void HeaderBindings(const std::vector<DeclarationToken>& tokens, const CustomReloadContext& context,
    const std::set<std::string>& headerFunctions) {
    std::set<std::size_t> declarations;
    std::size_t depth = 0;
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        const auto value = tokens[i].text;
        if (Registration(value))
            Reject("LUA_RELOAD_CUSTOM_HEADER_BINDING", "共享头部包含回调注册或接口别名，不能证明所有注册都使用转发。", tokens[i].offset);
        if (value == "function") {
            if (!depth && i + 2 < tokens.size() && tokens[i + 2].text == "("
                && headerFunctions.contains(std::string(tokens[i + 1].text))) declarations.insert(i + 1);
            ++depth;
        } else if (value == "if" || value == "do" || value == "repeat") ++depth;
        else if (value == "end" || value == "until") { if (depth) --depth; }
    }
    NoShadowedNames(tokens, context, declarations);
    NoEscapedReferences(tokens, context, declarations);
}
CustomReloadAnalysis Analyze(CustomReloadSource source, const CustomReloadContext& context, const Lua::SyntaxOptions& options) {
    Compile(source.text, options);
    const auto tokens = ScanDeclarationTokens(source.text, options);
    const auto functions = Functions(source.text, tokens);
    if (std::none_of(functions.begin(), functions.end(), [&](const auto& f) { return f.name == source.initializer; }))
        Reject("LUA_RELOAD_CUSTOM_INITIALIZER", "作者块缺少已登记的具名初始化函数。");
    std::set<std::size_t> declarations, allowedReferences, conditions;
    std::map<std::size_t, std::string> replacements;
    for (const auto& function : functions) declarations.insert(function.nameToken);
    NoShadowedNames(tokens, context, declarations);
    const auto singleName = [&](Argument argument) {
        return argument.second == argument.first + 1 && context.functions.contains(tokens[argument.first].text);
    };
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        const auto name = tokens[i].text;
        if (!Registration(name)) continue;
        if (name == "Condition" && conditions.contains(i)) continue;
        if ((i && (tokens[i - 1].text == "." || tokens[i - 1].text == ":")) || declarations.contains(i))
            Reject("LUA_RELOAD_CUSTOM_REGISTRATION", "回调接口经模块、方法或重定义使用，不能证明注册身份。", tokens[i].offset);
        const auto args = Arguments(tokens, i);
        std::size_t callback = 0;
        if (name == "TriggerAddAction" && args.size() == 2 && args[0].second == args[0].first + 1
            && Named(tokens[args[0].first].text) && singleName(args[1])) callback = args[1].first;
        else if (name == "TriggerAddCondition" && args.size() == 2 && args[0].second == args[0].first + 1
            && Named(tokens[args[0].first].text) && args[1].second == args[1].first + 4
            && tokens[args[1].first].text == "Condition" && tokens[args[1].first + 1].text == "("
            && tokens[args[1].first + 3].text == ")" && context.functions.contains(tokens[args[1].first + 2].text)) {
            conditions.insert(args[1].first); callback = args[1].first + 2;
        } else if (name == "TimerStart" && args.size() == 4 && singleName(args[3])) {
            allowedReferences.insert(args[3].first); continue; // 保留原函数对象，已有计时器不转发。
        } else Reject("LUA_RELOAD_CUSTOM_REGISTRATION", "只支持明确具名的动作、Condition 条件及计时器回调；匿名注册或接口别名需要重新测试。", tokens[i].offset);
        allowedReferences.insert(callback);
        replacements.emplace(tokens[callback].offset, "function(...) return " + std::string(tokens[callback].text) + "(...) end");
    }
    NoEscapedReferences(tokens, context, declarations, allowedReferences);
    CustomReloadAnalysis result;
    std::size_t position = 0;
    for (const auto& [offset, text] : replacements) {
        result.text.append(source.text.substr(position, offset - position)); result.text += text;
        const auto token = std::lower_bound(tokens.begin(), tokens.end(), offset, [](const auto& value, auto at) { return value.offset < at; });
        position = offset + token->text.size();
        if (result.text.size() > kBytes) Reject("LUA_RELOAD_CUSTOM_BUDGET", "转发后的作者代码超过 8 MiB 预算。");
    }
    result.text.append(source.text.substr(position));
    position = 0;
    for (const auto& function : functions) {
        result.shape.append(source.text.substr(position, function.body - position));
        if (function.name == source.initializer) result.shape.append(source.text.substr(function.body, function.bodyEnd - function.body));
        else result.shape += "\x1f" "function-body" "\x1f";
        position = function.bodyEnd;
    }
    result.shape.append(source.text.substr(position));
    if (result.text.size() > kBytes) Reject("LUA_RELOAD_CUSTOM_BUDGET", "转发后的作者代码超过 8 MiB 预算。");
    return result;
}
}

Result<CustomReloadAnalysis> AnalyzeCustomReload(CustomReloadSource source, const CustomReloadContext& context, const Lua::SyntaxOptions& requested) {
    try { return Result<CustomReloadAnalysis>::Success(Analyze(source, context, Limited(requested))); }
    catch (const Diagnostic& diagnostic) { return {std::nullopt, {diagnostic}}; }
    catch (const std::bad_alloc&) { return Result<CustomReloadAnalysis>::Failure("Script.LuaReload", "LUA_RELOAD_CUSTOM_ALLOCATION", "作者热重载分析内存不足。"); }
}
Result<CustomReloadContext> CaptureCustomReloadContext(std::string_view header,
    std::span<const CustomReloadSource> sources, const Lua::SyntaxOptions& requested) {
    try {
        const auto options = Limited(requested);
        if (sources.size() > 8192) Reject("LUA_RELOAD_CUSTOM_BUDGET", "作者块超过 8192 项预算。");
        std::size_t bytes = header.size();
        for (const auto& source : sources) {
            if (bytes > kBytes || source.text.size() > kBytes - bytes) Reject("LUA_RELOAD_CUSTOM_BUDGET", "作者上下文超过 8 MiB 预算。");
            bytes += source.text.size();
        }
        Compile(header, options);
        const auto headerDeclarations = CollectAuthorDeclarations(header, options);
        if (!headerDeclarations.locals.empty()) Reject("LUA_RELOAD_CUSTOM_HEADER_LOCAL", "共享头部有顶层局部名称，不能证明作者函数没有捕获旧局部状态。");
        CustomReloadContext context; context.functions.insert(headerDeclarations.functions.begin(), headerDeclarations.functions.end());
        for (const auto& source : sources) {
            Compile(source.text, options);
            for (const auto& function : Functions(source.text, ScanDeclarationTokens(source.text, options)))
                if (Registration(function.name) || !context.functions.insert(function.name).second || context.functions.size() > 8192)
                    Reject("LUA_RELOAD_CUSTOM_DECLARATIONS", "作者函数重名、覆盖回调接口或超过声明预算。");
        }
        HeaderBindings(ScanDeclarationTokens(header, options), context, headerDeclarations.functions);
        // 全部块都能插入转发才授予资格；避免兄弟块保留直接引用却让被引用函数热更。
        for (const auto& source : sources) (void)Analyze(source, context, options);
        return Result<CustomReloadContext>::Success(std::move(context));
    } catch (const Diagnostic& diagnostic) { return {std::nullopt, {diagnostic}}; }
    catch (const std::bad_alloc&) { return Result<CustomReloadContext>::Failure("Script.LuaReload", "LUA_RELOAD_CUSTOM_ALLOCATION", "作者上下文分析内存不足。"); }
}
} // namespace War3Map::Script::lua
