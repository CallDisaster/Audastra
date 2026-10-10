#include "Dock/DockHost.h"

#include "Dock/FloatFrame.h"
#include "Dock/ModuleDrag.h"
#include "Dock/SideGroup.h"
#include "Icons/SvgIcons.h"

#include <QDragEnterEvent>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QMimeData>
#include <QScreen>
#include <QSplitter>
#include <QStackedWidget>
#include <QStyle>
#include <QVBoxLayout>

#include <algorithm>

namespace War3Map::Shell {
namespace {

constexpr int kEdgeZoneWidth = 112;

Bounds ScreenBoundsAt(QPoint point) {
    QScreen* screen = QGuiApplication::screenAt(point);
    if (screen == nullptr) {
        screen = QGuiApplication::primaryScreen();
    }
    const QRect area = screen != nullptr ? screen->availableGeometry() : QRect(0, 0, 1920, 1080);
    return {area.x(), area.y(), area.width(), area.height()};
}

void Repolish(QWidget* widget) {
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
}

} // namespace

DockHost::DockHost(const ModuleRegistry& registry, QWidget* owner)
    : QWidget(owner),
      registry_(registry),
      owner_(owner) {
    setObjectName(QStringLiteral("dockHost"));
    setAcceptDrops(true);
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    splitter_ = new QSplitter(Qt::Horizontal, this);
    splitter_->setObjectName(QStringLiteral("dockSplitter"));
    splitter_->setChildrenCollapsible(false);
    main_ = new QStackedWidget(splitter_);
    main_->setObjectName(QStringLiteral("shellMain"));
    side_ = new SideGroup(splitter_);
    side_->setMinimumWidth(kMinSideWidth);
    splitter_->addWidget(main_);
    splitter_->addWidget(side_);
    splitter_->setStretchFactor(0, 1);
    splitter_->setStretchFactor(1, 0);
    side_->hide();
    layout->addWidget(splitter_);

    // 右边缘停靠区：只在拖动模块且右侧组为空时出现。
    edgeZone_ = new QFrame(this);
    edgeZone_->setObjectName(QStringLiteral("edgeDropZone"));
    auto* zoneLayout = new QVBoxLayout(edgeZone_);
    auto* zoneLabel = new QLabel(tr("停靠到右侧"), edgeZone_);
    zoneLabel->setAlignment(Qt::AlignCenter);
    zoneLabel->setWordWrap(true);
    zoneLayout->addWidget(zoneLabel);
    edgeZone_->hide();

    connect(splitter_, &QSplitter::splitterMoved, this, [this]() {
        if (!applying_ && side_->isVisible()) {
            sideWidth_ = side_->width();
            emit SideWidthEdited(sideWidth_);
        }
    });
    connect(side_, &SideGroup::ActivateRequested, this, [this](const ModuleId& id) { emit ActivateRequested(id, false); });
    connect(side_, &SideGroup::DetachRequested, this, [this](const ModuleId& id, QPoint pos) {
        emit PlaceRequested(id, Placement::Float(pos.x() - 120, pos.y() - 16));
    });
    connect(side_, &SideGroup::MenuRequested, this, &DockHost::MenuRequested);
    connect(side_, &SideGroup::ModuleDropped, this, [this](const ModuleId& id, int index) {
        emit PlaceRequested(id, Placement::Side(index));
    });
}

DockHost::~DockHost() {
    ReleaseModuleWidgets();
}

void DockHost::ReleaseModuleWidgets() {
    // 浮动窗口归属主窗口；先取回模块控件，避免随窗口或停靠区一起析构。
    for (auto& [id, frame] : frames_) {
        (void)id;
        (void)frame->TakeContent();
        delete frame;
    }
    frames_.clear();
    for (const auto& id : created_) {
        if (EditorModule* module = registry_.Find(id)) {
            QWidget* widget = module->Widget();
            widget->hide();
            widget->setParent(nullptr);
        }
    }
    created_.clear();
}

QWidget* DockHost::WidgetFor(const ModuleId& id) {
    EditorModule* module = registry_.Find(id);
    if (module == nullptr || !module->Info().available) {
        return nullptr;
    }
    created_.insert(id);
    return module->Widget();
}

FloatFrame* DockHost::Frame(const ModuleId& id) const {
    const auto found = frames_.find(id);
    return found == frames_.end() ? nullptr : found->second;
}

FloatFrame* DockHost::EnsureFrame(const ModuleId& id) {
    if (auto* frame = Frame(id)) {
        return frame;
    }
    auto* frame = new FloatFrame(registry_.Find(id)->Info(), owner_);
    frames_.emplace(id, frame);
    connect(frame, &FloatFrame::GeometryEdited, this, &DockHost::FloatGeometryEdited);
    connect(frame, &FloatFrame::Activated, this, [this](const ModuleId& module) { emit ActivateRequested(module, false); });
    connect(frame, &FloatFrame::DockRequested, this, [this](const ModuleId& module, Area area) {
        emit PlaceRequested(module, area == Area::Side ? Placement::Side() : Placement::Main());
    });
    connect(frame, &FloatFrame::MoveTracking, this, [this](const ModuleId&, QPoint pos) {
        const auto target = DockTargetAt(pos);
        ShowDropHint(target ? std::optional<Area>(target->area) : std::nullopt);
    });
    connect(frame, &FloatFrame::MoveFinished, this, [this](const ModuleId& module, QPoint pos) {
        ShowDropHint(std::nullopt);
        if (const auto target = DockTargetAt(pos)) {
            emit PlaceRequested(module, *target);
        }
    });
    return frame;
}

void DockHost::Apply(const DockLayout& layout) {
    const DockLayout available = Sanitize(layout, registry_.AvailableIds());
    applying_ = true;
    PlaceFloats(available);
    PlaceSide(available);
    PlaceMain(available);
    applying_ = false;
}

void DockHost::PlaceFloats(const DockLayout& layout) {
    // 不再浮动的窗口：取回控件（稍后由右侧组或主区接收），销毁窗口。
    for (auto it = frames_.begin(); it != frames_.end();) {
        const bool stillFloating = std::any_of(layout.floats.begin(), layout.floats.end(),
            [&](const FloatWindow& window) { return window.id == it->first; });
        if (stillFloating) {
            ++it;
            continue;
        }
        QWidget* content = it->second->TakeContent();
        if (content != nullptr) {
            content->hide();
            content->setParent(main_);
        }
        it->second->hide();
        it->second->deleteLater();
        it = frames_.erase(it);
    }
    for (const auto& window : layout.floats) {
        QWidget* content = WidgetFor(window.id);
        if (content == nullptr) {
            continue;
        }
        FloatFrame* frame = EnsureFrame(window.id);
        if (main_->indexOf(content) >= 0) {
            main_->removeWidget(content);
        }
        frame->SetContent(content);
        const FloatWindow clamped = window.maximized
            ? window
            : ClampFloat(window, ScreenBoundsAt(QPoint(window.x + window.w / 2, window.y + 16)));
        frame->ApplyGeometry(clamped);
    }
    // 前后次序：数组最后一个在最前。
    for (const auto& window : layout.floats) {
        if (auto* frame = Frame(window.id)) {
            frame->raise();
        }
    }
}

void DockHost::PlaceSide(const DockLayout& layout) {
    std::vector<ModuleId> ids;
    std::vector<QWidget*> widgets;
    std::vector<QString> labels;
    std::vector<QIcon> icons;
    for (const auto& id : layout.side) {
        QWidget* widget = WidgetFor(id);
        if (widget == nullptr) {
            continue;
        }
        if (main_->indexOf(widget) >= 0) {
            main_->removeWidget(widget);
        }
        const ModuleInfo info = registry_.Find(id)->Info();
        ids.push_back(id);
        widgets.push_back(widget);
        labels.push_back(info.label);
        icons.push_back(UiKit::SvgIcon(info.icon));
    }
    side_->SetModules(ids, widgets, labels, icons, layout.sideActive);
    const bool visible = !ids.empty();
    side_->setVisible(visible);
    sideWidth_ = layout.sideWidth;
    if (visible) {
        const int total = splitter_->width();
        const int sideWidth = std::clamp(sideWidth_, kMinSideWidth, std::max(kMinSideWidth, total - 360));
        splitter_->setSizes({std::max(1, total - sideWidth), sideWidth});
    }
}

void DockHost::PlaceMain(const DockLayout& layout) {
    const auto all = registry_.AvailableIds();
    const auto shown = MainShown(layout, all);
    for (const auto& id : MainModules(layout, all)) {
        // 未显示过的主区模块不创建控件（首次切换时才创建）。
        if (!created_.contains(id) && !(shown && *shown == id)) {
            continue;
        }
        QWidget* widget = WidgetFor(id);
        if (widget != nullptr && main_->indexOf(widget) < 0) {
            main_->addWidget(widget);
        }
    }
    if (shown) {
        if (QWidget* widget = WidgetFor(*shown)) {
            main_->setCurrentWidget(widget);
        }
    }
}

QRect DockHost::EdgeZoneRect() const {
    return QRect(width() - kEdgeZoneWidth, 0, kEdgeZoneWidth, height());
}

void DockHost::PositionEdgeZone() {
    edgeZone_->setGeometry(EdgeZoneRect().adjusted(6, 6, -6, -6));
    edgeZone_->raise();
}

std::optional<Placement> DockHost::DockTargetAt(QPoint globalPos) const {
    if (header_ != nullptr && header_->isVisible() &&
        header_->rect().contains(header_->mapFromGlobal(globalPos))) {
        return Placement::Main();
    }
    if (side_->isVisible() && side_->rect().contains(side_->mapFromGlobal(globalPos))) {
        return Placement::Side(side_->DropIndex(globalPos));
    }
    if (EdgeZoneRect().contains(mapFromGlobal(globalPos))) {
        return Placement::Side();
    }
    return std::nullopt;
}

void DockHost::ShowDropHint(std::optional<Area> area) {
    // 右侧组为空时显示右边缘区域；否则高亮右侧组本身。
    const bool side = area && *area == Area::Side;
    edgeZone_->setVisible(side && !side_->isVisible());
    if (edgeZone_->isVisible()) {
        PositionEdgeZone();
    }
    if (side_->property("dropTarget").toBool() != (side && side_->isVisible())) {
        side_->setProperty("dropTarget", side && side_->isVisible());
        Repolish(side_);
    }
    if (header_ != nullptr && header_->property("dropTarget").toBool() != (area && *area == Area::Main)) {
        header_->setProperty("dropTarget", area && *area == Area::Main);
        Repolish(header_);
    }
}

void DockHost::dragEnterEvent(QDragEnterEvent* event) {
    if (ModuleFromMime(event->mimeData())) {
        event->accept();
        if (!side_->isVisible()) {
            edgeZone_->show();
            PositionEdgeZone();
        }
    }
}

void DockHost::dragMoveEvent(QDragMoveEvent* event) {
    // 只有右边缘区域接收；主编辑区上释放 = 在该处浮动（由拖动发起方处理）。
    const bool inZone = EdgeZoneRect().contains(event->position().toPoint());
    edgeZone_->setProperty("active", inZone);
    Repolish(edgeZone_);
    if (inZone) {
        event->acceptProposedAction();
    } else {
        event->ignore();
    }
}

void DockHost::dragLeaveEvent(QDragLeaveEvent* event) {
    edgeZone_->hide();
    QWidget::dragLeaveEvent(event);
}

void DockHost::dropEvent(QDropEvent* event) {
    edgeZone_->hide();
    const auto id = ModuleFromMime(event->mimeData());
    if (id && EdgeZoneRect().contains(event->position().toPoint())) {
        event->acceptProposedAction();
        emit PlaceRequested(*id, Placement::Side());
        return;
    }
    event->ignore();
}

void DockHost::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    if (edgeZone_->isVisible()) {
        PositionEdgeZone();
    }
}

} // namespace War3Map::Shell
