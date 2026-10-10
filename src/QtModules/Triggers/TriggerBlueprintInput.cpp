#include "TriggerBlueprint.h"

#include <QApplication>
#include <QContextMenuEvent>
#include <QGraphicsPathItem>
#include <QGraphicsScene>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>

#include <cmath>

// 蓝图交互：缩放、选择、参数记号、端口拉线（移动 / 新建语句）与右键菜单。
using namespace TriggerBlueprintDetail;

TriggerBlueprint::Node* TriggerBlueprint::NodeAt(const QPoint& viewPos) const {
    for (QGraphicsItem* item = itemAt(viewPos); item != nullptr; item = item->parentItem()) {
        if (auto* node = dynamic_cast<Node*>(item)) return node;
    }
    return nullptr;
}

TriggerBlueprint::Pin* TriggerBlueprint::PinAt(const QPoint& viewPos) const {
    for (QGraphicsItem* item : items(viewPos)) {
        if (auto* pin = dynamic_cast<Pin*>(item)) return pin;
    }
    return nullptr;
}

TriggerBlueprint::Node* TriggerBlueprint::ProbeNode(War3Map::StableId ecaId) const {
    if (ecaId == 0) return startNode_ != nullptr ? startNode_ : conditionNode_;
    const auto found = nodes_.find(ecaId);
    return found == nodes_.end() ? nullptr : found->second;
}

QRectF TriggerBlueprint::NodeRect(War3Map::StableId ecaId) const {
    const auto found = nodes_.find(ecaId);
    return found == nodes_.end() ? QRectF{} : found->second->sceneBoundingRect();
}

QString TriggerBlueprint::NodeText(War3Map::StableId ecaId) const {
    const auto found = nodes_.find(ecaId);
    return found == nodes_.end() ? QString{} : found->second->Text();
}

QPoint TriggerBlueprint::ArgumentViewPos(War3Map::StableId ecaId, std::size_t position) const {
    const auto found = nodes_.find(ecaId);
    if (found == nodes_.end()) return QPoint(-1, -1);
    const QRectF rect = found->second->ArgumentRect(position);
    return rect.isValid() ? mapFromScene(rect.center()) : QPoint(-1, -1);
}

QString TriggerBlueprint::ConditionText() const {
    return conditionNode_ == nullptr ? QString{} : conditionNode_->Text();
}

QStringList TriggerBlueprint::PinLabels(War3Map::StableId ecaId) const {
    QStringList labels;
    if (const Node* node = ProbeNode(ecaId)) {
        for (const Pin* pin : node->Pins()) labels << pin->Label();
    }
    return labels;
}

QPoint TriggerBlueprint::PinViewPos(War3Map::StableId ecaId, const QString& label) const {
    if (const Node* node = ProbeNode(ecaId)) {
        for (const Pin* pin : node->Pins()) {
            if (pin->Label() == label) return mapFromScene(pin->scenePos());
        }
    }
    return {-1, -1};
}

std::vector<TriggerBlueprint::PinTarget> TriggerBlueprint::RootTargets() const {
    return {{{0, std::nullopt, 0, QObject::tr("事件")}, 0, QObject::tr("事件")},
        {{0, std::nullopt, 1, QObject::tr("触发条件")}, 0, QObject::tr("触发条件")},
        {{0, std::nullopt, 2, QObject::tr("动作")}, 0, QObject::tr("动作")}};
}

std::vector<TriggerBlueprint::PinTarget> TriggerBlueprint::MenuTargets(War3Map::StableId ecaId) const {
    const Node* node = ProbeNode(ecaId);
    return node == nullptr ? RootTargets() : node->MenuTargets();
}

void TriggerBlueprint::wheelEvent(QWheelEvent* event) {
    ZoomBy(event->angleDelta().y() > 0 ? 1.1 : 1 / 1.1);
    event->accept();
}

void TriggerBlueprint::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_F && event->modifiers() == Qt::NoModifier) {
        Fit();
        return;
    }
    if (event->key() == Qt::Key_Escape) {
        if (dragPin_ != nullptr) {
            dragPin_ = nullptr;
            delete dragWire_;
            dragWire_ = nullptr;
            return;
        }
        scene_->clearSelection();
        return;
    }
    QGraphicsView::keyPressEvent(event);
}

// 右键：端口 → 只提供“在这里添加”；语句节点 → 添加位置 + 语句命令；空白处 → 添加事件 / 条件 / 动作。
void TriggerBlueprint::contextMenuEvent(QContextMenuEvent* event) {
    if (!callbacks_.contextMenu) {
        return;
    }
    if (Pin* pin = PinAt(event->pos())) {
        callbacks_.contextMenu(0, {pin->Target()}, event->globalPos());
        return;
    }
    Node* node = NodeAt(event->pos());
    if (node == nullptr) {
        callbacks_.contextMenu(0, RootTargets(), event->globalPos());
        return;
    }
    const std::vector<PinTarget> targets = node->MenuTargets();
    const War3Map::StableId id = node->Id();
    if (id != 0) {
        Select(id, false);
        if (callbacks_.selected) callbacks_.selected(id);
    }
    callbacks_.contextMenu(id, targets, event->globalPos());
}

void TriggerBlueprint::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        if (Pin* pin = PinAt(event->pos())) {
            // 从端口拉线：不进入画布拖动。
            dragPin_ = pin;
            dragStart_ = event->pos();
            QPen pen(pin->Color(), 2, Qt::DashLine);
            dragWire_ = scene_->addPath(QPainterPath(pin->scenePos()), pen);
            dragWire_->setZValue(3);
            event->accept();
            return;
        }
    }
    QGraphicsView::mousePressEvent(event);
}

void TriggerBlueprint::mouseMoveEvent(QMouseEvent* event) {
    if (dragPin_ != nullptr && dragWire_ != nullptr) {
        const QPointF end = mapToScene(event->pos());
        const QPointF from = dragPin_->scenePos();
        // 从输入端口反向拉线时，线从上方进入端口。
        dragWire_->setPath(dragPin_->IsOutput()
            ? ExecWire(from, dragPin_->Direction(), end, QPointF(0, 1))
            : ExecWire(end, QPointF(1, 0), from, QPointF(0, 1)));
        event->accept();
        return;
    }
    QGraphicsView::mouseMoveEvent(event);
}

// 松开：按起点与落点决定“移动已有语句”还是“新建语句”。回调会重建场景，先取出需要的值再调用。
void TriggerBlueprint::FinishPinDrag(const QPoint& viewPos) {
    Pin* source = dragPin_;
    dragPin_ = nullptr;
    delete dragWire_;
    dragWire_ = nullptr;
    if (source == nullptr) return;
    const bool clicked = (viewPos - dragStart_).manhattanLength() < QApplication::startDragDistance();
    Pin* pinUnder = PinAt(viewPos);
    Node* nodeUnder = pinUnder != nullptr ? pinUnder->Owner() : NodeAt(viewPos);
    const PinTarget sourceTarget = source->Target();
    const War3Map::StableId sourceOwner = source->Owner()->Id();
    if (source->IsOutput()) {
        if (clicked || pinUnder == source || nodeUnder == nullptr) {
            if (callbacks_.create) callbacks_.create(sourceTarget);
            return;
        }
        if (pinUnder != nullptr && pinUnder->IsOutput()) {
            return; // 输出接输出没有意义
        }
        if (nodeUnder->Id() != 0 && callbacks_.connect) callbacks_.connect(nodeUnder->Id(), sourceTarget);
        return;
    }
    // 输入端口：拉到另一个输出端口 = 把本语句移过去；单击或拉到空白处 = 在本语句之前新建。
    if (clicked || nodeUnder == nullptr) {
        if (callbacks_.create) callbacks_.create(sourceTarget);
        return;
    }
    if (pinUnder != nullptr && pinUnder->IsOutput() && sourceOwner != 0 && callbacks_.connect) {
        callbacks_.connect(sourceOwner, pinUnder->Target());
    }
}

void TriggerBlueprint::mouseReleaseEvent(QMouseEvent* event) {
    if (dragPin_ != nullptr && event->button() == Qt::LeftButton) {
        FinishPinDrag(event->pos());
        event->accept();
        return;
    }
    QGraphicsView::mouseReleaseEvent(event);
    if (event->button() != Qt::LeftButton) {
        return;
    }
    Node* node = NodeAt(event->pos());
    if (node == nullptr) {
        return;
    }
    const QString anchor = node->AnchorAt(mapToScene(event->pos()));
    if (node == conditionNode_) {
        if (callbacks_.showConditions) callbacks_.showConditions();
        return;
    }
    if (anchor.startsWith(QLatin1String("arg:")) && node->Id() != 0) {
        if (callbacks_.selected) callbacks_.selected(node->Id());
        const QPoint global = viewport()->mapToGlobal(event->pos());
        const QRect anchorRect(global - QPoint(4, 8), QSize(8, 16));
        const auto position = anchor.mid(4).toULongLong();
        if (callbacks_.editScalar && callbacks_.editScalar(node->Id(), position, anchorRect)) return;
        if (callbacks_.argument) callbacks_.argument(node->Id(), position, anchorRect);
    }
}

void TriggerBlueprint::mouseDoubleClickEvent(QMouseEvent* event) {
    if (PinAt(event->pos()) != nullptr) {
        return;
    }
    Node* node = NodeAt(event->pos());
    if (node != nullptr && node->Id() == 0) {
        return; // 触发条件 / 开始节点：单击已处理
    }
    if (node != nullptr && node->AnchorAt(mapToScene(event->pos())).isEmpty() && callbacks_.activated) {
        callbacks_.activated(node->Id());
        return;
    }
    QGraphicsView::mouseDoubleClickEvent(event);
}

void TriggerBlueprint::drawBackground(QPainter* painter, const QRectF& rect) {
    painter->fillRect(rect, palette().color(QPalette::Window));
    if (transform().m11() < 0.5) {
        return; // 缩得很小时不画网格，避免大量点
    }
    // 点阵网格（24 单位），与原型画布一致。
    QColor dot = palette().color(QPalette::Mid);
    dot.setAlpha(90);
    painter->setPen(QPen(dot, 0));
    const double step = 24;
    for (double x = std::floor(rect.left() / step) * step; x < rect.right(); x += step) {
        for (double y = std::floor(rect.top() / step) * step; y < rect.bottom(); y += step) {
            painter->drawPoint(QPointF(x, y));
        }
    }
}

void TriggerBlueprint::resizeEvent(QResizeEvent* event) {
    QGraphicsView::resizeEvent(event);
    UpdateZoomLabel();
}
