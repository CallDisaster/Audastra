#pragma once

#include "aj/Host.h"
#include <functional>
#include <mutex>
#include <optional>
#include <thread>

namespace Audastra::Runtime32::Japi {
// 可测试的 VM 边界；生产实现只委托既有 GameVm127，不另建地址发现/句柄表。
class StringVm {
public:
    virtual ~StringVm() = default;
    virtual std::uint64_t Generation() const noexcept = 0; // 失效时返回 0
    virtual bool IsMainVm(std::uintptr_t vm) const noexcept = 0;
    virtual bool MatchesCurrentMainVm() const noexcept = 0;
    // 成功返回的非零 ID 带一个引用，直接转交 JASS native 返回槽；此处不能再 release。
    virtual std::optional<std::uint32_t> ReturnString(std::string_view text) = 0;
    // 只失效身份，不访问游戏内存；允许故障线程调用。
    virtual void Invalidate() noexcept = 0;
};
struct AttachResult {
    std::shared_ptr<StringVm> vm;
    std::string error;
    // 内置模块：Host 建好后在同一线程装入（Host::Install，同样核对契约与生命周期）。
    // 返回非 AJ_OK 属于运行时故障；地图内容不能经此增加任何模块。
    std::function<std::int32_t(audastra::japi::Host&)> install;
};
class NativeSession final {
public:
    using Attach = std::function<AttachResult()>;
    // 故障先使该 VM 会话永久失效，再通知宿主。启动器可结束专用测试；地图 DLL
    // 只能禁用本代际并记录错误，不能擅自结束玩家进程。
    // reply 只用于观测已成功交给 VM 的响应；异常不会改变已执行命令的结果。
    explicit NativeSession(std::function<void(std::string_view)> fatal,
        std::function<void(std::string_view, std::string_view)> reply = {}, std::string runtimeIdentity = {},
        std::function<bool(uint32_t, int32_t, int32_t, std::string_view)> testReport = {});
    ~NativeSession();
    // nullopt = 不属于本协议；0 = 已消费但 VM 故障，绝不回落原版解释器。
    std::optional<std::uint32_t> Dispatch(std::uint64_t generation, std::string_view input, const Attach& attach);
    void Reset();
    void BeforeVmDestroy(std::uintptr_t vm);
    [[nodiscard]] bool VmIdentityChanged();
private:
    void Fail(std::string_view error);
    void Stop();
    std::function<void(std::string_view)> fatal_;
    std::function<void(std::string_view, std::string_view)> reply_;
    std::recursive_mutex mutex_;
    std::thread::id thread_;
    std::uint64_t generation_ = 0;
    std::shared_ptr<StringVm> vm_;
    std::unique_ptr<audastra::japi::Host> host_;
    unsigned depth_ = 0;
    bool failed_ = false;
    std::string runtimeIdentity_;
    std::function<bool(uint32_t, int32_t, int32_t, std::string_view)> testReport_;
};
}
