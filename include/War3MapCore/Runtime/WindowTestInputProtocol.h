#pragma once
#include <cstdint>
#include <type_traits>

namespace War3Map::Runtime {
inline constexpr std::uint32_t kWindowInputMagic = 0x41554950;
inline constexpr wchar_t kWindowInputProperty[] = L"Audastra.TestInput.V1";
enum class WindowInputKind : std::uint32_t { Click = 1, Key = 2, Query = 3, Close = 4 };
enum class WindowInputState : std::uint32_t {
    Pending = 1, Complete = 2, Invalid = 16, WrongIdentity = 17,
    Busy = 18, WindowChanged = 19, DispatchFailed = 20, UnknownSequence = 21,
};
inline constexpr std::uint32_t kWindowInputStateMask = 0xff;
inline constexpr std::uint32_t kWindowInputCursorRead = 0x100;
inline constexpr std::uint32_t kWindowInputKeyRead = 0x200;
// WM_COPYDATA只传固定宽度值，不传宿主指针；两种位数必须使用同一布局。
struct WindowInputPacket {
    std::uint32_t magic = kWindowInputMagic, version = 1, bytes = sizeof(WindowInputPacket);
    WindowInputKind kind = WindowInputKind::Query;
    std::uint32_t processId = 0, createdLow = 0, createdHigh = 0, sequence = 0;
    std::int32_t width = 0, height = 0, x = 0, y = 0;
    std::uint32_t keyCount = 0, keys[8]{};
};
static_assert(sizeof(WindowInputPacket) == 84 && std::is_trivially_copyable_v<WindowInputPacket>);
}
