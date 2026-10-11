#pragma once

#include "War3MapCore/Runtime/LuaTestMailbox.h"
#include <optional>
#include <string>
#include <string_view>

namespace Audastra::Runtime32::OwnLua {

struct LuaReloadChunk {
    std::uint32_t sequence = 0;
    std::string chunkName;
    std::string source;
};

// 仅游戏已验证的主线程可以调用。此类只处理协议，不创建线程或执行 Lua。
class LuaMailboxRuntime {
public:
    explicit LuaMailboxRuntime(LuaTestMailbox& mailbox) noexcept : mailbox_(mailbox) {}
    [[nodiscard]] bool Attach(std::string_view sessionNonce, std::string_view mapSha256) noexcept;
    [[nodiscard]] std::optional<LuaReloadChunk> Take();
    [[nodiscard]] bool Complete(std::uint32_t sequence, LuaReloadResult result, std::string_view message) noexcept;
    void Stop() noexcept;
private:
    [[nodiscard]] bool Identity() const noexcept;
    [[nodiscard]] bool MainThread() const noexcept;
    LuaTestMailbox& mailbox_;
    char nonce_[33]{};
    char mapSha_[65]{};
    std::uint32_t threadId_ = 0;
    std::uint32_t lastSequence_ = 0;
};

} // namespace Audastra::Runtime32::OwnLua
