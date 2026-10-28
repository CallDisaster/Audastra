#include "../../src/ScriptBuild/GameScreenshot127.h"
#include "War3MapCore/Hash.h"
#include <windows.h>
#include <fstream>
#include <iostream>
#include <stdexcept>
// 仅将明确指定的原版映像映射为本进程数据；不LoadLibrary、不执行入口、不启动游戏。
int wmain(int argc, wchar_t** argv) {
    using namespace War3Map;
    using namespace War3Map::Script::detail;
    const auto require = [](bool value, const char* text) { if (!value) throw std::runtime_error(text); };
    struct Handles {
        HANDLE file = INVALID_HANDLE_VALUE, mapping = nullptr; void* view = nullptr;
        ~Handles() { if (view) UnmapViewOfFile(view); if (mapping) CloseHandle(mapping); if (file != INVALID_HANDLE_VALUE) CloseHandle(file); }
    } owned;
    try {
        require(argc == 3, "verified Game.dll path and nonexistent invalid fixture path required");
        const std::filesystem::path path = argv[1], invalid = argv[2];
        const auto before = Sha256File(path); require(before.Succeeded(), "source hash");
        require(*before.value == "e04d1716603c075eb0c8e1e21cf1093a664adc5249efab396bfa08d7b09d0c3a", "only approved fixed image");
        require(!std::filesystem::exists(invalid), "refuse overwrite fixture");
        owned.file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        require(owned.file != INVALID_HANDLE_VALUE, "hold original read lease");
        owned.mapping = CreateFileMappingW(owned.file, nullptr, PAGE_READONLY | SEC_IMAGE_NO_EXECUTE, 0, 0, nullptr);
        require(owned.mapping != nullptr, "map as image data without loader callbacks");
        owned.view = MapViewOfFileEx(owned.mapping, FILE_MAP_READ, 0, 0, 0, reinterpret_cast<void*>(0x64000000));
        require(owned.view != nullptr, "preferred image base free in own fixture process");
        const auto base = reinterpret_cast<std::uintptr_t>(owned.view);
        // 内核的数据映射未执行装载器的最终基址提交；仅在自己的COW头中模拟这个已知PE字段。
        DWORD headerPrevious = 0, headerIgnored = 0;
        auto* dos = static_cast<IMAGE_DOS_HEADER*>(owned.view);
        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(static_cast<unsigned char*>(owned.view) + dos->e_lfanew);
        require(VirtualProtect(owned.view, 1024, PAGE_WRITECOPY, &headerPrevious), "own loader-finalized PE header fixture");
        nt->OptionalHeader.ImageBase = static_cast<DWORD>(base);
        require(VirtualProtect(owned.view, 1024, headerPrevious, &headerIgnored), "restore finalized header protection");
        // SEC_IMAGE_NO_EXECUTE把数据也映成只读；只调整本进程的COW数据页，模拟真实映像的可写数据权限。
        DWORD dataPrevious = 0;
        require(VirtualProtect(static_cast<unsigned char*>(owned.view) + 0xbe3a38, 4, PAGE_WRITECOPY, &dataPrevious), "own image writable data fixture");
        auto qualified = QualifyGame127Screenshot(GetCurrentProcess(), base, path);
        for (const auto& d : qualified.diagnostics) std::cerr << d.code << ": " << d.message << '\n';
        if (!qualified.Succeeded()) {
            std::ifstream file(path, std::ios::binary); unsigned char bytes[1024]{}; file.read(reinterpret_cast<char*>(bytes), sizeof(bytes));
            unsigned count = 0;
            for (unsigned i = 0; i < sizeof(bytes); ++i) if (bytes[i] != static_cast<unsigned char*>(owned.view)[i]) {
                if (count++ < 12) std::cerr << "mapped_header_difference offset=" << i << " disk=" << static_cast<unsigned>(bytes[i])
                    << " mapped=" << static_cast<unsigned>(static_cast<unsigned char*>(owned.view)[i]) << '\n';
            }
            std::cerr << "mapped_header_difference_count=" << count << '\n';
        }
        require(qualified.Succeeded(), "stock identity and target page accepted");
        qualified.value.reset();
        require(VirtualProtect(owned.view, 1024, PAGE_WRITECOPY, &headerPrevious), "own DOS stub fixture");
        for (long i = 64; i < dos->e_lfanew; ++i) static_cast<unsigned char*>(owned.view)[i] ^= 0x5a;
        qualified = QualifyGame127Screenshot(GetCurrentProcess(), base, path);
        require(qualified.Succeeded() && qualified.value->dosStubChanged, "DOS stub mutations ignored within exact bounds");
        qualified.value.reset();
        for (long i = 64; i < dos->e_lfanew; ++i) static_cast<unsigned char*>(owned.view)[i] ^= 0x5a;
        const auto rejectByte = [&](std::size_t offset) {
            auto* bytes = static_cast<unsigned char*>(owned.view); bytes[offset] ^= 1;
            const auto refused = !QualifyGame127Screenshot(GetCurrentProcess(), base, path).Succeeded();
            bytes[offset] ^= 1; require(refused, "structural PE/DOS byte change must be rejected");
        };
        rejectByte(0); rejectByte(60); rejectByte(dos->e_lfanew); rejectByte(dos->e_lfanew + 8);
        rejectByte(dos->e_lfanew + 24 + 4);
        rejectByte(dos->e_lfanew + sizeof(IMAGE_NT_HEADERS32) + offsetof(IMAGE_SECTION_HEADER, Characteristics));
        require(VirtualProtect(owned.view, 1024, headerPrevious, &headerIgnored), "restore protected header after structural negatives");
        require(VirtualProtect(owned.view, 1024, PAGE_WRITECOPY, &headerPrevious), "own wrong image-base fixture");
        nt->OptionalHeader.ImageBase ^= 0x10000;
        require(!QualifyGame127Screenshot(GetCurrentProcess(), base, path).Succeeded(), "unknown mapped image base refused");
        nt->OptionalHeader.ImageBase = static_cast<DWORD>(base);
        require(VirtualProtect(owned.view, 1024, headerPrevious, &headerIgnored), "restore known actual base");
        DWORD previous = 0; auto* entry = static_cast<unsigned char*>(owned.view) + 0x175530;
        require(VirtualProtect(entry, 27, PAGE_WRITECOPY, &previous), "make only own mapped event copy writable");
        const auto original = entry[0]; entry[0] = 0xe9;
        qualified = QualifyGame127Screenshot(GetCurrentProcess(), base, path);
        require(qualified.Succeeded() && qualified.value->eventChanged, "same image with altered event is accepted without executing it");
        qualified.value.reset(); entry[0] = original;
        DWORD ignored = 0; require(VirtualProtect(entry, 27, previous, &ignored), "restore original fixture page permission");
        auto* header = static_cast<unsigned char*>(owned.view);
        require(VirtualProtect(header, 1024, PAGE_WRITECOPY, &previous), "own header copy");
        header[0] ^= 1;
        require(!QualifyGame127Screenshot(GetCurrentProcess(), base, path).Succeeded(), "changed mapped PE identity rejected");
        header[0] ^= 1; require(VirtualProtect(header, 1024, previous, &ignored), "restore own header");
        auto* flag = static_cast<unsigned char*>(owned.view) + 0xbe3a38;
        require(VirtualProtect(flag, 4, PAGE_READONLY, &previous), "read-only data negative fixture");
        require(!QualifyGame127Screenshot(GetCurrentProcess(), base, path).Succeeded(), "nonwritable target rejected");
        require(VirtualProtect(flag, 4, previous, &ignored), "restore own data page");
        { std::ofstream file(invalid, std::ios::binary); file << "original invalid image fixture"; require(!!file, "create only new invalid fixture"); }
        require(!QualifyGame127Screenshot(GetCurrentProcess(), base, invalid).Succeeded(), "unknown complete image identity rejected even at same mapping");
        const auto after = Sha256File(path); require(after.Succeeded() && *after.value == *before.value, "disk original unchanged");
        std::cout << "stock_pass=true\nrelocated_header_pass=true\ndos_stub_change_pass=true\ndos_magic_refused=true\ne_lfanew_refused=true\npe_signature_refused=true\nfile_header_refused=true\noptional_header_refused=true\nsection_table_refused=true\nwrong_image_base_refused=true\naltered_event_pass=true\nchanged_header_refused=true\nreadonly_flag_refused=true\nunknown_sha_refused=true\nsource_unchanged=true\ngame_started=false\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
