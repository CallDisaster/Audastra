#pragma once

#include "War3MapCore/MapData/TerrainPatchCodec.h"
#include "War3MapCore/MapData/TerrainSavePlan.h"

namespace War3Map::Services {

// Qt、CLI和MCP共用的薄适配：Core捕获实际来源，ObjectData读取严格typed覆盖，
// 仍由同一个TerrainSavePlan逐格验证旧WPM、安装输出和核对最终回读。
class TerrainPathingSave {
public:
    [[nodiscard]] static Result<MapData::TerrainSavePlan> PrepareForSave(const MapArchiveSession& source,
        const MapData::MapDataEditSession& edits, const std::filesystem::path& gameRoot,
        const MapData::TerrainSavePlanLimits& limits = {});
    [[nodiscard]] static Result<MapData::TerrainSavePlan> PrepareForSave(const WorkspaceSnapshot& source,
        const MapData::MapDataEditSession& edits, const std::filesystem::path& gameRoot,
        const MapData::TerrainSavePlanLimits& limits = {});
    [[nodiscard]] static Result<MapData::TerrainPathingResources> CaptureNative(const MapArchiveSession& source,
        const MapData::TerrainGridSnapshot& before, const MapData::TerrainGridSnapshot& after,
        const std::filesystem::path& gameRoot, std::size_t maximumBytes, const std::vector<std::byte>* currentDoodads = nullptr);
    [[nodiscard]] static Result<MapData::TerrainPathingResources> CaptureNative(const WorkspaceSnapshot& source,
        const MapData::TerrainGridSnapshot& before, const MapData::TerrainGridSnapshot& after,
        const std::filesystem::path& gameRoot, std::size_t maximumBytes, const std::vector<std::byte>* currentDoodads = nullptr);
    [[nodiscard]] static Result<MapData::TerrainPathingResources> CaptureWaterBrushRules(const MapArchiveSession& source,
        const MapData::TerrainGridSnapshot& sourceTerrain, const MapData::TerrainGridSnapshot& currentTerrain,
        const MapData::DoodadPlacements& currentDoodads, const std::filesystem::path& gameRoot,
        bool enforceWaterLimits, std::size_t maximumBytes);
    [[nodiscard]] static Result<MapData::TerrainPathingResources> CaptureWaterBrushRules(const WorkspaceSnapshot& source,
        const MapData::TerrainGridSnapshot& sourceTerrain, const MapData::TerrainGridSnapshot& currentTerrain,
        const MapData::DoodadPlacements& currentDoodads, const std::filesystem::path& gameRoot,
        bool enforceWaterLimits, std::size_t maximumBytes);
    [[nodiscard]] static Result<TerrainPatch> Seal(const MapDocument& original,
        std::span<const std::byte> terrain, std::span<const std::byte> pathing,
        const WorkspaceSnapshot& source, const MapData::MapDataEditSession& current,
        std::span<const MapData::TerrainStrokeIntent> strokes, const std::filesystem::path& gameRoot = {});
};

} // namespace War3Map::Services
