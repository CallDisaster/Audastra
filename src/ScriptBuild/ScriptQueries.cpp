#include "War3MapCore/Script/ScriptQueries.h"

#include <cstdlib>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <unordered_map>

namespace War3Map::Script {
namespace {

struct QueryState {
    const ObjectData::StandardObjectCatalog* standard = nullptr;
    std::unordered_map<std::string, ObjectData::ObjectRecord> units;
    std::unordered_map<std::string, ObjectData::ObjectRecord> items;
    std::unordered_map<std::string, ObjectData::ObjectRecord> abilities;

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
    for (auto& record : sources.units) state->units.emplace(record.key.rawcode, std::move(record));
    for (auto& record : sources.items) state->items.emplace(record.key.rawcode, std::move(record));
    for (auto& record : sources.abilities) state->abilities.emplace(record.key.rawcode, std::move(record));

    ObjectQueries queries;
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
    const auto computed = toolRoot / "compiler" / "script" / "computed";
    auto ids = ReadOrderTable(computed / "order_id.lua");
    for (auto& [group, table] : ids) state->orderIds.merge(table);
    state->orderLists = ReadOrderTable(computed / "order_list.lua");
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
