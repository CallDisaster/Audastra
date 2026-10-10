#include "ComposerWidgets.h"

#include "Icons/SvgIcons.h"

#include <QEnterEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QKeyEvent>
#include <QLocale>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace War3Map::Agent {

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
    QColor used = colors.color(QPalette::Text);
    if (fraction >= 0.9) used = QColor(226, 142, 134);
    painter.setPen(QPen(used, stroke, Qt::SolidLine, Qt::RoundCap));
    // Qt 角度单位为 1/16 度；从 12 点钟方向顺时针。
    painter.drawArc(ring, 90 * 16, -static_cast<int>(std::lround(fraction * 360.0 * 16.0)));
}

PermissionOption::PermissionOption(const QIcon& icon, const QString& title, const QString& description, QWidget* parent)
    : QWidget(parent) {
    setObjectName(QStringLiteral("agentPermissionOption"));
    setAttribute(Qt::WA_Hover);
    setCursor(Qt::PointingHandCursor);
    setAccessibleName(title);
    // 单行：图标、标题、勾选三者垂直居中；说明只放悬停提示，菜单本身不堆小字。
    setToolTip(description);
    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(14, 0, 14, 0);
    row->setSpacing(10);
    icon_ = new QLabel(this);
    icon_->setPixmap(icon.pixmap(16, 16));
    icon_->setFixedSize(16, 16);
    icon_->setAlignment(Qt::AlignCenter);
    row->addWidget(icon_, 0, Qt::AlignVCenter);
    title_ = new QLabel(title, this);
    title_->setObjectName(QStringLiteral("agentPermissionTitle"));
    row->addWidget(title_, 1, Qt::AlignVCenter);
    check_ = new QLabel(this);
    check_->setFixedSize(16, 16);
    check_->setPixmap(UiKit::SvgIcon(QStringLiteral("lucide/check")).pixmap(16, 16));
    check_->setVisible(false);
    QSizePolicy keep = check_->sizePolicy(); keep.setRetainSizeWhenHidden(true); check_->setSizePolicy(keep);
    row->addWidget(check_, 0, Qt::AlignVCenter);
    setFixedHeight(36);
    setMinimumWidth(200);
}

void PermissionOption::SetChecked(bool checked) { check_->setVisible(checked); }

void PermissionOption::SetWarning(bool warning) {
    title_->setProperty("warning", warning);
}

void PermissionOption::enterEvent(QEnterEvent* event) { hovered_ = true; update(); QWidget::enterEvent(event); }
void PermissionOption::leaveEvent(QEvent* event) { hovered_ = false; update(); QWidget::leaveEvent(event); }

void PermissionOption::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && rect().contains(event->position().toPoint())) emit Activated();
    QWidget::mouseReleaseEvent(event);
}

void PermissionOption::paintEvent(QPaintEvent*) {
    if (!hovered_) return;
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    QColor hover = palette().color(QPalette::Text);
    hover.setAlpha(16);
    painter.setPen(Qt::NoPen);
    painter.setBrush(hover);
    painter.drawRoundedRect(QRectF(rect()).adjusted(4, 1, -4, -1), 8, 8);
}

} // namespace War3Map::Agent
