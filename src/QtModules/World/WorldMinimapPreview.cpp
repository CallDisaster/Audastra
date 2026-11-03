#include "WorldMinimapPreview.h"
#include "EditorProject.h"
#include "War3MapCore/Services/MinimapPreview.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QPushButton>
#include <QThread>
#include <QTimer>
#include <QUndoStack>
#include <QVBoxLayout>

namespace War3Map::Modules {
WorldMinimapPreview::WorldMinimapPreview(QtProject::EditorProject& project, QWidget* parent)
    : QWidget(parent), project_(project) {
    setObjectName("worldMinimapPreview");
    auto* column = new QVBoxLayout(this); column->setContentsMargins(10, 6, 10, 6); column->setSpacing(4);
    auto* row = new QHBoxLayout;
    row->addWidget(new QLabel(tr("已保存小地图"), this)); row->addStretch();
    refresh_ = new QPushButton(tr("刷新"), this); refresh_->setObjectName("worldMinimapRefresh");
    row->addWidget(refresh_); column->addLayout(row);
    image_ = new QLabel(this); image_->setObjectName("worldMinimapImage");
    image_->setAlignment(Qt::AlignCenter); image_->setMinimumSize(128, 128); image_->setMaximumHeight(160);
    image_->setStyleSheet("background: #000;"); column->addWidget(image_);
    state_ = new QLabel(this); state_->setObjectName("worldMinimapState"); state_->setWordWrap(true);
    column->addWidget(state_);
    connect(refresh_, &QPushButton::clicked, this, [this] { loadedSha_.clear(); Refresh(); });
    connect(&project_, &QtProject::EditorProject::MapDocumentChanged, this, [this] { Refresh(); });
    connect(&project_, &QtProject::EditorProject::ImportsChanged, this, [this] { Refresh(); });
    connect(&project_, &QtProject::EditorProject::SourceStateChanged, this, [this] { Refresh(); });
    connect(&project_, &QtProject::EditorProject::EditTrackingBaselineChanged, this, [this] { UpdateStatus(); });
    if (project_.mapDataUndoStack_) connect(project_.mapDataUndoStack_, &QUndoStack::indexChanged,
        this, [this] { UpdateStatus(); });
    QTimer::singleShot(0, this, [this] { Refresh(); });
}

WorldMinimapPreview::~WorldMinimapPreview() {
    if (worker_) { disconnect(worker_, nullptr, this, nullptr); worker_->wait(); delete worker_.data(); }
}

void WorldMinimapPreview::Clear(const QString& reason) {
    loadedSha_.clear(); present_ = false; image_->clear(); state_->setText(reason);
    setProperty("sourceSha256", QString()); setProperty("rgbaSha256", QString());
}

void WorldMinimapPreview::UpdateStatus() {
    if (loadedSha_.isEmpty()) return;
    if (!present_) { state_->setText(tr("地图中没有已保存的小地图底图")); return; }
    state_->setText((project_.HasPendingCandidateEdits() || project_.HasPendingAssetEdits()
        || (project_.mapEdits_ && project_.mapEdits_->IsDirty()))
        ? tr("地图有未保存修改；此图仅含已保存内容") : tr("实际已保存底图 · 不含图标"));
}

void WorldMinimapPreview::Refresh() {
    if (worker_) { pending_ = true; Clear(tr("正在核对小地图来源…")); return; }
    if (project_.savingMap_ || project_.HasPendingTerrainCommit()) {
        QTimer::singleShot(100, this, [this] { Refresh(); }); return;
    }
    if (!project_.archiveSession_) { Clear(tr("请打开地图归档以查看已保存底图")); return; }
    if (project_.sourceChangeSuspected_ || project_.sourceChangeConfirmed_) {
        Clear(tr("地图来源待核验，预览已清除")); return;
    }
    const auto path = project_.sourceArchivePath_;
    const auto sha = project_.archiveSession_->SourceSha256();
    const auto generation = project_.editRevision_.Generation();
    if (loadedSha_ == QString::fromStdString(sha)) { UpdateStatus(); return; }
    Clear(tr("正在读取已保存底图…")); refresh_->setEnabled(false);
    auto result = std::make_shared<Result<Services::MinimapPreview>>();
    worker_ = QThread::create([path, sha, result] {
        try { *result = Services::OpenMinimapPreview(path, sha); }
        catch (...) { *result = Result<Services::MinimapPreview>::Failure("Minimap.Preview",
            "MINIMAP_PREVIEW_EXCEPTION", "小地图预览未完成，原地图未修改。"); }
    });
    auto* thread = worker_.data();
    connect(thread, &QThread::finished, this, [this, result, sha, path, generation] {
        worker_ = nullptr; refresh_->setEnabled(true);
        if (pending_ || project_.savingMap_ || !project_.archiveSession_
            || generation != project_.editRevision_.Generation() || path != project_.sourceArchivePath_
            || sha != project_.archiveSession_->SourceSha256()
            || project_.sourceChangeSuspected_ || project_.sourceChangeConfirmed_) {
            pending_ = false; Refresh(); return;
        }
        if (!result->Succeeded()) {
            QString reason = tr("小地图预览失败");
            for (const auto& diagnostic : result->diagnostics) if (diagnostic.severity == DiagnosticSeverity::Error) {
                reason = QString::fromStdString(diagnostic.code + ": " + diagnostic.message); break;
            }
            Clear(reason); return;
        }
        const auto& value = *result->value; present_ = value.present;
        loadedSha_ = QString::fromStdString(sha); setProperty("sourceSha256", loadedSha_);
        setProperty("rgbaSha256", QString::fromStdString(value.rgbaSha256));
        if (present_) {
            const QImage image(reinterpret_cast<const uchar*>(value.image.rgba.data()),
                static_cast<int>(value.image.width), static_cast<int>(value.image.height), QImage::Format_RGBA8888);
            image_->setPixmap(QPixmap::fromImage(image.copy()).scaled(160, 160, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        }
        UpdateStatus();
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start(QThread::LowPriority);
}
} // namespace War3Map::Modules
