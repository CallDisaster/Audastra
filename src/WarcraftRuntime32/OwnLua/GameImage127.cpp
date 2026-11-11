#include "GameImage127.h"
#include "Pe32OriginalCode.h"
#include "../RuntimeFiles.h"

#include <array>
#include <algorithm>
#include <cstring>
#include <vector>
#include <map>
#include <mutex>

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
    Handle mapping;
    const unsigned char* disk = nullptr;
    std::size_t diskBytes = 0;
    mutable std::mutex originalMutex;
    mutable std::map<std::uint32_t,std::array<unsigned char,6>> originalGetters;
    Impl(HANDLE file, std::filesystem::path source, std::uintptr_t mapped, ImageHeader shape)
        : lease(file), path(std::move(source)), base(mapped), header(shape) {}
    ~Impl() { if (disk) UnmapViewOfFile(disk); }
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
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(impl->lease.value, &size) || size.QuadPart <= 0 || size.QuadPart > 64 * 1024 * 1024)
        return {{}, "无法读取已锁定映像的原始代码。"};
    impl->mapping.value = CreateFileMappingW(impl->lease.value, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!impl->mapping.value) return {{}, "无法映射已核验的原始代码。"};
    impl->disk = static_cast<const unsigned char*>(MapViewOfFile(impl->mapping.value, FILE_MAP_READ, 0, 0, 0));
    if (!impl->disk) return {{}, "无法读取已核验的代码映射。"};
    impl->diskBytes = static_cast<std::size_t>(size.QuadPart);
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
bool GameImage127::OriginalCode(std::uint32_t rva, std::span<unsigned char> bytes) const noexcept {
    if(!Executable(impl_->base+rva,bytes.size())) return false;
    if(bytes.size()!=6) return Pe32OriginalCode({impl_->disk,impl_->diskBytes},static_cast<std::uint32_t>(impl_->base),rva,bytes);
    try {
        // 只缓存同一锁定文件/基址的原始字节；当前页权限及当前字节仍由每次调用核对。
        std::lock_guard lock(impl_->originalMutex);
        const auto old=impl_->originalGetters.find(rva);
        if(old!=impl_->originalGetters.end()) {std::copy(old->second.begin(),old->second.end(),bytes.begin());return true;}
        if(!Pe32OriginalCode({impl_->disk,impl_->diskBytes},static_cast<std::uint32_t>(impl_->base),rva,bytes)) return false;
        if(impl_->originalGetters.size()<512){std::array<unsigned char,6> data{};std::copy(bytes.begin(),bytes.end(),data.begin());impl_->originalGetters.emplace(rva,data);}
        return true;
    }catch(...){return false;}
}

HANDLE GameImage127::DuplicateActiveMapFile() const {
    constexpr std::uint32_t archiveGetter = 0x770f50, mapArchiveOpen = 0x76ed90, activeSlot = 0xbee150;
    constexpr std::uint32_t stormHandleInstruction = 0x1e494, archiveFileOffset = 0x10c;
    constexpr char stormSha[] = "f8f519cfaa6275a5172a014f0abed2212284390a33f1194677155a7d408e63eb";
    constexpr std::array<unsigned char, 21> stormCode{
        0x8b,0x86,0x0c,0x01,0x00,0x00,0x83,0xf8,0xff,0x74,0x10,
        0x83,0xbe,0x4c,0x01,0x00,0x00,0x00,0x75,0x07,0x50};
    std::array<unsigned char, 6> getter{};
    std::array<unsigned char, 0x72> loader{};
    if (!OriginalCode(archiveGetter, getter) || !Matches(archiveGetter, getter)
        || !OriginalCode(mapArchiveOpen, loader) || !Matches(mapArchiveOpen, loader))
        return INVALID_HANDLE_VALUE;
    // 0x76ed90 将成功打开的当前地图归档放入 getter 返回的首槽。实机同图重开
    // 会释放并重新设置这个槽；第二槽用于另一类归档，不作为活动地图来源。
    std::uint32_t slot = 0;
    std::memcpy(&slot, getter.data() + 1, sizeof(slot));
    if (getter.front() != 0xb8 || getter.back() != 0xc3 || slot != Base() + activeSlot)
        return INVALID_HANDLE_VALUE;
    const auto storm = GetModuleHandleW(L"Storm.dll");
    std::array<wchar_t, 4096> stormPath{};
    const auto length = storm ? GetModuleFileNameW(storm, stormPath.data(), static_cast<DWORD>(stormPath.size())) : 0;
    if (!length || length >= stormPath.size()
        || !EqualPath(std::wstring(stormPath.data(), length), (Path().parent_path() / L"Storm.dll").wstring()))
        return INVALID_HANDLE_VALUE;
    const auto stormInstruction = reinterpret_cast<std::uintptr_t>(storm) + stormHandleInstruction;
    MEMORY_BASIC_INFORMATION memory{};
    if (!VirtualQuery(reinterpret_cast<const void*>(stormInstruction), &memory, sizeof(memory))
        || memory.AllocationBase != storm || memory.Type != MEM_IMAGE || memory.State != MEM_COMMIT
        || !SameBytes(stormInstruction, stormCode.data(), stormCode.size()))
        return INVALID_HANDLE_VALUE;
    Handle stormLease(OpenLockedFile(stormPath.data()));
    if (stormLease.value == INVALID_HANDLE_VALUE || Sha256(stormLease.value, 1024 * 1024) != stormSha)
        return INVALID_HANDLE_VALUE;
    const auto process = GetCurrentProcess();
    const auto read = [&](std::uintptr_t address, std::uint32_t& value) {
        SIZE_T bytes = 0;
        return ReadProcessMemory(process, reinterpret_cast<const void*>(address), &value, sizeof(value), &bytes)
            && bytes == sizeof(value);
    };
    std::uint32_t archive = 0, file = 0;
    if (!read(slot, archive) || archive < 0x10000 || archive > UINT32_MAX - archiveFileOffset - sizeof(file)
        || !read(archive + archiveFileOffset, file) || !file || file == UINT32_MAX)
        return INVALID_HANDLE_VALUE;
    Handle duplicate;
    if (!DuplicateHandle(process, reinterpret_cast<HANDLE>(std::uintptr_t(file)), process,
        &duplicate.value, 0, FALSE, DUPLICATE_SAME_ACCESS) || GetFileType(duplicate.value) != FILE_TYPE_DISK)
        return INVALID_HANDLE_VALUE;
    std::uint32_t currentArchive = 0, currentFile = 0;
    if (!read(slot, currentArchive) || currentArchive != archive
        || !read(archive + archiveFileOffset, currentFile) || currentFile != file)
        return INVALID_HANDLE_VALUE;
    const auto result = duplicate.value;
    duplicate.value = INVALID_HANDLE_VALUE;
    return result;
}
} // namespace Audastra::Runtime32::OwnLua
