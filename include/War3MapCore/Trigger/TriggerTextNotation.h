#pragma once

#include "War3MapCore/Trigger/TriggerGraphEditing.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// 紧凑 T 写法：用接近 WE 显示的函数调用格式书写一段事件/条件/动作树。
// 每个函数名按函数库定义一一对应一个节点，只是树的另一种写法，不是 JASS 编译；
// 解析结果交给图编辑批次，走同一条类型检查、撤销与保存链。导出与解析互为往返。
//
//   events:
//       TriggerRegisterUnitEvent(udg_Hero, UnitEventSpellEffect)
//   conditions:
//       OperatorCompareAbilityId(GetSpellAbilityId(), OperatorEqualENE, 'A04W')
//   actions:
//       IfThenElseMultiple() {
//           if:   OperatorCompareInteger(udg_Count, OperatorGreater, 3)
//           then: KillUnit(GetTriggerUnit())
//       }
//
// 参数写法：Name(...) 函数；"文本"（\" \\ \n \r \t 转义）；'A04W' 物编编号；数字；true/false；
// 预设名；udg_名 或变量原名，数组 udg_名[下标]；gg_ 开头的预放对象/触发器；
// 反引号 `名字` 用于含特殊字符的名字；自定义代码类型参数可直接写标识符。
// 函数槽参数（eventcall/boolexpr/code）里直接写事件/条件/动作。
// 语句前加 ~ 表示禁用；// 到行尾是注释。块语句用 { 分支标签: 语句... }，
// 分支标签见 TriggerBlocks.h 的 key（如 if/then/else、actions），也可写中文名或 #分支号。
namespace War3Map::Trigger {

struct TriggerTextTarget {
    // 为空：触发器根，可用 events:/conditions:/actions:（或 事件:/条件:/动作:）分段，未分段默认动作。
    std::string blockFunction;
    // 插入块语句某个分支时的分支号。
    std::int32_t childBranch = 0;
};

// 语法错误在第一处停止；名称、参数个数、分支等错误一次全部列出（各带行列）。
// 空文本得到空列表（整体替换时表示清空）。
[[nodiscard]] Result<std::vector<std::shared_ptr<const GraphEcaValue>>> ParseTriggerText(
    std::string_view text, const TriggerDocument& document, const UiCatalog& catalog,
    const TriggerTextTarget& target, const TriggerGraphEditLimits& limits = {});

// 导出文本里每个节点（语句及参数中的函数调用）开始的位置；行列从 1 起，列按字符计，与解析报错一致。
struct TriggerTextAnchor {
    StableId ecaId = 0;
    std::size_t line = 1;
    std::size_t column = 1;
};
struct TriggerTextArgumentAnchor {
    StableId argumentId = 0;
    std::size_t line = 1, column = 1, byteOffset = 0, byteLength = 0;
};

// 把触发器的全部根 ECA 导出为同一写法；无法如实表达的结构明确报错，不输出近似文本。
// anchors 非空时按文本顺序填入各节点位置（供检查结果定位回 T）。
// 给出 mapObjects 时，区域/镜头/声音参数在能唯一换回同一生成名时导出为显示名；
// 读取与按行号修改须用同一份快照，导出才一致。
[[nodiscard]] Result<std::string> RenderTriggerText(const TriggerDocument& document, const UiCatalog& catalog,
    StableId triggerId, std::size_t maxBytes = 1024U * 1024U, std::vector<TriggerTextAnchor>* anchors = nullptr,
    const MapObjectReferenceSnapshot* mapObjects = nullptr,
    std::vector<TriggerTextArgumentAnchor>* argumentAnchors = nullptr);

// 单个值的写法与参数相同："文本"（转义同上）、'A000'、数字、名字（可用反引号）；返回值本体。
[[nodiscard]] Result<std::string> ParseTriggerTextValue(std::string_view text);
// 文本加双引号并转义；名字在含特殊字符时加反引号。与解析互为往返。
[[nodiscard]] std::string TriggerTextQuoted(std::string_view value);
[[nodiscard]] std::string TriggerTextName(std::string_view name);

} // namespace War3Map::Trigger
