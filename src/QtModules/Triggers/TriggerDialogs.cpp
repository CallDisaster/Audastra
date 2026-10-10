#include "TriggerDialogs.h"

#include "TriggerCommands.h"
#include "TriggerUiText.h"

#include "War3MapCore/Trigger/TriggerPresentation.h"
#include "War3MapCore/Trigger/TriggerTypes.h"
#include "War3MapCore/Trigger/UiCatalog.h"

#include <QAbstractItemView>
#include <QCheckBox>
#include <QCollator>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QHash>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSize>
#include <QSizePolicy>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QTableWidget>
#include <QTextBrowser>
#include <QTimer>
#include <QTreeView>
#include <QUndoStack>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <cctype>
#include <memory>
#include <optional>
#include <utility>

namespace {

using TriggerUiText::CallReturnType;
using TriggerUiText::Field;
using TriggerUiText::PlaceholderLabels;
using TriggerUiText::Utf8;

const War3Map::Trigger::EcaNode* FindEca(
    const War3Map::Trigger::TriggerDocument& document,
    War3Map::StableId id) {
    const auto found = std::find_if(
        document.ecaNodes.begin(), document.ecaNodes.end(),
        [id](const auto& value) { return value.id == id; });
    return found == document.ecaNodes.end() ? nullptr : &*found;
}

// 函数选择器：列表项用该角色存放 UiDefinition 指针。
constexpr int kDefinitionRole = Qt::UserRole + 1;
const QString kPickerSizeKey = QStringLiteral("triggers/functionPicker/size");
const QString kPickerSplitterKey = QStringLiteral("triggers/functionPicker/splitter");

// 详情里的自然语言模板：${参数} 显示为带下划线的参数名，与 WE 的函数说明写法一致。
QString DescriptionHtml(const QString& raw) {
    static const QRegularExpression placeholder(QStringLiteral(R"(\$\{([^}]*)\})"));
    QString html;
    qsizetype cursor = 0;
    for (auto it = placeholder.globalMatch(raw); it.hasNext();) {
        const auto match = it.next();
        html += raw.mid(cursor, match.capturedStart() - cursor).toHtmlEscaped();
        html += QStringLiteral("<u>%1</u>").arg(match.captured(1).toHtmlEscaped());
        cursor = match.capturedEnd();
    }
    return html + raw.mid(cursor).toHtmlEscaped();
}

} // namespace

UiDefinitionPickerDialog::UiDefinitionPickerDialog(
    const War3Map::Trigger::UiCatalog& catalog,
    War3Map::Trigger::UiDefinitionKind kind,
    std::string expectedReturnType,
    TriggerFunctionList::CategoryIcon categoryIcon,
    QWidget* parent)
    : QDialog(parent),
      catalog_(catalog),
      kind_(kind),
      expectedReturnType_(std::move(expectedReturnType)),
      functions_(catalog, std::move(categoryIcon)) {
    setWindowTitle(tr("选择函数"));
    setModal(true);
    setSizeGripEnabled(true);
    setMinimumSize(640, 420);
    resize(1100, 720);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(18, 18, 18, 18);
    layout->setSpacing(12);
    auto* filters = new QHBoxLayout;
    filters->setSpacing(8);
    category_ = new QComboBox;
    category_->setObjectName("definitionCategory");
    category_->setMinimumWidth(220);
    category_->setMaxVisibleItems(24);
    filters->addWidget(category_);
    search_ = new QLineEdit;
    search_->setObjectName("dialogSearch");
    search_->setClearButtonEnabled(true);
    search_->setPlaceholderText(tr("搜索函数名、分类、原始函数名或来源层…"));
    filters->addWidget(search_, 1);
    layout->addLayout(filters);

    splitter_ = new QSplitter;
    splitter_->setChildrenCollapsible(false);
    definitionModel_ = new QStandardItemModel(this);
    definitions_ = new QTreeView;
    definitions_->setObjectName("definitionList");
    definitions_->setModel(definitionModel_);
    definitions_->setHeaderHidden(true);
    definitions_->setRootIsDecorated(false);
    definitions_->setAlternatingRowColors(true);
    definitions_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    definitions_->setSelectionMode(QAbstractItemView::SingleSelection);
    definitions_->setUniformRowHeights(true);
    definitions_->setTextElideMode(Qt::ElideMiddle);
    definitions_->setIconSize(QSize(16, 16));
    splitter_->addWidget(definitions_);
    details_ = new QTextBrowser;
    details_->setObjectName("definitionDetails");
    splitter_->addWidget(details_);
    splitter_->setStretchFactor(0, 3);
    splitter_->setStretchFactor(1, 2);
    splitter_->setSizes({640, 420});
    layout->addWidget(splitter_, 1);

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("插入所选函数"));
    buttons->button(QDialogButtonBox::Ok)->setObjectName("primaryButton");
    buttons->button(QDialogButtonBox::Ok)->setEnabled(false);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    // 上次调整过的尺寸与分栏（本机设置）；没有记录时用上面的默认值。
    const QSettings settings;
    if (const QSize saved = settings.value(kPickerSizeKey).toSize(); saved.isValid()) {
        resize(saved.expandedTo(minimumSize()));
    }
    splitter_->restoreState(settings.value(kPickerSplitterKey).toByteArray());

    // 候选：按返回类型兼容规则过滤（YDWE 的 AnyReturnType 函数在任何位置可用），按列表标题排序一次。
    const War3Map::Trigger::TriggerTypeSystem types(catalog_);
    for (const auto* definition : catalog_.Definitions(kind_)) {
        if (kind_ == War3Map::Trigger::UiDefinitionKind::Call && !expectedReturnType_.empty() &&
            (CallReturnType(*definition).empty() || !types.Accepts(expectedReturnType_, CallReturnType(*definition)))) {
            continue;
        }
        candidates_.push_back(definition);
    }
    functions_.Sort(candidates_);
    BuildCategoryFilter();

    connect(search_, &QLineEdit::textChanged, this, [this]() { RebuildList(); });
    connect(category_, &QComboBox::currentIndexChanged, this, [this]() { RebuildList(); });
    connect(definitions_->selectionModel(), &QItemSelectionModel::currentChanged,
            this, [this, buttons](const QModelIndex&, const QModelIndex&) {
        ShowCurrentDefinition();
        buttons->button(QDialogButtonBox::Ok)->setEnabled(SelectedDefinition() != nullptr);
    });
    connect(definitions_, &QTreeView::doubleClicked, this, [this](const QModelIndex&) {
        if (SelectedDefinition() != nullptr) {
            accept();
        }
    });
    RebuildList();
    search_->setFocus();
}

void UiDefinitionPickerDialog::done(int result) {
    QSettings settings;
    settings.setValue(kPickerSizeKey, size());
    settings.setValue(kPickerSplitterKey, splitter_->saveState());
    QDialog::done(result);
}

const War3Map::Trigger::UiDefinition* UiDefinitionPickerDialog::SelectedDefinition() const {
    if (definitions_ == nullptr || definitionModel_ == nullptr) {
        return nullptr;
    }
    const QModelIndex current = definitions_->currentIndex();
    if (!current.isValid()) {
        return nullptr;
    }
    const QVariant roleData = definitionModel_->data(current, kDefinitionRole);
    if (!roleData.isValid()) {
        return nullptr;
    }
    return reinterpret_cast<const War3Map::Trigger::UiDefinition*>(roleData.value<quintptr>());
}

// 分类筛选：“全部”加上候选中实际出现的分类（带图标与数量），按分类名排序。
void UiDefinitionPickerDialog::BuildCategoryFilter() {
    QHash<QString, int> counts;
    for (const auto* definition : candidates_) {
        ++counts[functions_.CategoryId(*definition)];
    }
    std::vector<std::pair<QString, QString>> entries; // 名称, id
    for (auto it = counts.cbegin(); it != counts.cend(); ++it) {
        entries.emplace_back(functions_.CategoryName(it.key()), it.key());
    }
    QCollator collator(QLocale(QLocale::Chinese, QLocale::China));
    std::sort(entries.begin(), entries.end(), [&collator](const auto& left, const auto& right) {
        const int order = collator.compare(left.first, right.first);
        return order != 0 ? order < 0 : left.second < right.second;
    });
    const QSignalBlocker blocker(category_);
    category_->clear();
    category_->addItem(tr("全部分类  (%1)").arg(candidates_.size()), QVariant());
    for (const auto& [name, id] : entries) {
        category_->addItem(functions_.Icon(id), QStringLiteral("%1  (%2)").arg(name).arg(counts.value(id)), id);
        category_->setItemData(category_->count() - 1, id.isEmpty() ? tr("没有 category 字段的函数") : id, Qt::ToolTipRole);
    }
}

void UiDefinitionPickerDialog::RebuildList() {
    definitionModel_->clear();
    const QString query = search_->text().trimmed();
    const QVariant categoryData = category_->currentData();
    const bool allCategories = !categoryData.isValid();
    const QString category = categoryData.toString();
    for (const auto* definition : candidates_) {
        if (!allCategories && functions_.CategoryId(*definition) != category) {
            continue;
        }
        if (!functions_.Matches(*definition, query)) {
            continue;
        }
        definitionModel_->appendRow(MakeDefinitionItem(definition));
    }
    if (definitionModel_->rowCount() == 0) {
        details_->setHtml(tr("<h3>没有匹配项</h3><p>请调整搜索词或分类，或检查当前 UI 数据根是否安装了对应库。</p>"));
        return;
    }
    definitions_->setCurrentIndex(definitionModel_->index(0, 0));
}

QStandardItem* UiDefinitionPickerDialog::MakeDefinitionItem(
    const War3Map::Trigger::UiDefinition* definition) const {
    auto* item = new QStandardItem(functions_.Icon(*definition), functions_.Title(*definition));
    item->setEditable(false);
    item->setData(
        QVariant::fromValue(reinterpret_cast<quintptr>(definition)),
        kDefinitionRole);
    item->setToolTip(
        functions_.Title(*definition) + QLatin1Char('\n') +
        Utf8(definition->name) + QStringLiteral("  ·  ") +
        Utf8(definition->Active().layer));
    return item;
}

void UiDefinitionPickerDialog::ShowCurrentDefinition() {
    const auto* definition = SelectedDefinition();
    if (definition == nullptr) {
        return;
    }
    const auto description = Field(definition->Active(), "description");
    const auto comment = Field(definition->Active(), "comment");
    const QString categoryId = functions_.CategoryId(*definition);
    details_->setHtml(tr(
        "<h2>%1</h2><p class='logic'>%2</p>"
        "<hr><p><b>分类</b><br>%3</p>"
        "<p><b>原始函数名</b><br>%4</p>"
        "<p><b>签名</b><br>%5</p>"
        "<p><b>来源</b><br>%6 · %7</p>"
        "<p>%8</p>")
        .arg(functions_.Title(*definition).toHtmlEscaped())
        .arg(description.has_value() ? DescriptionHtml(Utf8(*description)) : tr("无自然语言模板"))
        .arg((categoryId.isEmpty() ? functions_.CategoryName(categoryId)
            : QStringLiteral("%1（%2）").arg(functions_.CategoryName(categoryId), categoryId)).toHtmlEscaped())
        .arg(Utf8(definition->name).toHtmlEscaped())
        .arg(Utf8(definition->Signature()).toHtmlEscaped())
        .arg(Utf8(definition->Active().layer).toHtmlEscaped())
        .arg(QString::fromStdWString(definition->Active().source.wstring()).toHtmlEscaped())
        .arg(comment.has_value() ? Utf8(*comment).toHtmlEscaped() : QString{}));
}

struct TriggerParameterDialog::Row {
    War3Map::StableId argumentId = 0;
    std::string expectedType;
    std::int32_t originalKind = 3;
    std::string originalValue;
    QWidget* panel = nullptr;
    QComboBox* kind = nullptr;
    QStackedWidget* valueStack = nullptr;
    QComboBox* preset = nullptr;
    QLineEdit* scalar = nullptr;
    QComboBox* booleanValue = nullptr;
    bool booleanLiteral = false;
    QLabel* function = nullptr;
    QPushButton* functionButton = nullptr;
    std::optional<std::string> selectedCall;
    std::optional<War3Map::StableId> nestedEcaId;
    bool readOnly = false;
};

TriggerParameterDialog::TriggerParameterDialog(
    const War3Map::Trigger::TriggerDocument& document,
    const War3Map::Trigger::UiCatalog& catalog,
    War3Map::StableId ecaId,
    QWidget* parent)
    : QDialog(parent), document_(document), catalog_(catalog), ecaId_(ecaId) {
    const auto* eca = FindEca(document_, ecaId_);
    if (eca == nullptr) {
        return;
    }
    const auto* uiDefinition = catalog_.Find(eca->type, eca->name.text);
    setWindowTitle(tr("编辑参数 · %1").arg(Utf8(eca->name.text)));
    resize(900, 640);
    setModal(true);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 18, 20, 18);
    layout->setSpacing(12);
    auto* title = new QLabel(
        uiDefinition == nullptr ? Utf8(eca->name.text) : Utf8(War3Map::Trigger::DefinitionTitle(catalog_, *uiDefinition)));
    title->setObjectName("dialogTitle");
    layout->addWidget(title);
    naturalPreview_ = new QLabel;
    naturalPreview_->setObjectName("logicPreview");
    naturalPreview_->setWordWrap(true);
    naturalPreview_->setTextFormat(Qt::RichText);
    naturalPreview_->setTextInteractionFlags(
        Qt::TextBrowserInteraction | Qt::LinksAccessibleByMouse);
    naturalPreview_->setOpenExternalLinks(false);
    connect(naturalPreview_, &QLabel::linkActivated, this, [this](const QString& link) {
        const QUrl url(link);
        if (url.scheme() != QLatin1String("param")) {
            return;
        }
        const QString path = url.path(); // e.g. "row/3"
        const int slash = path.indexOf(QLatin1Char('/'));
        if (slash < 0) {
            return;
        }
        bool ok = false;
        const int rowIndex = path.mid(slash + 1).toInt(&ok);
        if (ok && rowIndex >= 0 && rowIndex < static_cast<int>(rows_.size())) {
            FlashRow(rows_[static_cast<std::size_t>(rowIndex)]);
        }
    });
    layout->addWidget(naturalPreview_);

    scroll_ = new QScrollArea;
    scroll_->setWidgetResizable(true);
    scroll_->setFrameShape(QFrame::NoFrame);
    auto* content = new QWidget;
    auto* form = new QVBoxLayout(content);
    form->setContentsMargins(0, 4, 6, 4);
    form->setSpacing(8);
    scroll_->setWidget(content);
    layout->addWidget(scroll_, 1);

    std::vector<const War3Map::Trigger::UiArgumentDefinition*> definitions;
    if (uiDefinition != nullptr) {
        for (const auto& definition : uiDefinition->Active().arguments) {
            if (definition.type != "nothing") {
                definitions.push_back(&definition);
            }
        }
    }
    const auto labels = uiDefinition == nullptr
        ? std::vector<std::string>{}
        : PlaceholderLabels(*uiDefinition);
    // 期望类型按类型系统解析（Null 跟随类型选择器或前一个参数）。
    const War3Map::Trigger::TriggerTypeSystem types(catalog_);
    std::vector<War3Map::Trigger::LocalVariable> locals;
    for (const auto& trigger : document_.triggers) {
        if (trigger.id == eca->sourceTriggerId) locals = types.CollectLocals(document_, trigger);
    }
    for (std::size_t position = 0; position < eca->arguments.size(); ++position) {
        const std::uint32_t argumentIndex = eca->arguments[position];
        if (argumentIndex >= document_.argumentNodes.size()) {
            continue;
        }
        const auto* definition = position < definitions.size() ? definitions[position] : nullptr;
        const std::string label = position < labels.size() && !labels[position].empty()
            ? labels[position]
            : "参数 " + std::to_string(position + 1);
        AddArgumentRow(position, definition, label, document_.argumentNodes[argumentIndex]);
        rows_.back()->expectedType = types.ExpectedType(document_, *eca, position, locals);
        RefreshPresets(rows_.back(), document_.argumentNodes[argumentIndex]);
        RestrictKinds(rows_.back(), types);
        form->addWidget(rows_.back()->panel);
    }
    form->addStretch();
    UpdateNaturalPreview();
    resize(900, std::clamp(
        340 + static_cast<int>(rows_.size()) * 76,
        430,
        700));

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Save)->setText(tr("应用参数"));
    buttons->button(QDialogButtonBox::Save)->setObjectName("primaryButton");
    connect(buttons, &QDialogButtonBox::accepted, this, &TriggerParameterDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &TriggerParameterDialog::reject);
    layout->addWidget(buttons);
}

TriggerParameterDialog::~TriggerParameterDialog() {
    for (Row* row : rows_) {
        delete row;
    }
}

void TriggerParameterDialog::AddArgumentRow(
    std::size_t position,
    const War3Map::Trigger::UiArgumentDefinition* definition,
    std::string label,
    const War3Map::Trigger::ArgumentNode& argument) {
    (void)position;
    auto* row = new Row;
    row->argumentId = argument.id;
    row->expectedType = definition == nullptr ? std::string{} : definition->type;
    row->originalKind = argument.type;
    row->originalValue = argument.value.text;

    auto* panel = new QWidget;
    panel->setObjectName("parameterRow");
    panel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    panel->setFixedHeight(68);
    auto* layout = new QHBoxLayout(panel);
    layout->setContentsMargins(12, 10, 12, 10);
    layout->setSpacing(10);
    auto* name = new QLabel(
        Utf8(label) + (row->expectedType.empty()
            ? QString{}
            : QStringLiteral("\n") + Utf8(row->expectedType)));
    name->setObjectName("parameterName");
    name->setMinimumWidth(180);
    layout->addWidget(name);

    row->kind = new QComboBox(panel);
    row->kind->setObjectName("parameterValueKind");
    row->kind->setMinimumWidth(110);
    row->kind->addItem(tr("预设值"), 0);
    row->kind->addItem(tr("变量"), 1);
    row->kind->addItem(tr("直接值"), 3);
    row->kind->addItem(tr("函数"), 2);
    layout->addWidget(row->kind);

    row->valueStack = new QStackedWidget(panel);
    row->valueStack->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    row->valueStack->setFixedHeight(38);
    row->preset = new QComboBox;
    row->preset->setObjectName("parameterPresetValue");
    row->preset->setEditable(true);
    row->scalar = new QLineEdit;
    row->scalar->setClearButtonEnabled(true);
    row->booleanValue = new QComboBox;
    row->booleanValue->setObjectName("parameterBooleanValue");
    row->booleanValue->addItem(tr("真"), QStringLiteral("true"));
    row->booleanValue->addItem(tr("假"), QStringLiteral("false"));
    auto* functionPanel = new QWidget;
    auto* functionLayout = new QHBoxLayout(functionPanel);
    functionLayout->setContentsMargins(0, 0, 0, 0);
    row->function = new QLabel(tr("尚未选择函数"));
    row->function->setObjectName("functionValue");
    row->functionButton = new QPushButton(tr("选择函数…"));
    functionLayout->addWidget(row->function, 1);
    functionLayout->addWidget(row->functionButton);
    row->valueStack->addWidget(row->preset);
    row->valueStack->addWidget(row->scalar);
    row->valueStack->addWidget(functionPanel);
    row->valueStack->addWidget(row->booleanValue);
    layout->addWidget(row->valueStack, 1);

    const int kindIndex = row->kind->findData(argument.insertCall != 0 ? 2 : argument.type);
    row->kind->setCurrentIndex(kindIndex >= 0 ? kindIndex : row->kind->findData(3));
    row->scalar->setText(Utf8(argument.value.text));

    if (argument.insertCall != 0 && argument.callEca.has_value() &&
        *argument.callEca < document_.ecaNodes.size()) {
        const auto& call = document_.ecaNodes[*argument.callEca];
        row->selectedCall = call.name.text;
        row->nestedEcaId = call.id;
        row->function->setText(Utf8(PresentEca(document_, call, catalog_).text));
        row->functionButton->setText(tr("编辑函数参数"));
        // 种类仍可改：换成预设 / 变量 / 直接值时，应用时先摘除原函数（可撤销）。
    }
    if (argument.insertIndex != 0) {
        row->readOnly = true;
        row->kind->setEnabled(false);
        row->scalar->setReadOnly(true);
        row->preset->setEnabled(false);
        row->functionButton->setEnabled(false);
        row->scalar->setText(tr("数组索引参数（只读，请在数组编辑器中修改）"));
    }

    const auto updateStack = [row]() {
        const int valueKind = row->kind->currentData().toInt();
        row->valueStack->setCurrentIndex(valueKind == 0 ? 0 : valueKind == 2 ? 2
            : valueKind == 3 && row->booleanLiteral ? 3 : 1);
    };
    connect(row->kind, &QComboBox::currentIndexChanged, panel, [this, updateStack]() {
        updateStack();
        UpdateNaturalPreview();
    });
    connect(row->preset, &QComboBox::currentTextChanged, panel, [this]() {
        UpdateNaturalPreview();
    });
    connect(row->scalar, &QLineEdit::textChanged, panel, [this]() {
        UpdateNaturalPreview();
    });
    connect(row->booleanValue, &QComboBox::currentIndexChanged, panel, [this]() {
        UpdateNaturalPreview();
    });
    connect(row->functionButton, &QPushButton::clicked, panel, [this, row]() {
        if (row->nestedEcaId.has_value()) {
            requestedNestedEca_ = row->nestedEcaId;
            accept();
            return;
        }
        UiDefinitionPickerDialog picker(
            catalog_,
            War3Map::Trigger::UiDefinitionKind::Call,
            row->expectedType,
            categoryIcons_,
            this);
        if (picker.exec() != QDialog::Accepted || picker.SelectedDefinition() == nullptr) {
            return;
        }
        row->selectedCall = picker.SelectedDefinition()->name;
        row->function->setText(TriggerFunctionList(catalog_).Title(*picker.SelectedDefinition()));
        row->kind->setCurrentIndex(row->kind->findData(2));
        UpdateNaturalPreview();
    });
    row->panel = panel;
    updateStack();
    rows_.push_back(row);
}

// 按函数库定义去掉该类型不能使用的取值种类；当前已用的种类保留，已有数据不会被隐藏。
void TriggerParameterDialog::RestrictKinds(Row* row, const War3Map::Trigger::TriggerTypeSystem& types) {
    if (row->readOnly) return;
    const auto kinds = types.AllowedValueKinds(row->expectedType);
    const int current = row->kind->currentData().toInt();
    const auto drop = [row, current](int kind, bool allowed) {
        if (allowed || kind == current) return;
        if (const int index = row->kind->findData(kind); index >= 0) row->kind->removeItem(index);
    };
    drop(0, kinds.preset || row->booleanLiteral);
    drop(1, kinds.variable);
    drop(2, kinds.function);
    if (row->booleanLiteral && current == 3) row->valueStack->setCurrentIndex(3);
}

void TriggerParameterDialog::RefreshPresets(Row* row, const War3Map::Trigger::ArgumentNode& argument) {
    const QSignalBlocker blocker(row->preset);
    row->preset->clear();
    const War3Map::Trigger::TriggerTypeSystem types(catalog_);
    const auto presets = TriggerUiText::PresetsFor(catalog_, row->expectedType);
    // 与取值面板使用同一类型规则：boolean 原语始终可选，派生专用枚举保留自身预设。
    row->booleanLiteral = !row->readOnly &&
        types.LiteralKind(row->expectedType) == War3Map::Trigger::TriggerTypeSystem::Literal::Boolean &&
        (row->expectedType == "boolean" || presets.empty());
    if (row->booleanLiteral) {
        for (int index = 0; index < row->booleanValue->count(); ++index) {
            row->preset->addItem(row->booleanValue->itemText(index), row->booleanValue->itemData(index));
            row->preset->setItemData(row->preset->count() - 1, 3, Qt::UserRole + 1);
        }
        const QString original = Utf8(argument.value.text);
        int index = row->booleanValue->findData(original);
        if (index < 0 && argument.type == 3) {
            row->booleanValue->addItem(original, original); // 未知旧值保持原样，不隐式改成真。
            index = row->booleanValue->count() - 1;
        }
        if (index >= 0) row->booleanValue->setCurrentIndex(index);
        if (argument.type == 3) row->preset->setCurrentIndex(row->preset->findData(original));
    }
    for (const auto& preset : presets) {
        row->preset->addItem(preset.display, Utf8(preset.name));
        row->preset->setItemData(row->preset->count() - 1, tr("原始预设：%1").arg(Utf8(preset.name)), Qt::ToolTipRole);
    }
    if (argument.type == 0) {
        int presetIndex = -1;
        for (int index = 0; index < row->preset->count(); ++index) {
            if (row->preset->itemData(index).toString() == Utf8(argument.value.text) &&
                row->preset->itemData(index, Qt::UserRole + 1).toInt() == 0) { presetIndex = index; break; }
        }
        if (presetIndex >= 0) {
            row->preset->setCurrentIndex(presetIndex);
        } else {
            row->preset->addItem(Utf8(argument.value.text), Utf8(argument.value.text));
            row->preset->setCurrentIndex(row->preset->count() - 1);
        }
    }
}

void TriggerParameterDialog::UpdateNaturalPreview() {
    if (naturalPreview_ == nullptr) {
        return;
    }
    const auto* eca = FindEca(document_, ecaId_);
    const auto* definition = eca == nullptr
        ? nullptr
        : catalog_.Find(eca->type, eca->name.text);
    const auto description = definition == nullptr
        ? std::optional<std::string_view>{}
        : Field(definition->Active(), "description");
    if (!description.has_value() || description->empty()) {
        naturalPreview_->setText(
            tr("实时预览<br>%1").arg(eca == nullptr
                ? tr("节点不存在")
                : Utf8(PresentEca(document_, *eca, catalog_).text).toHtmlEscaped()));
        return;
    }

    // 构建富文本：将 ${...} 占位符替换为可点击链接。
    const QString raw = Utf8(*description);
    QString html;
    qsizetype cursor = 0;
    int rowIdx = 0;
    while (cursor < raw.size()) {
        const qsizetype start = raw.indexOf(QStringLiteral("${"), cursor);
        if (start < 0) {
            html += raw.mid(cursor).toHtmlEscaped();
            break;
        }
        const qsizetype end = raw.indexOf(QLatin1Char('}'), start + 2);
        if (end < 0) {
            html += raw.mid(cursor).toHtmlEscaped();
            break;
        }
        // 追加占位符前的普通文本
        html += raw.mid(cursor, start - cursor).toHtmlEscaped();
        // 获取当前行值
        QString value;
        if (rowIdx < static_cast<int>(rows_.size())) {
            const Row* row = rows_[static_cast<std::size_t>(rowIdx)];
            const int valueKind = row->kind->currentData().toInt();
            if (valueKind == 0) {
                value = row->preset->currentText();
            } else if (valueKind == 2) {
                value = row->function->text();
            } else if (valueKind == 3 && row->booleanLiteral) {
                value = row->booleanValue->currentText();
            } else {
                value = row->scalar->text();
            }
        }
        if (value.isEmpty()) {
            value = tr("〈未填写〉");
        }
        html += QStringLiteral("<a href=\"param://row/%1\" style=\"color:#e8b84b;"
            "text-decoration:none;\">%2</a>")
            .arg(rowIdx).arg(value.toHtmlEscaped());
        cursor = end + 1;
        ++rowIdx;
    }
    naturalPreview_->setText(tr("实时预览<br>%1").arg(html));
}

void TriggerParameterDialog::FlashRow(Row* row) {
    if (row == nullptr || row->panel == nullptr) {
        return;
    }
    if (scroll_ != nullptr) {
        scroll_->ensureWidgetVisible(row->panel, 40, 40);
    }
    row->panel->setStyleSheet(QStringLiteral(
        "QWidget#parameterRow { background: #2a2410; border: 1px solid #e8b84b; "
        "border-radius: 4px; }"));
    QTimer::singleShot(1500, row->panel, [panel = row->panel]() {
        panel->setStyleSheet(QString());
    });
}

std::vector<TriggerArgumentEditRequest> TriggerParameterDialog::PendingEdits() const {
    std::vector<TriggerArgumentEditRequest> result;
    for (const Row* row : rows_) {
        if (row->readOnly) {
            continue;
        }
        TriggerArgumentEditRequest request;
        request.argumentId = row->argumentId;
        request.valueKind = row->kind->currentData().toInt();
        request.expectedType = row->expectedType;
        if (request.valueKind == 0) {
            const int index = row->preset->currentIndex();
            if (index >= 0) request.valueKind = row->preset->itemData(index, Qt::UserRole + 1).toInt();
            request.value = index >= 0 && row->preset->itemData(index).isValid()
                ? row->preset->itemData(index).toString().toUtf8().toStdString()
                : row->preset->currentText().toUtf8().toStdString();
        } else if (request.valueKind == 2) {
            request.callFunction = row->selectedCall;
        } else if (request.valueKind == 3 && row->booleanLiteral) {
            request.value = row->booleanValue->currentData().toString().toUtf8().toStdString();
        } else {
            request.value = row->scalar->text().toUtf8().toStdString();
        }
        result.push_back(std::move(request));
    }
    return result;
}

std::optional<War3Map::StableId> TriggerParameterDialog::RequestedNestedEca() const noexcept {
    return requestedNestedEca_;
}

void TriggerParameterDialog::accept() {
    for (const Row* row : rows_) {
        if (row->kind->currentData().toInt() == 2 && !row->selectedCall.has_value()) {
            QMessageBox::warning(
                this,
                tr("尚未选择函数"),
                tr("函数型参数必须先从当前 UI Catalog 选择一个返回类型匹配的函数。"));
            return;
        }
    }
    QDialog::accept();
}
