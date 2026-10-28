#pragma once
#include "War3MapCore/Runtime/RuntimeTestEvidence.h"
#include <windows.h>
#include <mutex>
#include <string>
namespace Audastra::Runtime32::Japi {
class RuntimeTestWriter {
public:
    ~RuntimeTestWriter() { if (view_) UnmapViewOfFile(view_); if (mapping_) CloseHandle(mapping_); }
    void Loaded();
    bool Enabled() { std::lock_guard lock(mutex_); return Attach(); }
    void Installed(int status, std::string_view identity);
    void Hello(std::uint32_t epoch);
    void Reset(std::uint32_t epoch);
    bool Report(std::uint32_t epoch, int state, int status, std::string_view expected);
private:
    bool Attach();
    bool Begin();
    void End();
    std::mutex mutex_;
    bool attempted_ = false;
    HANDLE mapping_ = nullptr;
    RuntimeTestEvidence* view_ = nullptr;
    std::string nonce_;
};
}
