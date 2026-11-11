#include "TriggerModels.h"
#include "TriggerModelsDetail.h"

#include "War3MapCore/Trigger/TriggerSelection.h"

#include <QMimeData>
#include <QStringList>

#include <set>

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
    return EcaSlot{parent.id, group.branch, group.type, BranchLabel(parent.name.text, group.branch, group.type, catalog_)};
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
    // 多选拖动：每条选中的语句一行（分组头、分支头不可拖动）。
    QStringList ids;
    std::set<War3Map::StableId> seen;
    for (const QModelIndex& modelIndex : indexes) {
        if (const auto* eca = EcaAt(modelIndex); eca != nullptr && seen.insert(eca->id).second) {
            ids << QString::number(static_cast<qulonglong>(eca->id));
        }
    }
    if (ids.isEmpty()) {
        return nullptr;
    }
    auto* data = new QMimeData;
    data->setData(QString::fromLatin1(kEcaMime), ids.join(QLatin1Char('\n')).toLatin1());
    return data;
}

std::vector<War3Map::StableId> TriggerEcaModel::DecodeEcaMime(const QMimeData* data) const {
    std::vector<War3Map::StableId> ids;
    if (data == nullptr) {
        return ids;
    }
    const QString text = QString::fromLatin1(data->data(QString::fromLatin1(kEcaMime)));
    for (const QString& line : text.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        bool ok = false;
        const War3Map::StableId id = line.toULongLong(&ok);
        if (!ok) {
            return {};
        }
        ids.push_back(id);
    }
    return ids;
}

Qt::DropActions TriggerEcaModel::supportedDragActions() const {
    return Qt::MoveAction;
}

Qt::DropActions TriggerEcaModel::supportedDropActions() const {
    return Qt::MoveAction;
}

std::optional<TriggerEcaModel::DropTarget> TriggerEcaModel::ResolveDropTarget(
    int row, const QModelIndex& parentIndex) const {
    if (document_ == nullptr || !parentIndex.isValid()) {
        return std::nullopt; // 放在根上
    }
    const QModelIndex target = parentIndex.siblingAtColumn(0);
    std::optional<EcaSlot> slot;
    DropTarget result;
    result.slotParent = War3Map::Trigger::kInvalidNodeIndex;
    if (const auto* eca = EcaAt(target)) {
        if (row >= 0) return std::nullopt; // 块语句的分支头之间
        slot = SlotOf(eca->id);             // 放在语句上 = 插在它之前
        result.before = eca->id;
        result.slotParent = parents_[NodeIndex(target)];
    } else {
        slot = SlotAt(target);
        if (!slot) return std::nullopt;
        if (IsRoleHeaderHandle(target)) {
            result.slotParent = RoleParentOfHandle(target);
            const auto groups = RoleGroupsFor(result.slotParent);
            const auto& children = groups[static_cast<std::size_t>(RoleSlotOfHandle(target))].children;
            if (row >= 0 && row < static_cast<int>(children.size())) result.before = document_->ecaNodes[children[static_cast<std::size_t>(row)]].id;
        } else {
            const auto& bucket = groupBuckets_[static_cast<std::size_t>(GroupOfHandle(target))];
            if (row >= 0 && row < static_cast<int>(bucket.size())) result.before = document_->ecaNodes[bucket[static_cast<std::size_t>(row)]].id;
        }
    }
    if (!slot) {
        return std::nullopt;
    }
    result.slot = *slot;
    return result;
}

bool TriggerEcaModel::CanDropInto(std::size_t source, const DropTarget& target) const {
    // 类型必须匹配：事件只在事件组，条件进条件位置，动作进动作位置。
    if (target.slot.type != TypeToGroup(document_->ecaNodes[source].type)) {
        return false;
    }
    // 不能移入自身子树。
    for (std::uint32_t walk = target.slotParent; walk != War3Map::Trigger::kInvalidNodeIndex && walk < parents_.size(); walk = parents_[walk]) {
        if (walk == source) return false;
    }
    return true;
}

std::optional<EcaMoveRequest> TriggerEcaModel::ResolveDrop(
    War3Map::StableId ecaId, int row, const QModelIndex& parentIndex) const {
    const std::size_t source = NodeIndexOf(ecaId);
    if (source >= parents_.size() || rowsInParent_[source] == War3Map::Trigger::kInvalidNodeIndex || !parentIndex.isValid()) {
        return std::nullopt; // 不在当前触发器，或放在根上
    }
    const auto target = ResolveDropTarget(row, parentIndex);
    if (!target || target->before == ecaId || !CanDropInto(source, *target)) {
        return std::nullopt;
    }
    return EcaMoveRequest{ecaId, target->slot, target->before};
}

std::optional<EcaMoveRequest> TriggerEcaModel::ResolveMultiDrop(
    const std::vector<War3Map::StableId>& ecaIds, int row, const QModelIndex& parentIndex) const {
    if (document_ == nullptr || ecaIds.empty()) {
        return std::nullopt;
    }
    if (ecaIds.size() == 1) {
        return ResolveDrop(ecaIds.front(), row, parentIndex);
    }
    const auto selection = War3Map::Trigger::NormalizeEcaSelection(*document_, triggerId_, ecaIds);
    if (selection.ids.size() <= 1) {
        return selection.ids.empty() ? std::nullopt : ResolveDrop(selection.ids.front(), row, parentIndex);
    }
    const auto target = ResolveDropTarget(row, parentIndex);
    if (!target) {
        return std::nullopt;
    }
    for (const War3Map::StableId id : selection.ids) {
        const std::size_t source = NodeIndexOf(id);
        if (source >= parents_.size() || rowsInParent_[source] == War3Map::Trigger::kInvalidNodeIndex
            || !CanDropInto(source, *target)) {
            return std::nullopt;
        }
    }
    // 落在被拖动的语句上时，移动顺延到它之后第一条不移动的语句。
    EcaMoveRequest request{selection.ids.front(), target->slot, target->before, {}};
    request.ecaIds = selection.ids;
    return request;
}

bool TriggerEcaModel::canDropMimeData(const QMimeData* data, Qt::DropAction action,
    int row, int, const QModelIndex& parentIndex) const {
    if (action != Qt::MoveAction || data == nullptr) {
        return false;
    }
    return ResolveMultiDrop(DecodeEcaMime(data), row, parentIndex).has_value();
}

bool TriggerEcaModel::dropMimeData(const QMimeData* data, Qt::DropAction action,
    int row, int, const QModelIndex& parentIndex) {
    if (action != Qt::MoveAction || data == nullptr || !moveHandler_) {
        return false;
    }
    const auto request = ResolveMultiDrop(DecodeEcaMime(data), row, parentIndex);
    if (!request) {
        return false;
    }
    moveHandler_(*request); // 处理方排队执行，不在视图的拖放事件中重置模型
    return true;
}
