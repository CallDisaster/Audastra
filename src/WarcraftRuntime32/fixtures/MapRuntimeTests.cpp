#include "../RuntimeFiles.h"
#include "audastra_japi.h"
#include <iostream>
#include <set>
#include <stdexcept>

namespace {
unsigned checks = 0;
void Check(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
struct Module {
    HMODULE value = nullptr;
    explicit Module(const wchar_t* path)
        : value(LoadLibraryExW(path, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32)) {}
    ~Module() { if (value) FreeLibrary(value); }
};
void Contract(HMODULE module) {
    const auto* base = reinterpret_cast<const unsigned char*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    Check(dos->e_magic == IMAGE_DOS_SIGNATURE, "PE DOS header");
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    Check(nt->Signature == IMAGE_NT_SIGNATURE && nt->FileHeader.Machine == IMAGE_FILE_MACHINE_I386
        && nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC
        && (nt->FileHeader.Characteristics & IMAGE_FILE_DLL), "PE32 x86 DLL");
    const auto& exports = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    Check(exports.VirtualAddress && exports.Size, "export table exists");
    const auto* table = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + exports.VirtualAddress);
    Check(table->NumberOfNames == 1, "only one named public entry");
    const auto* names = reinterpret_cast<const DWORD*>(base + table->AddressOfNames);
    Check(std::string_view(reinterpret_cast<const char*>(base + names[0])) == "WarVKAIInit", "undecorated export");
    const auto& imports = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    Check(imports.VirtualAddress && imports.Size, "import table exists");
    const std::set<std::string> system{"kernel32.dll", "user32.dll", "advapi32.dll", "bcrypt.dll",
        "gdi32.dll", "ole32.dll", "shell32.dll"};
    const auto* entry = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + imports.VirtualAddress);
    unsigned count = 0;
    for (; entry->Name; ++entry) {
        Check(++count <= 16, "bounded import table");
        std::string name(reinterpret_cast<const char*>(base + entry->Name));
        for (auto& character : name) if (character >= 'A' && character <= 'Z') character += 'a' - 'A';
        Check(system.contains(name), "only explicit system DLLs; no dynamic CRT or companion runtime");
        std::cout << "import=" << name << '\n';
    }
    Check(!nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT].Size, "no delayed companion dependency");
}
}

int wmain(int argc, wchar_t** argv) {
    try {
        Check(argc == 3, "usage: map-runtime-tests <runtime.dll> <fixture-Game.dll>");
        Check(!GetModuleHandleW(L"Game.dll"), "fixture starts without a game");
        Module runtime(argv[1]);
        Check(runtime.value != nullptr, "runtime loads with system-only dependency search");
        Contract(runtime.value);
        const auto initialize = reinterpret_cast<int(__cdecl*)()>(GetProcAddress(runtime.value, "WarVKAIInit"));
        Check(initialize != nullptr, "cdecl entry can be resolved");
        for (unsigned repeat = 0; repeat < 20; ++repeat)
            Check(initialize() == AJ_POLICY, "no game must reject safely on every call");
        Module wrongGame(argv[2]);
        Check(wrongGame.value != nullptr && GetModuleHandleW(L"Game.dll") == wrongGame.value, "wrong image fixture loaded");
        for (unsigned repeat = 0; repeat < 20; ++repeat)
            Check(initialize() == AJ_POLICY, "unsupported game must reject before ABI/hooks");
        std::cout << "Standalone DLL contract: " << checks << " checks passed; no actual game launched\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Standalone DLL contract failed: " << error.what() << '\n'; return 1;
    }
}
