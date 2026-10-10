#pragma once

#include <QIcon>
#include <QPixmap>

namespace War3Map::UiKit {

// 产品标识保留原色；不走随主题着色的功能 SVG 图标通道。
QIcon ApplicationIcon();
QPixmap ApplicationBrandPixmap();

} // namespace War3Map::UiKit
