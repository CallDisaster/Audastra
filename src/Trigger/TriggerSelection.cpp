#include "War3MapCore/Trigger/TriggerSelection.h"

#include "War3MapCore/Trigger/TriggerDirectory.h"

#include <algorithm>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <utility>

// 多选整理与批量结构编辑：只组合现有的单项编辑（MoveTrigger / MoveCategory / RemoveEcaSubtree / MoveEca），
// 不另写一套挂载或压实逻辑；失败时用同一批回执或布局快照恢复。
namespace War3Map::Trigger {
namespace {

constexpr const char* kDomain = "Trigger.Selection";

// 顶层语句的显示分组：0 事件 / 1 条件 / 其余归入动作（与传统视图一致）。
std::int32_t GroupOf(std::int32_t type) {
    return type == 0 ? 0 : type == 1 ? 1 : 2;
}

const TriggerRecord* FindTriggerRecord(const TriggerDocument& document, StableId triggerId) {
    for (const auto& trigger : document.triggers) {
        if (trigger.id == triggerId) return &trigger;
    }
    return nullptr;
}

// 目录显示顺序：类别顺序，类别内按文档顺序，无效类别里的触发器排在最后。
std::vector<std::size_t> TriggerDisplayOrder(const TriggerDocument& document) {
    std::unordered_map<std::int32_t, std::size_t> rank;
    for (std::size_t index = 0; index < document.categories.size(); ++index) {
        rank.emplace(document.categories[index].categoryId, index);
    }
    const auto rankOf = [&](std::size_t triggerIndex) {
        const auto found = rank.find(document.triggers[triggerIndex].categoryId);
        return found == rank.end() ? document.categories.size() : found->second;
    };
    std::vector<std::size_t> order(document.triggers.size());
    for (std::size_t index = 0; index < order.size(); ++index) order[index] = index;
    std::stable_sort(order.begin(), order.end(), [&](std::size_t left, std::size_t right) {
        return rankOf(left) < rankOf(right);
    });
    return order;
}

// 触发器根按显示分组排列的顶层语句。
std::vector<std::uint32_t> OrderedRoots(const TriggerDocument& document, const TriggerRecord& trigger) {
    std::vector<std::uint32_t> roots;
    for (std::int32_t group = 0; group < 3; ++group) {
        for (const std::uint32_t root : trigger.ecas) {
            if (root < document.ecaNodes.size() && GroupOf(document.ecaNodes[root].type) == group) roots.push_back(root);
        }
    }
    return roots;
}

// 块语句的子语句按分支号、再按存储顺序。
std::vector<std::uint32_t> OrderedChildren(const TriggerDocument& document, std::uint32_t node) {
    std::vector<std::uint32_t> children;
    for (const std::uint32_t child : document.ecaNodes[node].children) {
        if (child < document.ecaNodes.size()) children.push_back(child);
    }
    std::stable_sort(children.begin(), children.end(), [&](std::uint32_t left, std::uint32_t right) {
        return document.ecaNodes[left].childId.value_or(0) < document.ecaNodes[right].childId.value_or(0);
    });
    return children;
}

// 触发器语句树：每个节点的父节点（kInvalidNodeIndex = 触发器根）与先序显示序号。重复挂载只算第一次。
struct EcaTree {
    std::unordered_map<std::uint32_t, std::uint32_t> parent;
    std::unordered_map<std::uint32_t, std::size_t> rank;
};

EcaTree BuildTree(const TriggerDocument& document, const TriggerRecord& trigger) {
    EcaTree tree;
    std::size_t next = 0;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> pending;
    const auto roots = OrderedRoots(document, trigger);
    for (auto root = roots.rbegin(); root != roots.rend(); ++root) pending.emplace_back(*root, kInvalidNodeIndex);
    while (!pending.empty()) {
        const auto entry = pending.back();
        pending.pop_back();
        if (entry.first >= document.ecaNodes.size() || tree.rank.contains(entry.first)) continue;
        tree.rank.emplace(entry.first, next++);
        tree.parent.emplace(entry.first, entry.second);
        const auto children = OrderedChildren(document, entry.first);
        for (auto child = children.rbegin(); child != children.rend(); ++child) pending.emplace_back(*child, entry.first);
    }
    return tree;
}

EcaSlotKey SlotKeyOf(const TriggerDocument& document, const EcaTree& tree, std::uint32_t node) {
    const std::uint32_t parent = tree.parent.at(node);
    if (parent == kInvalidNodeIndex) return {0, GroupOf(document.ecaNodes[node].type)};
    return {document.ecaNodes[parent].id, document.ecaNodes[node].childId.value_or(0)};
}

// 位置里的语句（节点下标，显示顺序）。
std::vector<std::uint32_t> SlotNodes(const TriggerDocument& document, const TriggerRecord& trigger,
    const EcaTree& tree, const EcaSlotKey& slot) {
    std::vector<std::uint32_t> nodes;
    if (slot.parentEcaId == 0) {
        for (const std::uint32_t root : trigger.ecas) {
            if (root < document.ecaNodes.size() && GroupOf(document.ecaNodes[root].type) == slot.branch) nodes.push_back(root);
        }
        return nodes;
    }
    for (const auto& entry : tree.rank) {
        const std::uint32_t node = entry.first;
        if (document.ecaNodes[node].id != slot.parentEcaId) continue;
        for (const std::uint32_t child : document.ecaNodes[node].children) {
            if (child < document.ecaNodes.size() && document.ecaNodes[child].childId.value_or(0) == slot.branch) {
                nodes.push_back(child);
            }
        }
        break;
    }
    return nodes;
}

std::optional<std::uint32_t> TreeNodeOf(const TriggerDocument& document, const EcaTree& tree, StableId ecaId) {
    for (const auto& entry : tree.rank) {
        if (document.ecaNodes[entry.first].id == ecaId) return entry.first;
    }
    return std::nullopt;
}

} // namespace

DirectorySelection NormalizeDirectorySelection(const TriggerDocument& document,
    std::span<const std::int32_t> categoryIds, std::span<const StableId> triggerIds) {
    const std::unordered_set<std::int32_t> wantedCategories(categoryIds.begin(), categoryIds.end());
    const std::unordered_set<StableId> wantedTriggers(triggerIds.begin(), triggerIds.end());
    DirectorySelection selection;
    std::unordered_set<std::int32_t> chosen;
    for (const auto& category : document.categories) {
        if (wantedCategories.contains(category.categoryId) && chosen.insert(category.categoryId).second) {
            selection.categories.push_back(category.categoryId);
        }
    }
    std::unordered_set<StableId> seen;
    for (const std::size_t index : TriggerDisplayOrder(document)) {
        const auto& trigger = document.triggers[index];
        if (!wantedTriggers.contains(trigger.id) || chosen.contains(trigger.categoryId)) continue;
        if (seen.insert(trigger.id).second) selection.triggers.push_back(trigger.id);
    }
    return selection;
}

std::vector<StableId> DirectorySelectionTriggers(const TriggerDocument& document, const DirectorySelection& selection) {
    const std::unordered_set<std::int32_t> categories(selection.categories.begin(), selection.categories.end());
    const std::unordered_set<StableId> loose(selection.triggers.begin(), selection.triggers.end());
    std::vector<StableId> ids;
    std::unordered_set<StableId> seen;
    for (const std::size_t index : TriggerDisplayOrder(document)) {
        const auto& trigger = document.triggers[index];
        if ((categories.contains(trigger.categoryId) || loose.contains(trigger.id)) && seen.insert(trigger.id).second) {
            ids.push_back(trigger.id);
        }
    }
    return ids;
}

Status MoveTriggers(TriggerDocument& document, std::span<const StableId> triggerIds,
    std::int32_t categoryId, StableId beforeTriggerId) {
    if (triggerIds.empty()) return Status::Ok();
    const std::unordered_set<StableId> moving(triggerIds.begin(), triggerIds.end());
    if (moving.size() != triggerIds.size()) {
        return Status::Failure(kDomain, "TRIGGER_MOVE_DUPLICATE", "要移动的触发器有重复。");
    }
    StableId before = 0;
    if (beforeTriggerId != 0) {
        bool reached = false;
        for (const auto& trigger : document.triggers) {
            if (trigger.categoryId != categoryId) continue;
            if (trigger.id == beforeTriggerId) reached = true;
            if (reached && !moving.contains(trigger.id)) {
                before = trigger.id;
                break;
            }
        }
        if (!reached) {
            return Status::Failure(kDomain, "TRIGGER_MOVE_ANCHOR_MISSING", "放置位置的触发器不在目标类别里。");
        }
    }
    const auto layout = TriggerDirectoryEditor::CaptureLayout(document);
    for (const StableId id : triggerIds) {
        const Status moved = TriggerDirectoryEditor::MoveTrigger(document, id, categoryId, before);
        if (!moved.Succeeded()) {
            TriggerDirectoryEditor::ApplyLayout(document, layout);
            return moved;
        }
    }
    return Status::Ok();
}

Status MoveCategories(TriggerDocument& document, std::span<const std::int32_t> categoryIds,
    std::optional<std::int32_t> beforeCategoryId) {
    if (categoryIds.empty()) return Status::Ok();
    const std::unordered_set<std::int32_t> moving(categoryIds.begin(), categoryIds.end());
    if (moving.size() != categoryIds.size()) {
        return Status::Failure(kDomain, "CATEGORY_MOVE_DUPLICATE", "要移动的类别有重复。");
    }
    std::optional<std::int32_t> before;
    if (beforeCategoryId) {
        bool reached = false;
        for (const auto& category : document.categories) {
            if (category.categoryId == *beforeCategoryId) reached = true;
            if (reached && !moving.contains(category.categoryId)) {
                before = category.categoryId;
                break;
            }
        }
        if (!reached) {
            return Status::Failure(kDomain, "CATEGORY_MOVE_ANCHOR_MISSING", "放置位置的类别已不存在。");
        }
    }
    const auto layout = TriggerDirectoryEditor::CaptureLayout(document);
    for (const std::int32_t id : categoryIds) {
        const Status moved = TriggerDirectoryEditor::MoveCategory(document, id, before);
        if (!moved.Succeeded()) {
            TriggerDirectoryEditor::ApplyLayout(document, layout);
            return moved;
        }
    }
    return Status::Ok();
}

EcaSelection NormalizeEcaSelection(const TriggerDocument& document, StableId triggerId,
    std::span<const StableId> ecaIds) {
    EcaSelection selection;
    const TriggerRecord* trigger = FindTriggerRecord(document, triggerId);
    if (trigger == nullptr || ecaIds.empty()) return selection;
    const EcaTree tree = BuildTree(document, *trigger);
    const std::unordered_set<StableId> wanted(ecaIds.begin(), ecaIds.end());
    std::unordered_set<std::uint32_t> chosen;
    for (const auto& entry : tree.rank) {
        if (wanted.contains(document.ecaNodes[entry.first].id)) chosen.insert(entry.first);
    }
    std::vector<std::uint32_t> kept;
    for (const std::uint32_t node : chosen) {
        bool covered = false;
        for (std::uint32_t parent = tree.parent.at(node); parent != kInvalidNodeIndex; parent = tree.parent.at(parent)) {
            if (chosen.contains(parent)) {
                covered = true;
                break;
            }
        }
        if (!covered) kept.push_back(node);
    }
    std::sort(kept.begin(), kept.end(), [&](std::uint32_t left, std::uint32_t right) {
        return tree.rank.at(left) < tree.rank.at(right);
    });
    std::map<std::pair<StableId, std::int32_t>, std::vector<std::uint32_t>> siblings;
    std::size_t lastPosition = 0;
    for (const std::uint32_t node : kept) {
        selection.ids.push_back(document.ecaNodes[node].id);
        const EcaSlotKey slot = SlotKeyOf(document, tree, node);
        auto& list = siblings[{slot.parentEcaId, slot.branch}];
        if (list.empty()) list = SlotNodes(document, *trigger, tree, slot);
        const auto at = std::find(list.begin(), list.end(), node);
        const std::size_t position = static_cast<std::size_t>(at - list.begin());
        if (!selection.runs.empty() && selection.runs.back().slot == slot && lastPosition + 1 == position) {
            selection.runs.back().ids.push_back(document.ecaNodes[node].id);
        } else {
            selection.runs.push_back({slot, {document.ecaNodes[node].id}});
        }
        lastPosition = position;
    }
    return selection;
}

std::optional<EcaSlotKey> EcaSlotOf(const TriggerDocument& document, StableId triggerId, StableId ecaId) {
    const TriggerRecord* trigger = FindTriggerRecord(document, triggerId);
    if (trigger == nullptr) return std::nullopt;
    const EcaTree tree = BuildTree(document, *trigger);
    const auto node = TreeNodeOf(document, tree, ecaId);
    if (!node) return std::nullopt;
    return SlotKeyOf(document, tree, *node);
}

std::vector<StableId> EcaSlotSiblings(const TriggerDocument& document, StableId triggerId, const EcaSlotKey& slot) {
    std::vector<StableId> ids;
    const TriggerRecord* trigger = FindTriggerRecord(document, triggerId);
    if (trigger == nullptr) return ids;
    const EcaTree tree = BuildTree(document, *trigger);
    for (const std::uint32_t node : SlotNodes(document, *trigger, tree, slot)) ids.push_back(document.ecaNodes[node].id);
    return ids;
}

Result<std::vector<TriggerDocumentEditor::EcaRemoveReceipt>> RemoveEcaSubtrees(
    TriggerDocument& document, std::span<const StableId> ecaIds) {
    using ResultT = Result<std::vector<TriggerDocumentEditor::EcaRemoveReceipt>>;
    std::vector<TriggerDocumentEditor::EcaRemoveReceipt> receipts;
    receipts.reserve(ecaIds.size());
    for (const StableId id : ecaIds) {
        auto removed = TriggerDocumentEditor::RemoveEcaSubtree(document, id);
        if (!removed.Succeeded()) {
            for (auto receipt = receipts.rbegin(); receipt != receipts.rend(); ++receipt) {
                (void)TriggerDocumentEditor::UndoRemoveEcaSubtree(document, *receipt);
            }
            ResultT failure;
            failure.diagnostics = std::move(removed.diagnostics);
            if (failure.diagnostics.empty()) {
                return ResultT::Failure(kDomain, "ECA_REMOVE_FAILED", "删除语句失败，已恢复。");
            }
            return failure;
        }
        receipts.push_back(std::move(*removed.value));
    }
    return ResultT::Success(std::move(receipts));
}

Result<std::vector<TriggerDocumentEditor::EcaMoveReceipt>> MoveEcas(TriggerDocument& document,
    StableId triggerId, std::span<const StableId> ecaIds, const EcaSlotKey& slot, StableId beforeEcaId) {
    using ResultT = Result<std::vector<TriggerDocumentEditor::EcaMoveReceipt>>;
    const std::unordered_set<StableId> moving(ecaIds.begin(), ecaIds.end());
    if (moving.size() != ecaIds.size()) {
        return ResultT::Failure(kDomain, "ECA_MOVE_DUPLICATE", "要移动的语句有重复。");
    }
    StableId before = 0;
    if (beforeEcaId != 0) {
        const auto siblings = EcaSlotSiblings(document, triggerId, slot);
        auto at = std::find(siblings.begin(), siblings.end(), beforeEcaId);
        if (at == siblings.end()) {
            return ResultT::Failure(kDomain, "ECA_MOVE_ANCHOR_MISSING", "放置位置的语句不在目标分支里。");
        }
        for (; at != siblings.end(); ++at) {
            if (!moving.contains(*at)) {
                before = *at;
                break;
            }
        }
    }
    const bool root = slot.parentEcaId == 0;
    std::vector<TriggerDocumentEditor::EcaMoveReceipt> receipts;
    receipts.reserve(ecaIds.size());
    for (const StableId id : ecaIds) {
        auto moved = TriggerDocumentEditor::MoveEca(document, id, root ? triggerId : 0, slot.parentEcaId,
            root ? std::nullopt : std::optional<std::int32_t>(slot.branch), before);
        if (!moved.Succeeded()) {
            for (auto receipt = receipts.rbegin(); receipt != receipts.rend(); ++receipt) {
                (void)TriggerDocumentEditor::UndoMoveEca(document, *receipt);
            }
            ResultT failure;
            failure.diagnostics = std::move(moved.diagnostics);
            if (failure.diagnostics.empty()) {
                return ResultT::Failure(kDomain, "ECA_MOVE_FAILED", "移动语句失败，已恢复。");
            }
            return failure;
        }
        receipts.push_back(std::move(*moved.value));
    }
    return ResultT::Success(std::move(receipts));
}

} // namespace War3Map::Trigger
