#include "Pe32OriginalCode.h"
#include <windows.h>
#include <array>
#include <algorithm>
#include <cstring>
namespace Audastra::Runtime32::OwnLua {
namespace {
template<class T> bool Read(std::span<const unsigned char> file, std::size_t offset, T& value) {
    if (offset > file.size() || sizeof(T) > file.size() - offset) return false;
    std::memcpy(&value, file.data() + offset, sizeof(T)); return true;
}
}
bool Pe32OriginalCode(std::span<const unsigned char> file, std::uint32_t base,
    std::uint32_t rva, std::span<unsigned char> out) noexcept {
    if (out.empty() || out.size() > 128) return false;
    IMAGE_DOS_HEADER dos{}; IMAGE_NT_HEADERS32 nt{};
    if (!Read(file, 0, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 64
        || !Read(file, dos.e_lfanew, nt) || nt.Signature != IMAGE_NT_SIGNATURE
        || nt.FileHeader.Machine != IMAGE_FILE_MACHINE_I386 || nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC
        || nt.FileHeader.SizeOfOptionalHeader != sizeof(IMAGE_OPTIONAL_HEADER32)
        || !nt.FileHeader.NumberOfSections || nt.FileHeader.NumberOfSections > 96
        || nt.OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_BASERELOC
        || nt.OptionalHeader.SizeOfImage > 0xffffffffull - base
        || rva >= nt.OptionalHeader.SizeOfImage || out.size() > nt.OptionalHeader.SizeOfImage - rva) return false;
    std::array<IMAGE_SECTION_HEADER, 96> sections{};
    for (unsigned i = 0; i < nt.FileHeader.NumberOfSections; ++i)
        if (!Read(file, static_cast<std::size_t>(dos.e_lfanew) + sizeof(nt) + i * sizeof(IMAGE_SECTION_HEADER), sections[i])) return false;
    const auto view = [&](std::uint32_t at, std::size_t bytes) -> const unsigned char* {
        std::size_t offset = 0; bool found = false;
        if (at < nt.OptionalHeader.SizeOfHeaders && bytes <= nt.OptionalHeader.SizeOfHeaders - at) {
            offset = at; found = true;
        } else for (unsigned i = 0; i < nt.FileHeader.NumberOfSections; ++i) {
            const auto& s = sections[i];
            if (at >= s.VirtualAddress && at - s.VirtualAddress <= s.SizeOfRawData
                && bytes <= s.SizeOfRawData - (at - s.VirtualAddress)) {
                if (found) return nullptr;
                offset = static_cast<std::size_t>(s.PointerToRawData) + at - s.VirtualAddress; found = true;
            }
        }
        return found && offset <= file.size() && bytes <= file.size() - offset ? file.data() + offset : nullptr;
    };
    const auto original = view(rva, out.size()); if (!original) return false;
    std::memcpy(out.data(), original, out.size());
    const auto delta = base - nt.OptionalHeader.ImageBase;
    const auto directory = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    if (!directory.Size) return !delta;
    if (directory.Size > 4 * 1024 * 1024) return false;
    const auto reloc = view(directory.VirtualAddress, directory.Size); if (!reloc) return false;
    std::array<std::uint32_t, 128> applied{}; std::size_t used = 0;
    for (std::size_t pos = 0; pos < directory.Size;) {
        IMAGE_BASE_RELOCATION block{};
        if (directory.Size - pos < sizeof(block)) return false;
        std::memcpy(&block, reloc + pos, sizeof(block));
        if (block.SizeOfBlock < sizeof(block) || block.SizeOfBlock > directory.Size - pos || block.SizeOfBlock % 2) return false;
        for (std::size_t entry = sizeof(block); entry < block.SizeOfBlock; entry += 2) {
            std::uint16_t item = 0; std::memcpy(&item, reloc + pos + entry, 2);
            const auto type = item >> 12;
            if (type == IMAGE_REL_BASED_ABSOLUTE) continue;
            if (type != IMAGE_REL_BASED_HIGHLOW || block.VirtualAddress > 0xffffffffu - (item & 0xfff)) return false;
            const auto at = block.VirtualAddress + (item & 0xfff);
            const auto end = static_cast<std::uint64_t>(at) + 4;
            if (end <= rva || at >= static_cast<std::uint64_t>(rva) + out.size()) continue;
            for (std::size_t i = 0; i < used; ++i) if (applied[i] == at) return false;
            if (used == applied.size()) return false; applied[used++] = at;
            const auto word = view(at, 4); if (!word) return false;
            std::uint32_t value = 0; std::memcpy(&value, word, 4); value += delta;
            const auto bytes = reinterpret_cast<const unsigned char*>(&value);
            for (unsigned i = 0; i < 4; ++i) if (at + i >= rva && static_cast<std::uint64_t>(at) + i < static_cast<std::uint64_t>(rva) + out.size())
                out[at + i - rva] = bytes[i];
        }
        pos += block.SizeOfBlock;
    }
    return true;
}
}
