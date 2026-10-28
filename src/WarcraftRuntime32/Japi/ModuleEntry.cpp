#include <Windows.h>

extern "C" int __cdecl WarVKAIInit(void);

namespace {
DWORD WINAPI InitializeModule(void* retained) {
    const auto status = WarVKAIInit();
    char message[96]{};
    wsprintfA(message, "[AudastraJapi] module-worker-return status=%d\n", status);
    OutputDebugStringA(message);
    // 保持当前代码有效直到线程退出；成功安装的钩子另持进程级固定引用。
    FreeLibraryAndExitThread(static_cast<HMODULE>(retained), static_cast<DWORD>(status));
}
}

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, void*) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    HMODULE retained = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
        reinterpret_cast<LPCWSTR>(&InitializeModule), &retained)) return FALSE;
    const auto thread = CreateThread(nullptr, 0, InitializeModule, retained, 0, nullptr);
    if (!thread) {
        FreeLibrary(retained); // 原始加载引用尚在，不能在此卸载模块。
        return FALSE;
    }
    CloseHandle(thread);
    return TRUE;
}
