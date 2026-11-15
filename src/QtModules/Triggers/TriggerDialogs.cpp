#include "TriggerDialogs.h"

#include "TriggerCommands.h"
#include "TriggerUiText.h"

#include "War3MapCore/Trigger/TriggerPresentation.h"
#include "War3MapCore/Trigger/TriggerTypes.h"
#include "War3MapCore/Trigger/TriggerVariableReference.h"
#include "War3MapCore/Trigger/UiCatalog.h"

#include <QAbstractItemView>
#include <QButtonGroup>
#include <QCheckBox>
#include <QCollator>
#include <QComboBox>
#include <QCompleter>
#include <QDialogButtonBox>
#include <QHash>
#include <QFocusEvent>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QSettings>
#include <QScreen>
#include <QScrollArea>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSize>
#include <QSizePolicy>
#include <QSplitter>
#include <QStandardItemModel>
#include <QTableWidget>
#include <QTextBrowser>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>
#include <QUndoStack>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <cctype>
#include <functional>
#include <memory>
#include <optional>
#include <utility>

namespace {

// 变量下拉条目的显示名（条目文字可能带类型后缀）。
constexpr int kVariableNameRole = Qt::UserRole + 2;

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
    search_->setPlaceholderText(tr("搜索"));
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
            !TriggerFunctionList::Fits(types, expectedReturnType_, *definition)) {
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

namespace {

using Literal = War3Map::Trigger::TriggerTypeSystem::Literal;

// 首次展开或获得焦点时才填充候选（大图上变量、函数都很多）；只填一次，换类型后重新挂上。
class LazyComboBox final : public QComboBox {
public:
    using QComboBox::QComboBox;
    void SetFill(std::function<void()> fill) { fill_ = std::move(fill); }
    void showPopup() override {
        Fill();
        QComboBox::showPopup();
    }

protected:
    void focusInEvent(QFocusEvent* event) override {
        Fill();
        QComboBox::focusInEvent(event);
    }

private:
    void Fill() {
        if (!fill_) return;
        auto run = std::move(fill_);
        fill_ = nullptr;
        run();
    }
    std::function<void()> fill_;
};

// 可输入搜索的下拉：按包含匹配弹出候选，不把输入的文字插成新条目。
void MakeSearchable(QComboBox* combo) {
    combo->setEditable(true);
    combo->setInsertPolicy(QComboBox::NoInsert);
    combo->setMaxVisibleItems(16);
    combo->completer()->setCompletionMode(QCompleter::PopupCompletion);
    combo->completer()->setFilterMode(Qt::MatchContains);
    combo->completer()->setCaseSensitivity(Qt::CaseInsensitive);
}

std::size_t ArgumentCount(const War3Map::Trigger::UiDefinition& definition) {
    const auto& arguments = definition.Active().arguments;
    return static_cast<std::size_t>(std::count_if(arguments.begin(), arguments.end(),
        [](const auto& argument) { return argument.type != "nothing"; }));
}

// 参数位置上的函数定义：函数槽（事件/条件/动作）先查对应种类，其余为调用函数。
const War3Map::Trigger::UiDefinition* FindFunction(
    const War3Map::Trigger::UiCatalog& catalog, std::string_view expected, const std::string& name) {
    if (const auto slot = War3Map::Trigger::TriggerTypeSystem::FunctionSlotKind(expected)) {
        if (const auto* found = catalog.Find(*slot, name)) return found;
    }
    return catalog.Find(War3Map::Trigger::UiDefinitionKind::Call, name);
}

// 与 WE 相同：模型文件参数可以选模型，图像文件参数可以选图像；其余字符串只能手填。
std::optional<War3Map::Services::ObjectResourceKind> ResourceKindForType(std::string_view type) {
    if (type == "modelfile") return War3Map::Services::ObjectResourceKind::Model;
    if (type == "imagefile") return War3Map::Services::ObjectResourceKind::Texture;
    return std::nullopt;
}

} // namespace

struct TriggerParameterDialog::Row {
    War3Map::StableId argumentId = 0;
    std::string declaredType;
    std::string expectedType;
    bool selector = false;
    std::int32_t originalKind = 3;
    std::string originalValue;
    // 新语句的空参数（或标着函数却没挂函数）：按类型选默认来源。
    bool fresh = false;
    bool readOnly = false;
    bool anyGlobal = false;
    bool writableTarget = false;
    bool booleanLiteral = false;
    bool variablesLoaded = false;
    bool functionsLoaded = false;
    std::optional<std::string> originalCall;
    std::optional<War3Map::StableId> nestedEcaId;
    QString label;
    QWidget* panel = nullptr;
    QLabel* name = nullptr;
    QButtonGroup* sources = nullptr;
    std::array<QWidget*, 4> lines{};
    QComboBox* preset = nullptr;
    LazyComboBox* variable = nullptr;
    LazyComboBox* function = nullptr;
    QToolButton* functionBrowse = nullptr;
    QPushButton* functionEdit = nullptr;
    QLabel* callText = nullptr;
    QLineEdit* scalar = nullptr;
    QComboBox* booleanValue = nullptr;
    QPushButton* resourceBrowse = nullptr;
};

TriggerParameterDialog::TriggerParameterDialog(
    const War3Map::Trigger::TriggerDocument& document,
    const War3Map::Trigger::UiCatalog& catalog,
    War3Map::StableId ecaId,
    QWidget* parent)
    : QDialog(parent), document_(document), catalog_(catalog), ecaId_(ecaId), types_(catalog) {
    const auto* eca = FindEca(document_, ecaId_);
    if (eca == nullptr) {
        return;
    }
    const auto* uiDefinition = catalog_.Find(eca->type, eca->name.text);
    setWindowTitle(tr("编辑参数 · %1").arg(Utf8(eca->name.text)));
    setModal(true);
    setSizeGripEnabled(true);

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
    form->setSpacing(4);
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
    for (const auto& trigger : document_.triggers) {
        if (trigger.id == eca->sourceTriggerId) locals_ = types_.CollectLocals(document_, trigger);
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
        AddArgumentRow(definition, label, document_.argumentNodes[argumentIndex]);
        Row* row = rows_.back();
        row->declaredType = uiDefinition == nullptr
            ? std::string{} : War3Map::Trigger::TriggerTypeSystem::DeclaredType(*uiDefinition, position);
        row->selector = types_.IsTypeSelector(row->declaredType);
        row->expectedType = types_.ExpectedType(document_, *eca, position, locals_);
        // 赋值目标（AnyGlobal / 设置变量的左侧）只能是真实可写变量，与取值面板相同。
        row->anyGlobal = definition != nullptr && definition->type == "AnyGlobal";
        row->writableTarget = row->anyGlobal || (eca->name.text == "SetVariable" && position == 0);
        ConfigureRow(row, true);
        form->addWidget(row->panel);
    }
    // 新语句的类型选择器默认选了第一个预设，跟随它的参数随之定下类型。
    RecomputeTypes(true);
    form->addStretch();
    UpdateNaturalPreview();

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Save)->setText(tr("应用参数"));
    buttons->button(QDialogButtonBox::Save)->setObjectName("primaryButton");
    connect(buttons, &QDialogButtonBox::accepted, this, &TriggerParameterDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &TriggerParameterDialog::reject);
    layout->addWidget(buttons);
    resize(940, std::clamp(content->sizeHint().height() + 220, 430, 860));
}

TriggerParameterDialog::~TriggerParameterDialog() {
    for (Row* row : rows_) {
        delete row;
    }
}

void TriggerParameterDialog::SetCategoryIcons(TriggerFunctionList::CategoryIcon icons) {
    categoryIcons_ = std::move(icons);
    functions_.reset();
}

TriggerFunctionList& TriggerParameterDialog::Functions() {
    if (!functions_) functions_.emplace(catalog_, categoryIcons_);
    return *functions_;
}

void TriggerParameterDialog::AddArgumentRow(
    const War3Map::Trigger::UiArgumentDefinition* definition,
    std::string label,
    const War3Map::Trigger::ArgumentNode& argument) {
    auto* row = new Row;
    rows_.push_back(row);
    row->argumentId = argument.id;
    row->expectedType = definition == nullptr ? std::string{} : definition->type;
    row->originalKind = argument.type;
    row->originalValue = argument.value.text;
    row->label = Utf8(label);
    row->readOnly = argument.insertIndex != 0;
    row->fresh = !argument.callEca.has_value() &&
        ((argument.type == Value && argument.value.text.empty()) || argument.type == Function);
    if (argument.callEca.has_value() && *argument.callEca < document_.ecaNodes.size()) {
        const auto& call = document_.ecaNodes[*argument.callEca];
        row->originalCall = call.name.text;
        row->nestedEcaId = call.id;
    }

    auto* panel = new QWidget;
    panel->setObjectName("parameterRow");
    panel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto* layout = new QHBoxLayout(panel);
    layout->setContentsMargins(12, 8, 12, 8);
    layout->setSpacing(12);
    row->name = new QLabel;
    row->name->setObjectName("parameterName");
    row->name->setMinimumWidth(180);
    row->name->setMaximumWidth(240);
    row->name->setWordWrap(true);
    layout->addWidget(row->name, 0, Qt::AlignTop);
    auto* lines = new QVBoxLayout;
    lines->setSpacing(6);
    layout->addLayout(lines, 1);

    // 与 WE 相同：可用的来源同时列出，单选决定使用哪一个。
    row->sources = new QButtonGroup(panel);
    const auto addLine = [row, lines](int source, const QString& text, const char* objectName) {
        auto* line = new QWidget;
        auto* outer = new QVBoxLayout(line);
        outer->setContentsMargins(0, 0, 0, 0);
        outer->setSpacing(2);
        auto* inner = new QHBoxLayout;
        inner->setSpacing(8);
        outer->addLayout(inner);
        auto* radio = new QRadioButton(text);
        radio->setObjectName(QString::fromLatin1(objectName));
        radio->setMinimumWidth(56);
        row->sources->addButton(radio, source);
        inner->addWidget(radio);
        row->lines[static_cast<std::size_t>(source)] = line;
        lines->addWidget(line);
        return std::pair{outer, inner};
    };

    const auto presetLine = addLine(Preset, tr("预设"), "parameterSourcePreset");
    row->preset = new QComboBox;
    row->preset->setObjectName("parameterPresetValue");
    MakeSearchable(row->preset);
    presetLine.second->addWidget(row->preset, 1);

    const auto variableLine = addLine(Variable, tr("变量"), "parameterSourceVariable");
    row->variable = new LazyComboBox;
    row->variable->setObjectName("parameterVariableValue");
    MakeSearchable(row->variable);
    variableLine.second->addWidget(row->variable, 1);

    const auto functionLine = addLine(Function, tr("函数"), "parameterSourceFunction");
    row->function = new LazyComboBox;
    row->function->setObjectName("parameterFunctionValue");
    MakeSearchable(row->function);
    row->functionBrowse = new QToolButton;
    row->functionBrowse->setObjectName("parameterFunctionBrowse");
    row->functionBrowse->setText(QStringLiteral("…"));
    row->functionBrowse->setToolTip(tr("按分类选择函数"));
    row->functionEdit = new QPushButton(tr("编辑参数"));
    row->functionEdit->setObjectName("parameterFunctionEdit");
    functionLine.second->addWidget(row->function, 1);
    functionLine.second->addWidget(row->functionBrowse);
    functionLine.second->addWidget(row->functionEdit);
    row->callText = new QLabel;
    row->callText->setObjectName("functionValue");
    row->callText->setWordWrap(true);
    row->callText->setTextFormat(Qt::PlainText);
    row->callText->setIndent(64);
    functionLine.first->addWidget(row->callText);

    const auto valueLine = addLine(Value, tr("值"), "parameterSourceValue");
    row->scalar = new QLineEdit;
    row->scalar->setObjectName("parameterScalarValue");
    row->scalar->setClearButtonEnabled(true);
    row->booleanValue = new QComboBox;
    row->booleanValue->setObjectName("parameterBooleanValue");
    row->booleanValue->addItem(tr("真"), QStringLiteral("true"));
    row->booleanValue->addItem(tr("假"), QStringLiteral("false"));
    row->resourceBrowse = new QPushButton;
    row->resourceBrowse->setObjectName("parameterResourceBrowse");
    row->resourceBrowse->hide();
    valueLine.second->addWidget(row->scalar, 1);
    valueLine.second->addWidget(row->booleanValue, 1);
    valueLine.second->addWidget(row->resourceBrowse);
    if (argument.type == Value) row->scalar->setText(Utf8(argument.value.text));

    if (row->readOnly) {
        row->scalar->setText(tr("数组索引"));
        for (QWidget* line : row->lines) line->setEnabled(false);
    }

    // 在某一行里选值或输入时自动选中这一行的来源。
    const auto pick = [row](int source) { return [row, source]() { SelectSource(row, source); }; };
    const auto changed = [this]() {
        RecomputeTypes();
        UpdateNaturalPreview();
    };
    connect(row->sources, &QButtonGroup::idToggled, panel, [changed](int, bool checked) {
        if (checked) changed();
    });
    connect(row->preset, &QComboBox::activated, panel, pick(Preset));
    connect(row->preset->lineEdit(), &QLineEdit::textEdited, panel, pick(Preset));
    connect(row->preset, &QComboBox::currentTextChanged, panel, changed);
    connect(row->variable, &QComboBox::activated, panel, pick(Variable));
    connect(row->variable->lineEdit(), &QLineEdit::textEdited, panel, pick(Variable));
    connect(row->variable, &QComboBox::currentTextChanged, panel, changed);
    connect(row->function, &QComboBox::activated, panel, pick(Function));
    connect(row->function->lineEdit(), &QLineEdit::textEdited, panel, pick(Function));
    connect(row->function, &QComboBox::currentTextChanged, panel, [this, row, changed]() {
        UpdateFunctionLine(row);
        changed();
    });
    connect(row->scalar, &QLineEdit::textEdited, panel, pick(Value));
    connect(row->scalar, &QLineEdit::textChanged, panel, changed);
    connect(row->booleanValue, &QComboBox::activated, panel, pick(Value));
    connect(row->booleanValue, &QComboBox::currentIndexChanged, panel, changed);
    connect(row->functionBrowse, &QToolButton::clicked, panel, [this, row]() { BrowseFunction(row); });
    connect(row->resourceBrowse, &QPushButton::clicked, panel, [this, row]() { BrowseResource(row); });
    connect(row->functionEdit, &QPushButton::clicked, panel, [this, row]() {
        SelectSource(row, Function);
        // 原函数：打开它的参数；新选的函数：应用后接着打开它的参数（EditEcaParameters 的嵌套流程）。
        if (row->nestedEcaId.has_value() && FunctionName(row) == row->originalCall) {
            requestedNestedEca_ = row->nestedEcaId;
        }
        accept();
    });
    row->panel = panel;
}

void TriggerParameterDialog::ConfigureRow(Row* row, bool initial) {
    const QSignalBlocker presetBlocker(row->preset), variableBlocker(row->variable),
        functionBlocker(row->function), booleanBlocker(row->booleanValue), sourceBlocker(row->sources);
    const std::string type = row->expectedType;
    row->name->setText(type.empty() ? row->label
        : QString(row->label + QLatin1Char('\n') + Utf8(types_.DisplayName(type))));

    // 预设：换类型后保留仍然有效的选择；首次打开时选中原值（不在预设表里的旧值原样保留）。
    const QString keepPreset = initial
        ? (row->originalKind == Preset ? Utf8(row->originalValue) : QString{})
        : PresetValue(row);
    row->preset->clear();
    const auto presets = TriggerUiText::PresetsFor(catalog_, type);
    for (const auto& preset : presets) {
        row->preset->addItem(preset.display, Utf8(preset.name));
        row->preset->setItemData(row->preset->count() - 1, tr("原始预设：%1").arg(Utf8(preset.name)), Qt::ToolTipRole);
    }
    int presetIndex = keepPreset.isEmpty() ? -1 : row->preset->findData(keepPreset);
    if (presetIndex < 0 && initial && !keepPreset.isEmpty()) {
        row->preset->addItem(keepPreset, keepPreset);
        presetIndex = row->preset->count() - 1;
    }
    row->preset->setCurrentIndex(presetIndex);
    if (presetIndex < 0) row->preset->setEditText(QString{});

    // 值：布尔原语用真/假下拉，派生专用枚举保留自身预设（与取值面板同一规则）。
    const auto literal = types_.LiteralKind(type);
    row->booleanLiteral = !row->readOnly && literal == Literal::Boolean && (type == "boolean" || presets.empty());
    if (initial && row->originalKind == Value) {
        const QString original = Utf8(row->originalValue);
        int index = row->booleanValue->findData(original);
        if (index < 0 && row->booleanLiteral && !original.isEmpty()) {
            row->booleanValue->addItem(original, original); // 未知旧值保持原样，不隐式改成真。
            index = row->booleanValue->count() - 1;
        }
        row->booleanValue->setCurrentIndex(std::max(index, 0));
    }
    row->scalar->setVisible(!row->booleanLiteral);
    row->booleanValue->setVisible(row->booleanLiteral);
    const auto resource = ResourceKindForType(type);
    row->resourceBrowse->setVisible(!row->readOnly && resource.has_value() && resourcePickerFactory_ != nullptr);
    if (resource) row->resourceBrowse->setText(*resource == War3Map::Services::ObjectResourceKind::Model ? tr("选择模型…") : tr("选择图像…"));

    // 变量、函数候选按新类型重新列出（展开或获得焦点时填充）。
    if (!initial) {
        row->variable->clear();
        row->variable->setEditText(QString{});
        row->variablesLoaded = false;
        const auto name = FunctionName(row);
        const auto* definition = name ? FindFunction(catalog_, type, *name) : nullptr;
        if (definition == nullptr || !TriggerFunctionList::Fits(types_, type, *definition)) {
            row->function->clear();
            row->function->setEditText(QString{});
        }
        row->functionsLoaded = false;
    } else if (row->originalCall.has_value()) {
        const auto* call = FindEca(document_, *row->nestedEcaId);
        const auto* definition = call == nullptr ? nullptr : catalog_.Find(call->type, call->name.text);
        row->function->addItem(definition == nullptr ? Utf8(*row->originalCall) : Functions().Title(*definition),
            Utf8(*row->originalCall));
        row->function->setCurrentIndex(0);
    }
    row->variable->SetFill([this, row]() { PopulateVariables(row); });
    row->function->SetFill([this, row]() { PopulateFunctions(row); });

    // 可用的来源行：依据函数库定义（AllowedValueKinds），没有固定值写法的类型不给“值”。
    std::array<bool, 4> show{};
    if (row->readOnly) {
        show[Value] = true;
    } else if (row->writableTarget) {
        show[Variable] = true;
    } else {
        const auto kinds = types_.AllowedValueKinds(type);
        show[Preset] = kinds.preset && row->preset->count() > 0;
        show[Variable] = kinds.variable;
        show[Function] = kinds.function;
        show[Value] = !types_.IsTypeSelector(type) &&
            (row->booleanLiteral || literal != Literal::Other || types_.Find(type) == nullptr);
    }
    int current = initial
        ? (row->nestedEcaId.has_value() ? Function : row->fresh ? -1 : row->originalKind)
        : CurrentSource(row);
    if (row->readOnly) current = Value;
    // 已有数据用的来源始终保留，不因类型规则被藏起来。
    if (initial && current >= Preset && current <= Value) show[static_cast<std::size_t>(current)] = true;
    for (std::size_t source = 0; source < show.size(); ++source) row->lines[source]->setVisible(show[source]);
    if (current < Preset || current > Value || !show[static_cast<std::size_t>(current)]) {
        current = DefaultSource(row);
        row->lines[static_cast<std::size_t>(current)]->setVisible(true);
    }
    SelectSource(row, current);
    if (current == Preset && row->preset->currentIndex() < 0 && row->preset->count() > 0) {
        row->preset->setCurrentIndex(0);
    }
    if (current == Variable) PopulateVariables(row);
    UpdateFunctionLine(row);
}

// 新参数或换类型后原来源不可用时的来源：赋值目标用变量，函数槽用函数，
// 类型选择器与只能取预设/引用的类型用第一个预设，其余先用值。
int TriggerParameterDialog::DefaultSource(const Row* row) const {
    const auto shown = [row](int source) { return !row->lines[static_cast<std::size_t>(source)]->isHidden(); };
    if (row->writableTarget) return Variable;
    if (shown(Function) && War3Map::Trigger::TriggerTypeSystem::FunctionSlotKind(row->expectedType)) return Function;
    if (shown(Preset) && (types_.IsTypeSelector(row->expectedType) ||
            types_.LiteralKind(row->expectedType) == Literal::Other)) {
        return Preset;
    }
    for (const int source : {Value, Preset, Variable, Function}) {
        if (shown(source)) return source;
    }
    return Value;
}

void TriggerParameterDialog::RecomputeTypes(bool unresolvedOnly) {
    for (std::size_t index = 0; index < rows_.size(); ++index) {
        Row* row = rows_[index];
        if (row->declaredType != "Null" || row->readOnly) continue;
        if (unresolvedOnly && row->expectedType != "Null") continue;
        std::string type = PendingExpectedType(index);
        if (type == row->expectedType) continue;
        row->expectedType = std::move(type);
        ConfigureRow(row, false);
    }
}

// 与 TriggerTypeSystem::ExpectedType 同一规则，只是读对话框里尚未应用的选择。
std::string TriggerParameterDialog::PendingExpectedType(std::size_t index) const {
    for (std::size_t position = index; position-- > 0;) {
        const Row* selector = rows_[position];
        if (!selector->selector) continue;
        if (CurrentSource(selector) != Preset) return "Null";
        const std::string type = War3Map::Trigger::TriggerTypeSystem::TypeFromTypename(
            PresetValue(selector).toUtf8().toStdString());
        return type.empty() ? "Null" : type;
    }
    if (index == 0) return "Null";
    const Row* previous = rows_[index - 1];
    std::string type = PendingValueType(previous);
    if (type.empty()) type = previous->expectedType;
    return type.empty() || type == "AnyGlobal" || type == "AnyType" || type == "anytype" ? "Null" : type;
}

std::string TriggerParameterDialog::PendingValueType(const Row* row) const {
    switch (CurrentSource(row)) {
    case Preset:
        return types_.PresetType(PresetValue(row).toUtf8().toStdString()).value_or(std::string{});
    case Variable: {
        const auto reference = War3Map::Trigger::ResolveVariableReference(
            document_, VariableValue(row).toUtf8().toStdString());
        return reference.Succeeded() && reference.value ? reference.value->type : std::string{};
    }
    case Function: {
        const auto name = FunctionName(row);
        if (!name.has_value()) return {};
        if (row->nestedEcaId.has_value() && name == row->originalCall) {
            if (const auto* call = FindEca(document_, *row->nestedEcaId)) return types_.ReturnType(document_, *call, locals_);
        }
        const auto* definition = FindFunction(catalog_, row->expectedType, *name);
        return definition == nullptr ? std::string{} : CallReturnType(*definition);
    }
    default:
        return {};
    }
}

void TriggerParameterDialog::PopulateVariables(Row* row) {
    if (row->variablesLoaded) return;
    row->variablesLoaded = true;
    const QSignalBlocker blocker(row->variable);
    const auto globals = War3Map::Trigger::CollectGlobalReferenceCandidates(
        document_, types_, row->expectedType, row->anyGlobal, row->writableTarget);
    const QString current = row->originalKind == Variable ? Utf8(row->originalValue) : QString{};
    int selected = -1;
    for (const auto& candidate : globals.items) {
        const QString name = Utf8(candidate.name), value = Utf8(candidate.value);
        const bool trigger = candidate.kind == War3Map::Trigger::VariableReferenceKind::GeneratedTrigger;
        const QString detail = trigger ? tr("触发器")
            : candidate.type == row->expectedType ? QString{} : Utf8(types_.DisplayName(candidate.type));
        row->variable->addItem(detail.isEmpty() ? name : name + QStringLiteral("　") + detail, value);
        row->variable->setItemData(row->variable->count() - 1, name, kVariableNameRole);
        if (trigger) row->variable->setItemData(row->variable->count() - 1, value, Qt::ToolTipRole);
        if (!current.isEmpty() && (value == current || (current.startsWith(QStringLiteral("udg_")) && value == current.mid(4))))
            selected = row->variable->count() - 1;
    }
    // 不在候选里的已有引用（预放对象、旧数据）原样保留。
    if (selected < 0 && !current.isEmpty()) {
        row->variable->addItem(current, current);
        row->variable->setItemData(row->variable->count() - 1, current, kVariableNameRole);
        selected = row->variable->count() - 1;
    }
    row->variable->setCurrentIndex(selected);
    if (selected < 0) row->variable->setEditText(QString{});
    row->variable->lineEdit()->setPlaceholderText(globals.items.empty() ? tr("没有该类型的变量")
        : types_.Accepts(row->expectedType, "trigger") && !row->writableTarget && !row->anyGlobal
            ? tr("搜索变量或触发器") : tr("搜索变量"));
}

void TriggerParameterDialog::PopulateFunctions(Row* row) {
    if (row->functionsLoaded) return;
    row->functionsLoaded = true;
    const QSignalBlocker blocker(row->function);
    const auto current = FunctionName(row);
    row->function->clear();
    auto& functions = Functions();
    const auto choices = functions.ForType(types_, row->expectedType);
    for (const auto* group : {&choices.slot, &choices.exact, &choices.compatible, &choices.generic}) {
        for (const auto* definition : *group) {
            row->function->addItem(functions.Icon(*definition), functions.Title(*definition), Utf8(definition->name));
            row->function->setItemData(row->function->count() - 1, Utf8(definition->name), Qt::ToolTipRole);
        }
    }
    int index = current.has_value() ? row->function->findData(Utf8(*current)) : -1;
    if (index < 0 && current.has_value()) {
        // 已有函数不在候选里（旧数据）：原样保留，交给应用时的类型校验。
        const auto* definition = FindFunction(catalog_, row->expectedType, *current);
        row->function->addItem(definition == nullptr ? Utf8(*current) : functions.Title(*definition), Utf8(*current));
        index = row->function->count() - 1;
    }
    row->function->setCurrentIndex(index);
    if (index < 0) row->function->setEditText(QString{});
    UpdateFunctionLine(row);
}

void TriggerParameterDialog::BrowseFunction(Row* row) {
    // 函数槽（如 AddTriggerEvent 的事件）与 WE 相同，从事件/条件/动作函数中选。
    const auto slot = War3Map::Trigger::TriggerTypeSystem::FunctionSlotKind(row->expectedType);
    UiDefinitionPickerDialog picker(
        catalog_,
        slot && !catalog_.Definitions(*slot).empty() ? *slot : War3Map::Trigger::UiDefinitionKind::Call,
        row->expectedType,
        categoryIcons_,
        this);
    if (picker.exec() != QDialog::Accepted || picker.SelectedDefinition() == nullptr) {
        return;
    }
    PopulateFunctions(row);
    const auto* definition = picker.SelectedDefinition();
    const QString name = Utf8(definition->name);
    int index = row->function->findData(name);
    if (index < 0) {
        row->function->addItem(Functions().Icon(*definition), Functions().Title(*definition), name);
        index = row->function->count() - 1;
    }
    row->function->setCurrentIndex(index);
    SelectSource(row, Function);
}

void TriggerParameterDialog::SetResourcePickerFactory(War3Map::Modules::ObjectResourcePickerFactory factory) {
    resourcePickerFactory_ = std::move(factory);
    for (Row* row : rows_) {
        const auto resource = ResourceKindForType(row->expectedType);
        row->resourceBrowse->setVisible(!row->readOnly && resource.has_value() && resourcePickerFactory_ != nullptr);
        if (resource) row->resourceBrowse->setText(*resource == War3Map::Services::ObjectResourceKind::Model ? tr("选择模型…") : tr("选择图像…"));
    }
}

// 打开物编同一个资源选择器（游戏文件、导入、其他物体用过的、自定义），选中后填回“值”并选中这一来源。
void TriggerParameterDialog::BrowseResource(Row* row) {
    const auto kind = ResourceKindForType(row->expectedType);
    if (!kind || !resourcePickerFactory_) return;
    for (auto* open : findChildren<War3Map::Modules::ObjectResourcePicker*>()) open->close();
    auto* picker = new War3Map::Modules::ObjectResourcePicker(
        War3Map::Services::ObjectResourceField{*kind, false, true}, row->scalar->text(), resourcePickerFactory_(), this);
    picker->setObjectName(QStringLiteral("parameterResourcePicker"));
    picker->SetCommit([this, row](const QString& value) {
        row->scalar->setText(value);
        SelectSource(row, Value);
        return true;
    });
    auto* escape = new QShortcut(QKeySequence(Qt::Key_Escape), picker);
    connect(escape, &QShortcut::activated, picker, &QWidget::close);
    // 贴在“值”输入框下方；放不下时放到上方，并留在当前屏幕工作区内。
    const auto available = row->scalar->screen()->availableGeometry();
    QRect geometry(row->scalar->mapToGlobal(QPoint(0, row->scalar->height() + 4)), picker->size());
    if (geometry.bottom() > available.bottom())
        geometry.moveBottom(row->scalar->mapToGlobal(QPoint(0, -4)).y());
    geometry.moveLeft(std::clamp(geometry.left(), available.left(), std::max(available.left(), available.right() - geometry.width())));
    geometry.moveTop(std::max(geometry.top(), available.top()));
    picker->move(geometry.topLeft());
    picker->show();
    picker->raise();
    picker->activateWindow();
}

void TriggerParameterDialog::UpdateFunctionLine(Row* row) {
    const auto name = FunctionName(row);
    const auto* definition = name.has_value() ? FindFunction(catalog_, row->expectedType, *name) : nullptr;
    row->functionEdit->setEnabled(!row->readOnly && definition != nullptr && ArgumentCount(*definition) > 0);
    // 原函数带参数时在下方显示整句，点“编辑参数”修改。
    QString sentence;
    if (row->nestedEcaId.has_value() && name == row->originalCall) {
        if (const auto* call = FindEca(document_, *row->nestedEcaId); call != nullptr && !call->arguments.empty()) {
            sentence = Utf8(PresentEca(document_, *call, catalog_).text);
        }
    }
    row->callText->setText(sentence);
    row->callText->setVisible(!sentence.isEmpty());
}

int TriggerParameterDialog::CurrentSource(const Row* row) {
    return row->sources->checkedId();
}

void TriggerParameterDialog::SelectSource(Row* row, int source) {
    if (auto* button = row->sources->button(source); button != nullptr && !button->isChecked()) {
        button->setChecked(true);
    }
}

// 下拉文字可以是条目显示名或原始预设名；匹配不到时原样交出，由应用时校验。
QString TriggerParameterDialog::PresetValue(const Row* row) {
    const QString text = row->preset->currentText().trimmed();
    const int index = row->preset->currentIndex();
    if (index >= 0 && row->preset->itemText(index) == text) return row->preset->itemData(index).toString();
    for (int item = 0; item < row->preset->count(); ++item) {
        if (row->preset->itemText(item) == text || row->preset->itemData(item).toString() == text)
            return row->preset->itemData(item).toString();
    }
    return text;
}

// 下拉文字可以是条目全文、名称或直接写的引用；匹配到条目时交出它实际存的引用（如 gg_trg_ 生成名）。
QString TriggerParameterDialog::VariableName(const Row* row) {
    const QString text = row->variable->currentText().trimmed();
    for (int index = 0; index < row->variable->count(); ++index) {
        if (row->variable->itemText(index) == text) return row->variable->itemData(index, kVariableNameRole).toString();
    }
    return text;
}

QString TriggerParameterDialog::VariableValue(const Row* row) {
    const QString text = row->variable->currentText().trimmed();
    for (int index = 0; index < row->variable->count(); ++index) {
        if (row->variable->itemText(index) == text || row->variable->itemData(index, kVariableNameRole).toString() == text)
            return row->variable->itemData(index).toString();
    }
    return text;
}

// 函数下拉的文字须对应一个条目（显示名或原始函数名）；任意文字不算选了函数。
std::optional<std::string> TriggerParameterDialog::FunctionName(const Row* row) {
    const QString text = row->function->currentText().trimmed();
    if (text.isEmpty()) return std::nullopt;
    const int index = row->function->currentIndex();
    if (index >= 0 && row->function->itemText(index) == text)
        return row->function->itemData(index).toString().toUtf8().toStdString();
    for (int item = 0; item < row->function->count(); ++item) {
        if (row->function->itemText(item) == text || row->function->itemData(item).toString() == text)
            return row->function->itemData(item).toString().toUtf8().toStdString();
    }
    return std::nullopt;
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
        html += raw.mid(cursor, start - cursor).toHtmlEscaped();
        QString value;
        if (rowIdx < static_cast<int>(rows_.size())) {
            const Row* row = rows_[static_cast<std::size_t>(rowIdx)];
            switch (CurrentSource(row)) {
            case Preset: value = row->preset->currentText(); break;
            case Variable: value = VariableName(row); break;
            case Function:
                value = row->callText->text().isEmpty() ? row->function->currentText() : row->callText->text();
                break;
            default:
                value = row->booleanLiteral ? row->booleanValue->currentText() : row->scalar->text();
                break;
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
        request.expectedType = row->expectedType;
        const int source = CurrentSource(row);
        request.valueKind = source < Preset ? Value : source;
        switch (request.valueKind) {
        case Preset: request.value = PresetValue(row).toUtf8().toStdString(); break;
        case Variable: request.value = VariableValue(row).toUtf8().toStdString(); break;
        case Function: request.callFunction = FunctionName(row); break;
        default:
            request.value = (row->booleanLiteral ? row->booleanValue->currentData().toString() : row->scalar->text())
                .toUtf8().toStdString();
            break;
        }
        result.push_back(std::move(request));
    }
    return result;
}

std::optional<War3Map::StableId> TriggerParameterDialog::RequestedNestedEca() const noexcept {
    return requestedNestedEca_;
}

void TriggerParameterDialog::accept() {
    for (Row* row : rows_) {
        if (!row->readOnly && CurrentSource(row) == Function && !FunctionName(row).has_value()) {
            FlashRow(row);
            requestedNestedEca_.reset();
            QMessageBox::warning(
                this,
                tr("尚未选择函数"),
                tr("“%1”选了函数，请从列表里选一个函数。").arg(row->label));
            return;
        }
    }
    QDialog::accept();
}
