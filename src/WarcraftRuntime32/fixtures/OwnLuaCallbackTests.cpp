#include "../OwnLua/LuaHost.h"
#include "../OwnLua/NativeProvider.h"

#include <windows.h>
#include <iostream>
#include <map>
#include <stdexcept>
#include <thread>

namespace {
using namespace Audastra::Runtime32;
using namespace Audastra::Runtime32::OwnLua;
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void Success(const LuaHostResult& result, const char* message) {
    if (!result.Succeeded()) throw std::runtime_error(std::string(message) + ": " + result.message);
}
// 原创调度器只验证闭包持有、释放和受保护调用，不模拟 Game.dll code ID 或声称游戏已接通。
class Scheduler final : public NativeProvider {
public:
    std::uint64_t generation = 3;
    std::map<std::int32_t, std::shared_ptr<NativeCallback>> callbacks;
    std::size_t calls = 0;
    std::uint64_t Generation() const noexcept override { return generation; }
    bool IsHandleType(std::string_view, std::string_view) const noexcept override { return false; }
    std::optional<NativeSignature> Lookup(std::string_view name) const override {
        if (name == "StoreAction" || name == "StoreCondition" || name == "Reject") return ParseNativeSignature("(IC)V");
        if (name == "Run") return ParseNativeSignature("(I)B");
        if (name == "Remove") return ParseNativeSignature("(I)V");
        if (name == "Sync") return ParseNativeSignature("(C)B");
        return {};
    }
    std::optional<NativeCallbackReturn> CallbackReturn(std::string_view name, std::size_t index) const override {
        if (index == 1 && (name == "StoreAction" || name == "Reject")) return NativeCallbackReturn::Nothing;
        if ((index == 1 && name == "StoreCondition") || (index == 0 && name == "Sync")) return NativeCallbackReturn::Boolean;
        return {};
    }
    NativeCallbackResult Run(std::int32_t key) {
        const auto found = callbacks.find(key);
        if (found == callbacks.end()) return {false, false, "fixture callback missing"};
        const auto callback = found->second;
        return callback->Invoke();
    }
    NativeCallResult Invoke(std::string_view name, std::span<const NativeValue> args) override {
        ++calls;
        if (name == "Sync") {
            if (std::holds_alternative<std::monostate>(args[0])) return {false, {}, "fixture synchronous callback is nil"};
            const auto result = std::get<std::shared_ptr<NativeCallback>>(args[0])->Invoke();
            return {result.succeeded, result.booleanValue, result.error};
        }
        const auto key = std::get<std::int32_t>(args[0]);
        if (name == "Run") {
            const auto result = Run(key);
            return {result.succeeded, result.booleanValue, result.error};
        }
        if (name == "Remove") { callbacks.erase(key); return {true, {}, {}}; }
        if (name == "Reject") return {false, {}, "fixture rejected registration"};
        if (std::holds_alternative<std::monostate>(args[1])) callbacks.erase(key);
        else callbacks[key] = std::get<std::shared_ptr<NativeCallback>>(args[1]);
        return {true, {}, {}};
    }
};

void LifetimesAndReentry() {
    const auto scheduler = std::make_shared<Scheduler>();
    std::shared_ptr<NativeCallback> afterClose;
    {
        LuaHost host({});
        Success(host.Initialize(), "initialize");
        Success(host.BindNatives(scheduler), "bind");
        Success(host.Execute(R"(
j=require('jass.common')
local count=0
j.StoreAction(1,function() count=count+1; observed=count end)
j.StoreCondition(2,function() return count==2 end)
assert(j.Sync(function() return not j.Run(2) end))
)", "@original-closures.lua"), "capture mutable locals");
        Check(host.CallbackCount() == 2, "synchronous temporary releases after native returns");
        Check(scheduler->Run(1).succeeded && scheduler->Run(1).succeeded, "async action runs twice");
        const auto condition = scheduler->Run(2);
        Check(condition.succeeded && condition.booleanValue, "async condition sees retained lexical scope");
        Success(host.Execute("assert(observed==2)", "@read-back.lua"), "async writes preserved");
        Success(host.Execute("j.StoreAction(1,function() j.Remove(1); after_remove=true end); j.Run(1); assert(after_remove)", "@self-remove.lua"), "self removal is safe");
        Check(host.CallbackCount() == 1 && !scheduler->callbacks.contains(1), "self removal releases closure");
        Success(host.Execute("j.StoreAction(1,function() j.StoreAction(1,function() replacement=true end) end); j.Run(1); j.Run(1); assert(replacement)", "@replace.lua"), "callback replacement keeps both lifetimes valid");
        Check(host.CallbackCount() == 2, "old closure released after replacement");
        const auto rejected = host.Execute("j.Reject(3,function() end)", "@registration-failure.lua");
        Check(!rejected.Succeeded() && host.CallbackCount() == 2, "failed registration releases registry reference");
        for (const auto invalid : {"j.StoreAction(3,123)", "j.StoreAction(3,{})", "j.StoreAction(3,'function')"}) {
            const auto before = scheduler->calls;
            Check(!host.Execute(invalid, "@invalid-callback.lua").Succeeded() && scheduler->calls == before,
                "invalid callback refused before provider");
        }
        Success(host.Execute("j.StoreAction(1,nil); j.Remove(2); j.StoreCondition(5,function() return 1 end)", "@nil-and-bool.lua"), "nil removes callback");
        Check(!scheduler->Run(5).succeeded, "conditions cannot return truthy non-booleans");
        Success(host.Execute("j.StoreAction(6,function() error('callback failure marker') end)", "@callback-error.lua"), "capture failing callback");
        const auto error = scheduler->Run(6);
        Check(!error.succeeded && error.error.find("callback-error.lua") != std::string::npos
            && error.error.find("stack traceback") != std::string::npos, "async errors preserve source and stack");
        Success(host.Execute("local n=0; local function nested() n=n+1; if n==4 then return true end; return j.Sync(nested) end; assert(j.Sync(nested)); assert(n==4)", "@nested.lua"), "bounded synchronous reentry");
        const auto recursion = host.Execute("local function loop() return j.Sync(loop) end; j.Sync(loop)", "@depth.lua");
        Check(!recursion.Succeeded() && recursion.message.find("32") != std::string::npos, "unbounded reentry rejected");
        Success(host.Execute("assert(j.Sync(function() return true end))", "@after-depth.lua"), "stack and execution state restore after nested errors");
        const auto offThread = scheduler->callbacks.at(6);
        bool refused = false;
        std::thread worker([&] { refused = !offThread->Invoke().succeeded; }); worker.join();
        Check(refused, "off-thread invoke refused");
        scheduler->callbacks.erase(5);
        auto releaseAway = scheduler->callbacks.at(6);
        scheduler->callbacks.erase(6);
        // 本地 offThread 仍持有6；另用7覆盖最后引用在工作线程释放的路径。
        Success(host.Execute("j.StoreAction(7,function() end)", "@release-away.lua"), "capture off-thread release");
        auto lastAway = std::move(scheduler->callbacks.at(7)); scheduler->callbacks.erase(7);
        const auto beforeDrain = host.CallbackCount();
        std::thread destroyer([owned = std::move(lastAway)]() mutable { owned.reset(); }); destroyer.join();
        Success(host.Execute("assert(true)", "@drain.lua"), "deferred release runs on owner");
        Check(host.CallbackCount() + 1 == beforeDrain, "off-thread destruction is eventually unreferenced");
        Success(host.Execute("j.StoreAction(8,function() after_exit=true end)", "@generation.lua"), "capture generation");
        afterClose = scheduler->callbacks.at(8);
        ++scheduler->generation;
        Check(!scheduler->Run(8).succeeded, "expired map generation refuses pure Lua callback");
        --scheduler->generation; // 单独区分下一项宿主关闭检查与上面的代际检查。
    }
    Check(!afterClose->Invoke().succeeded, "callback retained beyond host close fails safely");
    scheduler->callbacks.clear(); afterClose.reset();
}

void Budgets() {
    const auto scheduler = std::make_shared<Scheduler>();
    LuaHostLimits limits; limits.instructions = 20'000; limits.milliseconds = 1000;
    LuaHost host({}, limits); Success(host.Initialize(), "budget initialize"); Success(host.BindNatives(scheduler), "budget bind");
    Success(host.Execute("j=require('jass.common'); j.StoreAction(1,function() while true do end end)", "@capture-budget.lua"), "capture bounded callback");
    const auto async = scheduler->Run(1);
    Check(!async.succeeded && async.error.find("预算") != std::string::npos, "async callback has fresh bounded execution budget");
    const auto nested = host.Execute("local function busy() local x=0; for i=1,800 do x=x+i end; return true end; for i=1,100 do pcall(j.Sync,busy) end", "@shared-budget.lua");
    Check(nested.code == LuaReloadResult::BudgetExceeded, "nested callback cannot reset or swallow parent budget");
    Success(host.Execute("assert(j.Sync(function() return false end)==false)", "@budget-reset.lua"), "next independent execution gets fresh budget");
    scheduler->callbacks.clear();
    LuaHost many({}); Success(many.Initialize(), "many initialize"); Success(many.BindNatives(scheduler), "many bind");
    Success(many.Execute("j=require('jass.common'); for i=1,4096 do j.StoreAction(i,function() end) end", "@limit.lua"), "callback cap accepts maximum");
    Check(many.CallbackCount() == 4096, "all callback references counted");
    Check(!many.Execute("j.StoreAction(4097,function() end)", "@over-limit.lua").Succeeded()
        && scheduler->callbacks.size() == 4096, "over limit refuses before persistent provider registration");
    scheduler->callbacks.clear();
    Check(many.CallbackCount() == 0, "bulk release frees all registry references");
    Success(many.Execute("j.StoreAction(1,function() end)", "@after-release.lua"), "capacity reusable after release");
}
}

int main() {
    try { LifetimesAndReentry(); Budgets(); std::cout << "own Lua callback lifecycle tests passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
