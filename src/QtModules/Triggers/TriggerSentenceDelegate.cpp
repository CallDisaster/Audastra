#include "TriggerSentenceDelegate.h"

#include "TriggerModels.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QMouseEvent>
#include <QHelpEvent>
#include <QIcon>
#include <QPainter>
#include <QPainterPath>
#include <QToolTip>

#include <vector>

namespace {

struct Span {
    int begin = 0;
    int length = 0;
    std::size_t position = 0;
    bool empty = false;
    bool call = false;
    QIcon icon;
    QString raw, issue, reference;
    bool badge = false;
    QStringList candidates;
};

std::vector<Span> SpansOf(const QModelIndex& index) {
    std::vector<Span> spans;
    const QVariantList list = index.data(TriggerEcaModel::ArgumentSpansRole).toList();
    spans.reserve(static_cast<std::size_t>(list.size()));
    for (const QVariant& value : list) {
        const QVariantMap map = value.toMap();
        spans.push_back({map["begin"].toInt(), map["length"].toInt(), map["position"].toULongLong(),
            map["empty"].toBool(), map["call"].toBool(), map["icon"].value<QIcon>(),
            map["raw"].toString(), map["issue"].toString(), map["reference"].toString(), map["badge"].toBool(), map["candidates"].toStringList()});
    }
    return spans;
}

// 空值记号的红色波浪线。
void WavyLine(QPainter* painter, int left, int right, int y) {
    QPainterPath path;
    path.moveTo(left, y);
    bool up = true;
    for (int x = left; x < right; x += 2) {
        path.lineTo(std::min(x + 2, right), up ? y - 2 : y);
        up = !up;
    }
    painter->drawPath(path);
}

} // namespace

struct TriggerSentenceDelegate::Layout {
    QRect textRect;
    QString text;
    std::vector<Span> spans;
    std::vector<QRect> tokenRects; // 与 spans 对齐
};

TriggerSentenceDelegate::TriggerSentenceDelegate(QObject* parent)
    : QStyledItemDelegate(parent) {
}

TriggerSentenceDelegate::Layout TriggerSentenceDelegate::Measure(
    const QStyleOptionViewItem& option, const QModelIndex& index) const {
    Layout layout;
    QStyleOptionViewItem opt = option;
    initStyleOption(&opt, index);
    const QWidget* widget = opt.widget;
    QStyle* style = widget != nullptr ? widget->style() : QApplication::style();
    layout.textRect = style->subElementRect(QStyle::SE_ItemViewItemText, &opt, widget)
        .adjusted(3, 0, -3, 0); // 与样式绘制文字的内边距一致
    layout.text = opt.text;
    if (index.column() != 0) {
        return layout;
    }
    layout.spans = SpansOf(index);
    const QFontMetrics metrics(opt.font);
    int x = layout.textRect.left();
    int cursor = 0;
    for (const Span& span : layout.spans) {
        if (span.begin > cursor) x += metrics.horizontalAdvance(layout.text.mid(cursor, span.begin - cursor));
        const int left = x;
        const int added = (span.icon.isNull() ? 0 : 20) + (span.badge ? 16 : 0);
        const QString part = span.length > 0 ? layout.text.mid(span.begin, span.length) : QStringLiteral("  ");
        const int width = metrics.horizontalAdvance(part) + added;
        layout.tokenRects.push_back(QRect(left, layout.textRect.top(), std::max(width, 6), layout.textRect.height()));
        x += width;
        cursor = span.begin + span.length;
    }
    return layout;
}

QSize TriggerSentenceDelegate::sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const {
    QSize size = QStyledItemDelegate::sizeHint(option, index);
    const QFontMetrics metrics(option.font);
    for (const auto& span : SpansOf(index)) {
        size.rwidth() += (span.icon.isNull() ? 0 : 20) + (span.badge ? 16 : 0)
            + (span.length == 0 ? metrics.horizontalAdvance(QStringLiteral("  ")) : 0);
    }
    return size;
}

void TriggerSentenceDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const {
    const Layout layout = Measure(option, index);
    if (layout.spans.empty()) {
        QStyledItemDelegate::paint(painter, option, index);
        return;
    }
    // 背景、选中与图标交给样式，文字自己画（逐段着色）。
    QStyleOptionViewItem opt = option;
    initStyleOption(&opt, index);
    opt.text.clear();
    const QWidget* widget = opt.widget;
    QStyle* style = widget != nullptr ? widget->style() : QApplication::style();
    style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, widget);

    painter->save();
    painter->setClipRect(layout.textRect);
    painter->setFont(opt.font);
    const bool selected = (opt.state & QStyle::State_Selected) != 0;
    const QColor base = index.data(Qt::ForegroundRole).canConvert<QColor>() && !selected
        ? index.data(Qt::ForegroundRole).value<QColor>()
        : opt.palette.color(selected ? QPalette::HighlightedText : QPalette::Text);
    const QColor token = selected ? opt.palette.color(QPalette::HighlightedText) : opt.palette.color(QPalette::Link);
    const QColor missing(0xe5, 0x53, 0x4b);
    const QFontMetrics metrics(opt.font);
    const int baseline = layout.textRect.top() + (layout.textRect.height() + metrics.ascent() - metrics.descent()) / 2;
    int cursor = 0;
    int x = layout.textRect.left();
    const auto draw = [&](const QString& part, const QColor& color) {
        painter->setPen(color);
        painter->drawText(QPoint(x, baseline), part);
        x += metrics.horizontalAdvance(part);
    };
    for (std::size_t i = 0; i < layout.spans.size(); ++i) {
        const Span& span = layout.spans[i];
        if (span.begin > cursor) draw(layout.text.mid(cursor, span.begin - cursor), base);
        if (!span.icon.isNull()) {
            span.icon.paint(painter, QRect(x, layout.textRect.center().y() - 8, 16, 16));
            x += 20;
        }
        const int left = x;
        const QString part = span.length > 0 ? layout.text.mid(span.begin, span.length) : QStringLiteral("  ");
        draw(part, span.empty ? missing : token);
        painter->setPen(QPen(span.empty ? missing : token, 1, span.call ? Qt::SolidLine : Qt::DotLine));
        if (span.empty) {
            WavyLine(painter, left, x, baseline + 3);
        } else {
            painter->drawLine(left, baseline + 2, x, baseline + 2);
        }
        if (span.badge) {
            const QRect badge(x + 2, layout.textRect.center().y() - 6, 12, 12);
            painter->setPen(QPen(token, 1));
            painter->drawRoundedRect(badge, 2, 2);
            painter->drawLine(badge.left() + 3, badge.top() + 4, badge.right() - 3, badge.top() + 4);
            painter->drawLine(badge.left() + 3, badge.top() + 8, badge.right() - 3, badge.top() + 8);
            x += 16;
        }
        cursor = span.begin + span.length;
    }
    if (cursor < layout.text.size()) draw(layout.text.mid(cursor), base);
    painter->restore();
}

bool TriggerSentenceDelegate::helpEvent(QHelpEvent* event, QAbstractItemView* view,
    const QStyleOptionViewItem& option, const QModelIndex& index) {
    if (event && view) {
        const auto layout = Measure(option, index);
        for (std::size_t i = 0; i < layout.spans.size(); ++i) {
            if (!layout.tokenRects[i].contains(event->pos())) continue;
            const auto& span = layout.spans[i];
            QString tooltip = tr("原值：%1").arg(span.raw);
            if (!span.reference.isEmpty()) tooltip += tr("\n来自地图字符串表：%1").arg(span.reference);
            if (!span.issue.isEmpty()) tooltip += QLatin1Char('\n') + span.issue;
            for (const auto& candidate : span.candidates) tooltip += QStringLiteral("\n• ") + candidate;
            QToolTip::showText(event->globalPos(), tooltip.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>")), view);
            return true;
        }
    }
    return QStyledItemDelegate::helpEvent(event, view, option, index);
}

std::optional<TriggerSentenceDelegate::Token> TriggerSentenceDelegate::TokenAt(
    const QStyleOptionViewItem& option, const QModelIndex& index, const QPoint& point) const {
    const Layout layout = Measure(option, index);
    for (std::size_t i = 0; i < layout.spans.size(); ++i) {
        if (layout.tokenRects[i].contains(point)) return Token{layout.spans[i].position, layout.tokenRects[i]};
    }
    return std::nullopt;
}

std::optional<TriggerSentenceDelegate::Token> TriggerSentenceDelegate::TokenByOrder(
    const QStyleOptionViewItem& option, const QModelIndex& index, std::size_t ordinal) const {
    const Layout layout = Measure(option, index);
    if (ordinal >= layout.spans.size()) {
        return std::nullopt;
    }
    return Token{layout.spans[ordinal].position, layout.tokenRects[ordinal]};
}

bool TriggerSentenceDelegate::editorEvent(QEvent* event, QAbstractItemModel* model,
    const QStyleOptionViewItem& option, const QModelIndex& index) {
    if (event->type() == QEvent::MouseButtonRelease && activate_) {
        auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() == Qt::LeftButton) {
            if (const auto token = TokenAt(option, index, mouse->position().toPoint())) {
                const auto* view = qobject_cast<const QAbstractItemView*>(option.widget);
                const QRect global = view != nullptr
                    ? QRect(view->viewport()->mapToGlobal(token->rect.topLeft()), token->rect.size())
                    : token->rect;
                activate_(index, token->position, global);
                return true;
            }
        }
    }
    return QStyledItemDelegate::editorEvent(event, model, option, index);
}
