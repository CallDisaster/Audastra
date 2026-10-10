#pragma once

#include "Layout/DockLayout.h"

#include <QJsonObject>
#include <QJsonValue>

class QSettings;

namespace War3Map::Shell {

[[nodiscard]] QJsonObject LayoutToJson(const DockLayout& layout);
// 读取本机保存的布局；格式错误返回默认布局，未知模块、重复位置与非法尺寸丢弃。
[[nodiscard]] DockLayout LayoutFromJson(const QJsonValue& value, const std::vector<ModuleId>& known);

// 布局保存在编辑器自己的设置中（本机、按用户）。
inline constexpr char kLayoutSettingsKey[] = "shell/layout-v1";
[[nodiscard]] DockLayout LoadLayout(const QSettings& settings, const std::vector<ModuleId>& known);
void SaveLayout(QSettings& settings, const DockLayout& layout);

} // namespace War3Map::Shell
