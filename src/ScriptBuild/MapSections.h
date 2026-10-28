#pragma once

#include "ScriptText.h"
#include "MapRandomInitialization.h"

#include "War3MapCore/Script/MapScript.h"

#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace War3Map::Script::detail {

// 整图各段共享的生成上下文。
struct MapContext {
    const MapScriptInputs& in;
    ScriptText& out;
    std::vector<Diagnostic>& diagnostics;
    // 被触发器或自定义脚本引用的预放置对象；可破坏物仅能绑定 DOO 中需脚本创建的记录。
    std::set<std::string> referencedObjects;
    std::vector<std::size_t> scriptedDoodadIndexes;
    MapRandomInitialization random;

    [[nodiscard]] std::string Resolve(std::string_view text) const;
    [[nodiscard]] std::string RegionVariable(std::string_view name) const;
    [[nodiscard]] std::string RegionByCreation(std::int32_t creationNumber) const;
    [[nodiscard]] bool IsItem(std::string_view rawcode) const;
    [[nodiscard]] bool IsBuilding(std::string_view rawcode) const;
    [[nodiscard]] bool IsHero(std::string_view rawcode) const;
    [[nodiscard]] std::string PlacedVariable(std::string_view prefix, const MapData::UnitPlacement& unit) const;
    [[nodiscard]] std::string DoodadVariable(const MapData::DoodadPlacement& doodad) const;
    void Warn(std::string code, std::string message) const;
};

void WriteSectionBanner(ScriptText& out, std::string_view title);
[[nodiscard]] std::string Real1(double value);
[[nodiscard]] std::string Real3(double value);
[[nodiscard]] std::string Rawcode(std::string_view rawcode);

// MapGlobals.cpp
// 返回 YDTrigger 存储系统开关的插入位置（生成完触发器后才知道是否用到存储）。
[[nodiscard]] std::size_t WriteGlobals(MapContext& context, const std::vector<std::string>& triggerVariables);
// 触发器之外的手写代码（自定义脚本、文本触发器、自定义代码参数）是否直接引用 YDTrigger 存储。
[[nodiscard]] bool MentionsStorage(const Trigger::TriggerDocument& document);
void WriteInitGlobals(MapContext& context);
// MapObjects.cpp
void WriteItemTables(MapContext& context);
void WriteDoodadTables(MapContext& context);
void WriteSounds(MapContext& context);
void WriteDestructables(MapContext& context);
void WriteItems(MapContext& context);
void WriteUnits(MapContext& context);
void WriteRegions(MapContext& context);
void WriteCameras(MapContext& context);
[[nodiscard]] bool HasPlacedItems(const MapContext& context);
[[nodiscard]] bool HasScriptedDestructables(const MapContext& context);
[[nodiscard]] bool HasDoodadDropChoices(const MapData::DoodadPlacement& doodad);
// MapPlayers.cpp
[[nodiscard]] bool WriteUpgrades(MapContext& context);
[[nodiscard]] bool WriteTechRestrictions(MapContext& context);
void WritePlayers(MapContext& context);
void WriteMain(MapContext& context, bool upgrades, bool techTree);
void WriteConfig(MapContext& context);

} // namespace War3Map::Script::detail
