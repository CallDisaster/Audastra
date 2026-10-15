#include "MapInitializationPlanInternal.h"

#include "War3MapCore/MapData/ClassicMapLayouts.h"

#include <limits>
#include <map>
#include <numeric>
#include <set>
#include <tuple>

namespace War3Map::Script {
namespace {

using namespace init_detail;

std::string DayNightGroup(std::uint8_t tileset) {
    switch (tileset) {
    case 'A': return "Ashenvale";
    case 'C': return "Felwood";
    case 'D': return "Dungeon";
    case 'G': return "Underground";
    case 'J': case 'X': return "Dalaran";
    case 'B': case 'F': case 'I': case 'K': case 'L': case 'N': case 'O':
    case 'Q': case 'V': case 'W': case 'Y': case 'Z': return "Lordaeron";
    default: return {};
    }
}

std::string Ambient(std::uint8_t tileset) {
    switch (tileset) {
    case 'A': return "Ashenvale";
    case 'B': return "Barrens";
    case 'C': return "Felwood";
    case 'D': case 'G': return "Dungeon";
    case 'F': return "LordaeronFall";
    case 'I': return "Icecrown";
    case 'J': return "DalaranRuins";
    case 'K': return "BlackCitadel";
    case 'L': return "LordaeronSummer";
    case 'N': return "Northrend";
    case 'O': return "Outland";
    case 'Q': return "VillageFall";
    case 'V': return "Village";
    case 'W': return "LordaeronWinter";
    case 'X': return "Dalaran";
    case 'Y': return "CityScape";
    case 'Z': return "SunkenRuins";
    default: return {};
    }
}

class PlanBuilder {
public:
    PlanBuilder(const MapScriptInputs& inputs, const MapInitializationOptions& options)
        : inputs_(inputs), options_(options) {}

    MapInitializationPlan Build() {
        CaptureSettings();
        CaptureRegions();
        CaptureCameras();
        CaptureSounds();
        CaptureDoodads();
        CaptureUnits();
        ArrangeCreation();
        ArrangeMain();
        return std::move(plan_);
    }

private:
    struct UnitFacts {
        bool item = false, building = false, hero = false;
        int teamColor = -1;
    };
    struct SoundFacts {
        std::optional<std::string> label;
        std::optional<int> duration;
    };

    void AddSymbol(const std::string& name, std::string type, std::string_view member, std::size_t index) {
        if (!Identifier(name))
            Reject("MAP_INIT_SYMBOL_INVALID", "生成的对象符号不是有效标识符：" + name, member, index);
        const auto [found, inserted] = symbolSources_.emplace(name, plan_.symbols.size());
        if (!inserted) {
            const auto& earlier = plan_.symbols[found->second];
            Reject("MAP_INIT_SYMBOL_COLLISION", "对象符号 " + name + " 与 " + earlier.member
                + " 记录[" + std::to_string(earlier.sourceIndex) + "] 重复；不能覆盖绑定。", member, index);
        }
        plan_.symbols.push_back({name, std::move(type), std::string(member), index});
    }

    void CaptureSettings() {
        if (!inputs_.settings)
            Reject("MAP_INIT_SETTINGS_MISSING", "完整地图初始化需要地图设置。", kSettings);
        plan_.settings = *inputs_.settings;
        plan_.terrain = inputs_.terrain;
        if (inputs_.strings) plan_.strings = *inputs_.strings;
        const auto& settings = plan_.settings;
        const auto sourceVersion = settings.SourceFormatVersion();
        const bool roc = settings.HasRocLayout();
        if (!roc && ((settings.formatVersion != 25 && settings.formatVersion != 31)
            || (sourceVersion && *sourceVersion != 25 && *sourceVersion != 31)
            || (sourceVersion == 31 && settings.formatVersion != 31)))
            Reject("MAP_INIT_SETTINGS_VERSION_UNSUPPORTED", "初始化只支持混乱之治 v18、v25 或明确档案的 v31 已知子集；不能改版本标签丢字段。", kSettings);
        if (!settings.FullyParsed())
            Reject("MAP_INIT_SETTINGS_INCOMPLETE", "自定义表缺失或含未知尾部，不能把未解析部分当作空表。", kSettings);
        // 混乱之治的战役背景与载入画面编号由游戏读地图设置，脚本里没有对应语句。
        if ((!roc && (settings.rocCampaignBackground || settings.rocLoadingScreenNumber))
            || settings.forceDefaultCameraZoom || settings.forceMaxCameraZoom || settings.forceMinCameraZoom
            || (settings.formatVersion == 25 && !settings.HasClassicLayout()))
            Reject("MAP_INIT_SETTINGS_EXTENSION_UNSUPPORTED", "地图设置包含当前初始化尚不能表达的扩展字段。", kSettings);
        if (settings.scriptLanguage != 0 && settings.scriptLanguage != 1)
            Reject("MAP_INIT_SCRIPT_LANGUAGE_UNKNOWN", "scriptLanguage 不是已知的 JASS/Lua 档位。", kSettings);

        const bool needsPlayerLayout = !settings.players.empty() || (inputs_.units && !inputs_.units->units.empty());
        if (settings.formatVersion == 31 && needsPlayerLayout && !options_.playerLayout)
            Reject("MAP_INIT_PLAYER_LAYOUT_REQUIRED", "v31 的玩家与中立所有者编号须由来源档案明确指定，不能由格式版本推测。", kSettings);
        switch (options_.playerLayout.value_or(MapInitializationPlayerLayout::Classic12)) {
        case MapInitializationPlayerLayout::Classic12: break;
        case MapInitializationPlayerLayout::Extended24:
            plan_.playerSlotCount = 24; plan_.neutralAggressive = 24; plan_.neutralPassive = 27;
            break;
        default:
            Reject("MAP_INIT_PLAYER_LAYOUT_UNKNOWN", "未知玩家槽位档案。", kSettings);
        }
        CheckSettingsRecords();
        CheckConfigString(settings.mapName, "mapName");
        CheckConfigString(settings.description, "description");
        CaptureEnvironment();
    }

    void CheckConfigString(std::string_view text, std::string_view field) const {
        CheckString(text, field, kSettings);
        if (!text.starts_with("TRIGSTR_")) return;
        if (!plan_.strings)
            Reject("MAP_INIT_STRING_TABLE_MISSING", std::string(field) + " 引用 WTS，但本任务没有拥有该字符串表。", kSettings);
        const auto resolved = plan_.strings->Resolve(text);
        if (!resolved.Succeeded() || !resolved.value)
            Reject("MAP_INIT_STRING_REFERENCE_INVALID", std::string(field) + " 的 WTS 引用缺失、重复或格式无效：" + std::string(text), kSettings);
        CheckString(*resolved.value, field, "war3map.wts");
    }

    void CheckSettingsRecords() const {
        const auto& settings = plan_.settings;
        if (settings.players.size() > plan_.playerSlotCount)
            Reject("MAP_INIT_PLAYER_COUNT_INVALID", "玩家记录超过所选档案的玩家槽位数。", kSettings);
        std::set<std::int32_t> numbers;
        for (std::size_t i = 0; i < settings.players.size(); ++i) {
            const auto& player = settings.players[i];
            if (player.number < 0 || static_cast<std::uint32_t>(player.number) >= plan_.playerSlotCount
                || !numbers.insert(player.number).second)
                Reject("MAP_INIT_PLAYER_ID_INVALID", "玩家编号越界或重复。", kSettings, i);
            if (!MapData::KnownPlayerController(player.controller) || !MapData::KnownPlayerRace(player.race))
                Reject("MAP_INIT_PLAYER_ENUM_UNSUPPORTED", "控制者或种族未知，不能退回用户/随机种族默认值。", kSettings, i);
            if (player.enemyLowPriorityMask != 0 || player.enemyHighPriorityMask != 0)
                Reject("MAP_INIT_ENEMY_PRIORITY_UNSUPPORTED", "尚未实现新版敌对出生点优先级，不能忽略该字段。", kSettings, i);
            CheckFinite({player.startX, player.startY}, "玩家出生点", kSettings, i);
            CheckString(player.name, "玩家名称", kSettings, i);
        }
        for (std::size_t i = 0; i < settings.forces.size(); ++i)
            CheckString(settings.forces[i].name, "势力名称", kSettings, i);
        const auto& custom = *settings.custom;
        if (!custom.randomUnits.empty() || !custom.randomItems.empty())
            Reject("MAP_INIT_RANDOM_TABLE_UNSUPPORTED", "尚未实现地图随机单位/物品表初始化，不能省略随机表。", kSettings);
        for (std::size_t i = 0; i < custom.upgrades.size(); ++i) {
            const auto& upgrade = custom.upgrades[i];
            if (upgrade.level < 0 || upgrade.level == std::numeric_limits<std::int32_t>::max()
                || upgrade.availability < 0 || upgrade.availability > 2 || EmptyRawcode(upgrade.id))
                Reject("MAP_INIT_UPGRADE_INVALID", "科技等级、可用性或 ID 无效。", kSettings, i);
        }
        for (std::size_t i = 0; i < custom.techRestrictions.size(); ++i) {
            if (EmptyRawcode(custom.techRestrictions[i].id))
                Reject("MAP_INIT_TECH_INVALID", "限制科技的 ID 为空。", kSettings, i);
        }
    }

    void CaptureEnvironment() {
        const auto& settings = plan_.settings;
        const auto& environment = settings.environment;
        plan_.dayNightGroup = DayNightGroup(environment.lightEnvironment != 0
            ? environment.lightEnvironment : settings.tileset);
        plan_.ambientSound = Ambient(settings.tileset);
        if (plan_.dayNightGroup.empty() || plan_.ambientSound.empty())
            Reject("MAP_INIT_ENVIRONMENT_UNKNOWN", "地形集或光照环境未知，不能采用猜测的昼夜环境。", kSettings);
        CheckString(environment.soundEnvironment, "soundEnvironment", kSettings);
        CheckFinite({environment.fogStart, environment.fogEnd, environment.fogDensity}, "地形雾", kSettings);
        if ((settings.flags & MapData::kMapFlagTerrainFog) != 0 && (environment.fogStyle < 0 || environment.fogStyle > 2))
            Reject("MAP_INIT_FOG_STYLE_UNKNOWN", "未知地形雾模式。", kSettings);
        plan_.cameraBounds = settings.cameraBounds;
        for (const auto bound : plan_.cameraBounds) CheckFinite({bound}, "镜头边界", kSettings);
        if (plan_.terrain) {
            const auto& terrain = *plan_.terrain;
            if (!ValidTerrainExtent(terrain))
                Reject("MAP_INIT_TERRAIN_INVALID", "地形范围无效，不能生成镜头或天气范围。", "war3map.w3e");
            const auto& margins = settings.cameraMargins;
            const float left = terrain.offsetX + static_cast<float>(margins[0]) * 128.0f;
            const float right = static_cast<float>(terrain.offsetX + (static_cast<double>(terrain.width) - 1.0 - margins[1]) * 128.0);
            const float bottom = terrain.offsetY + static_cast<float>(margins[2]) * 128.0f;
            const float top = static_cast<float>(terrain.offsetY + (static_cast<double>(terrain.height) - 1.0 - margins[3]) * 128.0);
            CheckFinite({left, right, bottom, top}, "由地形计算的镜头边界", kSettings);
            if (left >= right || bottom >= top)
                Reject("MAP_INIT_CAMERA_BOUNDS_INVALID", "镜头边距使可用范围为空或逆序。", kSettings);
            plan_.cameraBounds = {left, bottom, right, top, left, top, right, bottom};
        }
        if (!EmptyRawcode(environment.weather)) {
            if (!plan_.terrain)
                Reject("MAP_INIT_WEATHER_TERRAIN_MISSING", "全局天气需要真实地形范围，不采用镜头边界替代。", "war3map.w3e");
            const auto& terrain = *plan_.terrain;
            plan_.weatherBounds = std::array<double, 4>{terrain.offsetX, terrain.offsetY,
                terrain.offsetX + (static_cast<double>(terrain.width) - 1.0) * 128.0,
                terrain.offsetY + (static_cast<double>(terrain.height) - 1.0) * 128.0};
        }
    }

    void CaptureRegions() {
        if (!inputs_.regions) return;
        if (inputs_.regions->version != 5)
            Reject("MAP_INIT_REGIONS_LAYOUT_UNSUPPORTED", "区域初始化只支持已知的 v5 布局。", kRegions);
        plan_.regions.reserve(inputs_.regions->entries.size());
        for (std::size_t i = 0; i < inputs_.regions->entries.size(); ++i) {
            const auto& region = inputs_.regions->entries[i];
            CheckFinite({region.left, region.bottom, region.right, region.top}, "区域范围", kRegions, i);
            if (region.left >= region.right || region.bottom >= region.top)
                Reject("MAP_INIT_REGION_BOUNDS_INVALID", "区域为空或逆序。", kRegions, i);
            CheckString(region.name, "区域名称", kRegions, i);
            const auto sound = MapData::ValidateRegionAmbientSound(region, inputs_.sounds);
            if (!sound.Succeeded()) Reject("MAP_INIT_REGION_SOUND_UNRESOLVED", sound.diagnostics.front().message, kRegions, i);
            MapInitRegion record{region, i, "gg_rct_" + ConvertObjectName(region.name)};
            if (region.creationNumber < 0 || !regionByCreation_.emplace(region.creationNumber, record.symbol).second)
                Reject("MAP_INIT_REGION_ID_INVALID", "区域创建编号为负或重复，传送门引用无法唯一绑定。", kRegions, i);
            AddSymbol(record.symbol, "rect", kRegions, i);
            plan_.regions.push_back(std::move(record));
        }
    }

    void CaptureCameras() {
        if (!inputs_.cameras) return;
        plan_.cameras.reserve(inputs_.cameras->entries.size());
        for (std::size_t i = 0; i < inputs_.cameras->entries.size(); ++i) {
            const auto& camera = inputs_.cameras->entries[i];
            CheckFinite({camera.targetX, camera.targetY, camera.zOffset, camera.rotation, camera.angleOfAttack,
                camera.targetDistance, camera.roll, camera.fieldOfView, camera.farClippingPlane,
                camera.nearClippingPlane}, "镜头参数", kCameras, i);
            CheckString(camera.name, "镜头名称", kCameras, i);
            MapInitCamera record{camera, i, "gg_cam_" + ConvertObjectName(camera.name)};
            AddSymbol(record.symbol, "camerasetup", kCameras, i);
            plan_.cameras.push_back(std::move(record));
        }
    }

    void CheckSound(const MapData::SoundRecord& sound, std::size_t index) const {
        CheckString(sound.name, "声音名称", kSounds, index);
        CheckString(sound.filePath, "声音路径", kSounds, index);
        CheckString(sound.eaxSetting, "EAX", kSounds, index);
        if ((sound.flags & ~15u) != 0)
            Reject("MAP_INIT_SOUND_FLAGS_UNSUPPORTED", "未知声音标志，不能当作普通声音处理。", kSounds, index);
        CheckFinite({sound.pitch, sound.pitchVariance, sound.minDistance, sound.maxDistance, sound.distanceCutoff,
            sound.coneAngleInside, sound.coneAngleOutside, sound.coneOrientation[0], sound.coneOrientation[1],
            sound.coneOrientation[2]}, "声音参数", kSounds, index);
        if (sound.IsMusic() || !sound.Is3D()) return;
        const auto uniform = [&](std::initializer_list<bool> defaults, std::string_view field) {
            const bool first = *defaults.begin();
            if (!std::all_of(defaults.begin(), defaults.end(), [first](bool value) { return value == first; }))
                Reject("MAP_INIT_SOUND_DEFAULT_GROUP_MIXED", std::string(field) + " 混用默认哨兵和显式值。", kSounds, index);
        };
        using MapData::IsSoundDefaultReal;
        uniform({IsSoundDefaultReal(sound.minDistance), IsSoundDefaultReal(sound.maxDistance)}, "距离");
        uniform({IsSoundDefaultReal(sound.coneAngleInside), IsSoundDefaultReal(sound.coneAngleOutside),
            MapData::IsSoundDefaultInteger(sound.coneOutsideVolume)}, "锥体角度");
        uniform({IsSoundDefaultReal(sound.coneOrientation[0]), IsSoundDefaultReal(sound.coneOrientation[1]),
            IsSoundDefaultReal(sound.coneOrientation[2])}, "锥体方向");
    }

    void CaptureSounds() {
        if (!inputs_.sounds) return;
        plan_.sounds.reserve(inputs_.sounds->entries.size());
        for (std::size_t i = 0; i < inputs_.sounds->entries.size(); ++i) {
            const auto& sound = inputs_.sounds->entries[i];
            CheckSound(sound, i);
            MapInitSound record{sound, i, sound.name.starts_with("gg_snd_")
                ? sound.name : "gg_snd_" + ConvertObjectName(sound.name), {}, {}};
            AddSymbol(record.symbol, sound.IsMusic() ? "string" : "sound", kSounds, i);
            if (!sound.IsMusic()) {
                auto [found, inserted] = soundFacts_.try_emplace(sound.filePath);
                auto& facts = found->second;
                if (inserted) {
                    if (inputs_.queries.soundLabel)
                        facts.label = ReadQuery([&] { return inputs_.queries.soundLabel(sound.filePath); }, "soundLabel", kSounds, i);
                    if (inputs_.queries.soundDuration)
                        facts.duration = ReadQuery([&] { return inputs_.queries.soundDuration(sound.filePath); }, "soundDuration", kSounds, i);
                    if (facts.label) CheckString(*facts.label, "声音标签", kSounds, i);
                    if (facts.duration && *facts.duration < 0)
                        Reject("MAP_INIT_SOUND_DURATION_INVALID", "资源查询返回了负的声音时长。", kSounds, i);
                }
                record.label = facts.label; record.duration = facts.duration;
            }
            plan_.sounds.push_back(std::move(record));
        }
    }

    void CaptureDoodads() {
        if (!inputs_.doodads) return;
        plan_.doodads.reserve(inputs_.doodads->doodads.size());
        for (std::size_t i = 0; i < inputs_.doodads->doodads.size(); ++i) {
            const auto& doodad = inputs_.doodads->doodads[i];
            CheckTransform(doodad, kDoodads, i);
            if (doodad.flags != 0 && doodad.flags != 2 && doodad.flags != 3
                && doodad.flags != 4 && doodad.flags != 6 && doodad.flags != 7)
                Reject("MAP_INIT_DOODAD_FLAGS_UNSUPPORTED", "DOO 创建标志未经初始化规则确认。", kDoodads, i);
            if (doodad.droppedItemTable != -1 || HasDropChoices(doodad.itemSets))
                Reject("MAP_INIT_DROP_TABLE_UNSUPPORTED", "尚未实现此 DOO 记录的物品掉落初始化。", kDoodads, i);
            MapInitDoodad record{doodad, i, {}, (doodad.flags & 2u) == 0};
            if (record.scripted) {
                CheckRawcode(doodad.rawcode, "rawcode", kDoodads, i);
                if (doodad.creationId < 0 || doodad.lifePercent != 100)
                    Reject("MAP_INIT_DESTRUCTABLE_INIT_UNSUPPORTED", "脚本化可破坏物创建编号或生命值尚不能可靠初始化。", kDoodads, i);
                record.symbol = NumberedSymbol("dest", doodad.rawcode, doodad.creationId, kDoodads, i);
                AddSymbol(record.symbol, "destructable", kDoodads, i);
                plan_.doodadCreationOrder.push_back(i);
            }
            plan_.doodads.push_back(std::move(record));
        }
        plan_.specialDoodads = inputs_.doodads->specialDoodads;
        for (std::size_t i = 0; i < plan_.specialDoodads.size(); ++i)
            CheckRawcode(plan_.specialDoodads[i].rawcode, "特殊装饰物 rawcode", kDoodads, i);
    }

    const UnitFacts& FactsFor(const MapData::UnitPlacement& unit, std::size_t index) {
        auto [found, inserted] = unitFacts_.try_emplace(unit.rawcode);
        if (!inserted) return found->second;
        const auto& queries = inputs_.queries;
        if (!queries.isItem)
            Reject("MAP_INIT_OBJECT_QUERY_MISSING", "缺少 isItem 查询，不能由 rawcode 猜测单位/物品。", kUnits, index);
        auto& facts = found->second;
        facts.item = ReadQuery([&] { return queries.isItem(unit.rawcode); }, "isItem", kUnits, index);
        if (!facts.item) {
            if (!queries.isBuilding || !queries.isHero || !queries.unitTeamColor)
                Reject("MAP_INIT_OBJECT_QUERY_MISSING", "缺少建筑、英雄或队伍颜色有效物编查询。", kUnits, index);
            facts.building = ReadQuery([&] { return queries.isBuilding(unit.rawcode); }, "isBuilding", kUnits, index);
            facts.hero = ReadQuery([&] { return queries.isHero(unit.rawcode); }, "isHero", kUnits, index);
            facts.teamColor = ReadQuery([&] { return queries.unitTeamColor(unit.rawcode); }, "unitTeamColor", kUnits, index);
            if (facts.teamColor < -1 || facts.teamColor > static_cast<int>(plan_.neutralPassive))
                Reject("MAP_INIT_TEAM_COLOR_INVALID", "类型队伍颜色超出来源档案范围。", kUnits, index);
        }
        return facts;
    }

    void CaptureAbilities(MapInitUnit& record) {
        const auto& placed = record.placement;
        if (!placed.abilities.empty() && !inputs_.queries.ability)
            Reject("MAP_INIT_ABILITY_QUERY_MISSING", "实例含技能设置，但没有有效技能元数据查询。", kUnits, record.sourceIndex);
        for (const auto& ability : placed.abilities) {
            CheckRawcode(ability.rawcode, "ability.rawcode", kUnits, record.sourceIndex);
            if (ability.level < 0)
                Reject("MAP_INIT_ABILITY_LEVEL_INVALID", "预放置技能等级为负。", kUnits, record.sourceIndex);
            auto [found, inserted] = abilityFacts_.try_emplace(ability.rawcode);
            if (inserted)
                found->second = ReadQuery([&] { return inputs_.queries.ability(ability.rawcode); }, "ability", kUnits, record.sourceIndex);
            const auto& effective = found->second;
            if (!effective.known)
                Reject("MAP_INIT_ABILITY_UNKNOWN", "无法确认预放置技能 " + ability.rawcode + " 的有效元数据。", kUnits, record.sourceIndex);
            CheckString(effective.orderOn, "自动施法开启命令", kUnits, record.sourceIndex);
            CheckString(effective.orderOff, "自动施法关闭命令", kUnits, record.sourceIndex);
            record.abilities.push_back({ability, effective});
        }
    }

    void CaptureUnits() {
        if (!inputs_.units) return;
        plan_.units.reserve(inputs_.units->units.size());
        for (std::size_t i = 0; i < inputs_.units->units.size(); ++i) {
            const auto& unit = inputs_.units->units[i];
            CheckTransform(unit, kUnits, i);
            CheckFinite({unit.targetAcquisition}, "主动攻击范围", kUnits, i);
            if (unit.flags >= 0x20 || unit.creationId < 0 || unit.owner < 0
                || static_cast<std::uint32_t>(unit.owner) > plan_.neutralPassive)
                Reject("MAP_INIT_UNIT_LAYOUT_INVALID", "实例标志、创建编号或所有者不属于明确的来源档案。", kUnits, i);
            if (!MapData::IsKnownFixedPlacementConfig(unit.random) || MapData::IsRandomPlacementMarker(unit.rawcode))
                Reject("MAP_INIT_RANDOM_PLACEMENT_UNSUPPORTED", "尚未实现该随机放置规则，不能创建固定类型代替随机结果。", kUnits, i);
            if (unit.droppedItemTable != -1 || HasDropChoices(unit.itemSets))
                Reject("MAP_INIT_DROP_TABLE_UNSUPPORTED", "尚未实现该单位的物品掉落初始化。", kUnits, i);
            MapInitUnit record;
            record.placement = unit; record.sourceIndex = i; record.startLocation = unit.rawcode == "sloc";
            if (!record.startLocation) {
                const auto& facts = FactsFor(unit, i);
                record.item = facts.item; record.building = facts.building;
                record.hero = facts.hero; record.typeTeamColor = facts.teamColor;
                CheckRawcode(unit.rawcode, "rawcode", kUnits, i);
                record.symbol = NumberedSymbol(record.item ? "item" : "unit", unit.rawcode, unit.creationId, kUnits, i);
                AddSymbol(record.symbol, record.item ? "item" : "unit", kUnits, i);
                if (!record.item) {
                    if (unit.teamColor < -1 || unit.teamColor > static_cast<int>(plan_.neutralPassive))
                        Reject("MAP_INIT_TEAM_COLOR_INVALID", "实例队伍颜色超出来源档案范围。", kUnits, i);
                    CaptureAbilities(record);
                    std::set<std::int32_t> inventorySlots;
                    for (const auto& item : unit.inventory) {
                        CheckRawcode(item.rawcode, "inventory.rawcode", kUnits, i);
                        if (item.slot < 0 || item.slot >= 6 || !inventorySlots.insert(item.slot).second)
                            Reject("MAP_INIT_INVENTORY_SLOT_INVALID", "物品栏槽位越界或重复。", kUnits, i);
                    }
                    if (unit.waygateRegionId != -1) {
                        const auto region = regionByCreation_.find(unit.waygateRegionId);
                        if (region == regionByCreation_.end())
                            Reject("MAP_INIT_WAYGATE_UNBOUND", "传送门目标没有唯一的实际区域记录。", kUnits, i);
                        record.waygateRegionSymbol = region->second;
                    }
                } else if (!unit.abilities.empty() || !unit.inventory.empty() || unit.waygateRegionId != -1) {
                    Reject("MAP_INIT_ITEM_FIELDS_UNSUPPORTED", "物品放置记录含单位专用初始化设置，不能静默忽略。", kUnits, i);
                }
            } else if (!unit.abilities.empty() || !unit.inventory.empty() || unit.waygateRegionId != -1) {
                Reject("MAP_INIT_START_LOCATION_FIELDS_UNSUPPORTED", "出生点含单位专用初始化设置。", kUnits, i);
            }
            plan_.units.push_back(std::move(record));
        }
    }

    void ArrangeCreation() {
        // 普通对象由实际成员全量捕获。sloc 和 flags&2、特殊DOO保留为归档驱动记录。
        for (std::size_t i = 0; i < plan_.units.size(); ++i) {
            if (!plan_.units[i].item && !plan_.units[i].startLocation) plan_.unitCreationOrder.push_back(i);
        }
        const auto rank = [&](const MapInitUnit& unit) {
            const auto owner = static_cast<std::uint32_t>(unit.placement.owner);
            const int group = unit.building ? 0 : 3;
            if (owner == plan_.neutralAggressive) return group;
            if (owner == plan_.neutralPassive) return group + 1;
            return group + 2;
        };
        std::stable_sort(plan_.unitCreationOrder.begin(), plan_.unitCreationOrder.end(), [&](std::size_t a, std::size_t b) {
            const auto& left = plan_.units[a]; const auto& right = plan_.units[b];
            return std::tuple{rank(left), left.placement.owner, left.placement.creationId}
                < std::tuple{rank(right), right.placement.owner, right.placement.creationId};
        });
        for (std::size_t i = plan_.units.size(); i != 0; --i) {
            if (plan_.units[i - 1].item) plan_.itemCreationOrder.push_back(i - 1);
        }
        std::stable_sort(plan_.itemCreationOrder.begin(), plan_.itemCreationOrder.end(), [&](std::size_t a, std::size_t b) {
            return plan_.units[a].placement.rawcode < plan_.units[b].placement.rawcode;
        });
        std::stable_sort(plan_.doodadCreationOrder.begin(), plan_.doodadCreationOrder.end(), [&](std::size_t a, std::size_t b) {
            return plan_.doodads[a].placement.rawcode < plan_.doodads[b].placement.rawcode;
        });
    }

    void ArrangeMain() {
        plan_.mainOrder = {MapInitStage::Sounds, MapInitStage::Regions};
        const auto add = [&](bool used, MapInitStage stage) { if (used) plan_.mainOrder.push_back(stage); };
        add(!plan_.cameras.empty(), MapInitStage::Cameras);
        add(!plan_.settings.custom->upgrades.empty(), MapInitStage::Upgrades);
        add(!plan_.settings.custom->techRestrictions.empty(), MapInitStage::TechTree);
        add(!plan_.doodadCreationOrder.empty(), MapInitStage::Destructables);
        add(!plan_.itemCreationOrder.empty(), MapInitStage::Items);
        for (const auto stage : {MapInitStage::Units, MapInitStage::Blizzard, MapInitStage::Globals,
            MapInitStage::CustomTriggers, MapInitStage::InitializationTriggers}) plan_.mainOrder.push_back(stage);
    }

    const MapScriptInputs& inputs_;
    const MapInitializationOptions& options_;
    MapInitializationPlan plan_;
    std::map<std::string, std::size_t> symbolSources_;
    std::map<std::int32_t, std::string> regionByCreation_;
    std::map<std::string, UnitFacts> unitFacts_;
    std::map<std::string, SoundFacts> soundFacts_;
    std::map<std::string, ObjectQueries::AbilityInfo> abilityFacts_;
};

} // namespace

Result<MapInitializationPlan> BuildMapInitializationPlan(const MapScriptInputs& inputs,
    const MapInitializationOptions& options) {
    try {
        // W3I在此处单独确认档位；其余复用现有经典布局门禁，不绕过SkinId或扩展记录。
        const auto layouts = MapData::ValidateClassicMapLayouts({inputs.units, inputs.doodads, inputs.cameras, inputs.sounds});
        if (!layouts.Succeeded()) return {{}, layouts.diagnostics};
        return Result<MapInitializationPlan>::Success(PlanBuilder(inputs, options).Build());
    } catch (const PlanFailure& failure) {
        return {{}, {failure.diagnostic}};
    } catch (const std::bad_alloc&) {
        return Result<MapInitializationPlan>::Failure("Script.MapInitialization", "MAP_INIT_ALLOCATION_FAILED",
            "捕获初始化计划时内存分配失败；未返回部分计划。");
    } catch (const std::length_error&) {
        return Result<MapInitializationPlan>::Failure("Script.MapInitialization", "MAP_INIT_CONTAINER_LIMIT",
            "初始化计划超出容器容量；未返回部分计划。");
    }
}

} // namespace War3Map::Script
