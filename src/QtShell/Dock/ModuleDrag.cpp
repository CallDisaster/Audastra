#include "Dock/ModuleDrag.h"

#include <QApplication>
#include <QDrag>
#include <QMimeData>
#include <QPixmap>
#include <QWidget>

#ifdef _WIN32
#include <windows.h>
#endif

namespace War3Map::Shell {

DragOutcome DragModule(QWidget* source, const ModuleId& id) {
    auto* mime = new QMimeData;
    mime->setData(kModuleMimeType, QByteArray::fromStdString(id));
    auto* drag = new QDrag(source);
    drag->setMimeData(mime);
    const QPixmap preview = source->grab();
    drag->setPixmap(preview);
    drag->setHotSpot(QPoint(preview.width() / 2, preview.height() / 2) / preview.devicePixelRatio());
    if (drag->exec(Qt::MoveAction, Qt::MoveAction) == Qt::MoveAction) {
        return DragOutcome::Docked;
    }
#ifdef _WIN32
    // 系统拖放循环在按下 Esc 时结束，此刻 Esc 仍处于按下状态；取消不能被当成“拖到空白处浮动”。
    if ((GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0) {
        return DragOutcome::Cancelled;
    }
#endif
    return DragOutcome::Released;
}

std::optional<ModuleId> ModuleFromMime(const QMimeData* mime) {
    if (mime == nullptr || !mime->hasFormat(kModuleMimeType)) {
        return std::nullopt;
    }
    const QByteArray bytes = mime->data(kModuleMimeType);
    if (bytes.isEmpty()) {
        return std::nullopt;
    }
    return bytes.toStdString();
}

bool PassedDragThreshold(QPoint pressed, QPoint current) {
    return (current - pressed).manhattanLength() >= QApplication::startDragDistance();
}

} // namespace War3Map::Shell
