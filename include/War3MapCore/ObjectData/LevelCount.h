#pragma once

#include "War3MapCore/ObjectData/ObjectFieldSchema.h"
#include "War3MapCore/ObjectData/ObjectTypes.h"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

// 技能的等级数（AbilityData 的 levels，原生 alev）与按等级重复的字段。
// 等级数决定物编显示哪几级；提高等级数时，新等级沿用上一级的值并实际写入（游戏不会替缺数据的等级补值）。
// 物编界面与 Agent 工具共用。
namespace War3Map::ObjectData {

// 技能的等级数字段。
[[nodiscard]] bool IsLevelCountField(const ObjectFieldDefinition& definition);
// 按等级重复的字段在某一级上的地址（repeat > 0 且等级 ≥ 1）。
[[nodiscard]] bool IsPerLevelField(const ObjectFieldDefinition& definition, const ModificationKey& key);
// 等级数字段的值；不是 1..1000 的整数时为空。
[[nodiscard]] std::optional<std::uint32_t> LevelCountValue(const Value& value);

struct LevelFieldValue {
    ModificationKey key;  // 含等级
    const ObjectFieldDefinition* definition = nullptr;
    Value value;          // 当前有效值
};

// 等级数从 oldCount 提高到 newCount 时要写入的新等级：同一字段（字段、数据序号、变化相同）里，
// 高于 oldCount、还没有值的等级沿用下面最近一级的值；已有值的等级不动，没有任何已知低等级的字段不补。
// 不提高时为空。
[[nodiscard]] std::vector<LevelFieldValue> FillAddedLevels(std::span<const LevelFieldValue> fields,
    std::uint32_t oldCount, std::uint32_t newCount);

} // namespace War3Map::ObjectData
