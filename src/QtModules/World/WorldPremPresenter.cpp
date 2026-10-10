#include "WorldPremPresenter.h"
#include "WorldViewport.h"

#include <QJsonArray>
#include <QScopedValueRollback>
#include <QThread>
#include <QStringList>

#include <algorithm>
#include <map>
#include <set>

namespace War3Map::Modules {
namespace {
Status Failure(const char* code, const char* text) { return Status::Failure("World.PREM", code, text); }
QString Number(std::uint64_t value) { return QString::number(value); }
bool SkinMatches(const War3RenderSkinStatusV2& skin, const Render::AnimationFrame& frame, std::uint64_t drawnFrame) {
    return skin.structSize >= sizeof(skin) && !skin.result && skin.active == 1 && !skin.reserved
        && skin.frameCount == drawnFrame && skin.sceneVersion == frame.sceneVersion
        && skin.contentVersion == frame.contentVersion && skin.staticUploadCount && skin.matrixUploadCount;
}
}

WorldPremPresenter::WorldPremPresenter(WorldViewport* viewport, QObject* parent) : QObject(parent), viewport_(viewport) {
    if (viewport_) viewport_->SetAcceptedAnimationObserver([self = QPointer<WorldPremPresenter>(this)](auto scene, auto frame) {
        if (self) self->Accept(std::move(scene), std::move(frame));
    });
}
WorldPremPresenter::~WorldPremPresenter() {
    if (viewport_) viewport_->SetAcceptedAnimationObserver({});
    Clear();
}
void WorldPremPresenter::Remove() {
    if (viewport_) {
        const auto removed = viewport_->RemovePremModelLayer(LayerId);
        if (!removed.Succeeded()) status_ = removed;
    }
    submitted_ = false; rendererEpoch_ = 0; parentModelEpoch_ = 0;
    acceptedParent_.reset(); packet_.reset();
}
void WorldPremPresenter::Clear() {
    Remove(); source_.reset(); registry_.reset(); owners_.clear(); live_ = {};
}
void WorldPremPresenter::Bind(std::shared_ptr<const Render::AnimatedModelScene> scene,
    std::shared_ptr<const Services::WorldInstanceRegistry> registry,
    std::vector<MapData::PlacementId> owners, std::function<bool()> live) {
    Clear(); source_ = std::move(scene); registry_ = std::move(registry);
    owners_ = std::move(owners); live_ = std::move(live); Refresh();
}
void WorldPremPresenter::Refresh() {
    if (viewport_ && source_) Accept(source_, viewport_->AnimationTarget());
}
Status WorldPremPresenter::CheckFrame(const std::shared_ptr<const Render::AnimatedModelScene>& scene,
    const std::shared_ptr<const Render::AnimationFrame>& frame) const {
    if (QThread::currentThread() != thread() || !viewport_ || !source_ || scene != source_ || !registry_ || !live_ || !live_()
        || !frame || frame != viewport_->AnimationTarget() || frame->sceneVersion != scene->sceneVersion
        || frame->documentRevision != scene->documentRevision || registry_->Binding().sourceGeneration != scene->generation
        || registry_->Binding().mapRevision != scene->documentRevision || registry_->Binding().sourceIdentity != scene->sourceIdentity)
        return Failure("WORLD_PREM_SOURCE_STALE", "PREM 父帧、地图来源或当前实例谱系已失效；旧子层已清理。");
    if (!frame->prem) return {};
    const auto& packet = *frame->prem;
    if (packet.parentSceneVersion != scene->sceneVersion || packet.parentContentVersion != frame->contentVersion
        || packet.parentDocumentRevision != frame->documentRevision || packet.parentGeneration != scene->generation
        || packet.parentSourceIdentity != scene->sourceIdentity || !packet.capabilities)
        return Failure("WORLD_PREM_PARENT_BINDING_INVALID", "PREM 包未绑定同次已接受父帧的完整身份；旧子层已清理。");
    for (const auto& pose : frame->poses) if (pose.evaluationEpoch && pose.evaluationEpoch != packet.parentEvaluationEpoch)
        return Failure("WORLD_PREM_PARENT_EPOCH_INVALID", "PREM 包与实际父姿势求值 epoch 不符；旧子层已清理。");
    if (packet.childSlots.empty()) return {};
    if (!packet.parentEvaluationEpoch || !packet.childScene || !packet.childFrame || packet.childScene != scene->premGeometry
        || packet.childScene->generation != scene->generation || packet.childScene->documentRevision != frame->documentRevision
        || packet.childScene->sourceIdentity != scene->sourceIdentity || packet.childFrame->prem
        || packet.childScene->sceneVersion != packet.childFrame->sceneVersion
        || packet.childFrame->documentRevision != frame->documentRevision)
        return Failure("WORLD_PREM_CHILD_BINDING_INVALID", "PREM 子场景/帧未绑定父来源或包含递归子层；旧子层已清理。");
    std::set<std::uint32_t> parentRequests;
    for (const auto& slot : packet.childSlots) {
        if (slot.childIndex >= packet.emission.children.size())
            return Failure("WORLD_PREM_SLOT_INVALID", "PREM 子槽位越界；旧子层已清理。");
        parentRequests.insert(packet.emission.children[slot.childIndex].parentRequestIndex);
    }
    std::map<std::uint32_t, const Render::AnimationPoseLink*> parentPoses, childPoses;
    for (const auto& pose : frame->poses) if (parentRequests.contains(pose.requestIndex) && pose.poseVersion
        && pose.evaluationEpoch == packet.parentEvaluationEpoch) parentPoses.try_emplace(pose.requestIndex, &pose);
    for (const auto& pose : packet.childFrame->poses) if (pose.poseVersion && !pose.hidden && !pose.outsideView
        && !pose.staticForBudget && !pose.noVisibleGeometry && pose.rejection.code == War3RenderCore::Code::Ok
        && pose.rejectionCode.empty()) childPoses.try_emplace(pose.requestIndex, &pose);
    std::set<std::uint32_t> uniqueSlots;
    for (const auto& slot : packet.childSlots) {
        if (!uniqueSlots.insert(slot.childRequestIndex).second || slot.childIndex >= packet.emission.children.size()
            || slot.childRequestIndex >= packet.childScene->specs.size() || slot.childRequestIndex >= packet.childFrame->states.size())
            return Failure("WORLD_PREM_SLOT_INVALID", "PREM 子槽位重复或越界；旧子层已清理。");
        const auto& child = packet.emission.children[slot.childIndex];
        const auto request = child.parentRequestIndex;
        if (!child.emitter || request >= scene->specs.size() || request >= frame->states.size()
            || request >= owners_.size() || request >= registry_->CapturedSpecs().size()
            || registry_->Find(owners_[request]) != std::optional(child.parentIdentity)
            || registry_->CapturedSpecs()[request].identity != child.parentIdentity
            || scene->specs[request].identity != child.parentIdentity || frame->states[request].identity != child.parentIdentity
            || frame->states[request].sequenceIndex != child.parentSequenceIndex || !viewport_->MeasurementRequestVisible(request))
            return Failure("WORLD_PREM_OWNER_INVALID", "PREM 子模型原所属对象、动作或显隐已经失效；旧子层已清理。");
        const auto pose = parentPoses.find(request);
        const auto clock = std::find_if(packet.requestClocks.begin(), packet.requestClocks.end(), [&](const auto& row) {
            return row.requestIndex == request && row.identity == child.parentIdentity && row.sequenceIndex == child.parentSequenceIndex
                && row.parentElapsedMs == frame->states[request].elapsedMs && row.parentGlobalMs == frame->states[request].globalMs;
        });
        const auto& state = packet.childFrame->states[slot.childRequestIndex];
        const auto childPose = childPoses.find(slot.childRequestIndex);
        if (pose == parentPoses.end() || pose->second->identity != child.parentIdentity || clock == packet.requestClocks.end()
            || state.identity != packet.childScene->specs[slot.childRequestIndex].identity || state.transform != child.transform
            || childPose == childPoses.end() || childPose->second->identity != state.identity
            || childPose->second->renderedInstance >= packet.childFrame->instances.size()
            || !std::equal(child.transform.begin(), child.transform.end(),
                std::begin(packet.childFrame->instances[childPose->second->renderedInstance].instance.transform)))
            return Failure("WORLD_PREM_SLOT_SOURCE_INVALID", "PREM 子矩阵、出生时钟或实际父姿势无法验证；旧子层已清理。");
    }
    return {};
}
void WorldPremPresenter::Accept(std::shared_ptr<const Render::AnimatedModelScene> scene,
    std::shared_ptr<const Render::AnimationFrame> frame) try {
    if (accepting_) return;
    QScopedValueRollback<bool> guard(accepting_, true);
    if (!source_) { Remove(); return; } // 尚未绑定/正常清场不制造来源错误；已绑定后失效走明确诊断。
    // Changed/状态刷新可能重复指向同一immutable包；只复核当前来源及显隐，不重扫整份主姿势表。
    if (submitted_ && scene == source_ && frame == acceptedParent_ && viewport_ && frame == viewport_->AnimationTarget()
        && live_ && live_() && registry_ && rendererEpoch_ == viewport_->MeasurementRendererEpoch()
        && parentModelEpoch_ == viewport_->MeasurementModelEpoch() && packet_
        && std::all_of(packet_->childSlots.begin(), packet_->childSlots.end(), [&](const auto& slot) {
            return slot.childIndex < packet_->emission.children.size()
                && viewport_->MeasurementRequestVisible(packet_->emission.children[slot.childIndex].parentRequestIndex);
        })) return;
    const auto valid = CheckFrame(scene, frame);
    if (!valid.Succeeded()) { status_ = valid; Remove(); return; }
    if (!frame->prem || frame->prem->childSlots.empty()) {
        Remove(); acceptedParent_ = frame; packet_ = frame->prem; status_ = {}; return;
    }
    if (!viewport_->RendererReady() || !viewport_->HasModelLayerSupport()) {
        status_ = Failure("WORLD_PREM_LAYER_UNAVAILABLE", "PREM 附加模型层尚未就绪，未签收 GPU 绘制。"); Remove(); return;
    }
    const auto epoch = viewport_->MeasurementRendererEpoch();
    const auto parentEpoch = viewport_->MeasurementModelEpoch();
    if (!parentEpoch) {
        status_ = Failure("WORLD_PREM_PARENT_RENDER_EPOCH_MISSING", "PREM 主层 renderer epoch 尚未就绪，未提交子层。"); Remove(); return;
    }
    if (submitted_ && acceptedParent_ == frame && rendererEpoch_ == epoch && parentModelEpoch_ == parentEpoch) return;
    auto update = submitted_ && rendererEpoch_ == epoch && parentModelEpoch_ == parentEpoch
        && packet_ && packet_->childScene == frame->prem->childScene
        ? viewport_->UpdateModelLayer(LayerId, frame->prem->childFrame)
        : viewport_->SetModelLayer(LayerId, frame->prem->childScene, frame->prem->childFrame);
    if (!update.Succeeded()) { status_ = std::move(update); Remove(); return; }
    if (!live_ || !live_() || frame != viewport_->AnimationTarget()) {
        status_ = Failure("WORLD_PREM_INSTALL_SOURCE_CHANGED", "PREM 提交后父来源已变化；没有保留旧子层。"); Remove(); return;
    }
    acceptedParent_ = std::move(frame); packet_ = acceptedParent_->prem;
    rendererEpoch_ = epoch; parentModelEpoch_ = parentEpoch; submitted_ = true; status_ = {};
} catch (...) {
    status_ = Failure("WORLD_PREM_INSTALL_EXCEPTION", "PREM 子层准备或提交发生异常；旧子层已清理。"); Remove();
}
QString WorldPremPresenter::Summary() const {
    if (!status_.Succeeded()) {
        QStringList lines;
        for (const auto& diagnostic : status_.diagnostics)
            lines << QString::fromStdString(diagnostic.code + ": " + diagnostic.message);
        return lines.join(QLatin1Char('\n'));
    }
    if (!packet_) return {};
    return tr("模型发射 PREM：基础预览 · 子模型冻结真实 Stand0ms 近似；支持 %1，未支持 %2，本帧 CPU 活跃 %3，已提交槽位 %4，省略 %5，采样错误 %6；GPU 绘制以实际完成帧核验。")
        .arg(packet_->supportedEmitterInstances).arg(packet_->unsupportedEmitterInstances)
        .arg(packet_->emission.children.size()).arg(packet_->childSlots.size())
        .arg(packet_->emission.suppressedChildren + packet_->slotSuppressedChildren).arg(packet_->sampleErrors.size());
}
Result<QJsonObject> WorldPremPresenter::CaptureSnapshot(const Render::CapturedFrameRgba& capture) const try {
    using Output = Result<QJsonObject>;
    const auto fail = [](const char* text) { return Output::Failure("World.PREM", "WORLD_PREM_CAPTURE_INVALID", text); };
    if (!status_.Succeeded() || !acceptedParent_ || capture.animation != acceptedParent_
        // 捕获修订属于视口呈现；地图编辑修订已由同一父包与 CheckFrame 绑定，不能混比两套编号。
        || !CheckFrame(source_, acceptedParent_).Succeeded() || capture.documentRevision != viewport_->DocumentRevision()
        || capture.modelLayerFrames.size() != capture.modelLayers.size() || !capture.skin || !capture.presentation
        || !SkinMatches(*capture.skin, *acceptedParent_, capture.view.identity.frameCount))
        return fail("PREM GPU 捕获未绑定当前父帧、来源或同次完成帧，未签收子层绘制。");
    if (capture.presentation->structSize < sizeof(*capture.presentation) || capture.presentation->reserved
        || !capture.presentation->modelEpoch || !capture.presentation->presentationRevision
        || capture.presentation->sceneVersion != acceptedParent_->sceneVersion
        || capture.presentation->contentVersion != acceptedParent_->contentVersion
        || capture.presentation->instanceCount != acceptedParent_->instances.size())
        return fail("PREM 主层呈现身份未对应同次父动画帧。");
    QJsonArray clocks;
    if (packet_) for (const auto& clock : packet_->requestClocks) clocks.push_back(QJsonObject{
        {"identity", Number(clock.identity.value)}, {"identityGeneration", Number(clock.identity.generation)},
        {"requestIndex", static_cast<double>(clock.requestIndex)}, {"sequenceIndex", static_cast<double>(clock.sequenceIndex)},
        {"parentElapsedMs", Number(clock.parentElapsedMs)}, {"parentGlobalMs", Number(clock.parentGlobalMs)},
        {"elapsedMs", Number(clock.elapsedMs)}, {"globalMs", Number(clock.globalMs)}, {"tailAging", clock.tailAging}});
    QJsonObject output{{"schemaVersion", 1}, {"captureSource", "renderer_completed_frame"}, {"layerId", Number(LayerId)},
        {"parentSceneVersion", Number(acceptedParent_->sceneVersion)}, {"parentContentVersion", Number(acceptedParent_->contentVersion)},
        {"parentDocumentRevision", Number(acceptedParent_->documentRevision)}, {"parentGeneration", Number(source_->generation)},
        {"parentEvaluationEpoch", Number(packet_ ? packet_->parentEvaluationEpoch : 0)},
        {"parentSourceIdentity", QString::fromStdString(source_->sourceIdentity)},
        {"parentModelEpoch", Number(capture.presentation->modelEpoch)}, {"layersRevision", Number(capture.modelLayersRevision)},
        {"captureFrameNumber", Number(capture.view.identity.frameCount)}, {"requestClocks", clocks},
        {"cpuEmissionCount", Number(packet_ ? packet_->emission.children.size() : 0)},
        {"submittedSlotCount", Number(packet_ ? packet_->childSlots.size() : 0)},
        {"suppressedChildren", Number(packet_ ? packet_->emission.suppressedChildren + packet_->slotSuppressedChildren : 0)},
        {"sampleErrorCount", Number(packet_ ? packet_->sampleErrors.size() : 0)},
        {"gpuLayerCompletionProven", false}, {"visibleAcceptance", false}, {"selectionAcceptance", false},
        {"gameplayVerified", false}, {"childStandFrozenApproximation", true}};
    QJsonArray capabilities, errors, children;
    const auto assetSource = [](const War3RenderCore::AssetSource& source) { return QJsonObject{
        {"path", QString::fromStdString(source.virtualPath)}, {"layer", QString::fromStdString(source.sourceLayer)},
        {"sha256", QString::fromStdString(source.contentDigest)}}; };
    if (packet_) {
        const auto& budget = packet_->budget;
        output["budget"] = QJsonObject{
            {"parentAssetBytesBefore", Number(budget.parentAssetBytesBefore)},
            {"parentAssetBytesAfter", Number(budget.parentAssetBytesAfter)},
            {"parentFixedFrameBytes", Number(budget.parentFixedFrameBytes)},
            {"childSourceBytes", Number(budget.childSourceBytes)},
            {"childResourceBytes", Number(budget.childResourceBytes)},
            {"childSceneBytes", Number(budget.childSceneBytes)},
            {"childStandFrameBytes", Number(budget.childStandFrameBytes)},
            {"childFrameLayoutBytes", Number(budget.childFrameLayoutBytes)},
            {"childFrameChargedBytes", Number(budget.childFrameChargedBytes)},
            {"childEvaluatorBytes", Number(budget.childEvaluatorBytes)},
            {"birthEvaluatorBytes", Number(budget.birthEvaluatorBytes)},
            {"preparationResourceBytes", Number(budget.preparationResourceBytes)}};
        output["supportedEmitterInstances"] = Number(packet_->supportedEmitterInstances);
        output["unsupportedEmitterInstances"] = Number(packet_->unsupportedEmitterInstances);
        output["requestBudgetOmissions"] = Number(packet_->requestBudgetOmissions);
        output["slotSuppressedChildren"] = Number(packet_->slotSuppressedChildren);
        output["clockRejectedChildren"] = Number(packet_->emission.clockRejectedChildren);
        for (const auto& capability : *packet_->capabilities) capabilities.push_back(QJsonObject{
            {"prepared", capability.prepared}, {"parentAsset", Number(capability.parentAsset.value)},
            {"parentAssetGeneration", Number(capability.parentAsset.generation)},
            {"sourceIndex", static_cast<double>(capability.sourceIndex)}, {"nodeId", static_cast<double>(capability.nodeId)},
            {"sourceByteOffset", Number(capability.sourceByteOffset)}, {"originalPath", QString::fromStdString(capability.originalPath)},
            {"resolvedPath", QString::fromStdString(capability.resolvedPath)},
            {"parentSource", assetSource(capability.parentSource)}, {"childSource", assetSource(capability.childSource)},
            {"rejectionCode", QString::fromStdString(capability.rejectionCode)},
            {"rejectionMessage", QString::fromStdString(capability.rejectionMessage)}});
        for (const auto& error : packet_->sampleErrors) errors.push_back(QJsonObject{
            {"identity", Number(error.identity.value)}, {"identityGeneration", Number(error.identity.generation)},
            {"requestIndex", static_cast<double>(error.requestIndex)}, {"sourceIndex", static_cast<double>(error.sourceIndex)},
            {"birthElapsedMs", Number(error.birthElapsedMs)}, {"birthGlobalMs", Number(error.birthGlobalMs)},
            {"code", QString::fromStdString(error.code)}});
        for (const auto& slot : packet_->childSlots) {
            const auto& child = packet_->emission.children[slot.childIndex];
            QJsonArray transform; for (const auto value : child.transform) transform.push_back(value);
            children.push_back(QJsonObject{{"parentIdentity", Number(child.parentIdentity.value)},
                {"parentIdentityGeneration", Number(child.parentIdentity.generation)},
                {"parentRequestIndex", static_cast<double>(child.parentRequestIndex)},
                {"parentHostKey", QString::fromStdString(source_->specs[child.parentRequestIndex].hostKey)},
                {"sourceIndex", static_cast<double>(child.emitter->Source().sourceIndex)},
                {"nodeId", static_cast<double>(child.emitter->Source().nodeId)},
                {"childRequestIndex", static_cast<double>(slot.childRequestIndex)},
                {"birthOrdinal", Number(child.birthOrdinal)}, {"birthElapsedMs", Number(child.birthElapsedMs)},
                {"birthGlobalMs", Number(child.birthGlobalMs)}, {"ageMs", Number(child.ageMs)},
                {"worldTransformRowMajor3x4", transform}, {"childSource", assetSource(child.emitter->Binding().childSource)}});
        }
    }
    output["capabilities"] = capabilities; output["sampleErrors"] = errors; output["children"] = children;
    std::optional<std::size_t> capturedLayer;
    for (std::size_t i = 0; i < capture.modelLayers.size(); ++i) if (capture.modelLayers[i].layerId == LayerId) {
        if (capturedLayer) return fail("PREM 捕获中固定子层身份重复。");
        capturedLayer = i;
    }
    if (!packet_ || packet_->childSlots.empty()) {
        if (capturedLayer || submitted_) return fail("没有 PREM 槽位但捕获到旧子层，未签收空帧。");
        output["absenceReason"] = "no-filled-slot-for-captured-parent"; return Output::Success(std::move(output));
    }
    if (!submitted_ || !capturedLayer || rendererEpoch_ != viewport_->MeasurementRendererEpoch()
        || parentModelEpoch_ != capture.presentation->modelEpoch) return fail("PREM 子层未完成或父 renderer epoch 已变化。");
    const auto& layer = capture.modelLayers[*capturedLayer];
    if (layer.structSize < sizeof(layer) || layer.result || !layer.revision || layer.parentModelEpoch != parentModelEpoch_
        || capture.modelLayerFrames[*capturedLayer] != packet_->childFrame
        || !SkinMatches(layer.skin, *packet_->childFrame, capture.view.identity.frameCount))
        return fail("PREM 子层回执与同序子帧的 scene/content/upload/draw 身份不符。");
    output["gpuLayerCompletionProven"] = true;
    output["layerRevision"] = Number(layer.revision);
    output["childSceneVersion"] = Number(layer.skin.sceneVersion); output["childContentVersion"] = Number(layer.skin.contentVersion);
    output["staticUploadCount"] = Number(layer.skin.staticUploadCount); output["matrixUploadCount"] = Number(layer.skin.matrixUploadCount);
    output["layerDrawFrameNumber"] = Number(layer.skin.frameCount);
    if (!CheckFrame(source_, acceptedParent_).Succeeded()) return fail("PREM 捕获核对期间当前来源或父帧已变化。");
    return Output::Success(std::move(output));
} catch (...) {
    return Result<QJsonObject>::Failure("World.PREM", "WORLD_PREM_CAPTURE_EXCEPTION", "PREM 捕获资格核验异常，没有生成 GPU 完成证明。");
}
} // namespace War3Map::Modules
