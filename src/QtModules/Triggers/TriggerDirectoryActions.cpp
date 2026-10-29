#include "TriggerModule.h"

#include "EditorProject.h"
#include "TriggerCommands.h"
#include "TriggerDirectoryCommands.h"
#include "TriggerModels.h"
#include "ProjectTriggerCodeIdentity.h"

#include "War3MapCore/Hash.h"

#include "War3MapCore/Services/ProjectReferenceIndex.h"
#include "War3MapCore/Trigger/LegacyTriggerReferences.h"
#include "War3MapCore/Trigger/TriggerPresentation.h"
#include "War3MapCore/Trigger/TriggerGraphEditing.h"
#include "War3MapCore/Trigger/TriggerVariableReference.h"

#include <QMessageBox>
#include <QPushButton>
#include <QSortFilterProxyModel>
#include <QTimer>
#include <QTreeView>
#include <QUndoStack>

#include <algorithm>

// 触发器目录操作（对应原型 TriggerTree / triggerFolders）：新建、重命名、删除、复制粘贴、拖放移动、允许 / 初始打开。
// 目录布局的修改（类别、顺序、归属）用布局快照撤销；增删触发器沿用触发器命令。
namespace War3Map::Modules {
namespace {

using Directory = Trigger::TriggerDirectoryEditor;
using Editor = Trigger::TriggerDocumentEditor;

QString U8(const std::string& text) {
    return QString::fromUtf8(text);
}

QString FirstMessage(const std::vector<Diagnostic>& diagnostics, const QString& fallback) {
    return diagnostics.empty() ? fallback : QString::fromUtf8(diagnostics.front().message);
}

// 与保存链、名称解析和物体管理器使用同一套中文/标点字节转换规则。
std::string TriggerToken(const std::string& name) {
    return "gg_trg_" + Directory::ScriptName(name);
}

const Trigger::TriggerRecord* FindTrigger(const Trigger::TriggerDocument& document, StableId id) {
    for (const auto& trigger : document.triggers) {
        if (trigger.id == id) return &trigger;
    }
    return nullptr;
}

// 同类别中紧随其后的触发器；没有时返回 0（表示放到类别末尾）。
StableId NextInCategory(const Trigger::TriggerDocument& document, StableId triggerId) {
    bool seen = false;
    std::int32_t categoryId = 0;
    for (const auto& trigger : document.triggers) {
        if (seen && trigger.categoryId == categoryId) return trigger.id;
        if (trigger.id == triggerId) {
            seen = true;
            categoryId = trigger.categoryId;
        }
    }
    return 0;
}

// 删除后的选中项：同类别的下一个，其次上一个，都没有时停在类别上。
TriggerDirectoryItem NeighbourAfterRemoval(const Trigger::TriggerDocument& document, const Trigger::TriggerRecord& removed) {
    if (const StableId next = NextInCategory(document, removed.id)) {
        return {false, next, removed.categoryId};
    }
    StableId previous = 0;
    for (const auto& trigger : document.triggers) {
        if (trigger.id == removed.id) break;
        if (trigger.categoryId == removed.categoryId) previous = trigger.id;
    }
    return previous != 0 ? TriggerDirectoryItem{false, previous, removed.categoryId}
                         : TriggerDirectoryItem{true, 0, removed.categoryId};
}

} // namespace

std::optional<TriggerDirectoryItem> TriggerModule::CurrentDirectoryItem() const {
    const auto* document = project_.triggerDocument_.get();
    if (document == nullptr) {
        return std::nullopt;
    }
    if (selectedTrigger_ != 0) {
        if (const auto* trigger = FindTrigger(*document, selectedTrigger_)) {
            return TriggerDirectoryItem{false, trigger->id, trigger->categoryId};
        }
    }
    if (selectedCategory_) {
        for (const auto& category : document->categories) {
            if (category.categoryId == *selectedCategory_) return TriggerDirectoryItem{true, 0, category.categoryId};
        }
    }
    return std::nullopt;
}

std::optional<std::int32_t> TriggerModule::CurrentCategoryId() const {
    if (const auto item = CurrentDirectoryItem()) {
        return item->categoryId;
    }
    const auto* document = project_.triggerDocument_.get();
    if (document != nullptr && !document->categories.empty()) {
        return document->categories.front().categoryId;
    }
    return std::nullopt;
}

void TriggerModule::SelectDirectoryItem(const TriggerDirectoryItem& item, bool edit) {
    if (!page_) {
        return;
    }
    if (!CommitPendingEdits()) return;
    selectionCategory_ = item.categoryId;
    if (item.category) {
        selectedTrigger_ = 0;
        selectedCategory_ = item.categoryId;
    } else {
        selectedTrigger_ = item.triggerId;
        selectedCategory_.reset();
        collapsedCategories_.erase(item.categoryId);
    }
    RefreshExplorer(item.category ? 0 : item.triggerId);
    if (edit) {
        const QModelIndex source = item.category ? explorerModel_->IndexForCategoryId(item.categoryId)
                                                 : explorerModel_->IndexForTriggerId(item.triggerId);
        const QModelIndex proxy = filter_->mapFromSource(source);
        if (proxy.isValid()) {
            explorerView_->setFocus();
            explorerView_->edit(proxy);
        }
    }
}

void TriggerModule::PushDirectoryChange(Trigger::TriggerDirectoryLayout before, const QString& label) {
    auto* document = project_.triggerDocument_.get();
    project_.triggerUndoStack_->push(new TriggerDirectoryUndoCommand(
        document, std::move(before), Directory::CaptureLayout(*document), label));
}

void TriggerModule::NewTrigger() {
    AddDirectoryTrigger(false);
}

void TriggerModule::NewComment() {
    AddDirectoryTrigger(true);
}

void TriggerModule::AddDirectoryTrigger(bool comment) {
    if (!CommitPendingEdits()) return;
    auto* document = project_.triggerDocument_.get();
    if (document == nullptr || project_.triggerUndoStack_ == nullptr) {
        return;
    }
    const auto current = CurrentDirectoryItem();
    auto categoryId = CurrentCategoryId();
    const auto before = Directory::CaptureLayout(*document);
    const bool needsCategory = !categoryId.has_value();
    if (needsCategory) {
        // 文档还没有类别：先建一个，与新触发器作为同一次修改。
        auto category = Directory::AddCategory(*document, Directory::UniqueCategoryName(*document, tr("未命名类别").toStdString()));
        if (!category.Succeeded()) {
            project_.AddDiagnostics(category.diagnostics);
            return;
        }
        categoryId = *category.value;
    }
    const auto withCategory = Directory::CaptureLayout(*document);
    auto added = Editor::AddTrigger(*document, *categoryId,
        Directory::UniqueTriggerName(*document, (comment ? tr("未命名注释") : tr("未命名触发器")).toStdString()));
    if (!added.Succeeded()) {
        Directory::ApplyLayout(*document, before);
        project_.AddDiagnostics(added.diagnostics);
        return;
    }
    if (comment) {
        Editor::FindTrigger(*document, *added.value)->type = 1;
    }
    // 插在当前触发器之后（原型行为）；否则放在类别末尾。
    if (current && !current->category && current->categoryId == *categoryId) {
        (void)Directory::MoveTrigger(*document, *added.value, *categoryId, NextInCategory(*document, current->triggerId));
    }
    auto* stack = project_.triggerUndoStack_;
    stack->beginMacro(comment ? tr("新建注释") : tr("新建触发器"));
    if (needsCategory) {
        stack->push(new TriggerDirectoryUndoCommand(document, before, withCategory, tr("新建类别")));
    }
    stack->push(new AddTriggerUndoCommand(document, *added.value, comment ? tr("新建注释") : tr("新建触发器"),
        nullptr, &project_.guiConversionSnapshots_));
    stack->endMacro();
    SelectDirectoryItem({false, *added.value, *categoryId}, true);
}

void TriggerModule::NewCategory() {
    if (!CommitPendingEdits()) return;
    auto* document = project_.triggerDocument_.get();
    if (document == nullptr || project_.triggerUndoStack_ == nullptr) {
        return;
    }
    const auto current = CurrentDirectoryItem();
    auto before = Directory::CaptureLayout(*document);
    auto added = Directory::AddCategory(*document, Directory::UniqueCategoryName(*document, tr("未命名类别").toStdString()),
        current ? std::optional<std::int32_t>(current->categoryId) : std::nullopt);
    if (!added.Succeeded()) {
        project_.AddDiagnostics(added.diagnostics);
        return;
    }
    PushDirectoryChange(std::move(before), tr("新建类别"));
    SelectDirectoryItem({true, 0, *added.value}, true);
}

bool TriggerModule::RenameDirectoryItem(const TriggerDirectoryItem& item, const QString& name) {
    const auto* document = project_.triggerDocument_.get();
    if (document == nullptr || project_.triggerUndoStack_ == nullptr) {
        return false;
    }
    if (!CommitPendingEdits()) return false;
    const std::string text = name.trimmed().toStdString();
    const Status valid = item.category ? Directory::ValidateCategoryName(*document, text, item.categoryId)
                                       : Directory::ValidateTriggerName(*document, text, item.triggerId);
    if (!valid.Succeeded()) {
        ShowStatus(tr("未重命名：%1").arg(FirstMessage(valid.diagnostics, tr("名称无效"))), 6000);
        return false;
    }
    // 捕获排队前的会话与真实 codec 绑定，不能在回调时借用另一张图的相同 ID。
    const auto generation = project_.editRevision_.Generation();
    const auto* identity = project_.triggerDocument_.get();
    const auto* catalog = project_.uiCatalog_.get();
    std::optional<Trigger::TriggerGraphReplayIdentity> binding;
    if (project_.sourceChangeSuspected_ || project_.sourceChangeConfirmed_) {
        ShowStatus(tr("未重命名：地图来源已发生变化，请先核对来源。"), 10000);
        return false;
    }
    if (!item.category) {
        if (!catalog) return false;
        const auto captured = Trigger::CaptureTriggerGraphReplayIdentity(*document, *catalog);
        if (!captured.Succeeded()) {
            project_.AddDiagnostics(captured.diagnostics);
            ShowStatus(tr("未重命名：%1").arg(FirstMessage(captured.diagnostics, tr("无法确认触发器来源"))), 10000);
            return false;
        }
        binding = *captured.value;
    }
    // 编辑器仍在提交数据：排队执行，避免在模型回调中重置模型。
    QTimer::singleShot(0, this, [this, item, text, generation, identity, catalog, binding]() {
        if (project_.triggerDocument_.get() != identity || project_.uiCatalog_.get() != catalog
            || project_.editRevision_.Generation() != generation
            || project_.sourceChangeSuspected_ || project_.sourceChangeConfirmed_) {
            ShowStatus(tr("未重命名：当前地图会话或函数目录已变化。"), 10000);
            return;
        }
        if (binding) {
            const auto current = Trigger::VerifyTriggerGraphReplayIdentity(*identity, *catalog, *binding);
            if (!current.Succeeded()) {
                ShowStatus(tr("未重命名：排队期间触发器图或来源已变化。"), 10000);
                return;
            }
        }
        ApplyRename(item, text);
    });
    return true;
}

void TriggerModule::ApplyRename(const TriggerDirectoryItem& item, const std::string& name) {
    auto* document = project_.triggerDocument_.get();
    if (document == nullptr || project_.triggerUndoStack_ == nullptr) {
        return;
    }
    if (!CommitPendingEdits()) return;
    if (!item.category) {
        const auto* trigger = FindTrigger(*document, item.triggerId);
        if (trigger && trigger->customTextFlag != 0) {
            if (trigger->name.text != name) (void)ApplyCustomCodeIdentity(false, item.triggerId, name, trigger->categoryId, 0);
            return;
        }
    }
    if (item.category) {
        const auto found = std::find_if(document->categories.begin(), document->categories.end(),
            [&item](const Trigger::CategoryRecord& category) { return category.categoryId == item.categoryId; });
        if (found == document->categories.end() || found->name.text == name) {
            return;
        }
        auto before = Directory::CaptureLayout(*document);
        if (const Status renamed = Directory::RenameCategory(*document, item.categoryId, name); !renamed.Succeeded()) {
            project_.AddDiagnostics(renamed.diagnostics);
            return;
        }
        PushDirectoryChange(std::move(before), tr("重命名类别"));
        RefreshExplorer();
        return;
    }
    const auto* trigger = FindTrigger(*document, item.triggerId);
    if (trigger == nullptr || trigger->name.text == name) {
        return;
    }
    // 不替作者签发代码审计：自定义正文、非空 WCT 与不透明脚本仍由 Core 明确拒绝。
    std::vector<Trigger::GraphEditOperation> operations;
    operations.emplace_back(Trigger::GraphRename{{Trigger::GraphRecordKind::Trigger, item.triggerId, {}}, trigger->name, name, {}});
    const auto renamed = QtProject::PublishTriggerGraphOperations(project_, std::move(operations), tr("重命名触发器"));
    if (!renamed.Succeeded()) {
        project_.AddDiagnostics(renamed.diagnostics);
        ShowStatus(tr("未重命名：%1").arg(FirstMessage(renamed.diagnostics, tr("引用同步校验失败"))), 10000);
        return;
    }
    RefreshExplorer();
}

bool TriggerModule::ApplyCustomCodeIdentity(bool copy, StableId triggerId, const std::string& name, std::int32_t categoryId,
    StableId afterTriggerId) {
    const QString verb = copy ? tr("复制") : tr("重命名");
    const auto fail = [&](const std::vector<Diagnostic>& diagnostics, const QString& fallback) {
        project_.AddDiagnostics(diagnostics);
        ShowStatus(tr("未%1：%2").arg(verb, FirstMessage(diagnostics, fallback)), 10000);
        return false;
    };
    auto* document = project_.triggerDocument_.get();
    const auto* trigger = document ? FindTrigger(*document, triggerId) : nullptr;
    if (trigger == nullptr || project_.uiCatalog_ == nullptr) return false;
    const auto context = QtProject::CaptureProjectTriggerCodeIdentityContext(project_);
    if (!context.Succeeded()) return fail(context.diagnostics, tr("无法确认触发器来源"));
    const auto binding = Trigger::CaptureTriggerGraphBinding(*document, *project_.uiCatalog_, context.value->mapObjects);
    if (!binding.Succeeded()) return fail(binding.diagnostics, tr("无法确认触发器来源"));
    const auto bytes = trigger->customText.ToBytes();
    if (!bytes.Succeeded()) return fail(bytes.diagnostics, tr("代码正文不能按原编码写出"));
    const auto sha = Sha256(*bytes.value);
    if (!sha.Succeeded()) return fail(sha.diagnostics, tr("无法计算代码指纹"));
    Services::TriggerCodeIdentityRequest request;
    request.action = copy ? Services::TriggerCodeIdentityAction::Copy : Services::TriggerCodeIdentityAction::Rename;
    request.expected = *binding.value;
    request.triggerId = triggerId;
    request.expectedName = trigger->name;
    request.expectedCodeSha256 = *sha.value;
    request.name = name;
    if (copy) {
        request.categoryId = categoryId;
        request.afterTriggerId = afterTriggerId;
        for (const auto& row : document->triggers)
            if (row.categoryId == categoryId) request.expectedCategoryOrder.push_back(row.id);
    }
    const auto prepared = Services::TriggerCodeIdentityEditing::Prepare(*context.value, request);
    if (!prepared.Succeeded()) return fail(prepared.diagnostics, tr("代码里的生成符号无法完整同步"));
    const auto applied = QtProject::ApplyProjectTriggerCodeIdentity(project_, *prepared.value,
        copy ? tr("复制自定义代码触发器") : tr("重命名自定义代码触发器"));
    if (!applied.Succeeded()) return fail(applied.diagnostics, tr("发布失败"));
    const auto& summary = prepared.value->Summary();
    RefreshExplorer(copy ? summary.resultId : triggerId);
    ShowStatus(tr("已%1为 %2，同步了 %3 处生成符号 · Ctrl+Z 撤销").arg(verb, U8(name)).arg(summary.changedReferences), 6000);
    return true;
}

QStringList TriggerModule::TriggerReferenceLines(const std::vector<StableId>& triggerIds, std::size_t& total) const {
    total = 0;
    const auto* document = project_.triggerDocument_.get();
    if (document == nullptr) {
        return {};
    }
    std::vector<std::string> tokens;
    std::vector<std::string> owners;
    for (const StableId id : triggerIds) {
        if (const auto* trigger = FindTrigger(*document, id)) {
            tokens.push_back(TriggerToken(trigger->name.text));
            owners.push_back(Services::TriggerOwnerId(id));
        }
    }
    QStringList lines;
    for (const auto* usage : project_.References().UsagesOf(tokens)) {
        if (std::find(owners.begin(), owners.end(), usage->ownerId) != owners.end()) {
            continue; // 被删除的触发器之间的互相引用不算
        }
        if (++total > 8) {
            continue;
        }
        QString text = U8(usage->detail);
        if (usage->ecaIndex && project_.uiCatalog_ != nullptr) {
            text = U8(Trigger::PresentEca(*document, *usage->ecaIndex, *project_.uiCatalog_).text);
        }
        lines << QStringLiteral("%1 · %2").arg(U8(usage->owner), text);
    }
    if (total > 8) {
        lines << tr("还有 %1 处").arg(total - 8);
    }
    return lines;
}

bool TriggerModule::Confirm(const QString& title, const QString& body, const QStringList& references) {
    if (confirm_) {
        return confirm_(title, body, references);
    }
    QMessageBox box(QMessageBox::Warning, title, body, QMessageBox::NoButton, page_);
    if (!references.isEmpty()) {
        box.setInformativeText(references.join('\n'));
    }
    QPushButton* remove = box.addButton(tr("删除"), QMessageBox::DestructiveRole);
    box.addButton(tr("取消"), QMessageBox::RejectRole);
    box.setDefaultButton(qobject_cast<QPushButton*>(box.buttons().last()));
    box.exec();
    return box.clickedButton() == remove;
}

void TriggerModule::DeleteCurrentItem() {
    if (const auto item = CurrentDirectoryItem()) {
        DeleteDirectoryItem(*item);
    }
}

void TriggerModule::DeleteDirectoryItem(const TriggerDirectoryItem& item) {
    if (!CommitPendingEdits()) return;
    auto* document = project_.triggerDocument_.get();
    if (document == nullptr || project_.uiCatalog_ == nullptr || project_.triggerUndoStack_ == nullptr) {
        return;
    }
    if (item.category) {
        DeleteCategory(item.categoryId);
        return;
    }
    const auto* trigger = FindTrigger(*document, item.triggerId);
    if (trigger == nullptr) {
        return;
    }
    const QString name = U8(trigger->name.text);
    std::size_t total = 0;
    const QStringList references = TriggerReferenceLines({item.triggerId}, total);
    // 被其他触发器引用（运行触发器、开关触发器等）时先确认；否则直接删除，可撤销，不打断连续整理。
    if (total != 0 && !Confirm(tr("删除触发器"),
            tr("“%1”被 %2 处引用，删除后这些位置会变成无效引用。").arg(name).arg(total), references)) {
        return;
    }
    const TriggerDirectoryItem neighbour = NeighbourAfterRemoval(*document, *trigger);
    auto removed = Editor::RemoveTrigger(*document, item.triggerId);
    if (!removed.Succeeded()) {
        project_.AddDiagnostics(removed.diagnostics);
        return;
    }
    if (!ValidateOrReport()) {
        (void)Editor::UndoRemoveTrigger(*document, *removed.value);
        ShowStatus(tr("删除后未通过 WTG/WCT 图验证，已回滚。"), 8000);
        return;
    }
    project_.triggerUndoStack_->push(new RemoveTriggerUndoCommand(document, std::move(*removed.value), tr("删除触发器 %1").arg(name),
        nullptr, &project_.guiConversionSnapshots_));
    SelectDirectoryItem(neighbour);
    ShowSelectedTrigger();
    ShowStatus(tr("已删除 %1 · Ctrl+Z 撤销").arg(name), 5000);
}

void TriggerModule::DeleteCategory(std::int32_t categoryId) {
    if (!CommitPendingEdits()) return;
    auto* document = project_.triggerDocument_.get();
    const auto category = std::find_if(document->categories.begin(), document->categories.end(),
        [categoryId](const Trigger::CategoryRecord& record) { return record.categoryId == categoryId; });
    if (category == document->categories.end()) {
        return;
    }
    const QString name = U8(category->name.text);
    std::vector<StableId> inside;
    for (const auto& trigger : document->triggers) {
        if (trigger.categoryId == categoryId) inside.push_back(trigger.id);
    }
    // 删除后停在相邻类别上。
    std::optional<std::int32_t> neighbour;
    const auto position = static_cast<std::size_t>(category - document->categories.begin());
    if (position + 1 < document->categories.size()) neighbour = document->categories[position + 1].categoryId;
    else if (position > 0) neighbour = document->categories[position - 1].categoryId;

    auto before = Directory::CaptureLayout(*document);
    if (inside.empty()) {
        if (const Status removed = Directory::RemoveEmptyCategory(*document, categoryId); !removed.Succeeded()) {
            project_.AddDiagnostics(removed.diagnostics);
            return;
        }
        PushDirectoryChange(std::move(before), tr("删除类别 %1").arg(name));
    } else {
        std::size_t total = 0;
        QStringList references = TriggerReferenceLines(inside, total);
        if (!references.isEmpty()) references.prepend(tr("类别外的引用："));
        if (!Confirm(tr("删除类别"),
                tr("删除类别“%1”及其中 %2 个触发器？可以用 Ctrl+Z 撤销。").arg(name).arg(inside.size()), references)) {
            return;
        }
        std::vector<Editor::TriggerRemoveReceipt> receipts;
        bool failed = false;
        for (const StableId id : inside) {
            auto removed = Editor::RemoveTrigger(*document, id);
            if (!removed.Succeeded()) {
                project_.AddDiagnostics(removed.diagnostics);
                failed = true;
                break;
            }
            receipts.push_back(std::move(*removed.value));
        }
        failed = failed || !Directory::RemoveEmptyCategory(*document, categoryId).Succeeded() || !ValidateOrReport();
        if (failed) {
            for (auto receipt = receipts.rbegin(); receipt != receipts.rend(); ++receipt) {
                (void)Editor::UndoRemoveTrigger(*document, *receipt);
            }
            Directory::ApplyLayout(*document, before);
            ShowStatus(tr("删除类别失败，已回滚；详见诊断。"), 8000);
            RefreshExplorer();
            return;
        }
        auto* stack = project_.triggerUndoStack_;
        stack->beginMacro(tr("删除类别 %1").arg(name));
        for (auto& receipt : receipts) {
            stack->push(new RemoveTriggerUndoCommand(document, std::move(receipt), tr("删除触发器"), nullptr, &project_.guiConversionSnapshots_));
        }
        stack->push(new TriggerDirectoryUndoCommand(document, std::move(before), Directory::CaptureLayout(*document), tr("删除类别")));
        stack->endMacro();
    }
    if (neighbour) {
        SelectDirectoryItem({true, 0, *neighbour});
    } else {
        selectedTrigger_ = 0;
        selectedCategory_.reset();
        RefreshExplorer();
    }
    ShowSelectedTrigger();
    ShowStatus(tr("已删除类别 %1 · Ctrl+Z 撤销").arg(name), 5000);
}

void TriggerModule::CopyCurrentTrigger() {
    if (!CommitPendingEdits()) return;
    const auto item = CurrentDirectoryItem();
    const auto* document = project_.triggerDocument_.get();
    if (!item || item->category || document == nullptr) {
        return;
    }
    auto copied = Directory::CopyTrigger(*document, item->triggerId);
    if (!copied.Succeeded()) {
        project_.AddDiagnostics(copied.diagnostics);
        return;
    }
    ShowStatus(tr("已复制触发器 %1").arg(U8(copied.value->trigger.name.text)), 4000);
    triggerClipboard_ = std::move(*copied.value);
}

void TriggerModule::PasteTrigger() {
    if (!CommitPendingEdits()) return;
    if (!triggerClipboard_) {
        ShowStatus(tr("剪贴板里没有触发器；先在目录中按 Ctrl+C 复制。"), 4000);
        return;
    }
    const auto categoryId = CurrentCategoryId();
    if (!categoryId) {
        return;
    }
    const auto item = CurrentDirectoryItem();
    PasteSnapshot(*triggerClipboard_, *categoryId, item && !item->category ? item->triggerId : 0);
}

void TriggerModule::DuplicateCurrentTrigger() {
    if (!CommitPendingEdits()) return;
    const auto item = CurrentDirectoryItem();
    const auto* document = project_.triggerDocument_.get();
    if (!item || item->category || document == nullptr) {
        return;
    }
    auto copied = Directory::CopyTrigger(*document, item->triggerId);
    if (!copied.Succeeded()) {
        project_.AddDiagnostics(copied.diagnostics);
        return;
    }
    PasteSnapshot(*copied.value, item->categoryId, item->triggerId);
}

void TriggerModule::PasteSnapshot(const Trigger::TriggerSnapshot& snapshot, std::int32_t categoryId, StableId afterTriggerId) {
    if (!CommitPendingEdits()) return;
    auto* document = project_.triggerDocument_.get();
    if (document == nullptr || project_.uiCatalog_ == nullptr || project_.triggerUndoStack_ == nullptr) {
        return;
    }
    std::string name = snapshot.trigger.name.text + tr(" 副本").toStdString();
    if (!Directory::ValidateTriggerName(*document, name).Succeeded()) {
        name = Directory::UniqueTriggerName(*document, name);
    }
    if (snapshot.trigger.customTextFlag != 0) {
        // 代码身份服务复制的是本图里现有的触发器：剪贴板里的必须就是它、且正文没改过，不能拿别处的正文冒充。
        const auto* source = FindTrigger(*document, snapshot.trigger.id);
        if (source == nullptr || source->customTextFlag == 0 || source->name.text != snapshot.trigger.name.text
            || source->customText.text != snapshot.trigger.customText.text || source->customText.encoding != snapshot.trigger.customText.encoding) {
            ShowStatus(tr("只能复制本地图里现有、复制后没改过的自定义代码触发器；请重新选中它再复制。"), 10000);
            return;
        }
        (void)ApplyCustomCodeIdentity(true, source->id, name, categoryId, afterTriggerId);
        return;
    }
    auto pasted = Directory::PasteTrigger(*document, *project_.uiCatalog_, snapshot, categoryId, name, afterTriggerId);
    if (!pasted.Succeeded()) {
        project_.AddDiagnostics(pasted.diagnostics);
        ShowStatus(tr("无法粘贴触发器：%1").arg(FirstMessage(pasted.diagnostics, tr("未知错误"))), 8000);
        return;
    }
    if (!ValidateOrReport()) {
        (void)Editor::RemoveTrigger(*document, *pasted.value);
        ShowStatus(tr("粘贴后未通过 WTG/WCT 图验证，已回滚。"), 8000);
        return;
    }
    project_.triggerUndoStack_->push(new AddTriggerUndoCommand(document, *pasted.value, tr("粘贴触发器"), nullptr, &project_.guiConversionSnapshots_));
    SelectDirectoryItem({false, *pasted.value, categoryId});
}

void TriggerModule::ApplyDirectoryMove(const TriggerDirectoryMove& move) {
    if (!CommitPendingEdits()) return;
    auto* document = project_.triggerDocument_.get();
    if (document == nullptr || project_.triggerUndoStack_ == nullptr) {
        return;
    }
    auto before = Directory::CaptureLayout(*document);
    const Status moved = move.source.category
        ? Directory::MoveCategory(*document, move.source.categoryId, move.beforeCategoryId)
        : Directory::MoveTrigger(*document, move.source.triggerId, move.targetCategoryId, move.beforeTriggerId);
    if (!moved.Succeeded()) {
        project_.AddDiagnostics(moved.diagnostics);
        return;
    }
    const auto after = Directory::CaptureLayout(*document);
    if (after.triggers == before.triggers &&
        std::equal(after.categories.begin(), after.categories.end(), before.categories.begin(), before.categories.end(),
            [](const auto& left, const auto& right) { return left.categoryId == right.categoryId; })) {
        return; // 放回原处：不入栈
    }
    PushDirectoryChange(std::move(before), move.source.category ? tr("移动类别") : tr("移动触发器"));
    if (move.source.category) {
        SelectDirectoryItem({true, 0, move.source.categoryId});
    } else {
        SelectDirectoryItem({false, move.source.triggerId, move.targetCategoryId});
    }
}

void TriggerModule::SetTriggerFlags(StableId triggerId, bool enabled, bool initiallyOn, const QString& label) {
    if (!CommitPendingEdits()) return;
    auto* document = project_.triggerDocument_.get();
    const auto* trigger = document == nullptr ? nullptr : FindTrigger(*document, triggerId);
    if (trigger == nullptr || project_.triggerUndoStack_ == nullptr) {
        return;
    }
    TriggerPropertyState before{trigger->enabled, trigger->initiallyClosed, trigger->runOnInit, trigger->description.text};
    TriggerPropertyState after = before;
    after.enabled = enabled ? 1 : 0;
    after.initiallyClosed = initiallyOn ? 0 : 1;
    project_.triggerUndoStack_->push(new EditTriggerPropertiesUndoCommand(document, triggerId, before, after, label));
    RefreshExplorer();
    PopulateProperties(CurrentTrigger());
}

void TriggerModule::FixLegacyTriggerReferences(StableId triggerId) {
    if (!CommitPendingEdits()) return;
    const auto* document = project_.triggerDocument_.get();
    if (document == nullptr) return;
    const auto references = Trigger::FindLegacyTriggerReferences(*document, triggerId);
    if (references.empty()) return;
    const auto fixed = QtProject::PublishTriggerGraphOperations(project_, Trigger::LegacyTriggerReferenceFixes(references),
        tr("改正旧版触发器引用"));
    if (!fixed.Succeeded()) {
        project_.AddDiagnostics(fixed.diagnostics);
        ShowStatus(tr("未改正：%1").arg(FirstMessage(fixed.diagnostics, tr("参数校验失败"))), 10000);
        return;
    }
    ShowStatus(tr("已改正 %1 处旧版触发器引用").arg(references.size()), 8000);
    RefreshExplorer();
    PopulateProperties(CurrentTrigger());
}

} // namespace War3Map::Modules
