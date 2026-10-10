#pragma once

#include <span>
#include <string_view>

namespace War3Map {

enum class CapabilityLevel { Unavailable, Partial, Supported };
// 实现能力与外部验收分开：Supported 不代表已经通过原版保存或游戏验证。
struct ModuleCapability {
    std::string_view id;
    std::string_view name;
    CapabilityLevel read;
    CapabilityLevel edit;
    CapabilityLevel roundtrip;
    CapabilityLevel compile;
    std::string_view remaining;
};

class CapabilityRegistry {
public:
    [[nodiscard]] static std::span<const ModuleCapability> Modules() noexcept;
    [[nodiscard]] static const ModuleCapability* Find(std::string_view id) noexcept;
};

} // namespace War3Map
