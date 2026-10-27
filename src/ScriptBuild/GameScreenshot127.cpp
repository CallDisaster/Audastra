#include "GameScreenshot127.h"

#include <array>
#include <cstring>
#include <set>
#include <string>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif

namespace War3Map::Script::detail {
namespace {

// Game.dll 1.27a（SHA e04d…0c3a）中的偏移，事实取自同一作者的 WarVK 研究。
constexpr std::uint32_t kScreenshotEventRva = 0x175530; // 截图键事件回调：事件号 0x212 时置位标志
constexpr std::uint32_t kScreenshotFlagRva = 0xBE3A38;  // 帧末检查的截图标志，存完后由游戏清零

Result<std::filesystem::path> Fail(const char* code, std::string message) {
    return Result<std::filesystem::path>::Failure("Map.Test", code, std::move(message));
}

#ifdef _WIN32
// mov edx,[flag]; mov eax,1; cmp dword [ecx],0x212; cmovz edx,eax; mov [flag],edx; ret
std::array<unsigned char, 27> ExpectedEvent(std::uint32_t flag) {
    std::array<unsigned char, 27> code{0x8b, 0x15, 0, 0, 0, 0, 0xb8, 0x01, 0x00, 0x00, 0x00, 0x81, 0x39, 0x12, 0x02, 0x00, 0x00,
        0x0f, 0x44, 0xd0, 0x89, 0x15, 0, 0, 0, 0, 0xc3};
    std::memcpy(code.data() + 2, &flag, sizeof(flag));
    std::memcpy(code.data() + 22, &flag, sizeof(flag));
    return code;
}

std::set<std::wstring> Screenshots(const std::filesystem::path& directory) {
    std::set<std::wstring> names;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(directory, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        auto name = it->path().filename().wstring();
        for (auto& c : name) if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
        if (name.size() > 4 && name.ends_with(L".tga")) names.insert(it->path().filename().wstring());
    }
    return names;
}

bool Read(HANDLE process, std::uintptr_t address, void* buffer, std::size_t bytes) {
    SIZE_T read = 0;
    return ReadProcessMemory(process, reinterpret_cast<LPCVOID>(address), buffer, bytes, &read) && read == bytes;
}
bool Write(HANDLE process, std::uintptr_t address, std::uint32_t value) {
    SIZE_T written = 0;
    return WriteProcessMemory(process, reinterpret_cast<LPVOID>(address), &value, sizeof(value), &written) && written == sizeof(value);
}
#endif

} // namespace

Result<std::filesystem::path> RequestGame127Screenshot(void* processHandle, const std::filesystem::path& gameRoot, std::uint32_t timeoutMs) {
#ifdef _WIN32
    const auto process = static_cast<HANDLE>(processHandle);
    std::array<HMODULE, 1024> modules{};
    DWORD needed = 0;
    if (!K32EnumProcessModulesEx(process, modules.data(), static_cast<DWORD>(sizeof(modules)), &needed, LIST_MODULES_32BIT))
        return Fail("MAP_TEST_SCREENSHOT_MODULES", "无法列出游戏进程的模块（Win32=" + std::to_string(GetLastError()) + "）。");
    const auto expected = (gameRoot / L"Game.dll").lexically_normal();
    std::uintptr_t base = 0;
    for (DWORD index = 0; index < needed / sizeof(HMODULE) && index < modules.size(); ++index) {
        std::array<wchar_t, 32768> path{};
        const auto length = K32GetModuleFileNameExW(process, modules[index], path.data(), static_cast<DWORD>(path.size()));
        if (length && CompareStringOrdinal(path.data(), static_cast<int>(length), expected.c_str(), -1, TRUE) == CSTR_EQUAL) {
            base = reinterpret_cast<std::uintptr_t>(modules[index]);
            break;
        }
    }
    if (!base) return Fail("MAP_TEST_SCREENSHOT_GAME_MODULE", "游戏还没有载入所选目录的 Game.dll，暂时不能截图。");
    const auto flag = base + kScreenshotFlagRva;
    std::array<unsigned char, 27> code{};
    if (base + kScreenshotFlagRva > 0xffffffffull || !Read(process, base + kScreenshotEventRva, code.data(), code.size())
        || code != ExpectedEvent(static_cast<std::uint32_t>(flag)))
        return Fail("MAP_TEST_SCREENSHOT_UNSUPPORTED", "游戏自带截图只支持 1.27a；这个 Game.dll 的截图入口不符。");
    std::uint32_t pending = 0;
    if (!Read(process, flag, &pending, sizeof(pending)))
        return Fail("MAP_TEST_SCREENSHOT_MEMORY", "无法读取游戏的截图状态。");
    if (pending) return Fail("MAP_TEST_SCREENSHOT_BUSY", "游戏上一张截图还没存完，稍后再试。");
    const auto directory = gameRoot / L"Screenshots";
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    const auto before = Screenshots(directory);
    if (!Write(process, flag, 1)) return Fail("MAP_TEST_SCREENSHOT_MEMORY", "无法向游戏发出截图请求（Win32=" + std::to_string(GetLastError()) + "）。");
    const auto started = GetTickCount64();
    ULONGLONG cleared = 0;
    while (GetTickCount64() - started < timeoutMs) {
        Sleep(50);
        if (WaitForSingleObject(process, 0) == WAIT_OBJECT_0) return Fail("MAP_TEST_SCREENSHOT_ENDED", "游戏在截图前退出了。");
        if (!Read(process, flag, &pending, sizeof(pending)) || pending) continue;
        // 标志清零表示游戏的保存函数已经返回；新文件不再被写入（可只读独占打开）才算写完。
        if (!cleared) cleared = GetTickCount64();
        for (const auto& name : Screenshots(directory)) {
            if (before.contains(name)) continue;
            const auto path = directory / name;
            const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            if (file == INVALID_HANDLE_VALUE) continue;
            CloseHandle(file);
            return Result<std::filesystem::path>::Success(path);
        }
        if (GetTickCount64() - cleared >= 1000)
            return Fail("MAP_TEST_SCREENSHOT_NOT_SAVED", "游戏处理了截图请求，但没有在 Screenshots 目录写出新图片（磁盘空间或目录权限）。");
    }
    // 没有画出新帧：撤回标志，免得以后突然多出一张截图。
    if (Read(process, flag, &pending, sizeof(pending)) && pending) (void)Write(process, flag, 0);
    return Fail("MAP_TEST_SCREENSHOT_TIMEOUT", "游戏在 " + std::to_string(timeoutMs / 1000) + " 秒内没有画出新的一帧（可能最小化、暂停或卡在载入）。");
#else
    (void)processHandle; (void)gameRoot; (void)timeoutMs;
    return Fail("MAP_TEST_SCREENSHOT_UNSUPPORTED", "当前平台不支持游戏截图。");
#endif
}

} // namespace War3Map::Script::detail
