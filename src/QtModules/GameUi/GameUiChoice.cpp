#include "GameUiChoice.h"

#include <QComboBox>
#include <QCompleter>
#include <QLineEdit>
#include <QSignalBlocker>
#include <QTimer>

#include <algorithm>

namespace War3Map::Modules {

GameUiChoiceDelegate::GameUiChoiceDelegate(std::function<GameUiOptions(const QModelIndex&)> options, QObject* parent,
    std::function<bool(const QModelIndex&)> freeText)
    : QStyledItemDelegate(parent), options_(std::move(options)), freeText_(std::move(freeText)) {}

QWidget* GameUiChoiceDelegate::createEditor(QWidget* parent, const QStyleOptionViewItem& option, const QModelIndex& index) const {
    const GameUiOptions options = options_ ? options_(index) : GameUiOptions{};
    if (options.isEmpty()) return QStyledItemDelegate::createEditor(parent, option, index);
    auto* box = new QComboBox(parent);
    box->setMaxVisibleItems(20);
    for (const auto& [label, data] : options) box->addItem(label, data);
    auto* self = const_cast<GameUiChoiceDelegate*>(this);
    const auto commit = [self, box] {
        emit self->commitData(box);
        emit self->closeEditor(box);
    };
    connect(box, &QComboBox::activated, self, commit);
    if (freeText_ && freeText_(index)) {
        box->setEditable(true);
        box->setInsertPolicy(QComboBox::NoInsert);
        box->completer()->setCompletionMode(QCompleter::PopupCompletion);
        box->completer()->setFilterMode(Qt::MatchContains);
        box->completer()->setCaseSensitivity(Qt::CaseInsensitive);
        // 列表外的输入按回车提交（列表里的项由 activated 提交）。
        connect(box->lineEdit(), &QLineEdit::returnPressed, self, [box, commit] {
            if (box->findText(box->currentText(), Qt::MatchFixedString) < 0) commit();
        });
    } else {
        QTimer::singleShot(0, box, &QComboBox::showPopup);
    }
    return box;
}

void GameUiChoiceDelegate::setEditorData(QWidget* editor, const QModelIndex& index) const {
    if (auto* box = qobject_cast<QComboBox*>(editor)) {
        const QVariant choice = index.data(kGameUiChoiceRole);
        const int found = box->findData(choice.isValid() ? choice : index.data(Qt::DisplayRole));
        if (box->isEditable() && found < 0) {
            box->setCurrentIndex(-1);
            box->setEditText(index.data(Qt::DisplayRole).toString());
        } else {
            box->setCurrentIndex(std::max(0, found));
        }
        if (box->isEditable()) box->lineEdit()->selectAll();
        return;
    }
    QStyledItemDelegate::setEditorData(editor, index);
}

void GameUiChoiceDelegate::setModelData(QWidget* editor, QAbstractItemModel* model, const QModelIndex& index) const {
    // 表格与树对多角色写入发信号的次数不同：先静默写取值，再写显示文字，只发一次修改且两者都是新值。
    if (const auto* box = qobject_cast<QComboBox*>(editor)) {
        QString label = box->currentText().trimmed();
        QVariant value = box->currentData();
        if (box->isEditable()) {
            // 可输入的单元：输入与某项文字相同即取该项，否则输入本身就是取值。
            const int found = box->findText(label, Qt::MatchFixedString);
            value = found >= 0 ? box->itemData(found) : QVariant(label);
            if (found >= 0) label = box->itemText(found);
        }
        {
            const QSignalBlocker blocker(model);
            model->setData(index, value, kGameUiChoiceRole);
        }
        model->setData(index, label, Qt::DisplayRole);
        return;
    }
    {
        // 普通文字编辑：清掉旧的取值，读取方按显示文字取。
        const QSignalBlocker blocker(model);
        model->setData(index, QVariant(), kGameUiChoiceRole);
    }
    QStyledItemDelegate::setModelData(editor, model, index);
}

QString GameUiChoiceLabel(const GameUiOptions& options, const QString& value) {
    for (const auto& [label, data] : options) {
        if (data == value) return label;
    }
    return value;
}

} // namespace War3Map::Modules
