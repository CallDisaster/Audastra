#include "War3MapCore/Services/TerrainPathingSave.h"

#include "War3MapCore/ObjectData/NativeDestructablePathing.h"
#include "War3MapCore/MapData/MapDataEditSession.h"

#include <new>

namespace War3Map::Services {
namespace {
template<class T, class Function> Result<T> Guard(Function&& function) {
    try { return function(); }
    catch (const std::bad_alloc&) {
        return Result<T>::Failure("Terrain.PathingSave", "TERRAIN_NATIVE_OBJECT_BUDGET", "严格对象保存适配分配失败，未建立部分保存计划。");
    }
}
} // namespace

Result<MapData::TerrainPathingResources> TerrainPathingSave::CaptureNative(const MapArchiveSession& source,
    const MapData::TerrainGridSnapshot& before, const MapData::TerrainGridSnapshot& after,
    const std::filesystem::path& gameRoot, std::size_t maximumBytes, const std::vector<std::byte>* currentDoodads) {
    return Guard<MapData::TerrainPathingResources>([&] {
        return MapData::TerrainPathingResources::CaptureNativeWithObjects(source, before, after, gameRoot,
            maximumBytes, ObjectData::InspectNativeTerrainObjects, currentDoodads);
    });
}

Result<MapData::TerrainPathingResources> TerrainPathingSave::CaptureNative(const WorkspaceSnapshot& source,
    const MapData::TerrainGridSnapshot& before, const MapData::TerrainGridSnapshot& after,
    const std::filesystem::path& gameRoot, std::size_t maximumBytes, const std::vector<std::byte>* currentDoodads) {
    return Guard<MapData::TerrainPathingResources>([&] {
        return MapData::TerrainPathingResources::CaptureNativeWithObjects(source, before, after, gameRoot,
            maximumBytes, ObjectData::InspectNativeTerrainObjects, currentDoodads);
    });
}

Result<MapData::TerrainSavePlan> TerrainPathingSave::PrepareForSave(const MapArchiveSession& source,
    const MapData::MapDataEditSession& edits, const std::filesystem::path& gameRoot,
    const MapData::TerrainSavePlanLimits& limits) {
    return Guard<MapData::TerrainSavePlan>([&] {
        const MapData::TerrainSavePlan::NativeResourceCapture capture = [&](const auto& before, const auto& after, const auto& doodads, auto maximumBytes) {
            return CaptureNative(source, before, after, gameRoot, maximumBytes, doodads);
        };
        return MapData::TerrainSavePlan::PrepareArchive(source, edits, limits, &gameRoot, &capture);
    });
}

Result<MapData::TerrainSavePlan> TerrainPathingSave::PrepareForSave(const WorkspaceSnapshot& source,
    const MapData::MapDataEditSession& edits, const std::filesystem::path& gameRoot,
    const MapData::TerrainSavePlanLimits& limits) {
    return Guard<MapData::TerrainSavePlan>([&] {
        const MapData::TerrainSavePlan::NativeResourceCapture capture = [&](const auto& before, const auto& after, const auto& doodads, auto maximumBytes) {
            return CaptureNative(source, before, after, gameRoot, maximumBytes, doodads);
        };
        return MapData::TerrainSavePlan::PrepareWorkspace(source, edits, limits, &gameRoot, &capture);
    });
}

Result<MapData::TerrainPathingResources> TerrainPathingSave::CaptureWaterBrushRules(const MapArchiveSession& source,
    const MapData::TerrainGridSnapshot& sourceTerrain, const MapData::TerrainGridSnapshot& currentTerrain,
    const MapData::DoodadPlacements& currentDoodads, const std::filesystem::path& gameRoot,
    bool enforceWaterLimits, std::size_t maximumBytes) {
    return Guard<MapData::TerrainPathingResources>([&]() -> Result<MapData::TerrainPathingResources> {
        auto rules = MapData::TerrainPathingResources::CaptureWaterBrushRules(source, sourceTerrain, currentTerrain,
            gameRoot, enforceWaterLimits, maximumBytes);
        if (!rules.Succeeded() || !rules.value) return rules;
        const auto bound = rules.value->CaptureWaterSpecialInputs([&](auto names, const auto& limits) {
            return source.CaptureExactMembers(names, limits);
        }, currentTerrain, currentDoodads, ObjectData::InspectNativeTerrainObjects);
        if (!bound.Succeeded()) return {{}, bound.diagnostics};
        return rules;
    });
}

Result<MapData::TerrainPathingResources> TerrainPathingSave::CaptureWaterBrushRules(const WorkspaceSnapshot& source,
    const MapData::TerrainGridSnapshot& sourceTerrain, const MapData::TerrainGridSnapshot& currentTerrain,
    const MapData::DoodadPlacements& currentDoodads, const std::filesystem::path& gameRoot,
    bool enforceWaterLimits, std::size_t maximumBytes) {
    return Guard<MapData::TerrainPathingResources>([&]() -> Result<MapData::TerrainPathingResources> {
        auto rules = MapData::TerrainPathingResources::CaptureWaterBrushRules(source, sourceTerrain, currentTerrain,
            gameRoot, enforceWaterLimits, maximumBytes);
        if (!rules.Succeeded() || !rules.value) return rules;
        const auto bound = rules.value->CaptureWaterSpecialInputs([&](auto names, const auto& limits) {
            return MapData::TerrainPathingResources::ReadDirectoryMembers(source, names, limits);
        }, currentTerrain, currentDoodads, ObjectData::InspectNativeTerrainObjects);
        if (!bound.Succeeded()) return {{}, bound.diagnostics};
        return rules;
    });
}

Result<TerrainPatch> TerrainPathingSave::Seal(const MapDocument& original,
    std::span<const std::byte> terrain, std::span<const std::byte> pathing,
    const WorkspaceSnapshot& source, const MapData::MapDataEditSession& current,
    std::span<const MapData::TerrainStrokeIntent> strokes, const std::filesystem::path& gameRoot) {
    return Guard<TerrainPatch>([&]() -> Result<TerrainPatch> {
        const auto plan = PrepareForSave(source, current, gameRoot);
        if (!plan.Succeeded() || !plan.value) return {{}, plan.diagnostics};
        return MapData::TerrainPatchCodec::SealWithPlan(original, terrain, pathing, source, current, strokes, gameRoot, &*plan.value);
    });
}

} // namespace War3Map::Services
