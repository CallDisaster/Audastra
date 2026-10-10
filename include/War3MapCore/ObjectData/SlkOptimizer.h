#pragma once

#include "War3MapCore/ObjectData/ObjectFieldSchema.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace War3Map::ObjectData {

// 原生 SLK 优化：把地图的自定义物体写成 SLK 表行和 profile 节，游戏直接从表中加载，
// 不再在载入时逐个复制父物体。原版物体的表行、profile 节和地图对它们的修改都逐字节保留。
// 写不进表的修改（超出表列的等级、含引号/换行的文本、未定义字段等）留在精简后的物编文件里，
// 由游戏叠加读取；完成后按同一读取规则回读，逐字段证明每个被转换物体的有效值不变。
struct SlkOptimizerInput {
    // 地图成员；不存在返回空。名称为游戏内路径，例如 "Units\\UnitData.slk"、"war3map.w3u"。
    std::function<std::optional<std::vector<std::byte>>(const std::string&)> mapMember;
    // 游戏 MPQ 文件；地图同名成员优先（与游戏加载顺序一致）。
    ObjectFieldSchema::FileReader gameFile;
    const ObjectFieldSchema* schema = nullptr;
};

struct SlkOptimizerReplacement {
    std::string name; // 游戏内路径
    std::vector<std::byte> bytes;
};

struct SlkOptimizerResult {
    std::vector<SlkOptimizerReplacement> replacements;
    std::size_t convertedObjects = 0;   // 已写入 SLK 的自定义物体
    std::size_t keptCustomObjects = 0;  // 保留为物编自定义物体（附原因）
    std::size_t residualFields = 0;     // 留在物编文件里的修改
    std::size_t verifiedValues = 0;     // 回读逐字段比较的值数量
    std::vector<std::string> notes;     // 保留原因等（中文）
};

[[nodiscard]] Result<SlkOptimizerResult> OptimizeObjectDataToSlk(const SlkOptimizerInput& input);
// 优化器可能读取的全部地图成员名（物编、字符串表、LNI 检查、各 SLK 表与 profile 文件）。
// 调用方可按名一次性精确捕获，不依赖归档 listfile。
[[nodiscard]] std::vector<std::string> SlkOptimizerMemberNames();

} // namespace War3Map::ObjectData
