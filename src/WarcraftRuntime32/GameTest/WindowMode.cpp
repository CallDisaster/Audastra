#include "WindowMode.h"

#include <atomic>

namespace Audastra::Runtime32::GameTest {
namespace {

constexpr LONG_PTR kFrameStyles = WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_BORDER | WS_DLGFRAME;
constexpr LONG_PTR kFrameExStyles = WS_EX_DLGMODALFRAME | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE | WS_EX_WINDOWEDGE;
// 最小化时系统把窗口移到这个坐标；这时不能把它拉回显示器。
constexpr int kMinimizedCoordinate = -32000;

WindowModeOptions settings;
std::atomic<bool> started{false};
std::atomic<HWND> mainWindow{nullptr};
WNDPROC original = nullptr;
bool unicode = true;
UINT applyMessage = 0;
// 当前由本模块设置的鼠标限制范围；只放开自己设的，不动别的程序设的限制。
// 限制与放开都只在游戏窗口自己的线程里做，后台线程只投递消息。
std::atomic<bool> clipping{false};
RECT clipRect{};

RECT MonitorRect(HWND window) {
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &info)) return RECT{};
    return info.rcMonitor;
}

LRESULT CallOriginal(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    return unicode ? CallWindowProcW(original, window, message, wParam, lParam)
                   : CallWindowProcA(original, window, message, wParam, lParam);
}

// 游戏窗口画面的屏幕范围。
bool ClientScreenRect(HWND window, RECT& rect) {
    if (!GetClientRect(window, &rect) || rect.right <= rect.left || rect.bottom <= rect.top) return false;
    POINT topLeft{rect.left, rect.top}, bottomRight{rect.right, rect.bottom};
    if (!ClientToScreen(window, &topLeft) || !ClientToScreen(window, &bottomRight)) return false;
    rect = RECT{topLeft.x, topLeft.y, bottomRight.x, bottomRight.y};
    return true;
}

void ReleaseClip() {
    if (!clipping.exchange(false)) return;
    RECT current{};
    // 期间别的程序改过限制就不动它。
    if (GetClipCursor(&current) && EqualRect(&current, &clipRect)) ClipCursor(nullptr);
}

// 游戏窗口在前台且没有最小化时把鼠标限制在画面里；范围变了就重设。
void UpdateClip(HWND window) {
    if (!settings.cursorLock) return;
    if (!window || GetForegroundWindow() != window || IsIconic(window)) { ReleaseClip(); return; }
    RECT rect{};
    if (!ClientScreenRect(window, rect)) { ReleaseClip(); return; }
    RECT current{};
    if (clipping && GetClipCursor(&current) && EqualRect(&current, &rect) && EqualRect(&clipRect, &rect)) return;
    if (!ClipCursor(&rect)) return;
    clipRect = rect;
    if (!clipping.exchange(true) && settings.clips) InterlockedIncrement(settings.clips);
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == applyMessage) {
        if (settings.borderless && MakeBorderless(window) && settings.applied) InterlockedIncrement(settings.applied);
        UpdateClip(window);
        return 0;
    }
    switch (message) {
    case WM_STYLECHANGING:
        // 游戏自己加回标题栏或边框时去掉。
        if (settings.borderless && lParam) {
            auto* change = reinterpret_cast<STYLESTRUCT*>(lParam);
            if (wParam == static_cast<WPARAM>(GWL_STYLE)) change->styleNew = (change->styleNew & ~static_cast<DWORD>(kFrameStyles)) | WS_POPUP;
            else if (wParam == static_cast<WPARAM>(GWL_EXSTYLE)) change->styleNew &= ~static_cast<DWORD>(kFrameExStyles);
        }
        break;
    case WM_WINDOWPOSCHANGING:
        // 游戏按分辨率改窗口大小或位置时仍铺满所在显示器；最小化不拦。
        if (settings.borderless && lParam && !IsIconic(window)) {
            auto* position = reinterpret_cast<WINDOWPOS*>(lParam);
            const bool resizes = !(position->flags & SWP_NOSIZE) || !(position->flags & SWP_NOMOVE);
            if (resizes && position->x != kMinimizedCoordinate && position->y != kMinimizedCoordinate) {
                const RECT monitor = MonitorRect(window);
                if (monitor.right > monitor.left && monitor.bottom > monitor.top) {
                    position->x = monitor.left;
                    position->y = monitor.top;
                    position->cx = monitor.right - monitor.left;
                    position->cy = monitor.bottom - monitor.top;
                    position->flags &= ~static_cast<UINT>(SWP_NOMOVE | SWP_NOSIZE);
                }
            }
        }
        break;
    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE) ReleaseClip();
        break;
    case WM_ACTIVATEAPP:
        if (!wParam) ReleaseClip();
        break;
    case WM_DESTROY:
    case WM_NCDESTROY:
        ReleaseClip();
        mainWindow = nullptr;
        break;
    default:
        break;
    }
    const LRESULT result = CallOriginal(window, message, wParam, lParam);
    // 激活、移动、改大小以后按新的画面范围限制鼠标。
    if (settings.cursorLock && (message == WM_ACTIVATE || message == WM_SIZE || message == WM_MOVE || message == WM_EXITSIZEMOVE))
        UpdateClip(window);
    return result;
}

struct Search {
    DWORD process = 0;
    HWND best = nullptr;
    LONG area = 0;
};

BOOL CALLBACK Consider(HWND window, LPARAM parameter) {
    auto& search = *reinterpret_cast<Search*>(parameter);
    DWORD process = 0;
    GetWindowThreadProcessId(window, &process);
    if (process != search.process || !IsWindowVisible(window) || GetWindow(window, GW_OWNER)
        || (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOOLWINDOW)) return TRUE;
    RECT rect{};
    if (!GetWindowRect(window, &rect)) return TRUE;
    const LONG area = (rect.right - rect.left) * (rect.bottom - rect.top);
    if (area > search.area) { search.best = window; search.area = area; }
    return TRUE;
}

// 找到游戏主窗口后子类化；以后定时补一次：游戏改回样式或大小、切回前台时重新放置与限制鼠标。
DWORD WINAPI Watch(void*) {
    for (;;) {
        HWND window = mainWindow.load();
        if (window && !IsWindow(window)) { mainWindow = nullptr; window = nullptr; }
        if (!window) {
            Search search;
            search.process = GetCurrentProcessId();
            EnumWindows(Consider, reinterpret_cast<LPARAM>(&search));
            if (search.best && !original) {
                unicode = IsWindowUnicode(search.best) != FALSE;
                const LONG_PTR previous = unicode
                    ? SetWindowLongPtrW(search.best, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(WindowProc))
                    : SetWindowLongPtrA(search.best, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(WindowProc));
                if (previous) {
                    original = reinterpret_cast<WNDPROC>(previous);
                    mainWindow = search.best;
                    window = search.best;
                }
            }
        }
        if (window) {
            if (settings.borderless && !IsIconic(window) && !IsBorderless(window)) PostMessageW(window, applyMessage, 0, 0);
            if (settings.cursorLock) {
                // 切走时放开、切回来时重设由窗口过程完成；这里补漏掉的情况（如锁屏、UAC 后系统清掉了限制）。
                const bool active = GetForegroundWindow() == window && !IsIconic(window);
                RECT current{}, rect{};
                const bool stale = active && (!clipping
                    || (GetClipCursor(&current) && ClientScreenRect(window, rect) && !EqualRect(&current, &rect)));
                if (stale || (!active && clipping)) PostMessageW(window, applyMessage, 0, 0);
            }
        }
        Sleep(window ? 250 : 100);
    }
}

} // namespace

bool IsBorderless(HWND window) {
    if (!IsWindow(window)) return false;
    if (GetWindowLongPtrW(window, GWL_STYLE) & kFrameStyles) return false;
    RECT rect{};
    const RECT monitor = MonitorRect(window);
    return GetWindowRect(window, &rect) && EqualRect(&rect, &monitor);
}

bool MakeBorderless(HWND window) {
    if (!IsWindow(window) || IsIconic(window) || IsBorderless(window)) return false;
    const LONG_PTR style = GetWindowLongPtrW(window, GWL_STYLE);
    SetWindowLongPtrW(window, GWL_STYLE, (style & ~kFrameStyles) | WS_POPUP);
    SetWindowLongPtrW(window, GWL_EXSTYLE, GetWindowLongPtrW(window, GWL_EXSTYLE) & ~kFrameExStyles);
    const RECT monitor = MonitorRect(window);
    if (monitor.right <= monitor.left || monitor.bottom <= monitor.top) return false;
    SetWindowPos(window, nullptr, monitor.left, monitor.top, monitor.right - monitor.left, monitor.bottom - monitor.top,
        SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    return IsBorderless(window);
}

bool StartWindowMode(const WindowModeOptions& options, std::string& error) {
    if (!options.borderless && !options.cursorLock) { error = "没有要接管的窗口设置。"; return false; }
    if (started.exchange(true)) { error = "窗口设置已经接管。"; return false; }
    settings = options;
    applyMessage = RegisterWindowMessageW(L"AudastraTestWindowMode");
    if (!applyMessage) { error = "无法登记窗口设置消息：" + std::to_string(GetLastError()); return false; }
    const HANDLE thread = CreateThread(nullptr, 0, Watch, nullptr, 0, nullptr);
    if (!thread) { error = "无法启动窗口设置线程：" + std::to_string(GetLastError()); return false; }
    CloseHandle(thread);
    return true;
}

} // namespace Audastra::Runtime32::GameTest
