#include "TriggerModule.h"

#include "EditorProject.h"
#include "Icons/SvgIcons.h"
#include "ProjectRemovalCodeAudit.h"
#include "TriggerActionHelpers.h"
#include "TriggerCommands.h"
#include "TriggerDirectoryCommands.h"
#include "TriggerModels.h"

#include "War3MapCore/Trigger/TriggerSelection.h"

#include <QAction>
#include <QMenu>
#include <QUndoStack>

#include <algorithm>
#include <set>

// 触发器目录的批量操作（多选）：删除、粘贴多个快照、剪切移动、拖放移动多项、允许 / 初始打开。
// 每次操作是一步撤销；选中的类别代表其中全部触发器；做不到的项明确说明原因，不静默丢弃。
namespace War3Map::Modules {
namespace {

using Directory = Trigger::TriggerDirectoryEditor;
using Editor = Trigger::TriggerDocumentEditor;
using TriggerActions::FindTrigger;
using TriggerActions::FirstMessage;
using TriggerActions::NextInCategory;

QIcon Icon(const char* name) {
    return UiKit::SvgIcon(QString::fromLatin1(name));
}

// 删除后的当前项：当前项所在类别中下一个留下的触发器，其次上一个，再其次类别本身；
// 类别也被删除时选相邻的留下的类别。
std::optional<TriggerDirectoryItem> SurvivorNear(const Trigger::TriggerDocument& document,
    const std::optional<TriggerDirectoryItem>& current, const std::set<StableId>& goneTriggers,
    const std::set<std::int32_t>& goneCategories) {
    if (!current) return std::nullopt;
    const std::int32_t categoryId = current->categoryId;
    if (!goneCategories.contains(categoryId)) {
        if (current->category) return current;
        StableId previous = 0;
        bool passed = false;
        for (const auto& trigger : document.triggers) {
            if (trigger.categoryId != categoryId) continue;
            if (trigger.id == current->triggerId && !current->category) {
                passed = true;
                if (!goneTriggers.contains(trigger.id)) return TriggerDirectoryItem{false, trigger.id, categoryId};
                continue;
            }
            if (goneTriggers.contains(trigger.id)) continue;
            if (passed) return TriggerDirectoryItem{false, trigger.id, categoryId};
            previous = trigger.id;
        }
        return previous != 0 ? TriggerDirectoryItem{false, previous, categoryId} : TriggerDirectoryItem{true, 0, categoryId};
    }
    const auto& categories = document.categories;
    const auto at = std::find_if(categories.begin(), categories.end(),
        [categoryId](const Trigger::CategoryRecord& record) { return record.categoryId == categoryId; });
    if (at == categories.end()) return std::nullopt;
    for (auto next = at + 1; next != categories.end(); ++next) {
        if (!goneCategories.contains(next->categoryId)) return TriggerDirectoryItem{true, 0, next->categoryId};
    }
    for (auto previous = at; previous != categories.begin();) {
        --previous;
        if (!goneCategories.contains(previous->categoryId)) return TriggerDirectoryItem{true, 0, previous->categoryId};
    }
    return std::nullopt;
}

} // namespace

void TriggerModule::AddDirectorySelectionActions(QMenu& menu, const std::vector<TriggerDirectoryItem>& items,
    std::vector<std::pair<QAction*, std::function<void()>>>& actions) {
    const auto* document = project_.triggerDocument_.get();
    if (document == nullptr || items.empty()) {
        return;
    }
    const auto bind = [&actions](QAction* action, std::function<void()> run) { actions.emplace_back(action, std::move(run)); };
    const auto add = [&menu](const char* icon, const QString& text, const QKeySequence& key = {}) {
        QAction* action = menu.addAction(Icon(icon), text);
        action->setShortcut(key);
        action->setShortcutVisibleInContextMenu(true);
        return action;
    };
    const auto triggerIds = SelectedTriggerIds();
    const bool hasCategories = std::any_of(items.begin(), items.end(), [](const auto& item) { return item.category; });
    bind(add("lucide/copy", tr("创建副本"), QKeySequence(Qt::CTRL | Qt::Key_D)), [this]() { DuplicateCurrentTrigger(); });
    bind(add("lucide/copy", tr("复制"), QKeySequence::Copy), [this]() { CopyCurrentTrigger(); });
    bind(add("lucide/copy", tr("剪切"), QKeySequence::Cut), [this]() { CutSelectedTriggers(); });
    const std::size_t pending = triggerCut_.empty() ? triggerClipboard_.size() : triggerCut_.size();
    QAction* paste = add("lucide/copy", pending == 0 ? tr("粘贴") : tr("粘贴 %1 个触发器").arg(pending), QKeySequence::Paste);
    paste->setEnabled(pending != 0);
    bind(paste, [this]() { PasteTrigger(); });
    menu.addSeparator();
    // 允许 / 初始打开：全部选中的触发器都是该状态时显示为勾选，点一下统一改为相反状态。
    bool anyFlags = false, allEnabled = true, allOn = true;
    for (const StableId id : triggerIds) {
        const auto* trigger = FindTrigger(*document, id);
        if (trigger == nullptr || trigger->type != 0) continue; // 注释条目没有允许 / 初始打开
        anyFlags = true;
        allEnabled = allEnabled && trigger->enabled != 0;
        allOn = allOn && trigger->initiallyClosed == 0;
    }
    QAction* allow = menu.addAction(tr("允许"));
    allow->setCheckable(true);
    allow->setChecked(anyFlags && allEnabled);
    allow->setEnabled(anyFlags);
    allow->setToolTip(ScriptRebuildNotice());
    bind(allow, [this, allEnabled]() { SetSelectedTriggerFlags(!allEnabled, std::nullopt); });
    QAction* open = menu.addAction(tr("初始打开"));
    open->setCheckable(true);
    open->setChecked(anyFlags && allOn);
    open->setEnabled(anyFlags);
    open->setToolTip(ScriptRebuildNotice());
    bind(open, [this, allOn]() { SetSelectedTriggerFlags(std::nullopt, !allOn); });
    // 移到类别：只用于触发器（类别本身只能在顶层排序）。
    QMenu* moveTo = menu.addMenu(Icon("lucide/folder-open"), tr("移到类别"));
    for (const auto& category : document->categories) {
        const std::int32_t target = category.categoryId;
        bind(moveTo->addAction(QString::fromUtf8(category.name.text)), [this, target, items]() {
            ApplyDirectoryMoves({items.front(), target, 0, std::nullopt, items});
        });
    }
    moveTo->setEnabled(!hasCategories && !moveTo->isEmpty());
    menu.addSeparator();
    bind(add("lucide/trash-2", tr("删除选中的项"), QKeySequence::Delete), [this]() { DeleteCurrentItem(); });
}

void TriggerModule::DeleteDirectoryItems(const std::vector<TriggerDirectoryItem>& items) {
    if (!CommitPendingEdits()) return;
    auto* document = project_.triggerDocument_.get();
    if (document == nullptr || project_.uiCatalog_ == nullptr || project_.triggerUndoStack_ == nullptr) {
        return;
    }
    std::vector<std::int32_t> categoryIds;
    std::vector<StableId> chosenTriggers;
    for (const auto& item : items) {
        if (item.category) categoryIds.push_back(item.categoryId);
        else chosenTriggers.push_back(item.triggerId);
    }
    const auto selection = Trigger::NormalizeDirectorySelection(*document, categoryIds, chosenTriggers);
    const auto triggers = Trigger::DirectorySelectionTriggers(*document, selection);
    if (selection.categories.empty() && triggers.empty()) {
        return;
    }
    // 引用检查一次覆盖全部要删除的触发器；被删除的触发器之间的互相引用不算。
    QtProject::RemovalCodeNotice code;
    if (!triggers.empty()) {
        auto checked = CheckRemovalCode(triggers);
        if (!checked) return;
        code = std::move(*checked);
    }
    std::size_t total = 0;
    QStringList references = TriggerReferenceLines(triggers, total);
    total += code.references;
    references << code.lines;
    if (!references.isEmpty()) references.prepend(tr("选中项之外的引用："));
    if (!code.incomplete.isEmpty()) references << tr("没能全部检查：%1").arg(code.incomplete);
    QString body = selection.categories.empty()
        ? tr("删除选中的 %1 个触发器？可以用 Ctrl+Z 撤销。").arg(triggers.size())
        : tr("删除选中的 %1 个类别，共 %2 个触发器？可以用 Ctrl+Z 撤销。").arg(selection.categories.size()).arg(triggers.size());
    if (total != 0) body += tr("\n有 %1 处引用会变成无效引用。").arg(total);
    if (!Confirm(tr("删除选中的项"), body, references)) {
        return;
    }
    // 确认期间文档可能被改动：要删除的项必须原样还在。
    if (project_.triggerDocument_.get() != document) return;
    const auto recheck = Trigger::NormalizeDirectorySelection(*document, selection.categories, selection.triggers);
    if (recheck.categories != selection.categories || recheck.triggers != selection.triggers
        || Trigger::DirectorySelectionTriggers(*document, recheck) != triggers) {
        ShowStatus(tr("未删除：确认期间目录有变化，请重新删除。"), 8000);
        return;
    }
    const std::set<StableId> goneTriggers(triggers.begin(), triggers.end());
    const std::set<std::int32_t> goneCategories(selection.categories.begin(), selection.categories.end());
    const auto neighbour = SurvivorNear(*document, CurrentDirectoryItem(), goneTriggers, goneCategories);

    auto before = Directory::CaptureLayout(*document);
    std::vector<Editor::TriggerRemoveReceipt> receipts;
    bool failed = false;
    for (const StableId id : triggers) {
        auto removed = Editor::RemoveTrigger(*document, id);
        if (!removed.Succeeded()) {
            project_.AddDiagnostics(removed.diagnostics);
            failed = true;
            break;
        }
        receipts.push_back(std::move(*removed.value));
    }
    for (const std::int32_t categoryId : selection.categories) {
        if (failed) break;
        if (const Status removed = Directory::RemoveEmptyCategory(*document, categoryId); !removed.Succeeded()) {
            project_.AddDiagnostics(removed.diagnostics);
            failed = true;
        }
    }
    failed = failed || !ValidateOrReport();
    if (failed) {
        for (auto receipt = receipts.rbegin(); receipt != receipts.rend(); ++receipt) {
            (void)Editor::UndoRemoveTrigger(*document, *receipt);
        }
        Directory::ApplyLayout(*document, before);
        ShowStatus(tr("删除失败，已回滚；详见诊断。"), 8000);
        RefreshExplorer();
        return;
    }
    const std::size_t count = selection.categories.size() + selection.triggers.size();
    auto* stack = project_.triggerUndoStack_;
    stack->beginMacro(tr("删除 %1 项").arg(count));
    for (auto& receipt : receipts) {
        stack->push(new RemoveTriggerUndoCommand(document, std::move(receipt), tr("删除触发器"), nullptr, &project_.guiConversionSnapshots_));
    }
    if (!selection.categories.empty()) {
        stack->push(new TriggerDirectoryUndoCommand(document, std::move(before), Directory::CaptureLayout(*document), tr("删除类别")));
    }
    stack->endMacro();
    if (neighbour) {
        SelectDirectoryItems({*neighbour});
    } else {
        selectedTrigger_ = 0;
        selectedCategory_.reset();
        explorerSelection_ = {};
        RefreshExplorer();
    }
    ShowSelectedTrigger();
    ShowStatus(tr("已删除 %1 个类别、%2 个触发器 · Ctrl+Z 撤销").arg(selection.categories.size()).arg(triggers.size())
        + (code.incomplete.isEmpty() ? QString() : tr(" · 代码引用没能全部检查：%1").arg(code.incomplete)), 8000);
}

void TriggerModule::PasteTriggerGroups(std::vector<TriggerPasteGroup> groups, const QString& label) {
    if (!CommitPendingEdits()) return;
    auto* document = project_.triggerDocument_.get();
    const auto* catalog = project_.uiCatalog_.get();
    if (document == nullptr || catalog == nullptr || project_.triggerUndoStack_ == nullptr) {
        return;
    }
    // 自定义代码触发器走代码身份服务，只能复制本图里现有、复制后没改过的源：先整体核对，不做一半。
    for (const auto& group : groups) {
        for (const auto& snapshot : group.snapshots) {
            if (snapshot.trigger.customTextFlag != 0 && !TriggerActions::CustomSourceIsCurrent(*document, snapshot)) {
                ShowStatus(tr("只能复制本地图里现有、复制后没改过的自定义代码触发器；请重新选中它再复制。"), 10000);
                return;
            }
        }
    }
    // 代码身份发布会重新绑定文档并清空剪贴板；同一文档里粘贴完成后按原样放回。
    const auto clipboard = triggerClipboard_;
    std::vector<TriggerDirectoryItem> pasted;
    std::vector<StableId> pending; // 已粘贴、尚未验证入栈的普通触发器
    QString failure;
    {
        TriggerActions::UndoBatch batch(project_.triggerUndoStack_, label);
        const auto flush = [&]() {
            if (pending.empty()) return true;
            if (!ValidateOrReport()) {
                for (auto id = pending.rbegin(); id != pending.rend(); ++id) (void)Editor::RemoveTrigger(*document, *id);
                pasted.resize(pasted.size() - pending.size());
                pending.clear();
                failure = tr("粘贴后未通过 WTG/WCT 图验证，已回滚这部分。");
                return false;
            }
            for (const StableId id : pending) {
                batch.Push(new AddTriggerUndoCommand(document, id, tr("粘贴触发器"), nullptr, &project_.guiConversionSnapshots_));
            }
            pending.clear();
            return true;
        };
        for (const auto& group : groups) {
            StableId after = group.afterTriggerId;
            for (const auto& snapshot : group.snapshots) {
                const std::string name = PasteTriggerName(snapshot.trigger.name.text);
                if (snapshot.trigger.customTextFlag != 0) {
                    if (!flush()) break;
                    batch.Open();
                    const auto copied = ApplyCustomCodeIdentity(true, snapshot.trigger.id, name, group.categoryId, after);
                    if (!copied) {
                        failure = tr("自定义代码触发器 %1 没有复制").arg(QString::fromUtf8(snapshot.trigger.name.text));
                        break;
                    }
                    after = *copied;
                    pasted.push_back({false, *copied, group.categoryId});
                    continue;
                }
                auto result = Directory::PasteTrigger(*document, *catalog, snapshot, group.categoryId, name, after);
                if (!result.Succeeded()) {
                    project_.AddDiagnostics(result.diagnostics);
                    failure = tr("无法粘贴触发器：%1").arg(FirstMessage(result.diagnostics, tr("未知错误")));
                    break;
                }
                after = *result.value;
                pending.push_back(after);
                pasted.push_back({false, after, group.categoryId});
            }
            if (!failure.isEmpty()) break;
        }
        if (failure.isEmpty()) {
            (void)flush();
        } else if (!pending.empty()) {
            // 中途失败：未验证的这部分撤回，已入栈的保留并如实说明。
            for (auto id = pending.rbegin(); id != pending.rend(); ++id) (void)Editor::RemoveTrigger(*document, *id);
            pasted.resize(pasted.size() - pending.size());
            pending.clear();
        }
    }
    if (project_.triggerDocument_.get() == document && triggerClipboard_.empty()) {
        triggerClipboard_ = clipboard;
    }
    if (!pasted.empty()) {
        SelectDirectoryItems(pasted);
    } else {
        RefreshExplorer();
    }
    if (failure.isEmpty()) {
        ShowStatus(tr("已粘贴 %1 个触发器 · Ctrl+Z 撤销").arg(pasted.size()), 6000);
    } else if (pasted.empty()) {
        ShowStatus(failure, 10000);
    } else {
        ShowStatus(tr("只粘贴了 %1 个触发器，其余未粘贴：%2 · Ctrl+Z 撤销已粘贴的部分").arg(pasted.size()).arg(failure), 10000);
    }
}

void TriggerModule::CutSelectedTriggers() {
    if (!CommitPendingEdits()) return;
    const auto triggerIds = SelectedTriggerIds();
    if (triggerIds.empty()) {
        return;
    }
    triggerCut_ = triggerIds;
    ShowStatus(tr("已剪切 %1 个触发器，Ctrl+V 粘贴到当前类别").arg(triggerIds.size()), 6000);
}

void TriggerModule::PasteCutTriggers() {
    const auto* document = project_.triggerDocument_.get();
    const auto categoryId = CurrentCategoryId();
    if (document == nullptr || !categoryId) {
        return;
    }
    std::vector<TriggerDirectoryItem> sources;
    for (const StableId id : triggerCut_) {
        if (const auto* trigger = FindTrigger(*document, id)) sources.push_back({false, id, trigger->categoryId});
    }
    const std::size_t missing = triggerCut_.size() - sources.size();
    triggerCut_.clear();
    if (sources.empty()) {
        ShowStatus(tr("剪切的触发器已不存在。"), 6000);
        return;
    }
    // 插在当前触发器之后；当前项是类别时放到类别末尾。
    const auto item = CurrentDirectoryItem();
    const StableId before = item && !item->category ? NextInCategory(*document, item->triggerId) : 0;
    ApplyDirectoryMoves({sources.front(), *categoryId, before, std::nullopt, sources});
    if (missing != 0) {
        ShowStatus(tr("有 %1 个剪切的触发器已不存在，未移动").arg(missing), 8000);
    }
}

void TriggerModule::ApplyDirectoryMoves(const TriggerDirectoryMove& move) {
    if (!CommitPendingEdits()) return;
    auto* document = project_.triggerDocument_.get();
    if (document == nullptr || project_.triggerUndoStack_ == nullptr) {
        return;
    }
    const auto& sources = move.sources.empty() ? std::vector<TriggerDirectoryItem>{move.source} : move.sources;
    std::vector<std::int32_t> categoryIds;
    std::vector<StableId> triggerIds;
    for (const auto& source : sources) {
        if (source.category) categoryIds.push_back(source.categoryId);
        else triggerIds.push_back(source.triggerId);
    }
    const auto selection = Trigger::NormalizeDirectorySelection(*document, categoryIds, triggerIds);
    if (!selection.categories.empty() && !selection.triggers.empty()) {
        ShowStatus(tr("类别和触发器不能一起移动；请分别拖动。"), 6000);
        return;
    }
    if (selection.categories.empty() && selection.triggers.empty()) {
        return;
    }
    auto before = Directory::CaptureLayout(*document);
    const bool categories = !selection.categories.empty();
    const Status moved = categories
        ? Trigger::MoveCategories(*document, selection.categories, move.beforeCategoryId)
        : Trigger::MoveTriggers(*document, selection.triggers, move.targetCategoryId, move.beforeTriggerId);
    if (!moved.Succeeded()) {
        project_.AddDiagnostics(moved.diagnostics);
        ShowStatus(tr("未移动：%1").arg(FirstMessage(moved.diagnostics, tr("目标位置无效"))), 6000);
        return;
    }
    if (TriggerActions::SameDirectoryLayout(before, Directory::CaptureLayout(*document))) {
        return; // 放回原处：不入栈
    }
    const std::size_t count = categories ? selection.categories.size() : selection.triggers.size();
    PushDirectoryChange(std::move(before), categories ? tr("移动 %1 个类别").arg(count) : tr("移动 %1 个触发器").arg(count));
    std::vector<TriggerDirectoryItem> items;
    for (const std::int32_t categoryId : selection.categories) items.push_back({true, 0, categoryId});
    for (const StableId triggerId : selection.triggers) items.push_back({false, triggerId, move.targetCategoryId});
    SelectDirectoryItems(items);
}

void TriggerModule::SetSelectedTriggerFlags(std::optional<bool> enabled, std::optional<bool> initiallyOn) {
    if (!CommitPendingEdits()) return;
    auto* document = project_.triggerDocument_.get();
    if (document == nullptr || project_.triggerUndoStack_ == nullptr || (!enabled && !initiallyOn)) {
        return;
    }
    struct Change {
        StableId id = 0;
        TriggerPropertyState before, after;
    };
    std::vector<Change> changes;
    for (const StableId id : SelectedTriggerIds()) {
        const auto* trigger = FindTrigger(*document, id);
        if (trigger == nullptr || trigger->type != 0) continue; // 注释条目没有允许 / 初始打开
        TriggerPropertyState before{trigger->enabled, trigger->initiallyClosed, trigger->runOnInit, trigger->description.text};
        TriggerPropertyState after = before;
        if (enabled) after.enabled = *enabled ? 1 : 0;
        if (initiallyOn) after.initiallyClosed = *initiallyOn ? 0 : 1;
        if (after.enabled == before.enabled && after.initiallyClosed == before.initiallyClosed) continue;
        changes.push_back({id, std::move(before), std::move(after)});
    }
    if (changes.empty()) {
        return;
    }
    const QString label = enabled ? (*enabled ? tr("允许触发器") : tr("禁用触发器")) : tr("触发器初始状态");
    {
        TriggerActions::UndoBatch batch(project_.triggerUndoStack_,
            changes.size() == 1 ? label : tr("%1（%2 个）").arg(label).arg(changes.size()));
        for (auto& change : changes) {
            batch.Push(new EditTriggerPropertiesUndoCommand(document, change.id, change.before, change.after, label));
        }
    }
    RefreshExplorer();
    PopulateProperties(CurrentTrigger());
}

} // namespace War3Map::Modules
