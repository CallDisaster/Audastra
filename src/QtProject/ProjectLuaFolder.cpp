#include "ProjectLuaFolder.h"
#include "EditorProject.h"
#include "AssetOwnedPayload.h"
#include "PendingImportPayload.h"
#include "PreparedAssetState.h"
#include "War3MapCore/Services/LuaFolderSave.h"
#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/Hash.h"

#include <QScopedValueRollback>
#include <algorithm>

namespace War3Map::QtProject {
namespace {
Result<std::optional<std::vector<std::byte>>> Metadata(const EditorProject& project) {
    const auto name = QString::fromUtf8(Services::kLuaFolderMember.data(), Services::kLuaFolderMember.size());
    const auto entries = project.AssetEntries();
    if (std::none_of(entries.begin(), entries.end(), [&](const auto& entry) { return EditorProject::AssetKey(entry.path) == EditorProject::AssetKey(name); }))
        return Result<std::optional<std::vector<std::byte>>>::Success(std::nullopt);
    const auto source = ReadOwnedAsset(project, name, 8192);
    if (!source.Succeeded()) return {std::nullopt, source.diagnostics};
    return Result<std::optional<std::vector<std::byte>>>::Success(*source.value->bytes);
}
Result<std::vector<Services::LuaFolderFile>> MapFiles(const EditorProject& project) {
    std::vector<Services::LuaFolderFile> result;
    std::size_t total = 0;
    for (const auto& entry : project.AssetEntries()) {
        if (!Services::IsAuthorLuaMember(entry.path.toStdString())) continue;
        // 此处必须读地图源，不能绕回已经关联的外部来源。
        const auto read = ReadOwnedMapAsset(project, entry.path);
        if (!read.Succeeded()) return {std::nullopt, read.diagnostics};
        const auto valid = Services::ValidateLuaFolderFile(entry.path.toStdString(), *read.value->bytes, false);
        if (!valid.Succeeded()) return {std::nullopt, valid.diagnostics};
        total += read.value->bytes->size();
        if (result.size() == Services::kLuaFolderFiles || total > Services::kLuaFolderTotalBytes)
            return Result<std::vector<Services::LuaFolderFile>>::Failure("LuaFolder", "LUA_FOLDER_TOTAL_BUDGET", "地图Lua文件超过4096项或32 MiB。");
        result.push_back({entry.path.toStdString(), read.value->sha256, read.value->bytes});
    }
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) { return EditorProject::AssetKey(QString::fromStdString(a.path)) < EditorProject::AssetKey(QString::fromStdString(b.path)); });
    return Result<std::vector<Services::LuaFolderFile>>::Success(std::move(result));
}
}

ProjectLuaFolder::ProjectLuaFolder(EditorProject& project) : QObject(&project), project_(project) {
    debounce_.setSingleShot(true);
    debounce_.setInterval(150);
    connect(&watcher_, &QFileSystemWatcher::fileChanged, &debounce_, qOverload<>(&QTimer::start));
    connect(&watcher_, &QFileSystemWatcher::directoryChanged, &debounce_, qOverload<>(&QTimer::start));
    connect(&debounce_, &QTimer::timeout, this, [this] { Refresh(); });
    connect(&project_, &EditorProject::ImportsChanged, this, [this] { Refresh(false); });
    connect(&project_, &EditorProject::SourceStateChanged, this, [this] { Refresh(false); });
}

void ProjectLuaFolder::Refresh(bool force) {
    if (refreshing_) return;
    QScopedValueRollback guard(refreshing_, true);
    const auto oldIdentity = snapshot_ ? snapshot_->sha256 : std::string{};
    const auto oldRoot = root_;
    const bool wasMissing = Missing();
    const auto metadata = Metadata(project_);
    std::string sha;
    std::filesystem::path root;
    std::vector<Diagnostic> diagnostics;
    if (!metadata.Succeeded()) diagnostics = metadata.diagnostics;
    else if (*metadata.value) {
        const auto hash = Sha256(**metadata.value);
        if (hash.Succeeded()) sha = *hash.value;
        const auto link = Services::DecodeLuaFolderLink(**metadata.value, project_.sourceArchivePath_);
        if (!link.Succeeded()) diagnostics = link.diagnostics;
        else if (*link.value) root = **link.value;
    }
    if (!force && sha == metadataSha_ && mapPath_ == project_.sourceArchivePath_ && diagnostics.empty()) return;
    if (!watcher_.files().isEmpty()) watcher_.removePaths(watcher_.files());
    if (!watcher_.directories().isEmpty()) watcher_.removePaths(watcher_.directories());
    metadataSha_ = sha;
    root_ = root;
    mapPath_ = project_.sourceArchivePath_;
    snapshot_.reset();
    if (!root.empty()) {
        auto snapshot = Services::CaptureLuaFolder(root);
        if (!snapshot.Succeeded()) {
            diagnostics = snapshot.diagnostics;
            // 非法编辑中的文件仍须在外部修好后恢复；失败不能丢失后续保存通知。
            debounce_.start(1000);
        }
        else {
            snapshot_ = std::move(*snapshot.value);
            if (snapshot_->missing) diagnostics.push_back({DiagnosticSeverity::Warning, "LuaFolder", "LUA_FOLDER_MISSING", "外部文件夹不存在，使用地图内Lua。"});
            else {
                QStringList paths;
                for (const auto& dir : snapshot_->directories) paths.push_back(QString::fromStdWString(dir.wstring()));
                for (const auto& file : snapshot_->files) paths.push_back(QString::fromStdWString((root / std::filesystem::u8path(file.path)).wstring()));
                const auto failed = watcher_.addPaths(paths);
                if (!failed.isEmpty()) debounce_.start(1000);
            }
        }
    }
    const bool changed = !std::ranges::equal(diagnostics_, diagnostics, [](const auto& a, const auto& b) {
        return a.code == b.code && a.message == b.message && a.severity == b.severity;
    });
    diagnostics_ = std::move(diagnostics);
    if (changed && !diagnostics_.empty()) project_.AddDiagnostics(diagnostics_);
    if (oldIdentity != (snapshot_ ? snapshot_->sha256 : std::string{}) || oldRoot != root_
        || wasMissing != Missing() || changed) emit project_.LuaSourceChanged();
}

Result<LuaFolderReview> ProjectLuaFolder::Review(const std::filesystem::path& root) const {
    if (!project_.archiveSession_ || project_.sourceArchivePath_.empty())
        return Result<LuaFolderReview>::Failure("LuaFolder", "LUA_FOLDER_MAP_REQUIRED", "请先打开或保存为地图归档，再关联文件夹。");
    const auto map = MapFiles(project_);
    if (!map.Succeeded()) return {std::nullopt, map.diagnostics};
    auto folder = Services::CaptureLuaFolder(root);
    if (!folder.Succeeded()) return {std::nullopt, folder.diagnostics};
    if (folder.value->missing) return Result<LuaFolderReview>::Failure("LuaFolder", "LUA_FOLDER_DIRECTORY", "请选择已经存在的文件夹。");
    LuaFolderReview result;
    result.generation = project_.editRevision_.Generation();
    result.revision = project_.editRevision_.Revision();
    result.map = *map.value;
    result.folder = std::move(*folder.value);
    result.difference = Services::CompareLuaFolder(result.map, result.folder);
    std::error_code ec;
    result.emptyDirectory = std::filesystem::is_empty(result.folder.root, ec);
    if (ec) return Result<LuaFolderReview>::Failure("LuaFolder", "LUA_FOLDER_IO", "无法核对文件夹是否为空。");
    return Result<LuaFolderReview>::Success(std::move(result));
}

Status ProjectLuaFolder::Associate(const LuaFolderReview& review, Services::LuaFolderChoice choice) {
    if (choice == Services::LuaFolderChoice::Cancel) return Status::Failure("LuaFolder", "LUA_FOLDER_CANCELLED", "已取消关联。");
    const auto metadata = Services::EncodeLuaFolderLink(review.folder.root, project_.sourceArchivePath_);
    if (!metadata.Succeeded()) return {metadata.diagnostics};
    const auto before = project_.CaptureAssets();
    auto candidate = before;
    const auto path = QString::fromUtf8(Services::kLuaFolderMember.data(), Services::kLuaFolderMember.size());
    std::erase_if(candidate.imports, [&](const auto& item) { return EditorProject::AssetKey(item.archivePath) == EditorProject::AssetKey(path); });
    std::erase_if(candidate.edits, [&](const auto& item) { return EditorProject::AssetKey(item.from) == EditorProject::AssetKey(path); });
    candidate.imports.push_back({path, {}, static_cast<qint64>(metadata.value->size()),
        std::make_shared<const std::vector<std::byte>>(*metadata.value), false});
    const auto prepared = project_.PrepareAssetStateEdit(candidate, review.generation, review.revision, "关联外部Lua文件夹");
    if (!prepared.Succeeded()) return {prepared.diagnostics};
    const auto reconciled = Services::ReconcileLuaFolder(review.folder, review.map, review.emptyDirectory ? Services::LuaFolderChoice::Map : choice);
    if (!reconciled.Succeeded()) return reconciled;
    if (project_.editRevision_.Generation() != review.generation || project_.editRevision_.Revision() != review.revision
        || project_.CaptureAssets() != before)
        return Status::Failure("LuaFolder", "LUA_FOLDER_LINK_STALE", "地图已改变，关联未发布；已确认的文件夹写盘不属于地图撤销，请重新审阅。");
    const auto published = project_.RecordAssetEdit(tr("关联外部Lua文件夹"), [&](AssetState& state) {
        state = candidate;
        return QString{};
    });
    if (!published.isEmpty()) return Status::Failure("LuaFolder", "LUA_FOLDER_LINK_STALE", published.toStdString()
        + "；关联未发布，已确认的文件夹写盘可能已完成，不属于地图撤销。");
    return Status::Ok();
}

Status ProjectLuaFolder::Unlink() {
    const auto message = project_.RecordAssetEdit(tr("取消Lua文件夹关联"), [&](AssetState& candidate) {
        return project_.RemoveAsset(candidate, QString::fromUtf8(Services::kLuaFolderMember.data(), Services::kLuaFolderMember.size()));
    });
    return message.isEmpty() ? Status::Ok() : Status::Failure("LuaFolder", "LUA_FOLDER_UNLINK", message.toStdString());
}

Result<std::pair<std::vector<std::string>, std::string>> ProjectLuaFolder::Deletions() const {
    using Output = Result<std::pair<std::vector<std::string>, std::string>>;
    if (!Linked()) return Output::Success({{}, {}});
    if (root_.empty()) return Output::Failure("LuaFolder", "LUA_FOLDER_METADATA_INVALID", "Lua关联信息无效，请取消关联或重新选择文件夹。");
    const auto folder = Services::CaptureLuaFolder(root_);
    if (!folder.Succeeded()) return {std::nullopt, folder.diagnostics};
    if (folder.value->missing) return Output::Success({{}, {}});
    const auto map = MapFiles(project_);
    if (!map.Succeeded()) return {std::nullopt, map.diagnostics};
    const auto diff = Services::CompareLuaFolder(*map.value, *folder.value);
    const auto identity = Services::LuaFolderDeletionIdentity(*map.value, *folder.value);
    if (!identity.Succeeded()) return {std::nullopt, identity.diagnostics};
    return Output::Success({diff.mapOnly, *identity.value});
}

QJsonObject LuaFolderSourceInfo(const EditorProject& project) {
    const auto& folder = project.LuaFolderSource();
    const auto source = folder.Active() ? "folder" : folder.Linked() && !folder.Missing() ? "unavailable" : "map";
    return {{"source", source}, {"linked", folder.Linked()}, {"connected", folder.Active()}, {"missing", folder.Missing()},
        {"path", QString::fromStdWString((folder.Active() ? folder.Root() : project.sourceArchivePath_).wstring())},
        {"folder_path", QString::fromStdWString(folder.Root().wstring())},
        {"snapshot_sha256", folder.Snapshot() ? QString::fromStdString(folder.Snapshot()->sha256) : QString{}}};
}
} // namespace War3Map::QtProject
