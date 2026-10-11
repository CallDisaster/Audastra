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
    [[nodiscard]] const NativeRegistration* Find(std::string_view name) const noexcept;
    [[nodiscard]] std::size_t Size() const noexcept { return entries_.size(); }
    [[nodiscard]] bool Valid() const noexcept { return !failed_; }
    [[nodiscard]] std::string_view Error() const noexcept { return error_; }
    [[nodiscard]] const auto& Entries() const noexcept { return entries_; }
private:
    std::map<std::string, NativeRegistration, std::less<>> entries_;
    bool failed_ = false;
    std::string error_;
};

} // namespace Audastra::Runtime32::OwnLua
