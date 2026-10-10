#include "McpWorkspace.h"
#include "War3MapCore/MapData/MapDataFieldPatchCodec.h"
#include "War3MapCore/MapData/MapDataPlacementPatchCodec.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/IO/FileBytes.h"

#include <algorithm>
#include <set>

namespace War3Map::Mcp {
namespace {
template<class T, class U> Result<T> Failed(const U& source) { return {{}, source.diagnostics}; }
std::vector<MapDataKind> ChangedKinds(const MapDocument& original, const MapData::MapDataFieldFreeze& frozen) {
    std::vector<MapDataKind> kinds;
    for (const auto& member : frozen.Members())
        for (const auto kind : {MapDataKind::Settings, MapDataKind::Strings,
            MapDataKind::Units, MapDataKind::Regions, MapDataKind::Cameras, MapDataKind::Sounds})
            if (original.Member(kind).memberName == member.name) kinds.push_back(kind);
    return kinds;
}
}

Status McpWorkspace::CaptureMapDataSources() {
    if (mapDataSource_) return Status::Ok();
    const auto checked = CheckEnvironmentUnchanged();
    if (!checked.Succeeded()) return checked;
    auto source = DirectoryWorkspace::Open(projectRoot_);
    if (!source.Succeeded()) return {source.diagnostics};
    if (source.value->Snapshot().aggregateSha256 != Revision().sourceSha256)
        return Status::Failure("MCP", "SOURCE_CHANGED", "地图数据目录快照与已打开工作区不一致。");
    auto document = MapDocument::FromDirectory(*source.value);
    if (!document.Succeeded()) return {document.diagnostics};
    auto owned = std::make_shared<const MapDocument>(std::move(*document.value));
    auto directory = std::make_unique<DirectoryWorkspace>(std::move(*source.value));
    const auto finalCheck = CheckEnvironmentUnchanged();
    if (!finalCheck.Succeeded()) return finalCheck;
    initialMapData_ = committedMapData_ = std::move(owned);
    projectSearchMapCache_.reset();
    mapDataSource_ = std::move(directory);
    if (activeEdit_) activeEdit_->mapData = committedMapData_;
    return Status::Ok();
}

Result<McpWorkspaceMapDataContext> McpWorkspace::MapDataContext(std::string documentId,
    std::uint64_t documentGeneration) {
    const auto checked = CaptureMapDataSources();
    if (!checked.Succeeded()) return Failed<McpWorkspaceMapDataContext>(checked);
    const auto environment = CheckEnvironmentUnchanged();
    if (!environment.Succeeded()) return Failed<McpWorkspaceMapDataContext>(environment);
    McpWorkspaceMapDataContext context;
    context.original = initialMapData_;
    context.document = activeEdit_ ? activeEdit_->mapData : committedMapData_;
    context.binding = {std::move(documentId), documentGeneration, toolEpoch_, Revision().sourceSha256};
    context.includesActiveTransactionOverlay = activeEdit_.has_value();
    context.preparedSnapshot = PreparedProjectSearchMapCache(context.binding);
    return Result<McpWorkspaceMapDataContext>::Success(std::move(context));
}

Status McpWorkspace::SeedMapDataOverlay(ActiveEdit& edit) {
    if (!initialMapData_) return Status::Ok();
    edit.mapData = committedMapData_;
    edit.placementIntent = committedPlacementIntent_;
    if (edit.placementIntent) {
        auto patch = MapData::MapDataPlacementPatchCodec::Seal(*initialMapData_, *edit.mapData,
            mapDataSource_->Snapshot(), *edit.placementIntent);
        if (!patch.Succeeded()) return {patch.diagnostics};
        const auto applied = session_.ApplyMapDataPatch(edit.id, std::move(*patch.value));
        if (applied.Succeeded()) ++edit.generation;
        return applied;
    }
    const auto frozen = MapData::FreezeOriginalMapDataFields(*initialMapData_, *edit.mapData);
    if (!frozen.Succeeded()) return {frozen.diagnostics};
    if (frozen.value->Members().empty()) return Status::Ok();
    const auto kinds = ChangedKinds(*initialMapData_, *frozen.value);
    auto patch = MapData::MapDataFieldPatchCodec::Seal(*initialMapData_, *edit.mapData,
        mapDataSource_->Snapshot(), kinds);
    if (!patch.Succeeded()) return {patch.diagnostics};
    const auto applied = session_.ApplyMapDataPatch(edit.id, std::move(*patch.value));
    if (applied.Succeeded()) ++edit.generation;
    return applied;
}

Result<MapData::MapDataEditSession::EditReceipt> McpWorkspace::ApplyMapDataPlan(
    const MapData::PreparedMapDataFields& plan, const MapData::MapDataPublishValidator& prepublish) {
    using Receipt = MapData::MapDataEditSession::EditReceipt;
    const auto checked = CheckActive(ActiveTransactionId(), Revision());
    if (!checked.Succeeded()) return Failed<Receipt>(checked);
    const auto captured = CaptureMapDataSources();
    if (!captured.Succeeded()) return Failed<Receipt>(captured);
    if (activeEdit_->placementIntent)
        return Result<Receipt>::Failure("MCP", "MCP_PLACEMENT_MIXED_FIELDS_UNSUPPORTED",
            "独立预放事务暂不混入字段变化；请使用独立事务。");
    const auto beforeDocument = activeEdit_->mapData;
    const auto beforeGeneration = activeEdit_->generation;
    const auto transactionId = activeEdit_->id;
    const auto baseRevision = activeEdit_->baseRevision;
    // 先在隔离 typed 会话完成完整预算、累计原值冻结和回执预检；原 overlay 尚未发布。
    const auto prior = MapData::FreezeOriginalMapDataFields(*initialMapData_, *activeEdit_->mapData);
    if (!prior.Succeeded()) return Failed<Receipt>(prior);
    const auto final = MapData::FreezeOriginalMapDataFields(*initialMapData_, plan.After());
    if (!final.Succeeded()) return Failed<Receipt>(final);
    MapData::MapDataEditSession candidate(*initialMapData_);
    for (const auto& member : prior.value->Members()) {
        const auto kind = std::find_if(initialMapData_->Members().begin(), initialMapData_->Members().end(),
            [&](const auto& input) { return input.memberName == member.name; });
        if (kind == initialMapData_->Members().end())
            return Result<Receipt>::Failure("MCP", "MAP_FIELD_PATCH_SCOPE", "累计字段成员不属于原文档。");
        const auto seeded = candidate.SetSerialized(kind->kind, member.data);
        if (!seeded.Succeeded()) return Failed<Receipt>(seeded);
    }
    const auto applied = candidate.ApplyPreparedFields(plan, prepublish);
    if (!applied.Succeeded()) return Failed<Receipt>(applied);
    const auto receipt = candidate.LastEdit();
    auto next = std::make_shared<const MapDocument>(plan.After());
    const std::array kinds{plan.Kind()};
    auto patch = MapData::MapDataFieldPatchCodec::Seal(*initialMapData_, *next, mapDataSource_->Snapshot(), kinds);
    if (!patch.Succeeded()) return Failed<Receipt>(patch);
    const auto finalGuard = CheckActive(ActiveTransactionId(), Revision());
    if (!finalGuard.Succeeded()) return Failed<Receipt>(finalGuard);
    if (activeEdit_->id != transactionId || activeEdit_->baseRevision != baseRevision
        || activeEdit_->generation != beforeGeneration || activeEdit_->mapData != beforeDocument)
        return Result<Receipt>::Failure("MCP", "MAP_FIELD_PLAN_STALE", "字段发布前的原事务已变化。");
    const auto published = session_.ApplyMapDataPatch(activeEdit_->id, std::move(*patch.value));
    if (!published.Succeeded()) return Failed<Receipt>(published);
    activeEdit_->mapData = std::move(next);
    projectSearchMapCache_.reset();
    ++activeEdit_->generation;
    ++toolEpoch_;
    activeEdit_->previewedGeneration.reset();
    activeEdit_->validatedGeneration.reset();
    mapObjectReferences_.reset();
    return Result<Receipt>::Success(receipt);
}

Result<Services::MapEditSaveReceipt> McpWorkspace::ExportMapDataArchiveCandidate(
    TransactionId transactionId, const ProjectRevision& expectedRevision, Services::MapEditSaveRequest request) {
    using Receipt = Services::MapEditSaveReceipt;
    const auto frozen = MapData::FreezeOriginalMapDataFields(*initialMapData_, *activeEdit_->mapData);
    if (!frozen.Succeeded()) return Failed<Receipt>(frozen);
    if (frozen.value->Operations().empty())
        return Result<Receipt>::Failure("MCP", "MAP_FIELD_NO_CHANGE", "当前事务没有相对原来源的地图字段变化。");
    auto triggers = codec_.Serialize(activeEdit_->document, catalog_);
    if (!triggers.Succeeded()) return Failed<Receipt>(triggers);
    const auto originalWtg = IO::ReadFileBytes(projectRoot_ / wtgRelativePath_, Trigger::TriggerTransactionLimits{}.maxMemberBytes);
    const auto originalWct = IO::ReadFileBytes(projectRoot_ / wctRelativePath_, Trigger::TriggerTransactionLimits{}.maxMemberBytes);
    if (!originalWtg.Succeeded()) return Failed<Receipt>(originalWtg);
    if (!originalWct.Succeeded()) return Failed<Receipt>(originalWct);
    const auto objects = FreezeObjectTransactions(request.sourceMap, request.expectedSourceSha256);
    if (!objects.Succeeded()) return Failed<Receipt>(objects);
    if (triggers.value->wtg != *originalWtg.value || triggers.value->wct != *originalWct.value || !objects.value->empty())
        return Result<Receipt>::Failure("MCP", "MAP_FIELD_MIXED_SAVE_UNSUPPORTED",
            "C1 字段归档候选暂不混入触发器或物编变化；请使用独立事务。");
    for (const auto& operation : frozen.value->Operations())
        request.fieldEdits.push_back({operation.expectedMemberSha256, operation.edit});
    const auto document = activeEdit_->mapData;
    const auto generation = activeEdit_->generation;
    const auto guard = [this, transactionId, expectedRevision, generation, document]() -> Status {
        if (!publishing_ || !activeEdit_ || activeEdit_->id != transactionId
            || activeEdit_->baseRevision != expectedRevision || session_.Revision() != expectedRevision
            || activeEdit_->generation != generation || activeEdit_->mapData != document
            || activeEdit_->previewedGeneration != generation || activeEdit_->validatedGeneration != generation)
            return Status::Failure("MCP", "MCP_ARCHIVE_PUBLISH_STALE", "字段归档发布身份已变化。");
        const auto environment = CheckEnvironmentUnchanged();
        if (!environment.Succeeded()) return environment;
        const auto freeze = MapData::FreezeOriginalMapDataFields(*initialMapData_, *document);
        if (!freeze.Succeeded()) return {freeze.diagnostics};
        const auto preview = session_.PreviewEdit(transactionId);
        if (!preview.Succeeded()) return {preview.diagnostics};
        std::set<std::string> matched;
        for (const auto& change : preview.value->changes) {
            // 显式恢复某成员原值仍留在原 overlay；仅允许完整原 SHA 的同成员空差异。
            if (change.kind == "map-data" && change.beforeSha256 == change.afterSha256) {
                const auto original = std::find_if(initialMapData_->Members().begin(), initialMapData_->Members().end(),
                    [&](const auto& input) {
                        return change.path == input.memberName || change.path == std::filesystem::path("map") / input.memberName;
                    });
                if (original != initialMapData_->Members().end() && original->sources.size() == 1) {
                    const auto sha = Sha256(original->sources.front().data);
                    if (sha.Succeeded() && change.beforeSha256 == *sha.value) continue;
                }
            }
            const auto member = std::find_if(freeze.value->Members().begin(), freeze.value->Members().end(),
                [&](const auto& input) {
                    return change.path == input.name || change.path == std::filesystem::path("map") / input.name;
                });
            if (change.kind != "map-data" || member == freeze.value->Members().end()
                || !matched.insert(member->name).second)
                return Status::Failure("MCP", "MCP_ARCHIVE_OVERLAY_MISMATCH", "字段候选覆盖中存在未冻结成员。");
            const auto after = Sha256(member->data);
            const auto original = std::find_if(initialMapData_->Members().begin(), initialMapData_->Members().end(),
                [&](const auto& input) { return input.memberName == member->name; });
            if (!after.Succeeded() || original == initialMapData_->Members().end() || original->sources.size() != 1)
                return Status::Failure("MCP", "MCP_ARCHIVE_OVERLAY_MISMATCH", "无法核对字段候选成员身份。");
            const auto before = Sha256(original->sources.front().data);
            if (!before.Succeeded() || change.beforeSha256 != *before.value || change.afterSha256 != *after.value)
                return Status::Failure("MCP", "MCP_ARCHIVE_OVERLAY_MISMATCH", "字段候选前后 SHA 与冻结内容不相等。");
        }
        if (matched.size() != freeze.value->Members().size())
            return Status::Failure("MCP", "MCP_ARCHIVE_OVERLAY_MISMATCH", "已改变字段缺少原事务覆盖。");
        return Status::Ok();
    };
    const auto proven = guard();
    if (!proven.Succeeded()) return Failed<Receipt>(proven);
    request.beforePublish = guard;
    return Services::SaveMapEditCandidate(request);
}

} // namespace War3Map::Mcp
