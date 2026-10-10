#include "War3MapCore/Update/UpdateArchive.h"
#include "War3MapCore/Hash.h"
#include "UpdatePath.h"
#include "UpdateInstallerInternal.h"

#include <miniz.h>
#include <sodium.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace War3Map::Update {
namespace {

using PackageResult = Result<UpdatePackageVerification>;
PackageResult Failure(std::string code, std::string message, const std::filesystem::path& path) {
    auto result = PackageResult::Failure("Update", std::move(code), std::move(message));
    result.diagnostics.back().origin = OriginSpan{path};
    return result;
}
std::string CaseKey(std::string_view value) {
    std::string result(value);
    for (auto& c : result) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    return result;
}

struct ZipMemoryBudget { std::size_t used = 0; static constexpr std::size_t limit = 16 * 1024 * 1024; };
struct alignas(std::max_align_t) Allocation { std::size_t bytes = 0; };
void* Allocate(void* opaque, size_t items, size_t size) {
    auto& budget = *static_cast<ZipMemoryBudget*>(opaque);
    if (size && items > ZipMemoryBudget::limit / size) return nullptr;
    const auto bytes = items * size;
    if (bytes > ZipMemoryBudget::limit - budget.used) return nullptr;
    auto* block = static_cast<Allocation*>(std::calloc(1, sizeof(Allocation) + bytes));
    if (!block) return nullptr;
    block->bytes = bytes;
    budget.used += bytes;
    return block + 1;
}
void Free(void* opaque, void* memory) {
    if (!memory) return;
    auto& budget = *static_cast<ZipMemoryBudget*>(opaque);
    auto* block = static_cast<Allocation*>(memory) - 1;
    budget.used -= block->bytes;
    std::free(block);
}
void* Reallocate(void* opaque, void* memory, size_t items, size_t size) {
    if (!memory) return Allocate(opaque, items, size);
    auto& budget = *static_cast<ZipMemoryBudget*>(opaque);
    auto* block = static_cast<Allocation*>(memory) - 1;
    if (size && items > ZipMemoryBudget::limit / size) return nullptr;
    const auto bytes = items * size;
    if (bytes > ZipMemoryBudget::limit - (budget.used - block->bytes)) return nullptr;
    const auto oldBytes = block->bytes;
    auto* replacement = static_cast<Allocation*>(std::realloc(block, sizeof(Allocation) + bytes));
    if (!replacement) return nullptr;
    replacement->bytes = bytes;
    budget.used = budget.used - oldBytes + bytes;
    return replacement + 1;
}
size_t ReadZip(void* opaque, mz_uint64 offset, void* buffer, size_t bytes) {
    auto& lease = *static_cast<Detail::FileReadLease*>(opaque);
    if (offset > lease.bytes || bytes > lease.bytes - offset || offset > INT64_MAX) return 0;
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(lease.file.Get(), position, nullptr, FILE_BEGIN)) return 0;
    std::size_t total = 0;
    while (total < bytes) {
        const auto requested = static_cast<DWORD>((std::min)(bytes - total, std::size_t{1024 * 1024}));
        DWORD read = 0;
        if (!ReadFile(lease.file.Get(), static_cast<std::byte*>(buffer) + total, requested, &read, nullptr)
            || read != requested) return total;
        total += read;
    }
    return total;
}
struct ZipReader {
    ZipMemoryBudget memory;
    mz_zip_archive archive{};
    ~ZipReader() { if (archive.m_pState) (void)mz_zip_reader_end(&archive); }
    bool Open(Detail::FileReadLease& lease) {
        archive.m_pAlloc = Allocate;
        archive.m_pRealloc = Reallocate;
        archive.m_pFree = Free;
        archive.m_pAlloc_opaque = &memory;
        archive.m_pRead = ReadZip;
        archive.m_pIO_opaque = &lease;
        return mz_zip_reader_init(&archive, lease.bytes, MZ_ZIP_FLAG_DO_NOT_SORT_CENTRAL_DIRECTORY) != 0;
    }
};
struct HashOutput {
    crypto_hash_sha256_state state{};
    std::uint64_t expected = 0;
    std::uint64_t received = 0;
    bool failed = false;
    HANDLE sink = INVALID_HANDLE_VALUE;
};
size_t HashChunk(void* opaque, mz_uint64 offset, const void* bytes, size_t size) {
    auto& output = *static_cast<HashOutput*>(opaque);
    if (output.failed || offset != output.received || size > output.expected - output.received
        || crypto_hash_sha256_update(&output.state, static_cast<const unsigned char*>(bytes),
            static_cast<unsigned long long>(size)) != 0) {
        output.failed = true;
        return 0;
    }
    if (output.sink != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        if (size > MAXDWORD || !WriteFile(output.sink, bytes, static_cast<DWORD>(size), &written, nullptr)
            || written != size) { output.failed = true; return 0; }
    }
    output.received += size;
    return size;
}
bool FileTypeAllowed(const mz_zip_archive_file_stat& stat, bool directory) {
    if (stat.m_external_attr & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE)) return false;
    if ((stat.m_version_made_by >> 8) == 3) {
        const auto type = (stat.m_external_attr >> 16) & 0xf000;
        if (type != 0 && type != (directory ? 0x4000u : 0x8000u)) return false;
    }
    return true;
}

} // namespace

static PackageResult VerifyLockedPackage(const VerifiedUpdateManifest& verified, Detail::FileReadLease& locked,
    const std::filesystem::path* stage) {
    const auto& packagePath = locked.path;
    try {
        const auto& manifest = verified.Manifest();
        if (locked.bytes != manifest.packageBytes) return Failure("UPDATE_PACKAGE_SIZE_MISMATCH",
            "更新ZIP实际大小与签名清单不符。", packagePath);
        const auto digest = Sha256Handle(locked.file.Get());
        if (!digest.Succeeded()) return {std::nullopt, digest.diagnostics};
        if (*digest.value != manifest.packageSha256) return Failure("UPDATE_PACKAGE_SHA_MISMATCH",
            "更新ZIP的SHA-256与签名清单不符。", packagePath);
        ZipReader reader;
        if (!reader.Open(locked)) return Failure("UPDATE_ZIP_INVALID", "无法打开有界的单卷ZIP归档。", packagePath);
        const auto count = mz_zip_reader_get_num_files(&reader.archive);
        if (count > kMaxUpdateFiles * 2 || mz_zip_get_central_dir_size(&reader.archive) > 8 * 1024 * 1024) {
            return Failure("UPDATE_ZIP_BUDGET", "ZIP成员数或central directory超过容量。", packagePath);
        }
        std::unordered_map<std::string, const UpdateFileEntry*> allowed;
        std::unordered_set<std::string> directories, seen;
        for (const auto& file : manifest.files) {
            allowed.emplace(file.path, &file);
            for (auto end = file.path.find('/'); end != std::string::npos; end = file.path.find('/', end + 1))
                directories.insert(file.path.substr(0, end));
        }
        UpdatePackageVerification result;
        result.packagePath = locked.path;
        result.bytes = locked.bytes;
        result.sha256 = *digest.value;
        result.manifestSha256 = verified.ManifestSha256();
        for (mz_uint index = 0; index < count; ++index) {
            mz_zip_archive_file_stat stat{};
            if (!mz_zip_reader_file_stat(&reader.archive, index, &stat) || stat.m_is_encrypted || !stat.m_is_supported
                || (stat.m_method != 0 && stat.m_method != 8) || !FileTypeAllowed(stat, stat.m_is_directory != 0)) {
                return Failure("UPDATE_ZIP_MEMBER_TYPE_REJECTED", "ZIP含加密、链接、设备或不支持的成员类型。", packagePath);
            }
            const auto nameBytes = mz_zip_reader_get_filename(&reader.archive, index, nullptr, 0);
            if (nameBytes <= 1 || nameBytes > 1026) return Failure("UPDATE_ZIP_PATH_INVALID", "ZIP成员文件名为空或超限。", packagePath);
            std::string name(nameBytes, '\0');
            if (mz_zip_reader_get_filename(&reader.archive, index, name.data(), nameBytes) != nameBytes
                || name.back() != '\0') return Failure("UPDATE_ZIP_PATH_INVALID", "ZIP成员文件名无法完整读取。", packagePath);
            name.pop_back();
            const bool directory = stat.m_is_directory != 0;
            if (directory) {
                if (!name.ends_with('/') || stat.m_uncomp_size != 0) return Failure("UPDATE_ZIP_DIRECTORY_INVALID",
                    "ZIP目录必须是明确的空父目录成员。", packagePath);
                name.pop_back();
            }
            if (!ValidateUpdateRelativePath(name).Succeeded() || !seen.insert(CaseKey(name)).second) return Failure(
                "UPDATE_ZIP_PATH_COLLISION", "ZIP含路径越界、ADS、重复路径或大小写碰撞。", packagePath);
            if (!mz_zip_validate_file(&reader.archive, index, MZ_ZIP_FLAG_VALIDATE_HEADERS_ONLY)) return Failure(
                "UPDATE_ZIP_HEADERS_INVALID", "ZIP local/central header身份、边界或长度不一致。", packagePath);
            if (directory) {
                if (!directories.contains(name)) return Failure("UPDATE_ZIP_EXTRA_MEMBER", "ZIP含签名清单不需要的目录成员。", packagePath);
                continue;
            }
            const auto found = allowed.find(name);
            if (found == allowed.end()) return Failure("UPDATE_ZIP_EXTRA_MEMBER", "ZIP含签名allowlist之外的程序文件。", packagePath);
            const auto& expected = *found->second;
            if (stat.m_uncomp_size != expected.bytes || stat.m_uncomp_size > kMaxUpdateFileBytes
                || stat.m_uncomp_size > kMaxUpdateExpandedBytes - result.expandedBytes) return Failure(
                    "UPDATE_ZIP_MEMBER_SIZE_MISMATCH", "ZIP成员长度与签名清单不符或超过解压容量。", packagePath);
            Detail::NativeHandle sink;
            std::optional<Detail::DirectoryReadLease> sinkParent;
            if (stage) {
                const auto destination = *stage / std::filesystem::path(std::wstring(name.begin(), name.end()));
                auto parent = Detail::LockSafeDirectory(destination.parent_path());
                if (!parent.Succeeded()) return {std::nullopt, std::move(parent.diagnostics)};
                sinkParent.emplace(std::move(*parent.value));
                sink = Detail::NativeHandle(CreateFileW(destination.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                    nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
                if (!sink.Valid()) return Failure("UPDATE_STAGE_PROGRAM_WRITE_FAILED", "无法独占创建签名程序文件。", destination);
                const auto checked = Detail::CheckOpenedPath(sink.Get(), destination, false);
                if (!checked.Succeeded()) return {std::nullopt, checked.diagnostics};
            }
            HashOutput output;
            output.expected = expected.bytes;
            output.sink = sink.Get();
            if (crypto_hash_sha256_init(&output.state) != 0
                || !mz_zip_reader_extract_to_callback(&reader.archive, index, HashChunk, &output, 0)
                || output.failed || output.received != output.expected) return Failure("UPDATE_ZIP_CONTENT_INVALID",
                    "ZIP成员无法完整解压并通过CRC检查，未返回部分内容。", packagePath);
            std::array<unsigned char, crypto_hash_sha256_BYTES> hash{}, expectedHash{};
            if (crypto_hash_sha256_final(&output.state, hash.data()) != 0
                || sodium_hex2bin(expectedHash.data(), expectedHash.size(), expected.sha256.data(), expected.sha256.size(),
                    nullptr, nullptr, nullptr) != 0 || sodium_memcmp(hash.data(), expectedHash.data(), hash.size()) != 0) {
                return Failure("UPDATE_ZIP_MEMBER_SHA_MISMATCH", "ZIP成员内容的SHA-256与签名清单不符。", packagePath);
            }
            if (sink.Valid() && !FlushFileBuffers(sink.Get())) return Failure("UPDATE_STAGE_FLUSH_FAILED",
                "签名程序文件未能完整持久写入，拒绝激活。", packagePath);
            result.expandedBytes += expected.bytes;
            result.files.push_back(expected);
            allowed.erase(found);
        }
        if (!allowed.empty()) return Failure("UPDATE_ZIP_MISSING_MEMBER", "ZIP缺少签名程序文件，拒绝不完整包。", packagePath);
        std::sort(result.files.begin(), result.files.end(), [](const auto& a, const auto& b) { return a.path < b.path; });
        return PackageResult::Success(std::move(result));
    } catch (const std::exception&) {
        return Failure("UPDATE_PACKAGE_VERIFY_FAILED", "无法完整验证更新包，未返回部分成功。", packagePath);
    }
}

PackageResult VerifyUpdatePackage(const VerifiedUpdateManifest& verified, const std::filesystem::path& packagePath) {
    auto lease = Detail::OpenSafeReadFile(packagePath, kMaxUpdatePackageBytes);
    if (!lease.Succeeded()) return {std::nullopt, std::move(lease.diagnostics)};
    return VerifyLockedPackage(verified, *lease.value, nullptr);
}

namespace Detail {
Status ExtractUpdatePackageToStage(const VerifiedUpdateManifest& target, FileReadLease& package,
    const std::filesystem::path& stage) {
    const auto result = VerifyLockedPackage(target, package, &stage);
    return {result.diagnostics};
}
} // namespace Detail

} // namespace War3Map::Update
