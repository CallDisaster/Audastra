#include "GameUiCanvas.h"

#include "GameUiCanvasPixels.h"

#include <QContextMenuEvent>
#include <QCursor>
#include <QFocusEvent>
#include <QFontMetricsF>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QTimer>
#include <QWheelEvent>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>

// 界面画布的编辑交互：选中标注（名字、宽高、到父控件的距离）、控制柄改大小、拖动与吸附参考线、
// 方向键微调、滚轮缩放与平移。画布本身不改界面，结果以信号交给工作区。
namespace War3Map::Modules {

using namespace War3Map::GameUi;

QRectF GameUiCanvas::SelectionPixelRect(const QRectF& screen) const {
    if (!tree_ || selected_ >= tree_->Nodes().size()) return {};
    // 自己的控件按排出的矩形（控制柄与改大小都以它为准），其余按画出来的范围。
    const FrameNode& node = tree_->Nodes()[selected_];
    if (resizable_ && (node.layout == LayoutState::Placed || node.layout == LayoutState::SizeFromContent))
        return UiToPixelRect(screen, tree_->Options(), node.rect);
    return NodePixelRect(screen, selected_);
}

std::vector<QPointF> GameUiCanvas::HandlePoints(const QRectF& box) const {
    const QPointF center = box.center();
    return {box.topLeft(), {center.x(), box.top()}, box.topRight(), {box.right(), center.y()}, box.bottomRight(),
        {center.x(), box.bottom()}, box.bottomLeft(), {box.left(), center.y()}};
}

int GameUiCanvas::HandleAt(const QPointF& position) const {
    if (!draggable_ || !resizable_ || selected_ == kNoNode) return -1;
    const QRectF selected = SelectionPixelRect(ScreenRect());
    if (selected.isNull()) return -1;
    const auto points = HandlePoints(selected.adjusted(0.75, 0.75, -0.75, -0.75));
    // 小控件的边中点和角挤在一起：先取角。
    for (const int index : {0, 2, 4, 6, 1, 3, 5, 7}) {
        const QPointF delta = position - points[static_cast<std::size_t>(index)];
        if (std::abs(delta.x()) <= 5.0 && std::abs(delta.y()) <= 5.0) return index;
    }
    return -1;
}

QString GameUiCanvas::MeasureText(double units, bool horizontal) const {
    if (measurePixels_) {
        const double pixels = horizontal ? UiPixelsFromUnitsX(units, resolution_) : UiPixelsFromUnitsY(units, resolution_);
        return QString::number(std::lround(pixels));
    }
    QString text = QString::number(units, 'f', 4);
    while (text.contains(QLatin1Char('.')) && (text.endsWith(QLatin1Char('0')) || text.endsWith(QLatin1Char('.')))) text.chop(1);
    return text == QLatin1String("-0") ? QStringLiteral("0") : text;
}

namespace {

// 矩形上对齐点的像素位置（像素坐标上为负方向）。
QPointF PixelPoint(const QRectF& rect, FramePoint point) {
    static constexpr std::array<std::pair<double, double>, 9> kFractions{{
        {0.0, 0.0}, {0.5, 0.0}, {1.0, 0.0}, {0.0, 0.5}, {0.5, 0.5}, {1.0, 0.5}, {0.0, 1.0}, {0.5, 1.0}, {1.0, 1.0}}};
    const auto [fx, fy] = kFractions[static_cast<std::size_t>(point)];
    return {rect.left() + rect.width() * fx, rect.top() + rect.height() * fy};
}

} // namespace

void GameUiCanvas::PaintChip(QPainter& painter, const QRectF& screen, const QPointF& anchor, const QString& text, bool below,
    const QColor& fill) const {
    const QFontMetricsF metrics(painter.font());
    const QSizeF size(metrics.horizontalAdvance(text) + 8.0, metrics.height() + 2.0);
    QRectF rect(anchor.x() - size.width() / 2.0, below ? anchor.y() + 3.0 : anchor.y() - size.height() - 3.0, size.width(), size.height());
    rect.moveLeft(std::clamp(rect.left(), screen.left() + 1.0, std::max(screen.left() + 1.0, screen.right() - rect.width() - 1.0)));
    painter.setPen(Qt::NoPen);
    painter.setBrush(fill);
    painter.drawRoundedRect(rect, 3.0, 3.0);
    painter.setPen(fill.lightnessF() > 0.6 ? QColor(20, 20, 20) : QColor(245, 245, 245));
    painter.drawText(rect, Qt::AlignCenter, text);
}

QString GameUiCanvas::NodeLabel(const FrameNode& node) const {
    if (node.uiFrame && !node.uiFrame->frame.empty()) return QString::fromStdString(node.name);
    if (!node.codeLabel.empty()) return QString::fromStdString(node.codeLabel);
    return QString::fromStdString(node.name.empty() ? node.frameType : node.name);
}

// 名字与宽高写在框下面（下面放不下时写在上面）。
void GameUiCanvas::PaintNameChip(QPainter& painter, const QRectF& screen, const FrameNode& node, const QRectF& box, const QColor& fill) const {
    const auto& options = tree_->Options();
    const double unitX = options.screenWidth / screen.width();
    const double unitY = options.screenHeight / screen.height();
    const QString name = NodeLabel(node);
    const QString size = QStringLiteral("%1 × %2").arg(MeasureText(box.width() * unitX, true), MeasureText(box.height() * unitY, false));
    const bool below = box.bottom() + QFontMetricsF(painter.font()).height() + 8.0 < screen.bottom();
    PaintChip(painter, screen, QPointF(box.center().x(), below ? box.bottom() : box.top()),
        name.isEmpty() ? size : name + QStringLiteral("  ") + size, below, fill);
}

// 父子连线：从对齐到的目标点画到控件自己的对齐点，线上写横纵位移。shown 为拖动、微调中的新位置。
void GameUiCanvas::PaintAnchorLines(QPainter& painter, const QRectF& screen, const FrameNode& node, const QRectF& original,
    const QRectF& shown, const QColor& color, bool labels) const {
    const auto& options = tree_->Options();
    const auto& nodes = tree_->Nodes();
    const double unitX = options.screenWidth / screen.width();
    const double unitY = options.screenHeight / screen.height();
    for (const FrameAnchor& anchor : node.anchors) {
        const std::uint32_t target = anchor.relativeNode != kNoNode ? anchor.relativeNode : node.parent;
        if (target >= nodes.size()) continue;
        const FrameNode& targetNode = nodes[target];
        if (target != kScreenNode && targetNode.layout != LayoutState::Placed && targetNode.layout != LayoutState::SizeFromContent) continue;
        const QRectF targetRect = target == kScreenNode ? screen : UiToPixelRect(screen, options, targetNode.rect);
        const QPointF from = PixelPoint(targetRect, anchor.relativePoint);
        const QPointF to = PixelPoint(shown, anchor.point);
        const QPointF moved = to - PixelPoint(original, anchor.point);
        painter.setPen(QPen(color, 1.0));
        painter.setBrush(Qt::NoBrush);
        painter.drawLine(from, to);
        painter.setBrush(color);
        painter.drawEllipse(from, 2.5, 2.5);
        painter.drawRect(QRectF(to.x() - 2.0, to.y() - 2.0, 4.0, 4.0));
        if (!labels) continue;
        const QString text = QStringLiteral("%1, %2").arg(MeasureText(anchor.x + moved.x() * unitX, true), MeasureText(anchor.y - moved.y() * unitY, false));
        PaintChip(painter, screen, (from + to) / 2.0, text, false, color);
    }
}

// 选中控件的标注：父控件画成虚线框，父子连线与位移，名字与宽高。
void GameUiCanvas::PaintMeasures(QPainter& painter, const QRectF& screen, const QRectF& original, const QRectF& shown) const {
    const auto& options = tree_->Options();
    const FrameNode& node = tree_->Nodes()[selected_];
    QFont small = font();
    small.setPixelSize(11);
    painter.setFont(small);
    const QColor accent = palette().color(QPalette::Accent);
    if (node.parent != kNoNode && node.parent != kScreenNode && node.parent < tree_->Nodes().size()) {
        const FrameNode& parentNode = tree_->Nodes()[node.parent];
        if (parentNode.layout == LayoutState::Placed || parentNode.layout == LayoutState::SizeFromContent) {
            QColor parentLine = accent;
            parentLine.setAlpha(120);
            painter.setPen(QPen(parentLine, 1.0, Qt::DotLine));
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(UiToPixelRect(screen, options, parentNode.rect).adjusted(0.5, 0.5, -0.5, -0.5));
        }
    }
    PaintAnchorLines(painter, screen, node, original, shown, accent, true);
    PaintNameChip(painter, screen, node, shown, accent);
}

void GameUiCanvas::mousePressEvent(QMouseEvent* event) {
    CommitNudge();
    // 中键或按住空格：平移视图（类型预览的放大视图不平移）。
    if (!focus_ && (event->button() == Qt::MiddleButton || (event->button() == Qt::LeftButton && spaceHeld_))) {
        panning_ = true;
        panStart_ = event->position();
        panOrigin_ = pan_;
        setCursor(Qt::ClosedHandCursor);
        return;
    }
    if (event->button() != Qt::LeftButton) return QWidget::mousePressEvent(event);
    dragStart_ = event->position();
    dragging_ = false;
    guides_.clear();
    // 选中控件的控制柄：改大小，不换选中。
    if (const int handle = HandleAt(event->position()); handle >= 0) {
        dragNode_ = selected_;
        dragMode_ = DragMode::Resize;
        dragHandle_ = handle;
        dragOrigin_ = SelectionPixelRect(ScreenRect());
        dragPreview_ = dragOrigin_;
        CollectSnapLines(dragNode_);
        return;
    }
    const std::uint32_t node = NodeAt(event->position());
    emit NodeClicked(node);
    // 点中的节点被选中且可拖动时，准备拖动。
    dragNode_ = draggable_ && node != kNoNode && node == selected_ ? node : kNoNode;
    dragMode_ = dragNode_ == kNoNode ? DragMode::None : DragMode::Move;
    dragHandle_ = -1;
    if (dragNode_ != kNoNode) {
        dragOrigin_ = SelectionPixelRect(ScreenRect());
        dragPreview_ = dragOrigin_;
        CollectSnapLines(dragNode_);
    }
}

void GameUiCanvas::mouseReleaseEvent(QMouseEvent* event) {
    if (panning_ && (event->button() == Qt::MiddleButton || event->button() == Qt::LeftButton)) {
        panning_ = false;
        UpdateCursor(event->position());
        return;
    }
    if (event->button() != Qt::LeftButton || dragNode_ == kNoNode) return QWidget::mouseReleaseEvent(event);
    const std::uint32_t node = std::exchange(dragNode_, kNoNode);
    const DragMode mode = std::exchange(dragMode_, DragMode::None);
    const bool moved = std::exchange(dragging_, false);
    guides_.clear();
    update();
    const QRectF screen = ScreenRect();
    if (!moved || !tree_ || screen.isEmpty()) return;
    const auto& options = tree_->Options();
    const double unitX = options.screenWidth / screen.width();
    const double unitY = options.screenHeight / screen.height();
    const auto roundUnits = [](double value) { return std::round(value * 10000.0) / 10000.0; };
    if (mode == DragMode::Resize) {
        // 像素矩形换回屏幕单位（左下为原点）。
        FrameRect rect;
        rect.left = static_cast<float>(roundUnits((dragPreview_.left() - screen.left()) * unitX));
        rect.right = static_cast<float>(roundUnits((dragPreview_.right() - screen.left()) * unitX));
        rect.top = static_cast<float>(roundUnits((screen.bottom() - dragPreview_.top()) * unitY));
        rect.bottom = static_cast<float>(roundUnits((screen.bottom() - dragPreview_.bottom()) * unitY));
        if (dragPreview_ != dragOrigin_ && rect.Width() > 0.0f && rect.Height() > 0.0f) emit NodeResized(node, rect);
        return;
    }
    const QPointF offset = dragPreview_.topLeft() - dragOrigin_.topLeft();
    const double dx = roundUnits(offset.x() * unitX);
    const double dy = roundUnits(-offset.y() * unitY);
    if (dx != 0.0 || dy != 0.0) emit NodeDragged(node, dx, dy);
}

void GameUiCanvas::CancelDrag() {
    dragNode_ = kNoNode;
    dragMode_ = DragMode::None;
    dragging_ = false;
    panning_ = false;
    guides_.clear();
    update();
}

void GameUiCanvas::CollectSnapLines(std::uint32_t node) {
    snapXs_.clear();
    snapYs_.clear();
    const QRectF screen = ScreenRect();
    const auto add = [this](const QRectF& rect) {
        snapXs_.insert(snapXs_.end(), {rect.left(), rect.center().x(), rect.right()});
        snapYs_.insert(snapYs_.end(), {rect.top(), rect.center().y(), rect.bottom()});
    };
    add(screen);
    if (!tree_ || node >= tree_->Nodes().size()) return;
    const auto& nodes = tree_->Nodes();
    const FrameNode& dragged = nodes[node];
    const auto inside = [&](std::uint32_t candidate) {
        for (std::uint32_t at = candidate; at != kNoNode && at < nodes.size(); at = nodes[at].parent) {
            if (at == node) return true;
        }
        return false;
    };
    const auto placed = [](const FrameNode& candidate) {
        return candidate.layout == LayoutState::Placed || candidate.layout == LayoutState::SizeFromContent;
    };
    if (dragged.parent != kNoNode && dragged.parent < nodes.size() && dragged.parent != kScreenNode && placed(nodes[dragged.parent]))
        add(UiToPixelRect(screen, tree_->Options(), nodes[dragged.parent].rect));
    // 同一界面的其他控件（不含自己和自己的子控件）；原版界面只取同一父节点下的兄弟。
    for (const FrameNode& candidate : nodes) {
        if (candidate.id == node || candidate.kind != DefinitionKind::Frame || !placed(candidate) || inside(candidate.id)) continue;
        const bool sameUi = dragged.uiFrame && candidate.uiFrame && candidate.uiFrame->description == dragged.uiFrame->description
            && !candidate.uiFrame->frame.empty();
        const bool sibling = !dragged.uiFrame && candidate.parent == dragged.parent;
        if (sameUi || sibling) add(UiToPixelRect(screen, tree_->Options(), candidate.rect));
    }
}

double GameUiCanvas::SnapValue(double value, bool horizontal, double* snapped) const {
    constexpr double kSnapDistance = 6.0;
    double best = kSnapDistance + 1.0;
    double result = value;
    for (const double line : horizontal ? snapXs_ : snapYs_) {
        if (std::abs(line - value) < best) {
            best = std::abs(line - value);
            result = line;
        }
    }
    if (showGrid_) {
        const QRectF screen = ScreenRect();
        const double step = GridStep(screen);
        const double origin = horizontal ? screen.left() : screen.top();
        const double line = origin + std::round((value - origin) / step) * step;
        if (std::abs(line - value) < best) {
            best = std::abs(line - value);
            result = line;
        }
    }
    if (best > kSnapDistance) return std::numeric_limits<double>::infinity();
    if (snapped) *snapped = result;
    return result - value;
}

void GameUiCanvas::UpdateDragPreview(const QPointF& position, Qt::KeyboardModifiers modifiers) {
    const QPointF delta = position - dragStart_;
    guides_.clear();
    const bool snap = !(modifiers & Qt::AltModifier);
    const QRectF screen = ScreenRect();
    const auto addGuide = [&](double at, bool vertical) {
        guides_.push_back(vertical ? QLineF(at, screen.top(), at, screen.bottom()) : QLineF(screen.left(), at, screen.right(), at));
    };
    if (dragMode_ == DragMode::Move) {
        QRectF moved = dragOrigin_.translated(delta);
        if (snap) {
            // 左、中、右里离候选线最近的一条贴上去；上下同理。
            for (const bool horizontal : {true, false}) {
                double bestShift = std::numeric_limits<double>::infinity();
                double bestLine = 0.0;
                const std::array<double, 3> edges = horizontal ? std::array<double, 3>{moved.left(), moved.center().x(), moved.right()}
                                                               : std::array<double, 3>{moved.top(), moved.center().y(), moved.bottom()};
                for (const double edge : edges) {
                    double line = 0.0;
                    const double shift = SnapValue(edge, horizontal, &line);
                    if (std::abs(shift) < std::abs(bestShift)) {
                        bestShift = shift;
                        bestLine = line;
                    }
                }
                if (std::isfinite(bestShift)) {
                    moved.translate(horizontal ? bestShift : 0.0, horizontal ? 0.0 : bestShift);
                    addGuide(bestLine, horizontal);
                }
            }
        }
        dragPreview_ = moved;
        return;
    }
    // 改大小：只动所拖的边；最小 4 像素。
    const bool left = dragHandle_ == 0 || dragHandle_ == 6 || dragHandle_ == 7;
    const bool right = dragHandle_ >= 2 && dragHandle_ <= 4;
    const bool top = dragHandle_ <= 2;
    const bool bottom = dragHandle_ >= 4 && dragHandle_ <= 6;
    double l = dragOrigin_.left() + (left ? delta.x() : 0.0);
    double r = dragOrigin_.right() + (right ? delta.x() : 0.0);
    double t = dragOrigin_.top() + (top ? delta.y() : 0.0);
    double b = dragOrigin_.bottom() + (bottom ? delta.y() : 0.0);
    const auto snapEdge = [&](double& edge, bool horizontal) {
        if (!snap) return;
        double line = 0.0;
        const double shift = SnapValue(edge, horizontal, &line);
        if (!std::isfinite(shift)) return;
        edge = line;
        addGuide(line, horizontal);
    };
    if (left) snapEdge(l, true);
    if (right) snapEdge(r, true);
    if (top) snapEdge(t, false);
    if (bottom) snapEdge(b, false);
    constexpr double kMinimum = 4.0;
    if (r - l < kMinimum) {
        if (left) l = r - kMinimum;
        else r = l + kMinimum;
    }
    if (b - t < kMinimum) {
        if (top) t = b - kMinimum;
        else b = t + kMinimum;
    }
    dragPreview_ = QRectF(QPointF(l, t), QPointF(r, b));
}

void GameUiCanvas::UpdateCursor(const QPointF& position) {
    if (panning_) return setCursor(Qt::ClosedHandCursor);
    if (spaceHeld_ && !focus_) return setCursor(Qt::OpenHandCursor);
    switch (HandleAt(position)) {
    case 0:
    case 4: return setCursor(Qt::SizeFDiagCursor);
    case 2:
    case 6: return setCursor(Qt::SizeBDiagCursor);
    case 1:
    case 5: return setCursor(Qt::SizeVerCursor);
    case 3:
    case 7: return setCursor(Qt::SizeHorCursor);
    default: break;
    }
    if (draggable_ && selected_ != kNoNode && SelectionPixelRect(ScreenRect()).contains(position)) return setCursor(Qt::SizeAllCursor);
    unsetCursor();
}

void GameUiCanvas::wheelEvent(QWheelEvent* event) {
    if (focus_ || dragNode_ != kNoNode || panning_) return QWidget::wheelEvent(event);
    const double steps = event->angleDelta().y() / 120.0;
    if (steps == 0.0) return;
    const QRectF before = ScreenRect();
    if (before.isEmpty()) return;
    const double zoom = std::clamp(zoom_ * std::pow(1.2, steps), 0.25, 16.0);
    if (zoom == zoom_) return;
    // 光标下的画面位置保持不动。
    const QPointF cursor = event->position();
    const QPointF fraction((cursor.x() - before.left()) / before.width(), (cursor.y() - before.top()) / before.height());
    const QSizeF size(before.width() / zoom_ * zoom, before.height() / zoom_ * zoom);
    const QPointF topLeft(cursor.x() - fraction.x() * size.width(), cursor.y() - fraction.y() * size.height());
    const QPointF fitCenter = before.center() - pan_;
    zoom_ = zoom;
    pan_ = topLeft + QPointF(size.width() / 2.0, size.height() / 2.0) - fitCenter;
    hits_.clear();
    update();
    event->accept();
}

void GameUiCanvas::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Space && !event->isAutoRepeat()) {
        spaceHeld_ = true;
        UpdateCursor(mapFromGlobal(QCursor::pos()));
        return;
    }
    if (event->key() == Qt::Key_Escape && dragNode_ != kNoNode) {
        CancelDrag();
        return;
    }
    if (event->key() == Qt::Key_0 && (event->modifiers() & Qt::ControlModifier)) {
        FitView();
        return;
    }
    const bool arrow = event->key() == Qt::Key_Left || event->key() == Qt::Key_Right || event->key() == Qt::Key_Up || event->key() == Qt::Key_Down;
    if (!arrow || !draggable_ || selected_ == kNoNode || dragNode_ != kNoNode) return QWidget::keyPressEvent(event);
    // 方向键微调：一次 1 像素（参考分辨率），Shift 10 像素；停下后合成一步修改。
    if (nudgeNode_ != selected_) {
        CommitNudge();
        nudgeNode_ = selected_;
    }
    const double pixels = (event->modifiers() & Qt::ShiftModifier) ? 10.0 : 1.0;
    const double stepX = UiUnitsFromPixelsX(pixels, resolution_);
    const double stepY = UiUnitsFromPixelsY(pixels, resolution_);
    switch (event->key()) {
    case Qt::Key_Left: nudge_.rx() -= stepX; break;
    case Qt::Key_Right: nudge_.rx() += stepX; break;
    case Qt::Key_Up: nudge_.ry() += stepY; break;
    default: nudge_.ry() -= stepY; break;
    }
    nudgeTimer_->start();
    update();
    event->accept();
}

void GameUiCanvas::keyReleaseEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Space && !event->isAutoRepeat()) {
        spaceHeld_ = false;
        UpdateCursor(mapFromGlobal(QCursor::pos()));
        return;
    }
    QWidget::keyReleaseEvent(event);
}

void GameUiCanvas::focusOutEvent(QFocusEvent* event) {
    spaceHeld_ = false;
    CommitNudge();
    QWidget::focusOutEvent(event);
}

void GameUiCanvas::CommitNudge() {
    nudgeTimer_->stop();
    const std::uint32_t node = std::exchange(nudgeNode_, kNoNode);
    const QPointF nudge = std::exchange(nudge_, QPointF());
    if (node == kNoNode || nudge.isNull()) return;
    update();
    const double dx = std::round(nudge.x() * 1000000.0) / 1000000.0;
    const double dy = std::round(nudge.y() * 1000000.0) / 1000000.0;
    if (dx != 0.0 || dy != 0.0) emit NodeDragged(node, dx, dy);
}

void GameUiCanvas::contextMenuEvent(QContextMenuEvent* event) {
    const QRectF screen = ScreenRect();
    if (!tree_ || screen.isEmpty()) return QWidget::contextMenuEvent(event);
    const auto& options = tree_->Options();
    const QPointF position = event->pos();
    const QPointF point((position.x() - screen.left()) / screen.width() * options.screenWidth,
        (screen.bottom() - position.y()) / screen.height() * options.screenHeight);
    emit ContextMenuRequested(NodeAt(position), point, event->globalPos());
}

void GameUiCanvas::mouseMoveEvent(QMouseEvent* event) {
    if (panning_) {
        pan_ = panOrigin_ + (event->position() - panStart_);
        hits_.clear();
        update();
        return;
    }
    if (dragNode_ != kNoNode && (event->buttons() & Qt::LeftButton)) {
        if (!dragging_ && (event->position() - dragStart_).manhattanLength() >= 4.0) dragging_ = true;
        if (dragging_) {
            UpdateDragPreview(event->position(), event->modifiers());
            update();
        }
        return;
    }
    UpdateCursor(event->position());
    const std::uint32_t node = NodeAt(event->position());
    if (hovered_ != node) {
        hovered_ = node;
        update();
    }
    if (!tree_ || node == kNoNode) {
        setToolTip({});
        return;
    }
    const FrameNode& frame = tree_->Nodes()[node];
    const QString name = !frame.codeLabel.empty() ? QString::fromStdString(frame.codeLabel)
        : frame.name.empty()                      ? QString::fromStdString(frame.frameType)
                                                  : QString::fromStdString(frame.name);
    setToolTip(frame.frameType.empty() ? name : QStringLiteral("%1 · %2").arg(name, QString::fromStdString(frame.frameType)));
}

void GameUiCanvas::leaveEvent(QEvent* event) {
    if (hovered_ != kNoNode) {
        hovered_ = kNoNode;
        update();
    }
    QWidget::leaveEvent(event);
}

} // namespace War3Map::Modules
