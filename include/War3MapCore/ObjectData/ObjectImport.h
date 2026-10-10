#pragma once

#include "War3MapCore/ObjectData/LniObjectCodec.h"
#include "War3MapCore/ObjectData/ObjectTypes.h"
#include "War3MapCore/Types.h"

#include <cstddef>
#include <span>
#include <vector>

namespace War3Map::ObjectData {

class LniObjectDocument;
class NativeObjectDocument;
class ObjectFieldSchema;
class StandardObjectCatalog;

// 在创建任何 pending 根或 native clone 前验证来源并返回迭代拓扑顺序。
// 峰值按实际 native wire 基线、普通新根的保守 wire 上界及全部克隆记录累加，
// 不物化对象、不先序列化候选；后续字段删除不能抵消克隆阶段的 64 MiB 预算。
[[nodiscard]] Result<std::vector<std::size_t>> PlanNativeCloneProjection(
    const NativeObjectDocument& nativeDocument,
    const LniObjectDocument& lniDocument,
    const ObjectFieldSchema* schema = nullptr);

// 仅物化克隆链中尚未写入 native 的普通新自定义根。复用新增对象的严格字段
// 编码规则，不重写已有 native 来源，也不创建克隆目标；失败保持输入完整不变。
[[nodiscard]] Status StagePendingNativeCloneSources(
    NativeObjectDocument& nativeDocument,
    const LniObjectDocument& lniDocument,
    const ObjectFieldSchema* schema = nullptr,
    const StandardObjectCatalog* standardCatalog = nullptr);

// 将 native 二进制物体数据（w3u/w3t/w3a 等）转换为 LNI 文本字节。
// 内部流程：NativeObjectCodec.Parse → RenderObjectRecordsToLni。
// 解析诊断与渲染诊断合并到返回值的 diagnostics 中。
[[nodiscard]] Result<std::vector<std::byte>> ConvertNativeObjectDataToLni(
    ObjectType type, std::span<const std::byte> nativeBytes);

// 将 LNI 文档相对 native 基线的差异回灌到 native 文档（另存为写回路径）。
// 支持：字段值修改、新增/重置字段、新建原版覆盖和自定义对象、清除原版覆盖和删除自定义对象。
// schema 用于精确区分新增字段的 real/unreal 等存储类型。
// 整组差异先写入隔离副本；失败时调用方文档保持原状。
// 清除 Original 表记录只恢复标准值，不删除游戏中的原版对象；变更父级仍拒绝。
// 多层自定义父链只在提供原版目录且能确认终点与全部继承字段时扁平化；
// LNI 工程中的原始父链不被改写。已有 native 对象的父 ID 不会被猜测重定向。
[[nodiscard]] Status ApplyLniChangesToNative(
    NativeObjectDocument& nativeDocument,
    const LniObjectDocument& lniDocument,
    const ObjectFieldSchema* schema = nullptr,
    const StandardObjectCatalog* standardCatalog = nullptr);

// 三方合并旧 native、当前可编辑 LNI、模板生成的完整 native。
// 从当前 LNI 副本出发，只应用生成侧相对旧 native 的无冲突字段变化，
// 因此保留未触及的注释、排版、不透明原文及自定义父链。
// 最终重新投影到旧 native，并要求完整二进制与 generatedNative 逐字节相同；
// 冲突、未知语义和不能无损投影的情况均拒绝，三个输入保持不变。
[[nodiscard]] Result<LniObjectDocument> MergeGeneratedNativeWithLniEdits(
    const NativeObjectDocument& nativeBaseline,
    const LniObjectDocument& currentEditable,
    const NativeObjectDocument& generatedNative,
    const ObjectFieldSchema* schema = nullptr,
    const StandardObjectCatalog* standardCatalog = nullptr);

} // namespace War3Map::ObjectData
