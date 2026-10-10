#include "Widgets/ElidedLabel.h"

#include <QHelpEvent>
#include <QPainter>
#include <QStyle>
#include <QStyleOption>
#include <QToolTip>

namespace War3Map::UiKit {

ElidedLabel::ElidedLabel(QWidget* parent) : ElidedLabel(QString{}, parent) {}

ElidedLabel::ElidedLabel(const QString& text, QWidget* parent) : QLabel(text, parent) {
    setWordWrap(false);
    setTextFormat(Qt::PlainText);
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
}

QSize ElidedLabel::minimumSizeHint() const {
    QSize size = QLabel::minimumSizeHint();
    size.setWidth(fontMetrics().horizontalAdvance(QStringLiteral("…")) + contentsMargins().left() + contentsMargins().right());
    return size;
}

bool ElidedLabel::IsElided() const {
    return fontMetrics().horizontalAdvance(text()) > contentsRect().width();
}

bool ElidedLabel::event(QEvent* event) {
    if (event->type() == QEvent::ToolTip && toolTip().isEmpty() && IsElided()) {
        QToolTip::showText(static_cast<QHelpEvent*>(event)->globalPos(), text(), this);
        return true;
    }
    return QLabel::event(event);
}

void ElidedLabel::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    QStyleOption option;
    option.initFrom(this);
    style()->drawPrimitive(QStyle::PE_Widget, &option, &painter, this);
    const QRect area = contentsRect();
    const QString shown = fontMetrics().elidedText(text(), Qt::ElideRight, area.width());
    painter.setPen(palette().color(isEnabled() ? QPalette::Active : QPalette::Disabled, foregroundRole()));
    painter.drawText(area, static_cast<int>(alignment() | Qt::TextSingleLine), shown);
}

} // namespace War3Map::UiKit
