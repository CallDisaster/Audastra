#pragma once

#include "War3MapCore/Trigger/TriggerDocument.h"
#include "War3MapCore/Trigger/MapObjectReferences.h"
#include "War3MapCore/Trigger/UiCatalog.h"
#include "War3MapCore/Types.h"

#include <string>
#include <vector>

// 两份触发器文档之间内容有变化的触发器，用紧凑 T 文本给出逐行差异，供写入前预览。
namespace War3Map::Trigger {

struct TriggerTextChange {
    StableId triggerId = 0;
    std::string name;
    std::string status; // added / removed / changed
    // 差异行："  " 未变（只保留改动附近两行）、"- " 删除、"+ " 新增；"@@ 第 N 行" 标出写入后文本的行号。
    // 无法导出紧凑 T（自定义代码触发器等）时为空，并在 note 里说明。
    std::string diff;
    std::string note;
};

// 只比较节点结构与参数值；名称、开关等触发器字段的变化也算 changed（diff 可能为空）。
// 结果按 after 中的顺序，删除的排在最后；总字节超出 maxBytes 时截断并在最后一项 note 中注明。
[[nodiscard]] std::vector<TriggerTextChange> DiffTriggerTexts(const TriggerDocument& before, const TriggerDocument& after,
    const UiCatalog& catalog, std::size_t maxBytes = 256U * 1024U, const MapObjectReferenceSnapshot* mapObjects = nullptr);

} // namespace War3Map::Trigger
