#include "War3MapCore/Script/MapTestToolchain.h"

#include "ExternalProcess.h"
#include "GameScreenshot127.h"
#include "MapTestLaunchInternal.h"
#include "../Core/Services/LuaTestMailboxSessionInternal.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/IO/FileBytes.h"
#include "War3MapCore/Services/MapTestWorkspace.h"
#include "War3MapCore/VerifiedFile.h"
#include "War3MapCore/Runtime/RuntimeTestEvidence.h"
#include "War3MapCore/Runtime/WindowTestInputProtocol.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <array>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <appmodel.h>
#endif

namespace War3Map::Script {
struct MapTestLaunchLease::Impl {
#ifdef _WIN32
    HANDLE job = nullptr;
    HANDLE launcher = nullptr;
    HANDLE completion = nullptr;
    std::atomic<DWORD> launcherId{0};
    std::atomic<bool> stop{false}, uncertain{false};
    mutable std::mutex mutex;
    mutable std::mutex lifecycle;
    bool finished = false;
    bool coverageConfirmed = false;
    DWORD exitCode = STILL_ACTIVE;
    bool cleaned = false;
    std::filesystem::path receipt;
    std::shared_ptr<Services::MapTestWorkspaceLease> mapFiles;
    std::vector<HANDLE> protectedDirectories;
    std::vector<HANDLE> protectedFiles;
    std::filesystem::path gameImage;
    std::shared_ptr<Services::RuntimeTestSession> runtimeEvidence;
    HDESK desktop = nullptr; // 后台测试的不可见桌面；进程都退出后随本记录关闭。
    bool cloaked = false; // 游戏窗口在用户桌面上隐身。
    FILETIME launcherCreated{};
    bool launcherGameIdentity = false;
    std::atomic<std::uint32_t> inputSequence{0};
    struct Child { HANDLE handle; DWORD id; FILETIME created; bool gameIdentity = false; bool exited = false; DWORD exitCode = STILL_ACTIVE; bool exitKnown = false; };
    std::vector<Child> children;
    std::thread monitor;
    static bool ImageMatches(HANDLE handle, const std::filesystem::path& expected) noexcept {
        std::array<wchar_t, 32768> text{}; DWORD length = static_cast<DWORD>(text.size());
        return !expected.empty() && QueryFullProcessImageNameW(handle, 0, text.data(), &length)
            && CompareStringOrdinal(text.data(), static_cast<int>(length), expected.c_str(), -1, TRUE) == CSTR_EQUAL;
    }
    Services::detail::LuaTestProcessOwnership Ownership(std::uint32_t id) const noexcept {
        using Ownership = Services::detail::LuaTestProcessOwnership;
        try {
            std::lock_guard stateLock(lifecycle);
            std::lock_guard childLock(mutex);
            const Child* selected = nullptr;
            for (const auto& child : children) if (child.id == id) {
                if (selected) return Ownership::Unconfirmed; // 曾复用的PID不能凭邮箱重新择一。
                selected = &child;
            }
            if (!selected || !selected->gameIdentity) return Ownership::Unconfirmed;
            if (!selected->handle) return selected->exited ? Ownership::OwnedExited : Ownership::Unconfirmed;
            FILETIME created{}, exited{}, kernel{}, user{}; BOOL belongs = FALSE;
            if (!GetProcessTimes(selected->handle, &created, &exited, &kernel, &user)
                || CompareFileTime(&created, &selected->created) != 0
                || !IsProcessInJob(selected->handle, job, &belongs) || !belongs
                || !ImageMatches(selected->handle, gameImage)) return Ownership::Unconfirmed;
            const auto wait = WaitForSingleObject(selected->handle, 0);
            return wait == WAIT_OBJECT_0 ? Ownership::OwnedExited
                : wait == WAIT_TIMEOUT ? Ownership::OwnedRunning : Ownership::Unconfirmed;
        } catch (...) { return Ownership::Unconfirmed; }
    }
    bool Terminal(bool& coverage) const noexcept {
        if (!job || !launcher) return false;
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
        if (!QueryInformationJobObject(job, JobObjectBasicAccountingInformation, &accounting, sizeof(accounting), nullptr)
            || accounting.ActiveProcesses != 0 || WaitForSingleObject(launcher, 0) != WAIT_OBJECT_0) return false;
        try {
            std::lock_guard lock(mutex);
            for (const auto& child : children) if (WaitForSingleObject(child.handle, 0) != WAIT_OBJECT_0) return false;
            coverage = !uncertain.load() && accounting.TotalProcesses == children.size() + 1;
            return true;
        } catch (...) { return false; }
    }
    void CloseObserved() noexcept {
        stop.store(true);
        if (completion) PostQueuedCompletionStatus(completion, 0, 0, nullptr);
        if (monitor.joinable()) monitor.join();
        // 关闭句柄后仍保留已核验的过去身份，最后一份异步诊断可继续读取。
        std::lock_guard lock(mutex);
        for (auto& child : children) if (child.handle) {
            child.exited = WaitForSingleObject(child.handle, 0) == WAIT_OBJECT_0;
            child.exitKnown = child.exited && GetExitCodeProcess(child.handle, &child.exitCode);
            CloseHandle(child.handle); child.handle = nullptr;
        }
        if (launcher) { CloseHandle(launcher); launcher = nullptr; }
        if (completion) { CloseHandle(completion); completion = nullptr; }
        if (job) { CloseHandle(job); job = nullptr; }
    }
    void Observe() noexcept {
        try {
            while (!stop.load()) {
                DWORD message = 0; ULONG_PTR key = 0; LPOVERLAPPED value = nullptr;
                if (!GetQueuedCompletionStatus(completion, &message, &key, &value, 100)) {
                    if (GetLastError() == WAIT_TIMEOUT) continue;
                    uncertain.store(true); return;
                }
                if (stop.load()) return;
                if (key != reinterpret_cast<ULONG_PTR>(this)) { uncertain.store(true); continue; }
                if (message != JOB_OBJECT_MSG_NEW_PROCESS) continue;
                const auto id = static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(value));
                if (id == launcherId.load()) continue; // 原launcher已有CreateProcess返回的原句柄。
                HANDLE handle = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_DUP_HANDLE, FALSE, id);
                BOOL belongs = FALSE; FILETIME created{}, exited{}, kernel{}, user{};
                if (!handle || !IsProcessInJob(handle, job, &belongs) || !belongs
                    || !GetProcessTimes(handle, &created, &exited, &kernel, &user)) {
                    if (handle) CloseHandle(handle); uncertain.store(true); continue;
                }
                // 最后一个宿主Job句柄关闭会移除命名空间名称。把同Job句柄直接交给
                // 这个已捕获原进程持有，随其自然退出关闭；不创建keeper进程、不杀游戏。
                // 仅保证当前已捕获的进程，编辑器退出后无法继续观察新后代。
                HANDLE remoteJob = nullptr;
                if (!DuplicateHandle(GetCurrentProcess(), job, handle, &remoteJob, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
                    CloseHandle(handle); uncertain.store(true); continue;
                }
                std::lock_guard lock(mutex);
                if (children.size() >= 256) { CloseHandle(handle); uncertain.store(true); continue; }
                bool duplicate = false;
                for (const auto& child : children) if (child.id == id && child.created.dwLowDateTime == created.dwLowDateTime
                    && child.created.dwHighDateTime == created.dwHighDateTime) duplicate = true;
                // 消息延迟/PID复用不能把两个过去身份折成一个新句柄。
                if (duplicate) { CloseHandle(handle); uncertain.store(true); continue; }
                const bool gameIdentity = CompareFileTime(&created, &launcherCreated) >= 0 && ImageMatches(handle, gameImage);
                try { children.push_back({handle, id, created, gameIdentity, false}); }
                catch (...) { CloseHandle(handle); throw; }
            }
        } catch (...) { uncertain.store(true); }
    }
    ~Impl() {
        CloseObserved();
        for (auto handle : protectedDirectories) CloseHandle(handle);
        for (auto handle : protectedFiles) CloseHandle(handle);
        if (desktop) CloseDesktop(desktop);
    }
#endif
};
MapTestLaunchLease::MapTestLaunchLease(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
MapTestLaunchLease::~MapTestLaunchLease() = default;
bool MapTestLaunchLease::IsActive() const noexcept {
#ifdef _WIN32
    if (!impl_) return true;
    try {
        std::lock_guard lock(impl_->lifecycle);
        if (impl_->finished) return false;
        if (!impl_->Terminal(impl_->coverageConfirmed)) return true;
        // Job无活动进程且已捕获原句柄终态后释放命名Job。
        // 捕获不完整时只解开重试入口，Cleanup仍保留所有副本。
        if (!GetExitCodeProcess(impl_->launcher, &impl_->exitCode)) return true;
        impl_->CloseObserved(); impl_->finished = true; return false;
    } catch (...) { return true; }
#else
    return true;
#endif
}

Status MapTestLaunchLease::Stop() {
#ifdef _WIN32
    if (!impl_) return Status::Failure("Map.Test", "MAP_TEST_STOP_FAILED", "测试启动记录无效。");
    std::lock_guard lock(impl_->lifecycle);
    if (impl_->finished || !impl_->job) return Status::Ok();
    if (!TerminateJobObject(impl_->job, 1))
        return Status::Failure("Map.Test", "MAP_TEST_STOP_FAILED", "无法结束本次测试的游戏进程（Win32=" + std::to_string(GetLastError()) + "）。");
    return Status::Ok();
#else
    return Status::Failure("Map.Test", "MAP_TEST_STOP_FAILED", "当前平台不支持结束测试。");
#endif
}

std::vector<std::uint32_t> MapTestLaunchLease::ProcessIds() const {
    std::vector<std::uint32_t> ids;
#ifdef _WIN32
    if (!impl_) return ids;
    std::lock_guard lock(impl_->lifecycle);
    if (impl_->finished || !impl_->job) return ids;
    struct { JOBOBJECT_BASIC_PROCESS_ID_LIST list; ULONG_PTR more[255]; } buffer{};
    if (!QueryInformationJobObject(impl_->job, JobObjectBasicProcessIdList, &buffer, sizeof(buffer), nullptr)) return ids;
    for (DWORD index = 0; index < buffer.list.NumberOfProcessIdsInList && index < 256; ++index)
        ids.push_back(static_cast<std::uint32_t>(buffer.list.ProcessIdList[index]));
#endif
    return ids;
}

void* MapTestLaunchLease::HiddenDesktop() const noexcept {
#ifdef _WIN32
    return impl_ ? impl_->desktop : nullptr;
#else
    return nullptr;
#endif
}

bool MapTestLaunchLease::Unseen() const noexcept {
#ifdef _WIN32
    return impl_ && (impl_->desktop || impl_->cloaked);
#else
    return false;
#endif
}

void* MapTestLaunchLease::GameWindow() const {
#ifdef _WIN32
    const auto processes = ProcessIds();
    struct Search { const std::vector<std::uint32_t>* processes; HWND best = nullptr; LONG area = 0; } search{&processes};
    const auto visit = [](HWND window, LPARAM parameter) -> BOOL {
        auto& found = *reinterpret_cast<Search*>(parameter);
        DWORD process = 0; GetWindowThreadProcessId(window, &process);
        if (!IsWindowVisible(window) || IsIconic(window)
            || std::find(found.processes->begin(), found.processes->end(), process) == found.processes->end()) return TRUE;
        RECT client{};
        if (!GetClientRect(window, &client)) return TRUE;
        const auto area = (client.right - client.left) * (client.bottom - client.top);
        if (area > found.area) { found.area = area; found.best = window; }
        return TRUE;
    };
    if (processes.empty()) return nullptr;
    if (const auto desktop = static_cast<HDESK>(HiddenDesktop())) EnumDesktopWindows(desktop, visit, reinterpret_cast<LPARAM>(&search));
    else EnumWindows(visit, reinterpret_cast<LPARAM>(&search));
    return search.best;
#else
    return nullptr;
#endif
}

bool MapTestLaunchLease::PressSpace() const {
#ifdef _WIN32
    if (impl_ && impl_->cloaked) return SendTestInput({MapTestInputKind::Key, 0, 0, {VK_SPACE}}).Succeeded();
    const auto window = static_cast<HWND>(GameWindow());
    if (!window) return false;
    PostMessageW(window, WM_KEYDOWN, VK_SPACE, 0x00390001);
    PostMessageW(window, WM_KEYUP, VK_SPACE, static_cast<LPARAM>(0xC0390001u));
    return true;
#else
    return false;
#endif
}

Result<MapTestInputReceipt> MapTestLaunchLease::SendTestInput(const MapTestInput& input) const {
    // 失败时只记录本Job窗口，不收集其他应用的标题或窗口内容。
    const auto ids = ProcessIds();
    const auto fail = [&](const char* code, const std::string& text) {
        std::string detail = text;
#ifdef _WIN32
        struct Inventory { const std::vector<std::uint32_t>& ids; nlohmann::json rows = nlohmann::json::array(); bool truncated = false; } inventory{ids};
        const auto visit = [](HWND window, LPARAM param) -> BOOL {
            auto& value = *reinterpret_cast<Inventory*>(param); DWORD pid = 0;
            GetWindowThreadProcessId(window, &pid);
            if (std::find(value.ids.begin(), value.ids.end(), pid) == value.ids.end()) return TRUE;
            if (value.rows.size() >= 64) { value.truncated = true; return FALSE; }
            char name[256]{}; GetClassNameA(window, name, sizeof(name)); RECT rect{};
            const bool geometry = GetClientRect(window, &rect) != FALSE;
            const bool receiver = GetPropW(window, Runtime::kWindowInputProperty)
                == reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(Runtime::kWindowInputMagic));
            DWORD cloak = 0; HRESULT cloakStatus = E_NOTIMPL;
            if (const auto module = LoadLibraryExW(L"dwmapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
                using Query = HRESULT(WINAPI*)(HWND, DWORD, PVOID, DWORD);
                if (const auto query = reinterpret_cast<Query>(GetProcAddress(module, "DwmGetWindowAttribute")))
                    cloakStatus = query(window, 14, &cloak, sizeof(cloak));
                FreeLibrary(module);
            }
            const char* reason = GetAncestor(window, GA_ROOT) != window ? "not_root" : !IsWindowVisible(window) ? "not_visible" : IsIconic(window) ? "minimized"
                : !receiver ? "test_receiver_missing" : !geometry || rect.right <= 0 || rect.bottom <= 0
                || rect.right > 32767 || rect.bottom > 32767 ? "invalid_client_geometry" : "eligible_requires_unique_game_identity";
            value.rows.push_back({{"hwnd", reinterpret_cast<std::uintptr_t>(window)}, {"pid", pid}, {"class", name},
                {"visible", IsWindowVisible(window) != FALSE}, {"minimized", IsIconic(window) != FALSE},
                {"width", rect.right}, {"height", rect.bottom}, {"test_receiver", receiver},
                {"dwm_cloak_known", SUCCEEDED(cloakStatus)}, {"dwm_cloak_flags", cloak}, {"reason", reason}});
            return TRUE;
        };
        const auto ok = impl_ && impl_->desktop ? EnumDesktopWindows(impl_->desktop, visit, reinterpret_cast<LPARAM>(&inventory))
            : EnumWindows(visit, reinterpret_cast<LPARAM>(&inventory));
        detail += " input_window_inventory=" + nlohmann::json({{"owned_job_pids", ids}, {"cloaked_plan", impl_ && impl_->cloaked},
            {"hidden_desktop", impl_ && impl_->desktop != nullptr}, {"enumeration_ok", ok != FALSE},
            {"truncated", inventory.truncated}, {"windows", inventory.rows}}).dump();
#endif
        return Result<MapTestInputReceipt>::Failure("Map.Test", code, std::move(detail));
    };
    const auto valid = ValidateMapTestInput(input);
    if (!valid.Succeeded()) { Result<MapTestInputReceipt> result; result.diagnostics = valid.diagnostics; return result; }
#ifdef _WIN32
    using namespace Runtime;
    if (!impl_ || !impl_->cloaked) return fail("MAP_TEST_INPUT_UNSUPPORTED", "虚拟测试输入只支持本次--cloaked启动，不会退回前台输入。");
    std::lock_guard stateLock(impl_->lifecycle);
    if (impl_->finished || !impl_->job) return fail("MAP_TEST_INPUT_ENDED", "本次测试已经结束。");
    HANDLE process = nullptr; DWORD id = 0; FILETIME created{};
    {
        std::lock_guard lock(impl_->mutex);
        for (const auto& child : impl_->children) if (child.gameIdentity && child.handle && WaitForSingleObject(child.handle, 0) == WAIT_TIMEOUT) {
            if (id) return fail("MAP_TEST_INPUT_AMBIGUOUS", "本次Job有多个游戏进程，拒绝选择输入目标。");
            process = child.handle; id = child.id; created = child.created;
        }
    }
    if (!id && impl_->launcherGameIdentity && impl_->launcher && WaitForSingleObject(impl_->launcher, 0) == WAIT_TIMEOUT) {
        process = impl_->launcher; id = impl_->launcherId.load(); created = impl_->launcherCreated;
    }
    const auto owned = [&] {
        FILETIME actual{}, exited{}, kernel{}, user{}; BOOL belongs = FALSE;
        return process && WaitForSingleObject(process, 0) == WAIT_TIMEOUT && GetProcessTimes(process, &actual, &exited, &kernel, &user)
            && CompareFileTime(&created, &actual) == 0 && IsProcessInJob(process, impl_->job, &belongs) && belongs && Impl::ImageMatches(process, impl_->gameImage);
    };
    if (!owned()) return fail("MAP_TEST_INPUT_NO_GAME", "无法确认本次测试原游戏句柄、创建时间、Job和映像身份。");
    struct Search { DWORD pid; HWND window = nullptr; LONG area = 0; bool tied = false; } search{id};
    const auto visit = [](HWND window, LPARAM parameter) -> BOOL {
        auto& value = *reinterpret_cast<Search*>(parameter); DWORD pid = 0;
        GetWindowThreadProcessId(window, &pid);
        if (pid != value.pid || GetAncestor(window, GA_ROOT) != window || !IsWindowVisible(window) || IsIconic(window)
            || GetPropW(window, kWindowInputProperty) != reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(kWindowInputMagic))) return TRUE;
        RECT rect{}; if (!GetClientRect(window, &rect) || rect.right <= 0 || rect.bottom <= 0 || rect.right > 32767 || rect.bottom > 32767) return TRUE;
        const auto area = rect.right * rect.bottom;
        if (area > value.area) { value.window = window; value.area = area; value.tied = false; }
        else if (area == value.area) value.tied = true;
        return TRUE;
    };
    if (impl_->desktop) EnumDesktopWindows(impl_->desktop, visit, reinterpret_cast<LPARAM>(&search));
    else EnumWindows(visit, reinterpret_cast<LPARAM>(&search));
    if (!search.window || search.tied) return fail("MAP_TEST_INPUT_NO_WINDOW", "本次游戏的测试接收窗口不存在或不唯一；未向其他窗口发送。");
    const auto window = search.window; DWORD pid = 0; const auto thread = GetWindowThreadProcessId(window, &pid);
    // 宿主与游戏可能采用不同DPI模式；仅切换本调用线程的查询上下文，结束后立即恢复。
    using QueryDpi = HANDLE(WINAPI*)(HWND);
    using ChangeDpi = HANDLE(WINAPI*)(HANDLE);
    struct DpiScope { ChangeDpi change = nullptr; HANDLE previous = nullptr; ~DpiScope() { if (previous) change(previous); } } dpi;
    const auto user32 = GetModuleHandleW(L"user32.dll");
    const auto queryDpi = reinterpret_cast<QueryDpi>(GetProcAddress(user32, "GetWindowDpiAwarenessContext"));
    dpi.change = reinterpret_cast<ChangeDpi>(GetProcAddress(user32, "SetThreadDpiAwarenessContext"));
    const auto context = queryDpi ? queryDpi(window) : nullptr;
    if (!context || !dpi.change || !(dpi.previous = dpi.change(context)))
        return fail("MAP_TEST_INPUT_DPI_UNCONFIRMED", "无法确认目标窗口的DPI客户区坐标，拒绝猜测像素或使用前台输入。");
    RECT client{};
    if (!thread || pid != id || !GetClientRect(window, &client)) return fail("MAP_TEST_INPUT_WINDOW_CHANGED", "测试窗口身份或客户区已改变。");
    WindowInputPacket packet; packet.processId = id; packet.createdLow = created.dwLowDateTime; packet.createdHigh = created.dwHighDateTime;
    packet.sequence = ++impl_->inputSequence; if (!packet.sequence) return fail("MAP_TEST_INPUT_SEQUENCE", "本次测试输入序号已耗尽。");
    packet.width = client.right; packet.height = client.bottom;
    packet.kind = input.kind == MapTestInputKind::Click ? WindowInputKind::Click : input.kind == MapTestInputKind::Key ? WindowInputKind::Key : WindowInputKind::Close;
    if (input.kind == MapTestInputKind::Click) {
        const auto pixel = MapTestInputPixel(input.x, input.y, client.right, client.bottom);
        if (!pixel.Succeeded() || !pixel.value) return fail("MAP_TEST_INPUT_GEOMETRY", "实际客户区无法换算界面坐标。");
        packet.x = pixel.value->first; packet.y = pixel.value->second;
    }
    packet.keyCount = static_cast<std::uint32_t>(input.keys.size()); std::copy(input.keys.begin(), input.keys.end(), packet.keys);
    const auto send = [&]() -> DWORD_PTR {
        DWORD actualPid = 0;
        if (!owned() || GetWindowThreadProcessId(window, &actualPid) != thread || actualPid != id
            || GetPropW(window, kWindowInputProperty) != reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(kWindowInputMagic))) return 0;
        COPYDATASTRUCT copy{ kWindowInputMagic, sizeof(packet), &packet }; DWORD_PTR result = 0;
        return SendMessageTimeoutW(window, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&copy), SMTO_ABORTIFHUNG | SMTO_BLOCK | SMTO_ERRORONEXIT, 1000, &result) ? result : 0;
    };
    auto result = send(); const auto start = GetTickCount64();
    while ((result & kWindowInputStateMask) == static_cast<unsigned>(WindowInputState::Pending) && GetTickCount64() - start < 3000) {
        Sleep(20); packet.kind = WindowInputKind::Query; result = send();
    }
    if ((result & kWindowInputStateMask) != static_cast<unsigned>(WindowInputState::Complete))
        return fail("MAP_TEST_INPUT_DELIVERY", "本次游戏未确认测试输入完成（state=" + std::to_string(result & kWindowInputStateMask)
            + "，pid=" + std::to_string(id) + "，hwnd=" + std::to_string(reinterpret_cast<std::uintptr_t>(window)) + "）；没有前台输入后备路径。");
    MapTestInputReceipt receipt; receipt.processId = id; receipt.sequence = packet.sequence; receipt.window = reinterpret_cast<std::uintptr_t>(window);
    receipt.width = client.right; receipt.height = client.bottom; receipt.pixelX = packet.x; receipt.pixelY = packet.y;
    receipt.cursorObserved = result & kWindowInputCursorRead; receipt.keyObserved = result & kWindowInputKeyRead;
    receipt.method = input.kind == MapTestInputKind::Close ? "owned_window_wm_close_posted" : "owned_window_messages_and_virtual_user32";
    return Result<MapTestInputReceipt>::Success(std::move(receipt));
#else
    return fail("MAP_TEST_INPUT_UNSUPPORTED", "当前平台不支持测试输入。");
#endif
}

std::vector<MapTestProcessExit> MapTestLaunchLease::ProcessExits() const {
    std::vector<MapTestProcessExit> result;
#ifdef _WIN32
    if (!impl_) return result;
    std::lock_guard stateLock(impl_->lifecycle); std::lock_guard lock(impl_->mutex);
    result.push_back({impl_->launcherId.load(), impl_->exitCode, true, impl_->launcherGameIdentity, impl_->finished});
    for (const auto& child : impl_->children) {
        DWORD code = child.exitCode;
        const bool known = child.handle ? WaitForSingleObject(child.handle, 0) == WAIT_OBJECT_0 && GetExitCodeProcess(child.handle, &code) : child.exitKnown;
        result.push_back({child.id, code, false, child.gameIdentity, known});
    }
#endif
    return result;
}

Result<std::filesystem::path> MapTestLaunchLease::RequestGameScreenshot(std::uint32_t timeoutMs) const {
    const auto fail = [](const char* code, std::string message) {
        return Result<std::filesystem::path>::Failure("Map.Test", code, std::move(message));
    };
#ifdef _WIN32
    if (!impl_) return fail("MAP_TEST_SCREENSHOT_ENDED", "测试启动记录无效。");
    HANDLE process = nullptr;
    {
        std::lock_guard stateLock(impl_->lifecycle);
        if (impl_->finished || !impl_->job) return fail("MAP_TEST_SCREENSHOT_ENDED", "本次测试已经结束。");
        // 经启动器时游戏是 Job 里捕获的子进程；直接启动时游戏就是原入口进程。
        DWORD id = 0; FILETIME created{};
        {
            std::lock_guard lock(impl_->mutex);
            for (const auto& child : impl_->children) {
                if (!child.gameIdentity || !child.handle || WaitForSingleObject(child.handle, 0) != WAIT_TIMEOUT) continue;
                if (id) return fail("MAP_TEST_SCREENSHOT_AMBIGUOUS", "本次测试有多个游戏进程，无法确定截哪一个。");
                id = child.id; created = child.created;
            }
        }
        if (!id && impl_->launcher && WaitForSingleObject(impl_->launcher, 0) == WAIT_TIMEOUT
            && Impl::ImageMatches(impl_->launcher, impl_->gameImage)) {
            id = impl_->launcherId.load(); created = impl_->launcherCreated;
        }
        if (!id) return fail("MAP_TEST_SCREENSHOT_NO_GAME", "本次测试的游戏进程还没启动或已经退出。");
        // 按编号重开后核对创建时间、Job 归属和映像路径，不会截到复用编号的其他进程。
        process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION | SYNCHRONIZE, FALSE, id);
        FILETIME actual{}, exited{}, kernel{}, user{}; BOOL belongs = FALSE;
        if (!process || !GetProcessTimes(process, &actual, &exited, &kernel, &user) || CompareFileTime(&actual, &created) != 0
            || !IsProcessInJob(process, impl_->job, &belongs) || !belongs || !Impl::ImageMatches(process, impl_->gameImage)) {
            if (process) CloseHandle(process);
            return fail("MAP_TEST_SCREENSHOT_NO_GAME", "无法确认本次测试的游戏进程身份。");
        }
    }
    struct Owned { HANDLE value; ~Owned() { CloseHandle(value); } } owned{process};
    return detail::RequestGame127Screenshot(process, impl_->gameImage.parent_path(), timeoutMs);
#else
    (void)timeoutMs;
    return fail("MAP_TEST_SCREENSHOT_UNSUPPORTED", "当前平台不支持游戏截图。");
#endif
}

Result<Services::RuntimeTestSnapshot> MapTestLaunchLease::QueryRuntimeEvidence() const {
#ifdef _WIN32
    if (impl_ && impl_->runtimeEvidence) return impl_->runtimeEvidence->Query();
#endif
    return Result<Services::RuntimeTestSnapshot>::Failure("Map.Test", "RUNTIME_TEST_UNAVAILABLE", "本次测试没有运行库证据会话。");
}

Status MapTestLaunchLease::Cleanup() {
#ifdef _WIN32
    if (IsActive()) return Status::Failure("Map.Test", "MAP_TEST_STILL_ACTIVE", "游戏进程树仍在运行或终态未确认，测试副本已保留。");
    std::lock_guard lock(impl_->lifecycle);
    if (impl_->cleaned) return Status::Ok();
    auto result = !impl_->coverageConfirmed
        ? Status::Failure("Map.Test", "MAP_TEST_LIFETIME_UNCONFIRMED", "测试Job已无活动进程，但快速退出的子进程身份未完整捕获。副本保留，可以重新测试。")
        : impl_->mapFiles ? impl_->mapFiles->Cleanup(true) : Status::Ok();
    for (auto handle : impl_->protectedDirectories) CloseHandle(handle);
    impl_->protectedDirectories.clear();
    for (auto handle : impl_->protectedFiles) CloseHandle(handle);
    impl_->protectedFiles.clear();
    impl_->cleaned = true;
    if (impl_->exitCode != 0) {
        std::string detail = "游戏测试进程已退出（代码=" + std::to_string(impl_->exitCode) + "）。";
        if (!impl_->receipt.empty()) {
            const auto sha = Sha256File(impl_->receipt);
            const auto bytes = sha.Succeeded() ? ReadVerifiedFile(impl_->receipt, *sha.value, 256 * 1024)
                : Result<std::vector<std::byte>>{};
            if (bytes.Succeeded()) {
                const auto report = nlohmann::json::parse(reinterpret_cast<const char*>(bytes.value->data()),
                    reinterpret_cast<const char*>(bytes.value->data()) + bytes.value->size(), nullptr, false);
                const auto schema = report.is_object() ? report.find("schema") : report.end();
                if (schema != report.end() && schema->is_string() && *schema == "audastra-warcraft-launch-v1") {
                    const auto stage = report.find("errorStage"), error = report.find("errorWin32");
                    if (stage != report.end() && stage->is_string() && error != report.end() && error->is_number_unsigned())
                        detail += " 独立运行时阶段=" + stage->get<std::string>() + "，Win32=" + std::to_string(error->get<unsigned>()) + "。";
                    const auto owned = report.find("ownedLua");
                    if (owned != report.end() && owned->is_object()) {
                        const auto failure = owned->find("failureText");
                        if (failure != owned->end() && failure->is_string())
                            detail += " " + failure->get<std::string>().substr(0, 8192);
                    }
                }
            }
            detail += " 报告：" + IO::GenericPathUtf8(impl_->receipt);
        }
        result.diagnostics.push_back({DiagnosticSeverity::Error, "Map.Test", "MAP_TEST_PROCESS_EXIT", std::move(detail), std::nullopt});
    }
    return result;
#else
    return Status::Failure("Map.Test", "MAP_TEST_PLATFORM", "独立测试启动仅支持Windows。");
#endif
}

Result<MapTestLaunchReceipt> LaunchMapTest(const Services::MapTestArtifact& artifact,
    const Services::MapTestProgress& progress) {
    // 每次真正发起都核对所选游戏、实际脚本依赖与冻结副本。
    const auto plan = BuildIndependentMapTestLaunchPlan(artifact, progress);
    if (!plan.Succeeded()) { Result<MapTestLaunchReceipt> result; result.diagnostics = plan.diagnostics; return result; }
    if (progress) progress("launch");
    auto launched = detail::LaunchVerifiedMapTestPlan(*plan.value);
    if (launched.Succeeded()) {
        launched.value->luaEntryModules = plan.value->luaEntryModules;
        launched.value->luaHotReload = plan.value->enableLuaHotReload;
    }
    // 运行环境选择等提示随结果一起返回给宿主显示。
    launched.diagnostics.insert(launched.diagnostics.begin(), plan.diagnostics.begin(), plan.diagnostics.end());
    return launched;
}

namespace detail {
struct MapTestLaunchAccess {
static Result<MapTestLaunchReceipt> Run(const Services::MapTestLaunchPlan& plan) {
    if (plan.enableLuaHotReload && !plan.useOwnedLuaRuntime)
        return Result<MapTestLaunchReceipt>::Failure("Map.Test", "MAP_TEST_LUA_RELOAD_RUNTIME", "热重载需要自有 Lua 运行时。");
    if (plan.useOwnedLuaRuntime && (plan.runtimeBootstrap.empty() || plan.runtimeBootstrapSha256.empty()
        || plan.gameExecutableSha256.empty() || plan.gameModuleSha256.empty() || plan.runtimeReceipt.empty()
        || !plan.runtimeRoot.empty()))
        return Result<MapTestLaunchReceipt>::Failure("Map.Test", "MAP_TEST_OWN_LUA_PLAN", "自有 Lua 测试计划缺少完整组件身份或混入外部运行时。");
#ifdef _WIN32
    auto destinationIdentity = IO::GenericPathUtf8(plan.workingDirectory.lexically_normal());
    for (auto& c : destinationIdentity) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    const auto digest = Sha256Text(destinationIdentity);
    if (!digest.Succeeded()) { Result<MapTestLaunchReceipt> result; result.diagnostics = digest.diagnostics; return result; }
    std::wstring suffix(digest.value->begin(), digest.value->end());
    // 同一游戏安装共用命名Job。已捕获进程各持有Job句柄，宿主关闭不杀它们；
    // 编辑器退出后不能声称继续观察新后代或证明未知树的终态。
    HANDLE startMutex = CreateMutexW(nullptr, FALSE, (L"Local\\ForgeMapTestStart_" + suffix).c_str());
    if (!startMutex) return Result<MapTestLaunchReceipt>::Failure("Map.Test", "MAP_TEST_LAUNCH_MUTEX", "无法建立测试启动互斥，测试尚未启动。");
    const auto mutexWait = WaitForSingleObject(startMutex, 0);
    if (mutexWait != WAIT_OBJECT_0 && mutexWait != WAIT_ABANDONED) { CloseHandle(startMutex);
        return Result<MapTestLaunchReceipt>::Failure("Map.Test", "MAP_TEST_ALREADY_ACTIVE", "另一个编辑器正在准备同一游戏安装的测试。"); }
    struct MutexScope { HANDLE handle; ~MutexScope() { ReleaseMutex(handle); CloseHandle(handle); } } startGuard{startMutex};
    auto state = std::make_unique<MapTestLaunchLease::Impl>();
    state->gameImage = (plan.workingDirectory / L"war3.exe").lexically_normal().make_preferred();
    state->job = CreateJobObjectW(nullptr, (L"Local\\ForgeMapTestGame_" + suffix).c_str());
    if (!state->job) return Result<MapTestLaunchReceipt>::Failure("Map.Test", "MAP_TEST_LAUNCH_JOB", "无法建立游戏测试生命周期Job。");
    if (GetLastError() == ERROR_ALREADY_EXISTS)
        return Result<MapTestLaunchReceipt>::Failure("Map.Test", "MAP_TEST_ALREADY_ACTIVE", "同一游戏安装仍有测试进程，或其终态尚未确认。");
    // 持有所选安装的原目录身份。只在Maps/Audastra下创建本轮短路径副本。
    const auto relative = plan.gameMapCopy.parent_path().lexically_relative(plan.workingDirectory);
    if (relative.empty() || relative.is_absolute() || relative.begin() == relative.end())
        return Result<MapTestLaunchReceipt>::Failure("Map.Test", "MAP_TEST_COPY_DIRECTORY", "测试复制目录不属于已验证游戏目录。");
    const auto holdDirectory = [&](const std::filesystem::path& path) {
        HANDLE handle = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY,
            FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        BY_HANDLE_FILE_INFORMATION info{};
        if (handle == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(handle, &info)
            || !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); return false;
        }
        try { state->protectedDirectories.push_back(handle); } catch (...) { CloseHandle(handle); throw; }
        return true;
    };
    auto directory = plan.workingDirectory;
    if (!holdDirectory(directory))
        return Result<MapTestLaunchReceipt>::Failure("Map.Test", "MAP_TEST_COPY_DIRECTORY", "游戏目录无法取得原目录租约，测试尚未启动。");
    std::size_t components = 0;
    for (const auto& component : relative) {
        auto name = component.wstring(); for (auto& c : name) if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
        if (++components > 3 || name == L".." || name == L"." || (components == 1 && name != L"maps")
            || (components == 2 && name != L"audastra"))
            return Result<MapTestLaunchReceipt>::Failure("Map.Test", "MAP_TEST_COPY_DIRECTORY", "测试复制目录不属于安全的Maps子目录。");
        directory /= component;
        if (!CreateDirectoryW(directory.c_str(), nullptr) && (components == 3 || GetLastError() != ERROR_ALREADY_EXISTS))
            return Result<MapTestLaunchReceipt>::Failure("Map.Test", "MAP_TEST_COPY_DIRECTORY", "测试复制父目录无法创建，测试尚未启动。");
        if (components < 3 && !holdDirectory(directory))
            return Result<MapTestLaunchReceipt>::Failure("Map.Test", "MAP_TEST_COPY_DIRECTORY", "测试复制父目录含重解析点或身份不可确认，测试尚未启动。");
    }
    if (components != 3 || plan.gameMapCopy.filename() != L"test.w3x" && plan.gameMapCopy.filename() != L"test.w3m")
        return Result<MapTestLaunchReceipt>::Failure("Map.Test", "MAP_TEST_COPY_DIRECTORY", "测试副本路径不是本次独占的Maps/Audastra任务。");
    const auto frozen = Services::FreezeMapTestWorkspace(directory);
    if (!frozen.Succeeded()) { Result<MapTestLaunchReceipt> result; result.diagnostics = frozen.diagnostics; return result; }
    state->mapFiles = *frozen.value;
    // CREATE_NEW等价的CopyFile(TRUE)拒绝覆盖；复制后冻结原文件身份并核对SHA。
    if (!CopyFileW(plan.testMap.c_str(), plan.gameMapCopy.c_str(), TRUE))
        return Result<MapTestLaunchReceipt>::Failure("Map.Test", "MAP_TEST_SHORT_COPY", "短路径测试副本复制失败，原图保持不变。");
    const auto captured = state->mapFiles->Refresh();
    if (!captured.Succeeded()) { Result<MapTestLaunchReceipt> result; result.diagnostics = captured.diagnostics; return result; }
    struct UnstartedCopy {
        std::shared_ptr<Services::MapTestWorkspaceLease> files;
        bool safe = true;
        ~UnstartedCopy() { if (safe) { try { (void)files->Cleanup(true); } catch (...) {} } }
    } copyGuard{state->mapFiles};
    auto copy = ReadVerifiedFile(plan.gameMapCopy, plan.testMapSha256, 1024ull * 1024 * 1024);
    if (!copy.Succeeded()) { Result<MapTestLaunchReceipt> result; result.diagnostics = copy.diagnostics; return result; }
    copy.value.reset();
    // 启动组件/Game.dll在预检与创建进程之间不可被替换或改写。
    struct Files { std::vector<HANDLE> handles; ~Files() { for (auto h : handles) CloseHandle(h); } } files;
    const auto verifyExecutable = [&](const std::filesystem::path& path, const std::string& expected) {
        HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        BY_HANDLE_FILE_INFORMATION info{};
        if (file == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(file, &info)
            || info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT) || info.nNumberOfLinks != 1) {
            if (file != INVALID_HANDLE_VALUE) CloseHandle(file); return false;
        }
        const auto sha = Sha256Handle(file);
        if (!sha.Succeeded() || *sha.value != expected) { CloseHandle(file); return false; }
        files.handles.push_back(file); return true;
    };
    if (!verifyExecutable(plan.executable, plan.executableSha256)
        || (!plan.gameExecutableSha256.empty() && !verifyExecutable(plan.workingDirectory / L"war3.exe", plan.gameExecutableSha256))
        || (!plan.gameModuleSha256.empty() && !verifyExecutable(plan.workingDirectory / L"Game.dll", plan.gameModuleSha256))
        || (!plan.runtimeBootstrap.empty() && !verifyExecutable(plan.runtimeBootstrap, plan.runtimeBootstrapSha256)))
        return Result<MapTestLaunchReceipt>::Failure("Map.Test", "MAP_TEST_EXECUTABLE_CHANGED", "测试启动文件在预检后发生变化，测试已停止。");
    if (!plan.runtimeReceipt.empty()) {
        const auto parent = plan.runtimeReceipt.parent_path();
        if ((!CreateDirectoryW(parent.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) || !holdDirectory(parent))
            return Result<MapTestLaunchReceipt>::Failure("Map.Test", "MAP_TEST_RUNTIME_RECEIPT", "无法保护独立运行时的回执目录。");
        state->receipt = plan.runtimeReceipt;
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{}; // 明确没有KILL_ON_JOB_CLOSE或breakaway。
    if (!SetInformationJobObject(state->job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
        return Result<MapTestLaunchReceipt>::Failure("Map.Test", "MAP_TEST_LAUNCH_JOB", "无法配置可继续运行的游戏测试Job，测试尚未启动。");
    state->completion = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1);
    JOBOBJECT_ASSOCIATE_COMPLETION_PORT association{state.get(), state->completion};
    if (!state->completion || !SetInformationJobObject(state->job, JobObjectAssociateCompletionPortInformation, &association, sizeof(association)))
        return Result<MapTestLaunchReceipt>::Failure("Map.Test", "MAP_TEST_LAUNCH_JOB", "无法建立Job进程生命周期回执，测试尚未启动。");
    state->monitor = std::thread([raw = state.get()] { raw->Observe(); });
    // 所有拥有者分配在CreateProcess之前完成，内存不足不能留下已恢复却无lease的游戏。
    state->cloaked = plan.cloakedWindow;
    auto* rawState = state.get();
    auto lease = std::shared_ptr<MapTestLaunchLease>(new MapTestLaunchLease(std::move(state)));
    const std::weak_ptr<MapTestLaunchLease> weakLease = lease;
    auto evidence = Services::RuntimeTestSession::Create(plan.testMapSha256, [weakLease](std::uint32_t id, std::uint64_t created) {
        const auto held = weakLease.lock();
        if (!held) return false;
        auto& state = *held->impl_;
        std::lock_guard stateLock(state.lifecycle); std::lock_guard childLock(state.mutex);
        const auto sameTime = [created](const FILETIME& time) {
            return ((static_cast<std::uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime) == created;
        };
        if (id == state.launcherId.load() && state.launcherGameIdentity && sameTime(state.launcherCreated)) return true;
        const MapTestLaunchLease::Impl::Child* found = nullptr;
        for (const auto& child : state.children) if (child.id == id) { if (found) return false; found = &child; }
        return found && found->gameIdentity && sameTime(found->created);
    });
    if (!evidence.Succeeded()) return {std::nullopt, evidence.diagnostics};
    rawState->runtimeEvidence = *evidence.value;
    // 环境只交给本次创建的进程树，不修改编辑器/作者进程环境；清除上游残留会话。
    std::vector<wchar_t> environment;
    std::vector<std::wstring> environmentRows;
    if (const auto inherited = GetEnvironmentStringsW()) {
        for (const wchar_t* row = inherited; *row; row += wcslen(row) + 1) {
            const auto keyLength = wcslen(Audastra::Runtime32::kRuntimeTestEnvironment);
            if (_wcsnicmp(row, Audastra::Runtime32::kRuntimeTestEnvironment, keyLength) == 0 && row[keyLength] == L'=') continue;
            environmentRows.emplace_back(row);
        }
        FreeEnvironmentStringsW(inherited);
    } else return Result<MapTestLaunchReceipt>::Failure("Map.Test", "RUNTIME_TEST_ENVIRONMENT", "无法冻结本次测试的子进程环境。");
    const auto& nonce = rawState->runtimeEvidence->Nonce();
    const auto entry = std::wstring(Audastra::Runtime32::kRuntimeTestEnvironment) + L"=" + std::wstring(nonce.begin(), nonce.end());
    environmentRows.push_back(entry);
    std::sort(environmentRows.begin(), environmentRows.end(), [](const auto& a, const auto& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
    for (const auto& row : environmentRows) { environment.insert(environment.end(), row.begin(), row.end()); environment.push_back(0); }
    environment.push_back(0);
    std::shared_ptr<Services::LuaTestMailboxSession> mailbox;
    std::unique_ptr<Services::detail::LuaTestMailboxLaunchHandle> inheritedMailbox;
    if (plan.useOwnedLuaRuntime) {
        auto createdMailbox = Services::CreateLuaTestMailboxSession(plan.testMapSha256);
        if (!createdMailbox.Succeeded()) return {std::nullopt, createdMailbox.diagnostics};
        mailbox = *createdMailbox.value;
        auto preparedMailbox = Services::detail::LuaTestMailboxLaunchAccess::Prepare(mailbox,
            [lease](std::uint32_t id) { return lease->impl_->Ownership(id); }, plan.enableLuaHotReload);
        if (!preparedMailbox.Succeeded()) return {std::nullopt, preparedMailbox.diagnostics};
        inheritedMailbox = std::move(*preparedMailbox.value);
    }
    std::wstring command = QuoteArgument(plan.executable.wstring());
    for (const auto& argument : plan.arguments) { command += L' '; command += QuoteArgument(argument); }
    if (inheritedMailbox) {
        command += L" --lua-mailbox-handle " + std::to_wstring(inheritedMailbox->Value());
        const auto luaNonce = inheritedMailbox->Nonce();
        command += L" --lua-session-nonce " + std::wstring(luaNonce.begin(), luaNonce.end());
    }
    struct Attributes {
        std::vector<std::byte> storage;
        LPPROC_THREAD_ATTRIBUTE_LIST value = nullptr;
        DWORD policy = PROCESS_CREATION_DESKTOP_APP_BREAKAWAY_ENABLE_PROCESS_TREE;
        HANDLE mailbox = nullptr;
        ~Attributes() { if (value) DeleteProcThreadAttributeList(value); }
        bool Initialize(HANDLE inherited) {
            UINT32 length = 0;
            const auto status = GetCurrentPackageFullName(&length, nullptr);
            const bool packaged = status == ERROR_INSUFFICIENT_BUFFER;
            if (status != APPMODEL_ERROR_NO_PACKAGE && !packaged) return false;
            const DWORD count = static_cast<DWORD>(packaged) + static_cast<DWORD>(inherited != nullptr);
            if (!count) return true;
            SIZE_T bytes = 0;
            InitializeProcThreadAttributeList(nullptr, count, 0, &bytes);
            if (!bytes) return false;
            storage.resize(bytes);
            auto* list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
            if (!InitializeProcThreadAttributeList(list, count, 0, &bytes)) return false;
            value = list;
            if (packaged && !UpdateProcThreadAttribute(value, 0, PROC_THREAD_ATTRIBUTE_DESKTOP_APP_POLICY,
                &policy, sizeof(policy), nullptr, nullptr)) return false;
            mailbox = inherited;
            return !mailbox || UpdateProcThreadAttribute(value, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                &mailbox, sizeof(mailbox), nullptr, nullptr) != FALSE;
        }
    } attributes;
    if (!attributes.Initialize(inheritedMailbox ? reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(inheritedMailbox->Value())) : nullptr))
        return Result<MapTestLaunchReceipt>::Failure("Map.Test", "MAP_TEST_LAUNCH_ATTRIBUTES", "无法为游戏进程设置独立桌面应用的启动语义。");
    // 后台测试：新建一个看不见的桌面，启动入口和它启动的游戏都在那里，不切换用户桌面、不抢前台、不收键鼠。
    // 建不成时直接失败，不退回用户桌面。
    std::wstring desktopPath;
    if (plan.hiddenDesktop) {
        std::array<wchar_t, 64> station{}; DWORD needed = 0;
        if (!GetUserObjectInformationW(GetProcessWindowStation(), UOI_NAME, station.data(), static_cast<DWORD>(sizeof(station)), &needed)
            || CompareStringOrdinal(station.data(), -1, L"WinSta0", -1, TRUE) != CSTR_EQUAL)
            return Result<MapTestLaunchReceipt>::Failure("Map.Test", "MAP_TEST_HIDDEN_DESKTOP", "当前会话不能建立后台测试桌面，测试尚未启动。");
        const auto name = L"AudastraMapTest_" + suffix.substr(0, 12) + L"_" + std::to_wstring(GetTickCount64());
        // CreateDesktopW 放在短命线程里调用，界面线程所属的桌面不变。
        DWORD desktopError = 0;
        std::thread([&] {
            SetLastError(0);
            rawState->desktop = CreateDesktopW(name.c_str(), nullptr, nullptr, 0,
                DESKTOP_READOBJECTS | DESKTOP_CREATEWINDOW | DESKTOP_ENUMERATE | DESKTOP_WRITEOBJECTS, nullptr);
            desktopError = GetLastError();
        }).join();
        if (!rawState->desktop || desktopError == ERROR_ALREADY_EXISTS)
            return Result<MapTestLaunchReceipt>::Failure("Map.Test", "MAP_TEST_HIDDEN_DESKTOP",
                "无法建立后台测试桌面（Win32=" + std::to_string(desktopError) + "），测试尚未启动。");
        desktopPath = std::wstring(station.data()) + L"\\" + name;
    }
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = attributes.value ? sizeof(startup) : sizeof(STARTUPINFOW);
    startup.lpAttributeList = attributes.value;
    if (!desktopPath.empty()) startup.StartupInfo.lpDesktop = desktopPath.data();
    PROCESS_INFORMATION process{};
    BOOL created = FALSE;
    DWORD creationError = 0;
    {
        // 子进程继承进程错误模式；仅覆盖CreateProcess期间，随后恢复本程序原值。
        static std::mutex creationMutex;
        std::lock_guard lock(creationMutex);
        const auto previous = SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
        created = CreateProcessW(plan.executable.c_str(), command.data(), nullptr, nullptr, inheritedMailbox ? TRUE : FALSE,
            CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED | (plan.useOwnedLuaRuntime ? DETACHED_PROCESS : CREATE_NO_WINDOW)
                | (attributes.value ? EXTENDED_STARTUPINFO_PRESENT : 0),
            environment.data(), plan.workingDirectory.c_str(), &startup.StartupInfo, &process);
        creationError = GetLastError();
        SetErrorMode(previous);
    }
    if (!created)
        return Result<MapTestLaunchReceipt>::Failure("Map.Test", "MAP_TEST_LAUNCH_FAILED",
            "独立游戏测试入口启动失败（Win32=" + std::to_string(creationError) + "）。");
    copyGuard.safe = false;
    rawState->launcher = process.hProcess;
    rawState->launcherId.store(process.dwProcessId);
    const auto abort = [&](const char* code, const char* message) {
        // 仅终止本次创建且尚未恢复的原launcher句柄，不枚举或触碰作者游戏。
        const auto error = GetLastError();
        const bool stopped = TerminateProcess(process.hProcess, 1) != FALSE
            && WaitForSingleObject(process.hProcess, 5000) == WAIT_OBJECT_0;
        copyGuard.safe = stopped;
        CloseHandle(process.hThread);
        return Result<MapTestLaunchReceipt>::Failure("Map.Test", code, std::string(message) + "（Win32=" + std::to_string(error)
            + (stopped ? "；原launcher已确认终态）。" : "；原launcher终态无法确认，保留本次测试文件）。"));
    };
    FILETIME launcherCreated{}, launcherExited{}, launcherKernel{}, launcherUser{};
    if (!GetProcessTimes(process.hProcess, &launcherCreated, &launcherExited, &launcherKernel, &launcherUser))
        return abort("MAP_TEST_LAUNCH_IDENTITY", "无法捕获原启动器创建身份，已停止尚未恢复的启动器");
    {
        std::lock_guard lock(rawState->mutex);
        rawState->launcherCreated = launcherCreated;
    }
    if (!AssignProcessToJobObject(rawState->job, process.hProcess))
        return abort("MAP_TEST_LAUNCH_JOB_ASSIGN", "无法证明本次独立启动器的Job归属，已停止尚未恢复的启动器");
    BOOL inJob = FALSE;
    if (!IsProcessInJob(process.hProcess, rawState->job, &inJob) || !inJob)
        return abort("MAP_TEST_LAUNCH_JOB_ASSIGN", "独立启动器的Job归属复核失败，已停止尚未恢复的启动器");
    rawState->launcherGameIdentity = MapTestLaunchLease::Impl::ImageMatches(process.hProcess, rawState->gameImage);
    // 使用CreateProcess的原launcher句柄，绝不按PID重开过去身份。
    HANDLE launcherJobKeeper = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), rawState->job, process.hProcess, &launcherJobKeeper, 0, FALSE, DUPLICATE_SAME_ACCESS))
        return abort("MAP_TEST_LAUNCH_JOB_KEEPER", "无法向本次原启动器交付Job持有句柄，已停止尚未恢复的启动器");
    if (ResumeThread(process.hThread) == static_cast<DWORD>(-1))
        return abort("MAP_TEST_LAUNCH_RESUME", "独立启动器主线程无法恢复，已停止本次启动器");
    CloseHandle(process.hThread);
    rawState->protectedFiles = std::move(files.handles);
    MapTestLaunchReceipt receipt{process.dwProcessId, std::move(lease)};
    receipt.luaMailbox = std::move(mailbox);
    return Result<MapTestLaunchReceipt>::Success(std::move(receipt));
#else
    return Result<MapTestLaunchReceipt>::Failure("Map.Test", "MAP_TEST_PLATFORM", "独立测试启动仅支持Windows。");
#endif
}
};
Result<MapTestLaunchReceipt> LaunchVerifiedMapTestPlan(const Services::MapTestLaunchPlan& plan) {
    return MapTestLaunchAccess::Run(plan);
}
} // namespace detail
} // namespace War3Map::Script
