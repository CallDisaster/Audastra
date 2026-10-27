#pragma once

#include <QAbstractItemView>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QObject>
#include <QPushButton>
#include <QString>
#include <QStringList>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <cstddef>
#include <initializer_list>

// 界面描述面板（数值、状态、事件、模拟）共用的表格：列定义、表头、单元与“上方按钮 + 表格”的页面。
namespace War3Map::Modules::GameUiTables {

struct Column {
    const char* key;
    const char* label;  // QT_TR_NOOP
};

template <std::size_t N>
QString ColumnKey(const Column (&columns)[N], int column) {
    return column >= 0 && column < static_cast<int>(N) ? QLatin1String(columns[column].key) : QString();
}

template <std::size_t N>
QString ColumnLabel(const Column (&columns)[N], int column) {
    return column >= 0 && column < static_cast<int>(N) ? QObject::tr(columns[column].label) : QString();
}

// raw 为真时表头写原始字段名。
template <std::size_t N>
void SetHeaders(QTableWidget* table, const Column (&columns)[N], bool raw) {
    QStringList labels;
    for (const auto& column : columns) labels.push_back(raw ? QLatin1String(column.key) : QObject::tr(column.label));
    table->setHorizontalHeaderLabels(labels);
}

template <std::size_t N>
QTableWidget* MakeTable(QWidget* parent, const char* name, const Column (&columns)[N]) {
    auto* table = new QTableWidget(0, static_cast<int>(N), parent);
    table->setObjectName(QLatin1String(name));
    table->verticalHeader()->setVisible(false);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    table->horizontalHeader()->setStretchLastSection(true);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::SelectedClicked |
        QAbstractItemView::EditKeyPressed | QAbstractItemView::AnyKeyPressed);
    SetHeaders(table, columns, false);
    return table;
}

inline QPushButton* Button(QWidget* parent, const char* name, const QString& text) {
    auto* button = new QPushButton(text, parent);
    button->setObjectName(QLatin1String(name));
    return button;
}

// 表格页：上方一行按钮，下方表格。
inline QWidget* TablePage(QWidget* parent, QTableWidget* table, std::initializer_list<QPushButton*> buttons) {
    auto* page = new QWidget(parent);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 4, 0, 0);
    layout->setSpacing(4);
    auto* row = new QHBoxLayout;
    for (auto* button : buttons) {
        button->setParent(page);
        row->addWidget(button);
    }
    row->addStretch(1);
    layout->addLayout(row);
    table->setParent(page);
    layout->addWidget(table, 1);
    return page;
}

inline QTableWidgetItem* Cell(const QString& text, bool editable = true) {
    auto* item = new QTableWidgetItem(text);
    if (!editable) item->setFlags(item->flags() & ~Qt::ItemIsEditable);
    if (!text.isEmpty()) item->setToolTip(text);
    return item;
}

inline QTableWidgetItem* CheckCell(bool checked) {
    auto* item = new QTableWidgetItem;
    item->setFlags((item->flags() | Qt::ItemIsUserCheckable) & ~Qt::ItemIsEditable);
    item->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
    return item;
}

// 列宽按内容，过长的（贴图路径等）收在上限内，悬停看全文。
inline void FitColumns(QTableWidget* table) {
    table->resizeColumnsToContents();
    table->resizeRowsToContents();
    for (int column = 0; column < table->columnCount(); ++column) {
        table->setColumnWidth(column, std::min(table->columnWidth(column), 180));
    }
}

} // namespace War3Map::Modules::GameUiTables
