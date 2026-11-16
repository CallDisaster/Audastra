#include "Widgets/TablePopupAnchor.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QGuiApplication>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QPersistentModelIndex>
#include <QPointer>
#include <QScreen>
#include <QScrollBar>
#include <QTimer>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <vector>

namespace War3Map::UiKit {
namespace {
// 一侧的候选位置：贴着锚点，另一方向按锚点对齐后夹进工作区。
QRect Candidate(PopupSide side, const QRect& anchor, const QSize& size, const QRect& available, int gap) {
    const int x = std::clamp(anchor.left(), available.left(), std::max(available.left(), available.right() - size.width() + 1));
    const int y = std::clamp(anchor.top(), available.top(), std::max(available.top(), available.bottom() - size.height() + 1));
    switch (side) {
    case PopupSide::Right: return {QPoint(anchor.right() + 1 + gap, y), size};
    case PopupSide::Left: return {QPoint(anchor.left() - gap - size.width(), y), size};
    case PopupSide::Below: return {QPoint(x, anchor.bottom() + 1 + gap), size};
    case PopupSide::Above: return {QPoint(x, anchor.top() - gap - size.height()), size};
    }
    return {QPoint(x, y), size};
}

// 浮层所在屏幕：锚点中心所在的屏幕，取不到时退回锚点控件的屏幕。
QScreen* ScreenFor(const QWidget& widget, const QRect& global) {
    if (QScreen* screen = QGuiApplication::screenAt(global.center())) return screen;
    if (QScreen* screen = widget.screen()) return screen;
    return QGuiApplication::primaryScreen();
}

// 缩放取整可能差一两个像素；只把明显的尺寸变化当作面板自己改了大小，避免反复放置。
bool ChangedBySelf(const QSize& now, const QSize& placed) {
    return std::abs(now.width() - placed.width()) > 2 || std::abs(now.height() - placed.height()) > 2;
}

class TablePopupAnchor final : public QObject {
public:
    TablePopupAnchor(QWidget* panel, QAbstractItemView* view, const QModelIndex& index, QWidget* cellAnchor)
        : QObject(panel), panel_(panel), view_(view), cellAnchor_(cellAnchor), index_(index), preferred_(panel->size()) {
        qApp->installEventFilter(this);
        connect(view->verticalScrollBar(), &QScrollBar::valueChanged, this, [this] { Reposition(); });
        connect(view->horizontalScrollBar(), &QScrollBar::valueChanged, this, [this] { Reposition(); });
        connect(view->selectionModel(), &QItemSelectionModel::currentChanged, this, [this](const QModelIndex& current) {
            if (current != index_) panel_->close();
        });
        connect(view->model(), &QAbstractItemModel::modelAboutToBeReset, panel, &QWidget::close);
        QTimer::singleShot(0, this, [this] { Reposition(); });
    }
    ~TablePopupAnchor() override { if (qApp) qApp->removeEventFilter(this); }
    bool eventFilter(QObject* object, QEvent* event) override {
        if (!panel_->isVisible()) return false;
        if (!view_) { panel_->close(); return false; }
        // 面板显示后会按内容（如模型预览）自己长高；记下新尺寸重新放置，不让底部按钮越出工作区。
        if (object == panel_ && event->type() == QEvent::Resize && ChangedBySelf(panel_->size(), placed_)) {
            preferred_ = panel_->size();
            QTimer::singleShot(0, this, [this] { Reposition(); });
        }
        auto* widget = qobject_cast<QWidget*>(object);
        if (event->type() == QEvent::KeyPress && static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape
            && widget && (widget == panel_ || panel_->isAncestorOf(widget) || widget == view_ || view_->isAncestorOf(widget))) {
            panel_->close(); return true;
        }
        // 面板内的下拉、菜单是独立顶层窗口，isAncestorOf 不跨窗口；沿父链判断，避免选下拉项就关掉整个面板。
        if (event->type() == QEvent::MouseButtonPress && widget && !OwnedByPanel(widget)) panel_->close();
        if ((object == view_ || object == view_->viewport() || object == view_->window())
            && (event->type() == QEvent::Move || event->type() == QEvent::Resize || event->type() == QEvent::Hide)) Reposition();
        return false;
    }
private:
    bool OwnedByPanel(const QWidget* widget) const {
        for (auto* current = widget; current; current = current->parentWidget())
            if (current == panel_) return true;
        return false;
    }
    void Reposition() {
        if (!view_ || !index_.isValid() || !view_->isVisible()) { panel_->close(); return; }
        const auto local = view_->visualRect(index_);
        if (local.isEmpty() || !view_->viewport()->rect().intersects(local)) { panel_->close(); return; }
        if (cellAnchor_) cellAnchor_->setGeometry(local);
        const QRect cell(view_->viewport()->mapToGlobal(local.topLeft()), local.size());
        QScreen* screen = ScreenFor(*view_, cell);
        if (!screen) return;
        static constexpr std::array kSides{PopupSide::Right, PopupSide::Left, PopupSide::Below, PopupSide::Above};
        const QRect geometry = PlaceAnchoredPopup(cell, preferred_, panel_->minimumSize(), screen->availableGeometry(), kSides);
        placed_ = geometry.size();  // 先记下：放置引起的改尺寸事件不当作面板自己长高
        panel_->setGeometry(geometry);
        placed_ = panel_->size();
    }
    QWidget* panel_;
    QPointer<QAbstractItemView> view_;
    QPointer<QWidget> cellAnchor_;
    QPersistentModelIndex index_;
    QSize preferred_;
    QSize placed_;
};

class FieldPopupAnchor final : public QObject {
public:
    FieldPopupAnchor(QWidget* panel, QWidget* anchor) : QObject(panel), panel_(panel), anchor_(anchor), preferred_(panel->size()) {
        // 锚点到所在窗口的整条父链：窗口拖动、改大小，滚动区域移动内容，布局重排，都会落到其中某一层的 Move/Resize。
        for (QWidget* widget = anchor; widget; widget = widget->isWindow() ? nullptr : widget->parentWidget()) {
            widget->installEventFilter(this);
            watched_.emplace_back(widget);
        }
        panel->installEventFilter(this);
        connect(anchor, &QObject::destroyed, panel, &QWidget::close);
        Reposition();
        QTimer::singleShot(0, this, [this] { Reposition(); });
    }
    ~FieldPopupAnchor() override {
        for (const auto& widget : watched_)
            if (widget) widget->removeEventFilter(this);
    }
    bool eventFilter(QObject* object, QEvent* event) override {
        if (object == panel_) {
            // 面板显示后会按内容（如模型预览）自己长高；记下新尺寸重新放置，不让底部按钮越出工作区。
            if ((event->type() == QEvent::Resize && ChangedBySelf(panel_->size(), placed_)) || event->type() == QEvent::Show) {
                if (event->type() == QEvent::Resize) preferred_ = panel_->size();
                QTimer::singleShot(0, this, [this] { Reposition(); });
            }
            return false;
        }
        switch (event->type()) {
        case QEvent::Move:
        case QEvent::Resize:
            if (panel_->isVisible()) Reposition();
            break;
        case QEvent::Hide:
        case QEvent::Close:
            // 父链上一层隐藏时锚点随之不可见；窗口关闭时面板一起关。
            QTimer::singleShot(0, panel_, [panel = QPointer<QWidget>(panel_), anchor = anchor_] {
                if (panel && (!anchor || !anchor->isVisible())) panel->close();
            });
            break;
        default:
            break;
        }
        return false;
    }
private:
    void Reposition() {
        if (!anchor_) { panel_->close(); return; }
        if (!anchor_->isVisible()) return;
        const QRect target(anchor_->mapToGlobal(QPoint()), anchor_->size());
        QScreen* screen = ScreenFor(*anchor_, target);
        if (!screen) return;
        const QRect available = screen->availableGeometry();
        // 锚点在上半屏时下方空间多，先试下方；在下半屏时先试上方。
        static constexpr std::array kDown{PopupSide::Below, PopupSide::Above};
        static constexpr std::array kUp{PopupSide::Above, PopupSide::Below};
        const bool upper = target.center().y() < available.center().y();
        const QRect geometry = PlaceAnchoredPopup(target, preferred_, panel_->minimumSize(), available,
            upper ? std::span<const PopupSide>(kDown) : std::span<const PopupSide>(kUp), 4);
        placed_ = geometry.size();  // 先记下：放置引起的改尺寸事件不当作面板自己长高
        if (geometry != panel_->geometry()) panel_->setGeometry(geometry);
        placed_ = panel_->size();
    }
    QWidget* panel_;
    QPointer<QWidget> anchor_;
    std::vector<QPointer<QWidget>> watched_;
    QSize preferred_;
    QSize placed_;
};
}

QRect PlaceAnchoredPopup(const QRect& anchor, QSize preferred, QSize minimum, const QRect& available,
    std::span<const PopupSide> sides, int gap) {
    if (available.isEmpty()) return {anchor.bottomLeft(), preferred};
    minimum = minimum.boundedTo(available.size());
    const QSize size = preferred.expandedTo(minimum).boundedTo(available.size());
    for (const PopupSide side : sides) {
        const QRect candidate = Candidate(side, anchor, size, available, gap);
        if (available.contains(candidate)) return candidate;
    }
    // 极小工作区没有完整可用的一侧时，取锚点上/下较大的一侧并压低高度，不压到锚点。
    const int above = std::max(0, anchor.top() - gap - available.top());
    const int below = std::max(0, available.bottom() - anchor.bottom() - gap);
    const int height = std::clamp(std::max(above, below), minimum.height(), size.height());
    const QSize shrunk(size.width(), height);
    // 面板最小尺寸放不进锚点上下任一侧时，宁可盖住锚点，也要整块留在工作区内。
    const int top = std::clamp(above >= below ? anchor.top() - gap - height : anchor.bottom() + 1 + gap,
        available.top(), std::max(available.top(), available.bottom() - height + 1));
    const int left = std::clamp(anchor.left(), available.left(), std::max(available.left(), available.right() - shrunk.width() + 1));
    return {QPoint(left, top), shrunk};
}

void AnchorTablePopup(QWidget* panel, QWidget* anchor, const QModelIndex& index, QWidget* cellAnchor) {
    for (auto* parent = anchor; parent; parent = parent->parentWidget())
        if (auto* view = qobject_cast<QAbstractItemView*>(parent)) { new TablePopupAnchor(panel, view, index, cellAnchor); return; }
}

void AnchorFieldPopup(QWidget* panel, QWidget* anchor) {
    if (panel && anchor) new FieldPopupAnchor(panel, anchor);
}
}
