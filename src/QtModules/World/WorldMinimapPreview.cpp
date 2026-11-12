#include "WorldMinimapPreview.h"
#include "WorldMinimapMarkerDialog.h"
#include "WorldViewport.h"
#include "EditorProject.h"
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QToolButton>
#include <QTabBar>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QTimer>
#include <QVBoxLayout>

namespace War3Map::Modules {
class MinimapCanvas final : public QLabel {
public:
    explicit MinimapCanvas(QWidget* parent) : QLabel(parent) {
    }
    std::function<void(QPointF)> pan;
    QPolygonF outline;
    QRectF ImageRect() const {
        auto size = pixmap().size();
        if (size.isEmpty())
            return {};
        size.scale(contentsRect().size(), Qt::KeepAspectRatio);
        return QRectF(QPointF((width() - size.width()) / 2.0, (height() - size.height()) / 2.0), size);
    }

protected:
    void paintEvent(QPaintEvent* event) override {
        Q_UNUSED(event);
        const auto rect = ImageRect();
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        const QRectF frame = QRectF(this->rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        QPainterPath cardPath;
        cardPath.addRoundedRect(frame, 6.0, 6.0);
        // 卡片底色与描边跟随主题调色板（Base = 底色，Mid = 分隔线），浅色主题下不出现深色框。
        p.fillPath(cardPath, palette().color(QPalette::Base));
        p.save();
        p.setClipPath(cardPath);
        if (!pixmap().isNull())
            p.drawPixmap(rect.toRect(), pixmap());
        if (!outline.isEmpty()) {
            p.setClipRect(rect, Qt::IntersectClip);
            QPolygonF points;
            for (const auto& point : outline)
                points << QPointF(rect.left() + point.x() * rect.width(), rect.top() + point.y() * rect.height());
            p.setPen(QPen(Qt::white, 1.5));
            p.setBrush(Qt::NoBrush);
            p.drawPolygon(points);
        }
        p.restore();
        p.setPen(QPen(palette().color(QPalette::Mid), 1.0));
        p.setBrush(Qt::NoBrush);
        p.drawPath(cardPath);
    }
    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton)
            Move(event->position());
    }
    void mouseMoveEvent(QMouseEvent* event) override {
        if (event->buttons() & Qt::LeftButton)
            Move(event->position());
    }
    void Move(QPointF point) {
        const auto r = ImageRect();
        if (pan && !r.isEmpty() && r.contains(point))
            pan({(point.x() - r.left()) / r.width(), (point.y() - r.top()) / r.height()});
    }
};

WorldMinimapPreview::WorldMinimapPreview(QtProject::EditorProject& project, QWidget* parent, WorldViewport* viewport)
    : QWidget(parent), project_(project), preview_(project, this), viewport_(viewport) {
    setObjectName("worldMinimapPreview");
    auto* column = new QVBoxLayout(this);
    column->setContentsMargins(10, 8, 10, 8);
    column->setSpacing(6);
    auto* row = new QHBoxLayout;
    row->setSpacing(6);
    source_ = new QTabBar(this);
    source_->setObjectName("worldMinimapSource");
    source_->addTab(tr("当前编辑"));
    source_->addTab(tr("已保存"));
    row->addWidget(source_);
    row->addStretch();
    layers_ = new QToolButton(this);
    layers_->setText(tr("图层"));
    layers_->setObjectName("worldMinimapLayers");
    layers_->setCursor(Qt::PointingHandCursor);
    auto *menu = new QMenu(layers_);
    layers_->setMenu(menu);
    layers_->setPopupMode(QToolButton::InstantPopup);
    row->addWidget(layers_);
    menu->addAction(tr("编辑图标…"), this, [this] { OpenMinimapMarkerDialog(project_, this); });
    menu->addSeparator();
    const auto layer = [&](const char* name, std::uint32_t bit) {
        auto* action = menu->addAction(tr(name));
        action->setCheckable(true);
        action->setChecked((options_.layers & bit) != 0);
        action->setData(bit);
        connect(action, &QAction::toggled, this, [this, bit](bool on) {
            if (on)
                options_.layers |= bit;
            else
                options_.layers &= ~bit;
            Refresh();
        });
    };
    layer("地形", 1);
    layer("水", 2);
    layer("装饰物 / 可破坏物", 8);
    layer("只画可玩区域", 128);
    layer("荒芜", 0x400);
    layer("通行", 4);
    layer("区域", 0x20);
    menu->addSeparator();
    for (auto [name, field] : {std::pair{"单位点", &QtProject::MinimapPreviewOptions::units},
                               {"起始位置点", &QtProject::MinimapPreviewOptions::starts},
                               {"图标点", &QtProject::MinimapPreviewOptions::icons}}) {
        auto* action = menu->addAction(tr(name));
        action->setCheckable(true);
        action->setToolTip(tr("点状叠加显示"));
        connect(action, &QAction::toggled, this, [this, field](bool on) {
            options_.*field = on;
            Refresh();
        });
    }
    refresh_ = new QPushButton(tr("刷新"), this);
    refresh_->setObjectName("worldMinimapRefresh");
    refresh_->setCursor(Qt::PointingHandCursor);
    row->addWidget(refresh_);
    column->addLayout(row);
    image_ = new MinimapCanvas(this);
    image_->setObjectName("worldMinimapImage");
    image_->setAlignment(Qt::AlignCenter);
    image_->setMinimumSize(128, 128);
    image_->setMaximumHeight(160);
    column->addWidget(image_);
    image_->pan = [this](QPointF point) {
        Pan(point);
    };
    state_ = new QLabel(this);
    state_->setObjectName("worldMinimapState");
    column->addWidget(state_);
    connect(refresh_, &QPushButton::clicked, this, [this] {
        Refresh(true);
    });
    connect(source_, &QTabBar::currentChanged, this, [this](int index) {
        options_.current = index == 0;
        layers_->setEnabled(options_.current);
        Refresh();
    });
    connect(&preview_, &QtProject::ProjectMinimapPreview::Changed, this, [this] {
        Display();
    });
    if (viewport_)
        connect(viewport_, &WorldViewport::MinimapCameraChanged, this, [this] {
            image_->outline.clear();
            image_->update();
            QTimer::singleShot(100, this, [this] {
                Outline();
            });
        });
    QTimer::singleShot(0, this, [this] {
        Refresh();
    });
}
void WorldMinimapPreview::Refresh(bool force) {
    preview_.Request(options_, force);
}
void WorldMinimapPreview::Display() {
    const bool keepPrevious =
        preview_.Pending() && options_.current && displayedCurrent_ && !image_->pixmap().isNull() &&
        project_.archiveSession_ && displayedGeneration_ == project_.editRevision_.Generation() &&
        property("sourceSha256").toString().toStdString() == project_.archiveSession_->SourceSha256() &&
        !project_.sourceChangeSuspected_ && !project_.sourceChangeConfirmed_;
    if (!keepPrevious) {
        image_->clear();
        setProperty("sourceSha256", QString());
        setProperty("rgbaSha256", QString());
    }
    image_->outline.clear();
    setProperty("updating", preview_.Pending());
    const auto &result = preview_.ResultValue();
    QString state;
    if (!project_.archiveSession_ && !project_.mapEdits_)
        state = {};
    else if (project_.sourceChangeSuspected_ || project_.sourceChangeConfirmed_)
        state = tr("地图文件已变化");
    else if (preview_.Pending())
        state = keepPrevious ? tr("更新中…") : tr("读取中…");
    else if (!result || !result->Succeeded()) {
        state = project_.sourceChangeSuspected_ || project_.sourceChangeConfirmed_ ? tr("地图文件已变化")
                                                                                   : tr("当前不可用");
    } else {
        const auto &value = *result->value;
        if (!value.present)
            state = tr("没有小地图");
        else {
            const QImage image(reinterpret_cast<const uchar *>(value.image.rgba.data()), int(value.image.width),
                               int(value.image.height), QImage::Format_RGBA8888);
            image_->setPixmap(
                QPixmap::fromImage(image.copy()).scaled(160, 160, Qt::KeepAspectRatio, Qt::SmoothTransformation));
            if (!options_.current && (project_.HasPendingCandidateEdits() || project_.HasPendingAssetEdits() ||
                                      (project_.mapEdits_ && project_.mapEdits_->IsDirty())))
                state = tr("有未保存修改");
        }
        setProperty("sourceSha256", QString::fromStdString(value.sourceSha256));
        setProperty("rgbaSha256", QString::fromStdString(value.rgbaSha256));
        setProperty("documentRevision", qulonglong(value.documentRevision));
        setProperty("generationMilliseconds", value.generationMilliseconds);
        displayedGeneration_ = project_.editRevision_.Generation();
        displayedCurrent_ = value.current;
    }
    state_->setText(state);
    state_->setVisible(!state.isEmpty());
    Outline();
}
void WorldMinimapPreview::Pan(QPointF normalized) {
    const auto &result = preview_.ResultValue();
    if (!viewport_ || !options_.current || preview_.Pending() || !result || !result->Succeeded())
        return;
    const auto &value = *result->value;
    const auto &p = value.pixelRectangle;
    const auto &w = value.worldRectangle;
    const double px = normalized.x() * 256, py = (1 - normalized.y()) * 256;
    if (px < p[0] || px >= p[2] || py < p[1] || py >= p[3] || p[2] <= p[0] || p[3] <= p[1])
        return;
    auto camera = viewport_->ReadCamera(viewport_->DocumentRevision());
    if (!camera.Succeeded())
        return;
    camera.value->targetX = float(w[0] + (px - p[0]) / (p[2] - p[0]) * (w[2] - w[0]));
    camera.value->targetY = float(w[1] + (py - p[1]) / (p[3] - p[1]) * (w[3] - w[1]));
    const auto status = viewport_->SetCamera(*camera.value, viewport_->DocumentRevision());
    if (!status.Succeeded())
        project_.AddDiagnostics(status.diagnostics);
}
void WorldMinimapPreview::Outline() {
    image_->outline.clear();
    const auto &result = preview_.ResultValue();
    if (viewport_ && options_.current && !preview_.Pending() && result && result->Succeeded()) {
        const auto &value = *result->value;
        const auto &w = value.worldRectangle;
        const auto &p = value.pixelRectangle;
        if (w[2] > w[0] && w[3] > w[1])
            for (const auto &point : viewport_->MinimapCameraOutline())
                image_->outline << QPointF((p[0] + (point[0] - w[0]) / (w[2] - w[0]) * (p[2] - p[0])) / 256,
                                           1 - (p[1] + (point[1] - w[1]) / (w[3] - w[1]) * (p[3] - p[1])) / 256);
    }
    image_->update();
}
} // namespace War3Map::Modules
