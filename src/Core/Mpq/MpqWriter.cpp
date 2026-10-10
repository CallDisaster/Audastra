#include "War3MapCore/Mpq/MpqWriter.h"

#include "War3MapCore/Mpq/MpqArchive.h"
#include "MpqCrypt.h"
#include "MpqDecompress.h"
#include "MpqWriterInternal.h"
#include "War3MapCore/ProjectSaveControl.h"

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <limits>
#include <span>
#include <set>

namespace War3Map::Mpq {
namespace {

constexpr std::uint32_t kMpqSignature = 0x1A51504D;
constexpr std::uint32_t kHeaderSize = 32;
constexpr std::uint32_t kHashEmpty = 0xFFFFFFFF;
constexpr std::size_t kMaxInputFiles = (1u << 19) - 1;
constexpr std::size_t kMaxArchivePrefixSize = 16u * 1024u * 1024u;

void PutU32(std::vector<std::byte>& buf, std::size_t at, std::uint32_t value) {
    std::memcpy(buf.data() + at, &value, 4);
}

bool NextPow2(std::uint64_t v, std::uint32_t& output) {
    std::uint32_t result = 4;
    while (result < v) {
        if (result > (std::numeric_limits<std::uint32_t>::max)() / 2) {
            return false;
        }
        result <<= 1;
    }
    output = result;
    return true;
}

Result<std::string> CanonicalBuildName(std::string_view name) {
    if (name.empty() || name.front() == '/' || name.front() == '\\') {
        return Result<std::string>::Failure(
            "Mpq", "BUILD_NAME_UNSAFE", "archive member name is empty or absolute");
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
                "Mpq", "BUILD_NAME_UNSAFE", "archive member name contains unsafe bytes");
        }
        const char c = static_cast<char>(raw);
        if (c == '/' || c == '\\') {
            if (componentStart || !componentSafe(component)) {
                return Result<std::string>::Failure(
                    "Mpq", "BUILD_NAME_UNSAFE", "archive member path is not canonical");
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
            "Mpq", "BUILD_NAME_UNSAFE", "archive member path is not canonical");
    }
    return Result<std::string>::Success(std::move(canonical));
}

Status WriteHandle(
    HANDLE handle,
    std::span<const std::byte> bytes, const ProjectSaveControl* control) {
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
        return Status::Failure(
            "Mpq", "OUTPUT_HANDLE_INVALID", "exclusive output handle is invalid");
    }
    bool succeeded = true;
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        if (control && !control->Checkpoint(ProjectSaveStage::Write, offset)) return ProjectSaveCancelledStatus();
        const DWORD length = static_cast<DWORD>((std::min)(
            bytes.size() - offset,
            control ? std::size_t{1024 * 1024} : static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
        DWORD written = 0;
        if (!WriteFile(handle, bytes.data() + offset, length, &written, nullptr) ||
            written != length) {
            succeeded = false;
            break;
        }
        offset += written;
    }
    if (control && !control->Checkpoint(ProjectSaveStage::Write, offset)) return ProjectSaveCancelledStatus();
    if (!FlushFileBuffers(handle)) {
        succeeded = false;
    }
    if (!succeeded) {
        return Status::Failure("Mpq", "WRITE_FAILED", "failed to write archive data");
    }
    return Status::Ok();
}

// 压缩单个扇区：尝试 Deflate，失败或无收益则存 raw。
// 返回写入归档的字节（含压缩掩码字节 0x02 或无掩码的 raw）。
std::vector<std::byte> CompressSector(
    std::span<const std::byte> sector, bool wantCompress) {
    if (!wantCompress || sector.empty()) {
        return std::vector<std::byte>(sector.begin(), sector.end());
    }
    auto compressed = Deflate(sector);
    // 压缩后必须比原始小才有收益（+1 是掩码字节）。
    if (compressed.size() + 1 >= sector.size()) {
        return std::vector<std::byte>(sector.begin(), sector.end());
    }
    // 自校验：inflate 必须还原。
    auto verified = Inflate(compressed, static_cast<std::uint32_t>(sector.size()));
    if (!verified.Succeeded() || !verified.value.has_value() ||
        verified.value->size() != sector.size() ||
        std::memcmp(verified.value->data(), sector.data(), sector.size()) != 0) {
        return std::vector<std::byte>(sector.begin(), sector.end());
    }
    // 成功：掩码字节 + 压缩数据。
    std::vector<std::byte> result;
    result.reserve(1 + compressed.size());
    result.push_back(std::byte{0x02}); // zlib/DEFLATE
    result.insert(result.end(), compressed.begin(), compressed.end());
    return result;
}

struct BuiltFile {
    std::string name;
    std::uint32_t fileSize = 0;
    std::uint32_t flags = 0;
    std::vector<std::byte> archiveData; // 扇区偏移表 + 扇区数据（新压缩的成员）
    std::span<const std::byte> reused;  // 原样复用的源块（指向调用方的 MpqRawBlock）
    [[nodiscard]] std::span<const std::byte> Bytes() const {
        return reused.empty() ? std::span<const std::byte>(archiveData) : reused;
    }
};

// 原样复用的块只接受与位置无关的普通成员块。
Status ValidateRawBlock(const MpqRawBlock& raw, std::uint32_t sectorSize) {
    if ((raw.flags & kFileExists) == 0 || (raw.flags & (kFileFixKey | kFilePatch)) != 0) {
        return Status::Failure("Mpq", "RAW_BLOCK_FLAGS_UNSUPPORTED",
            "reused block must exist and must not use position-dependent keys or patches");
    }
    if (raw.bytes.size() > (std::numeric_limits<std::uint32_t>::max)() ||
        (raw.fileSize == 0) != raw.bytes.empty()) {
        return Status::Failure("Mpq", "RAW_BLOCK_SIZE_INVALID", "reused block size is inconsistent");
    }
    // 分扇区块的偏移表按源归档的扇区大小切分，只能写进扇区大小相同的归档。
    if ((raw.flags & kFileSingleUnit) == 0 && raw.fileSize != 0 && raw.sectorSize != sectorSize) {
        return Status::Failure("Mpq", "RAW_BLOCK_NOT_PORTABLE", "reused block was split with a different sector size");
    }
    return Status::Ok();
}

Status ValidateSectorShift(std::uint16_t sectorShift) {
    if (sectorShift < kMpqWriterSectorShift || sectorShift > kMpqMaxSectorShift)
        return Status::Failure("Mpq", "SECTOR_SHIFT_INVALID", "sector shift must be between 3 and 15");
    return Status::Ok();
}

Status ValidateListfileSize(std::span<const MpqBuildFile> files) {
    std::size_t size = 0;
    for (const auto& file : files) {
        if (file.name.size() > (std::numeric_limits<std::uint32_t>::max)() - 2 ||
            size > (std::numeric_limits<std::uint32_t>::max)() - file.name.size() - 2)
            return Status::Failure("Mpq", "LISTFILE_SIZE_OVERFLOW", "generated (listfile) exceeds uint32");
        size += file.name.size() + 2;
    }
    return Status::Ok();
}

} // namespace

Status ValidateMpqArchiveInputs(
    std::span<const MpqBuildFile> files,
    std::span<const std::byte> leadingHeader,
    std::uint16_t sectorShift) {
    if (const auto valid = ValidateSectorShift(sectorShift); !valid.Succeeded()) return valid;
    const std::uint32_t sectorSize = 512u << sectorShift;
    if ((!leadingHeader.empty() && leadingHeader.size() % 512 != 0) ||
        leadingHeader.size() > kMaxArchivePrefixSize) {
        return Status::Failure(
            "Mpq", "PREFIX_ALIGNMENT_INVALID",
            "archive prefix must be 512-byte aligned and no larger than 16 MiB");
    }
    if (files.size() > kMaxInputFiles) {
        return Status::Failure(
            "Mpq", "COUNT_OVERFLOW",
            "archive member count exceeds the bounded writer inventory");
    }
    std::set<std::string> canonicalNames;
    for (const auto& file : files) {
        auto canonical = CanonicalBuildName(file.name);
        if (!canonical.Succeeded() || !canonical.value.has_value()) {
            Status failure;
            failure.diagnostics = std::move(canonical.diagnostics);
            return failure;
        }
        if (*canonical.value == "(LISTFILE)") {
            return Status::Failure(
                "Mpq", "LISTFILE_INPUT_REJECTED", "(listfile) is generated by the writer");
        }
        if (!canonicalNames.insert(*canonical.value).second) {
            return Status::Failure(
                "Mpq", "BUILD_NAME_DUPLICATE",
                "duplicate case/slash canonical archive member name: " + file.name);
        }
        if (file.data.size() > (std::numeric_limits<std::uint32_t>::max)()) {
            return Status::Failure("Mpq", "FILE_SIZE_OVERFLOW", "archive member exceeds uint32");
        }
        if (file.raw) {
            if (const auto valid = ValidateRawBlock(*file.raw, sectorSize); !valid.Succeeded()) return valid;
        }
    }
    return ValidateListfileSize(files);
}

Status WriteMpqArchiveToHandle(
    void* outputHandle, std::span<const MpqBuildFile> files,
    std::span<const std::byte> leadingHeader, std::uint16_t sectorShift, const ProjectSaveControl* control) {
    if (control && !control->Checkpoint(ProjectSaveStage::Build)) return ProjectSaveCancelledStatus();
    if (const auto valid = ValidateSectorShift(sectorShift); !valid.Succeeded()) return valid;
    if (outputHandle == nullptr || outputHandle == INVALID_HANDLE_VALUE) {
        return Status::Failure(
            "Mpq", "OUTPUT_HANDLE_INVALID", "exclusive output handle is invalid");
    }
    if (const auto valid = ValidateMpqArchiveInputs(files, leadingHeader, sectorShift); !valid.Succeeded()) return valid;
    const auto& crypt = MpqCrypt::Instance();
    const std::uint32_t sectorSize = 512u << sectorShift;

    // 单独生成 listfile；不复制调用方全部成员正文。
    MpqBuildFile listFile;
    {
        std::string listContent;
        for (const auto& f : files) {
            if (control && !control->Checkpoint(ProjectSaveStage::Build)) return ProjectSaveCancelledStatus();
            listContent += f.name;
            listContent += "\r\n";
        }
        listFile.name = "(listfile)";
        listFile.data.resize(listContent.size());
        if (!listContent.empty()) {
            std::memcpy(listFile.data.data(), listContent.data(), listContent.size());
        }
        listFile.compress = true;
    }

    const std::uint32_t fileCount = static_cast<std::uint32_t>(files.size() + 1);
    std::uint32_t hashCount = 0;
    if (!NextPow2((std::max)(std::uint64_t{4}, static_cast<std::uint64_t>(fileCount) * 2),
                  hashCount)) {
        return Status::Failure("Mpq", "HASH_COUNT_OVERFLOW", "hash table size exceeds uint32");
    }

    // 构建每个文件的归档数据。
    std::vector<BuiltFile> built(fileCount);
    for (std::uint32_t i = 0; i < fileCount; ++i) {
        if (control && !control->Checkpoint(ProjectSaveStage::Build, i)) return ProjectSaveCancelledStatus();
        const MpqBuildFile& src = i < files.size() ? files[i] : listFile;
        BuiltFile& bf = built[i];
        bf.name = src.name;
        if (src.raw) {
            bf.fileSize = src.raw->fileSize;
            bf.flags = src.raw->flags;
            bf.reused = src.raw->bytes;
            continue;
        }
        bf.fileSize = static_cast<std::uint32_t>(src.data.size());

        if (src.data.empty()) {
            bf.flags = kFileExists;
            continue;
        }

        const std::uint32_t sectorCount = static_cast<std::uint32_t>(
            (static_cast<std::uint64_t>(bf.fileSize) + sectorSize - 1) / sectorSize);
        const bool useCompression = src.compress && bf.fileSize > 0;

        if (!useCompression) {
            // 未压缩：直接存原始数据。
            bf.flags = kFileExists;
            bf.archiveData.resize(src.data.size());
            if (!CopyProjectSaveBytes(bf.archiveData, src.data, control)) return ProjectSaveCancelledStatus();
        } else {
            // 压缩：扇区偏移表 + 各扇区。
            bf.flags = kFileExists | kFileCompress;
            const std::size_t offsetTableSize = (sectorCount + 1) * 4;
            bf.archiveData.resize(offsetTableSize);

            std::vector<std::uint32_t> offsets(sectorCount + 1);
            offsets[0] = static_cast<std::uint32_t>(offsetTableSize);

            for (std::uint32_t s = 0; s < sectorCount; ++s) {
                if (control && !control->Checkpoint(ProjectSaveStage::Build, static_cast<std::uint64_t>(s) * sectorSize))
                    return ProjectSaveCancelledStatus();
                const std::size_t start = static_cast<std::size_t>(s) * sectorSize;
                const std::size_t len =
                    std::min<std::size_t>(sectorSize, src.data.size() - start);
                std::span<const std::byte> sector(
                    src.data.data() + start, len);
                auto sectorData = CompressSector(sector, true);
                bf.archiveData.insert(
                    bf.archiveData.end(), sectorData.begin(), sectorData.end());
                if (bf.archiveData.size() > (std::numeric_limits<std::uint32_t>::max)()) {
                    return Status::Failure(
                        "Mpq", "COMPRESSED_SIZE_OVERFLOW", "compressed member exceeds uint32");
                }
                offsets[s + 1] = static_cast<std::uint32_t>(bf.archiveData.size());
            }
            // 写入偏移表。
            for (std::uint32_t s = 0; s <= sectorCount; ++s) {
                PutU32(bf.archiveData, s * 4, offsets[s]);
            }
        }
    }

    // 计算布局。
    const std::uint64_t leadingSize = leadingHeader.size();
    const std::uint64_t archiveBase = leadingSize;

    std::uint64_t dataPos = archiveBase + kHeaderSize;
    std::vector<std::uint32_t> filePositions(fileCount);
    for (std::uint32_t i = 0; i < fileCount; ++i) {
        if (control && !control->Checkpoint(ProjectSaveStage::Build, i)) return ProjectSaveCancelledStatus();
        if (dataPos - archiveBase > (std::numeric_limits<std::uint32_t>::max)()) {
            return Status::Failure("Mpq", "ARCHIVE_OFFSET_OVERFLOW", "member offset exceeds uint32");
        }
        filePositions[i] = static_cast<std::uint32_t>(dataPos - archiveBase);
        dataPos += built[i].Bytes().size();
    }
    const std::uint64_t hashPos = dataPos;
    const std::uint64_t blockPos = hashPos +
        static_cast<std::uint64_t>(hashCount) * 16;
    const std::uint64_t totalSize = blockPos +
        static_cast<std::uint64_t>(fileCount) * 16;
    if (totalSize < archiveBase ||
        totalSize - archiveBase > (std::numeric_limits<std::uint32_t>::max)() ||
        totalSize > (std::numeric_limits<std::size_t>::max)()) {
        return Status::Failure("Mpq", "ARCHIVE_SIZE_OVERFLOW", "archive size exceeds uint32");
    }

    // 构建哈希表。
    struct HashEntry {
        std::uint32_t name1, name2, localePlatform, blockIndex;
    };
    std::vector<HashEntry> hashTable(hashCount,
        {kHashEmpty, kHashEmpty, kHashEmpty, kHashEmpty});
    for (std::uint32_t i = 0; i < fileCount; ++i) {
        if (control && !control->Checkpoint(ProjectSaveStage::Build, i)) return ProjectSaveCancelledStatus();
        const std::uint32_t bucket =
            crypt.HashString(built[i].name, MpqCrypt::HashType::TableOffset) &
            (hashCount - 1);
        std::uint32_t idx = bucket;
        while (hashTable[idx].blockIndex != kHashEmpty) {
            if (control && !control->Checkpoint(ProjectSaveStage::Build, i)) return ProjectSaveCancelledStatus();
            idx = (idx + 1) & (hashCount - 1);
        }
        hashTable[idx] = {
            crypt.HashString(built[i].name, MpqCrypt::HashType::NameA),
            crypt.HashString(built[i].name, MpqCrypt::HashType::NameB),
            0, i};
    }

    // 序列化整个归档到内存。
    std::vector<std::byte> image(static_cast<std::size_t>(totalSize), std::byte{0});

    // 前置头。
    if (!leadingHeader.empty()) {
        std::memcpy(image.data(), leadingHeader.data(), leadingHeader.size());
    }

    // MPQ 头。
    PutU32(image, static_cast<std::size_t>(archiveBase) + 0x00, kMpqSignature);
    PutU32(image, static_cast<std::size_t>(archiveBase) + 0x04, kHeaderSize);
    PutU32(image, static_cast<std::size_t>(archiveBase) + 0x08,
           static_cast<std::uint32_t>(totalSize - archiveBase));
    PutU32(image, static_cast<std::size_t>(archiveBase) + 0x0C,
           static_cast<std::uint32_t>(sectorShift) << 16); // version=0
    PutU32(image, static_cast<std::size_t>(archiveBase) + 0x10,
           static_cast<std::uint32_t>(hashPos - archiveBase));
    PutU32(image, static_cast<std::size_t>(archiveBase) + 0x14,
           static_cast<std::uint32_t>(blockPos - archiveBase));
    PutU32(image, static_cast<std::size_t>(archiveBase) + 0x18, hashCount);
    PutU32(image, static_cast<std::size_t>(archiveBase) + 0x1C, fileCount);

    // 文件数据。
    for (std::uint32_t i = 0; i < fileCount; ++i) {
        const auto bytes = built[i].Bytes();
        if (!bytes.empty()) {
            if (!CopyProjectSaveBytes(std::span(image).subspan(static_cast<std::size_t>(archiveBase) + filePositions[i], bytes.size()), bytes, control))
                return ProjectSaveCancelledStatus();
        }
    }

    // 哈希表（加密）。
    {
        std::vector<std::byte> hashBytes(hashCount * 16);
        for (std::uint32_t i = 0; i < hashCount; ++i) {
            if (control && (i % 256 == 0) && !control->Checkpoint(ProjectSaveStage::Build, i)) return ProjectSaveCancelledStatus();
            const std::size_t at = i * 16;
            PutU32(hashBytes, at + 0, hashTable[i].name1);
            PutU32(hashBytes, at + 4, hashTable[i].name2);
            PutU32(hashBytes, at + 8, hashTable[i].localePlatform);
            PutU32(hashBytes, at + 12, hashTable[i].blockIndex);
        }
        crypt.EncryptBlock(hashBytes.data(), hashBytes.size(),
            crypt.HashString("(hash table)", MpqCrypt::HashType::FileKey));
        std::memcpy(image.data() + hashPos, hashBytes.data(), hashBytes.size());
    }
    
    // 块表（加密）。
    {
        std::vector<std::byte> blockBytes(fileCount * 16);
        for (std::uint32_t i = 0; i < fileCount; ++i) {
            if (control && (i % 256 == 0) && !control->Checkpoint(ProjectSaveStage::Build, i)) return ProjectSaveCancelledStatus();
            const std::size_t at = i * 16;
            PutU32(blockBytes, at + 0, filePositions[i]);
            PutU32(blockBytes, at + 4,
                   static_cast<std::uint32_t>(built[i].Bytes().size()));
            PutU32(blockBytes, at + 8, built[i].fileSize);
            PutU32(blockBytes, at + 12, built[i].flags);
        }
        crypt.EncryptBlock(blockBytes.data(), blockBytes.size(),
            crypt.HashString("(block table)", MpqCrypt::HashType::FileKey));
        std::memcpy(image.data() + blockPos, blockBytes.data(), blockBytes.size());
    }

    return WriteHandle(static_cast<HANDLE>(outputHandle), image, control);
}

Status WriteMpqArchive(
    const std::filesystem::path& outPath,
    std::span<const MpqBuildFile> files,
    std::span<const std::byte> leadingHeader,
    std::uint16_t sectorShift) {
    HANDLE handle = CreateFileW(
        outPath.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE, FILE_SHARE_READ,
        nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return Status::Failure(
            "Mpq", "OUTPUT_CREATE_EXCLUSIVE_FAILED",
            "output must not exist and must be exclusively creatable: " + outPath.string());
    }
    auto status = WriteMpqArchiveToHandle(handle, files, leadingHeader, sectorShift);
    if (!status.Succeeded()) {
        FILE_DISPOSITION_INFO disposition{TRUE};
        SetFileInformationByHandle(
            handle, FileDispositionInfo, &disposition, sizeof(disposition));
    }
    CloseHandle(handle);
    return status;
}

} // namespace War3Map::Mpq
