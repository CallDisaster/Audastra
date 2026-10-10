// 便携包根目录的 Audastra.exe：找到编辑器并原样转交参数后立即退出。
// 静态运行库、不依赖 Qt，放在根目录也不需要任何 DLL；更新安装时它不占用 bin 下的文件。
#include "LauncherPaths.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <Windows.h>

namespace {

namespace fs = std::filesystem;

void ShowError(const std::wstring& text) {
    MessageBoxW(nullptr, text.c_str(), L"Audastra 无法启动", MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
}

fs::path ModuleDirectory() {
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const auto length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) return {};
        if (length < buffer.size()) { buffer.resize(length); break; }
        if (buffer.size() >= 32768) return {};
        buffer.resize(buffer.size() * 2);
    }
    return fs::path(buffer).parent_path();
}

bool IsFile(const fs::path& path) {
    std::error_code error;
    return fs::is_regular_file(path, error);
}

// 依次找：便携包布局 bin\war3-map-editor.exe；开发构建目录里同级的编辑器；
// 发布目录（含“最新版本.txt”）里最新版本自带的 Audastra.exe。
fs::path FindTarget(const fs::path& root, std::wstring& problem) {
    if (const auto packaged = root / L"bin" / L"war3-map-editor.exe"; IsFile(packaged)) return packaged;
    if (const auto sibling = root / L"war3-map-editor.exe"; IsFile(sibling)) return sibling;
    const auto latestFile = root / L"最新版本.txt";
    if (!IsFile(latestFile)) {
        problem = L"没有找到 bin\\war3-map-editor.exe。\n\n请保持解压后的目录结构，不要把 Audastra.exe 单独移到别处。";
        return {};
    }
    std::ifstream input(latestFile, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    const auto directory = Audastra::Launcher::LatestVersionDirectory(text);
    if (directory.empty()) {
        problem = L"“最新版本.txt”里没有有效的 version_dir，无法确定要启动的版本。";
        return {};
    }
    const auto nested = root / fs::path(std::wstring(directory.begin(), directory.end())) / L"Audastra" / L"Audastra.exe";
    if (!IsFile(nested)) {
        problem = L"最新版本的启动程序不存在：\n" + nested.wstring();
        return {};
    }
    return nested;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int showCommand) {
    const auto root = ModuleDirectory();
    if (root.empty()) { ShowError(L"无法确定 Audastra.exe 所在目录。"); return 1; }
    std::wstring problem;
    const auto target = FindTarget(root, problem);
    if (target.empty()) { ShowError(problem); return 1; }

    std::wstring commandLine = L"\"" + target.wstring() + L"\"";
    if (const auto arguments = Audastra::Launcher::ArgumentsAfterProgram(GetCommandLineW()); !arguments.empty()) {
        commandLine += L' ';
        commandLine.append(arguments);
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = static_cast<WORD>(showCommand);
    PROCESS_INFORMATION process{};
    // 工作目录沿用调用方，命令行里的相对地图路径保持原意。
    if (!CreateProcessW(target.c_str(), commandLine.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process)) {
        const auto code = GetLastError();
        ShowError(L"启动失败：\n" + target.wstring() + L"\n\nWindows 错误码 " + std::to_wstring(code) + L"。");
        return 1;
    }
    AllowSetForegroundWindow(process.dwProcessId);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return 0;
}
