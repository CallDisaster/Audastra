#include "War3MapCore/Script/ScriptQueries.h"

#include <cstdlib>
#include <charconv>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <set>
#include <unordered_map>

namespace War3Map::Script {
namespace {

struct QueryState {
    const ObjectData::StandardObjectCatalog* standard = nullptr;
    std::unordered_map<std::string, ObjectData::ObjectRecord> units;
    std::unordered_map<std::string, ObjectData::ObjectRecord> items;
    std::unordered_map<std::string, ObjectData::ObjectRecord> abilities;
    std::unordered_map<std::string, ObjectData::ObjectRecord> destructables;
    std::set<std::string> ambiguousUnits, ambiguousItems, ambiguousAbilities, ambiguousDestructables;

    struct Resolved {
        const ObjectData::StandardObjectEntry* base = nullptr;
        std::vector<const ObjectData::ObjectRecord*> overrides;
    };
    std::optional<Resolved> Resolve(ObjectData::ObjectType type,
        const std::unordered_map<std::string, ObjectData::ObjectRecord>& records,
        const std::set<std::string>& ambiguous, std::string_view id) const {
        if (!standard) return {};
        Resolved result; std::set<std::string> seen; std::string current(id);
        for (std::size_t depth = 0; depth < 128; ++depth) {
            if (!seen.insert(current).second || ambiguous.contains(current)) return {};
            const auto found = records.find(current);
            if (found != records.end()) {
                if (found->second.key.type != type) return {};
                result.overrides.push_back(&found->second);
                if (found->second.parentRawcode) { current = *found->second.parentRawcode; continue; }
            }
            result.base = standard->Find(type, current);
            if (!result.base) return {};
            return result;
        }
        return {};
    }
    static std::optional<std::string> Property(const Resolved& source, std::string_view nativeField, std::string_view standardField) {
        for (const auto* record : source.overrides) if (auto value = Modified(*record, nativeField)) return value;
        const auto value = source.base->Field(standardField);
        return value.empty() ? std::nullopt : std::optional(std::string(value));
    }
    std::optional<ObjectQueries::PlacementInfo> Placement(std::string_view id) const {
        const bool hasUnit = units.contains(std::string(id)) || (standard && standard->Find(ObjectData::ObjectType::Unit, id));
        const bool hasItem = items.contains(std::string(id)) || (standard && standard->Find(ObjectData::ObjectType::Item, id));
        if (hasUnit == hasItem) return {};
        const auto unit = Resolve(ObjectData::ObjectType::Unit, units, ambiguousUnits, id);
        const auto item = Resolve(ObjectData::ObjectType::Item, items, ambiguousItems, id);
        // 顶层或任何父级歧义不能借另一类别的成功结果掩盖。
        if (ambiguousUnits.contains(std::string(id)) || ambiguousItems.contains(std::string(id)) || unit.has_value() == item.has_value()) return {};
        if (item) return ObjectQueries::PlacementInfo{2, -1};
        for (const auto* record : unit->overrides) {
            std::set<std::string> fields;
            for (const auto& modification : record->modifications) {
                // 未经原生投影的LNI字段不能按原版默认值推断类别。
                if (modification.key.fieldId.size() != 4) return {};
                if (modification.key.fieldId != "ubdg" && modification.key.fieldId != "utco") continue;
                if (!fields.insert(modification.key.fieldId).second || modification.key.level
                    || modification.key.dataIndex || modification.key.variation) return {};
                if (!std::holds_alternative<std::int32_t>(modification.value)
                    && !(modification.key.fieldId == "ubdg" && std::holds_alternative<bool>(modification.value))) return {};
            }
        }
        const auto building = Property(*unit, "ubdg", "isbldg");
        if (!building || (*building != "0" && *building != "1" && *building != "false" && *building != "true")) return {};
        ObjectQueries::PlacementInfo result{*building == "1" || *building == "true" ? 1 : 0, -1};
        if (const auto color = Property(*unit, "utco", "teamcolor")) {
            const auto [end, error] = std::from_chars(color->data(), color->data() + color->size(), result.teamColor);
            if (error != std::errc{} || end != color->data() + color->size() || result.teamColor < -1 || result.teamColor > 15) return {};
        }
        return result;
    }
    std::optional<ObjectQueries::AbilityInfo> AbilityInitialization(std::string_view id) const {
        const auto source = Resolve(ObjectData::ObjectType::Ability, abilities, ambiguousAbilities, id);
        if (!source) return {};
        ObjectQueries::AbilityInfo result; result.known = true;
        const auto direct = abilities.find(std::string(id)); result.custom = direct != abilities.end() && direct->second.parentRawcode.has_value();
        for (const auto* record : source->overrides) {
            std::set<std::string> fields;
            for (const auto& modification : record->modifications) {
                const auto& key = modification.key;
                if (key.fieldId.size() != 4) return {};
                if (key.fieldId != "aher" && key.fieldId != "aoro" && key.fieldId != "aorf") continue;
                if (!fields.insert(key.fieldId).second || (key.level && *key.level != 0) || (key.dataIndex && *key.dataIndex != 0) || key.variation) return {};
                if (key.fieldId == "aher") {
                    if (!std::holds_alternative<std::int32_t>(modification.value) && !std::holds_alternative<bool>(modification.value)) return {};
                } else if (!std::holds_alternative<std::string>(modification.value)) return {};
            }
        }
        const auto boolean = [&](std::string_view native, std::string_view standard, bool& target) {
            auto value = Property(*source, native, standard);
            if (!value || (*value != "0" && *value != "1" && *value != "false" && *value != "true")) return false;
            target = *value == "1" || *value == "true"; return true;
        };
        if (!boolean("aher", "hero", result.hero)) return {};
        result.autocast = !source->base->Field("orderoff").empty();
        result.orderOn = Property(*source, "aoro", "orderon").value_or(""); result.orderOff = Property(*source, "aorf", "orderoff").value_or("");
        return result;
    }

    // 自定义物体的字段覆盖；未覆盖时返回空。
    static std::optional<std::string> Modified(const ObjectData::ObjectRecord& record, std::string_view field) {
        for (const auto& modification : record.modifications) {
            if (modification.key.fieldId == field) return ObjectData::ValueToDisplayString(modification.value);
        }
        return std::nullopt;
    }

    // 自定义物体取父物体 rawcode 查原版表。
    std::string StandardField(ObjectData::ObjectType type, const std::unordered_map<std::string, ObjectData::ObjectRecord>& custom,
        std::string_view rawcode, std::string_view field) const {
        std::string base(rawcode);
        if (const auto found = custom.find(base); found != custom.end() && found->second.parentRawcode) base = *found->second.parentRawcode;
        if (standard == nullptr) return {};
        const auto* entry = standard->Find(type, base);
        return entry == nullptr ? std::string{} : std::string(entry->Field(field));
    }
};

bool Truthy(std::string_view value) { return value == "1" || value == "true" || value == "True"; }

} // namespace

ObjectQueries BuildObjectQueries(ObjectQuerySources sources) {
    auto state = std::make_shared<QueryState>();
    state->standard = sources.standard;
    for (auto& record : sources.units) { const auto id = record.key.rawcode; if (!state->units.emplace(id, std::move(record)).second) state->ambiguousUnits.insert(id); }
    for (auto& record : sources.items) { const auto id = record.key.rawcode; if (!state->items.emplace(id, std::move(record)).second) state->ambiguousItems.insert(id); }
    for (auto& record : sources.abilities) { const auto id = record.key.rawcode; if (!state->abilities.emplace(id, std::move(record)).second) state->ambiguousAbilities.insert(id); }
    for (auto& record : sources.destructables) { const auto id = record.key.rawcode; if (!state->destructables.emplace(id, std::move(record)).second) state->ambiguousDestructables.insert(id); }

    ObjectQueries queries;
    queries.placement = [state](std::string_view id) { return state->Placement(id); };
    queries.abilityInitialization = [state](std::string_view id) { return state->AbilityInitialization(id); };
    queries.knownDestructable = [state](std::string_view id) { return state->Resolve(ObjectData::ObjectType::Destructable, state->destructables, state->ambiguousDestructables, id).has_value(); };
    queries.isItem = [state](std::string_view rawcode) {
        if (state->items.contains(std::string(rawcode))) return true;
        return state->standard != nullptr && state->standard->Find(ObjectData::ObjectType::Item, rawcode) != nullptr;
    };
    queries.isBuilding = [state](std::string_view rawcode) {
        if (const auto found = state->units.find(std::string(rawcode)); found != state->units.end()) {
            if (const auto value = QueryState::Modified(found->second, "ubdg")) return Truthy(*value);
        }
        return Truthy(state->StandardField(ObjectData::ObjectType::Unit, state->units, rawcode, "isbldg"));
    };
    queries.ability = [state](std::string_view rawcode) {
        ObjectQueries::AbilityInfo info;
        const auto custom = state->abilities.find(std::string(rawcode));
        const bool standard = state->standard != nullptr && state->standard->Find(ObjectData::ObjectType::Ability, rawcode) != nullptr;
        info.known = custom != state->abilities.end() || standard || state->standard == nullptr;
        const auto field = [&](std::string_view customField, std::string_view standardField) -> std::string {
            if (custom != state->abilities.end()) {
                if (const auto value = QueryState::Modified(custom->second, customField)) return *value;
            }
            return state->StandardField(ObjectData::ObjectType::Ability, state->abilities, rawcode, standardField);
        };
        info.hero = Truthy(field("aher", "hero"));
        // 能否自动施法看父技能的关闭命令；命令串优先取自定义覆盖，否则继承父技能。
        info.custom = custom != state->abilities.end() && custom->second.parentRawcode.has_value();
        info.autocast = !state->StandardField(ObjectData::ObjectType::Ability, state->abilities, rawcode, "orderoff").empty();
        const auto order = [&](std::string_view customField, std::string_view standardField) -> std::string {
            if (custom != state->abilities.end()) {
                if (const auto value = QueryState::Modified(custom->second, customField)) return *value;
            }
            return state->StandardField(ObjectData::ObjectType::Ability, state->abilities, rawcode, standardField);
        };
        info.orderOn = order("aoro", "orderon");
        info.orderOff = order("aorf", "orderoff");
        return info;
    };
    queries.unitTeamColor = [state](std::string_view rawcode) {
        if (const auto found = state->units.find(std::string(rawcode)); found != state->units.end()) {
            if (const auto value = QueryState::Modified(found->second, "utco")) return std::atoi(value->c_str());
        }
        const std::string value = state->StandardField(ObjectData::ObjectType::Unit, state->units, rawcode, "teamcolor");
        return value.empty() ? -1 : std::atoi(value.c_str());
    };
    return queries;
}

namespace {

// 解析形如 `name = 0xD0003,` 的数据行；遇到 `['Type'] = {` 时切换分组（order_list）。
std::map<std::string, std::map<std::string, std::int64_t>> ReadOrderTable(const std::filesystem::path& path) {
    std::map<std::string, std::map<std::string, std::int64_t>> groups;
    std::ifstream input(path, std::ios::binary);
    std::string group;
    for (std::string line; std::getline(input, line);) {
        const auto open = line.find("['");
        if (open != std::string::npos && line.find('{') != std::string::npos) {
            const auto close = line.find("']", open + 2);
            if (close != std::string::npos) group = line.substr(open + 2, close - open - 2);
            continue;
        }
        const auto equals = line.find('=');
        const auto hex = line.find("0x");
        if (equals == std::string::npos || hex == std::string::npos || hex < equals) continue;
        std::size_t begin = 0;
        while (begin < equals && (line[begin] == ' ' || line[begin] == '\t')) ++begin;
        std::size_t end = equals;
        while (end > begin && (line[end - 1] == ' ' || line[end - 1] == '\t')) --end;
        if (end == begin) continue;
        groups[group][line.substr(begin, end - begin)] = std::strtoll(line.c_str() + hex + 2, nullptr, 16);
    }
    return groups;
}

} // namespace

TemplateContext BuildTemplateContext(const std::filesystem::path& toolRoot, const ObjectData::StandardObjectCatalog* standard,
    std::vector<ObjectData::ObjectRecord> abilities) {
    struct State {
        std::map<std::string, std::int64_t> orderIds;
        std::map<std::string, std::map<std::string, std::int64_t>> orderLists;
        std::unordered_map<std::string, ObjectData::ObjectRecord> abilities;
        const ObjectData::StandardObjectCatalog* standard = nullptr;
    };
    auto state = std::make_shared<State>();
    // 命令表放在框架模板宿主的 computed 目录：新版 compiler/script（abilityOrder.lua），
    // 旧版 share/script（ability2order.lua）。两者都在时无法确定宿主，不读表，查询按未知失败。
    const auto modern = toolRoot / "compiler" / "script" / "computed";
    const auto legacy = toolRoot / "share" / "script" / "computed";
    std::error_code error;
    const bool hasModern = std::filesystem::is_regular_file(modern / "abilityOrder.lua", error);
    const bool hasLegacy = std::filesystem::is_regular_file(legacy / "ability2order.lua", error);
    const auto computed = hasModern && hasLegacy ? std::filesystem::path{} : hasLegacy ? legacy : modern;
    auto ids = computed.empty() ? decltype(ReadOrderTable(modern)){} : ReadOrderTable(computed / "order_id.lua");
    for (auto& [group, table] : ids) state->orderIds.merge(table);
    if (!computed.empty()) state->orderLists = ReadOrderTable(computed / "order_list.lua");
    for (auto& record : abilities) state->abilities.emplace(record.key.rawcode, std::move(record));
    state->standard = standard;

    TemplateContext context;
    context.abilityOrder = [state](std::string_view skill, std::string_view type) -> std::optional<std::int64_t> {
        std::string id(skill);
        if (id.size() >= 2 && id.front() == '\'' && id.back() == '\'') id = id.substr(1, id.size() - 2);
        const auto custom = state->abilities.find(id);
        const ObjectData::StandardObjectEntry* base = nullptr;
        std::string code = id;
        if (custom != state->abilities.end() && custom->second.parentRawcode) code = *custom->second.parentRawcode;
        if (state->standard != nullptr) base = state->standard->Find(ObjectData::ObjectType::Ability, code);
        if (custom == state->abilities.end() && base == nullptr) return std::nullopt;
        if (code == "ANcl") {
            // 通道：命令串为 DataF1（自定义字段 Ncl6 第 1 级）。
            std::string order;
            if (custom != state->abilities.end()) {
                for (const auto& modification : custom->second.modifications) {
                    if (modification.key.fieldId == "Ncl6" && modification.key.level.value_or(1) <= 1) {
                        order = ObjectData::ValueToDisplayString(modification.value);
                        break;
                    }
                }
            }
            if (order.empty() && base != nullptr) order = std::string(base->Field("dataf1"));
            const auto found = state->orderIds.find(order);
            if (found == state->orderIds.end()) return std::nullopt;
            return found->second;
        }
        const auto list = state->orderLists.find(std::string(type));
        if (list == state->orderLists.end()) return std::nullopt;
        const auto found = list->second.find(code);
        if (found == list->second.end()) return std::nullopt;
        return found->second;
    };
    return context;
}

} // namespace War3Map::Script
