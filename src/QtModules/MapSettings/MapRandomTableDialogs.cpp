#include "MapRandomTableDialogs.h"

#include <QComboBox>
#include <QCompleter>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QStyledItemDelegate>
#include <QTableWidget>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace War3Map::Modules {

QString SettingsRawText(const MapData::SettingsRawcode& id) {
    QString text;
    for (const auto byte : id) text.append(QChar(static_cast<char16_t>(static_cast<unsigned char>(byte))));
    return text;
}

std::optional<MapData::SettingsRawcode> SettingsRawFromText(const QString& text) {
    if (text.size() != 4) return std::nullopt;
    MapData::SettingsRawcode id{};
    for (int i = 0; i < 4; ++i) {
        const auto unit = text.at(i).unicode();
        if (unit < 0x20 || unit > 0x7e) return std::nullopt;
        id[static_cast<std::size_t>(i)] = static_cast<std::byte>(unit);
    }
    return id;
}

QString RandomColumnTypeName(std::int32_t type) {
    return type == 0 ? QObject::tr("单位") : type == 1 ? QObject::tr("建筑") : type == 2 ? QObject::tr("物品") : QObject::tr("未知 %1").arg(type);
}

namespace {

constexpr int RawRole = Qt::UserRole + 1;

// 对象格：编辑时才建可搜索的选择框（首项“无”），提交写回原始代码与显示名。
class ObjectCellDelegate final : public QStyledItemDelegate {
public:
    using ChoicesFor = std::function<const std::vector<RandomObjectChoice>*(const QModelIndex&)>;
    ObjectCellDelegate(ChoicesFor choices, RandomObjectName nameOf, QObject* parent)
        : QStyledItemDelegate(parent), choices_(std::move(choices)), nameOf_(std::move(nameOf)) {}

    QWidget* createEditor(QWidget* parent, const QStyleOptionViewItem&, const QModelIndex& index) const override {
        const auto* choices = choices_(index);
        if (!choices) return nullptr;
        auto* box = new QComboBox(parent);
        box->setObjectName(QStringLiteral("randomObjectEditor"));
        box->setEditable(true);
        box->setInsertPolicy(QComboBox::NoInsert);
        box->completer()->setFilterMode(Qt::MatchContains);
        box->completer()->setCompletionMode(QCompleter::PopupCompletion);
        box->addItem(tr("无"), QString{});
        for (const auto& choice : *choices) box->addItem(choice.display, choice.raw);
        return box;
    }
    void setEditorData(QWidget* editor, const QModelIndex& index) const override {
        auto* box = static_cast<QComboBox*>(editor);
        const QString raw = index.data(RawRole).toString();
        int found = box->findData(raw);
        if (found < 0) { // 原有代码（如等级占位）不在候选里时原样保留
            box->insertItem(1, raw, raw);
            found = 1;
        }
        box->setCurrentIndex(found);
    }
    void setModelData(QWidget* editor, QAbstractItemModel* model, const QModelIndex& index) const override {
        auto* box = static_cast<QComboBox*>(editor);
        int current = box->currentIndex();
        if (current < 0 || box->currentText() != box->itemText(current)) current = box->findText(box->currentText());
        if (current < 0) return; // 手输的文字不是候选：保留原值
        Write(model, index, box->itemData(current).toString());
    }
    void Write(QAbstractItemModel* model, const QModelIndex& index, const QString& raw) const {
        const auto id = SettingsRawFromText(raw);
        model->setData(index, raw, RawRole);
        model->setData(index, raw.isEmpty() || !id ? tr("无") : nameOf_(*id), Qt::DisplayRole);
        model->setData(index, raw, Qt::ToolTipRole);
    }

private:
    ChoicesFor choices_;
    RandomObjectName nameOf_;
};

class ChanceDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QWidget* createEditor(QWidget* parent, const QStyleOptionViewItem&, const QModelIndex&) const override {
        auto* spin = new QSpinBox(parent);
        spin->setObjectName(QStringLiteral("randomChanceEditor"));
        spin->setRange(0, 100);
        spin->setSuffix(QStringLiteral("%"));
        return spin;
    }
    // 概率格显示为百分数；物品组标题等非数字文字原样显示。
    QString displayText(const QVariant& value, const QLocale& locale) const override {
        bool number = false;
        const int chance = value.toString().toInt(&number);
        return number ? QStringLiteral("%1%").arg(chance) : QStyledItemDelegate::displayText(value, locale);
    }
    void setEditorData(QWidget* editor, const QModelIndex& index) const override {
        static_cast<QSpinBox*>(editor)->setValue(index.data().toInt());
    }
    void setModelData(QWidget* editor, QAbstractItemModel* model, const QModelIndex& index) const override {
        model->setData(index, QString::number(static_cast<QSpinBox*>(editor)->value()), Qt::DisplayRole);
    }
};

QLabel* ProblemLabel() {
    auto* problem = new QLabel;
    problem->setObjectName(QStringLiteral("formProblem"));
    problem->setWordWrap(true);
    problem->hide();
    return problem;
}

void ShowProblem(QLabel* label, const QString& problem) {
    label->setText(problem);
    label->setVisible(!problem.isEmpty());
}

} // namespace

RandomUnitTableDialog::RandomUnitTableDialog(const std::optional<MapData::MapRandomUnitTable>& current, const QString& shownName,
    std::int32_t number, const RandomObjectChoices& choices, RandomObjectName nameOf, Submit submit, QWidget* parent)
    : QDialog(parent), creating_(!current), initialName_(shownName), nameOf_(std::move(nameOf)), submit_(std::move(submit)) {
    setObjectName(QStringLiteral("randomUnitTableDialog"));
    setWindowTitle(creating_ ? tr("添加随机单位组") : tr("修改随机单位组"));
    setMinimumSize(560, 420);
    auto* layout = new QVBoxLayout(this);
    auto* form = new QFormLayout;
    name_ = new QLineEdit(shownName);
    name_->setObjectName(QStringLiteral("randomTableName"));
    form->addRow(tr("名称"), name_);
    number_ = new QSpinBox;
    number_->setObjectName(QStringLiteral("randomTableNumber"));
    number_->setRange(0, 1 << 30);
    number_->setValue(current ? current->number : number);
    number_->setFixedWidth(96);
    number_->setEnabled(creating_); // 编号是触发器与摆放引用的身份，不在修改时改
    form->addRow(tr("编号"), number_);
    auto* columns = new QHBoxLayout;
    columnTypes_ = new QHBoxLayout;
    columns->addLayout(columnTypes_);
    auto* addColumn = new QPushButton(tr("添加列"));
    addColumn->setObjectName(QStringLiteral("randomAddColumn"));
    auto* removeColumn = new QPushButton(tr("删除列"));
    removeColumn->setObjectName(QStringLiteral("randomRemoveColumn"));
    for (auto* button : {addColumn, removeColumn}) {
        button->setVisible(creating_); // 已有表的列结构变动需要完整引用覆盖，不在此开放
        columns->addWidget(button);
    }
    columns->addStretch(1);
    form->addRow(tr("列"), columns);
    layout->addLayout(form);

    rows_ = new QTableWidget(0, 1);
    rows_->setObjectName(QStringLiteral("randomUnitRows"));
    rows_->verticalHeader()->hide();
    rows_->verticalHeader()->setDefaultSectionSize(28);
    rows_->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    rows_->setSelectionBehavior(QAbstractItemView::SelectItems);
    rows_->setItemDelegateForColumn(0, new ChanceDelegate(rows_));
    rows_->setItemDelegate(new ObjectCellDelegate([this, &choices](const QModelIndex& index) -> const std::vector<RandomObjectChoice>* {
        const int column = index.column() - 1;
        if (column < 0 || column >= static_cast<int>(typeBoxes_.size())) return nullptr;
        return typeBoxes_[static_cast<std::size_t>(column)]->currentData().toInt() == 2 ? &choices.items : &choices.units;
    }, nameOf_, rows_));
    layout->addWidget(rows_, 1);
    auto* rowButtons = new QHBoxLayout;
    auto* addRow = new QPushButton(tr("添加行"));
    addRow->setObjectName(QStringLiteral("randomAddRow"));
    auto* removeRow = new QPushButton(tr("删除行"));
    removeRow->setObjectName(QStringLiteral("randomRemoveRow"));
    rowButtons->addWidget(addRow);
    rowButtons->addWidget(removeRow);
    rowButtons->addStretch(1);
    total_ = new QLabel;
    total_->setObjectName(QStringLiteral("randomChanceTotal"));
    rowButtons->addWidget(total_);
    layout->addLayout(rowButtons);
    problem_ = ProblemLabel();
    layout->addWidget(problem_);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    layout->addWidget(buttons);

    for (const auto type : current ? current->columnTypes : std::vector<std::int32_t>{0}) AddColumn(type);
    if (current) for (const auto& row : current->rows) AddRow(row);
    else AddRow({100, {MapData::SettingsRawcode{}}});
    SyncHeaders();
    UpdateTotal();
    connect(addColumn, &QPushButton::clicked, this, [this] { AddColumn(0); SyncHeaders(); });
    connect(removeColumn, &QPushButton::clicked, this, [this] {
        if (typeBoxes_.size() <= 1) return;
        delete typeBoxes_.back();
        typeBoxes_.pop_back();
        rows_->removeColumn(rows_->columnCount() - 1);
        SyncHeaders();
    });
    connect(addRow, &QPushButton::clicked, this, [this] {
        AddRow({0, std::vector<MapData::SettingsRawcode>(typeBoxes_.size())});
        UpdateTotal();
    });
    connect(removeRow, &QPushButton::clicked, this, [this] {
        if (rows_->currentRow() >= 0) rows_->removeRow(rows_->currentRow());
        UpdateTotal();
    });
    connect(rows_, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* item) { if (item->column() == 0) UpdateTotal(); });
    connect(buttons, &QDialogButtonBox::accepted, this, &RandomUnitTableDialog::Accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

void RandomUnitTableDialog::AddColumn(std::int32_t type) {
    auto* box = new QComboBox;
    box->setObjectName(QStringLiteral("randomColumnType%1").arg(typeBoxes_.size()));
    for (int value = 0; value <= 2; ++value) box->addItem(RandomColumnTypeName(value), value);
    if (type < 0 || type > 2) box->addItem(RandomColumnTypeName(type), type);
    box->setCurrentIndex(box->findData(type));
    box->setEnabled(creating_);
    columnTypes_->addWidget(box);
    typeBoxes_.push_back(box);
    connect(box, &QComboBox::currentIndexChanged, this, [this] { SyncHeaders(); });
    const int column = rows_->columnCount();
    rows_->insertColumn(column);
    for (int row = 0; row < rows_->rowCount(); ++row) {
        auto* item = new QTableWidgetItem(tr("无"));
        item->setData(RawRole, QString{});
        rows_->setItem(row, column, item);
    }
}

void RandomUnitTableDialog::AddRow(const MapData::MapRandomUnitRow& row) {
    const int index = rows_->rowCount();
    rows_->insertRow(index);
    rows_->setItem(index, 0, new QTableWidgetItem(QString::number(row.chance)));
    for (int column = 1; column < rows_->columnCount(); ++column) {
        const auto position = static_cast<std::size_t>(column - 1);
        const auto id = position < row.ids.size() ? row.ids[position] : MapData::SettingsRawcode{};
        const bool none = id == MapData::SettingsRawcode{};
        auto* item = new QTableWidgetItem(none ? tr("无") : nameOf_(id));
        item->setData(RawRole, none ? QString{} : SettingsRawText(id));
        if (!none) item->setToolTip(SettingsRawText(id));
        rows_->setItem(index, column, item);
    }
}

void RandomUnitTableDialog::SyncHeaders() {
    QStringList headers{tr("概率")};
    for (const auto* box : typeBoxes_) headers.append(box->currentText());
    rows_->setHorizontalHeaderLabels(headers);
    // 对象列等宽分摊，概率列固定窄宽。
    rows_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    rows_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Fixed);
    rows_->setColumnWidth(0, 72);
}

void RandomUnitTableDialog::UpdateTotal() {
    int total = 0;
    for (int row = 0; row < rows_->rowCount(); ++row) total += rows_->item(row, 0) ? rows_->item(row, 0)->text().toInt() : 0;
    total_->setText(tr("概率合计 %1").arg(total));
}

void RandomUnitTableDialog::Accept() {
    MapData::MapRandomUnitTable table;
    table.number = number_->value();
    const bool keepName = !creating_ && name_->text() == initialName_;
    table.name = name_->text().toUtf8().toStdString();
    for (const auto* box : typeBoxes_) table.columnTypes.push_back(box->currentData().toInt());
    for (int row = 0; row < rows_->rowCount(); ++row) {
        MapData::MapRandomUnitRow entry;
        entry.chance = rows_->item(row, 0) ? rows_->item(row, 0)->text().toInt() : 0;
        for (int column = 1; column < rows_->columnCount(); ++column) {
            const QString raw = rows_->item(row, column) ? rows_->item(row, column)->data(RawRole).toString() : QString{};
            const auto id = raw.isEmpty() ? std::optional{MapData::SettingsRawcode{}} : SettingsRawFromText(raw);
            if (!id) return ShowProblem(problem_, tr("第 %1 行的对象代码无效。").arg(row + 1));
            entry.ids.push_back(*id);
        }
        table.rows.push_back(std::move(entry));
    }
    const QString problem = submit_(table, keepName);
    if (problem.isEmpty()) {
        accept();
        return;
    }
    ShowProblem(problem_, problem);
}

RandomItemTableDialog::RandomItemTableDialog(const std::optional<MapData::MapRandomItemTable>& current, const QString& shownName,
    std::int32_t number, const RandomObjectChoices& choices, RandomObjectName nameOf, Submit submit, QWidget* parent)
    : QDialog(parent), creating_(!current), initialName_(shownName), nameOf_(std::move(nameOf)), submit_(std::move(submit)) {
    setObjectName(QStringLiteral("randomItemTableDialog"));
    setWindowTitle(creating_ ? tr("添加随机物品表") : tr("修改随机物品表"));
    setMinimumSize(520, 420);
    auto* layout = new QVBoxLayout(this);
    auto* form = new QFormLayout;
    name_ = new QLineEdit(shownName);
    name_->setObjectName(QStringLiteral("randomTableName"));
    form->addRow(tr("名称"), name_);
    number_ = new QSpinBox;
    number_->setObjectName(QStringLiteral("randomTableNumber"));
    number_->setRange(0, 1 << 30);
    number_->setValue(current ? current->number : number);
    number_->setFixedWidth(96);
    number_->setEnabled(creating_);
    form->addRow(tr("编号"), number_);
    layout->addLayout(form);
    sets_ = new QTreeWidget;
    sets_->setObjectName(QStringLiteral("randomItemSets"));
    sets_->setHeaderLabels({tr("物品组 / 概率"), tr("物品")});
    sets_->setColumnWidth(0, 160);
    sets_->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed | QAbstractItemView::AnyKeyPressed);
    sets_->setItemDelegateForColumn(0, new ChanceDelegate(sets_));
    sets_->setItemDelegateForColumn(1, new ObjectCellDelegate([&choices](const QModelIndex& index) -> const std::vector<RandomObjectChoice>* {
        return index.parent().isValid() ? &choices.items : nullptr;
    }, nameOf_, sets_));
    layout->addWidget(sets_, 1);
    auto* setButtons = new QHBoxLayout;
    auto* addSet = new QPushButton(tr("添加物品组"));
    addSet->setObjectName(QStringLiteral("randomAddSet"));
    auto* addItem = new QPushButton(tr("添加物品"));
    addItem->setObjectName(QStringLiteral("randomAddItem"));
    auto* remove = new QPushButton(tr("删除"));
    remove->setObjectName(QStringLiteral("randomRemoveEntry"));
    for (auto* button : {addSet, addItem, remove}) setButtons->addWidget(button);
    setButtons->addStretch(1);
    layout->addLayout(setButtons);
    problem_ = ProblemLabel();
    layout->addWidget(problem_);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    layout->addWidget(buttons);

    if (current) {
        for (const auto& set : current->sets) {
            AddSet();
            for (const auto& item : set) AddItem(item);
        }
    } else {
        AddSet();
        AddItem({100, MapData::SettingsRawcode{}});
    }
    sets_->expandAll();
    connect(addSet, &QPushButton::clicked, this, [this] { AddSet(); });
    connect(addItem, &QPushButton::clicked, this, [this] { AddItem({0, MapData::SettingsRawcode{}}); });
    connect(remove, &QPushButton::clicked, this, [this] {
        delete sets_->currentItem();
        for (int set = 0; set < sets_->topLevelItemCount(); ++set) sets_->topLevelItem(set)->setText(0, tr("物品组 %1").arg(set + 1));
    });
    connect(buttons, &QDialogButtonBox::accepted, this, &RandomItemTableDialog::Accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

void RandomItemTableDialog::AddSet() {
    auto* set = new QTreeWidgetItem(sets_, {tr("物品组 %1").arg(sets_->topLevelItemCount() + 1)});
    set->setFlags(set->flags() & ~Qt::ItemIsEditable);
    set->setExpanded(true);
    sets_->setCurrentItem(set);
}

void RandomItemTableDialog::AddItem(const MapData::MapRandomItem& item) {
    QTreeWidgetItem* set = sets_->currentItem();
    if (set && set->parent()) set = set->parent();
    if (!set) set = sets_->topLevelItemCount() ? sets_->topLevelItem(sets_->topLevelItemCount() - 1) : nullptr;
    if (!set) return;
    const bool none = item.id == MapData::SettingsRawcode{};
    auto* entry = new QTreeWidgetItem(set, {QString::number(item.chance), none ? tr("无") : nameOf_(item.id)});
    entry->setFlags(entry->flags() | Qt::ItemIsEditable);
    entry->setData(1, RawRole, none ? QString{} : SettingsRawText(item.id));
    if (!none) entry->setToolTip(1, SettingsRawText(item.id));
    set->setExpanded(true);
}

void RandomItemTableDialog::Accept() {
    MapData::MapRandomItemTable table;
    table.number = number_->value();
    const bool keepName = !creating_ && name_->text() == initialName_;
    table.name = name_->text().toUtf8().toStdString();
    for (int index = 0; index < sets_->topLevelItemCount(); ++index) {
        auto* node = sets_->topLevelItem(index);
        std::vector<MapData::MapRandomItem> set;
        for (int child = 0; child < node->childCount(); ++child) {
            const QString raw = node->child(child)->data(1, RawRole).toString();
            const auto id = raw.isEmpty() ? std::optional{MapData::SettingsRawcode{}} : SettingsRawFromText(raw);
            if (!id) return ShowProblem(problem_, tr("物品组 %1 的物品代码无效。").arg(index + 1));
            set.push_back({node->child(child)->text(0).toInt(), *id});
        }
        table.sets.push_back(std::move(set));
    }
    const QString problem = submit_(table, keepName);
    if (problem.isEmpty()) {
        accept();
        return;
    }
    ShowProblem(problem_, problem);
}

} // namespace War3Map::Modules
