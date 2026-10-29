#include "WindowInput.h"
#include "War3MapCore/Runtime/WindowTestInputProtocol.h"
#include <array>
#include <atomic>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwchar>

namespace Audastra::Runtime32::GameTest {
namespace {
using namespace War3Map::Runtime;
std::atomic<bool> endpointStarting{false};
std::atomic<bool> exitRequested{false};
std::atomic<unsigned> exitFaults{0};
HANDLE exitTrace = INVALID_HANDLE_VALUE;

void TraceExitAddress(const char* kind, unsigned code, void* address) noexcept {
    MEMORY_BASIC_INFORMATION region{}; char path[512]{};
    if (VirtualQuery(address, &region, sizeof(region)) && region.Type == MEM_IMAGE)
        GetModuleFileNameA(static_cast<HMODULE>(region.AllocationBase), path, sizeof(path));
    const auto name = std::strrchr(path, '\\'); char line[768]{};
    const auto length = sprintf_s(line, "pid=%lu tid=%lu %s code=%08X address=%p owner=%s offset=%llX\n",
        GetCurrentProcessId(), GetCurrentThreadId(), kind, code, address, name ? name + 1 : path,
        static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(address) - reinterpret_cast<std::uintptr_t>(region.AllocationBase)));
    DWORD written = 0;
    if (length > 0) { (void)WriteFile(exitTrace, line, static_cast<DWORD>(length), &written, nullptr); (void)FlushFileBuffers(exitTrace); }
}
LONG CALLBACK ObserveExitFault(EXCEPTION_POINTERS* fault) noexcept {
    if (!fault || !fault->ExceptionRecord || !exitRequested.load() || fault->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION
        || exitTrace == INVALID_HANDLE_VALUE || exitFaults.fetch_add(1) >= 16) return EXCEPTION_CONTINUE_SEARCH;
    thread_local bool reporting = false;
    if (reporting) return EXCEPTION_CONTINUE_SEARCH;
    reporting = true;
    TraceExitAddress("exception", fault->ExceptionRecord->ExceptionCode, fault->ExceptionRecord->ExceptionAddress);
    void* frames[12]{}; const auto count = CaptureStackBackTrace(0, 12, frames, nullptr);
    for (USHORT index = 0; index < count; ++index) TraceExitAddress("stack", index, frames[index]);
    reporting = false;
    return EXCEPTION_CONTINUE_SEARCH;
}
std::atomic<HWND> target{};
std::atomic<LONG> cursorX{0}, cursorY{0};
std::array<std::atomic<SHORT>, 256> keys{};
std::atomic<bool> cursorRead{false}, keyRead{false};
std::atomic<ULONGLONG> expires{0};
// 除虚拟查询的原子状态外，仅专用控制线程读写动作；不在游戏线程等待或睡眠。
WindowInputPacket action{};
WindowInputState state = WindowInputState::Complete;
UINT_PTR timer = 0;
unsigned phase = 0;
std::atomic<DWORD> ownerThread{0};
ULONGLONG due = 0;

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
    const auto previousDpi = SetThreadDpiAwarenessContext(GetWindowDpiAwarenessContext(window));
    struct Dpi { DPI_AWARENESS_CONTEXT previous; ~Dpi() { if (previous) SetThreadDpiAwarenessContext(previous); } } dpi{previousDpi};
    if (!previousDpi) { End(WindowInputState::WindowChanged); return; }
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
        // 确认已向目标队列投递且虚拟状态已完成；业务效果另由截图/回执验证。
        End(WindowInputState::Complete);
    }
}
std::uint32_t Report() {
    return static_cast<std::uint32_t>(state) | (cursorRead.load() ? kWindowInputCursorRead : 0) | (keyRead.load() ? kWindowInputKeyRead : 0);
}
LRESULT Receive(HWND window, const WindowInputPacket& request) {
    if (request.magic != kWindowInputMagic || request.version != 2 || request.bytes != sizeof(request) || !request.sequence) return static_cast<LRESULT>(WindowInputState::Invalid);
    if (!Owned(window) || !IsWindowVisible(window) || !Identity(request)
        || GetPropW(window, kWindowInputProperty) != reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(kWindowInputMagic))) return static_cast<LRESULT>(WindowInputState::WrongIdentity);
    DWORD emptyThread = 0; ownerThread.compare_exchange_strong(emptyThread, GetCurrentThreadId());
    if (ownerThread.load() != GetCurrentThreadId()) return static_cast<LRESULT>(WindowInputState::WrongIdentity);
    if (request.kind == WindowInputKind::Query)
        return request.sequence == action.sequence && window == target.load() ? Report() : static_cast<LRESULT>(WindowInputState::UnknownSequence);
    if (state == WindowInputState::Pending) return static_cast<LRESULT>(WindowInputState::Busy);
    const auto previousDpi = SetThreadDpiAwarenessContext(GetWindowDpiAwarenessContext(window));
    struct Dpi { DPI_AWARENESS_CONTEXT previous; ~Dpi() { if (previous) SetThreadDpiAwarenessContext(previous); } } dpi{previousDpi};
    if (!previousDpi) return static_cast<LRESULT>(WindowInputState::WindowChanged);
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
    if (request.kind == WindowInputKind::Close) {
        exitRequested = true;
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
LRESULT CALLBACK EndpointProc(HWND endpoint, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_COPYDATA && lparam) {
        const auto* copy = reinterpret_cast<const COPYDATASTRUCT*>(lparam);
        if (copy->dwData == kWindowInputMagic) {
            if (copy->cbData != sizeof(WindowInputPacket) || !copy->lpData) return static_cast<LRESULT>(WindowInputState::Invalid);
            const auto packet = *static_cast<const WindowInputPacket*>(copy->lpData);
            const auto window = reinterpret_cast<HWND>(static_cast<ULONG_PTR>(packet.targetWindow));
            const auto answer = Receive(window, packet);
            (void)SetPropW(endpoint, kWindowInputResultProperty, reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(answer)));
            (void)SetPropW(endpoint, kWindowInputSequenceProperty, reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(packet.sequence)));
            return answer;
        }
    }
    return DefWindowProcW(endpoint, message, wparam, lparam);
}
DWORD WINAPI EndpointThread(void*) {
    // 仅测试控制端存在时、且已请求退出后记录异常元数据；不读取指令或业务内存，不处理异常。
    wchar_t temporary[32768]{}, path[32768]{};
    const auto length = GetTempPathW(32768, temporary);
    if (length && length < 32768 && swprintf_s(path, L"%lsaudastra-test-exit-%lu-%llu.log", temporary,
        GetCurrentProcessId(), GetTickCount64()) > 0) {
        exitTrace = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (exitTrace != INVALID_HANDLE_VALUE) (void)AddVectoredExceptionHandler(0, ObserveExitFault);
    }
    WNDCLASSW type{}; type.lpfnWndProc = EndpointProc; type.hInstance = GetModuleHandleW(nullptr);
    type.lpszClassName = kWindowInputEndpointClass;
    if (!RegisterClassW(&type)) return 1;
    const auto endpoint = CreateWindowExW(0, type.lpszClassName, L"", 0, 0, 0, 0, 0,
        HWND_MESSAGE, nullptr, type.hInstance, nullptr);
    if (!endpoint || !SetPropW(endpoint, kWindowInputEndpointProperty,
        reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(kWindowInputMagic)))) return 2;
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) DispatchMessageW(&message);
    return 0;
}
}
bool InstallWindowInput(HWND window) {
    if (!Owned(window) || GetWindowThreadProcessId(window, nullptr) != GetCurrentThreadId()) return false;
    // 控制线程不依赖游戏加载页的消息循环；不创建可见窗口，不更换游戏窗口过程。
    bool empty = false;
    if (endpointStarting.compare_exchange_strong(empty, true)) {
        const auto thread = CreateThread(nullptr, 0, EndpointThread, nullptr, 0, nullptr);
        if (!thread) { endpointStarting = false; return false; }
        CloseHandle(thread);
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
