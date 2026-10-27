#include "VariablePropertiesDialog.h"

#include "War3MapCore/Trigger/TriggerDataText.h"
#include <QCheckBox>
#include <QCollator>
#include <QComboBox>
#include <QCompleter>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QSignalBlocker>
#include <QSpinBox>
#include <algorithm>
#include <unordered_set>

using namespace War3Map::Trigger;
using Literal = TriggerTypeSystem::Literal;

namespace {

QString Text(std::string_view text) { return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())); }

bool TakesTypedValue(Literal literal) {
    return literal == Literal::Integer || literal == Literal::Real || literal == Literal::String || literal == Literal::ObjectCode;
}

void MakeSearchable(QComboBox* box) {
    box->setEditable(true);
    box->setInsertPolicy(QComboBox::NoInsert);
    box->completer()->setCompletionMode(QCompleter::PopupCompletion);
    box->completer()->setFilterMode(Qt::MatchContains);
    box->completer()->setCaseSensitivity(Qt::CaseInsensitive);
}

} // namespace

VariablePropertiesDialog::VariablePropertiesDialog(const UiCatalog& catalog, ObjectChoices objects, bool showRawValues,
    const std::optional<GraphVariableValue>& current, QWidget* parent)
    : QDialog(parent), types_(catalog), objects_(std::move(objects)), showRaw_(showRawValues), creating_(!current),
      original_(current) {
    setObjectName(QStringLiteral("variablePropertiesDialog"));
    setWindowTitle(creating_ ? tr("新建变量") : tr("变量属性"));
    setMinimumWidth(400);
    auto* layout = new QFormLayout(this);
    layout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    name_ = new QLineEdit(this);
    name_->setObjectName(QStringLiteral("variableName"));
    if (current) {
        name_->setText(Text(current->name));
        name_->setReadOnly(true);
        name_->setFocusPolicy(Qt::ClickFocus);
    }
    type_ = new QComboBox(this);
    type_->setObjectName(QStringLiteral("variableType"));
    type_->setMaxVisibleItems(20);
    struct Entry { QString display, name; };
    std::vector<Entry> entries;
    std::unordered_set<std::string> seen;
    for (const auto* definition : catalog.Definitions(UiDefinitionKind::Define)) {
        if (definition->group != "TriggerTypes" || !seen.insert(definition->name).second) continue;
        const auto* info = types_.Find(definition->name);
        if (info == nullptr || !info->canBeGlobal) continue;
        entries.push_back({Text(showRaw_ ? definition->name : types_.DisplayName(definition->name)), Text(definition->name)});
    }
    QCollator collator(QLocale(QLocale::Chinese, QLocale::China));
    std::sort(entries.begin(), entries.end(), [&collator](const Entry& left, const Entry& right) {
        const int order = collator.compare(left.display, right.display);
        return order != 0 ? order < 0 : left.name < right.name;
    });
    for (const auto& entry : entries) {
        type_->addItem(entry.display, entry.name);
        type_->setItemData(type_->count() - 1, entry.name, Qt::ToolTipRole);
    }
    const QString currentType = current ? Text(current->typeName) : QStringLiteral("integer");
    int typeIndex = type_->findData(currentType);
    if (typeIndex < 0 && current) {
        // 当前函数库不认识的已有类型照原样留在首位，不悄悄换成别的类型。
        type_->insertItem(0, currentType, currentType);
        typeIndex = 0;
    }
    type_->setCurrentIndex(std::max(typeIndex, 0));

    array_ = new QCheckBox(tr("数组"), this);
    array_->setObjectName(QStringLiteral("variableArray"));
    array_->setChecked(current && current->isArray);
    size_ = new QSpinBox(this);
    size_->setObjectName(QStringLiteral("variableArraySize"));
    size_->setRange(1, 8192);
    size_->setPrefix(tr("大小 "));
    size_->setValue(current && current->isArray ? std::clamp(current->arraySize, 1, 8192) : 1);
    initialized_ = new QCheckBox(tr("初始值"), this);
    initialized_->setObjectName(QStringLiteral("variableInitialized"));
    initialized_->setChecked(current && current->initialized);
    initial_ = new QComboBox(this);
    initial_->setObjectName(QStringLiteral("variableInitialValue"));
    initial_->setMaxVisibleItems(20);
    problem_ = new QLabel(this);
    problem_->setObjectName(QStringLiteral("formProblem"));
    problem_->setWordWrap(true);
    problem_->hide();

    layout->addRow(tr("名称"), name_);
    layout->addRow(tr("类型"), type_);
    layout->addRow(array_, size_);
    layout->addRow(initialized_, initial_);
    layout->addRow(problem_);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    layout->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &VariablePropertiesDialog::Submit);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(type_, &QComboBox::currentIndexChanged, this, [this] {
        RebuildInitialChoices(CurrentInitialValue(), false);
        UpdateEnabled();
    });
    connect(array_, &QCheckBox::toggled, this, &VariablePropertiesDialog::UpdateEnabled);
    connect(initialized_, &QCheckBox::toggled, this, &VariablePropertiesDialog::UpdateEnabled);
    RebuildInitialChoices(current ? current->initialValue : std::string{}, true);
    UpdateEnabled();
    (creating_ ? static_cast<QWidget*>(name_) : type_)->setFocus();
}

std::string VariablePropertiesDialog::CurrentType() const { return type_->currentData().toString().toStdString(); }

std::string VariablePropertiesDialog::CurrentInitialValue() const {
    if (!initial_->isEditable()) return initial_->currentData().toString().toStdString();
    const QString text = initial_->currentText();
    // 输入框里是候选的显示名时取候选的原值；否则是作者直接写的字面值。
    if (const int index = initial_->findText(text); index >= 0) return initial_->itemData(index).toString().toStdString();
    return (types_.LiteralKind(CurrentType()) == Literal::String ? text : text.trimmed()).toStdString();
}

void VariablePropertiesDialog::RebuildInitialChoices(const std::string& keep, bool original) {
    const std::string type = CurrentType();
    const auto literal = types_.LiteralKind(type);
    const QSignalBlocker blocker(initial_);
    initial_->clear();
    initial_->setEditable(false);
    std::vector<Choice> choices;
    for (const auto& preset : PresetsOfType(types_.Catalog(), type))
        choices.push_back({Text(preset.name), Text(showRaw_ ? preset.name : preset.display)});
    if (literal == Literal::Boolean && choices.empty())
        choices = {{QStringLiteral("true"), showRaw_ ? QStringLiteral("true") : tr("真")},
            {QStringLiteral("false"), showRaw_ ? QStringLiteral("false") : tr("假")}};
    if (literal == Literal::ObjectCode && objects_) {
        for (auto& object : objects_(type)) {
            const bool named = !showRaw_ && !object.display.isEmpty() && object.display != object.value;
            choices.push_back({object.value, named ? QStringLiteral("%1 (%2)").arg(object.display, object.value) : object.value});
        }
    }
    const bool typed = TakesTypedValue(literal);
    initialAvailable_ = literal != Literal::Script && (typed || !choices.empty());
    if (typed) MakeSearchable(initial_);
    for (const auto& choice : choices) initial_->addItem(choice.display, choice.value);

    const QString kept = Text(keep);
    const int found = kept.isEmpty() ? -1 : initial_->findData(kept);
    if (found >= 0) {
        initial_->setCurrentIndex(found);
    } else if (typed) {
        initial_->setCurrentIndex(-1);
        initial_->setEditText(original || types_.ValidateLiteral(type, keep).Succeeded() ? kept : QString{});
    } else if (original && !kept.isEmpty()) {
        // 已有初值不在候选里（例如别的函数库的预设）时原样保留在首位。
        initial_->insertItem(0, kept, kept);
        initial_->setCurrentIndex(0);
    } else {
        initial_->setCurrentIndex(-1);
    }
    // 换类型后原初值用不上：取消勾选，由作者重新选，不替作者挑一个值。
    if (!original && !kept.isEmpty() && found < 0 && (!typed || initial_->currentText().isEmpty()))
        initialized_->setChecked(false);
}

void VariablePropertiesDialog::UpdateEnabled() {
    size_->setEnabled(array_->isChecked());
    initialized_->setEnabled(initialAvailable_);
    initial_->setEnabled(initialAvailable_ && initialized_->isChecked());
}

void VariablePropertiesDialog::ShowProblem(const QString& message) {
    problem_->setText(message);
    problem_->show();
}

void VariablePropertiesDialog::Submit() {
    if (creating_ && name_->text().trimmed().isEmpty()) {
        ShowProblem(tr("变量名称不能为空。"));
        name_->setFocus();
        return;
    }
    if (CurrentType().empty()) {
        ShowProblem(tr("请选择变量类型。"));
        return;
    }
    if (initialAvailable_ && initialized_->isChecked() && CurrentInitialValue().empty()
        && types_.LiteralKind(CurrentType()) != Literal::String) {
        ShowProblem(tr("请选择或输入初始值。"));
        initial_->setFocus();
        return;
    }
    accept();
}

GraphVariableValue VariablePropertiesDialog::Value() const {
    GraphVariableValue value;
    value.name = original_ ? original_->name : name_->text().trimmed().toStdString();
    value.typeName = CurrentType();
    value.isArray = array_->isChecked();
    value.arraySize = value.isArray ? size_->value() : 1;
    if (!initialAvailable_ && original_ && original_->typeName == value.typeName) {
        // 这一类型在表单里没有可选初值时不动原初值。
        value.initialized = original_->initialized;
        value.initialValue = original_->initialValue;
    } else {
        value.initialized = initialAvailable_ && initialized_->isChecked();
        value.initialValue = value.initialized ? CurrentInitialValue() : std::string{};
    }
    return value;
}

std::vector<GraphEditOperation> VariablePropertyOperations(const VariableRecord& before, const GraphVariableValue& after) {
    std::vector<GraphEditOperation> operations;
    const GraphRecordRef target{GraphRecordKind::Variable, before.id, {}};
    const auto set = [&](GraphField field, GraphFieldValue expected, GraphFieldValue value) {
        if (expected != value) operations.emplace_back(GraphSet{target, field, std::move(expected), std::move(value)});
    };
    set(GraphField::VariableType, before.typeName.text, after.typeName);
    set(GraphField::VariableArray, before.isArray != 0, after.isArray);
    if (after.isArray) set(GraphField::VariableArraySize, before.size, after.arraySize);
    else if (before.isArray != 0) set(GraphField::VariableArraySize, before.size, std::int32_t{1});
    set(GraphField::VariableInitialized, before.hasDefault != 0, after.initialized);
    if (after.initialized) set(GraphField::VariableInitialValue, before.value.text, after.initialValue);
    return operations;
}
