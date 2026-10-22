#pragma once

#include "War3MapCore/Trigger/TriggerDocument.h"
#include "War3MapCore/Trigger/MapObjectReferences.h"
#include "War3MapCore/Trigger/UiCatalog.h"
#include "War3MapCore/Types.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// 功能写法：一段文本声明一个功能用到的全局变量和若干触发器，整批写进同一个类别。
// 触发器体就是紧凑 T 写法（TriggerTextNotation.h），仍是普通 T 树。
//
//   变量:
//       VJ_Voodoo: integer = 0
//       VJ_Marks: unit[]
//       VJ_Model: string = "war3mapImported\\bonespirit.mdx"
//   触发器 沃金 Q 医疗波:
//       events:
//           TriggerRegisterAnyUnitEventBJ(EVENT_PLAYER_UNIT_SPELL_EFFECT)
//       actions:
//           ...
//   触发器 沃金 初始化 (init, off):
//       ...
//
// 顶层行（行首不缩进）只有 variables:/变量: 与 trigger/触发器 名字 [(选项)]: 两种，内容须缩进。
// 变量写 名字: 类型、名字: 类型[]（数组）或 名字: 类型[长度]，可加 = 初值（"文本"、'A000'、数字、预设名、null）；
// 名字可带 udg_ 前缀。触发器选项：init/初始化（地图初始化时运行）、off/关闭（初始关闭）、disabled/禁用。
// 名字含 : ( ) 或首尾空格时用双引号括起。// 开头的行是注释。
namespace War3Map::Trigger {

struct FeatureVariable {
    std::string name, type;
    bool isArray = false;
    std::int32_t size = 1;
    std::optional<std::string> initial;
    std::size_t line = 0;
};
struct FeatureTrigger {
    std::string name;
    bool enabled = true, initiallyOn = true, runOnInit = false;
    // 触发器体的紧凑 T 文本；前面补了空行，行列与整段功能文本一致。
    std::string body;
    std::size_t line = 0;
};
struct FeatureText {
    std::vector<FeatureVariable> variables;
    std::vector<FeatureTrigger> triggers;
};

// 结构错误一次全部列出（各带行号）；触发器体在应用时按紧凑 T 解析。
[[nodiscard]] Result<FeatureText> ParseFeatureText(std::string_view text);

// 把一个类别导出为同一写法：类别下全部触发器，以及它们引用到的全局变量（按变量表顺序）。
// 无法如实表达的内容明确报错。
[[nodiscard]] Result<std::string> RenderFeatureText(const TriggerDocument& document, const UiCatalog& catalog,
    std::int32_t categoryId, std::size_t maxBytes = 2U * 1024U * 1024U, const MapObjectReferenceSnapshot* mapObjects = nullptr);

} // namespace War3Map::Trigger
