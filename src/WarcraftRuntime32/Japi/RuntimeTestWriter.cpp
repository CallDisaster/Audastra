#include "RuntimeTestWriter.h"
#include <array>
namespace Audastra::Runtime32::Japi {
bool RuntimeTestWriter::Attach() {
    if (attempted_) return view_ != nullptr;
    attempted_ = true;
    std::array<wchar_t, 34> nonce{};
    if (GetEnvironmentVariableW(kRuntimeTestEnvironment, nonce.data(), static_cast<DWORD>(nonce.size())) != 32) return false;
    for (unsigned i = 0; i < 32; ++i) {
        if (!((nonce[i] >= L'0' && nonce[i] <= L'9') || (nonce[i] >= L'a' && nonce[i] <= L'f'))) return false;
        nonce_ += static_cast<char>(nonce[i]);
    }
    const auto name = std::wstring(kRuntimeTestMappingPrefix) + nonce.data();
    mapping_ = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, name.c_str());
    if (!mapping_) return false;
    auto* view = static_cast<RuntimeTestEvidence*>(MapViewOfFile(mapping_, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(RuntimeTestEvidence)));
    if (!view || !RuntimeTestHeader(*view) || view->nonce != nonce_) { if (view) UnmapViewOfFile(view); return false; }
    view_ = view; return true;
}
bool RuntimeTestWriter::Begin() {
    if (!Attach() || !RuntimeTestHeader(*view_) || view_->nonce != nonce_
        || InterlockedCompareExchange(&view_->writer, 1, 0) != 0) return false;
    if (view_->pid && view_->pid != GetCurrentProcessId()) { InterlockedExchange(&view_->writer, 0); return false; }
    InterlockedIncrement(&view_->revision); return true;
}
void RuntimeTestWriter::End() { MemoryBarrier(); InterlockedIncrement(&view_->revision); InterlockedExchange(&view_->writer, 0); }
void RuntimeTestWriter::Loaded() {
    std::lock_guard lock(mutex_); if (!Begin()) return;
    FILETIME created{}, exited{}, kernel{}, user{};
    if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
        view_->pid = GetCurrentProcessId(); view_->createdLow = created.dwLowDateTime; view_->createdHigh = created.dwHighDateTime;
        if (!view_->loadedMs) view_->loadedMs = GetTickCount64();
    }
    End();
}
void RuntimeTestWriter::Installed(int status, std::string_view identity) {
    std::lock_guard lock(mutex_); if (!Begin()) return;
    view_->installStatus = status;
    if (!view_->state || status) view_->status = status;
    if (!status && !view_->installedMs) view_->installedMs = GetTickCount64();
    if (RuntimeTestHex(identity, 64)) std::memcpy(view_->runtimeSha256, identity.data(), 64);
    End();
}
void RuntimeTestWriter::Hello(std::uint32_t epoch) {
    std::lock_guard lock(mutex_); if (!Begin()) return;
    if (view_->epoch != epoch) {
        view_->state = 0; view_->reportMs = 0; view_->expectedSha256[0] = 0; view_->helloMs = 0;
    }
    view_->epoch = epoch; if (!view_->helloMs) view_->helloMs = GetTickCount64(); End();
}
void RuntimeTestWriter::Reset(std::uint32_t epoch) {
    std::lock_guard lock(mutex_); if (!Begin()) return;
    view_->epoch = epoch; view_->state = 0; view_->status = 8; view_->reportMs = 0; view_->helloMs = 0;
    view_->expectedSha256[0] = 0; view_->loadedMs = GetTickCount64(); End();
}
bool RuntimeTestWriter::Report(std::uint32_t epoch, int state, int status, std::string_view expected) {
    std::lock_guard lock(mutex_);
    if (!RuntimeTestHex(expected, 64) || !((state == 3 && status == 0) || (state == -3 && status == 14)
        || (state == -2 && status != 0)) || !Begin()) return false;
    bool ok = epoch == view_->epoch && view_->helloMs && view_->installStatus == 0;
    if (ok && state == 3) ok = expected == std::string_view(view_->runtimeSha256, 64);
    if (ok && state == -3) ok = expected != std::string_view(view_->runtimeSha256, 64);
    if (ok) { view_->state = state; view_->status = status; view_->reportMs = GetTickCount64(); std::memcpy(view_->expectedSha256, expected.data(), 64); }
    End(); return ok;
}
}
