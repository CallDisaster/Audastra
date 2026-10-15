#include "War3MapCore/MapData/TerrainPatchCodec.h"
#include "War3MapCore/MapData/TerrainSavePlan.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/IO/FileBytes.h"

#include <algorithm>
#include <cctype>

namespace War3Map::MapData {
namespace {

Result<BinaryFilePatch> BindMember(const WorkspaceSnapshot& source, const char* name,
    std::span<const std::byte> before, std::vector<std::byte> after) {
    const WorkspaceFile* found = nullptr;
    for (const auto& file : source.files) {
        auto path = IO::GenericPathUtf8(file.relativePath);
        std::transform(path.begin(), path.end(), path.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (path != name && path != std::string("map/") + name) continue;
        if (found) return Result<BinaryFilePatch>::Failure("Terrain.Patch", "TERRAIN_PATCH_SOURCE_AMBIGUOUS", "地形原生成员有多个来源。");
        found = &file;
    }
    const auto sha = Sha256(before);
    if (!sha.Succeeded() || !sha.value) return {{}, sha.diagnostics};
    if (!found || found->size != before.size() || found->sha256 != *sha.value)
        return Result<BinaryFilePatch>::Failure("Terrain.Patch", "TERRAIN_PATCH_SOURCE_MISMATCH", "地形来源不属于原目录快照的精确成员。");
    return Result<BinaryFilePatch>::Success({found->relativePath, std::move(after), "MCP 共用地形笔划事务", *sha.value});
}

} // namespace

Result<TerrainPatch> TerrainPatchCodec::Seal(const MapDocument& original,
    std::span<const std::byte> terrain, std::span<const std::byte> pathing,
    const WorkspaceSnapshot& source, const MapDataEditSession& current,
    std::span<const TerrainStrokeIntent> strokes) {
    if (strokes.empty() || strokes.size() > 128)
        return Result<TerrainPatch>::Failure("Terrain.Patch", "TERRAIN_PATCH_STROKE_BUDGET", "地形目录事务需要 1 到 128 笔原始命令。");
    std::size_t pointCount = 0;
    for (const auto& stroke : strokes) {
        if (stroke.points.empty() || stroke.points.size() > 4096 - pointCount)
            return Result<TerrainPatch>::Failure("Terrain.Patch", "TERRAIN_PATCH_POINT_BUDGET", "地形目录事务累计采样点超过 4096 或包含空笔划。");
        pointCount += stroke.points.size();
    }
    MapDataEditSession replay(original);
    const auto attached = replay.AttachTerrain(terrain);
    if (!attached.Succeeded()) return {{}, attached.diagnostics};
    const auto attachedPathing = replay.AttachPathing(pathing);
    if (!attachedPathing.Succeeded()) return {{}, attachedPathing.diagnostics};
    if (!current.terrainOriginal_ || !current.terrain_ || !current.pathingOriginal_ || !current.pathing_
        || current.original_ != replay.original_ || current.memberNames_ != replay.memberNames_
        || *current.pathingOriginal_ != *replay.pathingOriginal_)
        return Result<TerrainPatch>::Failure("Terrain.Patch", "TERRAIN_PATCH_BASELINE_MISMATCH", "地形会话与重放命令不是同一来源基线。");
    const auto currentOriginal = SerializeMapTerrain(*current.terrainOriginal_);
    if (!currentOriginal.Succeeded() || !currentOriginal.value) return {{}, currentOriginal.diagnostics};
    if (!std::equal(terrain.begin(), terrain.end(), currentOriginal.value->begin(), currentOriginal.value->end()))
        return Result<TerrainPatch>::Failure("Terrain.Patch", "TERRAIN_PATCH_BASELINE_MISMATCH", "地形会话原 W3E 与目录来源不一致。");
    for (const auto& stroke : strokes) {
        for (const auto& point : stroke.points) {
            const auto checked = replay.InspectTerrainPoint(point);
            if (!checked.Succeeded()) return {{}, checked.diagnostics};
        }
        const auto edited = replay.EditTerrain(stroke.brush, stroke.points, stroke.followDoodads);
        if (!edited.Succeeded()) return {{}, edited.diagnostics};
    }
    // 净结果必须只来自这些笔划；不能夹带任何地图数据字段或任意 WPM 覆盖。
    if (current.current_ != replay.current_ || current.pathing_ != replay.pathing_)
        return Result<TerrainPatch>::Failure("Terrain.Patch", "TERRAIN_PATCH_REPLAY_MISMATCH", "当前地图成员或 WPM 不能由原始地形命令完整重放。");
    const auto replayTerrain = SerializeMapTerrain(*replay.terrain_);
    const auto currentTerrain = SerializeMapTerrain(*current.terrain_);
    if (!replayTerrain.Succeeded() || !replayTerrain.value) return {{}, replayTerrain.diagnostics};
    if (!currentTerrain.Succeeded() || !currentTerrain.value) return {{}, currentTerrain.diagnostics};
    if (*replayTerrain.value != *currentTerrain.value)
        return Result<TerrainPatch>::Failure("Terrain.Patch", "TERRAIN_PATCH_REPLAY_MISMATCH", "当前 W3E 不能由原始地形命令完整重放。");
    const auto gate = replay.VerifyTerrainPathingForSave();
    if (!gate.Succeeded()) return {{}, gate.diagnostics};
    // 当前规则只准 W3E/WPM；有地物联动时必须等独立派生一致性证明，不能偷放进 map-data。
    if (current.current_ != current.original_)
        return Result<TerrainPatch>::Failure("Terrain.Patch", "TERRAIN_PATCH_MEMBER_SCOPE", "地形目录事务尚不支持联动其它地图数据成员。");
    const auto plan = TerrainSavePlan::Prepare(source, current);
    if (!plan.Succeeded() || !plan.value) return {{}, plan.diagnostics};
    const auto planned = plan.value->VerifyForSave(current, source.aggregateSha256,
        current.Replacements(), current.Removals());
    if (!planned.Succeeded()) return {{}, planned.diagnostics};
    auto terrainFile = BindMember(source, "war3map.w3e", terrain, *currentTerrain.value);
    if (!terrainFile.Succeeded() || !terrainFile.value) return {{}, terrainFile.diagnostics};
    auto pathingFile = BindMember(source, "war3map.wpm", pathing, *current.pathing_);
    if (!pathingFile.Succeeded() || !pathingFile.value) return {{}, pathingFile.diagnostics};
    std::vector<BinaryFilePatch> files;
    files.push_back(std::move(*terrainFile.value)); files.push_back(std::move(*pathingFile.value));
    auto result = Result<TerrainPatch>::Success(TerrainPatch(std::move(files)));
    result.diagnostics = plan.value->Diagnostics();
    return result;
}

} // namespace War3Map::MapData
