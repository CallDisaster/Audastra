#include "War3MapCore/Services/TerrainSessionResources.h"
#include "War3MapCore/ObjectData/NativeDestructablePathing.h"
#include "TerrainPathingFinalOverlay.h"

#include <array>
#include <algorithm>
#include <exception>

namespace War3Map::Services {
namespace {
constexpr std::size_t HardBytes = 64u * 1024u * 1024u;
using Resources = MapData::TerrainPathingResources;
using Reader = Resources::HeightSourceReader;
Status Failure(const char* code, const char* message) {
    return Status::Failure("Terrain.SessionResources", code, message);
}
template<class T> Result<T> Failed(const Status& status) { return {{}, status.diagnostics}; }
}

struct TerrainSessionResources::State {
    Reader physical;
    std::shared_ptr<const MapData::MapDataEditSession> session;
    std::filesystem::path gameRoot;
    std::vector<MemberReplacement> members;
    std::optional<MapData::DoodadPlacements> doodads;
    std::optional<TerrainPathingFinalDetail::FinalMemberOverlay> overlay;
    std::size_t ownedBytes = 0, maximumBytes = 0, physicalTerrainBytes = 0;

    Status VerifyPhysical(std::size_t maximum) const {
        if (!maximum || maximum > maximumBytes || ownedBytes >= maximum)
            return Failure("TERRAIN_SESSION_RESOURCE_BUDGET", "当前结构资源的复核预算不足或被放宽。");
        const auto remaining = maximum - ownedBytes;
        // Core身份核验会重写旧W3E；actual读集仍驻留时必须为writer峰值留余量。
        if (!physicalTerrainBytes || physicalTerrainBytes > remaining / 4)
            return Failure("TERRAIN_SESSION_RESOURCE_BUDGET", "原地形身份重写与完整来源读集超过剩余预算。");
        std::vector<std::string> names;
        for (const auto name : MapData::TerrainJournalMemberNames) names.emplace_back(name);
        ArchiveExactMemberLimits limits;
        limits.memberCount = names.size(); limits.memberBytes = limits.totalBytes = remaining - physicalTerrainBytes * 3;
        auto actual = physical(names, limits);
        if (!actual.Succeeded() || !actual.value) return {std::move(actual.diagnostics)};
        return session->VerifyTerrainJournalBaseline(*actual.value);
    }
    Result<std::vector<ArchiveExactMember>> Read(std::span<const std::string> names,
        const ArchiveExactMemberLimits& limits) const {
        return overlay->Read(physical, names, limits, nullptr);
    }
};

Result<TerrainSessionResources> TerrainSessionResources::CaptureInputs(const MemberReader& reader,
    std::shared_ptr<const MapData::MapDataEditSession> session,
    const std::filesystem::path& gameRoot, std::size_t maximumBytes, std::size_t additionalOwnedBytes) try {
    using Output = Result<TerrainSessionResources>;
    if (!session || !session->Terrain() || !reader || !maximumBytes || maximumBytes > HardBytes)
        return Failed<TerrainSessionResources>(Failure("TERRAIN_SESSION_RESOURCE_INPUT", "当前结构资源需要实际来源、冻结会话和有界预算。"));
    const auto gate = session->VerifyTerrainCurrentStructureInput();
    if (!gate.Succeeded()) return {{}, gate.diagnostics};
    auto state = std::make_shared<State>(); state->physical = reader;
    state->session = std::move(session); state->gameRoot = gameRoot; state->maximumBytes = maximumBytes;
    if (additionalOwnedBytes >= maximumBytes)
        return Failed<TerrainSessionResources>(Failure("TERRAIN_SESSION_RESOURCE_BUDGET", "来源快照超过当前结构捕获预算。"));
    state->ownedBytes = sizeof(State) + 8192 + additionalOwnedBytes
        + state->gameRoot.native().capacity() * sizeof(std::filesystem::path::value_type);
    if (state->ownedBytes >= maximumBytes)
        return Failed<TerrainSessionResources>(Failure("TERRAIN_SESSION_RESOURCE_BUDGET", "当前结构元数据超过捕获预算。"));
    {
        const std::array<std::string, 1> name{"war3map.w3e"};
        ArchiveExactMemberLimits limits;
        limits.memberCount = 1; limits.memberBytes = limits.totalBytes = (maximumBytes - state->ownedBytes) / 4;
        auto terrain = reader(name, limits);
        if (!terrain.Succeeded() || !terrain.value) return {{}, std::move(terrain.diagnostics)};
        if (terrain.value->size() != 1 || !terrain.value->front().present || terrain.value->front().bytes.empty())
            return Failed<TerrainSessionResources>(Failure("TERRAIN_SESSION_RESOURCE_LAYOUT", "当前结构缺少唯一物理来源地形。"));
        state->physicalTerrainBytes = terrain.value->front().bytes.size();
    }
    const auto physical = state->VerifyPhysical(maximumBytes);
    if (!physical.Succeeded()) return {{}, physical.diagnostics};
    const auto append = [&](std::string name, Result<std::vector<std::byte>> wire) -> Status {
        if (!wire.Succeeded() || !wire.value) return {std::move(wire.diagnostics)};
        if (wire.value->capacity() > (maximumBytes - state->ownedBytes) / 2)
            return Failure("TERRAIN_SESSION_RESOURCE_BUDGET", "当前结构正文和读取副本超过捕获预算。");
        state->ownedBytes += wire.value->capacity() * 2 + name.capacity() + sizeof(MemberReplacement);
        if (state->ownedBytes >= maximumBytes)
            return Failure("TERRAIN_SESSION_RESOURCE_BUDGET", "当前结构正文没有留下资源读取预算。");
        state->members.push_back({std::move(name), std::move(*wire.value)}); return Status::Ok();
    };
    const auto& grid = *state->session->Terrain();
    const auto tableCount = grid.header.groundTilesets.size() + grid.header.cliffTilesets.size();
    if (tableCount > 512 || grid.vertices.size() > (maximumBytes - state->ownedBytes) / 28
        || 37 + tableCount * 4 > (maximumBytes - state->ownedBytes) / 4 - grid.vertices.size() * 7)
        return Failed<TerrainSessionResources>(Failure("TERRAIN_SESSION_RESOURCE_BUDGET", "当前W3E的writer、保留和读取副本超过捕获预算。"));
    auto status = append("war3map.w3e", MapData::SerializeMapTerrain(grid));
    if (!status.Succeeded()) return {{}, status.diagnostics};
    // 每个结构单独有界解析，不为捕获一笔水刷深拷贝整个地图文档。
    for (const auto kind : {MapDataKind::Settings, MapDataKind::Doodads, MapDataKind::Units}) {
        const auto size = state->session->SnapshotByteSize(kind);
        if (!size.Succeeded() || !size.value) return {{}, size.diagnostics};
        const auto remaining = maximumBytes - state->ownedBytes;
        if (*size.value > remaining / 32)
            return Failed<TerrainSessionResources>(Failure("TERRAIN_SESSION_RESOURCE_BUDGET", "当前结构解析、保留及读取副本超过捕获预算。"));
        auto snapshot = state->session->Snapshot(kind, remaining / 32);
        if (!snapshot.Succeeded() || !snapshot.value) return {{}, std::move(snapshot.diagnostics)};
        const auto& member = snapshot.value->Member(kind);
        if (kind == MapDataKind::Units && member.state == MapDataState::Missing && member.sources.empty()) continue;
        if (member.state != MapDataState::Ready || member.sources.size() != 1
            || (kind == MapDataKind::Settings && (!snapshot.value->Settings() || !snapshot.value->Settings()->FullyParsed()
                || snapshot.value->Settings()->formatVersion != 25))
            || (kind == MapDataKind::Doodads && !snapshot.value->Doodads()))
            return Failed<TerrainSessionResources>(Failure("TERRAIN_SESSION_RESOURCE_LAYOUT", "当前结构需要完整的经典设置和摆放，不能把未知当成缺失。"));
        const std::string name = kind == MapDataKind::Settings ? "war3map.w3i"
            : kind == MapDataKind::Doodads ? "war3map.doo" : "war3mapUnits.doo";
        status = append(name, Result<std::vector<std::byte>>::Success(member.sources.front().data));
        if (!status.Succeeded()) return {{}, status.diagnostics};
        // 预留解析对象及当前/读取副本峰值；DOO在owner内保活到最后一笔复核。
        const auto parseBytes = *size.value * 30;
        if (parseBytes > maximumBytes - state->ownedBytes)
            return Failed<TerrainSessionResources>(Failure("TERRAIN_SESSION_RESOURCE_BUDGET", "当前结构实际解析保留超过捕获预算。"));
        state->ownedBytes += parseBytes;
        if (kind == MapDataKind::Doodads) state->doodads = *snapshot.value->Doodads();
    }
    auto overlay = TerrainPathingFinalDetail::FinalMemberOverlay::Prepare(state->members, {}, maximumBytes - state->ownedBytes);
    if (!overlay.Succeeded() || !overlay.value) return {{}, std::move(overlay.diagnostics)};
    state->ownedBytes += overlay.value->MetadataBytes(); state->overlay.emplace(std::move(*overlay.value));
    return Output::Success(TerrainSessionResources(std::move(state)));
} catch (const std::exception&) {
    return Failed<TerrainSessionResources>(Failure("TERRAIN_SESSION_RESOURCE_CAPTURE", "当前结构资源准备失败，活动地图未修改。"));
}

Result<TerrainSessionResources> TerrainSessionResources::Capture(std::shared_ptr<const MapArchiveSession> source,
    std::shared_ptr<const MapData::MapDataEditSession> session, const std::filesystem::path& gameRoot,
    std::size_t maximumBytes) {
    if (!source) return Failed<TerrainSessionResources>(Failure("TERRAIN_SESSION_RESOURCE_INPUT", "当前结构资源缺少实际地图归档。"));
    return CaptureInputs([source = std::move(source)](auto names, const auto& limits) {
        return source->CaptureExactMembers(names, limits);
    }, std::move(session), gameRoot, maximumBytes);
}

Result<TerrainSessionResources> TerrainSessionResources::Capture(const WorkspaceSnapshot& source,
    const MapData::MapDataEditSession& session, const std::filesystem::path& gameRoot, std::size_t maximumBytes) try {
    if (!maximumBytes || maximumBytes > HardBytes || source.files.capacity() > maximumBytes / sizeof(WorkspaceFile))
        return Failed<TerrainSessionResources>(Failure("TERRAIN_SESSION_RESOURCE_BUDGET", "目录来源清单超过当前结构捕获预算。"));
    std::size_t retained = sizeof(WorkspaceSnapshot) + source.files.capacity() * sizeof(WorkspaceFile)
        + source.root.native().capacity() * sizeof(std::filesystem::path::value_type) + source.aggregateSha256.capacity();
    for (const auto& file : source.files) {
        const auto size = file.relativePath.native().capacity() * sizeof(std::filesystem::path::value_type)
            + file.sha256.capacity() + 2;
        if (retained >= maximumBytes || size > maximumBytes - retained)
            return Failed<TerrainSessionResources>(Failure("TERRAIN_SESSION_RESOURCE_BUDGET", "目录来源名称超过当前结构捕获预算。"));
        retained += size;
    }
    std::size_t copiedBytes = 0;
    auto frozen = session.FreezeForSave(maximumBytes, &copiedBytes);
    if (!frozen.Succeeded() || !frozen.value) return {{}, std::move(frozen.diagnostics)};
    return CaptureInputs([source](auto names, const auto& limits) {
        return Resources::ReadDirectoryMembers(source, names, limits, true);
    }, std::move(*frozen.value), gameRoot, maximumBytes,
        retained > maximumBytes || copiedBytes > maximumBytes - retained ? maximumBytes : retained + copiedBytes);
} catch (const std::exception&) {
    return Failed<TerrainSessionResources>(Failure("TERRAIN_SESSION_RESOURCE_CAPTURE", "当前目录结构来源快照无法保留。"));
}

Result<TerrainSessionResources> TerrainSessionResources::Capture(std::shared_ptr<const MapArchiveSession> source,
    const MapData::MapDataEditSession& session, const std::filesystem::path& gameRoot, std::size_t maximumBytes) {
    std::size_t copiedBytes = 0;
    auto frozen = session.FreezeForSave(maximumBytes, &copiedBytes);
    if (!frozen.Succeeded() || !frozen.value) return {{}, std::move(frozen.diagnostics)};
    if (!source) return Failed<TerrainSessionResources>(Failure("TERRAIN_SESSION_RESOURCE_INPUT", "当前结构资源缺少实际地图归档。"));
    return CaptureInputs([source = std::move(source)](auto names, const auto& limits) {
        return source->CaptureExactMembers(names, limits);
    }, std::move(*frozen.value), gameRoot, maximumBytes, copiedBytes);
}

Result<Resources> TerrainSessionResources::CaptureWater(bool enforceWaterLimits, std::size_t maximumBytes) const {
    return CaptureWater(*state_->session->Terrain(), enforceWaterLimits, maximumBytes);
}

Result<Resources> TerrainSessionResources::CaptureWater(const MapData::TerrainGridSnapshot& after,
    bool enforceWaterLimits, std::size_t maximumBytes) const {
    maximumBytes = std::min(maximumBytes, state_->maximumBytes);
    const auto checked = state_->VerifyPhysical(maximumBytes);
    if (!checked.Succeeded()) return {{}, checked.diagnostics};
    const Reader current = [state = state_](auto names, const auto& limits) { return state->Read(names, limits); };
    const auto terrain = state_->session->Terrain();
    auto resources = Resources::CaptureWaterBrushInputs(current, *terrain, after, state_->gameRoot,
        enforceWaterLimits, maximumBytes - state_->ownedBytes);
    if (!resources.Succeeded() || !resources.value) return resources;
    const auto special = resources.value->CaptureWaterSpecialInputs(current, *terrain,
        *state_->doodads, ObjectData::InspectNativeTerrainObjects);
    if (!special.Succeeded()) return {{}, special.diagnostics};
    return resources;
}

Result<Resources> TerrainSessionResources::CaptureNative(const MapData::TerrainGridSnapshot& after,
    const std::vector<std::byte>* currentDoodads, std::size_t maximumBytes) const {
    maximumBytes = std::min(maximumBytes, state_->maximumBytes);
    const auto checked = state_->VerifyPhysical(maximumBytes);
    if (!checked.Succeeded()) return {{}, checked.diagnostics};
    const Reader current = [state = state_](auto names, const auto& limits) { return state->Read(names, limits); };
    const Resources::NativeObjectValidator validator = ObjectData::InspectNativeTerrainObjects;
    return Resources::CaptureInputs(current, *state_->session->Terrain(), after, state_->gameRoot,
        maximumBytes - state_->ownedBytes, true, &validator, currentDoodads);
}

Status TerrainSessionResources::Verify(const Resources& resources, std::size_t maximumBytes) const {
    maximumBytes = std::min(maximumBytes, state_->maximumBytes);
    const auto resourceBytes = resources.OwnedCapacityBytes();
    if (resourceBytes >= maximumBytes)
        return Failure("TERRAIN_SESSION_RESOURCE_BUDGET", "规则owner与来源复核的同时驻留超过当前结构预算。");
    maximumBytes -= resourceBytes;
    const auto physical = state_->VerifyPhysical(maximumBytes);
    if (!physical.Succeeded()) return physical;
    return resources.VerifyReadbackMembers([state = state_](auto names, const auto& limits) {
        return state->Read(names, limits);
    }, maximumBytes - state_->ownedBytes, true);
}

std::size_t TerrainSessionResources::OwnedCapacityBytes() const noexcept { return state_->ownedBytes; }
} // namespace War3Map::Services
