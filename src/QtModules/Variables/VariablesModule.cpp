#include "VariablesModule.h"
#include "VariablesDialog.h"
#include "EditorProject.h"
#include "ObjectNames.h"
#include "ProjectRemovalCodeAudit.h"
#include "TriggerCommands.h"
#include "Modules/ModuleHost.h"
#include <QLabel>
#include <QScrollBar>
#include <QJsonArray>
#include <QTableWidget>

#include "Widgets/BlockingTask.h"
#include "Widgets/ColumnLayout.h"
#include <QMessageBox>
#include <QUndoStack>
#include <QVBoxLayout>

#include <algorithm>

namespace War3Map::Modules {

VariablesModule::VariablesModule(QtProject::EditorProject& project) : project_(project) {
    connect(&project_, &QtProject::EditorProject::TriggerDocumentAboutToChange,
        this, &VariablesModule::ReleaseEditor);
    connect(&project_, &QtProject::EditorProject::TriggerDocumentChanged, this, &VariablesModule::Refresh);
    connect(&project_, &QtProject::EditorProject::MapDocumentChanged, this, [this] { if (editor_) editor_->RefreshNames(); });
    connect(&project_, &QtProject::EditorProject::ObjectDocumentsReplaced, this, [this] { if (editor_) editor_->RefreshNames(); });
    connect(&project_, &QtProject::EditorProject::WorldEditStringsChanged, this, [this] { if (editor_) editor_->RefreshNames(); });
    connect(&project_, &QtProject::EditorProject::UiLibrariesChanged, this, [this] { if (editor_) editor_->RefreshNames(); });
    for (auto* stack : project_.objectUndoStacks_) {
        if (stack) connect(stack, &QUndoStack::indexChanged, this, [this] { if (editor_) editor_->RefreshNames(); });
    }
}

VariablesModule::~VariablesModule() { delete page_.data(); }

Shell::ModuleInfo VariablesModule::Info() const {
    return {"variables", tr("变量"), "lucide/variable", QKeySequence(Qt::ALT | Qt::Key_3)};
}

QWidget* VariablesModule::Widget() {
    if (!page_) {
        page_ = new QWidget;
        page_->setObjectName("variablesWorkspace");
        layout_ = new QVBoxLayout(page_);
        layout_->setContentsMargins(12, 10, 12, 10);
        auto* title = new QLabel(tr("全局变量"), page_);
        title->setObjectName("paneTitle");
        layout_->addWidget(title);
        Refresh();
    }
    return page_;
}

void VariablesModule::AttachHost(Shell::ModuleHost* host) { host_ = host; }
QUndoStack* VariablesModule::UndoStack() { return project_.triggerUndoStack_; }

void VariablesModule::ReleaseEditor() {
    delete editor_.data();
    delete empty_.data();
    boundDocument_ = nullptr;
    boundUndoStack_ = nullptr;
    pendingViewState_ = {}; // 新文档不能沿用旧工程的稳定 ID。
}

void VariablesModule::Refresh() {
    if (!page_) return;
    const auto* document = project_.triggerDocument_.get();
    if (document == boundDocument_ && boundUndoStack_ == project_.triggerUndoStack_
        && (editor_ || empty_)) return;
    const auto restore = pendingViewState_;
    ReleaseEditor();
    if (document) {
        editor_ = new VariablesDialog(project_.triggerDocument_.get(), project_.triggerUndoStack_, page_, true);
        editor_->SetNameResolver([this](std::string_view raw, std::string_view type) { return ResolveProjectName(project_, raw, type); });
        editor_->SetGraphEditing({
            [this] { return project_.uiCatalog_.get(); },
            [this](std::string_view type) {
                std::vector<VariablePropertiesDialog::Choice> choices;
                for (auto& object : ProjectObjectChoices(project_, type)) choices.push_back({std::move(object.rawcode), std::move(object.name)});
                return choices;
            },
            [this](std::vector<Trigger::GraphEditOperation> operations, const QString& label) {
                return Status{QtProject::PublishTriggerGraphOperations(project_, std::move(operations), label).diagnostics};
            }});
        // 删除前与 Agent 删除门禁同一审计：触发器参数、代码里的直接引用和按名使用。
        editor_->SetRemovalCheck([this](const Trigger::VariableRecord& record) -> std::optional<VariablesDialog::RemovalReview> {
            Trigger::GraphRemove remove;
            remove.target = {Trigger::GraphRecordKind::Variable, record.id, {}};
            remove.expectedName = record.name.text;
            const auto audit = QtProject::RemovalCodeAudit::Prepare(project_, {remove}, {});
            std::optional<QtProject::RemovalCodeNotice> result;
            const bool finished = UiKit::RunBlockingTask(page_, tr("正在检查变量的引用…"), tr("取消删除"),
                [&](const UiKit::BlockingTaskControl& control) { result = audit.Run({}, *control.stop); });
            if (!finished || !result) return std::nullopt;
            if (!audit.Current(project_)) {
                QMessageBox::information(page_, tr("删除变量"), tr("检查期间地图有变化，未删除；请重新删除。"));
                return std::nullopt;
            }
            return VariablesDialog::RemovalReview{std::move(result->lines), result->references, std::move(result->incomplete)};
        });
        layout_->addWidget(editor_, 1);
    } else {
        empty_ = new QLabel(tr("未加载触发器"), page_);
        empty_->setObjectName("emptyState");
        empty_->setAlignment(Qt::AlignCenter);
        layout_->addWidget(empty_, 1);
    }
    boundDocument_ = document;
    boundUndoStack_ = project_.triggerUndoStack_;
    if (!restore.isEmpty()) RestoreViewState(restore);
    if (host_) host_->UndoStackChanged("variables");
}

bool VariablesModule::Reveal(const Shell::NavigationTarget& target) {
    if (target.module != "variables" || (target.kind != "variable" && !target.kind.isEmpty())) return false;
    (void)Widget();
    Refresh();
    auto* table = editor_ ? editor_->findChild<QTableWidget*>("globalVariablesTable") : nullptr;
    if (!table) return false;
    const auto stableValue = target.extra.value("stableIdentity");
    if (target.extra.contains("stableIdentity") && stableValue.typeId() != QMetaType::Bool) return false;
    const bool stableOnly = stableValue.toBool();
    if (stableOnly && (target.id.isEmpty() || target.id.size() > 20 ||
        !std::all_of(target.id.cbegin(), target.id.cend(), [](QChar c) { return c >= '0' && c <= '9'; }))) return false;
    bool numeric = false;
    const auto id = target.id.toULongLong(&numeric);
    if (stableOnly && !numeric) return false;
    for (int row = 0; row < table->rowCount(); ++row) {
        const auto* cell = table->item(row, 0);
        if ((numeric && cell->data(Qt::UserRole).toULongLong() == id) || (!stableOnly && cell->text() == target.id)) {
            table->setCurrentCell(row, 0);
            table->selectRow(row);
            table->scrollToItem(table->item(row, 0));
            return true;
        }
    }
    return false;
}

QJsonObject VariablesModule::SaveViewState() const {
    auto* table = editor_ ? editor_->findChild<QTableWidget*>("globalVariablesTable") : nullptr;
    if (!table) return pendingViewState_;
    const auto* cell = table->item(table->currentRow(), 0);
    return {{"selected", cell ? QString::number(cell->data(Qt::UserRole).toULongLong()) : QString{}},
        {"scroll", table->verticalScrollBar()->value()},
        {"columns", UiKit::SaveColumnWidths(table->horizontalHeader())}};
}

void VariablesModule::RestoreViewState(const QJsonObject& state) {
    pendingViewState_ = state;
    if (!editor_) return;
    const auto selected = state["selected"].toString();
    if (!selected.isEmpty()) (void)Reveal({"variables", "variable", selected, {}});
    auto* table = editor_->findChild<QTableWidget*>("globalVariablesTable");
    if (table) {
        UiKit::RestoreColumnWidths(table->horizontalHeader(), state["columns"].toArray());
        table->verticalScrollBar()->setValue(state["scroll"].toInt());
    }
}

} // namespace War3Map::Modules
