#include "TriggerModule.h"

#include "EditorProject.h"
#include "TriggerCommands.h"
#include "TriggerDialogs.h"
#include "TriggerBlueprint.h"
#include "TriggerModels.h"

#include "War3MapCore/Trigger/TriggerCodec.h"
#include "War3MapCore/Trigger/UiCatalog.h"

#include <QAction>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QShortcut>
#include <QTreeView>
#include <QUndoStack>

#include <algorithm>

// ECA 节点编辑：新增、参数、删除、复制 / 剪切 / 粘贴与右键菜单。
// 每次改动先在文档上执行，再做整图验证；验证不过立即回滚，只有通过的修改才入撤销栈。
namespace War3Map::Modules {
namespace {

using Editor = Trigger::TriggerDocumentEditor;

QString FirstMessage(const std::vector<Diagnostic>& diagnostics, const QString& fallback) {
    return diagnostics.empty() ? fallback : QString::fromUtf8(diagnostics.front().message);
}

} // namespace

void TriggerModule::BuildShortcuts() {
    // 逻辑区快捷键只在逻辑区有焦点时生效（目录的同名按键由 HandleExplorerKey 处理）。
    const auto bind = [this](const QKeySequence& key, auto slot) {
        for (QWidget* target : {static_cast<QWidget*>(ecaView_), static_cast<QWidget*>(blueprint_)}) {
            auto* shortcut = new QShortcut(key, target);
            shortcut->setContext(Qt::WidgetShortcut);
            connect(shortcut, &QShortcut::activated, this, slot);
        }
    };
    // Enter：在取值面板中编辑首个参数（原型传统视图）；F2 / 双击：参数对话框。
    for (const QKeySequence& key : {QKeySequence(Qt::Key_Return), QKeySequence(Qt::Key_Enter)}) {
        bind(key, [this]() { EditFirstArgument(); });
    }
    bind(QKeySequence(Qt::Key_F2), [this]() { EditSelectedEca(); });
    bind(QKeySequence::Delete, [this]() { DeleteSelectedEca(); });
    bind(QKeySequence::Copy, [this]() { CopySelectedEca(false); });
    bind(QKeySequence::Cut, [this]() { CopySelectedEca(true); });
    bind(QKeySequence::Paste, [this]() { PasteEca(); });
    // Ctrl+D 快速复制节点（复制 + 立即粘贴到原节点后）。
    bind(QKeySequence(Qt::CTRL | Qt::Key_D), [this]() {
        CopySelectedEca(false);
        PasteEca();
    });
    // Alt+↑↓ 在同一分支内重排（原型传统视图）。
    bind(QKeySequence(Qt::ALT | Qt::Key_Up), [this]() { MoveSelectedEca(-1); });
    bind(QKeySequence(Qt::ALT | Qt::Key_Down), [this]() { MoveSelectedEca(1); });
}

bool TriggerModule::ValidateOrReport() {
    const Status validation = Trigger::WtgWctCodec{}.Validate(
        *project_.triggerDocument_, *project_.uiCatalog_);
    if (!validation.Succeeded()) {
        project_.AddDiagnostics(validation.diagnostics);
    }
    return validation.Succeeded();
}

void TriggerModule::ShowEcaContextMenu(const QPoint& pos) {
    ShowEcaMenuAt(ecaView_->indexAt(pos), ecaView_->viewport()->mapToGlobal(pos));
}

void TriggerModule::AddEcaCommands(
    QMenu& menu, StableId ecaId, std::vector<std::pair<QAction*, std::function<void()>>>& actions) {
    auto* document = project_.triggerDocument_.get();
    const auto* eca = document == nullptr ? nullptr : Editor::FindEca(*document, ecaId);
    if (eca == nullptr || project_.triggerUndoStack_ == nullptr) {
        return;
    }
    const auto bind = [&actions](QAction* action, std::function<void()> run) { actions.emplace_back(action, std::move(run)); };
    const bool enabled = eca->enabled != 0;
    QAction* edit = menu.addAction(tr("编辑参数…"));
    edit->setShortcut(QKeySequence(Qt::Key_F2));
    edit->setShortcutVisibleInContextMenu(true);
    edit->setEnabled(!eca->uiSource.empty() && !eca->arguments.empty());
    bind(edit, [this, ecaId]() { EditEcaParameters(ecaId); });
    QAction* toggle = menu.addAction(enabled ? tr("禁用") : tr("启用"));
    toggle->setToolTip(tr("禁用后该语句显示为灰字，不会编译进脚本"));
    bind(toggle, [this, document, ecaId, enabled]() {
        project_.triggerUndoStack_->push(new SetEcaEnabledUndoCommand(
            document, ecaId, enabled, !enabled, enabled ? tr("禁用语句") : tr("启用语句")));
        RefreshEditor(ecaId);
    });
    menu.addSeparator();
    const auto shortcut = [&menu, &bind](const QString& text, const QKeySequence& key, std::function<void()> run) {
        QAction* action = menu.addAction(text);
        action->setShortcut(key);
        action->setShortcutVisibleInContextMenu(true);
        bind(action, std::move(run));
        return action;
    };
    shortcut(tr("复制"), QKeySequence::Copy, [this]() { CopySelectedEca(false); });
    shortcut(tr("剪切"), QKeySequence::Cut, [this]() { CopySelectedEca(true); });
    shortcut(tr("粘贴"), QKeySequence::Paste, [this]() { PasteEca(); })->setEnabled(clipboard_.has_value());
    shortcut(tr("创建副本"), QKeySequence(Qt::CTRL | Qt::Key_D), [this]() { CopySelectedEca(false); PasteEca(); });
    menu.addSeparator();
    const bool blueprint = BlueprintVisible();
    bind(menu.addAction(blueprint ? tr("在传统视图中定位") : tr("在蓝图中定位")), [this, blueprint, ecaId]() {
        SetBlueprintVisible(!blueprint);
        if (!blueprint) Blueprint()->Select(ecaId);
        else ecaView_->scrollTo(ecaModel_->IndexForStableId(ecaId), QAbstractItemView::PositionAtCenter);
    });
    shortcut(tr("删除"), QKeySequence::Delete, [this]() { DeleteSelectedEca(); })
        ->setToolTip(tr("删除该语句及其整个子块（可撤销）"));
}

void TriggerModule::ShowEcaMenuAt(const QModelIndex& index, const QPoint& globalPos) {
    auto* document = project_.triggerDocument_.get();
    if (document == nullptr || project_.triggerUndoStack_ == nullptr) {
        return;
    }
    if (index.isValid()) {
        ecaView_->setCurrentIndex(index); // 右键即选中
    }
    QMenu menu(page_);
    std::vector<std::pair<QAction*, std::function<void()>>> actions;
    const auto* eca = ecaModel_->EcaAt(index);
    AddEcaStructureMenu(menu, index, actions);
    if (eca != nullptr) {
        menu.addSeparator();
        AddEcaCommands(menu, eca->id, actions);
    }
    if (actions.empty()) {
        return;
    }
    QAction* chosen = menu.exec(globalPos);
    for (auto& [action, run] : actions) {
        if (action == chosen) {
            run();
            break;
        }
    }
}

void TriggerModule::AddEca(int ecaType) {
    auto* document = project_.triggerDocument_.get();
    if (document == nullptr || project_.uiCatalog_ == nullptr) {
        return;
    }
    const auto* trigger = CurrentTrigger();
    if (trigger == nullptr || trigger->customTextFlag != 0) {
        QMessageBox::information(page_, tr("不能插入 GUI 语句"),
            tr("请先选择一个普通 GUI 触发器；自定义脚本触发器没有 ECA 编辑结构。"));
        return;
    }
    // 目标位置（原型行为：添加到所选行之后）：选中的分组头 / 分支头接受该类型时放进去；
    // 选中同类型语句时插在它之后；否则放到触发器根该组末尾。
    EcaSlot slot{0, std::nullopt, ecaType == 0 ? 0 : ecaType == 1 ? 1 : 2, {}};
    StableId before = 0;
    const QModelIndex current = ecaView_->currentIndex();
    if (const auto header = ecaModel_->SlotAt(current); header && header->type == slot.type) {
        slot = *header;
    } else if (const auto* selected = ecaModel_->EcaAt(current)) {
        if (const auto own = ecaModel_->SlotOf(selected->id); own && own->type == slot.type) {
            slot = *own;
            const auto siblings = ecaModel_->SiblingsOf(selected->id);
            const auto at = std::find(siblings.begin(), siblings.end(), selected->id);
            if (at != siblings.end() && at + 1 != siblings.end()) before = *(at + 1);
        }
    }
    ChooseAndInsertEca(slot, before);
}

void TriggerModule::EditSelectedEca() {
    const QModelIndex selected = ecaView_->currentIndex();
    const auto* eca = ecaModel_->EcaAt(selected);
    if (eca == nullptr) {
        return;
    }
    EditEcaParameters(eca->id);
}

void TriggerModule::DeleteSelectedEca() {
    auto* document = project_.triggerDocument_.get();
    if (document == nullptr || project_.uiCatalog_ == nullptr || project_.triggerUndoStack_ == nullptr) {
        return;
    }
    const auto* eca = ecaModel_->EcaAt(ecaView_->currentIndex());
    if (eca == nullptr) {
        return; // 分组头 / 角色头或空白：无可删除节点
    }
    const StableId ecaId = eca->id;
    auto removed = Editor::RemoveEcaSubtree(*document, ecaId);
    if (!removed.Succeeded()) {
        project_.AddDiagnostics(removed.diagnostics);
        return;
    }
    if (!ValidateOrReport()) {
        (void)Editor::UndoRemoveEcaSubtree(*document, *removed.value);
        QMessageBox::warning(page_, tr("无法删除节点"), tr("删除后未通过 WTG/WCT 图验证，修改已经回滚。"));
        return;
    }
    project_.triggerUndoStack_->push(new RemoveEcaUndoCommand(document, std::move(*removed.value), tr("删除节点")));
    RefreshEditor();
    project_.AddDiagnostic(tr("信息"), tr("触发器"), tr("ECA_REMOVED"),
        tr("已删除节点 %1（含其子树）；修改仅存在于内存事务，源 WTG/WCT 未被覆盖。").arg(static_cast<qulonglong>(ecaId)));
}

void TriggerModule::CopySelectedEca(bool cut) {
    auto* document = project_.triggerDocument_.get();
    if (document == nullptr) {
        return;
    }
    const auto* eca = ecaModel_->EcaAt(ecaView_->currentIndex());
    if (eca == nullptr) {
        ShowStatus(tr("请先选中一个节点再复制。"), 5000);
        return;
    }
    auto snapshot = Editor::CopyEcaSubtree(*document, eca->id);
    if (!snapshot.Succeeded()) {
        ShowStatus(tr("无法复制该节点。"), 8000);
        return;
    }
    const std::size_t nodeCount = snapshot.value->ecas.size();
    clipboard_ = std::move(*snapshot.value);
    ShowStatus(cut ? tr("已剪切 %1 个节点，Ctrl+V 粘贴。").arg(nodeCount) : tr("已复制 %1 个节点，Ctrl+V 粘贴。").arg(nodeCount), 6000);
    if (cut) {
        DeleteSelectedEca();
    }
}

void TriggerModule::PasteEca() {
    auto* document = project_.triggerDocument_.get();
    if (document == nullptr || project_.uiCatalog_ == nullptr || project_.triggerUndoStack_ == nullptr ||
        !clipboard_.has_value()) {
        return;
    }
    const QModelIndex selected = ecaView_->currentIndex();
    const auto* eca = ecaModel_->EcaAt(selected);
    if (eca == nullptr) {
        ShowStatus(tr("请先选中一个节点作为粘贴位置（粘贴为其后一个兄弟节点）。"), 6000);
        return;
    }
    // 兄弟语义：粘贴到当前节点之后，分支号（childId）与当前节点一致。
    const auto placement = Editor::LocateEcaPlacement(*document, eca->id);
    if (!placement.found) {
        return;
    }
    const auto childId = eca->childId;
    auto paste = Editor::PasteEcaSubtree(*document, *project_.uiCatalog_, *clipboard_,
        placement.parentTriggerId, placement.parentEcaId, placement.row + 1, childId);
    if (!paste.Succeeded()) {
        project_.AddDiagnostics(paste.diagnostics);
        return;
    }
    if (!ValidateOrReport()) {
        (void)Editor::RemoveEcaSubtree(*document, paste.value->rootEcaId);
        QMessageBox::warning(page_, tr("无法粘贴"), tr("粘贴后未通过 WTG/WCT 图验证，修改已经回滚。"));
        return;
    }
    project_.triggerUndoStack_->push(new PasteEcaUndoCommand(document, project_.uiCatalog_.get(), *clipboard_,
        placement.parentTriggerId, placement.parentEcaId, placement.row + 1, childId,
        paste.value->rootEcaId, tr("粘贴节点")));
    RefreshEditor(paste.value->rootEcaId);
}

void TriggerModule::EditEcaParameters(StableId ecaId) {
    // 参数对话框（F2 / 双击）：接受后按与取值面板相同的管线应用；请求编辑嵌套函数时继续打开它的对话框。
    while (ecaId != 0 && project_.triggerDocument_ != nullptr && project_.uiCatalog_ != nullptr) {
        auto& document = *project_.triggerDocument_;
        if (Editor::FindEca(document, ecaId) == nullptr) {
            return;
        }
        TriggerParameterDialog dialog(document, *project_.uiCatalog_, ecaId, page_);
        dialog.SetCategoryIcons(CategoryIcons());
        if (dialog.exec() != QDialog::Accepted) {
            return;
        }
        std::optional<StableId> newNestedEcaId;
        if (!ApplyArgumentEdits(ecaId, dialog.PendingEdits(), tr("编辑参数"), nullptr, &newNestedEcaId)) {
            return;
        }
        auto nested = dialog.RequestedNestedEca();
        if (!nested.has_value()) {
            nested = newNestedEcaId;
        }
        if (!nested.has_value()) {
            return;
        }
        ecaId = *nested;
    }
}

} // namespace War3Map::Modules
