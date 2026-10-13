#include "../OwnLua/LuaHost.h"
#include "../OwnLua/LuaMailboxRuntime.h"
#include "../OwnLua/LuaEntryCommand.h"
#include "War3MapCore/Runtime/LuaTestMailboxValidation.h"

#include <windows.h>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>
#include <unordered_map>

namespace {
using namespace Audastra::Runtime32;
using namespace Audastra::Runtime32::OwnLua;
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void Success(const LuaHostResult& result, const char* context) {
    if (!result.Succeeded()) throw std::runtime_error(std::string(context) + ": " + result.message);
}

void TextOnlyAndSandbox() {
    LuaHost host({});
    Success(host.Initialize(), "initialize");
    Success(host.Execute("value = 1; assert(type(string.sub) == 'function'); assert(require('math') == math)", "@first.lua"), "base libraries");
    Success(host.Execute("value = value + 1; assert(value == 2); assert(_VERSION == 'Lua 5.3')", "@second.lua"), "persistent state");
    Success(host.Execute("assert(io == nil and os == nil and package == nil and coroutine == nil); assert(string.dump == nil); assert(type(debug.traceback)=='function' and debug.getregistry==nil and debug.getupvalue==nil)", "@sandbox.lua"), "no external capabilities");
    Success(host.Execute("local t=setmetatable({}, {__index=function() return 7 end}); assert(t.value==7)", "@class.lua"), "ordinary metatables allowed");
    Check(host.Execute("setmetatable({}, {__gc=function() while true do end end})", "@finalizer.lua").code == LuaReloadResult::RuntimeError,
        "unbounded author finalizers refused before registration");
    for (const auto name : {"load", "loadfile", "dofile", "collectgarbage"}) {
        const auto result = host.Execute(std::string(name) + "('return 1')", "@disabled.lua");
        Check(result.code == LuaReloadResult::RuntimeError && result.message.find(name) != std::string::npos, "disabled capability names itself");
    }
    const auto syntax = host.Execute("local broken = )", "@syntax.lua");
    Check(syntax.code == LuaReloadResult::SyntaxError && syntax.message.find("syntax.lua:1:") != std::string::npos, "syntax source line");
    const auto runtime = host.Execute("local function nested() error('intentional') end\nnested()", "@runtime.lua");
    Check(runtime.code == LuaReloadResult::RuntimeError && runtime.message.find("runtime.lua:1:") != std::string::npos
        && runtime.message.find("stack traceback") != std::string::npos, "runtime traceback");
    const auto unavailable = host.Execute("require('jass.common').Player(0)", "@native.lua");
    Check(unavailable.code == LuaReloadResult::RuntimeError && unavailable.message.find("jass.common") != std::string::npos,
        "unimplemented native module not faked");
    Check(!host.Execute(std::string("\x1bLua\0\0", 6), "@binary.lua").Succeeded(), "binary rejected");
    Check(host.Execute(std::string("value=1\0value=2", 15), "@nul.lua").code == LuaReloadResult::InvalidRequest, "NUL rejected");
    bool rejected = false;
    std::thread other([&] { rejected = host.Execute("value=3", "@other.lua").code == LuaReloadResult::NotReady; });
    other.join(); Check(rejected, "only initialized thread executes");
    Success(host.Execute("assert(value == 2)", "@after.lua"), "failed chunk does not reset state");
    Check(!host.Initialize().Succeeded(), "duplicate initialization rejected");
}

void ArchiveModules() {
    const std::unordered_map<std::string, std::string> modules{
        {"main.lua", "loads=(loads or 0)+1; return {value=require('lib.value')}"},
        {"lib\\value.lua", "return 42"},
        {"cycle.lua", "return require('cycle')"},
        {"failure.lua", "error('module rejected')"},
        {"empty.lua", ""}
    };
    std::size_t reads = 0;
    LuaHost host([&](std::string_view member, std::size_t maxBytes) {
        ++reads;
        Check(member.find(':') == std::string::npos && maxBytes == 4 * 1024 * 1024, "member-only bounded reader");
        const auto found = modules.find(std::string(member));
        return found == modules.end() ? LuaModuleRead{} : LuaModuleRead{true, found->second, {}};
    });
    Success(host.Initialize(), "module initialize");
    Success(host.Execute("local m=require('main'); assert(m.value==42); assert(require('main')==m and loads==1); assert(require('empty')==true)", "@loader.lua"), "root and nested modules");
    Check(reads == 3, "cache avoids repeated reader calls");
    Success(host.ExecuteModule("main"), "classic entry shares require cache");
    Check(reads == 3, "classic entry cannot execute a second copy of a loaded module");
    Check(host.ExecuteModule("../outside").code == LuaReloadResult::InvalidRequest, "classic entry refuses escaping map path");
    for (const auto name : {"../outside", "/absolute", "C:outside", "a..b", "a\\\\b"}) {
        const auto before = reads;
        const auto result = host.Execute(std::string("require([[") + name + "]])", "@path.lua");
        Check(result.code == LuaReloadResult::RuntimeError && reads == before, "invalid path refused before reader");
    }
    Check(host.Execute("require('missing')", "@missing.lua").message.find("missing.lua") != std::string::npos, "missing member diagnostic");
    const auto cycle = host.Execute("require('cycle')", "@cycle-test.lua");
    Check(cycle.code == LuaReloadResult::RuntimeError && cycle.message.find("循环依赖") != std::string::npos, "cycle refused");
    const auto failed = host.Execute("require('failure')", "@failure-test.lua");
    Check(failed.code == LuaReloadResult::RuntimeError && failed.message.find("failure.lua:1:") != std::string::npos, "module source preserved");
    // C++ 异常展开必须释放 require 的 loading 标记，失败后重试仍报告原错，不是假循环。
    Check(host.Execute("require('failure')", "@retry.lua").message.find("module rejected") != std::string::npos, "failed loader unwinds");
}

void ClassicEntrySyntax() {
    for (const auto ordinary : {"", "whosyourdaddy", "EXEC-LUA:\"main\"", "other:exec-lua:\"main\""})
        Check(!ParseLuaEntryCommand(ordinary).recognized, "ordinary Cheat command remains original game behavior");
    for (const auto accepted : {"exec-lua:\"main\"", "exec-lua: \"main\"", "exec-lua:\t\"lib.value\" \t"}) {
        const auto result = ParseLuaEntryCommand(accepted);
        Check(result.recognized && !result.module.empty() && result.error.empty(), "documented quoted module entry");
    }
    for (const auto bad : {"exec-lua:", "exec-lua:main", "exec-lua:'main'", "exec-lua:\"\"", "exec-lua:\"../escape\"",
        "exec-lua:\"main\"; error('executed')", "exec-lua:\"C:/outside\"", "exec-lua:\"a..b\"", "exec-lua:\"a/\""}) {
        const auto result = ParseLuaEntryCommand(bad);
        Check(result.recognized && result.module.empty() && !result.error.empty(), "malformed Lua entry never falls back to ordinary Cheat");
    }
    Check(!ParseLuaEntryCommand("exec-lua:\"" + std::string(241, 'x') + "\"").error.empty(), "entry byte budget");
    LuaHost host([](std::string_view member, std::size_t) {
        return member == "entry.lua" ? LuaModuleRead{true, "entrySeen=true; error('entry failed')", {}} : LuaModuleRead{};
    });
    Success(host.Initialize(), "classic entry host");
    const auto error = host.ExecuteModule("entry");
    Check(error.code == LuaReloadResult::RuntimeError && error.message.find("entry.lua:1:") != std::string::npos
        && error.message.find("stack traceback") != std::string::npos, "classic entry keeps actual module line and trace");
    Success(host.Execute("assert(entrySeen)", "@after-entry.lua"), "failed classic entry unwinds host state");
}

void BudgetsAndRandom() {
    LuaHostLimits limits; limits.memoryBytes = 2 * 1024 * 1024; limits.instructions = 5000;
    LuaHost host({}, limits); Success(host.Initialize(), "budget initialize");
    Check(host.Execute("while true do end", "@loop.lua").code == LuaReloadResult::BudgetExceeded, "loop instruction budget");
    Check(host.Execute("while true do pcall(function() while true do end end) end", "@caught.lua").code == LuaReloadResult::BudgetExceeded,
        "pcall cannot swallow exhausted budget");
    Check(host.Execute("large=string.rep('x', 5*1024*1024)", "@memory.lua").code == LuaReloadResult::BudgetExceeded, "memory limit");
    Check(host.MemoryBytes() <= limits.memoryBytes, "allocator accounting bounded");
    Success(host.Execute("math.randomseed(7); randomValue=math.random(1,100); print('预算后仍可执行')", "@recover.lua"), "VM remains usable after protected error");
    LuaHost second({}, limits); Success(second.Initialize(), "independent initialize");
    Success(second.Execute("math.randomseed(11); local ignored=math.random(); math.randomseed(7); assert(math.random(1,100)>=1)", "@separate.lua"), "separate random state");
    Success(host.Execute("math.randomseed(7); assert(math.random(1,100)==randomValue)", "@repeat.lua"), "reproducible per-host random");
    Check(host.ConsoleText().find("预算后仍可执行") != std::string_view::npos, "bounded console captured");
}

void RuntimeBehaviorModules() {
    LuaHost host({}); Success(host.Initialize(), "behavior initialize");
    Success(host.Execute(R"lua(
        runtime = require('jass.runtime')
        assert(runtime == require('jass.runtime'))
        assert(runtime.handle_level == 2 and runtime.catch_crash and runtime.sleep == false)
        runtime.handle_level = 2
        runtime.catch_crash = true
        runtime.sleep = false
        assert(runtime.error_handle == nil)
        runtime.error_handle = function(message)
            errors = (errors or 0) + 1
            lastError = message
            assert(type(debug.traceback()) == 'string')
        end
    )lua", "@configure.lua"), "supported runtime properties");
    const auto original = host.Execute("error('original failure')", "@origin.lua");
    Check(original.code == LuaReloadResult::RuntimeError && original.message.find("origin.lua:1") != std::string::npos,
        "error handler cannot consume original failure");
    Success(host.Execute("assert(errors==1 and lastError:find('original failure',1,true))", "@error-count.lua"), "single error delivery");
    Check(host.Execute("local broken = )", "@broken.lua").code == LuaReloadResult::SyntaxError, "syntax remains syntax after handler");
    Success(host.Execute("assert(errors==2 and lastError:find('broken.lua:1',1,true))", "@syntax-count.lua"), "syntax delivered when handler registered");
    Success(host.Execute("runtime.error_handle=function() error('handler failure') end", "@handler.lua"), "replace handler");
    const auto nested = host.Execute("error('primary failure')", "@primary.lua");
    Check(nested.message.find("primary failure") != std::string::npos && nested.message.find("handler failure") != std::string::npos,
        "handler failure preserves both errors without recursion");
    Success(host.Execute("runtime.error_handle=nil; assert(runtime.error_handle==nil)", "@clear.lua"), "clear handler");
    for (const auto source : {"runtime.handle_level=0", "runtime.catch_crash=false", "runtime.sleep=true", "runtime.unknown=true", "runtime.error_handle=1"}) {
        const auto rejected = host.Execute(source, "@unsupported.lua");
        Check(rejected.code == LuaReloadResult::RuntimeError && rejected.message.find("jass.runtime.") != std::string::npos,
            "unsupported property names itself and does not pretend success");
    }
    Success(host.Execute("console=require('jass.console'); console.enable=false; print('hidden-output'); console.enable=true; print('visible-output')", "@console.lua"),
        "console toggle routes bounded log without a window");
    Check(host.ConsoleText().find("visible-output") != std::string::npos && host.ConsoleText().find("hidden-output") == std::string::npos,
        "console disable changes actual output");
    Check(host.Execute("console.enable=1", "@console-type.lua").code == LuaReloadResult::RuntimeError, "console strict boolean");
    Check(host.Execute("console.write('unknown')", "@console-unknown.lua").message.find("jass.console.write") != std::string::npos,
        "unverified console API refused by name");
    Check(host.Execute("debug.traceback({},1)", "@thread.lua").code == LuaReloadResult::RuntimeError, "no arbitrary stack/thread access");
    for (const auto module : {"jass.japi", "jass.debug", "jass.slk", "jass.message", "jass.hook"}) {
        const auto rejected = host.Execute(std::string("require('") + module + "')", "@unimplemented.lua");
        Check(rejected.code == LuaReloadResult::RuntimeError && rejected.message.find(module) != std::string::npos,
            "remaining modules explicit without stub behavior");
    }
    LuaHostLimits limits; limits.instructions = 5000;
    LuaHost bounded({}, limits); Success(bounded.Initialize(), "handler budget initialize");
    Success(bounded.Execute("runtime=require('jass.runtime'); runtime.error_handle=function() while true do end end", "@infinite-handler.lua"), "bounded handler configured");
    const auto budget = bounded.Execute("error('preserved')", "@budget-handler.lua");
    Check(budget.message.find("preserved") != std::string::npos && budget.message.find("jass.runtime.error_handle") != std::string::npos,
        "handler inherits execution budget and cannot hang host");
    Success(bounded.Execute("runtime.error_handle=nil; value=1", "@after-handler.lua"), "failed handler unwinds host");
}

void MailboxExecutesRealLua() {
    auto mailbox = std::make_unique<LuaTestMailbox>();
    const std::string nonce(32, '1'), sha(64, '2');
    std::memcpy(mailbox->sessionNonce, nonce.c_str(), nonce.size() + 1);
    std::memcpy(mailbox->mapSha256, sha.c_str(), sha.size() + 1);
    LuaMailboxRuntime receiver(*mailbox);
    LuaHost host({}); Success(host.Initialize(), "mailbox host initialize");
    Check(receiver.Attach(nonce, sha), "mailbox receiver attach");
    for (std::uint32_t seq = 1; seq <= 20; ++seq) {
        mailbox->state = static_cast<LONG>(LuaMailboxState::Writing);
        mailbox->requestSequence = seq;
        std::memcpy(mailbox->requestNonce, nonce.c_str(), nonce.size() + 1);
        std::memcpy(mailbox->requestMapSha256, sha.c_str(), sha.size() + 1);
        const std::string name = "@reload.lua";
        const std::string source = "counter=(counter or 0)+1; assert(counter==" + std::to_string(seq) + ")";
        mailbox->chunkNameBytes = static_cast<std::uint32_t>(name.size()); std::memcpy(mailbox->chunkName, name.data(), name.size());
        mailbox->sourceBytes = static_cast<std::uint32_t>(source.size()); std::memcpy(mailbox->source, source.data(), source.size());
        InterlockedExchange(reinterpret_cast<volatile LONG*>(&mailbox->state), static_cast<LONG>(LuaMailboxState::Ready));
        const auto request = receiver.Take(); Check(request.has_value(), "mailbox read");
        const auto executed = host.Execute(request->source, request->chunkName);
        Check(receiver.Complete(request->sequence, executed.code, executed.message), "mailbox result");
        Check(mailbox->resultSequence == seq && mailbox->resultCode == LuaReloadResult::Success, "actual Lua execution ack");
    }
    receiver.Stop();
}
}

void ModuleHotReload() {
    // 原创两模块：skill 返回函数表并保存一个计数状态；main 通过表字段间接调用，模拟已注册回调。
    const std::unordered_map<std::string, std::string> modules{
        {"skill.lua", "local M={hits=0}\nfunction M.damage() return 10 end\nreturn M"},
        {"main.lua", "local skill=require('skill')\ncallback=function() skill.hits=skill.hits+1; return skill.damage() end"},
        {"later.lua", "return {value=1}"},
    };
    LuaHost host([&](std::string_view member, std::size_t) {
        const auto found = modules.find(std::string(member));
        return found == modules.end() ? LuaModuleRead{} : LuaModuleRead{true, found->second, {}};
    });
    Success(host.Initialize(), "reload host initialize");
    Success(host.ExecuteModule("main"), "entry module");
    Success(host.Execute("assert(callback()==10 and callback()==10)", "@before.lua"), "original logic");
    Success(host.ReloadModule("skill", "local M={hits=0, added=5}\nfunction M.damage() return 25 end\nreturn M"), "reload loaded module");
    Success(host.Execute("local s=require('skill'); assert(callback()==25); assert(s.hits==3 and s.added==5)", "@after.lua"),
        "registered callback reaches new function while module state and identity are kept");
    Success(host.ReloadModule("later", "return {value=2}"), "reload not-yet-loaded module");
    Success(host.Execute("assert(require('later').value==2)", "@later-check.lua"), "later require uses reloaded text");
    const auto broken = host.ReloadModule("skill", "function M.damage( return 1 end");
    Check(broken.code == LuaReloadResult::SyntaxError || broken.code == LuaReloadResult::RuntimeError, "syntax error refused");
    Check(broken.message.find("skill.lua:1") != std::string::npos, "syntax error points at the module file and line");
    Success(host.Execute("assert(callback()==25)", "@unchanged.lua"), "failed reload keeps previous logic");
    Check(host.ReloadModule("../escape", "return 1").code == LuaReloadResult::InvalidRequest, "invalid module name refused");
    // YDWE |xxxx| 四字码在模块、代码段与热重载中都按整数编译；字符串内保持原文。
    Success(host.Execute("assert(|AHhb| == 1095264354 and '|AHhb|' == '|' .. 'AHhb|' and (6|1|0) == 7)", "@rawcode.lua"), "YDWE rawcode");
    Success(host.ReloadModule("skill", "local M={}\nfunction M.damage() return |hfoo| end\nreturn M"), "rawcode in reloaded module");
    Success(host.Execute("assert(callback()==1751543663)", "@rawcode-reload.lua"), "reloaded rawcode value");
    // 新函数通过 local M 读写状态时，热重载后仍作用于原模块表（游戏实测发现的计数停止问题）。
    Success(host.ReloadModule("later", "local M={n=0}\nfunction M.bump() M.n=M.n+1 return M.n end\nreturn M"), "stateful module");
    Success(host.Execute("local l=require('later'); assert(l.bump()==1 and l.bump()==2)", "@state-before.lua"), "state before reload");
    Success(host.ReloadModule("later", "local M={n=0}\nfunction M.bump() M.n=M.n+10 return M.n end\nreturn M"), "reload stateful module");
    Success(host.Execute("local l=require('later'); assert(l.bump()==12 and l.n==12)", "@state-after.lua"),
        "reloaded function updates the original table state");
    // 作者用斜杠或点号写同一子目录模块时共用一份结果，编辑器按文件路径（点号）热重载也能命中。
    LuaHost nested([](std::string_view member, std::size_t) {
        return member == "lib\\util.lua" ? LuaModuleRead{true, "local M={}\nfunction M.v() return 1 end\nreturn M", {}} : LuaModuleRead{};
    });
    Success(nested.Initialize(), "nested host initialize");
    Success(nested.Execute("u=require('lib/util'); assert(require('lib.util')==u)", "@nested.lua"), "same member shares result");
    Success(nested.ReloadModule("lib.util", "local M={}\nfunction M.v() return 2 end\nreturn M"), "reload by dotted name");
    Success(nested.Execute("assert(u.v()==2)", "@nested-check.lua"), "slash-required module receives reload");
}

int main() {
    try {
        TextOnlyAndSandbox(); ArchiveModules(); ClassicEntrySyntax(); BudgetsAndRandom(); RuntimeBehaviorModules(); MailboxExecutesRealLua();
        ModuleHotReload();
        std::cout << "Own Lua 5.3 host passed; original in-memory archive/native-free fixture; game_evidence=0\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
