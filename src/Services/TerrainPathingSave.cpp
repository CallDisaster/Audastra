#include "War3MapCore/Services/TerrainPathingSave.h"

#include "War3MapCore/ObjectData/NativeDestructablePathing.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/Services/TerrainEditService.h"
#include "War3MapCore/Services/TerrainBoundsEditService.h"
#include "War3MapCore/Hash.h"

#include <algorithm>
#include <cctype>
#include <new>

namespace War3Map::Services {
namespace {
template<class T, class Function> Result<T> Guard(Function&& function) {
    try { return function(); }
    catch (const std::bad_alloc&) {
        return Result<T>::Failure("Terrain.PathingSave", "TERRAIN_NATIVE_OBJECT_BUDGET", "严格对象保存适配分配失败，未建立部分保存计划。");
    }
}

bool Neutral(const MapData::TerrainBrush& brush) {
    const MapData::TerrainBrush other;
    return brush.kind == other.kind && brush.shape == other.shape && brush.radius == other.radius
        && brush.groundTexture == other.groundTexture && brush.cliffTexture == other.cliffTexture
        && brush.levelDelta == other.levelDelta && brush.value == other.value && brush.strength == other.strength
        && brush.seed == other.seed && !brush.anchor && !brush.size && !brush.variation
        && brush.randomizeVariation == other.randomizeVariation && brush.waterHeight == other.waterHeight
        && brush.pathingRestrictions == other.pathingRestrictions;
}

bool PaletteCommand(const MapData::TerrainStrokeIntent& stroke) {
    if (!stroke.palette || stroke.bounds || !Neutral(stroke.brush) || !stroke.points.empty()
        || stroke.waterBrush || stroke.followDoodads) return false;
    const auto& palette = *stroke.palette;
    const auto rawcode = [](std::string_view value) { return value.size() == 4
        && std::all_of(value.begin(), value.end(), [](unsigned char c) { return c >= 32 && c <= 126; }); };
    return (palette.table == MapData::TerrainTextureTable::Ground
        || palette.table == MapData::TerrainTextureTable::Cliff)
        && palette.slot < (palette.table == MapData::TerrainTextureTable::Ground ? 16u : 15u)
        && rawcode(palette.replacementRawcode)
        && ((palette.algorithm == 1 && rawcode(palette.expectedRawcode)
                && palette.expectedRawcode != palette.replacementRawcode)
            || (palette.algorithm == 2 && palette.table == MapData::TerrainTextureTable::Ground
                && palette.slot > 0 && palette.expectedRawcode.empty()));
}

Status ValidateJournalReplayAlignment(std::span<const MapData::TerrainStrokeIntent> strokes,
    const MapData::TerrainStrokeJournal* journal) {
    if (!journal) return Status::Ok();
    const auto valid = MapData::ValidateTerrainStrokeJournal(*journal);
    if (!valid.Succeeded()) return valid;
    if (journal->strokes.size() != strokes.size())
        return Status::Failure("Terrain.Journal", "TERRAIN_JOURNAL_INTENT_ALIGNMENT", "工程记录和私有重放意图数量不一致。");
    for (std::size_t i = 0; i < strokes.size(); ++i) {
        const auto& record = journal->strokes[i];
        // 即使工厂由同一份记录复制，仍逐项双向核对所有字段与准确variant。
        // 公共ReplayStrokes没有journal，不能靠空点/默认brush进入普通DOO分支。
        const bool placeholder = MapData::IsTerrainDoodadsJournalPlaceholder(strokes[i]);
        if (!MapData::EqualTerrainJournalIntent(strokes[i], record.intent)
            || placeholder != record.ordinaryDoodads.has_value() || record.frozenDoodads
            || (record.ordinaryDoodads && (!record.rulesSha256.empty() || record.cliffEnforceWaterLimits)))
            return Status::Failure("Terrain.Journal", "TERRAIN_JOURNAL_INTENT_ALIGNMENT", "普通DOO记录必须对应唯一中性占位，typed笔划不能借占位或顺序取得恢复资格。");
    }
    return Status::Ok();
}

template<class Source, class Reader>
Result<MapData::MapDataEditSession::PreparedTerrainEdit> PrepareTextureSlot(const Source& source, const MapData::MapDataEditSession& edits,
    MapData::TerrainTextureTable table, std::uint32_t slot, const std::string& rawcode,
    const std::filesystem::path& gameRoot, std::size_t maximumBytes, Reader&& reader) {
    using Output = Result<MapData::MapDataEditSession::PreparedTerrainEdit>;
    const auto fail = [](const char* code, const char* message) { return Output::Failure("Terrain.Palette", code, message); };
    if (!maximumBytes || maximumBytes > 64u * 1024u * 1024u)
        return fail("TERRAIN_PALETTE_BUDGET", "贴图记录资源预算只能在64MiB内收紧且不能为零。");
    const auto current = edits.Terrain(); const auto revision = edits.Revision();
    if (!current) return fail("MAP_TERRAIN_UNAVAILABLE", "地形不可编辑。");
    if (table != MapData::TerrainTextureTable::Ground && table != MapData::TerrainTextureTable::Cliff)
        return fail("TERRAIN_TILESET_TABLE", "未知的地形贴图表。");
    const auto& palette = table == MapData::TerrainTextureTable::Ground ? current->header.groundTilesets : current->header.cliffTilesets;
    const bool appendGround = table == MapData::TerrainTextureTable::Ground && slot == palette.size() && slot < 16;
    if ((!appendGround && slot >= palette.size()) || current->header.groundTilesets.empty() || current->header.groundTilesets.size() > 16
        || current->header.cliffTilesets.empty() || current->header.cliffTilesets.size() > 15)
        return fail("TERRAIN_PALETTE_JOURNAL_DOMAIN", "可信贴图记录只支持经典表的既有槽替换或Ground末尾追加，不能删除或重排。");
    // 原公共命令需要完整网格候选；先给两份候选与当前DOO解析预收固定硬帽。
    constexpr std::size_t candidateMaximum = 512u * 1024u * 1024u;
    constexpr std::size_t candidateFixedBytes = 16u * 1024u; // 两套经典表头、短rawcode与命令对象。
    if (current->vertices.size() > std::min((candidateMaximum - candidateFixedBytes) / 256, std::size_t{64} * 1024u * 1024u / 40))
        return fail("TERRAIN_PALETTE_BUDGET", "贴图替换完整候选超过512MiB预算。");
    // 同值仍经原Core验证；该入口当前会复制网格，必须先受候选硬帽约束。
    if (!appendGround && palette[slot] == rawcode) return edits.PrepareTerrainTextureSlot(table, slot, rawcode);
    const auto dooBytes = edits.SnapshotByteSize(MapDataKind::Doodads);
    if (!dooBytes.Succeeded() || !dooBytes.value) return {{}, dooBytes.diagnostics};
    if (*dooBytes.value > maximumBytes || *dooBytes.value > (candidateMaximum - candidateFixedBytes - current->vertices.size() * 256) / 32)
        return fail("TERRAIN_PALETTE_BUDGET", "贴图替换当前DOO解析超过候选预算。");
    auto prospective = *current;
    const auto replaced = MapData::ReplaceTerrainTextureSlot(prospective, table, slot, rawcode);
    if (!replaced.Succeeded() || !replaced.value) return {{}, replaced.diagnostics};
    const std::array<std::string, 1> names{"war3map.w3e"};
    ArchiveExactMemberLimits limits; limits.memberCount = 1; limits.memberBytes = limits.totalBytes = maximumBytes;
    auto sourceMember = reader(names, limits);
    if (!sourceMember.Succeeded() || !sourceMember.value) return {{}, sourceMember.diagnostics};
    if (sourceMember.value->size() != 1 || !sourceMember.value->front().present)
        return fail("TERRAIN_PALETTE_SOURCE_MISSING", "实际地图缺少唯一原始W3E。");
    const auto originalMatches = edits.VerifyTerrainSourceBaseline(sourceMember.value->front().bytes);
    if (!originalMatches.Succeeded()) return {{}, originalMatches.diagnostics};
    auto original = MapData::ParseMapTerrain(sourceMember.value->front().bytes);
    if (!original.Succeeded() || !original.value) return {{}, original.diagnostics};
    auto doodads = edits.Snapshot(MapDataKind::Doodads, maximumBytes);
    if (!doodads.Succeeded() || !doodads.value) return {{}, doodads.diagnostics};
    if (!doodads.value->Doodads() && doodads.value->Member(MapDataKind::Doodads).state != MapDataState::Missing)
        return fail("TERRAIN_PALETTE_DOODADS_UNAVAILABLE", "贴图表记录需要完整DOO或明确的缺失身份。");
    const MapData::DoodadPlacements empty;
    auto resources = TerrainPathingSave::CaptureWaterBrushRules(source, *original.value->Snapshot(), prospective,
        doodads.value->Doodads() ? *doodads.value->Doodads() : empty, gameRoot, true, maximumBytes);
    if (!resources.Succeeded() || !resources.value) return {{}, resources.diagnostics};
    const auto checked = resources.value->VerifySourceInputs(source, maximumBytes);
    if (!checked.Succeeded()) return {{}, checked.diagnostics};
    if (edits.Terrain() != current || edits.Revision() != revision)
        return fail("TERRAIN_EDIT_STALE", "实际来源捕获期间地形修订已变化，贴图未修改。");
    auto prepared = edits.PrepareTerrainTextureSlot(table, slot, rawcode, &*resources.value);
    if (!prepared.Succeeded() || !prepared.value) return prepared;
    const auto finalSource = resources.value->VerifySourceInputs(source, maximumBytes);
    if (!finalSource.Succeeded()) return {{}, finalSource.diagnostics};
    return prepared;
}

template<class Source>
Result<std::vector<MapData::TerrainPathingResources>> Replay(const Source& source,
    MapData::MapDataEditSession& candidate, const MapData::TerrainGridSnapshot& sourceTerrain,
    std::size_t pathingBytes,
    std::span<const MapData::TerrainStrokeIntent> strokes, const std::filesystem::path& gameRoot,
    const TerrainReplayLimits& limits, const std::string& sourceSha,
    const MapData::TerrainStrokeJournal* journal = nullptr,
    std::vector<MapData::MapDataEditSession::EditReceipt>* receipts = nullptr) {
    using Output = Result<std::vector<MapData::TerrainPathingResources>>;
    // 工厂已将实际W3E逐字核对并Attach，并持有落笔前owner直到整个序列完成。
    const auto fail = [](const char* code, const char* message) { return Output::Failure("Terrain.Replay", code, message); };
    const TerrainReplayLimits hard;
    if (!limits.maximumResourceBytes || limits.maximumResourceBytes > hard.maximumResourceBytes
        || !limits.maximumWork || limits.maximumWork > hard.maximumWork
        || !limits.maximumCandidateBytes || limits.maximumCandidateBytes > hard.maximumCandidateBytes)
        return fail("TERRAIN_REPLAY_LIMITS", "重放预算只能收紧且不能为零。");
    if (strokes.empty() || strokes.size() > 128)
        return fail("TERRAIN_PATCH_STROKE_BUDGET", "地形重放需要1到128笔原始命令。");
    const auto aligned = ValidateJournalReplayAlignment(strokes, journal);
    if (!aligned.Succeeded()) return {{}, aligned.diagnostics};
    std::size_t pointCount = 0;
    std::size_t inputIndex = 0;
    for (const auto& stroke : strokes) {
        const bool ordinary = journal && journal->strokes[inputIndex].ordinaryDoodads.has_value();
        ++inputIndex;
        if (ordinary) continue;
        if (stroke.palette) {
            if (!PaletteCommand(stroke)) return fail("TERRAIN_REPLAY_PALETTE_CONFLICT", "贴图表记录仅接受既有槽替换或Ground表尾追加，不能携带笔刷、范围或采样点。");
            continue;
        }
        if (stroke.bounds) {
            const auto& r = stroke.bounds->playable;
            if (!Neutral(stroke.brush) || !stroke.points.empty() || stroke.followDoodads || stroke.waterBrush
                || stroke.bounds->algorithm != 1 || r.west >= r.east || r.south >= r.north || r.east > 2048 || r.north > 2048)
                return fail("TERRAIN_REPLAY_BOUNDS_CONFLICT", "范围命令需要独立的半开地格矩形，不能携带笔刷或采样点。");
            continue;
        }
        if (stroke.points.empty() || stroke.points.size() > 4096 - pointCount)
            return fail("TERRAIN_PATCH_POINT_BUDGET", "地形重放累计采样点超过4096或包含空笔划。");
        pointCount += stroke.points.size();
        if (stroke.waterBrush && !Neutral(stroke.brush))
            return fail("TERRAIN_REPLAY_BRUSH_CONFLICT", "浅/深水笔划不能同时携带普通笔刷参数。");
    }
    std::vector<MapData::TerrainPathingResources> owners;
    std::size_t resourceBytes = 0, ownedResources = 0, work = 0, historyBytes = 0;
    const auto reserve = [](std::size_t& used, std::size_t limit, std::size_t count, std::size_t element = 1) {
        if (element && count > (limit - used) / element) return false;
        used += count * element; return true;
    };
    const MapDataQueryBinding document{"terrain-replay", 1, 0, sourceSha};
    std::size_t strokeIndex = 0;
    for (const auto& stroke : strokes) {
        const auto beforeRevision = candidate.Revision();
        if (!candidate.Terrain()) return fail("TERRAIN_REPLAY_UNAVAILABLE", "重放候选缺少当前地形。");
        const TerrainEditBinding binding{document, candidate.SessionIdentity(), candidate.Revision(), candidate.TerrainSourceRevision()};
        if (journal && journal->strokes[strokeIndex].ordinaryDoodads) {
            const auto& edit = *journal->strokes[strokeIndex].ordinaryDoodads;
            const auto available = limits.maximumCandidateBytes - historyBytes;
            const auto cost = MapData::MeasureTerrainDoodadsJournalEdit(edit.beforeBytes, edit.afterBytes,
                available, limits.maximumWork - work);
            if (!cost.Succeeded() || !cost.value
                || cost.value->workingBytes > available
                || !reserve(work, limits.maximumWork, cost.value->work)
                || !reserve(historyBytes, limits.maximumCandidateBytes, cost.value->retainedBytes))
                return fail("TERRAIN_REPLAY_BUDGET", "普通DOO完整codec、命令历史或累计散列预算不足。");
            const auto applied = candidate.ReplayTerrainJournalDoodads(edit, cost.value->workingBytes, cost.value->work);
            if (!applied.Succeeded()) return {{}, applied.diagnostics};
            if (candidate.Revision() == beforeRevision)
                return fail("TERRAIN_JOURNAL_NO_CHANGE", "普通DOO记录没有形成真实命令，未安装任何前缀。");
            if (receipts) receipts->push_back(candidate.LastEdit());
            ++strokeIndex; continue;
        } else if (stroke.palette) {
            const auto& intent = *stroke.palette;
            const auto& table = intent.table == MapData::TerrainTextureTable::Ground
                ? candidate.Terrain()->header.groundTilesets : candidate.Terrain()->header.cliffTilesets;
            if (intent.algorithm == 2 ? intent.slot != table.size()
                : intent.slot >= table.size() || table[intent.slot] != intent.expectedRawcode)
                return fail("TERRAIN_JOURNAL_PALETTE_DRIFT", "贴图槽已不存在或旧rawcode不同，未安装任何前缀。");
            // prospective与Core命令各自持有一次完整候选；全部扫描、解析和摘要在分配前预收。
            const auto vertices = candidate.Terrain()->vertices.size();
            std::size_t peakBytes = 0;
            const auto wire = candidate.SnapshotByteSize(MapDataKind::Doodads);
            if (!wire.Succeeded() || !wire.value) return {{}, wire.diagnostics};
            if (!reserve(peakBytes, limits.maximumCandidateBytes - historyBytes, 16u * 1024u)
                || !reserve(work, limits.maximumWork, vertices, 40)
                || !reserve(work, limits.maximumWork, *wire.value, 16)
                || !reserve(peakBytes, limits.maximumCandidateBytes - historyBytes, vertices, 256)
                || !reserve(peakBytes, limits.maximumCandidateBytes - historyBytes, *wire.value, 32))
                return fail("TERRAIN_REPLAY_BUDGET", "贴图表候选、当前DOO或累计扫描超过重放预算。");
            const auto remaining = std::min(MapData::TerrainSavePlanLimits{}.maximumBytes,
                std::min(limits.maximumResourceBytes - resourceBytes, limits.maximumResourceBytes - ownedResources));
            if (!remaining || *wire.value > remaining)
                return fail("TERRAIN_REPLAY_BUDGET", "贴图表实际资源捕获额度不足。");
            auto prospective = *candidate.Terrain();
            const auto replaced = MapData::ReplaceTerrainTextureSlot(prospective, intent.table, intent.slot, intent.replacementRawcode);
            if (!replaced.Succeeded() || !replaced.value) return {{}, replaced.diagnostics};
            auto resources = [&]() -> Result<MapData::TerrainPathingResources> {
                auto current = candidate.Snapshot(MapDataKind::Doodads, remaining);
                if (!current.Succeeded() || !current.value) return {{}, current.diagnostics};
                const auto& doodads = current.value->Doodads();
                if (!doodads && current.value->Member(MapDataKind::Doodads).state != MapDataState::Missing)
                    return Result<MapData::TerrainPathingResources>::Failure("Terrain.Replay", "TERRAIN_REPLAY_DOODADS_UNAVAILABLE", "贴图表记录不能把未知DOO当成空集合。");
                const MapData::DoodadPlacements empty;
                return TerrainPathingSave::CaptureWaterBrushRules(source, sourceTerrain, prospective,
                    doodads ? *doodads : empty, gameRoot, true, remaining);
            }();
            if (!resources.Succeeded() || !resources.value) return {{}, resources.diagnostics};
            const auto digestBytes = resources.value->WaterBrushDependencyBudgetBytes();
            if (!digestBytes || digestBytes > limits.maximumCandidateBytes - historyBytes - peakBytes
                || !reserve(work, limits.maximumWork, digestBytes, 6)
                || !reserve(resourceBytes, limits.maximumResourceBytes, resources.value->CaptureBudgetBytes())
                || !reserve(ownedResources, limits.maximumResourceBytes, resources.value->OwnedCapacityBytes()))
                return fail("TERRAIN_REPLAY_BUDGET", "贴图表完整依赖摘要或实际owner超过累计重放预算。");
            const auto digest = resources.value->WaterBrushDependencyFingerprint();
            if (!digest.Succeeded() || !digest.value) return {{}, digest.diagnostics};
            if (journal && *digest.value != journal->strokes[strokeIndex].rulesSha256)
                return fail("TERRAIN_JOURNAL_RESOURCE_DRIFT", "贴图表实际来源、规则winner或缺失项已变化，未安装任何前缀。");
            auto plan = candidate.PrepareTerrainTextureSlot(intent.table, intent.slot, intent.replacementRawcode, &*resources.value);
            if (!plan.Succeeded() || !plan.value) return {{}, plan.diagnostics};
            // 头部历史按真实保留容量收费，不将整张prospective复制进Undo，也不触碰special私有谱系。
            if (!reserve(historyBytes, limits.maximumCandidateBytes, plan.value->RetainedCommandBytes()))
                return fail("TERRAIN_REPLAY_BUDGET", "贴图表历史超过累计重放预算。");
            const auto applied = candidate.ApplyPreparedTerrainEdit(*plan.value);
            if (!applied.Succeeded()) return {{}, applied.diagnostics};
            owners.push_back(std::move(*resources.value));
            if (candidate.Revision() != beforeRevision) {
                if (receipts) receipts->push_back(candidate.LastEdit());
            } else if (journal) return fail("TERRAIN_JOURNAL_NO_CHANGE", "贴图表记录没有形成真实命令，未安装任何前缀。");
            ++strokeIndex; continue;
        }
        if (stroke.bounds) {
            const auto remaining = std::min(MapData::TerrainSavePlanLimits{}.maximumBytes,
                std::min(limits.maximumResourceBytes - resourceBytes, limits.maximumResourceBytes - ownedResources));
            if (!remaining || historyBytes == limits.maximumCandidateBytes || work == limits.maximumWork)
                return fail("TERRAIN_REPLAY_BUDGET", "范围重放的累计资源、工作或候选预算已用尽。");
            const auto wire = candidate.SnapshotByteSize(MapDataKind::Doodads);
            if (!wire.Succeeded() || !wire.value) return {{}, wire.diagnostics};
            if (!reserve(work, limits.maximumWork, *wire.value, 2)
                || *wire.value > remaining || *wire.value > (limits.maximumCandidateBytes - historyBytes) / 32)
                return fail("TERRAIN_REPLAY_BUDGET", "范围命令当前DOO快照超过整个重放预算。");
            auto resources = [&]() -> Result<MapData::TerrainPathingResources> {
                auto current = candidate.Snapshot(MapDataKind::Doodads, remaining);
                if (!current.Succeeded() || !current.value) return {{}, current.diagnostics};
                const auto& member = current.value->Member(MapDataKind::Doodads);
                if (member.state == MapDataState::Missing)
                    return TerrainPathingSave::CaptureNative(source, sourceTerrain, *candidate.Terrain(), gameRoot, remaining);
                if (member.state != MapDataState::Ready || member.sources.size() != 1)
                    return Result<MapData::TerrainPathingResources>::Failure("Terrain.Replay", "TERRAIN_REPLAY_DOODADS_UNAVAILABLE",
                        "范围命令需要唯一完整DOO来源，不能把未知来源当作空表。");
                return TerrainPathingSave::CaptureNative(source, sourceTerrain, *candidate.Terrain(), gameRoot, remaining,
                    &member.sources.front().data);
            }();
            if (!resources.Succeeded() || !resources.value) return {{}, resources.diagnostics};
            if (journal) {
                const auto digestBytes = resources.value->TerrainBoundsDependencyBudgetBytes();
                if (!digestBytes || digestBytes > limits.maximumCandidateBytes - historyBytes
                    || !reserve(work, limits.maximumWork, digestBytes, 3))
                    return fail("TERRAIN_REPLAY_BUDGET", "范围依赖摘要超过剩余候选或累计工作预算。");
                const auto digest = resources.value->TerrainBoundsDependencyFingerprint(digestBytes);
                if (!digest.Succeeded() || !digest.value || *digest.value != journal->strokes[strokeIndex].rulesSha256)
                    return fail("TERRAIN_JOURNAL_RESOURCE_DRIFT", "范围实际来源、规则或缺失项已变化，未安装任何前缀。");
            }
            if (!reserve(resourceBytes, limits.maximumResourceBytes, resources.value->CaptureBudgetBytes())
                || !reserve(ownedResources, limits.maximumResourceBytes, resources.value->OwnedCapacityBytes()))
                return fail("TERRAIN_REPLAY_BUDGET", "范围实际规则的累计捕获或保留量超过重放预算。");
            auto plan = PrepareTerrainBoundsEdit(candidate, document, binding, *resources.value, *stroke.bounds,
                limits.maximumCandidateBytes - historyBytes, limits.maximumWork - work);
            if (!plan.Succeeded() || !plan.value) return {{}, plan.diagnostics};
            if (!reserve(work, limits.maximumWork, plan.value->Plan().PreparationWork())
                || !reserve(work, limits.maximumWork, plan.value->Plan().RetainedCommandBytes(), 2)
                || plan.value->Plan().PreparationBytes() > limits.maximumCandidateBytes - historyBytes
                || !reserve(historyBytes, limits.maximumCandidateBytes, plan.value->Plan().RetainedCommandBytes()))
                return fail("TERRAIN_REPLAY_BUDGET", "范围事务累计历史或工作超出重放预算。");
            const auto applied = ApplyTerrainBoundsEdit(candidate, document, *plan.value);
            if (!applied.Succeeded()) return {{}, applied.diagnostics};
            owners.push_back(std::move(*resources.value));
            if (candidate.Revision() != beforeRevision) {
                if (receipts) receipts->push_back(candidate.LastEdit());
            } else if (journal) return fail("TERRAIN_JOURNAL_NO_CHANGE", "范围记录没有形成真实命令，未安装任何前缀。");
            ++strokeIndex; continue;
        }
        const bool nativeCliff = !stroke.waterBrush && stroke.brush.kind == MapData::TerrainBrushKind::CliffLevel
            && (!journal || journal->strokes[strokeIndex].cliffEnforceWaterLimits.has_value());
        const bool cornerRamp = !stroke.waterBrush && stroke.brush.kind == MapData::TerrainBrushKind::CornerRamp;
        if (stroke.waterBrush || nativeCliff || cornerRamp) {
            const bool enforceWaterLimits = stroke.waterBrush ? stroke.waterBrush->enforceWaterLimits
                : cornerRamp ? true : journal ? *journal->strokes[strokeIndex].cliffEnforceWaterLimits : true;
            const auto remaining = std::min(MapData::TerrainSavePlanLimits{}.maximumBytes,
                std::min(limits.maximumResourceBytes - resourceBytes, limits.maximumResourceBytes - ownedResources));
            if (!remaining || historyBytes == limits.maximumCandidateBytes || work == limits.maximumWork)
                return fail("TERRAIN_REPLAY_BUDGET", "整个序列的资源、工作或候选预算已用尽。");
            const auto wire = candidate.SnapshotByteSize(MapDataKind::Doodads);
            if (!wire.Succeeded() || !wire.value) return {{}, wire.diagnostics};
            // Snapshot解析本身在捕获前预收，不能先展开大DOO再检查累计预算。
            if (!reserve(work, limits.maximumWork, *wire.value, 2)
                || *wire.value > remaining || *wire.value > (limits.maximumCandidateBytes - historyBytes) / 32)
                return fail("TERRAIN_REPLAY_BUDGET", "当前DOO快照超过整个重放序列的预算。");
            auto resources = [&]() -> Result<MapData::TerrainPathingResources> {
                auto current = candidate.Snapshot(MapDataKind::Doodads, remaining);
                if (!current.Succeeded() || !current.value) return {{}, current.diagnostics};
                const auto& doodads = current.value->Doodads();
                if (!doodads && current.value->Member(MapDataKind::Doodads).state != MapDataState::Missing)
                    return Result<MapData::TerrainPathingResources>::Failure("Terrain.Replay", "TERRAIN_REPLAY_DOODADS_UNAVAILABLE", "未知DOO不能按空集合捕获水刷资源。");
                const MapData::DoodadPlacements empty;
                return TerrainPathingSave::CaptureWaterBrushRules(source, sourceTerrain, *candidate.Terrain(),
                    doodads ? *doodads : empty, gameRoot, enforceWaterLimits, remaining);
            }(); // 外层DOO解析快照先释放，再进入水刷事务自己的已预算解析/候选阶段。
            if (!resources.Succeeded() || !resources.value) return {{}, resources.diagnostics};
            if (journal) {
                const auto digestBytes = resources.value->WaterBrushDependencyBudgetBytes();
                if (!digestBytes || digestBytes > limits.maximumCandidateBytes - historyBytes
                    || !reserve(work, limits.maximumWork, digestBytes, 3))
                    return fail("TERRAIN_REPLAY_BUDGET", "水刷依赖摘要的临时wire或累计编码/散列工作超出预算。");
                const auto digest = resources.value->WaterBrushDependencyFingerprint();
                if (!digest.Succeeded() || !digest.value || *digest.value != journal->strokes[strokeIndex].rulesSha256)
                    return fail("TERRAIN_JOURNAL_RESOURCE_DRIFT", "水刷实际依赖、winner或缺失项已变化，未安装记录中的任何前缀。");
            }
            if (!reserve(resourceBytes, limits.maximumResourceBytes, resources.value->CaptureBudgetBytes())
                || !reserve(ownedResources, limits.maximumResourceBytes, resources.value->OwnedCapacityBytes()))
                return fail("TERRAIN_REPLAY_BUDGET", "逐笔实际资源的累计捕获或保留量超过重放预算。");
            MapData::TerrainWaterBrushLimits waterLimits;
            waterLimits.maximumCandidateBytes = std::min(waterLimits.maximumCandidateBytes, limits.maximumCandidateBytes - historyBytes);
            waterLimits.maximumWork = std::min(waterLimits.maximumWork, limits.maximumWork - work);
            if (!waterLimits.maximumWork) return fail("TERRAIN_REPLAY_BUDGET", "整个序列的工作预算已用尽。");
            auto plan = stroke.waterBrush
                ? PrepareTerrainWaterEdit(candidate, document, binding, *resources.value,
                    *stroke.waterBrush, stroke.points, stroke.followDoodads, waterLimits)
                : cornerRamp ? PrepareTerrainCornerRampEdit(candidate, document, binding, *resources.value,
                    stroke.brush, stroke.points, stroke.followDoodads, waterLimits)
                : PrepareTerrainCliffEdit(candidate, document, binding, *resources.value,
                    stroke.brush, stroke.points, stroke.followDoodads, waterLimits);
            if (!plan.Succeeded() || !plan.value) return {{}, plan.diagnostics};
            if (!reserve(work, limits.maximumWork, plan.value->Plan().WaterPreparationWork())
                || !reserve(work, limits.maximumWork, candidate.Terrain()->vertices.size(), 4)
                || !reserve(work, limits.maximumWork, plan.value->Plan().RetainedCommandBytes(), 2)
                || plan.value->Plan().WaterPreparationBytes() > limits.maximumCandidateBytes - historyBytes
                || !reserve(historyBytes, limits.maximumCandidateBytes, plan.value->Plan().RetainedCommandBytes()))
                return fail("TERRAIN_REPLAY_BUDGET", "水刷事务累计工作或候选预算超限。");
            const auto applied = ApplyTerrainEdit(candidate, document, *plan.value);
            if (!applied.Succeeded()) return {{}, applied.diagnostics};
            owners.push_back(std::move(*resources.value));
        } else {
            // 普通落笔的全图阶段与采样局部分开预收：两次网格复制、布局检查、
            // 派生刷新、区块差分/写回及净差邻域检查共按32次顶点访问覆盖；
            // WPM的解析副本、分层核对和最终blight核对按4次字节访问覆盖。
            // 每个采样只扫描自身足迹（平滑含9邻域、pending和局部位置刷新），
            // 不把一个尺寸1的落点按整张513²地图重复收费。
            const auto vertices = candidate.Terrain()->vertices.size();
            const auto pathingPasses = stroke.brush.kind == MapData::TerrainBrushKind::PathingRestriction ? 18u
                : stroke.brush.kind == MapData::TerrainBrushKind::Blight ? 12u : 4u;
            std::size_t peakBytes = 0;
            const auto availableBytes = limits.maximumCandidateBytes - historyBytes;
            if (!reserve(work, limits.maximumWork, vertices, 32)
                || !reserve(work, limits.maximumWork, pathingBytes, pathingPasses)
                || !reserve(peakBytes, availableBytes, vertices, 256)
                || !reserve(peakBytes, availableBytes, pathingBytes, 6))
                return fail("TERRAIN_REPLAY_BUDGET", "普通笔刷累计扫描或撤销候选预算超限。");
            for (const auto& point : stroke.points) {
                const auto footprint = stroke.brush.kind == MapData::TerrainBrushKind::PathingRestriction
                    ? MapData::GetPathingRestrictionFootprint(candidate.Terrain()->header, stroke.brush, point)
                    : MapData::GetTerrainBrushFootprint(*candidate.Terrain(), stroke.brush, point);
                if (!footprint)
                    return fail("TERRAIN_REPLAY_POINT", "普通笔刷尺寸、形态或落点无效，未返回部分候选。");
                const auto area = std::size_t(footprint->x1 - footprint->x0) * (footprint->y1 - footprint->y0);
                if (!reserve(work, limits.maximumWork, area, 16))
                    return fail("TERRAIN_REPLAY_BUDGET", "普通笔刷采样足迹累计预算超限。");
            }
            for (const auto kind : {MapDataKind::Doodads, MapDataKind::Units, MapDataKind::Settings}) {
                if (!stroke.followDoodads) break;
                const auto size = candidate.SnapshotByteSize(kind);
                if (!size.Succeeded() || !size.value) return {{}, size.diagnostics};
                if (!reserve(work, limits.maximumWork, *size.value, 8)
                    || !reserve(peakBytes, availableBytes, *size.value, 64))
                    return fail("TERRAIN_REPLAY_BUDGET", "普通笔刷摆放联动累计预算超限。");
            }
            const auto propagationLimit = std::min(MapData::TerrainLevelPropagationMaximumWork, limits.maximumWork - work);
            if (!propagationLimit) return fail("TERRAIN_REPLAY_BUDGET", "地形重放的工作预算已用尽。");
            // 一次已计访问最多追加一个uint32队列项；vector增长时旧/新三倍容量先预留，
            // 不能用很小网格的静态容量上界掩盖异常高低图的传播辅助队列。
            if (stroke.brush.kind == MapData::TerrainBrushKind::CliffLevel
                && !reserve(peakBytes, availableBytes, propagationLimit, 3 * sizeof(std::uint32_t)))
                return fail("TERRAIN_REPLAY_BUDGET", "层级传播队列的临时峰值超过候选预算。");
            auto plan = PrepareTerrainEdit(candidate, document, binding, stroke.brush, stroke.points, stroke.followDoodads, propagationLimit);
            if (!plan.Succeeded() || !plan.value) return {{}, plan.diagnostics};
            if (!reserve(work, limits.maximumWork, plan.value->Plan().Changed().levelPropagationWork))
                return fail("TERRAIN_REPLAY_BUDGET", "悬崖层级传播超过整个序列剩余工作预算。");
            if (!reserve(historyBytes, limits.maximumCandidateBytes, plan.value->Plan().RetainedCommandBytes()))
                return fail("TERRAIN_REPLAY_BUDGET", "普通笔刷累计保留历史容量超限。");
            const auto applied = ApplyTerrainEdit(candidate, document, *plan.value);
            if (!applied.Succeeded()) return {{}, applied.diagnostics};
        }
        if (candidate.Revision() != beforeRevision) {
            if (receipts) receipts->push_back(candidate.LastEdit());
        } else if (journal) return fail("TERRAIN_JOURNAL_NO_CHANGE", "记录包含未形成真实命令的空笔划，未安装任何前缀。");
        ++strokeIndex;
    }
    // 捕获之后可能发生来源变更；全部owners复核到同一实际来源后才返回候选。
    for (const auto& owner : owners) {
        const auto checked = owner.VerifySourceInputs(source, MapData::TerrainSavePlanLimits{}.maximumBytes);
        if (!checked.Succeeded()) return {{}, checked.diagnostics};
    }
    return Output::Success(std::move(owners));
}

std::string Canonical(std::string name) {
    std::replace(name.begin(), name.end(), '\\', '/');
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (name.starts_with("map/")) name.erase(0, 4);
    return name;
}

bool SourceMatches(const MapDocument& original, std::span<const std::byte> terrain,
    std::span<const std::byte> pathing, std::span<const ArchiveExactMember> actual) {
    if (actual.size() != 10 || !actual[0].present || !actual[1].present
        || !std::equal(terrain.begin(), terrain.end(), actual[0].bytes.begin(), actual[0].bytes.end())
        || !std::equal(pathing.begin(), pathing.end(), actual[1].bytes.begin(), actual[1].bytes.end())) return false;
    for (const auto& state : original.Members()) {
        std::size_t present = 0;
        for (std::size_t index = 2; index < actual.size(); ++index) {
            const auto& member = actual[index];
            if (state.kind == MapDataKind::Settings) {
                if (Canonical(member.name) != "war3map.w3i" && Canonical(member.name) != "table/w3i.ini") continue;
            } else if (Canonical(member.name) != Canonical(state.memberName)) continue;
            if (!member.present) continue;
            ++present;
            const auto expected = std::find_if(state.sources.begin(), state.sources.end(), [&](const auto& item) {
                return Canonical(item.name) == Canonical(member.name);
            });
            if (expected == state.sources.end() || expected->data != member.bytes) return false;
        }
        if (present != state.sources.size() || (present == 0 && state.state != MapDataState::Missing)) return false;
    }
    return true;
}

template<class Source, class Reader>
Result<TerrainReplayCandidate> CreateReplay(const Source& source, const MapDocument& original,
    std::span<const std::byte> terrain, std::span<const std::byte> pathing,
    std::span<const MapData::TerrainStrokeIntent> strokes, const std::filesystem::path& gameRoot,
    const TerrainReplayLimits& limits, const std::string& sha, Reader&& reader,
    const MapData::TerrainStrokeJournal* journal = nullptr) {
    using Output = Result<TerrainReplayCandidate>;
    const auto mismatch = [] { return Output::Failure("Terrain.Replay", "TERRAIN_REPLAY_SOURCE_MISMATCH", "原始W3E/WPM、地图成员或已证缺失项与实际来源不一致。"); };
    const auto budgetFailure = [] { return Output::Failure("Terrain.Replay", "TERRAIN_REPLAY_BUDGET", "实际来源、会话基线或累计重放预算超限。"); };
    const auto sourceReadFailure = [](std::vector<Diagnostic> diagnostics) {
        // 只有精确读取自身的容量不足允许回退完整raw；实际基线的身份、结构或I/O失败须硬拒。
        // 资源owner在Replay内另有捕获边界，不能按通用Mpq/IO子系统混判其资源缺失。
        const bool budgetOnly = !diagnostics.empty() && std::all_of(diagnostics.begin(), diagnostics.end(), [](const auto& diagnostic) {
            return (diagnostic.subsystem == "MapArchiveSession" && diagnostic.code == "EXACT_MEMBER_LIMIT")
                || (diagnostic.subsystem == "Mpq" && diagnostic.code == "FILE_READ_BUDGET_EXCEEDED")
                || (diagnostic.subsystem == "VerifiedFile" && diagnostic.code == "VERIFIED_FILE_BUDGET_EXCEEDED")
                || (diagnostic.subsystem == "Terrain.PathingResources" && diagnostic.code == "TERRAIN_PATHING_RESOURCE_BUDGET");
        });
        if (budgetOnly) return Output{{}, std::move(diagnostics)};
        auto failure = Output::Failure("Terrain.Replay", "TERRAIN_REPLAY_SOURCE_READ_FAILED", "实际地图基线无法完整验证，未恢复或安装任何前缀。");
        for (auto& diagnostic : diagnostics) failure.diagnostics.push_back(std::move(diagnostic));
        return failure;
    };
    const TerrainReplayLimits hard;
    if (!limits.maximumResourceBytes || limits.maximumResourceBytes > hard.maximumResourceBytes
        || !limits.maximumWork || limits.maximumWork > hard.maximumWork
        || !limits.maximumCandidateBytes || limits.maximumCandidateBytes > hard.maximumCandidateBytes)
        return Output::Failure("Terrain.Replay", "TERRAIN_REPLAY_LIMITS", "重放预算只能收紧且不能为零。");
    if (strokes.empty() || strokes.size() > 128)
        return Output::Failure("Terrain.Replay", "TERRAIN_PATCH_STROKE_BUDGET", "地形重放需要1到128笔原始命令。");
    const auto aligned = ValidateJournalReplayAlignment(strokes, journal);
    if (!aligned.Succeeded()) return {{}, aligned.diagnostics};
    std::size_t points = 0;
    std::size_t inputIndex = 0;
    for (const auto& stroke : strokes) {
        const bool ordinary = journal && journal->strokes[inputIndex].ordinaryDoodads.has_value();
        ++inputIndex;
        if (ordinary) continue;
        if (stroke.palette) {
            if (!PaletteCommand(stroke)) return Output::Failure("Terrain.Replay", "TERRAIN_REPLAY_PALETTE_CONFLICT", "贴图表记录仅接受既有槽替换或Ground表尾追加，不能携带笔刷、范围或采样点。");
            continue;
        }
        if (stroke.bounds) {
            const auto& r = stroke.bounds->playable;
            if (!Neutral(stroke.brush) || !stroke.points.empty() || stroke.followDoodads || stroke.waterBrush
                || stroke.bounds->algorithm != 1 || r.west >= r.east || r.south >= r.north || r.east > 2048 || r.north > 2048)
                return Output::Failure("Terrain.Replay", "TERRAIN_REPLAY_BOUNDS_CONFLICT", "范围命令需要独立的半开地格矩形，不能携带笔刷或采样点。");
            continue;
        }
        if (stroke.points.empty() || stroke.points.size() > 4096 - points)
            return Output::Failure("Terrain.Replay", "TERRAIN_PATCH_POINT_BUDGET", "地形重放累计点数超过4096或包含空笔划。");
        points += stroke.points.size();
        if (stroke.waterBrush && !Neutral(stroke.brush))
            return Output::Failure("Terrain.Replay", "TERRAIN_REPLAY_BRUSH_CONFLICT", "水刷不能同时指定普通笔刷。");
    }
    // 在实际捕获和Attach解析前预收。W3E的两次来源比较及解析/刷新按3倍wire，
    // WPM按两次来源比较、解析cells/original、保护层构造、两个会话副本等8次计。
    // 其它成员在此只做两个来源比较/原始会话副本，不重复按32倍解析opaque DOO/WTS。
    // 原始/当前网格与来源副本为常驻基线；每笔临时峰值和真实历史在Replay中分开核对。
    auto remainingLimits = limits;
    const auto charge = [](std::size_t& remaining, std::size_t count, std::size_t multiple) {
        if (count > remaining / multiple) return false;
        remaining -= count * multiple; return true;
    };
    if (journal && (!charge(remainingLimits.maximumCandidateBytes, journal->baselinePathingLayers.size(), 4)
        || !charge(remainingLimits.maximumWork, journal->baselinePathingLayers.size(), 4))) return budgetFailure();
    std::size_t sourceBytes = terrain.size();
    if (pathing.size() > limits.maximumResourceBytes || sourceBytes > limits.maximumResourceBytes - pathing.size()) return budgetFailure();
    sourceBytes += pathing.size();
    if (!charge(remainingLimits.maximumCandidateBytes, terrain.size(), 64)
        || !charge(remainingLimits.maximumCandidateBytes, pathing.size(), 4)
        || !charge(remainingLimits.maximumWork, terrain.size(), 3)
        || !charge(remainingLimits.maximumWork, pathing.size(), 8)
        || !charge(remainingLimits.maximumResourceBytes, terrain.size(), 2)
        || !charge(remainingLimits.maximumResourceBytes, pathing.size(), 2)) return budgetFailure();
    for (const auto& member : original.Members()) for (const auto& input : member.sources) {
        if (!charge(remainingLimits.maximumCandidateBytes, input.data.size(), 8)
            || !charge(remainingLimits.maximumWork, input.data.size(), 6)
            || !charge(remainingLimits.maximumResourceBytes, input.data.size(), 2)
            || input.data.size() > limits.maximumResourceBytes - sourceBytes) return budgetFailure();
        sourceBytes += input.data.size();
    }
    if (!remainingLimits.maximumCandidateBytes || !remainingLimits.maximumWork || !remainingLimits.maximumResourceBytes)
        return budgetFailure();
    // 精确捕获不依赖listfile；同时核原生/LNI来源和absence，在构造或落笔前拒绝替换。
    const std::array<std::string, 10> names{"war3map.w3e", "war3map.wpm", "war3map.w3i", "table/w3i.ini",
        "war3mapUnits.doo", "war3map.doo", "war3map.w3r", "war3map.w3c", "war3map.w3s", "war3map.wts"};
    ArchiveExactMemberLimits sourceLimits;
    sourceLimits.memberCount = names.size();
    // 解码后的基线已在上面预收；精确读取还要核对MPQ存储大小，压缩成员可比解码结果大。
    // 不用sourceBytes缩紧存储读取帽，仍受exact原硬限及调用方资源/候选帽约束。
    sourceLimits.totalBytes = std::min({sourceLimits.totalBytes, limits.maximumResourceBytes, limits.maximumCandidateBytes});
    sourceLimits.memberBytes = sourceLimits.totalBytes;
    auto actual = reader(names, sourceLimits);
    if (!actual.Succeeded() || !actual.value) return sourceReadFailure(std::move(actual.diagnostics));
    if (!SourceMatches(original, terrain, pathing, *actual.value)) return mismatch();
    if (journal) {
        if (journal->sourceSha256 != sha || journal->members.size() != actual.value->size())
            return Output::Failure("Terrain.Journal", "TERRAIN_JOURNAL_BASELINE_MISMATCH", "地形记录不属于实际工程基线。");
        for (std::size_t i = 0; i < actual.value->size(); ++i) {
            const auto& member = (*actual.value)[i]; const auto& expected = journal->members[i];
            if (expected.name != Canonical(member.name) || expected.present != member.present
                || expected.bytes != member.bytes.size())
                return Output::Failure("Terrain.Journal", "TERRAIN_JOURNAL_BASELINE_MISMATCH", "地形记录成员或缺失项与实际基线不符。");
            if (member.present) {
                const auto hash = Sha256(member.bytes);
                if (!hash.Succeeded() || !hash.value || *hash.value != expected.sha256)
                    return Output::Failure("Terrain.Journal", "TERRAIN_JOURNAL_BASELINE_MISMATCH", "地形记录完整来源成员身份不符。");
            }
        }
    }
    actual.value.reset(); // 完整来源副本不与候选、规则owner和历史并存。
    auto candidate = std::make_unique<MapData::MapDataEditSession>(original);
    const auto attached = candidate->AttachTerrain(terrain);
    if (!attached.Succeeded()) return {{}, attached.diagnostics};
    const auto attachedPathing = candidate->AttachPathing(pathing);
    if (!attachedPathing.Succeeded()) return {{}, attachedPathing.diagnostics};
    if (journal && !journal->baselinePathingLayers.empty()) {
        const auto restored = candidate->SetTerrainAndPathingDraft(terrain, pathing, journal->baselinePathingLayers);
        if (!restored.Succeeded()) return {{}, restored.diagnostics};
    }
    const auto baseline = candidate->Terrain();
    std::vector<MapData::MapDataEditSession::EditReceipt> receipts;
    receipts.reserve(strokes.size());
    auto replayed = Replay(source, *candidate, *baseline, pathing.size(), strokes, gameRoot, remainingLimits, sha, journal, &receipts);
    if (!replayed.Succeeded() || !replayed.value) return {{}, replayed.diagnostics};
    auto finalSource = reader(names, sourceLimits);
    if (!finalSource.Succeeded() || !finalSource.value) return sourceReadFailure(std::move(finalSource.diagnostics));
    if (!SourceMatches(original, terrain, pathing, *finalSource.value)) return mismatch();
    return Output::Success({std::move(candidate), std::move(*replayed.value), std::move(receipts)});
}

template<class Reader>
Result<MapData::TerrainStrokeJournal> CaptureJournal(const MapData::MapDataEditSession& edits,
    MapData::TerrainStrokeJournal journal, const std::string& sourceSha, Reader&& reader) {
    if (journal.strokes.empty()) return Result<MapData::TerrainStrokeJournal>::Success(std::move(journal));
    std::vector<std::string> names;
    for (const auto name : MapData::TerrainJournalMemberNames) names.emplace_back(name);
    ArchiveExactMemberLimits limits; limits.memberCount = names.size();
    // 精确成员API的原64MiB存储/解码帽保持不变，128MiB是owner累计资源预算。
    const auto actual = reader(names, limits);
    if (!actual.Succeeded() || !actual.value) return {{}, actual.diagnostics};
    const auto checked = edits.VerifyTerrainJournalBaseline(*actual.value);
    if (!checked.Succeeded()) return {{}, checked.diagnostics};
    journal.sourceSha256 = sourceSha;
    journal.members.clear();
    for (std::size_t i = 0; i < actual.value->size(); ++i) {
        const auto& member = (*actual.value)[i];
        std::string hash;
        if (member.present) {
            const auto sha = Sha256(member.bytes);
            if (!sha.Succeeded() || !sha.value) return {{}, sha.diagnostics};
            hash = *sha.value;
        }
        journal.members.push_back({std::string(MapData::TerrainJournalMemberNames[i]), member.present, std::move(hash), member.bytes.size()});
    }
    const auto materialized = MapData::MaterializeTerrainJournalDoodads(journal);
    if (!materialized.Succeeded()) return {{}, materialized.diagnostics};
    const auto valid = MapData::ValidateTerrainStrokeJournal(journal);
    if (!valid.Succeeded()) return {{}, valid.diagnostics};
    return Result<MapData::TerrainStrokeJournal>::Success(std::move(journal));
}

template<class Source, class Reader>
Result<TerrainReplayCandidate> RestoreJournal(const Source& source, const MapDocument& original,
    std::span<const std::byte> terrain, std::span<const std::byte> pathing,
    const MapData::TerrainStrokeJournal& journal, const MapData::MapDataEditSession& expected,
    const std::filesystem::path& gameRoot, const TerrainReplayLimits& limits,
    const std::string& sourceSha, Reader&& reader) {
    const TerrainReplayLimits hard;
    if (!limits.maximumResourceBytes || limits.maximumResourceBytes > hard.maximumResourceBytes
        || !limits.maximumWork || limits.maximumWork > hard.maximumWork
        || !limits.maximumCandidateBytes || limits.maximumCandidateBytes > hard.maximumCandidateBytes)
        return Result<TerrainReplayCandidate>::Failure("Terrain.Replay", "TERRAIN_REPLAY_LIMITS", "重放预算只能收紧且不能为零。");
    // 结构校验不编码、不分配64MiB临时wire；随后直接预收同一总预算。
    const auto valid = MapData::ValidateTerrainStrokeJournal(journal);
    if (!valid.Succeeded()) return {{}, valid.diagnostics};
    auto remaining = limits;
    const auto rawCost = expected.MeasureTerrainJournalDraft(limits.maximumCandidateBytes, limits.maximumWork);
    if (!rawCost.Succeeded() || !rawCost.value) return {{}, rawCost.diagnostics};
    remaining.maximumCandidateBytes -= rawCost.value->retainedBytes;
    remaining.maximumWork -= rawCost.value->comparisonWork;
    if (!remaining.maximumCandidateBytes || !remaining.maximumWork)
        return Result<TerrainReplayCandidate>::Failure("Terrain.Journal", "TERRAIN_REPLAY_BUDGET", "raw草稿未给完整重放保留候选或工作预算。");
    const auto charge = [](std::size_t& budget, std::size_t count, std::size_t element) {
        if (count > budget / element) return false;
        budget -= count * element; return true;
    };
    // 四说明字段正文保留在raw W3I；为重放后的窄域合并预收临时解析/命令容量。
    // source与expected都计，不能在CreateReplay耗尽预算后额外分配。
    std::size_t descriptionBytes = 0, descriptionWork = 0;
    const auto expectedSettings = expected.SnapshotByteSize(MapDataKind::Settings);
    if (!expectedSettings.Succeeded() || !expectedSettings.value) return {{}, expectedSettings.diagnostics};
    const auto& sourceSettings = original.Member(MapDataKind::Settings);
    std::size_t sourceSettingsBytes = 0;
    for (const auto& member : sourceSettings.sources) {
        if (member.data.size() > 1024u * 1024u - sourceSettingsBytes)
            return Result<TerrainReplayCandidate>::Failure("Terrain.Journal", "TERRAIN_REPLAY_BUDGET", "工程原始W3I超过说明恢复预算。");
        sourceSettingsBytes += member.data.size();
    }
    if (*expectedSettings.value > 1024u * 1024u)
        return Result<TerrainReplayCandidate>::Failure("Terrain.Journal", "TERRAIN_REPLAY_BUDGET", "工程最终W3I超过说明恢复预算。");
    descriptionBytes = 4096 + (*expectedSettings.value + sourceSettingsBytes) * 16;
    descriptionWork = 4096 + (*expectedSettings.value + sourceSettingsBytes) * 8;
    if (!charge(remaining.maximumCandidateBytes, descriptionBytes, 1)
        || !charge(remaining.maximumWork, descriptionWork, 1))
        return Result<TerrainReplayCandidate>::Failure("Terrain.Journal", "TERRAIN_REPLAY_BUDGET", "工程说明合并没有留下完整重放预算。");
    // 解码后的journal正文仍与候选共存；普通DOO的插入载荷不能只收临时patch/命令而漏掉输入驻留。
    for (const auto& stroke : journal.strokes)
        if (stroke.ordinaryDoodads
            && (!charge(remaining.maximumCandidateBytes, stroke.ordinaryDoodads->inserted.capacity(), 1)
                || !charge(remaining.maximumWork, stroke.ordinaryDoodads->inserted.size(), 1)))
            return Result<TerrainReplayCandidate>::Failure("Terrain.Journal", "TERRAIN_REPLAY_BUDGET", "普通DOO记录的输入载荷超过剩余候选或工作预算。");
    // owned factory现有接口接收连续typed intents；在复制任何点之前预收该有界副本。
    if (!charge(remaining.maximumCandidateBytes, journal.strokes.size(), sizeof(MapData::TerrainStrokeIntent)))
        return Result<TerrainReplayCandidate>::Failure("Terrain.Journal", "TERRAIN_REPLAY_BUDGET", "前向笔划副本超过候选预算。");
    for (const auto& stroke : journal.strokes)
        if (!charge(remaining.maximumCandidateBytes, stroke.intent.points.size(), sizeof(MapData::TerrainStrokePoint))
            || !charge(remaining.maximumCandidateBytes, stroke.intent.brush.groundTexture.size(), 1)
            || !charge(remaining.maximumWork, stroke.intent.points.size(), 2)
            || !charge(remaining.maximumWork, 1, 64))
            return Result<TerrainReplayCandidate>::Failure("Terrain.Journal", "TERRAIN_REPLAY_BUDGET", "前向点序列复制或工作预算不足。");
    if (!remaining.maximumCandidateBytes || !remaining.maximumWork)
        return Result<TerrainReplayCandidate>::Failure("Terrain.Journal", "TERRAIN_REPLAY_BUDGET", "前向笔划副本未留下完整重放预算。");
    std::vector<MapData::TerrainStrokeIntent> intents;
    intents.reserve(journal.strokes.size());
    for (const auto& stroke : journal.strokes) intents.push_back(stroke.intent);
    auto candidate = CreateReplay(source, original, terrain, pathing, intents, gameRoot, remaining, sourceSha,
        std::forward<Reader>(reader), &journal);
    if (!candidate.Succeeded() || !candidate.value) return candidate;
    const auto beforeDescriptions = candidate.value->session->Revision();
    const auto descriptions = candidate.value->session->RestoreTerrainJournalDescriptions(expected, descriptionBytes, descriptionWork);
    if (!descriptions.Succeeded()) return {{}, descriptions.diagnostics};
    if (candidate.value->session->Revision() != beforeDescriptions)
        candidate.value->receipts.push_back(candidate.value->session->LastEdit());
    // W3E/WPM、完整DOO/Units和所有其它成员/删除、P/savedS/currentS必须共同相等。
    if (!candidate.value->session->MatchesTerrainJournalDraft(expected))
        return Result<TerrainReplayCandidate>::Failure("Terrain.Journal", "TERRAIN_JOURNAL_OUTPUT_MISMATCH",
            "前向记录重放与完整工程草稿或通行来源分层不符，未安装任何前缀。");
    return candidate;
}
} // namespace

Result<MapData::TerrainStrokeResult> TerrainPathingSave::EditTextureSlot(const MapArchiveSession& source,
    MapData::MapDataEditSession& edits, MapData::TerrainTextureTable table, std::uint32_t slot,
    const std::string& rawcode, const std::filesystem::path& gameRoot, std::size_t maximumBytes) {
    return Guard<MapData::TerrainStrokeResult>([&]() -> Result<MapData::TerrainStrokeResult> {
        auto prepared = PrepareTextureSlot(source, edits, table, slot, rawcode, gameRoot, maximumBytes);
        if (!prepared.Succeeded() || !prepared.value) return {{}, prepared.diagnostics};
        return edits.ApplyPreparedTerrainEdit(*prepared.value);
    });
}

Result<MapData::TerrainStrokeResult> TerrainPathingSave::EditTextureSlot(const WorkspaceSnapshot& source,
    MapData::MapDataEditSession& edits, MapData::TerrainTextureTable table, std::uint32_t slot,
    const std::string& rawcode, const std::filesystem::path& gameRoot, std::size_t maximumBytes) {
    return Guard<MapData::TerrainStrokeResult>([&]() -> Result<MapData::TerrainStrokeResult> {
        auto prepared = PrepareTextureSlot(source, edits, table, slot, rawcode, gameRoot, maximumBytes);
        if (!prepared.Succeeded() || !prepared.value) return {{}, prepared.diagnostics};
        return edits.ApplyPreparedTerrainEdit(*prepared.value);
    });
}

Result<MapData::MapDataEditSession::PreparedTerrainEdit> TerrainPathingSave::PrepareTextureSlot(const MapArchiveSession& source,
    const MapData::MapDataEditSession& edits, MapData::TerrainTextureTable table, std::uint32_t slot,
    const std::string& rawcode, const std::filesystem::path& gameRoot, std::size_t maximumBytes) {
    return Guard<MapData::MapDataEditSession::PreparedTerrainEdit>([&] { return Services::PrepareTextureSlot(source, edits, table, slot, rawcode, gameRoot, maximumBytes,
        [&](auto names, const auto& limits) { return source.CaptureExactMembers(names, limits); }); });
}

Result<MapData::MapDataEditSession::PreparedTerrainEdit> TerrainPathingSave::PrepareTextureSlot(const WorkspaceSnapshot& source,
    const MapData::MapDataEditSession& edits, MapData::TerrainTextureTable table, std::uint32_t slot,
    const std::string& rawcode, const std::filesystem::path& gameRoot, std::size_t maximumBytes) {
    return Guard<MapData::MapDataEditSession::PreparedTerrainEdit>([&] { return Services::PrepareTextureSlot(source, edits, table, slot, rawcode, gameRoot, maximumBytes,
        [&](auto names, const auto& limits) { return MapData::TerrainPathingResources::ReadDirectoryMembers(source, names, limits); }); });
}

Result<MapData::TerrainStrokeJournal> TerrainPathingSave::CaptureJournal(const MapArchiveSession& source,
    const MapData::MapDataEditSession& edits) {
    return Guard<MapData::TerrainStrokeJournal>([&] {
        auto prefix = edits.ExportTerrainStrokeJournal();
        if (!prefix.Succeeded() || !prefix.value) return prefix;
        return CaptureJournal(source, edits, std::move(*prefix.value));
    });
}
Result<MapData::TerrainStrokeJournal> TerrainPathingSave::CaptureJournal(const WorkspaceSnapshot& source,
    const MapData::MapDataEditSession& edits) {
    return Guard<MapData::TerrainStrokeJournal>([&] {
        auto prefix = edits.ExportTerrainStrokeJournal();
        if (!prefix.Succeeded() || !prefix.value) return prefix;
        return CaptureJournal(source, edits, std::move(*prefix.value));
    });
}
Result<MapData::TerrainStrokeJournal> TerrainPathingSave::CaptureJournal(const MapArchiveSession& source,
    const MapData::MapDataEditSession& frozenBaseline, MapData::TerrainStrokeJournal prefix) {
    return Guard<MapData::TerrainStrokeJournal>([&] { return Services::CaptureJournal(frozenBaseline, std::move(prefix), source.SourceSha256(),
        [&](auto names, const auto& limits) { return source.CaptureExactMembers(names, limits); }); });
}
Result<MapData::TerrainStrokeJournal> TerrainPathingSave::CaptureJournal(const WorkspaceSnapshot& source,
    const MapData::MapDataEditSession& frozenBaseline, MapData::TerrainStrokeJournal prefix) {
    return Guard<MapData::TerrainStrokeJournal>([&] { return Services::CaptureJournal(frozenBaseline, std::move(prefix), source.aggregateSha256,
        [&](auto names, const auto& limits) { return MapData::TerrainPathingResources::ReadDirectoryMembers(source, names, limits, true); }); });
}
Result<TerrainReplayCandidate> TerrainPathingSave::RestoreJournal(const MapArchiveSession& source,
    const MapDocument& original, std::span<const std::byte> terrain, std::span<const std::byte> pathing,
    const MapData::TerrainStrokeJournal& journal, const MapData::MapDataEditSession& expectedDraft,
    const std::filesystem::path& gameRoot, const TerrainReplayLimits& limits) {
    return Guard<TerrainReplayCandidate>([&] { return Services::RestoreJournal(source, original, terrain, pathing, journal, expectedDraft,
        gameRoot, limits, source.SourceSha256(), [&](auto names, const auto& captureLimits) { return source.CaptureExactMembers(names, captureLimits); }); });
}
Result<TerrainReplayCandidate> TerrainPathingSave::RestoreJournal(const WorkspaceSnapshot& source,
    const MapDocument& original, std::span<const std::byte> terrain, std::span<const std::byte> pathing,
    const MapData::TerrainStrokeJournal& journal, const MapData::MapDataEditSession& expectedDraft,
    const std::filesystem::path& gameRoot, const TerrainReplayLimits& limits) {
    return Guard<TerrainReplayCandidate>([&] { return Services::RestoreJournal(source, original, terrain, pathing, journal, expectedDraft,
        gameRoot, limits, source.aggregateSha256, [&](auto names, const auto& captureLimits) {
            return MapData::TerrainPathingResources::ReadDirectoryMembers(source, names, captureLimits, true); }); });
}

Result<TerrainReplayCandidate> TerrainPathingSave::ReplayStrokes(
    const MapArchiveSession& source, const MapDocument& original,
    std::span<const std::byte> terrain, std::span<const std::byte> pathing, std::span<const MapData::TerrainStrokeIntent> strokes,
    const std::filesystem::path& gameRoot, const TerrainReplayLimits& limits) {
    return Guard<TerrainReplayCandidate>([&] { return CreateReplay(source, original, terrain, pathing, strokes, gameRoot, limits, source.SourceSha256(),
        [&](auto names, const auto& captureLimits) { return source.CaptureExactMembers(names, captureLimits); }); });
}

Result<TerrainReplayCandidate> TerrainPathingSave::ReplayStrokes(
    const WorkspaceSnapshot& source, const MapDocument& original,
    std::span<const std::byte> terrain, std::span<const std::byte> pathing, std::span<const MapData::TerrainStrokeIntent> strokes,
    const std::filesystem::path& gameRoot, const TerrainReplayLimits& limits) {
    return Guard<TerrainReplayCandidate>([&] { return CreateReplay(source, original, terrain, pathing, strokes, gameRoot, limits, source.aggregateSha256,
        [&](auto names, const auto& captureLimits) { return MapData::TerrainPathingResources::ReadDirectoryMembers(source, names, captureLimits, true); }); });
}

Result<MapData::TerrainPathingResources> TerrainPathingSave::CaptureNative(const MapArchiveSession& source,
    const MapData::TerrainGridSnapshot& before, const MapData::TerrainGridSnapshot& after,
    const std::filesystem::path& gameRoot, std::size_t maximumBytes, const std::vector<std::byte>* currentDoodads) {
    return Guard<MapData::TerrainPathingResources>([&] {
        return MapData::TerrainPathingResources::CaptureNativeWithObjects(source, before, after, gameRoot,
            maximumBytes, ObjectData::InspectNativeTerrainObjects, currentDoodads);
    });
}

Result<MapData::TerrainPathingResources> TerrainPathingSave::CaptureNative(const WorkspaceSnapshot& source,
    const MapData::TerrainGridSnapshot& before, const MapData::TerrainGridSnapshot& after,
    const std::filesystem::path& gameRoot, std::size_t maximumBytes, const std::vector<std::byte>* currentDoodads) {
    return Guard<MapData::TerrainPathingResources>([&] {
        return MapData::TerrainPathingResources::CaptureNativeWithObjects(source, before, after, gameRoot,
            maximumBytes, ObjectData::InspectNativeTerrainObjects, currentDoodads);
    });
}

Result<MapData::TerrainSavePlan> TerrainPathingSave::PrepareForSave(const MapArchiveSession& source,
    const MapData::MapDataEditSession& edits, const std::filesystem::path& gameRoot,
    const MapData::TerrainSavePlanLimits& limits, std::span<const MemberReplacement> finalMembers,
    std::span<const std::string> finalRemovals) {
    return Guard<MapData::TerrainSavePlan>([&] {
        const MapData::TerrainSavePlan::NativeResourceCapture capture = [&](const auto& before, const auto& after, const auto& doodads, auto maximumBytes) {
            return CaptureNative(source, before, after, gameRoot, maximumBytes, doodads);
        };
        return MapData::TerrainSavePlan::PrepareArchive(source, edits, limits, &gameRoot, &capture, finalMembers, finalRemovals);
    });
}

Result<MapData::TerrainSavePlan> TerrainPathingSave::PrepareForSave(const WorkspaceSnapshot& source,
    const MapData::MapDataEditSession& edits, const std::filesystem::path& gameRoot,
    const MapData::TerrainSavePlanLimits& limits, std::span<const MemberReplacement> finalMembers,
    std::span<const std::string> finalRemovals) {
    return Guard<MapData::TerrainSavePlan>([&] {
        const MapData::TerrainSavePlan::NativeResourceCapture capture = [&](const auto& before, const auto& after, const auto& doodads, auto maximumBytes) {
            return CaptureNative(source, before, after, gameRoot, maximumBytes, doodads);
        };
        return MapData::TerrainSavePlan::PrepareWorkspace(source, edits, limits, &gameRoot, &capture, finalMembers, finalRemovals);
    });
}

Result<MapData::TerrainPathingResources> TerrainPathingSave::CaptureWaterBrushRules(const MapArchiveSession& source,
    const MapData::TerrainGridSnapshot& sourceTerrain, const MapData::TerrainGridSnapshot& currentTerrain,
    const MapData::DoodadPlacements& currentDoodads, const std::filesystem::path& gameRoot,
    bool enforceWaterLimits, std::size_t maximumBytes) {
    return Guard<MapData::TerrainPathingResources>([&]() -> Result<MapData::TerrainPathingResources> {
        auto rules = MapData::TerrainPathingResources::CaptureWaterBrushRules(source, sourceTerrain, currentTerrain,
            gameRoot, enforceWaterLimits, maximumBytes);
        if (!rules.Succeeded() || !rules.value) return rules;
        const auto bound = rules.value->CaptureWaterSpecialInputs([&](auto names, const auto& limits) {
            return source.CaptureExactMembers(names, limits);
        }, currentTerrain, currentDoodads, ObjectData::InspectNativeTerrainObjects);
        if (!bound.Succeeded()) return {{}, bound.diagnostics};
        return rules;
    });
}

Result<MapData::TerrainPathingResources> TerrainPathingSave::CaptureWaterBrushRules(const WorkspaceSnapshot& source,
    const MapData::TerrainGridSnapshot& sourceTerrain, const MapData::TerrainGridSnapshot& currentTerrain,
    const MapData::DoodadPlacements& currentDoodads, const std::filesystem::path& gameRoot,
    bool enforceWaterLimits, std::size_t maximumBytes) {
    return Guard<MapData::TerrainPathingResources>([&]() -> Result<MapData::TerrainPathingResources> {
        auto rules = MapData::TerrainPathingResources::CaptureWaterBrushRules(source, sourceTerrain, currentTerrain,
            gameRoot, enforceWaterLimits, maximumBytes);
        if (!rules.Succeeded() || !rules.value) return rules;
        const auto bound = rules.value->CaptureWaterSpecialInputs([&](auto names, const auto& limits) {
            return MapData::TerrainPathingResources::ReadDirectoryMembers(source, names, limits);
        }, currentTerrain, currentDoodads, ObjectData::InspectNativeTerrainObjects);
        if (!bound.Succeeded()) return {{}, bound.diagnostics};
        return rules;
    });
}

Result<TerrainPatch> TerrainPathingSave::Seal(const MapDocument& original,
    std::span<const std::byte> terrain, std::span<const std::byte> pathing,
    const WorkspaceSnapshot& source, const MapData::MapDataEditSession& current,
    std::span<const MapData::TerrainStrokeIntent> strokes, const std::filesystem::path& gameRoot) {
    return Guard<TerrainPatch>([&]() -> Result<TerrainPatch> {
        const auto replayed = ReplayStrokes(source, original, terrain, pathing, strokes, gameRoot);
        if (!replayed.Succeeded() || !replayed.value) return {{}, replayed.diagnostics};
        const auto plan = PrepareForSave(source, current, gameRoot);
        if (!plan.Succeeded() || !plan.value) return {{}, plan.diagnostics};
        return MapData::TerrainPatchCodec::SealWithPlan(original, terrain, pathing, source, current, strokes, gameRoot, &*plan.value, replayed.value->session.get());
    });
}

} // namespace War3Map::Services
