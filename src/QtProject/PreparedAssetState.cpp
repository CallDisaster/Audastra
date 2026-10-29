#include "PreparedAssetState.h"
#include "PendingImportPayload.h"
#include <QPointer>
#include <QSet>
#include <set>
#include <algorithm>

namespace War3Map::QtProject {
Result<std::shared_ptr<PreparedAssetStateEdit>> EditorProject::PrepareAssetStateEdit(AssetState candidate,
    std::uint64_t expectedGeneration, std::uint64_t expectedRevision, std::string description, EditOperation operation) {
    return PrepareAssetStateEditImpl(std::move(candidate), expectedGeneration, expectedRevision, std::move(description), operation, nullptr);
}

Result<std::shared_ptr<PreparedAssetStateEdit>> EditorProject::PrepareAssetStateEditImpl(AssetState candidate,
    std::uint64_t expectedGeneration, std::uint64_t expectedRevision, std::string description, EditOperation operation,
    const AssetState* sealedReceipt) {
    using Output = Result<std::shared_ptr<PreparedAssetStateEdit>>;
    try {
        if (editTrackingFailed_ || sourceChangeSuspected_ || sourceChangeConfirmed_
            || editRevision_.Generation() != expectedGeneration || editRevision_.Revision() != expectedRevision)
            return Output::Failure("Asset", "ASSET_SOURCE_STALE", "资源来源或编辑修订已变化，未准备资源修改。");
        if (const auto valid = ValidatePendingImports(candidate.imports); !valid.Succeeded()) return {std::nullopt, valid.diagnostics};
        auto before = CaptureAssets();
        std::set<const std::vector<std::byte>*> trustedBodies;
        if (editTrackingReady_) for (const auto& entry : before.imports) if (entry.ownedPayload) trustedBodies.insert(entry.ownedPayload.get());
        // 仅内部撤销命令可重用它保存的已封存正文；公共准备入口仍复制新传入的正文。
        if (sealedReceipt) for (const auto& entry : sealedReceipt->imports) if (entry.ownedPayload) trustedBodies.insert(entry.ownedPayload.get());
        for (auto& entry : candidate.imports) if (entry.ownedPayload && !trustedBodies.contains(entry.ownedPayload.get()))
            entry.ownedPayload = std::make_shared<const std::vector<std::byte>>(*entry.ownedPayload);
        QSet<QString> importPaths, editPaths;
        for (const auto& entry : candidate.imports) {
            const auto problem = CheckAssetPath(entry.archivePath);
            if (!problem.isEmpty() || importPaths.contains(AssetKey(entry.archivePath)))
                return Output::Failure("Asset", "ASSET_REVISION_PREPARE_FAILED", "候选导入路径无效或重复：" + entry.archivePath.toStdString());
            importPaths.insert(AssetKey(entry.archivePath));
        }
        for (const auto& entry : candidate.edits) {
            if (!CheckAssetPath(entry.from).isEmpty() || (!entry.to.isEmpty() && !CheckAssetPath(entry.to).isEmpty())
                || editPaths.contains(AssetKey(entry.from)))
                return Output::Failure("Asset", "ASSET_REVISION_PREPARE_FAILED", "候选资源编辑路径无效或重复。");
            editPaths.insert(AssetKey(entry.from));
        }
        std::vector<std::pair<std::size_t, std::string>> diskFingerprints;
        for (std::size_t i = 0; i < candidate.imports.size(); ++i) {
            auto hash = FingerprintPendingImport(candidate.imports[i]);
            if (!hash.Succeeded()) return {std::nullopt, hash.diagnostics};
            if (!candidate.imports[i].ownedPayload) diskFingerprints.emplace_back(i, std::move(*hash.value));
        }
        std::optional<PreparedEditObservation> observation;
        if (editTrackingReady_) {
            const auto current = CaptureEditScopes();
            if (!current || *current != editRevision_.Current())
                return Output::Failure("Asset", "ASSET_SOURCE_STALE", "当前资源与修订快照不一致，未修改资源。");
            auto scopes = CaptureEditScopes(&candidate);
            if (!scopes) return Output::Failure("Asset", "ASSET_REVISION_PREPARE_FAILED", "无法建立候选资源的完整指纹和编辑修订，未修改资源。");
            auto prepared = editRevision_.PrepareObservation(expectedGeneration, expectedRevision, std::move(*scopes), std::move(description), operation);
            if (!prepared.Succeeded()) return {std::nullopt, prepared.diagnostics};
            observation.emplace(std::move(*prepared.value));
        }
        // 未打开地图的资源队列沿用既有模式；不伪造全图修订、不把已有failed状态降回未跟踪。
        return Output::Success(std::shared_ptr<PreparedAssetStateEdit>(new PreparedAssetStateEdit(*this, std::move(before), std::move(candidate),
            std::move(observation), std::move(diskFingerprints))));
    } catch (...) {
        return Output::Failure("Asset", "ASSET_REVISION_PREPARE_FAILED", "无法完整准备资源候选和编辑修订，未修改资源或撤销栈。");
    }
}

Status EditorProject::PublishPreparedAssetState(PreparedAssetStateEdit& prepared) {
    QPointer<EditorProject> guard;
    try {
        if (prepared.consumed_ || prepared.owner_ != this || archiveSession_.get() != prepared.archive_ || directorySnapshot_.get() != prepared.directory_
            || sourceArchivePath_ != prepared.sourcePath_ || projectFilePath_ != prepared.projectPath_
            || pendingImports_ != prepared.before_.imports || assetEdits_ != prepared.before_.edits
            || runtimeApprovedPackageSha256_ != prepared.before_.runtimeApprovedPackageSha256
            || prepared.tracked_ != editTrackingReady_ || prepared.generation_ != editRevision_.Generation() || prepared.revision_ != editRevision_.Revision()
            || editTrackingFailed_ || sourceChangeSuspected_ || sourceChangeConfirmed_)
            return Status::Failure("Asset", "ASSET_SOURCE_STALE", "候选资源的来源或前值已变化，未发布。");
        for (const auto& [index, expected] : prepared.diskFingerprints_) {
            const auto hash = FingerprintPendingImport(prepared.after_.imports[index]);
            if (!hash.Succeeded() || *hash.value != expected)
                return Status::Failure("Asset", "ASSET_SOURCE_STALE", "候选磁盘导入在准备后发生变化，未发布。");
        }
        struct State { EditorProject* project; AssetState* candidate; } commitState{this, &prepared.after_};
        guard = this;
        const auto publish = [](void* opaque) noexcept {
            auto& state = *static_cast<State*>(opaque);
            state.project->pendingImports_.swap(state.candidate->imports);
            state.project->assetEdits_.swap(state.candidate->edits);
            state.project->runtimeApprovedPackageSha256_.swap(state.candidate->runtimeApprovedPackageSha256);
        };
        if (prepared.observation_) {
            const auto published = editRevision_.CommitObservation(std::move(*prepared.observation_), publish, &commitState);
            if (!published.Succeeded()) return published;
        } else publish(&commitState);
        prepared.consumed_ = true;
    } catch (...) {
        return Status::Failure("Asset", "ASSET_REVISION_PREPARE_FAILED", "发布前无法复核候选资源，未修改资源。");
    }
    // 只修改资源scope，物编和触发器脏标记不变；不在发布后重新分配/捕获整图指纹。
    emit ImportsChanged();
    if (guard) emit guard->ImportsEdited();
    return Status::Ok();
}
}
