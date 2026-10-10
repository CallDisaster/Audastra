#include "War3MapCore/Lua/LuaSemantic.h"
#include "LuaCompileInspection.h"

extern "C" {
#include "lobject.h"
#include "lopcodes.h"
#include "lstate.h"
}

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>

namespace War3Map::Lua {
namespace {
struct AnalysisStopped {};
std::string_view Text(const TString* value) {
    return value ? std::string_view(getstr(value), tsslen(value)) : std::string_view{};
}
std::string_view Constant(const Proto& proto, int index) {
    if (index < 0 || index >= proto.sizek || !ttisstring(&proto.k[index])) return {};
    return Text(tsvalue(&proto.k[index]));
}
std::string_view Key(const Proto& proto, int operand) {
    return ISK(operand) ? Constant(proto, INDEXK(operand)) : std::string_view{};
}
bool Environment(const Proto& proto, int index) {
    return index >= 0 && index < proto.sizeupvalues && Text(proto.upvalues[index].name) == "_ENV";
}
const std::set<std::string, std::less<>>& StandardGlobals() {
    // Lua 5.3标准库的全局入口，内部字段不凭此表声明为运行库接口。
    static const std::set<std::string, std::less<>> values{
        "_G", "_VERSION", "assert", "collectgarbage", "coroutine", "debug", "dofile", "error",
        "getmetatable", "io", "ipairs", "load", "loadfile", "math", "next", "os", "package",
        "pairs", "pcall", "print", "rawequal", "rawget", "rawlen", "rawset", "require",
        "select", "setmetatable", "string", "table", "tonumber", "tostring", "type", "utf8", "xpcall"};
    return values;
}
bool ValidContext(const SemanticContext& context) {
    constexpr std::size_t limit = 16 * 1024 * 1024;
    std::size_t bytes = 0;
    const auto text = [&](std::string_view value) {
        if (value.empty() || value.size() > 1024 || value.find('\0') != std::string_view::npos
            || value.size() > limit - (std::min)(limit, bytes)) return false;
        bytes += value.size(); return true;
    };
    if (context.runtime.binding.size() > 128 || context.runtime.reason.size() > 4096
        || context.runtime.presentModules.size() > 256 || context.runtime.commonArguments.size() > 8192
        || context.mapGlobals.size() > 262144) return false;
    for (const auto& module : context.runtime.presentModules) if (!text(module)) return false;
    for (const auto& [name, count] : context.runtime.commonArguments) if (!text(name) || count > 255) return false;
    for (const auto& name : context.mapGlobals) if (!text(name)) return false;
    return true;
}
std::string BoundedSourceReason(std::string_view value) {
    constexpr std::size_t limit = 1024;
    if (value.size() <= limit) return std::string(value);
    std::size_t end = limit;
    while (end > 0 && (static_cast<unsigned char>(value[end]) & 0xc0u) == 0x80u) --end;
    return std::string(value.substr(0, end)) + "…";
}
enum class ValueKind { Unknown, Global, String, Module, Field };
struct Value {
    ValueKind kind = ValueKind::Unknown;
    std::string first, second;
};
struct Analyzer {
    SemanticResult& result;
    const SemanticContext& context;
    const SyntaxOptions& options;
    std::chrono::steady_clock::time_point deadline;
    bool warn = false;
    bool environmentStable = true;
    std::size_t instructions = 0, prototypes = 0;
    bool diagnosticsTruncated = false;

    void Poll() const {
        if (options.cancel.stop_requested() || std::chrono::steady_clock::now() >= deadline) throw AnalysisStopped{};
    }
    void Add(std::string code, std::string message, int line) {
        if (!warn) return;
        if (result.warnings.size() >= (std::min)(context.maxDiagnostics, std::size_t{4096})) { diagnosticsTruncated = true; return; }
        result.warnings.push_back({std::move(code), std::move(message), static_cast<std::uint32_t>((std::max)(0, line))});
    }
    int Line(const Proto& proto, int pc) const {
        return pc >= 0 && pc < proto.sizelineinfo ? proto.lineinfo[pc] : proto.linedefined;
    }
    void Collect(const Proto& proto, bool inheritedEnvironment, unsigned depth = 0) {
        Poll();
        if (++prototypes > 65536 || depth > 200 || proto.sizecode < 0
            || static_cast<std::size_t>(proto.sizecode) > (std::min)(context.maxInstructions, std::size_t{2 * 1024 * 1024})
                - (std::min)(instructions, (std::min)(context.maxInstructions, std::size_t{2 * 1024 * 1024})))
            throw AnalysisStopped{};
        instructions += static_cast<std::size_t>(proto.sizecode);
        bool validEnvironment = inheritedEnvironment;
        for (int pc = 0; pc < proto.sizecode; ++pc) {
            if ((pc & 4095) == 0) Poll();
            const auto code = proto.code[pc];
            if (validEnvironment && GET_OPCODE(code) == OP_SETUPVAL && Environment(proto, GETARG_B(code))) {
                environmentStable = false; result.globalDefinitionsComplete = false;
            }
            if (validEnvironment && GET_OPCODE(code) == OP_SETTABUP && Environment(proto, GETARG_A(code))) {
                const auto key = Key(proto, GETARG_B(code));
                if (!key.empty() && key.size() <= 1024 && result.declaredGlobals.size() < 262144) result.declaredGlobals.emplace(key);
                else result.globalDefinitionsComplete = false;
            }
        }
        for (int child = 0; child < proto.sizep; ++child) {
            bool childEnvironment = false;
            const auto& nested = *proto.p[child];
            for (int upvalue = 0; upvalue < nested.sizeupvalues; ++upvalue) {
                const auto& captured = nested.upvalues[upvalue];
                if (Text(captured.name) == "_ENV" && !captured.instack
                    && Environment(proto, captured.idx)) childEnvironment = validEnvironment;
            }
            Collect(nested, childEnvironment, depth + 1);
        }
    }
    Value Field(const Value& base, std::string_view key) const {
        if (key.empty() || key.size() > 1024) return {};
        if (base.kind == ValueKind::Module) return {ValueKind::Field, base.first, std::string(key)};
        return {};
    }
    void Function(const Proto& proto, bool validEnvironment) {
        Poll();
        std::vector<bool> leaders(static_cast<std::size_t>(proto.sizecode) + 1, false);
        leaders.front() = true;
        const auto leader = [&](int pc) {
            if (pc >= 0 && pc <= proto.sizecode) leaders[static_cast<std::size_t>(pc)] = true;
        };
        for (int pc = 0; pc < proto.sizecode; ++pc) {
            if ((pc & 4095) == 0) Poll();
            const auto code = proto.code[pc]; const auto op = GET_OPCODE(code);
            if (op == OP_JMP || op == OP_FORLOOP || op == OP_FORPREP || op == OP_TFORLOOP) {
                leader(pc + 1 + GETARG_sBx(code)); leader(pc + 1);
            }
            if (testTMode(op) || (op == OP_LOADBOOL && GETARG_C(code))) { leader(pc + 1); leader(pc + 2); }
            if (op == OP_RETURN || op == OP_TAILCALL) leader(pc + 1);
        }
        // 每个函数的符号状态放堆上，避免合法嵌套原型耗尽Windows工作线程栈。
        std::vector<Value> values(256);
        std::vector<int> locals(256, -1);
        bool runtimePathsStable = !context.mapGlobals.contains("require") && !context.mapGlobals.contains("package");
        std::vector<int> active;
        std::vector<bool> used(static_cast<std::size_t>(proto.sizelocvars), false);
        int nextLocal = 0;
        const auto read = [&](int reg) {
            if (reg >= 0 && reg < 256 && locals[static_cast<std::size_t>(reg)] >= 0)
                used[static_cast<std::size_t>(locals[static_cast<std::size_t>(reg)])] = true;
        };
        const auto rk = [&](int operand) { if (!ISK(operand)) read(operand); };
        const auto range = [&](int begin, int end) { for (int reg = begin; reg < (std::min)(end, 256); ++reg) read(reg); };
        const auto global = [&](std::string_view key, int line) {
            if (!key.empty() && context.mapGlobalsComplete && result.globalDefinitionsComplete && environmentStable
                && !StandardGlobals().contains(key) && !context.mapGlobals.contains(key)
                && !result.declaredGlobals.contains(key) && !(key == "jass" && !context.runtime.presentModules.empty()))
                Add("LUA_UNDEFINED_GLOBAL", "未在已冻结来源中找到全局定义：" + std::string(key), line);
        };
        for (int pc = 0; pc < proto.sizecode; ++pc) {
            if ((pc & 1023) == 0) Poll();
            // Lua调试局部变量按开始pc排序；寄存器对应当时活动局部变量的顺序。
            std::erase_if(active, [&](int index) { return proto.locvars[index].endpc <= pc; });
            while (nextLocal < proto.sizelocvars && proto.locvars[nextLocal].startpc <= pc) {
                if (proto.locvars[nextLocal].endpc > pc) active.push_back(nextLocal);
                ++nextLocal;
            }
            std::fill(locals.begin(), locals.end(), -1);
            for (std::size_t reg = 0; reg < active.size() && reg < locals.size(); ++reg) locals[reg] = active[reg];
            if (leaders[static_cast<std::size_t>(pc)]) std::fill(values.begin(), values.end(), Value{});
            const auto code = proto.code[pc]; const auto op = GET_OPCODE(code);
            const int a = GETARG_A(code), b = GETARG_B(code), c = GETARG_C(code);
            if (getOpMode(op) == iABC) {
                if (getBMode(op) == OpArgR) read(b); else if (getBMode(op) == OpArgK) rk(b);
                if (getCMode(op) == OpArgR) read(c); else if (getCMode(op) == OpArgK) rk(c);
            }
            switch (op) {
            case OP_SETUPVAL: case OP_SETTABLE: case OP_TEST: read(a); break;
            case OP_CALL: case OP_TAILCALL: range(a, b ? a + b : proto.maxstacksize); break;
            case OP_RETURN: range(a, b ? a + b - 1 : proto.maxstacksize); break;
            case OP_FORLOOP: case OP_FORPREP: case OP_TFORCALL: range(a, a + 3); break;
            case OP_TFORLOOP: read(a + 1); break;
            case OP_SETLIST: read(a); range(a + 1, b ? a + b + 1 : proto.maxstacksize); break;
            case OP_CONCAT: range(b, c + 1); break;
            case OP_CLOSURE: {
                const auto& child = *proto.p[GETARG_Bx(code)];
                for (int index = 0; index < child.sizeupvalues; ++index) if (child.upvalues[index].instack) read(child.upvalues[index].idx);
                break;
            }
            default: break;
            }
            Value output;
            if (op == OP_MOVE) output = values[static_cast<std::size_t>(b)];
            else if (op == OP_LOADK || op == OP_LOADKX) {
                const auto key = Constant(proto, op == OP_LOADK ? GETARG_Bx(code) : GETARG_Ax(proto.code[pc + 1]));
                // 只有短名称用于模块/字段解析，不复制大型字符串常量到符号状态。
                if (!key.empty() && key.size() <= 1024) output = {ValueKind::String, std::string(key), {}};
            } else if (op == OP_GETTABUP && validEnvironment && environmentStable && Environment(proto, b)) {
                const auto key = Key(proto, c);
                if (!key.empty() && key.size() <= 1024) {
                    global(key, Line(proto, pc));
                    if (!result.declaredGlobals.contains(key) && !context.mapGlobals.contains(key)
                        && runtimePathsStable) output = {ValueKind::Global, std::string(key), {}};
                }
            } else if (op == OP_GETTABLE || op == OP_SELF) {
                auto key = Key(proto, c);
                if (key.empty() && !ISK(c) && values[static_cast<std::size_t>(c)].kind == ValueKind::String) key = values[static_cast<std::size_t>(c)].first;
                output = Field(values[static_cast<std::size_t>(b)], key);
                if (op == OP_SELF) values[static_cast<std::size_t>(a + 1)] = values[static_cast<std::size_t>(b)];
            } else if (op == OP_CALL || op == OP_TAILCALL) {
                const auto& function = values[static_cast<std::size_t>(a)];
                const bool standardRequire = runtimePathsStable && function.kind == ValueKind::Global && function.first == "require";
                if (standardRequire && b == 2) {
                    const auto& argument = values[static_cast<std::size_t>(a + 1)];
                    if (argument.kind == ValueKind::String) {
                        const auto& name = argument.first;
                        if (context.runtime.presentModules.contains(name)) output = {ValueKind::Module, name, {}};
                        else if (name.starts_with("jass.") && context.runtime.moduleRegistryComplete)
                            Add("LUA_RUNTIME_MODULE_MISSING", "已核验的当前运行库未注册模块：" + name, Line(proto, pc));
                    }
                } else if (function.kind == ValueKind::Field && function.first == "jass.common"
                    && runtimePathsStable && context.runtime.commonLookupVerified && b > 0) {
                    if (const auto found = context.runtime.commonArguments.find(function.second); found != context.runtime.commonArguments.end()
                        && found->second != static_cast<std::size_t>(b - 1))
                        Add("LUA_COMMON_ARGUMENT_COUNT", "jass.common." + function.second + " 参数个数与已绑定common.j不符：需要 "
                            + std::to_string(found->second) + "，当前为 " + std::to_string(b - 1), Line(proto, pc));
                }
                const int end = c == 0 || op == OP_TAILCALL ? proto.maxstacksize : a + (std::max)(1, c - 1);
                for (int reg = a; reg < (std::min)(end, 256); ++reg) values[static_cast<std::size_t>(reg)] = {};
                // 任意Lua调用可重写模块表；其后仅保留本次已证实require的返回路径。
                std::fill(values.begin(), values.end(), Value{});
                if (!standardRequire) runtimePathsStable = false;
            }
            if (testAMode(op) || op == OP_CALL || op == OP_TAILCALL) values[static_cast<std::size_t>(a)] = std::move(output);
            if (op == OP_LOADNIL) for (int reg = a; reg <= a + b && reg < 256; ++reg) values[static_cast<std::size_t>(reg)] = {};
            if (op == OP_VARARG) for (int reg = a; reg < (b ? a + b - 1 : proto.maxstacksize); ++reg) values[static_cast<std::size_t>(reg)] = {};
            if (op == OP_TFORCALL) for (int reg = a + 3; reg < a + 3 + c; ++reg) values[static_cast<std::size_t>(reg)] = {};
            if (op == OP_FORPREP || op == OP_FORLOOP)
                for (int reg = a; reg < a + 4; ++reg) values[static_cast<std::size_t>(reg)] = {};
            // 表/环境可能由作者重写；写操作之后不保留“原生模块函数”假设。
            if (op == OP_SETTABLE || op == OP_SETTABUP || op == OP_SETUPVAL) {
                std::fill(values.begin(), values.end(), Value{});
                runtimePathsStable = false;
            }
        }
        for (int index = 0; index < proto.sizelocvars; ++index) {
            if ((index & 1023) == 0) Poll();
            const auto name = Text(proto.locvars[index].varname);
            if (!used[static_cast<std::size_t>(index)] && !name.empty() && name.size() <= 1024 && name.front() != '(' && name != "_" && name != "_ENV")
                Add("LUA_UNUSED_LOCAL", "未读取局部变量：" + std::string(name), Line(proto, (std::max)(0, proto.locvars[index].startpc - 1)));
        }
        for (int child = 0; child < proto.sizep; ++child) {
            const auto& nested = *proto.p[child]; bool childEnvironment = false;
            for (int index = 0; index < nested.sizeupvalues; ++index)
                if (Text(nested.upvalues[index].name) == "_ENV" && !nested.upvalues[index].instack
                    && Environment(proto, nested.upvalues[index].idx)) childEnvironment = validEnvironment;
            Function(nested, childEnvironment);
        }
    }
};
SemanticResult Analyze(std::string_view source, std::string_view name, const SemanticContext& context,
    const SyntaxOptions& options, bool collectOnly) {
    SemanticResult result;
    if (!collectOnly && !ValidContext(context)) {
        result.syntax = CheckSyntax(source, name, options);
        result.reason = "目前只检查语法（Lua 5.3）；语义来源名称、数量或累计文本超过预算。";
        return result;
    }
    if (!collectOnly && (!context.runtime.verified || context.runtime.binding.empty())) {
        result.syntax = CheckSyntax(source, name, options);
        result.reason = "目前只检查语法（Lua 5.3）；" + (context.runtime.reason.empty()
            ? std::string("当前检查未加载已核验的本机运行库接口清单，未验证 jass.* 接口存在性。") : context.runtime.reason);
        return result;
    }
    const auto deadline = (std::min)(options.deadline, std::chrono::steady_clock::now()
        + std::clamp(options.maxDuration, std::chrono::milliseconds::zero(), std::chrono::milliseconds(30000)));
    result.syntax = Detail::CheckSyntaxWithInspection(source, name, options, [&](lua_State* state) {
        try {
            Analyzer analyzer{result, context, options, deadline, !collectOnly};
            const auto& proto = *getproto(state->top - 1);
            analyzer.Collect(proto, true);
            if (!collectOnly) analyzer.Function(proto, true);
            result.checked = !collectOnly;
            // 参数检查仅覆盖固定参数、基本块内可证明路径；不声称整个程序动态语义完备。
            result.complete = false;
            result.reason = collectOnly ? "已按编译原型收集常量全局定义。"
                : "已检查Lua静态语义提示；动态调用、跨分支别名与变参数量仍未确定。";
            if (!collectOnly) {
                if (!context.runtime.reason.empty()) result.reason += " " + BoundedSourceReason(context.runtime.reason);
                // 关键边界单独追加，不因来源说明长度而丢失。
                result.reason += " 来源仅为所选本机运行库的静态声明契约，未确认实际游戏加载。";
                result.reason += " 本次后台捕获的接口库存绑定：" + context.runtime.binding + "。";
            }
            if (!collectOnly && !context.mapGlobalsComplete) result.reason += " 地图全局上下文未完整，未提示未定义全局。";
            if (!collectOnly && !context.runtime.moduleRegistryComplete) result.reason += " 模块注册覆盖未完整，未将未知模块判为缺失。";
            if (analyzer.diagnosticsTruncated) result.reason += " 警告已达到显示预算。";
        } catch (...) {
            result.checked = false; result.complete = false; result.globalDefinitionsComplete = false;
            result.warnings.clear(); result.declaredGlobals.clear();
            result.reason = "语法编译已完成；Lua语义检查已取消、超过预算或无法完成，未形成语义结论。";
        }
    });
    return result;
}
} // namespace

SemanticResult CheckSemantics(std::string_view source, std::string_view name,
    const SemanticContext& context, const SyntaxOptions& options) {
    return Analyze(source, name, context, options, false);
}
SemanticResult CollectGlobalDefinitions(std::string_view source, std::string_view name, const SyntaxOptions& options) {
    return Analyze(source, name, {}, options, true);
}
} // namespace War3Map::Lua
