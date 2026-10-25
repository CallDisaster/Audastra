#include "War3MapCore/MapData/TerrainResizeResources.h"

#include "TerrainResizeResourceState.h"
#include "War3MapCore/MapData/MapDataEditSession.h"

#include <cmath>
#include <new>
#include <stdexcept>

namespace War3Map::MapData {
namespace {
constexpr auto kSubsystem = "Terrain.ResizeResources";
struct Failure { std::vector<Diagnostic> diagnostics; };
[[noreturn]] void Fail(const char* code, std::string message) {
    throw Failure{Status::Failure(kSubsystem, code, std::move(message)).diagnostics};
}
void Check(Status status) { if (!status.Succeeded()) throw Failure{std::move(status.diagnostics)}; }
} // namespace

const TerrainResizeDimensionRules& TerrainResizeResources::DimensionRules() const noexcept { return state_->dimensions; }
const std::array<float, 4>& TerrainResizeResources::CameraMargins() const noexcept { return state_->margins; }
const ArchiveExactMember& TerrainResizeResources::SourceShadow() const noexcept { return state_->shadow; }
std::span<const TerrainWaterSpecialDoodadShape> TerrainResizeResources::SpecialShapes() const noexcept { return state_->special; }
const std::string& TerrainResizeResources::DependencyFingerprint() const noexcept { return state_->fingerprint; }
std::size_t TerrainResizeResources::CaptureBudgetBytes() const noexcept { return state_ ? state_->captureBytes : 0; }
std::size_t TerrainResizeResources::OwnedCapacityBytes() const noexcept { return state_ ? state_->ownedBytes : 0; }
std::size_t TerrainResizeResources::CaptureWork() const noexcept { return state_ ? state_->work : 0; }

Status TerrainResizeResources::VerifyInput(const MapDataEditSession& session, const TerrainResizeIntent& intent,
    std::size_t maximumBytes, std::size_t maximumWork) const try {
    const auto valid = ValidateTerrainResizeIntent(intent); Check(valid);
    if (!state_ || !state_->native || state_->fingerprint.empty() || session.SessionIdentity() != state_->session
        || session.Revision() != state_->revision || session.TerrainSourceRevision() != state_->sourceRevision || session.Terrain() != state_->terrain)
        Fail("TERRAIN_EDIT_STALE", "尺寸资源不属于当前会话、修订和地形输入。");
    if (state_->captureBytes > maximumBytes || state_->work > maximumWork)
        Fail("TERRAIN_RESIZE_BUDGET", "完整尺寸来源与空间预检超过本次预算。");
    auto work = state_->work;
    const auto step = [&] { if (work == maximumWork) Fail("TERRAIN_RESIZE_WORK", "完整空间预检超过工作预算。"); ++work; };
    const auto& h = state_->terrain->header; const auto& c = intent.canvas;
    const double left = double(h.offset[0]) + double(c.west) * 128, bottom = double(h.offset[1]) + double(c.south) * 128;
    const double right = double(h.offset[0]) + double(c.east) * 128, top = double(h.offset[1]) + double(c.north) * 128;
    const auto point = [&](double x, double y, std::string kind) {
        step();
        if (!std::isfinite(x) || !std::isfinite(y) || x < left || x > right || y < bottom || y > top)
            Fail("TERRAIN_RESIZE_SPATIAL_CLIPPED", "新外矩形裁及" + kind + "，世界位置(" + std::to_string(x) + "," + std::to_string(y) + ")；未移动或删除。");
    };
    if (state_->spatial.Units()) for (const auto& unit : state_->spatial.Units()->units)
        point(unit.position[0], unit.position[1], "单位/起点 " + unit.rawcode + " #" + std::to_string(unit.creationId));
    if (state_->spatial.Doodads()) for (const auto& object : state_->spatial.Doodads()->doodads)
        point(object.position[0], object.position[1], "普通装饰物 " + object.rawcode + " #" + std::to_string(object.creationId));
    if (state_->spatial.Regions()) for (const auto& region : state_->spatial.Regions()->entries) {
        point(region.left, region.bottom, "区域 #" + std::to_string(region.creationNumber));
        point(region.right, region.top, "区域 #" + std::to_string(region.creationNumber));
    }
    if (state_->spatial.Cameras()) for (const auto& camera : state_->spatial.Cameras()->entries)
        point(camera.targetX, camera.targetY, "镜头 " + camera.name);
    if (state_->spatial.Settings()) for (const auto& player : state_->spatial.Settings()->players)
        point(player.startX, player.startY, "玩家起点 #" + std::to_string(player.number));
    for (const auto& footprint : state_->native->spatialFootprints_) {
        step(); const auto& f = footprint.rectangle;
        if (f[0] < std::int64_t(c.west) * 4 || f[1] < std::int64_t(c.south) * 4
            || f[2] > std::int64_t(c.east) * 4 || f[3] > std::int64_t(c.north) * 4)
            Fail("TERRAIN_RESIZE_FOOTPRINT_CLIPPED", "新外矩形裁及完整实际占地 " + footprint.rawcode + " #" + std::to_string(footprint.creationId));
    }
    for (const auto& special : state_->special) {
        step();
        const auto x0 = special.hasFootprint ? std::int64_t(special.x0) : special.position[0];
        const auto y0 = special.hasFootprint ? std::int64_t(special.y0) : special.position[1];
        const auto x1 = special.hasFootprint ? std::int64_t(special.x1) : x0;
        const auto y1 = special.hasFootprint ? std::int64_t(special.y1) : y0;
        if (x0 < std::int64_t(c.west) || y0 < std::int64_t(c.south)
            || x1 > std::int64_t(c.east) || y1 > std::int64_t(c.north))
            Fail("TERRAIN_RESIZE_SPECIAL_CLIPPED", "新外矩形裁及特殊装饰物 #" + std::to_string(special.sourceRecordIndex) + " " + special.rawcode);
    }
    return Status::Ok();
} catch (Failure& failure) { return {std::move(failure.diagnostics)}; }
catch (const std::bad_alloc&) { return Status::Failure(kSubsystem, "TERRAIN_RESIZE_BUDGET", "尺寸空间预检内存不足。"); }
catch (const std::length_error&) { return Status::Failure(kSubsystem, "TERRAIN_RESIZE_BUDGET", "尺寸空间预检容量超限。"); }

} // namespace War3Map::MapData
