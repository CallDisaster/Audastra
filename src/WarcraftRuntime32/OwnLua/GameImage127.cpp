#include "GameImage127.h"
#include "../RuntimeFiles.h"

#include <array>
#include <cstring>
#include <vector>

namespace Audastra::Runtime32::OwnLua {
namespace {
constexpr char kVerifiedGameSha[] = "e04d1716603c075eb0c8e1e21cf1093a664adc5249efab396bfa08d7b09d0c3a";
struct Range { std::uint32_t begin = 0, bytes = 0; };
struct ImageHeader { std::uint32_t bytes = 0, ranges = 0; std::array<Range, 96> code{}; };
ImageHeader Header(std::uintptr_t base) noexcept {
    __try {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 1024 * 1024) return {};
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386
            || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC || nt->OptionalHeader.SizeOfImage <= 0xbe423c
            || nt->FileHeader.NumberOfSections > 96 || nt->FileHeader.SizeOfOptionalHeader != sizeof(IMAGE_OPTIONAL_HEADER32)) return {};
        ImageHeader result; result.bytes = nt->OptionalHeader.SizeOfImage;
        const auto* section = IMAGE_FIRST_SECTION(nt);
        for (unsigned index = 0; index < nt->FileHeader.NumberOfSections; ++index) {
            const auto& item = section[index];
            if (!(item.Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
            if (!item.Misc.VirtualSize || item.VirtualAddress >= result.bytes
                || item.Misc.VirtualSize > result.bytes - item.VirtualAddress) return {};
            result.code[result.ranges++] = {item.VirtualAddress, item.Misc.VirtualSize};
        }
        return result.ranges ? result : ImageHeader{};
    } __except (EXCEPTION_EXECUTE_HANDLER) { return {}; }
}
bool SameBytes(std::uintptr_t address, const unsigned char* bytes, std::size_t count) noexcept {
    __try { return std::memcmp(reinterpret_cast<const void*>(address), bytes, count) == 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
}
struct GameImage127::Impl {
    Handle lease;
    std::filesystem::path path;
    std::uintptr_t base;
    ImageHeader header;
    Impl(HANDLE file, std::filesystem::path source, std::uintptr_t mapped, ImageHeader shape)
        : lease(file), path(std::move(source)), base(mapped), header(shape) {}
};
GameImage127::GameImage127(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
GameImage127::~GameImage127() = default;
GameImage127::OpenResult GameImage127::Open(HMODULE module) {
    if (!module) return {{}, "需要明确的 Game.dll 映像。"};
    std::array<wchar_t, 4096> path{};
    const auto length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length >= path.size()) return {{}, "无法核对 Game.dll 的实际映像来源。"};
    Handle lease(OpenLockedFile(path.data()));
    if (lease.value == INVALID_HANDLE_VALUE || Sha256(lease.value) != kVerifiedGameSha)
        return {{}, "Game.dll 不是已核验的经典 1.27a 映像；拒绝调用内部 ABI。"};
    const auto base = reinterpret_cast<std::uintptr_t>(module);
    const auto header = Header(base);
    if (!header.bytes) return {{}, "Game.dll 的已映射 PE 结构无效。"};
    auto impl = std::make_unique<Impl>(lease.value, path.data(), base, header);
    lease.value = INVALID_HANDLE_VALUE;
    return {std::shared_ptr<GameImage127>(new GameImage127(std::move(impl))), {}};
}
std::uintptr_t GameImage127::Base() const noexcept { return impl_->base; }
const std::filesystem::path& GameImage127::Path() const noexcept { return impl_->path; }
bool GameImage127::Executable(std::uintptr_t address, std::size_t bytes) const noexcept {
    const auto& self = *impl_;
    if (!bytes || address < self.base || address - self.base >= self.header.bytes
        || bytes > self.header.bytes - (address - self.base)) return false;
    const auto rva = address - self.base;
    bool code = false;
    for (unsigned index = 0; index < self.header.ranges; ++index) {
        const auto range = self.header.code[index];
        if (rva >= range.begin && rva - range.begin < range.bytes && bytes <= range.bytes - (rva - range.begin)) code = true;
    }
    if (!code) return false;
    MEMORY_BASIC_INFORMATION memory{};
    if (!VirtualQuery(reinterpret_cast<const void*>(address), &memory, sizeof(memory))
        || memory.AllocationBase != reinterpret_cast<void*>(self.base) || memory.Type != MEM_IMAGE || memory.State != MEM_COMMIT
        || memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    const auto protect = memory.Protect & 0xff;
    return (protect == PAGE_EXECUTE || protect == PAGE_EXECUTE_READ || protect == PAGE_EXECUTE_READWRITE || protect == PAGE_EXECUTE_WRITECOPY)
        && bytes <= memory.RegionSize - (address - reinterpret_cast<std::uintptr_t>(memory.BaseAddress));
}
bool GameImage127::Matches(std::uintptr_t rva, std::span<const unsigned char> bytes) const noexcept {
    if (rva >= impl_->header.bytes || !Executable(impl_->base + rva, bytes.size())) return false;
    return SameBytes(impl_->base + rva, bytes.data(), bytes.size());
}
} // namespace Audastra::Runtime32::OwnLua
