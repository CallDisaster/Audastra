#include "Widgets/ToolPopupDismissal.h"

#include <QApplication>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QTimer>
#include <QWidget>
#include <QWindow>

namespace War3Map::UiKit {

ToolPopupDismissal::ToolPopupDismissal(QWidget& window, std::function<void(Reason)> dismiss)
    : QObject(&window), window_(&window), dismiss_(std::move(dismiss)) {
    window.installEventFilter(this);
}

ToolPopupDismissal::~ToolPopupDismissal() { StopWatching(); }

void ToolPopupDismissal::Open(QWidget* focus) {
    if (!window_) return;
    ++openGeneration_;
    StartWatching();
    window_->show();
    window_->raise();
    window_->activateWindow();
    (focus != nullptr ? focus : window_.data())->setFocus(Qt::PopupFocusReason);
}

void ToolPopupDismissal::StartWatching() {
    composing_ = false;
    if (watching_) return;
    qApp->installEventFilter(this);
    watching_ = true;
}

void ToolPopupDismissal::StopWatching() {
    composing_ = false;
    if (!watching_) return;
    qApp->removeEventFilter(this);
    watching_ = false;
}

bool ToolPopupDismissal::Inside(const QObject* object) const {
    // 跨窗口沿父控件向上找：浮层里下拉框、菜单的弹出窗口也属于浮层。
    for (auto* widget = qobject_cast<const QWidget*>(object); widget != nullptr; widget = widget->parentWidget()) {
        if (widget == window_) return true;
    }
    return false;
}

void ToolPopupDismissal::Dismiss(Reason reason) {
    if (!window_ || !window_->isVisible()) return;
    if (dismiss_) dismiss_(reason);
    else window_->hide();
}

bool ToolPopupDismissal::eventFilter(QObject* watched, QEvent* event) {
    if (!window_) return false;
    const auto type = event->type();
    if (watched == window_) {
        if (type == QEvent::Show) StartWatching();
        else if (type == QEvent::Hide) {
            // 同一事件会经过应用级和窗口级两次过滤，只处理第一次。
            const bool wasWatching = watching_;
            StopWatching();
            // 收起的是焦点窗口（Esc、选中后关闭）时把激活交还所属窗口；点到别的窗口时那个窗口已先激活，不抢。
            // 按底层焦点窗口判断：隐藏时 Qt 可能已先清空 activeWindow，但焦点窗口仍是浮层。
            const QWindow* handle = window_->windowHandle();
            const bool focused = QApplication::activeWindow() == window_ || (handle && QGuiApplication::focusWindow() == handle);
            if (wasWatching && focused && QApplication::applicationState() == Qt::ApplicationActive) {
                if (QWidget* owner = window_->parentWidget()) owner->window()->activateWindow();
            }
        }
    }
    if (!watching_ || !window_->isVisible()) return false;
    switch (type) {
    case QEvent::MouseButtonPress:
    case QEvent::NonClientAreaMouseButtonPress: {
        // 只按控件判断；同一次点击先送到原生窗口对象，再送到控件。
        auto* widget = qobject_cast<QWidget*>(watched);
        if (widget == nullptr || Inside(widget)) break;
        const QPoint global = static_cast<QMouseEvent*>(event)->globalPosition().toPoint();
        if (window_->frameGeometry().contains(global)) break;
        if (auto* popup = QApplication::activePopupWidget(); popup && Inside(popup) && popup->frameGeometry().contains(global)) break;
        const bool onToggleAnchor = toggleAnchor_ && (widget == toggleAnchor_ || toggleAnchor_->isAncestorOf(widget));
        Dismiss(Reason::OutsideClick);
        // 其他外部点击不吞：点到的控件照常响应；点开关控件只收起。
        if (onToggleAnchor) return true;
        break;
    }
    case QEvent::KeyPress:
        if (static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape && !composing_ && Inside(watched)) {
            Dismiss(Reason::Escape);
            return true;
        }
        break;
    case QEvent::InputMethod:
        if (Inside(watched)) composing_ = !static_cast<QInputMethodEvent*>(event)->preeditString().isEmpty();
        break;
    case QEvent::ApplicationStateChange:
        if (watched != qApp || static_cast<QApplicationStateChangeEvent*>(event)->applicationState() == Qt::ApplicationActive) break;
        // 输入法候选窗可能短暂抢走激活；回到事件循环后应用仍失活且不在组字才收起。
        // 期间重新打开过的浮层不受上一次失活影响。
        QTimer::singleShot(0, this, [this, generation = openGeneration_] {
            if (generation != openGeneration_ || !window_ || !window_->isVisible() || composing_) return;
            if (QApplication::applicationState() != Qt::ApplicationActive) Dismiss(Reason::Deactivated);
        });
        break;
    default:
        break;
    }
    return false;
}

} // namespace War3Map::UiKit
