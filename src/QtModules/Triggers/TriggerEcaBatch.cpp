#include "TriggerModule.h"

#include "EditorProject.h"
#include "TriggerActionHelpers.h"
#include "TriggerCommands.h"
#include "TriggerDirectoryCommands.h"
#include "TriggerModels.h"
#include "TriggerModelsDetail.h"

#include "War3MapCore/Trigger/TriggerSelection.h"
#include "War3MapCore/Trigger/UiCatalog.h"

#include <QMessageBox>
#include <QTreeView>
#include <QUndoStack>

#include <algorithm>
#include <set>

// 语句的批量操作（多选）：删除、粘贴多段、创建副本、允许 / 禁用、整段上移 / 下移、拖放多条。
// 先在文档上执行再整图验证，验证不过全部回滚；通过后整批作为一步撤销入栈，并选中结果。
namespace War3Map::Modules {
namespace {

using Editor = Trigger::TriggerDocumentEditor;
using TriggerModelsDetail::EcaKindName;
using TriggerActions::FirstMessage;

// 删除后的当前语句：第一段之后第一条留下的兄弟，其次之前的，都没有时为所在块语句（根上为 0）。
StableId FocusAfterRemoval(const Trigger::TriggerDocument& document, StableId triggerId, const Trigger::EcaSelection& selection) {
    if (selection.runs.empty()) return 0;
    const auto& run = selection.runs.front();
    const std::set<StableId> removed(selection.ids.begin(), selection.ids.end());
    const auto siblings = Trigger::EcaSlotSiblings(document, triggerId, run.slot);
    const auto first = std::find(siblings.begin(), siblings.end(), run.ids.front());
    const auto last = std::find(siblings.begin(), siblings.end(), run.ids.back());
    if (first != siblings.end() && last != siblings.end()) {
        for (auto next = last + 1; next != siblings.end(); ++next) {
            if (!removed.contains(*next)) return *next;
        }
        for (auto previous = first; previous != siblings.begin();) {
            --previous;
            if (!removed.contains(*previous)) return *previous;
        }
    }
    return run.slot.parentEcaId;
}

// 拖放位置换成 Core 的位置键：触发器根按类型分组，块语句按分支号。
Trigger::EcaSlotKey SlotKeyOf(const EcaSlot& slot) {
    return slot.parentEcaId == 0 ? Trigger::EcaSlotKey{0, slot.type} : Trigger::EcaSlotKey{slot.parentEcaId, slot.childId.value_or(0)};
}

} // namespace

void TriggerModule::DeleteEcas(const std::vector<StableId>& ecaIds) {
    auto* document = project_.triggerDocument_.get();
    if (document == nullptr || project_.uiCatalog_ == nullptr || project_.triggerUndoStack_ == nullptr) {
        return;
    }
    const auto selection = Trigger::NormalizeEcaSelection(*document, displayedTrigger_, ecaIds);
    if (selection.ids.empty()) {
        return;
    }
    const StableId focus = FocusAfterRemoval(*document, displayedTrigger_, selection);
    auto removed = Trigger::RemoveEcaSubtrees(*document, selection.ids);
    if (!removed.Succeeded()) {
        project_.AddDiagnostics(removed.diagnostics);
        ShowStatus(tr("未删除：%1").arg(FirstMessage(removed.diagnostics, tr("语句已变化"))), 8000);
        return;
    }
    if (!ValidateOrReport()) {
        for (auto receipt = removed.value->rbegin(); receipt != removed.value->rend(); ++receipt) {
            (void)Editor::UndoRemoveEcaSubtree(*document, *receipt);
        }
        QMessageBox::warning(page_, tr("无法删除节点"), tr("删除后未通过 WTG/WCT 图验证，修改已经回滚。"));
        return;
    }
    // 先记下删除后的选区：入栈后的刷新按它恢复。
    ecaSelection_.clear();
    if (focus != 0) ecaSelection_.push_back(focus);
    auto* stack = project_.triggerUndoStack_;
    stack->beginMacro(tr("删除 %1 条语句").arg(selection.ids.size()));
    for (auto& receipt : *removed.value) {
        stack->push(new RemoveEcaUndoCommand(document, std::move(receipt), tr("删除节点")));
    }
    stack->endMacro();
    RefreshEditor(focus);
    project_.AddDiagnostic(tr("信息"), tr("触发器"), tr("ECA_REMOVED"),
        tr("已删除 %1 条语句（含其子树）；修改仅存在于内存事务，源 WTG/WCT 未被覆盖。").arg(selection.ids.size()));
}

bool TriggerModule::PasteEcaPlans(const std::vector<EcaPastePlan>& plans, const QString& label) {
    auto* document = project_.triggerDocument_.get();
    const auto* catalog = project_.uiCatalog_.get();
    if (document == nullptr || catalog == nullptr || project_.triggerUndoStack_ == nullptr || plans.empty()) {
        return false;
    }
    // 块语句的分支只接受该分支的语句类型：有不合适的就整体不粘贴（触发器根按类型自动归组，都可以）。
    for (const auto& plan : plans) {
        const auto slot = ecaModel_->SlotOf(plan.anchorEcaId);
        if (!slot) {
            ShowStatus(tr("请先选中粘贴位置"), 6000);
            return false;
        }
        if (slot->parentEcaId == 0) continue;
        for (const auto* snapshot : plan.snapshots) {
            const std::int32_t type = TriggerActions::SnapshotRootType(*snapshot);
            if (TriggerModelsDetail::TypeToGroup(type) != slot->type) {
                ShowStatus(tr("不能把%1粘贴到“%2”").arg(EcaKindName(type), slot->label), 8000);
                return false;
            }
        }
    }
    struct Pasted {
        Editor::EcaPasteReceipt receipt;
        const Editor::EcaSubtreeSnapshot* snapshot = nullptr;
        std::optional<std::int32_t> childId;
    };
    std::vector<Pasted> pasted;
    bool failed = false;
    for (const auto& plan : plans) {
        // 兄弟语义：依次放在参照语句之后，分支号（childId）与参照语句一致。
        const auto* anchor = Editor::FindEca(*document, plan.anchorEcaId);
        const auto placement = Editor::LocateEcaPlacement(*document, plan.anchorEcaId);
        if (anchor == nullptr || !placement.found) {
            failed = true;
            break;
        }
        const auto childId = anchor->childId;
        for (std::size_t offset = 0; offset < plan.snapshots.size(); ++offset) {
            auto paste = Editor::PasteEcaSubtree(*document, *catalog, *plan.snapshots[offset],
                placement.parentTriggerId, placement.parentEcaId, placement.row + 1 + offset, childId);
            if (!paste.Succeeded()) {
                project_.AddDiagnostics(paste.diagnostics);
                failed = true;
                break;
            }
            pasted.push_back({*paste.value, plan.snapshots[offset], childId});
        }
        if (failed) break;
    }
    const bool valid = !failed && ValidateOrReport();
    if (!valid) {
        for (auto item = pasted.rbegin(); item != pasted.rend(); ++item) {
            (void)Editor::RemoveEcaSubtree(*document, item->receipt.rootEcaId);
        }
        QMessageBox::warning(page_, tr("无法粘贴"), failed ? tr("粘贴失败，修改已经回滚；详见诊断。")
                                                         : tr("粘贴后未通过 WTG/WCT 图验证，修改已经回滚。"));
        return false;
    }
    ecaSelection_.clear();
    for (const auto& item : pasted) ecaSelection_.push_back(item.receipt.rootEcaId);
    {
        TriggerActions::UndoBatch batch(project_.triggerUndoStack_, label);
        for (const auto& item : pasted) {
            batch.Push(new PasteEcaUndoCommand(document, catalog, *item.snapshot, item.receipt.parentTriggerId,
                item.receipt.parentEcaId, item.receipt.row, item.childId, item.receipt.rootEcaId, tr("粘贴节点")));
        }
    }
    RefreshEditor(pasted.empty() ? 0 : pasted.front().receipt.rootEcaId);
    return true;
}

void TriggerModule::DuplicateSelectedEca() {
    auto* document = project_.triggerDocument_.get();
    if (document == nullptr) {
        return;
    }
    const auto selected = SelectedEcaIds();
    if (selected.size() <= 1) {
        CopySelectedEca(false);
        PasteEca();
        return;
    }
    // 复制整个选区（同时进剪贴板，与单条创建副本一致），副本放在每段最后一条之后。
    clipboard_.clear();
    CopySelectedEca(false);
    if (clipboard_.size() != selected.size()) {
        return; // 复制失败（已提示）
    }
    const auto selection = Trigger::NormalizeEcaSelection(*document, displayedTrigger_, selected);
    std::vector<EcaPastePlan> plans;
    std::size_t offset = 0;
    for (const auto& run : selection.runs) {
        EcaPastePlan plan{run.ids.back(), {}};
        for (std::size_t index = 0; index < run.ids.size(); ++index) plan.snapshots.push_back(&clipboard_[offset + index]);
        offset += run.ids.size();
        plans.push_back(std::move(plan));
    }
    (void)PasteEcaPlans(plans, tr("创建 %1 条语句的副本").arg(selected.size()));
}

void TriggerModule::SetSelectedEcaEnabled(bool enabled) {
    auto* document = project_.triggerDocument_.get();
    if (document == nullptr || project_.triggerUndoStack_ == nullptr) {
        return;
    }
    std::vector<StableId> changes;
    for (const StableId id : SelectedEcaRowIds()) {
        const auto* eca = Editor::FindEca(*document, id);
        if (eca != nullptr && (eca->enabled != 0) != enabled) changes.push_back(id);
    }
    if (changes.empty()) {
        return;
    }
    const auto* current = ecaModel_->EcaAt(ecaView_->currentIndex());
    const StableId focus = current != nullptr ? current->id : changes.front();
    const QString label = enabled ? tr("启用语句") : tr("禁用语句");
    {
        TriggerActions::UndoBatch batch(project_.triggerUndoStack_,
            changes.size() == 1 ? label : tr("%1（%2 条）").arg(label).arg(changes.size()));
        for (const StableId id : changes) {
            batch.Push(new SetEcaEnabledUndoCommand(document, id, !enabled, enabled, label));
        }
    }
    RefreshEditor(focus);
}

void TriggerModule::MoveSelectedEcaBlock(int delta) {
    auto* document = project_.triggerDocument_.get();
    if (document == nullptr || project_.uiCatalog_ == nullptr || project_.triggerUndoStack_ == nullptr || delta == 0) {
        return;
    }
    const auto selection = Trigger::NormalizeEcaSelection(*document, displayedTrigger_, SelectedEcaIds());
    if (selection.runs.size() != 1) {
        ShowStatus(tr("只能整体移动同一分支里相邻的语句"), 6000);
        return;
    }
    const auto& run = selection.runs.front();
    const auto siblings = Trigger::EcaSlotSiblings(*document, displayedTrigger_, run.slot);
    const auto first = std::find(siblings.begin(), siblings.end(), run.ids.front());
    const auto last = std::find(siblings.begin(), siblings.end(), run.ids.back());
    if (first == siblings.end() || last == siblings.end()) {
        return;
    }
    // 整段上移 = 把上一条移到这段之后；整段下移 = 把下一条移到这段之前。
    StableId neighbour = 0;
    StableId before = 0;
    if (delta < 0) {
        if (first == siblings.begin()) return; // 已在分支首
        neighbour = *(first - 1);
        before = last + 1 != siblings.end() ? *(last + 1) : 0;
    } else {
        if (last + 1 == siblings.end()) return; // 已在分支尾
        neighbour = *(last + 1);
        before = *first;
    }
    const auto* current = ecaModel_->EcaAt(ecaView_->currentIndex());
    const StableId focus = current != nullptr ? current->id : run.ids.front();
    const StableId moving[] = {neighbour};
    auto moved = Trigger::MoveEcas(*document, displayedTrigger_, moving, run.slot, before);
    if (!moved.Succeeded() || moved.value->empty()) {
        ShowStatus(tr("不能移动到这里：%1").arg(FirstMessage(moved.diagnostics, tr("位置无效"))), 6000);
        return;
    }
    if (!ValidateOrReport()) {
        (void)Editor::UndoMoveEca(*document, moved.value->front());
        ShowStatus(tr("移动后未通过 WTG/WCT 图验证，已回滚。"), 8000);
        RefreshEditor(focus);
        return;
    }
    project_.triggerUndoStack_->push(new MoveEcaUndoCommand(document, std::move(moved.value->front()),
        delta < 0 ? tr("上移 %1 条语句").arg(run.ids.size()) : tr("下移 %1 条语句").arg(run.ids.size())));
    RefreshEditor(focus);
}

void TriggerModule::ApplyEcaMoves(const EcaMoveRequest& request) {
    auto* document = project_.triggerDocument_.get();
    const auto* trigger = CurrentTrigger();
    if (document == nullptr || trigger == nullptr || project_.uiCatalog_ == nullptr || project_.triggerUndoStack_ == nullptr) {
        return;
    }
    const StableId triggerId = trigger->id;
    const auto selection = Trigger::NormalizeEcaSelection(*document, triggerId, request.ecaIds);
    if (selection.ids.empty()) {
        return;
    }
    // 每条语句的类型都要适合目标位置（事件只在事件组，条件进条件位置，动作进动作位置）。
    for (const StableId id : selection.ids) {
        const auto* eca = Editor::FindEca(*document, id);
        if (eca == nullptr || TriggerModelsDetail::TypeToGroup(eca->type) != request.slot.type) {
            ShowStatus(tr("不能把%1移到“%2”").arg(EcaKindName(eca == nullptr ? 2 : eca->type), request.slot.label), 6000);
            return;
        }
    }
    const auto key = SlotKeyOf(request.slot);
    const auto order = Trigger::EcaSlotSiblings(*document, triggerId, key);
    const bool allInSlot = std::all_of(selection.ids.begin(), selection.ids.end(), [&](StableId id) {
        const auto slot = Trigger::EcaSlotOf(*document, triggerId, id);
        return slot && *slot == key;
    });
    auto moved = Trigger::MoveEcas(*document, triggerId, selection.ids, key, request.beforeEcaId);
    if (!moved.Succeeded()) {
        ShowStatus(tr("不能移动到这里：%1").arg(FirstMessage(moved.diagnostics, tr("位置无效"))), 6000);
        return;
    }
    const auto undoAll = [&] {
        for (auto receipt = moved.value->rbegin(); receipt != moved.value->rend(); ++receipt) {
            (void)Editor::UndoMoveEca(*document, *receipt);
        }
    };
    if (allInSlot && Trigger::EcaSlotSiblings(*document, triggerId, key) == order) {
        undoAll(); // 放回原处：不入栈
        return;
    }
    if (!ValidateOrReport()) {
        undoAll();
        ShowStatus(tr("移动后未通过 WTG/WCT 图验证，已回滚。"), 8000);
        RefreshEditor(request.ecaId);
        return;
    }
    ecaSelection_ = selection.ids;
    {
        TriggerActions::UndoBatch batch(project_.triggerUndoStack_, tr("移动 %1 条语句").arg(selection.ids.size()));
        for (auto& receipt : *moved.value) {
            batch.Push(new MoveEcaUndoCommand(document, std::move(receipt), tr("移动语句")));
        }
    }
    RefreshEditor(selection.ids.front());
}

} // namespace War3Map::Modules
