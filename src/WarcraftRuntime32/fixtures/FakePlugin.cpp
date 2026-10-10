#include "../RuntimeProtocol.h"

#ifdef AUDASTRA_FIXTURE_PLUGIN_BIT
extern "C" void Initialize() {
    auto* shared = Audastra::Runtime32::OpenChannel();
    if (!shared) RaiseException(ERROR_INVALID_DATA, 0, 0, nullptr);
    if (AUDASTRA_FIXTURE_PLUGIN_BIT == 1 && shared->fixtureInitializeDelayMs > 0) {
        InterlockedExchange(&shared->fixtureConcurrentEntered, 1);
        const auto start = GetTickCount64();
        while (!InterlockedCompareExchange(&shared->fixtureConcurrentWaitObserved, 0, 0) && GetTickCount64() - start < 2000) Sleep(1);
        Sleep(static_cast<DWORD>(shared->fixtureInitializeDelayMs));
    }
    InterlockedOr(&shared->fixturePluginBits, AUDASTRA_FIXTURE_PLUGIN_BIT);
    UnmapViewOfFile(shared);
}
#endif
BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH && wcsstr(GetCommandLineW(), L"probe-init-outside.w3x")) {
        auto* shared = Audastra::Runtime32::OpenChannel();
        if (shared && shared->probeEnabled) {
            wchar_t path[Audastra::Runtime32::kPathCapacity]{};
            wcscpy_s(path, shared->runtimeRoot); wcscat_s(path, L"\\malicious-dllmain.txt");
            const auto file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
            UnmapViewOfFile(shared);
        }
    }
    return TRUE;
}
