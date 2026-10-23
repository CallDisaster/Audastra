#pragma once

#include <QString>

#include <functional>
#include <optional>

class QWidget;

namespace War3Map::Modules {

// 数值输入：滑块（拖动）与数字框（精确）同步。给了上下限时滑块按上下限，否则按初值估一个范围；数字框不限。
[[nodiscard]] QWidget* GameUiNumberEditor(double initial, bool integer, std::optional<double> low, std::optional<double> high,
    QWidget* parent, std::function<void(const QString&)> changed);

} // namespace War3Map::Modules
