#pragma once

#include "NativeSignature.h"
#include <memory>
#include <span>
#include <variant>

namespace Audastra::Runtime32::OwnLua {

// 生产拥有者必须持有真实 VM 引用，并在同一地图代际释放；失效地图不能再调用 native。
// 原创 fixture 可另作提供者，但不能将其存在当成真实游戏提供者已经实现。
class NativeHandleOwner {
public:
    virtual ~NativeHandleOwner() = default;
    [[nodiscard]] virtual bool Alive() const noexcept = 0;
};

struct NativeHandle {
    std::uint32_t value = 0;
    std::uint64_t generation = 0;
    std::string type;
    std::shared_ptr<NativeHandleOwner> owner;
};

using NativeValue = std::variant<std::monostate, std::int32_t, float, bool, std::string, NativeHandle>;

struct NativeCallResult {
    bool succeeded = false;
    NativeValue value;
    std::string error;
};

class NativeProvider {
public:
    virtual ~NativeProvider() = default;
    [[nodiscard]] virtual std::uint64_t Generation() const noexcept = 0;
    [[nodiscard]] virtual std::optional<NativeSignature> Lookup(std::string_view name) const = 0;
    // 只能根据已取得的真实类型父链判断；未知子类型不能猜成 handle 兼容。
    [[nodiscard]] virtual bool IsHandleType(std::string_view actual, std::string_view expected) const noexcept = 0;
    [[nodiscard]] virtual NativeCallResult Invoke(std::string_view name, std::span<const NativeValue> arguments) = 0;
};

} // namespace Audastra::Runtime32::OwnLua
