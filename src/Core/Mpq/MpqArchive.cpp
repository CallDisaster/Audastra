#include "War3MapCore/Mpq/MpqArchive.h"
#include "War3MapCore/ProjectSaveControl.h"

#include "MpqCrypt.h"
#include "MpqDecompress.h"

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <string>

namespace War3Map::Mpq {
namespace {

constexpr std::uint32_t kMpqSignature = 0x1A51504D;     // 'MPQ\x1A'
constexpr std::uint32_t kMpqUserDataSig = 0x1B51504D;   // 'MPQ\x1B'
constexpr std::uint32_t kHashEmpty = 0xFFFFFFFF;
constexpr std::uint32_t kHashDeleted = 0xFFFFFFFE;
constexpr std::size_t kHashEntrySize = 16;
constexpr std::size_t kBlockEntrySize = 16;
constexpr std::uint32_t kMaxListfileSize = 4u * 1024u * 1024u;
constexpr std::size_t kMaxListfileEntries = 32767; // 加上 (listfile) 后最多 32768。
constexpr std::uint32_t kKnownFileFlags =
    kFileImplode | kFileCompress | kFileEncrypted | kFileFixKey | kFilePatch |
    kFileSingleUnit | kFileSectorCrc | kFileExists;

std::uint32_t ReadU32(const std::byte* p) noexcept {
    std::uint32_t v = 0;
    std::memcpy(&v, p, 4);
    return v;
}

std::uint16_t ReadU16(const std::byte* p) noexcept {
    std::uint16_t v = 0;
    std::memcpy(&v, p, 2);
    return v;
}

struct HashEntry {
    std::uint32_t name1 = 0;
    std::uint32_t name2 = 0;
    std::uint16_t locale = 0;
    std::uint16_t platform = 0;
    std::uint32_t blockIndex = 0;
};

struct BlockEntry {
    std::uint32_t filePos = 0;
    std::uint32_t compressedSize = 0;
    std::uint32_t fileSize = 0;
    std::uint32_t flags = 0;
};

std::string BaseName(std::string_view name) {
    const std::size_t slash = name.find_last_of("\\/");
    return std::string(slash == std::string_view::npos ? name : name.substr(slash + 1));
}

Result<std::string> CanonicalMemberName(std::string_view name) {
    if (name.empty() || name.front() == '/' || name.front() == '\\') {
        return Result<std::string>::Failure(
            "Mpq", "LISTFILE_NAME_UNSAFE", "archive member name is empty or absolute");
    }
    std::string canonical;
    canonical.reserve(name.size());
    bool componentStart = true;
    std::string component;
    auto componentSafe = [](std::string_view value) {
        if (value.empty() || value == "." || value == ".." ||
            value.back() == '.' || value.back() == ' ') {
            return false;
        }
        std::string upper;
        upper.reserve(value.size());
        for (const unsigned char c : value) {
            upper.push_back(static_cast<char>(std::toupper(c)));
        }
        const std::string stem = upper.substr(0, upper.find('.'));
        static const std::set<std::string> kReserved{
            "CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "COM5",
            "COM6", "COM7", "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5",
            "LPT6", "LPT7", "LPT8", "LPT9"};
        return !kReserved.contains(stem);
    };
    for (const unsigned char raw : name) {
        if (raw < 0x20 || raw >= 0x7F || raw == ':' || raw == '"' ||
            raw == '<' || raw == '>' || raw == '|' || raw == '?' || raw == '*') {
            return Result<std::string>::Failure(
                "Mpq", "LISTFILE_NAME_UNSAFE", "archive member name contains unsafe bytes");
        }
        const char c = static_cast<char>(raw);
        if (c == '/' || c == '\\') {
            if (componentStart || !componentSafe(component)) {
                return Result<std::string>::Failure(
                    "Mpq", "LISTFILE_NAME_UNSAFE", "archive member path is not canonical");
            }
            canonical.push_back('\\');
            component.clear();
            componentStart = true;
            continue;
        }
        canonical.push_back(static_cast<char>(std::toupper(raw)));
        component.push_back(c);
        componentStart = false;
    }
    if (componentStart || !componentSafe(component)) {
        return Result<std::string>::Failure(
            "Mpq", "LISTFILE_NAME_UNSAFE", "archive member path is not canonical");
    }
    return Result<std::string>::Success(std::move(canonical));
}

} // namespace

class MpqArchive::Impl {
public:
    std::filesystem::path path;
    HANDLE handle = INVALID_HANDLE_VALUE;
    std::vector<std::byte> ownedBytes;
    std::uint64_t archiveBase = 0;
    std::uint64_t archiveSize = 0;
    std::uint64_t physicalSize = 0;
    std::uint32_t blockTablePos = 0;
    std::uint32_t sectorSize = 0;
    std::vector<HashEntry> hashTable;
    std::vector<BlockEntry> blockTable;
    std::vector<std::string> listNames;
    std::vector<std::string> rawListNames;
    bool listfilePresent = false;
    bool listfileReadable = false;
    bool strictListfileSyntax = false;
    bool listfileEntryBudgetExceeded = false;
    std::size_t strictMemberLimit = (std::numeric_limits<std::size_t>::max)();

    ~Impl() {
        if (handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
        }
    }

    [[nodiscard]] bool ReadAt(std::uint64_t offset, std::size_t size,
                              std::vector<std::byte>& out, const ProjectSaveControl* control = nullptr) const {
        if (control && !control->Checkpoint(ProjectSaveStage::Read)) return false;
        if (offset > physicalSize ||
            size > physicalSize - offset ||
            offset > static_cast<std::uint64_t>((std::numeric_limits<LONGLONG>::max)())) {
            return false;
        }
        out.resize(size);
        if (size == 0) {
            return true;
        }
        if (!ownedBytes.empty()) {
            return CopyProjectSaveBytes(out, std::span(ownedBytes).subspan(static_cast<std::size_t>(offset), size), control);
        }
        if (handle == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER position{};
        position.QuadPart = static_cast<LONGLONG>(offset);
        if (!SetFilePointerEx(handle, position, nullptr, FILE_BEGIN)) {
            return false;
        }
        std::size_t readOffset = 0;
        while (readOffset < size) {
            if (control && !control->Checkpoint(ProjectSaveStage::Read, readOffset)) return false;
            const DWORD chunk = static_cast<DWORD>((std::min)(
                size - readOffset,
                control ? std::size_t{1024 * 1024} : static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
            DWORD bytesRead = 0;
            if (!::ReadFile(handle, out.data() + readOffset, chunk, &bytesRead, nullptr) ||
                bytesRead != chunk) {
                return false;
            }
            readOffset += bytesRead;
        }
        return !control || control->Checkpoint(ProjectSaveStage::Read, readOffset);
    }

    [[nodiscard]] const BlockEntry* FindBlock(std::string_view name) const {
        if (hashTable.empty()) {
            return nullptr;
        }
        const auto& crypt = MpqCrypt::Instance();
        const std::uint32_t mask = static_cast<std::uint32_t>(hashTable.size()) - 1;
        const std::uint32_t start = crypt.HashString(name, MpqCrypt::HashType::TableOffset) & mask;
        const std::uint32_t n1 = crypt.HashString(name, MpqCrypt::HashType::NameA);
        const std::uint32_t n2 = crypt.HashString(name, MpqCrypt::HashType::NameB);
        std::uint32_t index = start;
        do {
            const HashEntry& entry = hashTable[index];
            if (entry.blockIndex == kHashEmpty) {
                return nullptr; // 桶链终止
            }
            if (entry.blockIndex != kHashDeleted && entry.name1 == n1 &&
                entry.name2 == n2 && entry.blockIndex < blockTable.size()) {
                return &blockTable[entry.blockIndex];
            }
            index = (index + 1) & mask;
        } while (index != start);
        return nullptr;
    }

    // limit：分扇区文件读到至少 limit 字节即停止（只取文件头时避免解压整个文件）。
    [[nodiscard]] Result<std::vector<std::byte>> ReadFileBlock(
        std::string_view name, const BlockEntry& block,
        std::size_t limit = (std::numeric_limits<std::size_t>::max)(), const ProjectSaveControl* control = nullptr) const {
        if (control && !control->Checkpoint(ProjectSaveStage::Read)) return {{}, ProjectSaveCancelledStatus().diagnostics};
        if ((block.flags & kFileExists) == 0) {
            return Result<std::vector<std::byte>>::Failure(
                "Mpq", "FILE_NOT_EXISTS", "block is not marked as existing");
        }
        if ((block.flags & kFilePatch) != 0) {
            return Result<std::vector<std::byte>>::Failure(
                "Mpq", "FILE_PATCH_UNSUPPORTED", "incremental patch files are unsupported");
        }
        const auto& crypt = MpqCrypt::Instance();
        const std::uint64_t dataPos = archiveBase + block.filePos;
        const bool encrypted = (block.flags & kFileEncrypted) != 0;
        const bool compressed = (block.flags & kFileCompress) != 0;
        const bool imploded = (block.flags & kFileImplode) != 0;

        std::uint32_t key = 0;
        if (encrypted) {
            key = crypt.HashString(BaseName(name), MpqCrypt::HashType::FileKey);
            if ((block.flags & kFileFixKey) != 0) {
                key = (key + block.filePos) ^ block.fileSize;
            }
        }

        // 单块文件：整段一次解压。
        if ((block.flags & kFileSingleUnit) != 0) {
            if (!compressed && !imploded && block.compressedSize != block.fileSize) {
                return Result<std::vector<std::byte>>::Failure(
                    "Mpq", "STORED_SIZE_INVALID",
                    "stored single-unit compressedSize must equal fileSize");
            }
            std::vector<std::byte> raw;
            if (!ReadAt(dataPos, block.compressedSize, raw, control)) {
                return Result<std::vector<std::byte>>::Failure(
                    "Mpq", "READ_FAILED", "failed to read single-unit file data");
            }
            if (encrypted) {
                crypt.DecryptBlock(raw.data(), raw.size(), key);
            }
            if (compressed || imploded) {
                return DecompressSector(raw, block.fileSize, imploded && !compressed);
            }
            raw.resize(block.fileSize);
            return Result<std::vector<std::byte>>::Success(std::move(raw));
        }

        // 分扇区文件。
        const std::uint32_t sectorCount = sectorSize == 0 ? 0 :
            static_cast<std::uint32_t>(
                (static_cast<std::uint64_t>(block.fileSize) + sectorSize - 1) / sectorSize);
        if (sectorCount == 0) {
            if (block.compressedSize != 0) {
                return Result<std::vector<std::byte>>::Failure(
                    "Mpq", "EMPTY_FILE_SIZE_INVALID",
                    "an empty member must not hide non-empty archive bytes");
            }
            return Result<std::vector<std::byte>>::Success({});
        }

        if (!compressed && !imploded) {
            if (block.compressedSize != block.fileSize) {
                return Result<std::vector<std::byte>>::Failure(
                    "Mpq", "STORED_SIZE_INVALID",
                    "stored sector file compressedSize must equal fileSize");
            }
            // 未压缩分扇区文件：直接读原始数据。
            std::vector<std::byte> raw;
            if (!ReadAt(dataPos, block.fileSize, raw, control)) {
                return Result<std::vector<std::byte>>::Failure(
                    "Mpq", "READ_FAILED", "failed to read stored file data");
            }
            if (encrypted) {
                for (std::uint32_t i = 0; i < sectorCount; ++i) {
                    if (control && !control->Checkpoint(ProjectSaveStage::Verify, static_cast<std::uint64_t>(i) * sectorSize))
                        return {{}, ProjectSaveCancelledStatus().diagnostics};
                    const std::size_t off = static_cast<std::size_t>(i) * sectorSize;
                    const std::size_t len =
                        std::min<std::size_t>(sectorSize, raw.size() - off);
                    crypt.DecryptBlock(raw.data() + off, len, key + i);
                }
            }
            return Result<std::vector<std::byte>>::Success(std::move(raw));
        }

        // 经典校验标志会多存一个校验区末偏移；原版包也会保留标志但让校验区为空。
        const bool sectorChecksums = (block.flags & kFileSectorCrc) != 0;
        const std::size_t offsetCount = static_cast<std::size_t>(sectorCount) + (sectorChecksums ? 2 : 1);
        const std::size_t offsetTableBytes = offsetCount * sizeof(std::uint32_t);
        // 所有入口（包括预读清单和只读文件头）都先核对成员边界，不能读到下一成员。
        if (offsetTableBytes > block.compressedSize) {
            return Result<std::vector<std::byte>>::Failure(
                "Mpq", "SECTOR_OFFSET_INVALID", "sector offset table exceeds member storage");
        }
        std::vector<std::byte> offsetBytes;
        if (!ReadAt(dataPos, offsetTableBytes, offsetBytes, control)) {
            return Result<std::vector<std::byte>>::Failure(
                "Mpq", "READ_FAILED", "failed to read sector offset table");
        }
        if (encrypted) {
            crypt.DecryptBlock(offsetBytes.data(), offsetBytes.size(), key - 1);
        }
        std::vector<std::uint32_t> offsets(offsetCount);
        for (std::size_t i = 0; i < offsetCount; ++i) {
            if (control && (i % 256 == 0) && !control->Checkpoint(ProjectSaveStage::Verify, i * 4))
                return {{}, ProjectSaveCancelledStatus().diagnostics};
            offsets[i] = ReadU32(offsetBytes.data() + i * 4);
        }
        if (offsets.front() != offsetTableBytes ||
            offsets.back() != block.compressedSize) {
            return Result<std::vector<std::byte>>::Failure(
                "Mpq", "SECTOR_OFFSET_INVALID",
                "sector offsets must start exactly after the table and end at compressedSize");
        }
        for (std::uint32_t i = 0; i < sectorCount; ++i) {
            if (offsets[i + 1] <= offsets[i] || offsets[i + 1] > block.compressedSize) {
                return Result<std::vector<std::byte>>::Failure(
                    "Mpq", "SECTOR_OFFSET_INVALID", "sector offset table is inconsistent");
            }
        }
        if (sectorChecksums && offsets[sectorCount] != offsets.back()) {
            // 非空校验数组尚未实现，必须明确拒绝，不能把未校验的内容交给调用方。
            return Result<std::vector<std::byte>>::Failure(
                "Mpq", "SECTOR_CRC_UNSUPPORTED", "non-empty sector checksum tables are unsupported");
        }

        std::vector<std::byte> out;
        out.reserve(std::min<std::size_t>(block.fileSize, limit));
        for (std::uint32_t i = 0; i < sectorCount && out.size() < limit; ++i) {
            if (control && !control->Checkpoint(ProjectSaveStage::Verify, out.size()))
                return {{}, ProjectSaveCancelledStatus().diagnostics};
            const std::uint32_t rawLen = offsets[i + 1] - offsets[i];
            std::vector<std::byte> sector;
            if (!ReadAt(dataPos + offsets[i], rawLen, sector, control)) {
                return Result<std::vector<std::byte>>::Failure(
                    "Mpq", "READ_FAILED", "failed to read sector data");
            }
            if (encrypted) {
                crypt.DecryptBlock(sector.data(), sector.size(), key + i);
            }
            const std::uint32_t expected =
                std::min<std::uint32_t>(sectorSize, block.fileSize -
                    i * sectorSize);
            auto decompressed = DecompressSector(sector, expected, imploded && !compressed);
            if (!decompressed.Succeeded()) {
                Result<std::vector<std::byte>> failure;
                failure.diagnostics = std::move(decompressed.diagnostics);
                return failure;
            }
            out.insert(out.end(), decompressed.value->begin(), decompressed.value->end());
        }
        return Result<std::vector<std::byte>>::Success(std::move(out));
    }

    void LoadListfile() {
        const BlockEntry* block = FindBlock("(listfile)");
        if (block == nullptr) {
            return;
        }
        listfilePresent = true;
        // OpenReadOnly 会在严格清单可用前解析 (listfile)。先施加独立硬预算，
        // 避免恶意块表在任何调用方有机会执行更高层门禁前触发巨额分配。
        if (block->fileSize > kMaxListfileSize ||
            block->compressedSize > kMaxListfileSize) {
            return;
        }
        auto data = ReadFileBlock("(listfile)", *block);
        if (!data.Succeeded() || !data.value.has_value()) {
            return;
        }
        listfileReadable = true;
        std::string text(
            reinterpret_cast<const char*>(data.value->data()), data.value->size());
        std::set<std::string> unique;
        std::string current;
        bool syntaxValid = !text.empty();
        for (std::size_t i = 0; i < text.size();) {
            const unsigned char c = static_cast<unsigned char>(text[i]);
            if (c == '\r' || c == '\n') {
                if (current.empty()) {
                    syntaxValid = false;
                } else {
                    if (rawListNames.size() >= kMaxListfileEntries) {
                        listfileEntryBudgetExceeded = true;
                        return;
                    }
                    rawListNames.push_back(current);
                    unique.insert(current);
                    current.clear();
                }
                if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n') {
                    i += 2;
                } else {
                    ++i;
                }
                if (i == text.size()) {
                    break;
                }
                continue;
            }
            if (c < 0x20 || c >= 0x7F) {
                syntaxValid = false;
            }
            current.push_back(static_cast<char>(c));
            ++i;
        }
        if (!current.empty()) {
            if (rawListNames.size() >= kMaxListfileEntries) {
                listfileEntryBudgetExceeded = true;
                return;
            }
            rawListNames.push_back(current);
            unique.insert(current);
        }
        strictListfileSyntax = syntaxValid && !rawListNames.empty();
        listNames.assign(unique.begin(), unique.end());
    }
};

MpqArchive::MpqArchive(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
MpqArchive::MpqArchive(MpqArchive&&) noexcept = default;
MpqArchive& MpqArchive::operator=(MpqArchive&&) noexcept = default;
MpqArchive::~MpqArchive() = default;

Result<MpqArchive> MpqArchive::OpenReadOnly(const std::filesystem::path& archivePath) {
    HANDLE handle = CreateFileW(
        archivePath.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return Result<MpqArchive>::Failure(
            "Mpq", "OPEN_FAILED", "cannot open archive: " + archivePath.string());
    }
    auto result = OpenReadOnlyHandle(handle, archivePath);
    CloseHandle(handle);
    return result;
}

Result<MpqArchive> MpqArchive::OpenReadOnlyHandle(
    void* fileHandle,
    const std::filesystem::path& diagnosticPath,
    const MpqOpenLimits& limits) {
    try {
        if (limits.maxDeclaredArchiveBytes == 0 ||
            limits.maxHashTableEntries == 0 ||
            limits.maxBlockTableEntries == 0 ||
            limits.maxStrictMemberCount == 0) {
            return Result<MpqArchive>::Failure(
                "Mpq", "OPEN_LIMIT_INVALID", "MPQ open limits must be non-zero");
        }
        const HANDLE borrowed = static_cast<HANDLE>(fileHandle);
        if (borrowed == nullptr || borrowed == INVALID_HANDLE_VALUE) {
            return Result<MpqArchive>::Failure(
                "Mpq", "OPEN_HANDLE_INVALID", "borrowed archive handle is invalid");
        }
        auto impl = std::make_unique<Impl>();
        if (!DuplicateHandle(
                GetCurrentProcess(), borrowed, GetCurrentProcess(), &impl->handle,
                0, FALSE, DUPLICATE_SAME_ACCESS)) {
            return Result<MpqArchive>::Failure(
                "Mpq", "OPEN_HANDLE_DUPLICATE_FAILED",
                "failed to duplicate the borrowed archive handle");
        }
        impl->path = diagnosticPath;
        impl->strictMemberLimit = limits.maxStrictMemberCount;
        LARGE_INTEGER endPosition{};
        if (!GetFileSizeEx(impl->handle, &endPosition) || endPosition.QuadPart < 0) {
            return Result<MpqArchive>::Failure("Mpq", "FILE_SIZE_FAILED",
                                               "failed to determine archive file size");
        }
        const auto fileSize = static_cast<std::uint64_t>(endPosition.QuadPart);
        impl->physicalSize = fileSize;
        return OpenParsed(std::move(impl), limits);
    } catch (const std::exception& exception) {
        return Result<MpqArchive>::Failure("Mpq", "OPEN_EXCEPTION", exception.what());
    }
}

Result<MpqArchive> MpqArchive::OpenReadOnlyBytes(std::vector<std::byte> bytes,
    const std::filesystem::path& diagnosticPath, const MpqOpenLimits& limits) {
    try {
        if (bytes.size() > 64u * 1024u * 1024u)
            return Result<MpqArchive>::Failure("Mpq", "MEMORY_ARCHIVE_BUDGET_EXCEEDED",
                "owned MPQ bytes exceed the 64 MiB input budget");
        auto impl = std::make_unique<Impl>();
        impl->path = diagnosticPath;
        impl->physicalSize = bytes.size();
        impl->ownedBytes = std::move(bytes);
        return OpenParsed(std::move(impl), limits);
    } catch (const std::exception& exception) {
        return Result<MpqArchive>::Failure("Mpq", "OPEN_EXCEPTION", exception.what());
    }
}

Result<MpqArchive> MpqArchive::OpenParsed(std::unique_ptr<Impl> impl, const MpqOpenLimits& limits) {
    try {
        if (limits.maxDeclaredArchiveBytes == 0 || limits.maxHashTableEntries == 0 ||
            limits.maxBlockTableEntries == 0 || limits.maxStrictMemberCount == 0)
            return Result<MpqArchive>::Failure("Mpq", "OPEN_LIMIT_INVALID", "MPQ open limits must be non-zero");
        impl->strictMemberLimit = limits.maxStrictMemberCount;
        const auto fileSize = impl->physicalSize;

        // 扫描 512 对齐位置寻找 MPQ 归档头（跳过 WC3 的 HM3W 头及可选用户数据头）。
        std::uint64_t headerOffset = 0;
        bool found = false;
        std::vector<std::byte> probe;
        for (std::uint64_t off = 0;
             off + 32 <= fileSize && off <= limits.maxArchivePrefixBytes;
             off += 0x200) {
            if (!impl->ReadAt(off, 4, probe)) {
                break;
            }
            const std::uint32_t sig = ReadU32(probe.data());
            if (sig == kMpqSignature) {
                headerOffset = off;
                found = true;
                break;
            }
            if (sig == kMpqUserDataSig) {
                continue; // 用户数据头，继续扫描真正的归档头
            }
        }
        if (!found) {
            if (limits.maxArchivePrefixBytes !=
                    (std::numeric_limits<std::uint64_t>::max)() &&
                fileSize > limits.maxArchivePrefixBytes) {
                return Result<MpqArchive>::Failure(
                    "Mpq", "ARCHIVE_PREFIX_SCAN_LIMIT_EXCEEDED",
                    "no MPQ header was found within the bounded prefix scan");
            }
            return Result<MpqArchive>::Failure(
                "Mpq", "SIGNATURE_NOT_FOUND",
                "no MPQ archive header found (not a w3x/w3m/MPQ file?)");
        }

        std::vector<std::byte> header;
        if (!impl->ReadAt(headerOffset, 32, header)) {
            return Result<MpqArchive>::Failure("Mpq", "HEADER_READ_FAILED",
                                               "failed to read MPQ header");
        }
        const std::uint16_t formatVersion = ReadU16(header.data() + 0x0C);
        const std::uint16_t blockSizeShift = ReadU16(header.data() + 0x0E);
        const std::uint32_t headerSize = ReadU32(header.data() + 0x04);
        const std::uint32_t archiveSize = ReadU32(header.data() + 0x08);
        const std::uint32_t hashTablePos = ReadU32(header.data() + 0x10);
        const std::uint32_t blockTablePos = ReadU32(header.data() + 0x14);
        const std::uint32_t hashTableCount = ReadU32(header.data() + 0x18);
        const std::uint32_t blockTableCount = ReadU32(header.data() + 0x1C);
        if (formatVersion != 0) {
            // v1+ 含本读取器尚未解析的扩展头/高位偏移，严格拒绝半支持。
            return Result<MpqArchive>::Failure("Mpq", "FORMAT_UNSUPPORTED",
                                               "MPQ format version " +
                                                   std::to_string(formatVersion) + " unsupported");
        }
        if (headerSize != 32 || archiveSize < headerSize || headerOffset > fileSize ||
            archiveSize > fileSize - headerOffset) {
            return Result<MpqArchive>::Failure(
                "Mpq", "ARCHIVE_BOUNDS_INVALID",
                "MPQ v0 requires a 32-byte header fully contained by archiveSize and the source");
        }
        if (archiveSize > limits.maxDeclaredArchiveBytes) {
            return Result<MpqArchive>::Failure(
                "Mpq", "DECLARED_ARCHIVE_BUDGET_EXCEEDED",
                "declared MPQ size exceeds the caller's bounded open contract");
        }
        if (blockSizeShift > 15) {
            return Result<MpqArchive>::Failure("Mpq", "BLOCK_SHIFT_INVALID",
                                               "MPQ blockSizeShift is unsafe");
        }
        if (hashTableCount == 0 || (hashTableCount & (hashTableCount - 1)) != 0 ||
            blockTableCount == 0 || hashTableCount > (1u << 20) || blockTableCount > (1u << 20)) {
            return Result<MpqArchive>::Failure(
                "Mpq", "TABLE_COUNT_INVALID",
                "hash table must be a non-empty power of two and block table must be non-empty");
        }
        if (hashTableCount > limits.maxHashTableEntries ||
            blockTableCount > limits.maxBlockTableEntries) {
            return Result<MpqArchive>::Failure(
                "Mpq", "TABLE_COUNT_LIMIT_EXCEEDED",
                "MPQ table entries exceed the caller's bounded open contract");
        }
        const std::uint64_t hashBytesSize =
            static_cast<std::uint64_t>(hashTableCount) * kHashEntrySize;
        const std::uint64_t blockBytesSize =
            static_cast<std::uint64_t>(blockTableCount) * kBlockEntrySize;
        if (hashTablePos > archiveSize || hashBytesSize > archiveSize - hashTablePos ||
            blockTablePos > archiveSize || blockBytesSize > archiveSize - blockTablePos) {
            return Result<MpqArchive>::Failure("Mpq", "TABLE_BOUNDS_INVALID",
                                               "MPQ tables exceed archiveSize");
        }
        const auto overlaps = [](std::uint64_t aStart, std::uint64_t aSize, std::uint64_t bStart,
                                 std::uint64_t bSize) {
            return aStart < bStart + bSize && bStart < aStart + aSize;
        };
        if (overlaps(hashTablePos, hashBytesSize, blockTablePos, blockBytesSize) ||
            overlaps(0, headerSize, hashTablePos, hashBytesSize) ||
            overlaps(0, headerSize, blockTablePos, blockBytesSize)) {
            return Result<MpqArchive>::Failure("Mpq", "TABLE_OVERLAP_INVALID",
                                               "MPQ metadata ranges overlap");
        }

        impl->archiveBase = headerOffset;
        impl->archiveSize = archiveSize;
        impl->blockTablePos = blockTablePos;
        impl->sectorSize = 512u << blockSizeShift;

        const auto &crypt = MpqCrypt::Instance();

        // 哈希表。
        std::vector<std::byte> hashBytes;
        if (!impl->ReadAt(headerOffset + hashTablePos,
                          static_cast<std::size_t>(hashTableCount) * kHashEntrySize, hashBytes)) {
            return Result<MpqArchive>::Failure("Mpq", "HASH_TABLE_READ_FAILED",
                                               "failed to read hash table");
        }
        crypt.DecryptBlock(hashBytes.data(), hashBytes.size(),
                           crypt.HashString("(hash table)", MpqCrypt::HashType::FileKey));
        impl->hashTable.resize(hashTableCount);
        for (std::uint32_t i = 0; i < hashTableCount; ++i) {
            const std::byte *p = hashBytes.data() + static_cast<std::size_t>(i) * kHashEntrySize;
            impl->hashTable[i] = HashEntry{ReadU32(p), ReadU32(p + 4), ReadU16(p + 8),
                                           ReadU16(p + 10), ReadU32(p + 12)};
        }

        // 块表。
        std::vector<std::byte> blockBytes;
        if (!impl->ReadAt(headerOffset + blockTablePos,
                          static_cast<std::size_t>(blockTableCount) * kBlockEntrySize,
                          blockBytes)) {
            return Result<MpqArchive>::Failure("Mpq", "BLOCK_TABLE_READ_FAILED",
                                               "failed to read block table");
        }
        crypt.DecryptBlock(blockBytes.data(), blockBytes.size(),
                           crypt.HashString("(block table)", MpqCrypt::HashType::FileKey));
        impl->blockTable.resize(blockTableCount);
        for (std::uint32_t i = 0; i < blockTableCount; ++i) {
            const std::byte *p = blockBytes.data() + static_cast<std::size_t>(i) * kBlockEntrySize;
            impl->blockTable[i] =
                BlockEntry{ReadU32(p), ReadU32(p + 4), ReadU32(p + 8), ReadU32(p + 12)};
            const BlockEntry &block = impl->blockTable[i];
            if ((block.flags & kFileExists) != 0 &&
                (block.filePos > archiveSize ||
                 block.compressedSize > archiveSize - block.filePos)) {
                return Result<MpqArchive>::Failure("Mpq", "BLOCK_BOUNDS_INVALID",
                                                   "existing MPQ block exceeds archiveSize");
            }
            if ((block.flags & kFileExists) != 0 && block.compressedSize != 0 &&
                (overlaps(block.filePos, block.compressedSize, 0, headerSize) ||
                 overlaps(block.filePos, block.compressedSize, hashTablePos, hashBytesSize) ||
                 overlaps(block.filePos, block.compressedSize, blockTablePos, blockBytesSize))) {
                return Result<MpqArchive>::Failure("Mpq", "BLOCK_TABLE_OVERLAP_INVALID",
                                                   "existing MPQ block overlaps metadata ranges");
            }
        }
        std::vector<std::pair<std::uint64_t, std::uint64_t>> liveRanges;
        liveRanges.reserve(impl->blockTable.size());
        for (const auto &block : impl->blockTable) {
            if ((block.flags & kFileExists) != 0 && block.compressedSize != 0) {
                liveRanges.emplace_back(block.filePos, static_cast<std::uint64_t>(block.filePos) +
                                                           block.compressedSize);
            }
        }
        std::sort(liveRanges.begin(), liveRanges.end());
        for (std::size_t i = 1; i < liveRanges.size(); ++i) {
            if (liveRanges[i].first < liveRanges[i - 1].second) {
                return Result<MpqArchive>::Failure("Mpq", "BLOCK_OVERLAP_INVALID",
                                                   "existing MPQ data blocks overlap");
            }
        }

        impl->LoadListfile();
        return Result<MpqArchive>::Success(MpqArchive(std::move(impl)));
    } catch (const std::exception &exception) {
        return Result<MpqArchive>::Failure("Mpq", "OPEN_EXCEPTION", exception.what());
    } catch (...) {
        return Result<MpqArchive>::Failure("Mpq", "OPEN_EXCEPTION",
                                           "unknown exception while opening archive");
    }
}

bool MpqArchive::HasFile(std::string_view name) const {
    return impl_->FindBlock(name) != nullptr;
}

Result<std::vector<std::byte>> MpqArchive::ReadFile(std::string_view name, const ProjectSaveControl* control) const {
    const BlockEntry* block = impl_->FindBlock(name);
    if (block == nullptr) {
        return Result<std::vector<std::byte>>::Failure(
            "Mpq", "FILE_NOT_FOUND", "file not present in archive: " + std::string(name));
    }
    auto result = impl_->ReadFileBlock(name, *block, (std::numeric_limits<std::size_t>::max)(), control);
    if (control && control->CancellationRequested()) return {{}, ProjectSaveCancelledStatus().diagnostics};
    return result;
}

Result<std::vector<std::byte>> MpqArchive::ReadFileBounded(
    std::string_view name, std::size_t maxBytes) const {
    const BlockEntry* block = impl_->FindBlock(name);
    if (block == nullptr) {
        return Result<std::vector<std::byte>>::Failure(
            "Mpq", "FILE_NOT_FOUND", "file not present in archive: " + std::string(name));
    }
    // 不能用 ReadFileHead 截断代替门禁：单块与未压缩成员会一次读入整个数据区。
    if (block->fileSize > maxBytes || block->compressedSize > maxBytes) {
        return Result<std::vector<std::byte>>::Failure(
            "Mpq", "FILE_READ_BUDGET_EXCEEDED",
            "成员的存储或解压大小超过读取预算: " + std::string(name) +
                " (stored=" + std::to_string(block->compressedSize) +
                ", decoded=" + std::to_string(block->fileSize) +
                ", limit=" + std::to_string(maxBytes) + ")");
    }
    if ((block->flags & kFileSingleUnit) == 0 && block->fileSize != 0 &&
        (block->flags & (kFileCompress | kFileImplode)) != 0) {
        const auto sectorCount =
            (static_cast<std::uint64_t>(block->fileSize) + impl_->sectorSize - 1) /
            impl_->sectorSize;
        // 在 ReadFileBlock 分配偏移表前拒绝连偏移表也装不下的块，避免越过成员边界。
        const auto offsetCount = sectorCount + ((block->flags & kFileSectorCrc) ? 2 : 1);
        if (offsetCount * sizeof(std::uint32_t) > block->compressedSize) {
            return Result<std::vector<std::byte>>::Failure(
                "Mpq", "SECTOR_OFFSET_INVALID", "sector offset table exceeds member storage");
        }
    }
    auto result = impl_->ReadFileBlock(name, *block);
    if (result.Succeeded() && result.value->size() != block->fileSize) {
        return Result<std::vector<std::byte>>::Failure(
            "Mpq", "FILE_DECODE_SIZE_INVALID", "decoded member size differs from block table");
    }
    return result;
}

Result<MpqRawBlock> MpqArchive::ReadRawBlock(std::string_view name) const {
    const BlockEntry* block = impl_->FindBlock(name);
    if (block == nullptr) {
        return Result<MpqRawBlock>::Failure(
            "Mpq", "FILE_NOT_FOUND", "file not present in archive: " + std::string(name));
    }
    // FixKey 的密钥含块位置；补丁块不是完整内容。分扇区块记下本归档的扇区大小，
    // 写出器只把它写进扇区大小相同的归档。
    if ((block->flags & kFileExists) == 0 || (block->flags & (kFileFixKey | kFilePatch)) != 0) {
        return Result<MpqRawBlock>::Failure(
            "Mpq", "RAW_BLOCK_NOT_PORTABLE", "block layout depends on its position: " + std::string(name));
    }
    if ((block->fileSize == 0) != (block->compressedSize == 0)) {
        return Result<MpqRawBlock>::Failure(
            "Mpq", "RAW_BLOCK_SIZE_INVALID", "empty member must have an empty block: " + std::string(name));
    }
    MpqRawBlock raw;
    raw.fileSize = block->fileSize;
    raw.flags = block->flags;
    raw.sectorSize = impl_->sectorSize;
    if (!impl_->ReadAt(impl_->archiveBase + block->filePos, block->compressedSize, raw.bytes)) {
        return Result<MpqRawBlock>::Failure(
            "Mpq", "READ_FAILED", "failed to read raw block: " + std::string(name));
    }
    return Result<MpqRawBlock>::Success(std::move(raw));
}

Result<std::vector<std::byte>> MpqArchive::ReadFileHead(std::string_view name, std::size_t maxBytes) const {
    const BlockEntry* block = impl_->FindBlock(name);
    if (block == nullptr) {
        return Result<std::vector<std::byte>>::Failure(
            "Mpq", "FILE_NOT_FOUND", "file not present in archive: " + std::string(name));
    }
    auto head = impl_->ReadFileBlock(name, *block, maxBytes);
    if (head.Succeeded() && head.value->size() > maxBytes) head.value->resize(maxBytes);
    return head;
}

std::vector<std::string> MpqArchive::ListFiles() const {
    return impl_->listNames;
}

Result<std::vector<std::byte>> MpqArchive::ReadArchivePrefix() const {
    std::vector<std::byte> prefix;
    if (!impl_->ReadAt(0, static_cast<std::size_t>(impl_->archiveBase), prefix)) {
        return Result<std::vector<std::byte>>::Failure(
            "Mpq", "PREFIX_READ_FAILED", "failed to read archive prefix bytes");
    }
    return Result<std::vector<std::byte>>::Success(std::move(prefix));
}

std::uint64_t MpqArchive::ArchivePrefixSize() const noexcept {
    return impl_->archiveBase;
}

std::uint64_t MpqArchive::DeclaredArchiveSize() const noexcept {
    return impl_->archiveSize;
}

std::vector<MpqHashEntry> MpqArchive::HashTableEntries() const {
    std::vector<MpqHashEntry> entries;
    entries.reserve(impl_->hashTable.size());
    for (const auto& entry : impl_->hashTable) {
        entries.push_back({entry.name1, entry.name2, entry.locale, entry.platform, entry.blockIndex});
    }
    return entries;
}

std::vector<MpqBlockEntry> MpqArchive::BlockTableEntries() const {
    std::vector<MpqBlockEntry> entries;
    entries.reserve(impl_->blockTable.size());
    for (const auto& entry : impl_->blockTable) {
        entries.push_back({entry.filePos, entry.compressedSize, entry.fileSize, entry.flags});
    }
    return entries;
}

bool IsMpqMetadataMemberName(std::string_view name) noexcept {
    const auto equals = [name](std::string_view expected) {
        return name.size() == expected.size() && std::equal(name.begin(), name.end(), expected.begin(),
            [](char left, char right) {
                return std::tolower(static_cast<unsigned char>(left)) == static_cast<unsigned char>(right);
            });
    };
    return equals("(attributes)") || equals("(signature)");
}

const std::vector<std::string>& MpqArchive::RawListfileNames() const noexcept {
    return impl_->rawListNames;
}

Result<std::vector<MpqStrictFileInfo>> MpqArchive::ListFilesStrict() const {
    return ListFilesValidated(false);
}

Result<std::vector<MpqStrictFileInfo>> MpqArchive::ListFilesPreservingOpaque() const {
    return ListFilesValidated(true);
}

Result<std::vector<MpqStrictFileInfo>> MpqArchive::ListFilesValidated(bool allowOpaque) const {
    if (!impl_->listfilePresent) {
        return Result<std::vector<MpqStrictFileInfo>>::Failure(
            "Mpq", "LISTFILE_MISSING", "archive does not contain (listfile)");
    }
    if (!impl_->listfileReadable) {
        return Result<std::vector<MpqStrictFileInfo>>::Failure(
            "Mpq", "LISTFILE_UNREADABLE", "archive (listfile) cannot be decoded");
    }
    if (impl_->listfileEntryBudgetExceeded) {
        return Result<std::vector<MpqStrictFileInfo>>::Failure(
            "Mpq", "LISTFILE_ENTRY_BUDGET_EXCEEDED",
            "archive (listfile) exceeds the bounded strict member inventory");
    }
    if (!impl_->strictListfileSyntax) {
        return Result<std::vector<MpqStrictFileInfo>>::Failure(
            "Mpq", "LISTFILE_SYNTAX_INVALID",
            "strict (listfile) requires non-empty ASCII CR/LF-delimited names without blank lines");
    }
    if (impl_->archiveBase + impl_->archiveSize != impl_->physicalSize) {
        return Result<std::vector<MpqStrictFileInfo>>::Failure(
            "Mpq", "ARCHIVE_TRAILING_BYTES_REJECTED",
            "strict side-write rejects bytes after the declared MPQ archive");
    }
    const std::size_t existingFileCount = FileCount();
    if (existingFileCount > impl_->strictMemberLimit) {
        return Result<std::vector<MpqStrictFileInfo>>::Failure(
            "Mpq", "STRICT_MEMBER_LIMIT_EXCEEDED",
            "existing MPQ blocks exceed the caller's bounded strict inventory");
    }

    std::size_t activeHashCount = 0;
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::vector<const HashEntry*>> hashPairs;
    std::set<std::uint32_t> activeBlockIndices;
    for (const auto& entry : impl_->hashTable) {
        if (entry.blockIndex == kHashEmpty || entry.blockIndex == kHashDeleted) {
            continue;
        }
        ++activeHashCount;
        hashPairs[{entry.name1, entry.name2}].push_back(&entry);
        // 只检查 platform 字节；reserved 字节（世界编辑器常写 0xFF）不参与游戏查找。
        if (entry.locale != 0 || MpqPlatformByte(entry.platform) != 0 ||
            entry.blockIndex >= impl_->blockTable.size() ||
            (impl_->blockTable[entry.blockIndex].flags & kFileExists) == 0) {
            return Result<std::vector<MpqStrictFileInfo>>::Failure(
                "Mpq", "HASH_ENTRY_UNSUPPORTED",
                "active hash entries must use locale/platform zero and reference an existing block");
        }
        if (allowOpaque && !activeBlockIndices.insert(entry.blockIndex).second) {
            return Result<std::vector<MpqStrictFileInfo>>::Failure(
                "Mpq", "OPAQUE_HASH_BLOCK_ALIAS", "opaque-preserving patch requires one hash entry per existing block");
        }
    }
    if (activeHashCount != existingFileCount) {
        return Result<std::vector<MpqStrictFileInfo>>::Failure(
            "Mpq", "HASH_BLOCK_BIJECTION_FAILED",
            "active hash entries and existing blocks are not a strict bijection");
    }
    if (allowOpaque) {
        for (const auto& [pair, entries] : hashPairs) {
            if (entries.size() != 1) {
                return Result<std::vector<MpqStrictFileInfo>>::Failure(
                    "Mpq", "OPAQUE_HASH_PAIR_AMBIGUOUS",
                    "opaque-preserving patch requires unique active name hash pairs");
            }
        }
    }

    std::vector<std::string> names = impl_->rawListNames;
    // (listfile) 自身已读到；部分编辑器也把它写进清单，此时不能再补一次。
    const bool listfileListed = std::any_of(names.begin(), names.end(), [](const std::string& name) {
        return _stricmp(name.c_str(), "(listfile)") == 0;
    });
    if (!listfileListed) names.emplace_back("(listfile)");
    // 元数据成员通常不在 (listfile) 中；存在且未被列出时按约定名补入，
    // 使严格清单仍能证明每个块都有名字。
    for (const char* metadata : {"(attributes)", "(signature)"}) {
        const bool listed = std::any_of(names.begin(), names.end(), [metadata](const std::string& name) {
            return _stricmp(name.c_str(), metadata) == 0;
        });
        const auto& hashing = MpqCrypt::Instance();
        if (!listed && hashPairs.contains({hashing.HashString(metadata, MpqCrypt::HashType::NameA),
                hashing.HashString(metadata, MpqCrypt::HashType::NameB)})) {
            names.emplace_back(metadata);
        }
    }
    std::set<std::string> canonicalNames;
    std::size_t duplicateNames = 0;
    std::string firstDuplicate;
    std::map<std::uint32_t, std::string> blockNames;
    std::vector<MpqStrictFileInfo> result;
    result.reserve(names.size());
    const auto& crypt = MpqCrypt::Instance();
    for (const auto& name : names) {
        auto canonical = CanonicalMemberName(name);
        if (!canonical.Succeeded() || !canonical.value.has_value()) {
            Result<std::vector<MpqStrictFileInfo>> failure;
            failure.diagnostics = std::move(canonical.diagnostics);
            return failure;
        }
        if (!canonicalNames.insert(*canonical.value).second) {
            // 名字哈希不分大小写、正反斜杠，规范名相同必然指向同一成员；重复行只是清单冗余，保留首次写法。
            if (duplicateNames++ == 0) firstDuplicate = name;
            continue;
        }

        const std::uint32_t n1 = crypt.HashString(name, MpqCrypt::HashType::NameA);
        const std::uint32_t n2 = crypt.HashString(name, MpqCrypt::HashType::NameB);
        const auto pair = hashPairs.find({n1, n2});
        if (pair == hashPairs.end()) {
            return Result<std::vector<MpqStrictFileInfo>>::Failure(
                "Mpq", "LISTFILE_MEMBER_MISSING",
                "(listfile) names a member with no active hash entry: " + name);
        }
        const HashEntry* match = pair != hashPairs.end() && pair->second.size() == 1
            ? pair->second.front() : nullptr;
        if (match == nullptr ||
            match->blockIndex >= impl_->blockTable.size()) {
            return Result<std::vector<MpqStrictFileInfo>>::Failure(
                "Mpq", "LISTFILE_HASH_AMBIGUOUS",
                "listed member must resolve to exactly one hash entry: " + name);
        }
        const std::uint32_t mask = static_cast<std::uint32_t>(impl_->hashTable.size() - 1);
        const std::uint32_t start =
            crypt.HashString(name, MpqCrypt::HashType::TableOffset) & mask;
        std::uint32_t probe = start;
        bool reachable = false;
        do {
            const HashEntry& entry = impl_->hashTable[probe];
            if (entry.blockIndex == kHashEmpty) {
                break;
            }
            if (&entry == match) {
                reachable = true;
                break;
            }
            probe = (probe + 1) & mask;
        } while (probe != start);
        if (!reachable) {
            return Result<std::vector<MpqStrictFileInfo>>::Failure(
                "Mpq", "HASH_ENTRY_UNREACHABLE",
                "listed member hash entry is not reachable from its TableOffset bucket: " + name);
        }
        if (!blockNames.emplace(match->blockIndex, name).second) {
            return Result<std::vector<MpqStrictFileInfo>>::Failure(
                "Mpq", "BLOCK_NAME_DUPLICATE",
                "multiple listed names resolve to one block");
        }
        const BlockEntry& block = impl_->blockTable[match->blockIndex];
        if ((block.flags & ~kKnownFileFlags) != 0 ||
            ((block.flags & kFileFixKey) != 0 && (block.flags & kFileEncrypted) == 0) ||
            ((block.flags & kFileImplode) != 0 && (block.flags & kFileCompress) != 0)) {
            return Result<std::vector<MpqStrictFileInfo>>::Failure(
                "Mpq", "FILE_FLAGS_UNSUPPORTED",
                "strict side-write rejects unknown or ambiguous member flags");
        }
        if ((block.flags & kFilePatch) != 0) {
            return Result<std::vector<MpqStrictFileInfo>>::Failure(
                "Mpq", "FILE_PATCH_UNSUPPORTED", "patch members cannot be side-written");
        }
        if ((block.flags & kFileSectorCrc) != 0) {
            return Result<std::vector<MpqStrictFileInfo>>::Failure(
                "Mpq", "SECTOR_CRC_UNSUPPORTED",
                "strict side-write rejects unimplemented sector CRC tables");
        }
        result.push_back(MpqStrictFileInfo{
            name, match->blockIndex, block.compressedSize, block.fileSize, block.flags});
    }

    if (!allowOpaque && blockNames.size() != existingFileCount) {
        return Result<std::vector<MpqStrictFileInfo>>::Failure(
            "Mpq", "ANONYMOUS_BLOCK_REJECTED",
            "(listfile) does not uniquely name every existing block");
    }
    auto success = Result<std::vector<MpqStrictFileInfo>>::Success(std::move(result));
    if (duplicateNames > 0) {
        success.diagnostics.push_back({DiagnosticSeverity::Warning, "Mpq", "LISTFILE_NAME_DUPLICATE",
            "地图文件清单 (listfile) 中有 " + std::to_string(duplicateNames) + " 行与前面的名字重复（不分大小写和斜杠方向），"
            "它们指向同一个文件；已按首次出现的写法处理，保存时清单不再重复。首个重复：" + firstDuplicate, {}});
    }
    return success;
}

Result<std::vector<std::uint32_t>> MpqArchive::PrepareOpaquePreservingPatchInputs(
    std::span<const MpqBuildFile> replacements) const {
    try {
        const auto inventory = ListFilesPreservingOpaque();
        if (!inventory.Succeeded()) return Result<std::vector<std::uint32_t>>{std::nullopt, inventory.diagnostics};
        if (inventory.value->size() >= FileCount()) {
            return Result<std::vector<std::uint32_t>>::Failure("Mpq", "OPAQUE_PATCH_NOT_REQUIRED", "此归档没有匿名成员，应使用完整保存路径。");
        }
        std::map<std::string, std::uint32_t> namedBlocks;
        for (const auto& member : *inventory.value) {
            const auto key = CanonicalMemberName(member.name);
            if (!key.Succeeded()) return Result<std::vector<std::uint32_t>>{std::nullopt, key.diagnostics};
            namedBlocks.emplace(*key.value, member.blockIndex);
        }
        std::map<std::uint32_t, const MpqBuildFile*> edits;
        std::vector<std::uint32_t> indices;
        indices.reserve(replacements.size());
        std::uint64_t appendedBytes = 0;
        for (const auto& replacement : replacements) {
            const auto key = CanonicalMemberName(replacement.name);
            if (!key.Succeeded()) return Result<std::vector<std::uint32_t>>{std::nullopt, key.diagnostics};
            const auto found = namedBlocks.find(*key.value);
            if (found == namedBlocks.end() || *key.value == "(LISTFILE)") {
                return Result<std::vector<std::uint32_t>>::Failure("Mpq", "OPAQUE_NEW_MEMBER_REJECTED",
                    "匿名归档目前只允许替换清单中的现有成员：" + replacement.name);
            }
            if (!edits.emplace(found->second, &replacement).second) {
                return Result<std::vector<std::uint32_t>>::Failure("Mpq", "OPAQUE_REPLACEMENT_DUPLICATE", "同一成员被重复替换。");
            }
            if (replacement.data.size() > (std::numeric_limits<std::uint32_t>::max)() - appendedBytes) {
                return Result<std::vector<std::uint32_t>>::Failure("Mpq", "OPAQUE_PATCH_SIZE_OVERFLOW", "替换成员超过 MPQ v0 的长度范围。");
            }
            appendedBytes += replacement.data.size();
            indices.push_back(found->second);
        }
        const std::uint64_t tableBytes = static_cast<std::uint64_t>(impl_->blockTable.size()) * kBlockEntrySize;
        if (impl_->archiveSize > (std::numeric_limits<std::uint32_t>::max)() - appendedBytes ||
            impl_->archiveSize + appendedBytes > (std::numeric_limits<std::uint32_t>::max)() - tableBytes) {
            return Result<std::vector<std::uint32_t>>::Failure("Mpq", "OPAQUE_PATCH_SIZE_OVERFLOW", "候选归档超过 MPQ v0 的长度范围。");
        }
        return Result<std::vector<std::uint32_t>>::Success(std::move(indices));
    } catch (const std::exception&) {
        return Result<std::vector<std::uint32_t>>::Failure("Mpq", "OPAQUE_PATCH_FAILED", "匿名归档候选构建失败。");
    }
}

Status MpqArchive::ValidateOpaquePreservingPatchInputs(std::span<const MpqBuildFile> replacements) const {
    auto prepared = PrepareOpaquePreservingPatchInputs(replacements);
    return Status{std::move(prepared.diagnostics)};
}

Status MpqArchive::WriteOpaquePreservingPatchToHandle(
    void* outputHandle, std::span<const MpqBuildFile> replacements) const {
    try {
        const HANDLE output = static_cast<HANDLE>(outputHandle);
        if (output == nullptr || output == INVALID_HANDLE_VALUE) {
            return Status::Failure("Mpq", "OUTPUT_HANDLE_INVALID", "候选文件句柄无效。");
        }
        const auto prepared = PrepareOpaquePreservingPatchInputs(replacements);
        if (!prepared.Succeeded()) return Status{prepared.diagnostics};
        const std::uint64_t tableBytes = static_cast<std::uint64_t>(impl_->blockTable.size()) * kBlockEntrySize;
        const auto setPosition = [output](std::uint64_t offset) {
            LARGE_INTEGER position{};
            position.QuadPart = static_cast<LONGLONG>(offset);
            return SetFilePointerEx(output, position, nullptr, FILE_BEGIN) != 0;
        };
        const auto writeBytes = [output](std::span<const std::byte> data) {
            std::size_t writtenTotal = 0;
            while (writtenTotal < data.size()) {
                const DWORD request = static_cast<DWORD>((std::min)(data.size() - writtenTotal,
                    static_cast<std::size_t>(1u << 20)));
                DWORD written = 0;
                if (!WriteFile(output, data.data() + writtenTotal, request, &written, nullptr) || written != request) {
                    return false;
                }
                writtenTotal += written;
            }
            return true;
        };
        if (!setPosition(0)) return Status::Failure("Mpq", "OPAQUE_OUTPUT_SEEK_FAILED", "无法定位候选输出。");
        std::vector<std::byte> chunk;
        for (std::uint64_t offset = 0; offset < impl_->physicalSize;) {
            const std::size_t length = static_cast<std::size_t>((std::min)(
                impl_->physicalSize - offset, std::uint64_t{1u << 20}));
            if (!impl_->ReadAt(offset, length, chunk) || !writeBytes(chunk)) {
                return Status::Failure("Mpq", "OPAQUE_COPY_FAILED", "无法完整复制源归档原始字节。");
            }
            offset += length;
        }
        std::vector<BlockEntry> blocks = impl_->blockTable;
        std::uint32_t nextPos = static_cast<std::uint32_t>(impl_->archiveSize);
        std::size_t index = 0;
        for (const auto& replacement : replacements) {
            const auto blockIndex = (*prepared.value)[index++];
            auto& block = blocks[blockIndex];
            block.filePos = nextPos;
            block.compressedSize = static_cast<std::uint32_t>(replacement.data.size());
            block.fileSize = block.compressedSize;
            block.flags = kFileExists | kFileSingleUnit;
            if (!writeBytes(replacement.data)) {
                return Status::Failure("Mpq", "OPAQUE_APPEND_FAILED", "无法追加替换成员。");
            }
            nextPos += block.compressedSize;
        }
        const std::uint32_t newBlockTablePos = nextPos;
        std::vector<std::byte> blockBytes(static_cast<std::size_t>(tableBytes));
        for (std::size_t i = 0; i < blocks.size(); ++i) {
            const auto at = i * kBlockEntrySize;
            std::memcpy(blockBytes.data() + at, &blocks[i].filePos, 4);
            std::memcpy(blockBytes.data() + at + 4, &blocks[i].compressedSize, 4);
            std::memcpy(blockBytes.data() + at + 8, &blocks[i].fileSize, 4);
            std::memcpy(blockBytes.data() + at + 12, &blocks[i].flags, 4);
        }
        const auto& crypt = MpqCrypt::Instance();
        crypt.EncryptBlock(blockBytes.data(), blockBytes.size(),
            crypt.HashString("(block table)", MpqCrypt::HashType::FileKey));
        if (!writeBytes(blockBytes)) {
            return Status::Failure("Mpq", "OPAQUE_APPEND_FAILED", "无法追加候选块表。");
        }
        const std::uint32_t newArchiveSize = newBlockTablePos + static_cast<std::uint32_t>(tableBytes);
        if (!setPosition(impl_->archiveBase + 8) ||
            !writeBytes({reinterpret_cast<const std::byte*>(&newArchiveSize), 4}) ||
            !setPosition(impl_->archiveBase + 0x14) ||
            !writeBytes({reinterpret_cast<const std::byte*>(&newBlockTablePos), 4}) ||
            !FlushFileBuffers(output)) {
            return Status::Failure("Mpq", "OPAQUE_HEADER_WRITE_FAILED", "无法发布候选头和块表。");
        }
        return Status::Ok();
    } catch (const std::exception&) {
        return Status::Failure("Mpq", "OPAQUE_PATCH_FAILED", "匿名归档候选构建失败。");
    }
}

Status MpqArchive::VerifyOpaquePreservingPatch(
    const MpqArchive& candidate, std::span<const MpqBuildFile> replacements) const {
    const auto sourceInventory = ListFilesPreservingOpaque();
    if (!sourceInventory.Succeeded()) return Status{sourceInventory.diagnostics};
    const auto candidateInventory = candidate.ListFilesPreservingOpaque();
    if (!candidateInventory.Succeeded()) return Status{candidateInventory.diagnostics};
    if (sourceInventory.value->size() != candidateInventory.value->size() ||
        impl_->archiveBase != candidate.impl_->archiveBase ||
        impl_->blockTable.size() != candidate.impl_->blockTable.size() ||
        impl_->hashTable.size() != candidate.impl_->hashTable.size()) {
        return Status::Failure("Mpq", "OPAQUE_VERIFY_STRUCTURE_CHANGED", "候选改变了原归档的成员索引结构。");
    }
    std::map<std::string, const MpqBuildFile*> edits;
    std::map<std::string, std::uint32_t> namedBlockIds;
    for (const auto& member : *sourceInventory.value) {
        const auto canonical = CanonicalMemberName(member.name);
        if (!canonical.Succeeded()) return Status{canonical.diagnostics};
        namedBlockIds.emplace(*canonical.value, member.blockIndex);
    }
    std::set<std::uint32_t> editedBlocks;
    std::uint64_t nextPosition = impl_->archiveSize;
    for (const auto& edit : replacements) {
        const auto canonical = CanonicalMemberName(edit.name);
        if (!canonical.Succeeded()) return Status{canonical.diagnostics};
        if (!edits.emplace(*canonical.value, &edit).second) {
            return Status::Failure("Mpq", "OPAQUE_REPLACEMENT_DUPLICATE", "候选替换成员重复。");
        }
        const auto found = namedBlockIds.find(*canonical.value);
        if (found == namedBlockIds.end()) {
            return Status::Failure("Mpq", "OPAQUE_VERIFY_NEW_MEMBER", "候选引入清单外的成员。");
        }
        editedBlocks.insert(found->second);
        const auto& block = candidate.impl_->blockTable[found->second];
        if (block.filePos != nextPosition || block.compressedSize != edit.data.size() ||
            block.fileSize != edit.data.size() || block.flags != (kFileExists | kFileSingleUnit)) {
            return Status::Failure("Mpq", "OPAQUE_VERIFY_EDIT_BLOCK_INVALID", "替换成员的追加块与预期不一致。");
        }
        nextPosition += edit.data.size();
    }
    if (candidate.impl_->blockTablePos != nextPosition ||
        candidate.impl_->archiveSize != nextPosition +
            static_cast<std::uint64_t>(impl_->blockTable.size()) * kBlockEntrySize ||
        candidate.impl_->physicalSize != candidate.impl_->archiveBase + candidate.impl_->archiveSize) {
        return Status::Failure("Mpq", "OPAQUE_VERIFY_APPEND_LAYOUT_INVALID", "候选追加区域和块表布局不一致。");
    }
    for (std::size_t i = 0; i < sourceInventory.value->size(); ++i) {
        const auto& sourceMember = (*sourceInventory.value)[i];
        const auto& candidateMember = (*candidateInventory.value)[i];
        const auto key = CanonicalMemberName(sourceMember.name);
        const auto candidateKey = CanonicalMemberName(candidateMember.name);
        if (!key.Succeeded() || !candidateKey.Succeeded() || *key.value != *candidateKey.value ||
            sourceMember.blockIndex != candidateMember.blockIndex) {
            return Status::Failure("Mpq", "OPAQUE_VERIFY_IDENTITY_CHANGED", "候选改变了清单成员身份。");
        }
        const auto edit = edits.find(*key.value);
        const auto actual = candidate.ReadFile(sourceMember.name);
        if (!actual.Succeeded()) return Status{actual.diagnostics};
        if (edit != edits.end()) {
            if (*actual.value != edit->second->data) {
                return Status::Failure("Mpq", "OPAQUE_VERIFY_EDIT_MISMATCH", "替换成员回读不一致。");
            }
        } else {
            const auto original = ReadFile(sourceMember.name);
            if (!original.Succeeded()) return Status{original.diagnostics};
            if (*actual.value != *original.value) {
                return Status::Failure("Mpq", "OPAQUE_VERIFY_MEMBER_CHANGED", "未修改成员回读不一致。");
            }
        }
    }
    for (std::size_t i = 0; i < impl_->blockTable.size(); ++i) {
        const auto& before = impl_->blockTable[i];
        const auto& after = candidate.impl_->blockTable[i];
        if (!editedBlocks.contains(static_cast<std::uint32_t>(i)) &&
            (before.filePos != after.filePos ||
            before.compressedSize != after.compressedSize || before.fileSize != after.fileSize ||
            before.flags != after.flags)) {
            return Status::Failure("Mpq", "OPAQUE_VERIFY_BLOCK_CHANGED", "未替换的块元数据发生变化。");
        }
    }
    if (candidate.impl_->physicalSize < impl_->physicalSize) {
        return Status::Failure("Mpq", "OPAQUE_VERIFY_TRUNCATED", "候选截断了原始归档。");
    }
    std::vector<std::byte> sourceChunk;
    std::vector<std::byte> candidateChunk;
    for (std::uint64_t offset = 0; offset < impl_->physicalSize;) {
        const std::size_t length = static_cast<std::size_t>((std::min)(
            impl_->physicalSize - offset, std::uint64_t{1u << 20}));
        if (!impl_->ReadAt(offset, length, sourceChunk) ||
            !candidate.impl_->ReadAt(offset, length, candidateChunk)) {
            return Status::Failure("Mpq", "OPAQUE_VERIFY_READ_FAILED", "无法复核原始归档字节。");
        }
        for (std::size_t i = 0; i < length; ++i) {
            const auto absolute = offset + i;
            if ((absolute >= impl_->archiveBase + 8 && absolute < impl_->archiveBase + 12) ||
                (absolute >= impl_->archiveBase + 0x14 && absolute < impl_->archiveBase + 0x18)) continue;
            if (sourceChunk[i] != candidateChunk[i]) {
                return Status::Failure("Mpq", "OPAQUE_VERIFY_SOURCE_BYTES_CHANGED",
                    "候选修改了原归档中非头字段的原始字节。");
            }
        }
        offset += length;
    }
    return Status::Ok();
}

Status MpqArchive::ExtractTo(
    const std::filesystem::path& outDir,
    std::span<const std::string> names) const {
    Status status = Status::Ok();
    std::error_code ec;
    std::filesystem::create_directories(outDir, ec);
    if (ec) {
        return Status::Failure("Mpq", "EXTRACT_MKDIR_FAILED",
                               "cannot create output directory: " + ec.message());
    }
    for (const std::string& name : names) {
        const BlockEntry* block = impl_->FindBlock(name);
        if (block == nullptr) {
            status.diagnostics.push_back(Diagnostic{
                DiagnosticSeverity::Info, "Mpq", "FILE_ABSENT",
                "archive does not contain: " + name});
            continue;
        }
        auto data = impl_->ReadFileBlock(name, *block);
        if (!data.Succeeded() || !data.value.has_value()) {
            for (auto& diag : data.diagnostics) {
                status.diagnostics.push_back(std::move(diag));
            }
            continue;
        }
        // 归一路径分隔符为子目录。
        std::string relative = name;
        std::replace(relative.begin(), relative.end(), '\\', '/');
        const std::filesystem::path outPath = outDir / std::filesystem::path(relative);
        std::filesystem::create_directories(outPath.parent_path(), ec);
        std::ofstream out(outPath, std::ios::binary);
        if (!out) {
            status.diagnostics.push_back(Diagnostic{
                DiagnosticSeverity::Warning, "Mpq", "EXTRACT_WRITE_FAILED",
                "cannot write extracted file: " + outPath.string()});
            continue;
        }
        out.write(reinterpret_cast<const char*>(data.value->data()),
                  static_cast<std::streamsize>(data.value->size()));
    }
    return status;
}

std::uint32_t MpqArchive::SectorSize() const noexcept {
    return impl_->sectorSize;
}

std::size_t MpqArchive::FileCount() const noexcept {
    std::size_t count = 0;
    for (const auto& block : impl_->blockTable) {
        if ((block.flags & kFileExists) != 0) {
            ++count;
        }
    }
    return count;
}

} // namespace War3Map::Mpq
