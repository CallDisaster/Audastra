#pragma once

#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/Services/MapDataQuery.h"

#include <span>

namespace War3Map::Services {

// 文档修订和地形会话修订一起绑定。换图、其他领域编辑、撤销重做都会使旧请求失效。
struct TerrainEditBinding {
    MapDataQueryBinding document;
    std::uint64_t sessionIdentity = 0;
    std::uint64_t terrainRevision = 0;
    std::uint64_t terrainSourceRevision = 0;
    [[nodiscard]] bool operator==(const TerrainEditBinding&) const = default;
};

struct TerrainEditState {
    TerrainEditBinding binding;
    MapData::TerrainHeader header;
    std::size_t vertexCount = 0;
    bool pathingAvailable = false;
    // 仅地形派生数据这一道门，不代表脚本、来源、归档或完整地图保存已经通过。
    Status terrainPathingGate;
};

inline constexpr std::size_t TerrainQueryMaximumPoints = 256;
inline constexpr std::size_t TerrainEditMaximumPoints = 4096;

// 只核文档/会话CAS，不扫描地形和派生WPM；用于已在worker准备的密封命令发布。
[[nodiscard]] Status ValidateTerrainEditBinding(const MapData::MapDataEditSession& session,
    const MapDataQueryBinding& document, const TerrainEditBinding& expected);

class PreparedTerrainEdit {
public:
    [[nodiscard]] const TerrainEditBinding& Binding() const noexcept { return binding_; }
    [[nodiscard]] const MapData::MapDataEditSession::PreparedTerrainEdit& Plan() const noexcept { return plan_; }
    [[nodiscard]] const std::optional<MapData::TerrainWaterStrokeOptions>& WaterOptions() const noexcept { return waterOptions_; }
private:
    PreparedTerrainEdit(TerrainEditBinding binding, MapData::MapDataEditSession::PreparedTerrainEdit plan)
        : binding_(std::move(binding)), plan_(std::move(plan)) {}
    TerrainEditBinding binding_;
    MapData::MapDataEditSession::PreparedTerrainEdit plan_;
    std::optional<MapData::TerrainWaterStrokeOptions> waterOptions_;
    friend Result<PreparedTerrainEdit> PrepareTerrainEdit(const MapData::MapDataEditSession&,
        const MapDataQueryBinding&, const TerrainEditBinding&, const MapData::TerrainBrush&,
        std::span<const MapData::TerrainStrokePoint>, bool, std::size_t);
    friend Result<PreparedTerrainEdit> PrepareTerrainWaterEdit(const MapData::MapDataEditSession&,
        const MapDataQueryBinding&, const TerrainEditBinding&, const MapData::TerrainPathingResources&,
        const MapData::TerrainWaterStrokeOptions&, std::span<const MapData::TerrainStrokePoint>, bool,
        const MapData::TerrainWaterBrushLimits&);
    friend Result<PreparedTerrainEdit> PrepareTerrainCliffEdit(const MapData::MapDataEditSession&,
        const MapDataQueryBinding&, const TerrainEditBinding&, const MapData::TerrainPathingResources&,
        const MapData::TerrainBrush&, std::span<const MapData::TerrainStrokePoint>, bool,
        const MapData::TerrainWaterBrushLimits&);
    friend Result<PreparedTerrainEdit> PrepareTerrainCornerRampEdit(const MapData::MapDataEditSession&,
        const MapDataQueryBinding&, const TerrainEditBinding&, const MapData::TerrainPathingResources&,
        const MapData::TerrainBrush&, std::span<const MapData::TerrainStrokePoint>, bool,
        const MapData::TerrainWaterBrushLimits&, MapData::TerrainCornerRampAlgorithm);
    friend Result<PreparedTerrainEdit> PrepareTerrainStraightRampEdit(const MapData::MapDataEditSession&,
        const MapDataQueryBinding&, const TerrainEditBinding&, const MapData::TerrainPathingResources&,
        const MapData::TerrainBrush&, std::span<const MapData::TerrainStrokePoint>, bool,
        const MapData::TerrainWaterBrushLimits&, MapData::TerrainStraightRampAlgorithm);
};

// 宿主在同一串行临界区提供实际文档身份；服务不读盘、不替宿主猜来源或修订。
[[nodiscard]] Result<TerrainEditState> CaptureTerrainEditState(
    const MapData::MapDataEditSession& session, const MapDataQueryBinding& document);
[[nodiscard]] Result<std::vector<MapData::TerrainPointSample>> QueryTerrainPoints(
    const MapData::MapDataEditSession& session, const MapDataQueryBinding& document,
    const TerrainEditBinding& expected, std::span<const MapData::TerrainStrokePoint> points);

// dry-run 与实际编辑共用同一密封候选。准备不会消费修订、撤销身份或清空 redo。
[[nodiscard]] Result<PreparedTerrainEdit> PrepareTerrainEdit(
    const MapData::MapDataEditSession& session, const MapDataQueryBinding& document,
    const TerrainEditBinding& expected, const MapData::TerrainBrush& brush,
    std::span<const MapData::TerrainStrokePoint> points, bool followDoodads = false,
    std::size_t maximumLevelPropagationWork = MapData::TerrainLevelPropagationMaximumWork);
// resources须由宿主从实际地图/游戏来源和本笔当前地形捕获；服务不读盘。
// 第一采样点只捕获一次anchor，整笔固定seed/anchor/points，仍不授予保存资格。
[[nodiscard]] Result<PreparedTerrainEdit> PrepareTerrainWaterEdit(
    const MapData::MapDataEditSession& session, const MapDataQueryBinding& document,
    const TerrainEditBinding& expected, const MapData::TerrainPathingResources& resources,
    const MapData::TerrainWaterStrokeOptions& options, std::span<const MapData::TerrainStrokePoint> points,
    bool followDoodads = false, const MapData::TerrainWaterBrushLimits& limits = {});
// 普通悬崖仍是CliffLevel命令；仅规则来源和内部后处理与水刷共享。
[[nodiscard]] Result<PreparedTerrainEdit> PrepareTerrainCliffEdit(
    const MapData::MapDataEditSession& session, const MapDataQueryBinding& document,
    const TerrainEditBinding& expected, const MapData::TerrainPathingResources& resources,
    const MapData::TerrainBrush& brush, std::span<const MapData::TerrainStrokePoint> points,
    bool followDoodads = false, const MapData::TerrainWaterBrushLimits& limits = {});
// 单点一级拐角；完整规则/DOO owner、历史与原子提交和水刷共用。
[[nodiscard]] Result<PreparedTerrainEdit> PrepareTerrainCornerRampEdit(
    const MapData::MapDataEditSession& session, const MapDataQueryBinding& document,
    const TerrainEditBinding& expected, const MapData::TerrainPathingResources& resources,
    const MapData::TerrainBrush& brush, std::span<const MapData::TerrainStrokePoint> points,
    bool followDoodads = false, const MapData::TerrainWaterBrushLimits& limits = {},
    MapData::TerrainCornerRampAlgorithm algorithm = MapData::TerrainCornerRampAlgorithm::WetTerrain);
// 新直坡保留旧Ramp语义，实际规则/来源、单点、预算、提交与拐角共用。
[[nodiscard]] Result<PreparedTerrainEdit> PrepareTerrainStraightRampEdit(
    const MapData::MapDataEditSession& session, const MapDataQueryBinding& document,
    const TerrainEditBinding& expected, const MapData::TerrainPathingResources& resources,
    const MapData::TerrainBrush& brush, std::span<const MapData::TerrainStrokePoint> points,
    bool followDoodads = false, const MapData::TerrainWaterBrushLimits& limits = {},
    MapData::TerrainStraightRampAlgorithm algorithm = MapData::TerrainStraightRampAlgorithm::Independent);
[[nodiscard]] Result<MapData::TerrainStrokeResult> ApplyTerrainEdit(
    MapData::MapDataEditSession& session, const MapDataQueryBinding& document,
    const PreparedTerrainEdit& prepared,
    const std::function<Status(const MapData::MapDataEditSession::EditReceipt&)>& prepublish = {});

} // namespace War3Map::Services
