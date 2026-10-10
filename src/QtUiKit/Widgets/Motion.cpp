#include "Widgets/Motion.h"

#include <QEvent>
#include <QGraphicsOpacityEffect>
#include <QPropertyAnimation>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace War3Map::UiKit {

bool AnimationsEnabled() {
#ifdef Q_OS_WIN
    BOOL enabled = TRUE;
    if (SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0)) return enabled != FALSE;
#endif
    return true;
}

void FadeIn(QWidget* widget, int durationMs, qreal from) {
    if (!widget) return;
    constexpr auto kName = "uikitFadeIn";
    if (auto* running = widget->findChild<QPropertyAnimation*>(QLatin1String(kName), Qt::FindDirectChildrenOnly)) {
        running->stop();
        running->deleteLater();
    }
    if (!AnimationsEnabled() || !widget->isVisible() || durationMs <= 0) {
        widget->setGraphicsEffect(nullptr);
        return;
    }
    auto* effect = new QGraphicsOpacityEffect(widget);
    effect->setOpacity(from);
    widget->setGraphicsEffect(effect); // 取代并删除旧效果
    auto* animation = new QPropertyAnimation(effect, "opacity", widget);
    animation->setObjectName(QLatin1String(kName));
    animation->setDuration(durationMs);
    animation->setStartValue(from);
    animation->setEndValue(1.0);
    animation->setEasingCurve(QEasingCurve::OutCubic);
    QObject::connect(animation, &QPropertyAnimation::finished, widget, [widget, effect] {
        if (widget->graphicsEffect() == effect) widget->setGraphicsEffect(nullptr);
    });
    animation->start(QAbstractAnimation::DeleteWhenStopped);
}

SlidingHighlight::SlidingHighlight(QWidget* parent) : QFrame(parent) {
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setFocusPolicy(Qt::NoFocus);
    animation_ = new QPropertyAnimation(this, "geometry", this);
    animation_->setDuration(180);
    animation_->setEasingCurve(QEasingCurve::OutCubic);
    hide();
}

void SlidingHighlight::Track(QWidget* target, bool animate) {
    if (target_ != target) {
        if (target_) target_->removeEventFilter(this);
        target_ = target;
        if (target_) target_->installEventFilter(this);
    }
    Follow(animate);
}

void SlidingHighlight::Follow(bool animate) {
    animation_->stop();
    if (!target_ || !target_->isVisible()) { hide(); return; }
    const QRect goal = target_->geometry();
    const bool wasShown = isVisible();
    show();
    lower();
    if (!animate || !wasShown || !AnimationsEnabled() || geometry().isEmpty()) {
        setGeometry(goal);
        return;
    }
    animation_->setStartValue(geometry());
    animation_->setEndValue(goal);
    animation_->start();
}

bool SlidingHighlight::eventFilter(QObject* watched, QEvent* event) {
    if (watched == target_) {
        switch (event->type()) {
        case QEvent::Move:
        case QEvent::Resize:
            // 布局调整（窗口缩放、字体变化）时直接贴合，不播放滑动。
            if (animation_->state() != QAbstractAnimation::Running) Follow(false);
            else animation_->setEndValue(target_->geometry());
            break;
        case QEvent::Show:
        case QEvent::Hide:
            Follow(false);
            break;
        default:
            break;
        }
    }
    return QFrame::eventFilter(watched, event);
}

} // namespace War3Map::UiKit
