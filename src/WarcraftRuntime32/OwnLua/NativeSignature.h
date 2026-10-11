#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Audastra::Runtime32::OwnLua {

enum class NativeKind { Nothing, Integer, Real, Boolean, String, Code, Handle };
struct NativeType {
    NativeKind kind = NativeKind::Nothing;
    std::string handleType;
    bool operator==(const NativeType&) const = default;
};
struct NativeSignature {
    std::vector<NativeType> arguments;
    NativeType returns;
    bool operator==(const NativeSignature&) const = default;
};

// 游戏实际注册签名，例如 (Htimer;RBC)V。解析失败不猜参数数量或 ABI。
[[nodiscard]] std::optional<NativeSignature> ParseNativeSignature(std::string_view text);
[[nodiscard]] bool NativeIdentifier(std::string_view text) noexcept;

} // namespace Audastra::Runtime32::OwnLua
