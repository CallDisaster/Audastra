#include <windows.h>
#include "../../src/WarcraftRuntime32/OwnLua/LuaMailboxRuntime.h"
#include "../../src/WarcraftRuntime32/Japi/RuntimeTestWriter.h"
#include "../../src/WarcraftRuntime32/GameTest/WindowInput.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {
using namespace Audastra::Runtime32;
unsigned mouseDowns = 0, mouseUps = 0, qDowns = 0;
LRESULT CALLBACK InputWindowProc(HWND window, UINT message, WPARAM key, LPARAM point) {
    if (message == WM_LBUTTONDOWN) ++mouseDowns;
    if (message == WM_LBUTTONUP) ++mouseUps;
    if (message == WM_KEYDOWN && key == 'Q') ++qDowns;
    if (message == WM_CLOSE) { DestroyWindow(window); return 0; }
    if (message == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(window, message, key, point);
}
int InputFixture(const std::filesystem::path& marker, DWORD timeout) {
    WNDCLASSW type{}; type.lpfnWndProc = InputWindowProc; type.hInstance = GetModuleHandleW(nullptr); type.lpszClassName = L"AudastraOwnedInputFixture";
    if (!RegisterClassW(&type)) return 81;
    const auto window = CreateWindowW(type.lpszClassName, L"owned hidden input fixture", WS_OVERLAPPEDWINDOW, 0, 0, 640, 480, nullptr, nullptr, type.hInstance, nullptr);
    if (!window || !GameTest::InstallWindowInput(window)) return 82;
    const auto start = GetTickCount64(); bool held = false, cursor = false;
    while (GetTickCount64() - start < timeout) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                std::ofstream output(marker); output << mouseDowns << ',' << mouseUps << ',' << qDowns << ',' << held << ',' << cursor;
                return 0;
            }
            TranslateMessage(&message); DispatchMessageW(&message);
        }
        POINT position{}; cursor |= GameTest::WindowInputCursor(window, position);
        held |= (GameTest::WindowInputKey(VK_LBUTTON, true) & 0x8000) != 0;
        (void)GameTest::WindowInputKey(VK_CONTROL, false);
        BYTE keyboard[256]{}; (void)GameTest::WindowInputKeyboard(keyboard);
        Sleep(1);
    }
    DestroyWindow(window); return 83;
}
std::wstring Value(int argc, wchar_t** argv, std::wstring_view flag) {
    for (int index = 2; index + 1 < argc; ++index) if (argv[index] == flag) return argv[index + 1];
    return {};
}
std::string HexText(std::wstring_view text) {
    std::string result;
    for (const auto c : text) {
        if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f'))) return {};
        result += static_cast<char>(c);
    }
    return result;
}
int OwnedFixture(int argc, wchar_t** argv) {
    const auto raw = Value(argc, argv, L"--lua-mailbox-handle");
    if (raw.empty()) return 60;
    const auto handle = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(std::stoull(raw)));
    const auto nonce = Value(argc, argv, L"--lua-session-nonce"), sha = Value(argc, argv, L"--lua-map-sha256");
    const auto marker = std::filesystem::path(Value(argc, argv, L"--marker"));
    if (nonce.size() != 32 || sha.size() != 64 || marker.empty()) return 61;
    if (Value(argc, argv, L"--startup-failure") == L"yes") {
        std::ofstream output(std::filesystem::path(Value(argc, argv, L"--receipt")), std::ios::binary);
        output << R"({"schema":"audastra-warcraft-launch-v1","errorStage":"bootstrap","errorWin32":13,"ownedLua":{"failureText":"original pre-handshake failure"}})";
        output.close(); return 13;
    }
    if (std::wstring_view(argv[1]) == L"--owned-child") {
        auto* mailbox = static_cast<LuaTestMailbox*>(MapViewOfFile(handle, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(LuaTestMailbox)));
        if (!mailbox) return 62;
        OwnLua::LuaMailboxRuntime runtime(*mailbox);
        if (!runtime.Attach(HexText(nonce), HexText(sha))) return 63;
        if (!runtime.Report(LuaReloadResult::RuntimeError, "original launch fixture asynchronous diagnostic")) return 64;
        if (Value(argc, argv, L"--spoof-pid") == L"yes") mailbox->gameProcessId ^= 0x40000000u;
        const auto start = GetTickCount64();
        while (GetTickCount64() - start < 1400) {
            if (auto change = runtime.Take())
                if (!runtime.Complete(change->sequence, LuaReloadResult::Success, "original fixture ACK")) return 65;
            Sleep(10);
        }
        runtime.Stop(); UnmapViewOfFile(mailbox);
        std::ofstream output(marker, std::ios::binary); output << "owned child completed naturally";
        return output ? 0 : 66;
    }
    const auto forbidden = Value(argc, argv, L"--forbidden-handle");
    if (!forbidden.empty()) {
        auto* view = MapViewOfFile(reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(std::stoull(forbidden))), FILE_MAP_READ, 0, 0, 16);
        if (view) {
            const bool leaked = std::memcmp(view, "not-for-child", 13) == 0;
            UnmapViewOfFile(view);
            if (leaked) return 67;
        }
    }
    const auto game = Value(argc, argv, L"--game");
    std::wstring command = L"\"" + game + L"\" --owned-child";
    for (int index = 2; index < argc; ++index) {
        const std::wstring argument = argv[index];
        if (argument.find(L'"') != std::wstring::npos) return 68;
        command += L" \"" + argument + L"\"";
    }
    SIZE_T bytes = 0; InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    std::vector<std::byte> storage(bytes);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &bytes)) return 69;
    if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
        const_cast<HANDLE*>(&handle), sizeof(handle), nullptr, nullptr)) { DeleteProcThreadAttributeList(attributes); return 70; }
    STARTUPINFOEXW startup{}; startup.StartupInfo.cb = sizeof(startup); startup.lpAttributeList = attributes;
    PROCESS_INFORMATION process{};
    const bool created = CreateProcessW(game.c_str(), command.data(), nullptr, nullptr, TRUE,
        EXTENDED_STARTUPINFO_PRESENT | DETACHED_PROCESS, nullptr, nullptr, &startup.StartupInfo, &process) != FALSE;
    DeleteProcThreadAttributeList(attributes);
    if (!created) return 71;
    CloseHandle(process.hThread); CloseHandle(process.hProcess);
    Sleep(250);
    return 0;
}
}

int wmain(int argc, wchar_t** argv) {
    if (argc >= 2 && (std::wstring_view(argv[1]) == L"--owned-parent" || std::wstring_view(argv[1]) == L"--owned-child"))
        return OwnedFixture(argc, argv);
    if (argc != 4) return 2;
    const std::wstring mode = argv[1];
    const std::filesystem::path marker = argv[2];
    const auto milliseconds = std::stoul(argv[3]);
    if (milliseconds > 10000) return 3;
    if (mode == L"--input-window") return InputFixture(marker, static_cast<DWORD>(milliseconds));
    if (mode == L"--evidence") {
        Audastra::Runtime32::Japi::RuntimeTestWriter writer;
        writer.Loaded(); writer.Installed(0, std::string(64, 'b')); writer.Hello(7);
        if (!writer.Report(7, 3, 0, std::string(64, 'b'))) return 71;
        Sleep(static_cast<DWORD>(milliseconds));
        std::ofstream file(marker, std::ios::binary); file << "original fixture reported its own test evidence";
        return file ? 0 : 72;
    }
    if (mode == L"--child") {
        Sleep(static_cast<DWORD>(milliseconds));
        std::ofstream file(marker, std::ios::binary); file << "child completed naturally";
        return file ? 0 : 4;
    }
    if (mode != L"--parent") return 5;
    wchar_t executable[32768]{};
    const auto count = GetModuleFileNameW(nullptr, executable, 32768);
    if (!count || count >= 32768) return 6;
    // 输入夹具标记路径由测试自己拥有，禁止控制字符/引号，参数用标准Windows引号。
    if (marker.wstring().find(L'"') != std::wstring::npos) return 7;
    std::wstring command = L"\"" + std::wstring(executable, count) + L"\" --child \"" + marker.wstring()
        + L"\" " + std::to_wstring(milliseconds);
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) return 8;
    CloseHandle(process.hThread); CloseHandle(process.hProcess);
    // 给completion-port接收线程取得子原句柄留出明确窗口；parent在child之前自然结束。
    Sleep(250);
    return 0;
}
