#include "War3MapCore/Services/PlacementCreation.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <type_traits>

namespace War3Map::Services {
namespace {
using ObjectData::ObjectType;
Status TypeExists(const ObjectCommandContext& context, const std::string& rawcode) {
    if (context.document && !context.document->IsStructurallySafe())
        return Status::Failure("Placement.Create", "PLACEMENT_OBJECT_SOURCE_UNSAFE", "该类物编尚未完整读取，不能确认新建类型。");
    const auto fields = ReadObjectFields(context, rawcode); // 同一父链/标准来源验证，不另写继承解析。
    if (!fields.Succeeded() && fields.diagnostics.empty())
        return Status::Failure("Placement.Create", "PLACEMENT_OBJECT_UNAVAILABLE", "不能确认物体类型及其父链。");
    return {fields.diagnostics};
}
Result<NewPlacementRecord> Failure(const char* code, const char* text) {
    return Result<NewPlacementRecord>::Failure("Placement.Create", code, text);
}
}

NewPlacementRecord MakeDefaultPlacement(ObjectType type, const std::string& rawcode,
    std::int32_t owner, const std::array<float, 3>& position) {
    constexpr float facing = 4.71238898f;
    if (type == ObjectType::Doodad || type == ObjectType::Destructable) {
        MapData::DoodadPlacement doodad;
        doodad.rawcode = rawcode; doodad.position = position; doodad.rotation = facing; doodad.scale = {1, 1, 1};
        doodad.flags = 2; doodad.lifePercent = 100; doodad.droppedItemTable = -1;
        return doodad;
    }
    MapData::UnitPlacement unit;
    unit.rawcode = rawcode; unit.position = position; unit.rotation = facing; unit.scale = {1, 1, 1};
    unit.flags = 2; unit.owner = type == ObjectType::Item ? 15 : owner;
    unit.hitPoints = -1; unit.mana = -1; unit.droppedItemTable = -1; unit.gold = 12500;
    unit.targetAcquisition = -1; unit.heroLevel = 1; unit.random = MapData::RandomAnyUnit{1, 0};
    unit.teamColor = -1; unit.waygateRegionId = -1;
    return unit;
}

Result<NewPlacementRecord> PreparePlacementCreation(const PlacementCreateRequest& request,
    const ObjectCommandContext& source, const MapData::TerrainPointSample& sample,
    const ObjectCommandContext* drops) {
    const auto type = request.type;
    if (type != ObjectType::Unit && type != ObjectType::Item && type != ObjectType::Doodad && type != ObjectType::Destructable)
        return Failure("PLACEMENT_OBJECT_KIND", "只有单位、物品、装饰物和可破坏物可以新建放置。");
    if (source.type != type) return Failure("PLACEMENT_OBJECT_KIND", "对象目录类型与本次新建类型不同。");
    if (!std::isfinite(request.point.x) || !std::isfinite(request.point.y)
        || sample.point.x != request.point.x || sample.point.y != request.point.y || !std::isfinite(sample.groundHeight))
        return Failure("PLACEMENT_TERRAIN_BINDING", "新建坐标没有同一点的当前地形采样，不能猜测地高或边界。");
    if (sample.groundHeightApproximate)
        return Failure("PLACEMENT_TERRAIN_APPROXIMATE", "该坡道点只有近似地高，缺少实际Terrain.MaxHeight来源，未新建任何对象。");
    const double z = double(sample.groundHeight) + request.heightOffset;
    if (!std::isfinite(z) || std::abs(z) > 1e6 || std::abs(request.point.x) > 1e6 || std::abs(request.point.y) > 1e6)
        return Failure("PLACEMENT_COORDINATE_INVALID", "新建坐标超出有限世界坐标范围。");
    if (!std::isfinite(request.facingRadians) || request.facingRadians < 0 || request.facingRadians >= 6.28318530718f
        || std::any_of(request.scale.begin(), request.scale.end(), [](float value) { return !std::isfinite(value) || value <= 0 || value > 1000; })
        || request.variation < 0)
        return Failure("PLACEMENT_TRANSFORM_INVALID", "朝向须为 [0,2π) 弧度、缩放须为 (0,1000]，变体须非负。");
    const bool unit = type == ObjectType::Unit;
    const bool item = type == ObjectType::Item;
    if (request.owner && (*request.owner < 0 || *request.owner > 15 || (!unit && !item) || (item && *request.owner != 15)))
        return Failure("PLACEMENT_OWNER_INVALID", "单位玩家须为 0..15；物品固定为玩家 15；地物没有所属玩家。");
    if ((request.heroLevel && (!unit || *request.heroLevel < 1)) || (request.mana && (!unit || *request.mana < -1)))
        return Failure("PLACEMENT_UNIT_FIELD_INVALID", "英雄等级与绝对魔法值只用于单位，等级须至少为 1，魔法为 -1 或非负整数。");
    if (request.hitPointsPercent && (*request.hitPointsPercent < (unit ? -1 : 0) || *request.hitPointsPercent > 100 || item))
        return Failure("PLACEMENT_LIFE_INVALID", "单位生命为默认 -1 或 0..100%；地物为 0..100%；物品不接受该字段。");
    if (const auto exists = TypeExists(source, request.rawcode); !exists.Succeeded()) return {std::nullopt, exists.diagnostics};
    if (request.rawcode == "sloc" && unit)
        return Failure("MAP_PLACEMENT_START_CREATE_UNSUPPORTED", "起始位置需要单独同步玩家设置。");
    if (request.itemSets.size() > 32 || (!request.itemSets.empty() && type != ObjectType::Unit && type != ObjectType::Destructable))
        return Failure("PLACEMENT_DROPS_INVALID", "内联掉落只用于单位和可破坏物，每次最多 32 组。");
    if (!request.itemSets.empty()) {
        if (!drops || drops->type != ObjectType::Item)
            return Failure("PLACEMENT_DROPS_SOURCE", "内联掉落需要真实的物品目录。");
        std::size_t total = 0; std::set<std::string> checked;
        for (const auto& set : request.itemSets) {
            if (set.items.empty() || set.items.size() > 32 || set.items.size() > 128 - total)
                return Failure("PLACEMENT_DROPS_INVALID", "每个掉落组须含 1..32 条物品，全记录最多 128 条。");
            total += set.items.size(); int chance = 0;
            for (const auto& choice : set.items) {
                if (choice.chance < 0 || choice.chance > 100 || chance > 100 - choice.chance)
                    return Failure("PLACEMENT_DROPS_INVALID", "每组掉落的概率须为 0..100 整数且总和不超过 100。");
                chance += choice.chance;
                if (checked.insert(choice.rawcode).second) {
                    const auto exists = TypeExists(*drops, choice.rawcode);
                    if (!exists.Succeeded()) return {std::nullopt, exists.diagnostics};
                }
            }
        }
    }
    auto record = MakeDefaultPlacement(type, request.rawcode, request.owner.value_or(0),
        {static_cast<float>(request.point.x), static_cast<float>(request.point.y), static_cast<float>(z)});
    std::visit([&](auto& value) {
        value.rotation = request.facingRadians; value.scale = request.scale; value.variation = request.variation;
        value.itemSets = request.itemSets;
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, MapData::UnitPlacement>) {
            if (request.heroLevel) value.heroLevel = *request.heroLevel;
            if (request.hitPointsPercent) value.hitPoints = *request.hitPointsPercent;
            if (request.mana) value.mana = *request.mana;
        } else if (request.hitPointsPercent) value.lifePercent = static_cast<std::uint8_t>(*request.hitPointsPercent);
    }, record);
    return Result<NewPlacementRecord>::Success(std::move(record));
}

} // namespace War3Map::Services
