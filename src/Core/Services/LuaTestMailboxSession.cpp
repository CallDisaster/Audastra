#include "War3MapCore/Services/LuaTestMailboxSession.h"
#include "War3MapCore/Runtime/LuaTestMailboxValidation.h"
#include "LuaTestMailboxSessionInternal.h"

#include <array>
#include <algorithm>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>

#include <windows.h>
#include <bcrypt.h>

namespace War3Map::Services {
using namespace Audastra::Runtime32;
namespace {
LONG Read(const std::int32_t& value) noexcept {
    return InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(const_cast<std::int32_t*>(&value)), 0, 0);
}
void Store(std::int32_t& value, LuaMailboxState state) noexcept {
    InterlockedExchange(reinterpret_cast<volatile LONG*>(&value), static_cast<LONG>(state));
}
bool Change(std::int32_t& value, LuaMailboxState from, LuaMailboxState to) noexcept {
    return InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(&value),
        static_cast<LONG>(to), static_cast<LONG>(from)) == static_cast<LONG>(from);
}
template<class T> Result<T> Failure(const char* code, const char* message) {
    return Result<T>::Failure("Lua.Test", code, message);
}
std::uint32_t ReadDiagnostic(const void* value) noexcept {
    return static_cast<std::uint32_t>(InterlockedCompareExchange(
        static_cast<volatile LONG*>(const_cast<void*>(value)), 0, 0));
}

struct RuntimeDiagnostic {
    std::uint32_t sequence = 0;
    LuaReloadResult code = LuaReloadResult::NotReady;
    std::string message;
    bool truncated = false;
};

bool ReadRuntimeDiagnostic(const LuaTestMailbox& mailbox, RuntimeDiagnostic& saved) {
    // 有界重试；游戏连续报错时返回上次稳定快照，下次轮询再接收，不能卡住界面。
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        const auto revision = ReadDiagnostic(&mailbox.runtimeDiagnosticRevision);
        if (revision & 1) continue;
        if (!revision) return saved.sequence == 0;
        const auto sequence = ReadDiagnostic(&mailbox.runtimeDiagnosticSequence);
        const auto code = ReadDiagnostic(&mailbox.runtimeDiagnosticCode);
        const auto bytes = ReadDiagnostic(&mailbox.runtimeDiagnosticBytes);
        const auto truncated = ReadDiagnostic(&mailbox.runtimeDiagnosticTruncated);
        std::array<char, kLuaReloadResultCapacity> text{};
        if (bytes < text.size()) {
            for (std::size_t offset = 0; offset <= bytes; offset += sizeof(std::uint32_t)) {
                const auto word = ReadDiagnostic(mailbox.runtimeDiagnosticText + offset);
                std::memcpy(text.data() + offset, &word, sizeof(word));
            }
        }
        if (ReadDiagnostic(&mailbox.runtimeDiagnosticRevision) != revision) continue;
        if (sequence != revision / 2 || sequence < saved.sequence || bytes >= text.size()
            || truncated > 1 || code > static_cast<std::uint32_t>(LuaReloadResult::HostFailure)
            || text[bytes] || !LuaMailboxUtf8({text.data(), bytes})) return false;
        saved = {sequence, static_cast<LuaReloadResult>(code), std::string(text.data(), bytes), truncated != 0};
        return true;
    }
    return true;
}
}

struct LuaTestMailboxSession::Impl {
    HANDLE mapping = nullptr;
    LuaTestMailbox* mailbox = nullptr;
    std::string nonce, sha;
    std::mutex mutex;
    std::uint32_t lastSequence = 0;
    RuntimeDiagnostic diagnostic;
    detail::LuaTestProcessVerifier verify;
    ~Impl() {
        // 不修改仍由游戏执行的槽；关闭编辑器不会终止游戏，也不会伪报正在执行的结果。
        if (mailbox) UnmapViewOfFile(mailbox);
        if (mapping) CloseHandle(mapping);
    }
    bool Identity() const noexcept {
        return mailbox && LuaMailboxHeader(*mailbox)
            && std::string_view(mailbox->sessionNonce, 32) == nonce
            && std::string_view(mailbox->mapSha256, 64) == sha;
    }
    detail::LuaTestProcessOwnership Ownership() const noexcept {
        if (!verify || !mailbox->gameProcessId || !mailbox->mainThreadId)
            return detail::LuaTestProcessOwnership::Unconfirmed;
        try { return verify(mailbox->gameProcessId); }
        catch (...) { return detail::LuaTestProcessOwnership::Unconfirmed; }
    }
};

LuaTestMailboxSession::LuaTestMailboxSession(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
LuaTestMailboxSession::~LuaTestMailboxSession() = default;
std::string_view LuaTestMailboxSession::MapSha256() const noexcept { return impl_->sha; }

Result<std::shared_ptr<LuaTestMailboxSession>> CreateLuaTestMailboxSession(std::string_view sha) {
    using Session = std::shared_ptr<LuaTestMailboxSession>;
    if (!LuaMailboxHex(sha, 64)) return Failure<Session>("LUA_TEST_MAP_IDENTITY", "Lua 测试地图身份无效。");
    auto impl = std::make_unique<LuaTestMailboxSession::Impl>();
    impl->sha = sha;
    std::array<unsigned char, 16> nonce{};
    if (BCryptGenRandom(nullptr, nonce.data(), static_cast<ULONG>(nonce.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
        return Failure<Session>("LUA_TEST_NONCE", "无法生成 Lua 测试会话身份。");
    constexpr char hex[] = "0123456789abcdef";
    for (const auto byte : nonce) { impl->nonce += hex[byte >> 4]; impl->nonce += hex[byte & 15]; }
    impl->mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(LuaTestMailbox), nullptr);
    if (!impl->mapping) return Failure<Session>("LUA_TEST_MAPPING", "无法建立 Lua 测试邮箱。");
    impl->mailbox = static_cast<LuaTestMailbox*>(MapViewOfFile(impl->mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(LuaTestMailbox)));
    if (!impl->mailbox) return Failure<Session>("LUA_TEST_MAPPING", "无法映射 Lua 测试邮箱。");
    ::new (impl->mailbox) LuaTestMailbox{};
    std::memcpy(impl->mailbox->sessionNonce, impl->nonce.data(), impl->nonce.size());
    std::memcpy(impl->mailbox->mapSha256, impl->sha.data(), impl->sha.size());
    return Result<Session>::Success(Session(new LuaTestMailboxSession(std::move(impl))));
}

struct LuaTestMailboxOperations {
static Result<std::uint32_t> Submit(LuaTestMailboxSession& session, const LuaTestReloadRequest& request) {
    if (!LuaMailboxChunkName(request.chunkName) || request.source.empty()
        || request.source.size() > kLuaReloadSourceCapacity || !LuaMailboxUtf8(request.source))
        return Failure<std::uint32_t>("LUA_TEST_REQUEST", "热重载代码或名称不是有效的有界 UTF-8 文本。");
    auto& impl = *session.impl_;
    std::lock_guard lock(impl.mutex);
    auto& mailbox = *impl.mailbox;
    if (!impl.Identity()) return Failure<std::uint32_t>("LUA_TEST_IDENTITY", "Lua 测试会话或地图身份已变化。");
    if (Read(mailbox.runtimeState) != static_cast<LONG>(LuaRuntimeState::Ready)
        || impl.Ownership() != detail::LuaTestProcessOwnership::OwnedRunning)
        return Failure<std::uint32_t>("LUA_TEST_NOT_READY", "本次测试运行时或原进程归属尚未确认，不能发送代码。");
    if (impl.lastSequence == std::numeric_limits<std::uint32_t>::max())
        return Failure<std::uint32_t>("LUA_TEST_SEQUENCE_EXHAUSTED", "热重载序号已用尽，请重新测试地图。");
    auto from = static_cast<LuaMailboxState>(Read(mailbox.state));
    if ((from != LuaMailboxState::Empty && from != LuaMailboxState::Complete)
        || !Change(mailbox.state, from, LuaMailboxState::Writing))
        return Failure<std::uint32_t>("LUA_TEST_BUSY", "上一份热重载仍待处理，不能覆盖其代码或结果。");
    const auto sequence = ++impl.lastSequence;
    mailbox.requestSequence = sequence;
    std::memcpy(mailbox.requestNonce, impl.nonce.c_str(), impl.nonce.size() + 1);
    std::memcpy(mailbox.requestMapSha256, impl.sha.c_str(), impl.sha.size() + 1);
    mailbox.chunkNameBytes = static_cast<std::uint32_t>(request.chunkName.size());
    std::memcpy(mailbox.chunkName, request.chunkName.data(), request.chunkName.size());
    mailbox.chunkName[request.chunkName.size()] = 0;
    mailbox.sourceBytes = static_cast<std::uint32_t>(request.source.size());
    std::memcpy(mailbox.source, request.source.data(), request.source.size());
    mailbox.resultSequence = 0;
    mailbox.resultBytes = 0;
    mailbox.resultTruncated = 0;
    mailbox.resultCode = LuaReloadResult::NotReady;
    Store(mailbox.state, LuaMailboxState::Ready);
    return Result<std::uint32_t>::Success(sequence);
}

static Result<LuaTestReloadSnapshot> Query(LuaTestMailboxSession& session, std::uint32_t sequence) {
    auto& impl = *session.impl_;
    std::lock_guard lock(impl.mutex);
    auto& mailbox = *impl.mailbox;
    if (!impl.Identity()) return Failure<LuaTestReloadSnapshot>("LUA_TEST_IDENTITY", "Lua 测试会话或地图身份已变化。");
    if (sequence && sequence != impl.lastSequence)
        return Failure<LuaTestReloadSnapshot>("LUA_TEST_SEQUENCE", "这份热重载请求已过期，不能使用其他序号的结果。");
    LuaTestReloadSnapshot snapshot;
    snapshot.sequence = impl.lastSequence;
    const auto runtime = Read(mailbox.runtimeState);
    if (runtime < static_cast<LONG>(LuaRuntimeState::Starting) || runtime > static_cast<LONG>(LuaRuntimeState::Stopped))
        return Failure<LuaTestReloadSnapshot>("LUA_TEST_RUNTIME_STATE", "Lua 测试运行时状态无效。");
    snapshot.runtimeState = static_cast<LuaRuntimeState>(runtime);
    if (snapshot.runtimeState == LuaRuntimeState::Starting) return Result<LuaTestReloadSnapshot>::Success(snapshot);
    const auto ownership = impl.Ownership();
    if (ownership == detail::LuaTestProcessOwnership::Unconfirmed)
        return Failure<LuaTestReloadSnapshot>("LUA_TEST_PROCESS_UNCONFIRMED", "Lua 运行时尚未与本次启动的原进程身份对应。");
    snapshot.processActive = ownership == detail::LuaTestProcessOwnership::OwnedRunning;
    if (!ReadRuntimeDiagnostic(mailbox, impl.diagnostic))
        return Failure<LuaTestReloadSnapshot>("LUA_TEST_RUNTIME_DIAGNOSTIC", "Lua 运行时诊断的序号或正文无效。");
    snapshot.runtimeDiagnosticSequence = impl.diagnostic.sequence;
    snapshot.runtimeDiagnosticCode = impl.diagnostic.code;
    snapshot.runtimeMessage = impl.diagnostic.message;
    snapshot.runtimeDiagnosticTruncated = impl.diagnostic.truncated;
    const auto state = static_cast<LuaMailboxState>(Read(mailbox.state));
    if (state == LuaMailboxState::Complete) {
        // Complete 由运行时 release 发布，编辑器持本地互斥后不会被另一 Submit 覆盖。
        if (!impl.lastSequence || mailbox.requestSequence != impl.lastSequence || mailbox.resultSequence != impl.lastSequence
            || std::memcmp(mailbox.requestNonce, impl.nonce.c_str(), impl.nonce.size() + 1)
            || std::memcmp(mailbox.requestMapSha256, impl.sha.c_str(), impl.sha.size() + 1)
            || mailbox.resultBytes >= kLuaReloadResultCapacity || mailbox.resultTruncated > 1
            || static_cast<std::uint32_t>(mailbox.resultCode) > static_cast<std::uint32_t>(LuaReloadResult::HostFailure)
            || !LuaMailboxUtf8({mailbox.result, mailbox.resultBytes}))
            return Failure<LuaTestReloadSnapshot>("LUA_TEST_ACK", "热重载回执的身份、序号或正文无效，结果不能确认。");
        snapshot.state = LuaTestReloadState::Complete;
        snapshot.resultCode = mailbox.resultCode;
        snapshot.message.assign(mailbox.result, mailbox.resultBytes);
        snapshot.resultTruncated = mailbox.resultTruncated != 0;
    } else if (state == LuaMailboxState::Running) {
        // 即使进程刚退出也保留 Running，不能把已执行但未 ACK 的请求描述成成功或取消。
        snapshot.state = LuaTestReloadState::Running;
    } else if (state == LuaMailboxState::Stopped || !snapshot.processActive
        || snapshot.runtimeState == LuaRuntimeState::Failed || snapshot.runtimeState == LuaRuntimeState::Stopped) {
        snapshot.state = LuaTestReloadState::Stopped;
    } else if (state == LuaMailboxState::Empty) snapshot.state = LuaTestReloadState::Idle;
    else if (state == LuaMailboxState::Ready) snapshot.state = LuaTestReloadState::Queued;
    else return Failure<LuaTestReloadSnapshot>("LUA_TEST_MAILBOX_STATE", "Lua 测试邮箱状态无效。");
    return Result<LuaTestReloadSnapshot>::Success(std::move(snapshot));
}

static Status Cancel(LuaTestMailboxSession& session, std::uint32_t sequence) {
    auto& impl = *session.impl_;
    std::lock_guard lock(impl.mutex);
    auto& mailbox = *impl.mailbox;
    if (!sequence || sequence != impl.lastSequence || !impl.Identity())
        return Status::Failure("Lua.Test", "LUA_TEST_SEQUENCE", "热重载身份或序号不符，取消请求已拒绝。");
    if (!Change(mailbox.state, LuaMailboxState::Ready, LuaMailboxState::Writing))
        return Status::Failure("Lua.Test", "LUA_TEST_CANCEL_TOO_LATE", "请求已开始执行或已经结束，不能假报取消。");
    mailbox.resultCode = LuaReloadResult::Cancelled;
    mailbox.resultSequence = sequence;
    mailbox.resultBytes = 0;
    mailbox.resultTruncated = 0;
    Store(mailbox.state, LuaMailboxState::Complete);
    return Status::Ok();
}
};

Result<std::uint32_t> SubmitLuaTestReload(LuaTestMailboxSession& session, const LuaTestReloadRequest& request) {
    return LuaTestMailboxOperations::Submit(session, request);
}
Result<LuaTestReloadSnapshot> QueryLuaTestReload(LuaTestMailboxSession& session, std::uint32_t sequence) {
    return LuaTestMailboxOperations::Query(session, sequence);
}
Status CancelLuaTestReload(LuaTestMailboxSession& session, std::uint32_t sequence) {
    return LuaTestMailboxOperations::Cancel(session, sequence);
}

namespace detail {
struct LuaTestMailboxLaunchHandle::Impl {
    HANDLE handle = nullptr;
    std::shared_ptr<LuaTestMailboxSession> session;
    std::string nonce;
    ~Impl() { if (handle) CloseHandle(handle); }
};
LuaTestMailboxLaunchHandle::LuaTestMailboxLaunchHandle(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
LuaTestMailboxLaunchHandle::~LuaTestMailboxLaunchHandle() = default;
std::uint64_t LuaTestMailboxLaunchHandle::Value() const noexcept {
    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(impl_->handle));
}
std::string_view LuaTestMailboxLaunchHandle::Nonce() const noexcept { return impl_->nonce; }
Result<std::unique_ptr<LuaTestMailboxLaunchHandle>> LuaTestMailboxLaunchAccess::Prepare(
    const std::shared_ptr<LuaTestMailboxSession>& session, LuaTestProcessVerifier verify) {
    using Handle = std::unique_ptr<LuaTestMailboxLaunchHandle>;
    if (!session || !verify) return Failure<Handle>("LUA_TEST_OWNER", "Lua 测试启动缺少原进程归属验证。");
    auto& impl = *session->impl_;
    std::lock_guard lock(impl.mutex);
    if (impl.verify || !impl.Identity() || Read(impl.mailbox->runtimeState) != static_cast<LONG>(LuaRuntimeState::Starting))
        return Failure<Handle>("LUA_TEST_OWNER", "Lua 测试会话已经绑定或身份无效，不能重复启动。");
    auto handle = std::make_unique<LuaTestMailboxLaunchHandle::Impl>();
    handle->session = session;
    handle->nonce = impl.nonce;
    if (!DuplicateHandle(GetCurrentProcess(), impl.mapping, GetCurrentProcess(), &handle->handle, 0, TRUE, DUPLICATE_SAME_ACCESS))
        return Failure<Handle>("LUA_TEST_HANDLE", "无法准备 Lua 测试邮箱的继承句柄。");
    auto result = Handle(new LuaTestMailboxLaunchHandle(std::move(handle)));
    impl.verify = std::move(verify);
    return Result<Handle>::Success(std::move(result));
}
} // namespace detail
} // namespace War3Map::Services
