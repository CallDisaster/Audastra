#pragma once
#include <Windows.h>
#include <string>
#include <memory>
#include <mutex>
#include <functional>
#include <string_view>

namespace Audastra::Runtime32::Japi {
using RuntimeInitialize = int(__cdecl*)();
class ProcessOwnerLease final {
public:
    ~ProcessOwnerLease();
    ProcessOwnerLease(const ProcessOwnerLease&) = delete;
    ProcessOwnerLease& operator=(const ProcessOwnerLease&) = delete;
    static std::unique_ptr<ProcessOwnerLease> Acquire(const void* ownCode);
    int status = 0;
    std::string identity, error;
    RuntimeInitialize existing = nullptr;
private:
    ProcessOwnerLease() = default;
    HANDLE mutex_ = nullptr;
    bool locked_ = false;
};
// 统一先取进程锁，再委托或取模块锁；不能让副本间委托形成锁顺序反转。
int InitializeProcessRuntime(const void* ownCode, std::mutex& moduleLock,
    const std::function<int(std::string_view)>& install, std::string& error);
}
