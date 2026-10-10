#include "WorldViewport.h"
#include "WorldViewportInternals.h"

#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QKeyEvent>
#include <QMimeData>

namespace War3Map::Modules {
namespace {
constexpr const char* kAssetMime = "application/x-forge-world-asset-v1";

bool SameDropView(const Render::ViewFrameIdentity& first, const Render::ViewFrameIdentity& second) {
    return first.surfaceEpoch == second.surfaceEpoch && first.sceneEpoch == second.sceneEpoch && first.viewEpoch == second.viewEpoch;
}

QByteArray AssetBytes(const QMimeData* mime) {
    if (!mime || !mime->hasFormat(kAssetMime)) return {};
    const auto bytes = mime->data(kAssetMime);
    return bytes.size() <= 4096 ? bytes : QByteArray{};
}
} // namespace

void WorldViewport::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        ++selectionGestureSerial_; selectionRetries_ = 0;
        ClearSelectionOutline();
    }
    if (placing_ && event->key() == Qt::Key_Control) {
        hoverModifiers_ |= Qt::ControlModifier; hoverView_.reset(); ScheduleHover();
    }
    if (!brushMode_ && event->modifiers().testFlag(Qt::ControlModifier)) {
        if (event->key() == Qt::Key_C) { emit CopyRequested(); event->accept(); return; }
        if (event->key() == Qt::Key_V) { emit PasteRequested(); event->accept(); return; }
    }
    const bool idle = !brushMode_ && !placing_ && !strokeActive_ && !moving_ && !selectionBox_ && dragButton_ == Qt::NoButton;
    if (idle && !event->modifiers().testFlag(Qt::AltModifier)) {
        if (event->modifiers().testFlag(Qt::ControlModifier)) {
            if (event->key() == Qt::Key_D) { emit DuplicateRequested(); event->accept(); return; }
            if (event->key() == Qt::Key_A) { emit SelectAllRequested(); event->accept(); return; }
        } else {
            if (event->key() == Qt::Key_F) { emit FocusSelectionRequested(); event->accept(); return; }
            if (event->key() == Qt::Key_Home) { ResetCamera(); event->accept(); return; }
            const double step = event->modifiers().testFlag(Qt::ShiftModifier) ? 64.0 : 16.0;
            switch (event->key()) {
            case Qt::Key_Left: emit NudgeRequested(-step, 0); event->accept(); return;
            case Qt::Key_Right: emit NudgeRequested(step, 0); event->accept(); return;
            case Qt::Key_Up: emit NudgeRequested(0, step); event->accept(); return;
            case Qt::Key_Down: emit NudgeRequested(0, -step); event->accept(); return;
            default: break;
            }
        }
    }
    if (event->key() == Qt::Key_Escape && selectionBox_) {
        selectionBox_ = false; dragging_ = false; dragButton_ = Qt::NoButton; pressView_.reset();
        event->accept(); return;
    }
    if (event->key() == Qt::Key_Escape && strokeActive_) {
        CancelTerrainStroke(); event->accept(); return;
    }
    if (event->key() == Qt::Key_Escape && brushMode_) {
        brushMode_ = false; emit TerrainBrushExitRequested(); event->accept(); return;
    }
    if (brushMode_ && (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace)) {
        event->accept(); return;
    }
    if (event->key() == Qt::Key_Escape && moving_ && moved_) {
        CancelMove(); dragButton_ = Qt::NoButton; dragging_ = false; event->accept(); return;
    }
    if ((event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) && !moving_) {
        emit DeleteRequested(); event->accept(); return;
    }
    if (event->key() == Qt::Key_Escape && placing_) {
        placing_ = false; emit PlacingCancelled(); event->accept(); return;
    }
    if (event->key() == Qt::Key_Escape) {
        emit InstanceClicked(-1, 0);
        selectedCoordinate_.reset(); pressView_.reset();
        SetCoordinateTexts(hoverText_, tr("未选择地形坐标")); event->accept(); return;
    }
    QWidget::keyPressEvent(event);
}

void WorldViewport::dragEnterEvent(QDragEnterEvent* event) {
    dropView_.reset();
    if (brushMode_ || strokeActive_ || moving_ || selectionBox_ || dragButton_ != Qt::NoButton
        || !event->possibleActions().testFlag(Qt::CopyAction) || AssetBytes(event->mimeData()).isEmpty()) {
        event->ignore(); return;
    }
    const auto frame = ReadPickingFrame();
    if (!frame.Succeeded()) { event->ignore(); return; }
    dropView_ = frame.value->identity; dropRevision_ = revision_;
    event->setDropAction(Qt::CopyAction); event->accept();
}

void WorldViewport::dragMoveEvent(QDragMoveEvent* event) {
    if (!dropView_ || dropRevision_ != revision_ || AssetBytes(event->mimeData()).isEmpty()) { event->ignore(); return; }
    const auto frame = ReadPickingFrame();
    if (!frame.Succeeded() || !SameDropView(*dropView_, frame.value->identity)) { event->ignore(); return; }
    const auto ground = PickAt(event->position(), *frame.value);
    if (!ground.Succeeded() || !ground.value->hit) { event->ignore(); return; }
    event->setDropAction(Qt::CopyAction); event->accept();
}

void WorldViewport::dragLeaveEvent(QDragLeaveEvent* event) {
    dropView_.reset(); event->accept();
}

void WorldViewport::dropEvent(QDropEvent* event) {
    const auto expectedView = dropView_;
    dropView_.reset();
    const auto bytes = AssetBytes(event->mimeData());
    if (!expectedView || dropRevision_ != revision_ || brushMode_ || strokeActive_ || moving_ || selectionBox_
        || dragButton_ != Qt::NoButton || !event->possibleActions().testFlag(Qt::CopyAction) || bytes.isEmpty()) {
        event->ignore(); return;
    }
    const auto frame = ReadPickingFrame();
    if (!frame.Succeeded() || !SameDropView(*expectedView, frame.value->identity)) {
        ClearCoordinates(tr("拖放期间视图已改变，请重新拖入资源")); event->ignore(); return;
    }
    const auto ground = PickAt(event->position(), *frame.value);
    if (!ground.Succeeded() || !ground.value->hit || dropRevision_ != revision_) {
        event->ignore(); return;
    }
    // 只认同一完成视图的真实地形点；地图/目录完整身份仍由宿主 ParseDrag 和已有事务复核。
    event->setDropAction(Qt::CopyAction); event->accept(); setFocus(Qt::MouseFocusReason);
    emit AssetDropped(bytes, ground.value->hit->position[0], ground.value->hit->position[1],
        event->modifiers().testFlag(Qt::ControlModifier));
}

} // namespace War3Map::Modules
