#include "AssetCommands.h"

#include "War3MapCore/MapArchiveSession.h"

#include <QUndoStack>

#include <exception>
#include <utility>

namespace War3Map::QtProject {

AssetStateCommand::AssetStateCommand(EditorProject& project, AssetState before, AssetState after, QString label)
    : QUndoCommand(std::move(label)), project_(project), before_(std::move(before)), after_(std::move(after)),
      archive_(project.archiveSession_.get()), directory_(project.directorySnapshot_.get()),
      sourceSha256_(archive_ ? archive_->SourceSha256() : std::string{}),
      sourcePath_(project.sourceArchivePath_), projectPath_(project.projectFilePath_),
      generation_(project.editRevision_.Generation()) {}

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
            project_.AddDiagnostic(QObject::tr("错误"), QObject::tr("资源"), "ASSET_UNDO_STALE",
                QObject::tr("资源来源或当前状态已改变，不能把旧资源收据覆盖到当前工程；此次撤销或重做未修改资源。"));
            setObsolete(true);
            return;
        }
        // 两个容器都先分配完整候选，再由同一个发布入口交换；分配失败保留两侧。
        candidate = wanted;
    } catch (const std::exception&) {
        project_.AddDiagnostic(QObject::tr("错误"), QObject::tr("资源"), "ASSET_UNDO_PREPARE_FAILED",
            QObject::tr("无法准备资源收据，当前资源保持原状。"));
        setObsolete(true);
        return;
    }
    project_.PublishAssetState(std::move(candidate));
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
        auto command = std::make_unique<AssetStateCommand>(*this, std::move(before), std::move(candidate), label);
        assetUndoStack_->push(command.release());
        return {};
    } catch (const std::exception&) {
        return tr("无法准备资源编辑，当前资源保持原状。");
    }
}

} // namespace War3Map::QtProject
