#pragma once

#include "War3MapCore/MapData/MapTerrain.h"
#include "War3MapCore/MapData/PlacementEditing.h"

#include <array>
#include <span>
#include <vector>

namespace War3Map::Modules {

enum class WorldPlacementProperty { X, Y, Facing };

// 世界动画宿主键只映射 typed 摆放身份；预览请求使用另一命名空间。
[[nodiscard]] std::string WorldPlacementHostKey(const MapData::PlacementId& id);

// 仅借用当前 typed 文档；没有模型也能取得真实属性。不取得任何保存资格。
struct WorldPlacementProperties {
    struct StoredFacts {
        std::optional<std::int32_t> owner, hitPoints, mana, heroLevel, lifePercent;
    };
    std::vector<MapData::PlacementTransform> transforms;
    std::vector<StoredFacts> facts; // 与 transforms / 输入身份逐项对应；不授予字段写入能力。
    [[nodiscard]] bool Same(WorldPlacementProperty property) const noexcept;
};

[[nodiscard]] Result<WorldPlacementProperties> CaptureWorldPlacementProperties(
    const MapDocument& document, std::span<const MapData::PlacementId> ids);

// 输入 X/Y 为世界坐标，Facing 为弧度；先规划整个选择，不修改会话、撤销栈或磁盘。
// 上层仍须核对共同选择/文档修订，随后一次调用既有 MovePlacements。
[[nodiscard]] Result<std::vector<MapData::PlacementMove>> PlanWorldPlacementProperty(
    const MapDocument& document, const MapData::TerrainGridSnapshot& terrain,
    std::span<const MapData::PlacementId> ids, WorldPlacementProperty property, float value);

// 拖动与方向键共用同一整组选中项规划；各项保留当前相对地表高度。
[[nodiscard]] Result<std::vector<MapData::PlacementMove>> BuildSelectedPlacementOffsetMoves(
    const MapDocument& document, const MapData::TerrainGridSnapshot& terrain,
    std::span<const MapData::PlacementId> ids, std::array<double, 2> delta);

struct WorldPlacementRotationOrigin {
    std::vector<MapData::PlacementId> ids;
    WorldPlacementProperties original;
    std::array<double, 2> center{};
    double bearing = 0;
};
[[nodiscard]] Result<WorldPlacementRotationOrigin> CaptureWorldPlacementRotationOrigin(
    const MapDocument& document, std::span<const MapData::PlacementId> ids, std::array<double, 2> pressed);
[[nodiscard]] Result<std::vector<MapData::PlacementMove>> BuildSelectedPlacementRotationMoves(
    const MapDocument& document, const MapData::TerrainGridSnapshot& terrain,
    const WorldPlacementRotationOrigin& origin, std::array<double, 2> cursor, double stepRadians = 0);

} // namespace War3Map::Modules
