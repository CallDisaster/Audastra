#include "BackgroundPlay127.h"

#include <detours.h>

#include <atomic>
#include <cstdint>
#include <cstring>

namespace Audastra::Runtime32::GameTest {
namespace {

// Game.dll 1.27a（SHA e04d…0c3a）中的偏移。
constexpr std::uintptr_t kGamePauseRva = 0x354240;      // 界面对象上的暂停/继续游戏
constexpr std::uintptr_t kIdleSleepRva = 0x1552E0;      // 失焦后 Storm 消息循环的空闲等待毫秒
constexpr std::uintptr_t kIdleSleepValueRva = 0xBDAC04; // 上一函数读取的全局毫秒数

// __fastcall：ecx 为界面对象，其后 5 个栈参数由被调方清理；pause 非零为暂停，零为继续。
using GamePauseFunction = void(__fastcall*)(void* ui, void* edx, BOOL pause,
    std::uint32_t a2, std::uint32_t a3, std::uint32_t a4, std::uint32_t a5);
using IdleSleepFunction = DWORD(__cdecl*)();

GamePauseFunction originalPause = nullptr;
IdleSleepFunction originalIdleSleep = nullptr;
std::atomic<bool> installed{false};

void __fastcall KeepRunning(void* ui, void* edx, BOOL pause,
    std::uint32_t a2, std::uint32_t a3, std::uint32_t a4, std::uint32_t a5) {
    if (pause) return;
    originalPause(ui, edx, pause, a2, a3, a4, a5);
}

DWORD __cdecl NoIdleSleep() { return 0; }

// mov eax, [Game+0xBDAC04]; ret —— 操作数按实际装载基址重定位后再比较。
bool IdleSleepMatches(const OwnLua::GameImage127& image) {
    const auto address = image.Base() + kIdleSleepRva;
    if (!image.Executable(address, 6)) return false;
    const auto* code = reinterpret_cast<const unsigned char*>(address);
    std::uint32_t operand = 0;
    std::memcpy(&operand, code + 1, sizeof(operand));
    return code[0] == 0xA1 && code[5] == 0xC3 && operand == static_cast<std::uint32_t>(image.Base() + kIdleSleepValueRva);
}

} // namespace

bool InstallBackgroundPlay127(const OwnLua::GameImage127& image, std::string& error) {
    if (installed.load()) { error = "后台运行钩子已经安装。"; return false; }
    constexpr unsigned char pausePrologue[]{0x55, 0x8b, 0xec, 0x81, 0xec, 0x6c, 0x01, 0x00, 0x00};
    if (!image.Matches(kGamePauseRva, pausePrologue) || !IdleSleepMatches(image)) {
        error = "Game.dll 的暂停或后台等待入口与 1.27a 不符，拒绝安装后台运行钩子。";
        return false;
    }
    originalPause = reinterpret_cast<GamePauseFunction>(image.Base() + kGamePauseRva);
    originalIdleSleep = reinterpret_cast<IdleSleepFunction>(image.Base() + kIdleSleepRva);
    LONG code = DetourTransactionBegin();
    if (code == NO_ERROR) {
        code = DetourUpdateThread(GetCurrentThread());
        if (code == NO_ERROR) code = DetourAttach(reinterpret_cast<PVOID*>(&originalPause), KeepRunning);
        if (code == NO_ERROR) code = DetourAttach(reinterpret_cast<PVOID*>(&originalIdleSleep), NoIdleSleep);
        if (code == NO_ERROR) code = DetourTransactionCommit();
        else DetourTransactionAbort();
    }
    if (code != NO_ERROR) { error = "安装后台运行钩子失败：" + std::to_string(code); return false; }
    installed = true;
    return true;
}

} // namespace Audastra::Runtime32::GameTest
