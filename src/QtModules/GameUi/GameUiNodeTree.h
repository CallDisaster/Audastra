#pragma once

#include "War3MapCore/GameUi/FrameTree.h"

#include <QObject>
#include <QString>
#include <Qt>

// 界面工作区左侧节点树：列、数据角色与共用的显示文字。
namespace War3Map::Modules::GameUiNodeTree {

inline constexpr int kNodeRole = Qt::UserRole + 1;
// 节点名（代码区域显示中文名，搜索仍可按标识查）。
inline constexpr int kNameRole = Qt::UserRole + 2;

// 逻辑列：名称、类型、状态在前（代码按 text(0) 取名称），可见、可选两列排在最前面显示。
inline constexpr int kNameColumn = 0;
inline constexpr int kTypeColumn = 1;
inline constexpr int kStateColumn = 2;
inline constexpr int kVisibleColumn = 3;
inline constexpr int kSelectColumn = 4;
inline constexpr int kColumnCount = 5;

// 节点类型列：贴图、文字与层写种类，框架写游戏里的框架类型。
[[nodiscard]] inline QString TypeText(const GameUi::FrameNode& node) {
    switch (node.kind) {
    case GameUi::DefinitionKind::Texture: return QStringLiteral("Texture");
    case GameUi::DefinitionKind::String: return QStringLiteral("String");
    case GameUi::DefinitionKind::Layer: return QStringLiteral("Layer");
    default: return QString::fromStdString(node.frameType);
    }
}

[[nodiscard]] inline QString LayoutText(GameUi::LayoutState state) {
    switch (state) {
    case GameUi::LayoutState::Placed: return QObject::tr("已定位");
    case GameUi::LayoutState::SizeFromContent: return QObject::tr("随文字");
    case GameUi::LayoutState::Cycle: return QObject::tr("锚点循环");
    default: return QObject::tr("未定位");
    }
}

} // namespace War3Map::Modules::GameUiNodeTree
