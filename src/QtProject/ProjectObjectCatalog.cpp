#include "ProjectObjectCatalog.h"
#include "EditorProject.h"

#include "War3MapCore/Game/WorldEditStrings.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/IO/FileBytes.h"
#include "War3MapCore/Render/AnimationSourceGuard.h"
#include "War3MapCore/Resources/ImportTable.h"
#include "War3MapCore/Script/ReadOnlyScriptSource.h"
#include "War3MapCore/Services/CampaignService.h"
#include "War3MapCore/Workspace.h"

#include <QPointer>
#include <algorithm>
#include <map>
#include <set>

namespace War3Map::QtProject {
namespace {
constexpr std::size_t kMaximum = 32U * 1024U * 1024U;
Status Fail(std::string code, std::string message) { return Status::Failure("Object.Catalog", std::move(code), std::move(message)); }

std::string Logical(const EditorProject& project, const QString& value, const std::set<std::string>& domain) {
    const auto key = Resources::CanonicalImportPath(value.toStdString());
    if (!key.Succeeded()) return {};
    if (domain.contains(*key.value)) return *key.value;
    if (!project.archiveSession_ && !project.readOnlyArchive_ && project.directorySnapshot_
        && key.value->starts_with("MAP\\") && domain.contains(key.value->substr(4))) return key.value->substr(4);
    return {};
}
} // namespace

Services::ObjectCatalogSources ProjectObjectCatalogSources::Sources() const {
    auto output = base; output.replacements = replacements; output.removals = removals;
    return output;
}

Result<ProjectObjectCatalogSources> CaptureProjectObjectCatalogSources(const EditorProject& project,
    std::span<const std::string> requested) {
    using Output = Result<ProjectObjectCatalogSources>;
    if (project.sourceChangeSuspected_ || project.sourceChangeConfirmed_)
        return Output::Failure("Object.Catalog", "OBJECT_CATALOG_SOURCE_CHANGED", "地图来源尚待核验，不能使用旧物编目录。");
    auto names = requested.empty() ? ObjectData::EffectiveObjectCatalog::InputNames()
        : Result<std::vector<std::string>>::Success({requested.begin(), requested.end()});
    if (!names.Succeeded()) return {{}, names.diagnostics};
    std::set<std::string> domain;
    for (const auto& name : *names.value) {
        const auto key = Resources::CanonicalImportPath(name);
        if (!key.Succeeded()) return {{}, key.diagnostics};
        domain.insert(*key.value);
    }
    // WESTRING 来源由游戏/工具链管理；不读取地图会话中同名的未受信任导入。
    for (const auto& name : ObjectData::CatalogLoadOptions{}.stringFiles) {
        const auto key = Resources::CanonicalImportPath(name);
        if (key.Succeeded()) domain.erase(*key.value);
    }
    ProjectObjectCatalogSources result;
    result.base.archive = project.archiveSession_.get();
    result.base.readOnlyArchive = result.base.archive ? nullptr : project.readOnlyArchive_.get();
    result.base.directory = result.base.archive || result.base.readOnlyArchive ? nullptr : project.directorySnapshot_.get();
    result.base.campaign = project.campaignScope_.get();
    result.base.gameRoot = project.war3GameRoot_;
    const auto assets = project.CaptureAssets();
    std::set<std::string> affected, removed;
    for (const auto& edit : assets.edits) {
        const auto from = Logical(project, edit.from, domain), to = Logical(project, edit.to, domain);
        if (!from.empty() && from != to) removed.insert(from);
        if (!to.empty() && from != to) affected.insert(to);
    }
    for (const auto& entry : assets.imports) {
        const auto logical = Logical(project, entry.archivePath, domain);
        if (!logical.empty()) affected.insert(logical);
    }
    if (affected.empty() && removed.empty()) return Output::Success(std::move(result));
    auto guard = std::make_shared<Render::AnimationSourceGuard>();
    const auto entries = project.AssetEntries(assets);
    std::map<std::string, const AssetEntry*, std::less<>> selected;
    std::vector<std::string> renamedMembers;
    for (const auto& entry : entries) {
        const auto logical = Logical(project, entry.path, domain);
        if (!affected.contains(logical)) continue;
        if (entry.ambiguous || !selected.emplace(logical, &entry).second)
            return Output::Failure("Object.Catalog", "OBJECT_CATALOG_OVERLAY_CONFLICT", "物编表会话覆盖存在同名冲突。");
        if (entry.size < 0 || static_cast<std::uint64_t>(entry.size) > kMaximum)
            return Output::Failure("Object.Catalog", "OBJECT_CATALOG_SOURCE_BUDGET", "物编表会话覆盖超过 32 MiB。");
        if (!entry.Pending() && result.base.archive) renamedMembers.push_back(entry.originalPath.toStdString());
    }
    std::map<std::string, std::vector<std::byte>> renamedBytes;
    if (!renamedMembers.empty()) {
        const auto captured = result.base.archive->CaptureExactMembers(renamedMembers);
        if (!captured.Succeeded()) return {{}, captured.diagnostics};
        for (const auto& input : *captured.value) {
            if (!input.present) return Output::Failure("Object.Catalog", "OBJECT_CATALOG_SOURCE_CHANGED", "改名的物编表实际来源已缺失。");
            renamedBytes.emplace(input.name, input.bytes);
        }
    }
    std::string identity;
    const auto append = [&](std::string_view text) { identity += std::to_string(text.size()) + ':'; identity.append(text); };
    std::size_t retained = 0;
    for (const auto& name : affected) {
        const auto found = selected.find(name);
        if (found == selected.end()) return Output::Failure("Object.Catalog", "OBJECT_CATALOG_OVERLAY_UNAVAILABLE", "会话物编表的最终来源未能确认。");
        const auto& entry = *found->second;
        std::vector<std::byte> bytes;
        if (entry.ownedPayload) bytes = *entry.ownedPayload;
        else if (!entry.diskPath.isEmpty()) {
            const auto path = std::filesystem::path(entry.diskPath.toStdWString());
            if (const auto status = guard->Watch(path); !status.Succeeded()) return {{}, status.diagnostics};
            auto read = Script::CaptureReadOnlyScriptSource(path, true, kMaximum);
            if (!read.Succeeded()) return {{}, std::move(read.diagnostics)};
            bytes = std::move(read.value->bytes);
            append(IO::GenericPathUtf8(path));
        } else if (result.base.archive) bytes = renamedBytes.at(entry.originalPath.toStdString());
        else if (result.base.readOnlyArchive) {
            auto read = result.base.readOnlyArchive->ReadMember(entry.originalPath.toStdString(), kMaximum);
            if (!read.Succeeded()) return {{}, std::move(read.diagnostics)};
            bytes = std::move(*read.value);
        } else if (result.base.campaign) {
            const std::vector<std::string> oldName{entry.originalPath.toStdString()};
            auto read = result.base.campaign->CaptureExactMembers(result.base.campaign->Revision(), oldName);
            if (!read.Succeeded()) return {{}, std::move(read.diagnostics)};
            if (!read.value->front().present) return Output::Failure("Object.Catalog", "OBJECT_CATALOG_SOURCE_CHANGED", "改名的战役物编表来源已缺失。");
            bytes = std::move(read.value->front().bytes);
        } else if (result.base.directory) {
            const WorkspaceFile* source = nullptr;
            for (const auto& file : result.base.directory->Snapshot().files) {
                if (EditorProject::AssetKey(QString::fromStdWString(file.relativePath.wstring())) != EditorProject::AssetKey(entry.originalPath)) continue;
                if (source) return Output::Failure("Object.Catalog", "OBJECT_CATALOG_DIRECTORY_CONFLICT", "改名的物编表存在多个实际来源。");
                source = &file;
            }
            if (!source) return Output::Failure("Object.Catalog", "OBJECT_CATALOG_SOURCE_CHANGED", "改名的物编表没有目录来源。");
            const auto path = result.base.directory->Snapshot().root / source->relativePath;
            if (const auto status = guard->Watch(path); !status.Succeeded()) return {{}, status.diagnostics};
            auto read = Script::CaptureReadOnlyScriptSource(path, true, kMaximum);
            if (!read.Succeeded()) return {{}, std::move(read.diagnostics)};
            if (read.value->binding.sha256 != source->sha256 || read.value->binding.bytes != source->size)
                return Output::Failure("Object.Catalog", "OBJECT_CATALOG_SOURCE_CHANGED", "改名物编表的字节与目录快照不同。");
            bytes = std::move(read.value->bytes);
        } else return Output::Failure("Object.Catalog", "OBJECT_CATALOG_OVERLAY_UNAVAILABLE", "物编表覆盖没有可核验的源。");
        if (bytes.size() != static_cast<std::size_t>(entry.size) || bytes.size() > 64U * 1024U * 1024U - retained)
            return Output::Failure("Object.Catalog", "OBJECT_CATALOG_SOURCE_BUDGET", "物编表会话覆盖长度变化或超过累积预算。");
        retained += bytes.size();
        auto hash = Sha256(bytes);
        if (!hash.Succeeded()) return {{}, std::move(hash.diagnostics)};
        append(name); append(entry.originalPath.toStdString()); append(*hash.value);
        removed.erase(name);
        result.replacements.push_back({name, std::move(bytes)});
    }
    result.removals.assign(removed.begin(), removed.end());
    for (const auto& name : removed) { append("removed"); append(name); }
    const auto hash = Sha256Text(identity);
    if (!hash.Succeeded()) return {{}, hash.diagnostics};
    result.base.overlayIdentity = *hash.value;
    result.base.overlayVerifier = [guard] { return guard->Verify(); };
    if (const auto status = guard->Verify(); !status.Succeeded()) return {{}, status.diagnostics};
    return Output::Success(std::move(result));
}

class ProjectObjectCatalogState {
public:
    const MapArchiveSession* archive = nullptr;
    const MpqWorkspace* readOnly = nullptr;
    const DirectoryWorkspace* directory = nullptr;
    const Services::CampaignSession* campaign = nullptr;
    Services::CampaignRevision campaignRevision;
    std::filesystem::path gameRoot;
    std::shared_ptr<const Game::WorldEditStrings> strings;
    AssetState assets;
    std::shared_ptr<const ObjectData::EffectiveObjectCatalog> catalog;
    bool refreshing = false;
};

Status EnsureProjectObjectCatalog(EditorProject& project, bool force) {
    if (project.war3GameRoot_.empty() && !project.objectCatalogState_ && !project.archiveSession_
        && !project.readOnlyArchive_ && !project.directorySnapshot_ && !project.campaignScope_) return Status::Ok();
    if (!project.objectCatalogState_) project.objectCatalogState_ = std::make_shared<ProjectObjectCatalogState>();
    auto& state = *project.objectCatalogState_;
    if (state.refreshing) return Status::Ok();
    const auto assets = project.CaptureAssets();
    const auto strings = project.CurrentWorldEditStrings();
    const auto campaignRevision = project.campaignScope_ ? project.campaignScope_->Revision() : Services::CampaignRevision{};
    const bool same = state.archive == project.archiveSession_.get() && state.readOnly == project.readOnlyArchive_.get()
        && state.directory == project.directorySnapshot_.get() && state.campaign == project.campaignScope_.get()
        && state.campaignRevision == campaignRevision && state.gameRoot == project.war3GameRoot_
        && state.strings == strings && state.assets == assets;
    if (!force && same && state.catalog && !project.sourceChangeSuspected_ && !project.sourceChangeConfirmed_) {
        const auto status = state.catalog->VerifyCapturedSources();
        if (status.Succeeded()) return status;
        emit project.ObjectCatalogAboutToChange();
        project.standardCatalog_.reset(); state.catalog.reset();
        emit project.ObjectCatalogChanged();
        return status;
    }
    state.refreshing = true;
    struct Finish { bool& active; ~Finish() { active = false; } } finish{state.refreshing};
    emit project.ObjectCatalogAboutToChange();
    struct Notify { EditorProject& project; ~Notify() { emit project.ObjectCatalogChanged(); } } notify{project};
    project.standardCatalog_.reset(); state.catalog.reset();
    const auto source = CaptureProjectObjectCatalogSources(project);
    if (!source.Succeeded()) return {source.diagnostics};
    ObjectData::CatalogLoadOptions options; options.strings = strings.get();
    auto loaded = Services::LoadEffectiveObjectCatalog(source.value->Sources(), options);
    if (!loaded.Succeeded()) return {loaded.diagnostics};
    state.archive = project.archiveSession_.get(); state.readOnly = project.readOnlyArchive_.get();
    state.directory = project.directorySnapshot_.get(); state.campaign = project.campaignScope_.get();
    state.campaignRevision = campaignRevision; state.gameRoot = project.war3GameRoot_; state.assets = assets; state.strings = strings;
    state.catalog = std::make_shared<const ObjectData::EffectiveObjectCatalog>(std::move(*loaded.value));
    project.standardCatalog_ = std::shared_ptr<const ObjectData::StandardObjectCatalog>(state.catalog, &state.catalog->Catalog());
    return {std::move(loaded.diagnostics)};
}

std::shared_ptr<const ObjectData::EffectiveObjectCatalog> ProjectEffectiveObjectCatalog(const EditorProject& project) {
    return project.objectCatalogState_ ? project.objectCatalogState_->catalog : nullptr;
}

Status VerifyProjectObjectCatalog(const EditorProject& project) {
    const auto expected = ProjectEffectiveObjectCatalog(project);
    if (!expected) return Fail("OBJECT_CATALOG_UNAVAILABLE", "没有已核验的有效物编目录。");
    auto captured = CaptureProjectObjectCatalogSources(project);
    if (!captured.Succeeded()) return {captured.diagnostics};
    return Services::VerifyEffectiveObjectCatalog(*expected, captured.value->Sources());
}

void InstallProjectObjectCatalogInvalidation(EditorProject& project) {
    const QPointer<EditorProject> guarded(&project);
    const auto refresh = [guarded] {
        if (!guarded) return;
        const auto status = EnsureProjectObjectCatalog(*guarded);
        for (const auto& diagnostic : status.diagnostics) if (diagnostic.severity == DiagnosticSeverity::Error)
            guarded->AddDiagnostic(QObject::tr("错误"), QObject::tr("物编来源"), QString::fromStdString(diagnostic.code), QString::fromStdString(diagnostic.message));
    };
    // 在EditorProject构造期间注册，先于各页面观察者；后者不会看到上张图的基础目录。
    QObject::connect(&project, &EditorProject::MapDocumentChanged, &project, refresh);
    QObject::connect(&project, &EditorProject::ImportsChanged, &project, refresh);
    QObject::connect(&project, &EditorProject::WorldEditStringsChanged, &project, refresh);
    QObject::connect(&project, &EditorProject::SourceStateChanged, &project, refresh);
}
} // namespace War3Map::QtProject
