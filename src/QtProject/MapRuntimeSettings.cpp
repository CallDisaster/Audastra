#include "EditorProject.h"
#include "PendingImportPayload.h"
#include "War3MapCore/VerifiedFile.h"
#include "War3MapCore/Workspace.h"

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
        ArchiveExactMemberLimits limits; limits.memberCount = 1; limits.memberBytes = limits.totalBytes = 4096;
        const auto captured = project.archiveSession_->CaptureExactMembers(
            std::vector<std::string>{std::string(Services::kMapRuntimeSettingsMember)},limits);
        if (!captured.Succeeded()) return {{},captured.diagnostics};
        return captured.value->front().present ? Services::ParseMapRuntimeSettings(captured.value->front().bytes)
            : Result<Settings>::Success({});
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

Status EditorProject::PrepareMapRuntimeSettings(AssetState& state, const Settings& settings,
    std::uint64_t expectedGeneration, std::uint64_t expectedRevision) const {
    if (!editTrackingReady_ || editTrackingFailed_ || sourceChangeSuspected_ || sourceChangeConfirmed_
        || editRevision_.Generation() != expectedGeneration || editRevision_.Revision() != expectedRevision)
        return Status::Failure("MapRuntime","RUNTIME_REVISION_STALE","启用决定的工程修订已变化；未提交旧确认。");
    const auto previous = ReadSettings(*this);
    if (!previous.Succeeded()) return {previous.diagnostics};
    const auto bytes = Services::SerializeMapRuntimeSettings(settings);
    if (!bytes.Succeeded()) return {bytes.diagnostics};
    if (settings.mode != Services::MapRuntimeMode::Disabled) {
        if (settings.packageSha256.empty())
            return Status::Failure("MapRuntime","RUNTIME_CONSENT_REQUIRED","明确启用必须选择已列出的可信包身份。");
        const Services::MapRuntimePackageSource* source = nullptr;
        for (const auto& item : runtimePackageSources_) if (item.manifestSha256 == settings.packageSha256) {
            if (source) return Status::Failure("MapRuntime","RUNTIME_PACKAGE_CONFLICT","可信目录存在重复包身份。");
            source = &item;
        }
        if (!source) return Status::Failure("MapRuntime","RUNTIME_PACKAGE_MISSING","本机缺少所选可信包；未采用地图提供的路径或最新版。");
        // 允许确认内部包身份，但保存用途和独立副本限制仍由共用保存服务核实。
        const auto captured = Services::MapRuntimePackage::Capture(*source,Services::MapRuntimeSavePurpose::InternalValidation);
        if (!captured.Succeeded()) return {captured.diagnostics};
    }
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
