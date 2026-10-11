#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace Audastra::Runtime32 {

inline constexpr std::uint32_t kLuaMailboxMagic = 0x414c5541;
inline constexpr std::uint32_t kLuaMailboxVersion = 1;
inline constexpr std::size_t kLuaReloadSourceCapacity = 1024 * 1024;
inline constexpr std::size_t kLuaReloadResultCapacity = 16 * 1024;
inline constexpr std::size_t kLuaReloadChunkCapacity = 256;

enum class LuaMailboxState : std::int32_t {
    Empty, Writing, Ready, Running, Complete, Stopped
};
enum class LuaRuntimeState : std::int32_t { Starting, Ready, Failed, Stopped };
enum class LuaReloadResult : std::uint32_t {
    Success, SyntaxError, RuntimeError, BudgetExceeded, InvalidRequest,
    IdentityMismatch, NotReady, Cancelled, HostFailure
};

// editor/launcher/game 共享的固定布局。所有发布通过对齐的 state/runtimeState 做
// Interlocked 操作；除握手外，每次只有状态所有者可以写请求或结果正文。
// 不放指针、size_t、bool、句柄或 C++ atomic，避免 x64/x86 布局差异。
struct alignas(4) LuaTestMailbox {
    std::uint32_t magic = kLuaMailboxMagic;
    std::uint32_t version = kLuaMailboxVersion;
    std::uint32_t structBytes = sizeof(LuaTestMailbox);
    std::int32_t state = static_cast<std::int32_t>(LuaMailboxState::Empty);
    std::int32_t runtimeState = static_cast<std::int32_t>(LuaRuntimeState::Starting);
    std::uint32_t gameProcessId = 0;
    std::uint32_t mainThreadId = 0;
    std::uint32_t requestSequence = 0;
    std::uint32_t resultSequence = 0;
    LuaReloadResult resultCode = LuaReloadResult::NotReady;
    std::uint32_t resultTruncated = 0;
    std::uint32_t chunkNameBytes = 0;
    std::uint32_t sourceBytes = 0;
    std::uint32_t resultBytes = 0;
    char sessionNonce[33]{};
    char mapSha256[65]{};
    char requestNonce[33]{};
    char requestMapSha256[65]{};
    char chunkName[kLuaReloadChunkCapacity]{};
    char source[kLuaReloadSourceCapacity]{};
    char result[kLuaReloadResultCapacity]{};
};

static_assert(std::is_standard_layout_v<LuaTestMailbox>);
static_assert(std::is_trivially_copyable_v<LuaTestMailbox>);
static_assert(alignof(LuaTestMailbox) == 4);
static_assert(offsetof(LuaTestMailbox, state) == 12);
static_assert(offsetof(LuaTestMailbox, sessionNonce) == 56);
static_assert(offsetof(LuaTestMailbox, chunkName) == 252);
static_assert(offsetof(LuaTestMailbox, source) == 508);
static_assert(sizeof(LuaTestMailbox) == 1065468);

} // namespace Audastra::Runtime32
