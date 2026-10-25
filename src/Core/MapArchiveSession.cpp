#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/ProjectSaveControl.h"

#include "War3MapCore/Hash.h"
#include "War3MapCore/Mpq/MpqArchive.h"
#include "War3MapCore/Mpq/MpqWriter.h"
#include "War3MapCore/Resources/ImportTable.h"
#include "Mpq/MpqCrypt.h"
#include "Mpq/MpqWriterInternal.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <exception>
#include <cwctype>
#include <functional>
#include <map>
#include <memory>
#include <set>

namespace War3Map {
namespace {

constexpr std::uint64_t kMaxDecodedBytes = 512ull * 1024ull * 1024ull;
// 单个 .forge 来源成员可包含 340 MiB 的已归档地图；总解码预算仍为 512 MiB。
constexpr std::uint64_t kMaxMemberBytes = 512ull * 1024ull * 1024ull;

template <typename T>
Result<T> ForwardFailure(std::vector<Diagnostic> diagnostics) {
    Result<T> result;
    result.diagnostics = std::move(diagnostics);
    return result;
}

class FileHandle {
public:
    explicit FileHandle(HANDLE handle = INVALID_HANDLE_VALUE) : handle_(handle) {}
    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;
    ~FileHandle() {
        if (Valid()) {
            if (remove_) {
                FILE_DISPOSITION_INFO info{TRUE};
                (void)SetFileInformationByHandle(handle_, FileDispositionInfo, &info, sizeof(info));
            }
            CloseHandle(handle_);
        }
    }
    bool Valid() const { return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE; }
    HANDLE Get() const { return handle_; }
    void RemoveOnClose() { remove_ = true; }
    void Commit() { remove_ = false; }
private:
    HANDLE handle_;
    bool remove_ = false;
};

std::string Canonical(std::string_view name) {
    std::string result(name);
    for (char& value : result) {
        value = value == '/' ? '\\' : static_cast<char>(std::toupper(static_cast<unsigned char>(value)));
    }
    return result;
}

bool SamePath(const std::filesystem::path& left, const std::filesystem::path& right) {
    return _wcsicmp(left.c_str(), right.c_str()) == 0;
}

std::uint64_t FileId(const BY_HANDLE_FILE_INFORMATION& info) {
    return (static_cast<std::uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
}

Result<std::filesystem::path> FinalPath(HANDLE handle) {
    const DWORD needed = GetFinalPathNameByHandleW(handle, nullptr, 0, FILE_NAME_NORMALIZED);
    if (!needed) return Result<std::filesystem::path>::Failure(
        "MapArchiveSession", "FILE_PATH_UNAVAILABLE", "无法确认文件句柄对应的实际路径。");
    std::wstring name(needed, L'\0');
    const DWORD count = GetFinalPathNameByHandleW(handle, name.data(), needed, FILE_NAME_NORMALIZED);
    if (!count || count >= needed) return Result<std::filesystem::path>::Failure(
        "MapArchiveSession", "FILE_PATH_UNAVAILABLE", "无法确认完整文件路径。");
    name.resize(count);
    if (name.starts_with(L"\\\\?\\UNC\\")) name = L"\\\\" + name.substr(8);
    else if (name.starts_with(L"\\\\?\\")) name.erase(0, 4);
    return Result<std::filesystem::path>::Success(std::filesystem::path(name).lexically_normal());
}

Status VerifyIdentity(HANDLE handle, std::uint32_t volume, std::uint64_t id) {
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle, &info) ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        return Status::Failure("MapArchiveSession", "SOURCE_UNAVAILABLE", "无法读取源归档文件身份。");
    }
    if (info.dwVolumeSerialNumber != volume || FileId(info) != id) {
        return Status::Failure("MapArchiveSession", "SOURCE_REPLACED", "源归档已被外部替换，请重新打开。");
    }
    return Status::Ok();
}

Status VerifyHandle(HANDLE handle, std::uint32_t volume, std::uint64_t id,
    const std::string& expectedSha, const ProjectSaveControl* control = nullptr) {
    if (const auto identity = VerifyIdentity(handle, volume, id); !identity.Succeeded()) return identity;
    const auto sha = Sha256Handle(handle, control);
    if (!sha.Succeeded()) return Status{sha.diagnostics};
    if (*sha.value != expectedSha) {
        return Status::Failure("MapArchiveSession", "SOURCE_CHANGED", "源归档已被外部修改，请重新打开。");
    }
    return Status::Ok();
}

Mpq::MpqOpenLimits OpenLimits() {
    Mpq::MpqOpenLimits limits;
    limits.maxArchivePrefixBytes = 16ull * 1024ull * 1024ull;
    limits.maxDeclaredArchiveBytes = 1024ull * 1024ull * 1024ull;
    limits.maxStrictMemberCount = 131072;
    return limits;
}

bool SafeTargetFilename(const std::filesystem::path& path) {
    const auto name = path.filename().wstring();
    if (name.empty() || name == L"." || name == L".." || name.back() == L'.' || name.back() == L' ') return false;
    if (std::any_of(name.begin(), name.end(), [](wchar_t value) {
        return value < 32 || value == L'<' || value == L'>' || value == L':' || value == L'"'
            || value == L'/' || value == L'\\' || value == L'|' || value == L'?' || value == L'*';
    })) return false;
    std::wstring stem = name.substr(0, name.find(L'.'));
    std::transform(stem.begin(), stem.end(), stem.begin(), [](wchar_t c) { return std::towupper(c); });
    static const std::set<std::wstring> reserved{L"CON", L"PRN", L"AUX", L"NUL", L"COM1", L"COM2",
        L"COM3", L"COM4", L"COM5", L"COM6", L"COM7", L"COM8", L"COM9", L"LPT1", L"LPT2",
        L"LPT3", L"LPT4", L"LPT5", L"LPT6", L"LPT7", L"LPT8", L"LPT9"};
    return !reserved.contains(stem);
}

} // namespace

Result<MapArchiveSession> MapArchiveSession::Open(const std::filesystem::path& path, const ProjectSaveControl* control) {
    try {
        FileHandle source(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!source.Valid()) return Result<MapArchiveSession>::Failure(
            "MapArchiveSession", "SOURCE_LEASE_FAILED", "无法只读锁定源归档；请等待外部写入完成。");
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(source.Get(), &info) || (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            return Result<MapArchiveSession>::Failure("MapArchiveSession", "SOURCE_INVALID", "源归档必须是普通文件。");
        }
        auto sourcePath = FinalPath(source.Get());
        if (!sourcePath.Succeeded()) return ForwardFailure<MapArchiveSession>(std::move(sourcePath.diagnostics));
        auto sha = Sha256Handle(source.Get(), control);
        if (!sha.Succeeded()) return ForwardFailure<MapArchiveSession>(std::move(sha.diagnostics));
        auto opened = Mpq::MpqArchive::OpenReadOnlyHandle(source.Get(), *sourcePath.value, OpenLimits());
        if (!opened.Succeeded()) return ForwardFailure<MapArchiveSession>(std::move(opened.diagnostics));
        auto inventory = opened.value->ListFilesStrict();
        const bool opaqueMembers = !inventory.Succeeded() && std::any_of(
            inventory.diagnostics.begin(), inventory.diagnostics.end(), [](const Diagnostic& item) {
                return item.code == "ANONYMOUS_BLOCK_REJECTED";
            });
        if (opaqueMembers) inventory = opened.value->ListFilesPreservingOpaque();
        if (!inventory.Succeeded()) return ForwardFailure<MapArchiveSession>(std::move(inventory.diagnostics));
        LARGE_INTEGER actualSize{};
        if (!GetFileSizeEx(source.Get(), &actualSize) || actualSize.QuadPart < 0 ||
            static_cast<std::uint64_t>(actualSize.QuadPart) !=
                opened.value->ArchivePrefixSize() + opened.value->DeclaredArchiveSize()) {
            return Result<MapArchiveSession>::Failure("MapArchiveSession", "ARCHIVE_TRAILING_DATA",
                "归档声明范围外存在未识别数据，不能在重建时丢弃。");
        }
        std::uint64_t decodedBytes = 0;
        for (const auto& member : *inventory.value) {
            if (member.fileSize > kMaxMemberBytes || member.fileSize > kMaxDecodedBytes - decodedBytes) {
                return Result<MapArchiveSession>::Failure("MapArchiveSession", "SNAPSHOT_BUDGET_EXCEEDED",
                    "归档解码内容超过会话预算（单成员及总计均为 512 MiB）。");
            }
            decodedBytes += member.fileSize;
        }
        auto prefix = opened.value->ReadArchivePrefix();
        if (!prefix.Succeeded()) return ForwardFailure<MapArchiveSession>(std::move(prefix.diagnostics));
        MapArchiveSession session;
        session.sourcePath_ = std::move(*sourcePath.value);
        session.sourceSha256_ = std::move(*sha.value);
        session.volumeSerial_ = info.dwVolumeSerialNumber;
        session.fileId_ = FileId(info);
        session.opaqueMembers_ = opaqueMembers;
        session.opaqueMemberCount_ = opaqueMembers ? opened.value->FileCount() - inventory.value->size() : 0;
        session.prefix_ = std::move(*prefix.value);
        for (const auto& member : *inventory.value) {
            if (Mpq::IsMpqMetadataMemberName(member.name)) {
                session.metadataMembers_.push_back(member.name);
                continue;
            }
            auto bytes = opened.value->ReadFile(member.name, control);
            if (!bytes.Succeeded()) return ForwardFailure<MapArchiveSession>(std::move(bytes.diagnostics));
            if (Canonical(member.name) == "(LISTFILE)") session.listfile_ = std::move(*bytes.value);
            else session.members_.push_back({member.name, std::move(*bytes.value)});
        }
        const auto verified = VerifyHandle(source.Get(), session.volumeSerial_, session.fileId_, session.sourceSha256_, control);
        if (!verified.Succeeded()) return ForwardFailure<MapArchiveSession>(verified.diagnostics);
        return Result<MapArchiveSession>::Success(std::move(session));
    } catch (const std::exception&) {
        return Result<MapArchiveSession>::Failure("MapArchiveSession", "SNAPSHOT_FAILED", "读取归档快照失败，会话未建立。");
    }
}

Status MapArchiveSession::VerifySourceUnchanged(const ProjectSaveControl* control) const {
    FileHandle source(CreateFileW(sourcePath_.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!source.Valid()) return Status::Failure("MapArchiveSession", "SOURCE_LEASE_FAILED", "源归档不可读取或正在被外部写入。");
    return VerifyHandle(source.Get(), volumeSerial_, fileId_, sourceSha256_, control);
}

bool MapArchiveSession::HasMember(std::string_view name) const {
    const auto key = Canonical(name);
    if (key == "(LISTFILE)") return true;
    return std::any_of(members_.begin(), members_.end(), [&key](const auto& member) {
        return Canonical(member.name) == key;
    });
}

Result<std::vector<std::byte>> MapArchiveSession::ReadMember(std::string_view name, const ProjectSaveControl* control) const {
    const auto key = Canonical(name);
    const auto copy = [control](const std::vector<std::byte>& input) -> Result<std::vector<std::byte>> {
        std::vector<std::byte> result(input.size());
        if (!CopyProjectSaveBytes(result, input, control)) return {{}, ProjectSaveCancelledStatus().diagnostics};
        return Result<std::vector<std::byte>>::Success(std::move(result));
    };
    if (key == "(LISTFILE)") return copy(listfile_);
    const auto found = std::find_if(members_.begin(), members_.end(), [&key](const auto& member) {
        return Canonical(member.name) == key;
    });
    if (found == members_.end()) return Result<std::vector<std::byte>>::Failure(
        "MapArchiveSession", "MEMBER_MISSING", "归档快照中没有成员：" + std::string(name));
    return copy(found->data);
}

Result<std::vector<ArchiveExactMember>> MapArchiveSession::CaptureExactMembers(
    std::span<const std::string> names, const ArchiveExactMemberLimits& limits) const {
    using Output = std::vector<ArchiveExactMember>;
    const auto fail = [](std::string code, std::string message) {
        return Result<Output>::Failure("MapArchiveSession", std::move(code), std::move(message));
    };
    try {
        const ArchiveExactMemberLimits hard;
        if (limits.memberCount > hard.memberCount || limits.pathBytes > hard.pathBytes ||
            limits.totalPathBytes > hard.totalPathBytes || limits.memberBytes > hard.memberBytes ||
            limits.totalBytes > hard.totalBytes || names.size() > limits.memberCount) {
            return fail("EXACT_MEMBER_LIMIT", "明确来源捕获只能收紧硬预算，且请求数量不能超限。");
        }
        std::size_t paths = 0;
        std::vector<std::string> canonicalNames;
        canonicalNames.reserve(names.size());
        std::map<std::string, const MapArchiveMember*> namedSnapshots;
        for (const auto& name : names) {
            if (name.size() > limits.pathBytes || name.size() > limits.totalPathBytes - paths)
                return fail("EXACT_MEMBER_LIMIT", "明确来源请求路径超过单项或累计预算。");
            paths += name.size();
            auto canonical = Resources::CanonicalImportPath(name);
            if (!canonical.Succeeded()) return ForwardFailure<Output>(std::move(canonical.diagnostics));
            if (*canonical.value == "(LISTFILE)" || Mpq::IsMpqMetadataMemberName(*canonical.value))
                return fail("EXACT_MEMBER_RESERVED", "明确来源捕获不接受归档内部保留成员。");
            if (!namedSnapshots.emplace(*canonical.value, nullptr).second)
                return fail("EXACT_MEMBER_NAME_CONFLICT", "明确来源请求包含大小写或斜杠归一后重复的名字。");
            canonicalNames.push_back(std::move(*canonical.value));
        }
        // 只为本次查询记录已有内存祖先，不把匿名查询补进原会话清单。
        for (const auto& member : members_) {
            const auto found = namedSnapshots.find(Canonical(member.name));
            if (found != namedSnapshots.end()) found->second = &member;
        }
        FileHandle source(CreateFileW(sourcePath_.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!source.Valid()) return fail("SOURCE_LEASE_FAILED", "源归档不可读取或正在被外部写入。");
        const auto verified = VerifyHandle(source.Get(), volumeSerial_, fileId_, sourceSha256_);
        if (!verified.Succeeded()) return ForwardFailure<Output>(verified.diagnostics);
        auto sourcePath = FinalPath(source.Get());
        if (!sourcePath.Succeeded()) return ForwardFailure<Output>(std::move(sourcePath.diagnostics));
        if (!SamePath(*sourcePath.value, sourcePath_))
            return fail("SOURCE_REPLACED", "源归档的实际路径已与会话基线不同。");
        auto opened = Mpq::MpqArchive::OpenReadOnlyHandle(source.Get(), sourcePath_, OpenLimits());
        if (!opened.Succeeded()) return ForwardFailure<Output>(std::move(opened.diagnostics));
        // 对正常与匿名归档采用同一全局 hash 唯一性、locale/platform 和块双射证明。
        auto inventory = opened.value->ListFilesPreservingOpaque();
        if (!inventory.Succeeded()) return ForwardFailure<Output>(std::move(inventory.diagnostics));
        const auto hashes = opened.value->HashTableEntries();
        const auto blocks = opened.value->BlockTableEntries();
        const auto& crypt = Mpq::MpqCrypt::Instance();
        std::map<std::pair<std::uint32_t, std::uint32_t>, std::size_t> queriedHashes;
        for (std::size_t index = 0; index < canonicalNames.size(); ++index) {
            const auto& name = canonicalNames[index];
            if (!queriedHashes.emplace(std::pair{crypt.HashString(name, Mpq::MpqCrypt::HashType::NameA),
                    crypt.HashString(name, Mpq::MpqCrypt::HashType::NameB)}, index).second)
                return fail("EXACT_MEMBER_HASH_CONFLICT", "不同明确来源名字具有相同的 MPQ hash 对。");
        }
        std::vector<std::uint32_t> queriedBlocks(names.size(), Mpq::kMpqHashEmpty);
        for (const auto& hash : hashes) {
            if (hash.blockIndex == Mpq::kMpqHashEmpty || hash.blockIndex == Mpq::kMpqHashDeleted) continue;
            const auto wanted = queriedHashes.find({hash.name1, hash.name2});
            if (wanted != queriedHashes.end()) queriedBlocks[wanted->second] = hash.blockIndex;
        }
        Output result;
        result.reserve(names.size());
        std::size_t retained = 0;
        constexpr auto knownFlags = Mpq::kFileImplode | Mpq::kFileCompress | Mpq::kFileEncrypted |
            Mpq::kFileFixKey | Mpq::kFilePatch | Mpq::kFileSingleUnit | Mpq::kFileSectorCrc | Mpq::kFileExists;
        for (std::size_t index = 0; index < names.size(); ++index) {
            const auto& name = canonicalNames[index];
            const auto blockIndex = queriedBlocks[index];
            const bool present = opened.value->HasFile(name);
            // 未列名块的桶可达性不由 listfile 校验覆盖；匹配存在却查不到不能算 Missing。
            if (present != (blockIndex != Mpq::kMpqHashEmpty))
                return fail("EXACT_MEMBER_HASH_UNREACHABLE", "明确来源的名字 hash 存在但桶链不可达：" + names[index]);
            ArchiveExactMember member{names[index], present, {}};
            if (present) {
                const auto& block = blocks[blockIndex]; // inventory 已验证每个有效 hash 的块边界。
                if ((block.flags & ~knownFlags) != 0 ||
                    ((block.flags & Mpq::kFileFixKey) != 0 && (block.flags & Mpq::kFileEncrypted) == 0) ||
                    ((block.flags & Mpq::kFileImplode) != 0 && (block.flags & Mpq::kFileCompress) != 0))
                    return fail("FILE_FLAGS_UNSUPPORTED", "明确来源块标志不受支持：" + names[index]);
                if ((block.flags & (Mpq::kFilePatch | Mpq::kFileSectorCrc)) != 0)
                    return fail("EXACT_MEMBER_ENCODING_UNSUPPORTED", "明确来源为未支持的增量补丁或扇区 CRC 块：" + names[index]);
                const auto remaining = limits.totalBytes - retained;
                const auto maximum = (std::min)(limits.memberBytes, remaining);
                // ReadFileBounded 在读取、偏移表分配和解码前检查存储与输出长度。
                auto bytes = opened.value->ReadFileBounded(name, maximum);
                if (!bytes.Succeeded()) return ForwardFailure<Output>(std::move(bytes.diagnostics));
                retained += bytes.value->size();
                member.bytes = std::move(*bytes.value);
            }
            const auto* snapshot = namedSnapshots.at(name);
            if (snapshot && (!present || snapshot->data != member.bytes))
                return fail("EXACT_MEMBER_SNAPSHOT_MISMATCH", "明确来源与会话已有命名快照不一致：" + names[index]);
            result.push_back(std::move(member));
        }
        // 全批次共用同一禁止写入/删除的句柄；末尾只复核身份和路径，不再次散列。
        const auto identity = VerifyIdentity(source.Get(), volumeSerial_, fileId_);
        if (!identity.Succeeded()) return ForwardFailure<Output>(identity.diagnostics);
        const auto pathIdentity = VerifySourceIdentity();
        if (!pathIdentity.Succeeded()) return ForwardFailure<Output>(pathIdentity.diagnostics);
        return Result<Output>::Success(std::move(result));
    } catch (const std::exception&) {
        return fail("EXACT_MEMBER_CAPTURE_FAILED", "明确来源捕获未完成，未返回部分结果。");
    }
}

Result<ArchiveExactIdentityResult> MapArchiveSession::DescribeExactMemberIdentities(
    std::string_view expectedSourceSha256, std::span<const std::string> names,
    const ArchiveExactMemberLimits& limits) const {
    using Output = ArchiveExactIdentityResult;
    const auto fail = [](std::string code, std::string message) {
        return Result<Output>::Failure("MapArchiveSession", std::move(code), std::move(message));
    };
    try {
        if (expectedSourceSha256.size() != 64 ||
            !std::all_of(expectedSourceSha256.begin(), expectedSourceSha256.end(), [](unsigned char value) {
                return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f') ||
                    (value >= 'A' && value <= 'F');
            })) return fail("EXACT_IDENTITY_SOURCE_INVALID", "精确成员身份查询必须提供完整的64位十六进制源SHA-256。");
        std::string expected(expectedSourceSha256);
        std::transform(expected.begin(), expected.end(), expected.begin(), [](unsigned char value) {
            return static_cast<char>(std::tolower(value));
        });
        if (expected != sourceSha256_)
            return fail("EXACT_IDENTITY_SOURCE_STALE", "精确成员身份查询的期望源SHA与当前会话不同；须重新打开并绑定来源。");
        // 捕获负责源hash/文件身份、同批只读租约、精确正负证明与输入/解码预算。
        // 本层只消费已拥有字节，不使用Members/HasMember作为存在性证据，也不再访问磁盘。
        auto captured = CaptureExactMembers(names, limits);
        if (!captured.Succeeded()) return ForwardFailure<Output>(std::move(captured.diagnostics));
        Output output;
        output.sourceSha256 = sourceSha256_;
        output.members.reserve(captured.value->size());
        for (const auto& member : *captured.value) {
            ArchiveExactMemberIdentity identity;
            identity.name = member.name;
            identity.present = member.present;
            if (member.present) {
                auto hash = Sha256(member.bytes);
                if (!hash.Succeeded()) return ForwardFailure<Output>(std::move(hash.diagnostics));
                identity.byteSize = static_cast<std::uint64_t>(member.bytes.size());
                identity.sha256 = std::move(*hash.value);
            }
            output.members.push_back(std::move(identity));
        }
        auto result = Result<Output>::Success(std::move(output));
        result.diagnostics = std::move(captured.diagnostics);
        return result;
    } catch (const std::exception&) {
        return fail("EXACT_IDENTITY_DESCRIBE_FAILED", "精确成员身份查询未完成，未返回部分结果。");
    }
}

namespace {

std::uint64_t DecodedSize(const Mpq::MpqBuildFile& file) {
    return file.raw ? file.raw->fileSize : file.data.size();
}

// 回读验证：原样复用的块逐字节比较原始块（与源一致即内容一致），其余成员完整解压比较。
Status VerifyCandidateMembers(const Mpq::MpqArchive& candidate, std::span<const Mpq::MpqBuildFile> files,
    const ProjectSaveControl* control = nullptr) {
    for (const auto& file : files) {
        if (control && !control->Checkpoint(ProjectSaveStage::Verify)) return ProjectSaveCancelledStatus();
        if (file.raw) {
            auto actual = candidate.ReadRawBlock(file.name);
            if (!actual.Succeeded()) return Status{std::move(actual.diagnostics)};
            if (actual.value->bytes != file.raw->bytes || actual.value->flags != file.raw->flags ||
                actual.value->fileSize != file.raw->fileSize) {
                return Status::Failure("MapArchiveSession", "CANDIDATE_MEMBER_MISMATCH", "候选成员原始块回读不一致：" + file.name);
            }
            continue;
        }
        auto actual = candidate.ReadFile(file.name, control);
        if (!actual.Succeeded()) return Status{std::move(actual.diagnostics)};
        if (!EqualProjectSaveBytes(*actual.value, file.data, control)) {
            if (control && control->CancellationRequested()) return ProjectSaveCancelledStatus();
            return Status::Failure(
            "MapArchiveSession", "CANDIDATE_MEMBER_MISMATCH", "候选成员完整回读不一致：" + file.name);
        }
    }
    return Status::Ok();
}

struct CandidateTargetLease {
    std::filesystem::path parent;
    std::filesystem::path target;
    std::vector<std::unique_ptr<FileHandle>> handles;
};

Result<CandidateTargetLease> LockCandidateTarget(const std::filesystem::path& targetPath) {
    if (!SafeTargetFilename(targetPath)) return Result<CandidateTargetLease>::Failure(
        "MapArchiveSession", "TARGET_PATH_INVALID", "候选路径必须包含合法文件名。");
    std::error_code ec;
    const auto absoluteTarget = std::filesystem::absolute(targetPath, ec).lexically_normal();
    if (ec) return Result<CandidateTargetLease>::Failure("MapArchiveSession", "TARGET_PATH_INVALID", "无法解析候选路径。");
    CandidateTargetLease lease;
    lease.handles.push_back(std::make_unique<FileHandle>(CreateFileW(absoluteTarget.parent_path().c_str(),
        FILE_READ_ATTRIBUTES | FILE_TRAVERSE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr)));
    if (!lease.handles.front()->Valid()) return Result<CandidateTargetLease>::Failure(
        "MapArchiveSession", "TARGET_PARENT_INVALID", "候选目录必须已存在且可访问。");
    auto parent = FinalPath(lease.handles.front()->Get());
    if (!parent.Succeeded()) return ForwardFailure<CandidateTargetLease>(std::move(parent.diagnostics));
    // 目标使用已解析路径，锁住所有可重命名的祖先，防止发布时路径被转向。
    for (auto ancestor = parent.value->parent_path(); !ancestor.empty();) {
        const auto next = ancestor.parent_path();
        if (next.empty() || SamePath(ancestor, next)) break;
        auto handle = std::make_unique<FileHandle>(CreateFileW(ancestor.c_str(),
            FILE_READ_ATTRIBUTES | FILE_TRAVERSE, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
        if (!handle->Valid()) return Result<CandidateTargetLease>::Failure(
            "MapArchiveSession", "TARGET_ANCESTOR_LEASE_FAILED", "无法锁定候选目录的完整路径。");
        lease.handles.push_back(std::move(handle));
        ancestor = next;
    }
    const auto stableParent = FinalPath(lease.handles.front()->Get());
    if (!stableParent.Succeeded() || !SamePath(*stableParent.value, *parent.value)) {
        return Result<CandidateTargetLease>::Failure(
            "MapArchiveSession", "TARGET_DIRECTORY_CHANGED", "候选目录在锁定期间移动。");
    }
    const auto target = *parent.value / absoluteTarget.filename();
    if (std::filesystem::exists(target, ec) || ec) return Result<CandidateTargetLease>::Failure(
        "MapArchiveSession", "TARGET_EXISTS", "候选路径已存在或无法确认，不允许覆盖。");
    lease.parent = std::move(*parent.value);
    lease.target = target;
    return Result<CandidateTargetLease>::Success(std::move(lease));
}

Result<ArchiveSaveResult> WriteArchiveWithGuard(
    const std::filesystem::path& targetPath, std::span<const Mpq::MpqBuildFile> files,
    std::span<const std::byte> leadingHeader, const std::function<Status()>& finalGuard,
    const Mpq::MpqArchive* opaqueSource = nullptr, std::uint16_t sectorShift = Mpq::kMpqWriterSectorShift,
    bool preflight = false, ProjectSaveControl* control = nullptr, bool finalPublication = true) {
    try {
        std::optional<CandidateTargetLease> destination;
        if (!preflight || !targetPath.empty()) {
            auto locked = LockCandidateTarget(targetPath);
            if (!locked.Succeeded()) return ForwardFailure<ArchiveSaveResult>(std::move(locked.diagnostics));
            destination = std::move(*locked.value);
        }
        std::uint64_t decodedBytes = 0;
        for (const auto& file : files) {
            const auto size = DecodedSize(file);
            if (size > kMaxMemberBytes || size > kMaxDecodedBytes - decodedBytes) {
                return Result<ArchiveSaveResult>::Failure("MapArchiveSession", "SNAPSHOT_BUDGET_EXCEEDED", "候选成员超过会话解码预算。");
            }
            decodedBytes += size;
        }
        if (preflight) {
            const auto inputs = opaqueSource ? opaqueSource->ValidateOpaquePreservingPatchInputs(files)
                : Mpq::ValidateMpqArchiveInputs(files, leadingHeader, sectorShift);
            if (!inputs.Succeeded()) return ForwardFailure<ArchiveSaveResult>(inputs.diagnostics);
            const auto verified = finalGuard();
            if (!verified.Succeeded()) return ForwardFailure<ArchiveSaveResult>(verified.diagnostics);
            return Result<ArchiveSaveResult>::Success({}); // 仅内部状态，不是写出回执。
        }
        const auto& target = destination->target;
        const auto& parent = destination->parent;
        static std::atomic<std::uint64_t> sequence{0};
        const auto temporary = parent / (L".forge-candidate-" + std::to_wstring(GetCurrentProcessId())
            + L"-" + std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(sequence.fetch_add(1)) + L".tmp");
        FileHandle output(CreateFileW(temporary.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE, 0,
            nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!output.Valid()) return Result<ArchiveSaveResult>::Failure(
            "MapArchiveSession", "TEMP_CREATE_FAILED", "无法独占创建临时候选（Windows 错误 " + std::to_string(GetLastError()) + "）。");
        output.RemoveOnClose();
        auto actualTemporary = FinalPath(output.Get());
        if (!actualTemporary.Succeeded() || !SamePath(actualTemporary.value->parent_path(), parent)) {
            return Result<ArchiveSaveResult>::Failure("MapArchiveSession", "TARGET_DIRECTORY_CHANGED", "候选目录在创建过程中改变。");
        }
        const auto written = opaqueSource
            ? opaqueSource->WriteOpaquePreservingPatchToHandle(output.Get(), files)
            : Mpq::WriteMpqArchiveToHandle(output.Get(), files, leadingHeader, sectorShift, control);
        if (!written.Succeeded()) return ForwardFailure<ArchiveSaveResult>(written.diagnostics);
        auto candidate = Mpq::MpqArchive::OpenReadOnlyHandle(output.Get(), temporary, OpenLimits());
        if (!candidate.Succeeded()) return ForwardFailure<ArchiveSaveResult>(std::move(candidate.diagnostics));
        if (opaqueSource) {
            const auto verifiedOpaque = opaqueSource->VerifyOpaquePreservingPatch(*candidate.value, files);
            if (!verifiedOpaque.Succeeded()) return ForwardFailure<ArchiveSaveResult>(verifiedOpaque.diagnostics);
        } else {
            auto inventory = candidate.value->ListFilesStrict();
            auto prefix = candidate.value->ReadArchivePrefix();
            if (!inventory.Succeeded()) return ForwardFailure<ArchiveSaveResult>(std::move(inventory.diagnostics));
            if (!prefix.Succeeded()) return ForwardFailure<ArchiveSaveResult>(std::move(prefix.diagnostics));
            if (!std::equal(prefix.value->begin(), prefix.value->end(), leadingHeader.begin(), leadingHeader.end()) || inventory.value->size() != files.size() + 1) {
                return Result<ArchiveSaveResult>::Failure("MapArchiveSession", "CANDIDATE_STRUCTURE_MISMATCH", "候选的前置数据或成员数量不一致。");
            }
            const auto members = VerifyCandidateMembers(*candidate.value, files, control);
            if (!members.Succeeded()) return ForwardFailure<ArchiveSaveResult>(members.diagnostics);
        }
        candidate.value.reset();
        auto candidateSha = Sha256Handle(output.Get(), control);
        if (!candidateSha.Succeeded()) return ForwardFailure<ArchiveSaveResult>(std::move(candidateSha.diagnostics));
        LARGE_INTEGER candidateSize{};
        if (!GetFileSizeEx(output.Get(), &candidateSize) || candidateSize.QuadPart < 0) return Result<ArchiveSaveResult>::Failure(
            "MapArchiveSession", "CANDIDATE_SIZE_UNAVAILABLE", "无法读取候选大小。");
        const auto verified = finalGuard();
        if (!verified.Succeeded()) return ForwardFailure<ArchiveSaveResult>(std::move(verified.diagnostics));
        // 结果分配在发布之前完成。重命名使用自有句柄，不删除或替换任何既有目标。
        ArchiveSaveResult result{target, std::move(*candidateSha.value),
            opaqueSource ? opaqueSource->FileCount() - 1 : files.size(),
            static_cast<std::uint64_t>(candidateSize.QuadPart)};
        const auto targetName = target.wstring();
        std::vector<std::byte> renameStorage(sizeof(FILE_RENAME_INFO) + targetName.size() * sizeof(wchar_t));
        auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(renameStorage.data());
        rename->ReplaceIfExists = FALSE;
        rename->RootDirectory = nullptr;
        rename->FileNameLength = static_cast<DWORD>(targetName.size() * sizeof(wchar_t));
        std::memcpy(rename->FileName, targetName.data(), rename->FileNameLength);
        if (control && finalPublication && !control->BeginPublication())
            return ForwardFailure<ArchiveSaveResult>(ProjectSaveCancelledStatus().diagnostics);
        if (control && !finalPublication && !control->Checkpoint(ProjectSaveStage::Verify))
            return ForwardFailure<ArchiveSaveResult>(ProjectSaveCancelledStatus().diagnostics);
        if (!SetFileInformationByHandle(output.Get(), FileRenameInfo, rename,
            static_cast<DWORD>(renameStorage.size()))) return Result<ArchiveSaveResult>::Failure(
                "MapArchiveSession", "CANDIDATE_PUBLISH_FAILED", "无法发布候选；目标可能已经存在，会话保持不变。");
        const auto publishedGuard = finalGuard();
        if (!publishedGuard.Succeeded()) return ForwardFailure<ArchiveSaveResult>(publishedGuard.diagnostics);
        const auto published = FinalPath(output.Get());
        if (!published.Succeeded() || !SamePath(*published.value, target)) return Result<ArchiveSaveResult>::Failure(
            "MapArchiveSession", "CANDIDATE_PATH_CHANGED", "候选发布路径发生改变，未提交保存。");
        output.Commit();
        return Result<ArchiveSaveResult>::Success(std::move(result));
    } catch (const std::exception&) {
        return Result<ArchiveSaveResult>::Failure("MapArchiveSession", "CANDIDATE_SAVE_FAILED", "候选保存失败，会话保持不变。");
    }
}

Result<std::vector<Mpq::MpqBuildFile>> BuildFiles(std::span<const MapArchiveMember> members,
    const ProjectSaveControl* control) {
    std::vector<Mpq::MpqBuildFile> files;
    files.reserve(members.size());
    for (const auto& member : members) {
        if (control && !control->Checkpoint(ProjectSaveStage::Build)) return {{}, ProjectSaveCancelledStatus().diagnostics};
        Mpq::MpqBuildFile file{member.name, {}, true, std::nullopt};
        file.data.resize(member.data.size());
        if (!CopyProjectSaveBytes(file.data, member.data, control)) return {{}, ProjectSaveCancelledStatus().diagnostics};
        files.push_back(std::move(file));
    }
    return Result<std::vector<Mpq::MpqBuildFile>>::Success(std::move(files));
}

} // namespace

Result<ArchiveSaveResult> WriteVerifiedArchive(const std::filesystem::path& target,
    std::span<const MapArchiveMember> members, std::span<const std::byte> leadingHeader,
    ProjectSaveControl* control, bool finalPublication) {
    const auto files = BuildFiles(members, control);
    if (!files.Succeeded()) return ForwardFailure<ArchiveSaveResult>(files.diagnostics);
    return WriteArchiveWithGuard(target, *files.value, leadingHeader, [] { return Status::Ok(); },
        nullptr, Mpq::kMpqWriterSectorShift, false, control, finalPublication);
}

std::vector<MapArchiveMember> MapArchiveSession::MembersAfter(
    std::span<const MemberReplacement> replacements, std::span<const std::string> removals) const {
    std::set<std::string> removed;
    for (const auto& name : removals) removed.insert(Canonical(name));
    std::vector<MapArchiveMember> members;
    std::map<std::string, std::size_t> byName;
    for (const auto& member : members_) {
        const auto key = Canonical(member.name);
        if (removed.contains(key)) continue;
        byName.emplace(key, members.size());
        members.push_back(member);
    }
    for (const auto& replacement : replacements) {
        const auto key = Canonical(replacement.name);
        if (const auto found = byName.find(key); found != byName.end()) {
            members[found->second].data = replacement.data;
        } else {
            byName.emplace(key, members.size());
            members.push_back(replacement);
        }
    }
    return members;
}

Result<MapArchiveSession> MapArchiveSession::AfterSave(const ArchiveSaveResult& written,
    std::span<const MemberReplacement> replacements, std::span<const std::string> removals) const {
    try {
        FileHandle file(CreateFileW(written.path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!file.Valid()) return Result<MapArchiveSession>::Failure(
            "MapArchiveSession", "SOURCE_LEASE_FAILED", "无法只读锁定保存结果。");
        BY_HANDLE_FILE_INFORMATION info{};
        LARGE_INTEGER size{};
        if (!GetFileInformationByHandle(file.Get(), &info) || (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
            !GetFileSizeEx(file.Get(), &size) || size.QuadPart < 0 ||
            static_cast<std::uint64_t>(size.QuadPart) != written.fileSize) {
            return Result<MapArchiveSession>::Failure("MapArchiveSession", "SAVED_FILE_CHANGED",
                "保存结果的大小与写出时不同，请重新打开地图。");
        }
        auto path = FinalPath(file.Get());
        if (!path.Succeeded()) return ForwardFailure<MapArchiveSession>(std::move(path.diagnostics));
        if (!SamePath(*path.value, written.path)) return Result<MapArchiveSession>::Failure(
            "MapArchiveSession", "SAVED_FILE_CHANGED", "保存结果的路径与写出时不同，请重新打开地图。");
        auto sha = Sha256Handle(file.Get());
        if (!sha.Succeeded()) return ForwardFailure<MapArchiveSession>(std::move(sha.diagnostics));
        if (*sha.value != written.sha256) return Result<MapArchiveSession>::Failure(
            "MapArchiveSession", "SAVED_FILE_CHANGED", "保存结果的 SHA-256 与写出时不同，请重新打开地图。");
        // 匿名成员保留路径的清单与表来自源归档，校验实际产物后按新文件重建会话。
        if (opaqueMembers_) return Open(written.path);
        auto opened = Mpq::MpqArchive::OpenReadOnlyHandle(file.Get(), *path.value, OpenLimits());
        if (!opened.Succeeded()) return ForwardFailure<MapArchiveSession>(std::move(opened.diagnostics));
        auto inventory = opened.value->ListFilesStrict();
        if (!inventory.Succeeded()) return ForwardFailure<MapArchiveSession>(std::move(inventory.diagnostics));
        auto listfile = opened.value->ReadFile("(listfile)");
        if (!listfile.Succeeded()) return ForwardFailure<MapArchiveSession>(std::move(listfile.diagnostics));

        MapArchiveSession session;
        session.members_ = MembersAfter(replacements, removals);
        // 新文件的成员清单必须恰好是快照成员加自动清单（不含元数据成员）。
        std::multiset<std::string> expected{"(LISTFILE)"};
        for (const auto& member : session.members_) expected.insert(Canonical(member.name));
        std::multiset<std::string> actual;
        for (const auto& member : *inventory.value) actual.insert(Canonical(member.name));
        if (actual != expected) return Result<MapArchiveSession>::Failure("MapArchiveSession", "SAVED_FILE_CHANGED",
            "保存结果的成员清单与写出时不同，请重新打开地图。");
        session.sourcePath_ = std::move(*path.value);
        session.sourceSha256_ = std::move(*sha.value);
        session.volumeSerial_ = info.dwVolumeSerialNumber;
        session.fileId_ = FileId(info);
        session.prefix_ = prefix_;
        session.listfile_ = std::move(*listfile.value);
        return Result<MapArchiveSession>::Success(std::move(session));
    } catch (const std::exception&) {
        return Result<MapArchiveSession>::Failure("MapArchiveSession", "SNAPSHOT_FAILED", "建立保存后的会话失败。");
    }
}

Status MapArchiveSession::VerifySourceIdentity() const {
    FileHandle source(CreateFileW(sourcePath_.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!source.Valid()) return Status::Failure("MapArchiveSession", "SOURCE_UNAVAILABLE", "源归档路径已不可访问。");
    return VerifyIdentity(source.Get(), volumeSerial_, fileId_);
}

Result<MapArchiveSession> MapArchiveSession::WithLeadingHeader(std::span<const std::byte> header) const {
    if (header.size() != prefix_.size() && !(prefix_.empty() && header.size() == 512))
        return Result<MapArchiveSession>::Failure("MapArchiveSession", "ARCHIVE_HEADER_SIZE", "外头大小不受支持，源归档保持不变。");
    const bool changed = header.size() != prefix_.size() || !std::equal(header.begin(), header.end(), prefix_.begin());
    if (changed && opaqueMembers_)
        return Result<MapArchiveSession>::Failure("MapArchiveSession", "OPAQUE_HEADER_EDIT_UNSUPPORTED", "归档含有未命名成员，当前无损追加保存不能更改外头；请先提供完整成员清单。");
    try {
        auto result = *this;
        result.prefix_.assign(header.begin(), header.end());
        return Result<MapArchiveSession>::Success(std::move(result));
    } catch (const std::bad_alloc&) {
        return Result<MapArchiveSession>::Failure("MapArchiveSession", "ARCHIVE_HEADER_MEMORY", "无法准备外头保存快照。");
    }
}

Result<ArchiveSaveResult> MapArchiveSession::SaveCandidate(
    const std::filesystem::path& targetPath, std::span<const MemberReplacement> replacements,
    std::span<const std::string> removals) const {
    return WriteCandidate(targetPath, replacements, removals, true);
}

Status MapArchiveSession::PreflightSave(std::span<const MemberReplacement> replacements,
    std::span<const std::string> removals, const std::filesystem::path& target) const {
    auto checked = WriteCandidate(target, replacements, removals, true, true);
    return Status{std::move(checked.diagnostics)};
}

Result<ArchiveSaveResult> MapArchiveSession::WriteCandidate(
    const std::filesystem::path& targetPath, std::span<const MemberReplacement> replacements,
    std::span<const std::string> removals, bool hashSource, bool preflight) const {
    try {
        // 租约句柄不共享写入与删除：持有期间源图内容不会变化，只需在开头散列一次，
        // 之后的检查只核对文件身份与路径指向。调用方已在外层租约下散列过时（原地保存）跳过散列。
        FileHandle source(CreateFileW(sourcePath_.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!source.Valid()) return Result<ArchiveSaveResult>::Failure(
            "MapArchiveSession", "SOURCE_LEASE_FAILED", "源归档不可读取或正在被外部写入。");
        auto verified = hashSource ? VerifyHandle(source.Get(), volumeSerial_, fileId_, sourceSha256_)
                                   : VerifyIdentity(source.Get(), volumeSerial_, fileId_);
        if (!verified.Succeeded()) return ForwardFailure<ArchiveSaveResult>(std::move(verified.diagnostics));
        const auto guard = [&] {
            const auto check = VerifyIdentity(source.Get(), volumeSerial_, fileId_);
            if (!check.Succeeded()) return check;
            return VerifySourceIdentity();
        };

        std::set<std::string> replacementNames;
        bool replaceWtg = false;
        bool replaceWct = false;
        for (const auto& replacement : replacements) {
            const auto key = Canonical(replacement.name);
            if (key == "(LISTFILE)" || Mpq::IsMpqMetadataMemberName(key) || !replacementNames.insert(key).second) return Result<ArchiveSaveResult>::Failure(
                "MapArchiveSession", "REPLACEMENT_NAME_CONFLICT", "不能替换自动清单或提供重复成员：" + replacement.name);
            replaceWtg = replaceWtg || key == "WAR3MAP.WTG";
            replaceWct = replaceWct || key == "WAR3MAP.WCT";
        }
        if (replaceWtg != replaceWct) return Result<ArchiveSaveResult>::Failure(
            "MapArchiveSession", "TRIGGER_PAIR_REQUIRED", "war3map.wtg 与 war3map.wct 必须成对替换。");
        std::set<std::string> removed;
        for (const auto& name : removals) {
            const auto key = Canonical(name);
            if (key == "(LISTFILE)" || Mpq::IsMpqMetadataMemberName(key) || replacementNames.contains(key)) return Result<ArchiveSaveResult>::Failure(
                "MapArchiveSession", "REMOVAL_NAME_CONFLICT", "不能删除自动清单，或同时替换并删除同一成员：" + name);
            if (key == "WAR3MAP.WTG" || key == "WAR3MAP.WCT") return Result<ArchiveSaveResult>::Failure(
                "MapArchiveSession", "REMOVAL_NAME_CONFLICT", "不能删除触发器数据：" + name);
            removed.insert(key);
        }
        if (opaqueMembers_ && !removed.empty()) return Result<ArchiveSaveResult>::Failure(
            "MapArchiveSession", "REMOVAL_WITH_OPAQUE_MEMBERS", "归档含未命名成员，只能原样追加保存，不能删除或改名成员。");
        auto opened = Mpq::MpqArchive::OpenReadOnlyHandle(source.Get(), sourcePath_, OpenLimits());
        if (!opened.Succeeded()) return ForwardFailure<ArchiveSaveResult>(std::move(opened.diagnostics));
        if (opaqueMembers_) {
            std::vector<Mpq::MpqBuildFile> edits;
            edits.reserve(replacements.size());
            for (const auto& replacement : replacements) edits.push_back({replacement.name, replacement.data, true, std::nullopt});
            return WriteArchiveWithGuard(targetPath, edits, {}, guard, &*opened.value,
                Mpq::kMpqWriterSectorShift, preflight);
        }
        // 未改动的成员直接复用源归档的原始块（源图在租约下与快照一致）；候选沿用源归档的扇区大小
        // （YDWE 保存的地图为 64 KB），分扇区块因此都能复用。块与位置相关的回退为按快照内容重新压缩。
        std::uint16_t sectorShift = Mpq::kMpqWriterSectorShift;
        while (sectorShift < Mpq::kMpqMaxSectorShift && (512u << sectorShift) < opened.value->SectorSize()) ++sectorShift;
        if ((512u << sectorShift) != opened.value->SectorSize()) sectorShift = Mpq::kMpqWriterSectorShift;
        std::vector<Mpq::MpqBuildFile> files;
        std::map<std::string, std::size_t> byName;
        for (const auto& member : members_) {
            const auto key = Canonical(member.name);
            if (removed.contains(key)) continue;
            byName.emplace(key, files.size());
            if (replacementNames.contains(key)) {
                files.push_back({member.name, {}, true, std::nullopt});
                continue;
            }
            auto raw = opened.value->ReadRawBlock(member.name);
            if (raw.Succeeded() && raw.value && raw.value->fileSize == member.data.size() &&
                ((raw.value->flags & Mpq::kFileSingleUnit) != 0 || raw.value->fileSize == 0 || raw.value->sectorSize == (512u << sectorShift))) {
                files.push_back({member.name, {}, true, std::move(*raw.value)});
            } else {
                files.push_back({member.name, member.data, true, std::nullopt});
            }
        }
        for (const auto& replacement : replacements) {
            const auto found = byName.find(Canonical(replacement.name));
            if (found == byName.end()) {
                byName.emplace(Canonical(replacement.name), files.size());
                files.push_back({replacement.name, replacement.data, true, std::nullopt});
            } else files[found->second].data = replacement.data;
        }
        return WriteArchiveWithGuard(targetPath, files, prefix_, guard, nullptr, sectorShift, preflight);
    } catch (const std::exception&) {
        return Result<ArchiveSaveResult>::Failure("MapArchiveSession", "CANDIDATE_SAVE_FAILED", "候选保存失败，会话保持不变。");
    }
}

} // namespace War3Map
