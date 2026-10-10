#pragma once

#include "War3MapCore/MapData/MapAuxiliary.h"
#include "War3MapCore/ObjectData/ObjectFieldSchema.h"
#include "War3MapCore/Script/SlkTemplatePlan.h"

#include <functional>

namespace War3Map::Script {

enum class SlkTemplateProofCoverage {
    ExplicitAssignmentsOnly,
};

struct SlkTemplateFieldProof {
    ObjectData::ObjectKey object;
    ObjectData::ModificationKey modification;
    ObjectData::NativeValueType valueType;
    SlkTemplateSource source;
};

struct SlkTemplateProof {
    SlkTemplateProofCoverage coverage = SlkTemplateProofCoverage::ExplicitAssignmentsOnly;
    std::size_t abilityCount = 0;
    std::size_t assignmentCount = 0; // 包括被后续同地址赋值覆盖的写入
    std::vector<SlkTemplateFieldProof> fields; // 每个最终 native 地址一条证据
};

struct SlkTemplateProofOptions {
    // 从调用方已固定的标准物编目录解析，不从 rawcode 前缀推断。
    // 必须证明父对象存在，返回 rawcode == parentId 及其有效 SLK baseCode。
    // 缺省或查询失败均拒绝；本轮不支持自定义父链和地图中的父对象覆盖。
    std::function<Result<ObjectData::ObjectFieldContext>(std::string_view parentId)> resolveStandardParent;
};

// 纯只读校验；不执行 Lua、不改文档、不生成 overlay，也不接入保存。
// 每次赋值均须唯一匹配 metadata 并通过类型校验，再按执行顺序取同 native 地址的最后值。
// Int32 拒绝溢出/小数，bool 仅可写 metadata bool；Real/Unreal 分开核对并按 Float32 位比较。
// 字符串两侧使用 WTS 完整引用解析，无递归、无缺失/重复 ID 猜测；其余字符串按字节比较。
// 每个最终字段必须已有显式 native modification，缺项不会回退到继承值。
// 成功仅证明 parent 身份及显式字段；不证明 new() 默认继承、额外字段或 W2lo 清理等副作用。
// 调用方绝不能仅凭 Succeeded() 放行跳过模板；这些范围必须另行证明。
[[nodiscard]] Result<SlkTemplateProof> ProveSlkTemplateExplicitAssignments(
    const SlkTemplatePlan& plan,
    const ObjectData::NativeObjectDocument& abilities,
    const ObjectData::ObjectFieldSchema& schema,
    const MapData::StringTableDocument& strings,
    const SlkTemplateProofOptions& options = {});

} // namespace War3Map::Script
