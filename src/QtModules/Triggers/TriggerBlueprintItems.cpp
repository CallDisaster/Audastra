#include "TriggerBlueprintItems.h"

#include <QTextBlock>
#include <QAbstractTextDocumentLayout>
#include <QGraphicsSceneHoverEvent>
#include <QGraphicsTextItem>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QTextDocument>
#include <QWidget>

#include <algorithm>
#include <cmath>

namespace TriggerBlueprintDetail {

namespace {

constexpr double kPinRadius = 6;
constexpr double kPinHit = 11; // 命中区比画出来的端口大，便于拖拽

} // namespace

QPainterPath ExecWire(const QPointF& from, const QPointF& leaveDirection, const QPointF& to, const QPointF& arriveDirection) {
    const QPointF delta = to - from;
    const double bend = std::clamp(std::hypot(delta.x(), delta.y()) / 2, 24.0, 120.0);
    QPainterPath path(from);
    path.cubicTo(from + leaveDirection * bend, to - arriveDirection * bend, to);
    return path;
}

BlueprintPin::BlueprintPin(BlueprintNode* owner, PinKind kind, QString label, QColor color, PinTarget target)
    : QGraphicsItem(owner), owner_(owner), kind_(kind), label_(std::move(label)), color_(color), target_(std::move(target)) {
    setAcceptHoverEvents(true);
    setCursor(Qt::CrossCursor);
    setZValue(2);
    setToolTip(kind_ == PinKind::In
        ? QObject::tr("输入：拖到其他节点的输出端口把这条语句移过去；拖到空白处在它之前新建语句")
        : QObject::tr("输出：拖到其他语句把它接到这里；拖到空白处或单击在这里新建语句"));
}

QRectF BlueprintPin::boundingRect() const {
    return {-kPinHit, -kPinHit, kPinHit * 2, kPinHit * 2};
}

QPointF BlueprintPin::Direction() const {
    return kind_ == PinKind::Branch || kind_ == PinKind::Out ? QPointF(1, 0) : QPointF(0, 1);
}

void BlueprintPin::SetConnected(bool connected) {
    connected_ = connected;
    update();
}

void BlueprintPin::paint(QPainter* painter, const QStyleOptionGraphicsItem*, QWidget* widget) {
    const QPalette palette = widget != nullptr ? widget->palette() : QPalette{};
    painter->setRenderHint(QPainter::Antialiasing);
    const double radius = hovered_ ? kPinRadius + 1.5 : kPinRadius;
    // 执行端口画成向执行方向的三角（虚幻蓝图的执行引脚）；已连接实心，未连接空心。
    QPainterPath arrow;
    if (Direction().x() > 0) {
        arrow.moveTo(-radius, -radius);
        arrow.lineTo(radius, 0);
        arrow.lineTo(-radius, radius);
    } else {
        arrow.moveTo(-radius, -radius * 0.8);
        arrow.lineTo(radius, -radius * 0.8);
        arrow.lineTo(0, radius);
    }
    arrow.closeSubpath();
    painter->setPen(QPen(color_, 1.6));
    painter->setBrush(connected_ || hovered_ ? QBrush(color_) : QBrush(palette.color(QPalette::Base)));
    painter->drawPath(arrow);
}

void BlueprintPin::hoverEnterEvent(QGraphicsSceneHoverEvent* event) {
    hovered_ = true;
    update();
    QGraphicsItem::hoverEnterEvent(event);
}

void BlueprintPin::hoverLeaveEvent(QGraphicsSceneHoverEvent* event) {
    hovered_ = false;
    update();
    QGraphicsItem::hoverLeaveEvent(event);
}

BlueprintNode::BlueprintNode(War3Map::StableId id, std::int32_t ecaType, QString title, QString kind, QColor color, bool enabled)
    : id_(id), ecaType_(ecaType), title_(std::move(title)), kind_(std::move(kind)), color_(color), enabled_(enabled) {
    setFlag(ItemIsSelectable);
    setAcceptHoverEvents(true);
    body_ = new QGraphicsTextItem(this);
    body_->setTextWidth(kNodeWidth - 16);
    body_->setPos(8, kNodeHeader + 2);
    if (!enabled_) setOpacity(0.55);
}

void BlueprintNode::SetBody(const QString& html, const QColor& text) {
    prepareGeometryChange();
    body_->setDefaultTextColor(text);
    body_->setHtml(html);
    bodyHeight_ = body_->boundingRect().height();
    LayoutPins();
}

void BlueprintNode::AddImage(const QUrl& url, const QImage& image) {
    body_->document()->addResource(QTextDocument::ImageResource, url, image);
}

void BlueprintNode::SetArgumentTooltip(std::size_t position, QString tooltip) {
    argumentTooltips_.insert(QStringLiteral("arg:%1").arg(position), std::move(tooltip));
}

BlueprintPin* BlueprintNode::AddPin(PinKind kind, const QString& label, const QColor& color, const PinTarget& target) {
    prepareGeometryChange();
    auto* pin = new BlueprintPin(this, kind, label, color, target);
    pins_.push_back(pin);
    LayoutPins();
    return pin;
}

BlueprintPin* BlueprintNode::FindPin(PinKind kind, const QString& label) const {
    for (BlueprintPin* pin : pins_) {
        if (pin->Kind() == kind && (label.isEmpty() || pin->Label() == label)) return pin;
    }
    return nullptr;
}

// 端口位置：输入在顶边中央，下一步在底边中央，输出在标题行右侧，分支在正文下方逐行靠右。
void BlueprintNode::LayoutPins() {
    double row = kNodeHeader + bodyHeight_ + 4;
    for (BlueprintPin* pin : pins_) {
        if (pin->Kind() == PinKind::Branch) {
            pin->setPos(kNodeWidth, row + kBranchRow / 2);
            row += kBranchRow;
        }
    }
    const double height = row + 4;
    for (BlueprintPin* pin : pins_) {
        switch (pin->Kind()) {
        case PinKind::In: pin->setPos(kNodeWidth / 2, 0); break;
        case PinKind::Next: pin->setPos(kNodeWidth / 2, height); break;
        case PinKind::Out: pin->setPos(kNodeWidth, kNodeHeader / 2); break;
        case PinKind::Branch: break;
        }
    }
}

QRectF BlueprintNode::boundingRect() const {
    double branches = 0;
    for (const BlueprintPin* pin : pins_) {
        if (pin->Kind() == PinKind::Branch) branches += kBranchRow;
    }
    return {0, 0, kNodeWidth, kNodeHeader + bodyHeight_ + 8 + branches};
}

void BlueprintNode::paint(QPainter* painter, const QStyleOptionGraphicsItem*, QWidget* widget) {
    const QPalette palette = widget != nullptr ? widget->palette() : QPalette{};
    const QRectF bounds = boundingRect();
    painter->setRenderHint(QPainter::Antialiasing);

    const QRectF card = bounds.adjusted(0.5, 0.5, -0.5, -0.5);
    const QColor surface = palette.color(QPalette::AlternateBase);
    const QColor border = isSelected() ? palette.color(QPalette::Accent) : palette.color(QPalette::Mid);
    painter->setPen(QPen(border, isSelected() ? 1.8 : 1.0));
    painter->setBrush(surface);
    painter->drawRoundedRect(card, 8, 8);

    // 顶部标题带：左深右浅渐变，只圆上方两角，底部留细分隔线。
    QPainterPath header;
    const QRectF headerRect(card.left() + 0.5, card.top() + 0.5, card.width() - 1.0, kNodeHeader);
    header.moveTo(headerRect.left(), headerRect.bottom());
    header.lineTo(headerRect.left(), headerRect.top() + 7.5);
    header.quadTo(headerRect.left(), headerRect.top(), headerRect.left() + 7.5, headerRect.top());
    header.lineTo(headerRect.right() - 7.5, headerRect.top());
    header.quadTo(headerRect.right(), headerRect.top(), headerRect.right(), headerRect.top() + 7.5);
    header.lineTo(headerRect.right(), headerRect.bottom());
    header.closeSubpath();

    QLinearGradient band(headerRect.topLeft(), headerRect.topRight());
    QColor bandStart = color_;
    bandStart.setAlpha(isSelected() ? 72 : 52);
    QColor bandEnd = color_;
    bandEnd.setAlpha(isSelected() ? 28 : 14);
    band.setColorAt(0.0, bandStart);
    band.setColorAt(1.0, bandEnd);
    painter->fillPath(header, band);

    QColor headerDivider = color_;
    headerDivider.setAlpha(55);
    painter->setPen(QPen(headerDivider, 1.0));
    painter->drawLine(QPointF(card.left() + 1.0, headerRect.bottom()), QPointF(card.right() - 1.0, headerRect.bottom()));

    QFont titleFont = painter->font();
    titleFont.setWeight(QFont::Medium);
    painter->save();
    painter->setFont(titleFont);
    painter->setPen(color_);
    painter->drawText(QRectF(11, 1, kNodeWidth - 92, kNodeHeader), Qt::AlignVCenter | Qt::AlignLeft, title_);
    painter->restore();

    painter->setPen(palette.color(QPalette::PlaceholderText));
    painter->drawText(QRectF(kNodeWidth - 90, 1, 74, kNodeHeader), Qt::AlignVCenter | Qt::AlignRight,
        enabled_ ? kind_ : QObject::tr("已禁用"));

    // 分支行：分隔线 + 右对齐的分支名（端口画在右边缘上）。
    double row = kNodeHeader + bodyHeight_ + 4;
    for (const BlueprintPin* pin : pins_) {
        if (pin->Kind() != PinKind::Branch) continue;
        QColor line = palette.color(QPalette::Mid);
        line.setAlpha(95);
        painter->setPen(line);
        painter->drawLine(QPointF(10, row), QPointF(kNodeWidth - 10, row));
        painter->setPen(pin->Color());
        painter->drawText(QRectF(10, row, kNodeWidth - 26, kBranchRow), Qt::AlignVCenter | Qt::AlignRight, pin->Label());
        row += kBranchRow;
    }
}

QString BlueprintNode::AnchorAt(const QPointF& scenePos) const {
    const QPointF local = body_->mapFromScene(scenePos);
    return body_->document()->documentLayout()->anchorAt(local);
}

QString BlueprintNode::Text() const {
    return body_->toPlainText();
}


QRectF BlueprintNode::ArgumentRect(std::size_t position) const {
    const QString anchor = QStringLiteral("arg:") + QString::number(position);
    QAbstractTextDocumentLayout* layout = body_->document()->documentLayout();
    const QSizeF size = body_->document()->size();
    for (qreal y = 1; y < size.height(); y += 4) {
        for (qreal x = 1; x < size.width(); x += 4) {
            const QPointF local(x, y);
            if (layout->anchorAt(local) != anchor) continue;
            return mapRectToScene(body_->mapRectToParent(QRectF(local, QSizeF(8, 8))));
        }
    }
    return {};
}
void BlueprintNode::hoverMoveEvent(QGraphicsSceneHoverEvent* event) {
    const auto anchor = AnchorAt(event->scenePos());
    setCursor(anchor.isEmpty() ? Qt::ArrowCursor : Qt::PointingHandCursor);
    if (id_ != 0) setToolTip(argumentTooltips_.value(anchor));
}

} // namespace TriggerBlueprintDetail
