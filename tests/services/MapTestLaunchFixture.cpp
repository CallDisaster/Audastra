#include <windows.h>

#include <filesystem>
#include <fstream>
#include <string>

int wmain(int argc, wchar_t** argv) {
    if (argc != 4) return 2;
    const std::wstring mode = argv[1];
    const std::filesystem::path marker = argv[2];
    const auto milliseconds = std::stoul(argv[3]);
    if (milliseconds > 10000) return 3;
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
