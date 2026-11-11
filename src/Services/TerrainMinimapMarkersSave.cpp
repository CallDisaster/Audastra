#include "War3MapCore/Services/TerrainMinimapMarkersSave.h"

#include "War3MapCore/Services/TerrainMinimapMarkers.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/Resources/ImportTable.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <iterator>
#include <new>

namespace War3Map::Services {
namespace {
bool IsMmp(std::string_view name) {
    constexpr std::string_view expected = "war3map.mmp";
    return name.size() == expected.size() && std::equal(name.begin(), name.end(), expected.begin(),
        [](unsigned char a, char b) { return std::tolower(a) == b; });
}
bool IsProvenance(std::string_view name) {
    const auto key = Resources::CanonicalImportPath(name);
    return key.Succeeded() && key.value && *key.value == "AUDASTRA\\MINIMAP-MARKERS.BIN";
}
bool CanPreserveUnverifiedMarkers(const Diagnostic& diagnostic) {
    constexpr std::string_view codes[]{
        "MINIMAP_MARKER_PROVENANCE", "MINIMAP_MARKER_VERSION", "MINIMAP_MARKER_NEGATIVE_COUNT",
        "MINIMAP_MARKER_TRUNCATED", "MINIMAP_MARKER_TRAILING_DATA", "MINIMAP_MARKER_BYTE_LIMIT",
        "MINIMAP_MARKER_COUNT_LIMIT", "MINIMAP_MARKER_ALLOCATION", "MINIMAP_MARKER_BUDGET"
    };
    return diagnostic.severity != DiagnosticSeverity::Error ||
        std::find(std::begin(codes), std::end(codes), diagnostic.code) != std::end(codes);
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
    if (verifyGeneration_ && (terrain == inputs_.end() || terrain->sha256 != plan.Members()[0].outputSha256))
        return Status::Failure("Terrain.MinimapMarkers", "TERRAIN_MMP_DEPENDENCY_STALE", "图标未绑定地形计划的最终W3E。");
    if (plan.cumulativeBytes_ > plan.maximumBytes_ || bytes_.size() > plan.maximumBytes_ - plan.cumulativeBytes_
        || provenance_.size() > plan.maximumBytes_ - plan.cumulativeBytes_ - bytes_.size())
        return Status::Failure("Terrain.MinimapMarkers", "TERRAIN_MMP_BUDGET", "图标输出超过地形保存计划剩余预算。");
    if (provenance_.empty()) return plan.AdmitGeneratedMarkers(edits, bytes_);
    if (std::any_of(plan.replacements_.begin(), plan.replacements_.end(), [](const auto& member) { return IsProvenance(member.name); }))
        return Status::Failure("Terrain.MinimapMarkers", "TERRAIN_MMP_FINAL_CONFLICT", "同次地形计划不能重复封存图标归属。");
    try {
        MemberReplacement companion{kMinimapMarkerProvenance, provenance_};
        plan.replacements_.reserve(plan.replacements_.size() + 2);
        const auto admitted = plan.AdmitGeneratedMarkers(edits, bytes_);
        if (!admitted.Succeeded()) return admitted;
        plan.replacements_.push_back(std::move(companion));
        plan.cumulativeBytes_ += provenance_.size();
        return Status::Ok();
    } catch (const std::bad_alloc&) {
        return Status::Failure("Terrain.MinimapMarkers", "TERRAIN_MMP_ALLOCATION", "无法冻结图标归属输出。");
    }
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
    const auto pending = std::find_if(task.replacements.begin(), task.replacements.end(),
        [](const auto& member) { return IsProvenance(member.name); });
    if (std::any_of(task.removals.begin(), task.removals.end(), IsProvenance) ||
        (pending != task.replacements.end() && (pending->data.empty() ||
            std::any_of(std::next(pending), task.replacements.end(), [](const auto& m) { return IsProvenance(m.name); }))))
        return Status::Failure("Terrain.MinimapMarkers", "TERRAIN_MMP_FINAL_CONFLICT", "图标归属不能删除、清空或重复替换。");
    const auto staged = pending == task.replacements.end() ? std::span<const std::byte>{} :
        std::span<const std::byte>(pending->data);
    auto editsState = MinimapMarkerEdits::Read(*task.archive, staged);
    if (!editsState.Succeeded() || !editsState.value) {
        if (editsState.diagnostics.empty())
            return Status::Failure("Terrain.MinimapMarkers", "TERRAIN_MMP_CAPTURE_EMPTY", "图标捕获未返回结果或诊断。");
        if (!staged.empty() || !std::all_of(editsState.diagnostics.begin(), editsState.diagnostics.end(), CanPreserveUnverifiedMarkers))
            return {std::move(editsState.diagnostics)};
        for (const auto& diagnostic : editsState.diagnostics)
            task.diagnostics.push_back({DiagnosticSeverity::Warning, "Terrain.MinimapMarkers",
                "TERRAIN_MMP_PRESERVED_UNVERIFIED", "保留原小地图图标：" + diagnostic.code + "：" + diagnostic.message});
        return Status::Ok();
    }
    if (!editsState.value->ShouldWrite()) {
        task.diagnostics.push_back({DiagnosticSeverity::Warning, "Terrain.MinimapMarkers",
            "TERRAIN_MMP_PRESERVED_LEGACY", "旧图标来源未登记，保留原图标。"});
        return Status::Ok();
    }
    std::shared_ptr<const TerrainMinimapMarkers> generated;
    if (editsState.value->UsesGenerator()) {
        auto captured = TerrainMinimapMarkers::Capture(*task.archive, gameRoot, task.replacements, task.removals);
        if (captured.Succeeded() && captured.value) generated = std::move(*captured.value);
        else {
            if (captured.diagnostics.empty())
                return Status::Failure("Terrain.MinimapMarkers", "TERRAIN_MMP_CAPTURE_EMPTY", "图标捕获未返回结果或诊断。");
            if (!std::all_of(captured.diagnostics.begin(), captured.diagnostics.end(), Unsupported))
                return {std::move(captured.diagnostics)};
            for (const auto& diagnostic : captured.diagnostics) {
                task.diagnostics.push_back({DiagnosticSeverity::Warning, "Terrain.MinimapMarkers",
                    "TERRAIN_MMP_PRESERVED_UNSUPPORTED", "保留自动图标，不重建：" + diagnostic.code + "：" + diagnostic.message});
            }
            if (!editsState.value->HasPending()) return Status::Ok();
        }
    }
    auto composed = TerrainMinimapMarkers::Compose(*task.archive, std::move(*editsState.value), generated);
    if (!composed.Succeeded() || !composed.value) return {std::move(composed.diagnostics)};
    auto owner = std::move(*composed.value);
    auto nextPlan = plan;
    if (plan) {
        if (!edits) return Status::Failure("Terrain.MinimapMarkers", "TERRAIN_MMP_SAVE_STAGE", "地形计划缺少当前会话。");
        auto candidate = std::make_shared<MapData::TerrainSavePlan>(*plan);
        const auto admitted = owner->AdmitToPlan(*candidate, *edits);
        if (!admitted.Succeeded()) {
            if (!staged.empty() || admitted.diagnostics.empty()
                || !std::all_of(admitted.diagnostics.begin(), admitted.diagnostics.end(), Unsupported)) return admitted;
            for (const auto& diagnostic : admitted.diagnostics)
                task.diagnostics.push_back({DiagnosticSeverity::Warning, "Terrain.MinimapMarkers",
                    "TERRAIN_MMP_PRESERVED_UNSUPPORTED", "保留原小地图图标：" + diagnostic.code + "：" + diagnostic.message});
            return Status::Ok();
        }
        nextPlan = std::move(candidate);
    }
    // 原版 MMP 和归属一起安装、一起回读。
    std::erase_if(task.replacements, [](const auto& member) { return IsProvenance(member.name); });
    task.replacements.push_back({"war3map.mmp", owner->Bytes()});
    task.replacements.push_back({kMinimapMarkerProvenance, owner->ProvenanceBytes()});
    const auto checked = owner->VerifyFrozenOutput(*task.archive, task.replacements, task.removals);
    if (!checked.Succeeded()) return checked;
    task.minimapMarkers = std::move(owner); plan = std::move(nextPlan);
    return Status::Ok();
}

} // namespace War3Map::Services
