#pragma once

class QWidget;
class QModelIndex;

namespace War3Map::UiKit {
// 浮层跟随实际模型行；滚出视口、切换行或模型重置时关闭。
// 优先放在字段右/左/下/上方，受当前屏幕工作区约束。
void AnchorTablePopup(QWidget* panel, QWidget* anchor, const QModelIndex& index,
    QWidget* cellAnchor = nullptr);
}
