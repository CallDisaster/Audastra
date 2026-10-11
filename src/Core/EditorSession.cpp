#include "War3MapCore/IO/FileBytes.h"
#include "War3MapCore/EditorSession.h"

#include "War3MapCore/Hash.h"
#include "War3MapCore/Workspace.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cwctype>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <sstream>
#include <string_view>
#include <system_error>

namespace War3Map {
namespace {

struct OverlayEntry {
    std::vector<std::byte> bytes;
    std::string summary;
    std::string kind;
};

struct Transaction {
    TransactionId id = 0;
    ProjectRevision baseRevision;
    std::map<std::filesystem::path, OverlayEntry> overlay;
};

std::string LowerGenericPath(const std::filesystem::path& path) {
    auto value = path.generic_string();
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

bool IsTriggerMember(const std::filesystem::path& path) {
    const auto value = LowerGenericPath(path);
    return value == "war3map.wtg" ||
           value == "war3map.wct" ||
           value == "map/war3map.wtg" ||
           value == "map/war3map.wct";
}

struct ObjectMemberNames {
    std::string_view lni;
    std::string_view native;
};

constexpr std::array<ObjectMemberNames, 7> kObjectMemberNames{{
    {"unit.ini", "war3map.w3u"},
    {"ability.ini", "war3map.w3a"},
    {"item.ini", "war3map.w3t"},
    {"buff.ini", "war3map.w3h"},
    {"upgrade.ini", "war3map.w3q"},
    {"doodad.ini", "war3map.w3d"},
    {"destructable.ini", "war3map.w3b"},
}};

bool IsObjectMemberFor(const std::filesystem::path& path, std::size_t category, bool native) {
    if (category >= kObjectMemberNames.size()) return false;
    const auto value = LowerGenericPath(path);
    const auto member = native ? kObjectMemberNames[category].native : kObjectMemberNames[category].lni;
    return value == member || value == std::string(native ? "map/" : "table/") + std::string(member);
}

bool IsObjectMember(const std::filesystem::path& path) {
    for (std::size_t category = 0; category < kObjectMemberNames.size(); ++category)
        if (IsObjectMemberFor(path, category, false) || IsObjectMemberFor(path, category, true)) return true;
    return false;
}

bool IsMapDataFieldMember(const std::filesystem::path& path) {
    const auto value = LowerGenericPath(path);
    constexpr std::array<std::string_view, 7> names{
        "war3mapunits.doo", "war3map.doo", "war3map.w3r", "war3map.w3c", "war3map.w3i", "war3map.wts", "war3map.w3s"};
    return std::any_of(names.begin(), names.end(), [&value](std::string_view name) {
        return value == name || value == std::string("map/") + std::string(name);
    });
}

bool IsTerrainMember(const std::filesystem::path& path) {
    const auto value = LowerGenericPath(path);
    return value == "war3map.w3e" || value == "map/war3map.w3e"
        || value == "war3map.wpm" || value == "map/war3map.wpm";
}

bool IsSafeRelativePath(const std::filesystem::path& path) {
    if (path.empty() || path.is_absolute() || path.has_root_path()) {
        return false;
    }
    for (const auto& component : path) {
        if (component == L"..") {
            return false;
        }
    }
    return true;
}

class WinHandle {
public:
    explicit WinHandle(HANDLE handle = INVALID_HANDLE_VALUE) noexcept
        : handle_(handle) {
    }

    ~WinHandle() {
        if (handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
    }

    WinHandle(const WinHandle&) = delete;
    WinHandle& operator=(const WinHandle&) = delete;

    [[nodiscard]] HANDLE Get() const noexcept {
        return handle_;
    }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

Result<std::filesystem::path> FinalWindowsPath(
    const std::filesystem::path& existingPath) {
    WinHandle handle(CreateFileW(
        existingPath.c_str(),
        FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS,
        nullptr));
    if (handle.Get() == INVALID_HANDLE_VALUE) {
        return Result<std::filesystem::path>::Failure(
            "Transaction",
            "PATH_HANDLE_OPEN_FAILED",
            "Unable to open a final-path handle (" +
                std::to_string(GetLastError()) + "): " + existingPath.string());
    }

    const DWORD required = GetFinalPathNameByHandleW(
        handle.Get(), nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (required == 0) {
        return Result<std::filesystem::path>::Failure(
            "Transaction",
            "PATH_FINAL_RESOLUTION_FAILED",
            "Unable to resolve final Windows path: " + existingPath.string());
    }
    std::wstring buffer(static_cast<std::size_t>(required), L'\0');
    const DWORD written = GetFinalPathNameByHandleW(
        handle.Get(), buffer.data(), required, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (written == 0 || written >= required) {
        return Result<std::filesystem::path>::Failure(
            "Transaction",
            "PATH_FINAL_RESOLUTION_FAILED",
            "Unable to read final Windows path: " + existingPath.string());
    }
    buffer.resize(written);
    constexpr std::wstring_view uncPrefix = L"\\\\?\\UNC\\";
    constexpr std::wstring_view devicePrefix = L"\\\\?\\";
    if (buffer.starts_with(uncPrefix)) {
        buffer = L"\\\\" + buffer.substr(uncPrefix.size());
    } else if (buffer.starts_with(devicePrefix)) {
        buffer.erase(0, devicePrefix.size());
    }
    return Result<std::filesystem::path>::Success(
        std::filesystem::path(std::move(buffer)).lexically_normal());
}

std::wstring NormalizedWindowsIdentity(const std::filesystem::path& path) {
    auto value = path.lexically_normal().native();
    std::replace(value.begin(), value.end(), L'/', L'\\');
    while (value.size() > 3 && value.back() == L'\\') {
        value.pop_back();
    }
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t character) {
        return static_cast<wchar_t>(std::towlower(character));
    });
    return value;
}

bool IsInsideWindows(
    const std::filesystem::path& child,
    const std::filesystem::path& parent) {
    const auto childValue = NormalizedWindowsIdentity(child);
    const auto parentValue = NormalizedWindowsIdentity(parent);
    if (childValue == parentValue) {
        return true;
    }
    return childValue.size() > parentValue.size() &&
           childValue.starts_with(parentValue) &&
           childValue[parentValue.size()] == L'\\';
}

std::string EscapeJson(std::string_view value) {
    std::ostringstream output;
    for (const unsigned char character : value) {
        switch (character) {
        case '\\': output << "\\\\"; break;
        case '"': output << "\\\""; break;
        case '\b': output << "\\b"; break;
        case '\f': output << "\\f"; break;
        case '\n': output << "\\n"; break;
        case '\r': output << "\\r"; break;
        case '\t': output << "\\t"; break;
        default:
            if (character < 0x20) {
                constexpr char digits[] = "0123456789abcdef";
                output << "\\u00"
                       << digits[(character >> 4) & 0x0f]
                       << digits[character & 0x0f];
            } else {
                output << static_cast<char>(character);
            }
            break;
        }
    }
    return output.str();
}



const WorkspaceFile* FindFile(
    const WorkspaceSnapshot& snapshot,
    const std::filesystem::path& relativePath) {
    const auto iterator = std::find_if(
        snapshot.files.begin(),
        snapshot.files.end(),
        [&relativePath](const WorkspaceFile& file) {
            return file.relativePath == relativePath;
        });
    return iterator == snapshot.files.end() ? nullptr : &*iterator;
}

Status WriteBytes(
    const std::filesystem::path& path,
    const std::vector<std::byte>& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        return Status::Failure(
            "Transaction", "CANDIDATE_WRITE_FAILED", "Unable to create candidate file: " + path.string());
    }
    output.write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    if (!output) {
        return Status::Failure(
            "Transaction", "CANDIDATE_WRITE_FAILED", "Unable to write candidate file: " + path.string());
    }
    output.close();
    if (!output) {
        return Status::Failure(
            "Transaction", "CANDIDATE_WRITE_FAILED", "Unable to close candidate file: " + path.string());
    }
    return Status::Ok();
}

} // namespace

class EditorSession::Impl {
public:
    Impl(
        DirectoryWorkspace workspaceValue,
        CompatibilityProfile profileValue,
        std::string catalogSha)
        : workspace(std::move(workspaceValue)),
          profile(std::move(profileValue)) {
        revision.generation = 1;
        revision.sourceSha256 = workspace.Snapshot().aggregateSha256;
        revision.uiCatalogSha256 = std::move(catalogSha);

        auto finalRoot = FinalWindowsPath(workspace.Snapshot().root);
        if (finalRoot.Succeeded()) {
            const auto identity = NormalizedWindowsIdentity(*finalRoot.value);
            const auto identityHash = Sha256(std::as_bytes(std::span(
                identity.data(), identity.size())));
            if (identityHash.Succeeded()) {
                const std::wstring hash(identityHash.value->begin(), identityHash.value->end());
                const std::wstring name = L"Local\\War3MapEditor64-" + hash;
                writerSemaphore = CreateSemaphoreW(nullptr, 1, 1, name.c_str());
            }
        }
    }

    ~Impl() {
        if (writerLeaseHeld && writerSemaphore != nullptr) {
            ReleaseSemaphore(writerSemaphore, 1, nullptr);
        }
        if (writerSemaphore != nullptr) {
            CloseHandle(writerSemaphore);
        }
    }

    Status ApplyFiles(
        TransactionId transactionId,
        std::vector<BinaryFilePatch> patches,
        const std::string& kind,
        bool (*isAllowedMember)(const std::filesystem::path&)) {
        std::scoped_lock lock(mutex);
        if (!transaction || transaction->id != transactionId) {
            return Status::Failure(
                "Transaction", "TRANSACTION_NOT_ACTIVE", "The requested edit transaction is not active.");
        }
        if (patches.empty()) {
            return Status::Failure(
                "Transaction", "PATCH_EMPTY", "An edit patch must contain at least one file.");
        }
        for (auto& patch : patches) {
            patch.relativePath = patch.relativePath.lexically_normal();
            if (!IsSafeRelativePath(patch.relativePath)) {
                return Status::Failure(
                    "Transaction",
                    "PATCH_PATH_UNSAFE",
                    "Patch path must be a safe path relative to the workspace.");
            }
            if (!isAllowedMember(patch.relativePath)) {
                return Status::Failure(
                    "Transaction",
                    "PATCH_MEMBER_DOMAIN_MISMATCH",
                    "Validated patch attempted to modify a member outside its format domain: " +
                        patch.relativePath.string());
            }
            const auto* source = FindFile(workspace.Snapshot(), patch.relativePath);
            if (source == nullptr) {
                return Status::Failure(
                    "Transaction",
                    "PATCH_SOURCE_MISSING",
                    "Foundation patches may only replace an existing workspace file: " +
                        patch.relativePath.string());
            }
            if (patch.expectedSourceSha256.size() != 64 ||
                patch.expectedSourceSha256 != source->sha256) {
                return Status::Failure(
                    "Transaction",
                    "PATCH_SOURCE_IDENTITY_MISMATCH",
                    "Validated patch was not produced from the current source member: " +
                        patch.relativePath.string());
            }
        }
        // 所有节点分配与覆盖构造完成后一次交换；后项失败不能留下前项已发布的覆盖。
        auto nextOverlay = transaction->overlay;
        for (auto& patch : patches) {
            nextOverlay.insert_or_assign(
                patch.relativePath,
                OverlayEntry{
                    std::move(patch.replacement),
                    std::move(patch.summary),
                    kind,
                });
        }
        transaction->overlay.swap(nextOverlay);
        return Status::Ok();
    }

    DirectoryWorkspace workspace;
    CompatibilityProfile profile;
    ProjectRevision revision;
    mutable std::mutex mutex;
    std::optional<Transaction> transaction;
    TransactionId nextTransactionId = 1;
    HANDLE writerSemaphore = nullptr;
    bool writerLeaseHeld = false;
};

TriggerPatch::TriggerPatch(
    std::vector<BinaryFilePatch> files,
    std::string uiCatalogSha256)
    : files_(std::move(files)),
      uiCatalogSha256_(std::move(uiCatalogSha256)) {
}

ObjectPatch::ObjectPatch(std::vector<BinaryFilePatch> files, ObjectData::ObjectType type, bool native)
    : files_(std::move(files)), type_(type), native_(native) {
}

MapDataPatch::MapDataPatch(std::vector<BinaryFilePatch> files) : files_(std::move(files)) {}

EditorSession::EditorSession(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {
}

EditorSession::EditorSession(EditorSession&&) noexcept = default;
EditorSession& EditorSession::operator=(EditorSession&&) noexcept = default;
EditorSession::~EditorSession() = default;

Result<EditorSession> EditorSession::Open(OpenRequest request) {
    const auto profileStatus = request.profile.Validate();
    if (!profileStatus.Succeeded()) {
        Result<EditorSession> result;
        result.diagnostics = profileStatus.diagnostics;
        return result;
    }

    auto workspace = DirectoryWorkspace::Open(request.workspacePath);
    if (!workspace.Succeeded()) {
        Result<EditorSession> result;
        result.diagnostics = std::move(workspace.diagnostics);
        return result;
    }

    auto implementation = std::make_unique<Impl>(
        std::move(*workspace.value),
        std::move(request.profile),
        std::move(request.uiCatalogSha256));
    if (implementation->writerSemaphore == nullptr) {
        return Result<EditorSession>::Failure(
            "Transaction",
            "WRITER_LOCK_INITIALIZATION_FAILED",
            "Unable to establish the workspace-wide single-writer lock.");
    }
    return Result<EditorSession>::Success(EditorSession(std::move(implementation)));
}

ProjectRevision EditorSession::Revision() const {
    std::scoped_lock lock(impl_->mutex);
    return impl_->revision;
}

const CompatibilityProfile& EditorSession::Profile() const noexcept {
    return impl_->profile;
}

Status EditorSession::VerifySourceUnchanged() const {
    std::scoped_lock lock(impl_->mutex);
    return impl_->workspace.VerifyUnchanged();
}

Result<TransactionId> EditorSession::BeginEdit(
    const ProjectRevision& expectedRevision) {
    std::scoped_lock lock(impl_->mutex);
    if (expectedRevision != impl_->revision) {
        return Result<TransactionId>::Failure(
            "Transaction",
            "REVISION_CONFLICT",
            "The requested base revision is stale.");
    }
    if (impl_->transaction.has_value()) {
        return Result<TransactionId>::Failure(
            "Transaction",
            "WRITER_ALREADY_ACTIVE",
            "Only one write transaction may be active in a project session.");
    }
    const Status sourceStatus = impl_->workspace.VerifyUnchanged();
    if (!sourceStatus.Succeeded()) {
        Result<TransactionId> result;
        result.diagnostics = sourceStatus.diagnostics;
        return result;
    }
    if (WaitForSingleObject(impl_->writerSemaphore, 0) != WAIT_OBJECT_0) {
        return Result<TransactionId>::Failure(
            "Transaction",
            "WRITER_ALREADY_ACTIVE",
            "Another session or process already holds the write lease for this workspace.");
    }
    impl_->writerLeaseHeld = true;
    const Status leasedSourceStatus = impl_->workspace.VerifyUnchanged();
    if (!leasedSourceStatus.Succeeded()) {
        ReleaseSemaphore(impl_->writerSemaphore, 1, nullptr);
        impl_->writerLeaseHeld = false;
        Result<TransactionId> result;
        result.diagnostics = leasedSourceStatus.diagnostics;
        return result;
    }
    Transaction transaction;
    transaction.id = impl_->nextTransactionId++;
    transaction.baseRevision = impl_->revision;
    const auto id = transaction.id;
    impl_->transaction.emplace(std::move(transaction));
    return Result<TransactionId>::Success(id);
}

Status EditorSession::ApplyTriggerPatch(
    TransactionId transactionId,
    TriggerPatch patch) {
    {
        std::scoped_lock lock(impl_->mutex);
        if (impl_->revision.uiCatalogSha256.size() != 64 ||
            patch.uiCatalogSha256_.size() != 64 ||
            patch.uiCatalogSha256_ != impl_->revision.uiCatalogSha256) {
            return Status::Failure(
                "Trigger",
                "UI_CATALOG_IDENTITY_MISMATCH",
                "Trigger patch catalog SHA-256 does not match the project revision.");
        }
    }
    return impl_->ApplyFiles(
        transactionId,
        std::move(patch.files_),
        "trigger",
        &IsTriggerMember);
}

Status EditorSession::ApplyObjectPatch(
    TransactionId transactionId,
    ObjectPatch patch) {
    return impl_->ApplyFiles(
        transactionId,
        std::move(patch.files_),
        "object",
        &IsObjectMember);
}

Status EditorSession::ApplyMapDataPatch(TransactionId transactionId, MapDataPatch patch) {
    // 六类字段可在原事务中累计 reseed；地物仍仅由 placement codec 密封。
    // 白名单与原成员 SHA 校验保留，不接受协议提供的任意 bytes。
    if (patch.files_.empty() || patch.files_.size() > 6)
        return Status::Failure("MapData", "MAP_FIELD_PATCH_SCOPE_INVALID", "密封地图数据事务只能替换一到六个已有地图设置、字符串、单位、地物、区域、镜头或声音成员。");
    return impl_->ApplyFiles(transactionId, std::move(patch.files_), "map-data", &IsMapDataFieldMember);
}

Status EditorSession::ApplyTerrainPatch(TransactionId transactionId, TerrainPatch patch) {
    if (patch.files_.size() != 2)
        return Status::Failure("Terrain.Patch", "TERRAIN_PATCH_SCOPE_INVALID", "密封地形事务必须同时绑定已有 W3E 和 WPM。");
    return impl_->ApplyFiles(transactionId, std::move(patch.files_), "terrain", &IsTerrainMember);
}

Status EditorSession::CreateObjectMember(TransactionId transactionId, ObjectPatch patch) {
    std::scoped_lock lock(impl_->mutex);
    if (!impl_->transaction || impl_->transaction->id != transactionId)
        return Status::Failure("Transaction", "TRANSACTION_NOT_ACTIVE", "请求的物编事务未处于活动状态。");
    if (patch.files_.size() != 1)
        return Status::Failure("Transaction", "OBJECT_CREATION_PATCH_INVALID", "新增物编只能提交单个 codec 密封的类别成员。");
    auto& file = patch.files_.front();
    file.relativePath = file.relativePath.lexically_normal();
    const auto category = static_cast<std::size_t>(patch.type_);
    if (!IsSafeRelativePath(file.relativePath))
        return Status::Failure("Transaction", "PATCH_PATH_UNSAFE", "新增物编必须使用安全的工作区相对路径。");
    if (!IsObjectMemberFor(file.relativePath, category, patch.native_))
        return Status::Failure("Transaction", "OBJECT_CREATION_DOMAIN_MISMATCH", "新增路径与 codec 密封的物编类别或 LNI/native 格式不符。");
    if (file.replacement.size() > 64U * 1024U * 1024U)
        return Status::Failure("Transaction", "OBJECT_CREATION_BYTE_BUDGET", "新增物编成员超过 64 MiB 预算。");
    const auto unchanged = impl_->workspace.VerifyUnchanged();
    if (!unchanged.Succeeded()) return unchanged;
    // 类别所有受支持的来源位置都须缺失；不能绕过存在的 root/map/table 别名。
    for (const auto& source : impl_->workspace.Snapshot().files)
        if (IsObjectMemberFor(source.relativePath, category, false) || IsObjectMemberFor(source.relativePath, category, true))
            return Status::Failure("Transaction", "OBJECT_CREATION_SOURCE_PRESENT", "本类别已有物编来源，应使用原有替换链，不能当作缺失成员新建。");
    std::array<std::byte, 12> nativeEmpty{};
    nativeEmpty.front() = std::byte{2};
    const auto emptyHash = patch.native_ ? Sha256(nativeEmpty) : Sha256(std::span<const std::byte>{});
    if (!emptyHash.Succeeded()) return Status{emptyHash.diagnostics};
    if (file.expectedSourceSha256 != *emptyHash.value)
        return Status::Failure("Transaction", "OBJECT_CREATION_BASELINE_MISMATCH", "新增物编必须来自真正空 LNI 或经典 v2 空 native 基线，不能复制任意旧来源字节。");
    for (const auto& [path, entry] : impl_->transaction->overlay) {
        if (!IsObjectMemberFor(path, category, false) && !IsObjectMemberFor(path, category, true)) continue;
        if (path != file.relativePath || entry.kind != "object")
            return Status::Failure("Transaction", "OBJECT_CREATION_OVERLAY_CONFLICT", "本类别已有另一来源位置的事务覆盖，未写入新增成员。");
    }
    impl_->transaction->overlay.insert_or_assign(file.relativePath,
        OverlayEntry{std::move(file.replacement), std::move(file.summary), "object"});
    return Status::Ok();
}

Result<SemanticDiff> EditorSession::PreviewEdit(
    TransactionId transactionId) const {
    std::scoped_lock lock(impl_->mutex);
    if (!impl_->transaction || impl_->transaction->id != transactionId) {
        return Result<SemanticDiff>::Failure(
            "Transaction", "TRANSACTION_NOT_ACTIVE", "The requested edit transaction is not active.");
    }

    SemanticDiff diff;
    diff.transactionId = transactionId;
    diff.baseRevision = impl_->transaction->baseRevision;
    for (const auto& [path, entry] : impl_->transaction->overlay) {
        const auto hash = Sha256(entry.bytes);
        if (!hash.Succeeded()) {
            Result<SemanticDiff> result;
            result.diagnostics = hash.diagnostics;
            return result;
        }
        const auto* source = FindFile(impl_->workspace.Snapshot(), path);
        diff.changes.push_back(SemanticChange{
            entry.kind,
            path,
            entry.summary,
            source == nullptr ? std::string{} : source->sha256,
            *hash.value,
        });
    }
    return Result<SemanticDiff>::Success(std::move(diff));
}

ValidationReport EditorSession::Validate(
    TransactionId transactionId,
    const CompatibilityProfile& profile,
    CompatibilityLevel requestedLevel) const {
    ValidationReport report;
    report.requestedLevel = requestedLevel;
    report.highestProvenLevel = CompatibilityLevel::Parsed;

    std::scoped_lock lock(impl_->mutex);
    if (!impl_->transaction || impl_->transaction->id != transactionId) {
        report.diagnostics.push_back(Diagnostic{
            DiagnosticSeverity::Error,
            "Transaction",
            "TRANSACTION_NOT_ACTIVE",
            "The requested edit transaction is not active.",
            std::nullopt,
        });
        return report;
    }
    if (profile.id != impl_->profile.id ||
        profile.definitionSha256 != impl_->profile.definitionSha256) {
        report.diagnostics.push_back(Diagnostic{
            DiagnosticSeverity::Error,
            "Profile",
            "PROFILE_MISMATCH",
            "Validation profile differs from the profile used to open the project.",
            std::nullopt,
        });
        return report;
    }

    const auto sourceStatus = impl_->workspace.VerifyUnchanged();
    report.diagnostics.insert(
        report.diagnostics.end(),
        sourceStatus.diagnostics.begin(),
        sourceStatus.diagnostics.end());
    if (!sourceStatus.Succeeded()) {
        return report;
    }

    report.satisfied =
        requestedLevel == CompatibilityLevel::Parsed &&
        !impl_->transaction->overlay.empty();
    if (impl_->transaction->overlay.empty()) {
        report.diagnostics.push_back(Diagnostic{
            DiagnosticSeverity::Error,
            "Validation",
            "NO_VALIDATED_PATCH",
            "L0 requires at least one patch produced by a format codec.",
            std::nullopt,
        });
        return report;
    }
    if (!report.satisfied) {
        report.diagnostics.push_back(Diagnostic{
            DiagnosticSeverity::Warning,
            "Validation",
            "EXTERNAL_GATE_REQUIRED",
            "The requested compatibility level requires codec or external editor evidence not yet attached to this transaction.",
            std::nullopt,
        });
    }
    return report;
}

Status EditorSession::CommitCandidate(
    TransactionId transactionId,
    const ProjectRevision& expectedRevision,
    const std::filesystem::path& outputPath,
    std::function<Status()> finalGuard) {
    std::scoped_lock lock(impl_->mutex);
    if (!impl_->transaction || impl_->transaction->id != transactionId) {
        return Status::Failure(
            "Transaction", "TRANSACTION_NOT_ACTIVE", "The requested edit transaction is not active.");
    }
    if (expectedRevision != impl_->revision ||
        impl_->transaction->baseRevision != expectedRevision) {
        return Status::Failure(
            "Transaction", "REVISION_CONFLICT", "The candidate commit revision is stale.");
    }
    if (impl_->transaction->overlay.empty()) {
        return Status::Failure(
            "Transaction", "EMPTY_COMMIT_REJECTED", "A candidate must contain at least one edit.");
    }

    const auto sourceStatus = impl_->workspace.VerifyUnchanged();
    if (!sourceStatus.Succeeded()) {
        return sourceStatus;
    }

    std::error_code error;
    const auto candidate = std::filesystem::absolute(outputPath, error).lexically_normal();
    if (error || candidate.empty() || candidate.filename().empty()) {
        return Status::Failure(
            "Transaction", "CANDIDATE_PATH_INVALID", "Unable to resolve candidate output path.");
    }
    const auto sourceRoot = impl_->workspace.Snapshot().root.lexically_normal();
    const auto parent = candidate.parent_path();
    const bool candidateExists = std::filesystem::exists(candidate, error);
    if (error) {
        return Status::Failure(
            "Transaction", "CANDIDATE_STATUS_FAILED", "Unable to inspect candidate output path.");
    }
    if (candidateExists) {
        return Status::Failure(
            "Transaction",
            "CANDIDATE_ALREADY_EXISTS",
            "Candidate output already exists; existing outputs are never overwritten.");
    }
    const bool parentIsDirectory = std::filesystem::is_directory(parent, error);
    if (error || !parentIsDirectory) {
        return Status::Failure(
            "Transaction",
            "CANDIDATE_PARENT_MISSING",
            "Candidate parent directory must already exist and be accessible.");
    }

    auto finalSourceRoot = FinalWindowsPath(sourceRoot);
    auto finalParent = FinalWindowsPath(parent);
    if (!finalSourceRoot.Succeeded() || !finalParent.Succeeded()) {
        Status status;
        status.diagnostics = !finalSourceRoot.Succeeded()
            ? std::move(finalSourceRoot.diagnostics)
            : std::move(finalParent.diagnostics);
        return status;
    }
    const auto finalCandidate = (*finalParent.value / candidate.filename()).lexically_normal();
    if (IsInsideWindows(finalCandidate, *finalSourceRoot.value)) {
        return Status::Failure(
            "Transaction",
            "CANDIDATE_INSIDE_SOURCE",
            "Candidate output must not resolve to the source workspace or a child of it.");
    }

    const auto temporary = parent / (
        L"." + candidate.filename().wstring() +
        L".war3me-tmp-" + std::to_wstring(transactionId));
    const auto manifestPath = parent /
        (candidate.filename().wstring() + L".war3me-manifest.json");
    const auto temporaryManifest = parent / (
        L"." + candidate.filename().wstring() +
        L".war3me-manifest-tmp-" + std::to_wstring(transactionId) + L".json");
    const bool temporaryExists = std::filesystem::exists(temporary, error);
    if (error || temporaryExists) {
        return Status::Failure(
            "Transaction",
            "CANDIDATE_TEMP_EXISTS",
            "A temporary candidate directory already exists or cannot be inspected.");
    }
    const bool manifestExists = std::filesystem::exists(manifestPath, error);
    if (error || manifestExists || std::filesystem::exists(temporaryManifest, error) || error) {
        return Status::Failure(
            "Transaction",
            "CANDIDATE_MANIFEST_EXISTS",
            "Candidate manifest path already exists or cannot be inspected; existing files are never overwritten.");
    }

    bool candidateRenamed = false;
    try {
        const auto cleanupTemporary = [&temporary, &temporaryManifest]() {
            std::error_code cleanupError;
            std::filesystem::remove_all(temporary, cleanupError);
            std::filesystem::remove(temporaryManifest, cleanupError);
        };

        std::filesystem::create_directory(temporary);
        auto finalTemporary = FinalWindowsPath(temporary);
        if (!finalTemporary.Succeeded() ||
            IsInsideWindows(*finalTemporary.value, *finalSourceRoot.value)) {
            cleanupTemporary();
            return Status::Failure(
                "Transaction",
                "CANDIDATE_TEMP_PATH_UNSAFE",
                "Temporary candidate resolved inside the source workspace.");
        }

        for (const auto& file : impl_->workspace.Snapshot().files) {
            const auto destination = temporary / file.relativePath;
            std::filesystem::create_directories(destination.parent_path());
            std::filesystem::copy_file(
                sourceRoot / file.relativePath,
                destination,
                std::filesystem::copy_options::none);
            const auto copiedHash = Sha256File(destination);
            if (!copiedHash.Succeeded() || *copiedHash.value != file.sha256) {
                cleanupTemporary();
                return Status::Failure(
                    "Transaction",
                    "CANDIDATE_COPY_IDENTITY_MISMATCH",
                    "A copied source member did not match the opened revision: " +
                        file.relativePath.string());
            }
        }

        for (const auto& [relativePath, entry] : impl_->transaction->overlay) {
            const auto destination = temporary / relativePath;
            std::filesystem::create_directories(destination.parent_path());
            const auto writeStatus = WriteBytes(destination, entry.bytes);
            if (!writeStatus.Succeeded()) {
                cleanupTemporary();
                return writeStatus;
            }
            const auto expectedHash = Sha256(entry.bytes);
            const auto actualHash = Sha256File(destination);
            if (!expectedHash.Succeeded() || !actualHash.Succeeded() ||
                *expectedHash.value != *actualHash.value) {
                cleanupTemporary();
                return Status::Failure(
                    "Transaction",
                    "CANDIDATE_OVERLAY_IDENTITY_MISMATCH",
                    "A serialized overlay failed its post-write SHA-256 check: " +
                        relativePath.string());
            }
        }

        const auto secondSourceStatus = impl_->workspace.VerifyUnchanged();
        if (!secondSourceStatus.Succeeded()) {
            cleanupTemporary();
            return secondSourceStatus;
        }
        auto candidateWorkspace = DirectoryWorkspace::Open(temporary);
        if (!candidateWorkspace.Succeeded()) {
            cleanupTemporary();
            Status status;
            status.diagnostics = std::move(candidateWorkspace.diagnostics);
            return status;
        }

        std::ofstream manifest(temporaryManifest, std::ios::binary | std::ios::trunc);
        if (!manifest) {
            cleanupTemporary();
            return Status::Failure(
                "Transaction", "MANIFEST_WRITE_FAILED", "Unable to create candidate manifest sidecar.");
        }
        manifest << "{\n"
                 << "  \"schemaVersion\": 1,\n"
                 << "  \"profile\": \"" << EscapeJson(impl_->profile.id) << "\",\n"
                 << "  \"profileSha256\": \"" << impl_->profile.definitionSha256 << "\",\n"
                 << "  \"sourceSha256\": \"" << impl_->revision.sourceSha256 << "\",\n"
                 << "  \"candidateSha256\": \""
                 << candidateWorkspace.value->Snapshot().aggregateSha256 << "\",\n"
                 << "  \"uiCatalogSha256\": \"" << EscapeJson(impl_->revision.uiCatalogSha256) << "\",\n"
                 << "  \"transactionId\": " << transactionId << ",\n"
                 << "  \"changes\": [\n";
        std::size_t changeIndex = 0;
        for (const auto& [relativePath, entry] : impl_->transaction->overlay) {
            const auto hash = Sha256(entry.bytes);
            if (!hash.Succeeded()) {
                manifest.close();
                cleanupTemporary();
                Status status;
                status.diagnostics = hash.diagnostics;
                return status;
            }
            manifest << "    {\"path\": \"" << EscapeJson(IO::GenericPathUtf8(relativePath))
                     << "\", \"kind\": \"" << EscapeJson(entry.kind)
                     << "\", \"sha256\": \"" << *hash.value
                     << "\", \"summary\": \"" << EscapeJson(entry.summary) << "\"}";
            if (++changeIndex != impl_->transaction->overlay.size()) {
                manifest << ',';
            }
            manifest << '\n';
        }
        manifest << "  ]\n}\n";
        manifest.close();
        if (!manifest) {
            cleanupTemporary();
            return Status::Failure(
                "Transaction", "MANIFEST_WRITE_FAILED", "Unable to finalize candidate manifest sidecar.");
        }
        const auto finalSourceStatus = impl_->workspace.VerifyUnchanged();
        if (!finalSourceStatus.Succeeded()) {
            cleanupTemporary();
            return finalSourceStatus;
        }
        if (finalGuard) {
            const Status guardStatus = finalGuard();
            if (!guardStatus.Succeeded()) {
                cleanupTemporary();
                return guardStatus;
            }
        }

        std::filesystem::rename(temporary, candidate);
        candidateRenamed = true;
        std::filesystem::rename(temporaryManifest, manifestPath);
    } catch (const std::exception& exception) {
        std::error_code cleanupError;
        std::filesystem::remove_all(temporary, cleanupError);
        std::filesystem::remove(temporaryManifest, cleanupError);
        if (candidateRenamed) {
            std::filesystem::remove_all(candidate, cleanupError);
        }
        return Status::Failure(
            "Transaction",
            "CANDIDATE_COMMIT_FAILED",
            std::string("Candidate commit failed: ") + exception.what());
    }

    impl_->transaction.reset();
    ReleaseSemaphore(impl_->writerSemaphore, 1, nullptr);
    impl_->writerLeaseHeld = false;
    ++impl_->revision.generation;
    return Status::Ok();
}

Status EditorSession::Rollback(TransactionId transactionId) {
    std::scoped_lock lock(impl_->mutex);
    if (!impl_->transaction || impl_->transaction->id != transactionId) {
        return Status::Failure(
            "Transaction", "TRANSACTION_NOT_ACTIVE", "The requested edit transaction is not active.");
    }
    impl_->transaction.reset();
    ReleaseSemaphore(impl_->writerSemaphore, 1, nullptr);
    impl_->writerLeaseHeld = false;
    return Status::Ok();
}

} // namespace War3Map
