#include <Windows.h>
#include <string>

// 原创替身只记录工作线程，不接触游戏、钩子或通讯。
extern "C" __declspec(dllexport) int __cdecl WarVKAIInit() {
    wchar_t prefix[160]{};
    if (!GetEnvironmentVariableW(L"AUDASTRA_MODULE_ENTRY_FIXTURE", prefix, 160)) return 1;
    const auto mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, prefix);
    if (!mapping) return 2;
    const auto state = static_cast<volatile LONG*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, 0));
    if (!state) { CloseHandle(mapping); return 3; }
    InterlockedIncrement(state);
    InterlockedExchange(state + 1, static_cast<LONG>(GetCurrentThreadId()));
    const auto gateName = std::wstring(prefix) + L"-gate";
    const auto gate = OpenEventW(SYNCHRONIZE, FALSE, gateName.c_str());
    if (gate) { WaitForSingleObject(gate, 5000); CloseHandle(gate); }
    InterlockedExchange(state + 2, 1);
    UnmapViewOfFile(const_cast<LONG*>(state)); CloseHandle(mapping);
    return 0;
}
extern "C" __declspec(dllexport) int __cdecl WarVKRuntimeOwnerV1() { return 0; }
