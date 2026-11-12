#include "Widgets/TableRowFilter.h"

#include <QAbstractItemModel>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLineEdit>
#include <QTableView>
#include <QTimer>

#include <algorithm>

namespace War3Map::UiKit {

RowFilterCounts ApplyTableRowFilter(QTableView* table, const QString& query) {
    RowFilterCounts counts;
    if (!table || !table->model()) return counts;
    const QAbstractItemModel* model = table->model();
    const QModelIndex root = table->rootIndex();
    const int rows = model->rowCount(root);
    const int columns = model->columnCount(root);
    counts.total = rows;
    const QString needle = query.trimmed();
    // 无筛选词且没有隐藏行（整表重建后的常见情况）时不必遍历。
    if (needle.isEmpty() && table->verticalHeader()->hiddenSectionCount() == 0) {
        counts.shown = rows;
        return counts;
    }
    QItemSelectionModel* selection = table->selectionModel();
    QItemSelection deselect;
    int runStart = -1;
    // 相邻的待取消行合并成一个范围，最后一次性取消选中。
    const auto flush = [&](int end) {
        if (runStart < 0) return;
        deselect.select(model->index(runStart, 0, root), model->index(end, columns - 1, root));
        runStart = -1;
    };
    for (int row = 0; row < rows; ++row) {
        bool match = needle.isEmpty();
        for (int column = 0; !match && column < columns; ++column) {
            match = model->index(row, column, root).data(Qt::DisplayRole).toString()
                .contains(needle, Qt::CaseInsensitive);
        }
        if (table->isRowHidden(row) == match) table->setRowHidden(row, !match);
        if (match) ++counts.shown;
        const bool drop = !match && selection && columns > 0 && selection->rowIntersectsSelection(row, root);
        if (drop) {
            if (runStart < 0) runStart = row;
        } else {
            flush(row - 1);
        }
    }
    flush(rows - 1);
    if (selection && !deselect.isEmpty()) selection->select(deselect, QItemSelectionModel::Deselect);
    return counts;
}

std::vector<int> VisibleSelectedRows(const QTableView* table) {
    std::vector<int> rows;
    if (!table || !table->selectionModel()) return rows;
    for (const auto& index : table->selectionModel()->selectedRows()) {
        if (!table->isRowHidden(index.row())) rows.push_back(index.row());
    }
    std::sort(rows.begin(), rows.end());
    rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
    return rows;
}

int VisibleCurrentRow(const QTableView* table) {
    if (!table || !table->selectionModel()) return -1;
    const QModelIndex current = table->selectionModel()->currentIndex();
    if (!current.isValid() || table->isRowHidden(current.row())) return -1;
    return current.row();
}

void BindRowFilterInput(QLineEdit* edit, std::function<void()> apply, int delayMs) {
    if (!edit || !apply) return;
    auto* timer = new QTimer(edit);
    timer->setSingleShot(true);
    timer->setInterval(std::max(0, delayMs));
    QObject::connect(timer, &QTimer::timeout, edit, apply);
    QObject::connect(edit, &QLineEdit::textChanged, timer, [timer, apply](const QString& text) {
        if (!text.trimmed().isEmpty()) {
            timer->start();
            return;
        }
        timer->stop();
        apply();
    });
    QObject::connect(edit, &QLineEdit::returnPressed, timer, [timer, apply] {
        if (!timer->isActive()) return;
        timer->stop();
        apply();
    });
}

} // namespace War3Map::UiKit
