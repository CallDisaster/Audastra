#pragma once

#include <QJsonArray>

#include <initializer_list>

class QHeaderView;

namespace War3Map::UiKit {

// 资源管理器式列：每列都可拖动调整宽度，双击分隔线按内容自适应，最后一列填满剩余宽度。
// initialWidths 依次给前几列的初始宽度（像素，按当前字体缩放前的设计值）；没给的列按内容。
void ConfigureResizableColumns(QHeaderView* header, std::initializer_list<int> initialWidths = {});

// 列宽写入视图状态 / 从视图状态恢复（列数不符时忽略）。
[[nodiscard]] QJsonArray SaveColumnWidths(const QHeaderView* header);
void RestoreColumnWidths(QHeaderView* header, const QJsonArray& widths);

} // namespace War3Map::UiKit
