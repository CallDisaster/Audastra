#pragma once
#include "War3MapCore/Script/MapScript.h"
#include <map>
#include <set>

namespace War3Map::Script::detail {
struct MapContext;
struct RandomPlacedObject {
    std::int32_t kind = 0;
    std::string typeExpression;
    std::map<std::string, std::int32_t> typeColors;
    std::vector<ObjectQueries::AbilityInfo> abilities;
};
// 仅生成期间持有，来源表仍由同一MapScriptInputs的只读拥有者保活。
struct MapRandomInitialization {
    std::map<std::size_t, RandomPlacedObject> placements;
    std::set<std::int32_t> unitTables, itemTables;
    bool hasRandomInitialization = false;
};
[[nodiscard]] std::string RandomGroupSymbol(std::int32_t number);
[[nodiscard]] std::optional<std::string> RandomChoiceExpression(std::string_view rawcode, std::int32_t kind);
[[nodiscard]] std::string LegacyItemChoiceExpression(std::string_view rawcode);
[[nodiscard]] Result<MapRandomInitialization> CaptureMapRandomInitialization(const MapScriptInputs& inputs);
void WriteRandomGroupGlobals(MapContext& context);
void WriteRandomGroups(MapContext& context);
} // namespace War3Map::Script::detail
