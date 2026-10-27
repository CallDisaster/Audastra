#include "VariablesDialog.h"

#include "Widgets/ColumnLayout.h"
#include "TriggerCommands.h"
#include <QAbstractItemView>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QUndoStack>
#include <QVBoxLayout>
#include <algorithm>

// ── 变量管理对话框（N2b）────────────────────────────────────────

VariablesDialog::VariablesDialog(
    War3Map::Trigger::TriggerDocument* document,
    QUndoStack* undoStack,
    QWidget* parent,
    bool embedded)
    : QDialog(parent),
      document_(document),
      undoStack_(undoStack),
      embedded_(embedded) {
    if (embedded) setWindowFlags(Qt::Widget);
    setWindowTitle(tr("变量管理"));
    resize(520, 420);
    auto* layout = new QVBoxLayout(this);
    auto* rawValues = new QCheckBox(tr("显示原始字段"), this);
    rawValues->setObjectName("variableRawValues");
    layout->addWidget(rawValues);
    connect(rawValues, &QCheckBox::toggled, this, [this](bool raw) { showRawValues_ = raw; RebuildList(); });
    table_ = new QTableWidget(0, 5, this);
    table_->setObjectName(QStringLiteral("globalVariablesTable"));
    table_->setHorizontalHeaderLabels({tr("名称"), tr("类型"), tr("数组"), tr("初始化数量"), tr("初始值")});
    table_->verticalHeader()->hide();
    table_->verticalHeader()->setDefaultSectionSize(32);
    table_->setShowGrid(false);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    // 名称、类型等一眼看完；各列可拖动调整宽度，最后一列（初始值）填满剩余空间。
    War3Map::UiKit::ConfigureResizableColumns(table_->horizontalHeader(), {240, 110, 60, 90});
    layout->addWidget(table_, 1);

    auto* buttons = new QHBoxLayout;
    auto* addButton = new QPushButton(tr("新建变量…"));
    addButton->setObjectName(QStringLiteral("addVariableButton"));
    propertiesButton_ = new QPushButton(tr("属性…"));
    propertiesButton_->setObjectName(QStringLiteral("variablePropertiesButton"));
    renameButton_ = new QPushButton(tr("重命名…"));
    renameButton_->setObjectName(QStringLiteral("renameVariableButton"));
    removeButton_ = new QPushButton(tr("删除"));
    removeButton_->setObjectName(QStringLiteral("removeVariableButton"));
    auto* closeButton = new QPushButton(tr("关闭"));
    propertiesButton_->setEnabled(false);
    renameButton_->setEnabled(false);
    removeButton_->setEnabled(false);
    buttons->addWidget(addButton);
    buttons->addWidget(propertiesButton_);
    buttons->addWidget(renameButton_);
    buttons->addWidget(removeButton_);
    buttons->addStretch();
    if (!embedded) buttons->addWidget(closeButton);
    else { closeButton->setParent(this); closeButton->hide(); }
    layout->addLayout(buttons);

    connect(addButton, &QPushButton::clicked, this, &VariablesDialog::AddVariable);
    connect(propertiesButton_, &QPushButton::clicked, this, &VariablesDialog::EditSelected);
    connect(table_, &QTableWidget::cellDoubleClicked, this, [this] { if (GraphEditingReady()) EditSelected(); });
    connect(renameButton_, &QPushButton::clicked, this, &VariablesDialog::RenameSelected);
    connect(removeButton_, &QPushButton::clicked, this, &VariablesDialog::RemoveSelected);
    connect(closeButton, &QPushButton::clicked, this, &QDialog::accept);
    connect(table_->selectionModel(), &QItemSelectionModel::selectionChanged,
            this, [this]() {
                const bool has = SelectedRow().has_value();
                propertiesButton_->setEnabled(has && GraphEditingReady());
                renameButton_->setEnabled(has);
                removeButton_->setEnabled(has);
            });
    RebuildList();
    if (undoStack_) {
        connect(undoStack_, &QUndoStack::indexChanged, this, [this]() { RebuildList(); });
    }
}

void VariablesDialog::accept() {
    if (!embedded_) QDialog::accept();
}

void VariablesDialog::reject() {
    // QDialog 即使使用 Qt::Widget 仍会将 Esc 转为 reject；工作区不能因此消失。
    if (!embedded_) QDialog::reject();
}

void VariablesDialog::SetNameResolver(NameResolver resolver) {
    nameResolver_ = std::move(resolver);
    RebuildList();
}

void VariablesDialog::SetGraphEditing(GraphEditing editing) {
    graphEditing_ = std::move(editing);
    RebuildList();
}

bool VariablesDialog::GraphEditingReady() const {
    return document_ != nullptr && graphEditing_.catalog && graphEditing_.publish && graphEditing_.catalog() != nullptr;
}

void VariablesDialog::RebuildList(std::optional<War3Map::StableId> preferredId) {
    const int previousRow = table_->currentRow();
    if (!preferredId && previousRow >= 0) {
        if (const auto* name = table_->item(previousRow, 0)) {
            preferredId = name->data(Qt::UserRole).toULongLong();
        }
    }
    const QSignalBlocker selectionBlocker(table_->selectionModel());
    table_->setRowCount(0);
    if (document_ == nullptr) {
        propertiesButton_->setEnabled(false);
        renameButton_->setEnabled(false);
        removeButton_->setEnabled(false);
        return;
    }
    int selectedRow = -1;
    // 类型列默认显示函数库里的中文名，“显示原始字段”时显示类型名。
    std::optional<War3Map::Trigger::TriggerTypeSystem> types;
    if (!showRawValues_ && graphEditing_.catalog) {
        if (const auto* catalog = graphEditing_.catalog()) types.emplace(*catalog);
    }
    table_->setRowCount(static_cast<int>(document_->variables.size()));
    for (std::size_t row = 0; row < document_->variables.size(); ++row) {
        const auto& variable = document_->variables[row];
        const int r = static_cast<int>(row);
        auto* name = new QTableWidgetItem(QString::fromUtf8(variable.name.text));
        name->setData(Qt::UserRole, QVariant::fromValue<qulonglong>(variable.id));
        table_->setItem(r, 0, name);
        if (preferredId && variable.id == *preferredId) selectedRow = r;
        auto* type = new QTableWidgetItem(QString::fromUtf8(variable.typeName.text));
        if (types) {
            type->setText(QString::fromStdString(types->DisplayName(variable.typeName.text)));
            type->setToolTip(QString::fromUtf8(variable.typeName.text));
        }
        table_->setItem(r, 1, type);
        table_->setItem(r, 2, new QTableWidgetItem(
            variable.isArray != 0 ? tr("数组") : tr("—")));
        table_->setItem(r, 3, new QTableWidgetItem(variable.isArray ? QString::number(variable.size) : tr("—")));
        auto* initial = new QTableWidgetItem(variable.hasDefault ? QString::fromUtf8(variable.value.text) : tr("默认"));
        if (variable.hasDefault && nameResolver_) {
            const auto resolved = nameResolver_(variable.value.text, variable.typeName.text);
            if (!showRawValues_) initial->setText(QString::fromUtf8(resolved.displayName));
            initial->setToolTip(tr("原值：%1%2").arg(QString::fromUtf8(variable.value.text),
                resolved.reason.empty() ? QString{} : QLatin1Char('\n') + QString::fromUtf8(resolved.reason)));
        }
        table_->setItem(r, 4, initial);
    }
    if (selectedRow < 0 && table_->rowCount() > 0) {
        selectedRow = std::clamp(previousRow, 0, table_->rowCount() - 1);
    }
    if (selectedRow >= 0) {
        table_->setCurrentCell(selectedRow, 0);
        table_->selectRow(selectedRow);
    }
    propertiesButton_->setEnabled(selectedRow >= 0 && GraphEditingReady());
    renameButton_->setEnabled(selectedRow >= 0);
    removeButton_->setEnabled(selectedRow >= 0);
}

std::optional<std::size_t> VariablesDialog::SelectedRow() const {
    const auto items = table_->selectedItems();
    if (items.isEmpty()) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(items.first()->row());
}

namespace {
QString FirstProblem(const War3Map::Status& status) {
    const auto error = std::find_if(status.diagnostics.begin(), status.diagnostics.end(),
        [](const auto& diagnostic) { return diagnostic.severity == War3Map::DiagnosticSeverity::Error; });
    if (error != status.diagnostics.end()) return QString::fromUtf8(error->message);
    return status.diagnostics.empty() ? QObject::tr("修改未通过检查。") : QString::fromUtf8(status.diagnostics.front().message);
}
} // namespace

void VariablesDialog::EditSelected() {
    const auto row = SelectedRow();
    if (!GraphEditingReady() || !row || *row >= document_->variables.size()) return;
    const auto& variable = document_->variables[*row];
    const auto id = variable.id;
    War3Map::Trigger::GraphVariableValue current{variable.name.text, variable.typeName.text, variable.isArray != 0,
        variable.size, variable.hasDefault != 0, variable.value.text};
    VariablePropertiesDialog form(*graphEditing_.catalog(), graphEditing_.objects, showRawValues_, current, this);
    while (form.exec() == QDialog::Accepted) {
        // 表单打开期间文档可能被撤销等改动：按稳定 ID 重新取当前记录作前值，前值变了由批次拒绝。
        const auto found = std::find_if(document_->variables.begin(), document_->variables.end(),
            [id](const auto& candidate) { return candidate.id == id; });
        if (found == document_->variables.end()) return;
        auto operations = VariablePropertyOperations(*found, form.Value());
        if (operations.empty()) return;
        const auto published = graphEditing_.publish(std::move(operations), tr("修改变量属性"));
        if (published.Succeeded()) {
            RebuildList(id);
            return;
        }
        form.ShowProblem(FirstProblem(published));
    }
}

void VariablesDialog::AddVariable() {
    if (document_ == nullptr || undoStack_ == nullptr) {
        return;
    }
    if (GraphEditingReady()) {
        VariablePropertiesDialog form(*graphEditing_.catalog(), graphEditing_.objects, showRawValues_, std::nullopt, this);
        while (form.exec() == QDialog::Accepted) {
            War3Map::Trigger::GraphCreate create;
            create.clientKey = "variable";
            create.value = form.Value();
            create.parent.kind = War3Map::Trigger::GraphParentKind::Variables;
            for (const auto& variable : document_->variables)
                create.parent.orderedChildren.push_back({War3Map::Trigger::GraphRecordKind::Variable, variable.id, {}});
            std::vector<War3Map::Trigger::GraphEditOperation> operations;
            operations.emplace_back(std::move(create));
            const auto published = graphEditing_.publish(std::move(operations), tr("新建变量"));
            if (published.Succeeded()) {
                if (!document_->variables.empty()) RebuildList(document_->variables.back().id);
                return;
            }
            form.ShowProblem(FirstProblem(published));
        }
        return;
    }
    QDialog form(this);
    form.setWindowTitle(tr("新建变量"));
    auto* layout = new QFormLayout(&form);
    auto* nameEdit = new QLineEdit(&form);
    auto* typeEdit = new QLineEdit(QStringLiteral("integer"), &form);
    auto* arrayCheck = new QCheckBox(tr("数组"), &form);
    layout->addRow(tr("名称："), nameEdit);
    layout->addRow(tr("类型（JASS 类型名）："), typeEdit);
    layout->addRow(QString(), arrayCheck);
    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &form);
    layout->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &form, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &form, &QDialog::reject);
    if (form.exec() != QDialog::Accepted) {
        return;
    }
    const std::string name = nameEdit->text().trimmed().toStdString();
    const std::string typeName = typeEdit->text().trimmed().toStdString();
    if (name.empty() || typeName.empty()) {
        return;
    }
    const bool isArray = arrayCheck->isChecked();
    const War3Map::Status added =
        War3Map::Trigger::TriggerDocumentEditor::AddVariable(
            *document_, name, typeName, isArray);
    if (!added.Succeeded()) {
        QMessageBox::warning(
            this, tr("无法新建变量"),
            added.diagnostics.empty()
                ? tr("变量名重复或非法。")
                : QString::fromUtf8(added.diagnostics.front().message));
        return;
    }
    undoStack_->push(new AddVariableUndoCommand(
        document_, name, typeName, isArray, tr("新建变量")));
    RebuildList(document_->variables.back().id);
}

void VariablesDialog::RenameSelected() {
    const auto row = SelectedRow();
    if (document_ == nullptr || undoStack_ == nullptr || !row.has_value() ||
        *row >= document_->variables.size()) {
        return;
    }
    const std::string before = document_->variables[*row].name.text;
    bool ok = false;
    const QString name = QInputDialog::getText(
        this, tr("重命名变量"), tr("变量名称："),
        QLineEdit::Normal, QString::fromUtf8(before), &ok);
    if (!ok || name.trimmed().isEmpty() ||
        name.trimmed().toStdString() == before) {
        return;
    }
    const std::string after = name.trimmed().toStdString();
    const auto variableId = document_->variables[*row].id;
    std::vector<TriggerArgumentChange> references;
    std::optional<War3Map::Trigger::TriggerDocument> candidate;
    for (const auto& argument : document_->argumentNodes) {
        if (argument.type != 1 ||
            (argument.value.text != before && argument.value.text != "udg_" + before)) continue;
        if (!candidate) candidate.emplace(*document_);
        const auto checked = War3Map::Trigger::TriggerDocumentEditor::SetScalarArgument(
            *candidate, argument.id, 1, argument.value.text == before ? after : "udg_" + after);
        if (!checked.Succeeded()) {
            QMessageBox::warning(this, tr("无法重命名变量"),
                tr("无法同步此变量的引用。\n%1").arg(checked.diagnostics.empty()
                    ? tr("当前引用不支持重命名。") : QString::fromUtf8(checked.diagnostics.front().message)));
            return;
        }
        references.push_back({argument.id, argument,
            *War3Map::Trigger::TriggerDocumentEditor::FindArgument(*candidate, argument.id)});
    }
    // GUI 引用可精确识别；文本未经过语言解析，仅提示同名标记，不盲替换字符串/注释。
    const QRegularExpression marker(QStringLiteral("(?<![A-Za-z0-9_])(?:%1|udg_%1)(?![A-Za-z0-9_])")
        .arg(QRegularExpression::escape(QString::fromUtf8(before))));
    bool textReference = marker.match(QString::fromUtf8(document_->wct.customCode.text)).hasMatch();
    for (const auto& trigger : document_->triggers) {
        textReference = textReference || marker.match(QString::fromUtf8(trigger.customText.text)).hasMatch();
    }
    for (const auto& argument : document_->argumentNodes) {
        if (argument.type == 3) {
            textReference = textReference || marker.match(QString::fromUtf8(argument.value.text)).hasMatch();
        }
    }
    if (textReference && QMessageBox::warning(this, tr("自定义文本引用"),
        tr("自定义文本含有同名标记，需要手动同步。继续重命名变量及可视引用？"),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return;
    const War3Map::Status renamed =
        War3Map::Trigger::TriggerDocumentEditor::RenameVariable(
            *document_, before, after);
    if (!renamed.Succeeded()) {
        QMessageBox::warning(
            this, tr("无法重命名变量"),
            renamed.diagnostics.empty()
                ? tr("变量名重复或非法。")
                : QString::fromUtf8(renamed.diagnostics.front().message));
        return;
    }
    for (const auto& change : references) {
        const auto applied = War3Map::Trigger::TriggerDocumentEditor::SetScalarArgument(
            *document_, change.argumentId, change.after.type, change.after.value.text);
        if (!applied.Succeeded()) {
            // 预检与实际执行间若有异常，沿现有撤销命令恢复整次事务。
            EditTriggerArgumentsUndoCommand rollbackReferences(document_, references, tr("变量引用"));
            rollbackReferences.undo();
            RenameVariableUndoCommand rollbackName(document_, before, after, tr("变量名称"));
            rollbackName.undo();
            QMessageBox::warning(this, tr("无法重命名变量"), tr("引用更新失败，已恢复原始名称与引用。"));
            return;
        }
    }
    auto* transaction = new QUndoCommand(tr("重命名变量"));
    new RenameVariableUndoCommand(document_, before, after, tr("变量名称"), transaction);
    if (!references.empty()) {
        new EditTriggerArgumentsUndoCommand(document_, std::move(references), tr("变量引用"), transaction);
    }
    undoStack_->push(transaction);
    RebuildList(variableId);
}

void VariablesDialog::RemoveSelected() {
    const auto row = SelectedRow();
    if (document_ == nullptr || undoStack_ == nullptr || !row.has_value() ||
        *row >= document_->variables.size()) {
        return;
    }
    const War3Map::Trigger::VariableRecord record = document_->variables[*row];
    if (QMessageBox::question(
            this, tr("删除变量"),
            tr("确定删除变量“%1”吗？引用它的脚本将不再编译（可撤销）。")
                .arg(QString::fromUtf8(record.name.text)),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No) != QMessageBox::Yes) {
        return;
    }
    const War3Map::Status removed =
        War3Map::Trigger::TriggerDocumentEditor::RemoveVariable(
            *document_, record.name.text);
    if (!removed.Succeeded()) {
        return;
    }
    undoStack_->push(new RemoveVariableUndoCommand(
        document_, record, *row, tr("删除变量")));
    RebuildList();
}
