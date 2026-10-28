#include "GameScreenshot127.h"
#include "War3MapCore/Hash.h"

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

template<class T = std::filesystem::path> Result<T> Fail(const char* code, std::string message) {
    return Result<T>::Failure("Map.Test", code, std::move(message));
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

Result<VerifiedScreenshot127> QualifyGame127Screenshot(void* processHandle, std::uintptr_t base, const std::filesystem::path& expected) {
#ifdef _WIN32
    const auto process = static_cast<HANDLE>(processHandle);
    // 只向已知映像的数据槽置位，不执行入口代码。入口可能已由其他合法组件改写，
    // 因此身份取自持有只读租约的完整磁盘映像、映射PE头和目标数据页，不能仅凭跳转指令猜版本。
    const auto handle = CreateFileW(expected.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    const auto image = std::shared_ptr<void>(handle, [](void* h) { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); });
    BY_HANDLE_FILE_INFORMATION info{};
    const auto hash = handle == INVALID_HANDLE_VALUE ? Result<std::string>{} : Sha256Handle(handle);
    if (!hash.Succeeded() || *hash.value != "e04d1716603c075eb0c8e1e21cf1093a664adc5249efab396bfa08d7b09d0c3a"
        || !GetFileInformationByHandle(handle, &info) || info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))
        return Fail<VerifiedScreenshot127>("MAP_TEST_SCREENSHOT_UNSUPPORTED", "截图只支持已核验完整身份的经典1.27a Game.dll。");
    std::array<unsigned char, 1024> header{};
    std::array<unsigned char, 1024> diskHeader{}; LARGE_INTEGER zero{}; DWORD headerBytes = 0;
    if (!SetFilePointerEx(handle, zero, nullptr, FILE_BEGIN)
        || !ReadFile(handle, diskHeader.data(), static_cast<DWORD>(diskHeader.size()), &headerBytes, nullptr)
        || headerBytes != diskHeader.size() || !Read(process, base, header.data(), header.size()))
        return Fail<VerifiedScreenshot127>("MAP_TEST_SCREENSHOT_UNSUPPORTED", "Game.dll映射头与已核验磁盘映像不符，拒绝截图。");
    // 加载器可将PE32的ImageBase改成实际映射基址；只允许这一处可证明的装载变化。
    IMAGE_DOS_HEADER dos{}; std::memcpy(&dos, diskHeader.data(), sizeof(dos));
    if (dos.e_lfanew < 0 || static_cast<std::size_t>(dos.e_lfanew) > diskHeader.size() - sizeof(IMAGE_NT_HEADERS32)
        || base > 0xffffffffull)
        return Fail<VerifiedScreenshot127>("MAP_TEST_SCREENSHOT_UNSUPPORTED", "截图映像的PE32基址不可确认。");
    const bool originalHeader = header == diskHeader;
    const auto imageBaseOffset = static_cast<std::size_t>(dos.e_lfanew) + offsetof(IMAGE_NT_HEADERS32, OptionalHeader)
        + offsetof(IMAGE_OPTIONAL_HEADER32, ImageBase);
    const auto actualBase = static_cast<std::uint32_t>(base);
    std::memcpy(diskHeader.data() + imageBaseOffset, &actualBase, sizeof(actualBase));
    if (!originalHeader && header != diskHeader)
        return Fail<VerifiedScreenshot127>("MAP_TEST_SCREENSHOT_UNSUPPORTED", "Game.dll映射头除实际基址外仍与已核验映像不符，拒绝截图。");
    if (base > 0xffffffffull - kScreenshotFlagRva)
        return Fail<VerifiedScreenshot127>("MAP_TEST_SCREENSHOT_UNSUPPORTED", "截图数据槽超出PE32地址范围。");
    const auto flag = base + kScreenshotFlagRva;
    std::array<unsigned char, 27> code{};
    MEMORY_BASIC_INFORMATION page{};
    if (flag > 0xffffffffull || !VirtualQueryEx(process, reinterpret_cast<void*>(flag), &page, sizeof(page))
        || page.AllocationBase != reinterpret_cast<void*>(base) || page.Type != MEM_IMAGE || page.State != MEM_COMMIT
        || (page.Protect & (PAGE_GUARD | PAGE_NOACCESS))
        || !((page.Protect & 0xff) == PAGE_READWRITE || (page.Protect & 0xff) == PAGE_WRITECOPY)
        || flag + sizeof(std::uint32_t) > reinterpret_cast<std::uintptr_t>(page.BaseAddress) + page.RegionSize
        || !Read(process, base + kScreenshotEventRva, code.data(), code.size()))
        return Fail<VerifiedScreenshot127>("MAP_TEST_SCREENSHOT_UNSUPPORTED", "已核验映像的截图数据页或入口不可确认，拒绝截图。");
    return Result<VerifiedScreenshot127>::Success({flag, code != ExpectedEvent(static_cast<std::uint32_t>(flag)), image});
#else
    (void)processHandle; (void)base; (void)expected;
    return Fail<VerifiedScreenshot127>("MAP_TEST_SCREENSHOT_UNSUPPORTED", "当前平台不支持游戏截图。");
#endif
}

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
            base = reinterpret_cast<std::uintptr_t>(modules[index]); break;
        }
    }
    if (!base) return Fail("MAP_TEST_SCREENSHOT_GAME_MODULE", "游戏还没有载入所选目录的 Game.dll，暂时不能截图。");
    const auto qualified = QualifyGame127Screenshot(process, base, expected);
    if (!qualified.Succeeded()) return {std::nullopt, qualified.diagnostics};
    const auto flag = qualified.value->flag;
    const bool eventChanged = qualified.value->eventChanged;
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
            auto result = Result<std::filesystem::path>::Success(path);
            result.diagnostics.push_back({DiagnosticSeverity::Info, "Map.Test", "MAP_TEST_SCREENSHOT_IDENTITY",
                std::string("完整1.27a磁盘身份、映射头与数据页已核验；入口字节") + (eventChanged ? "已改变" : "与原版一致")
                    + "；只请求游戏自带截图标志，不执行或改写入口。", std::nullopt});
            return result;
        }
        if (GetTickCount64() - cleared >= 1000)
            return Fail("MAP_TEST_SCREENSHOT_NOT_SAVED", "游戏处理了截图请求，但没有在 Screenshots 目录写出新图片（磁盘空间或目录权限）。");
    }
    // 没有画出新帧：撤回标志，免得以后突然多出一张截图。
    if (Read(process, flag, &pending, sizeof(pending)) && pending) (void)Write(process, flag, 0);
    return Fail("MAP_TEST_SCREENSHOT_TIMEOUT", "完整1.27a身份已核验；入口字节" + std::string(eventChanged ? "已改变" : "与原版一致")
        + "。游戏在 " + std::to_string(timeoutMs / 1000) + " 秒内没有画出新的一帧（可能最小化、暂停或卡在载入）。");
#else
    (void)processHandle; (void)gameRoot; (void)timeoutMs;
    return Fail("MAP_TEST_SCREENSHOT_UNSUPPORTED", "当前平台不支持游戏截图。");
#endif
}

} // namespace War3Map::Script::detail
