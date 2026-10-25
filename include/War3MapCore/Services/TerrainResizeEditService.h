#pragma once

#include "War3MapCore/MapData/TerrainResizeResources.h"
#include "War3MapCore/Services/TerrainEditService.h"

namespace War3Map::Services {

class TerrainResizeEditService {
public:
    // 读取实际来源并核原始W3E；当前空间成员取同一冻结会话。不得在GUI线程读盘。
    [[nodiscard]] static Result<MapData::TerrainResizeResources> Capture(const MapArchiveSession& source,
        const MapData::MapDataEditSession& session, const std::filesystem::path& gameRoot,
        const MapData::TerrainResizeLimits& limits = {});
    [[nodiscard]] static Result<MapData::TerrainResizeResources> Capture(const WorkspaceSnapshot& source,
        const MapData::MapDataEditSession& session, const std::filesystem::path& gameRoot,
        const MapData::TerrainResizeLimits& limits = {});
};

class PreparedTerrainResizeEdit {
public:
    [[nodiscard]] const TerrainEditBinding& Binding() const noexcept { return binding_; }
    [[nodiscard]] const MapData::MapDataEditSession::PreparedTerrainResizeEdit& Plan() const noexcept { return plan_; }
private:
    PreparedTerrainResizeEdit(TerrainEditBinding binding, MapData::MapDataEditSession::PreparedTerrainResizeEdit plan)
        : binding_(std::move(binding)), plan_(std::move(plan)) {}
    TerrainEditBinding binding_;
    MapData::MapDataEditSession::PreparedTerrainResizeEdit plan_;
    friend Result<PreparedTerrainResizeEdit> PrepareTerrainResizeEdit(const MapData::MapDataEditSession&,
        const MapDataQueryBinding&, const TerrainEditBinding&, const MapData::TerrainResizeResources&,
        const MapData::TerrainResizeIntent&, const MapData::TerrainResizeLimits&);
};

[[nodiscard]] Result<PreparedTerrainResizeEdit> PrepareTerrainResizeEdit(const MapData::MapDataEditSession& session,
    const MapDataQueryBinding& document, const TerrainEditBinding& expected,
    const MapData::TerrainResizeResources& resources, const MapData::TerrainResizeIntent& intent,
    const MapData::TerrainResizeLimits& limits = {});
[[nodiscard]] Status ApplyTerrainResizeEdit(MapData::MapDataEditSession& session, const MapDataQueryBinding& document,
    const PreparedTerrainResizeEdit& prepared,
    const std::function<Status(const MapData::MapDataEditSession::EditReceipt&)>& prepublish = {});

} // namespace War3Map::Services
