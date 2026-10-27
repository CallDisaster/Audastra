#include "RuntimeProtocol.h"
#include "RuntimeFiles.h"
#include "War3MapCore/Runtime/LuaTestMailboxValidation.h"
#include <detours.h>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <memory>
#include <stdexcept>

using namespace Audastra::Runtime32;
namespace {
std::wstring Quote(std::wstring_view value) {
    std::wstring result = L"\"";
    std::size_t slashes = 0;
    for (const auto character : value) {
        if (character == L'\\') { ++slashes; continue; }
        if (character == L'\"') result.append(slashes * 2 + 1, L'\\');
        else result.append(slashes, L'\\');
        slashes = 0;
        result += character;
    }
    result.append(slashes * 2, L'\\');
    return result + L'\"';
}
std::string Utf8(std::wstring_view text) {
    const auto size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (!size && !text.empty()) throw std::runtime_error("invalid UTF-16 path");
    std::string result(size, '\0');
    if (size) WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), size, nullptr, nullptr);
    return result;
}
std::string Json(std::string_view value) {
    std::string result = "\"";
    constexpr char alphabet[] = "0123456789abcdef";
    for (const auto character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte == '"' || byte == '\\') { result += '\\'; result += character; }
        else if (byte < 32) { result += "\\u00"; result += alphabet[byte >> 4]; result += alphabet[byte & 15]; }
        else result += character;
    }
    return result + '"';
}
bool SafeMap(std::wstring_view text) {
    if (text.empty() || text.size() >= 54) return false;
    for (const auto c : text) if (c < 32 || c > 126 || c == L':' || c == L'"' || c == L'<' || c == L'>' || c == L'|' || c == L'?' || c == L'*') return false;
    const auto path = std::filesystem::path(text);
    if (path.is_absolute() || path.has_root_name() || path.has_root_directory()) return false;
    const auto extension = path.extension().wstring();
    if (extension != L".w3x" && extension != L".w3m") return false;
    unsigned parts = 0;
    for (const auto& part : path) {
        auto name = part.wstring();
        if (name.empty() || name == L"." || name == L".." || name.back() == L'.' || name.back() == L' ') return false;
        if (++parts == 1 && !EqualPath(name, L"Maps")) return false;
    }
    return parts >= 3;
}
std::filesystem::path CanonicalBlockedPath(const std::filesystem::path& path) {
    // 仅只读规范化。slash 和存在父链的 8.3 拼写必须在创建游戏前去重。
    auto text = path.wstring();
    std::replace(text.begin(), text.end(), L'/', L'\\');
    std::array<wchar_t, kPathCapacity> full{};
    const auto count = GetFullPathNameW(text.c_str(), static_cast<DWORD>(full.size()), full.data(), nullptr);
    if (!count || count >= full.size()) return {};
    auto existing = std::filesystem::path(std::wstring(full.data(), count));
    std::vector<std::filesystem::path> suffix;
    while (!existing.empty()) {
        std::array<wchar_t, kPathCapacity> expanded{};
        const auto length = GetLongPathNameW(existing.c_str(), expanded.data(), static_cast<DWORD>(expanded.size()));
        if (length && length < expanded.size()) {
            auto canonical = std::filesystem::path(std::wstring(expanded.data(), length));
            for (auto part = suffix.rbegin(); part != suffix.rend(); ++part) canonical /= *part;
            // 与 guard 的 FullPath/Lower 和 raw 拒写 ledger 使用同一大小写形态。
            auto normalized = canonical.wstring();
            for (auto& character : normalized) character = static_cast<wchar_t>(towlower(character));
            canonical = std::filesystem::path(std::move(normalized));
            return PlainPath(canonical) ? canonical : std::filesystem::path{};
        }
        const auto attributes = GetFileAttributesW(existing.c_str());
        const auto error = GetLastError();
        if (attributes != INVALID_FILE_ATTRIBUTES || (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)) return {};
        const auto parent = existing.parent_path();
        if (parent == existing) return {};
        suffix.push_back(existing.filename()); existing = parent;
    }
    return {};
}
std::string ImportPath(const std::filesystem::path& path) {
    auto text = path.wstring();
    const auto convert = [](std::wstring_view value) {
        BOOL usedDefault = FALSE;
        const bool utf8CodePage = GetACP() == CP_UTF8;
        const auto flags = utf8CodePage ? WC_ERR_INVALID_CHARS : WC_NO_BEST_FIT_CHARS;
        auto* fallback = utf8CodePage ? nullptr : &usedDefault;
        const auto size = WideCharToMultiByte(CP_ACP, flags, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, fallback);
        if (!size || usedDefault) return std::string{};
        std::string result(size, '\0');
        WideCharToMultiByte(CP_ACP, flags, value.data(), static_cast<int>(value.size()), result.data(), size, nullptr, fallback);
        return usedDefault ? std::string{} : result;
    };
    auto result = convert(text);
    if (!result.empty()) return result;
    std::array<wchar_t, kPathCapacity> shortPath{};
    const auto length = GetShortPathNameW(path.c_str(), shortPath.data(), static_cast<DWORD>(shortPath.size()));
    if (length && length < shortPath.size()) result = convert(std::wstring_view(shortPath.data(), length));
    return result;
}
std::vector<wchar_t> ChildEnvironment(HANDLE channel, const std::filesystem::path& probeTemporaryRoot = {}) {
    auto* strings = GetEnvironmentStringsW();
    if (!strings) throw std::runtime_error("environment read failed");
    std::vector<std::wstring> entries;
    for (auto* entry = strings; *entry; entry += wcslen(entry) + 1) {
        std::wstring value(entry);
        if (_wcsnicmp(value.c_str(), L"AUDASTRA_TEST_", 14) != 0 && _wcsnicmp(value.c_str(), L"ydwe-process-name=", 18) != 0
            && (probeTemporaryRoot.empty() || (_wcsnicmp(value.c_str(), L"TEMP=", 5) != 0 && _wcsnicmp(value.c_str(), L"TMP=", 4) != 0))) entries.push_back(std::move(value));
    }
    FreeEnvironmentStringsW(strings);
    entries.push_back(L"AUDASTRA_TEST_CHANNEL=" + std::to_wstring(reinterpret_cast<ULONG_PTR>(channel)));
    if (!probeTemporaryRoot.empty()) {
        entries.push_back(L"TEMP=" + probeTemporaryRoot.wstring());
        entries.push_back(L"TMP=" + probeTemporaryRoot.wstring());
    }
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
    std::vector<wchar_t> block;
    for (const auto& value : entries) { block.insert(block.end(), value.begin(), value.end()); block.push_back(L'\0'); }
    block.push_back(L'\0');
    return block;
}
struct AttributeList {
    std::vector<std::byte> storage;
    LPPROC_THREAD_ATTRIBUTE_LIST value = nullptr;
    std::array<HANDLE, 2> inheritedChannels{};
    ~AttributeList() { if (value) DeleteProcThreadAttributeList(value); }
    bool Initialize(HANDLE channel, HANDLE luaMailbox = nullptr) {
        SIZE_T bytes = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
        storage.resize(bytes);
        value = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
        if (!InitializeProcThreadAttributeList(value, 1, 0, &bytes)) { value = nullptr; return false; }
        inheritedChannels = {channel, luaMailbox};
        return UpdateProcThreadAttribute(value, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inheritedChannels.data(),
            sizeof(HANDLE) * (luaMailbox ? 2 : 1), nullptr, nullptr) != FALSE;
    }
};
// 仅显式元数据 probe 使用；默认 F9/控制台不注册 class，不创建窗口。
struct ProbeDesktopWitness {
    static constexpr DWORD kStyle = WS_POPUP;
    static constexpr DWORD kExStyle = WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;
    bool requested = false, created = false, destroyed = false, unregistered = false, visible = false;
    HWND window = nullptr;
    HINSTANCE instance = nullptr;
    ATOM atom = 0;
    DWORD processId = 0, threadId = 0, style = 0, exStyle = 0, error = 0;
    ULONG_PTR originalWindow = 0;
    std::wstring nonce, className;
    ProbeDesktopWitness() = default;
    ProbeDesktopWitness(const ProbeDesktopWitness&) = delete;
    ProbeDesktopWitness& operator=(const ProbeDesktopWitness&) = delete;
    ~ProbeDesktopWitness() { Finalize(); } // 异常兜底，成功回执必须先显式 Finalize。
    bool Create(std::wstring_view value) {
        requested = true;
        nonce = value; className = L"AudastraProbeDesktopWitness-" + nonce;
        processId = GetCurrentProcessId(); threadId = GetCurrentThreadId();
        instance = GetModuleHandleW(nullptr);
        WNDCLASSEXW definition{}; definition.cbSize = sizeof(definition);
        definition.lpfnWndProc = DefWindowProcW; definition.hInstance = instance;
        definition.lpszClassName = className.c_str();
        atom = RegisterClassExW(&definition);
        if (!atom) { error = GetLastError(); return false; }
        window = CreateWindowExW(kExStyle, className.c_str(), nullptr, kStyle,
            0, 0, 1, 1, nullptr, nullptr, instance, nullptr);
        if (!window) { error = GetLastError(); return false; }
        created = true; originalWindow = reinterpret_cast<ULONG_PTR>(window);
        SetLastError(0);
        const auto actualStyle = GetWindowLongPtrW(window, GWL_STYLE);
        if (!actualStyle && GetLastError()) { error = GetLastError(); return false; }
        style = static_cast<DWORD>(actualStyle);
        SetLastError(0);
        const auto actualExStyle = GetWindowLongPtrW(window, GWL_EXSTYLE);
        if (!actualExStyle && GetLastError()) { error = GetLastError(); return false; }
        exStyle = static_cast<DWORD>(actualExStyle);
        visible = IsWindowVisible(window) != FALSE;
        DWORD owner = 0;
        const auto creator = GetWindowThreadProcessId(window, &owner);
        if (creator != threadId || owner != processId || visible
            || !(style & WS_POPUP) || (style & (WS_VISIBLE | WS_CHILD)) || exStyle != kExStyle) {
            error = ERROR_INVALID_DATA; return false;
        }
        return true;
    }
    bool Finalize() noexcept {
        if (!requested) return true;
        if (GetCurrentThreadId() != threadId) { error = ERROR_INVALID_THREAD_ID; return false; }
        if (window) {
            if (!DestroyWindow(window)) { error = GetLastError(); return false; }
            window = nullptr; destroyed = true;
        }
        if (atom) {
            if (!UnregisterClassW(className.c_str(), instance)) { error = GetLastError(); return false; }
            atom = 0; unregistered = true;
        }
        return true;
    }
    std::string JsonEvidence() const {
        const auto flag = [](bool value) { return value ? "true" : "false"; };
        std::string result = "{\"requested\":true,\"mode\":\"hidden-window\",\"proofKind\":\"hidden-window\",\"nonce\":" + Json(Utf8(nonce));
        result += ",\"className\":" + Json(Utf8(className)) + ",\"hwnd\":" + std::to_string(originalWindow);
        result += ",\"bridgePid\":" + std::to_string(processId) + ",\"threadId\":" + std::to_string(threadId);
        result += ",\"style\":" + std::to_string(style) + ",\"exStyle\":" + std::to_string(exStyle);
        result += ",\"visible\":" + std::string(flag(visible)) + ",\"created\":" + flag(created) + ",\"destroyed\":" + flag(destroyed);
        result += ",\"unregistered\":" + std::string(flag(unregistered)) + ",\"errorWin32\":" + std::to_string(error) + '}';
        return result;
    }
};
// 只读诊断不授予 Desktop、地图开始或 Lua 捕获证明；借用的 Desktop/进程句柄不在这里关闭。
struct ProbeWindowDiagnostics {
    using CaptionReader = int (WINAPI*)(HWND, LPWSTR, int);
    static constexpr DWORD kCallbackLimit = 64, kOwnedLimit = 32, kDialogChildLimit = 16;
    static constexpr int kTextLimit = 160;
    struct Row {
        ULONG_PTR hwnd = 0, parentHwnd = 0;
        DWORD pid = 0, tid = 0, error = 0, parentPid = 0, parentTid = 0, childCallbacks = 0, childEnumError = 0;
        bool ownerStable = false, classTruncated = false, captionTruncated = false, modalClass = false;
        bool parentStable = false, childEnumerationAttempted = false, childEnumReturned = false;
        std::string className, caption;
        const char* errorApi = "none";
    };
    struct Sample {
        std::array<Row, kOwnedLimit> rows{};
        DWORD callbacks = 0, rowCount = 0, error = 0, enumError = 0, waitBefore = WAIT_FAILED, waitAfter = WAIT_FAILED;
        ULONGLONG elapsedMs = 0;
        bool enumerated = false, complete = false, callbackFailed = false, budgetStopped = false;
        const char* reason = "none";
        const char* errorApi = "none";
    };
    bool requested = false, desktopVerified = false;
    HDESK desktop = nullptr;
    HANDLE game = nullptr;
    DWORD gamePid = 0, desktopError = 0;
    CaptionReader captionReader = nullptr;
    ULONGLONG started = 0;
    std::array<Sample, 2> samples{};
    DWORD sampleCount = 0;
    void Bind(HANDLE originalGame, DWORD originalPid, std::wstring_view expectedDesktop) noexcept {
        game = originalGame; gamePid = originalPid;
        SetLastError(0); desktop = GetThreadDesktop(GetCurrentThreadId()); desktopError = GetLastError();
        std::array<wchar_t, 128> name{}; DWORD needed = 0;
        if (desktop) {
            SetLastError(0);
            const auto queried = GetUserObjectInformationW(desktop, UOI_NAME, name.data(), static_cast<DWORD>(sizeof(name)), &needed);
            desktopError = GetLastError();
            desktopVerified = queried && needed <= sizeof(name) && std::wstring_view(name.data()) == expectedDesktop;
            if (!desktopVerified && !desktopError) desktopError = ERROR_INVALID_DATA;
        }
        const auto user = GetModuleHandleW(L"user32.dll");
        if (user) captionReader = reinterpret_cast<CaptionReader>(GetProcAddress(user, "InternalGetWindowText"));
    }
    struct Enumeration {
        ProbeWindowDiagnostics* owner;
        Sample* sample;
        HWND dialog = nullptr;
        DWORD dialogTid = 0, childCallbacks = 0;
    };
    static BOOL Failure(Sample& sample, const char* api, DWORD error) noexcept {
        sample.callbackFailed = true; sample.errorApi = api; sample.error = error ? error : ERROR_INVALID_DATA;
        SetLastError(sample.error); return FALSE;
    }
    static BOOL BudgetStop(Sample& sample) noexcept {
        sample.budgetStopped = true; sample.error = ERROR_MORE_DATA; SetLastError(ERROR_MORE_DATA); return FALSE;
    }
    static bool DialogStillMatches(const Enumeration& context, HWND child = nullptr) noexcept {
        auto& self = *context.owner; auto& sample = *context.sample;
        SetLastError(0); const auto alive = WaitForSingleObject(self.game, 0); const auto aliveError = GetLastError();
        if (alive != WAIT_TIMEOUT) return Failure(sample, "original-game-not-alive-dialog", aliveError) != FALSE;
        DWORD pid = 0;
        SetLastError(0); const auto tid = GetWindowThreadProcessId(context.dialog, &pid); const auto ownerError = GetLastError();
        if (pid != self.gamePid || tid != context.dialogTid) return Failure(sample, "dialog-owner-changed", ownerError) != FALSE;
        std::array<wchar_t, kTextLimit + 1> name{};
        SetLastError(0); const auto count = GetClassNameW(context.dialog, name.data(), static_cast<int>(name.size()));
        const auto classError = GetLastError();
        if (count != 6 || std::wstring_view(name.data(), count) != L"#32770") return Failure(sample, "dialog-class-changed", classError) != FALSE;
        if (child && !IsChild(context.dialog, child)) return Failure(sample, "dialog-child-relation-changed", ERROR_INVALID_DATA) != FALSE;
        return true;
    }
    static BOOL CALLBACK Collect(HWND hwnd, LPARAM parameter) noexcept {
        auto& context = *reinterpret_cast<Enumeration*>(parameter);
        auto& self = *context.owner; auto& sample = *context.sample;
        if (sample.callbacks >= kCallbackLimit || sample.rowCount >= kOwnedLimit
            || (context.dialog && context.childCallbacks >= kDialogChildLimit)) return BudgetStop(sample);
        ++sample.callbacks;
        if (context.dialog) ++context.childCallbacks;
        const auto failure = [&](const char* api, DWORD error) {
            return Failure(sample, api, error);
        };
        try {
            if (context.dialog && !DialogStillMatches(context, hwnd)) return FALSE;
            DWORD pid = 0;
            SetLastError(0); const auto tid = GetWindowThreadProcessId(hwnd, &pid); const auto ownerError = GetLastError();
            if (!tid) return failure("GetWindowThreadProcessId-before", ownerError);
            if (context.dialog && (pid != self.gamePid || tid != context.dialogTid)) return failure("dialog-child-owner", ERROR_INVALID_DATA);
            if (pid == self.gamePid) {
                std::array<wchar_t, kTextLimit + 1> text{};
                SetLastError(0); const auto classCount = GetClassNameW(hwnd, text.data(), static_cast<int>(text.size()));
                if (!classCount || classCount > kTextLimit) return failure("GetClassNameW", GetLastError());
                const bool selectedChild = !context.dialog || _wcsicmp(text.data(), L"Static") == 0 || _wcsicmp(text.data(), L"Button") == 0;
                if (!selectedChild) {
                    DWORD secondPid = 0;
                    const auto secondTid = GetWindowThreadProcessId(hwnd, &secondPid);
                    if (secondTid != tid || secondPid != pid) return failure("unselected-child-owner-changed", ERROR_INVALID_DATA);
                    if (!DialogStillMatches(context, hwnd)) return FALSE;
                    return sample.callbacks >= kCallbackLimit || context.childCallbacks >= kDialogChildLimit ? BudgetStop(sample) : TRUE;
                }
                auto& row = sample.rows[sample.rowCount++];
                row.hwnd = reinterpret_cast<ULONG_PTR>(hwnd); row.pid = pid; row.tid = tid;
                row.parentHwnd = reinterpret_cast<ULONG_PTR>(context.dialog);
                if (context.dialog) { row.parentPid = self.gamePid; row.parentTid = context.dialogTid; }
                const auto store = [&](int count, std::string& destination, bool& truncated) {
                    truncated = count >= kTextLimit;
                    // 截断不留下单独 UTF-16 高代理项；非法原文本仅标记诊断失败。
                    if (count && text[static_cast<std::size_t>(count - 1)] >= 0xD800 && text[static_cast<std::size_t>(count - 1)] <= 0xDBFF) {
                        --count; truncated = true;
                    }
                    destination = Utf8(std::wstring_view(text.data(), static_cast<std::size_t>(count)));
                };
                row.modalClass = std::wstring_view(text.data(), classCount) == L"#32770";
                store(classCount, row.className, row.classTruncated);
                if (self.captionReader) {
                    text.fill(0); SetLastError(0);
                    const auto captionCount = self.captionReader(hwnd, text.data(), static_cast<int>(text.size()));
                    const auto captionError = GetLastError();
                    if (captionCount < 0 || captionCount > kTextLimit || (!captionCount && captionError)) {
                        row.errorApi = "InternalGetWindowText"; row.error = captionError;
                        return failure("InternalGetWindowText", captionError);
                    }
                    store(captionCount, row.caption, row.captionTruncated);
                } else { row.errorApi = "InternalGetWindowText-unavailable"; row.error = ERROR_PROC_NOT_FOUND; }
                DWORD secondPid = 0;
                SetLastError(0); const auto secondTid = GetWindowThreadProcessId(hwnd, &secondPid); const auto secondError = GetLastError();
                row.ownerStable = secondTid == tid && secondPid == pid;
                if (!row.ownerStable) {
                    row.errorApi = "GetWindowThreadProcessId-after"; row.error = secondError;
                    return failure("GetWindowThreadProcessId-after", secondError);
                }
                if (context.dialog) {
                    if (!DialogStillMatches(context, hwnd)) return FALSE;
                    // 子控件 class 和实际所属父链复核后才标记稳定；不读取其它类的正文。
                    std::array<wchar_t, kTextLimit + 1> repeated{};
                    SetLastError(0); const auto repeatedCount = GetClassNameW(hwnd, repeated.data(), static_cast<int>(repeated.size()));
                    if (repeatedCount <= 0 || repeatedCount > kTextLimit || Utf8(std::wstring_view(repeated.data(), repeatedCount)) != row.className)
                        return failure("dialog-child-class-changed", GetLastError());
                    row.parentStable = true;
                } else if (row.modalClass) {
                    if (sample.callbacks >= kCallbackLimit || sample.rowCount >= kOwnedLimit) return BudgetStop(sample);
                    Enumeration children{&self, &sample, hwnd, tid};
                    if (!DialogStillMatches(children)) return FALSE;
                    row.childEnumerationAttempted = true;
                    SetLastError(0);
                    const auto returned = EnumChildWindows(hwnd, Collect, reinterpret_cast<LPARAM>(&children));
                    const auto childError = GetLastError();
                    row.childEnumReturned = returned != FALSE; row.childEnumError = childError; row.childCallbacks = children.childCallbacks;
                    // 官方声明此 BOOL 返回值不用于判定结果，仅原样记录；不由它授予 child 完整证明。
                    if (sample.callbackFailed || sample.budgetStopped) return FALSE;
                    if (childError) return failure("EnumChildWindows-diagnostic", childError);
                    if (!DialogStillMatches(children)) return FALSE;
                }
            }
            if (sample.callbacks >= kCallbackLimit || sample.rowCount >= kOwnedLimit
                || (context.dialog && context.childCallbacks >= kDialogChildLimit)) return BudgetStop(sample);
            return TRUE;
        } catch (...) { return failure("diagnostic-text-or-allocation", ERROR_NO_UNICODE_TRANSLATION); }
    }
    void Capture(const char* reason) noexcept {
        if (!requested || sampleCount >= samples.size()) return;
        auto& sample = samples[sampleCount++];
        try {
            sample.reason = reason; sample.elapsedMs = started ? GetTickCount64() - started : 0;
            SetLastError(0); sample.waitBefore = WaitForSingleObject(game, 0); const auto waitError = GetLastError();
            if (sample.waitBefore != WAIT_TIMEOUT) {
                sample.errorApi = "original-game-not-alive-before"; sample.error = waitError; return;
            }
            SetLastError(0);
            if (GetProcessId(game) != gamePid) { sample.errorApi = "original-game-pid"; sample.error = GetLastError(); return; }
            if (!desktopVerified) { sample.errorApi = "borrowed-desktop-name"; sample.error = desktopError; return; }
            Enumeration context{this, &sample};
            SetLastError(0); const auto enumerated = EnumDesktopWindows(desktop, Collect, reinterpret_cast<LPARAM>(&context));
            const auto enumError = GetLastError(); sample.enumError = enumError; sample.enumerated = enumerated != FALSE;
            if (!enumerated && !sample.callbackFailed && !sample.budgetStopped) { sample.errorApi = "EnumDesktopWindows"; sample.error = enumError; }
            SetLastError(0); sample.waitAfter = WaitForSingleObject(game, 0); const auto afterError = GetLastError();
            if (sample.waitAfter != WAIT_TIMEOUT) { sample.errorApi = "original-game-not-alive-after"; sample.error = afterError; }
            sample.complete = sample.enumerated && !sample.callbackFailed && !sample.budgetStopped && sample.waitAfter == WAIT_TIMEOUT;
        } catch (...) { sample.complete = false; sample.error = ERROR_NOT_ENOUGH_MEMORY; }
    }
    void CaptureFirstIfDue() noexcept {
        if (requested && started && !sampleCount && GetTickCount64() - started >= 5000) Capture("approximately-five-seconds");
    }
    std::string JsonEvidence() const {
        const auto flag = [](bool value) { return value ? "true" : "false"; };
        std::string json = "{\"requested\":true,\"mode\":\"bounded\",\"acceptanceProof\":false,\"gamePid\":" + std::to_string(gamePid);
        json += ",\"borrowedDesktopVerified\":" + std::string(flag(desktopVerified)) + ",\"desktopErrorWin32\":" + std::to_string(desktopError);
        json += ",\"captionReaderAvailable\":" + std::string(flag(captionReader != nullptr)) + ",\"callbackLimit\":64,\"ownedWindowLimit\":32,\"dialogChildLimit\":16,\"textLimit\":160,\"childCompletionProof\":false,\"samples\":[";
        for (DWORD index = 0; index < sampleCount; ++index) {
            if (index) json += ',';
            const auto& sample = samples[index];
            json += "{\"reason\":" + Json(sample.reason) + ",\"elapsedMs\":" + std::to_string(sample.elapsedMs);
            json += ",\"enumerated\":" + std::string(flag(sample.enumerated)) + ",\"enumerationComplete\":" + flag(sample.complete);
            json += ",\"errorWin32\":" + std::to_string(sample.error) + ",\"enumErrorWin32\":" + std::to_string(sample.enumError) + ",\"errorApi\":" + Json(sample.errorApi);
            json += ",\"callbackCount\":" + std::to_string(sample.callbacks) + ",\"ownedWindowCount\":" + std::to_string(sample.rowCount);
            json += ",\"originalProcessWaitBefore\":" + std::to_string(sample.waitBefore) + ",\"originalProcessWaitAfter\":" + std::to_string(sample.waitAfter);
            json += ",\"callbackFailed\":" + std::string(flag(sample.callbackFailed)) + ",\"budgetStopped\":" + flag(sample.budgetStopped) + ",\"windows\":[";
            for (DWORD rowIndex = 0; rowIndex < sample.rowCount; ++rowIndex) {
                if (rowIndex) json += ',';
                const auto& row = sample.rows[rowIndex];
                json += "{\"hwnd\":" + std::to_string(row.hwnd) + ",\"ownerPid\":" + std::to_string(row.pid) + ",\"ownerTid\":" + std::to_string(row.tid);
                json += ",\"parentHwnd\":" + std::to_string(row.parentHwnd) + ",\"parentOwnerPid\":" + std::to_string(row.parentPid) + ",\"parentOwnerTid\":" + std::to_string(row.parentTid);
                json += ",\"parentStable\":" + std::string(flag(row.parentStable)) + ",\"childEnumerationAttempted\":" + flag(row.childEnumerationAttempted);
                json += ",\"childEnumReturned\":" + std::string(flag(row.childEnumReturned)) + ",\"childEnumErrorWin32\":" + std::to_string(row.childEnumError) + ",\"childCallbackCount\":" + std::to_string(row.childCallbacks);
                json += ",\"ownerStable\":" + std::string(flag(row.ownerStable)) + ",\"className\":" + Json(row.className) + ",\"caption\":" + Json(row.caption);
                json += ",\"classTruncated\":" + std::string(flag(row.classTruncated)) + ",\"captionTruncated\":" + flag(row.captionTruncated);
                json += ",\"modalClass\":" + std::string(flag(row.modalClass)) + ",\"errorWin32\":" + std::to_string(row.error) + ",\"errorApi\":" + Json(row.errorApi) + '}';
            }
            json += "]}";
        }
        return json + "]}";
    }
};
struct RunReceipt {
    ProbeDesktopWitness* witness = nullptr;
    ProbeWindowDiagnostics* windowDiagnostics = nullptr;
    SharedEvidence* evidence = nullptr;
    LuaTestMailbox* luaMailbox = nullptr;
    HANDLE file = INVALID_HANDLE_VALUE;
    bool created = false, witnessCleanupFailed = false;
    DWORD gameProcessId = 0, gameExit = STILL_ACTIVE, bridgeExit = kRuntimeFailure, error = 0;
    std::string stage;
    std::string gameSha, bootstrapSha, probeMapSha, probeScriptSha;
    std::string probeGraphicsBackendRequested;
    bool minimizedNoActivateRequested = false, visibleNoActivateRequested = false;
    std::vector<std::wstring> gameArgv;
    bool Write() {
        if (file == INVALID_HANDLE_VALUE) return false;
        if (witness && !witness->Finalize()) {
            witnessCleanupFailed = true;
            bridgeExit = kRuntimeFailure; stage = "probe-desktop-witness-cleanup"; error = witness->error;
        }
        const auto flag = [](bool value) { return value ? "true" : "false"; };
        std::string json = "{\"schema\":\"audastra-warcraft-launch-v1\",\"gameRuntimeVerified\":false,\"gameplayVerified\":false,\"processCreated\":";
        json += flag(created);
        json += ",\"gameProcessId\":" + std::to_string(gameProcessId) + ",\"gameExitCode\":" + std::to_string(gameExit);
        json += ",\"bridgeExitCode\":" + std::to_string(bridgeExit) + ",\"gameExecutableSha256\":" + Json(gameSha) + ",\"bootstrapSha256\":" + Json(bootstrapSha);
        if (!probeGraphicsBackendRequested.empty()) json += ",\"probeGraphicsBackendRequested\":" + Json(probeGraphicsBackendRequested);
        json += ",\"gameArgv\":[";
        for (std::size_t index = 0; index < gameArgv.size(); ++index) { if (index) json += ','; json += Json(Utf8(gameArgv[index])); }
        json += ']';
        json += ",\"hookInstalled\":" + std::string(flag(evidence && evidence->hookInstalled));
        json += ",\"gameObserved\":" + std::string(flag(evidence && evidence->gameObserved));
        json += ",\"runtimeInitialized\":" + std::string(flag(evidence && evidence->runtimeInitialized));
        json += ",\"runtimeLayout\":" + Json(evidence && evidence->runtimeLayout == RuntimeLayout::YdweTree ? "ydwe-tree" : "flat");
        const auto modeName = !evidence ? "external-ydwe" : evidence->runtimeMode == RuntimeMode::OwnedLua ? "owned-lua"
            : evidence->runtimeMode == RuntimeMode::Bare ? "bare" : "external-ydwe";
        json += ",\"runtimeMode\":" + Json(modeName);
        json += ",\"backgroundPlay\":" + std::string(flag(evidence && evidence->backgroundPlay));
        json += ",\"backgroundPlayInstalled\":" + std::string(flag(evidence && evidence->backgroundPlayInstalled));
        if (evidence && evidence->runtimeMode == RuntimeMode::OwnedLua) {
            const auto& owned = evidence->ownedLua;
            json += ",\"ownedLua\":{\"mapSha256\":" + Json(owned.mapSha256) + ",\"nonce\":" + Json(owned.nonce);
            json += ",\"observerInstalled\":" + std::string(flag(owned.observerInstalled != 0));
            json += ",\"minimizedNoActivateRequested\":" + std::string(flag(minimizedNoActivateRequested));
            json += ",\"visibleNoActivateRequested\":" + std::string(flag(visibleNoActivateRequested));
            json += ",\"ready\":" + std::string(flag(owned.ready != 0));
            json += ",\"mainThreadId\":" + std::to_string(owned.mainThreadId) + ",\"nativeCount\":" + std::to_string(owned.nativeCount);
            json += ",\"modules\":" + std::to_string(owned.modules) + ",\"callbacks\":" + std::to_string(owned.callbacks);
            json += ",\"polls\":" + std::to_string(owned.polls) + ",\"nativeCalls\":" + std::to_string(owned.nativeCalls);
            json += ",\"errors\":" + std::to_string(owned.errors) + ",\"failureText\":" + Json(owned.failureText);
            const bool stable = luaMailbox && LuaMailboxHeader(*luaMailbox) && !(luaMailbox->runtimeDiagnosticRevision & 1)
                && luaMailbox->runtimeDiagnosticSequence == luaMailbox->runtimeDiagnosticRevision / 2
                && luaMailbox->runtimeDiagnosticBytes < kLuaReloadResultCapacity
                && LuaMailboxUtf8({luaMailbox->runtimeDiagnosticText, luaMailbox->runtimeDiagnosticBytes});
            json += ",\"runtimeDiagnosticStable\":" + std::string(flag(stable));
            if (stable) {
                json += ",\"runtimeDiagnosticSequence\":" + std::to_string(luaMailbox->runtimeDiagnosticSequence);
                json += ",\"runtimeDiagnosticCode\":" + std::to_string(static_cast<std::uint32_t>(luaMailbox->runtimeDiagnosticCode));
                json += ",\"runtimeMessage\":" + Json({luaMailbox->runtimeDiagnosticText, luaMailbox->runtimeDiagnosticBytes});
                json += ",\"runtimeDiagnosticTruncated\":" + std::string(flag(luaMailbox->runtimeDiagnosticTruncated != 0));
            }
            json += '}';
        }
        json += ",\"fixturePluginBits\":" + std::to_string(evidence ? evidence->fixturePluginBits : 0);
        json += ",\"fixtureConcurrentPass\":" + std::to_string(evidence ? evidence->fixtureConcurrentPass : 0);
        json += ",\"fixtureConcurrentWaitObserved\":" + std::to_string(evidence ? evidence->fixtureConcurrentWaitObserved : 0);
        json += ",\"gameDllPath\":" + Json(evidence ? Utf8(evidence->gameModule.path) : "");
        json += ",\"gameDllSha256\":" + Json(evidence ? evidence->gameModule.actualSha256 : "");
        json += ",\"expectedGameDllSha256\":" + Json(evidence ? evidence->gameModule.expectedSha256 : "");
        json += ",\"errorStage\":" + Json(evidence && evidence->failed && !witnessCleanupFailed ? evidence->errorStage : stage);
        json += ",\"errorWin32\":" + std::to_string(evidence && evidence->failed && !witnessCleanupFailed ? evidence->errorWin32 : error) + ",\"modules\":[";
        if (evidence) for (std::size_t index = 0; index < kModules.size(); ++index) {
            if (index) json += ',';
            const auto& module = evidence->modules[index];
            json += "{\"name\":" + Json(Utf8(kModules[index])) + ",\"path\":" + Json(Utf8(module.path));
            json += ",\"expectedSha256\":" + Json(module.expectedSha256) + ",\"sha256\":" + Json(module.actualSha256);
            json += ",\"loaded\":" + std::string(flag(module.loaded != 0)) + ",\"initializeCalled\":" + flag(module.initializeCalled != 0) + '}';
        }
        json += ']';
        if (witness && witness->requested) json += ",\"desktopWitness\":" + witness->JsonEvidence();
        if (windowDiagnostics && windowDiagnostics->requested) json += ",\"windowDiagnostics\":" + windowDiagnostics->JsonEvidence();
        if (evidence && evidence->probeEnabled) {
            const auto bytes = (std::min)(evidence->probePayloadBytes, static_cast<DWORD>(kLuaProbePayloadCapacity - 1));
            json += ",\"luaApiProbe\":{\"enabled\":true,\"nonce\":" + Json(evidence->probeNonce);
            json += ",\"mapSha256\":" + Json(probeMapSha);
            json += ",\"scriptSha256\":" + Json(probeScriptSha);
            json += ",\"writeGuardInstalled\":" + std::string(flag(evidence->probeWriteGuardInstalled != 0));
            json += ",\"observerInstalled\":" + std::string(flag(evidence->probeObserverInstalled != 0));
            json += ",\"writeDenied\":" + std::string(flag(evidence->probeWriteDenied != 0));
            json += ",\"nonfatalBlockedPaths\":[";
            for (DWORD index = 0; index < (std::min)(evidence->probeNonfatalBlockedPathCount, static_cast<DWORD>(kNonfatalBlockedPathCapacity)); ++index) {
                if (index) json += ',';
                json += Json(Utf8(evidence->probeNonfatalBlockedPaths[index].data()));
            }
            json += ']';
            json += ",\"blockedWriteCount\":" + std::to_string(evidence->probeBlockedWriteCount);
            json += ",\"fatalWriteCount\":" + std::to_string(evidence->probeFatalWriteCount);
            json += ",\"blockedWriteOverflow\":" + std::string(flag(evidence->probeBlockedWriteOverflow != 0));
            json += ",\"blockedWrites\":[";
            const auto records = (std::min)((std::max)(evidence->probeBlockedWriteCount, 0L), static_cast<LONG>(kBlockedWriteCapacity));
            for (LONG index = 0; index < records; ++index) {
                if (index) json += ',';
                const auto& record = evidence->probeBlockedWrites[static_cast<std::size_t>(index)];
                json += "{\"path\":" + Json(Utf8(record.path)) + ",\"ntStatus\":" + std::to_string(record.status);
                json += ",\"nonfatal\":" + std::string(flag(record.nonfatal != 0)) + ",\"ready\":" + flag(record.ready != 0);
                json += ",\"originalNtDispatched\":false}";
            }
            json += ']';
            json += ",\"deniedPath\":" + Json(Utf8(evidence->probeDeniedPath));
            json += ",\"logRedirects\":" + std::to_string(evidence->probeLogRedirects);
            json += ",\"logPath\":" + Json(Utf8(evidence->probeLogPath));
            json += ",\"captureStarted\":" + std::string(flag(evidence->probeCaptureStarted != 0));
            json += ",\"captured\":" + std::string(flag(evidence->probeCaptured != 0));
            json += ",\"payloadJson\":" + Json(std::string_view(evidence->probePayload, bytes)) + '}';
        }
        json += "}\n";
        DWORD written = 0;
        return WriteFile(file, json.data(), static_cast<DWORD>(json.size()), &written, nullptr) && written == json.size() && FlushFileBuffers(file);
    }
};
int Run(int argc, wchar_t** argv) {
    if (argc < 11 || argc % 2 != 1) {
        std::fputs("Required: --game --map --runtime --bootstrap --receipt [--runtime-layout flat|ydwe-tree]"
            " | --runtime-mode bare (no --runtime) [--background-play enabled] [--game-nosound enabled]\n", stderr);
        return 64;
    }
    std::filesystem::path game, runtime, bootstrap, receiptPath;
    std::wstring map;
    RuntimeLayout layout = RuntimeLayout::Flat;
    bool layoutSpecified = false;
    RuntimeMode mode = RuntimeMode::ExternalYdwe;
    bool modeSpecified = false, hotReload = false, backgroundPlay = false, noSound = false;
    std::wstring ownedMapSha, ownedNonce, ownedMailboxHandle, ownedWindowPolicy;
    DWORD ownedTimeout = 0;
    std::wstring probeNonce, expectedProbeMapSha, desktop, probeRenderer;
    bool desktopWitnessRequested = false, windowDiagnosticsRequested = false;
    std::filesystem::path probeScript;
    std::wstring expectedProbeScriptSha;
    std::vector<std::filesystem::path> nonfatalBlockedPaths;
    DWORD probeTimeout = 45000;
    bool probeTimeoutSpecified = false;
    for (int index = 1; index < argc; index += 2) {
        const std::wstring key(argv[index]);
        if (key == L"--game" && game.empty()) game = argv[index + 1];
        else if (key == L"--map" && map.empty()) map = argv[index + 1];
        else if (key == L"--runtime" && runtime.empty()) runtime = argv[index + 1];
        else if (key == L"--bootstrap" && bootstrap.empty()) bootstrap = argv[index + 1];
        else if (key == L"--receipt" && receiptPath.empty()) receiptPath = argv[index + 1];
        else if (key == L"--runtime-layout" && !layoutSpecified) {
            const std::wstring value(argv[index + 1]);
            if (value == L"ydwe-tree") layout = RuntimeLayout::YdweTree;
            else if (value != L"flat") return 64;
            layoutSpecified = true;
        }
        else if (key == L"--runtime-mode" && !modeSpecified) {
            const std::wstring value(argv[index + 1]);
            if (value == L"owned-lua") mode = RuntimeMode::OwnedLua;
            else if (value == L"bare") mode = RuntimeMode::Bare;
            else if (value != L"external-ydwe") return 64;
            modeSpecified = true;
        }
        // 两个开关都只接受显式 enabled；省略即关闭，缺省命令与行为不变。
        else if (key == L"--background-play" && !backgroundPlay) {
            if (std::wstring_view(argv[index + 1]) != L"enabled") return 64;
            backgroundPlay = true;
        }
        else if (key == L"--game-nosound" && !noSound) {
            if (std::wstring_view(argv[index + 1]) != L"enabled") return 64;
            noSound = true;
        }
        else if (key == L"--lua-map-sha256" && ownedMapSha.empty()) ownedMapSha = argv[index + 1];
        else if (key == L"--lua-session-nonce" && ownedNonce.empty()) ownedNonce = argv[index + 1];
        else if (key == L"--lua-mailbox-handle" && ownedMailboxHandle.empty()) ownedMailboxHandle = argv[index + 1];
        else if (key == L"--lua-hot-reload" && !hotReload) {
            if (std::wstring_view(argv[index + 1]) != L"enabled") return 64;
            hotReload = true;
        }
        else if (key == L"--owned-timeout-ms" && !ownedTimeout) {
            const std::wstring value(argv[index + 1]);
            if (value.empty() || value.size() > 6 || !std::all_of(value.begin(), value.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; })) return 64;
            ownedTimeout = static_cast<DWORD>(wcstoul(value.c_str(), nullptr, 10));
            if (ownedTimeout < 1000 || ownedTimeout > 120000) return 64;
        }
        else if (key == L"--owned-window-policy" && ownedWindowPolicy.empty()) {
            ownedWindowPolicy = argv[index + 1];
            if (ownedWindowPolicy != L"minimized-no-activate" && ownedWindowPolicy != L"visible-no-activate") return 64;
        }
        else if (key == L"--lua-api-probe" && probeNonce.empty()) probeNonce = argv[index + 1];
        else if (key == L"--probe-map-sha256" && expectedProbeMapSha.empty()) expectedProbeMapSha = argv[index + 1];
        else if (key == L"--probe-script" && probeScript.empty()) probeScript = argv[index + 1];
        else if (key == L"--probe-script-sha256" && expectedProbeScriptSha.empty()) expectedProbeScriptSha = argv[index + 1];
        else if (key == L"--probe-nonfatal-blocked-path") {
            if (nonfatalBlockedPaths.size() >= kNonfatalBlockedPathCapacity) return 64;
            const std::filesystem::path path(argv[index + 1]);
            if (!PlainPath(path)) return 64;
            for (const auto& part : path.relative_path()) {
                const auto text = part.wstring();
                if (text.empty() || text == L"." || text == L".." || text.back() == L'.' || text.back() == L' ' || text.find(L':') != std::wstring::npos) return 64;
            }
            const auto canonical = CanonicalBlockedPath(path);
            if (canonical.empty()) return 64;
            for (const auto& prior : nonfatalBlockedPaths) if (EqualPath(prior.wstring(), canonical.wstring())) return 64;
            nonfatalBlockedPaths.push_back(canonical);
        }
        else if (key == L"--desktop" && desktop.empty()) desktop = argv[index + 1];
        else if (key == L"--probe-desktop-witness" && !desktopWitnessRequested) {
            if (std::wstring_view(argv[index + 1]) != L"hidden-window") return 64;
            desktopWitnessRequested = true;
        }
        else if (key == L"--probe-window-diagnostics" && !windowDiagnosticsRequested) {
            if (std::wstring_view(argv[index + 1]) != L"bounded") return 64;
            windowDiagnosticsRequested = true;
        }
        else if (key == L"--probe-renderer" && probeRenderer.empty()) {
            if (std::wstring_view(argv[index + 1]) != L"opengl") return 64;
            probeRenderer = L"opengl";
        }
        else if (key == L"--probe-timeout-ms" && !probeTimeoutSpecified) {
            const std::wstring value(argv[index + 1]);
            if (value.empty() || value.size() > 6 || !std::all_of(value.begin(), value.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; })) return 64;
            probeTimeout = static_cast<DWORD>(wcstoul(value.c_str(), nullptr, 10)); probeTimeoutSpecified = true;
            if (probeTimeout < 1000 || probeTimeout > 120000) return 64;
        }
        else return 64;
    }
    const bool owned = mode == RuntimeMode::OwnedLua;
    const bool bare = mode == RuntimeMode::Bare;
    if (!PlainPath(game) || (!owned && !bare && !PlainPath(runtime)) || !PlainPath(bootstrap) || !PlainPath(receiptPath)
        || !SafeMap(map) || !EqualPath(game.filename().wstring(), L"war3.exe")) return 64;
    // 裸模式不加载任何扩展运行库，也不承担探针。
    if (bare && (!runtime.empty() || layoutSpecified || !probeNonce.empty())) return 64;
    const bool probe = !probeNonce.empty();
    const auto lowercaseHex = [](const std::wstring& value, std::size_t length) {
        return value.size() == length && std::all_of(value.begin(), value.end(), [](wchar_t c) { return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f'); });
    };
    if (owned) {
        if (probe || !runtime.empty() || layoutSpecified || !lowercaseHex(ownedMapSha, 64) || !lowercaseHex(ownedNonce, 32)) return 64;
        if (!ownedWindowPolicy.empty() && !ownedTimeout) return 64;
        if (!desktop.empty()) {
            if (desktop != L"WinSta0\\AudastraOwnedLua-" + ownedNonce) return 64;
            std::array<wchar_t, 256> actualDesktop{}; DWORD returned = 0;
            if (!GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()), UOI_NAME, actualDesktop.data(),
                static_cast<DWORD>(actualDesktop.size() * sizeof(wchar_t)), &returned)
                || std::wstring(actualDesktop.data()) != L"AudastraOwnedLua-" + ownedNonce) return 64;
        }
    } else if (!ownedMapSha.empty() || !ownedNonce.empty() || !ownedMailboxHandle.empty() || hotReload || ownedTimeout || !ownedWindowPolicy.empty()) return 64;
    if (probe) {
        // 探针必须先由调度者放在独立 Desktop 和 Job；严禁默默落到用户桌面。
        if (!lowercaseHex(probeNonce, 32) || !lowercaseHex(expectedProbeMapSha, 64)
            || !PlainPath(probeScript) || !lowercaseHex(expectedProbeScriptSha, 64)
            || desktop != L"WinSta0\\AudastraRuntimeProbe-" + probeNonce || layout != RuntimeLayout::YdweTree) return 64;
        std::array<wchar_t, 256> actualDesktop{}; DWORD returned = 0;
        if (!GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()), UOI_NAME, actualDesktop.data(),
            static_cast<DWORD>(actualDesktop.size() * sizeof(wchar_t)), &returned)
            || std::wstring(actualDesktop.data()) != L"AudastraRuntimeProbe-" + probeNonce) return 64;
    } else if (!expectedProbeMapSha.empty() || (!owned && !desktop.empty()) || probeTimeoutSpecified || !probeScript.empty() || !expectedProbeScriptSha.empty() || !nonfatalBlockedPaths.empty() || desktopWitnessRequested || windowDiagnosticsRequested || !probeRenderer.empty()) return 64;
    game = game.lexically_normal(); runtime = runtime.lexically_normal(); bootstrap = bootstrap.lexically_normal();
    Handle output(CreateFileW(receiptPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (output.value == INVALID_HANDLE_VALUE) return 65;
    ProbeDesktopWitness witness;
    ProbeWindowDiagnostics windowDiagnostics; windowDiagnostics.requested = windowDiagnosticsRequested;
    RunReceipt result; result.file = output.value; result.witness = &witness; result.windowDiagnostics = &windowDiagnostics;
    result.visibleNoActivateRequested = owned && ownedTimeout != 0 && ownedWindowPolicy == L"visible-no-activate";
    result.minimizedNoActivateRequested = owned && ownedTimeout != 0 && !result.visibleNoActivateRequested;
    if (probe) result.probeGraphicsBackendRequested = probeRenderer.empty() ? "game-default" : "opengl";
    const auto fail = [&](const char* stage, DWORD error = GetLastError()) {
        result.stage = stage; result.error = error; result.bridgeExit = kRuntimeFailure;
        if (!result.Write()) return 65;
        return static_cast<int>(result.bridgeExit);
    };
    BOOL bridgeInJob = FALSE;
    if (!IsProcessInJob(GetCurrentProcess(), nullptr, &bridgeInJob) || !bridgeInJob) return fail("bridge-job-required", ERROR_ACCESS_DENIED);
    if (desktopWitnessRequested && !witness.Create(probeNonce)) return fail("probe-desktop-witness-create", witness.error);
    Handle gameFile(OpenLockedFile(game)), bootstrapFile(OpenLockedFile(bootstrap)), mapFile(OpenLockedFile(game.parent_path() / map));
    Handle gameDll(OpenLockedFile(game.parent_path() / L"Game.dll"));
    if (gameFile.value == INVALID_HANDLE_VALUE || bootstrapFile.value == INVALID_HANDLE_VALUE || mapFile.value == INVALID_HANDLE_VALUE || gameDll.value == INVALID_HANDLE_VALUE
        || !IsX86Image(gameFile.value) || !IsX86Image(bootstrapFile.value) || !IsX86Image(gameDll.value)) return fail("game-bootstrap-preflight", ERROR_BAD_EXE_FORMAT);
    result.gameSha = Sha256(gameFile.value); result.bootstrapSha = Sha256(bootstrapFile.value);
    if (result.gameSha.empty() || result.bootstrapSha.empty()) return fail("game-bootstrap-sha", ERROR_CRC);
    if (probe) {
        result.probeMapSha = Sha256(mapFile.value);
        if (result.probeMapSha != Utf8(expectedProbeMapSha)) return fail("lua-probe-map-sha", ERROR_CRC);
    }
    SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    Handle luaMapping;
    LuaTestMailbox* luaMailbox = nullptr;
    struct LuaView { LuaTestMailbox*& value; ~LuaView() { if (value) UnmapViewOfFile(value); } } luaView{luaMailbox};
    if (owned) {
        const auto mapSha = Sha256(mapFile.value, 1024ull * 1024 * 1024);
        if (mapSha != Utf8(ownedMapSha)) return fail("owned-lua-map-sha", ERROR_CRC);
        if (!ownedMailboxHandle.empty()) {
            if (ownedMailboxHandle.size() > 10 || !std::all_of(ownedMailboxHandle.begin(), ownedMailboxHandle.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; }))
                return fail("owned-lua-mailbox-handle", ERROR_INVALID_HANDLE);
            wchar_t* end = nullptr;
            const auto numeric = _wcstoui64(ownedMailboxHandle.c_str(), &end, 10);
            if (!numeric || numeric >= 0xffffffffull || !end || *end) return fail("owned-lua-mailbox-handle", ERROR_INVALID_HANDLE);
            // 只复制本次启动明确继承到的句柄；不按PID重开，不连接具名对象。
            if (!DuplicateHandle(GetCurrentProcess(), reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(numeric)),
                GetCurrentProcess(), &luaMapping.value, 0, TRUE, DUPLICATE_SAME_ACCESS)) return fail("owned-lua-mailbox-inheritance");
        } else {
            luaMapping.value = CreateFileMappingW(INVALID_HANDLE_VALUE, &inherit, PAGE_READWRITE, 0, sizeof(LuaTestMailbox), nullptr);
            if (!luaMapping.value) return fail("owned-lua-mailbox-create");
        }
        luaMailbox = static_cast<LuaTestMailbox*>(MapViewOfFile(luaMapping.value, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(LuaTestMailbox)));
        if (!luaMailbox) return fail("owned-lua-mailbox-map");
        if (ownedMailboxHandle.empty()) {
            ::new (luaMailbox) LuaTestMailbox{};
            strcpy_s(luaMailbox->sessionNonce, Utf8(ownedNonce).c_str());
            strcpy_s(luaMailbox->mapSha256, mapSha.c_str());
        }
        if (!LuaMailboxHeader(*luaMailbox) || luaMailbox->sessionNonce != Utf8(ownedNonce)
            || luaMailbox->mapSha256 != mapSha || luaMailbox->state != static_cast<LONG>(LuaMailboxState::Empty)
            || luaMailbox->runtimeState != static_cast<LONG>(LuaRuntimeState::Starting))
            return fail("owned-lua-mailbox-identity", ERROR_INVALID_DATA);
        result.luaMailbox = luaMailbox;
    }
    Handle mapping(CreateFileMappingW(INVALID_HANDLE_VALUE, &inherit, PAGE_READWRITE, 0, sizeof(SharedEvidence), nullptr));
    if (!mapping.value) return fail("channel-create");
    auto* shared = static_cast<SharedEvidence*>(MapViewOfFile(mapping.value, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedEvidence)));
    if (!shared) return fail("channel-view");
    struct View { void* value; ~View() { UnmapViewOfFile(value); } } view{shared};
    new (shared) SharedEvidence();
    wcscpy_s(shared->runtimeRoot, runtime.c_str());
    shared->runtimeLayout = layout;
    shared->runtimeMode = mode;
    shared->backgroundPlay = backgroundPlay ? 1 : 0;
    if (owned) {
        wcscpy_s(shared->ownedLua.mapPath, (game.parent_path() / map).c_str());
        strcpy_s(shared->ownedLua.mapSha256, Utf8(ownedMapSha).c_str());
        strcpy_s(shared->ownedLua.nonce, Utf8(ownedNonce).c_str());
        shared->ownedLua.mailboxHandle = static_cast<std::uint32_t>(reinterpret_cast<ULONG_PTR>(luaMapping.value));
        shared->ownedLua.hotReload = hotReload ? 1 : 0;
    }
    if (probe) {
        shared->probeEnabled = 1; strcpy_s(shared->probeNonce, Utf8(probeNonce).c_str());
        Handle scriptFile(OpenLockedFile(probeScript));
        if (scriptFile.value == INVALID_HANDLE_VALUE) return fail("lua-probe-script-read-lease", ERROR_ACCESS_DENIED);
        result.probeScriptSha = Sha256(scriptFile.value);
        if (result.probeScriptSha != Utf8(expectedProbeScriptSha)) return fail("lua-probe-script-sha", ERROR_CRC);
        LARGE_INTEGER zero{}, sourceSize{}; DWORD sourceRead = 0;
        if (!GetFileSizeEx(scriptFile.value, &sourceSize) || sourceSize.QuadPart <= 0 || sourceSize.QuadPart >= kLuaProbeSourceCapacity
            || !SetFilePointerEx(scriptFile.value, zero, nullptr, FILE_BEGIN)
            || !ReadFile(scriptFile.value, shared->probeSource, static_cast<DWORD>(sourceSize.QuadPart), &sourceRead, nullptr)
            || sourceRead != sourceSize.QuadPart || std::memchr(shared->probeSource, 0, sourceRead)) return fail("lua-probe-script-input-budget", ERROR_INVALID_DATA);
        shared->probeSourceBytes = sourceRead;
        wcscpy_s(shared->probeWriteRoot, game.parent_path().c_str());
        const auto logPath = (game.parent_path() / map).parent_path() / L"audastra-runtime-probe-war3.log";
        wcscpy_s(shared->probeLogPath, logPath.c_str());
        shared->probeNonfatalBlockedPathCount = static_cast<DWORD>(nonfatalBlockedPaths.size());
        for (std::size_t index = 0; index < nonfatalBlockedPaths.size(); ++index)
            wcscpy_s(shared->probeNonfatalBlockedPaths[index].data(), kPathCapacity, nonfatalBlockedPaths[index].c_str());
    }
    result.evidence = shared;
    wcscpy_s(shared->gameModule.path, (game.parent_path() / L"Game.dll").c_str());
    const auto gameDllSha = Sha256(gameDll.value);
    if (gameDllSha.empty()) return fail("game-dll-sha", ERROR_CRC);
    strcpy_s(shared->gameModule.expectedSha256, gameDllSha.c_str());
    std::vector<std::unique_ptr<Handle>> runtimeFiles;
    for (std::size_t index = 0; !owned && !bare && index < kModules.size(); ++index) {
        auto file = std::make_unique<Handle>(OpenLockedFile(runtime / ModuleRelativePath(layout, index)));
        if (file->value == INVALID_HANDLE_VALUE || !IsX86Image(file->value)) return fail("runtime-preflight", ERROR_BAD_EXE_FORMAT);
        const auto sha = Sha256(file->value);
        if (sha.empty()) return fail("runtime-sha", ERROR_CRC);
        strcpy_s(shared->modules[index].expectedSha256, sha.c_str());
        runtimeFiles.push_back(std::move(file));
    }
    const auto importPath = ImportPath(bootstrap);
    if (importPath.empty()) return fail("bootstrap-path-encoding", ERROR_NO_UNICODE_TRANSLATION);
    AttributeList attributes;
    if (!attributes.Initialize(mapping.value, owned ? luaMapping.value : nullptr)) return fail("channel-handle-whitelist");
    auto environment = ChildEnvironment(mapping.value, (probe || owned) ? (game.parent_path() / map).parent_path() : std::filesystem::path{});
    result.gameArgv = {game.wstring(), L"-window"};
    if (noSound) result.gameArgv.push_back(L"-nosound");
    if (!probeRenderer.empty()) result.gameArgv.push_back(L"-opengl");
    result.gameArgv.push_back(L"-loadfile"); result.gameArgv.push_back(map);
    // 唯一向量既形成实际 CreateProcessW 命令，也进入回执；缺省命令字节形态保持不变。
    std::wstring command = Quote(result.gameArgv.front());
    for (std::size_t index = 1; index < result.gameArgv.size(); ++index)
        command += L" " + (index + 1 == result.gameArgv.size() ? Quote(result.gameArgv[index]) : result.gameArgv[index]);
    STARTUPINFOEXW startup{}; startup.StartupInfo.cb = sizeof(startup); startup.lpAttributeList = attributes.value;
    if (result.minimizedNoActivateRequested || result.visibleNoActivateRequested) {
        // 显式观察窗口供无人值守开发验证；普通 F9 没有观察期限，不改变其窗口显示。
        // 默认仍最小化；可见但不激活只用于显式图形初始化对照。两者都是请求，
        // 调度者仍须监测真实前台归属，不能把此标志当成游戏不抢焦点的证据。
        startup.StartupInfo.dwFlags |= STARTF_USESHOWWINDOW;
        startup.StartupInfo.wShowWindow = result.visibleNoActivateRequested ? SW_SHOWNOACTIVATE : SW_SHOWMINNOACTIVE;
    }
    if (!desktop.empty()) startup.StartupInfo.lpDesktop = desktop.data();
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(game.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW,
        environment.data(), game.parent_path().c_str(), &startup.StartupInfo, &process)) return fail("game-create");
    Handle target(process.hProcess), thread(process.hThread);
    result.created = true; result.gameProcessId = process.dwProcessId;
    if (windowDiagnosticsRequested) windowDiagnostics.Bind(target.value, process.dwProcessId, std::wstring_view(desktop).substr(8)); // 已校验 WinSta0\\ 前缀。
    const auto abort = [&](const char* stage, DWORD error = GetLastError()) {
        windowDiagnostics.Capture(stage);
        TerminateProcess(target.value, kRuntimeFailure);
        if (WaitForSingleObject(target.value, 5000) == WAIT_OBJECT_0) GetExitCodeProcess(target.value, &result.gameExit);
        return fail(stage, error);
    };
    // 不使用BREAKAWAY；Windows创建规则继承bridge的原Job，恢复前再确认新原句柄归属。
    BOOL childInJob = FALSE;
    if (!IsProcessInJob(target.value, nullptr, &childInJob) || !childInJob) return abort("game-job-membership", ERROR_ACCESS_DENIED);
    const char* injected[] = {importPath.c_str()};
    if (!DetourUpdateProcessWithDll(target.value, injected, 1)) return abort("bootstrap-injection");
    if (ResumeThread(thread.value) == static_cast<DWORD>(-1)) return abort("game-resume");
    windowDiagnostics.started = GetTickCount64();
    // 仅初始化握手有15秒预算；通过后才进入正常的游戏全生命周期等待。
    // 未观测Game.dll或插件未完成不能让裸游戏无期限运行到用户手动退出。
    const auto handshakeStart = GetTickCount64();
    while (true) {
        windowDiagnostics.CaptureFirstIfDue();
        MemoryBarrier();
        if (shared->failed) return abort("bootstrap-reported-failure", shared->errorWin32);
        if (shared->hookInstalled && shared->gameObserved && shared->runtimeInitialized
            && (!backgroundPlay || shared->backgroundPlayInstalled)) break;
        const auto terminal = WaitForSingleObject(target.value, 50);
        if (terminal == WAIT_OBJECT_0) break;
        if (terminal != WAIT_TIMEOUT) return abort("game-initialization-wait");
        if (GetTickCount64() - handshakeStart >= 15000) return abort("runtime-initialization-timeout", WAIT_TIMEOUT);
    }
    if (probe) {
        const auto probeStart = GetTickCount64();
        while (true) {
            windowDiagnostics.CaptureFirstIfDue();
            MemoryBarrier();
            if (shared->failed) return abort("lua-probe-reported-failure", shared->errorWin32);
            if (shared->probeCaptured) {
                windowDiagnostics.Capture("before-captured-stop");
                if (!TerminateProcess(target.value, 0) || WaitForSingleObject(target.value, 5000) != WAIT_OBJECT_0
                    || !GetExitCodeProcess(target.value, &result.gameExit)) return abort("lua-probe-exact-stop");
                result.bridgeExit = 0; result.stage = "lua-api-probe-captured";
                if (!result.Write()) return 65;
                return static_cast<int>(result.bridgeExit);
            }
            const auto terminal = WaitForSingleObject(target.value, 50);
            if (terminal == WAIT_OBJECT_0) break;
            if (terminal != WAIT_TIMEOUT) return abort("lua-probe-wait");
            if (GetTickCount64() - probeStart >= probeTimeout) return abort("lua-probe-result-timeout", WAIT_TIMEOUT);
        }
        windowDiagnostics.Capture("game-exited-without-result");
        return fail("lua-probe-game-exited-without-result", ERROR_INVALID_DATA);
    }
    if (owned && ownedTimeout) {
        const auto start = GetTickCount64();
        while (WaitForSingleObject(target.value, 50) == WAIT_TIMEOUT) {
            if (shared->failed) return abort("owned-lua-reported-failure", shared->errorWin32);
            if (GetTickCount64() - start < ownedTimeout) continue;
            if (!TerminateProcess(target.value, 0) || WaitForSingleObject(target.value, 5000) != WAIT_OBJECT_0
                || !GetExitCodeProcess(target.value, &result.gameExit)) return abort("owned-lua-exact-stop");
            result.stage = "owned-lua-observation-window-ended";
            result.bridgeExit = shared->ownedLua.ready && shared->ownedLua.modules && !shared->ownedLua.errors ? 0 : kRuntimeFailure;
            if (!result.Write()) return 65;
            return static_cast<int>(result.bridgeExit);
        }
    }
    if (WaitForSingleObject(target.value, INFINITE) != WAIT_OBJECT_0 || !GetExitCodeProcess(target.value, &result.gameExit)) return fail("game-terminal-query");
    MemoryBarrier();
    if (shared->failed || !shared->hookInstalled || !shared->gameObserved || !shared->runtimeInitialized
        || (backgroundPlay && !shared->backgroundPlayInstalled)) {
        if (!shared->failed) { result.stage = "runtime-initialization-unconfirmed"; result.error = ERROR_INVALID_DATA; }
        result.bridgeExit = kRuntimeFailure;
    } else if (owned && (!shared->ownedLua.modules || shared->ownedLua.errors)) {
        result.stage = "owned-lua-entry-unconfirmed-or-failed"; result.bridgeExit = kRuntimeFailure;
    } else result.bridgeExit = result.gameExit;
    if (!result.Write()) return 65;
    return static_cast<int>(result.bridgeExit);
}
} // namespace
int wmain(int argc, wchar_t** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    try { return Run(argc, argv); }
    catch (...) { std::fputs("Audastra runtime bridge failed before a confirmed result.\n", stderr); return 65; }
}
