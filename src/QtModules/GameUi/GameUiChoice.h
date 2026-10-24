#pragma once

#include <QList>
#include <QString>
#include <QStyledItemDelegate>

#include <functional>
#include <utility>

namespace War3Map::Modules {

// 显示文字、文件里的取值。
using GameUiOptions = QList<std::pair<QString, QString>>;
// 选择单元在文件里的取值（显示文字在 DisplayRole）。
inline constexpr int kGameUiChoiceRole = Qt::UserRole + 41;

// 选择单元：编辑时弹出列表，选中即提交，显示文字与取值一次写入；该单元没有可选项时用普通文字编辑。
// freeText 为真的单元可在列表外直接输入（例如数值或 编号[] 取值），列表可按输入筛选。
// 界面描述的表格与属性树共用。
class GameUiChoiceDelegate final : public QStyledItemDelegate {
public:
    GameUiChoiceDelegate(std::function<GameUiOptions(const QModelIndex&)> options, QObject* parent,
        std::function<bool(const QModelIndex&)> freeText = {});

    QWidget* createEditor(QWidget* parent, const QStyleOptionViewItem& option, const QModelIndex& index) const override;
    void setEditorData(QWidget* editor, const QModelIndex& index) const override;
    void setModelData(QWidget* editor, QAbstractItemModel* model, const QModelIndex& index) const override;

private:
    std::function<GameUiOptions(const QModelIndex&)> options_;
    std::function<bool(const QModelIndex&)> freeText_;
};

// 取值对应的显示文字；不在可选项里时原样显示。
[[nodiscard]] QString GameUiChoiceLabel(const GameUiOptions& options, const QString& value);

} // namespace War3Map::Modules
