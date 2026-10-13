#include "ObjectModule.h"
#include "ObjectModuleShared.h"

#include "EditorProject.h"
#include "ObjectModels.h"
#include "ObjectModelsDetail.h"
#include "War3MapCore/ObjectData/ObjectBatchEdit.h"
#include "War3MapCore/ObjectData/ObjectImport.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>
#include <optional>

namespace War3Map::Modules {
namespace {

using namespace ObjectData;
using namespace ObjectDetail;

struct BatchField {
    ModificationKey key;
    QString label;
    QString currentValue;
    std::string valueType;
    std::vector<std::pair<QString, QString>> choices;
};

std::vector<BatchField> CommonFields(LniObjectDocument& document,
    const std::vector<ObjectKey>& targets, const StandardObjectCatalog* catalog,
    const ObjectFieldSchema* schema, bool rawLabels) {
    std::vector<BatchField> common;
    for (std::size_t index = 0; index < targets.size(); ++index) {
        ObjectFieldModel model;
        model.SetContext(catalog, schema);
        model.SetDocument(&document, &targets[index]);
        std::vector<BatchField> available;
        for (int row = 0; row < model.rowCount(); ++row) {
            const auto* modification = model.ModificationAt(row);
            const auto* definition = model.DefinitionAt(row);
            if (!modification || !definition || !definition->nativeValueType ||
                !definition->referenceTypes.empty() || definition->isList ||
                ObjectModelsDetail::IsMultilineTextField(definition, model.EditorValueAt(row)) ||
                !(model.flags(model.index(row, 3)) & Qt::ItemIsEditable)) continue;
            BatchField field{modification->key,
                ObjectFieldCategory(definition) + QStringLiteral(" / ") + ObjectFieldLabel(definition, rawLabels),
                U8(ValueToDisplayString(modification->value)), definition->valueType, {}};
            for (const auto& choice : definition->choices)
                field.choices.emplace_back(U8(choice.displayName), U8(choice.value));
            available.push_back(std::move(field));
        }
        if (index == 0) {
            common = std::move(available);
        } else {
            std::erase_if(common, [&available](const BatchField& field) {
                return std::none_of(available.begin(), available.end(), [&field](const BatchField& candidate) {
                    return candidate.key == field.key && candidate.valueType == field.valueType;
                });
            });
        }
        if (common.empty()) break;
    }
    return common;
}

const ObjectModification* FindModification(const ObjectRecord* record, const ModificationKey& field) {
    if (!record) return nullptr;
    const auto found = std::find_if(record->modifications.begin(), record->modifications.end(),
        [&field](const ObjectModification& modification) { return modification.key == field; });
    return found == record->modifications.end() ? nullptr : &*found;
}

} // namespace

bool ObjectModule::CommitBatchFieldValue(const std::vector<ObjectKey>& targets,
    const ModificationKey& field, const QString& value) {
    const int index = currentObjectType_;
    if (targets.size() < 2 || index < 0 || index >= 7 || !project_->objectDocuments_[index] ||
        !project_->objectFieldSchema_ || !project_->standardCatalog_ ||
        !project_->objectDocuments_[index]->IsStructurallySafe()) return false;

    const auto* document = project_->objectDocuments_[index].get();
    LniObjectDocument validated = *document;
    std::vector<ObjectKey> changed;
    std::optional<Value> typedValue;
    for (const auto& target : targets) {
        if (target.type != static_cast<ObjectType>(index) || !validated.FindObject(target)) return false;
        ObjectFieldModel trial;
        trial.SetContext(project_->standardCatalog_.get(), project_->objectFieldSchema_.get());
        trial.SetDocument(&validated, &target);
        int matchingRow = -1;
        for (int row = 0; row < trial.rowCount(); ++row) {
            const auto* modification = trial.ModificationAt(row);
            if (!modification || modification->key != field) continue;
            if (matchingRow >= 0) return false;
            matchingRow = row;
        }
        if (matchingRow < 0 || !(trial.flags(trial.index(matchingRow, 3)) & Qt::ItemIsEditable)) return false;
        const auto before = validated.DocumentFingerprint();
        if (!trial.setData(trial.index(matchingRow, 3), value, Qt::EditRole)) return false;
        if (validated.DocumentFingerprint() == before) continue;
        const auto* modified = FindModification(validated.FindObject(target), field);
        if (!modified) return false;
        if (typedValue && (typedValue->index() != modified->value.index() ||
            ValueToDisplayString(*typedValue) != ValueToDisplayString(modified->value))) return false;
        typedValue = modified->value;
        changed.push_back(target);
    }
    if (changed.empty()) return true;
    if (!typedValue) return false;
    const auto* native = project_->nativeObjectDocuments_[index].get();
    if (native && native->Supports(NativeObjectOperation::EditExistingValue)
        && !native->Supports(NativeObjectOperation::EditStructure)) {
        // 有限档位在进入撤销栈前走同一native回灌预检；不能在批量入口新增源中不存在的字段。
        auto checked = *native;
        const auto writable = ApplyLniChangesToNative(checked, validated,
            project_->objectFieldSchema_.get(), project_->standardCatalog_.get());
        if (!writable.Succeeded()) {
            project_->AddDiagnostics(writable.diagnostics);
            ShowStatus(tr("这批修改超出当前物编版本的已有字段写出范围"), 7000);
            return false;
        }
    } else if (!EnsureObjectDocument(index)) return false;
    auto applied = ApplyBatchModification(*document, changed, field, *typedValue);
    if (!applied.Succeeded() || !applied.value ||
        applied.value->DocumentFingerprint() != validated.DocumentFingerprint()) return false;
    CommitObjectCreation(index, std::move(*applied.value), U8(targets.front().rawcode),
        tr("批量修改 %1 个物体的 %2").arg(changed.size()).arg(U8(field.fieldId)));
    ShowStatus(tr("已修改 %1 个对象 · 可一次撤销").arg(changed.size()), 5000);
    return true;
}

void ObjectModule::BatchEditObjects() {
    const int index = currentObjectType_;
    auto* document = project_->objectDocuments_[index].get();
    if (!document || !document->IsStructurallySafe() || !project_->objectFieldSchema_ ||
        !project_->standardCatalog_ || document->Objects().size() < 2) {
        ShowStatus(tr("批量修改需要至少两个可编辑的地图对象，以及原版字段目录"), 6000);
        return;
    }
    const auto type = static_cast<ObjectType>(index);
    QDialog dialog(page_);
    dialog.setWindowTitle(tr("批量修改 · %1").arg(BreadcrumbText()));
    dialog.setMinimumSize(530, 480);
    auto* layout = new QVBoxLayout(&dialog);
    auto* objects = new QListWidget(&dialog);
    objects->setObjectName("batchObjectList");
    const QString currentId = objectListView_ && objectListView_->currentIndex().isValid()
        ? objectListView_->currentIndex().data(Qt::UserRole).toString() : QString{};
    for (const auto& record : document->Objects()) {
        const auto presentation = ResolveProjectObjectPresentation(*project_, record.key);
        auto* item = new QListWidgetItem(tr("%1  ·  %2")
            .arg(U8(record.key.rawcode), showRawFields_->isChecked() ? U8(record.key.rawcode) : presentation.name), objects);
        item->setToolTip(tr("四字码：%1%2%3").arg(U8(record.key.rawcode),
            presentation.nameReference.isEmpty() ? QString{} : tr("\n字符串表：%1").arg(presentation.nameReference),
            presentation.issue.isEmpty() ? QString{} : QLatin1Char('\n') + presentation.issue));
        if (!showRawFields_->isChecked()) item->setIcon(project_->iconProvider_->IconForTexture(presentation.iconPath));
        item->setData(Qt::UserRole, U8(record.key.rawcode));
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(U8(record.key.rawcode) == currentId ? Qt::Checked : Qt::Unchecked);
    }
    layout->addWidget(objects, 1);
    auto* form = new QFormLayout;
    auto* fields = new QComboBox(&dialog);
    fields->setObjectName("batchField");
    auto* entry = new QLineEdit(&dialog);
    entry->setObjectName("batchValue");
    auto* choices = new QComboBox(&dialog);
    choices->setObjectName("batchChoices");
    choices->hide();
    form->addRow(tr("共同字段"), fields);
    form->addRow(tr("设为"), entry);
    form->addRow(QString{}, choices);
    layout->addLayout(form);
    auto* issue = new QLabel(&dialog);
    issue->setObjectName("batchIssue");
    issue->setWordWrap(true);
    layout->addWidget(issue);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Apply | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Apply)->setText(tr("应用修改"));
    layout->addWidget(buttons);
    std::vector<BatchField> common;
    const auto selected = [objects, type] {
        std::vector<ObjectKey> result;
        for (int row = 0; row < objects->count(); ++row) {
            const auto* item = objects->item(row);
            if (item->checkState() == Qt::Checked)
                result.push_back({type, item->data(Qt::UserRole).toString().toStdString()});
        }
        return result;
    };
    const auto updateChoice = [&] {
        const int choice = fields->currentIndex();
        if (choice < 0 || static_cast<std::size_t>(choice) >= common.size()) return;
        const auto& field = common[static_cast<std::size_t>(choice)];
        choices->clear();
        for (const auto& [label, value] : field.choices) choices->addItem(label, value);
        if (field.valueType == "bool" && choices->count() == 0) {
            choices->addItem(QObject::tr("是"), QStringLiteral("true"));
            choices->addItem(QObject::tr("否"), QStringLiteral("false"));
        }
        const bool useChoices = choices->count() > 0;
        entry->setVisible(!useChoices);
        choices->setVisible(useChoices);
        if (useChoices) {
            int current = choices->findData(field.currentValue);
            if (current < 0 && field.valueType == "bool")
                current = choices->findData(field.currentValue == QStringLiteral("1") ? QStringLiteral("true") : QStringLiteral("false"));
            choices->setCurrentIndex(std::max(0, current));
        } else entry->setText(field.currentValue);
    };
    const auto updateFields = [&] {
        const auto targets = selected();
        common = targets.size() < 2 ? std::vector<BatchField>{}
            : CommonFields(*document, targets, project_->standardCatalog_.get(),
                project_->objectFieldSchema_.get(), showRawFields_->isChecked());
        fields->clear();
        for (const auto& field : common) fields->addItem(field.label);
        issue->setText(targets.size() < 2 ? tr("至少选择两个对象")
            : common.empty() ? tr("这些对象没有共同且可安全编辑的标量字段") : QString{});
        buttons->button(QDialogButtonBox::Apply)->setEnabled(!common.empty());
        if (!common.empty()) updateChoice();
    };
    connect(objects, &QListWidget::itemChanged, &dialog, [updateFields](QListWidgetItem*) { updateFields(); });
    connect(fields, &QComboBox::currentIndexChanged, &dialog, [updateChoice](int) { updateChoice(); });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, &dialog, [&] {
        const int choice = fields->currentIndex();
        if (choice < 0 || static_cast<std::size_t>(choice) >= common.size()) return;
        const QString value = choices->isVisible() ? choices->currentData().toString() : entry->text();
        if (CommitBatchFieldValue(selected(), common[static_cast<std::size_t>(choice)].key, value))
            dialog.accept();
        else issue->setText(tr("修改未提交：有目标字段、取值或原生基线未通过校验"));
    });
    updateFields();
    dialog.exec();
}

} // namespace War3Map::Modules
