#pragma once

#include "War3MapCore/Services/LuaTestMailboxSession.h"
#include <functional>

namespace War3Map::Services::detail {

// 仅启动器适配层使用。验证器查已持有的原进程句柄和测试 Job，不按邮箱 PID 重开。
enum class LuaTestProcessOwnership { Unconfirmed, OwnedRunning, OwnedExited };
using LuaTestProcessVerifier = std::function<LuaTestProcessOwnership(std::uint32_t)>;

class LuaTestMailboxLaunchHandle final {
public:
    ~LuaTestMailboxLaunchHandle();
    LuaTestMailboxLaunchHandle(const LuaTestMailboxLaunchHandle&) = delete;
    LuaTestMailboxLaunchHandle& operator=(const LuaTestMailboxLaunchHandle&) = delete;
    [[nodiscard]] std::uint64_t Value() const noexcept;
    [[nodiscard]] std::string_view Nonce() const noexcept;
private:
    struct Impl;
    explicit LuaTestMailboxLaunchHandle(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
    friend class LuaTestMailboxLaunchAccess;
};

class LuaTestMailboxLaunchAccess final {
public:
    // 一个会话只绑定一次启动。返回的可继承句柄必须放入显式 HANDLE_LIST；
    // CreateProcess 返回后销毁此局部拥有者，游戏保有自己的映射引用。
    [[nodiscard]] static Result<std::unique_ptr<LuaTestMailboxLaunchHandle>> Prepare(
        const std::shared_ptr<LuaTestMailboxSession>& session, LuaTestProcessVerifier verify,
        bool enableReload = true);
};

} // namespace War3Map::Services::detail
