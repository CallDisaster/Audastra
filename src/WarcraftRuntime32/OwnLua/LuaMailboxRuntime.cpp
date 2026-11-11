#include "LuaMailboxRuntime.h"
#include "War3MapCore/Runtime/LuaTestMailboxValidation.h"
#include <windows.h>
#include <algorithm>
#include <cstring>
#include <limits>
#include <new>

namespace Audastra::Runtime32::OwnLua {
namespace {
LONG Read(const std::int32_t& value) noexcept {
    return InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(const_cast<std::int32_t*>(&value)), 0, 0);
}
void Store(std::int32_t& value, std::int32_t next) noexcept {
    InterlockedExchange(reinterpret_cast<volatile LONG*>(&value), next);
}
void StoreDiagnostic(void* value, std::uint32_t next) noexcept {
    InterlockedExchange(static_cast<volatile LONG*>(value), static_cast<LONG>(next));
}
}

bool LuaMailboxRuntime::Attach(std::string_view nonce, std::string_view sha, std::int32_t generation) noexcept {
    if (threadId_ || !LuaMailboxHeader(mailbox_) || !LuaMailboxHex(nonce, 32) || !LuaMailboxHex(sha, 64)
        || nonce != std::string_view(mailbox_.sessionNonce, 32) || sha != std::string_view(mailbox_.mapSha256, 64)
        || generation <= 0 || Read(mailbox_.generation) != generation - 1
        || Read(mailbox_.runtimeState) != static_cast<LONG>(LuaRuntimeState::Starting)) return false;
    const auto state = Read(mailbox_.state);
    if (generation == 1) {
        if (state != static_cast<LONG>(LuaMailboxState::Empty) || mailbox_.requestSequence || mailbox_.resultSequence
            || mailbox_.runtimeDiagnosticRevision || mailbox_.runtimeDiagnosticSequence || mailbox_.requestGeneration)
            return false;
    } else {
        if (mailbox_.gameProcessId != GetCurrentProcessId() || mailbox_.mainThreadId != GetCurrentThreadId()
            || state < static_cast<LONG>(LuaMailboxState::Empty) || state > static_cast<LONG>(LuaMailboxState::Stopped)
            || state == static_cast<LONG>(LuaMailboxState::Writing) || state == static_cast<LONG>(LuaMailboxState::Running)
            || InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(&mailbox_.state),
                static_cast<LONG>(LuaMailboxState::Resetting), state) != state) return false;
        mailbox_.requestSequence = mailbox_.resultSequence = 0;
        mailbox_.requestGeneration = 0;
        mailbox_.chunkNameBytes = mailbox_.sourceBytes = mailbox_.resultBytes = 0;
        mailbox_.resultTruncated = 0;
        mailbox_.resultCode = LuaReloadResult::NotReady;
        // 诊断槽是进程日志，保留单调序号；它不是可执行请求或 ACK。
        diagnosticSequence_ = mailbox_.runtimeDiagnosticSequence;
    }
    std::memcpy(nonce_, nonce.data(), nonce.size());
    std::memcpy(mapSha_, sha.data(), sha.size());
    threadId_ = GetCurrentThreadId();
    generation_ = generation;
    mailbox_.gameProcessId = GetCurrentProcessId();
    mailbox_.mainThreadId = threadId_;
    Store(mailbox_.generation, generation_);
    Store(mailbox_.state, static_cast<LONG>(LuaMailboxState::Empty));
    Store(mailbox_.runtimeState, static_cast<LONG>(LuaRuntimeState::Ready));
    return true;
}

bool LuaMailboxRuntime::MainThread() const noexcept {
    return !retired_ && threadId_ && threadId_ == GetCurrentThreadId();
}
bool LuaMailboxRuntime::Identity() const noexcept {
    return generation_ > 0 && Read(mailbox_.generation) == generation_ && LuaMailboxHeader(mailbox_)
        && std::memcmp(mailbox_.sessionNonce, nonce_, sizeof(nonce_)) == 0
        && std::memcmp(mailbox_.mapSha256, mapSha_, sizeof(mapSha_)) == 0;
}

std::optional<LuaReloadChunk> LuaMailboxRuntime::Take() {
    if (!MainThread() || Read(mailbox_.generation) != generation_
        || Read(mailbox_.runtimeState) != static_cast<LONG>(LuaRuntimeState::Ready)) return {};
    if (InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(&mailbox_.state),
        static_cast<LONG>(LuaMailboxState::Running), static_cast<LONG>(LuaMailboxState::Ready))
        != static_cast<LONG>(LuaMailboxState::Ready)) return {};
    const auto sequence = mailbox_.requestSequence;
    if (!Identity() || mailbox_.requestGeneration != generation_
        || std::memcmp(mailbox_.requestNonce, nonce_, sizeof(nonce_)) != 0
        || std::memcmp(mailbox_.requestMapSha256, mapSha_, sizeof(mapSha_)) != 0) {
        (void)Complete(sequence, LuaReloadResult::IdentityMismatch, "热重载会话或地图身份不符。");
        return {};
    }
    if (!sequence || sequence <= lastSequence_ || !mailbox_.sourceBytes
        || mailbox_.sourceBytes > kLuaReloadSourceCapacity || !mailbox_.chunkNameBytes
        || mailbox_.chunkNameBytes >= kLuaReloadChunkCapacity) {
        (void)Complete(sequence, LuaReloadResult::InvalidRequest, "热重载序号或正文长度无效。");
        return {};
    }
    lastSequence_ = sequence;
    const std::string_view source(mailbox_.source, mailbox_.sourceBytes);
    const std::string_view name(mailbox_.chunkName, mailbox_.chunkNameBytes);
    if (!LuaMailboxUtf8(source) || !LuaMailboxChunkName(name)) {
        (void)Complete(sequence, LuaReloadResult::InvalidRequest, "热重载代码或来源名称不是有效的 UTF-8 文本。");
        return {};
    }
    try { return LuaReloadChunk{sequence, std::string(name), std::string(source)}; }
    catch (const std::bad_alloc&) {
        (void)Complete(sequence, LuaReloadResult::BudgetExceeded, "无法分配热重载代码快照。");
        return {};
    }
}

bool LuaMailboxRuntime::Complete(std::uint32_t sequence, LuaReloadResult result, std::string_view message) noexcept {
    if (!MainThread() || Read(mailbox_.generation) != generation_
        || Read(mailbox_.state) != static_cast<LONG>(LuaMailboxState::Running)
        || mailbox_.requestSequence != sequence) return false;
    if (!LuaMailboxUtf8(message)) {
        message = "运行时返回了无效 UTF-8 诊断。";
        result = LuaReloadResult::HostFailure;
    }
    const auto bytes = LuaMailboxUtf8Prefix(message, kLuaReloadResultCapacity - 1);
    if (bytes) std::memcpy(mailbox_.result, message.data(), bytes);
    mailbox_.result[bytes] = 0;
    mailbox_.resultBytes = static_cast<std::uint32_t>(bytes);
    mailbox_.resultTruncated = bytes != message.size();
    mailbox_.resultCode = result;
    mailbox_.resultSequence = sequence;
    Store(mailbox_.state, static_cast<LONG>(LuaMailboxState::Complete));
    return true;
}

bool LuaMailboxRuntime::Report(LuaReloadResult result, std::string_view message) noexcept {
    if (!MainThread() || !Identity()
        || Read(mailbox_.runtimeState) == static_cast<LONG>(LuaRuntimeState::Stopped)
        || diagnosticSequence_ == std::numeric_limits<std::uint32_t>::max() / 2) return false;
    if (!LuaMailboxUtf8(message)
        || static_cast<std::uint32_t>(result) > static_cast<std::uint32_t>(LuaReloadResult::HostFailure)) {
        message = "运行时返回了无效诊断。";
        result = LuaReloadResult::HostFailure;
    }
    const auto bytes = LuaMailboxUtf8Prefix(message, kLuaReloadResultCapacity - 1);
    const auto sequence = ++diagnosticSequence_;
    StoreDiagnostic(&mailbox_.runtimeDiagnosticRevision, sequence * 2 - 1);
    StoreDiagnostic(&mailbox_.runtimeDiagnosticSequence, sequence);
    StoreDiagnostic(&mailbox_.runtimeDiagnosticCode, static_cast<std::uint32_t>(result));
    StoreDiagnostic(&mailbox_.runtimeDiagnosticBytes, static_cast<std::uint32_t>(bytes));
    StoreDiagnostic(&mailbox_.runtimeDiagnosticTruncated, bytes != message.size());
    // 原子字正文让同进程 fixture 也没有与读取方的普通内存数据竞争。
    for (std::size_t offset = 0; offset <= bytes; offset += sizeof(std::uint32_t)) {
        std::uint32_t word = 0;
        const auto count = (std::min)(sizeof(word), bytes - offset);
        if (count) std::memcpy(&word, message.data() + offset, count);
        StoreDiagnostic(mailbox_.runtimeDiagnosticText + offset, word);
    }
    StoreDiagnostic(&mailbox_.runtimeDiagnosticRevision, sequence * 2);
    return true;
}

void LuaMailboxRuntime::Stop() noexcept {
    if (!MainThread() || !Identity()) return;
    Store(mailbox_.runtimeState, static_cast<LONG>(LuaRuntimeState::Stopped));
    // Running 的已发生副作用不能伪报取消；保留其最后状态供编辑器判断未知结果。
    if (Read(mailbox_.state) != static_cast<LONG>(LuaMailboxState::Running))
        Store(mailbox_.state, static_cast<LONG>(LuaMailboxState::Stopped));
}

bool LuaMailboxRuntime::RetireForRestart() noexcept {
    if (!MainThread() || !Identity()
        || Read(mailbox_.runtimeState) != static_cast<LONG>(LuaRuntimeState::Ready)
        || Read(mailbox_.state) == static_cast<LONG>(LuaMailboxState::Running)) return false;
    // 先撤下可提交状态；正在由宿主写入的槽由其发布，后继 VM 只在写完后取得重置权。
    Store(mailbox_.runtimeState, static_cast<LONG>(LuaRuntimeState::Starting));
    retired_ = true;
    return true;
}

} // namespace Audastra::Runtime32::OwnLua
