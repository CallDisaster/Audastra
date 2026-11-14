#pragma once

#include "War3MapCore/Trigger/TriggerEditing.h"
#include "War3MapCore/Trigger/TriggerGraphEditing.h"

#include <span>
#include <string>
#include <vector>

namespace War3Map::Trigger {

// 作者代码里对触发器/变量的确定引用：全局自定义代码、自定义代码触发器、自定义脚本等代码参数、变量初值。
// 只看作者写的原文，不生成整图脚本、不展开包含文件或函数库；
// 按名称动态调用（ExecuteFunc(变量) 等）判断不了指向谁，不算引用。
// 每个代码块对每个目标最多报一处（第一处）。
struct DeclarationCodeReference {
    std::size_t target = 0; // targets 下标
    AtomicRenameCodeKind kind = AtomicRenameCodeKind::GlobalCustomCode;
    StableId blockId = 0;   // 全局代码为 0；自定义代码为触发器；代码参数为参数；变量初值为变量
    std::size_t line = 0;
    std::string excerpt;
};

[[nodiscard]] Result<std::vector<DeclarationCodeReference>> FindDeclarationCodeReferences(
    const TriggerDocument& document, const UiCatalog& catalog, std::span<const GraphRecordRef> targets);

} // namespace War3Map::Trigger
