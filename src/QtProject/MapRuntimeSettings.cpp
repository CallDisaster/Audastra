#include "EditorProject.h"
#include "PendingImportPayload.h"
#include "../QtCommon/ApplicationPaths.h"
#include "War3MapCore/VerifiedFile.h"
#include "War3MapCore/Workspace.h"
#include "War3MapCore/Services/MapRuntimePreparation.h"

namespace War3Map::QtProject {
namespace {
using Settings = Services::MapRuntimeSettings;
Result<Settings> ReadSettings(const EditorProject& project) {
    const auto wanted = EditorProject::AssetKey(QString::fromUtf8(Services::kMapRuntimeSettingsMember));
    const auto fail = [](std::string message) {
        return Result<Settings>::Failure("MapRuntime", "RUNTIME_SETTINGS_INVALID", std::move(message));
    };
    for (const auto& edit : project.assetEdits_)
        if (EditorProject::AssetKey(edit.from) == wanted || EditorProject::AssetKey(edit.to) == wanted)
            return fail("运行库设置被普通资源命令移动或删除；请先撤销该改动，再使用共用设置事务。");
    const PendingImport* pending = nullptr;
    for (const auto& item : project.pendingImports_) if (EditorProject::AssetKey(item.archivePath) == wanted) {
        if (pending) return fail("运行库设置存在重复导入来源。");
        pending = &item;
    }
    if (pending) {
        if (pending->size < 0 || pending->size > 4096) return fail("运行库设置超过4KiB预算。");
        const auto identity = FingerprintPendingImport(*pending);
        if (!identity.Succeeded()) return {{},identity.diagnostics};
        const auto bytes = pending->ownedPayload ? ReadFrozenPendingImport(*pending,*identity.value)
            : ReadVerifiedFile(std::filesystem::path(pending->diskPath.toStdWString()),*identity.value,4096);
        return bytes.Succeeded() ? Services::ParseMapRuntimeSettings(*bytes.value) : Result<Settings>{{},bytes.diagnostics};
    }
    if (project.archiveSession_) {
        return Services::ReadMapRuntimeSettings(*project.archiveSession_);
    }
    if (project.directorySnapshot_) {
        const auto& snapshot = project.directorySnapshot_->Snapshot();
        const WorkspaceFile* found = nullptr;
        for (const auto& file : snapshot.files)
            if (EditorProject::AssetKey(QString::fromStdWString(file.relativePath.wstring())) == wanted) {
                if (found) return fail("目录中的运行库设置存在大小写或斜杠歧义。");
                found = &file;
            }
        if (!found) return Result<Settings>::Success({});
        const auto bytes = ReadVerifiedFile(snapshot.root / found->relativePath,found->sha256,4096);
        return bytes.Succeeded() ? Services::ParseMapRuntimeSettings(*bytes.value) : Result<Settings>{{},bytes.diagnostics};
    }
    return fail("工程缺少可核验的地图或目录来源。");
}
}

Result<Services::MapRuntimeSettings> EditorProject::QueryMapRuntimeSettings() const { return ReadSettings(*this); }

Status EditorProject::ReloadInstalledMapRuntimeCatalog() {
    if (savingMap_) return Status::Failure("MapRuntime", "RUNTIME_CATALOG_BUSY", "保存期间不能重新载入运行库安装目录。");
    const auto loaded = Services::LoadInstalledMapRuntimeSources(
        std::filesystem::path(QtCommon::InstallationRoot().toStdWString()).lexically_normal());
    runtimeCatalogDiagnostics_ = loaded.diagnostics;
    runtimePackageSources_ = loaded.Succeeded() ? *loaded.value : std::vector<Services::MapRuntimePackageSource>{};
    return {runtimeCatalogDiagnostics_};
}

Result<EditorProject::MapRuntimeState> EditorProject::QueryMapRuntimeState() const {
    using Output = Result<MapRuntimeState>;
    if (!editTrackingReady_ || editTrackingFailed_ || sourceChangeSuspected_ || sourceChangeConfirmed_)
        return Output::Failure("MapRuntime", "RUNTIME_REVISION_STALE", "工程修订或来源尚未核实，不能返回可用于启用的旧状态。");
    if (!runtimeCatalogDiagnostics_.empty()) return {{},runtimeCatalogDiagnostics_};
    MapRuntimeState state; state.generation = editRevision_.Generation(); state.revision = editRevision_.Revision();
    const auto settings = ReadSettings(*this);
    if (!settings.Succeeded()) return {{},settings.diagnostics};
    const auto packages = Services::QueryMapRuntimePackages(runtimePackageSources_);
    if (!packages.Succeeded()) return {{},packages.diagnostics};
    state.settings = *settings.value; state.packages = *packages.value;
    state.approved = !state.settings.packageSha256.empty()
        && state.settings.mode != Services::MapRuntimeMode::Disabled
        && runtimeApprovedPackageSha256_ == state.settings.packageSha256;
    if (state.generation != editRevision_.Generation() || state.revision != editRevision_.Revision())
        return Output::Failure("MapRuntime", "RUNTIME_REVISION_STALE", "查询运行库时工程修订已变化。");
    return Output::Success(std::move(state));
}

Status EditorProject::PrepareMapRuntimeSettings(AssetState& state, const Settings& settings,
    std::uint64_t expectedGeneration, std::uint64_t expectedRevision) const {
    if (!editTrackingReady_ || editTrackingFailed_ || sourceChangeSuspected_ || sourceChangeConfirmed_
        || editRevision_.Generation() != expectedGeneration || editRevision_.Revision() != expectedRevision)
        return Status::Failure("MapRuntime","RUNTIME_REVISION_STALE","启用决定的工程修订已变化；未提交旧确认。");
    const auto previous = ReadSettings(*this);
    if (!previous.Succeeded()) return {previous.diagnostics};
    const auto bytes = Services::SerializeMapRuntimeSettings(settings);
    if (!bytes.Succeeded()) return {bytes.diagnostics};
    const auto validated = Services::ValidateMapRuntimeDecision({settings, settings.mode != Services::MapRuntimeMode::Disabled},
        runtimePackageSources_, runtimeCatalogDiagnostics_);
    if (!validated.Succeeded()) return validated;
    auto candidate = state;
    if (*previous.value != settings) {
        std::vector<ImportPayload> payloads{{QString::fromUtf8(Services::kMapRuntimeSettingsMember),
            std::make_shared<const std::vector<std::byte>>(*bytes.value)}};
        const auto problem = PrepareImportPayloads(candidate,payloads,true);
        if (!problem.isEmpty()) return Status::Failure("MapRuntime","RUNTIME_SETTINGS_CONFLICT",problem.toStdString());
    }
    candidate.runtimeApprovedPackageSha256 = settings.mode == Services::MapRuntimeMode::Disabled ? std::string{} : settings.packageSha256;
    state = std::move(candidate);
    return Status::Ok();
}
} // namespace War3Map::QtProject
