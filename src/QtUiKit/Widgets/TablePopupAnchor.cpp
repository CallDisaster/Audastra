#include "Widgets/TablePopupAnchor.h"

#include <QAbstractItemView>
#include <QApplication>
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

namespace War3Map::UiKit {
namespace {
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
        if (object == panel_ && event->type() == QEvent::Resize && GrewBySelf()) {
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
        if (!view_->screen()) return;
        const auto available = view_->screen()->availableGeometry();
        const QRect cell(view_->viewport()->mapToGlobal(local.topLeft()), local.size());
        const auto size = preferred_.expandedTo(panel_->minimumSize()).boundedTo(available.size());
        const int x = std::clamp(cell.left(), available.left(), std::max(available.left(), available.right() - size.width() + 1));
        const int y = std::clamp(cell.top(), available.top(), std::max(available.top(), available.bottom() - size.height() + 1));
        const std::array<QPoint, 4> positions{{{cell.right() + 1, y}, {cell.left() - size.width(), y},
            {x, cell.bottom() + 1}, {x, cell.top() - size.height()}}};
        for (const auto& position : positions) if (available.contains(QRect(position, size))) {
            Place(size, position); return;
        }
        // 极小工作区没有完整可用侧面时，取字段上/下较大的一侧；不压到正在编辑的字段。
        const int above = std::max(0, cell.top() - available.top());
        const int below = std::max(0, available.bottom() - cell.bottom());
        const int height = std::min(size.height(), std::max(above, below));
        panel_->resize(size.width(), height);
        placed_ = panel_->size();
        panel_->move(x, above >= below ? cell.top() - panel_->height() : cell.bottom() + 1);
    }
    // 缩放取整可能差一两个像素；只把明显的尺寸变化当作面板自己改了大小，避免反复放置。
    bool GrewBySelf() const {
        const auto size = panel_->size();
        return std::abs(size.width() - placed_.width()) > 2 || std::abs(size.height() - placed_.height()) > 2;
    }
    void Place(const QSize& size, const QPoint& position) {
        panel_->resize(size);
        placed_ = panel_->size();
        panel_->move(position);
    }
    QWidget* panel_;
    QPointer<QAbstractItemView> view_;
    QPointer<QWidget> cellAnchor_;
    QPersistentModelIndex index_;
    QSize preferred_;
    QSize placed_;
};
}
void AnchorTablePopup(QWidget* panel, QWidget* anchor, const QModelIndex& index, QWidget* cellAnchor) {
    for (auto* parent = anchor; parent; parent = parent->parentWidget())
        if (auto* view = qobject_cast<QAbstractItemView*>(parent)) { new TablePopupAnchor(panel, view, index, cellAnchor); return; }
}
}
