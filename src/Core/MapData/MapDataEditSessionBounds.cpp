#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/MapData/TerrainEditInspection.h"
#include "War3MapCore/MapData/TerrainPathingResources.h"

#include <bit>
#include <new>
#include <stdexcept>

namespace War3Map::MapData {
namespace {
Status Refuse(const char* code, const char* message) {
    return Status::Failure("MapData.Edit", code, message);
}
bool SameCamera(const std::array<float, 8>& a, const std::array<float, 8>& b) noexcept {
    return std::bit_cast<std::array<std::uint32_t, 8>>(a) == std::bit_cast<std::array<std::uint32_t, 8>>(b);
}
}

std::shared_ptr<const MapDataEditSession::TerrainBoundsState> MapDataEditSession::TerrainBoundsState::Capture(
    const MapSettings& settings, const TerrainGridSnapshot& terrain) {
    auto state = std::make_shared<TerrainBoundsState>();
    state->cameraBounds = settings.cameraBounds; state->cameraMargins = settings.cameraMargins;
    state->playableWidth = settings.playableWidth; state->playableHeight = settings.playableHeight;
    state->flags = settings.flags & 0x400u; state->width = terrain.header.width; state->height = terrain.header.height;
    state->mapEdges.resize((terrain.vertices.size() + 7) / 8);
    for (std::size_t i = 0; i < terrain.vertices.size(); ++i)
        if (terrain.vertices[i].IsMapEdge()) state->mapEdges[i / 8] |= std::byte(1u << (i % 8));
    return state;
}

bool MapDataEditSession::TerrainBoundsState::Matches(const MapSettings& settings,
    const TerrainGridSnapshot& terrain) const noexcept {
    if (!MatchesSettings(settings) || width != terrain.header.width || height != terrain.header.height
        || terrain.vertices.size() != static_cast<std::size_t>(width) * height
        || mapEdges.size() != (terrain.vertices.size() + 7) / 8) return false;
    for (std::size_t i = 0; i < terrain.vertices.size(); ++i)
        if (terrain.vertices[i].IsMapEdge() != ((mapEdges[i / 8] & std::byte(1u << (i % 8))) != std::byte{0})) return false;
    return true;
}

bool MapDataEditSession::TerrainBoundsState::MatchesSettings(const MapSettings& settings) const noexcept {
    return SameCamera(cameraBounds, settings.cameraBounds) && cameraMargins == settings.cameraMargins
        && playableWidth == settings.playableWidth && playableHeight == settings.playableHeight && flags == (settings.flags & 0x400u);
}

bool MapDataEditSession::TerrainBoundsState::MatchesSettingsBytes(std::span<const std::byte> bytes) const noexcept {
    // v25的geometry前只有三个I32和四个零结尾说明串。无分配地读取已由会话
    // 完整解析过的窄前缀，供noexcept dirty检测覆盖历史Replay/Undo的raw设置。
    const auto number = [&](std::size_t offset) {
        std::uint32_t value = 0;
        for (unsigned i = 0; i < 4; ++i) value |= std::to_integer<std::uint32_t>(bytes[offset + i]) << (i * 8);
        return value;
    };
    if (bytes.size() < 12 || number(0) != 25) return false;
    std::size_t cursor = 12;
    for (unsigned field = 0; field < 4; ++field) {
        while (cursor < bytes.size() && bytes[cursor] != std::byte{0}) ++cursor;
        if (cursor == bytes.size()) return false;
        ++cursor;
    }
    if (bytes.size() - cursor < 60) return false;
    for (const auto value : cameraBounds) { if (number(cursor) != std::bit_cast<std::uint32_t>(value)) return false; cursor += 4; }
    for (const auto value : cameraMargins) { if (number(cursor) != std::bit_cast<std::uint32_t>(value)) return false; cursor += 4; }
    if (number(cursor) != std::bit_cast<std::uint32_t>(playableWidth)
        || number(cursor + 4) != std::bit_cast<std::uint32_t>(playableHeight)) return false;
    return (number(cursor + 8) & 0x400u) == flags;
}

bool MapDataEditSession::TerrainBoundsState::Equivalent(const TerrainBoundsState& other) const noexcept {
    return SameCamera(cameraBounds, other.cameraBounds) && cameraMargins == other.cameraMargins
        && playableWidth == other.playableWidth && playableHeight == other.playableHeight && flags == other.flags
        && width == other.width && height == other.height && mapEdges == other.mapEdges;
}

void MapDataEditSession::TerrainBoundsState::RestoreGeometry(MapSettings& settings) const noexcept {
    settings.cameraBounds = cameraBounds; settings.cameraMargins = cameraMargins;
    settings.playableWidth = playableWidth; settings.playableHeight = playableHeight;
    settings.flags = (settings.flags & ~0x400u) | flags;
}

bool MapDataEditSession::HasTerrainBoundsChanges() const noexcept {
    const auto& settings = current_[Index(MapDataKind::Settings)];
    return boundsState_ && (!savedBoundsState_ || !boundsState_->Equivalent(*savedBoundsState_)
        || !settings || !boundsState_->MatchesSettingsBytes(*settings));
}

Status MapDataEditSession::VerifyTerrainSettingsForSave() const try {
    const auto& before = original_[Index(MapDataKind::Settings)];
    const auto& after = current_[Index(MapDataKind::Settings)];
    if (!boundsState_) return ValidateTerrainSettingsPathingChange(before, after);
    if (!savedBoundsState_ || !before || !after || !terrainOriginal_ || !terrain_
        || before->size() > 1024u * 1024u || after->size() > 1024u * 1024u)
        return Refuse("TERRAIN_BOUNDS_STATE_UNAVAILABLE", "地图范围缺少完整可信保存状态。");
    auto original = MapSettings::Parse(*before), current = MapSettings::Parse(*after);
    if (!original.Succeeded() || !original.value || !current.Succeeded() || !current.value
        || !original.value->FullyParsed() || !current.value->FullyParsed()
        || original.value->formatVersion != 25 || current.value->formatVersion != 25)
        return Refuse("TERRAIN_BOUNDS_STATE_CHANGED", "地图范围的完整W3I布局已改变。");
    const auto oldWire = original.value->Serialize(), currentWire = current.value->Serialize();
    if (!oldWire.Succeeded() || !oldWire.value || *oldWire.value != *before
        || !currentWire.Succeeded() || !currentWire.value || *currentWire.value != *after
        || !savedBoundsState_->Matches(*original.value, *terrainOriginal_)
        || !boundsState_->Matches(*current.value, *terrain_))
        return Refuse("TERRAIN_BOUNDS_STATE_CHANGED", "范围字段或MapEdge已脱离可信命令状态，不能通过原始字节覆写保存。");
    savedBoundsState_->RestoreGeometry(*current.value);
    auto normalized = current.value->Serialize();
    if (!normalized.Succeeded() || !normalized.value) return {normalized.diagnostics};
    return ValidateTerrainSettingsPathingChange(before, normalized.value);
} catch (const std::bad_alloc&) {
    return Refuse("TERRAIN_BOUNDS_BUDGET", "范围保存检查内存不足，原编辑保留。");
} catch (const std::length_error&) {
    return Refuse("TERRAIN_BOUNDS_BUDGET", "范围保存检查容量超限，原编辑保留。");
}

Result<MapDataEditSession::PreparedTerrainBoundsEdit> MapDataEditSession::PrepareTerrainBoundsEdit(
    const TerrainPathingResources& resources, const TerrainBoundsEditIntent& intent,
    std::size_t maximumBytes, std::size_t maximumWork) const try {
    using Output = Result<PreparedTerrainBoundsEdit>;
    const auto fail = [](const char* code, const char* message) { return Output::Failure("MapData.Edit", code, message); };
    if (maximumBytes > 512u * 1024u * 1024u || maximumWork > 64u * 1024u * 1024u)
        return fail("TERRAIN_BOUNDS_LIMITS", "地图范围准备预算只能收紧硬上限。");
    const auto& oldSettings = original_[Index(MapDataKind::Settings)];
    const auto& currentSettings = current_[Index(MapDataKind::Settings)];
    if (!terrain_ || !terrainOriginal_ || !pathing_ || !pathingOriginal_ || !oldSettings || !currentSettings
        || !resources.NativeCameraMargins() || terrain_->header.width < 2 || terrain_->header.width > 2049
        || terrain_->header.height < 2 || terrain_->header.height > 2049
        || terrain_->vertices.size() != static_cast<std::size_t>(terrain_->header.width) * terrain_->header.height)
        return fail("TERRAIN_BOUNDS_SOURCE_UNAVAILABLE", "范围编辑需要完整地形、通行、设置和实际原版规则。");
    // 预收两次候选网格、发布网格、undo前后raw、设置解析/全文复核与密封bitset。
    // 不依赖调用者声称的输入长度；每一项来自当前实际容器。
    std::size_t cost = 4096;
    const auto reserve = [&](std::size_t count, std::size_t element = 1) {
        if (cost > maximumBytes || (element && count > (maximumBytes - cost) / element)) return false;
        cost += count * element; return true;
    };
    const auto vertices = terrain_->vertices.size();
    if (!reserve(vertices, 3 * sizeof(TerrainVertex) + 28)
        || !reserve(terrain_->tiles.size(), 3 * sizeof(TerrainTile))
        || !reserve(oldSettings->size(), 8) || !reserve(currentSettings->size(), 8)
        || !reserve((vertices + 7) / 8, 4)
        || vertices > maximumWork / 16)
        return fail("TERRAIN_BOUNDS_BUDGET", "范围候选、撤销或逐顶点检查超出本次预算。");
    const auto input = resources.VerifyTerrainBoundsInput(*terrainOriginal_, *terrain_, *oldSettings);
    if (!input.Succeeded()) return {{}, input.diagnostics};
    const auto invariant = VerifyTerrainSettingsForSave();
    if (!invariant.Succeeded()) return {{}, invariant.diagnostics};
    auto parsed = MapSettings::Parse(*currentSettings);
    if (!parsed.Succeeded() || !parsed.value) return {{}, parsed.diagnostics};
    auto projected = ProjectTerrainPlayableBounds(*terrain_, *parsed.value, *resources.NativeCameraMargins(), intent);
    if (!projected.Succeeded() || !projected.value) return {{}, projected.diagnostics};
    auto next = std::make_shared<TerrainGridSnapshot>(*terrain_);
    auto edges = ApplyTerrainPlayableEdges(*next, intent.playable,
        static_cast<std::size_t>(next->header.width - 1) * (next->header.height - 1));
    if (!edges.Succeeded() || !edges.value) return {{}, edges.diagnostics};
    auto beforeState = boundsState_ ? boundsState_ : TerrainBoundsState::Capture(*parsed.value, *terrain_);
    if (!boundsState_) {
        const auto sourceSettings = MapSettings::Parse(*oldSettings);
        if (!sourceSettings.Succeeded() || !sourceSettings.value) return {{}, sourceSettings.diagnostics};
        const auto baseline = TerrainBoundsState::Capture(*sourceSettings.value, *terrainOriginal_);
        if (!beforeState->Equivalent(*baseline))
            return fail("TERRAIN_BOUNDS_STATE_CHANGED", "范围编辑之前已有未经可信命令记录的MapEdge或几何变化。");
    }
    auto afterState = TerrainBoundsState::Capture(*projected.value, *next);
    PreparedTerrainBoundsEdit prepared;
    prepared.before_ = terrain_; prepared.beforeBounds_ = boundsState_;
    prepared.sessionIdentity_ = sessionIdentity_; prepared.revision_ = revision_; prepared.sourceRevision_ = terrainSourceRevision_;
    prepared.intent_ = intent; prepared.cameraBounds_ = projected.value->cameraBounds;
    prepared.changedEdges_ = *edges.value; prepared.preparationBytes_ = cost; prepared.preparationWork_ = vertices * 16;
    if (beforeState->Equivalent(*afterState)) return Output::Success(std::move(prepared));
    const TerrainStrokeResult changed{0, 0, next->header.width, next->header.height};
    auto candidate = PrepareTerrainCandidate(next, changed, {}, false, false, false, false);
    if (!candidate.Succeeded() || !candidate.value) return {{}, candidate.diagnostics};
    auto command = *candidate.value->command_;
    const auto settings = Prepare(MapDataKind::Settings, projected.value->Serialize(), command.changes, true);
    if (!settings.Succeeded()) return {{}, settings.diagnostics};
    auto fingerprint = resources.TerrainBoundsDependencyFingerprint();
    if (!fingerprint.Succeeded() || !fingerprint.value) return {{}, fingerprint.diagnostics};
    command.boundsBefore = std::move(beforeState); command.boundsAfter = std::move(afterState);
    command.boundsIntent = intent; command.boundsRulesSha256 = std::move(*fingerprint.value);
    prepared.command_ = std::make_shared<const Command>(std::move(command));
    return Output::Success(std::move(prepared));
} catch (const std::bad_alloc&) {
    return Result<PreparedTerrainBoundsEdit>::Failure("MapData.Edit", "TERRAIN_BOUNDS_BUDGET", "范围候选内存不足，未修改会话。");
} catch (const std::length_error&) {
    return Result<PreparedTerrainBoundsEdit>::Failure("MapData.Edit", "TERRAIN_BOUNDS_BUDGET", "范围候选容量超限，未修改会话。");
}

Status MapDataEditSession::ApplyPreparedTerrainBoundsEdit(const PreparedTerrainBoundsEdit& plan,
    const std::function<Status(const EditReceipt&)>& prepublish) {
    if (plan.sessionIdentity_ != sessionIdentity_ || plan.revision_ != revision_
        || plan.sourceRevision_ != terrainSourceRevision_ || plan.before_ != terrain_ || plan.beforeBounds_ != boundsState_)
        return Refuse("TERRAIN_BOUNDS_EDIT_STALE", "范围候选不属于当前会话、修订或地形来源，请重新预览。");
    if (!plan.command_) return Status::Ok();
    return CommitCommand(*plan.command_, prepublish);
}

} // namespace War3Map::MapData
