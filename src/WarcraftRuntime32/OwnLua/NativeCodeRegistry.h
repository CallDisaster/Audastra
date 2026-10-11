#pragma once

#include "NativeProvider.h"
#include <compare>
#include <functional>

namespace Audastra::Runtime32::OwnLua {
struct NativeCodeOwner {
    std::uintptr_t object = 0;
    std::uint32_t tokenA = 0, tokenB = 0;
    auto operator<=>(const NativeCodeOwner&) const = default;
};
class NativeCodeBackend {
public:
    virtual ~NativeCodeBackend() = default;
    [[nodiscard]] virtual std::uint64_t Generation() const noexcept = 0;
    [[nodiscard]] virtual std::optional<std::uint32_t> Register(std::uintptr_t token) = 0;
    virtual void Release(std::uint32_t code) noexcept = 0;
    [[nodiscard]] virtual std::uintptr_t Resolve(std::uint32_t vmIndex, std::uint32_t code) const noexcept = 0;
    [[nodiscard]] virtual bool OwnerAlive(const NativeCodeOwner& owner) const noexcept = 0;
};
class NativeCodeLease final {
public:
    ~NativeCodeLease();
    [[nodiscard]] std::uint32_t Id() const noexcept;
private:
    friend class NativeCodeRegistry;
    struct Impl;
    explicit NativeCodeLease(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};
struct NativeCodeCreate { std::shared_ptr<NativeCodeLease> lease; std::string error; };

// VM code ID 和 Lua registry 引用的同一拥有者。持久消费者按实际游戏对象弱身份持有；
// 同步消费者只持有本次调用。销毁对象、替换回调与地图退出均能释放，不保留到进程结束。
class NativeCodeRegistry final {
public:
    explicit NativeCodeRegistry(std::shared_ptr<NativeCodeBackend> backend);
    ~NativeCodeRegistry();
    [[nodiscard]] NativeCodeCreate Create(std::shared_ptr<NativeCallback> callback);
    [[nodiscard]] bool RetainFor(const NativeCodeOwner& owner, std::shared_ptr<NativeCodeLease> lease);
    void Forget(const NativeCodeOwner& owner);
    void Sweep(std::size_t maxOwners = 64);
    // 未登记的 token 返回 nullopt，由游戏继续原来的 JASS 执行；命中的错误必须拦截。
    [[nodiscard]] std::optional<NativeCallbackResult> Dispatch(std::uint32_t vmIndex, std::uint32_t code,
        bool hasParameters);
    [[nodiscard]] std::size_t CodeCount() const noexcept;
    [[nodiscard]] std::size_t OwnerCount() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace Audastra::Runtime32::OwnLua
