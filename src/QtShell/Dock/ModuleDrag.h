#pragma once

#include "Layout/DockLayout.h"

#include <QPoint>

#include <optional>

class QMimeData;
class QWidget;

namespace War3Map::Shell {

inline constexpr char kModuleMimeType[] = "application/x-war3map-module";

enum class DragOutcome {
    Docked,    // 被某个停靠目标接收
    Released,  // 在应用外或空白处释放：调用方在释放位置创建浮动窗口
    Cancelled, // 按 Esc 取消：什么都不做
};

// 拖动模块（切换器条目、右侧标签）。
DragOutcome DragModule(QWidget* source, const ModuleId& id);

[[nodiscard]] std::optional<ModuleId> ModuleFromMime(const QMimeData* mime);

// 按下后移动超过系统拖动阈值才算拖动，避免点击被误判。
[[nodiscard]] bool PassedDragThreshold(QPoint pressed, QPoint current);

} // namespace War3Map::Shell
