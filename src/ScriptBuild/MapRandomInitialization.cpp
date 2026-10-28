#include "MapRandomInitialization.h"
#include "MapSections.h"
#include "War3MapCore/MapData/PlacementInitializationRules.h"
#include <algorithm>
#include <cstdio>
#include <limits>
#include <cmath>

namespace War3Map::Script {
bool RandomMapInitializationAvailable() noexcept {
    return true;
}
}
namespace War3Map::Script::detail {
namespace {
std::string Raw(const MapData::SettingsRawcode& id) { return {reinterpret_cast<const char*>(id.data()), 4}; }
bool Empty(std::string_view id) { return id == std::string_view("\0\0\0\0", 4); }
bool IsPseudo(std::string_view id) { return id.size() == 4 && id[0] == 'Y' && (id[2] == 'I' || id[2] == 'U' || id[2] == 'B'); }
Status Bad(const char* code, std::string message) { return Status::Failure("Script.RandomInitialization", code, std::move(message)); }
std::optional<std::string> ItemPseudoExpression(std::string_view id) {
    if (id.size() != 4 || id[0] != 'Y' || id[2] != 'I') return {};
    constexpr std::pair<char, const char*> classes[] = {{'Y', "ITEM_TYPE_ANY"}, {'i', "ITEM_TYPE_PERMANENT"},
        {'j', "ITEM_TYPE_CHARGED"}, {'k', "ITEM_TYPE_POWERUP"}, {'l', "ITEM_TYPE_ARTIFACT"},
        {'m', "ITEM_TYPE_PURCHASABLE"}, {'n', "ITEM_TYPE_CAMPAIGN"}, {'o', "ITEM_TYPE_MISCELLANEOUS"}};
    for (const auto& [code, type] : classes) if (id[1] == code)
        return std::string("ChooseRandomItemEx( ") + type + ", " + std::to_string(id[3] - '0') + " )";
    return {};
}
Status CheckChoice(const MapScriptInputs& inputs, std::string_view id, std::int32_t kind,
    std::map<std::string, std::int32_t>* colors = nullptr) {
    if (!RandomChoiceExpression(id, kind)) return Bad("MAP_RANDOM_CHOICE_UNSUPPORTED", "随机选择项无法按已知类别和原始四字节安全表达。");
    if (Empty(id)) return Status::Ok();
    if (IsPseudo(id)) return Status::Ok();
    if (!inputs.queries.placement) return Bad("MAP_RANDOM_OBJECT_SOURCE_REQUIRED", "随机初始化需要正向证明的物编类别，不能用未知对象的默认值。");
    std::optional<ObjectQueries::PlacementInfo> facts;
    try { facts = inputs.queries.placement(id); }
    catch (const std::bad_alloc&) { throw; }
    catch (...) { return Bad("MAP_RANDOM_OBJECT_QUERY_FAILED", "随机初始化物编查询失败，未借用默认类别。"); }
    if (!facts || facts->kind != kind || facts->teamColor < -1 || facts->teamColor > 15)
        return Bad("MAP_RANDOM_OBJECT_CLASS_UNVERIFIED", "随机表选择项不存在、类别不符或其初始化字段无法证明。");
    if (colors && kind != 2 && facts->teamColor != -1) colors->emplace(std::string(id), facts->teamColor);
    return Status::Ok();
}
} // namespace

std::string RandomGroupSymbol(std::int32_t number) {
    char digits[32]; std::snprintf(digits, sizeof(digits), "%03d", number); return "gg_rg_" + std::string(digits);
}
std::string LegacyItemChoiceExpression(std::string_view id) {
    if (id.empty() || Empty(id)) return "-1";
    if (auto value = ItemPseudoExpression(id)) return *value;
    return Rawcode(id);
}
std::optional<std::string> RandomChoiceExpression(std::string_view id, std::int32_t kind) {
    if (id.size() != 4 || kind < 0 || kind > 2) return {};
    if (Empty(id)) return "-1";
    if (IsPseudo(id)) {
        const auto level = static_cast<unsigned char>(id[3]);
        if (level < '/' || level > '~') return {};
        if (kind == 0 && id[0] == 'Y' && id[1] == 'Y' && id[2] == 'U')
            return "ChooseRandomCreep( " + std::to_string(level - '0') + " )";
        if (kind == 2) return ItemPseudoExpression(id);
        return {}; // 未经验证的随机建筑/类别编码继续只读，不能把伪ID当对象rawcode。
    }
    return Rawcode(id);
}
Result<MapRandomInitialization> CaptureMapRandomInitialization(const MapScriptInputs& inputs) {
    using R = Result<MapRandomInitialization>;
    const auto fail = [](Status status) -> R { return {{}, std::move(status.diagnostics)}; };
    MapRandomInitialization result;
    const auto* custom = inputs.settings && inputs.settings->custom ? &*inputs.settings->custom : nullptr;
    if (custom && (!custom->randomUnits.empty() || !custom->randomItems.empty()) && inputs.settings->formatVersion != 25)
        return fail(Bad("MAP_RANDOM_LAYOUT_UNSUPPORTED", "随机初始化目前仅资格化经典v25 JASS目标。"));
    std::size_t cells = 0;
    if (custom) {
        if (custom->randomUnits.size() > 4096 || custom->randomItems.size() > 4096) return fail(Bad("MAP_RANDOM_TABLE_BUDGET", "随机表数量超过初始化预算。"));
        for (const auto& table : custom->randomUnits) {
            result.hasRandomInitialization = true;
            if (table.number < 0 || !result.unitTables.insert(table.number).second) return fail(Bad("MAP_RANDOM_TABLE_NUMBER_INVALID", "随机单位表编号非法或重复。"));
            if (table.columnTypes.size() > 64 || table.rows.size() > 4096) return fail(Bad("MAP_RANDOM_TABLE_BUDGET", "随机单位表行列超过初始化预算。"));
            for (const auto kind : table.columnTypes) if (kind < 0 || kind > 2) return fail(Bad("MAP_RANDOM_COLUMN_UNSUPPORTED", "随机表列类别未知。"));
            std::uint64_t total = 0;
            for (const auto& row : table.rows) {
                if (row.chance < 0 || row.chance > 100 || row.ids.size() != table.columnTypes.size()) return fail(Bad("MAP_RANDOM_ROW_INVALID", "随机单位表概率或行列数量不合法。"));
                if (row.ids.size() > 16384 - cells) return fail(Bad("MAP_RANDOM_CELL_BUDGET", "随机初始化总单元数超过16384。"));
                cells += row.ids.size(); total += row.chance;
                for (std::size_t c = 0; c < row.ids.size(); ++c) { auto checked = CheckChoice(inputs, Raw(row.ids[c]), table.columnTypes[c]); if (!checked.Succeeded()) return fail(checked); }
            }
            if (!total) return fail(Bad("MAP_RANDOM_WEIGHT_EMPTY", "随机单位表权重总和为0，不能随机选择空范围。"));
        }
        for (const auto& table : custom->randomItems) {
            result.hasRandomInitialization = true;
            if (table.number < 0 || !result.itemTables.insert(table.number).second) return fail(Bad("MAP_RANDOM_TABLE_NUMBER_INVALID", "随机物品表编号非法或重复。"));
            if (table.sets.size() > 4096) return fail(Bad("MAP_RANDOM_TABLE_BUDGET", "随机物品组数超过初始化预算。"));
            for (const auto& set : table.sets) {
                if (set.size() > 16384 - cells) return fail(Bad("MAP_RANDOM_CELL_BUDGET", "随机初始化总单元数超过16384。"));
                cells += set.size(); std::uint64_t total = 0;
                for (const auto& item : set) {
                    if (item.chance < 0 || item.chance > 100) return fail(Bad("MAP_RANDOM_ITEM_CHANCE_INVALID", "随机物品概率超出0..100。"));
                    total += item.chance; auto checked = CheckChoice(inputs, Raw(item.id), 2); if (!checked.Succeeded()) return fail(checked);
                }
                if (total > 100) return fail(Bad("MAP_RANDOM_ITEM_CHANCE_SUM", "随机物品组概率总和超过100。"));
            }
        }
    }
    const auto drop = [&](std::int32_t number) { return number == -1 || result.itemTables.contains(number); };
    std::size_t abilityWork = 0;
    if (inputs.units) for (std::size_t i = 0; i < inputs.units->units.size(); ++i) {
        const auto& unit = inputs.units->units[i];
        if (!drop(unit.droppedItemTable)) return fail(Bad("MAP_RANDOM_DROP_TABLE_MISSING", "单位引用的全局物品表不存在。"));
        const bool marker = MapData::IsRandomPlacementMarker(unit.rawcode);
        const auto* global = std::get_if<MapData::RandomGlobalTable>(&unit.random);
        if (!marker && MapData::IsKnownFixedPlacementConfig(unit.random)) {
            if (unit.droppedItemTable != -1) {
                std::optional<ObjectQueries::PlacementInfo> facts;
                try { if (inputs.queries.placement) facts = inputs.queries.placement(unit.rawcode); }
                catch (...) { return fail(Bad("MAP_RANDOM_OBJECT_QUERY_FAILED", "全局掉落的单位类别查询失败。")); }
                if (!facts || (facts->kind != 0 && facts->kind != 1)) return fail(Bad("MAP_RANDOM_DROP_UNIT_UNVERIFIED", "全局掉落须绑定实际单位或建筑，不能给物品或未知对象装单位事件。"));
            }
            continue;
        }
        result.hasRandomInitialization = true;
        if (!marker || !global || !custom) return fail(Bad("MAP_RANDOM_PLACEMENT_UNSUPPORTED", "仅开放有明确占位类别的全局随机表放置；其它随机模式继续拒绝。"));
        const auto found = std::find_if(custom->randomUnits.begin(), custom->randomUnits.end(), [&](const auto& t) { return t.number == global->tableId; });
        if (found == custom->randomUnits.end() || global->column < 0 || static_cast<std::size_t>(global->column) >= found->columnTypes.size())
            return fail(Bad("MAP_RANDOM_PLACEMENT_REFERENCE_INVALID", "随机放置表编号不存在或列越界。"));
        RandomPlacedObject placed; placed.kind = unit.rawcode == "iDNR" ? 2 : unit.rawcode == "bDNR" ? 1 : 0;
        if (placed.kind != found->columnTypes[global->column]) return fail(Bad("MAP_RANDOM_PLACEMENT_CLASS_MISMATCH", "随机占位类别与表列不符。"));
        if (placed.kind == 2 && (!unit.abilities.empty() || !unit.inventory.empty() || !unit.itemSets.empty() || unit.waygateRegionId != -1 || unit.droppedItemTable != -1))
            return fail(Bad("MAP_RANDOM_ITEM_UNIT_FIELDS", "随机物品携带单位初始化字段，不能静默忽略。"));
        if (!std::all_of(unit.position.begin(), unit.position.end(), [](float v) { return std::isfinite(v); }) || !std::isfinite(unit.rotation)
            || !std::all_of(unit.scale.begin(), unit.scale.end(), [](float v) { return std::isfinite(v); }) || !std::isfinite(unit.targetAcquisition)
            || unit.skinId || unit.creationId < 0 || unit.owner < 0 || unit.owner > 15 || unit.flags >= 0x20 || unit.teamColor < -1 || unit.teamColor > 15)
            return fail(Bad("MAP_RANDOM_PLACEMENT_FIELDS_UNSUPPORTED", "随机实例变换、创建编号、颜色或外观字段无法初始化。"));
        std::set<std::int32_t> slots;
        for (const auto& item : unit.inventory) {
            if (item.slot < 0 || item.slot >= 6 || !slots.insert(item.slot).second || Empty(item.rawcode) || IsPseudo(item.rawcode)) return fail(Bad("MAP_RANDOM_INVENTORY_INVALID", "随机实例物品栏须为唯一合法槽位及实际物品ID。"));
            auto checked = CheckChoice(inputs, item.rawcode, 2); if (!checked.Succeeded()) return fail(checked);
        }
        if (unit.waygateRegionId != -1 && (!inputs.regions || std::count_if(inputs.regions->entries.begin(), inputs.regions->entries.end(), [&](const auto& r) { return r.creationNumber == unit.waygateRegionId; }) != 1))
            return fail(Bad("MAP_RANDOM_WAYGATE_UNBOUND", "随机传送门需要唯一的真实区域引用。"));
        for (const auto& ability : unit.abilities) {
            if (ability.level < 0 || ability.level > 4096 || static_cast<std::size_t>(ability.level) > 65536 - abilityWork)
                return fail(Bad("MAP_RANDOM_ABILITY_BUDGET", "随机实例技能等级或初始化工作量超出预算。"));
            abilityWork += ability.level;
            std::optional<ObjectQueries::AbilityInfo> facts;
            try { if (inputs.queries.abilityInitialization) facts = inputs.queries.abilityInitialization(ability.rawcode); }
            catch (...) { return fail(Bad("MAP_RANDOM_ABILITY_QUERY_FAILED", "随机实例技能来源查询失败。")); }
            if (!facts || !facts->known || ((ability.autocast || facts->autocast) && (ability.autocast ? facts->orderOn.empty() : facts->orderOff.empty())))
                return fail(Bad("MAP_RANDOM_ABILITY_SOURCE_REQUIRED", "随机实例技能或自动施法命令缺少真实来源。"));
            placed.abilities.push_back(std::move(*facts));
        }
        placed.typeExpression = RandomGroupSymbol(global->tableId) + "[" + std::to_string(global->column) + "]";
        for (const auto& row : found->rows) { auto checked = CheckChoice(inputs, Raw(row.ids[global->column]), placed.kind, &placed.typeColors); if (!checked.Succeeded()) return fail(checked); }
        if (result.placements.size() >= 4096) return fail(Bad("MAP_RANDOM_PLACEMENT_BUDGET", "随机放置数量超过初始化预算。"));
        result.placements.emplace(i, std::move(placed));
    }
    if (inputs.doodads) for (const auto& doodad : inputs.doodads->doodads) {
        if (!drop(doodad.droppedItemTable)) return fail(Bad("MAP_RANDOM_DROP_TABLE_MISSING", "可破坏物引用的全局物品表不存在。"));
        if (doodad.droppedItemTable != -1) {
            result.hasRandomInitialization = true;
            bool known = false; try { known = inputs.queries.knownDestructable && inputs.queries.knownDestructable(doodad.rawcode); }
            catch (...) { return fail(Bad("MAP_RANDOM_DESTRUCTABLE_QUERY_FAILED", "全局掉落的可破坏物来源查询失败。")); }
            if (!known) return fail(Bad("MAP_RANDOM_DESTRUCTABLE_SOURCE_REQUIRED", "全局掉落须绑定实际可破坏物类型，不能猜测普通装饰物。"));
        }
    }
    return R::Success(std::move(result));
}
void WriteRandomGroupGlobals(MapContext& context) {
    for (const auto number : context.random.unitTables) context.out.Line("integer array " + RandomGroupSymbol(number));
}
void WriteRandomGroups(MapContext& context) {
    if (context.random.unitTables.empty()) return;
    auto& out = context.out;
    out.Line("function InitRandomGroups takes nothing returns nothing"); out.Begin(); out.Line("local integer curset");
    for (const auto& table : context.in.settings->custom->randomUnits) {
        out.Line("call RandomDistReset(  )");
        for (std::size_t r = 0; r < table.rows.size(); ++r) out.Line("call RandomDistAddItem( " + std::to_string(r) + ", " + std::to_string(table.rows[r].chance) + " )");
        out.Line("set curset = RandomDistChoose(  )");
        for (std::size_t c = 0; c < table.columnTypes.size(); ++c) out.Line("set " + RandomGroupSymbol(table.number) + "[" + std::to_string(c) + "] = -1");
        for (std::size_t r = 0; r < table.rows.size(); ++r) {
            out.Line(std::string(r ? "elseif" : "if") + " (curset == " + std::to_string(r) + ") then"); out.Begin();
            for (std::size_t c = 0; c < table.columnTypes.size(); ++c) out.Line("set " + RandomGroupSymbol(table.number) + "[" + std::to_string(c) + "] = " + *RandomChoiceExpression(Raw(table.rows[r].ids[c]), table.columnTypes[c]));
            out.End();
        }
        if (!table.rows.empty()) out.Line("endif");
    }
    out.End(); out.Line("endfunction"); out.Line("");
}
} // namespace War3Map::Script::detail
