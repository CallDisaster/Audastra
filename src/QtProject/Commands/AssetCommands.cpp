#include "AssetCommands.h"
#include "PreparedAssetState.h"

#include "War3MapCore/MapArchiveSession.h"

#include <QUndoStack>

#include <exception>
#include <utility>

namespace War3Map::QtProject {

AssetStateCommand::AssetStateCommand(EditorProject& project, AssetState before, AssetState after, QString label,
    std::shared_ptr<PreparedAssetStateEdit> firstPrepared, std::shared_ptr<Status> firstStatus)
    : QUndoCommand(std::move(label)), project_(project), before_(std::move(before)), after_(std::move(after)),
      archive_(project.archiveSession_.get()), directory_(project.directorySnapshot_.get()),
      sourceSha256_(archive_ ? archive_->SourceSha256() : std::string{}),
      sourcePath_(project.sourceArchivePath_), projectPath_(project.projectFilePath_),
      generation_(project.editRevision_.Generation()), firstPrepared_(std::move(firstPrepared)), firstStatus_(std::move(firstStatus)) {}

void AssetStateCommand::Apply(bool forward) {
    const auto& expected = forward ? before_ : after_;
    const auto& wanted = forward ? after_ : before_;
    const bool bound = project_.archiveSession_.get() == archive_
        && project_.directorySnapshot_.get() == directory_
        && project_.sourceArchivePath_ == sourcePath_ && project_.projectFilePath_ == projectPath_
        && project_.editRevision_.Generation() == generation_
        && (!archive_ || archive_->SourceSha256() == sourceSha256_);
    AssetState candidate;
    try {
        if (!bound || project_.CaptureAssets() != expected) {
            if (firstStatus_) *firstStatus_ = Status::Failure("Asset", "ASSET_SOURCE_STALE", "资源前值或来源已变化，未发布。");
            project_.AddDiagnostic(QObject::tr("错误"), QObject::tr("资源"), "ASSET_UNDO_STALE",
                QObject::tr("资源来源或当前状态已改变，不能把旧资源收据覆盖到当前工程；此次撤销或重做未修改资源。"));
            setObsolete(true);
            return;
        }
        // 两个容器都先分配完整候选，再由同一个发布入口交换；分配失败保留两侧。
        candidate = wanted;
    } catch (const std::exception&) {
        if (firstStatus_) *firstStatus_ = Status::Failure("Asset", "ASSET_REVISION_PREPARE_FAILED", "资源候选准备失败，未发布。");
        project_.AddDiagnostic(QObject::tr("错误"), QObject::tr("资源"), "ASSET_UNDO_PREPARE_FAILED",
            QObject::tr("无法准备资源收据，当前资源保持原状。"));
        setObsolete(true);
        return;
    }
    auto prepared = firstPrepared_ ? Result<std::shared_ptr<PreparedAssetStateEdit>>::Success(std::move(firstPrepared_))
        : project_.PrepareAssetStateEdit(std::move(candidate), generation_, project_.editRevision_.Revision(),
            forward ? "重做资源修改" : "撤销资源修改", forward ? EditOperation::Redo : EditOperation::Undo);
    Status status;
    if (!prepared.Succeeded()) status.diagnostics = prepared.diagnostics;
    else {
        AssetState sealed;
        try { sealed = (*prepared.value)->Candidate(); }
        catch (...) {
            status = Status::Failure("Asset", "ASSET_REVISION_PREPARE_FAILED", "无法保存已封存资源收据，未发布。");
        }
        if (status.Succeeded()) {
            auto& cached = forward ? after_ : before_;
            std::swap(cached, sealed);
            status = project_.PublishPreparedAssetState(**prepared.value);
            if (!status.Succeeded()) std::swap(cached, sealed);
        }
    }
    if (firstStatus_) { *firstStatus_ = status; firstStatus_.reset(); }
    if (!status.Succeeded()) {
        setObsolete(true);
        for (const auto& diagnostic : status.diagnostics) project_.AddDiagnostic(QObject::tr("错误"), QObject::tr("资源"),
            QString::fromStdString(diagnostic.code), QString::fromStdString(diagnostic.message));
    }
}

QString EditorProject::RecordAssetEdit(const QString& label,
    const std::function<QString(AssetState&)>& prepare) {
    if (!assetUndoStack_) return tr("资源撤销栈未就绪。");
    try {
        auto before = CaptureAssets();
        auto candidate = before;
        const auto problem = prepare(candidate);
        if (!problem.isEmpty()) return problem;
        if (candidate == before) return {};
        auto prepared = PrepareAssetStateEdit(candidate, editRevision_.Generation(), editRevision_.Revision(), label.toStdString());
        if (!prepared.Succeeded()) return QString::fromStdString(prepared.diagnostics.front().code + " · " + prepared.diagnostics.front().message);
        auto firstStatus = std::make_shared<Status>();
        auto command = std::make_unique<AssetStateCommand>(*this, std::move(before), std::move(candidate), label, *prepared.value, firstStatus);
        assetUndoStack_->push(command.release());
        if (!firstStatus->Succeeded()) return QString::fromStdString(firstStatus->diagnostics.front().code + " · " + firstStatus->diagnostics.front().message);
        return {};
    } catch (const std::exception&) {
        return tr("无法准备资源编辑，当前资源保持原状。");
    }
}

} // namespace War3Map::QtProject
