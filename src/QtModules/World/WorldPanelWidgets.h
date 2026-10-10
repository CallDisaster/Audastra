#pragma once

#include <QString>

#include <initializer_list>
#include <utility>

class QButtonGroup;
class QFrame;
class QLabel;
class QWidget;

// 世界视图左侧工具面板与右侧属性栏共用的小控件（样式由 Workbench.qss 的“世界视图”一节指定）。
namespace War3Map::Modules::WorldUi {

// 面板内的容器：用面板底色（全局样式表把普通 QWidget 画成页面底色）。
[[nodiscard]] QWidget* Surface(QWidget* parent = nullptr);
// 小号大写分组标题。
[[nodiscard]] QLabel* SectionLabel(const QString& text, QWidget* parent);
// 动态属性变化后重新套用样式。
void Repolish(QWidget* widget);
// 分段选择：一组互斥按钮加入 group（id 为给定值），底块随选中项滑动。按钮对象名为 name + 序号。
[[nodiscard]] QFrame* Segment(QWidget* parent, QButtonGroup* group,
    std::initializer_list<std::pair<QString, int>> items, const char* name);

} // namespace War3Map::Modules::WorldUi
