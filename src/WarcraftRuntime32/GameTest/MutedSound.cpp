#include "MutedSound.h"

#include <windows.h>
#include <detours.h>

namespace Audastra::Runtime32::GameTest {
namespace {

// Miles 以 stdcall 修饰名导出；Game.dll 按这个名字导入。
constexpr char kOpenDigitalDriver[] = "_AIL_open_digital_driver@16";
using OpenDigitalDriverFunction = void*(__stdcall*)(unsigned, int, int, unsigned);

OpenDigitalDriverFunction originalOpen = nullptr;

// 返回空句柄即“没有可用声卡”，与没有声卡的电脑相同。
void* __stdcall NoDigitalDriver(unsigned, int, int, unsigned) { return nullptr; }

} // namespace

bool InstallMutedSound(std::string& error) {
    if (originalOpen) { error = "静音钩子已经安装。"; return false; }
    const auto miles = GetModuleHandleW(L"Mss32.dll");
    if (!miles) { error = "游戏没有装载 Mss32.dll。"; return false; }
    const auto open = reinterpret_cast<OpenDigitalDriverFunction>(GetProcAddress(miles, kOpenDigitalDriver));
    if (!open) { error = "Mss32.dll 没有导出打开数字声卡的函数。"; return false; }
    originalOpen = open;
    LONG code = DetourTransactionBegin();
    if (code == NO_ERROR) {
        code = DetourUpdateThread(GetCurrentThread());
        if (code == NO_ERROR) code = DetourAttach(reinterpret_cast<PVOID*>(&originalOpen), reinterpret_cast<PVOID>(NoDigitalDriver));
        if (code == NO_ERROR) code = DetourTransactionCommit();
        else DetourTransactionAbort();
    }
    if (code != NO_ERROR) { originalOpen = nullptr; error = "安装静音钩子失败：" + std::to_string(code); return false; }
    return true;
}

} // namespace Audastra::Runtime32::GameTest
