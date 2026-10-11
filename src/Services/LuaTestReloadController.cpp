#include "War3MapCore/Services/LuaTestReloadController.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/MapArchiveSession.h"

#include <algorithm>
#include <charconv>
#include <map>
#include <mutex>
#include <set>

namespace War3Map::Services {
struct LuaTestReloadController::State {
    std::mutex mutex;
    std::shared_ptr<LuaTestMailboxSession> mailbox;
    Script::LuaTriggerUnit baseline;
    std::optional<std::set<std::string>> installedBindings;
    bool bindingsValid = true;
    std::string binding;
    std::optional<Script::LuaTriggerUnit> pending;
    Script::LuaTestReloadPatch patch;
    LuaReloadControllerSnapshot snapshot;
    std::chrono::steady_clock::time_point started;
};
LuaTestReloadController::LuaTestReloadController(std::shared_ptr<LuaTestMailboxSession> mailbox,
    Script::LuaTriggerUnit baseline, std::string binding,
    std::optional<std::vector<std::string>> installedBindings) : state_(std::make_unique<State>()) {
    state_->mailbox = std::move(mailbox); state_->baseline = std::move(baseline); state_->binding = std::move(binding);
    if (installedBindings) {
        auto& installed = state_->installedBindings.emplace();
        // 与完整Lua生成器的API4096、常量4096、全局65536上限一致，不缩小大图资格。
        state_->bindingsValid = installedBindings->size() <= 4096 + 4096 + 65536;
        if (state_->bindingsValid) for (const auto& name : *installedBindings) {
            if (name.empty() || name.size() > 256) { state_->bindingsValid = false; break; }
            installed.insert(name);
        }
    }
    state_->snapshot.baselineScriptSha256 = state_->baseline.scriptSha256;
}
LuaTestReloadController::~LuaTestReloadController() = default;

Result<LuaReloadControllerSnapshot> LuaTestReloadController::Submit(Script::LuaTriggerUnit current,
    std::string_view binding, const Lua::SyntaxOptions& syntax) {
    std::lock_guard guard(state_->mutex);
    auto& state = *state_;
    if (!state.mailbox) return Result<LuaReloadControllerSnapshot>::Failure("Lua.Test", "LUA_RELOAD_NO_SESSION", "没有本编辑器启动的 Lua 测试会话。");
    if (state.pending) return Result<LuaReloadControllerSnapshot>::Failure("Lua.Test", "LUA_RELOAD_PENDING", "上一份差异仍在等待游戏结果，请先查询结果。");
    if (state.snapshot.outcome == LuaReloadOutcome::RestartRequired || state.snapshot.outcome == LuaReloadOutcome::Stopped)
        return Result<LuaReloadControllerSnapshot>::Failure("Lua.Test", "LUA_RELOAD_RESTART_REQUIRED", "本局状态不能可靠继续，请重新测试。");
    auto prepared = Script::PrepareLuaTestReload(state.baseline, current, state.binding, binding, syntax);
    if (!prepared.Succeeded() || !prepared.value) return {std::nullopt, std::move(prepared.diagnostics)};
    if (!state.bindingsValid)
        return Result<LuaReloadControllerSnapshot>::Failure("Lua.Test", "LUA_RELOAD_BINDINGS_INVALID",
            "本局安装的符号白名单不完整或超出预算；差异没有发送，需要重新测试。");
    if (state.installedBindings) {
        for (const auto* names : {&current.requiredApis, &current.requiredGlobals, &current.requiredConstants})
            for (const auto& name : *names) if (!state.installedBindings->contains(name))
                return Result<LuaReloadControllerSnapshot>::Failure("Lua.Test", "LUA_RELOAD_BINDING_NOT_INSTALLED",
                    "本局尚未安装符号 " + name + "；差异没有发送，需要重新测试以安装新依赖。");
    }
    if (prepared.value->source.empty()) {
        state.baseline = std::move(current);
        state.snapshot = {LuaReloadOutcome::NoChanges, 0, "没有需要执行的触发器差异。", state.baseline.scriptSha256};
        return Result<LuaReloadControllerSnapshot>::Success(state.snapshot);
    }
    auto queued = SubmitLuaTestReload(*state.mailbox, {"@Audastra/test-reload.lua", prepared.value->source});
    if (!queued.Succeeded() || !queued.value) return {std::nullopt, std::move(queued.diagnostics)};
    state.patch = std::move(*prepared.value); state.pending = std::move(current);
    state.started = std::chrono::steady_clock::now();
    state.snapshot = {LuaReloadOutcome::Pending, *queued.value, "差异已发送，正在等待游戏确认。", state.baseline.scriptSha256};
    return Result<LuaReloadControllerSnapshot>::Success(state.snapshot);
}

Result<LuaReloadControllerSnapshot> LuaTestReloadController::Poll() {
    std::lock_guard guard(state_->mutex);
    auto& state = *state_;
    if (!state.mailbox) return Result<LuaReloadControllerSnapshot>::Failure("Lua.Test", "LUA_RELOAD_NO_SESSION", "没有本编辑器启动的 Lua 测试会话。");
    const auto queried = QueryLuaTestReload(*state.mailbox, state.pending ? state.snapshot.sequence : 0);
    if (!queried.Succeeded() || !queried.value) return {std::nullopt, queried.diagnostics};
    const auto& result = *queried.value;
    state.snapshot.runtimeDiagnosticSequence = result.runtimeDiagnosticSequence;
    state.snapshot.runtimeDiagnosticCode = result.runtimeDiagnosticCode;
    state.snapshot.runtimeMessage = result.runtimeMessage;
    state.snapshot.runtimeDiagnosticTruncated = result.runtimeDiagnosticTruncated;
    if (result.state == LuaTestReloadState::Starting) {
        state.snapshot.message = "测试运行时正在初始化，尚未允许热重载。";
    } else if (!result.processActive || result.state == LuaTestReloadState::Stopped) {
        state.snapshot.outcome = LuaReloadOutcome::Stopped;
        state.snapshot.message = "本次测试游戏已退出。";
        state.pending.reset();
    } else if (state.pending) {
        state.snapshot.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - state.started).count();
        if (result.state == LuaTestReloadState::Complete) {
            if (result.sequence != state.snapshot.sequence)
                return Result<LuaReloadControllerSnapshot>::Failure("Lua.Test", "LUA_RELOAD_ACK_MISMATCH", "游戏回执序号不属于本次差异，基线没有前推。");
            state.snapshot.message = result.message;
            if (result.resultCode == Audastra::Runtime32::LuaReloadResult::Success) {
                state.baseline = std::move(*state.pending);
                state.snapshot.baselineScriptSha256 = state.baseline.scriptSha256;
                state.snapshot.outcome = LuaReloadOutcome::Succeeded;
            } else {
                // 编译错误未执行；其他错误可能已改变全局函数或销毁旧句柄，不能假报回滚。
                state.snapshot.outcome = result.resultCode == Audastra::Runtime32::LuaReloadResult::SyntaxError
                    || result.resultCode == Audastra::Runtime32::LuaReloadResult::Cancelled
                    ? LuaReloadOutcome::Failed : LuaReloadOutcome::RestartRequired;
                const std::string_view marker = "test-reload.lua:";
                const auto at = result.message.find(marker);
                if (at != std::string::npos) {
                    std::size_t line = 0;
                    const auto begin = result.message.data() + at + marker.size();
                    const auto parsed = std::from_chars(begin, result.message.data() + result.message.size(), line);
                    if (parsed.ec == std::errc{}) state.snapshot.location = Script::LocateLuaTestReload(state.patch, line);
                }
                if (state.snapshot.outcome == LuaReloadOutcome::RestartRequired)
                    state.snapshot.message += "\n差异可能已部分执行，原基线保留，需要重新测试。";
            }
            state.pending.reset(); state.patch = {};
        } else if (state.snapshot.seconds > 5) {
            // 只提示超时，仍等同一序号ACK；不假称已取消正在运行的Lua。
            state.snapshot.message = "游戏暂未确认差异；基线没有前推，请继续查询或重新测试。";
        }
    }
    return Result<LuaReloadControllerSnapshot>::Success(state.snapshot);
}

Result<std::string> CaptureLuaTestMapBinding(const MapArchiveSession& archive,
    std::span<const MemberReplacement> replacements, std::span<const std::string> removals,
    std::span<const std::string> scripts) {
    const auto canonical = [](std::string_view name) {
        std::string result(name);
        for (auto& c : result) { if (c == '\\') c = '/'; if (c >= 'A' && c <= 'Z') c += 'a' - 'A'; }
        return result;
    };
    std::set<std::string> excluded{"war3map.wtg", "war3map.wct", "(listfile)", "(attributes)", "(signature)"};
    for (const auto& script : scripts) excluded.insert(canonical(script));
    std::map<std::string, std::span<const std::byte>> members;
    for (const auto& member : archive.Members()) members.emplace(canonical(member.name), member.data);
    for (const auto& removed : removals) members.erase(canonical(removed));
    for (const auto& member : replacements) members[canonical(member.name)] = member.data;
    if (members.size() > 65536) return Result<std::string>::Failure("Lua.Test", "LUA_RELOAD_MAP_BUDGET", "地图事实超过65536个成员，需重新测试。");
    std::string identity = "Audastra Lua reload map facts v1\n";
    if (archive.HasOpaqueMembers()) identity += "opaque:" + archive.SourceSha256() + "\n";
    std::uint64_t bytes = 0;
    for (const auto& [name, value] : members) {
        if (excluded.contains(name)) continue;
        bytes += value.size();
        if (bytes > 2ull * 1024 * 1024 * 1024)
            return Result<std::string>::Failure("Lua.Test", "LUA_RELOAD_MAP_BUDGET", "地图事实超过2GiB散列预算，需重新测试。");
        const auto hash = Sha256(value);
        if (!hash.Succeeded() || !hash.value) return {std::nullopt, hash.diagnostics};
        identity += std::to_string(name.size()) + ":" + name + ":" + *hash.value + "\n";
    }
    return Sha256Text(identity);
}
} // namespace War3Map::Services
