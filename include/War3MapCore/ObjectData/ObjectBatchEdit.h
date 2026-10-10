#pragma once

#include "War3MapCore/ObjectData/LniObjectCodec.h"

#include <span>

namespace War3Map::ObjectData {

// 多对象共同字段的原子编辑：先在副本上验证全部目标，再把完整副本交给调用方
// 作为单次撤销命令的 after 状态。当前不跨类别，不猜字段是否适用于某个基础对象。
[[nodiscard]] Result<LniObjectDocument> ApplyBatchModification(
    const LniObjectDocument& document,
    std::span<const ObjectKey> targets,
    const ModificationKey& field,
    const Value& value);

} // namespace War3Map::ObjectData
