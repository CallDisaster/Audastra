#include "ComposerWidgets.h"

#include "Icons/SvgIcons.h"
#include "Theme/ThemeManager.h"

#include <QEnterEvent>
#include <QEvent>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QKeyEvent>
#include <QLocale>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace War3Map::Agent {
namespace {

// 用量接近上限时的警示色（与圆环相同阈值）；未到阈值返回 normal。
QColor ContextWarningColor(const UiKit::Tokens::ThemeTokens& theme, double fraction, const QColor& normal) {
    if (fraction >= 0.9) return UiKit::ToQColor(theme.colors.red);
    if (fraction >= 0.75) return UiKit::ToQColor(theme.colors.amber);
    return normal;
}

} // namespace

ContextSegmentColors ResolveContextSegmentColors(const UiKit::Tokens::ThemeTokens& theme) {
    const auto& c = theme.colors;
    const QColor input = UiKit::ToQColor(c.blue);
    const QColor base = UiKit::ToQColor(c.bg);
    // 缓存命中是输入的一部分：取输入色与主题底色的不透明混合，深浅主题下都比输入色更淡且同色系。
    const QColor cache = QColor::fromRgbF(input.redF() * 0.55f + base.redF() * 0.45f,
        input.greenF() * 0.55f + base.greenF() * 0.45f, input.blueF() * 0.55f + base.blueF() * 0.45f);
    return {input, cache, UiKit::ToQColor(c.violet), UiKit::ToQColor(c.scrollbar)};
}

void ScheduleThemeRetint(const QEvent* event, QObject* context, bool& queued, std::function<void()> retint) {
    if (event == nullptr || context == nullptr || !retint) return;
    if (event->type() != QEvent::PaletteChange && event->type() != QEvent::StyleChange) return;
    if (queued) return;
    queued = true;
    QTimer::singleShot(0, context, [&queued, retint = std::move(retint)] {
        queued = false;
        retint();
    });
}

QString FormatTokens(qint64 tokens) {
    if (tokens < 1000) return QString::number(std::max<qint64>(0, tokens));
    const double k = static_cast<double>(tokens) / 1000.0;
    if (k < 100.0) return QStringLiteral("%1K").arg(k, 0, 'f', k == std::floor(k) ? 0 : 1);
    if (tokens >= 1000000) {
        const double m = static_cast<double>(tokens) / 1000000.0;
        return QStringLiteral("%1M").arg(m, 0, 'f', m == std::floor(m) ? 0 : 1);
    }
    return QStringLiteral("%1K").arg(std::llround(k));
}

ContextRing::ContextRing(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("agentContextRing"));
    setFixedSize(28, 28);
    setAttribute(Qt::WA_Hover);
    setAccessibleName(QStringLiteral("上下文用量"));
    setFocusPolicy(Qt::StrongFocus);
    setCursor(Qt::PointingHandCursor);
    UpdateToolTip();
}

QSize ContextRing::sizeHint() const { return {28, 28}; }

void ContextRing::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && rect().contains(event->position().toPoint())) {
        pressed_ = true; setFocus(Qt::MouseFocusReason); event->accept(); return;
    }
    QWidget::mousePressEvent(event);
}

void ContextRing::mouseReleaseEvent(QMouseEvent* event) {
    const bool activated = pressed_ && event->button() == Qt::LeftButton && rect().contains(event->position().toPoint());
    pressed_ = false;
    if (activated) {
        emit Activated(); event->accept(); return;
    }
    QWidget::mouseReleaseEvent(event);
}

void ContextRing::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter || event->key() == Qt::Key_Space) {
        emit Activated(); event->accept(); return;
    }
    QWidget::keyPressEvent(event);
}

void ContextRing::SetLimit(qint64 tokens, bool estimated, const QString& source) {
    limit_ = std::max<qint64>(0, tokens);
    estimated_ = estimated;
    source_ = source;
    UpdateToolTip();
    update();
}

void ContextRing::SetUsed(qint64 tokens) {
    used_ = std::max<qint64>(0, tokens);
    UpdateToolTip();
    update();
}

void ContextRing::ClearUsed() {
    used_ = -1;
    UpdateToolTip();
    update();
}

double ContextRing::Fraction() const noexcept {
    if (used_ < 0 || limit_ <= 0) return 0.0;
    return std::clamp(static_cast<double>(used_) / static_cast<double>(limit_), 0.0, 1.0);
}

void ContextRing::UpdateToolTip() {
    QString text;
    if (limit_ <= 0) text = QStringLiteral("尚未选择模型");
    else if (used_ < 0) text = QStringLiteral("上下文上限 %1%2\n本会话尚无用量").arg(FormatTokens(limit_),
        estimated_ ? QStringLiteral("（估算）") : QString());
    else text = QStringLiteral("已用 %1 / %2%3 · %4%").arg(FormatTokens(used_), FormatTokens(limit_),
        estimated_ ? QStringLiteral("（估算上限）") : QString()).arg(std::lround(Fraction() * 100.0));
    if (!source_.isEmpty() && limit_ > 0) text += QStringLiteral("\n上限来源：%1").arg(source_);
    setToolTip(text);
}

void ContextRing::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QPalette colors = palette();
    const qreal stroke = 2.5;
    const qreal diameter = 15.0;
    const QRectF ring((width() - diameter) / 2.0, (height() - diameter) / 2.0, diameter, diameter);
    if (underMouse()) {
        painter.setPen(Qt::NoPen);
        QColor hover = colors.color(QPalette::Text);
        hover.setAlpha(22);
        painter.setBrush(hover);
        painter.drawEllipse(rect().adjusted(1, 1, -1, -1));
    }
    QColor track = colors.color(QPalette::Text);
    track.setAlpha(isEnabled() ? 62 : 32);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(track, stroke, Qt::SolidLine, Qt::FlatCap));
    painter.drawEllipse(ring);
    if (hasFocus()) {
        QColor focus = colors.color(QPalette::Highlight);
        focus.setAlpha(170);
        painter.setPen(QPen(focus, 1.0));
        painter.drawEllipse(rect().adjusted(2, 2, -2, -2));
    }
    const double fraction = Fraction();
    if (fraction <= 0.0) return;
    const auto& theme = UiKit::ThemeManager::ThemeForWidget(this);
    const QColor used = ContextWarningColor(theme, fraction, UiKit::ToQColor(theme.colors.accent));
    painter.setPen(QPen(used, stroke, Qt::SolidLine, Qt::RoundCap));
    // Qt 角度单位为 1/16 度；从 12 点钟方向顺时针。
    painter.drawArc(ring, 90 * 16, -static_cast<int>(std::lround(fraction * 360.0 * 16.0)));
}

ContextSegmentBar::ContextSegmentBar(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("agentContextSegmentBar"));
    setFixedHeight(7);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

QSize ContextSegmentBar::sizeHint() const { return {180, 7}; }

void ContextSegmentBar::SetBreakdown(qint64 limit, qint64 used, qint64 uncachedInput, qint64 cachedRead, qint64 output) {
    limit_ = std::max<qint64>(0, limit);
    used_ = used;
    uncachedInput_ = std::max<qint64>(0, uncachedInput);
    cachedRead_ = std::max<qint64>(0, cachedRead);
    output_ = std::max<qint64>(0, output);
    update();
}

void ContextSegmentBar::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF bar = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    if (bar.width() <= 2.0 || bar.height() <= 2.0) return;
    const qreal radius = bar.height() / 2.0;

    QPainterPath clip;
    clip.addRoundedRect(bar, radius, radius);
    painter.setClipPath(clip);

    // 底槽与各分段的颜色和检查栏图例取自同一份令牌配色。
    const auto& theme = UiKit::ThemeManager::ThemeForWidget(this);
    const ContextSegmentColors colors = ResolveContextSegmentColors(theme);
    painter.fillRect(bar, colors.remaining);

    if (limit_ <= 0 || used_ <= 0) return;

    const double totalCapacity = static_cast<double>(limit_);
    const qint64 classified = uncachedInput_ + cachedRead_ + output_;
    // 提供商只给总量（或分项不足总量）时，剩下的已用量单独画成未分类段，不算到任何一项里。
    const qint64 unclassified = std::max<qint64>(0, used_ - classified);
    const qint64 drawn = classified + unclassified;
    const double scale = drawn > limit_ ? totalCapacity / static_cast<double>(drawn) : 1.0;
    qreal cursor = bar.left();

    const auto drawPart = [&](qint64 tokens, const QColor& color) {
        const qint64 scaled = static_cast<qint64>(std::llround(static_cast<double>(tokens) * scale));
        if (scaled <= 0 || cursor >= bar.right()) return;
        const qreal segWidth = std::min(bar.right() - cursor,
            std::max<qreal>(2.0, static_cast<qreal>(static_cast<double>(scaled) / totalCapacity * bar.width())));
        painter.fillRect(QRectF(cursor, bar.top(), segWidth, bar.height()), color);
        cursor += segWidth;
    };

    drawPart(uncachedInput_, colors.input);
    drawPart(cachedRead_, colors.cache);
    drawPart(output_, colors.output);
    QColor rest = UiKit::ToQColor(theme.colors.muted);
    if (classified == 0) rest = ContextWarningColor(theme, static_cast<double>(used_) / totalCapacity, rest);
    drawPart(unclassified, rest);
}

PermissionOption::PermissionOption(const QIcon& icon, const QString& title, const QString& description, QWidget* parent)
    : QWidget(parent) {
    setObjectName(QStringLiteral("agentPermissionOption"));
    setAttribute(Qt::WA_Hover);
    setCursor(Qt::PointingHandCursor);
    setAccessibleName(title);
    // 单行：图标、标题、勾选三者垂直居中；说明只放悬停提示，菜单本身不堆小字。
    setToolTip(description);
    setAccessibleDescription(description);
    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(14, 0, 14, 0);
    row->setSpacing(10);
    icon_ = new QLabel(this);
    icon_->setPixmap(icon.pixmap(15, 15));
    icon_->setFixedSize(16, 16);
    icon_->setAlignment(Qt::AlignCenter);
    row->addWidget(icon_, 0, Qt::AlignVCenter);
    title_ = new QLabel(title, this);
    title_->setObjectName(QStringLiteral("agentPermissionTitle"));
    row->addWidget(title_, 1, Qt::AlignVCenter);
    check_ = new QLabel(this);
    check_->setFixedSize(16, 16);
    UpdateCheckIcon();
    check_->setVisible(false);
    QSizePolicy keep = check_->sizePolicy(); keep.setRetainSizeWhenHidden(true); check_->setSizePolicy(keep);
    row->addWidget(check_, 0, Qt::AlignVCenter);
    setFixedHeight(36);
    setMinimumWidth(200);
}

void PermissionOption::SetChecked(bool checked) {
    checked_ = checked;
    check_->setVisible(checked);
    update();
}

void PermissionOption::SetWarning(bool warning) {
    title_->setProperty("warning", warning);
}

void PermissionOption::UpdateCheckIcon() {
    if (check_ == nullptr) return;
    check_->setPixmap(UiKit::SvgIcon(QStringLiteral("lucide/check"),
        UiKit::ToQColor(UiKit::ThemeManager::ThemeForWidget(this).colors.accent)).pixmap(15, 15));
}

void PermissionOption::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    // 主题切换后按新令牌重新着色勾选图标。
    ScheduleThemeRetint(event, this, retintQueued_, [this] { UpdateCheckIcon(); });
}

void PermissionOption::enterEvent(QEnterEvent* event) { hovered_ = true; update(); QWidget::enterEvent(event); }
void PermissionOption::leaveEvent(QEvent* event) { hovered_ = false; update(); QWidget::leaveEvent(event); }

void PermissionOption::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && rect().contains(event->position().toPoint())) emit Activated();
    QWidget::mouseReleaseEvent(event);
}

void PermissionOption::paintEvent(QPaintEvent*) {
    if (!hovered_ && !checked_) return;
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    QColor fill = palette().color(QPalette::Text);
    fill.setAlpha(hovered_ ? 20 : 10);
    painter.setPen(Qt::NoPen);
    painter.setBrush(fill);
    painter.drawRoundedRect(QRectF(rect()).adjusted(5, 2, -5, -2), 8, 8);
}

} // namespace War3Map::Agent
