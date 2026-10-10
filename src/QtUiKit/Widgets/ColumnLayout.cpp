#include "ColumnLayout.h"

#include <QAbstractItemView>
#include <QHeaderView>

namespace War3Map::UiKit {

void ConfigureResizableColumns(QHeaderView* header, std::initializer_list<int> initialWidths) {
    if (header == nullptr) {
        return;
    }
    header->setSectionsMovable(false);
    header->setMinimumSectionSize(40);
    header->setStretchLastSection(true);
    header->setSectionResizeMode(QHeaderView::Interactive);
    int column = 0;
    for (const int width : initialWidths) {
        header->resizeSection(column++, width);
    }
    // 双击分隔线：按内容自适应（与资源管理器一致）。
    QObject::connect(header, &QHeaderView::sectionHandleDoubleClicked, header, [header](int section) {
        if (auto* view = qobject_cast<QAbstractItemView*>(header->parentWidget())) {
            const int hint = std::max(header->sectionSizeHint(section), view->sizeHintForColumn(section));
            header->resizeSection(section, hint + 12);
        }
    });
}

QJsonArray SaveColumnWidths(const QHeaderView* header) {
    QJsonArray widths;
    if (header == nullptr) {
        return widths;
    }
    for (int section = 0; section < header->count(); ++section) widths.append(header->sectionSize(section));
    return widths;
}

void RestoreColumnWidths(QHeaderView* header, const QJsonArray& widths) {
    if (header == nullptr || widths.size() != header->count()) {
        return;
    }
    // 最后一列由拉伸决定，不恢复。
    for (int section = 0; section + 1 < header->count(); ++section) {
        const int width = widths.at(section).toInt();
        if (width >= header->minimumSectionSize()) header->resizeSection(section, width);
    }
}

} // namespace War3Map::UiKit
