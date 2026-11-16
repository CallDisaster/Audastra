#pragma once

#include "War3MapCore/GameUi/FrameTree.h"

#include <QPointF>
#include <QRectF>

// 界面画布的坐标换算：屏幕单位（左下为原点）↔ 画布像素（左上为原点）。
namespace War3Map::Modules {

inline QPointF UiToPixel(const QRectF& screen, const GameUi::FrameTreeOptions& options, float x, float y) {
    return {screen.left() + static_cast<double>(x) / options.screenWidth * screen.width(),
        screen.top() + (options.screenHeight - static_cast<double>(y)) / options.screenHeight * screen.height()};
}

inline QRectF UiToPixelRect(const QRectF& screen, const GameUi::FrameTreeOptions& options, const GameUi::FrameRect& rect) {
    return QRectF(UiToPixel(screen, options, rect.left, rect.top), UiToPixel(screen, options, rect.right, rect.bottom)).normalized();
}

} // namespace War3Map::Modules
