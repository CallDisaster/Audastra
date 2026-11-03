#include "WorldMinimapPreview.h"
#include "EditorProject.h"
#include "War3MapCore/Services/MinimapPreview.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/Resources/ImportTable.h"
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
    connect(&project_, &QtProject::EditorProject::PendingTerrainCommitStateChanged,
        this, [this](bool pending) { if (!pending) Refresh(); });
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
    state_->setVisible(!reason.isEmpty());
    setProperty("sourceSha256", QString()); setProperty("rgbaSha256", QString());
}

void WorldMinimapPreview::UpdateStatus() {
    if (loadedSha_.isEmpty()) return;
    if (!present_) { state_->setText(tr("没有小地图")); state_->show(); return; }
    state_->setText((project_.HasPendingCandidateEdits() || project_.HasPendingAssetEdits()
        || (project_.mapEdits_ && project_.mapEdits_->IsDirty()))
        ? tr("有未保存修改") : QString());
    state_->setVisible(!state_->text().isEmpty());
}

void WorldMinimapPreview::Refresh() {
    if (worker_) { pending_ = true; Clear(tr("读取中…")); return; }
    if (project_.savingMap_) {
        QTimer::singleShot(100, this, [this] { Refresh(); }); return;
    }
    if (project_.HasPendingTerrainCommit()) return;
    if (!project_.archiveSession_) { Clear({}); return; }
    if (project_.sourceChangeSuspected_ || project_.sourceChangeConfirmed_) {
        Clear(tr("地图文件已变化")); return;
    }
    const auto path = project_.sourceArchivePath_;
    const auto sha = project_.archiveSession_->SourceSha256();
    const auto generation = project_.editRevision_.Generation();
    if (loadedSha_ == QString::fromStdString(sha)) { UpdateStatus(); return; }
    Clear(tr("读取中…")); refresh_->setEnabled(false);
    // 只冻结打开时已经核验的底图。后台不重新持有磁盘源句柄，不妨碍原地保存。
    Services::MinimapPreviewInput input; input.sourceSha256 = sha;
    for (const auto& member : project_.archiveSession_->Members()) {
        const auto key = Resources::CanonicalImportPath(member.name);
        if (!key.Succeeded() || *key.value != "WAR3MAPMAP.BLP") continue;
        if (member.data.size() > 4U * 1024U * 1024U) {
            project_.AddDiagnostic(tr("错误"), "Minimap.Preview", "MINIMAP_PREVIEW_BUDGET", tr("底图超过4MiB输入预算。"));
            refresh_->setEnabled(true); Clear(tr("读取失败")); return;
        }
        input.member = member.data; break;
    }
    if (!input.member && project_.archiveSession_->HasOpaqueMembers()) {
        project_.AddDiagnostic(tr("警告"), "Minimap.Preview", "MINIMAP_PREVIEW_UNCERTAIN",
            tr("打开快照未列出底图，匿名成员使缺失状态未确定；可用精确归档查询确认。"));
        refresh_->setEnabled(true); Clear(tr("无法确认是否有小地图")); return;
    }
    auto result = std::make_shared<Result<Services::MinimapPreview>>();
    worker_ = QThread::create([input = std::move(input), result] {
        try { *result = Services::DecodeMinimapPreview(input); }
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
            project_.AddDiagnostics(result->diagnostics);
            Clear(tr("读取失败")); return;
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
