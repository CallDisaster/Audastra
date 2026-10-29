#include "War3MapCore/Services/RuntimeTestSession.h"
#include "War3MapCore/Runtime/RuntimeTestEvidence.h"
#include <windows.h>
#include <bcrypt.h>
#include <array>
#include <utility>
namespace War3Map::Services {
using Audastra::Runtime32::RuntimeTestEvidence;
namespace {
template<class T> Result<T> Fail(const char* code, const char* text) {
    return Result<T>::Failure("Map.Test", code, text);
}
}
struct RuntimeTestSession::Impl {
    HANDLE mapping = nullptr;
    RuntimeTestEvidence* view = nullptr;
    std::string nonce, sha;
    std::function<bool(std::uint32_t, std::uint64_t)> owns;
    ~Impl() { if (view) UnmapViewOfFile(view); if (mapping) CloseHandle(mapping); }
};
RuntimeTestSession::RuntimeTestSession(std::unique_ptr<Impl> p) : impl_(std::move(p)) {}
RuntimeTestSession::~RuntimeTestSession() = default;
const std::string& RuntimeTestSession::Nonce() const { return impl_->nonce; }
Result<std::shared_ptr<RuntimeTestSession>> RuntimeTestSession::Create(std::string sha, std::function<bool(std::uint32_t, std::uint64_t)> owns) {
    using namespace Audastra::Runtime32;
    using Session = std::shared_ptr<RuntimeTestSession>;
    if (!RuntimeTestHex(sha, 64) || !owns) return Fail<Session>("RUNTIME_TEST_IDENTITY", "测试证据缺少地图身份或原进程归属验证。");
    auto p = std::make_unique<Impl>(); p->sha = std::move(sha); p->owns = std::move(owns);
    std::array<unsigned char, 16> random{};
    if (BCryptGenRandom(nullptr, random.data(), static_cast<ULONG>(random.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
        return Fail<Session>("RUNTIME_TEST_NONCE", "无法建立本次测试的随机会话身份。");
    for (const auto c : random) { p->nonce += "0123456789abcdef"[c >> 4]; p->nonce += "0123456789abcdef"[c & 15]; }
    const auto name = std::wstring(kRuntimeTestMappingPrefix) + std::wstring(p->nonce.begin(), p->nonce.end());
    p->mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(RuntimeTestEvidence), name.c_str());
    if (!p->mapping || GetLastError() == ERROR_ALREADY_EXISTS) return Fail<Session>("RUNTIME_TEST_MAPPING", "测试证据名称冲突或创建失败。");
    p->view = static_cast<RuntimeTestEvidence*>(MapViewOfFile(p->mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(RuntimeTestEvidence)));
    if (!p->view) return Fail<Session>("RUNTIME_TEST_MAPPING", "无法映射本次测试证据。");
    ::new (p->view) RuntimeTestEvidence{};
    std::memcpy(p->view->nonce, p->nonce.c_str(), 33); std::memcpy(p->view->mapSha256, p->sha.c_str(), 65);
    return Result<Session>::Success(Session(new RuntimeTestSession(std::move(p))));
}
Result<RuntimeTestSnapshot> RuntimeTestSession::Query() const {
    using namespace Audastra::Runtime32;
    RuntimeTestEvidence value;
    bool stable = false;
    for (unsigned retry = 0; retry < 8; ++retry) {
        const auto before = InterlockedCompareExchange(&impl_->view->revision, 0, 0);
        if (before & 1) continue;
        std::memcpy(&value, impl_->view, sizeof(value)); MemoryBarrier();
        if (before == InterlockedCompareExchange(&impl_->view->revision, 0, 0)) { stable = true; break; }
    }
    if (!stable) return Fail<RuntimeTestSnapshot>("RUNTIME_TEST_BUSY", "运行库正在更新本次测试证据，请稍后重试。");
    if (!RuntimeTestHeader(value) || value.nonce != impl_->nonce || value.mapSha256 != impl_->sha
        || value.runtimeSha256[64] || value.expectedSha256[64]
        || value.state < -3 || value.state > 3)
        return Fail<RuntimeTestSnapshot>("RUNTIME_TEST_IDENTITY", "测试证据结构或会话身份不符。");
    RuntimeTestSnapshot out; out.sessionNonce = impl_->nonce; out.mapSha256 = impl_->sha;
    const auto created = (static_cast<std::uint64_t>(value.createdHigh) << 32) | value.createdLow;
    if (value.pid && (!created || !impl_->owns(value.pid, created))) return Fail<RuntimeTestSnapshot>("RUNTIME_TEST_PROCESS", "证据写入者不属于本次已核验的原游戏进程。");
    out.processId = value.pid; out.processCreated = (static_cast<std::uint64_t>(value.createdHigh) << 32) | value.createdLow;
    out.epoch = value.epoch; out.loadedMs = value.loadedMs; out.installedMs = value.installedMs;
    out.helloMs = value.helloMs; out.reportMs = value.reportMs; out.status = value.status;
    out.runtimeSha256 = value.runtimeSha256; out.expectedSha256 = value.expectedSha256;
    out.moduleLoaded = value.pid && value.loadedMs;
    out.handshakeSuccess = out.moduleLoaded && value.epoch && value.state == 3 && value.reportMs && RuntimeTestHex(out.runtimeSha256, 64)
        && out.runtimeSha256 == out.expectedSha256 && out.helloMs && value.status == 0;
    if (value.installStatus != 0 && value.installStatus != 8) {
        // 后来的初始化拒绝优先于同VM的旧成功确认，不能把旧ack用于本次载入尝试。
        out.handshakeSuccess = false; out.state = "initialization_rejected";
        return Result<RuntimeTestSnapshot>::Success(std::move(out));
    }
    if (value.state == 3 && !out.handshakeSuccess)
        return Fail<RuntimeTestSnapshot>("RUNTIME_TEST_IDENTITY", "成功回执缺少一致的DLL身份、握手或时间证据。");
    if (out.handshakeSuccess) out.state = "handshake_success";
    else if (value.state == -3) out.state = "identity_rejected";
    else if (value.state == -2) out.state = "handshake_timeout";
    else if (value.loadedMs && GetTickCount64() >= value.loadedMs + 35000) {
        out.state = "handshake_timeout"; out.hostDeadline = true;
    } else if (value.loadedMs) out.state = "module_loaded";
    return Result<RuntimeTestSnapshot>::Success(std::move(out));
}
}
