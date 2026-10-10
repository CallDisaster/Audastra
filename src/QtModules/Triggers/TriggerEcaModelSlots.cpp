#include "TriggerModels.h"
#include "TriggerModelsDetail.h"

#include <QMimeData>

using namespace TriggerModelsDetail;

// ECA 逻辑树的放置位置（触发器根分组 / 块语句分支）、兄弟顺序与拖放移动。
namespace {

constexpr auto kEcaMime = "application/x-war3-trigger-eca";

} // namespace

std::size_t TriggerEcaModel::NodeIndexOf(War3Map::StableId ecaId) const {
    if (document_ == nullptr) {
        return std::numeric_limits<std::size_t>::max();
    }
    for (std::size_t index = 0; index < document_->ecaNodes.size(); ++index) {
        if (document_->ecaNodes[index].id == ecaId) return index;
    }
    return std::numeric_limits<std::size_t>::max();
}

EcaSlot TriggerEcaModel::SlotForGroup(std::uint32_t parentNode, const RoleGroup& group) const {
    const auto& parent = document_->ecaNodes[parentNode];
    return EcaSlot{parent.id, group.branch, group.type, BranchLabel(parent.name.text, group.branch, group.type)};
}

std::optional<EcaSlot> TriggerEcaModel::SlotAt(const QModelIndex& modelIndex) const {
    if (document_ == nullptr || !modelIndex.isValid()) {
        return std::nullopt;
    }
    if (IsTopGroupHandle(modelIndex)) {
        const int group = GroupOfHandle(modelIndex);
        if (group < 0 || group >= 3) return std::nullopt;
        return EcaSlot{0, std::nullopt, group, GroupName(group)};
    }
    if (IsRoleHeaderHandle(modelIndex)) {
        const std::uint32_t parentNode = RoleParentOfHandle(modelIndex);
        const int slot = RoleSlotOfHandle(modelIndex);
        if (parentNode >= document_->ecaNodes.size()) return std::nullopt;
        const auto groups = RoleGroupsFor(parentNode);
        if (slot < 0 || slot >= static_cast<int>(groups.size())) return std::nullopt;
        return SlotForGroup(parentNode, groups[static_cast<std::size_t>(slot)]);
    }
    return std::nullopt;
}

std::optional<EcaSlot> TriggerEcaModel::SlotOf(War3Map::StableId ecaId) const {
    const std::size_t node = NodeIndexOf(ecaId);
    if (node >= parents_.size()) {
        return std::nullopt;
    }
    if (topLevelGroup_[node] != kNoGroup) {
        const int group = topLevelGroup_[node];
        return EcaSlot{0, std::nullopt, group, GroupName(group)};
    }
    const std::uint32_t parentNode = parents_[node];
    if (parentNode >= document_->ecaNodes.size()) {
        return std::nullopt;
    }
    const std::int32_t branch = document_->ecaNodes[node].childId.value_or(0);
    for (const RoleGroup& group : RoleGroupsFor(parentNode)) {
        if (group.branch == branch) return SlotForGroup(parentNode, group);
    }
    return std::nullopt;
}

std::vector<EcaSlot> TriggerEcaModel::BranchesOf(War3Map::StableId ecaId) const {
    std::vector<EcaSlot> branches;
    const std::size_t node = NodeIndexOf(ecaId);
    if (node >= parents_.size()) {
        return branches;
    }
    for (const RoleGroup& group : RoleGroupsFor(static_cast<std::uint32_t>(node))) {
        branches.push_back(SlotForGroup(static_cast<std::uint32_t>(node), group));
    }
    return branches;
}

std::vector<War3Map::StableId> TriggerEcaModel::SiblingsOf(War3Map::StableId ecaId) const {
    std::vector<War3Map::StableId> ids;
    const std::size_t node = NodeIndexOf(ecaId);
    if (node >= parents_.size()) {
        return ids;
    }
    if (topLevelGroup_[node] != kNoGroup) {
        for (const std::uint32_t sibling : groupBuckets_[topLevelGroup_[node]]) ids.push_back(document_->ecaNodes[sibling].id);
        return ids;
    }
    const std::uint32_t parentNode = parents_[node];
    if (parentNode >= document_->ecaNodes.size()) {
        return ids;
    }
    const std::int32_t branch = document_->ecaNodes[node].childId.value_or(0);
    for (const RoleGroup& group : RoleGroupsFor(parentNode)) {
        if (group.branch != branch) continue;
        for (const std::uint32_t sibling : group.children) ids.push_back(document_->ecaNodes[sibling].id);
    }
    return ids;
}

QStringList TriggerEcaModel::mimeTypes() const {
    return {QString::fromLatin1(kEcaMime)};
}

QMimeData* TriggerEcaModel::mimeData(const QModelIndexList& indexes) const {
    for (const QModelIndex& modelIndex : indexes) {
        if (const auto* eca = EcaAt(modelIndex)) {
            auto* data = new QMimeData;
            data->setData(QString::fromLatin1(kEcaMime), QByteArray::number(static_cast<qulonglong>(eca->id)));
            return data;
        }
    }
    return nullptr;
}

Qt::DropActions TriggerEcaModel::supportedDragActions() const {
    return Qt::MoveAction;
}

Qt::DropActions TriggerEcaModel::supportedDropActions() const {
    return Qt::MoveAction;
}

std::optional<EcaMoveRequest> TriggerEcaModel::ResolveDrop(
    War3Map::StableId ecaId, int row, const QModelIndex& parentIndex) const {
    const std::size_t source = NodeIndexOf(ecaId);
    if (source >= parents_.size() || rowsInParent_[source] == War3Map::Trigger::kInvalidNodeIndex || !parentIndex.isValid()) {
        return std::nullopt; // 不在当前触发器，或放在根上
    }
    const QModelIndex target = parentIndex.siblingAtColumn(0);
    std::optional<EcaSlot> slot;
    War3Map::StableId before = 0;
    std::uint32_t slotParent = War3Map::Trigger::kInvalidNodeIndex;
    if (const auto* eca = EcaAt(target)) {
        if (row >= 0) return std::nullopt; // 块语句的分支头之间
        slot = SlotOf(eca->id);             // 放在语句上 = 插在它之前
        before = eca->id;
        slotParent = parents_[NodeIndex(target)];
    } else {
        slot = SlotAt(target);
        if (!slot) return std::nullopt;
        if (IsRoleHeaderHandle(target)) {
            slotParent = RoleParentOfHandle(target);
            const auto groups = RoleGroupsFor(slotParent);
            const auto& children = groups[static_cast<std::size_t>(RoleSlotOfHandle(target))].children;
            if (row >= 0 && row < static_cast<int>(children.size())) before = document_->ecaNodes[children[static_cast<std::size_t>(row)]].id;
        } else {
            const auto& bucket = groupBuckets_[static_cast<std::size_t>(GroupOfHandle(target))];
            if (row >= 0 && row < static_cast<int>(bucket.size())) before = document_->ecaNodes[bucket[static_cast<std::size_t>(row)]].id;
        }
    }
    if (!slot || before == ecaId) {
        return std::nullopt;
    }
    // 类型必须匹配：事件只在事件组，条件进条件位置，动作进动作位置。
    if (slot->type != TypeToGroup(document_->ecaNodes[source].type)) {
        return std::nullopt;
    }
    // 不能移入自身子树。
    for (std::uint32_t walk = slotParent; walk != War3Map::Trigger::kInvalidNodeIndex && walk < parents_.size(); walk = parents_[walk]) {
        if (walk == source) return std::nullopt;
    }
    return EcaMoveRequest{ecaId, *slot, before};
}

bool TriggerEcaModel::canDropMimeData(const QMimeData* data, Qt::DropAction action,
    int row, int, const QModelIndex& parentIndex) const {
    if (action != Qt::MoveAction || data == nullptr) {
        return false;
    }
    bool ok = false;
    const War3Map::StableId id = data->data(QString::fromLatin1(kEcaMime)).toULongLong(&ok);
    return ok && ResolveDrop(id, row, parentIndex).has_value();
}

bool TriggerEcaModel::dropMimeData(const QMimeData* data, Qt::DropAction action,
    int row, int, const QModelIndex& parentIndex) {
    if (action != Qt::MoveAction || data == nullptr || !moveHandler_) {
        return false;
    }
    bool ok = false;
    const War3Map::StableId id = data->data(QString::fromLatin1(kEcaMime)).toULongLong(&ok);
    const auto request = ok ? ResolveDrop(id, row, parentIndex) : std::nullopt;
    if (!request) {
        return false;
    }
    moveHandler_(*request); // 处理方排队执行，不在视图的拖放事件中重置模型
    return true;
}
