#include "War3MapCore/MapData/MapDataEditSession.h"

#include <algorithm>

namespace War3Map::MapData {

Result<MapDataEditSession::PreparedTerrainEdit> MapDataEditSession::PreparePathingRestrictionEdit(
    const TerrainBrush& brush, std::span<const TerrainStrokePoint> points, bool followDoodads, bool inspectSaveGate) const {
    if (!terrain_ || !pathing_ || !pathingOriginal_)
        return Result<PreparedTerrainEdit>::Failure("MapData.Edit", "MAP_PATHING_UNAVAILABLE", "通行限制需要唯一且可解析的源 W3E/WPM。");
    if (terrain_->header.version != 11)
        return Result<PreparedTerrainEdit>::Failure("MapData.Edit", "MAP_TERRAIN_READ_ONLY", "新版地形只读，不能编辑关联通行限制。");
    const auto& baseline = TerrainHistoryPathing();
    const auto savedS = TerrainHistorySavedRestrictions();
    if (!savedS || !pathingRestrictions_ || !TerrainHistoryProtected())
        return Result<PreparedTerrainEdit>::Failure("MapData.Edit", "MAP_PATHING_UNAVAILABLE", "当前扩裁状态缺少同来源的完整通行分层。");
    auto result = ApplyPathingRestrictionStroke(terrain_->header, baseline, *pathing_,
        *TerrainHistoryProtected(), *savedS, *pathingRestrictions_, brush, points);
    if (!result.Succeeded() || !result.value) return {{}, std::move(result.diagnostics)};
    Command command;
    if (!result.value->bytes.empty()) {
        command.pathingBefore = *pathing_;
        command.pathingAfter = std::move(result.value->bytes);
    }
    if (result.value->intent != *pathingRestrictions_) {
        command.restrictionBefore = pathingRestrictions_;
        command.restrictionAfter = std::make_shared<const std::vector<std::byte>>(std::move(result.value->intent));
    }
    PreparedTerrainEdit prepared;
    prepared.before_ = terrain_;
    prepared.sessionIdentity_ = sessionIdentity_; prepared.revision_ = revision_;
    prepared.sourceRevision_ = terrainSourceRevision_;
    prepared.changed_ = result.value->changed;
    prepared.terrainPathingGate_ = inspectSaveGate ? VerifyTerrainCandidateForSave(terrain_,
        command.pathingAfter ? command.pathingAfter : pathing_, current_[Index(MapDataKind::Doodads)], nullptr,
        (command.restrictionAfter ? command.restrictionAfter : pathingRestrictions_).get())
        : Status::Failure("MapData.Edit", "TERRAIN_SAVE_GATE_NOT_INSPECTED", "此落笔尚未检查地形保存资格。");
    prepared.brush_ = brush; prepared.points_.assign(points.begin(), points.end());
    prepared.followDoodads_ = followDoodads; // 不改地形，也不移动装饰物。
    prepared.command_ = std::make_shared<const Command>(std::move(command));
    return Result<PreparedTerrainEdit>::Success(std::move(prepared));
}

Result<std::vector<std::byte>> MapDataEditSession::SerializePathingRestrictionDraft() const {
    if (!pathing_) return Result<std::vector<std::byte>>::Success({});
    if (!terrainOriginal_ || !pathingOriginal_)
        return Result<std::vector<std::byte>>::Failure("MapData.Edit", "PATHING_DRAFT_SOURCE_MISSING", "通行限制草稿没有完整来源。");
    const auto shape = resizeState_ ? resizeState_ : resizeDraftState_;
    if (shape) {
        return EncodePathingRestrictionDraft(*terrain_, TerrainHistoryPathing(), *pathing_, *TerrainHistoryProtected(),
            *TerrainHistorySavedRestrictions(), *pathingRestrictions_);
    }
    return EncodePathingRestrictionDraft(*terrainOriginal_, *pathingOriginal_, *pathing_,
        *pathingProtected_, *originalPathingRestrictions_, *pathingRestrictions_);
}

Result<PathingRestrictionPreview> MapDataEditSession::QueryPathingRestrictions(std::size_t maximumCells) const {
    using Output = Result<PathingRestrictionPreview>;
    if (!terrain_ || !pathing_ || !pathingOriginal_ || !pathingRestrictions_ || pathingRestrictions_->size() + 16 != pathing_->size())
        return Output::Failure("MapData.Edit", "MAP_PATHING_UNAVAILABLE", "通行限制需要唯一且可解析的源 W3E/WPM。");
    if (maximumCells == 0 || maximumCells > 4096)
        return Output::Failure("MapData.Edit", "PATHING_QUERY_BUDGET", "通行限制预览最多返回 1 到 4096 格。");
    const auto additions = [this](std::size_t i) {
        return std::to_integer<std::uint8_t>((*pathingRestrictions_)[i - 16]);
    };
    PathingRestrictionPreview output;
    for (std::size_t i = 16; i < pathing_->size(); ++i) if (additions(i)) ++output.totalCells;
    const auto stride = std::max(std::size_t{1}, (output.totalCells + maximumCells - 1) / maximumCells);
    output.cells.reserve(std::min(output.totalCells, maximumCells));
    const auto width = (terrain_->header.width - 1) * 4;
    std::size_t ordinal = 0;
    for (std::size_t i = 16; i < pathing_->size(); ++i) {
        const auto flags = additions(i);
        if (flags && ordinal++ % stride == 0) output.cells.push_back({
            static_cast<std::uint32_t>((i - 16) % width), static_cast<std::uint32_t>((i - 16) / width), flags});
    }
    return Output::Success(std::move(output));
}

} // namespace War3Map::MapData
