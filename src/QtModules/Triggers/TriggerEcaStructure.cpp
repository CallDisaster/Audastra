#include "TriggerModule.h"

#include "EditorProject.h"
#include "Icons/SvgIcons.h"
#include "TriggerCommands.h"
#include "TriggerDialogs.h"
#include "TriggerDirectoryCommands.h"
#include "TriggerModels.h"

#include "War3MapCore/Trigger/UiCatalog.h"

#include <QAction>
#include <QMenu>
#include <QTreeView>
#include <QUndoStack>

#include <algorithm>

// 语句结构（对应原型传统视图的行操作）：上移 / 下移（Alt+↑↓）、拖放跨分支移动、
// 在语句之后添加、添加到块语句的分支（含空的“否则”）。每次改动都做整图验证，失败即回滚。
namespace War3Map::Modules {
namespace {

using Editor = Trigger::TriggerDocumentEditor;

Trigger::UiDefinitionKind KindOf(std::int32_t type) {
    switch (type) {
    case 0: return Trigger::UiDefinitionKind::Event;
    case 1: return Trigger::UiDefinitionKind::Condition;
    default: return Trigger::UiDefinitionKind::Action;
    }
}

QString KindName(std::int32_t type) {
    switch (type) {
    case 0: return QObject::tr("事件");
    case 1: return QObject::tr("条件");
    default: return QObject::tr("动作");
    }
}

} // namespace

void TriggerModule::ApplyEcaMove(const EcaMoveRequest& request) {
    auto* document = project_.triggerDocument_.get();
    const auto* trigger = CurrentTrigger();
    if (document == nullptr || trigger == nullptr || project_.uiCatalog_ == nullptr || project_.triggerUndoStack_ == nullptr) {
        return;
    }
    const bool root = request.slot.parentEcaId == 0;
    auto moved = Editor::MoveEca(*document, request.ecaId, root ? trigger->id : 0, request.slot.parentEcaId,
        root ? std::nullopt : request.slot.childId, request.beforeEcaId);
    if (!moved.Succeeded()) {
        ShowStatus(tr("不能移动到这里：%1").arg(QString::fromUtf8(moved.diagnostics.front().message)), 6000);
        return;
    }
    if (!ValidateOrReport()) {
        (void)Editor::UndoMoveEca(*document, *moved.value);
        ShowStatus(tr("移动后未通过 WTG/WCT 图验证，已回滚。"), 8000);
        RefreshEditor(request.ecaId);
        return;
    }
    project_.triggerUndoStack_->push(new MoveEcaUndoCommand(document, std::move(*moved.value), tr("移动语句")));
    RefreshEditor(request.ecaId);
}

void TriggerModule::MoveSelectedEca(int delta) {
    const auto* eca = ecaModel_->EcaAt(ecaView_->currentIndex());
    if (eca == nullptr || delta == 0) {
        return;
    }
    const StableId id = eca->id;
    const auto slot = ecaModel_->SlotOf(id);
    const auto siblings = ecaModel_->SiblingsOf(id);
    const auto at = std::find(siblings.begin(), siblings.end(), id);
    if (!slot || at == siblings.end()) {
        return;
    }
    const auto position = static_cast<std::ptrdiff_t>(at - siblings.begin());
    const std::ptrdiff_t target = position + delta;
    if (target < 0 || target >= static_cast<std::ptrdiff_t>(siblings.size())) {
        return; // 已在分支首 / 尾
    }
    // 上移：插在上一个之前；下移：插在下下个之前（没有则放到末尾）。
    const std::ptrdiff_t beforeIndex = delta < 0 ? target : target + 1;
    const StableId before = beforeIndex < static_cast<std::ptrdiff_t>(siblings.size())
        ? siblings[static_cast<std::size_t>(beforeIndex)] : 0;
    ApplyEcaMove({id, *slot, before});
}

StableId TriggerModule::InsertEca(const EcaSlot& slot, StableId beforeEcaId, const std::string& functionName) {
    auto* document = project_.triggerDocument_.get();
    const auto* trigger = CurrentTrigger();
    if (document == nullptr || trigger == nullptr || trigger->customTextFlag != 0 || project_.uiCatalog_ == nullptr ||
        project_.triggerUndoStack_ == nullptr) {
        return 0;
    }
    const StableId triggerId = trigger->id;
    auto appended = Editor::AppendRootEca(*document, *project_.uiCatalog_, triggerId, KindOf(slot.type), functionName);
    if (!appended.Succeeded()) {
        project_.AddDiagnostics(appended.diagnostics);
        ShowStatus(tr("无法创建语句：%1").arg(QString::fromUtf8(appended.diagnostics.front().message)), 8000);
        return 0;
    }
    const StableId addedId = appended.value->rootEcaId;
    // 新语句先追加在触发器根，再移到目标位置（根末尾之外的任何位置都需要移动）。
    std::optional<Editor::EcaMoveReceipt> move;
    if (slot.parentEcaId != 0 || beforeEcaId != 0) {
        auto moved = Editor::MoveEca(*document, addedId, slot.parentEcaId == 0 ? triggerId : 0, slot.parentEcaId,
            slot.parentEcaId == 0 ? std::nullopt : slot.childId, beforeEcaId);
        if (!moved.Succeeded()) {
            (void)Editor::UndoAppend(*document, *appended.value);
            project_.AddDiagnostics(moved.diagnostics);
            return 0;
        }
        move = std::move(*moved.value);
    }
    if (!ValidateOrReport()) {
        if (move) (void)Editor::UndoMoveEca(*document, *move);
        (void)Editor::UndoAppend(*document, *appended.value);
        ShowStatus(tr("新增语句没有通过 WTG/WCT 图验证，已回滚。"), 8000);
        return 0;
    }
    const QString label = tr("新增%1").arg(KindName(slot.type));
    auto* stack = project_.triggerUndoStack_;
    stack->beginMacro(label);
    stack->push(new AppendEcaUndoCommand(document, std::move(*appended.value), label));
    if (move) stack->push(new MoveEcaUndoCommand(document, std::move(*move), tr("移动语句")));
    stack->endMacro();
    RefreshEditor(addedId);
    return addedId;
}

void TriggerModule::ChooseAndInsertEca(const EcaSlot& slot, StableId beforeEcaId) {
    if (project_.uiCatalog_ == nullptr) {
        return;
    }
    UiDefinitionPickerDialog picker(*project_.uiCatalog_, KindOf(slot.type), {}, CategoryIcons(), page_);
    if (picker.exec() != QDialog::Accepted || picker.SelectedDefinition() == nullptr) {
        return;
    }
    const StableId added = InsertEca(slot, beforeEcaId, picker.SelectedDefinition()->name);
    const auto* node = added == 0 || project_.triggerDocument_ == nullptr ? nullptr
        : Editor::FindEca(*project_.triggerDocument_, added);
    if (node != nullptr && !node->arguments.empty()) {
        EditEcaParameters(added);
    }
}

void TriggerModule::AddEcaStructureMenu(
    QMenu& menu, const QModelIndex& index, std::vector<std::pair<QAction*, std::function<void()>>>& actions) {
    const auto bind = [&actions](QAction* action, std::function<void()> run) { actions.emplace_back(action, std::move(run)); };
    // 分组头 / 分支头：添加到这里。
    if (const auto slot = ecaModel_->SlotAt(index)) {
        bind(menu.addAction(UiKit::SvgIcon(QStringLiteral("lucide/plus")), tr("添加%1到“%2”…").arg(KindName(slot->type), slot->label)),
            [this, slot = *slot]() { ChooseAndInsertEca(slot, 0); });
        return;
    }
    const auto* eca = ecaModel_->EcaAt(index);
    if (eca == nullptr) {
        return;
    }
    const StableId id = eca->id;
    const auto slot = ecaModel_->SlotOf(id);
    const auto siblings = ecaModel_->SiblingsOf(id);
    const auto at = std::find(siblings.begin(), siblings.end(), id);
    if (slot) {
        const StableId next = at != siblings.end() && at + 1 != siblings.end() ? *(at + 1) : 0;
        bind(menu.addAction(UiKit::SvgIcon(QStringLiteral("lucide/plus")), tr("在之后添加%1…").arg(KindName(slot->type))),
            [this, slot = *slot, next]() { ChooseAndInsertEca(slot, next); });
    }
    const auto branches = ecaModel_->BranchesOf(id);
    if (!branches.empty()) {
        QMenu* into = menu.addMenu(tr("添加到子块"));
        for (const EcaSlot& branch : branches) {
            bind(into->addAction(branch.label), [this, branch]() { ChooseAndInsertEca(branch, 0); });
        }
    }
    QAction* up = menu.addAction(UiKit::SvgIcon(QStringLiteral("lucide/chevron-up")), tr("上移"));
    up->setShortcut(QKeySequence(Qt::ALT | Qt::Key_Up));
    up->setShortcutVisibleInContextMenu(true);
    up->setEnabled(at != siblings.end() && at != siblings.begin());
    bind(up, [this]() { MoveSelectedEca(-1); });
    QAction* down = menu.addAction(UiKit::SvgIcon(QStringLiteral("lucide/chevron-down")), tr("下移"));
    down->setShortcut(QKeySequence(Qt::ALT | Qt::Key_Down));
    down->setShortcutVisibleInContextMenu(true);
    down->setEnabled(at != siblings.end() && at + 1 != siblings.end());
    bind(down, [this]() { MoveSelectedEca(1); });
}

} // namespace War3Map::Modules
