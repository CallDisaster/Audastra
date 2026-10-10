#include "War3MapCore/MapData/PlacementEditing.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <vector>

namespace War3Map::MapData {
namespace detail {
// 唯一受信入口：由本文件真正完成的操作提交谱系，不向任意整表编辑公开映射写入。
struct PlacementEditingAccess {
    static Status Commit(MapDataEditSession& session, const MapDocument& source, const UnitPlacements* units, const DoodadPlacements* doodads,
        std::vector<PlacementRowLineage> lineage) {
        return session.EditPlacementRows(source, units, doodads, std::move(lineage));
    }
};
}
namespace {

constexpr float kMaximumCoordinate = 1e6f;

Status Failure(const char* code, std::string message) {
    return Status::Failure("MapData.Placement", code, std::move(message));
}

template <class Placement>
bool Matches(const std::vector<Placement>& list, const PlacementId& id) {
    return id.index < list.size() && list[id.index].rawcode == id.rawcode && list[id.index].creationId == id.creationId;
}

bool Matches(const MapDocument& document, const PlacementId& id) {
    if (id.file == PlacementFile::Units) return document.Units() && Matches(document.Units()->units, id);
    return document.Doodads() && Matches(document.Doodads()->doodads, id);
}

Status Unavailable(const PlacementId& id) {
    return Failure("PLACEMENT_STALE", "摆放 " + id.rawcode + "（序号 " + std::to_string(id.index)
        + "）已不存在或已变化，请重新选择");
}

bool FiniteCoordinate(float value) { return std::isfinite(value) && std::abs(value) <= kMaximumCoordinate; }

// 按文件把修改后的副本一次提交；未涉及的文件传空指针。
PlacementRowLineage Rows(PlacementFile file, std::size_t before, std::size_t after,
    std::span<const PlacementId> deleted = {}) {
    PlacementRowLineage result;
    result.kind = file == PlacementFile::Units ? MapDataKind::Units : MapDataKind::Doodads;
    result.beforeCount = static_cast<std::uint32_t>(before); result.afterCount = static_cast<std::uint32_t>(after);
    std::set<std::uint32_t> removed;
    for (const auto& id : deleted) if (id.file == file) removed.insert(id.index);
    result.beforeToAfter.resize(before);
    std::uint32_t next = 0;
    for (std::uint32_t row = 0; row < before; ++row) if (!removed.contains(row)) result.beforeToAfter[row] = next++;
    return result;
}
template <class Edit>
Status EditCopies(MapDataEditSession& session, const MapDocument& document, bool units, bool doodads, Edit&& edit,
    std::span<const PlacementId> deleted = {}) {
    std::optional<UnitPlacements> nextUnits;
    std::optional<DoodadPlacements> nextDoodads;
    if (units) nextUnits = *document.Units();
    if (doodads) nextDoodads = *document.Doodads();
    if (auto status = edit(nextUnits ? &*nextUnits : nullptr, nextDoodads ? &*nextDoodads : nullptr); !status.Succeeded())
        return status;
    std::vector<PlacementRowLineage> lineage;
    if (nextUnits) lineage.push_back(Rows(PlacementFile::Units, document.Units()->units.size(), nextUnits->units.size(), deleted));
    if (nextDoodads) lineage.push_back(Rows(PlacementFile::Doodads, document.Doodads()->doodads.size(), nextDoodads->doodads.size(), deleted));
    return detail::PlacementEditingAccess::Commit(session, document, nextUnits ? &*nextUnits : nullptr, nextDoodads ? &*nextDoodads : nullptr,
        std::move(lineage));
}

} // namespace

PlacementId MakePlacementId(PlacementFile file, std::uint32_t index, const UnitPlacement& unit) {
    return {file, index, unit.rawcode, unit.creationId};
}
PlacementId MakePlacementId(PlacementFile file, std::uint32_t index, const DoodadPlacement& doodad) {
    return {file, index, doodad.rawcode, doodad.creationId};
}

Result<PlacementTransform> FindPlacement(const MapDocument& document, const PlacementId& id) {
    if (!Matches(document, id)) {
        Result<PlacementTransform> result;
        result.diagnostics = Unavailable(id).diagnostics;
        return result;
    }
    if (id.file == PlacementFile::Units) return Result<PlacementTransform>::Success(document.Units()->units[id.index]);
    return Result<PlacementTransform>::Success(document.Doodads()->doodads[id.index]);
}

Result<detail::PlacementEditCandidate> detail::PreparePlacementMoves(const MapDocument& document,
    std::span<const PlacementMove> moves) {
    const auto fail = [](Status status) { return Result<PlacementEditCandidate>{std::nullopt, std::move(status.diagnostics)}; };
    if (moves.empty()) return Result<PlacementEditCandidate>::Success({});
    bool units = false, doodads = false;
    for (std::size_t i = 0; i < moves.size(); ++i) {
        const auto& move = moves[i];
        if (!std::all_of(move.position.begin(), move.position.end(), FiniteCoordinate)
            || (move.rotation && !std::isfinite(*move.rotation)))
            return fail(Failure("PLACEMENT_COORDINATE_INVALID", "摆放坐标或朝向不是有效数值，或超出 ±1000000"));
        if (move.id.file != PlacementFile::Units && move.id.file != PlacementFile::Doodads)
            return fail(Failure("PLACEMENT_FILE_UNAVAILABLE", "未知摆放文件，不能移动"));
        if (!Matches(document, move.id)) return fail(Unavailable(move.id));
        for (std::size_t j = 0; j < i; ++j)
            if (moves[j].id == move.id) return fail(Failure("PLACEMENT_DUPLICATE", "同一摆放在一次移动中出现多次"));
        (move.id.file == PlacementFile::Units ? units : doodads) = true;
    }
    PlacementEditCandidate candidate;
    if (units) candidate.units = *document.Units();
    if (doodads) candidate.doodads = *document.Doodads();
    for (const auto& move : moves) {
        PlacementTransform& target = move.id.file == PlacementFile::Units
            ? static_cast<PlacementTransform&>(candidate.units->units[move.id.index])
            : static_cast<PlacementTransform&>(candidate.doodads->doodads[move.id.index]);
        target.position = move.position;
        if (move.rotation) {
            // 原版保存为 [0, 2π) 的弧度。
            constexpr float kTurn = 6.28318530718f;
            target.rotation = std::fmod(std::fmod(*move.rotation, kTurn) + kTurn, kTurn);
        }
    }
    if (units) candidate.lineage.push_back(Rows(PlacementFile::Units, document.Units()->units.size(), candidate.units->units.size()));
    if (doodads) candidate.lineage.push_back(Rows(PlacementFile::Doodads, document.Doodads()->doodads.size(), candidate.doodads->doodads.size()));
    return Result<PlacementEditCandidate>::Success(std::move(candidate));
}

Status MovePlacements(MapDataEditSession& session, std::span<const PlacementMove> moves) {
    if (moves.empty()) return Status::Ok();
    const auto document = session.Snapshot();
    auto candidate = detail::PreparePlacementMoves(document, moves);
    if (!candidate.Succeeded()) return {candidate.diagnostics};
    return detail::PlacementEditingAccess::Commit(session, document,
        candidate.value->units ? &*candidate.value->units : nullptr,
        candidate.value->doodads ? &*candidate.value->doodads : nullptr, std::move(candidate.value->lineage));
}

Status DeletePlacements(MapDataEditSession& session, std::span<const PlacementId> ids) {
    if (ids.empty()) return Status::Ok();
    const auto document = session.Snapshot();
    bool units = false, doodads = false;
    for (std::size_t i = 0; i < ids.size(); ++i) {
        if (!Matches(document, ids[i])) return Unavailable(ids[i]);
        for (std::size_t j = 0; j < i; ++j)
            if (ids[j] == ids[i]) return Failure("PLACEMENT_DUPLICATE", "同一摆放在一次删除中出现多次");
        (ids[i].file == PlacementFile::Units ? units : doodads) = true;
    }
    return EditCopies(session, document, units, doodads, [&](UnitPlacements* unitList, DoodadPlacements* doodadList) {
        const auto erase = [&ids](auto& list, PlacementFile file) {
            std::vector<std::uint32_t> indices;
            for (const auto& id : ids) if (id.file == file) indices.push_back(id.index);
            std::sort(indices.rbegin(), indices.rend());
            for (const auto index : indices) list.erase(list.begin() + index);
        };
        if (unitList) erase(unitList->units, PlacementFile::Units);
        if (doodadList) erase(doodadList->doodads, PlacementFile::Doodads);
        return Status::Ok();
    }, ids);
}

namespace {
template <class List>
std::int32_t NextCreationId(const List& list) {
    std::int32_t maximum = -1;
    for (const auto& entry : list) maximum = std::max(maximum, entry.creationId);
    return maximum == std::numeric_limits<std::int32_t>::max() ? -1 : maximum + 1;
}

bool ValidTransform(const PlacementTransform& placement) {
    return placement.rawcode.size() == 4 && std::all_of(placement.position.begin(), placement.position.end(), FiniteCoordinate)
        && std::isfinite(placement.rotation) && std::all_of(placement.scale.begin(), placement.scale.end(),
            [](float value) { return std::isfinite(value) && std::abs(value) <= 1000.0f; });
}

template <class Id>
Status StaleAuxiliary(const Id& id) {
    return Failure("PLACEMENT_STALE", "“" + id.name + "”（序号 " + std::to_string(id.index) + "）已不存在或已变化，请重新选择");
}
} // namespace

Result<PlacementId> AddUnitPlacement(MapDataEditSession& session, UnitPlacement placement) {
    const auto fail = [](Status status) { return Result<PlacementId>{std::nullopt, std::move(status.diagnostics)}; };
    if (!ValidTransform(placement)) return fail(Failure("PLACEMENT_COORDINATE_INVALID", "新摆放的 rawcode、坐标、朝向或缩放无效"));
    const auto document = session.Snapshot();
    if (!document.Units()) return fail(Failure("PLACEMENT_FILE_UNAVAILABLE", "地图单位文件缺失或不可编辑，不能新增单位/物品"));
    if (document.Units()->recordLayout != PlacementRecordLayout::Classic)
        return fail(Failure("PLACEMENT_LAYOUT_UNDETERMINED", "单位记录布局尚未确定，不能新增摆放"));
    auto units = *document.Units();
    placement.creationId = NextCreationId(units.units);
    if (placement.creationId < 0) return fail(Failure("PLACEMENT_ID_EXHAUSTED", "创建编号已用尽"));
    units.units.push_back(placement);
    if (auto status = detail::PlacementEditingAccess::Commit(session, document, &units, nullptr,
        {Rows(PlacementFile::Units, document.Units()->units.size(), units.units.size())}); !status.Succeeded()) return fail(std::move(status));
    return Result<PlacementId>::Success(MakePlacementId(PlacementFile::Units,
        static_cast<std::uint32_t>(units.units.size() - 1), units.units.back()));
}

Result<PlacementId> AddDoodadPlacement(MapDataEditSession& session, DoodadPlacement placement) {
    const auto fail = [](Status status) { return Result<PlacementId>{std::nullopt, std::move(status.diagnostics)}; };
    if (!ValidTransform(placement)) return fail(Failure("PLACEMENT_COORDINATE_INVALID", "新摆放的 rawcode、坐标、朝向或缩放无效"));
    const auto document = session.Snapshot();
    if (!document.Doodads()) return fail(Failure("PLACEMENT_FILE_UNAVAILABLE", "地图装饰物文件缺失或不可编辑，不能新增装饰物"));
    if (document.Doodads()->recordLayout != PlacementRecordLayout::Classic)
        return fail(Failure("PLACEMENT_LAYOUT_UNDETERMINED", "地物记录布局尚未确定，不能新增摆放"));
    auto doodads = *document.Doodads();
    placement.creationId = NextCreationId(doodads.doodads);
    if (placement.creationId < 0) return fail(Failure("PLACEMENT_ID_EXHAUSTED", "创建编号已用尽"));
    doodads.doodads.push_back(placement);
    if (auto status = detail::PlacementEditingAccess::Commit(session, document, nullptr, &doodads,
        {Rows(PlacementFile::Doodads, document.Doodads()->doodads.size(), doodads.doodads.size())}); !status.Succeeded()) return fail(std::move(status));
    return Result<PlacementId>::Success(MakePlacementId(PlacementFile::Doodads,
        static_cast<std::uint32_t>(doodads.doodads.size() - 1), doodads.doodads.back()));
}

Result<detail::PlacementEditCandidate> detail::PreparePlacementCopies(const MapDocument& document,
    std::span<const PlacementId> ids, std::array<float, 3> offset) {
    const auto fail = [](Status status) {
        return Result<PlacementEditCandidate>{std::nullopt, std::move(status.diagnostics)};
    };
    if (ids.empty()) return Result<PlacementEditCandidate>::Success({});
    if (ids.size() > 4096) return fail(Failure("PLACEMENT_COPY_LIMIT", "一次最多复制 4096 个摆放"));
    if (!std::all_of(offset.begin(), offset.end(), FiniteCoordinate))
        return fail(Failure("PLACEMENT_COORDINATE_INVALID", "复制偏移不是有效世界坐标"));

    std::set<std::pair<PlacementFile, std::uint32_t>> seen;
    std::size_t unitCount = 0, doodadCount = 0;
    for (const auto& id : ids) {
        if (id.file != PlacementFile::Units && id.file != PlacementFile::Doodads)
            return fail(Failure("PLACEMENT_FILE_UNAVAILABLE", "未知摆放文件，不能复制"));
        if (!Matches(document, id)) return fail(Unavailable(id));
        if (!seen.emplace(id.file, id.index).second)
            return fail(Failure("PLACEMENT_DUPLICATE", "同一摆放在复制组中出现多次"));
        const auto& source = id.file == PlacementFile::Units
            ? static_cast<const PlacementTransform&>(document.Units()->units[id.index])
            : static_cast<const PlacementTransform&>(document.Doodads()->doodads[id.index]);
        if (source.rawcode == "sloc")
            return fail(Failure("PLACEMENT_START_COPY_UNSUPPORTED", "不能复制起始位置"));
        if (!ValidTransform(source))
            return fail(Failure("PLACEMENT_SOURCE_INVALID", "源摆放包含无效坐标、缩放或类型，不能复制"));
        for (std::size_t axis = 0; axis < 3; ++axis)
            if (!FiniteCoordinate(static_cast<float>(double(source.position[axis]) + offset[axis])))
                return fail(Failure("PLACEMENT_COORDINATE_INVALID", "复制后的坐标超出地图编辑范围"));
        (id.file == PlacementFile::Units ? unitCount : doodadCount)++;
    }
    constexpr auto maxId = static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max());
    const auto capacity = PlacementReadLimits{}.maxPlacements;
    const auto validFile = [&](PlacementFile file, std::size_t count) {
        if (count == 0) return true;
        const auto ordinary = file == PlacementFile::Units ? document.Units()->units.size() : document.Doodads()->doodads.size();
        const auto special = file == PlacementFile::Units ? 0 : document.Doodads()->specialDoodads.size();
        const auto first = file == PlacementFile::Units
            ? NextCreationId(document.Units()->units) : NextCreationId(document.Doodads()->doodads);
        return count <= capacity && ordinary <= capacity - count && special <= capacity - count - ordinary && first >= 0
            && static_cast<std::uint64_t>(first) + count - 1 <= maxId;
    };
    if (!validFile(PlacementFile::Units, unitCount) || !validFile(PlacementFile::Doodads, doodadCount))
        return fail(Failure("PLACEMENT_ID_EXHAUSTED", "复制组超出文件容量或创建编号已用尽"));

    std::vector<std::uint32_t> unitRows, doodadRows;
    for (const auto& id : ids) (id.file == PlacementFile::Units ? unitRows : doodadRows).push_back(id.index);
    // 原 API 保持格式默认 128MiB/4096；先计量全局嵌套记录和真实追加 wire，再整表复制。
    if (unitCount) { const auto measured = MeasureClassicPlacementWire(*document.Units(), unitRows); if (!measured.Succeeded()) return {std::nullopt, measured.diagnostics}; }
    if (doodadCount) { const auto measured = MeasureClassicPlacementWire(*document.Doodads(), doodadRows); if (!measured.Succeeded()) return {std::nullopt, measured.diagnostics}; }
    const auto firstUnit = unitCount ? NextCreationId(document.Units()->units) : 0;
    const auto firstDoodad = doodadCount ? NextCreationId(document.Doodads()->doodads) : 0;
    PlacementEditCandidate candidate;
    candidate.newIds.reserve(ids.size());
    if (unitCount) candidate.units = *document.Units();
    if (doodadCount) candidate.doodads = *document.Doodads();
    std::size_t nextUnit = 0, nextDoodad = 0;
    for (const auto& id : ids) {
        if (id.file == PlacementFile::Units) {
            auto copy = document.Units()->units[id.index];
            for (std::size_t axis = 0; axis < 3; ++axis)
                copy.position[axis] = static_cast<float>(double(copy.position[axis]) + offset[axis]);
            copy.creationId = static_cast<std::int32_t>(std::uint64_t(firstUnit) + nextUnit++);
            candidate.units->units.push_back(std::move(copy));
            candidate.newIds.push_back(MakePlacementId(PlacementFile::Units,
                static_cast<std::uint32_t>(candidate.units->units.size() - 1), candidate.units->units.back()));
        } else {
            auto copy = document.Doodads()->doodads[id.index];
            for (std::size_t axis = 0; axis < 3; ++axis)
                copy.position[axis] = static_cast<float>(double(copy.position[axis]) + offset[axis]);
            copy.creationId = static_cast<std::int32_t>(std::uint64_t(firstDoodad) + nextDoodad++);
            candidate.doodads->doodads.push_back(std::move(copy));
            candidate.newIds.push_back(MakePlacementId(PlacementFile::Doodads,
                static_cast<std::uint32_t>(candidate.doodads->doodads.size() - 1), candidate.doodads->doodads.back()));
        }
    }
    if (unitCount) candidate.lineage.push_back(Rows(PlacementFile::Units, document.Units()->units.size(), candidate.units->units.size()));
    if (doodadCount) candidate.lineage.push_back(Rows(PlacementFile::Doodads, document.Doodads()->doodads.size(), candidate.doodads->doodads.size()));
    return Result<PlacementEditCandidate>::Success(std::move(candidate));
}

Result<std::vector<PlacementId>> CopyPlacements(MapDataEditSession& session,
    std::span<const PlacementId> ids, std::array<float, 3> offset) {
    if (ids.empty()) return Result<std::vector<PlacementId>>::Success({});
    const auto document = session.Snapshot();
    auto candidate = detail::PreparePlacementCopies(document, ids, offset);
    if (!candidate.Succeeded()) return {std::nullopt, candidate.diagnostics};
    const auto status = detail::PlacementEditingAccess::Commit(session, document,
        candidate.value->units ? &*candidate.value->units : nullptr,
        candidate.value->doodads ? &*candidate.value->doodads : nullptr, std::move(candidate.value->lineage));
    if (!status.Succeeded()) return {std::nullopt, status.diagnostics};
    return Result<std::vector<PlacementId>>::Success(std::move(candidate.value->newIds));
}

RegionId MakeRegionId(std::uint32_t index, const RegionRecord& region) { return {index, region.name, region.creationNumber}; }
CameraId MakeCameraId(std::uint32_t index, const CameraRecord& camera) { return {index, camera.name}; }

Result<RegionId> AddRegion(MapDataEditSession& session, RegionRecord record) {
    const auto fail = [](const char* code, std::string message) {
        return Result<RegionId>{std::nullopt, Failure(code, std::move(message)).diagnostics};
    };
    if (!FiniteCoordinate(record.left) || !FiniteCoordinate(record.bottom) ||
        !FiniteCoordinate(record.right) || !FiniteCoordinate(record.top) ||
        !(record.left < record.right) || !(record.bottom < record.top))
        return fail("REGION_BOUNDS_INVALID", "新区域边界须有限且左 < 右、下 < 上");
    if (record.name.empty() || record.name.size() > 4096 || record.name.find('\0') != std::string::npos ||
        record.ambientSound.size() > 4096 || record.ambientSound.find('\0') != std::string::npos ||
        record.creationNumber < 0)
        return fail("REGION_RECORD_INVALID", "新区域名称、环境音效或创建编号无效");
    const auto document = session.Snapshot();
    if (!document.Regions() || document.Regions()->version != 5)
        return fail("REGION_FILE_UNAVAILABLE", "经典区域成员缺失或版本不支持，不能创建区域");
    auto regions = *document.Regions();
    if (regions.entries.size() >= 100000)
        return fail("REGION_COUNT_LIMIT", "区域数量已达到成员上限");
    if (std::any_of(regions.entries.begin(), regions.entries.end(), [&](const auto& existing) {
            return existing.name == record.name || existing.creationNumber == record.creationNumber;
        }))
        return fail("REGION_ID_DUPLICATE", "区域名称或创建编号已存在");
    regions.entries.push_back(std::move(record));
    const auto id = MakeRegionId(static_cast<std::uint32_t>(regions.entries.size() - 1), regions.entries.back());
    const auto status = session.EditRegions(regions);
    if (!status.Succeeded()) return {std::nullopt, status.diagnostics};
    return Result<RegionId>::Success(id);
}

Result<CameraId> AddCamera(MapDataEditSession& session, CameraRecord record) {
    const auto fail = [](const char* code, std::string message) {
        return Result<CameraId>{std::nullopt, Failure(code, std::move(message)).diagnostics};
    };
    const std::array<float, 10> values{record.targetX, record.targetY, record.zOffset, record.rotation,
        record.angleOfAttack, record.targetDistance, record.roll, record.fieldOfView,
        record.farClippingPlane, record.nearClippingPlane};
    if (!std::all_of(values.begin(), values.end(), FiniteCoordinate) || record.name.empty() ||
        record.name.size() > 4096 || record.name.find('\0') != std::string::npos)
        return fail("CAMERA_RECORD_INVALID", "新镜头包含无效名称或非有限/超范围数值");
    const auto document = session.Snapshot();
    if (!document.Cameras() || document.Cameras()->version != 0)
        return fail("CAMERA_FILE_UNAVAILABLE", "经典镜头成员缺失或版本不支持，不能创建镜头");
    auto cameras = *document.Cameras();
    if (cameras.entries.size() >= 100000)
        return fail("CAMERA_COUNT_LIMIT", "镜头数量已达到成员上限");
    if (std::any_of(cameras.entries.begin(), cameras.entries.end(), [&](const auto& existing) {
            return existing.name == record.name;
        }))
        return fail("CAMERA_ID_DUPLICATE", "镜头名称已存在");
    cameras.entries.push_back(std::move(record));
    const auto id = MakeCameraId(static_cast<std::uint32_t>(cameras.entries.size() - 1), cameras.entries.back());
    const auto status = session.EditCameras(cameras);
    if (!status.Succeeded()) return {std::nullopt, status.diagnostics};
    return Result<CameraId>::Success(id);
}

Status SetRegionBounds(MapDataEditSession& session, const RegionId& id, float left, float bottom, float right, float top) {
    if (!FiniteCoordinate(left) || !FiniteCoordinate(bottom) || !FiniteCoordinate(right) || !FiniteCoordinate(top)
        || !(left < right) || !(bottom < top))
        return Failure("REGION_BOUNDS_INVALID", "区域边界无效：须有限且左 < 右、下 < 上");
    const auto document = session.Snapshot();
    if (!document.Regions() || id.index >= document.Regions()->entries.size()) return StaleAuxiliary(id);
    auto regions = *document.Regions();
    auto& region = regions.entries[id.index];
    if (region.name != id.name || region.creationNumber != id.creationNumber) return StaleAuxiliary(id);
    region.left = left; region.bottom = bottom; region.right = right; region.top = top;
    return session.EditRegions(regions);
}

Status SetCameraTarget(MapDataEditSession& session, const CameraId& id, float x, float y) {
    if (!FiniteCoordinate(x) || !FiniteCoordinate(y)) return Failure("CAMERA_TARGET_INVALID", "镜头目标坐标无效");
    const auto document = session.Snapshot();
    if (!document.Cameras() || id.index >= document.Cameras()->entries.size()) return StaleAuxiliary(id);
    auto cameras = *document.Cameras();
    auto& camera = cameras.entries[id.index];
    if (camera.name != id.name) return StaleAuxiliary(id);
    camera.targetX = x; camera.targetY = y;
    return session.EditCameras(cameras);
}

} // namespace War3Map::MapData
