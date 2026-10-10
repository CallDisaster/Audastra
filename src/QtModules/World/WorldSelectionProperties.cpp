#include "WorldSelectionProperties.h"
#include "War3MapCore/Render/TerrainPicking.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <numbers>
#include <unordered_set>
#include <utility>

namespace War3Map::Modules {
namespace {
float Value(const MapData::PlacementTransform& transform, WorldPlacementProperty property) {
    return property == WorldPlacementProperty::Facing ? transform.rotation
        : transform.position[property == WorldPlacementProperty::X ? 0 : 1];
}

Result<std::array<float, 3>> GroundedPosition(const MapData::TerrainGridSnapshot& terrain,
    const MapData::PlacementTransform& source, std::array<float, 3> position) {
    using Output = Result<std::array<float, 3>>;
    if (!std::isfinite(position[0]) || !std::isfinite(position[1])
        || std::abs(position[0]) > 1000000.0f || std::abs(position[1]) > 1000000.0f)
        return Output::Failure("World", "WORLD_PROPERTY_INVALID", "坐标不是有效数值，整组选中项未移动");
    if (position[0] == source.position[0] && position[1] == source.position[1]) {
        if (!std::isfinite(position[2]) || std::abs(position[2]) > 1000000.0f)
            return Output::Failure("World", "WORLD_PROPERTY_INVALID", "目标高度超出有效范围，整组选中项未移动");
        return Output::Success(position);
    }
    const auto ground = Render::SampleTerrainHeight(terrain, position[0], position[1]);
    if (!ground) return Output::Failure("World", "WORLD_PROPERTY_OUTSIDE_MAP", "坐标超出地图范围，整组选中项未移动");
    const auto before = Render::SampleTerrainHeight(terrain, source.position[0], source.position[1]);
    // 沿用既有单选/拖动规则：当前位置无地表时，落到目标地面。
    position[2] = *ground + (before ? source.position[2] - *before : 0.0f);
    if (!std::isfinite(position[2]) || std::abs(position[2]) > 1000000.0f)
        return Output::Failure("World", "WORLD_PROPERTY_INVALID", "目标地表高度超出有效范围，整组选中项未移动");
    return Output::Success(position);
}
} // namespace

std::string WorldPlacementHostKey(const MapData::PlacementId& id) {
    return std::to_string(static_cast<unsigned>(id.file)) + ":" + std::to_string(id.index) + ":"
        + id.rawcode + ":" + std::to_string(id.creationId);
}

bool WorldPlacementProperties::Same(WorldPlacementProperty property) const noexcept {
    if (transforms.empty()) return true;
    const auto first = std::bit_cast<std::uint32_t>(Value(transforms.front(), property));
    return std::all_of(transforms.begin(), transforms.end(), [first, property](const auto& transform) {
        return std::bit_cast<std::uint32_t>(Value(transform, property)) == first;
    });
}

Result<WorldPlacementProperties> CaptureWorldPlacementProperties(
    const MapDocument& document, std::span<const MapData::PlacementId> ids) {
    if (ids.empty() || ids.size() > 4096)
        return Result<WorldPlacementProperties>::Failure("World", "WORLD_SELECTION_LIMIT", "需要选择1至4096个摆放");
    WorldPlacementProperties captured;
    captured.transforms.reserve(ids.size());
    captured.facts.reserve(ids.size());
    std::unordered_set<std::uint64_t> seen;
    for (const auto& id : ids) {
        if (id.file != MapData::PlacementFile::Units && id.file != MapData::PlacementFile::Doodads)
            return Result<WorldPlacementProperties>::Failure("World", "WORLD_SELECTION_KIND", "摆放来源类型不可用");
        const auto key = (static_cast<std::uint64_t>(id.file) << 32) | id.index;
        if (!seen.insert(key).second)
            return Result<WorldPlacementProperties>::Failure("World", "WORLD_SELECTION_DUPLICATE", "选择中含重复摆放");
        auto found = MapData::FindPlacement(document, id);
        if (!found.Succeeded()) return {std::nullopt, std::move(found.diagnostics)};
        if (!std::isfinite(found.value->rotation) || !std::all_of(found.value->position.begin(), found.value->position.end(),
            [](float value) { return std::isfinite(value); }))
            return Result<WorldPlacementProperties>::Failure("World", "WORLD_PROPERTY_SOURCE_INVALID", "源摆放的坐标或朝向不是有效数值");
        captured.transforms.push_back(*found.value);
        WorldPlacementProperties::StoredFacts facts;
        if (id.file == MapData::PlacementFile::Units) {
            const auto& source = document.Units()->units[id.index];
            facts.owner = source.owner; facts.hitPoints = source.hitPoints; facts.mana = source.mana; facts.heroLevel = source.heroLevel;
        } else facts.lifePercent = document.Doodads()->doodads[id.index].lifePercent;
        captured.facts.push_back(std::move(facts));
    }
    return Result<WorldPlacementProperties>::Success(std::move(captured));
}

Result<std::vector<MapData::PlacementMove>> PlanWorldPlacementProperty(
    const MapDocument& document, const MapData::TerrainGridSnapshot& terrain,
    std::span<const MapData::PlacementId> ids, WorldPlacementProperty property, float value) {
    using Planned = Result<std::vector<MapData::PlacementMove>>;
    if ((property != WorldPlacementProperty::X && property != WorldPlacementProperty::Y && property != WorldPlacementProperty::Facing)
        || !std::isfinite(value) || (property != WorldPlacementProperty::Facing && std::abs(value) > 1000000.0f))
        return Planned::Failure("World", "WORLD_PROPERTY_INVALID", "属性值不是有效的坐标或朝向");
    auto captured = CaptureWorldPlacementProperties(document, ids);
    if (!captured.Succeeded()) return {std::nullopt, std::move(captured.diagnostics)};
    std::vector<MapData::PlacementMove> moves;
    moves.reserve(ids.size());
    for (std::size_t index = 0; index < ids.size(); ++index) {
        const auto& source = captured.value->transforms[index];
        MapData::PlacementMove move{ids[index], source.position, std::nullopt};
        if (property == WorldPlacementProperty::Facing) move.rotation = value;
        else if (Value(source, property) != value) {
            move.position[property == WorldPlacementProperty::X ? 0 : 1] = value;
            auto grounded = GroundedPosition(terrain, source, move.position);
            if (!grounded.Succeeded()) return {std::nullopt, std::move(grounded.diagnostics)};
            move.position = *grounded.value;
        }
        moves.push_back(move);
    }
    return Planned::Success(std::move(moves));
}

Result<std::vector<MapData::PlacementMove>> BuildSelectedPlacementOffsetMoves(
    const MapDocument& document, const MapData::TerrainGridSnapshot& terrain,
    std::span<const MapData::PlacementId> ids, std::array<double, 2> delta) {
    using Planned = Result<std::vector<MapData::PlacementMove>>;
    if (!std::isfinite(delta[0]) || !std::isfinite(delta[1]))
        return Planned::Failure("World", "WORLD_PROPERTY_INVALID", "移动距离不是有效数值，整组选中项未移动");
    auto captured = CaptureWorldPlacementProperties(document, ids);
    if (!captured.Succeeded()) return {std::nullopt, std::move(captured.diagnostics)};
    std::vector<MapData::PlacementMove> moves;
    moves.reserve(ids.size());
    for (std::size_t index = 0; index < ids.size(); ++index) {
        const auto& source = captured.value->transforms[index];
        auto position = source.position;
        for (std::size_t axis = 0; axis < delta.size(); ++axis) {
            const double target = double(position[axis]) + delta[axis];
            if (!std::isfinite(target) || std::abs(target) > 1000000.0)
                return Planned::Failure("World", "WORLD_PROPERTY_INVALID", "坐标不是有效数值，整组选中项未移动");
            position[axis] = static_cast<float>(target);
        }
        auto grounded = GroundedPosition(terrain, source, position);
        if (!grounded.Succeeded()) return {std::nullopt, std::move(grounded.diagnostics)};
        moves.push_back({ids[index], *grounded.value, std::nullopt});
    }
    return Planned::Success(std::move(moves));
}

Result<WorldPlacementRotationOrigin> CaptureWorldPlacementRotationOrigin(
    const MapDocument& document, std::span<const MapData::PlacementId> ids, std::array<double, 2> pressed) {
    using Output = Result<WorldPlacementRotationOrigin>;
    if (!std::isfinite(pressed[0]) || !std::isfinite(pressed[1]))
        return Output::Failure("World", "WORLD_ROTATION_INVALID", "旋转按下位置不是有效数值");
    auto captured = CaptureWorldPlacementProperties(document, ids);
    if (!captured.Succeeded()) return {std::nullopt, std::move(captured.diagnostics)};
    WorldPlacementRotationOrigin origin;
    origin.ids.assign(ids.begin(), ids.end()); origin.original = std::move(*captured.value);
    for (const auto& transform : origin.original.transforms) {
        origin.center[0] += transform.position[0]; origin.center[1] += transform.position[1];
    }
    for (auto& component : origin.center) component /= static_cast<double>(ids.size());
    const double dx = pressed[0] - origin.center[0], dy = pressed[1] - origin.center[1];
    // 单选保留原有“朝向指针”交互；多选以按下处相对组中心的角度为零点。
    origin.bearing = ids.size() == 1 || std::hypot(dx, dy) < 1.0 ? origin.original.transforms.front().rotation : std::atan2(dy, dx);
    return Output::Success(std::move(origin));
}

Result<std::vector<MapData::PlacementMove>> BuildSelectedPlacementRotationMoves(
    const MapDocument& document, const MapData::TerrainGridSnapshot& terrain,
    const WorldPlacementRotationOrigin& origin, std::array<double, 2> cursor, double stepRadians) {
    using Planned = Result<std::vector<MapData::PlacementMove>>;
    if (!std::isfinite(cursor[0]) || !std::isfinite(cursor[1]) || !std::isfinite(origin.bearing)
        || !std::isfinite(origin.center[0]) || !std::isfinite(origin.center[1])
        || origin.ids.size() != origin.original.transforms.size()
        || !std::isfinite(stepRadians) || stepRadians < 0 || stepRadians > std::numbers::pi_v<double>)
        return Planned::Failure("World", "WORLD_ROTATION_INVALID", "旋转草稿不是有效输入，整组未旋转");
    auto captured = CaptureWorldPlacementProperties(document, origin.ids);
    if (!captured.Succeeded()) return {std::nullopt, std::move(captured.diagnostics)};
    double delta = std::remainder(std::atan2(cursor[1] - origin.center[1], cursor[0] - origin.center[0])
        - origin.bearing, 2.0 * std::numbers::pi_v<double>);
    if (stepRadians > 0) {
        const auto baseline = origin.ids.size() == 1 ? double(origin.original.transforms.front().rotation) : 0.0;
        delta = std::round((baseline + delta) / stepRadians) * stepRadians - baseline;
    }
    const double sine = std::sin(delta), cosine = std::cos(delta);
    std::vector<MapData::PlacementMove> moves; moves.reserve(origin.ids.size());
    for (std::size_t index = 0; index < origin.ids.size(); ++index) {
        const auto& source = origin.original.transforms[index]; const auto& current = captured.value->transforms[index];
        if (!std::equal(source.position.begin(), source.position.end(), current.position.begin(), [](float left, float right) {
                return std::bit_cast<std::uint32_t>(left) == std::bit_cast<std::uint32_t>(right);
            }) || std::bit_cast<std::uint32_t>(source.rotation) != std::bit_cast<std::uint32_t>(current.rotation))
            return Planned::Failure("World", "WORLD_ROTATION_STALE", "按下后摆放已经改变，整组未旋转");
        if (delta == 0) {
            auto unchanged = GroundedPosition(terrain, source, source.position);
            if (!unchanged.Succeeded()) return {std::nullopt, std::move(unchanged.diagnostics)};
            moves.push_back({origin.ids[index], *unchanged.value, std::nullopt}); continue;
        }
        const double dx = source.position[0] - origin.center[0], dy = source.position[1] - origin.center[1];
        const std::array<double, 2> target{origin.center[0] + dx * cosine - dy * sine, origin.center[1] + dx * sine + dy * cosine};
        if (!std::all_of(target.begin(), target.end(), [](double value) { return std::isfinite(value) && std::abs(value) <= 1000000.0; }))
            return Planned::Failure("World", "WORLD_ROTATION_INVALID", "旋转目标坐标无效，整组未旋转");
        auto position = source.position; position[0] = static_cast<float>(target[0]); position[1] = static_cast<float>(target[1]);
        auto grounded = GroundedPosition(terrain, source, position);
        if (!grounded.Succeeded()) return {std::nullopt, std::move(grounded.diagnostics)};
        constexpr double turn = 2.0 * std::numbers::pi_v<double>;
        const auto angle = static_cast<float>(std::fmod(std::fmod(double(source.rotation) + delta, turn) + turn, turn));
        moves.push_back({origin.ids[index], *grounded.value, angle});
    }
    return Planned::Success(std::move(moves));
}

} // namespace War3Map::Modules
