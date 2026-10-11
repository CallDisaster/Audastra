#pragma once

#include "NativeSignature.h"
#include <map>

namespace Audastra::Runtime32::OwnLua {

struct NativeRegistration {
    std::string name;
    NativeSignature signature;
    std::uintptr_t function = 0;
};

[[nodiscard]] std::string DescribeNativeRegistration(std::size_t acceptedEntries, std::string_view name,
    std::string_view signature, std::uintptr_t function);

// 由经过 SHA 身份检查的游戏注册观察器填充。不是硬编码 native 地址表。
class NativeRegistry final {
public:
    [[nodiscard]] bool Observe(std::string_view name, std::string_view signature, std::uintptr_t function);
    // 原版分 stock 与补充两个明确阶段；任何阶段错误会使整个快照失效。
    [[nodiscard]] bool CompleteStock();
    [[nodiscard]] bool BeginSupplement();
    [[nodiscard]] bool CompleteSupplement();
    // 首次 Lua 入口前封存；后续模块可重复确认，但任何登记修改都必须拒绝。
    [[nodiscard]] bool Seal();
    [[nodiscard]] bool Collecting() const noexcept;
    [[nodiscard]] bool Ready() const noexcept;
    [[nodiscard]] bool Sealed() const noexcept;
    [[nodiscard]] const NativeRegistration* Find(std::string_view name) const noexcept;
    [[nodiscard]] std::size_t Size() const noexcept { return entries_.size(); }
    [[nodiscard]] bool Valid() const noexcept { return !failed_; }
    [[nodiscard]] std::string_view Error() const noexcept { return error_; }
    [[nodiscard]] const auto& Entries() const noexcept { return entries_; }
private:
    enum class Phase { Stock, StockReady, Supplement, SupplementReady, Sealed };
    [[nodiscard]] bool Advance(Phase expected, Phase next);
    Phase phase_ = Phase::Stock;
    std::map<std::string, NativeRegistration, std::less<>> entries_;
    bool failed_ = false;
    std::string error_;
};

} // namespace Audastra::Runtime32::OwnLua
