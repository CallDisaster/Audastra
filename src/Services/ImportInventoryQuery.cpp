#include "War3MapCore/Services/ImportInventoryQuery.h"

#include "War3MapCore/Hash.h"
#include "War3MapCore/IO/FileBytes.h"
#include "War3MapCore/Mpq/MpqArchive.h"
#include "War3MapCore/Resources/ImportTable.h"
#include "War3MapCore/VerifiedFile.h"

#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <utility>

namespace War3Map::Services {
namespace {
constexpr std::size_t kMaxRows = 262144, kMetadataBytes = 32U * 1024U * 1024U;
constexpr std::size_t kImpBytes = 8U * 1024U * 1024U;
using SnapshotResult = Result<std::shared_ptr<const ImportInventorySnapshot>>;
using ExactQuery = std::function<Result<ArchiveExactIdentityResult>(const std::vector<std::string>&)>;
struct Seed {
    std::string id, kind, path;
    std::optional<std::uint8_t> flags;
    std::optional<std::uint64_t> reportedBytes;
};
template<class T> Result<T> Fail(const char* code, std::string message) {
    return Result<T>::Failure("Imports.Query", code, std::move(message));
}
bool Sha(std::string_view value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
bool TextKey(std::string_view value) { return !value.empty() && value.size() <= 4096 && value.find('\0') == value.npos; }
void Append(std::string& out, std::string_view value) {
    out += std::to_string(value.size()); out += ':'; out += value;
}
bool Reserved(std::string_view path) {
    const auto canonical = Resources::CanonicalImportPath(path);
    return canonical.Succeeded() && (*canonical.value == "(LISTFILE)" || Mpq::IsMpqMetadataMemberName(*canonical.value));
}
Status CheckBinding(const ImportInventoryBinding& binding) {
    if (!TextKey(binding.documentId) || !TextKey(binding.revision) || !TextKey(binding.assetRevision)
        || !TextKey(binding.contentScope) || !Sha(binding.sourceSha256))
        return Status::Failure("Imports.Query", "IMPORT_BINDING_INVALID", "导入查询必须绑定真实文档、修订、资产修订及完整来源SHA。");
    return Status::Ok();
}
} // namespace

struct ImportInventorySnapshot::Impl {
    ImportInventoryDescription description;
    std::vector<Seed> rows;
    std::map<std::string, std::size_t> ids;
    std::map<std::string, std::string> pathEdits;
    std::function<Status()> verify;
    ExactQuery exact;
};

namespace {
SnapshotResult FinishCapture(std::shared_ptr<ImportInventorySnapshot::Impl> impl,
    const std::optional<std::vector<std::byte>>& imp,
    const std::vector<std::pair<std::string, std::uint64_t>>& physical,
    ImportInventoryOverlay overlay) {
    auto& description = impl->description;
    const auto checked = CheckBinding(description.binding);
    if (!checked.Succeeded()) return {{}, checked.diagnostics};
    if ((!overlay.pathEdits.empty() || !overlay.pending.empty()) && !overlay.includesUnsavedChanges)
        return Fail<std::shared_ptr<const ImportInventorySnapshot>>("IMPORT_OVERLAY_SCOPE", "资产覆盖存在时必须明确标记未保存内容。");
    description.includesUnsavedChanges = overlay.includesUnsavedChanges;
    std::string metadata;
    for (const auto& value : {description.binding.documentId, std::to_string(description.binding.generation),
        description.binding.revision, description.binding.sourceSha256, description.binding.assetRevision,
        description.binding.contentScope, description.sourceKind}) Append(metadata, value);
    Append(metadata, description.hasOpaqueArchiveMembers ? "opaque" : "named-complete");
    Append(metadata, overlay.includesUnsavedChanges ? "overlay" : "source");
    std::map<std::string, std::size_t> occurrences;
    auto add = [&](std::string kind, std::string path, std::optional<std::uint8_t> flags,
        std::optional<std::uint64_t> bytes) -> Status {
        if (impl->rows.size() >= kMaxRows || path.size() > kMetadataBytes - std::min(metadata.size(), kMetadataBytes))
            return Status::Failure("Imports.Query", "IMPORT_CAPTURE_BUDGET", "导入元数据快照超过262144行或32MiB预算。");
        std::string identity; Append(identity, kind); Append(identity, path);
        Append(identity, flags ? std::to_string(*flags) : "no-flags");
        const auto digest = Sha256Text(identity);
        if (!digest.Succeeded()) return {digest.diagnostics};
        const auto occurrence = occurrences[*digest.value]++;
        const auto id = kind + ":" + *digest.value + ":" + std::to_string(occurrence);
        impl->ids.emplace(id, impl->rows.size());
        impl->rows.push_back({id, kind, path, flags, bytes});
        Append(metadata, identity); Append(metadata, std::to_string(occurrence));
        Append(metadata, bytes ? std::to_string(*bytes) : "unknown");
        if (metadata.size() > kMetadataBytes)
            return Status::Failure("Imports.Query", "IMPORT_CAPTURE_BUDGET", "导入元数据快照超过32MiB预算。");
        return Status::Ok();
    };
    if (imp) {
        if (imp->size() > kImpBytes) return Fail<std::shared_ptr<const ImportInventorySnapshot>>("IMPORT_IMP_BUDGET", "IMP超过8MiB解析预算。");
        // 只做分配前数量预算；记录、版本、路径与尾部仍全部交原ImportTable解析。
        if (imp->size() >= 8) {
            std::uint32_t declared = 0;
            for (unsigned index = 0; index < 4; ++index)
                declared |= static_cast<std::uint32_t>(std::to_integer<unsigned char>((*imp)[4 + index])) << (index * 8);
            if (declared > kMaxRows)
                return Fail<std::shared_ptr<const ImportInventorySnapshot>>("IMPORT_CAPTURE_BUDGET", "IMP声明记录数超过262144行快照预算。");
        }
        const auto table = Resources::ImportTable::Parse(*imp);
        if (!table.Succeeded()) return {{}, table.diagnostics};
        const auto digest = Sha256(*imp);
        if (!digest.Succeeded()) return {{}, digest.diagnostics};
        description.impState = "present"; description.impSha256 = *digest.value;
        description.impRecordCount = table.value->Entries().size();
        for (const auto& entry : table.value->Entries()) {
            const auto added = add("imp-record", entry.path, entry.flags, {});
            if (!added.Succeeded()) return {{}, added.diagnostics};
        }
    } else description.impState = "missing";
    Append(metadata, description.impSha256.value_or("missing"));
    Append(metadata, description.impSourcePath.value_or("missing"));
    for (const auto& [path, bytes] : physical) {
        if (Reserved(path)) { ++description.excludedArchiveMetadataCount; continue; }
        const auto added = add("physical-member", path, {}, bytes);
        if (!added.Succeeded()) return {{}, added.diagnostics};
        ++description.physicalMemberCount;
    }
    for (const auto& edit : overlay.pathEdits) {
        const auto from = Resources::CanonicalImportPath(edit.from);
        if (!from.Succeeded()) return {{}, from.diagnostics};
        if (!edit.to.empty()) {
            const auto to = Resources::CanonicalImportPath(edit.to);
            if (!to.Succeeded()) return {{}, to.diagnostics};
        }
        if (!impl->pathEdits.emplace(*from.value, edit.to).second)
            return Fail<std::shared_ptr<const ImportInventorySnapshot>>("IMPORT_OVERLAY_AMBIGUOUS", "同一原成员有多个资产路径修改，不能选择覆盖。");
        Append(metadata, edit.from); Append(metadata, edit.to);
        if (metadata.size() > kMetadataBytes) return Fail<std::shared_ptr<const ImportInventorySnapshot>>("IMPORT_CAPTURE_BUDGET", "资产路径覆盖超过元数据预算。");
    }
    for (const auto& pending : overlay.pending) {
        const auto added = add("queued-unfrozen", pending.archivePath, {}, pending.reportedBytes);
        if (!added.Succeeded()) return {{}, added.diagnostics};
        ++description.queuedCount;
    }
    const auto digest = Sha256Text(metadata);
    if (!digest.Succeeded()) return {{}, digest.diagnostics};
    description.inventorySha256 = *digest.value;
    const auto current = impl->verify();
    if (!current.Succeeded()) return {{}, current.diagnostics};
    return SnapshotResult::Success(std::make_shared<const ImportInventorySnapshot>(std::move(impl)));
}

// 复用ResolveArchivePath自身要求的候选，不复制flags/标准前缀的判定。
std::vector<std::string> CandidatePaths(const Seed& seed) {
    std::vector<std::string> names;
    if (seed.kind == "queued-unfrozen") return names;
    if (!seed.flags) {
        if (Resources::CanonicalImportPath(seed.path).Succeeded() && !Reserved(seed.path)) names.push_back(seed.path);
        return names;
    }
    const auto resolved = Resources::ResolveArchivePath({*seed.flags, seed.path}, [&](std::string_view name) {
        if (Resources::CanonicalImportPath(name).Succeeded() && !Reserved(name)) names.emplace_back(name);
        return false;
    });
    if (resolved.Succeeded() && !Reserved(*resolved.value)) names.push_back(*resolved.value);
    return names;
}

Result<std::vector<ImportInventoryRow>> Materialize(const ImportInventorySnapshot::Impl& impl,
    const std::vector<std::size_t>& indices) {
    std::map<std::string, std::string> requested;
    for (const auto index : indices) for (const auto& path : CandidatePaths(impl.rows[index])) {
        const auto key = Resources::CanonicalImportPath(path);
        if (key.Succeeded()) requested.emplace(*key.value, path);
    }
    std::vector<std::string> names;
    for (const auto& [key, path] : requested) { (void)key; names.push_back(path); }
    std::map<std::string, ArchiveExactMemberIdentity> identities;
    if (!names.empty()) {
        const auto result = impl.exact(names);
        if (!result.Succeeded()) return {{}, result.diagnostics};
        if (result.value->sourceSha256 != impl.description.binding.sourceSha256 || result.value->members.size() != names.size())
            return Fail<std::vector<ImportInventoryRow>>("IMPORT_SOURCE_IDENTITY", "精确成员身份与原来源或请求集合不符。");
        for (const auto& member : result.value->members) {
            const auto key = Resources::CanonicalImportPath(member.name);
            if (!key.Succeeded() || !requested.contains(*key.value) || !identities.emplace(*key.value, member).second)
                return Fail<std::vector<ImportInventoryRow>>("IMPORT_SOURCE_IDENTITY", "精确成员身份集合有未请求、重复或无效路径。");
        }
    }
    std::vector<ImportInventoryRow> rows;
    for (const auto index : indices) {
        const auto& seed = impl.rows[index];
        ImportInventoryRow row;
        row.importId = seed.id; row.kind = seed.kind; row.rawPath = seed.path; row.flags = seed.flags;
        row.sourceLayer = impl.description.sourceKind == "archive" ? "map-archive" : "map-directory";
        row.state = "unresolved";
        if (seed.kind == "queued-unfrozen") {
            row.sourceLayer = "session-queue"; row.state = "queued-unfrozen"; row.overlayState = "queued";
            row.reportedBytes = seed.reportedBytes;
            const auto path = Resources::CanonicalImportPath(seed.path);
            if (path.Succeeded()) row.archivePath = seed.path; else row.diagnostics = path.diagnostics;
            rows.push_back(std::move(row)); continue;
        }
        std::set<std::string> seen;
        for (const auto& path : CandidatePaths(seed)) {
            const auto key = Resources::CanonicalImportPath(path);
            if (key.Succeeded() && seen.insert(*key.value).second && identities.contains(*key.value))
                row.candidates.push_back(identities.at(*key.value));
        }
        auto resolved = seed.flags ? Resources::ResolveArchivePath({*seed.flags, seed.path}, [&](std::string_view path) {
            const auto key = Resources::CanonicalImportPath(path);
            return key.Succeeded() && identities.contains(*key.value) && identities.at(*key.value).present;
        }) : Resources::CanonicalImportPath(seed.path);
        if (resolved.Succeeded() && !Reserved(*resolved.value)) {
            const auto key = Resources::CanonicalImportPath(*resolved.value);
            const auto found = identities.find(*key.value);
            if (found == identities.end())
                return Fail<std::vector<ImportInventoryRow>>("IMPORT_SOURCE_IDENTITY", "解析路径没有对应的精确成员身份。");
            row.sourcePath = seed.flags ? *resolved.value : seed.path;
            row.archivePath = row.sourcePath;
            row.state = found->second.present ? "present" : "missing";
            row.bytes = found->second.byteSize; row.sha256 = found->second.sha256;
            if (const auto edited = impl.pathEdits.find(*key.value); edited != impl.pathEdits.end()) {
                row.overlayState = edited->second.empty() ? "removed" : "moved";
                row.archivePath = edited->second.empty() ? std::nullopt : std::optional<std::string>(edited->second);
            }
        } else {
            row.diagnostics = resolved.diagnostics;
            if (row.diagnostics.empty()) row.diagnostics.push_back({DiagnosticSeverity::Warning, "Imports.Query", "IMPORT_RESERVED_PATH", "归档保留成员不作为导入载荷读取。"});
        }
        rows.push_back(std::move(row));
    }
    return Result<std::vector<ImportInventoryRow>>::Success(std::move(rows));
}
} // namespace

ImportInventorySnapshot::ImportInventorySnapshot(std::shared_ptr<const Impl> impl) : impl_(std::move(impl)) {}
const ImportInventoryDescription& ImportInventorySnapshot::Description() const noexcept { return impl_->description; }
Status ImportInventorySnapshot::VerifyCurrent() const { return impl_->verify(); }

Result<ImportInventoryPage> ImportInventorySnapshot::Query(const ImportInventoryBinding& expected,
    std::string query, const std::optional<ImportInventoryCursor>& cursor, std::size_t limit, std::size_t maxExamined) const {
    if (expected != Description().binding) return Fail<ImportInventoryPage>("IMPORT_REVISION_CONFLICT", "导入快照身份已变化。");
    if (!limit || limit > 128 || !maxExamined || maxExamined > 4096 || query.size() > 4096 || query.find('\0') != query.npos)
        return Fail<ImportInventoryPage>("IMPORT_QUERY_BUDGET", "查询最多128行、4096次谓词检查，query最多4096字节且不得含NUL。");
    std::size_t start = 0;
    if (cursor) {
        if (cursor->binding != expected || cursor->inventorySha256 != Description().inventorySha256 || cursor->query != query
            || cursor->nextIndex > impl_->rows.size())
            return Fail<ImportInventoryPage>("IMPORT_CURSOR_STALE", "导入游标与来源、IMP、覆盖、query或行范围不符。");
        start = static_cast<std::size_t>(cursor->nextIndex);
    }
    const auto current = VerifyCurrent(); if (!current.Succeeded()) return {{}, current.diagnostics};
    ImportInventoryPage page; page.description = Description(); page.knownRowCount = impl_->rows.size();
    std::vector<std::size_t> selected;
    const bool completeScan = impl_->rows.size() <= maxExamined;
    std::uint64_t matched = 0;
    std::size_t next = impl_->rows.size();
    for (std::size_t index = completeScan ? 0 : start; index < impl_->rows.size(); ++index) {
        if (page.examined == maxExamined) { next = index; page.scanBudgetExhausted = true; break; }
        ++page.examined;
        if (impl_->rows[index].path.find(query) == std::string::npos) continue;
        ++matched;
        if (index < start) continue;
        if (selected.size() < limit) selected.push_back(index);
        else if (next == impl_->rows.size()) next = index;
        if (!completeScan && selected.size() == limit) { next = index + 1; break; }
    }
    if (completeScan) page.matchedTotal = matched;
    if (next < impl_->rows.size()) page.next = ImportInventoryCursor{expected, Description().inventorySha256, query, next};
    const auto rows = Materialize(*impl_, selected); if (!rows.Succeeded()) return {{}, rows.diagnostics};
    page.rows = *rows.value;
    const auto after = VerifyCurrent(); if (!after.Succeeded()) return {{}, after.diagnostics};
    return Result<ImportInventoryPage>::Success(std::move(page));
}

Result<ImportInventoryRow> ImportInventorySnapshot::Get(const ImportInventoryBinding& expected,
    std::string_view expectedInventorySha256, std::string_view importId) const {
    if (expected != Description().binding || expectedInventorySha256 != Description().inventorySha256)
        return Fail<ImportInventoryRow>("IMPORT_REVISION_CONFLICT", "导入详情必须绑定当前来源、IMP与资产覆盖快照。");
    if (importId.empty() || importId.size() > 256)
        return Fail<ImportInventoryRow>("IMPORT_ID_MISSING", "import_id必须来自当前清单且不得超过256字节。");
    const auto found = impl_->ids.find(std::string(importId));
    if (found == impl_->ids.end()) return Fail<ImportInventoryRow>("IMPORT_ID_MISSING", "当前快照没有该import_id，请先查询清单。");
    const auto before = VerifyCurrent(); if (!before.Succeeded()) return {{}, before.diagnostics};
    const auto rows = Materialize(*impl_, {found->second}); if (!rows.Succeeded()) return {{}, rows.diagnostics};
    const auto after = VerifyCurrent(); if (!after.Succeeded()) return {{}, after.diagnostics};
    return Result<ImportInventoryRow>::Success(rows.value->front());
}

SnapshotResult CaptureImportInventory(const MapArchiveSession& source,
    ImportInventoryBinding binding, ImportInventoryOverlay overlay) {
    const auto checked = CheckBinding(binding); if (!checked.Succeeded()) return {{}, checked.diagnostics};
    if (binding.sourceSha256 != source.SourceSha256())
        return Fail<std::shared_ptr<const ImportInventorySnapshot>>("IMPORT_SOURCE_STALE", "地图SHA与原归档会话不符。");
    if (source.Members().size() > kMaxRows || overlay.pending.size() > kMaxRows || overlay.pathEdits.size() > kMaxRows)
        return Fail<std::shared_ptr<const ImportInventorySnapshot>>("IMPORT_CAPTURE_BUDGET", "来源或资产覆盖行数超过262144行预算。");
    auto impl = std::make_shared<ImportInventorySnapshot::Impl>();
    impl->description.binding = std::move(binding); impl->description.sourceKind = "archive";
    impl->description.hasOpaqueArchiveMembers = source.HasOpaqueMembers();
    impl->verify = [&source] { return source.VerifySourceUnchanged(); };
    impl->exact = [&source, sha = impl->description.binding.sourceSha256](const std::vector<std::string>& names) {
        return source.DescribeExactMemberIdentities(sha, names);
    };
    ArchiveExactMemberLimits limits; limits.memberBytes = kImpBytes; limits.totalBytes = kImpBytes;
    const std::vector<std::string> names{"war3map.imp"};
    const auto captured = source.CaptureExactMembers(names, limits);
    if (!captured.Succeeded()) return {{}, captured.diagnostics};
    std::optional<std::vector<std::byte>> imp;
    if (captured.value->front().present) {
        imp = captured.value->front().bytes; impl->description.impSourcePath = "war3map.imp";
    }
    std::vector<std::pair<std::string, std::uint64_t>> physical;
    for (const auto& member : source.Members()) physical.emplace_back(member.name, member.data.size());
    return FinishCapture(std::move(impl), imp, physical, std::move(overlay));
}

SnapshotResult CaptureImportInventory(const DirectoryWorkspace& source,
    ImportInventoryBinding binding, ImportInventoryOverlay overlay) {
    const auto checked = CheckBinding(binding); if (!checked.Succeeded()) return {{}, checked.diagnostics};
    if (binding.sourceSha256 != source.Snapshot().aggregateSha256)
        return Fail<std::shared_ptr<const ImportInventorySnapshot>>("IMPORT_SOURCE_STALE", "目录完整SHA与原快照不符。");
    if (source.Snapshot().files.size() > kMaxRows || overlay.pending.size() > kMaxRows || overlay.pathEdits.size() > kMaxRows)
        return Fail<std::shared_ptr<const ImportInventorySnapshot>>("IMPORT_CAPTURE_BUDGET", "来源或资产覆盖行数超过262144行预算。");
    auto owned = std::make_shared<DirectoryWorkspace>(source);
    auto impl = std::make_shared<ImportInventorySnapshot::Impl>();
    impl->description.binding = std::move(binding); impl->description.sourceKind = "directory-literal-paths";
    impl->verify = [owned] { return owned->VerifyUnchanged(); };
    auto paths = std::make_shared<std::map<std::string, std::vector<WorkspaceFile>>>();
    std::vector<std::pair<std::string, std::uint64_t>> physical;
    const WorkspaceFile* impFile = nullptr;
    for (const auto& file : owned->Snapshot().files) {
        const auto path = IO::GenericPathUtf8(file.relativePath);
        const auto canonical = Resources::CanonicalImportPath(path);
        physical.emplace_back(path, file.size);
        if (!canonical.Succeeded()) continue;
        (*paths)[*canonical.value].push_back(file);
        if (*canonical.value == "WAR3MAP.IMP" || *canonical.value == "MAP\\WAR3MAP.IMP") {
            if (impFile) return Fail<std::shared_ptr<const ImportInventorySnapshot>>("IMPORT_IMP_AMBIGUOUS", "目录存在多个root/map IMP来源，不能猜测有效清单。");
            impFile = &file;
        }
    }
    std::optional<std::vector<std::byte>> imp;
    if (impFile) {
        const auto bytes = ReadVerifiedFile(owned->Snapshot().root / impFile->relativePath, impFile->sha256, kImpBytes);
        if (!bytes.Succeeded()) return {{}, bytes.diagnostics};
        imp = *bytes.value; impl->description.impSourcePath = IO::GenericPathUtf8(impFile->relativePath);
    }
    impl->exact = [owned, paths](const std::vector<std::string>& names) -> Result<ArchiveExactIdentityResult> {
        const ArchiveExactMemberLimits limits;
        if (names.size() > limits.memberCount) return Fail<ArchiveExactIdentityResult>("EXACT_MEMBER_LIMIT", "目录身份查询超过256路径。");
        ArchiveExactIdentityResult result; result.sourceSha256 = owned->Snapshot().aggregateSha256;
        std::uint64_t total = 0;
        for (const auto& name : names) {
            if (name.size() > limits.pathBytes) return Fail<ArchiveExactIdentityResult>("EXACT_MEMBER_LIMIT", "目录身份路径超过1024字节。");
            const auto key = Resources::CanonicalImportPath(name); if (!key.Succeeded()) return {{}, key.diagnostics};
            const auto found = paths->find(*key.value);
            if (found == paths->end()) { result.members.push_back({name, false, {}, {}}); continue; }
            if (found->second.size() != 1) return Fail<ArchiveExactIdentityResult>("IMPORT_DIRECTORY_PATH_AMBIGUOUS", "目录成员大小写/斜杠归一后有歧义。");
            const auto& file = found->second.front();
            if (file.size > limits.totalBytes - total) return Fail<ArchiveExactIdentityResult>("EXACT_MEMBER_LIMIT", "目录身份成员累计字节超过64MiB预算。");
            total += file.size;
            result.members.push_back({name, true, static_cast<std::uint64_t>(file.size), file.sha256});
        }
        return Result<ArchiveExactIdentityResult>::Success(std::move(result));
    };
    return FinishCapture(std::move(impl), imp, physical, std::move(overlay));
}

} // namespace War3Map::Services
