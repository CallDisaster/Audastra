#pragma once

#include <QStyledItemDelegate>

#include <functional>

namespace War3Map::Modules {

// 玩家表“控制类型”“种族”列：单元格保存 W3I 整数，界面显示中文名称并用下拉框选择。
// 来源中的未知值显示为“未知（N）”并可原样保留；原始字段模式只显示数字。
class MapPlayerEnumDelegate final : public QStyledItemDelegate {
public:
    enum class Kind { Controller, Race };

    MapPlayerEnumDelegate(Kind kind, std::function<bool()> rawFields, QObject* parent);

    [[nodiscard]] static QString Label(Kind kind, int value);

    QString displayText(const QVariant& value, const QLocale& locale) const override;
    QWidget* createEditor(QWidget* parent, const QStyleOptionViewItem& option, const QModelIndex& index) const override;
    void setEditorData(QWidget* editor, const QModelIndex& index) const override;
    void setModelData(QWidget* editor, QAbstractItemModel* model, const QModelIndex& index) const override;

private:
    Kind kind_;
    std::function<bool()> rawFields_;
};

} // namespace War3Map::Modules
