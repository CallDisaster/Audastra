#pragma once

#include "War3MapCore/Trigger/TriggerDocument.h"
#include "War3MapCore/Trigger/TriggerGraphEditing.h"

#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace War3Map::Trigger {

// 旧版本编辑器写的触发器引用：gg_trg_ 后面是没按原字节转换的名字（解码后的文字转换或原样），
// 且不是任何触发器的 WE 写法。生成脚本照常认出，但 WE/YDWE 打开再保存后找不到对应声明。
// 键为旧写法，值为能对上的触发器（文档顺序）；生成脚本取第一个，改正只改唯一对上的。
[[nodiscard]] std::map<std::string, std::vector<StableId>> LegacyTriggerSymbols(const TriggerDocument& document);

struct LegacyTriggerReference {
    StableId argumentId = 0;
    StableId ownerTriggerId = 0;   // 引用所在的触发器
    StableId targetTriggerId = 0;  // 引用的触发器
    std::string value;             // 旧写法
    std::string symbol;            // WE 写法
};
// 按触发器顺序列出声明变量参数里旧写法的引用（字符串字面值不算）；ownerTrigger 只看该触发器。
// 同一旧写法对上多个触发器时不列，不猜是哪一个。
[[nodiscard]] std::vector<LegacyTriggerReference> FindLegacyTriggerReferences(const TriggerDocument& document,
    std::optional<StableId> ownerTrigger = std::nullopt);
// 把旧写法改成 WE 写法的参数修改；带原值核对，文档已变时整批拒绝。
[[nodiscard]] std::vector<GraphEditOperation> LegacyTriggerReferenceFixes(std::span<const LegacyTriggerReference> references);

} // namespace War3Map::Trigger
