#include "MapSections.h"

#include "ScriptNames.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <map>
#include <numbers>
#include <optional>

namespace War3Map::Script::detail {
namespace {

using ItemChoices = std::vector<std::pair<std::string, std::int32_t>>;

constexpr std::int32_t kNeutralHostile = 12;
constexpr std::int32_t kNeutralPassive = 15;

std::string Numbered6(std::string_view prefix, std::int32_t number, std::string_view suffix) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%06d", number);
    return std::string(prefix) + buffer + std::string(suffix);
}

std::string RawcodeFromSettings(const MapData::SettingsRawcode& raw) {
    std::string text;
    for (const auto byte : raw) text += static_cast<char>(byte);
    return text;
}

bool EmptyRawcode(std::string_view rawcode) {
    return rawcode.empty() || rawcode == std::string_view("\0\0\0\0", 4);
}

// 物品组条目：空位写 -1；Y?I<等级> 为指定类别与等级的随机物品。
std::string ItemChoice(std::string_view rawcode) {
    return LegacyItemChoiceExpression(rawcode);
}

// 掉落物品函数：每个物品组独立随机一件，单位死亡或被中立敌对玩家转移时掉落。
void WriteDropFunction(ScriptText& out, const std::string& name, const std::vector<ItemChoices>& sets) {
    out.Line("function " + name + " takes nothing returns nothing");
    out.Begin();
    out.Line("local widget  trigWidget = null");
    out.Line("local unit    trigUnit   = null");
    out.Line("local integer itemID     = 0");
    out.Line("local boolean canDrop    = true");
    out.Line("");
    out.Line("set trigWidget = bj_lastDyingWidget");
    out.Line("if (trigWidget == null) then");
    out.Line("    set trigUnit = GetTriggerUnit()");
    out.Line("endif");
    out.Line("");
    out.Line("if (trigUnit != null) then");
    out.Line("    set canDrop = not IsUnitHidden(trigUnit)");
    out.Line("    if (canDrop and GetChangingUnit() != null) then");
    out.Line("        set canDrop = (GetChangingUnitPrevOwner() == Player(PLAYER_NEUTRAL_AGGRESSIVE))");
    out.Line("    endif");
    out.Line("endif");
    out.Line("");
    out.Line("if (canDrop) then");
    out.Begin();
    for (std::size_t set = 0; set < sets.size(); ++set) {
        out.Line("// Item set " + std::to_string(set));
        out.Line("call RandomDistReset(  )");
        std::int32_t total = 0;
        for (const auto& [rawcode, chance] : sets[set]) {
            // 0权重仍保留在W3I；不占BJ分布数组，避免大量零项耗尽经典数组槽。
            if (chance == 0) continue;
            out.Line("call RandomDistAddItem( " + ItemChoice(rawcode) + ", " + std::to_string(chance) + " )");
            total += chance;
        }
        // 概率不足 100 的部分为“不掉落”。
        if (total < 100) out.Line("call RandomDistAddItem( -1, " + std::to_string(100 - total) + " )");
        out.Line("set itemID = RandomDistChoose(  )");
        out.Line("if (trigUnit != null) then");
        out.Line("    call UnitDropItem( trigUnit, itemID )");
        out.Line("else");
        out.Line("    call WidgetDropItem( trigWidget, itemID )");
        out.Line("endif");
        out.Line("");
    }
    out.End();
    out.Line("endif");
    out.Line("");
    out.Line("set bj_lastDyingWidget = null");
    out.Line("call DestroyTrigger(GetTriggeringTrigger())");
    out.End();
    out.Line("endfunction");
    out.Line("");
}

std::string Bool(bool value) { return value ? "true" : "false"; }

// W3S uses 2^32 as the floating-point "default" sentinel (the integer fields use -1).
// It is a finite float, so generic finite-number checks cannot distinguish it from an override.
bool DefaultSoundReal(float value) { return value == 4294967296.0f; }

struct UnitBucket {
    std::string function;
    std::string player;
    std::vector<std::size_t> units;
};

std::string OwnerKey(std::int32_t owner) {
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "%02d", owner);
    return buffer;
}

std::string PlayerExpression(std::int32_t owner) {
    if (owner == kNeutralHostile) return "Player(PLAYER_NEUTRAL_AGGRESSIVE)";
    if (owner == kNeutralPassive) return "Player(PLAYER_NEUTRAL_PASSIVE)";
    return "Player(" + std::to_string(owner) + ")";
}

std::string BucketFunction(std::int32_t owner, bool building) {
    if (owner == kNeutralHostile) return building ? "CreateNeutralHostileBuildings" : "CreateNeutralHostile";
    if (owner == kNeutralPassive) return building ? "CreateNeutralPassiveBuildings" : "CreateNeutralPassive";
    return (building ? "CreateBuildingsForPlayer" : "CreateUnitsForPlayer") + std::to_string(owner);
}

// WE 按创建编号输出预放置对象；返回按 creationId 稳定排序的下标。
std::vector<std::size_t> CreationOrder(const std::vector<MapData::UnitPlacement>& units) {
    std::vector<std::size_t> order(units.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return units[a].creationId < units[b].creationId; });
    return order;
}

} // namespace

bool HasPlacedItems(const MapContext& context) {
    if (context.in.units == nullptr) return false;
    for (const auto& unit : context.in.units->units) {
        if (context.IsItem(unit.rawcode)) return true;
    }
    return false;
}

bool HasScriptedDestructables(const MapContext& context) { return !context.scriptedDoodadIndexes.empty(); }

bool HasDoodadDropChoices(const MapData::DoodadPlacement& doodad) {
    if (doodad.droppedItemTable != -1) return true;
    // 原版对空 itemSet 不生成掉落函数或死亡事件；需至少一个真实选项。
    return std::any_of(doodad.itemSets.begin(), doodad.itemSets.end(), [](const auto& set) { return !set.items.empty(); });
}

void WriteItemTables(MapContext& context) {
    auto& out = context.out;
    const auto* settings = context.in.settings;
    if (settings != nullptr && settings->custom && !settings->custom->randomItems.empty()) {
        WriteSectionBanner(out, "Map Item Tables");
        for (const auto& table : settings->custom->randomItems) {
            std::vector<ItemChoices> sets;
            for (const auto& set : table.sets) {
                auto& choices = sets.emplace_back();
                for (const auto& item : set) {
                    choices.emplace_back(RawcodeFromSettings(item.id), item.chance);
                }
            }
            WriteDropFunction(out, Numbered6("ItemTable", table.number, "_DropItems"), sets);
        }
        out.Line("");
    }
    if (context.in.units == nullptr) return;
    bool banner = false;
    const auto& units = context.in.units->units;
    for (std::size_t index = 0; index < units.size(); ++index) {
        const auto& unit = units[index];
        if (unit.itemSets.empty() || unit.droppedItemTable != -1) continue;
        if (!banner) {
            WriteSectionBanner(out, "Unit Item Tables");
            banner = true;
        }
        std::vector<ItemChoices> sets;
        for (const auto& set : unit.itemSets) {
            auto& choices = sets.emplace_back();
            for (const auto& item : set.items) choices.emplace_back(item.rawcode, item.chance);
        }
        WriteDropFunction(out, Numbered6("Unit", static_cast<std::int32_t>(index), "_DropItems"), sets);
    }
    if (banner) out.Line("");
}

void WriteDoodadTables(MapContext& context) {
    if (context.in.doodads == nullptr) return;
    bool banner = false;
    for (const auto index : context.scriptedDoodadIndexes) {
        const auto& doodad = context.in.doodads->doodads[index];
        if (!HasDoodadDropChoices(doodad) || doodad.droppedItemTable != -1) continue;
        if (!banner) {
            WriteSectionBanner(context.out, "Destructible Item Tables");
            banner = true;
        }
        std::vector<ItemChoices> sets;
        for (const auto& set : doodad.itemSets) {
            auto& choices = sets.emplace_back();
            for (const auto& item : set.items) choices.emplace_back(item.rawcode, item.chance);
        }
        WriteDropFunction(context.out, Numbered6("Doodad", static_cast<std::int32_t>(index), "_DropItems"), sets);
    }
    if (banner) context.out.Line("");
}

void WriteSounds(MapContext& context) {
    auto& out = context.out;
    WriteSectionBanner(out, "Sounds");
    out.Line("function InitSounds takes nothing returns nothing");
    out.Begin();
    if (context.in.sounds != nullptr) {
        const auto& queries = context.in.queries;
        for (const auto& sound : context.in.sounds->entries) {
            const std::string name = sound.name.rfind("gg_snd_", 0) == 0 ? sound.name : "gg_snd_" + ConvertObjectName(sound.name);
            const std::string path = QuoteJassString(sound.filePath);
            if (sound.IsMusic()) {
                out.Line("set " + name + " = " + path);
                continue;
            }
            const auto defaultReal = [](float value) { return DefaultSoundReal(value); };
            const bool defaultSpatial = defaultReal(sound.minDistance) || defaultReal(sound.maxDistance)
                || defaultReal(sound.distanceCutoff) || defaultReal(sound.coneAngleInside) || defaultReal(sound.coneAngleOutside)
                || sound.coneOutsideVolume == -1 || std::any_of(sound.coneOrientation.begin(), sound.coneOrientation.end(), defaultReal);
            const bool usesDefaults = sound.volume == -1 || sound.channel == -1 || sound.priority == -1
                || defaultReal(sound.pitch) || defaultReal(sound.pitchVariance) || defaultSpatial;
            for (const float value : {sound.pitch, sound.pitchVariance, sound.minDistance, sound.maxDistance, sound.distanceCutoff,
                    sound.coneAngleInside, sound.coneAngleOutside, sound.coneOrientation[0], sound.coneOrientation[1], sound.coneOrientation[2]}) {
                if (std::isfinite(value)) continue;
                context.diagnostics.push_back({DiagnosticSeverity::Error, "Script.Map", "SOUND_PARAMETER_NONFINITE",
                    "声音 " + name + " 含非有限浮点参数，拒绝脚本重建", {}});
                break;
            }
            const auto uniformDefaults = [&](std::initializer_list<bool> fields, std::string_view group) -> std::optional<bool> {
                const bool first = *fields.begin();
                if (std::all_of(fields.begin(), fields.end(), [first](bool value) { return value == first; })) return first;
                context.diagnostics.push_back({DiagnosticSeverity::Error, "Script.Map", "SOUND_DEFAULT_GROUP_MIXED",
                    "声音 " + name + " 的" + std::string(group) + "同时含默认哨兵和显式值，无法安全生成一条 JASS 设置语句", {}});
                return std::nullopt;
            };
            out.Line("set " + name + " = CreateSound( " + path + ", " + Bool(sound.IsLooping()) + ", " + Bool(sound.Is3D()) + ", " +
                Bool(sound.StopsOutOfRange()) + ", " + std::to_string(sound.fadeInRate) + ", " +
                std::to_string(sound.fadeOutRate) + ", " + QuoteJassString(sound.eaxSetting) + " )");
            const auto duration = queries.soundDuration ? queries.soundDuration(sound.filePath) : std::nullopt;
            if (usesDefaults) {
                if (const auto label = queries.soundLabel ? queries.soundLabel(sound.filePath) : std::nullopt) {
                    out.Line("call SetSoundParamsFromLabel( " + name + ", " + QuoteJassString(*label) + " )");
                }
            }
            if (duration) out.Line("call SetSoundDuration( " + name + ", " + std::to_string(*duration) + " )");
            if (sound.channel != -1) out.Line("call SetSoundChannel( " + name + ", " + std::to_string(sound.channel) + " )");
            if (sound.volume != -1) out.Line("call SetSoundVolume( " + name + ", " + std::to_string(sound.volume) + " )");
            if (!defaultReal(sound.pitch)) out.Line("call SetSoundPitch( " + name + ", " + Real1(sound.pitch) + " )");
            if (sound.Is3D()) {
                const auto distances = uniformDefaults({defaultReal(sound.minDistance), defaultReal(sound.maxDistance)}, "距离参数");
                if (distances && !*distances) out.Line("call SetSoundDistances( " + name + ", " + Real1(sound.minDistance) + ", " + Real1(sound.maxDistance) + " )");
                if (!defaultReal(sound.distanceCutoff)) out.Line("call SetSoundDistanceCutoff( " + name + ", " + Real1(sound.distanceCutoff) + " )");
                const auto angles = uniformDefaults({defaultReal(sound.coneAngleInside), defaultReal(sound.coneAngleOutside),
                    sound.coneOutsideVolume == -1}, "锥体角度参数");
                if (angles && !*angles) out.Line("call SetSoundConeAngles( " + name + ", " + Real1(sound.coneAngleInside) + ", " +
                    Real1(sound.coneAngleOutside) + ", " + std::to_string(sound.coneOutsideVolume) + " )");
                const auto orientation = uniformDefaults({defaultReal(sound.coneOrientation[0]), defaultReal(sound.coneOrientation[1]),
                    defaultReal(sound.coneOrientation[2])}, "锥体朝向参数");
                if (orientation && !*orientation) out.Line("call SetSoundConeOrientation( " + name + ", " + Real1(sound.coneOrientation[0]) + ", " +
                    Real1(sound.coneOrientation[1]) + ", " + Real1(sound.coneOrientation[2]) + " )");
            }
        }
    }
    out.End();
    out.Line("endfunction");
    out.Line("");
}

void WriteDestructables(MapContext& context) {
    if (!HasScriptedDestructables(context)) return;
    auto& out = context.out;
    WriteSectionBanner(out, "Destructable Objects");
    out.Line("function CreateAllDestructables takes nothing returns nothing");
    out.Begin();
    out.Line("local destructable d");
    out.Line("local trigger t");
    out.Line("local real life");
    std::vector<std::size_t> ordered = context.scriptedDoodadIndexes;
    const auto& doodads = context.in.doodads->doodads;
    std::stable_sort(ordered.begin(), ordered.end(), [&](std::size_t a, std::size_t b) {
        return doodads[a].rawcode < doodads[b].rawcode;
    });
    for (const auto index : ordered) {
        const auto& doodad = doodads[index];
        float facing = doodad.rotation * (180.0f / std::numbers::pi_v<float>);
        if (facing < 0) facing += 360.0f;
        const bool z = doodad.flags == 4;
        std::string call = z ? "CreateDestructableZ( " : "CreateDestructable( ";
        call += Rawcode(doodad.rawcode) + ", " + Real1(doodad.position[0]) + ", " + Real1(doodad.position[1]) + ", ";
        if (z) call += Real1(doodad.position[2]) + ", ";
        call += Real3(facing) + ", " + Real3(doodad.scale[0]) + ", " + std::to_string(doodad.variation) + " )";
        const std::string global = context.DoodadVariable(doodad);
        const std::string variable = context.referencedObjects.contains(global) ? global : "d";
        out.Line("set " + variable + " = " + call);
        if (HasDoodadDropChoices(doodad)) {
            out.Line("set t = CreateTrigger(  )");
            out.Line("call TriggerRegisterDeathEvent( t, " + variable + " )");
            out.Line("call TriggerAddAction( t, function SaveDyingWidget )");
            const auto drop = doodad.droppedItemTable != -1 ? Numbered6("ItemTable", doodad.droppedItemTable, "_DropItems")
                : Numbered6("Doodad", static_cast<std::int32_t>(index), "_DropItems");
            out.Line("call TriggerAddAction( t, function " + drop + " )");
        }
    }
    out.End();
    out.Line("endfunction");
    out.Line("");
}

void WriteItems(MapContext& context) {
    if (!HasPlacedItems(context)) return;
    auto& out = context.out;
    WriteSectionBanner(out, "Items");
    out.Line("function CreateAllItems takes nothing returns nothing");
    out.Begin();
    out.Line("local integer itemID");
    out.Line("");
    // 与 WE 输出一致：按物品类型分组（rawcode 升序），同类按文件中的逆序创建。
    std::vector<const MapData::UnitPlacement*> items;
    const auto& placed = context.in.units->units;
    for (std::size_t reverse = placed.size(); reverse-- > 0;) {
        if (context.IsItem(placed[reverse].rawcode)) items.push_back(&placed[reverse]);
    }
    std::stable_sort(items.begin(), items.end(), [](const auto* a, const auto* b) { return a->rawcode < b->rawcode; });
    for (const auto* item : items) {
        const auto& unit = *item;
        const auto index = static_cast<std::size_t>(item - placed.data());
        const auto random = context.random.placements.find(index);
        const bool dynamic = random != context.random.placements.end();
        if (dynamic) {
            out.Line("set itemID = " + random->second.typeExpression);
            out.Line("if (itemID != -1 and itemID != 0) then"); out.Begin();
        }
        const std::string call = "CreateItem( " + (dynamic ? std::string("itemID") : Rawcode(unit.rawcode)) + ", " + Real1(unit.position[0]) + ", " + Real1(unit.position[1]) + " )";
        const std::string variable = context.PlacedVariable("gg_item_", unit);
        out.Line(context.referencedObjects.contains(variable) ? "set " + variable + " = " + call : "call " + call);
        if (dynamic) { out.End(); out.Line("endif"); }
    }
    out.End();
    out.Line("endfunction");
    out.Line("");
}

namespace {

void WriteUnit(MapContext& context, const MapData::UnitPlacement& unit, std::size_t index) {
    auto& out = context.out;
    const std::string global = context.PlacedVariable("gg_unit_", unit);
    const std::string v = context.referencedObjects.contains(global) ? global : "u";
    const auto random = context.random.placements.find(index);
    const bool dynamic = random != context.random.placements.end();
    if (dynamic) {
        out.Line("set unitID = " + random->second.typeExpression);
        out.Line("if (unitID != -1 and unitID != 0) then"); out.Begin();
    }
    // WE 以单精度换算角度（影响第三位小数的舍入）。
    float facing = unit.rotation * (180.0f / std::numbers::pi_v<float>);
    if (facing < 0) facing += 360.0f;
    if (dynamic) {
        out.Line("if (unitID == 'ugol') then"); out.Begin();
        out.Line("set " + v + " = CreateBlightedGoldmine( p, " + Real1(unit.position[0]) + ", " + Real1(unit.position[1]) + ", " + Real3(facing) + " )");
        out.End(); out.Line("else"); out.Begin();
        out.Line("set " + v + " = CreateUnit( p, unitID, " + Real1(unit.position[0]) + ", " + Real1(unit.position[1]) + ", " + Real3(facing) + " )");
        out.End(); out.Line("endif");
    } else if (unit.rawcode == "ugol") {
        out.Line("set " + v + " = CreateBlightedGoldmine( p, " + Real1(unit.position[0]) + ", " + Real1(unit.position[1]) + ", " + Real3(facing) + " )");
    } else {
        out.Line("set " + v + " = CreateUnit( p, " + Rawcode(unit.rawcode) + ", " + Real1(unit.position[0]) + ", " +
            Real1(unit.position[1]) + ", " + Real3(facing) + " )");
    }
    if (unit.hitPoints != -1) {
        char percent[32];
        std::snprintf(percent, sizeof(percent), "%.2f", unit.hitPoints / 100.0);
        out.Line("set life = GetUnitState( " + v + ", UNIT_STATE_LIFE )");
        out.Line("call SetUnitState( " + v + ", UNIT_STATE_LIFE, " + percent + " * life )");
    }
    // 12500 是常见实例值，并不是“默认”的哨兵；WE 对金矿总是显式设置储量。
    if (dynamic) {
        out.Line("if (unitID == 'ngol' or unitID == 'ugol') then"); out.Begin();
        out.Line("call SetResourceAmount( " + v + ", " + std::to_string(unit.gold) + " )"); out.End(); out.Line("endif");
    } else if (unit.rawcode == "ngol" || unit.rawcode == "ugol") {
        out.Line("call SetResourceAmount( " + v + ", " + std::to_string(unit.gold) + " )");
    }
    const bool hero = dynamic || context.IsHero(unit.rawcode);
    const bool heroFields = unit.heroLevel > 1 || unit.heroStrength > 0 || unit.heroAgility > 0 || unit.heroIntelligence > 0;
    if (dynamic && heroFields) { out.Line("if (IsUnitType( " + v + ", UNIT_TYPE_HERO )) then"); out.Begin(); }
    if (hero && unit.heroLevel > 1) out.Line("call SetHeroLevel( " + v + ", " + std::to_string(unit.heroLevel) + ", false )");
    if (hero && unit.heroStrength > 0) out.Line("call SetHeroStr( " + v + ", " + std::to_string(unit.heroStrength) + ", true )");
    if (hero && unit.heroAgility > 0) out.Line("call SetHeroAgi( " + v + ", " + std::to_string(unit.heroAgility) + ", true )");
    if (hero && unit.heroIntelligence > 0) out.Line("call SetHeroInt( " + v + ", " + std::to_string(unit.heroIntelligence) + ", true )");
    if (dynamic && heroFields) { out.End(); out.Line("endif"); }
    if (unit.mana != -1) out.Line("call SetUnitState( " + v + ", UNIT_STATE_MANA, " + std::to_string(unit.mana) + " )");
    // 放置时指定的颜色优先；否则单位类型自带颜色时写类型颜色。
    const int typeColor = !dynamic && context.in.queries.unitTeamColor ? context.in.queries.unitTeamColor(unit.rawcode) : -1;
    if (unit.teamColor != -1) out.Line("call SetUnitColor( " + v + ", ConvertPlayerColor(" + std::to_string(unit.teamColor) + ") )");
    else if (typeColor != -1) out.Line("call SetUnitColor( " + v + ", ConvertPlayerColor(" + std::to_string(typeColor) + ") )");
    else if (dynamic) for (const auto& [id, color] : random->second.typeColors) {
        out.Line("if (unitID == " + *RandomChoiceExpression(id, random->second.kind) + ") then"); out.Begin();
        out.Line("call SetUnitColor( " + v + ", ConvertPlayerColor(" + std::to_string(color) + ") )"); out.End(); out.Line("endif");
    }
    if (unit.targetAcquisition == -2.0f) out.Line("call SetUnitAcquireRange( " + v + ", 200.0 )");
    else if (unit.targetAcquisition >= 0.0f) out.Line("call SetUnitAcquireRange( " + v + ", " + Real1(unit.targetAcquisition) + " )");
    for (std::size_t abilityIndex = 0; abilityIndex < unit.abilities.size(); ++abilityIndex) {
        const auto& ability = unit.abilities[abilityIndex];
        ObjectQueries::AbilityInfo info;
        if (dynamic) info = random->second.abilities.at(abilityIndex);
        else if (context.in.queries.ability) info = context.in.queries.ability(ability.rawcode);
        else info.autocast = ability.autocast;
        // 非英雄单位上的英雄技能在游戏中无效，WE 不写任何语句。
        if (!info.known || (info.hero && !hero)) continue;
        if (dynamic && info.hero) { out.Line("if (IsUnitType( " + v + ", UNIT_TYPE_HERO )) then"); out.Begin(); }
        const bool selected = hero && info.hero && ability.level > 0;
        for (std::int32_t level = 0; selected && level < ability.level; ++level) {
            out.Line("call SelectHeroSkill( " + v + ", " + Rawcode(ability.rawcode) + " )");
        }
        // 放置时开启了自动施法、技能本身可自动施法（写关闭命令），或学习了自定义英雄技能（WE 同样写一次命令）。
        const std::string& order = ability.autocast ? info.orderOn : info.orderOff;
        if (ability.autocast || info.autocast || (info.custom && selected)) {
            out.Line("call IssueImmediateOrder( " + v + ", " + QuoteJassString(order) + " )");
        }
        if (dynamic && info.hero) { out.End(); out.Line("endif"); }
    }
    for (const auto& item : unit.inventory) {
        out.Line("call UnitAddItemToSlotById( " + v + ", " + Rawcode(item.rawcode) + ", " + std::to_string(item.slot) + " )");
    }
    if (unit.waygateRegionId != -1) {
        const std::string region = context.RegionByCreation(unit.waygateRegionId);
        if (!region.empty()) {
            out.Line("call WaygateSetDestination( " + v + ", GetRectCenterX(" + region + "), GetRectCenterY(" + region + ") )");
            out.Line("call WaygateActivate( " + v + ", true )");
        }
    }
    std::string drop;
    if (unit.droppedItemTable != -1) drop = Numbered6("ItemTable", unit.droppedItemTable, "_DropItems");
    else if (!unit.itemSets.empty()) drop = Numbered6("Unit", static_cast<std::int32_t>(index), "_DropItems");
    if (!drop.empty()) {
        out.Line("set t = CreateTrigger(  )");
        out.Line("call TriggerRegisterUnitEvent( t, " + v + ", EVENT_UNIT_DEATH )");
        out.Line("call TriggerRegisterUnitEvent( t, " + v + ", EVENT_UNIT_CHANGE_OWNER )");
        out.Line("call TriggerAddAction( t, function " + drop + " )");
    }
    if (dynamic) { out.End(); out.Line("endif"); }
}

} // namespace

void WriteUnits(MapContext& context) {
    auto& out = context.out;
    WriteSectionBanner(out, "Unit Creation");
    std::map<std::string, UnitBucket> buckets;
    if (context.in.units != nullptr) {
        const auto& units = context.in.units->units;
        for (const auto index : CreationOrder(units)) {
            const auto& unit = units[index];
            if (unit.rawcode == "sloc" || context.IsItem(unit.rawcode)) continue;
            const bool building = context.IsBuilding(unit.rawcode);
            auto& bucket = buckets[OwnerKey(unit.owner) + (building ? "0" : "1")];
            bucket.function = BucketFunction(unit.owner, building);
            bucket.player = PlayerExpression(unit.owner);
            bucket.units.push_back(index);
        }
    }
    const auto writeBucket = [&](const UnitBucket& bucket) {
        out.Line("//===========================================================================");
        out.Line("function " + bucket.function + " takes nothing returns nothing");
        out.Begin();
        out.Line("local player p = " + bucket.player);
        out.Line("local unit u");
        out.Line("local integer unitID");
        out.Line("local trigger t");
        out.Line("local real life");
        out.Line("");
        for (const auto index : bucket.units) WriteUnit(context, context.in.units->units[index], index);
        out.End();
        out.Line("endfunction");
        out.Line("");
    };
    std::vector<std::string> playerBuildings;
    std::vector<std::string> playerUnits;
    for (const auto& [key, bucket] : buckets) {
        writeBucket(bucket);
        const std::string owner = key.substr(0, 2);
        const bool neutral = owner == OwnerKey(kNeutralHostile) || owner == OwnerKey(kNeutralPassive);
        if (!neutral) (key.back() == '0' ? playerBuildings : playerUnits).push_back(bucket.function);
    }
    const auto aggregate = [&](const std::string& name, const std::vector<std::string>& calls) {
        out.Line("//===========================================================================");
        out.Line("function " + name + " takes nothing returns nothing");
        out.Begin();
        for (const auto& call : calls) out.Line("call " + call + "(  )");
        out.End();
        out.Line("endfunction");
        out.Line("");
    };
    aggregate("CreatePlayerBuildings", playerBuildings);
    aggregate("CreatePlayerUnits", playerUnits);
    std::vector<std::string> all;
    const auto has = [&](const std::string& key) { return buckets.contains(key); };
    if (has(OwnerKey(kNeutralHostile) + "0")) all.push_back("CreateNeutralHostileBuildings");
    if (has(OwnerKey(kNeutralPassive) + "0")) all.push_back("CreateNeutralPassiveBuildings");
    all.push_back("CreatePlayerBuildings");
    if (has(OwnerKey(kNeutralHostile) + "1")) all.push_back("CreateNeutralHostile");
    if (has(OwnerKey(kNeutralPassive) + "1")) all.push_back("CreateNeutralPassive");
    all.push_back("CreatePlayerUnits");
    aggregate("CreateAllUnits", all);
}

void WriteRegions(MapContext& context) {
    auto& out = context.out;
    WriteSectionBanner(out, "Regions");
    out.Line("function CreateRegions takes nothing returns nothing");
    out.Begin();
    out.Line("local weathereffect we");
    out.Line("");
    if (context.in.regions != nullptr) {
        for (const auto& region : context.in.regions->entries) {
            const auto sound = MapData::ValidateRegionAmbientSound(region, context.in.sounds);
            if (!sound.Succeeded()) {
                context.diagnostics.insert(context.diagnostics.end(), sound.diagnostics.begin(), sound.diagnostics.end());
                continue;
            }
            const std::string name = context.RegionVariable(region.name);
            out.Line("set " + name + " = Rect( " + Real1(region.left) + ", " + Real1(region.bottom) + ", " +
                Real1(region.right) + ", " + Real1(region.top) + " )");
            std::string weather;
            for (const auto byte : region.weatherId) weather += static_cast<char>(byte);
            if (!EmptyRawcode(weather)) {
                out.Line("set we = AddWeatherEffect( " + name + ", " + Rawcode(weather) + " )");
                out.Line("call EnableWeatherEffect( we, true )");
            }
            if (!region.ambientSound.empty()) {
                out.Line("call SetSoundPosition( " + region.ambientSound + ", "
                    + Real1((double(region.left) + region.right) / 2.0) + ", "
                    + Real1((double(region.bottom) + region.top) / 2.0) + ", 0.0 )");
                out.Line("call RegisterStackedSound( " + region.ambientSound + ", true, "
                    + Real1(double(region.right) - region.left) + ", " + Real1(double(region.top) - region.bottom) + " )");
            }
        }
    }
    out.End();
    out.Line("endfunction");
    out.Line("");
}

void WriteCameras(MapContext& context) {
    if (context.in.cameras == nullptr || context.in.cameras->entries.empty()) return;
    auto& out = context.out;
    WriteSectionBanner(out, "Cameras");
    out.Line("function CreateCameras takes nothing returns nothing");
    out.Begin();
    out.Line("");
    for (const auto& camera : context.in.cameras->entries) {
        const std::string name = "gg_cam_" + ConvertObjectName(camera.name);
        out.Line("set " + name + " = CreateCameraSetup(  )");
        const auto field = [&](const char* id, float value) {
            out.Line("call CameraSetupSetField( " + name + ", " + id + ", " + Real1(value) + ", 0.0 )");
        };
        field("CAMERA_FIELD_ZOFFSET", camera.zOffset);
        field("CAMERA_FIELD_ROTATION", camera.rotation);
        field("CAMERA_FIELD_ANGLE_OF_ATTACK", camera.angleOfAttack);
        field("CAMERA_FIELD_TARGET_DISTANCE", camera.targetDistance);
        field("CAMERA_FIELD_ROLL", camera.roll);
        field("CAMERA_FIELD_FIELD_OF_VIEW", camera.fieldOfView);
        field("CAMERA_FIELD_FARZ", camera.farClippingPlane);
        out.Line("call CameraSetupSetDestPosition( " + name + ", " + Real1(camera.targetX) + ", " + Real1(camera.targetY) + ", 0.0 )");
        out.Line("");
    }
    out.End();
    out.Line("endfunction");
    out.Line("");
}

} // namespace War3Map::Script::detail
