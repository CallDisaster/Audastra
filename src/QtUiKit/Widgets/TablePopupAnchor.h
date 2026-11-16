#pragma once

#include <QRect>
#include <QSize>

#include <span>

class QWidget;
class QModelIndex;

namespace War3Map::UiKit {
// 浮层放在锚点的哪一侧。
enum class PopupSide { Right, Left, Below, Above };

// 浮层位置：尺寸先受工作区限制；按 sides 顺序试，整块放得进工作区的第一侧胜出（贴着锚点、左右或上下越界时平移）。
// 都放不下时取锚点上下空间较大的一侧并压低高度（不低于 minimum）；仍放不下才盖住锚点，整块留在工作区内。
// 表格浮层与字段浮层共用，也供测试直接核对。
[[nodiscard]] QRect PlaceAnchoredPopup(const QRect& anchor, QSize preferred, QSize minimum, const QRect& available,
    std::span<const PopupSide> sides, int gap = 0);

// 浮层跟随实际模型行；滚出视口、切换行或模型重置时关闭。
// 优先放在字段右/左/下/上方，受当前屏幕工作区约束。
void AnchorTablePopup(QWidget* panel, QWidget* anchor, const QModelIndex& index,
    QWidget* cellAnchor = nullptr);

// 浮层跟随一个字段（输入框、按钮）：锚点在屏幕上半部时放在下方，下半部时放在上方，左右越界时平移；
// 尺寸受锚点所在屏幕的工作区限制。所在窗口移动或改大小、锚点被滚动或重排、面板自己长高时实时重新放置；
// 锚点隐藏或销毁时关闭面板。
void AnchorFieldPopup(QWidget* panel, QWidget* anchor);
}
