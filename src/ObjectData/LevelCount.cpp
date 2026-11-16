#include "War3MapCore/ObjectData/LevelCount.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <string>
#include <tuple>

namespace War3Map::ObjectData {
namespace {

constexpr std::uint32_t kMaximumLevels = 1000;

bool EqualsIgnoreCase(std::string_view left, std::string_view right) {
    return left.size() == right.size() && std::equal(left.begin(), left.end(), right.begin(), [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
    });
}

} // namespace

bool IsLevelCountField(const ObjectFieldDefinition& definition) {
    return definition.objectType == ObjectType::Ability && definition.repeat == 0
        && EqualsIgnoreCase(definition.sourceField, "levels") && EqualsIgnoreCase(definition.sourceTable, "AbilityData");
}

bool IsPerLevelField(const ObjectFieldDefinition& definition, const ModificationKey& key) {
    return definition.repeat > 0 && key.level && *key.level >= 1;
}

std::optional<std::uint32_t> LevelCountValue(const Value& value) {
    const auto* number = std::get_if<std::int32_t>(&value);
    if (!number || *number < 1 || *number > static_cast<std::int32_t>(kMaximumLevels)) return std::nullopt;
    return static_cast<std::uint32_t>(*number);
}

std::vector<LevelFieldValue> FillAddedLevels(std::span<const LevelFieldValue> fields, std::uint32_t oldCount, std::uint32_t newCount) {
    std::vector<LevelFieldValue> added;
    newCount = std::min(newCount, kMaximumLevels);
    if (newCount <= oldCount) return added;
    // 同一字段的各级按等级排好；字段保持首次出现的次序，写入次序稳定。
    using Family = std::tuple<std::string, std::optional<std::uint32_t>, std::optional<std::uint32_t>>;
    std::vector<Family> order;
    std::map<Family, std::map<std::uint32_t, const LevelFieldValue*>> families;
    for (const auto& field : fields) {
        if (!field.definition || !IsPerLevelField(*field.definition, field.key)) continue;
        Family family{field.key.fieldId, field.key.dataIndex, field.key.variation};
        auto [found, inserted] = families.try_emplace(family);
        if (inserted) order.push_back(family);
        found->second.try_emplace(*field.key.level, &field);
    }
    for (const auto& family : order) {
        const auto& levels = families.at(family);
        const LevelFieldValue* previous = nullptr;
        std::optional<Value> carried;
        for (std::uint32_t level = 1; level <= newCount; ++level) {
            if (const auto known = levels.find(level); known != levels.end()) {
                previous = known->second;
                carried = known->second->value;
                continue;
            }
            if (level <= oldCount || !previous || !carried) continue;
            LevelFieldValue fill{previous->key, previous->definition, *carried};
            fill.key.level = level;
            added.push_back(std::move(fill));
        }
    }
    return added;
}

} // namespace War3Map::ObjectData
