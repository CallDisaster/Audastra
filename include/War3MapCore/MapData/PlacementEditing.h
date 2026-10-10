#pragma once

#include "War3MapCore/MapData/MapDataEditSession.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace War3Map::MapData {

// 世界视图、物体列表与未来 CLI/Agent 共用的摆放编辑。只经 MapDataEditSession 提交，一次调用是一个事务。
enum class PlacementFile : std::uint8_t { Units, Doodads }; // war3mapUnits.doo（单位/物品）或 war3map.doo

// 稳定标识：文件 + 序号 + rawcode + 创建编号必须同时吻合；删除/插入导致序号错位时判为过期，
// 不按 rawcode 或位置猜测替代对象。
struct PlacementId {
    PlacementFile file = PlacementFile::Units;
    std::uint32_t index = 0;
    std::string rawcode;
    std::int32_t creationId = 0;
    [[nodiscard]] bool operator==(const PlacementId&) const = default;
};
[[nodiscard]] PlacementId MakePlacementId(PlacementFile file, std::uint32_t index, const UnitPlacement& unit);
[[nodiscard]] PlacementId MakePlacementId(PlacementFile file, std::uint32_t index, const DoodadPlacement& doodad);

// 当前文档中的摆放（只读）；标识过期或文件不可用时失败。
[[nodiscard]] Result<PlacementTransform> FindPlacement(const MapDocument& document, const PlacementId& id);

struct PlacementMove {
    PlacementId id;
    std::array<float, 3> position{};
    std::optional<float> rotation; // 弧度；空表示不变
};

// 移动一个或多个摆放（可跨两个文件，仍为一个事务）。坐标须有限且 |v| <= 1e6。
// 起始位置（sloc）的坐标联动由 EditUnits 同一规则写入玩家设置。
[[nodiscard]] Status MovePlacements(MapDataEditSession& session, std::span<const PlacementMove> moves);
// 删除摆放；起始位置受 EditUnits 规则保护，不能删除。标识重复或过期时整体拒绝。
[[nodiscard]] Status DeletePlacements(MapDataEditSession& session, std::span<const PlacementId> ids);

// 新增摆放：rawcode、坐标等由调用方按物编默认值填好；创建编号取该文件现有最大值 + 1。
// 返回新摆放的标识（追加在文件末尾）。
[[nodiscard]] Result<PlacementId> AddUnitPlacement(MapDataEditSession& session, UnitPlacement placement);
[[nodiscard]] Result<PlacementId> AddDoodadPlacement(MapDataEditSession& session, DoodadPlacement placement);

// 按同一世界坐标偏移复制最多 4096 个单位/物品/装饰物/可破坏物，可跨两个文件。
// 所有身份、坐标、创建编号及起始位置规则先校验，再一次提交；返回标识与请求顺序一致。
// sloc 不可复制。其余完整字段（所属玩家、随机单位、掉落、物品和技能等）原样保留。
[[nodiscard]] Result<std::vector<PlacementId>> CopyPlacements(MapDataEditSession& session,
    std::span<const PlacementId> ids, std::array<float, 3> offset);

namespace detail {
// UI、保存与 prepared facade 共用唯一移动/复制算法；仅在来源/预算预检后构造副本。
struct PlacementEditCandidate {
    std::optional<UnitPlacements> units;
    std::optional<DoodadPlacements> doodads;
    std::vector<PlacementRowLineage> lineage;
    std::vector<PlacementId> newIds;
};
[[nodiscard]] Result<PlacementEditCandidate> PreparePlacementMoves(
    const MapDocument& document, std::span<const PlacementMove> moves);
[[nodiscard]] Result<PlacementEditCandidate> PreparePlacementCopies(
    const MapDocument& document, std::span<const PlacementId> ids, std::array<float, 3> offset);
}

// 区域与镜头：序号与名称（区域另核创建编号）同时吻合才修改，否则判为过期。
struct RegionId {
    std::uint32_t index = 0;
    std::string name;
    std::int32_t creationNumber = 0;
    [[nodiscard]] bool operator==(const RegionId&) const = default;
};
struct CameraId {
    std::uint32_t index = 0;
    std::string name;
    [[nodiscard]] bool operator==(const CameraId&) const = default;
};
[[nodiscard]] RegionId MakeRegionId(std::uint32_t index, const RegionRecord& region);
[[nodiscard]] CameraId MakeCameraId(std::uint32_t index, const CameraRecord& camera);
// 创建者显式提供完整经典记录；不推断原版默认镜头/区域属性。名称与创建编号须唯一。
[[nodiscard]] Result<RegionId> AddRegion(MapDataEditSession& session, RegionRecord record);
[[nodiscard]] Result<CameraId> AddCamera(MapDataEditSession& session, CameraRecord record);
// 区域边界须有限、|v| <= 1e6、left < right、bottom < top；不做网格吸附（由调用方决定）。
[[nodiscard]] Status SetRegionBounds(MapDataEditSession& session, const RegionId& id,
    float left, float bottom, float right, float top);
// 只改镜头目标点 XY，其余镜头参数不变。
[[nodiscard]] Status SetCameraTarget(MapDataEditSession& session, const CameraId& id, float x, float y);

} // namespace War3Map::MapData
