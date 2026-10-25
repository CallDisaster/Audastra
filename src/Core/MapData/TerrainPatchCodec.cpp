#include "War3MapCore/MapData/TerrainPatchCodec.h"
#include "War3MapCore/MapData/TerrainSavePlan.h"
#include "War3MapCore/MapData/TerrainDoodadsJournal.h"
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
    std::span<const TerrainStrokeIntent> strokes, const std::filesystem::path& gameRoot) {
    return SealWithPlan(original, terrain, pathing, source, current, strokes, gameRoot, nullptr);
}

Result<TerrainPatch> TerrainPatchCodec::SealWithPlan(const MapDocument& original,
    std::span<const std::byte> terrain, std::span<const std::byte> pathing,
    const WorkspaceSnapshot& source, const MapDataEditSession& current,
    std::span<const TerrainStrokeIntent> strokes, const std::filesystem::path& gameRoot,
    const TerrainSavePlan* prepared, const MapDataEditSession* trustedReplay) {
    if (strokes.empty() || strokes.size() > 128)
        return Result<TerrainPatch>::Failure("Terrain.Patch", "TERRAIN_PATCH_STROKE_BUDGET", "地形目录事务需要 1 到 128 笔原始命令。");
    const bool water = std::any_of(strokes.begin(), strokes.end(), [](const auto& stroke) { return stroke.waterBrush.has_value(); });
    const bool bounds = std::any_of(strokes.begin(), strokes.end(), [](const auto& stroke) { return stroke.bounds.has_value(); });
    if (std::any_of(strokes.begin(), strokes.end(), [](const auto& stroke) { return stroke.resize.has_value(); }))
        return Result<TerrainPatch>::Failure("Terrain.Patch", "TERRAIN_PATCH_RESIZE_SAVE_REQUIRED",
            "外矩形扩裁需要完整地图保存、脚本和派生成员重建；独立目录补丁不支持扩裁。");
    if (bounds)
        return Result<TerrainPatch>::Failure("Terrain.Patch", "TERRAIN_PATCH_BOUNDS_SCRIPT_REQUIRED",
            "地图范围需连同最终W3I重建游戏镜头初始化脚本，请使用普通地图保存；独立地形目录补丁不写脚本。");
    if (!trustedReplay && (water || current.specialDoodads_ != current.savedSpecialDoodads_))
        return Result<TerrainPatch>::Failure("Terrain.Patch", "TERRAIN_PATCH_WATER_REPLAY_UNSUPPORTED",
            "Core普通重放入口不捕获水刷资源；须通过Services从实际来源逐笔重放，不能从DOO/W3E覆盖恢复特殊模型删除授权。");
    std::size_t pointCount = 0;
    for (const auto& stroke : strokes) {
        if (stroke.blockPaste) {
            TerrainStrokeIntent expected;
            expected.blockPaste = stroke.blockPaste; expected.followDoodads = stroke.followDoodads;
            // 仅Services已从实际来源严格重放的typed矩形可以没有采样点。
            // 公开Core入口不能借block字段绕过普通笔刷或special来源门。
            if (!trustedReplay || !EqualTerrainJournalIntent(stroke, expected)
                || !ValidateTerrainBlockPasteIntent(*stroke.blockPaste).Succeeded())
                return Result<TerrainPatch>::Failure("Terrain.Patch", "TERRAIN_PATCH_BLOCK_INVALID",
                    "地形块补丁需要实际来源重放的独立矩形命令，不能夹带笔刷、原始载荷或其它变体。");
            continue;
        }
        if (stroke.points.empty() || stroke.points.size() > 4096 - pointCount)
            return Result<TerrainPatch>::Failure("Terrain.Patch", "TERRAIN_PATCH_POINT_BUDGET", "地形目录事务累计采样点超过 4096 或包含空笔划。");
        pointCount += stroke.points.size();
    }
    std::unique_ptr<MapDataEditSession> ordinaryReplay;
    if (!trustedReplay) {
        ordinaryReplay = std::make_unique<MapDataEditSession>(original);
        const auto attached = ordinaryReplay->AttachTerrain(terrain);
        if (!attached.Succeeded()) return {{}, attached.diagnostics};
        const auto attachedPathing = ordinaryReplay->AttachPathing(pathing);
        if (!attachedPathing.Succeeded()) return {{}, attachedPathing.diagnostics};
    }
    const auto& replay = trustedReplay ? *trustedReplay : *ordinaryReplay;
    if (!current.terrainOriginal_ || !current.terrain_ || !current.pathingOriginal_ || !current.pathing_
        || current.original_ != replay.original_ || current.memberNames_ != replay.memberNames_
        || *current.pathingOriginal_ != *replay.pathingOriginal_)
        return Result<TerrainPatch>::Failure("Terrain.Patch", "TERRAIN_PATCH_BASELINE_MISMATCH", "地形会话与重放命令不是同一来源基线。");
    const auto currentOriginal = SerializeMapTerrain(*current.terrainOriginal_);
    if (!currentOriginal.Succeeded() || !currentOriginal.value) return {{}, currentOriginal.diagnostics};
    if (!std::equal(terrain.begin(), terrain.end(), currentOriginal.value->begin(), currentOriginal.value->end()))
        return Result<TerrainPatch>::Failure("Terrain.Patch", "TERRAIN_PATCH_BASELINE_MISMATCH", "地形会话原 W3E 与目录来源不一致。");
    if (!trustedReplay) {
        for (const auto& stroke : strokes) {
            for (const auto& point : stroke.points) {
                const auto checked = replay.InspectTerrainPoint(point);
                if (!checked.Succeeded()) return {{}, checked.diagnostics};
            }
            const auto edited = ordinaryReplay->EditTerrain(stroke.brush, stroke.points, stroke.followDoodads);
            if (!edited.Succeeded()) return {{}, edited.diagnostics};
        }
    }
    // 净结果必须只来自这些笔划；不能夹带任何地图数据字段或任意 WPM 覆盖。
    if (current.current_ != replay.current_ || current.pathing_ != replay.pathing_ || current.Removals() != replay.Removals()
        || !current.pathingProtected_ || !current.originalPathingRestrictions_ || !current.pathingRestrictions_
        || *current.pathingProtected_ != *replay.pathingProtected_
        || *current.originalPathingRestrictions_ != *replay.originalPathingRestrictions_
        || *current.pathingRestrictions_ != *replay.pathingRestrictions_)
        return Result<TerrainPatch>::Failure("Terrain.Patch", "TERRAIN_PATCH_REPLAY_MISMATCH", "当前地图成员、WPM或通行限制归属不能由原始地形命令完整重放。");
    const auto replayTerrain = SerializeMapTerrain(*replay.terrain_);
    const auto currentTerrain = SerializeMapTerrain(*current.terrain_);
    if (!replayTerrain.Succeeded() || !replayTerrain.value) return {{}, replayTerrain.diagnostics};
    if (!currentTerrain.Succeeded() || !currentTerrain.value) return {{}, currentTerrain.diagnostics};
    if (*replayTerrain.value != *currentTerrain.value)
        return Result<TerrainPatch>::Failure("Terrain.Patch", "TERRAIN_PATCH_REPLAY_MISMATCH", "当前 W3E 不能由原始地形命令完整重放。");
    // Services私有桥只接其自己真实重放的会话。完整DOO比较不替代双方私有谱系验证。
    for (const auto* session : {&current, &replay}) {
        const auto index = MapDataEditSession::Index(MapDataKind::Doodads);
        if (session->original_[index] && session->current_[index]) {
            const auto checked = session->VerifyTerrainDoodadChange(*session->original_[index], *session->current_[index]);
            if (!checked.Succeeded()) return {{}, checked.diagnostics};
        }
    }
    // Units仍只允许跟随；DOO可包含真实水刷命令产生且与独立重放逐字相等的special删除。
    const bool follows = std::any_of(strokes.begin(), strokes.end(),
        [](const auto& stroke) { return stroke.followDoodads; });
    for (std::size_t index = 0; index < current.current_.size(); ++index) {
        if (current.current_[index] == current.original_[index]) continue;
        if (!((follows && index == MapDataEditSession::Index(MapDataKind::Units))
                || ((follows || water) && index == MapDataEditSession::Index(MapDataKind::Doodads)))
            || !current.original_[index] || !current.current_[index])
            return Result<TerrainPatch>::Failure("Terrain.Patch", "TERRAIN_PATCH_MEMBER_SCOPE", "地形目录事务只支持完整重放产生的摆放高度或水刷特殊模型变化。");
    }
    std::optional<TerrainSavePlan> captured;
    if (!prepared) {
        auto plan = TerrainSavePlan::PrepareForSave(source, current, gameRoot);
        if (!plan.Succeeded() || !plan.value) return {{}, plan.diagnostics};
        captured = std::move(*plan.value);
        prepared = &*captured;
    }
    auto replacements = current.Replacements();
    const auto installed = prepared->InstallPathingOutput(current, replacements);
    if (!installed.Succeeded()) return {{}, installed.diagnostics};
    const auto planned = prepared->VerifyForSave(current, source.aggregateSha256,
        replacements, current.Removals());
    if (!planned.Succeeded()) return {{}, planned.diagnostics};
    auto terrainFile = BindMember(source, "war3map.w3e", terrain, *currentTerrain.value);
    if (!terrainFile.Succeeded() || !terrainFile.value) return {{}, terrainFile.diagnostics};
    const auto generated = std::find_if(replacements.begin(), replacements.end(),
        [](const auto& member) { return member.name == "war3map.wpm"; });
    auto pathingFile = BindMember(source, "war3map.wpm", pathing,
        generated == replacements.end() ? *current.pathing_ : generated->data);
    if (!pathingFile.Succeeded() || !pathingFile.value) return {{}, pathingFile.diagnostics};
    std::vector<BinaryFilePatch> files;
    files.push_back(std::move(*terrainFile.value)); files.push_back(std::move(*pathingFile.value));
    for (const auto kind : {MapDataKind::Units, MapDataKind::Doodads}) {
        const auto index = MapDataEditSession::Index(kind);
        if (current.current_[index] == current.original_[index]) continue;
        const auto name = kind == MapDataKind::Units ? "war3mapunits.doo" : "war3map.doo";
        auto placement = BindMember(source, name, *current.original_[index], *current.current_[index]);
        if (!placement.Succeeded() || !placement.value) return {{}, placement.diagnostics};
        files.push_back(std::move(*placement.value));
    }
    auto result = Result<TerrainPatch>::Success(TerrainPatch(std::move(files)));
    result.diagnostics = prepared->Diagnostics();
    return result;
}

} // namespace War3Map::MapData
