#pragma once

#include <QString>

#include <functional>
#include <vector>

class QLineEdit;
class QTableView;

namespace War3Map::UiKit {

struct RowFilterCounts {
    int shown = 0;
    int total = 0;
};

// 按单元格显示文字筛选表格行：任一列包含 query（不区分大小写）即显示，query 为空时全部显示。
// 一次遍历整表；被隐藏的行同时取消选中。只在筛选词变化或整表重建完成后调用一次，
// 不能接在逐行插入或逐格变化的模型信号上（表格重建时会变成平方级开销，且行内容尚未填好）。
RowFilterCounts ApplyTableRowFilter(QTableView* table, const QString& query);

// 已选中且未被筛选隐藏的行，升序去重。Ctrl+A 与 Shift 范围选择会把隐藏行一起选上，
// 删除、复制等批量操作只能使用这里的结果。
[[nodiscard]] std::vector<int> VisibleSelectedRows(const QTableView* table);

// 当前行存在且未被隐藏时返回行号，否则返回 -1。
[[nodiscard]] int VisibleCurrentRow(const QTableView* table);

// 筛选输入框：停止输入 delayMs 毫秒后执行一次 apply（连续输入只遍历一次）；
// 清空或回车时立即执行，导航前清空筛选的调用方可同步看到全部行。
void BindRowFilterInput(QLineEdit* edit, std::function<void()> apply, int delayMs = 150);

} // namespace War3Map::UiKit
