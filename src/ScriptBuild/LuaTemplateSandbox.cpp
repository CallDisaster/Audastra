#include "War3MapCore/Script/LuaTemplateSandbox.h"
#include "War3MapCore/Script/ScriptToolchainProfile.h"

#include "LuaTemplateSlkBridge.h"
#include "ScriptNames.h"
#include "TemplateImports.h"

#include "lauxlib.h"
#include "lualib.h"

#include <algorithm>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace War3Map::Script {
namespace detail {
extern const std::string_view kLuaTemplateSlkSource;
} // namespace detail

namespace {

constexpr std::string_view kSubsystem = "Script.LuaTemplate";
constexpr int kHookInterval = 10000;
constexpr const char* kHostKey = "audastra.template.host";
constexpr const char* kSlkKey = "audastra.template.slk";
constexpr const char* kFinishKey = "audastra.template.finish";

struct alignas(std::max_align_t) Allocation { std::size_t bytes; };

struct Host {
    const LuaTemplateRequest& request;
    std::size_t used = 0, limit = 0;
    std::uint64_t instructions = 0;
    std::chrono::steady_clock::time_point deadline;
    bool memoryExceeded = false, budgetExceeded = false;
    std::vector<MemberReplacement> imports;
    std::vector<Diagnostic> diagnostics;
    detail::TemplateImportSink sink;
    std::vector<std::string> messages;
    std::optional<LuaTemplateSlkSources> slk;
    std::vector<std::uint32_t> lineMap; // 生成 chunk 的行号 -> 预处理脚本行号

    explicit Host(const LuaTemplateRequest& source)
        : request(source), sink(source.importRoot, imports, diagnostics) {}
};

void* Allocate(void* opaque, void* block, std::size_t, std::size_t newBytes) noexcept {
    auto& host = *static_cast<Host*>(opaque);
    auto* allocation = block ? static_cast<Allocation*>(block) - 1 : nullptr;
    const auto oldBytes = allocation ? allocation->bytes : 0;
    if (newBytes == 0) {
        std::free(allocation);
        host.used -= std::min(oldBytes, host.used);
        return nullptr;
    }
    if (newBytes > std::numeric_limits<std::size_t>::max() - sizeof(Allocation)) return nullptr;
    const auto requested = newBytes + sizeof(Allocation);
    // Lua 分配器契约：缩小不能失败。
    if (allocation && requested <= oldBytes) {
        auto* smaller = static_cast<Allocation*>(std::realloc(allocation, requested));
        if (!smaller) return block;
        smaller->bytes = requested;
        host.used -= oldBytes - requested;
        return smaller + 1;
    }
    const auto retained = host.used - std::min(oldBytes, host.used);
    if (requested > host.limit || retained > host.limit - requested) {
        host.memoryExceeded = true;
        return nullptr;
    }
    auto* replacement = static_cast<Allocation*>(std::realloc(allocation, requested));
    if (!replacement) { host.memoryExceeded = true; return nullptr; }
    replacement->bytes = requested;
    host.used = retained + requested;
    return replacement + 1;
}

Host& HostOf(lua_State* lua) {
    void* opaque = nullptr;
    lua_getallocf(lua, &opaque);
    return *static_cast<Host*>(opaque);
}

void Hook(lua_State* lua, lua_Debug*) {
    auto& host = HostOf(lua);
    host.instructions += kHookInterval;
    if (host.instructions > host.request.limits.instructions) {
        host.budgetExceeded = true;
        luaL_error(lua, "模板执行步数超过上限（可能是死循环）");
    }
    if (std::chrono::steady_clock::now() >= host.deadline) {
        host.budgetExceeded = true;
        luaL_error(lua, "模板执行超时");
    }
}

// Lua 5.3 %q 的转义规则：换行写成反斜杠加换行，因此生成 chunk 与原文逐行对应。
void Quote(std::string& out, std::string_view text) {
    out += '"';
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto c = static_cast<unsigned char>(text[i]);
        if (c == '"' || c == '\\') { out += '\\'; out += static_cast<char>(c); }
        else if (c == '\n') out += "\\\n";
        else if (c == '\r') out += "\\r";
        else if (c == 0) out += "\\0";
        else if (c < 0x20 || c == 0x7F) {
            char buffer[8];
            std::snprintf(buffer, sizeof(buffer), "\\%03u", static_cast<unsigned>(c));
            out += buffer;
        } else out += static_cast<char>(c);
    }
    out += '"';
}

struct Chunk {
    std::string code;
    std::vector<std::uint32_t> lines;
    std::string error;
};

// 与 YDWE/KKWE 相同：块外文本 -> __put(文本)，<?= 表达式 ?> -> __put(表达式)，<? 代码 ?> 原样。
Chunk Precompile(std::string_view text) {
    Chunk chunk;
    std::uint32_t line = 1;
    const auto emit = [&](std::string_view generated) {
        for (const char c : generated) {
            if (c == '\n') { chunk.lines.push_back(line); ++line; }
        }
        chunk.code += generated;
    };
    const auto join = [&] { chunk.lines.push_back(line); chunk.code += '\n'; };
    chunk.code = "local __put, __finish = ... ";
    std::size_t at = 0;
    while (at <= text.size()) {
        const auto open = text.find("<?", at);
        const auto plain = text.substr(at, (open == std::string_view::npos ? text.size() : open) - at);
        if (!plain.empty()) {
            std::string quoted = "__put(";
            Quote(quoted, plain);
            quoted += ")";
            emit(quoted);
            join();
        }
        if (open == std::string_view::npos) break;
        const auto close = text.find("?>", open + 2);
        if (close == std::string_view::npos) {
            chunk.error = "第 " + std::to_string(line) + " 行的模板标记 <? 没有对应的 ?>";
            return chunk;
        }
        const bool expression = open + 2 < text.size() && text[open + 2] == '=';
        const auto code = text.substr(open + (expression ? 3 : 2), close - open - (expression ? 3 : 2));
        if (expression) {
            // 括号强制表达式只保留第一个返回值，与 YDWE 模板里
            // __JASS__[#__JASS__+1]=表达式 的 Lua 赋值语义一致；
            // 例如 memory_hack 的 <?=math.modf(8192 / MHTDSMaxCount)?> 返回两个值。
            // 空表达式 <?= ?> 用于显式输出空串，仍生成 __put()。
            const bool empty = code.find_first_not_of(" \t\r\n") == std::string_view::npos;
            emit(empty ? "__put(" : "__put((");
            emit(code);
            emit(empty ? ")" : "))");
        } else {
            emit(code);
        }
        join();
        at = close + 2;
    }
    chunk.code += "return __finish()";
    chunk.lines.push_back(line);
    return chunk;
}

// 把 "template:N:" 的生成行号换成预处理脚本行号。
std::string Located(const Host& host, std::string message) {
    constexpr std::string_view prefix = "template:";
    for (auto at = message.find(prefix); at != std::string::npos; at = message.find(prefix, at + 1)) {
        auto end = at + prefix.size();
        std::size_t number = 0;
        const auto digits = end;
        while (end < message.size() && message[end] >= '0' && message[end] <= '9')
            number = number * 10 + static_cast<std::size_t>(message[end++] - '0');
        if (end == digits || end >= message.size() || message[end] != ':') continue;
        const auto line = number >= 1 && number <= host.lineMap.size() ? host.lineMap[number - 1] : number;
        const auto replacement = "第 " + std::to_string(line) + " 行";
        message.replace(at, end - at, replacement);
        at += replacement.size();
    }
    return message;
}

int Put(lua_State* lua) {
    const int count = lua_gettop(lua);
    if (count == 0 || lua_isnil(lua, 1)) return 0;
    // YDWE 的模板执行在 Lua 5.1 的赋值上下文里，多返回值只保留第一个。
    if (count > 1) lua_settop(lua, 1);
    if (lua_type(lua, 1) == LUA_TNUMBER && !lua_isinteger(lua, 1)) {
        const auto number = lua_tonumber(lua, 1);
        if (number != number || number == HUGE_VAL || number == -HUGE_VAL)
            return luaL_error(lua, "<?= ?> 的结果不是有限数值，不能写进脚本");
    }
    if (lua_type(lua, 1) == LUA_TNUMBER) {
        // 与 Lua 5.1 的 tostring 一致：整数值的浮点不补 ".0"，
        // 否则 32.0 会写进 JASS 的 integer 表达式并被 pjass 拒绝。
        char formatted[64]{};
        if (lua_isinteger(lua, 1))
            std::snprintf(formatted, sizeof(formatted), "%lld", static_cast<long long>(lua_tointeger(lua, 1)));
        else
            std::snprintf(formatted, sizeof(formatted), "%.14g", static_cast<double>(lua_tonumber(lua, 1)));
        lua_pushstring(lua, formatted);
    } else if (!lua_isstring(lua, 1)) {
        return luaL_error(lua, "<?= ?> 的结果是 %s，不能写进脚本", lua_typename(lua, lua_type(lua, 1)));
    } else {
        lua_pushvalue(lua, 1);
        lua_tostring(lua, -1);
    }
    lua_getfield(lua, LUA_REGISTRYINDEX, "audastra.template.output");
    const auto length = static_cast<lua_Integer>(luaL_len(lua, -1));
    lua_pushvalue(lua, -2);
    lua_tostring(lua, -1);
    lua_rawseti(lua, -2, length + 1);
    return 0;
}

int StringHash(lua_State* lua) {
    std::size_t size = 0;
    const char* text = luaL_checklstring(lua, 1, &size);
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "0x%08X", StormStringHash(std::string_view(text, size)));
    lua_pushstring(lua, buffer);
    return 1;
}

int AbilityOrder(lua_State* lua) {
    const char* ability = luaL_checkstring(lua, 1);
    const char* type = luaL_checkstring(lua, 2);
    auto& host = HostOf(lua);
    std::optional<std::int64_t> order;
    if (host.request.abilityOrder) order = host.request.abilityOrder(ability, type);
    if (!order) {
        // 与内置求值一致：按 0 处理并警告；框架子集由工具链把此警告升级为失败。
        host.diagnostics.push_back({DiagnosticSeverity::Warning, "Script.Template", "ABILITY_ORDER_UNKNOWN",
            std::string("无法确定技能 ") + ability + " 的命令 ID（" + type + "），按 0 处理", {}});
    }
    lua_pushinteger(lua, static_cast<lua_Integer>(order.value_or(0)));
    return 1;
}

int ImportWrite(lua_State* lua) {
    auto& host = HostOf(lua);
    const std::string member = lua_tostring(lua, lua_upvalueindex(1));
    std::size_t size = 0;
    const char* content = luaL_checklstring(lua, 1, &size);
    const bool path = lua_toboolean(lua, 2) != 0;
    std::string error;
    if (!host.sink.Add(member, std::string_view(content, size), path, error))
        return luaL_error(lua, "%s", error.c_str());
    return 0;
}

int Import(lua_State* lua) {
    auto& host = HostOf(lua);
    std::string error;
    const auto member = host.sink.MemberName(luaL_checkstring(lua, 1), error);
    if (!member) return luaL_error(lua, "%s", error.c_str());
    lua_pushlstring(lua, member->data(), member->size());
    lua_pushcclosure(lua, ImportWrite, 1);
    return 1;
}

int Print(lua_State* lua) {
    auto& host = HostOf(lua);
    std::string line;
    for (int i = 1; i <= lua_gettop(lua); ++i) {
        if (i > 1) line += '\t';
        line += luaL_tolstring(lua, i, nullptr);
        lua_pop(lua, 1);
    }
    if (host.messages.size() < 200) host.messages.push_back(line.substr(0, 4096));
    return 0;
}

int Require(lua_State* lua) {
    const char* name = luaL_checkstring(lua, 1);
    if (std::string_view(name) != "slk") return luaL_error(lua, "模板只能 require 'slk'，不能加载 '%s'", name);
    if (lua_getfield(lua, LUA_REGISTRYINDEX, kSlkKey) != LUA_TNIL) return 1;
    lua_pop(lua, 1);
    auto& host = HostOf(lua);
    if (!host.request.slk) return luaL_error(lua, "当前没有可用的物编数据，模板不能使用 slk 库");
    auto sources = host.request.slk();
    if (!sources.Succeeded() || !sources.value) {
        const std::string message = sources.diagnostics.empty() ? "无法准备 slk 物编数据" : sources.diagnostics.front().message;
        return luaL_error(lua, "%s", message.c_str());
    }
    host.slk = std::move(*sources.value);
    const auto library = detail::kLuaTemplateSlkSource;
    if (luaL_loadbufferx(lua, library.data(), library.size(), "=slk", "t") != LUA_OK) return lua_error(lua);
    lua_rawgeti(lua, LUA_REGISTRYINDEX, LUA_RIDX_GLOBALS);
    lua_setupvalue(lua, -2, 1);
    const auto error = detail::PushSlkData(lua, *host.slk, &host.diagnostics);
    if (!error.empty()) return luaL_error(lua, "%s", error.c_str());
    lua_call(lua, 1, 2);
    lua_setfield(lua, LUA_REGISTRYINDEX, kFinishKey);
    lua_pushvalue(lua, -1);
    lua_setfield(lua, LUA_REGISTRYINDEX, kSlkKey);
    return 1;
}

// 模板结束：拼接输出，并在使用过 slk 时取得物编改动。
int Finish(lua_State* lua) {
    lua_getfield(lua, LUA_REGISTRYINDEX, "audastra.template.output");
    const auto count = luaL_len(lua, -1);
    const int table = lua_gettop(lua);
    luaL_Buffer buffer;
    luaL_buffinit(lua, &buffer);
    auto& host = HostOf(lua);
    std::size_t total = 0;
    for (lua_Integer i = 1; i <= count; ++i) {
        lua_rawgeti(lua, table, i);
        std::size_t size = 0;
        lua_tolstring(lua, -1, &size);
        total += size;
        if (total > host.request.limits.outputBytes) return luaL_error(lua, "模板输出超过上限");
        luaL_addvalue(&buffer);
    }
    luaL_pushresult(&buffer);
    if (lua_getfield(lua, LUA_REGISTRYINDEX, kFinishKey) == LUA_TFUNCTION) {
        lua_call(lua, 0, 1);
    } else {
        lua_pop(lua, 1);
        lua_pushnil(lua);
    }
    return 2;
}

struct Applied {
    TemplateTransactionOutput* output;
    std::string error;
};

int ApplyProtected(lua_State* lua) {
    auto& applied = *static_cast<Applied*>(lua_touserdata(lua, 1));
    auto& host = HostOf(lua);
    applied.error = detail::ApplySlkResult(lua, 2, *host.slk, *applied.output);
    return 0;
}

void OpenLibraries(lua_State* lua) {
    luaL_requiref(lua, "_G", luaopen_base, 1); lua_pop(lua, 1);
    luaL_requiref(lua, "table", luaopen_table, 1); lua_pop(lua, 1);
    luaL_requiref(lua, "string", luaopen_string, 1); lua_pop(lua, 1);
    luaL_requiref(lua, "math", luaopen_math, 1); lua_pop(lua, 1);
    luaL_requiref(lua, "utf8", luaopen_utf8, 1); lua_pop(lua, 1);
    lua_rawgeti(lua, LUA_REGISTRYINDEX, LUA_RIDX_GLOBALS);
    // 不能加载代码或文件；print 收集到工具消息。
    for (const char* name : {"dofile", "loadfile", "load", "collectgarbage"}) {
        lua_pushnil(lua);
        lua_setfield(lua, -2, name);
    }
    lua_getfield(lua, -1, "string");
    lua_pushnil(lua);
    lua_setfield(lua, -2, "dump");
    lua_pop(lua, 1);
    lua_pushcfunction(lua, Print);
    lua_setfield(lua, -2, "print");
    lua_pushcfunction(lua, Require);
    lua_setfield(lua, -2, "require");
    lua_pop(lua, 1);
}

// 模板 chunk 的环境：YDWE 提供的函数 + 只读回退到沙盒全局表，模板自己的全局变量写在环境里。
void PushEnvironment(lua_State* lua) {
    lua_createtable(lua, 0, 4);
    lua_pushcfunction(lua, Import);
    lua_setfield(lua, -2, "import");
    lua_pushcfunction(lua, StringHash);
    lua_setfield(lua, -2, "StringHash");
    lua_pushcfunction(lua, AbilityOrder);
    lua_setfield(lua, -2, "abilityOrder");
    lua_pushcfunction(lua, AbilityOrder);
    lua_setfield(lua, -2, "ability2order");
    lua_createtable(lua, 0, 1);
    lua_rawgeti(lua, LUA_REGISTRYINDEX, LUA_RIDX_GLOBALS);
    lua_setfield(lua, -2, "__index");
    lua_setmetatable(lua, -2);
}

// 原创实现：框架 XGuserdataGC 插件头文件调用的模板函数。gc_bind_unit/gc_bind_item 先标记触发器，
// 之后同一触发器的 TriggerAddAction 照常输出，并追加一次对应的绑定调用；未标记的触发器原样输出。
constexpr std::string_view kUserdataGcLibrary = R"lua(
local pending = {}
local function trim(text) return (text:match('^%s*(.-)%s*$')) end
function gc_bind_unit_lua(trig) pending[trim(trig)] = 'gc_bind_unit'; return 'DoNothing()' end
function gc_bind_item_lua(trig) pending[trim(trig)] = 'gc_bind_item'; return 'DoNothing()' end
function TriggerAddAction_lua(trig, action)
    local text = 'TriggerAddAction(' .. trig .. ',' .. action .. ')'
    local key = trim(trig)
    local bind = pending[key]
    if not bind then return text end
    pending[key] = nil
    return text .. '\ncall ' .. bind .. '(' .. trim(action) .. ')'
end
)lua";

// 把已开启的函数集定义到模板环境（栈顶）；失败时返回原因。
std::string LoadLibraries(lua_State* lua, const std::vector<std::string>& libraries) {
    for (const auto& id : libraries) {
        if (id != kUserdataGcTemplateLibrary) return "未知模板函数集：" + id;
        int status = luaL_loadbufferx(lua, kUserdataGcLibrary.data(), kUserdataGcLibrary.size(), "=userdata-gc", "t");
        if (status == LUA_OK) {
            lua_pushvalue(lua, -2); // 函数集写进模板环境
            lua_setupvalue(lua, -2, 1);
            status = lua_pcall(lua, 0, 0, 0);
        }
        if (status != LUA_OK) {
            std::string message = lua_type(lua, -1) == LUA_TSTRING ? lua_tostring(lua, -1) : "未知错误";
            lua_pop(lua, 1);
            return "模板函数集 " + id + " 加载失败：" + message;
        }
    }
    return {};
}

struct StateCloser {
    void operator()(lua_State* lua) const noexcept { lua_close(lua); }
};

Result<TemplateTransactionOutput> Fail(std::string code, std::string message) {
    return Result<TemplateTransactionOutput>::Failure(std::string(kSubsystem), std::move(code), std::move(message));
}

} // namespace

std::optional<std::filesystem::path> FindTemplateW3x2lniRoot(const std::filesystem::path& toolRoot) {
    std::error_code error;
    for (const auto& candidate : {toolRoot, toolRoot / "share" / "w3x2lni"}) {
        // 新版 core 或旧入口 prebuilt 的 metadata 任一存在；具体用哪套由 DetectSlkDatasetLayout 判定。
        const auto script = candidate / "plugin" / "w3x2lni" / "script";
        if (std::filesystem::is_regular_file(script / "core" / "defined" / "metadata.ini", error) ||
            std::filesystem::is_regular_file(script / "prebuilt" / "defined" / "metadata.ini", error))
            return candidate;
    }
    return std::nullopt;
}

Result<TemplateTransactionOutput> RunLuaTemplate(const LuaTemplateRequest& request) {
    Host host(request);
    host.limit = request.limits.memoryBytes;
    host.deadline = std::chrono::steady_clock::now() + request.limits.timeout;
    auto chunk = Precompile(request.preprocessed);
    if (!chunk.error.empty()) return Fail("TEMPLATE_UNTERMINATED", chunk.error);
    host.lineMap = std::move(chunk.lines);

    std::unique_ptr<lua_State, StateCloser> state(lua_newstate(Allocate, &host));
    if (!state) return Fail("TEMPLATE_HOST_FAILED", "无法建立模板 Lua 环境。");
    lua_State* lua = state.get();
    TemplateTransactionOutput output;
    std::string failure;
    {
        // 初始化本身也可能内存不足，统一放在保护调用中。
        lua_pushcfunction(lua, [](lua_State* L) -> int {
            OpenLibraries(L);
            lua_newtable(L);
            lua_setfield(L, LUA_REGISTRYINDEX, "audastra.template.output");
            return 0;
        });
        if (lua_pcall(lua, 0, 0, 0) != LUA_OK) return Fail("TEMPLATE_HOST_FAILED", "无法初始化模板 Lua 环境。");
    }
    if (luaL_loadbufferx(lua, chunk.code.data(), chunk.code.size(), "=template", "t") != LUA_OK) {
        failure = lua_isstring(lua, -1) ? lua_tostring(lua, -1) : "模板代码无法编译";
        return Fail("TEMPLATE_SYNTAX", "模板 Lua 代码有语法错误：" + Located(host, failure));
    }
    chunk.code.clear();
    chunk.code.shrink_to_fit();
    PushEnvironment(lua);
    if (const auto error = LoadLibraries(lua, request.libraries); !error.empty()) return Fail("TEMPLATE_HOST_FAILED", error);
    lua_setupvalue(lua, -2, 1);
    lua_pushcfunction(lua, Put);
    lua_pushcfunction(lua, Finish);
    lua_sethook(lua, Hook, LUA_MASKCOUNT, kHookInterval);
    const int status = lua_pcall(lua, 2, 2, 0);
    lua_sethook(lua, nullptr, 0, 0);
    if (status != LUA_OK) {
        if (host.memoryExceeded) return Fail("TEMPLATE_LIMIT", "模板执行超过内存上限。");
        if (host.budgetExceeded) return Fail("TEMPLATE_LIMIT", Located(host, lua_type(lua, -1) == LUA_TSTRING ? lua_tostring(lua, -1) : "模板执行超过限制"));
        failure = lua_type(lua, -1) == LUA_TSTRING ? lua_tostring(lua, -1) : "模板执行失败（未知错误）";
        auto result = Fail("TEMPLATE_LUA_ERROR", "模板执行失败：" + Located(host, failure));
        for (std::size_t i = 0; i < host.messages.size() && i < 20; ++i)
            result.diagnostics.push_back({DiagnosticSeverity::Info, std::string(kSubsystem), "TEMPLATE_PRINT", host.messages[i], {}});
        return result;
    }
    std::size_t size = 0;
    const char* text = lua_tolstring(lua, -2, &size);
    output.script.assign(text ? text : "", text ? size : 0);
    if (lua_istable(lua, -1) && host.slk) {
        lua_getfield(lua, -1, "report");
        if (const char* report = lua_tostring(lua, -1); report && *report) {
            std::string_view rest(report);
            while (!rest.empty() && output.toolMessages.size() < 200) {
                const auto end = rest.find('\n');
                if (const auto line = rest.substr(0, end); !line.empty()) output.toolMessages.emplace_back(line);
                rest = end == std::string_view::npos ? std::string_view{} : rest.substr(end + 1);
            }
        }
        lua_pop(lua, 1);
        // slk 报告的问题与 YDWE 一样不中止保存，但逐条作为警告给出。
        lua_getfield(lua, -1, "warnings");
        for (lua_Integer i = 1; lua_istable(lua, -1) && lua_rawgeti(lua, -1, i) == LUA_TSTRING; ++i) {
            if (output.diagnostics.size() < 50)
                output.diagnostics.push_back({DiagnosticSeverity::Warning, std::string(kSubsystem), "TEMPLATE_SLK_REPORT",
                    lua_tostring(lua, -1), {}});
            lua_pop(lua, 1);
        }
        lua_pop(lua, 2);
        Applied applied{&output, {}};
        lua_pushcfunction(lua, ApplyProtected);
        lua_pushlightuserdata(lua, &applied);
        lua_pushvalue(lua, -3);
        if (lua_pcall(lua, 2, 0, 0) != LUA_OK)
            return Fail("TEMPLATE_SLK_WRITE", "写出模板物编失败：" + std::string(lua_type(lua, -1) == LUA_TSTRING ? lua_tostring(lua, -1) : "资源不足"));
        if (!applied.error.empty()) return Fail("TEMPLATE_SLK_WRITE", applied.error);
    }
    for (auto& message : host.messages) output.toolMessages.push_back("print: " + message);
    output.imports = std::move(host.imports);
    for (auto& diagnostic : host.diagnostics) output.diagnostics.push_back(std::move(diagnostic));
    return Result<TemplateTransactionOutput>::Success(std::move(output));
}

std::function<Result<TemplateTransactionOutput>(std::string_view, const std::vector<std::string>&)>
    MakeLuaWholeChunk(const TemplateContext& templates, LuaWholeChunkSources sources, std::optional<SlkMapSourceSnapshot>& slkCache) {
    return [abilityOrder = templates.abilityOrder, importRoot = templates.importRoot, sources = std::move(sources), &slkCache](
        std::string_view preprocessed, const std::vector<std::string>& libraries) -> Result<TemplateTransactionOutput> {
        LuaTemplateRequest lua;
        lua.preprocessed = preprocessed;
        lua.libraries = libraries;
        lua.abilityOrder = abilityOrder;
        lua.importRoot = importRoot;
        lua.slk = [&]() -> Result<LuaTemplateSlkSources> {
            const auto root = sources.toolRoot.empty() ? std::nullopt : FindTemplateW3x2lniRoot(sources.toolRoot);
            if (!root)
                return Result<LuaTemplateSlkSources>::Failure("Script.LuaTemplate", "TEMPLATE_SLK_DATA_MISSING",
                    "工具目录里找不到 w3x2lni 物编数据（plugin/w3x2lni），模板 slk 库不能使用。");
            auto layout = DetectSlkDatasetLayout(*root);
            if (!layout.Succeeded() || !layout.value) return {std::nullopt, std::move(layout.diagnostics)};
            if (!slkCache) {
                if (sources.archive == nullptr)
                    return Result<LuaTemplateSlkSources>::Failure("Script.LuaTemplate", "TEMPLATE_SLK_SOURCE_MISSING",
                        "没有地图归档来源，模板 slk 库不能读取本图物编。");
                auto snapshot = SlkMapSourceSnapshot::Capture(*sources.archive, sources.edits, sources.removals, sources.revision);
                if (!snapshot.Succeeded() || !snapshot.value) return {std::nullopt, std::move(snapshot.diagnostics)};
                slkCache = std::move(*snapshot.value);
            }
            return Result<LuaTemplateSlkSources>::Success({*root, &*slkCache, *layout.value});
        };
        if (sources.beforeRun) sources.beforeRun();
        return RunLuaTemplate(lua);
    };
}

} // namespace War3Map::Script
