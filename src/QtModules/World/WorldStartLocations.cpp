#include "WorldStartLocations.h"
#include "WorldModule.h"
#include "WorldViewport.h"
#include "EditorProject.h"
#include "War3MapCore/Render/EditorOverlay.h"
#include "War3MapCore/Render/TerrainPicking.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace War3Map::Modules {
namespace {
constexpr double kMarkerRadius = 40;
constexpr Render::OverlayColor kMarker{0.35f, 0.72f, 1.0f};
constexpr Render::OverlayColor kSelectedMarker{0.36f, 0.92f, 0.68f};
}

Result<WorldStartLocationCapture> CaptureWorldStartLocations(const MapDocument& document, std::size_t maxMarkers) {
    using Output = Result<WorldStartLocationCapture>;
    if (maxMarkers == 0 || maxMarkers > 256)
        return Output::Failure("World", "WORLD_START_MARKER_BUDGET", "起始位置标记预算须在 1 到 256 之间。");
    const auto& member = document.Member(MapDataKind::Units);
    if (member.state == MapDataState::Missing && !document.Units()) return Output::Success({});
    if (member.state != MapDataState::Ready || member.sources.size() != 1 || !document.Units())
        return Output::Failure("World", "WORLD_START_MARKER_SOURCE", "单位来源尚未完整读取，不能显示可编辑的起始位置标记。");
    WorldStartLocationCapture result;
    const auto& units = document.Units()->units;
    for (std::size_t index = 0; index < units.size(); ++index) {
        const auto& unit = units[index];
        if (unit.rawcode != "sloc") continue;
        ++result.sourceCount;
        if (index > std::numeric_limits<std::uint32_t>::max()
            || !std::all_of(unit.position.begin(), unit.position.end(), [](float value) {
                return std::isfinite(value) && std::abs(value) <= 1000000.0f;
            })) return Output::Failure("World", "WORLD_START_MARKER_POSITION", "起始位置的身份或坐标无效，标记未发布。");
        if (result.markers.size() == maxMarkers) { ++result.omittedCount; continue; }
        result.markers.push_back({MapData::MakePlacementId(MapData::PlacementFile::Units,
            static_cast<std::uint32_t>(index), unit), unit.owner, unit.position});
    }
    return Output::Success(std::move(result));
}

bool WorldModule::StartLocationsCurrent() const {
    return startLocationBinding_ && selectionDocument_ && selectionMapData_ && worldSelection_.Version()
        && !project_.sourceChangeSuspected_ && !project_.sourceChangeConfirmed_
        && *startLocationBinding_ == selectionMapData_->Binding()
        && worldSelection_.Version()->document == *startLocationBinding_
        && selectionMapKey_ == MapKey()
        && selectionSessionIdentity_ == (project_.mapEdits_ ? project_.mapEdits_->SessionIdentity() : 0)
        && startLocationBinding_->revision == (project_.mapEdits_ ? project_.mapEdits_->Revision() : 0)
        && startLocationBinding_->generation == std::max<std::uint64_t>(1, project_.editRevision_.Generation());
}

std::optional<MapData::PlacementId> WorldModule::PickStartLocation(double x, double y, bool selectedOnly) const {
    if (!StartLocationsCurrent() || !startLocationOverlaySubmitted_ || !terrain_ || !viewport_ || !viewport_->HasOverlaySupport()
        || !std::isfinite(x) || !std::isfinite(y)) return {};
    std::optional<MapData::PlacementId> result;
    double nearest = kMarkerRadius;
    for (const auto& marker : startLocationMarkers_.markers) {
        if (!Render::SampleTerrainHeight(*terrain_, marker.position[0], marker.position[1])) continue;
        const auto flags = worldSelection_.Flags(marker.id);
        if (!flags.visible || flags.locked || (selectedOnly
            && std::find(selectedPlacements_.begin(), selectedPlacements_.end(), marker.id) == selectedPlacements_.end())) continue;
        const double distance = std::hypot(marker.position[0] - x, marker.position[1] - y);
        // 距离相同保持来源顺序；不按 owner 猜测唯一对象，提交再核对 W3I 绑定。
        if (distance <= nearest && (!result || distance < nearest)) { nearest = distance; result = marker.id; }
    }
    return result;
}

void WorldModule::AppendStartLocationOverlay(std::vector<War3RenderOverlayLineV2>& lines) const {
    if (!StartLocationsCurrent() || !terrain_ || !viewport_ || !viewport_->HasOverlaySupport()) return;
    for (const auto& marker : startLocationMarkers_.markers) {
        if (!worldSelection_.Flags(marker.id).visible) continue;
        auto position = marker.position;
        const auto draft = std::find_if(dragGroup_.begin(), dragGroup_.end(), [&](const auto& move) { return move.id == marker.id; });
        if (draft != dragGroup_.end()) position = draft->position;
        if (!Render::SampleTerrainHeight(*terrain_, position[0], position[1])) continue;
        const bool selected = std::find(selectedPlacements_.begin(), selectedPlacements_.end(), marker.id) != selectedPlacements_.end();
        const auto& color = selected ? kSelectedMarker : kMarker;
        Render::AppendGroundCircle(lines, terrain_.get(), position[0], position[1], kMarkerRadius, color);
        for (int axis = 0; axis < 2; ++axis) {
            auto from = position, to = position;
            from[axis] -= 24; to[axis] += 24;
            const auto fromGround = Render::SampleTerrainHeight(*terrain_, from[0], from[1]);
            const auto toGround = Render::SampleTerrainHeight(*terrain_, to[0], to[1]);
            if (!fromGround || !toGround) continue;
            from[2] = *fromGround + 4; to[2] = *toGround + 4;
            lines.push_back({{from[0], from[1], from[2]}, {to[0], to[1], to[2]}, {color[0], color[1], color[2]}});
        }
    }
}
} // namespace War3Map::Modules
