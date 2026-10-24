#include "War3MapCore/Services/TerrainMinimapMarkersSave.h"

#include "War3MapCore/Services/TerrainMinimapMarkers.h"
#include "War3MapCore/MapData/MapDataEditSession.h"

#include <algorithm>
#include <array>
#include <cctype>

namespace War3Map::Services {
namespace {
bool IsMmp(std::string_view name) {
    constexpr std::string_view expected = "war3map.mmp";
    return name.size() == expected.size() && std::equal(name.begin(), name.end(), expected.begin(),
        [](unsigned char a, char b) { return std::tolower(a) == b; });
}
bool Unsupported(const Diagnostic& diagnostic) {
    constexpr std::string_view codes[]{
        "TERRAIN_MMP_BUDGET", "TERRAIN_MMP_ALLOCATION", "TERRAIN_MMP_GAME_ROOT", "TERRAIN_MMP_SOURCE_MISSING",
        "TERRAIN_MMP_TERRAIN_LAYOUT", "TERRAIN_MMP_UNITS_LAYOUT", "TERRAIN_MMP_TABLE_HEADER", "TERRAIN_MMP_TABLE_COLUMN",
        "TERRAIN_MMP_TABLE_ID", "TERRAIN_MMP_OBJECT_LAYOUT", "TERRAIN_MMP_OBJECT_VALUE", "TERRAIN_MMP_OBJECT_UNKNOWN",
        "TERRAIN_MMP_OBJECT_TYPE", "TERRAIN_MMP_METADATA", "TERRAIN_MMP_INHERITANCE", "TERRAIN_MMP_RACE_UNSUPPORTED",
        "TERRAIN_MMP_ABILITY_LIST", "TERRAIN_MMP_ABILITY_UNKNOWN", "TERRAIN_MMP_PROFILE_MISSING", "TERRAIN_MMP_PROFILE_DUPLICATE",
        "TERRAIN_MMP_CAMERA_CONFIG", "TERRAIN_MMP_START_OWNER", "TERRAIN_MMP_TEAM_COLOR_CONFIG", "TERRAIN_MMP_TEAM_COLOR_IMAGE",
        "NATIVE_MMP_PLAYABLE_LAYOUT", "NATIVE_MMP_CAMERA_CONFIG", "NATIVE_MMP_CAMERA_BOUNDS", "NATIVE_MMP_PLAYABLE_EMPTY",
        "NATIVE_MMP_FRAME", "NATIVE_MMP_BUDGET", "NATIVE_MMP_WORLD_RECTANGLE", "NATIVE_MMP_PIXEL_RECTANGLE",
        "NATIVE_MMP_POSITION", "NATIVE_MMP_POSITION_OVERFLOW", "NATIVE_MMP_TYPE", "NATIVE_MMP_COLOR_IMAGE", "NATIVE_MMP_ALLOCATION",
        "PLACEMENT_VERSION_UNSUPPORTED", "PLACEMENT_LAYOUT_UNSUPPORTED", "PLACEMENT_BYTE_LIMIT",
        "PLACEMENT_RECORD_LIMIT", "PLACEMENT_MEMORY_LIMIT", "EXACT_MEMBER_LIMIT"
    };
    return diagnostic.severity != DiagnosticSeverity::Error
        || std::find(std::begin(codes), std::end(codes), diagnostic.code) != std::end(codes);
}
}

Status TerrainMinimapMarkers::AdmitToPlan(MapData::TerrainSavePlan& plan,
    const MapData::MapDataEditSession& edits) const {
    if (sourceSha256_ != plan.SourceSha256())
        return Status::Failure("Terrain.MinimapMarkers", "TERRAIN_MMP_SOURCE_STALE", "图标与地形保存计划来源不同。");
    const auto terrain = std::find_if(inputs_.begin(), inputs_.end(), [](const auto& input) { return input.name == "WAR3MAP.W3E"; });
    if (terrain == inputs_.end() || terrain->sha256 != plan.Members()[0].outputSha256)
        return Status::Failure("Terrain.MinimapMarkers", "TERRAIN_MMP_DEPENDENCY_STALE", "图标未绑定地形计划的最终W3E。");
    if (plan.cumulativeBytes_ > plan.maximumBytes_ || bytes_.size() > plan.maximumBytes_ - plan.cumulativeBytes_)
        return Status::Failure("Terrain.MinimapMarkers", "TERRAIN_MMP_BUDGET", "图标输出超过地形保存计划剩余预算。");
    return plan.AdmitGeneratedMarkers(edits, bytes_);
}

Status PrepareTerrainMinimapMarkers(MapSaveTask& task, const std::filesystem::path& gameRoot,
    const MapData::MapDataEditSession* edits, std::shared_ptr<const MapData::TerrainSavePlan>& plan) {
    if (!task.archive || (task.executed && !task.InFinalDerivedStage()))
        return Status::Failure("Terrain.MinimapMarkers", "TERRAIN_MMP_SAVE_STAGE", "图标只能在冻结来源的同次最终保存阶段准备。");
    if (task.sourceSha256 != task.archive->SourceSha256())
        return Status::Failure("Terrain.MinimapMarkers", "TERRAIN_MMP_SOURCE_STALE", "图标任务与冻结归档来源不同。");
    if (task.minimapMarkers) return Status::Failure("Terrain.MinimapMarkers", "TERRAIN_MMP_FINAL_CONFLICT", "同次保存不能重复准备图标。");
    if (std::any_of(task.removals.begin(), task.removals.end(), IsMmp)
        || std::any_of(task.replacements.begin(), task.replacements.end(), [](const auto& member) { return IsMmp(member.name); }))
        return Status::Failure("Terrain.MinimapMarkers", "TERRAIN_MMP_FINAL_CONFLICT", "自动图标不能和手动MMP输出或删除混用。");
    auto captured = TerrainMinimapMarkers::Capture(*task.archive, gameRoot, task.replacements, task.removals);
    if (!captured.Succeeded() || !captured.value) {
        if (captured.diagnostics.empty())
            return Status::Failure("Terrain.MinimapMarkers", "TERRAIN_MMP_CAPTURE_EMPTY", "图标捕获未返回结果或诊断。");
        if (!std::all_of(captured.diagnostics.begin(), captured.diagnostics.end(), Unsupported))
            return {std::move(captured.diagnostics)};
        for (const auto& diagnostic : captured.diagnostics) {
            task.diagnostics.push_back({DiagnosticSeverity::Warning, "Terrain.MinimapMarkers",
                "TERRAIN_MMP_PRESERVED_UNSUPPORTED", "保留原小地图图标：" + diagnostic.code + "：" + diagnostic.message});
        }
        return Status::Ok();
    }
    auto owner = std::move(*captured.value);
    auto nextPlan = plan;
    if (plan) {
        if (!edits) return Status::Failure("Terrain.MinimapMarkers", "TERRAIN_MMP_SAVE_STAGE", "地形计划缺少当前会话。");
        auto candidate = std::make_shared<MapData::TerrainSavePlan>(*plan);
        const auto admitted = owner->AdmitToPlan(*candidate, *edits);
        if (!admitted.Succeeded()) {
            if (admitted.diagnostics.empty()
                || !std::all_of(admitted.diagnostics.begin(), admitted.diagnostics.end(), Unsupported)) return admitted;
            for (const auto& diagnostic : admitted.diagnostics)
                task.diagnostics.push_back({DiagnosticSeverity::Warning, "Terrain.MinimapMarkers",
                    "TERRAIN_MMP_PRESERVED_UNSUPPORTED", "保留原小地图图标：" + diagnostic.code + "：" + diagnostic.message});
            return Status::Ok();
        }
        nextPlan = std::move(candidate);
    }
    // 安装只替换MMP，不改变通行/脚本/作者图像；owner在发布前和回读时再次核对。
    std::erase_if(task.replacements, [](const auto& member) { return IsMmp(member.name); });
    task.replacements.push_back({"war3map.mmp", owner->Bytes()});
    const auto checked = owner->VerifyFrozenOutput(*task.archive, task.replacements, task.removals);
    if (!checked.Succeeded()) return checked;
    task.minimapMarkers = std::move(owner); plan = std::move(nextPlan);
    return Status::Ok();
}

} // namespace War3Map::Services
