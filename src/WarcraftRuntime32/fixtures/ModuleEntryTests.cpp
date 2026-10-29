#include <Windows.h>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void Check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
template<class Predicate> bool Until(Predicate predicate) {
    const auto until = GetTickCount64() + 5000;
    do { if (predicate()) return true; Sleep(1); } while (GetTickCount64() < until);
    return false;
}
}
int wmain(int argc, wchar_t** argv) {
    try {
        Check(argc == 3, "module-entry-tests <AI-fixture> <module-fixture>");
        const auto name = L"Local\\audastra-module-entry-" + std::to_wstring(GetCurrentProcessId());
        const auto mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 12, name.c_str());
        Check(mapping != nullptr && GetLastError() != ERROR_ALREADY_EXISTS, "unique fixture mapping");
        const auto state = static_cast<volatile LONG*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, 12));
        Check(state != nullptr, "fixture state mapped");
        const auto gateName = name + L"-gate";
        const auto gate = CreateEventW(nullptr, TRUE, FALSE, gateName.c_str());
        Check(gate != nullptr, "fixture worker gate");
        Check(SetEnvironmentVariableW(L"AUDASTRA_MODULE_ENTRY_FIXTURE", name.c_str()) != FALSE, "owned host fixture selector");
        const auto ai = LoadLibraryW(argv[1]); Check(ai != nullptr, "AI fixture loads");
        Sleep(20); Check(state[0] == 0, "AI product does not auto-start");
        FreeLibrary(ai);
        const auto module = LoadLibraryW(argv[2]); Check(module != nullptr, "module fixture loads");
        Check(Until([&] { return state[0] == 1 && state[1] != 0; }), "worker starts without calling any export");
        Check(static_cast<DWORD>(state[1]) != GetCurrentThreadId(), "initialization runs on separate thread");
        Check(FreeLibrary(module) != FALSE, "host releases original loader reference");
        HMODULE observed = nullptr;
        Check(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, argv[2], &observed) && observed == module,
            "worker retained reference keeps code loaded after host release");
        SetEvent(gate);
        Check(Until([&] { return state[2] == 1; }), "worker completed through shared entry");
        Check(Until([&] { HMODULE current = nullptr; return !GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, argv[2], &current); }), "uninstalled worker module releases own reference on exit");
        Check(state[0] == 1, "single module attach schedules exactly one entry");
        SetEnvironmentVariableW(L"AUDASTRA_MODULE_ENTRY_FIXTURE", nullptr);
        UnmapViewOfFile(const_cast<LONG*>(state)); CloseHandle(mapping); CloseHandle(gate);
        std::cout << "module worker lifetime and AI separation passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
