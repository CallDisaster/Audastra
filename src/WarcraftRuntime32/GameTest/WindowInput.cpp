#include "WindowInput.h"
#include "War3MapCore/Runtime/WindowTestInputProtocol.h"
#include <array>
#include <atomic>
#include <algorithm>

namespace Audastra::Runtime32::GameTest {
namespace {
using namespace War3Map::Runtime;
thread_local HHOOK inputHook = nullptr;
thread_local HHOOK postedInputHook = nullptr;
std::atomic<HWND> target{};
std::atomic<LONG> cursorX{0}, cursorY{0};
std::array<std::atomic<SHORT>, 256> keys{};
std::atomic<bool> cursorRead{false}, keyRead{false};
std::atomic<ULONGLONG> expires{0};
// 除虚拟查询的原子状态外，仅窗口线程读写动作；不在游戏线程等待或睡眠。
WindowInputPacket action{};
WindowInputState state = WindowInputState::Complete;
UINT_PTR timer = 0;
unsigned phase = 0;
std::atomic<DWORD> ownerThread{0};
ULONGLONG due = 0;
unsigned downSeen = 0, upSeen = 0;
bool moveSeen = false;

bool Owned(HWND window) {
    DWORD pid = 0;
    return IsWindow(window) && GetWindowThreadProcessId(window, &pid) && pid == GetCurrentProcessId()
        && GetAncestor(window, GA_ROOT) == window;
}
bool Identity(const WindowInputPacket& request) {
    FILETIME created{}, exited{}, kernel{}, user{};
    return request.processId == GetCurrentProcessId() && GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)
        && created.dwLowDateTime == request.createdLow && created.dwHighDateTime == request.createdHigh;
}
void ClearKeys() { for (auto& key : keys) key.store(0); }
SHORT ReadVirtualKey(unsigned key, bool edge) {
    const auto read = [edge](unsigned index) { return edge ? keys[index].fetch_and(static_cast<SHORT>(0x8000)) : keys[index].load(); };
    SHORT value = read(key);
    // 无侧别修饰键按左侧键生成；通用查询同时汇总左右侧，保持虚拟查询的一致性。
    for (const auto generic : {VK_SHIFT, VK_CONTROL, VK_MENU}) {
        const unsigned left = generic == VK_SHIFT ? VK_LSHIFT : generic == VK_CONTROL ? VK_LCONTROL : VK_LMENU;
        if (key == static_cast<unsigned>(generic)) value = static_cast<SHORT>(value | read(left) | read(left + 1));
        else if (key == left) value = static_cast<SHORT>(value | read(generic));
    }
    return edge ? value : static_cast<SHORT>(value & 0x8000);
}
LPARAM KeyBits(unsigned key, bool up, bool alt);
void End(WindowInputState result) {
    if (timer) { KillTimer(nullptr, timer); timer = 0; }
    if (result != WindowInputState::Complete && state == WindowInputState::Pending && phase == 1) {
        const auto window = target.load();
        if (Owned(window)) {
            if (action.kind == WindowInputKind::Click) (void)PostMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(action.x, action.y));
            else for (unsigned i = action.keyCount; i > 0; --i) {
                const auto key = action.keys[i - 1];
                const bool alt = std::find(action.keys, action.keys + action.keyCount, static_cast<std::uint32_t>(VK_MENU)) != action.keys + action.keyCount;
                (void)PostMessageW(window, alt || key == VK_F10 ? WM_SYSKEYUP : WM_KEYUP, key, KeyBits(key, true, alt));
            }
        }
    }
    ClearKeys(); expires = 0; state = result;
}
LPARAM KeyBits(unsigned key, bool up, bool alt) {
    const auto scan = MapVirtualKeyW(key, MAPVK_VK_TO_VSC_EX);
    auto bits = 1u | ((scan & 0xff) << 16);
    if (scan & 0xff00) bits |= 1u << 24;
    if (alt) bits |= 1u << 29;
    if (up) bits |= 3u << 30;
    return static_cast<LPARAM>(bits);
}
bool PostKeys(HWND window, bool up) {
    const bool alt = std::find(action.keys, action.keys + action.keyCount, static_cast<std::uint32_t>(VK_MENU)) != action.keys + action.keyCount;
    for (unsigned index = 0; index < action.keyCount; ++index) {
        const auto key = action.keys[up ? action.keyCount - 1 - index : index];
        keys[key].store(up ? 0 : static_cast<SHORT>(0x8001));
        if (!PostMessageW(window, alt || key == VK_F10 ? (up ? WM_SYSKEYUP : WM_SYSKEYDOWN) : (up ? WM_KEYUP : WM_KEYDOWN),
            key, KeyBits(key, up, alt))) return false;
    }
    return true;
}
void CALLBACK Advance(HWND, UINT, UINT_PTR id, DWORD) {
    if (id != timer || state != WindowInputState::Pending || GetTickCount64() < due) return;
    if (GetTickCount64() >= expires.load()) { End(WindowInputState::DispatchFailed); return; }
    const auto window = target.load();
    if (!Owned(window)) { End(WindowInputState::WindowChanged); return; }
    RECT client{};
    if (!GetClientRect(window, &client) || client.right != action.width || client.bottom != action.height) { End(WindowInputState::WindowChanged); return; }
    const LPARAM pixel = MAKELPARAM(action.x, action.y);
    if (phase == 0) {
        bool ok = false;
        if (action.kind == WindowInputKind::Click) {
            keys[VK_LBUTTON].store(static_cast<SHORT>(0x8001));
            ok = PostMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, pixel) != FALSE;
        } else ok = PostKeys(window, false);
        if (!ok) { End(WindowInputState::DispatchFailed); return; }
        phase = 1; due = GetTickCount64() + 250;
    } else if (phase == 1) {
        bool ok = false;
        if (action.kind == WindowInputKind::Click) {
            keys[VK_LBUTTON].store(0);
            ok = PostMessageW(window, WM_LBUTTONUP, 0, pixel) != FALSE;
        } else ok = PostKeys(window, true);
        if (!ok) { End(WindowInputState::DispatchFailed); return; }
        phase = 2; due = GetTickCount64() + 100;
    } else {
        const auto expected = action.kind == WindowInputKind::Click ? 1u : (1u << action.keyCount) - 1;
        if (downSeen == expected && upSeen == expected && (action.kind != WindowInputKind::Click || moveSeen)) End(WindowInputState::Complete);
    }
}
std::uint32_t Report() {
    return static_cast<std::uint32_t>(state) | (cursorRead.load() ? kWindowInputCursorRead : 0) | (keyRead.load() ? kWindowInputKeyRead : 0);
}
LRESULT Receive(HWND window, const WindowInputPacket& request) {
    if (request.magic != kWindowInputMagic || request.version != 1 || request.bytes != sizeof(request) || !request.sequence) return static_cast<LRESULT>(WindowInputState::Invalid);
    if (!Owned(window) || !Identity(request)) return static_cast<LRESULT>(WindowInputState::WrongIdentity);
    DWORD emptyThread = 0; ownerThread.compare_exchange_strong(emptyThread, GetCurrentThreadId());
    if (ownerThread.load() != GetCurrentThreadId()) return static_cast<LRESULT>(WindowInputState::WrongIdentity);
    if (request.kind == WindowInputKind::Query)
        return request.sequence == action.sequence && window == target.load() ? Report() : static_cast<LRESULT>(WindowInputState::UnknownSequence);
    if (state == WindowInputState::Pending) return static_cast<LRESULT>(WindowInputState::Busy);
    RECT client{};
    if (!GetClientRect(window, &client) || request.width < 1 || request.height < 1 || request.width > 32767 || request.height > 32767
        || request.width != client.right || request.height != client.bottom) return static_cast<LRESULT>(WindowInputState::WindowChanged);
    if (request.kind == WindowInputKind::Click) {
        if (request.keyCount || request.x < 0 || request.x >= request.width || request.y < 0 || request.y >= request.height) return static_cast<LRESULT>(WindowInputState::Invalid);
    } else if (request.kind == WindowInputKind::Key) {
        if (!request.keyCount || request.keyCount > 8) return static_cast<LRESULT>(WindowInputState::Invalid);
        for (unsigned i = 0; i < request.keyCount; ++i) {
            const auto key = request.keys[i];
            if (key < 8 || key > 254 || key == 20 || key == 91 || key == 92 || key == 144 || key == 145
                || std::find(request.keys, request.keys + i, key) != request.keys + i) return static_cast<LRESULT>(WindowInputState::Invalid);
        }
    } else if (request.kind != WindowInputKind::Close || request.keyCount) return static_cast<LRESULT>(WindowInputState::Invalid);
    if (target.load() != window) { cursorX = client.right / 2; cursorY = client.bottom / 2; }
    action = request; target = window; ownerThread = GetCurrentThreadId();
    cursorRead = false; keyRead = false; ClearKeys();
    downSeen = upSeen = 0; moveSeen = false;
    if (request.kind == WindowInputKind::Close) {
        state = PostMessageW(window, WM_CLOSE, 0, 0) ? WindowInputState::Complete : WindowInputState::DispatchFailed;
        return Report();
    }
    if (request.kind == WindowInputKind::Click) {
        cursorX = request.x; cursorY = request.y;
        if (!PostMessageW(window, WM_MOUSEMOVE, 0, MAKELPARAM(request.x, request.y))) { state = WindowInputState::DispatchFailed; return Report(); }
    }
    state = WindowInputState::Pending; phase = 0; due = GetTickCount64() + 100; expires = GetTickCount64() + 2000;
    timer = SetTimer(nullptr, 0, 20, Advance);
    if (!timer) End(WindowInputState::DispatchFailed);
    return Report();
}
void ObserveInput(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (GetPropW(window, kWindowInputProperty) != reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(kWindowInputMagic))) return;
    if (message == WM_COPYDATA && lparam) {
        const auto* copy = reinterpret_cast<const COPYDATASTRUCT*>(lparam);
        if (copy->dwData == kWindowInputMagic) {
            if (copy->cbData != sizeof(WindowInputPacket) || !copy->lpData) return;
            const auto packet = *static_cast<const WindowInputPacket*>(copy->lpData);
            const auto answer = Receive(window, packet);
            // 应用可以更换窗口过程或自行处理WM_COPYDATA；回执不依赖其返回值。
            (void)SetPropW(window, kWindowInputResultProperty, reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(answer)));
            (void)SetPropW(window, kWindowInputSequenceProperty, reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(packet.sequence)));
            return;
        }
    }
    if (window == target.load() && ownerThread.load() == GetCurrentThreadId() && state == WindowInputState::Pending) {
        if (action.kind == WindowInputKind::Click && lparam == MAKELPARAM(action.x, action.y)) {
            if (message == WM_MOUSEMOVE) moveSeen = true;
            if (message == WM_LBUTTONDOWN) downSeen = 1;
            if (message == WM_LBUTTONUP) upSeen = 1;
        } else if (action.kind == WindowInputKind::Key) {
            for (unsigned i = 0; i < action.keyCount; ++i) if (wparam == action.keys[i]) {
                if (message == WM_KEYDOWN || message == WM_SYSKEYDOWN) downSeen |= 1u << i;
                if (message == WM_KEYUP || message == WM_SYSKEYUP) upSeen |= 1u << i;
            }
        }
    }
    if (message == WM_NCDESTROY) {
        RemovePropW(window, kWindowInputProperty);
        RemovePropW(window, kWindowInputSequenceProperty);
        RemovePropW(window, kWindowInputResultProperty);
        if (target.load() == window) { End(WindowInputState::WindowChanged); target = nullptr; ownerThread = 0; }
    }
}
LRESULT CALLBACK InputHook(int code, WPARAM wparam, LPARAM lparam) {
    if (code >= 0 && lparam) {
        const auto& message = *reinterpret_cast<const CWPSTRUCT*>(lparam);
        ObserveInput(message.hwnd, message.message, message.wParam, message.lParam);
    }
    return CallNextHookEx(nullptr, code, wparam, lparam);
}
LRESULT CALLBACK PostedInputHook(int code, WPARAM wparam, LPARAM lparam) {
    if (code >= 0 && wparam == PM_REMOVE && lparam) {
        const auto& message = *reinterpret_cast<const MSG*>(lparam);
        if ((message.message >= WM_KEYFIRST && message.message <= WM_KEYLAST)
            || (message.message >= WM_MOUSEFIRST && message.message <= WM_MOUSELAST))
            ObserveInput(message.hwnd, message.message, message.wParam, message.lParam);
    }
    return CallNextHookEx(nullptr, code, wparam, lparam);
}
}
bool InstallWindowInput(HWND window) {
    if (!Owned(window) || GetWindowThreadProcessId(window, nullptr) != GetCurrentThreadId()) return false;
    // 只监视本线程的窗口消息，不替换应用窗口过程，后续过程切换也不会丢失接收器。
    if (!inputHook && !(inputHook = SetWindowsHookExW(WH_CALLWNDPROC, InputHook, nullptr, GetCurrentThreadId()))) return false;
    if (!postedInputHook && !(postedInputHook = SetWindowsHookExW(WH_GETMESSAGE, PostedInputHook, nullptr, GetCurrentThreadId()))) {
        UnhookWindowsHookEx(inputHook); inputHook = nullptr; return false;
    }
    if (!SetPropW(window, War3Map::Runtime::kWindowInputProperty, reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(War3Map::Runtime::kWindowInputMagic)))) {
        return false;
    }
    return true;
}
HWND WindowInputTarget() { const auto value = target.load(); return Owned(value) ? value : nullptr; }
bool WindowInputCursor(HWND window, POINT& point) {
    if (window != WindowInputTarget()) return false;
    point = {cursorX.load(), cursorY.load()};
    cursorRead = true;
    return ClientToScreen(window, &point) != FALSE;
}
SHORT WindowInputKey(int key, bool consumeEdge) {
    if (key < 0 || key >= 256) return 0;
    if (WindowInputTarget()) keyRead = true;
    if (const auto limit = expires.load(); limit && GetTickCount64() >= limit) return 0;
    return ReadVirtualKey(static_cast<unsigned>(key), consumeEdge);
}
BOOL WindowInputKeyboard(PBYTE output) {
    if (!output) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    const auto limit = expires.load(); const bool expired = limit && GetTickCount64() >= limit;
    for (unsigned i = 0; i < 256; ++i) output[i] = !expired && (ReadVirtualKey(i, false) & 0x8000) ? 0x80 : 0;
    if (WindowInputTarget()) keyRead = true;
    return TRUE;
}
}
