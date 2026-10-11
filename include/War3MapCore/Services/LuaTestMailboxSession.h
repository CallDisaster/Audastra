#pragma once

#include "War3MapCore/Runtime/LuaTestMailbox.h"
#include "War3MapCore/Types.h"

#include <memory>
#include <string_view>

namespace War3Map::Services {

namespace detail { class LuaTestMailboxLaunchAccess; }

// 仅用于编辑器新建的单人本地测试。会话不公开内存映射或系统句柄；
// 启动层必须绑定原进程归属，未绑定或未完成运行时握手不能发送代码。
class LuaTestMailboxSession final {
public:
    ~LuaTestMailboxSession();
    LuaTestMailboxSession(const LuaTestMailboxSession&) = delete;
    LuaTestMailboxSession& operator=(const LuaTestMailboxSession&) = delete;
    [[nodiscard]] std::string_view MapSha256() const noexcept;
private:
    struct Impl;
    explicit LuaTestMailboxSession(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
    friend class detail::LuaTestMailboxLaunchAccess;
    friend Result<std::shared_ptr<LuaTestMailboxSession>> CreateLuaTestMailboxSession(std::string_view);
    friend struct LuaTestMailboxOperations;
};

struct LuaTestReloadRequest {
    std::string chunkName;
    std::string source;
};

enum class LuaTestReloadState { Starting, Idle, Queued, Running, Complete, Stopped };

struct LuaTestReloadSnapshot {
    LuaTestReloadState state = LuaTestReloadState::Starting;
    Audastra::Runtime32::LuaRuntimeState runtimeState = Audastra::Runtime32::LuaRuntimeState::Starting;
    std::uint32_t sequence = 0;
    Audastra::Runtime32::LuaReloadResult resultCode = Audastra::Runtime32::LuaReloadResult::NotReady;
    std::string message;
    bool resultTruncated = false;
    bool processActive = false;
};

// 只创建匿名映射，不启动进程、不开放运行时门禁。SHA 必须是64位小写十六进制。
[[nodiscard]] Result<std::shared_ptr<LuaTestMailboxSession>> CreateLuaTestMailboxSession(
    std::string_view mapSha256);
[[nodiscard]] Result<std::uint32_t> SubmitLuaTestReload(
    LuaTestMailboxSession& session, const LuaTestReloadRequest& request);
// sequence=0 查询当前槽；给出序号时拒绝过期请求，不把别的 ACK 当成本次结果。
[[nodiscard]] Result<LuaTestReloadSnapshot> QueryLuaTestReload(
    LuaTestMailboxSession& session, std::uint32_t sequence = 0);
// 仅尚未执行的 Queued 请求可取消。Running 的副作用未知，不假报回滚或取消。
[[nodiscard]] Status CancelLuaTestReload(LuaTestMailboxSession& session, std::uint32_t sequence);

} // namespace War3Map::Services
