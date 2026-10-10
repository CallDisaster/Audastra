#include "War3MapCore/Trigger/TriggerEditing.h"

#include <algorithm>

// ECA 子树移动：只改挂载关系（触发器根列表 / 父节点 children）与分支号，节点表不变，
// 因此撤销只需按回执把挂载关系改回去。
namespace War3Map::Trigger {
namespace {

constexpr const char* kDomain = "Trigger.Edit";

std::uint32_t NodeIndex(const TriggerDocument& document, StableId id) {
    for (std::size_t index = 0; index < document.ecaNodes.size(); ++index) {
        if (document.ecaNodes[index].id == id) return static_cast<std::uint32_t>(index);
    }
    return kInvalidNodeIndex;
}

// 挂载点对应的列表：触发器根或父节点的 children。
std::vector<std::uint32_t>* ListOf(TriggerDocument& document, StableId triggerId, StableId ecaId) {
    if (triggerId != 0) {
        TriggerRecord* trigger = TriggerDocumentEditor::FindTrigger(document, triggerId);
        return trigger == nullptr ? nullptr : &trigger->ecas;
    }
    EcaNode* parent = TriggerDocumentEditor::FindEca(document, ecaId);
    return parent == nullptr ? nullptr : &parent->children;
}

// node 是否位于 root 的子树中（含 root 本身）；深度受限，防御损坏的图。
bool InSubtree(const TriggerDocument& document, std::uint32_t root, std::uint32_t node, std::size_t depth = 0) {
    if (root == node) return true;
    if (root >= document.ecaNodes.size() || depth > 256) return false;
    for (const std::uint32_t child : document.ecaNodes[root].children) {
        if (InSubtree(document, child, node, depth + 1)) return true;
    }
    return false;
}

// 节点所属触发器：沿父链上溯到触发器根。
StableId OwnerTrigger(const TriggerDocument& document, std::uint32_t node) {
    for (std::size_t guard = 0; guard < 512; ++guard) {
        for (const auto& trigger : document.triggers) {
            if (std::find(trigger.ecas.begin(), trigger.ecas.end(), node) != trigger.ecas.end()) return trigger.id;
        }
        bool found = false;
        for (std::size_t index = 0; index < document.ecaNodes.size() && !found; ++index) {
            const auto& children = document.ecaNodes[index].children;
            if (std::find(children.begin(), children.end(), node) != children.end()) {
                node = static_cast<std::uint32_t>(index);
                found = true;
            }
        }
        if (!found) return 0;
    }
    return 0;
}

Status Relink(TriggerDocument& document, std::uint32_t node,
    StableId fromTrigger, StableId fromEca, StableId toTrigger, StableId toEca,
    std::size_t toRow, std::optional<std::int32_t> childId) {
    auto* from = ListOf(document, fromTrigger, fromEca);
    auto* to = ListOf(document, toTrigger, toEca);
    if (from == nullptr || to == nullptr) {
        return Status::Failure(kDomain, "ECA_MOVE_PARENT_MISSING", "移动的源或目标挂载点已不存在。");
    }
    const auto at = std::find(from->begin(), from->end(), node);
    if (at == from->end()) {
        return Status::Failure(kDomain, "ECA_MOVE_SOURCE_MISMATCH", "节点不在回执记录的挂载点上。");
    }
    from->erase(at);
    to->insert(to->begin() + static_cast<std::ptrdiff_t>(std::min(toRow, to->size())), node);
    document.ecaNodes[node].childId = childId;
    return Status::Ok();
}

} // namespace

Result<TriggerDocumentEditor::EcaMoveReceipt> TriggerDocumentEditor::MoveEca(
    TriggerDocument& document,
    StableId ecaId,
    StableId targetTriggerId,
    StableId targetEcaId,
    std::optional<std::int32_t> childId,
    StableId beforeEcaId) {
    using ResultT = Result<EcaMoveReceipt>;
    if ((targetTriggerId == 0) == (targetEcaId == 0)) {
        return ResultT::Failure(kDomain, "ECA_MOVE_TARGET_INVALID", "移动目标必须是触发器根或一个子块之一。");
    }
    if ((targetTriggerId != 0) == childId.has_value()) {
        return ResultT::Failure(kDomain, "ECA_MOVE_BRANCH_INVALID", "触发器根不带分支号，子块必须指定分支号。");
    }
    const std::uint32_t node = NodeIndex(document, ecaId);
    const auto placement = LocateEcaPlacement(document, ecaId);
    if (node == kInvalidNodeIndex || !placement.found) {
        return ResultT::Failure(kDomain, "ECA_MOVE_MISSING", "找不到要移动的节点。");
    }
    if (targetEcaId != 0) {
        const std::uint32_t parent = NodeIndex(document, targetEcaId);
        if (parent == kInvalidNodeIndex) {
            return ResultT::Failure(kDomain, "ECA_MOVE_PARENT_MISSING", "目标子块不存在。");
        }
        if (InSubtree(document, node, parent)) {
            return ResultT::Failure(kDomain, "ECA_MOVE_INTO_SELF", "不能把节点移入它自己的子块。");
        }
    }
    const StableId owner = OwnerTrigger(document, node);
    const StableId targetOwner = targetTriggerId != 0 ? targetTriggerId : OwnerTrigger(document, NodeIndex(document, targetEcaId));
    if (owner == 0 || owner != targetOwner) {
        return ResultT::Failure(kDomain, "ECA_MOVE_CROSS_TRIGGER", "只能在同一个触发器内移动节点。");
    }

    EcaMoveReceipt receipt;
    receipt.ecaId = ecaId;
    receipt.fromTriggerId = placement.parentTriggerId;
    receipt.fromEcaId = placement.parentEcaId;
    receipt.fromRow = placement.row;
    receipt.fromChildId = document.ecaNodes[node].childId;
    receipt.toTriggerId = targetTriggerId;
    receipt.toEcaId = targetEcaId;
    receipt.toChildId = childId;

    // 目标行：在移除源节点之后的列表里计算。
    auto* from = ListOf(document, placement.parentTriggerId, placement.parentEcaId);
    from->erase(from->begin() + static_cast<std::ptrdiff_t>(placement.row));
    auto* to = ListOf(document, targetTriggerId, targetEcaId);
    std::size_t row = to->size();
    if (beforeEcaId != 0 && beforeEcaId != ecaId) {
        const std::uint32_t before = NodeIndex(document, beforeEcaId);
        if (const auto found = std::find(to->begin(), to->end(), before); found != to->end()) {
            row = static_cast<std::size_t>(found - to->begin());
        }
    } else if (childId.has_value()) {
        // 分支末尾：该分支最后一个节点之后（children 中各分支可能交错）。
        for (std::size_t index = to->size(); index > 0; --index) {
            if (document.ecaNodes[(*to)[index - 1]].childId == childId) {
                row = index;
                break;
            }
        }
    } else {
        // 触发器根：同类型（事件 / 条件 / 动作）的最后一个之后。
        const std::int32_t type = document.ecaNodes[node].type;
        for (std::size_t index = to->size(); index > 0; --index) {
            if (document.ecaNodes[(*to)[index - 1]].type == type) {
                row = index;
                break;
            }
        }
    }
    to->insert(to->begin() + static_cast<std::ptrdiff_t>(row), node);
    document.ecaNodes[node].childId = childId;
    receipt.toRow = row;
    return ResultT::Success(std::move(receipt));
}

Status TriggerDocumentEditor::UndoMoveEca(TriggerDocument& document, const EcaMoveReceipt& receipt) {
    const std::uint32_t node = NodeIndex(document, receipt.ecaId);
    if (node == kInvalidNodeIndex) {
        return Status::Failure(kDomain, "ECA_MOVE_MISSING", "找不到要恢复位置的节点。");
    }
    return Relink(document, node, receipt.toTriggerId, receipt.toEcaId,
        receipt.fromTriggerId, receipt.fromEcaId, receipt.fromRow, receipt.fromChildId);
}

Status TriggerDocumentEditor::RedoMoveEca(TriggerDocument& document, const EcaMoveReceipt& receipt) {
    const std::uint32_t node = NodeIndex(document, receipt.ecaId);
    if (node == kInvalidNodeIndex) {
        return Status::Failure(kDomain, "ECA_MOVE_MISSING", "找不到要移动的节点。");
    }
    return Relink(document, node, receipt.fromTriggerId, receipt.fromEcaId,
        receipt.toTriggerId, receipt.toEcaId, receipt.toRow, receipt.toChildId);
}

} // namespace War3Map::Trigger
