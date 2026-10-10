#pragma once

#include "War3MapCore/Script/MapScript.h"

#include <array>

namespace War3Map::Script {

// 玩家槽位是来源档案的事实，不能仅凭W3I版本推测中立所有者编号。
enum class MapInitializationPlayerLayout : std::uint8_t { Classic12, Extended24 };
struct MapInitializationOptions {
    // v25未指定时沿用Classic12；v31存在玩家/单位时必须显式给定。
    std::optional<MapInitializationPlayerLayout> playerLayout;
};

enum class MapInitStage : std::uint8_t {
    Sounds, Regions, Cameras, Upgrades, TechTree, Destructables, Items, Units,
    Blizzard, Globals, CustomTriggers, InitializationTriggers
};

struct MapInitSymbol {
    std::string name, type, member;
    std::size_t sourceIndex = 0;
};
struct MapInitAbility {
    MapData::PlacementAbility placement;
    ObjectQueries::AbilityInfo effective;
};
struct MapInitUnit {
    MapData::UnitPlacement placement;
    std::size_t sourceIndex = 0;
    std::string symbol;
    bool item = false, startLocation = false, building = false, hero = false;
    int typeTeamColor = -1;
    std::vector<MapInitAbility> abilities;
    std::optional<std::string> waygateRegionSymbol;
};
struct MapInitDoodad {
    MapData::DoodadPlacement placement;
    std::size_t sourceIndex = 0;
    std::string symbol;
    bool scripted = false;
};
struct MapInitSound {
    MapData::SoundRecord sound;
    std::size_t sourceIndex = 0;
    std::string symbol;
    std::optional<std::string> label;
    std::optional<int> duration;
};
struct MapInitRegion {
    MapData::RegionRecord region;
    std::size_t sourceIndex = 0;
    std::string symbol;
};
struct MapInitCamera {
    MapData::CameraRecord camera;
    std::size_t sourceIndex = 0;
    std::string symbol;
};

// 拥有完整初始化事实与确定顺序；不保留磁盘回调、源文档指针或生成后的JASS文本。
// 原始记录保留未发射项的来源。writer必须显式处理或拒绝，不能静默丢字段。
struct MapInitializationPlan {
    MapData::MapSettings settings;
    std::optional<MapData::StringTableDocument> strings;
    std::optional<TerrainExtent> terrain;
    std::uint32_t playerSlotCount = 12, neutralAggressive = 12, neutralPassive = 15;
    std::vector<MapInitUnit> units;
    std::vector<MapInitDoodad> doodads;
    std::vector<MapData::SpecialDoodadPlacement> specialDoodads;
    std::vector<MapInitSound> sounds;
    std::vector<MapInitRegion> regions;
    std::vector<MapInitCamera> cameras;
    std::vector<MapInitSymbol> symbols;
    std::vector<std::size_t> unitCreationOrder, itemCreationOrder, doodadCreationOrder;
    std::vector<MapInitStage> mainOrder;
    std::array<float, 8> cameraBounds{};
    std::optional<std::array<double, 4>> weatherBounds;
    std::string dayNightGroup, ambientSound;
};

[[nodiscard]] Result<MapInitializationPlan> BuildMapInitializationPlan(const MapScriptInputs& inputs,
    const MapInitializationOptions& options = {});

} // namespace War3Map::Script
