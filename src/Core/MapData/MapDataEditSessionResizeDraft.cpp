#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/MapData/MapPathing.h"
#include "War3MapCore/Hash.h"

#include <algorithm>
#include <bit>
#include <new>
#include <stdexcept>

namespace War3Map::MapData {
namespace {
constexpr std::size_t Maximum = TerrainJournalMaximumBytes;
struct ResizeDraftWire {
    std::vector<std::byte> bytes;
    void Number(std::uint32_t value) {
        if (Maximum - bytes.size() < 4) throw std::length_error("resize draft");
        for (unsigned i = 0; i < 4; ++i) bytes.push_back(std::byte((value >> (8 * i)) & 255));
    }
    void Data(std::span<const std::byte> value) {
        if (value.size() > Maximum - bytes.size() || Maximum - bytes.size() - value.size() < 4) throw std::length_error("resize draft");
        Number(static_cast<std::uint32_t>(value.size())); bytes.insert(bytes.end(), value.begin(), value.end());
    }
    void Text(std::string_view value) { Data(std::as_bytes(std::span(value.data(), value.size()))); }
};
struct ResizeDraftReader {
    std::span<const std::byte> bytes;
    std::size_t position = 0;
    std::uint32_t Number() {
        if (bytes.size() - position < 4) throw std::invalid_argument("resize draft");
        std::uint32_t value = 0;
        for (unsigned i = 0; i < 4; ++i) value |= std::to_integer<std::uint32_t>(bytes[position++]) << (8 * i);
        return value;
    }
    std::span<const std::byte> Data(std::size_t maximum = Maximum) {
        const auto size = Number();
        if (size > maximum || size > bytes.size() - position) throw std::invalid_argument("resize draft");
        const auto value = bytes.subspan(position, size); position += size; return value;
    }
    std::string Text(std::size_t maximum) {
        const auto data = Data(maximum); return {reinterpret_cast<const char*>(data.data()), data.size()};
    }
};
template<class T> Result<T> DraftFailure(const char* message) {
    return Result<T>::Failure("Terrain.Resize", "TERRAIN_RESIZE_DRAFT_INVALID", message);
}
}

Result<ArchiveExactMember> MapDataEditSession::TerrainResizeShadowDraft() const {
    const auto state = resizeState_ ? resizeState_ : resizeDraftState_;
    if (!state) return DraftFailure<ArchiveExactMember>("当前会话没有扩裁阴影草稿。");
    return Result<ArchiveExactMember>::Success(*TerrainHistoryShadow());
}

Result<std::vector<std::byte>> MapDataEditSession::ExportTerrainResizeDraft() const try {
    using Output = Result<std::vector<std::byte>>;
    const auto state = resizeState_ ? resizeState_ : resizeDraftState_;
    if (!state) return Output::Success({});
    if (!terrainOriginal_ || !terrain_ || !pathingOriginal_ || !pathing_ || !pathingRestrictions_
        || !state->savedRestrictions || !state->protectedPathing || !state->historyPathing)
        return DraftFailure<std::vector<std::byte>>("扩裁草稿缺少完整物理来源或同shape分层。");
    const auto& baseline = TerrainHistoryPathing();
    const auto p = TerrainHistoryProtected();
    const auto savedS = TerrainHistorySavedRestrictions();
    const auto* shadow = TerrainHistoryShadow();
    if (!p || !savedS || !shadow) return DraftFailure<std::vector<std::byte>>("扩裁草稿缺少真实保存前后分层。");
    const auto layers = VerifyPathingRestrictionLayers(terrain_->header, baseline, *pathing_, *p, *savedS, *pathingRestrictions_);
    if (!layers.Succeeded()) return {{}, layers.diagnostics};
    // 两次完整writer及wire增长与四层向量同时存在，逐项预收后才序列化。
    std::size_t bytes = 4096;
    const auto reserve = [&](std::size_t count, std::size_t unit) {
        if (count > (Maximum - bytes) / unit) return false; bytes += count * unit; return true;
    };
    if (!reserve(terrainOriginal_->vertices.size() + terrain_->vertices.size(), 16)
        || !reserve(baseline.size(), 3) || !reserve(p->size() + savedS->size() + pathingRestrictions_->size(), 3)
        || !reserve(shadow->bytes.size(), 3))
        return DraftFailure<std::vector<std::byte>>("扩裁草稿完整wire峰值超过64MiB。");
    auto old = SerializeMapTerrain(*terrainOriginal_), current = SerializeMapTerrain(*terrain_);
    if (!old.Succeeded() || !old.value) return {{}, old.diagnostics};
    if (!current.Succeeded() || !current.value) return {{}, current.diagnostics};
    ResizeDraftWire wire; wire.Number(0x31445254u);
    for (const auto value : {std::span<const std::byte>(*old.value), std::span<const std::byte>(*pathingOriginal_),
        std::span<const std::byte>(*current.value), std::span<const std::byte>(*pathing_)}) {
        auto sha = Sha256(value); if (!sha.Succeeded() || !sha.value) return {{}, sha.diagnostics}; wire.Text(*sha.value);
    }
    wire.Data(baseline); wire.Data(*p); wire.Data(*savedS); wire.Data(*pathingRestrictions_);
    wire.Text("war3map.shd"); wire.Number(shadow->present ? 1 : 0); wire.Data(shadow->bytes);
    return Output::Success(std::move(wire.bytes));
} catch (const std::bad_alloc&) { return DraftFailure<std::vector<std::byte>>("扩裁草稿内存不足。"); }
  catch (const std::length_error&) { return DraftFailure<std::vector<std::byte>>("扩裁草稿容量超过预算。"); }

Status MapDataEditSession::SetTerrainResizeDraft(std::span<const std::byte> terrain,
    std::span<const std::byte> pathing, std::span<const std::byte> encoded) try {
    const auto fail = [](const char* message) { return Status{DraftFailure<std::vector<std::byte>>(message).diagnostics}; };
    if (encoded.empty() || encoded.size() > Maximum || !terrainOriginal_ || !terrain_ || !pathingOriginal_ || !pathing_
        || resizeState_ || resizeDraftState_ || TerrainDirty() || pathing_ != pathingOriginal_
        || terrain.size() > Maximum / 4 || pathing.size() > Maximum / 4
        || encoded.size() > Maximum - terrain.size() * 2 - pathing.size() * 2)
        return fail("扩裁raw inspection只能在原始地形来源上安装，且完整输入须在64MiB内。");
    // 原始writer、当前网格解码、路径复制和四层wire解包的峰值共同计量。
    std::size_t retained = 8192;
    const auto reserve = [&](std::size_t count, std::size_t unit = 1) {
        if (count > (Maximum - retained) / unit) return false;
        retained += count * unit; return true;
    };
    if (!reserve(encoded.size(), 2) || !reserve(pathing.size(), 3)
        || !reserve(terrainOriginal_->vertices.size(), 14)
        || !reserve(terrainOriginal_->vertices.capacity(), sizeof(TerrainVertex))
        || !reserve(terrainOriginal_->tiles.capacity(), sizeof(TerrainTile)))
        return fail("扩裁raw来源、wire与路径复制的累计驻留超过64MiB。");
    ResizeDraftReader reader{encoded};
    if (reader.Number() != 0x31445254u) return fail("扩裁草稿版本不是TRD1。");
    auto old = SerializeMapTerrain(*terrainOriginal_);
    if (!old.Succeeded() || !old.value) return {old.diagnostics};
    for (const auto bytes : {std::span<const std::byte>(*old.value), std::span<const std::byte>(*pathingOriginal_), terrain, pathing}) {
        const auto expected = reader.Text(64); auto sha = Sha256(bytes);
        if (!sha.Succeeded() || !sha.value) return {sha.diagnostics};
        if (expected != *sha.value) return fail("扩裁草稿的物理来源或当前W3E/WPM全文身份不一致。");
    }
    TerrainReadLimits terrainLimits;
    terrainLimits.maxDecodedBytes = Maximum - retained;
    auto grid = ParseMapTerrain(terrain, terrainLimits);
    if (!grid.Succeeded() || !grid.value || grid.value->Snapshot()->header.version != 11)
        return fail("扩裁raw地形不是完整经典W3E。");
    auto checked = ParseMapPathing(pathing, &grid.value->Snapshot()->header);
    if (!checked.Succeeded()) return {checked.diagnostics};
    const auto baseline = reader.Data(), p = reader.Data(), savedS = reader.Data(), currentS = reader.Data();
    auto state = std::make_shared<TerrainResizeState>();
    state->identity = std::make_shared<const std::uint8_t>(0);
    state->header = grid.value->Snapshot()->header; state->geometry = grid.value->Snapshot();
    state->shadow.name = reader.Text(64);
    const auto present = reader.Number(); const auto shadow = reader.Data();
    const auto cells = pathing.size() - 16;
    if (reader.position != encoded.size() || state->shadow.name != "war3map.shd" || present > 1
        || (present ? shadow.size() != cells : !shadow.empty())) return fail("TRD1阴影存在状态或shape无效。");
    const auto verified = VerifyPathingRestrictionLayers(state->header, baseline, pathing, p, savedS, currentS);
    if (!verified.Succeeded()) return verified;
    state->shadow.present = present != 0; state->shadow.bytes.assign(shadow.begin(), shadow.end());
    state->historyPathing = std::make_shared<const std::vector<std::byte>>(baseline.begin(), baseline.end());
    state->protectedPathing = std::make_shared<const std::vector<std::byte>>(p.begin(), p.end());
    state->savedRestrictions = std::make_shared<const std::vector<std::byte>>(savedS.begin(), savedS.end());
    auto before = std::make_shared<TerrainResizeState>();
    before->identity = std::make_shared<const std::uint8_t>(0);
    before->rawBaselineOnly = true;
    before->header = terrain_->header; before->geometry = terrain_;
    before->historyPathing = std::make_shared<const std::vector<std::byte>>(*pathingOriginal_);
    before->protectedPathing = pathingProtected_; before->savedRestrictions = originalPathingRestrictions_;
    Command command; command.fullTerrainBefore = terrain_; command.fullTerrainAfter = grid.value->Snapshot();
    command.resizeDraftBefore = std::move(before); command.resizeDraftAfter = std::move(state);
    command.pathingBefore = *pathing_; command.pathingAfter = std::vector<std::byte>(pathing.begin(), pathing.end());
    command.restrictionBefore = pathingRestrictions_;
    command.restrictionAfter = std::make_shared<const std::vector<std::byte>>(currentS.begin(), currentS.end());
    return CommitCommand(std::move(command));
} catch (const std::bad_alloc&) { return {DraftFailure<std::vector<std::byte>>("扩裁草稿内存不足，未安装raw候选。").diagnostics}; }
  catch (const std::length_error&) { return {DraftFailure<std::vector<std::byte>>("扩裁草稿容量超限，未安装raw候选。").diagnostics}; }
  catch (const std::invalid_argument&) { return {DraftFailure<std::vector<std::byte>>("TRD1字段截断或长度错误，未安装raw候选。").diagnostics}; }
} // namespace War3Map::MapData
