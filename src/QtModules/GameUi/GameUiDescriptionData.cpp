#include "GameUiModule.h"

#include "GameUiAuthoringJson.h"
#include "GameUiChoice.h"

#include <QAbstractItemView>
#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonObject>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <utility>

// 界面描述的数值、状态与事件定义：在表格里直接改。每次修改把整个列表写回描述（数值改名用 rename_value，
// 同步框架里的取值与文字），与框架编辑走同一条检查、撤销与保存路径。
namespace War3Map::Modules {
namespace {

using namespace War3Map::GameUi;

QString Text(const std::string& text) { return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())); }

constexpr int kChoiceRole = kGameUiChoiceRole;

struct Column {
    const char* key;
    const char* label;
};

constexpr Column kValueColumns[] = {{"id", QT_TR_NOOP("编号")}, {"label", QT_TR_NOOP("显示名")}, {"type", QT_TR_NOOP("类型")},
    {"size", QT_TR_NOOP("格数")}, {"default", QT_TR_NOOP("默认值")}, {"min", QT_TR_NOOP("最小")}, {"max", QT_TR_NOOP("最大")}};
constexpr Column kStateColumns[] = {{"id", QT_TR_NOOP("编号")}, {"label", QT_TR_NOOP("显示名")}, {"set", QT_TR_NOOP("覆盖")}};
constexpr Column kOverrideColumns[] = {{"frame", QT_TR_NOOP("框架")}, {"texture", QT_TR_NOOP("贴图")}, {"text", QT_TR_NOOP("文字")},
    {"visible", QT_TR_NOOP("显示")}, {"value", QT_TR_NOOP("进度")}};
constexpr Column kEventColumns[] = {{"id", QT_TR_NOOP("编号")}, {"frame", QT_TR_NOOP("框架")}, {"on", QT_TR_NOOP("触发")}};

using Options = GameUiOptions;

template <std::size_t N>
QString ColumnKey(const Column (&columns)[N], int column) {
    return column >= 0 && column < static_cast<int>(N) ? QLatin1String(columns[column].key) : QString();
}

template <std::size_t N>
QString ColumnLabel(const Column (&columns)[N], int column) {
    return column >= 0 && column < static_cast<int>(N) ? QObject::tr(columns[column].label) : QString();
}

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

void SetChoices(QTableWidget* table, int column, std::function<Options()> options) {
    table->setItemDelegateForColumn(column,
        new GameUiChoiceDelegate([options = std::move(options)](const QModelIndex&) { return options(); }, table));
}

QPushButton* Button(QWidget* parent, const char* name, const QString& text) {
    auto* button = new QPushButton(text, parent);
    button->setObjectName(QLatin1String(name));
    return button;
}

// 表格页：上方一行按钮，下方表格。
QWidget* TablePage(QWidget* parent, QTableWidget* table, std::initializer_list<QPushButton*> buttons) {
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

QTableWidgetItem* Cell(const QString& text, bool editable = true) {
    auto* item = new QTableWidgetItem(text);
    if (!editable) item->setFlags(item->flags() & ~Qt::ItemIsEditable);
    if (!text.isEmpty()) item->setToolTip(text);
    return item;
}

QTableWidgetItem* ChoiceCell(const Options& options, const QString& value) {
    auto* item = Cell(GameUiChoiceLabel(options, value));
    item->setData(kChoiceRole, value);
    return item;
}

QTableWidgetItem* CheckCell(bool checked) {
    auto* item = new QTableWidgetItem;
    item->setFlags((item->flags() | Qt::ItemIsUserCheckable) & ~Qt::ItemIsEditable);
    item->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
    return item;
}

// 单元的取值：选择列为文件里的取值，其余为去掉首尾空白的文字。
QString CellValue(const QTableWidgetItem* item) {
    const QVariant choice = item->data(kChoiceRole);
    return choice.isValid() ? choice.toString() : item->text().trimmed();
}

QString Number(double value) { return QString::number(value, 'g', 15); }

// 列宽按内容，过长的（贴图路径等）收在上限内，悬停看全文。
void FitColumns(QTableWidget* table) {
    table->resizeColumnsToContents();
    table->resizeRowsToContents();
    for (int column = 0; column < table->columnCount(); ++column) {
        table->setColumnWidth(column, std::min(table->columnWidth(column), 180));
    }
}

Options ValueTypes(bool raw) {
    const auto label = [raw](const char* name, const QString& text) { return raw ? QString::fromLatin1(name) : text; };
    return {{label("real", QObject::tr("实数")), QStringLiteral("real")}, {label("integer", QObject::tr("整数")), QStringLiteral("integer")},
        {label("boolean", QObject::tr("布尔")), QStringLiteral("boolean")}, {label("string", QObject::tr("文字")), QStringLiteral("string")}};
}

Options EventKinds(bool raw) {
    const auto label = [raw](const char* name, const QString& text) { return raw ? QString::fromLatin1(name) : text; };
    return {{label("click", QObject::tr("点击")), QStringLiteral("click")}, {label("enter", QObject::tr("进入")), QStringLiteral("enter")},
        {label("leave", QObject::tr("离开")), QStringLiteral("leave")}};
}

Options Visibility() {
    return {{QObject::tr("不改"), QString()}, {QObject::tr("显示"), QStringLiteral("true")}, {QObject::tr("隐藏"), QStringLiteral("false")}};
}

void CollectFrames(const std::vector<UiFrame>& frames, bool raw, Options& options) {
    for (const auto& frame : frames) {
        options.push_back({raw || frame.label.empty() ? Text(frame.id) : QStringLiteral("%1  %2").arg(Text(frame.label), Text(frame.id)),
            Text(frame.id)});
        CollectFrames(frame.children, raw, options);
    }
}

// 列表里还没用过的编号：prefix 后接从 1 起最小的序号。
QString NextFreeId(const QJsonArray& items, const QString& prefix) {
    for (int n = 1;; ++n) {
        const QString id = prefix + QString::number(n);
        if (std::none_of(items.begin(), items.end(), [&](const QJsonValue& item) { return item.toObject().value("id").toString() == id; })) {
            return id;
        }
    }
}

// 单元文字写回字段：空为删除；numeric 时须是数值。
bool PutField(QJsonObject& object, const QString& key, const QString& text, bool numeric, QString* error) {
    if (text.isEmpty()) {
        object.remove(key);
        return true;
    }
    if (!numeric) {
        object.insert(key, text);
        return true;
    }
    bool ok = false;
    const double number = text.toDouble(&ok);
    if (!ok) {
        *error = QObject::tr("%1 不是数值").arg(text);
        return false;
    }
    object.insert(key, number);
    return true;
}

bool IsNumericType(const QString& type) { return type == QLatin1String("real") || type == QLatin1String("integer"); }

// 改数值类型：只有数值类型可有上下限；默认值能换算就换算（实数改整数取整），否则回到该类型的默认。
void SetValueType(QJsonObject& object, const QString& type) {
    const QString old = object.value("type").toString();
    object.insert("type", type);
    if (!IsNumericType(type)) {
        object.remove("min");
        object.remove("max");
    }
    if (!object.contains("default")) return;
    if (!IsNumericType(old) || !IsNumericType(type)) object.remove("default");
    else if (type == QLatin1String("integer")) object.insert("default", std::round(object.value("default").toDouble()));
}

// 改列表第 row 项。
std::function<bool(QJsonArray&, QString*)> AtRow(int row, std::function<bool(QJsonObject&, QString*)> change) {
    return [row, change = std::move(change)](QJsonArray& items, QString* error) {
        if (row < 0 || row >= items.size()) return false;
        QJsonObject item = items[row].toObject();
        if (!change(item, error)) return false;
        items[row] = item;
        return true;
    };
}

std::function<bool(QJsonArray&, QString*)> RemoveRow(int row) {
    return [row](QJsonArray& items, QString*) {
        if (row < 0 || row >= items.size()) return false;
        items.removeAt(row);
        return true;
    };
}

} // namespace

void GameUiModule::ChangeDescriptionList(const QString& list, const std::function<bool(QJsonArray&, QString*)>& change,
    const QString& what, std::function<void()> after) {
    const QString id = descriptionPanelFor_;
    const UiDescription* description = Description(id);
    if (!description) return;
    const QJsonObject document = DescriptionObject(*description);
    const bool overrides = list == QLatin1String("set");
    QJsonArray states = document.value(QStringLiteral("states")).toArray();
    if (overrides && (stateRow_ < 0 || stateRow_ >= states.size())) return;
    QJsonArray items = overrides ? states[stateRow_].toObject().value(list).toArray() : document.value(list).toArray();
    const QJsonArray original = items;
    QString error;
    if (!change(items, &error)) {
        if (!error.isEmpty()) {
            ReportAuthoring(error);
            QTimer::singleShot(0, this, [this] {
                descriptionPanelFor_.clear();
                UpdateDescriptionPanel();
            });
        }
        return;
    }
    if (items == original) return;  // 没有实际变化，不留撤销步骤
    QString key = list;
    QJsonValue payload = items;
    if (overrides) {
        QJsonObject state = states[stateRow_].toObject();
        if (items.isEmpty()) state.remove(list);
        else state.insert(list, items);
        states[stateRow_] = state;
        key = QStringLiteral("states");
        payload = states;
    }
    const QJsonObject edit{{"op", QStringLiteral("set_") + key}, {key, payload}};
    CommitDescriptionEdits(id, {edit}, tr("界面 %1：%2").arg(id, what), std::move(after));
}

void GameUiModule::BuildDescriptionData(QTabWidget* tabs) {
    // 当前选中、且属于面板所示描述的框架编号。
    const auto selectedFrame = [this]() -> QString {
        if (!tree_ || selected_ >= tree_->Nodes().size()) return {};
        const auto& link = tree_->Nodes()[selected_].uiFrame;
        return link && Text(link->description) == descriptionPanelFor_ ? Text(link->frame) : QString();
    };
    const auto frameOptions = [this] {
        Options options;
        if (const UiDescription* description = Description(descriptionPanelFor_)) CollectFrames(description->frames, descriptionRaw_, options);
        return options;
    };

    // 数值
    valueTable_ = MakeTable(tabs, "gameUiValueTable", kValueColumns);
    SetChoices(valueTable_, 2, [this] { return ValueTypes(descriptionRaw_); });
    auto* addValue = Button(tabs, "gameUiAddValue", tr("添加"));
    auto* removeValue = Button(tabs, "gameUiRemoveValue", tr("删除"));
    tabs->addTab(TablePage(tabs, valueTable_, {addValue, removeValue}), tr("数值"));
    connect(addValue, &QPushButton::clicked, this, [this] {
        ChangeDescriptionList(QStringLiteral("values"), [](QJsonArray& items, QString*) {
            items.append(QJsonObject{{"id", NextFreeId(items, QStringLiteral("value"))}, {"type", "real"}});
            return true;
        }, tr("添加数值"));
    });
    connect(removeValue, &QPushButton::clicked, this, [this] {
        const int row = valueTable_->currentRow();
        if (row >= 0) ChangeDescriptionList(QStringLiteral("values"), RemoveRow(row), tr("删除数值"));
    });
    connect(valueTable_, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* item) {
        const UiDescription* description = Description(descriptionPanelFor_);
        const int row = item->row();
        if (!description || row < 0 || static_cast<std::size_t>(row) >= description->values.size()) return;
        const UiValue& value = description->values[static_cast<std::size_t>(row)];
        const QString id = Text(value.id);
        const QString key = ColumnKey(kValueColumns, item->column());
        const QString text = CellValue(item);
        const QString what = tr("改数值 %1 的%2").arg(id, ColumnLabel(kValueColumns, item->column()));
        if (key == QLatin1String("id")) {
            if (text == id) return;
            const QJsonObject edit{{"op", "rename_value"}, {"from", id}, {"to", text}};
            CommitDescriptionEdits(descriptionPanelFor_, {edit}, tr("界面 %1：数值 %2 改名为 %3").arg(descriptionPanelFor_, id, text));
        } else if (key == QLatin1String("type")) {
            ChangeDescriptionList(QStringLiteral("values"), AtRow(row, [text](QJsonObject& object, QString*) {
                SetValueType(object, text);
                return true;
            }), what);
        } else if (key == QLatin1String("default") && value.type == UiValueType::Boolean) {
            const bool on = item->checkState() == Qt::Checked;
            ChangeDescriptionList(QStringLiteral("values"), AtRow(row, [on](QJsonObject& object, QString*) {
                if (on) object.insert("default", true);
                else object.remove("default");
                return true;
            }), what);
        } else {
            const bool numeric = key != QLatin1String("label") &&
                (key != QLatin1String("default") || value.type == UiValueType::Real || value.type == UiValueType::Integer);
            ChangeDescriptionList(QStringLiteral("values"),
                AtRow(row, [key, text, numeric](QJsonObject& object, QString* error) { return PutField(object, key, text, numeric, error); }),
                what);
        }
    });

    // 状态：上为状态，下为所选状态的覆盖。选中状态时预览切到该状态。
    stateTable_ = MakeTable(tabs, "gameUiStateTable", kStateColumns);
    overrideTable_ = MakeTable(tabs, "gameUiOverrideTable", kOverrideColumns);
    SetChoices(overrideTable_, 0, frameOptions);
    SetChoices(overrideTable_, 3, [] { return Visibility(); });
    auto* addState = Button(tabs, "gameUiAddState", tr("添加状态"));
    auto* removeState = Button(tabs, "gameUiRemoveState", tr("删除状态"));
    auto* addOverride = Button(tabs, "gameUiAddOverride", tr("添加覆盖"));
    auto* removeOverride = Button(tabs, "gameUiRemoveOverride", tr("删除覆盖"));
    auto* stateSplit = new QSplitter(Qt::Vertical, tabs);
    stateSplit->setChildrenCollapsible(false);
    stateSplit->addWidget(TablePage(stateSplit, stateTable_, {addState, removeState}));
    overridePage_ = TablePage(stateSplit, overrideTable_, {addOverride, removeOverride});
    stateSplit->addWidget(overridePage_);
    tabs->addTab(stateSplit, tr("状态"));
    connect(addState, &QPushButton::clicked, this, [this] {
        const UiDescription* description = Description(descriptionPanelFor_);
        if (!description) return;
        const QJsonArray states = DescriptionObject(*description).value(QStringLiteral("states")).toArray();
        const QString id = NextFreeId(states, QStringLiteral("state"));
        stateRow_ = static_cast<int>(states.size());
        ChangeDescriptionList(QStringLiteral("states"), [id](QJsonArray& items, QString*) {
            items.append(QJsonObject{{"id", id}});
            return true;
        }, tr("添加状态 %1").arg(id), [this, description = descriptionPanelFor_, id] { (void)SetDescriptionState(description, id); });
    });
    connect(removeState, &QPushButton::clicked, this, [this] {
        if (stateRow_ >= 0 && stateRow_ < stateTable_->rowCount()) {
            ChangeDescriptionList(QStringLiteral("states"), RemoveRow(stateRow_), tr("删除状态"));
        }
    });
    const auto pickState = [this](int row) {
        const UiDescription* description = Description(descriptionPanelFor_);
        if (!description || row < 0 || static_cast<std::size_t>(row) >= description->states.size()) return;
        if (row != stateRow_) {
            stateRow_ = row;
            FillStateOverrides();
        }
        const QString state = Text(description->states[static_cast<std::size_t>(row)].id);
        if (Text(DescriptionPreview(descriptionPanelFor_).state) != state) (void)SetDescriptionState(descriptionPanelFor_, state);
    };
    connect(stateTable_, &QTableWidget::currentCellChanged, this, pickState);
    connect(stateTable_, &QTableWidget::cellClicked, this, pickState);
    connect(stateTable_, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* item) {
        const UiDescription* description = Description(descriptionPanelFor_);
        const int row = item->row();
        if (!description || row < 0 || static_cast<std::size_t>(row) >= description->states.size()) return;
        const QString old = Text(description->states[static_cast<std::size_t>(row)].id);
        const QString key = ColumnKey(kStateColumns, item->column());
        const QString text = CellValue(item);
        std::function<void()> after;
        if (key == QLatin1String("id") && Text(DescriptionPreview(descriptionPanelFor_).state) == old) {
            after = [this, description = descriptionPanelFor_, text] { (void)SetDescriptionState(description, text); };
        }
        ChangeDescriptionList(QStringLiteral("states"),
            AtRow(row, [key, text](QJsonObject& object, QString* error) { return PutField(object, key, text, false, error); }),
            tr("改状态 %1 的%2").arg(old, ColumnLabel(kStateColumns, item->column())), std::move(after));
    });
    connect(addOverride, &QPushButton::clicked, this, [this, selectedFrame] {
        const UiDescription* description = Description(descriptionPanelFor_);
        if (!description || description->frames.empty()) return;
        const QString frameId = selectedFrame().isEmpty() ? Text(description->frames.front().id) : selectedFrame();
        const UiFrame* frame = FindUiFrame(*description, frameId.toStdString());
        if (!frame) return;
        // 先带上该框架最常改的一项，作者再改。
        QJsonObject entry{{"frame", frameId}};
        switch (frame->type) {
        case UiFrameType::Text: entry.insert("text", Text(frame->text)); break;
        case UiFrameType::Bar: entry.insert("value", 1); break;
        case UiFrameType::Backdrop:
        case UiFrameType::Button: entry.insert("texture", Text(frame->texture)); break;
        default: entry.insert("visible", true); break;
        }
        ChangeDescriptionList(QStringLiteral("set"), [entry](QJsonArray& items, QString*) {
            items.append(entry);
            return true;
        }, tr("添加 %1 的状态覆盖").arg(frameId));
    });
    connect(removeOverride, &QPushButton::clicked, this, [this] {
        const int row = overrideTable_->currentRow();
        if (row >= 0) ChangeDescriptionList(QStringLiteral("set"), RemoveRow(row), tr("删除状态覆盖"));
    });
    connect(overrideTable_, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* item) {
        const QString key = ColumnKey(kOverrideColumns, item->column());
        const QString text = CellValue(item);
        ChangeDescriptionList(QStringLiteral("set"), AtRow(item->row(), [key, text](QJsonObject& object, QString* error) {
            if (key != QLatin1String("visible")) return PutField(object, key, text, key == QLatin1String("value"), error);
            if (text.isEmpty()) object.remove(key);
            else object.insert(key, text == QLatin1String("true"));
            return true;
        }), tr("改状态覆盖的%1").arg(ColumnLabel(kOverrideColumns, item->column())));
    });

    // 事件
    eventTable_ = MakeTable(tabs, "gameUiEventTable", kEventColumns);
    SetChoices(eventTable_, 1, frameOptions);
    SetChoices(eventTable_, 2, [this] { return EventKinds(descriptionRaw_); });
    auto* addEvent = Button(tabs, "gameUiAddEvent", tr("添加"));
    auto* removeEvent = Button(tabs, "gameUiRemoveEvent", tr("删除"));
    tabs->addTab(TablePage(tabs, eventTable_, {addEvent, removeEvent}), tr("事件"));
    connect(addEvent, &QPushButton::clicked, this, [this, selectedFrame] {
        const UiDescription* description = Description(descriptionPanelFor_);
        if (!description || description->frames.empty()) return;
        const QString frameId = selectedFrame().isEmpty() ? Text(description->frames.front().id) : selectedFrame();
        const UiFrame* frame = FindUiFrame(*description, frameId.toStdString());
        if (!frame) return;
        const QString kind = frame->type == UiFrameType::Button ? QStringLiteral("click") : QStringLiteral("enter");
        ChangeDescriptionList(QStringLiteral("events"), [frameId, kind](QJsonArray& items, QString*) {
            items.append(QJsonObject{{"id", NextFreeId(items, QStringLiteral("event"))}, {"frame", frameId}, {"on", kind}});
            return true;
        }, tr("添加 %1 的事件").arg(frameId));
    });
    connect(removeEvent, &QPushButton::clicked, this, [this] {
        const int row = eventTable_->currentRow();
        if (row >= 0) ChangeDescriptionList(QStringLiteral("events"), RemoveRow(row), tr("删除事件"));
    });
    connect(eventTable_, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* item) {
        const QString key = ColumnKey(kEventColumns, item->column());
        const QString text = CellValue(item);
        ChangeDescriptionList(QStringLiteral("events"),
            AtRow(item->row(), [key, text](QJsonObject& object, QString* error) { return PutField(object, key, text, false, error); }),
            tr("改事件的%1").arg(ColumnLabel(kEventColumns, item->column())));
    });
}

void GameUiModule::UpdateDescriptionData(const UiDescription& description) {
    if (!valueTable_) return;
    const bool raw = descriptionRaw_;
    SetHeaders(valueTable_, kValueColumns, raw);
    SetHeaders(stateTable_, kStateColumns, raw);
    SetHeaders(overrideTable_, kOverrideColumns, raw);
    SetHeaders(eventTable_, kEventColumns, raw);
    Options frames;
    CollectFrames(description.frames, raw, frames);

    {
        const QSignalBlocker block(valueTable_);
        valueTable_->setRowCount(0);
        const Options types = ValueTypes(raw);
        for (const auto& value : description.values) {
            const int row = valueTable_->rowCount();
            valueTable_->insertRow(row);
            const bool numeric = value.type == UiValueType::Real || value.type == UiValueType::Integer;
            valueTable_->setItem(row, 0, Cell(Text(value.id)));
            valueTable_->setItem(row, 1, Cell(Text(value.label)));
            valueTable_->setItem(row, 2, ChoiceCell(types, Text(std::string(UiValueTypeName(value.type)))));
            valueTable_->setItem(row, 3, Cell(value.size ? QString::number(value.size) : QString()));
            valueTable_->setItem(row, 4, value.type == UiValueType::Boolean ? CheckCell(value.defaultValue == "true") : Cell(Text(value.defaultValue)));
            valueTable_->setItem(row, 5, Cell(value.min ? Number(*value.min) : QString(), numeric));
            valueTable_->setItem(row, 6, Cell(value.max ? Number(*value.max) : QString(), numeric));
        }
        FitColumns(valueTable_);
    }

    {
        const QSignalBlocker block(stateTable_);
        stateTable_->setRowCount(0);
        for (const auto& state : description.states) {
            const int row = stateTable_->rowCount();
            stateTable_->insertRow(row);
            stateTable_->setItem(row, 0, Cell(Text(state.id)));
            stateTable_->setItem(row, 1, Cell(Text(state.label)));
            stateTable_->setItem(row, 2, Cell(QString::number(state.overrides.size()), false));
        }
        FitColumns(stateTable_);
        stateRow_ = std::clamp(stateRow_, 0, std::max(0, stateTable_->rowCount() - 1));
    }
    SyncDescriptionStateRow(description, true);

    {
        const QSignalBlocker block(eventTable_);
        eventTable_->setRowCount(0);
        const Options kinds = EventKinds(raw);
        for (const auto& event : description.events) {
            const int row = eventTable_->rowCount();
            eventTable_->insertRow(row);
            eventTable_->setItem(row, 0, Cell(Text(event.id)));
            eventTable_->setItem(row, 1, ChoiceCell(frames, Text(event.frame)));
            eventTable_->setItem(row, 2, ChoiceCell(kinds,
                QLatin1String(event.kind == UiEventKind::Click ? "click" : event.kind == UiEventKind::Enter ? "enter" : "leave")));
        }
        FitColumns(eventTable_);
    }
}

// 预览切到某个状态时，状态页选中它并列出它的覆盖；基础状态时保持原选择。
void GameUiModule::SyncDescriptionStateRow(const UiDescription& description, bool refill) {
    const std::string& previewed = DescriptionPreview(Text(description.id)).state;
    for (std::size_t i = 0; i < description.states.size(); ++i) {
        if (description.states[i].id == previewed && static_cast<int>(i) != stateRow_) {
            stateRow_ = static_cast<int>(i);
            refill = true;
        }
    }
    if (stateTable_->rowCount() > 0) {
        const QSignalBlocker block(stateTable_);
        stateTable_->setCurrentCell(stateRow_, 0);
    }
    if (refill) FillStateOverrides();
}

void GameUiModule::FillStateOverrides() {
    const UiDescription* description = Description(descriptionPanelFor_);
    const bool valid = description && stateRow_ >= 0 && static_cast<std::size_t>(stateRow_) < description->states.size();
    overridePage_->setEnabled(valid);
    const QSignalBlocker block(overrideTable_);
    overrideTable_->setRowCount(0);
    if (!valid) return;
    Options frames;
    CollectFrames(description->frames, descriptionRaw_, frames);
    const Options visibility = Visibility();
    for (const auto& entry : description->states[static_cast<std::size_t>(stateRow_)].overrides) {
        const int row = overrideTable_->rowCount();
        overrideTable_->insertRow(row);
        overrideTable_->setItem(row, 0, ChoiceCell(frames, Text(entry.frame)));
        overrideTable_->setItem(row, 1, Cell(entry.texture ? Text(*entry.texture) : QString()));
        overrideTable_->setItem(row, 2, Cell(entry.text ? Text(*entry.text) : QString()));
        overrideTable_->setItem(row, 3, ChoiceCell(visibility,
            entry.visible ? (*entry.visible ? QStringLiteral("true") : QStringLiteral("false")) : QString()));
        overrideTable_->setItem(row, 4, Cell(entry.value ? Number(*entry.value) : QString()));
    }
    FitColumns(overrideTable_);
}

} // namespace War3Map::Modules
