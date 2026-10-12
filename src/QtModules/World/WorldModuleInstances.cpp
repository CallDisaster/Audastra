#include "WorldModule.h"
#include "WorldAnimationPlayer.h"
#include "WorldPlacementModels.h"
#include "WorldPlacementPreview.h"
#include "WorldSelectionPresenter.h"
#include "WorldPathingHelperPresenter.h"
#include "WorldCliffLayer.h"
#include "EditorProject.h"
#include "Widgets/ElidedLabel.h"
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <algorithm>
#include <unordered_map>
#include <utility>

namespace War3Map::Modules {
namespace {
std::optional<WorldPlacementCapture> CaptureUnitTail(const QtProject::EditorProject& project,
    const MapDocument& document, const MapData::MapDataEditSession::EditReceipt& receipt,
    const Services::WorldInstanceRegistry& previous, std::span<const Render::ModelInstanceRequest> requests,
    std::span<const MapData::PlacementId> ids, std::span<const WorldPlacementPresentationCandidate> presentations) {
    const auto& units = document.Units();
    const auto lineage = receipt.PlacementLineage();
    if (!units || units->version != 8 || units->subversion != 11
        || units->recordLayout != MapData::PlacementRecordLayout::Classic
        || document.Member(MapDataKind::Units).state != MapDataState::Ready
        || lineage.size() != 1 || lineage.front().kind != MapDataKind::Units || requests.size() != ids.size()) return std::nullopt;
    const auto& map = lineage.front();
    const auto oldCount = receipt.Forward() ? map.beforeCount : map.afterCount;
    const auto newCount = receipt.Forward() ? map.afterCount : map.beforeCount;
    if (oldCount == newCount || previous.Placements(MapData::PlacementFile::Units).size() != oldCount
        || units->units.size() != newCount || map.beforeToAfter.size() != map.beforeCount) return std::nullopt;
    const auto surviving = std::min(map.beforeCount, map.afterCount);
    for (std::uint32_t i = 0; i < map.beforeCount; ++i)
        if (i < surviving ? map.beforeToAfter[i] != std::optional(i) : map.beforeToAfter[i].has_value()) return std::nullopt;
    const auto before = receipt.PlacementMemberBytes(MapDataKind::Units, false);
    const auto after = receipt.PlacementMemberBytes(MapDataKind::Units, true);
    if (!before || !after || before->size() < 16 || after->size() < 16
        || (newCount > oldCount ? after->size() <= before->size() : after->size() >= before->size())) return std::nullopt;
    const auto word = [](std::span<const std::byte> bytes, std::size_t at) {
        std::uint32_t value = 0;
        for (unsigned i = 0; i < 4; ++i) value |= std::to_integer<std::uint32_t>(bytes[at + i]) << (8 * i);
        return value;
    };
    if (word(*before, 0) != 0x6f643357u || word(*before, 4) != 8 || word(*before, 8) != 11
        || word(*before, 12) != oldCount || word(*after, 12) != newCount
        || !std::equal(before->begin(), before->begin() + 12, after->begin())) return std::nullopt;
    // 仅限实际解析为Classic/Ready的单位表；该解析器要求读到EOF，没有装饰物special尾块。
    // 除count外全部存活记录原字节须一致，未知扩展/皮肤/中间删除或任何旧行改动均走原捕获。
    const auto commonBytes = std::min(before->size(), after->size());
    if (!std::equal(before->begin() + 16, before->begin() + commonBytes, after->begin() + 16)) return std::nullopt;
    std::vector<MapData::PlacementId> added;
    for (std::uint32_t i = oldCount; i < newCount; ++i)
        added.push_back(MapData::MakePlacementId(MapData::PlacementFile::Units, i, units->units[i]));
    auto captured = CapturePlacementRows(project, document, added);
    if (!captured.Succeeded()) return std::nullopt;
    WorldPlacementCapture merged;
    merged.requests.reserve(requests.size() + captured.value->requests.size());
    merged.ids.reserve(ids.size() + captured.value->ids.size());
    std::vector<std::uint32_t> remap(requests.size(), UINT32_MAX);
    for (std::size_t i = 0; i < ids.size(); ++i) {
        if (ids[i].file == MapData::PlacementFile::Units && ids[i].index >= newCount) continue;
        remap[i] = static_cast<std::uint32_t>(merged.requests.size());
        merged.requests.push_back(requests[i]); merged.ids.push_back(ids[i]);
    }
    for (auto presentation : presentations) {
        if (presentation.requestIndex >= remap.size() || remap[presentation.requestIndex] == UINT32_MAX) continue;
        presentation.requestIndex = remap[presentation.requestIndex]; merged.presentations.push_back(std::move(presentation));
    }
    const auto base = static_cast<std::uint32_t>(merged.requests.size());
    for (auto& request : captured.value->requests) merged.requests.push_back(std::move(request));
    for (auto& id : captured.value->ids) merged.ids.push_back(std::move(id));
    for (auto& presentation : captured.value->presentations) {
        presentation.requestIndex += base; merged.presentations.push_back(std::move(presentation));
    }
    return merged;
}
}
std::optional<Services::WorldInstanceBinding> WorldModule::CurrentPlacementBinding() const {
    if (!project_.mapEdits_ || !project_.archiveSession_) return std::nullopt;
    const auto& sha = project_.archiveSession_->SourceSha256();
    Services::WorldInstanceBinding binding;
    const auto registry = stagedInstanceRegistry_ ? stagedInstanceRegistry_ : instanceRegistry_;
    if (registry && registry->Binding().editSessionIdentity == project_.mapEdits_->SessionIdentity()
        && registry->Binding().sourceIdentity == sha) binding = registry->Binding();
    else binding = {generation_, 0, project_.mapEdits_->SessionIdentity(), 0, sha};
    binding.mapRevision = binding.editRevision = project_.mapEdits_->Revision();
    return binding;
}
void WorldModule::RememberCreatedPlacements(std::span<const MapData::PlacementId> rows,
    std::span<const Render::ModelInstanceRequest> requests) {
    placementTransactionCompletedNs_ = WorldInstanceNowNs();
    const auto binding = CurrentPlacementBinding();
    if (!binding) return;
    // 此私有入口只由AddUnit/AddDoodad/CopyPlacements刚返回的正式新行调用。
    // 同一lastApplied回执先绑定创建身份，注册器之后补充真实render identity。
    const auto status = pendingPlacements_.RememberCreated(*binding, project_.mapEdits_->LastAppliedEdit(), rows, requests);
    if (!status.Succeeded()) project_.AddDiagnostics(status.diagnostics);
    else pendingPlacements_.RecordCreatedTiming(project_.mapEdits_->LastAppliedEdit().CommandIdentity(),
        placementInputStartedNs_, placementTransactionCompletedNs_);
    UpdateOverlay(); // 在后续整表registry准备前，渲染线程即可画出真正新增行的占位。
}
bool WorldModule::StagePlacementInstances(const MapDocument& document, const WorldPlacementRefreshTimings& refresh) {
    if (!animation_ || !instanceRegistry_ || !stagedInstanceRegistry_ || !instanceRevisionGate_
        || !project_.mapEdits_ || !viewport_->HasInstanceRosterSupport()) return false;
    const auto revision = project_.mapEdits_->Revision();
    if (stagedInstanceRegistry_->Binding().editRevision == revision) {
        if (!pendingPlacements_.Entries().empty()) {
            const auto remap = pendingPlacements_.Reconcile(stagedInstanceRegistry_, &document);
            if (!remap.Succeeded()) project_.AddDiagnostics(remap.diagnostics);
        }
        UpdateOverlay(); QueuePlacementInstances(); return true;
    }
    stagedInstanceTiming_ = {};
    stagedInstanceTiming_.refresh = refresh;
    stagedInstanceTiming_.generation = generation_;
    stagedInstanceTiming_.editSessionIdentity = project_.mapEdits_->SessionIdentity();
    stagedInstanceTiming_.documentRevision = revision;
    stagedInstanceTiming_.commandIdentity = project_.mapEdits_->LastAppliedEdit().CommandIdentity();
    stagedInstanceTiming_.inputStartedNs = std::exchange(placementInputStartedNs_, 0);
    stagedInstanceTiming_.transactionCompletedNs = std::exchange(placementTransactionCompletedNs_, 0);
    stagedInstanceTiming_.captureStartedNs = WorldInstanceNowNs();
    const auto& receipt = project_.mapEdits_->LastAppliedEdit();
    const auto& binding = stagedInstanceRegistry_->Binding();
    const auto activeScene = animation_->Scene();
    std::optional<MapData::PlacementFile> changedFile;
    // Refresh()在物编/目录变化时递增selectionCatalogRevision并清空两个registry。
    // 这里还复核实际来源层序指纹；不能把同一路径或同一个rawcode当作复用证据。
    // sourceGeneration是渲染场景的编号空间，不可与WorldModule::generation_混用。
    if (activeScene && pathingHelperCache_ && project_.archiveSession_
        && stagedRequests_.size() == stagedPlacementIds_.size()
        && stagedRequests_.size() == stagedInstanceRegistry_->CapturedSpecs().size()
        && binding.sourceGeneration == activeScene->generation && binding.sourceIdentity == activeScene->sourceIdentity
        && binding.sourceIdentity == project_.archiveSession_->SourceSha256()
        && binding.editSessionIdentity == receipt.SessionIdentity()
        && binding.editSessionIdentity == project_.mapEdits_->SessionIdentity()
        && receipt.BeforeRevision() == binding.editRevision && receipt.AfterRevision() == revision
        && revision > binding.editRevision && revision - binding.editRevision == 1
        && receipt.ChangesPlacements() && receipt.HasPlacementLineage()) {
        const auto unitBefore = receipt.PlacementMemberBytes(MapDataKind::Units, false);
        const auto unitAfter = receipt.PlacementMemberBytes(MapDataKind::Units, true);
        const auto doodadBefore = receipt.PlacementMemberBytes(MapDataKind::Doodads, false);
        const auto doodadAfter = receipt.PlacementMemberBytes(MapDataKind::Doodads, true);
        if (unitBefore.has_value() == unitAfter.has_value() && doodadBefore.has_value() == doodadAfter.has_value()
            && unitBefore.has_value() != doodadBefore.has_value()) {
            const auto source = CaptureWorldTerrainSources(project_);
            if (source.Succeeded()) {
                const auto identity = WorldPathingHelperResourceIdentity(*source.value, PlacementPresentationVersion().catalogRevision);
                if (identity.Succeeded() && *identity.value == pathingHelperCache_->resourceIdentity)
                    changedFile = unitBefore ? MapData::PlacementFile::Units : MapData::PlacementFile::Doodads;
            }
        }
    }
    auto tail = changedFile == MapData::PlacementFile::Units
        ? CaptureUnitTail(project_, document, receipt, *stagedInstanceRegistry_, stagedRequests_, stagedPlacementIds_, stagedPresentations_)
        : std::nullopt;
    auto capture = tail ? std::move(*tail) : CaptureWorldPlacements(project_, document, changedFile);
    if (changedFile && !tail) {
        WorldPlacementCapture merged;
        merged.requests.reserve(stagedRequests_.size() + capture.requests.size());
        merged.ids.reserve(stagedPlacementIds_.size() + capture.ids.size());
        merged.presentations.reserve(stagedPresentations_.size() + capture.presentations.size());
        std::vector<std::uint32_t> retainedRemap(stagedRequests_.size(), UINT32_MAX);
        for (const auto file : {MapData::PlacementFile::Doodads, MapData::PlacementFile::Units}) {
            if (file == *changedFile) {
                const auto base = static_cast<std::uint32_t>(merged.requests.size());
                for (auto& request : capture.requests) merged.requests.push_back(std::move(request));
                for (auto& id : capture.ids) merged.ids.push_back(std::move(id));
                for (auto& presentation : capture.presentations) {
                    presentation.requestIndex += base; merged.presentations.push_back(std::move(presentation));
                }
            } else for (std::size_t index = 0; index < stagedPlacementIds_.size(); ++index) {
                if (stagedPlacementIds_[index].file != file) continue;
                retainedRemap[index] = static_cast<std::uint32_t>(merged.requests.size());
                merged.requests.push_back(stagedRequests_[index]); merged.ids.push_back(stagedPlacementIds_[index]);
            }
        }
        for (auto presentation : stagedPresentations_)
            if (presentation.requestIndex < retainedRemap.size() && retainedRemap[presentation.requestIndex] != UINT32_MAX) {
                presentation.requestIndex = retainedRemap[presentation.requestIndex]; merged.presentations.push_back(std::move(presentation));
            }
        capture.requests = std::move(merged.requests); capture.ids = std::move(merged.ids);
        capture.presentations = std::move(merged.presentations);
        // 这里只生成候选数组，不将receipt的“未修改”声明当作SHA证明。下方原Prepare
        // 必须完整核验两个成员的前后SHA和真实谱系，失败时不会发布复用的请求。
    }
    stagedInstanceTiming_.captureNs = WorldInstanceNowNs() - stagedInstanceTiming_.captureStartedNs;
    Render::AnimationInstanceSpec defaults; defaults.globalMs = 1000;
    const auto registryStarted = WorldInstanceNowNs();
    auto change = stagedInstanceRegistry_->Prepare(project_.mapEdits_->LastAppliedEdit(),
        stagedInstanceRegistry_->Binding(), revision, document, capture.requests, capture.ids, defaults);
    stagedInstanceTiming_.registryPrepareNs = WorldInstanceNowNs() - registryStarted;
    pendingPlacements_.RecordCaptureTiming(stagedInstanceTiming_);
    if (!change.Succeeded()) {
        project_.AddDiagnostics(change.diagnostics); return false;
    }
    stagedInstanceRegistry_ = std::move(change.value->registry);
    stagedRequests_ = std::move(capture.requests); stagedPlacementIds_ = std::move(capture.ids);
    stagedPresentations_ = std::move(capture.presentations);
    allowedInstanceEditRevision_ = revision; *instanceRevisionGate_ = revision;
    placementsDirty_ = false;
    if (!pendingPlacements_.Entries().empty()) {
        const auto remap = pendingPlacements_.Reconcile(stagedInstanceRegistry_, &document);
        if (!remap.Succeeded()) project_.AddDiagnostics(remap.diagnostics);
    }
    UpdateOverlay();
    QueuePlacementInstances();
    return true;
}
void WorldModule::QueuePlacementInstances() {
    if (!animation_ || animation_->InstancesBusy() || !instanceRegistry_ || !stagedInstanceRegistry_
        || instanceRegistry_->Binding() == stagedInstanceRegistry_->Binding()) return;
    auto candidate = stagedInstanceRegistry_;
    // 下方只同步读取，最终重排后的render数组才交给worker独立持有；不先复制整表再复制一次。
    const auto& requests = stagedRequests_;
    const auto& ids = stagedPlacementIds_;
    auto difference = instanceRegistry_->DeltaTo(*candidate);
    const auto sourceSnapshotStarted = WorldInstanceNowNs();
    auto sources = CaptureWorldTerrainSources(project_);
    const auto sourceSnapshotNs = WorldInstanceNowNs() - sourceSnapshotStarted;
    if (!difference.Succeeded() || !sources.Succeeded()) {
        project_.AddDiagnostics(difference.Succeeded() ? sources.diagnostics : difference.diagnostics); return;
    }
    const auto mapKey = MapKey(); const auto generation = generation_;
    const auto editSession = project_.mapEdits_->SessionIdentity();
    std::unordered_map<std::uint64_t, std::size_t> requestsByIdentity;
    requestsByIdentity.reserve(candidate->CapturedSpecs().size());
    for (std::size_t i = 0; i < candidate->CapturedSpecs().size(); ++i)
        requestsByIdentity.emplace(candidate->CapturedSpecs()[i].identity.value, i);
    WorldAnimationPlayer::InstanceChange change;
    change.delta = std::move(*difference.value); change.sources = std::move(*sources.value);
    if (placementPreview_) change.preloadedAssets = placementPreview_->PreloadedAssets(change.sources);
    change.timing = stagedInstanceTiming_; change.timing.sourceSnapshotNs = sourceSnapshotNs;
    change.timing.commands = pendingPlacements_.TimingCommands(instanceRegistry_->Binding().editRevision,
        candidate->Binding().editRevision);
    change.valid = [this, mapKey, generation, editSession, catalogRevision = selectionCatalogRevision_] {
        return page_ && generation_ == generation && MapKey() == mapKey && project_.mapEdits_
            && selectionCatalogRevision_ == catalogRevision
            && project_.mapEdits_->SessionIdentity() == editSession && stagedInstanceRegistry_
            && project_.mapEdits_->Revision() == stagedInstanceRegistry_->Binding().editRevision;
    };
    // CPU增量保留旧请求顺序，新增追加；显隐必须按新服务的同一身份序列构造。
    std::vector<War3RenderCore::InstanceIdentity> nextIdentities;
    const auto& oldSpecs = animation_->Scene()->specs;
    nextIdentities.reserve(oldSpecs.size() + change.delta.additions.size());
    for (const auto& old : oldSpecs) if (requestsByIdentity.contains(old.identity.value)) nextIdentities.push_back(old.identity);
    for (const auto& added : change.delta.additions) nextIdentities.push_back(added.identity);
    auto renderIds = std::make_shared<std::vector<MapData::PlacementId>>(); renderIds->reserve(nextIdentities.size());
    auto renderRequests = std::make_shared<std::vector<Render::ModelInstanceRequest>>(); renderRequests->reserve(nextIdentities.size());
    std::vector<WorldPlacementPresentationCandidate> presentations;
    presentations.reserve(stagedPresentations_.size());
    std::vector<std::uint32_t> requestRemap(requests.size(), UINT32_MAX);
    for (const auto identity : nextIdentities) {
        const auto at = requestsByIdentity.at(identity.value);
        requestRemap.at(at) = static_cast<std::uint32_t>(renderRequests->size());
        renderIds->push_back(ids.at(at)); renderRequests->push_back(requests.at(at));
    }
    for (auto presentation : stagedPresentations_) {
        if (presentation.requestIndex >= requestRemap.size() || requestRemap[presentation.requestIndex] == UINT32_MAX) continue;
        presentation.requestIndex = requestRemap[presentation.requestIndex]; presentations.push_back(std::move(presentation));
    }
    const auto capturedVersion = PlacementPresentationVersion();
    const auto previousHelpers = pathingHelperPresentation_;
    const auto previousRegistry = instanceRegistry_;
    const auto& nextBinding = candidate->Binding();
    const auto currentVersion = BindWorldPathingHelperVersion(capturedVersion, *previousRegistry, *candidate);
    if (!currentVersion.Succeeded() || capturedVersion.document.documentId != selectionDocumentId_
        || selectionMapKey_ != mapKey || capturedVersion.catalogRevision != selectionCatalogRevision_
        || capturedVersion.document.generation != std::max<std::uint64_t>(1, project_.editRevision_.Generation())
        || !project_.archiveSession_ || capturedVersion.document.sourceSha256 != project_.archiveSession_->SourceSha256()
        || nextBinding.mapRevision != project_.mapEdits_->Revision() || nextBinding.editRevision != project_.mapEdits_->Revision()
        || nextBinding.editSessionIdentity != editSession
        || (previousHelpers && (previousHelpers->version.document.documentId != capturedVersion.document.documentId
            || previousHelpers->version.document.generation != capturedVersion.document.generation
            || previousHelpers->version.catalogRevision != capturedVersion.catalogRevision))) {
        project_.AddDiagnostics(Status::Failure("World.Helper", "WORLD_HELPER_INSTANCE_STALE",
            "辅助物选择来源与当前地图/目录或实例谱系不一致，未排队旧事实。").diagnostics);
        placementsDirty_ = true; return;
    }
    const auto version = *currentVersion.value;
    auto resourceIdentity = WorldPathingHelperResourceIdentity(change.sources, version.catalogRevision);
    if (!resourceIdentity.Succeeded()) { project_.AddDiagnostics(resourceIdentity.diagnostics); return; }
    auto preparedHelpers = std::make_shared<std::optional<WorldPathingHelperPrepared>>();
    std::unordered_map<std::uint64_t, std::uint32_t> renderIndexByIdentity;
    renderIndexByIdentity.reserve(change.delta.additions.size());
    for (const auto& addition : change.delta.additions) {
        const auto found = requestsByIdentity.find(addition.identity.value);
        if (found == requestsByIdentity.end() || requestRemap.at(found->second) == UINT32_MAX) {
            project_.AddDiagnostics(Status::Failure("World.Helper", "WORLD_HELPER_INSTANCE_STALE",
                "新增实例不属于已验证的候选请求映射，未排队。").diagnostics); return;
        }
        renderIndexByIdentity.emplace(addition.identity.value, requestRemap.at(found->second));
    }
    change.prepareOwnedAssets = [renderRequests, presentations = std::move(presentations), version,
        resourceIdentity = std::move(*resourceIdentity.value), previousHelpers, previousRegistry, candidate,
        identities = std::move(nextIdentities), preparedHelpers, renderIndexByIdentity = std::move(renderIndexByIdentity)]
        (Render::AnimationInstanceDelta& delta, std::shared_ptr<WorldResourceReader> reader, WorldPathingHelperTimings& helperTiming) {
        Services::PlacementPresentationContext context{version, resourceIdentity, WorldPathingHelperResourceReader(reader)};
        auto prepared = PrepareWorldPathingHelperDelta(*renderRequests, presentations, context, reader, previousHelpers,
            *previousRegistry, *candidate, identities);
        if (!prepared.Succeeded()) return Status{std::move(prepared.diagnostics)};
        helperTiming = prepared.value->timings;
        for (auto& addition : delta.additions) {
            const auto at = renderIndexByIdentity.find(addition.identity.value);
            if (at == renderIndexByIdentity.end()) return Status::Failure("World.Helper", "WORLD_HELPER_INSTANCE_STALE", "新辅助实例不属于同一次注册表映射。");
            addition.model = (*renderRequests)[at->second];
        }
        *preparedHelpers = std::move(*prepared.value); return Status{};
    };
    change.validateOwnedAssets = [preparedHelpers] {
        return *preparedHelpers && (*preparedHelpers)->cache && (*preparedHelpers)->cache->sourceOwner
            ? (*preparedHelpers)->cache->sourceOwner->VerifyCurrentSource()
            : Status::Failure("World.Helper", "WORLD_HELPER_SOURCE_STALE", "辅助物来源捕获尚未完成，未提交部分候选。");
    };
    change.visible = [this, renderIds, preparedHelpers](std::uint32_t request) {
        const bool helper = *preparedHelpers && request < (*preparedHelpers)->helperRequests.size()
            && (*preparedHelpers)->helperRequests[request];
        return request < renderIds->size() && worldSelection_.Flags((*renderIds)[request]).visible
            && (!helper || !showPathingHelpers_ || showPathingHelpers_->isChecked());
    };
    change.completed = [this, candidate, renderIds, renderRequests, generation, mapKey, preparedHelpers](bool accepted, const Status& status) {
        if (!page_ || generation_ != generation || MapKey() != mapKey) return;
        if (!accepted) {
            project_.AddDiagnostics(status.diagnostics);
            if (selectionStatus_) selectionStatus_->setText(tr("摆放数据已提交，显示增量未接收；原世界保持，详见诊断"));
            UpdateOverlay(); // 保留正式新行的明确占位，不把失败当作模型已经显示。
            return; // 相同失败不无限重试，不以重建掩盖驻留事务的出口。
        }
        instanceRegistry_ = candidate; scene_ = animation_->Picking();
        // completed回调只在同次Draw/Present票据通过后到达，不能以CPU目标移除占位。
        placementCompletedScene_ = animation_->Scene(); placementCompletedFrame_ = animation_->Frame();
        requests_ = *renderRequests; placementIds_ = *renderIds;
        pathingHelperRequests_.assign(requests_.size(), 0);
        pathingHelperPresentation_.reset();
        if (*preparedHelpers) {
            pathingHelperCache_ = (*preparedHelpers)->cache;
            pathingHelperRequests_ = (*preparedHelpers)->helperRequests;
            project_.AddDiagnostics((*preparedHelpers)->diagnostics);
            pathingHelperPresentation_ = std::make_shared<const WorldPathingHelperPrepared>(std::move(**preparedHelpers));
        }
        ValidateSelection(); BindInstanceDiagnostics();
        if (selectionPresenter_) selectionPresenter_->RebindParent();
        if (cliffLayer_ && cliffsInLayer_) cliffLayer_->RebindParent();
        RefreshBuildingSplats(true);
        if (placing_ || pasting_) StartPlacementPreview();
        UpdateOverlay(); UpdateSelectionText();
        // 多次正式提交以已验证谱系合并；完成前缀后再处理剩余差集，不复用旧行号。
        QTimer::singleShot(0, this, &WorldModule::QueuePlacementInstances);
    };
    if (!animation_->QueueInstances(std::move(change)) && selectionStatus_)
        selectionStatus_->setText(tr("实例增量等待当前求值或完成帧"));
}
void WorldModule::UpdateSelectionCircles() {
    if (!selectionPresenter_) return;
    std::vector<Render::ModelInstanceRequest> requests;
    std::vector<MapData::PlacementId> ids;
    for (const auto& id : selectedPlacements_) {
        if (!worldSelection_.Flags(id).visible) continue;
        const auto found = std::find(placementIds_.begin(), placementIds_.end(), id);
        if (found == placementIds_.end()) continue;
        const auto index = static_cast<std::size_t>(found - placementIds_.begin());
        if (index >= requests_.size() || !requests_[index].selection) continue;
        auto request = requests_[index];
        for (const auto& move : dragGroup_) if (move.id == id) {
            request.position = WorldPlacementDisplayPosition(request, move.position);
            if (move.rotation) request.angle = *move.rotation;
        }
        if (selection_ && id == *selection_) {
            if (dragPosition_) request.position = WorldPlacementDisplayPosition(request, *dragPosition_);
            if (dragRotation_) request.angle = *dragRotation_;
        }
        requests.push_back(std::move(request)); ids.push_back(id);
    }
    if (requests.empty()) selectionPresenter_->Hide();
    else selectionPresenter_->Show(std::move(requests), terrain_, {}, std::move(ids));
}
} // namespace War3Map::Modules
