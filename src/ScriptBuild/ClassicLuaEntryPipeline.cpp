#include "War3MapCore/Script/ClassicLuaEntryPipeline.h"
#include "War3MapCore/Script/ClassicLuaCompatibility.h"
#include "LuaMapScriptInternal.h"

#include "../Jass/JassCheckBudget.h"
#include "../Jass/JassLexer.h"

#include <algorithm>
#include <array>
#include <exception>
#include <map>
#include <set>

namespace War3Map::Script {
namespace {

using Clock = std::chrono::steady_clock;
constexpr std::size_t kEntryBytes = 32 * 1024 * 1024;

void Fail(PipelineResult& result, const char* code, std::string message) {
    result.diagnostics.push_back({DiagnosticSeverity::Error, "Script.ClassicLuaEntry", code, std::move(message), {}});
}

Jass::JassCheckOptions WithDeadline(const Jass::JassCheckOptions& limits) {
    auto options = limits;
    const auto started = Clock::now();
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>((Clock::time_point::max)() - started);
    const auto end = options.maxDuration <= std::chrono::milliseconds::zero() ? started
        : options.maxDuration >= remaining ? (Clock::time_point::max)() : started + options.maxDuration;
    options.deadline = (std::min)(options.deadline, end);
    return options;
}

std::string_view Text(const ScriptBaseInput& input) {
    if (input.bytes.empty()) return {};
    return {reinterpret_cast<const char*>(input.bytes.data()), input.bytes.size()};
}

struct Stage {
    PipelineResult& result;
    std::size_t index;
    Clock::time_point started = Clock::now();
    Stage(PipelineResult& owner, const char* name, const std::function<void(std::string_view)>& callback)
        : result(owner), index(owner.stages.size()) {
        result.stages.push_back({name, {}, {}, 1, {}});
        if (callback) callback(name);
    }
    ~Stage() {
        result.stages[index].elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started);
    }
    PipelineStage& Value() { return result.stages[index]; }
    void Pass(std::string log = {}) { Value().exitCode = 0; Value().log = std::move(log); }
};

// 生成的原生入口不需要检查器 pragma。使用同一有界 lexer 识别真正注释，
// 字符串中的 "//#" 不构成指令；三个来源共用预算和本次检查的绝对截止时间。
bool CheckNoPragmas(PipelineResult& result, std::span<const Jass::JassSource> sources,
    const Jass::JassCheckOptions& options) {
    try {
        Jass::detail::CheckBudget budget(options);
        for (const auto& source : sources) {
            budget.RegisterSource(source.text.size());
            auto tokens = Jass::detail::LexJassBounded(source.text, {}, budget.RemainingTokens(), &budget);
            if (!tokens.Succeeded()) {
                for (auto& diagnostic : tokens.diagnostics) {
                    if (diagnostic.origin) diagnostic.origin->source = source.name;
                    result.diagnostics.push_back(std::move(diagnostic));
                }
                Fail(result, "CLASSIC_LUA_ENTRY_PRAGMA_CHECK_INCOMPLETE", "最终检查开关核验未完整执行");
                return false;
            }
            budget.ConsumeTokens(tokens.value->size());
            for (const auto& token : *tokens.value) {
                budget.PollOccasionally(token.offset);
                if (token.kind != Jass::detail::TokenKind::Pragma) continue;
                Fail(result, "CLASSIC_LUA_ENTRY_PRAGMA_UNSUPPORTED", "经典 Lua 原生入口及基础库不能通过 //# 注释改变最终检查规则");
                result.diagnostics.back().origin = OriginSpan{source.name, token.offset, token.text.size(), {}};
                return false;
            }
        }
        budget.Poll();
        return true;
    } catch (const Jass::detail::CheckAborted&) {
        Fail(result, "CLASSIC_LUA_ENTRY_PRAGMA_CHECK_INCOMPLETE", "最终检查开关核验已取消、超时或达到预算");
        return false;
    }
}

} // namespace

PipelineResult CheckClassicLuaEntryScript(std::string_view generatedJass,
    std::shared_ptr<const ScriptBaseInputs> bases, const Jass::JassCheckOptions& limits,
    const std::function<void(std::string_view)>& onStage) {
    PipelineResult result;
    try {
        const auto options = WithDeadline(limits);
        if (!bases || options.syntaxOnly || options.returnBug || bases->allowReturnBug || bases->gameLibraryVersion != 24) {
            Fail(result, "CLASSIC_LUA_ENTRY_CHECK_POLICY", "经典 Lua 原生入口需要已绑定的 1.24+ 基础库和完整类型检查");
            return result;
        }
        if (generatedJass.empty() || generatedJass.size() > (std::min)(kEntryBytes, options.maxSourceBytes)) {
            Fail(result, "CLASSIC_LUA_ENTRY_SOURCE_LIMIT", "经典 Lua 原生入口为空或超过脚本字节预算");
            return result;
        }
        const auto verified = VerifyScriptBaseInputs(*bases, ScriptBasePolicy::MapGameOnly);
        result.diagnostics = verified.diagnostics;
        if (!verified.Succeeded()) return result;
        // 回调前拥有输入副本，避免调用方在进度回调中修改原缓冲后改变被检查的文本。
        const auto frozen = std::make_shared<const ScriptBaseInputs>(*bases);
        const std::string script(generatedJass);
        result.baseInputs = frozen;
        {
            Stage stage(result, "classic-lua-entry-inputs", onStage);
            stage.Pass("commonSha256=" + frozen->common.sha256 + "\nblizzardSha256=" + frozen->blizzard.sha256);
        }
        const std::array<Jass::JassSource, 3> sources{{
            {"common.j", Text(frozen->common)}, {"blizzard.j", Text(frozen->blizzard)}, {"war3map.j", script}}};
        {
            Stage stage(result, "classic-lua-entry-check", onStage);
            auto checked = Jass::CheckJass(sources, options);
            const bool succeeded = checked.Succeeded();
            stage.Value().log = std::to_string(checked.lines) + " 行，" + std::to_string(checked.functions) + " 个函数";
            for (auto& diagnostic : checked.diagnostics) result.diagnostics.push_back(std::move(diagnostic));
            if (!checked.completed || !checked.semanticChecked || checked.errorsIgnored != 0) {
                Fail(result, "CLASSIC_LUA_ENTRY_CHECK_INCOMPLETE", "最终 JASS 检查未完整完成，或有被忽略的错误");
                return result;
            }
            if (!succeeded) return result;
            if (!CheckNoPragmas(result, sources, options)) return result;
            stage.Pass(stage.Value().log);
        }
        result.script = script;
        result.checkedBy = "内置经典 Lua 原生入口 JASS 语法与类型检查";
        result.syntaxChecked = true;
    } catch (const std::exception&) {
        result.script.clear();
        result.syntaxChecked = false;
        Fail(result, "CLASSIC_LUA_ENTRY_CHECK_FAILED", "经典 Lua 原生入口检查异常终止，未返回可保存脚本");
    }
    return result;
}

Status ValidateClassicLuaDependencies(const MapInitializationPlan& plan,
    const LuaMapScriptResult& generated, const LuaTargetProfile& target, const ScriptBaseInputs& bases,
    const Jass::JassCheckOptions& limits) {
    Status result;
    const auto fail = [&](const char* code, std::string message) {
        result.diagnostics.push_back({DiagnosticSeverity::Error, "Script.ClassicLuaEntry", code, std::move(message), {}});
    };
    try {
        auto options = WithDeadline(limits);
        options.collectSymbols = true;
        if (options.syntaxOnly || options.returnBug || bases.allowReturnBug || bases.gameLibraryVersion != 24
            || !generated.Succeeded() || generated.targetIdentity != target.identity || target.nativeAuthorCode
            || generated.classicObjectsContinuation != kClassicLuaObjectsContinuation
            || generated.classicMainContinuation != kClassicLuaMainContinuation) {
            fail("CLASSIC_LUA_DEPENDENCY_INPUT", "依赖核验需要成功的经典 Lua 生成结果、相同目标和完整 1.24+ 检查策略");
            return result;
        }
        if (target.apis.size() > 4096 || target.constants.size() > 4096 || target.handleTypes.size() > 512
            || generated.requiredApis.size() > 4096 || generated.requiredConstants.size() > 4096
            || generated.requiredGlobals.size() > 65536 || plan.symbols.size() > 65536
            || generated.objectSpans.size() > 262144) {
            fail("CLASSIC_LUA_DEPENDENCY_LIMIT", "经典 Lua 依赖目录超过核验预算");
            return result;
        }
        result = VerifyScriptBaseInputs(bases, ScriptBasePolicy::MapGameOnly);
        if (!result.Succeeded()) return result;
        const auto sourceMap = lua::ComputeLuaMapSourceMapSha256(generated);
        result.diagnostics.insert(result.diagnostics.end(), sourceMap.diagnostics.begin(), sourceMap.diagnostics.end());
        if (!sourceMap.Succeeded() || *sourceMap.value != generated.sourceMapSha256) {
            fail("CLASSIC_LUA_DEPENDENCY_GENERATED_CHANGED", "依赖所属的完整 Lua 文本或来源范围在生成后变化");
            return result;
        }
        // 复用生产 writer 对目标目录的验证与指纹，不另写一套目标签名序列化。
        const auto targetOnly = GenerateLuaTriggerUnit({}, {}, target);
        if (!targetOnly.Succeeded() || targetOnly.targetSha256 != generated.targetSha256) {
            fail("CLASSIC_LUA_DEPENDENCY_TARGET_CHANGED", "Lua 目标签名目录与生成时的指纹不一致");
            return result;
        }
        const auto compatibility = BuildClassicLuaCompatibility(target);
        result.diagnostics.insert(result.diagnostics.end(), compatibility.diagnostics.begin(), compatibility.diagnostics.end());
        if (!compatibility.Succeeded()) return result;
        const std::array<Jass::JassSource, 1> sources{{{"common.j", Text(bases.common)}}};
        auto checked = Jass::CheckJass(sources, options);
        const bool succeeded = checked.Succeeded();
        result.diagnostics.insert(result.diagnostics.end(), checked.diagnostics.begin(), checked.diagnostics.end());
        if (!succeeded || !checked.semanticChecked || checked.errorsIgnored || !checked.symbols.completed) {
            fail("CLASSIC_LUA_DEPENDENCY_INDEX_INCOMPLETE", "common.j 真实语义索引不完整，不能据部分声明放行 Lua 依赖");
            return result;
        }
        PipelineResult pragmaResult;
        if (!CheckNoPragmas(pragmaResult, sources, options)) {
            result.diagnostics.insert(result.diagnostics.end(), pragmaResult.diagnostics.begin(), pragmaResult.diagnostics.end());
            return result;
        }
        Jass::detail::CheckBudget budget(options);
        std::map<std::string, const Jass::SymbolDefinition*, std::less<>> declarations;
        for (const auto& definition : checked.symbols.definitions) {
            budget.Poll();
            if (!definition.scope) declarations.emplace(definition.name, &definition);
        }
        std::map<std::string, const LuaApiSignature*, std::less<>> apis, helpers;
        std::map<std::string, std::string, std::less<>> constants, parents;
        for (const auto& api : target.apis) apis.emplace(api.name, &api);
        for (const auto& api : compatibility.providedApis) helpers.emplace(api.name, &api);
        for (const auto& item : target.constants) constants.emplace(item.name, item.type);
        for (const auto& [name, parent] : target.handleTypes) parents.emplace(name, parent);
        const auto type = [&](std::string name, bool scalar) {
            std::set<std::string> seen;
            while (name != "handle") {
                budget.Poll();
                if (name == "integer" || name == "real" || name == "boolean" || name == "string") return true;
                if (name == "code" || name == "nothing") return !scalar;
                const auto expected = parents.find(name);
                const auto actual = declarations.find(name);
                if (seen.size() >= 32 || !seen.insert(name).second || expected == parents.end() || actual == declarations.end()
                    || actual->second->kind != Jass::SymbolKind::Type || actual->second->type != expected->second) return false;
                name = expected->second;
            }
            return true;
        };
        const auto native = [&](std::string_view name, bool allowHelper) {
            budget.Poll();
            const auto expected = apis.find(name);
            if (expected == apis.end()) {
                fail("CLASSIC_LUA_DEPENDENCY_API", "生成脚本所需 API 不在冻结目标中：" + std::string(name)); return;
            }
            const auto& api = *expected->second;
            bool valid = type(api.returns, false);
            for (const auto& parameter : api.parameters) valid = valid && type(parameter, false);
            const auto helper = helpers.find(name);
            if (allowHelper && helper != helpers.end()) {
                valid = valid && api.returns == helper->second->returns && api.parameters == helper->second->parameters;
            } else {
                const auto actual = declarations.find(name);
                valid = valid && actual != declarations.end() && actual->second->kind == Jass::SymbolKind::Native;
                if (valid) {
                    const auto& definition = *actual->second;
                    valid = definition.type == api.returns && definition.parameters.size() == api.parameters.size();
                    for (std::size_t i = 0; valid && i < api.parameters.size(); ++i)
                        valid = definition.parameters[i].type == api.parameters[i];
                }
            }
            if (!valid) fail("CLASSIC_LUA_DEPENDENCY_API", "Lua API 的真实 native/原创兼容签名或句柄父链不匹配：" + std::string(name));
        };
        const auto constant = [&](std::string_view name) {
            budget.Poll();
            const auto expected = constants.find(name);
            const auto actual = declarations.find(name);
            if (expected == constants.end() || actual == declarations.end() || actual->second->kind != Jass::SymbolKind::Global
                || !actual->second->constant || actual->second->array || actual->second->type != expected->second
                || !type(expected->second, true))
                fail("CLASSIC_LUA_DEPENDENCY_CONSTANT", "Lua 常量不是 common.j 中相同类型的真实标量常量：" + std::string(name));
        };
        // 兼容模块工厂会读取全部八个常量，即使本图没有调用某个 helper 也必须可用。
        for (const auto& name : compatibility.requiredNativeApis) native(name, false);
        for (const auto& name : compatibility.requiredConstants) constant(name);
        for (const auto& name : generated.requiredApis) native(name, true);
        for (const auto& name : generated.requiredConstants) constant(name);
        std::map<std::string, const MapInitSymbol*, std::less<>> mapGlobals;
        for (const auto& symbol : plan.symbols) {
            budget.Poll();
            if (!mapGlobals.emplace(symbol.name, &symbol).second)
                fail("CLASSIC_LUA_DEPENDENCY_GLOBAL", "初始化计划重复声明全局符号：" + symbol.name);
        }
        const auto sameSource = [](const MapInitSymbol& left, const MapInitSymbol& right) {
            return left.name == right.name && left.type == right.type && left.member == right.member
                && left.sourceIndex == right.sourceIndex;
        };
        std::map<std::string, const MapInitSymbol*, std::less<>> objectGlobals;
        for (const auto& span : generated.objectSpans) {
            budget.Poll();
            const auto [found, added] = objectGlobals.emplace(span.source.name, &span.source);
            if (!added && !sameSource(*found->second, span.source))
                fail("CLASSIC_LUA_DEPENDENCY_GLOBAL", "同名地图对象的生成来源不一致：" + span.source.name);
        }
        for (const auto& name : generated.requiredGlobals) {
            budget.Poll();
            const auto symbol = mapGlobals.find(name);
            if (symbol != mapGlobals.end()) {
                const auto& expected = *symbol->second;
                const auto source = objectGlobals.find(name);
                const bool bound = source != objectGlobals.end() && sameSource(*source->second, expected);
                if (!bound || !type(expected.type, true) || declarations.contains(name))
                    fail("CLASSIC_LUA_DEPENDENCY_GLOBAL", "地图对象全局未绑定到同类型的生成来源：" + name);
            } else {
                const auto actual = declarations.find(name);
                if (actual == declarations.end() || actual->second->kind != Jass::SymbolKind::Global
                    || actual->second->array || !type(actual->second->type, true))
                    fail("CLASSIC_LUA_DEPENDENCY_GLOBAL", "Lua 全局既不是地图生成对象，也不是 common.j 中可读取的标量：" + name);
            }
        }
        budget.Poll();
    } catch (const Jass::detail::CheckAborted&) {
        fail("CLASSIC_LUA_DEPENDENCY_CHECK_INCOMPLETE", "经典 Lua 依赖核验已取消、超时或达到预算");
    } catch (const std::exception&) {
        fail("CLASSIC_LUA_DEPENDENCY_CHECK_FAILED", "经典 Lua 依赖核验异常终止，未放行部分依赖");
    }
    return result;
}

} // namespace War3Map::Script
