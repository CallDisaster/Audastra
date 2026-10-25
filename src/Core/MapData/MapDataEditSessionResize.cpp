#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/MapData/TerrainResizeResources.h"
#include "War3MapCore/Hash.h"
#include "TerrainPaletteEditInternal.h"

#include <algorithm>
#include <bit>
#include <limits>
#include <new>
#include <stdexcept>

namespace War3Map::MapData {
namespace {
bool SameShape(const TerrainHeader& a, const TerrainHeader& b) noexcept {
    return a.version == b.version && a.width == b.width && a.height == b.height
        && std::bit_cast<std::array<std::uint32_t, 2>>(a.offset) == std::bit_cast<std::array<std::uint32_t, 2>>(b.offset);
}
void Write32(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes[offset + i] = std::byte((value >> (8 * i)) & 255);
}
std::vector<std::byte> MoveCells(std::span<const std::byte> old, std::uint32_t oldWidth, std::uint32_t oldHeight,
    std::uint32_t newWidth, std::uint32_t newHeight, const TerrainCanvasRectangle& rectangle,
    bool shadow = false) {
    std::vector<std::byte> output(std::size_t(newWidth) * newHeight);
    const auto west = std::int64_t(rectangle.west) * 4, south = std::int64_t(rectangle.south) * 4;
    for (std::uint32_t y = 0; y < newHeight; ++y) {
        const auto sy = std::int64_t(y) + south;
        if (sy < 0 || sy >= oldHeight) continue;
        for (std::uint32_t x = 0; x < newWidth; ++x) {
            const auto sx = std::int64_t(x) + west;
            if (sx < 0 || sx >= oldWidth) continue;
            const auto value = old[std::size_t(sy) * oldWidth + sx];
            // 磁盘非零在原版load时先成为runtime bit0；writer再把该位写为FF。
            output[std::size_t(y) * newWidth + x] = shadow ? (value == std::byte{0} ? std::byte{0} : std::byte{0xff}) : value;
        }
    }
    return output;
}
std::vector<std::byte> MoveWpm(std::span<const std::byte> old, std::uint32_t oldWidth, std::uint32_t oldHeight,
    std::uint32_t newWidth, std::uint32_t newHeight, const TerrainCanvasRectangle& rectangle) {
    auto cells = MoveCells(old.subspan(16), oldWidth, oldHeight, newWidth, newHeight, rectangle);
    std::vector<std::byte> output(old.begin(), old.begin() + 16);
    Write32(output, 8, newWidth); Write32(output, 12, newHeight);
    output.insert(output.end(), cells.begin(), cells.end()); return output;
}
}

const std::vector<std::byte>& MapDataEditSession::TerrainHistoryPathing(const TerrainResizeState* shape) const {
    const auto* target = shape ? shape : resizeState_ ? resizeState_.get() : resizeDraftState_.get();
    if (target && target->identity && savedResizeState_ && target->identity == savedResizeState_->identity && pathingOriginal_)
        return *pathingOriginal_;
    if (target && target->historyPathing) return *target->historyPathing;
    if (!pathingOriginal_) throw std::logic_error("terrain pathing history baseline missing");
    return *pathingOriginal_;
}

std::shared_ptr<const std::vector<std::byte>> MapDataEditSession::TerrainHistoryProtected(const TerrainResizeState* shape) const {
    const auto* target = shape ? shape : resizeState_ ? resizeState_.get() : resizeDraftState_.get();
    if (target && target->identity && savedResizeState_ && target->identity == savedResizeState_->identity)
        return savedResizeState_->protectedPathing;
    return target ? target->protectedPathing : pathingProtected_;
}

std::shared_ptr<const std::vector<std::byte>> MapDataEditSession::TerrainHistorySavedRestrictions(const TerrainResizeState* shape) const {
    const auto* target = shape ? shape : resizeState_ ? resizeState_.get() : resizeDraftState_.get();
    if (target && target->identity && savedResizeState_ && target->identity == savedResizeState_->identity)
        return originalPathingRestrictions_;
    return target ? target->savedRestrictions : originalPathingRestrictions_;
}

const ArchiveExactMember* MapDataEditSession::TerrainHistoryShadow(const TerrainResizeState* shape) const noexcept {
    const auto* target = shape ? shape : resizeState_ ? resizeState_.get() : resizeDraftState_.get();
    if (target && target->identity && savedResizeState_ && target->identity == savedResizeState_->identity)
        return &savedResizeState_->shadow;
    return target ? &target->shadow : nullptr;
}

bool MapDataEditSession::HasTerrainResizeChanges() const noexcept {
    return bool(resizeDraftState_) || (resizeState_ && (!savedResizeState_ || resizeState_->identity != savedResizeState_->identity));
}

Status MapDataEditSession::VerifyTerrainCurrentStructureInput() const {
    return resizeState_ ? VerifyTerrainResizeForSave() : VerifyTerrainPathingForSave();
}

Result<std::size_t> MapDataEditSession::MeasureTerrainResizeResourceValidation(
    std::size_t maximumBytes, std::size_t maximumWork) const {
    using Output = Result<std::size_t>;
    if (!resizeState_) return Output::Success(0);
    const auto fail = [] { return Output::Failure("Terrain.Resize", "TERRAIN_RESIZE_BUDGET",
        "扩裁当前结构的私有几何、通行分层和设置验证超过本笔预算。"); };
    if (!terrain_ || !terrainOriginal_ || !pathing_) return fail();
    std::size_t bytes = 0, work = 0;
    const auto add = [](std::size_t& used, std::size_t limit, std::size_t count, std::size_t unit) {
        if (used > limit || (unit && count > (limit - used) / unit)) return false;
        used += count * unit; return true;
    };
    // RawInput逐顶点一次、当前/原始Bounds各一次，分层逐通行格一次。
    if (!add(bytes, maximumBytes, 8192, 1)
        || !add(work, maximumWork, terrain_->vertices.size(), 2)
        || !add(work, maximumWork, terrainOriginal_->vertices.size(), 1)
        || !add(work, maximumWork, pathing_->size(), 1)) return fail();
    const auto index = Index(MapDataKind::Settings);
    for (const auto* raw : {&original_[index], &current_[index]}) {
        if (!*raw || !add(bytes, maximumBytes, (*raw)->size(), 8)
            || !add(work, maximumWork, (*raw)->size(), 8)) return fail();
    }
    return Output::Success(work);
}

Status MapDataEditSession::VerifyTerrainResizeRawInput() const {
    const auto fail = [](const char* message) { return Status::Failure("Terrain.Resize", "TERRAIN_RESIZE_RAW_INPUT", message); };
    if (!terrain_ || !terrainOriginal_) return fail("扩裁缺少完整原始及当前地形。");
    const auto& current = terrain_->header; const auto& physical = terrainOriginal_->header;
    if (current.version != 11 || physical.version != 11 || current.mainTileset != physical.mainTileset
        || current.groundTilesets.empty() || current.groundTilesets.size() > 16
        || (current.cliffTilesets != physical.cliffTilesets
            && !TerrainPaletteEditDetail::SupportsCliffTableDelta(physical.cliffTilesets, current.cliffTilesets)))
        return fail("扩裁不能把未支持的主地形、布局或悬崖表变化封存为可信几何。");
    const auto before = resizeState_ ? resizeState_->geometry : terrainOriginal_;
    if (!before || !SameShape(before->header, current) || before->vertices.size() != terrain_->vertices.size())
        return fail("扩裁前网格不属于已有typed形状或真实原始形状。");
    for (std::size_t i = 0; i < before->vertices.size(); ++i) {
        const auto& a = before->vertices[i]; const auto& b = terrain_->vertices[i];
        if (a.extendedTextureLayout || b.extendedTextureLayout || a.textureFlagsExtensionRaw != b.textureFlagsExtensionRaw
            || ((a.waterHeightAndFlagsRaw ^ b.waterHeightAndFlagsRaw) & 0x8000u)
            || ((a.groundHeightRaw ^ b.groundHeightRaw) & 0xc000u)
            || (a.groundHeightRaw != b.groundHeightRaw && ((a.groundHeightRaw | b.groundHeightRaw) & 0xc000u)))
            return fail("扩裁前raw覆盖改变了未知顶点字段；不能借最近边缘复制取得保存资格。");
    }
    return Status::Ok();
}

Result<MapDataEditSession::PreparedTerrainResizeEdit> MapDataEditSession::PrepareTerrainResizeEdit(
    const TerrainResizeResources& resources, const TerrainResizeIntent& intent, const TerrainResizeLimits& limits) const try {
    using Output = Result<PreparedTerrainResizeEdit>;
    const auto fail = [](const char* code, const char* message) { return Output::Failure("Terrain.Resize", code, message); };
    const auto verified = resources.VerifyInput(*this, intent, limits.maximumCandidateBytes, limits.maximumWork);
    if (!verified.Succeeded()) return {{}, verified.diagnostics};
    const auto& rawSettings = current_[Index(MapDataKind::Settings)];
    if (resizeDraftState_ || !terrain_ || !terrainOriginal_ || !pathing_ || !pathingOriginal_ || !pathingProtected_ || !pathingRestrictions_
        || !originalPathingRestrictions_ || !rawSettings || !specialDoodads_)
        return fail("TERRAIN_RESIZE_SOURCE_UNAVAILABLE", "扩裁需要完整地形、通行、限制层、设置和特殊摆放来源。");
    if (terrain_->vertices.size() > limits.maximumWork)
        return fail("TERRAIN_RESIZE_BUDGET", "扩裁前未知字段复核超过工作预算。");
    const auto rawInput = VerifyTerrainResizeRawInput();
    if (!rawInput.Succeeded()) return {{}, rawInput.diagnostics};
    auto settings = MapSettings::Parse(*rawSettings);
    if (!settings.Succeeded() || !settings.value) return {{}, settings.diagnostics};
    const auto originalSettingsGate = VerifyTerrainSettingsForSave();
    if (!originalSettingsGate.Succeeded()) return {{}, originalSettingsGate.diagnostics};
    auto projected = ProjectTerrainResizeGeometry(*terrain_, *settings.value, resources.CameraMargins(), resources.DimensionRules(), intent, limits);
    if (!projected.Succeeded() || !projected.value) return {{}, projected.diagnostics};
    const auto oldWidth = (terrain_->header.width - 1) * 4, oldHeight = (terrain_->header.height - 1) * 4;
    const auto newWidth = (projected.value->terrain.header.width - 1) * 4, newHeight = (projected.value->terrain.header.height - 1) * 4;
    const std::size_t oldCells = std::size_t(oldWidth) * oldHeight, newCells = std::size_t(newWidth) * newHeight;
    const auto& historyWpm = TerrainHistoryPathing();
    if (pathing_->size() != oldCells + 16 || historyWpm.size() != oldCells + 16
        || pathingProtected_->size() != oldCells || pathingRestrictions_->size() != oldCells)
        return fail("TERRAIN_RESIZE_PATHING_SHAPE", "当前WPM和分层意图不属于扩裁前尺寸。");
    const auto& shadow = resizeState_ ? *TerrainHistoryShadow() : resources.SourceShadow();
    if (shadow.name != "war3map.shd" || (shadow.present ? shadow.bytes.size() != oldCells : !shadow.bytes.empty()))
        return fail("TERRAIN_RESIZE_SHADOW_SHAPE", "真实SHD来源或当前typed阴影状态与扩裁前尺寸不一致。");
    std::size_t bytes = projected.value->preparationBytes, work = projected.value->preparationWork;
    const auto reserve = [&](std::size_t count, std::size_t element = 1) {
        if (bytes > limits.maximumCandidateBytes || (element && count > (limits.maximumCandidateBytes - bytes) / element)) return false;
        bytes += count * element; return true;
    };
    if (terrain_->vertices.size() > limits.maximumWork - work
        || oldCells > (limits.maximumWork - work - terrain_->vertices.size()) / 4
        || newCells > (limits.maximumWork - work - terrain_->vertices.size() - oldCells * 4) / 5
        || !reserve(resources.CaptureBudgetBytes()) || !reserve(oldCells, 5) || !reserve(newCells, 7)
        || !reserve(rawSettings->size(), 8) || !reserve(specialDoodads_->size(), sizeof(SpecialDoodadPlacement) * 4))
        return fail("TERRAIN_RESIZE_BUDGET", "扩裁的规则、网格、通行分层、阴影和撤销累计超过预算。");
    work += terrain_->vertices.size() + oldCells * 4 + newCells * 5;
    PreparedTerrainResizeEdit prepared;
    prepared.before_ = terrain_; prepared.beforeResize_ = resizeState_;
    prepared.after_ = std::make_shared<const TerrainGridSnapshot>(std::move(projected.value->terrain));
    prepared.sessionIdentity_ = sessionIdentity_; prepared.revision_ = revision_; prepared.sourceRevision_ = terrainSourceRevision_;
    prepared.intent_ = intent; prepared.preparationBytes_ = bytes; prepared.preparationWork_ = work;
    const auto& r = intent.canvas;
    if (r.west == 0 && r.south == 0 && r.east == std::int64_t(terrain_->header.width - 1)
        && r.north == std::int64_t(terrain_->header.height - 1)) { prepared.after_ = terrain_; return Output::Success(std::move(prepared)); }
    auto before = resizeState_ ? resizeState_ : std::make_shared<TerrainResizeState>();
    if (!resizeState_) {
        auto initial = std::make_shared<TerrainResizeState>();
        initial->identity = std::make_shared<const std::uint8_t>(0);
        initial->header = terrain_->header; initial->geometry = terrain_; initial->shadow = shadow;
        initial->historyPathing = std::make_shared<const std::vector<std::byte>>(historyWpm);
        initial->protectedPathing = pathingProtected_; initial->savedRestrictions = originalPathingRestrictions_;
        initial->rulesSha256 = resources.DependencyFingerprint(); before = std::move(initial);
    }
    auto after = std::make_shared<TerrainResizeState>();
    after->identity = std::make_shared<const std::uint8_t>(0);
    after->header = prepared.after_->header; after->geometry = prepared.after_; after->shadow.name = "war3map.shd"; after->shadow.present = shadow.present;
    if (shadow.present) after->shadow.bytes = MoveCells(shadow.bytes, oldWidth, oldHeight, newWidth, newHeight, r, true);
    after->historyPathing = std::make_shared<const std::vector<std::byte>>(MoveWpm(historyWpm, oldWidth, oldHeight, newWidth, newHeight, r));
    after->protectedPathing = std::make_shared<const std::vector<std::byte>>(MoveCells(*pathingProtected_, oldWidth, oldHeight, newWidth, newHeight, r));
    const auto savedRestrictions = TerrainHistorySavedRestrictions();
    if (!savedRestrictions) return fail("TERRAIN_RESIZE_PATHING_SHAPE", "扩裁前savedS历史层缺失。");
    const auto& savedS = *savedRestrictions;
    if (savedS.size() != oldCells) return fail("TERRAIN_RESIZE_PATHING_SHAPE", "扩裁前savedS历史层尺寸不匹配。");
    after->savedRestrictions = std::make_shared<const std::vector<std::byte>>(MoveCells(savedS, oldWidth, oldHeight, newWidth, newHeight, r));
    after->rulesSha256 = resources.DependencyFingerprint();
    Command command;
    command.fullTerrainBefore = terrain_; command.fullTerrainAfter = prepared.after_;
    command.resizeBefore = before; command.resizeAfter = std::move(after);
    command.pathingBefore = *pathing_; command.pathingAfter = MoveWpm(*pathing_, oldWidth, oldHeight, newWidth, newHeight, r);
    command.restrictionBefore = pathingRestrictions_;
    command.restrictionAfter = std::make_shared<const std::vector<std::byte>>(MoveCells(*pathingRestrictions_, oldWidth, oldHeight, newWidth, newHeight, r));
    command.boundsBefore = boundsState_ ? boundsState_ : TerrainBoundsState::Capture(*settings.value, *terrain_);
    command.boundsAfter = TerrainBoundsState::Capture(projected.value->settings, *prepared.after_);
    const auto settingsStatus = Prepare(MapDataKind::Settings, projected.value->settings.Serialize(), command.changes, true);
    if (!settingsStatus.Succeeded()) return {{}, settingsStatus.diagnostics};
    if (!specialDoodads_->empty()) {
        const auto& raw = current_[Index(MapDataKind::Doodads)];
        if (!raw) return fail("TERRAIN_RESIZE_SPECIAL_STATE", "特殊摆放缺少完整原始DOO。");
        auto placed = ParseDoodadPlacements(*raw);
        if (!placed.Succeeded() || !placed.value) return {{}, placed.diagnostics};
        for (auto& special : placed.value->specialDoodads) {
            special.position[0] -= r.west; special.position[1] -= r.south;
        }
        const auto changed = Prepare(MapDataKind::Doodads, placed.value->Serialize(), command.changes);
        if (!changed.Succeeded()) return {{}, changed.diagnostics};
        command.specialBefore = specialDoodads_;
        command.specialAfter = std::make_shared<const std::vector<SpecialDoodadPlacement>>(std::move(placed.value->specialDoodads));
    }
    TerrainJournalStroke stroke; stroke.intent.resize = intent; stroke.rulesSha256 = resources.DependencyFingerprint();
    command.terrainStroke = std::move(stroke);
    prepared.command_ = std::make_shared<const Command>(std::move(command));
    return Output::Success(std::move(prepared));
} catch (const std::bad_alloc&) {
    return Result<PreparedTerrainResizeEdit>::Failure("Terrain.Resize", "TERRAIN_RESIZE_BUDGET", "扩裁内存不足，未修改会话。");
} catch (const std::length_error&) {
    return Result<PreparedTerrainResizeEdit>::Failure("Terrain.Resize", "TERRAIN_RESIZE_BUDGET", "扩裁容量超限，未修改会话。");
}

Status MapDataEditSession::ApplyPreparedTerrainResizeEdit(const PreparedTerrainResizeEdit& prepared,
    const std::function<Status(const EditReceipt&)>& prepublish) {
    if (prepared.sessionIdentity_ != sessionIdentity_ || prepared.revision_ != revision_
        || prepared.sourceRevision_ != terrainSourceRevision_ || prepared.before_ != terrain_ || prepared.beforeResize_ != resizeState_)
        return Status::Failure("Terrain.Resize", "TERRAIN_RESIZE_STALE", "扩裁候选的会话、修订、来源或当前形状已改变，未提交任何内容。");
    return prepared.command_ ? CommitCommand(*prepared.command_, prepublish) : Status::Ok();
}

Status MapDataEditSession::VerifyTerrainResizeForSave() const {
    const auto fail = [](const char* code, const char* message) { return Status::Failure("Terrain.Resize", code, message); };
    if (!resizeState_ || !savedResizeState_ || !terrain_ || !terrainOriginal_ || !pathing_ || !pathingOriginal_
        || !resizeState_->geometry || !SameShape(resizeState_->header, terrain_->header)
        || !SameShape(savedResizeState_->header, terrainOriginal_->header)
        || !SameShape(resizeState_->geometry->header, terrain_->header)
        || resizeState_->geometry->vertices.size() != terrain_->vertices.size())
        return fail("TERRAIN_RESIZE_PROVENANCE_REQUIRED", "尺寸或原点变化没有完整typed扩裁状态；raw草稿不能取得正式保存资格。");
    const auto rawInput = VerifyTerrainResizeRawInput();
    if (!rawInput.Succeeded()) return rawInput;
    const auto cells = std::size_t(terrain_->header.width - 1) * (terrain_->header.height - 1) * 16;
    const auto* shadow = TerrainHistoryShadow();
    if (!shadow || shadow->name != "war3map.shd" || (shadow->present ? shadow->bytes.size() != cells : !shadow->bytes.empty()))
        return fail("TERRAIN_RESIZE_SHADOW_SHAPE", "当前typed阴影的明确存在状态或长度与新形状不符。");
    const auto p = TerrainHistoryProtected();
    const auto& base = TerrainHistoryPathing();
    const auto savedS = TerrainHistorySavedRestrictions();
    if (!p || !savedS || !pathingRestrictions_)
        return fail("TERRAIN_RESIZE_PATHING_SHAPE", "不同尺寸的受保护通行来源或意图层缺失。");
    const auto layers = VerifyPathingRestrictionLayers(terrain_->header, base, *pathing_, *p, *savedS, *pathingRestrictions_);
    if (!layers.Succeeded()) return layers;
    return VerifyTerrainSettingsForSave();
}

std::size_t MapDataEditSession::PreparedTerrainResizeEdit::RetainedCommandBytes() const noexcept {
    if (!command_) return 0;
    std::size_t bytes = sizeof(Command) + 8192;
    const auto add = [&](std::size_t n) { bytes = n > std::numeric_limits<std::size_t>::max() - bytes
        ? std::numeric_limits<std::size_t>::max() : bytes + n; };
    for (const auto& grid : {command_->fullTerrainBefore, command_->fullTerrainAfter}) if (grid) {
        add(grid->vertices.capacity() * sizeof(TerrainVertex)); add(grid->tiles.capacity() * sizeof(TerrainTile));
    }
    for (const auto& state : {command_->resizeBefore, command_->resizeAfter}) if (state) {
        add(sizeof(TerrainResizeState) + state->shadow.bytes.capacity() + state->rulesSha256.capacity());
        for (const auto& layer : {state->historyPathing, state->protectedPathing, state->savedRestrictions}) if (layer) add(layer->capacity());
    }
    for (const auto& pathing : {&command_->pathingBefore, &command_->pathingAfter}) if (*pathing) add((*pathing)->capacity());
    for (const auto& layer : {command_->restrictionBefore, command_->restrictionAfter}) if (layer) add(layer->capacity());
    for (const auto& change : command_->changes) { if (change.before) add(change.before->capacity()); if (change.after) add(change.after->capacity()); }
    return bytes;
}
} // namespace War3Map::MapData
