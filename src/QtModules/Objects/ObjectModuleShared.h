#pragma once

#include "ObjectModels.h"

#include "War3MapCore/ObjectData/ObjectTypes.h"

#include <QFrame>
#include <QHeaderView>
#include <QRegularExpression>
#include <QSortFilterProxyModel>
#include <QString>
#include <QStringList>
#include <QTableView>

#include <optional>
#include <string_view>

// 物编模块内部共享的小工具：页面构建（ObjectModule.cpp）与字段展示（ObjectFieldsView.cpp）共用。
namespace War3Map::Modules::ObjectDetail {

inline QString U8(std::string_view text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

inline QString Kind(ObjectData::ObjectType type) {
    return U8(ObjectData::ToString(type));
}

inline constexpr int kFieldCategoryRole = Qt::UserRole + 41;

// 原版字段表的筛选：分类 + 显示名 / 原始字段名搜索。
class StandardFieldFilter final : public QSortFilterProxyModel {
public:
    explicit StandardFieldFilter(QObject* parent) : QSortFilterProxyModel(parent) {}
    void SetCategory(const QString& category) {
        category_ = category;
        invalidateFilter();
    }

protected:
    bool filterAcceptsRow(int row, const QModelIndex& parent) const override {
        const auto key = sourceModel()->index(row, 0, parent);
        return (category_.isEmpty() || key.data(kFieldCategoryRole).toString() == category_)
            && (QSortFilterProxyModel::filterAcceptsRow(row, parent)
                || filterRegularExpression().match(key.data(Qt::UserRole).toString()).hasMatch());
    }

private:
    QString category_;
};

inline void ConfigureTable(QTableView* table) {
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setAlternatingRowColors(false);
    table->verticalHeader()->hide();
    table->verticalHeader()->setDefaultSectionSize(34);
    table->setFrameShape(QFrame::NoFrame);
    table->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    table->horizontalHeader()->setStretchLastSection(true);
    table->setShowGrid(false);
}

} // namespace War3Map::Modules::ObjectDetail
