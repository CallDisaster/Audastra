#pragma once

#include "War3MapCore/Services/TerrainEditService.h"

namespace War3Map::Services {

class PreparedTerrainBoundsEdit {
public:
    [[nodiscard]] const TerrainEditBinding& Binding() const noexcept { return binding_; }
    [[nodiscard]] const MapData::MapDataEditSession::PreparedTerrainBoundsEdit& Plan() const noexcept { return plan_; }
private:
    PreparedTerrainBoundsEdit(TerrainEditBinding binding, MapData::MapDataEditSession::PreparedTerrainBoundsEdit plan)
        : binding_(std::move(binding)), plan_(std::move(plan)) {}
    TerrainEditBinding binding_;
    MapData::MapDataEditSession::PreparedTerrainBoundsEdit plan_;
    friend Result<PreparedTerrainBoundsEdit> PrepareTerrainBoundsEdit(const MapData::MapDataEditSession&,
        const MapDataQueryBinding&, const TerrainEditBinding&, const MapData::TerrainPathingResources&,
        const MapData::TerrainBoundsEditIntent&, std::size_t, std::size_t);
};

// UI/CLI/MCP使用同一个密封命令。宿主捕获实际native资源，并在发布前排除文件/文档变化。
[[nodiscard]] Result<PreparedTerrainBoundsEdit> PrepareTerrainBoundsEdit(const MapData::MapDataEditSession& session,
    const MapDataQueryBinding& document, const TerrainEditBinding& expected,
    const MapData::TerrainPathingResources& resources, const MapData::TerrainBoundsEditIntent& intent,
    std::size_t maximumBytes = 512u * 1024u * 1024u, std::size_t maximumWork = 64u * 1024u * 1024u);
[[nodiscard]] Status ApplyTerrainBoundsEdit(MapData::MapDataEditSession& session, const MapDataQueryBinding& document,
    const PreparedTerrainBoundsEdit& prepared,
    const std::function<Status(const MapData::MapDataEditSession::EditReceipt&)>& prepublish = {});

} // namespace War3Map::Services
