#include "Widgets/PopoverSurface.h"

#include "Widgets/Motion.h"
#include "Widgets/RoundedPopups.h"

#include <QApplication>
#include <QEvent>
#include <QGuiApplication>
#include <QHideEvent>
#include <QKeyEvent>
#include <QLayout>
#include <QPainter>
#include <QPropertyAnimation>
#include <QScreen>
#include <QShowEvent>
#include <QVBoxLayout>

#include <algorithm>

namespace War3Map::UiKit {

PopoverSurface::PopoverSurface(QWidget* parent)
    : QFrame(parent, Qt::Popup | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint) {
    setObjectName(QStringLiteral("uikitPopoverSurface"));
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_NoSystemBackground);
    setAttribute(Qt::WA_NoMouseReplay);
    setAttribute(Qt::WA_OpaquePaintEvent, false);
    setAutoFillBackground(false);
    setFrameShape(QFrame::NoFrame);
    setFocusPolicy(Qt::StrongFocus);
    // 应用 QSS 的 QWidget 基础底色不能覆盖透明窗口；仅命中本窗口与默认内容。
    setProperty("forgePopoverSurface", true);
    setStyleSheet(QStringLiteral(
        "QFrame[forgePopoverSurface=\"true\"] { background-color:transparent; border:0; }"
        "QWidget[forgePopoverContent=\"true\"] { background-color:transparent; border:0; }"));
    auto* outer = new QVBoxLayout(this);
    const int margin = PopupShadowExtent() + PopupContentPadding();
    outer->setContentsMargins(margin, margin, margin, margin);
    outer->setSpacing(0);
    content_ = new QWidget(this);
    content_->setObjectName(QStringLiteral("uikitPopoverContent"));
    content_->setProperty("forgePopoverContent", true);
    outer->addWidget(content_);
    fade_ = new QPropertyAnimation(this, "windowOpacity", this);
    fade_->setDuration(120);
    fade_->setEasingCurve(QEasingCurve::OutCubic);
    // 子内容取得焦点时仍由浮层处理 Esc。
    if (qApp) qApp->installEventFilter(this);
}

PopoverSurface::~PopoverSurface() {
    if (anchor_) anchor_->removeEventFilter(this);
}

void PopoverSurface::SetContentWidget(QWidget* content) {
    if (!content || content == content_) return;
    content->setParent(this);
    content->setAutoFillBackground(false);
    content->setProperty("forgePopoverContent", true);
    delete layout()->replaceWidget(content_, content);
    delete content_;
    content_ = content;
    if (isVisible()) PositionAtAnchor();
}

QRect PopoverSurface::AnchoredGeometry(const QRect& anchor, QSize size, const QRect& available,
    PreferredSide side, Qt::Alignment alignment) {
    if (available.isEmpty()) return {anchor.bottomLeft(), size};
    size.setWidth(std::clamp(size.width(), 1, available.width()));
    size.setHeight(std::clamp(size.height(), 1, available.height()));
    const int extent = PopupShadowExtent();
    const int gap = 4;
    const int below = anchor.bottom() + 1 + gap - extent;
    const int above = anchor.top() - gap + extent - size.height();
    int y = side == PreferredSide::Below ? below : above;
    const bool fitsBelow = below + size.height() - 1 <= available.bottom();
    const bool fitsAbove = above >= available.top();
    if (side == PreferredSide::Below && !fitsBelow && fitsAbove) y = above;
    if (side == PreferredSide::Above && !fitsAbove && fitsBelow) y = below;
    int x = anchor.left() - extent;
    if (alignment.testFlag(Qt::AlignRight)) x = anchor.right() + 1 + extent - size.width();
    else if (alignment.testFlag(Qt::AlignHCenter)) x = anchor.center().x() - size.width() / 2;
    x = std::clamp(x, available.left(), available.right() - size.width() + 1);
    y = std::clamp(y, available.top(), available.bottom() - size.height() + 1);
    return {{x, y}, size};
}

void PopoverSurface::PositionAtAnchor() {
    if (!anchor_) return;
    const QRect target(anchor_->mapToGlobal(QPoint()), anchor_->size());
    QScreen* screen = QGuiApplication::screenAt(target.center());
    if (!screen) screen = anchor_->screen();
    if (!screen) screen = QGuiApplication::primaryScreen();
    if (!screen) return;
    const QRect available = screen->availableGeometry();
    if (layout()) layout()->activate();
    QSize desired = sizeHint().expandedTo(minimumSizeHint()).expandedTo(minimumSize());
    if (!desired.isValid() || desired.isEmpty()) desired = size();
    setGeometry(AnchoredGeometry(target, desired, available, side_, alignment_));
}

void PopoverSurface::ShowAnchored(QWidget* anchor, PreferredSide side, Qt::Alignment alignment) {
    if (!anchor) return;
    if (anchor_ != anchor) {
        if (anchor_) anchor_->removeEventFilter(this);
        anchor_ = anchor;
        anchorWindow_ = anchor->window();
        anchor_->installEventFilter(this);
    }
    side_ = side;
    alignment_ = alignment;
    previousFocus_ = QApplication::focusWidget();
    PositionAtAnchor();
    show();
    raise();
    if (QWidget* first = content_->nextInFocusChain(); first && first != this && isAncestorOf(first)
        && first->focusPolicy() != Qt::NoFocus) first->setFocus(Qt::PopupFocusReason);
    else setFocus(Qt::PopupFocusReason);
}

void PopoverSurface::ToggleAnchored(QWidget* anchor, PreferredSide side, Qt::Alignment alignment) {
    if (isVisible() && anchor_ == anchor) Dismiss();
    else ShowAnchored(anchor, side, alignment);
}

void PopoverSurface::SetReducedMotion(bool reduced) {
    reducedMotion_ = reduced;
    if (reduced) {
        fade_->stop();
        setWindowOpacity(1.0);
    }
}

void PopoverSurface::BeginFade() {
    fade_->stop();
    if (reducedMotion_ || !AnimationsEnabled()) { setWindowOpacity(1.0); return; }
    fade_->setStartValue(0.0);
    fade_->setEndValue(1.0);
    fade_->start();
}

void PopoverSurface::Dismiss() { hide(); }

bool PopoverSurface::eventFilter(QObject* watched, QEvent* event) {
    if (watched == this && isVisible() && event->type() == QEvent::LayoutRequest) PositionAtAnchor();
    if (isVisible() && event->type() == QEvent::MouseButtonPress) {
        auto* widget = qobject_cast<QWidget*>(watched);
        if (widget && widget != this && !isAncestorOf(widget)) {
            const bool wasAnchor = widget == anchor_ || (anchor_ && anchor_->isAncestorOf(widget));
            Dismiss();
            // 再点锚点只收起，避免 Popup 的点击重放把它立即重新打开。
            if (wasAnchor) return true;
        }
    }
    if (isVisible() && event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        auto* widget = qobject_cast<QWidget*>(watched);
        if (key->key() == Qt::Key_Escape && widget && (widget == this || isAncestorOf(widget))) {
            Dismiss();
            key->accept();
            return true;
        }
    }
    if ((watched == anchor_ || watched == anchorWindow_) && isVisible()) {
        if (event->type() == QEvent::Hide || event->type() == QEvent::Close) Dismiss();
        else if (event->type() == QEvent::Move || event->type() == QEvent::Resize) PositionAtAnchor();
    }
    return QFrame::eventFilter(watched, event);
}

void PopoverSurface::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) { Dismiss(); event->accept(); return; }
    QFrame::keyPressEvent(event);
}

void PopoverSurface::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    PaintRoundedSurface(painter, QRectF(rect()), palette());
}

void PopoverSurface::hideEvent(QHideEvent* event) {
    fade_->stop();
    setWindowOpacity(1.0);
    QFrame::hideEvent(event);
    if (previousFocus_ && previousFocus_->isVisible()) previousFocus_->setFocus(Qt::PopupFocusReason);
    previousFocus_.clear();
    emit dismissed();
}

void PopoverSurface::showEvent(QShowEvent* event) {
    QFrame::showEvent(event);
    BeginFade();
}

} // namespace War3Map::UiKit
