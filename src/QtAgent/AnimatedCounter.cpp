#include "AnimatedCounter.h"

#include "Fonts/FontLoader.h"
#include "Widgets/Motion.h"

#include <QLocale>

#include <algorithm>
#include <cmath>
#include <limits>

namespace War3Map::Agent {
namespace {

constexpr qint64 kDurationMs = 240;

} // namespace

AnimatedCounter::AnimatedCounter(QWidget* parent) : QLabel(parent) {
    setFont(UiKit::MonoFont());
    setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    timer_.setInterval(24);
    timer_.setTimerType(Qt::PreciseTimer);
    connect(&timer_, &QTimer::timeout, this, &AnimatedCounter::Advance);
    ReserveWidth(0);
    Display(0);
}

qint64 AnimatedCounter::Value() const noexcept { return value_; }
qint64 AnimatedCounter::TargetValue() const noexcept { return target_; }
bool AnimatedCounter::IsAnimating() const noexcept { return timer_.isActive(); }

void AnimatedCounter::SetTarget(qint64 target) {
    // 新目标从当前已显示值接续，不把旧动画的起点或终点重新播放。
    if (target == target_) return;
    target_ = target;
    ReserveWidth(target);
    if (reducedMotion_ || !UiKit::AnimationsEnabled()) {
        Finish();
        return;
    }
    elapsed_.restart();
    // token 高频到达时不能反复重启 tick，否则整个流式阶段都没有一次绘制。
    if (!timer_.isActive()) {
        frameElapsed_.start();
        timer_.start();
    }
}

void AnimatedCounter::SetValue(qint64 value) {
    timer_.stop();
    target_ = value;
    ReserveWidth(value);
    Display(value);
}

void AnimatedCounter::SetReducedMotion(bool reduced) {
    reducedMotion_ = reduced;
    if (reduced) Finish();
}

void AnimatedCounter::Finish() {
    timer_.stop();
    Display(target_);
}

void AnimatedCounter::SetMinimumDigits(int digits) {
    reservedDigits_ = std::clamp(digits, 1, 24);
    ReserveWidth(target_);
}

void AnimatedCounter::Advance() {
    if (!UiKit::AnimationsEnabled() || elapsed_.elapsed() >= kDurationMs) {
        Finish();
        return;
    }
    const qint64 frameMilliseconds = frameElapsed_.restart();
    const long double eased = 1.0L - std::exp(-static_cast<long double>(frameMilliseconds) / 70.0L);
    // 不用目标与当前值的整数差，避免接近 qint64 边界时有符号溢出。
    const long double next = static_cast<long double>(value_) * (1.0L - eased)
        + static_cast<long double>(target_) * eased;
    const qint64 candidate = next >= static_cast<long double>(std::numeric_limits<qint64>::max())
        ? std::numeric_limits<qint64>::max()
        : next <= static_cast<long double>(std::numeric_limits<qint64>::min())
        ? std::numeric_limits<qint64>::min() : static_cast<qint64>(next);
    Display(std::clamp(candidate, std::min(value_, target_), std::max(value_, target_)));
}

void AnimatedCounter::Display(qint64 value) {
    const bool changed = value_ != value;
    value_ = value;
    setText(QLocale().toString(value));
    setAccessibleName(QStringLiteral("数值 %1").arg(value));
    if (changed) emit ValueChanged(value);
}

void AnimatedCounter::ReserveWidth(qint64 value) {
    const QString formatted = QLocale().toString(value);
    reservedDigits_ = std::max(reservedDigits_, static_cast<int>(formatted.size()));
    // 字宽按目标增长一次；逐帧的个位/十位变化不触发布局抖动。
    setFixedWidth(fontMetrics().horizontalAdvance(QString(reservedDigits_, QLatin1Char('8'))) + 2);
}

} // namespace War3Map::Agent
