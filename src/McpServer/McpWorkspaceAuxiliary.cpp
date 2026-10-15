#include "McpWorkspace.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/IO/FileBytes.h"

#include <algorithm>
#include <limits>
#include <set>

namespace War3Map::Mcp {
namespace {
template<class T, class U> Result<T> Failed(const U& source) { return {{}, source.diagnostics}; }
}

Services::AuxiliaryReferenceContext McpWorkspace::AuxiliaryReferences() const {
    Services::AuxiliaryReferenceContext refs{&VisibleDocument(), true, {}};
    if (mapDataSource_) for (const auto& file : mapDataSource_->Snapshot().files)
        Services::ObserveAuxiliaryScriptSource(refs, file.relativePath.generic_string());
    else refs.opaqueScriptSources.push_back("地图数据来源尚未捕获");
    return refs;
}

Result<MapData::MapDataEditSession::EditReceipt> McpWorkspace::ApplyAuxiliaryPlan(
    const Services::PreparedAuxiliaryEdit& plan, const MapData::MapDataPublishValidator& prepublish) {
    using Receipt = MapData::MapDataEditSession::EditReceipt;
    const auto active = CheckActive(ActiveTransactionId(), Revision());
    if (!active.Succeeded()) return Failed<Receipt>(active);
    const auto terrain = CheckTerrainMixingAllowed();
    if (!terrain.Succeeded()) return Failed<Receipt>(terrain);
    const auto captured = CaptureMapDataSources();
    if (!captured.Succeeded()) return Failed<Receipt>(captured);
    if (activeEdit_->placementIntent)
        return Result<Receipt>::Failure("MCP", "MCP_AUXILIARY_MIXED_UNSUPPORTED", "区域/镜头结构命令暂不混入独立预放事务。");
    if (!activeEdit_->auxiliary) {
        const auto fields = MapData::FreezeOriginalMapDataFields(*initialMapData_, *activeEdit_->mapData);
        if (!fields.Succeeded()) return Failed<Receipt>(fields);
        if (!fields.value->Operations().empty())
            return Result<Receipt>::Failure("MCP", "MCP_AUXILIARY_MIXED_UNSUPPORTED", "区域/镜头结构命令暂不混入字段事务，请使用独立事务。");
    }
    const auto before = activeEdit_->mapData;
    const auto generation = activeEdit_->generation, epoch = toolEpoch_;
    const auto id = activeEdit_->id; const auto revision = activeEdit_->baseRevision;
    if (!Services::AuxiliaryEditing::AreEquivalentMapSources(*before, plan.Before()))
        return Result<Receipt>::Failure("MCP", "AUX_PLAN_STALE", "区域/镜头计划不属于当前工作区文档。");
    MapData::MapDataEditSession candidate(*before);
    // 在隔离候选上执行原服务和回执预检，发布前再核对同一事务。
    const auto applied = Services::AuxiliaryEditing::Apply(candidate, plan, AuxiliaryReferences(), prepublish);
    if (!applied.Succeeded()) return Failed<Receipt>(applied);
    const auto afterCallback = CheckActive(id, revision);
    if (!afterCallback.Succeeded()) return Failed<Receipt>(afterCallback);
    if (activeEdit_->generation != generation || toolEpoch_ != epoch || activeEdit_->mapData != before)
        return Result<Receipt>::Failure("MCP", "AUX_PLAN_STALE", "区域/镜头回执预检期间原事务已变化。");
    if (!plan.HasChanges()) return Result<Receipt>::Success({});
    if (generation == (std::numeric_limits<std::uint64_t>::max)() || epoch == (std::numeric_limits<std::uint64_t>::max)())
        return Result<Receipt>::Failure("MCP", "MCP_AUXILIARY_REVISION_EXHAUSTED", "区域/镜头事务修订已用尽，请重新打开。");
    std::vector<Services::AuxiliaryEditRequest> commands;
    if (activeEdit_->auxiliary) commands.assign(activeEdit_->auxiliary->Commands().begin(), activeEdit_->auxiliary->Commands().end());
    commands.push_back(plan.Request());
    auto frozen = Services::AuxiliaryTransactionSnapshot::Freeze(*initialMapData_, candidate.Snapshot(), commands, AuxiliaryReferences());
    if (!frozen.Succeeded()) return Failed<Receipt>(frozen);
    auto patch = (*frozen.value)->Seal(mapDataSource_->Snapshot());
    if (!patch.Succeeded()) return Failed<Receipt>(patch);
    const auto guard = CheckActive(id, revision);
    if (!guard.Succeeded()) return Failed<Receipt>(guard);
    if (activeEdit_->generation != generation || toolEpoch_ != epoch || activeEdit_->mapData != before)
        return Result<Receipt>::Failure("MCP", "AUX_PLAN_STALE", "区域/镜头发布前的事务已变化。");
    const auto published = session_.ApplyAuxiliaryPatch(id, std::move(*patch.value));
    if (!published.Succeeded()) return Failed<Receipt>(published);
    activeEdit_->auxiliary = std::move(*frozen.value);
    activeEdit_->mapData = plan.OwnedAfter();
    ++activeEdit_->generation; ++toolEpoch_;
    activeEdit_->previewedGeneration.reset(); activeEdit_->validatedGeneration.reset();
    projectSearchMapCache_.reset(); mapObjectReferences_.reset();
    return Result<Receipt>::Success(candidate.LastEdit());
}

Status McpWorkspace::ValidateAuxiliaryOverlay() const {
    const auto& snapshot = activeEdit_->auxiliary;
    if (!Services::AuxiliaryEditing::AreEquivalentMapSources(snapshot->Document(), *activeEdit_->mapData))
        return Status::Failure("MCP", "AUX_TRANSACTION_REPLAY_MISMATCH", "当前区域/镜头文档与封印不同。");
    const auto verified = snapshot->VerifyAgainst(*initialMapData_, AuxiliaryReferences());
    if (!verified.Succeeded()) return verified;
    const auto preview = session_.PreviewEdit(activeEdit_->id);
    if (!preview.Succeeded()) return {preview.diagnostics};
    std::set<std::string> seen;
    for (const auto& change : preview.value->changes) {
        const auto member = std::find_if(snapshot->Members().begin(), snapshot->Members().end(), [&](const auto& value) {
            return change.path == value.name || change.path == std::filesystem::path("map") / value.name;
        });
        if (change.kind != "map-auxiliary" || member == snapshot->Members().end() || !seen.insert(member->name).second)
            return Status::Failure("MCP", "MCP_AUXILIARY_OVERLAY_MISMATCH", "区域/镜头覆盖中存在未冻结成员或其它领域变化。");
        const auto kind = member->name == "war3map.w3r" ? MapDataKind::Regions : MapDataKind::Cameras;
        const auto before = Services::AuxiliaryEditing::MemberIdentity(*initialMapData_, kind);
        const auto after = Sha256(member->data);
        if (!before.Succeeded() || !after.Succeeded() || change.afterSha256 != *after.value
            || change.beforeSha256 != (*before.value == "missing" ? std::string{} : *before.value))
            return Status::Failure("MCP", "MCP_AUXILIARY_OVERLAY_MISMATCH", "区域/镜头覆盖 SHA 与冻结候选不同。");
    }
    if (seen.size() != snapshot->Members().size())
        return Status::Failure("MCP", "MCP_AUXILIARY_OVERLAY_MISMATCH", "区域/镜头冻结候选缺少原事务覆盖。");
    return Status::Ok();
}

Result<Services::MapEditSaveReceipt> McpWorkspace::ExportAuxiliaryArchiveCandidate(
    TransactionId transactionId, const ProjectRevision& expectedRevision, Services::MapEditSaveRequest request) {
    using Receipt = Services::MapEditSaveReceipt;
    const auto snapshot = activeEdit_->auxiliary;
    if (!snapshot->HasChanges())
        return Result<Receipt>::Failure("MCP", "AUX_TRANSACTION_NO_CHANGE", "区域/镜头事务已恢复原值，不生成空归档候选。");
    const auto encoded = codec_.Serialize(activeEdit_->document, catalog_);
    if (!encoded.Succeeded()) return Failed<Receipt>(encoded);
    const auto wtg = IO::ReadFileBytes(projectRoot_ / wtgRelativePath_, Trigger::TriggerTransactionLimits{}.maxMemberBytes);
    const auto wct = IO::ReadFileBytes(projectRoot_ / wctRelativePath_, Trigger::TriggerTransactionLimits{}.maxMemberBytes);
    if (!wtg.Succeeded()) return Failed<Receipt>(wtg);
    if (!wct.Succeeded()) return Failed<Receipt>(wct);
    const auto objects = FreezeObjectTransactions(request.sourceMap, request.expectedSourceSha256);
    if (!objects.Succeeded()) return Failed<Receipt>(objects);
    if (encoded.value->wtg != *wtg.value || encoded.value->wct != *wct.value || !objects.value->empty())
        return Result<Receipt>::Failure("MCP", "MCP_AUXILIARY_MIXED_UNSUPPORTED", "区域/镜头归档候选暂不混入触发器或物编变化。");
    const auto document = activeEdit_->mapData;
    const auto generation = activeEdit_->generation;
    const auto guard = [this, transactionId, expectedRevision, generation, snapshot, document]() -> Status {
        if (!publishing_ || !activeEdit_ || activeEdit_->id != transactionId || session_.Revision() != expectedRevision
            || activeEdit_->baseRevision != expectedRevision || activeEdit_->generation != generation
            || activeEdit_->previewedGeneration != generation || activeEdit_->validatedGeneration != generation
            || activeEdit_->auxiliary != snapshot || activeEdit_->mapData != document)
            return Status::Failure("MCP", "MCP_ARCHIVE_PUBLISH_STALE", "区域/镜头归档发布身份已变化。");
        const auto environment = CheckEnvironmentUnchanged();
        if (!environment.Succeeded()) return environment;
        return ValidateAuxiliaryOverlay();
    };
    const auto checked = guard();
    if (!checked.Succeeded()) return Failed<Receipt>(checked);
    request.auxiliaryTransaction = snapshot;
    request.beforePublish = guard;
    return Services::SaveMapEditCandidate(request);
}

} // namespace War3Map::Mcp
