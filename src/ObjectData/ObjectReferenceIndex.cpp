#include "War3MapCore/ObjectData/ObjectReferenceIndex.h"

#include "War3MapCore/MapDocument.h"
#include "War3MapCore/ObjectData/ObjectFieldSchema.h"

#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <tuple>
#include <utility>

namespace War3Map::ObjectData {
namespace {

constexpr std::array<ObjectType, 7> kAllTypes{
    ObjectType::Unit, ObjectType::Ability, ObjectType::Item, ObjectType::Buff,
    ObjectType::Upgrade, ObjectType::Doodad, ObjectType::Destructable,
};

ReferenceInputState Convert(MapDataState state) {
    switch (state) {
    case MapDataState::Missing: return ReferenceInputState::Missing;
    case MapDataState::Ready: return ReferenceInputState::Ready;
    case MapDataState::Partial: return ReferenceInputState::Partial;
    case MapDataState::Invalid: return ReferenceInputState::Invalid;
    case MapDataState::Ambiguous: return ReferenceInputState::Ambiguous;
    case MapDataState::Unreadable: return ReferenceInputState::Unreadable;
    }
    return ReferenceInputState::Invalid;
}

bool IsEmptyRawcode(std::string_view value) {
    return value.empty() || value == "_" || value == "-"
        || value == std::string_view{"\0\0\0\0", 4};
}

void MarkPartial(ReferenceInputCoverage& coverage, std::string detail) {
    if (coverage.state == ReferenceInputState::Ready) coverage.state = ReferenceInputState::Partial;
    if (coverage.detail.empty()) coverage.detail = std::move(detail);
}

struct ReferenceOwner {
    std::optional<ObjectKey> object;
    std::string fieldId;
    std::optional<std::size_t> placement;
};

void Append(std::vector<IndexedObjectReference>& output,
    ReferenceInputCoverage& coverage,
    std::string_view rawcode,
    std::vector<ObjectType> types,
    bool exactType,
    IndexedReferenceOrigin origin,
    const std::string& memberName,
    std::string location,
    const ReferenceOwner& owner = {}) {
    if (IsEmptyRawcode(rawcode)) return;
    if (rawcode.size() != 4 || types.empty()) {
        MarkPartial(coverage, "发现未能确定的对象 ID；此来源不能视为引用扫描完整。");
        return;
    }
    std::sort(types.begin(), types.end());
    types.erase(std::unique(types.begin(), types.end()), types.end());
    output.push_back({std::string(rawcode), std::move(types), exactType,
        origin, memberName, std::move(location), owner.object, owner.fieldId, owner.placement});
}

std::string ModificationLocation(const ObjectModification& modification) {
    std::string location = "field[" + modification.key.fieldId;
    if (modification.key.level) location += ",level=" + std::to_string(*modification.key.level);
    if (modification.key.dataIndex) location += ",data=" + std::to_string(*modification.key.dataIndex);
    if (modification.key.variation) location += ",variation=" + std::to_string(*modification.key.variation);
    location += ']';
    if (modification.sourceLine != 0) location += ":line=" + std::to_string(modification.sourceLine);
    return location;
}

void IndexObjectSource(std::vector<IndexedObjectReference>& output,
    ReferenceInputCoverage& coverage,
    const ObjectRecordSource& source,
    const ObjectFieldSchema* schema) {
    std::set<std::string> seenObjects;
    for (const ObjectRecord& record : source.records) {
        const std::string objectLocation = "object[" + std::string(ToString(record.key.type))
            + ':' + record.key.rawcode + ']';
        if (record.key.type != source.type || record.key.rawcode.size() != 4) {
            MarkPartial(coverage, "对象记录类型或 ID 与输入来源不一致。");
            continue;
        }
        if (!seenObjects.insert(record.key.rawcode).second) {
            MarkPartial(coverage, "同一物编来源包含重复对象 ID；不能认定覆盖完整。");
        }
        if (record.parentRawcode) {
            Append(output, coverage, *record.parentRawcode, {source.type}, true,
                IndexedReferenceOrigin::ObjectParent, coverage.memberName,
                objectLocation + ".parent", {record.key, {}, {}});
        }
        for (const ObjectModification& modification : record.modifications) {
            const auto* scalar = std::get_if<RawcodeValue>(&modification.value);
            const auto* list = std::get_if<RawcodeListValue>(&modification.value);
            if (scalar == nullptr && list == nullptr) continue;
            const ObjectFieldDefinition* definition = schema == nullptr ? nullptr
                : schema->FindNative(source.type, modification.key.fieldId);
            const bool knownType = definition != nullptr && !definition->referenceTypes.empty();
            const std::vector<ObjectType> types = knownType ? definition->referenceTypes
                : std::vector<ObjectType>(kAllTypes.begin(), kAllTypes.end());
            if (!knownType) {
                MarkPartial(coverage, "存在目标类型未知的 Rawcode 字段；按所有对象类型保守索引。");
            }
            const std::string location = objectLocation + '.' + ModificationLocation(modification);
            const ReferenceOwner owner{record.key, modification.key.fieldId, {}};
            if (scalar != nullptr) {
                Append(output, coverage, scalar->value, types, knownType && types.size() == 1,
                    IndexedReferenceOrigin::ObjectField, coverage.memberName, location, owner);
            } else {
                for (std::size_t i = 0; i < list->values.size(); ++i) {
                    Append(output, coverage, list->values[i], types, knownType && types.size() == 1,
                        IndexedReferenceOrigin::ObjectField, coverage.memberName,
                        location + ".value[" + std::to_string(i) + ']', owner);
                }
            }
        }
    }
}

void IndexItemSets(std::vector<IndexedObjectReference>& output,
    ReferenceInputCoverage& coverage,
    const std::vector<MapData::PlacementItemSet>& sets,
    const std::string& placementLocation,
    std::size_t placement) {
    for (std::size_t set = 0; set < sets.size(); ++set) {
        for (std::size_t item = 0; item < sets[set].items.size(); ++item) {
            Append(output, coverage, sets[set].items[item].rawcode, {ObjectType::Item}, true,
                IndexedReferenceOrigin::PlacedDropItem, coverage.memberName,
                placementLocation + ".itemSet[" + std::to_string(set) + "].item["
                    + std::to_string(item) + ']', {{}, {}, placement});
        }
    }
}

void IndexPlacements(std::vector<IndexedObjectReference>& output,
    ReferenceInputCoverage& coverage,
    const MapData::UnitPlacements& placements) {
    for (std::size_t i = 0; i < placements.units.size(); ++i) {
        const MapData::UnitPlacement& unit = placements.units[i];
        const std::string location = "placement[" + std::to_string(i) + "]";
        if (std::holds_alternative<std::monostate>(unit.random)) {
            // war3mapUnits.doo 同时包含单位和物品，无显式种类标志。
            Append(output, coverage, unit.rawcode, {ObjectType::Unit, ObjectType::Item}, false,
                IndexedReferenceOrigin::PlacedUnitOrItem, coverage.memberName, location + ".type", {{}, {}, i});
        } else if (const auto* custom = std::get_if<MapData::RandomCustomTable>(&unit.random)) {
            // 随机单位的占位 rawcode 不是实际对象类型。
            for (std::size_t choice = 0; choice < custom->choices.size(); ++choice) {
                Append(output, coverage, custom->choices[choice].rawcode, {ObjectType::Unit}, true,
                    IndexedReferenceOrigin::RandomUnitChoice, coverage.memberName,
                    location + ".random.choice[" + std::to_string(choice) + ']', {{}, {}, i});
            }
        }
        IndexItemSets(output, coverage, unit.itemSets, location, i);
        for (std::size_t item = 0; item < unit.inventory.size(); ++item) {
            Append(output, coverage, unit.inventory[item].rawcode, {ObjectType::Item}, true,
                IndexedReferenceOrigin::PlacedInventoryItem, coverage.memberName,
                location + ".inventory[" + std::to_string(item) + ']', {{}, {}, i});
        }
        for (std::size_t ability = 0; ability < unit.abilities.size(); ++ability) {
            Append(output, coverage, unit.abilities[ability].rawcode, {ObjectType::Ability}, true,
                IndexedReferenceOrigin::PlacedAbility, coverage.memberName,
                location + ".ability[" + std::to_string(ability) + ']', {{}, {}, i});
        }
    }
}

void IndexPlacements(std::vector<IndexedObjectReference>& output,
    ReferenceInputCoverage& coverage,
    const MapData::DoodadPlacements& placements) {
    for (std::size_t i = 0; i < placements.doodads.size(); ++i) {
        const MapData::DoodadPlacement& doodad = placements.doodads[i];
        const std::string location = "placement[" + std::to_string(i) + "]";
        Append(output, coverage, doodad.rawcode, {ObjectType::Doodad, ObjectType::Destructable}, false,
            IndexedReferenceOrigin::PlacedDoodadOrDestructable, coverage.memberName, location + ".type", {{}, {}, i});
        IndexItemSets(output, coverage, doodad.itemSets, location, i);
    }
    for (std::size_t i = 0; i < placements.specialDoodads.size(); ++i) {
        Append(output, coverage, placements.specialDoodads[i].rawcode,
            {ObjectType::Doodad, ObjectType::Destructable}, false,
            IndexedReferenceOrigin::SpecialDoodadOrDestructable, coverage.memberName,
            "special[" + std::to_string(i) + "].type", {{}, {}, i});
    }
}

} // namespace

ObjectReferenceIndex ObjectReferenceIndex::Build(
    std::span<const ObjectRecordSource> objectSources,
    const MapDocument& map,
    const ObjectFieldSchema* schema) {
    ObjectReferenceIndex result;
    std::map<ObjectType, std::size_t> sourceCounts;
    for (const ObjectRecordSource& source : objectSources) ++sourceCounts[source.type];
    for (const ObjectType type : kAllTypes) {
        if (!sourceCounts.contains(type)) {
            result.coverage_.push_back({std::string(ToString(type)), ReferenceInputState::Missing,
                "未提供此类物编输入，零命中不能证明没有引用。"});
        }
    }
    for (const ObjectRecordSource& source : objectSources) {
        std::string name = source.memberName;
        if (name.empty()) name = "<unknown:" + std::string(ToString(source.type)) + '>';
        ReferenceInputCoverage coverage{name, source.state, {}};
        if (source.memberName.empty() || sourceCounts[source.type] != 1) {
            coverage.state = ReferenceInputState::Ambiguous;
            coverage.detail = "物编来源身份缺失或同类存在多个输入；不能认定覆盖完整。";
        }
        IndexObjectSource(result.references_, coverage, source, schema);
        result.coverage_.push_back(std::move(coverage));
    }
    for (const auto kind : {MapDataKind::Units, MapDataKind::Doodads}) {
        const MapMemberState& member = map.Member(kind);
        ReferenceInputCoverage coverage{member.memberName, Convert(member.state), {}};
        if (kind == MapDataKind::Units && map.Units()) {
            IndexPlacements(result.references_, coverage, *map.Units());
        } else if (kind == MapDataKind::Doodads && map.Doodads()) {
            IndexPlacements(result.references_, coverage, *map.Doodads());
        }
        if (coverage.state != ReferenceInputState::Ready && coverage.detail.empty()) {
            coverage.detail = "成员缺失、部分或解析失败；不能据此判断无引用。";
        }
        result.coverage_.push_back(std::move(coverage));
    }
    std::sort(result.references_.begin(), result.references_.end(),
        [](const IndexedObjectReference& left, const IndexedObjectReference& right) {
            return std::tie(left.memberName, left.location, left.rawcode, left.origin)
                < std::tie(right.memberName, right.location, right.rawcode, right.origin);
        });
    std::sort(result.coverage_.begin(), result.coverage_.end(),
        [](const ReferenceInputCoverage& left, const ReferenceInputCoverage& right) {
            return left.memberName < right.memberName;
        });
    return result;
}

ObjectReferenceMatches ObjectReferenceIndex::Find(const ObjectKey& target) const {
    ObjectReferenceMatches result;
    for (const IndexedObjectReference& reference : references_) {
        if (reference.rawcode != target.rawcode
            || std::find(reference.targetTypes.begin(), reference.targetTypes.end(), target.type)
                == reference.targetTypes.end()) continue;
        if (reference.exactType) result.definite.push_back(reference);
        else result.possible.push_back(reference);
    }
    return result;
}

} // namespace War3Map::ObjectData
