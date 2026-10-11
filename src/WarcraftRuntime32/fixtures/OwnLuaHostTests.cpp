#include "../OwnLua/LuaHost.h"
#include "../OwnLua/LuaMailboxRuntime.h"
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
    Success(host.Execute("assert(io == nil and os == nil and package == nil and debug == nil and coroutine == nil); assert(string.dump == nil)", "@sandbox.lua"), "no external capabilities");
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

int main() {
    try {
        TextOnlyAndSandbox(); ArchiveModules(); BudgetsAndRandom(); MailboxExecutesRealLua();
        std::cout << "Own Lua 5.3 host passed; original in-memory archive/native-free fixture; game_evidence=0\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
