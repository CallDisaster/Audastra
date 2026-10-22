#include "McpWorkspace.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/IO/FileBytes.h"
#include "War3MapCore/MapData/TerrainPatchCodec.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <set>

namespace War3Map::Mcp {

struct McpTerrainOverlay {
    explicit McpTerrainOverlay(const MapDocument& original) : edits(original) {}
    MapData::MapDataEditSession edits;
    std::vector<MapData::TerrainStrokeIntent> strokes;
};

struct McpTerrainWorkspaceState {
    WorkspaceFile terrainFile;
    std::optional<WorkspaceFile> pathingFile;
    std::vector<std::byte> terrain, pathing;
    std::shared_ptr<McpTerrainOverlay> committed;
};

namespace {
template<class T, class U> Result<T> Failed(const U& source) { return {{}, source.diagnostics}; }

Result<std::optional<WorkspaceFile>> FindSource(const WorkspaceSnapshot& source, std::string_view member) {
    std::optional<WorkspaceFile> found;
    std::string wanted(member);
    std::transform(wanted.begin(), wanted.end(), wanted.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    for (const auto& file : source.files) {
        auto path = IO::GenericPathUtf8(file.relativePath);
        std::transform(path.begin(), path.end(), path.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (path != wanted && path != std::string("map/") + wanted) continue;
        if (found) return Result<std::optional<WorkspaceFile>>::Failure("MCP", "MCP_TERRAIN_SOURCE_AMBIGUOUS",
            "地形成员在目录根和 map 中存在多个来源，不能猜测。");
        found = file;
    }
    return Result<std::optional<WorkspaceFile>>::Success(std::move(found));
}

Result<std::vector<std::byte>> ReadSource(const WorkspaceSnapshot& source, const WorkspaceFile& file) {
    auto bytes = IO::ReadFileBytes(source.root / file.relativePath, 64 * 1024 * 1024 + 16);
    if (!bytes.Succeeded()) return bytes;
    const auto sha = Sha256(*bytes.value);
    if (!sha.Succeeded()) return Failed<std::vector<std::byte>>(sha);
    if (bytes.value->size() != file.size || *sha.value != file.sha256)
        return Result<std::vector<std::byte>>::Failure("MCP", "SOURCE_CHANGED", "地形成员字节与已打开目录快照不一致。");
    return bytes;
}

Result<std::shared_ptr<McpTerrainOverlay>> Replay(const MapDocument& original,
    const McpTerrainWorkspaceState& source, const std::vector<MapData::TerrainStrokeIntent>& strokes) {
    auto overlay = std::make_shared<McpTerrainOverlay>(original);
    const auto terrain = overlay->edits.AttachTerrain(source.terrain);
    if (!terrain.Succeeded()) return Failed<std::shared_ptr<McpTerrainOverlay>>(terrain);
    if (source.pathingFile) {
        const auto pathing = overlay->edits.AttachPathing(source.pathing);
        if (!pathing.Succeeded()) return Failed<std::shared_ptr<McpTerrainOverlay>>(pathing);
    }
    for (const auto& stroke : strokes) {
        const auto edited = overlay->edits.EditTerrain(stroke.brush, stroke.points, stroke.followDoodads);
        if (!edited.Succeeded()) return Failed<std::shared_ptr<McpTerrainOverlay>>(edited);
    }
    overlay->strokes = strokes;
    return Result<std::shared_ptr<McpTerrainOverlay>>::Success(std::move(overlay));
}

Status StrokeBudget(const std::vector<MapData::TerrainStrokeIntent>& strokes) {
    if (strokes.size() > 128) return Status::Failure("MCP", "TERRAIN_PATCH_STROKE_BUDGET", "目录事务最多累计 128 笔地形命令。");
    std::size_t points = 0;
    for (const auto& stroke : strokes) {
        if (stroke.points.empty() || stroke.points.size() > 4096 - points)
            return Status::Failure("MCP", "TERRAIN_PATCH_POINT_BUDGET", "目录事务累计采样点超过 4096 或包含空笔划。");
        points += stroke.points.size();
    }
    return Status::Ok();
}

Status CheckOnlyTerrainFiles(const SemanticDiff& diff, const TerrainPatch& sealed) {
    if (diff.changes.size() != sealed.Files().size())
        return Status::Failure("MCP", "MCP_TERRAIN_OVERLAY_MISMATCH", "地形候选必须恰好包含共用笔划重放产生的成员。");
    std::set<std::filesystem::path> matched;
    for (const auto& change : diff.changes) {
        const auto file = std::find_if(sealed.Files().begin(), sealed.Files().end(),
            [&](const auto& item) { return item.relativePath == change.path; });
        if (change.kind != "terrain" || file == sealed.Files().end() || !matched.insert(change.path).second
            || change.beforeSha256 != file->expectedSourceSha256)
            return Status::Failure("MCP", "MCP_TERRAIN_OVERLAY_MISMATCH", "地形目录覆盖与已证明重放结果不一致。");
        const auto sha = Sha256(file->replacement);
        if (!sha.Succeeded()) return {sha.diagnostics};
        if (change.afterSha256 != *sha.value)
            return Status::Failure("MCP", "MCP_TERRAIN_OVERLAY_MISMATCH", "地形目录覆盖与已证明重放结果不一致。");
    }
    return Status::Ok();
}

Status MatchArchiveSource(const Services::MapEditSaveRequest& request, const WorkspaceSnapshot& directory,
    const MapDocument& original) {
    const auto archive = MapArchiveSession::Open(request.sourceMap);
    if (!archive.Succeeded()) return {archive.diagnostics};
    std::vector<std::string> names{"war3map.w3e", "war3map.wpm", "war3map.wtg", "war3map.wct"};
    for (const auto& member : original.Members()) names.push_back(member.memberName);
    const auto identities = archive.value->DescribeExactMemberIdentities(request.expectedSourceSha256, names);
    if (!identities.Succeeded()) return {identities.diagnostics};
    for (const auto& identity : identities.value->members) {
        const auto expected = FindSource(directory, identity.name);
        if (!expected.Succeeded()) return {expected.diagnostics};
        if (identity.present != expected.value->has_value()
            || (identity.present && (identity.sha256 != (*expected.value)->sha256 || identity.byteSize != (*expected.value)->size)))
            return Status::Failure("MCP", "MCP_TERRAIN_ARCHIVE_SOURCE_MISMATCH",
                "指定地图的地形、WPM、触发器或地图数据成员与当前目录来源不一致，未生成候选。");
    }
    return Status::Ok();
}
} // namespace

Status McpWorkspace::CaptureTerrainSources() {
    const auto environment = CheckEnvironmentUnchanged();
    if (!environment.Succeeded()) return environment;
    if (terrainState_) return Status::Ok();
    const auto captured = CaptureMapDataSources();
    if (!captured.Succeeded()) return captured;
    const auto& source = mapDataSource_->Snapshot();
    const auto terrainFile = FindSource(source, "war3map.w3e"), pathingFile = FindSource(source, "war3map.wpm");
    if (!terrainFile.Succeeded()) return {terrainFile.diagnostics};
    if (!pathingFile.Succeeded()) return {pathingFile.diagnostics};
    if (!*terrainFile.value) return Status::Failure("MCP", "MCP_TERRAIN_MISSING", "当前目录没有地形成员 war3map.w3e。");
    auto state = std::make_shared<McpTerrainWorkspaceState>();
    state->terrainFile = **terrainFile.value; state->pathingFile = *pathingFile.value;
    auto terrain = ReadSource(source, state->terrainFile);
    if (!terrain.Succeeded()) return {terrain.diagnostics};
    state->terrain = std::move(*terrain.value);
    if (state->pathingFile) {
        auto pathing = ReadSource(source, *state->pathingFile);
        if (!pathing.Succeeded()) return {pathing.diagnostics};
        state->pathing = std::move(*pathing.value);
    }
    auto committed = Replay(*initialMapData_, *state, {});
    if (!committed.Succeeded()) return {committed.diagnostics};
    std::shared_ptr<McpTerrainOverlay> active;
    if (activeEdit_) {
        auto created = Replay(*initialMapData_, *state, {});
        if (!created.Succeeded()) return {created.diagnostics};
        active = std::move(*created.value);
    }
    const auto final = CheckEnvironmentUnchanged();
    if (!final.Succeeded()) return final;
    state->committed = std::move(*committed.value);
    terrainState_ = std::move(state);
    if (activeEdit_) activeEdit_->terrain = std::move(active);
    return Status::Ok();
}

Result<McpWorkspaceTerrainContext> McpWorkspace::TerrainContext(std::string documentId, std::uint64_t generation) {
    const auto loaded = CaptureTerrainSources();
    if (!loaded.Succeeded()) return Failed<McpWorkspaceTerrainContext>(loaded);
    const auto& overlay = activeEdit_ ? activeEdit_->terrain : terrainState_->committed;
    if (!overlay) return Result<McpWorkspaceTerrainContext>::Failure("MCP", "MCP_TERRAIN_UNAVAILABLE", "当前事务没有地形编辑会话。");
    return Result<McpWorkspaceTerrainContext>::Success({&overlay->edits,
        {std::move(documentId), generation, toolEpoch_, Revision().sourceSha256}, activeEdit_.has_value()});
}

Status McpWorkspace::SeedTerrainOverlay(ActiveEdit& edit) {
    if (!terrainState_) return Status::Ok();
    auto replay = Replay(*initialMapData_, *terrainState_, terrainState_->committed->strokes);
    if (!replay.Succeeded()) return {replay.diagnostics};
    edit.terrain = std::move(*replay.value);
    if (edit.terrain->strokes.empty()) return Status::Ok();
    auto sealed = MapData::TerrainPatchCodec::Seal(*initialMapData_, terrainState_->terrain, terrainState_->pathing,
        mapDataSource_->Snapshot(), edit.terrain->edits, edit.terrain->strokes);
    if (!sealed.Succeeded()) return {sealed.diagnostics};
    const auto published = session_.ApplyTerrainPatch(edit.id, std::move(*sealed.value));
    if (published.Succeeded()) ++edit.generation;
    return published;
}

bool McpWorkspace::HasTerrainOverlay() const noexcept {
    return activeEdit_ && activeEdit_->terrain && !activeEdit_->terrain->strokes.empty();
}

Status McpWorkspace::CheckTerrainMixingAllowed() const {
    if (HasTerrainOverlay() || (!activeEdit_ && terrainState_ && !terrainState_->committed->strokes.empty()))
        return Status::Failure("MCP", "MCP_TERRAIN_MIXED_UNSUPPORTED",
            "独立地形目录事务暂不混入触发器、物编、预放或地图字段；请导出并从候选重新打开。");
    return Status::Ok();
}

Status McpWorkspace::ValidateTerrainOverlay() const {
    if (!HasTerrainOverlay()) return Status::Ok();
    const auto sealed = MapData::TerrainPatchCodec::Seal(*initialMapData_, terrainState_->terrain, terrainState_->pathing,
        mapDataSource_->Snapshot(), activeEdit_->terrain->edits, activeEdit_->terrain->strokes);
    if (!sealed.Succeeded()) return {sealed.diagnostics};
    const auto preview = session_.PreviewEdit(activeEdit_->id);
    if (!preview.Succeeded()) return {preview.diagnostics};
    return CheckOnlyTerrainFiles(*preview.value, *sealed.value);
}

void McpWorkspace::CommitTerrainOverlay() {
    if (terrainState_ && activeEdit_ && activeEdit_->terrain) terrainState_->committed = activeEdit_->terrain;
}

Result<MapData::TerrainStrokeResult> McpWorkspace::ApplyTerrainPlan(const Services::PreparedTerrainEdit& plan,
    const std::function<Status(const MapData::MapDataEditSession::EditReceipt&)>& prepublish) {
    using Receipt = MapData::TerrainStrokeResult;
    const auto checked = CheckActive(ActiveTransactionId(), Revision());
    if (!checked.Succeeded()) return Failed<Receipt>(checked);
    const auto loaded = CaptureTerrainSources();
    if (!loaded.Succeeded()) return Failed<Receipt>(loaded);
    const auto binding = Services::MapDataQueryBinding{plan.Binding().document.documentId,
        plan.Binding().document.generation, toolEpoch_, Revision().sourceSha256};
    if (!plan.Plan().HasChanges())
        return Services::ApplyTerrainEdit(activeEdit_->terrain->edits, binding, plan, prepublish);
    if (!terrainState_->pathingFile)
        return Result<Receipt>::Failure("MCP", "MCP_TERRAIN_PATHING_MISSING", "缺少原 WPM；可读取地形，不能证明目录保存的一致性。");
    const auto prior = session_.PreviewEdit(activeEdit_->id);
    if (!prior.Succeeded()) return Failed<Receipt>(prior);
    if (!HasTerrainOverlay() && !prior.value->changes.empty())
        return Result<Receipt>::Failure("MCP", "MCP_TERRAIN_MIXED_UNSUPPORTED", "现有事务包含其它领域变化，不能混入独立地形候选。");
    const auto priorCheck = ValidateTerrainOverlay();
    if (!priorCheck.Succeeded()) return Failed<Receipt>(priorCheck);
    auto overlay = activeEdit_->terrain;
    auto strokes = overlay->strokes;
    strokes.push_back({plan.Plan().Brush(), {plan.Plan().Points().begin(), plan.Plan().Points().end()}, plan.Plan().FollowDoodads()});
    const auto budget = StrokeBudget(strokes);
    if (!budget.Succeeded()) return Failed<Receipt>(budget);
    auto candidate = Replay(*initialMapData_, *terrainState_, strokes);
    if (!candidate.Succeeded()) return Failed<Receipt>(candidate);
    auto sealed = MapData::TerrainPatchCodec::Seal(*initialMapData_, terrainState_->terrain, terrainState_->pathing,
        mapDataSource_->Snapshot(), (*candidate.value)->edits, strokes);
    if (!sealed.Succeeded()) return Failed<Receipt>(sealed);
    if (toolEpoch_ == (std::numeric_limits<std::uint64_t>::max)()
        || activeEdit_->generation == (std::numeric_limits<std::uint64_t>::max)())
        return Result<Receipt>::Failure("MCP", "MCP_TERRAIN_REVISION_EXHAUSTED", "地形事务修订已用尽，请重新打开工作区。");
    const auto transaction = activeEdit_->id;
    const auto generation = activeEdit_->generation, epoch = toolEpoch_;
    const auto base = activeEdit_->baseRevision;
    publishing_ = true;
    struct Lease { bool& flag; ~Lease() { flag = false; } } lease{publishing_};
    const auto applied = Services::ApplyTerrainEdit(overlay->edits, binding, plan, [&](const auto& receipt) {
        if (prepublish) {
            const auto accepted = prepublish(receipt);
            if (!accepted.Succeeded()) return accepted;
        }
        if (!activeEdit_ || activeEdit_->id != transaction || activeEdit_->generation != generation
            || activeEdit_->terrain != overlay || activeEdit_->baseRevision != base || Revision() != base || toolEpoch_ != epoch
            || overlay->edits.Revision() != plan.Binding().terrainRevision
            || overlay->edits.TerrainSourceRevision() != plan.Binding().terrainSourceRevision)
            return Status::Failure("MCP", "TERRAIN_EDIT_STALE", "地形发布前活动事务或来源身份已变化。");
        const auto environment = CheckEnvironmentUnchanged();
        if (!environment.Succeeded()) return environment;
        const auto unchangedOverlay = session_.PreviewEdit(transaction);
        if (!unchangedOverlay.Succeeded()) return Status{unchangedOverlay.diagnostics};
        if (overlay->strokes.empty()) {
            if (!unchangedOverlay.value->changes.empty())
                return Status::Failure("MCP", "MCP_TERRAIN_OVERLAY_MISMATCH", "发布前回调改变了原目录覆盖。");
        } else {
            const auto unchanged = CheckOnlyTerrainFiles(*unchangedOverlay.value, *terrainState_, *overlay);
            if (!unchanged.Succeeded()) return unchanged;
        }
        // 最后的可失败写步骤：原 EditorSession 一次 swap 发布两成员，之后 Core 发布预分配命令。
        return session_.ApplyTerrainPatch(transaction, std::move(*sealed.value));
    });
    if (!applied.Succeeded()) return applied;
    overlay->strokes.swap(strokes);
    ++activeEdit_->generation; ++toolEpoch_;
    activeEdit_->previewedGeneration.reset(); activeEdit_->validatedGeneration.reset();
    return applied;
}

Result<Services::MapEditSaveReceipt> McpWorkspace::ExportTerrainArchiveCandidate(
    TransactionId transaction, const ProjectRevision& revision, Services::MapEditSaveRequest request) {
    using Receipt = Services::MapEditSaveReceipt;
    const auto overlay = activeEdit_->terrain;
    const auto generation = activeEdit_->generation;
    const auto guard = [this, overlay, generation, transaction, revision]() -> Status {
        if (!publishing_ || !activeEdit_ || activeEdit_->id != transaction || activeEdit_->baseRevision != revision
            || Revision() != revision || activeEdit_->generation != generation || activeEdit_->terrain != overlay
            || activeEdit_->previewedGeneration != generation || activeEdit_->validatedGeneration != generation)
            return Status::Failure("MCP", "MCP_ARCHIVE_PUBLISH_STALE", "地形归档发布身份已变化。");
        const auto environment = CheckEnvironmentUnchanged();
        if (!environment.Succeeded()) return environment;
        return ValidateTerrainOverlay();
    };
    const auto proven = guard();
    if (!proven.Succeeded()) return Failed<Receipt>(proven);
    // W3E 相同不代表 WPM 或装饰物跟随上下文相同；按精确名字核对同源成员，包括已证缺失。
    const auto source = MatchArchiveSource(request, mapDataSource_->Snapshot(), *initialMapData_);
    if (!source.Succeeded()) return Failed<Receipt>(source);
    request.expectedTerrainSha256 = terrainState_->terrainFile.sha256;
    for (const auto& stroke : overlay->strokes)
        request.terrainStrokes.push_back({stroke.brush, stroke.points, Services::MapEditPointSpace::World, stroke.followDoodads});
    request.beforePublish = guard;
    return Services::SaveMapEditCandidate(request);
}

} // namespace War3Map::Mcp
