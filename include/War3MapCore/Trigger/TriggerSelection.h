#pragma once

#include "War3MapCore/Trigger/TriggerEditing.h"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

// 多选（Windows 式 Ctrl / Shift 选取）的整理与批量结构编辑：触发器目录与传统视图共用，
// 命令行与 Agent 的批量操作也可直接复用。整理只读；批量编辑任一步失败时文档恢复原样。
namespace War3Map::Trigger {

// 目录选区，类别与触发器都按目录显示顺序（类别顺序；类别内按文档顺序；无效类别里的触发器排在最后）。
// 选中的类别代表其中全部触发器，类别里同时单独选中的触发器不再单列。已不存在的项丢弃。
struct DirectorySelection {
    std::vector<std::int32_t> categories;
    std::vector<StableId> triggers;
};
[[nodiscard]] DirectorySelection NormalizeDirectorySelection(const TriggerDocument& document,
    std::span<const std::int32_t> categoryIds, std::span<const StableId> triggerIds);
// 选区代表的全部触发器：选中类别里的全部触发器与单独选中的触发器，按目录显示顺序。
[[nodiscard]] std::vector<StableId> DirectorySelectionTriggers(const TriggerDocument& document,
    const DirectorySelection& selection);

// 把多个触发器按给定顺序一起移到 categoryId，插在 beforeTriggerId 之前（0 = 类别末尾）；
// beforeTriggerId 本身也在移动时顺延到它之后第一个不移动的触发器。只改目录布局，失败时布局不变。
[[nodiscard]] Status MoveTriggers(TriggerDocument& document, std::span<const StableId> triggerIds,
    std::int32_t categoryId, StableId beforeTriggerId);
// 同理把多个类别按给定顺序一起移到 beforeCategoryId 之前（缺省放到最后）。
[[nodiscard]] Status MoveCategories(TriggerDocument& document, std::span<const std::int32_t> categoryIds,
    std::optional<std::int32_t> beforeCategoryId);

// 语句所在位置：父语句（0 = 触发器根）与分支号；触发器根按类型分组，branch 为 0 事件 / 1 条件 / 2 动作。
struct EcaSlotKey {
    StableId parentEcaId = 0;
    std::int32_t branch = 0;
    bool operator==(const EcaSlotKey&) const = default;
};
// 选区里同一位置内相邻的一段语句（显示顺序）。
struct EcaSelectionRun {
    EcaSlotKey slot;
    std::vector<StableId> ids;
};
struct EcaSelection {
    std::vector<StableId> ids;          // 显示顺序；祖先也被选中的语句由祖先代表，不再单列
    std::vector<EcaSelectionRun> runs;  // ids 按“同一位置内相邻”分段
};
// 只保留 triggerId 语句树中的语句（参数里的函数调用不算），按传统视图显示顺序排列：
// 根上先事件、再条件、再动作，组内按存储顺序；块语句内按分支号、再按存储顺序。
[[nodiscard]] EcaSelection NormalizeEcaSelection(const TriggerDocument& document, StableId triggerId,
    std::span<const StableId> ecaIds);
// 语句所在位置；不在 triggerId 的语句树中时为空。
[[nodiscard]] std::optional<EcaSlotKey> EcaSlotOf(const TriggerDocument& document, StableId triggerId, StableId ecaId);
// 位置里的全部语句（显示顺序）；位置不在 triggerId 的语句树中时为空。
[[nodiscard]] std::vector<StableId> EcaSlotSiblings(const TriggerDocument& document, StableId triggerId,
    const EcaSlotKey& slot);

// 依次删除多个语句子树（压实节点表）；任一失败时已删的按回执恢复，文档不变。撤销须按回执倒序。
[[nodiscard]] Result<std::vector<TriggerDocumentEditor::EcaRemoveReceipt>> RemoveEcaSubtrees(
    TriggerDocument& document, std::span<const StableId> ecaIds);
// 依次把多个语句按给定顺序移到 triggerId 中的同一位置，插在 beforeEcaId 之前（0 = 该位置末尾）；
// beforeEcaId 本身也在移动时顺延。语句类型是否适合该位置由调用方核对。任一失败时已移动的按回执恢复。
[[nodiscard]] Result<std::vector<TriggerDocumentEditor::EcaMoveReceipt>> MoveEcas(TriggerDocument& document,
    StableId triggerId, std::span<const StableId> ecaIds, const EcaSlotKey& slot, StableId beforeEcaId);

} // namespace War3Map::Trigger
