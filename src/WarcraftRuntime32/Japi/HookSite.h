#pragma once
#include <windows.h>
#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>
namespace Audastra::Runtime32::Japi {
// 只批准原字节，或保留原尾部的单条rel32入口跳转。持有所观察模块直到链不再使用。
class HookSite final {
public:
    HookSite(std::uintptr_t address, std::span<const unsigned char> relocatedOriginal, bool allowRel32Chain);
    [[nodiscard]] bool Approved() const { return approved_; }
    [[nodiscard]] bool Chained() const { return chained_; }
    [[nodiscard]] const std::string& Reason() const { return reason_; }
    [[nodiscard]] bool StillMatches() const noexcept;
    [[nodiscard]] std::uintptr_t Address() const { return address_; }
    [[nodiscard]] std::size_t ObservationCount() const noexcept { return count_; }
    [[nodiscard]] std::uintptr_t ObservationAddress(std::size_t i) const noexcept { return i < count_ ? snapshots_[i].address : 0; }
private:
    struct Snapshot { std::uintptr_t address; std::array<unsigned char, 32> bytes{}; std::size_t count = 0; };
    std::uintptr_t address_ = 0;
    std::vector<std::shared_ptr<void>> owners_;
    std::array<Snapshot, 9> snapshots_{};
    std::size_t count_ = 0;
    bool approved_ = false, chained_ = false;
    std::string reason_;
};
// 事务由调用方统一开始/提交；核对实际修补的是获批入口，不跟随到先前回调正文。
LONG AttachQualifiedHook(const HookSite& site, PVOID* next, PVOID callback) noexcept;
[[nodiscard]] bool HookIsHead(std::uintptr_t site, const void* callback) noexcept;
// 仅测试宿主启用后调用；输出相对模块位置，不执行跳转目标。
std::string DescribeHookSite(std::uintptr_t site);
}
