#include "../Japi/HookSite.h"
#include "../OwnLua/Pe32OriginalCode.h"
#include "../RuntimeFiles.h"
#include <detours.h>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <cstring>
namespace {
using namespace Audastra::Runtime32;
using namespace Audastra::Runtime32::Japi;
using Fn = void(__cdecl*)();
Fn nextB = nullptr, nextC = nullptr;
std::vector<int> order;
void __cdecl Record(int n) { order.push_back(n); }
void __cdecl HookB() { Record(10); nextB(); Record(11); }
void __cdecl HookC() { Record(40); nextC(); Record(41); }
void Check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
std::vector<unsigned char> ReadFileImage(const std::filesystem::path& path) {
    Check(_wcsicmp(path.extension().c_str(), L".dll") == 0, "only explicit DLL images, never AI files");
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    Check(!!file && file.tellg() > 0 && file.tellg() < 64 * 1024 * 1024, "bounded fixture image");
    std::vector<unsigned char> bytes(static_cast<std::size_t>(file.tellg())); file.seekg(0);
    file.read(reinterpret_cast<char*>(bytes.data()), bytes.size()); Check(!!file, "read explicit non-AI fixture image"); return bytes;
}
void Patch(void* address, std::span<const unsigned char> value) {
    DWORD previous = 0, ignored = 0;
    Check(VirtualProtect(address, value.size(), PAGE_EXECUTE_READWRITE, &previous), "owned fixture patch protection");
    std::memcpy(address, value.data(), value.size()); FlushInstructionCache(GetCurrentProcess(), address, value.size());
    Check(VirtualProtect(address, value.size(), previous, &ignored), "restore fixture page permissions");
}
void Jump(void* address, void* destination) {
    std::array<unsigned char, 5> bytes{0xe9};
    const auto relative = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(destination) - reinterpret_cast<std::uintptr_t>(address) - 5);
    std::memcpy(bytes.data() + 1, &relative, 4); Patch(address, bytes);
}
void Attach(const HookSite& site, Fn& next, void* callback) {
    Check(DetourTransactionBegin() == NO_ERROR, "fixture begin");
    const auto status = AttachQualifiedHook(site, reinterpret_cast<void**>(&next), callback);
    if (status != NO_ERROR) { DetourTransactionAbort(); Check(false, "qualified actual Detours attach"); }
    Check(site.StillMatches(), "site retained before atomic commit");
    Check(DetourTransactionCommit() == NO_ERROR, "fixture commit");
}
void Detach(std::uintptr_t entry, Fn& next, void* callback) {
    Check(HookIsHead(entry, callback), "only current owner may detach head");
    Check(DetourTransactionBegin() == NO_ERROR && DetourDetach(reinterpret_cast<void**>(&next), callback) == NO_ERROR
        && DetourTransactionCommit() == NO_ERROR, "LIFO fixture detach");
}
}
int wmain(int argc, wchar_t** argv) {
    try {
        Check(argc == 3 || argc == 4, "base and provider fixture paths, optional verified game image");
        auto base = LoadLibraryW(argv[1]), provider = LoadLibraryW(argv[2]); Check(base && provider, "own fixture modules load");
        const auto entry = reinterpret_cast<Fn>(GetProcAddress(base, "ChainEntry"));
        const auto absolute = GetProcAddress(base, "ChainAbsolute");
        const auto count = reinterpret_cast<int(__cdecl*)()>(GetProcAddress(base, "ChainCount"));
        const auto record = reinterpret_cast<void(__cdecl*)(void(__cdecl*)(int))>(GetProcAddress(base, "ChainSetRecord"));
        const auto gate = reinterpret_cast<void*(__cdecl*)(int)>(GetProcAddress(provider, "ChainGate"));
        const auto data = reinterpret_cast<void*(__cdecl*)()>(GetProcAddress(provider, "ChainData"));
        const auto setNext = reinterpret_cast<void(__cdecl*)(Fn, void(__cdecl*)(int))>(GetProcAddress(provider, "ChainSetNext"));
        auto hookA = GetProcAddress(provider, "ChainProvider"); Check(entry && absolute && count && record && gate && setNext && hookA && data, "fixture exports");
        auto disk = ReadFileImage(argv[1]); std::array<unsigned char, 32> original{}, expectedAbsolute{};
        const auto at = reinterpret_cast<std::uintptr_t>(entry);
        Check(OwnLua::Pe32OriginalCode(disk, reinterpret_cast<std::uint32_t>(base), static_cast<std::uint32_t>(at - reinterpret_cast<std::uintptr_t>(base)), original)
            && std::memcmp(original.data(), entry, original.size()) == 0 && original[0] == 0xe8, "original CALL window from actual file");
        Check(OwnLua::Pe32OriginalCode(disk, reinterpret_cast<std::uint32_t>(base), static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(absolute) - reinterpret_cast<std::uintptr_t>(base)), expectedAbsolute)
            && std::memcmp(expectedAbsolute.data(), absolute, expectedAbsolute.size()) == 0, "HIGHLOW relocated absolute code matches actual loaded fixture");
        std::array<unsigned char, 32> wrongBase{};
        Check(OwnLua::Pe32OriginalCode(disk, reinterpret_cast<std::uint32_t>(base) + 0x10000,
            static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(absolute) - reinterpret_cast<std::uintptr_t>(base)), wrongBase)
            && !HookSite(reinterpret_cast<std::uintptr_t>(absolute), wrongBase, false).Approved(), "incorrect relocation cannot masquerade as stock code");
        Check(!OwnLua::Pe32OriginalCode(std::span<const unsigned char>(disk.data(), 20), 0x20000000, 0, expectedAbsolute), "truncated PE refused");
        auto malformed = disk; IMAGE_DOS_HEADER dos{}; std::memcpy(&dos, malformed.data(), sizeof(dos));
        const auto relocDirectory = dos.e_lfanew + offsetof(IMAGE_NT_HEADERS32, OptionalHeader) + offsetof(IMAGE_OPTIONAL_HEADER32, DataDirectory)
            + IMAGE_DIRECTORY_ENTRY_BASERELOC * sizeof(IMAGE_DATA_DIRECTORY);
        std::memset(malformed.data() + relocDirectory, 0, sizeof(IMAGE_DATA_DIRECTORY));
        Check(!OwnLua::Pe32OriginalCode(malformed, 0x20000000, static_cast<std::uint32_t>(at - reinterpret_cast<std::uintptr_t>(base)), expectedAbsolute), "missing relocations cannot guess rebasing");
        record(Record); Fn nextA = entry;
        Jump(gate(1), reinterpret_cast<void*>(hookA)); Jump(gate(0), gate(1));
        HookSite stock(at, original, true); Check(stock.Approved() && !stock.Chained(), "stock site accepted");
        Attach(stock, nextA, gate(0)); setNext(nextA, Record);
        Check(!HookSite(at, original, false).Approved(), "unapproved points refuse existing jumps");
        auto chained = std::make_unique<HookSite>(at, original, true);
        Check(chained->Approved() && chained->Chained(), "prior module and bounded direct jump chain accepted");
        const auto description = DescribeHookSite(at);
        Check(description.find("bytes=e9") != description.npos && description.find("jump_owner=" + std::filesystem::path(argv[2]).filename().string() + "+0x") != description.npos
            && chained->ObservationCount() >= 4, "test diagnostics identify actual bytes and module-relative target without fixed provider names");
        std::array<unsigned char, 5> head{}; std::memcpy(head.data(), entry, 5);
        auto rejectTarget = [&](void* target) { Jump(reinterpret_cast<void*>(entry), target); Check(!HookSite(at, original, true).Approved(), "invalid jump target refused"); Patch(reinterpret_cast<void*>(entry), head); };
        rejectTarget(data()); rejectTarget(reinterpret_cast<void*>(entry));
        auto privateCode = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE); Check(privateCode, "owned private negative fixture");
        rejectTarget(privateCode); VirtualFree(privateCode, 0, MEM_RELEASE);
        auto badTail = original; badTail[7] ^= 1; Patch(reinterpret_cast<void*>(at + 5), std::span(badTail).subspan(5));
        Check(!HookSite(at, original, true).Approved(), "changed tail refused"); Patch(reinterpret_cast<void*>(at + 5), std::span(original).subspan(5));
        Jump(gate(1), gate(0)); Check(!HookSite(at, original, true).Approved(), "cyclic chain refused"); Jump(gate(1), reinterpret_cast<void*>(hookA));
        for (int i = 0; i < 9; ++i) Jump(gate(i), gate(i + 1)); Jump(gate(9), reinterpret_cast<void*>(hookA));
        Check(!HookSite(at, original, true).Approved(), "excessive jump depth refused"); Jump(gate(1), reinterpret_cast<void*>(hookA));
        auto terminalJump = std::array<unsigned char, 5>{0xff, 0x25, 0, 0, 0}; Patch(gate(1), terminalJump);
        Check(!HookSite(at, original, true).Approved(), "indirect target jump refused"); Jump(gate(1), reinterpret_cast<void*>(hookA));
        Check(chained->StillMatches(), "restored fixture remains exact");
        auto mutated = head; mutated[0] = 0xeb; Patch(reinterpret_cast<void*>(entry), mutated);
        Check(!HookSite(at, original, true).Approved() && !chained->StillMatches(), "short jump and changed snapshot refused");
        Fn raceNext = entry;
        Check(AttachQualifiedHook(*chained, reinterpret_cast<void**>(&raceNext), HookB) == ERROR_INVALID_DATA && raceNext == entry, "race cannot attach without matching snapshot");
        Patch(reinterpret_cast<void*>(entry), head);
        Check(DetourTransactionBegin() == NO_ERROR && AttachQualifiedHook(*chained, reinterpret_cast<void**>(&raceNext), HookB) == NO_ERROR,
            "pending valid transaction fixture");
        Patch(reinterpret_cast<void*>(at + 7), std::span<const unsigned char>(badTail).subspan(7, 1));
        Check(!chained->StillMatches(), "tail change after attach is detected before commit");
        Check(DetourTransactionAbort() == NO_ERROR && raceNext == entry && std::memcmp(head.data(), entry, 5) == 0,
            "aborting raced transaction does not overwrite prior head or publish own trampoline");
        Patch(reinterpret_cast<void*>(at + 7), std::span<const unsigned char>(original).subspan(7, 1));
        nextB = entry; Attach(*chained, nextB, HookB);
        Check(FreeLibrary(provider), "external owner releases original loader reference");
        HMODULE retained = nullptr; Check(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(hookA), &retained) && retained == provider, "chain lease prevents predecessor DLL unload");
        order.clear(); entry(); Check(order == std::vector<int>({10,20,30,21,11}) && count() == 1, "old hook and original CALL execute exactly once");
        auto later = std::make_unique<HookSite>(at, original, true); Check(later->Approved() && later->Chained(), "later provider can chain at same entry");
        nextC = entry; Attach(*later, nextC, HookC);
        for (int i = 0; i < 4; ++i) { order.clear(); entry(); Check(order == std::vector<int>({40,10,20,30,21,11,41}) && count() == i + 2, "multiple initialization passes preserve order and call count"); }
        Check(!HookIsHead(at, HookB) && HookIsHead(at, HookC), "non-head removal refused without overwriting newer provider");
        Detach(at, nextC, HookC); later.reset();
        order.clear(); entry(); Check(order == std::vector<int>({10,20,30,21,11}) && count() == 6, "removing newest layer restores full prior chain");
        Detach(at, nextB, HookB);
        order.clear(); entry(); Check(order == std::vector<int>({20,30,21}) && count() == 7, "removing middle after head preserves predecessor and original call");
        Detach(at, nextA, gate(0)); chained.reset();
        order.clear(); entry(); Check(order == std::vector<int>({30}) && count() == 8 && std::memcmp(original.data(), entry, original.size()) == 0, "LIFO restoration returns exact original bytes and CALL semantics");
        if (argc == 4) {
            Check(_wcsicmp(std::filesystem::path(argv[3]).extension().c_str(), L".dll") == 0, "stock input must be a DLL");
            Handle file(OpenLockedFile(argv[3])); Check(file.value != INVALID_HANDLE_VALUE && Sha256(file.value) == "e04d1716603c075eb0c8e1e21cf1093a664adc5249efab396bfa08d7b09d0c3a", "explicit known stock image only");
            const auto game = ReadFileImage(argv[3]);
            for (const auto rva : {0x1e31d0u,0x1e9a50u,0x7dd070u,0x1f3a10u,0x1f3af0u,0x1f3570u,0x1cf9d0u}) {
                std::array<unsigned char,32> rebased{}; Check(OwnLua::Pe32OriginalCode(game,0x5e400000,rva,rebased),"known stock relocation window");
            }
            std::cout << "known_stock_seven_relocation_windows=true\n";
        }
        FreeLibrary(base);
        std::cout << "chain_order=true\noriginal_call_exactly_once=true\nmultiple_layers=true\nowner_retained=true\nlifo_restore=true\nunknown_targets_refused=true\nrace_refused=true\ngame_started=false\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
