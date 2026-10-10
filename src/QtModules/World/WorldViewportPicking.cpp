#include "WorldViewport.h"
#include "WorldViewportInternals.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <string>
#include <utility>

namespace War3Map::Modules {
namespace {
bool SameView(const Render::ViewFrameIdentity& first, const Render::ViewFrameIdentity& second) {
    return first.surfaceEpoch == second.surfaceEpoch && first.sceneEpoch == second.sceneEpoch && first.viewEpoch == second.viewEpoch;
}
bool SameView(const Render::ViewProjectionFrame& first, const Render::ViewProjectionFrame& second) {
    return SameView(first.identity, second.identity) && first.physicalWidth == second.physicalWidth &&
        first.physicalHeight == second.physicalHeight && first.rowMajorViewProjection == second.rowMajorViewProjection;
}
bool PickPending(const std::vector<Diagnostic>& diagnostics) {
    if (diagnostics.empty()) return false;
    const auto& code = diagnostics.front().code;
    return code == "WORLD_PICK_FRAME_PENDING" || code == "WORLD_MODEL_PICK_FRAME_PENDING"
        || code == "WORLD_MODEL_PICK_PRESENTATION_PENDING";
}
QString CoordinateText(const Render::TerrainPickHit& hit) {
    const auto coordinate = [](double value) { return QString::number(std::abs(value) < 0.005 ? 0 : value, 'f', 2); };
    return QObject::tr("X %1 · Y %2 · Z %3 · 格 (%4,%5)")
        .arg(coordinate(hit.position[0]), coordinate(hit.position[1]), coordinate(hit.position[2]))
        .arg(hit.tileX).arg(hit.tileY);
}
} // namespace

QString PickDiagnostic(const std::vector<Diagnostic>& diagnostics) {
    return diagnostics.empty() ? QObject::tr("无法取得地形坐标") : QString::fromUtf8(diagnostics.front().message);
}

void WorldViewport::SetCoordinateTexts(QString hover, QString selection) {
    if (hover == hoverText_ && selection == selectionText_) return;
    hoverText_ = std::move(hover); selectionText_ = std::move(selection);
    emit CoordinatesChanged(hoverText_, selectionText_);
}

void WorldViewport::ClearCoordinates(QString reason, bool clearSelection, bool preservePlacementPreview) {
    const bool hadHover = hoverCoordinate_.has_value();
    hoverCoordinate_.reset(); hoverView_.reset();
    // 精确坐标立即失效；预览可以等新回执时保留最后有效位置，不能把它当作新的点击落点。
    if (placing_ && !preservePlacementPreview && (hadHover || placementHoverActive_)) {
        placementHoverActive_ = false;
        emit PlacementHoverLost();
    }
    if (clearSelection) selectedCoordinate_.reset();
    SetCoordinateTexts(std::move(reason), clearSelection ? tr("未选择地形坐标") : selectionText_);
}

Result<Render::ViewProjectionFrame> WorldViewport::ReadPickingFrame() const {
    const auto fail = [](std::string code, std::string message) {
        return Result<Render::ViewProjectionFrame>::Failure("World", std::move(code), std::move(message));
    };
    if (!terrain_) return fail("WORLD_PICK_NO_TERRAIN", "尚未载入可选取的地形。");
    if (terrainStride_ != 1) return fail("WORLD_PICK_SIMPLIFIED", "当前为降采样预览，暂不可精确拾取地形坐标。");
    if (!renderer_ || failed_ || destroying_ || pickingSuspended_ || !CanRender()) return fail("WORLD_PICK_VIEW_UNAVAILABLE", "视口未就绪，无法选取地形坐标。");
    if (!HasViewInfo()) return fail("WORLD_PICK_UNSUPPORTED", "当前渲染插件缺少完成帧回执，暂不可拾取坐标。");
    War3RenderViewInfoV2 info{};
    info.structSize = sizeof(info);
    const auto status = api_->getViewInfo(renderer_, &info);
    if (status == 2) return fail("WORLD_PICK_FRAME_PENDING", "视图更新中，等待完成帧。");
    if (status != 0) {
        ReportPluginFailure("getViewInfo", status);
        return fail("WORLD_PICK_VIEW_FAILED", "无法取得当前视图回执，坐标选取已暂停。");
    }
    const auto size = PhysicalSize();
    if (info.structSize != sizeof(info) || info.documentRevision != revision_ || info.frameCount == 0 ||
        info.surfaceEpoch == 0 || info.sceneEpoch == 0 || info.viewEpoch == 0 ||
        info.width != static_cast<std::uint32_t>(size.width()) || info.height != static_cast<std::uint32_t>(size.height())) {
        return fail("WORLD_PICK_FRAME_STALE", "完成帧与当前地图或物理视口不匹配，坐标结果已丢弃。");
    }
    Render::ViewProjectionFrame frame;
    frame.identity = {info.surfaceEpoch, info.sceneEpoch, info.viewEpoch, info.frameCount};
    frame.physicalWidth = info.width; frame.physicalHeight = info.height;
    std::copy_n(info.viewProjection, frame.rowMajorViewProjection.size(), frame.rowMajorViewProjection.begin());
    return Result<Render::ViewProjectionFrame>::Success(frame);
}

Result<Render::TerrainPickResult> WorldViewport::PickAt(QPointF point, const Render::ViewProjectionFrame& frame,
    const MapData::TerrainGridSnapshot* fixedTerrain) const {
    const auto ray = Render::MakeViewRayFromLogical(frame,
        {static_cast<std::uint32_t>(width()), static_cast<std::uint32_t>(height()), devicePixelRatioF()}, {point.x(), point.y()});
    if (!ray.Succeeded()) return {std::nullopt, ray.diagnostics};
    // 只对 stride=1、受上传预算限制的完整网格执行；50ms 合并且相同视图/鼠标点不重复计算。
    // 无地图 IO/GPU 等待/事件泵，GUI 线程上的有界计算期间不会借用异步 QObject。
    const auto picked = Render::PickTerrainHeightPreview(fixedTerrain ? *fixedTerrain : *terrain_,
        ray.value->ray, {ray.value->maxDistance, 8192});
    if (!picked.Succeeded()) return picked;
    const auto completed = ReadPickingFrame();
    if (!completed.Succeeded()) return {std::nullopt, completed.diagnostics};
    if (!SameView(frame.identity, completed.value->identity)) {
        return Result<Render::TerrainPickResult>::Failure("World", "WORLD_PICK_FRAME_STALE", "视图已改变，坐标结果已丢弃。");
    }
    return picked;
}

bool WorldViewport::IsInstanceSelectable(std::uint32_t instance) const {
    if (!models_ || instance >= models_->instances.size() || instance >= models_->instanceRequests.size()) return false;
    const auto request = models_->instanceRequests[instance];
    return request != Render::kBakedInstance && (!instanceVisible_ || instanceVisible_(request))
        && (!instanceSelectable_ || instanceSelectable_(request));
}

std::optional<std::array<double, 3>> WorldViewport::ProjectInstanceCenter(std::uint32_t instance,
    const Render::ViewProjectionFrame& frame) const {
    if (!models_ || instance >= models_->instances.size()) return std::nullopt;
    // 选择以摆放的世界中心为准，不以可能很大的模型包围盒边缘判定框选。
    const auto* t = models_->instances[instance].transform;
    const auto& m = frame.rowMajorViewProjection;
    const double x = t[3], y = t[7], z = t[11];
    const double cx = x * m[0] + y * m[4] + z * m[8] + m[12];
    const double cy = x * m[1] + y * m[5] + z * m[9] + m[13];
    const double cz = x * m[2] + y * m[6] + z * m[10] + m[14];
    const double w = x * m[3] + y * m[7] + z * m[11] + m[15];
    if (!std::isfinite(cx) || !std::isfinite(cy) || !std::isfinite(cz) || !std::isfinite(w)
        || !(w > 1e-6) || cz < 0 || cz > w || std::abs(cx) > w || std::abs(cy) > w) return std::nullopt;
    return std::array<double, 3>{(cx / w * 0.5 + 0.5) * width(), (0.5 - cy / w * 0.5) * height(), cz / w};
}

std::vector<std::pair<std::uint32_t, QPointF>> WorldViewport::ProjectedSelectableInstances(std::size_t limit) const {
    std::vector<std::pair<std::uint32_t, QPointF>> result;
    const auto frame = ReadPickingFrame();
    if (!frame.Succeeded() || !models_) return result;
    for (std::uint32_t index = 0; index < models_->instances.size() && result.size() < limit; ++index) {
        if (!IsInstanceSelectable(index)) continue;
        const auto center = ProjectInstanceCenter(index, *frame.value);
        if (center) result.emplace_back(index, QPointF((*center)[0], (*center)[1]));
    }
    return result;
}

std::optional<std::array<float, 3>> WorldViewport::SelectableInstancePosition(std::uint32_t instance) const {
    if (!IsInstanceSelectable(instance)) return std::nullopt;
    const auto* t = models_->instances[instance].transform;
    return std::array<float, 3>{t[3], t[7], t[11]};
}

Result<WorldViewport::ModelPickPolicy> WorldViewport::ReadModelPickPolicy() const try {
    using Output = Result<ModelPickPolicy>;
    if (!animationScene_)
        return Output::Failure("World", "WORLD_MODEL_PICK_BUDGET", "精确点选需要有界的当前动画场景。");
    ModelPickPolicy policy; policy.generation = animationScene_->generation; policy.sceneVersion = animationScene_->sceneVersion;
    policy.visible.reserve(animationScene_->specs.size()); policy.selectable.reserve(animationScene_->specs.size());
    for (std::uint32_t request = 0; request < animationScene_->specs.size(); ++request) {
        policy.visible.push_back(static_cast<std::uint8_t>(!instanceVisible_ || instanceVisible_(request)));
        policy.selectable.push_back(static_cast<std::uint8_t>(!instanceSelectable_ || instanceSelectable_(request)));
    }
    return Output::Success(std::move(policy));
} catch (...) {
    return Result<ModelPickPolicy>::Failure("World", "WORLD_MODEL_PICK_POLICY", "无法冻结当前显隐/锁定标志，点选已取消。");
}

Result<WorldViewport::CompletedModelPickFrame> WorldViewport::ReadCompletedModelPickFrame(bool forGeometryPick,
    bool requireSuccessfulPresent) const try {
    using Output = Result<CompletedModelPickFrame>;
    const auto fail = [](const char* code, const char* message) { return Output::Failure("World", code, message); };
    if (!ModelPresentationCurrent())
        return fail("WORLD_MODEL_PICK_SOURCE_INVALID", "动画画面已冻结，只读；旧姿势不能用于当前点选。");
    if (!renderer_ || !CanRender() || failed_ || destroying_ || pickingSuspended_ || !animationScene_ || !animationFrame_)
        return fail("WORLD_MODEL_PICK_UNAVAILABLE", "当前没有可精确点选的动画场景。");
    if (!api_ || api_->apiVersion != WAR3_RENDER_PLUGIN_API_V2 ||
        api_->structSize < offsetof(War3RenderPluginApiV2, getFrameMetrics) + sizeof(api_->getFrameMetrics) ||
        !api_->getFrameMetrics || !HasEditorVisibilitySupport())
        return fail("WORLD_MODEL_PICK_UNSUPPORTED", "当前插件缺少同帧姿势/显隐绘制回执，不能精确点选模型。");
    if (!editorPresentationTarget_ || editorVisibilityDirty_)
        return fail("WORLD_MODEL_PICK_PRESENTATION_PENDING", "编辑器显隐仍待呈现，点选已取消。");
    const auto currentView = ReadPickingFrame();
    if (!currentView.Succeeded()) return {std::nullopt, currentView.diagnostics};
    auto policy = forGeometryPick ? ReadModelPickPolicy() : Result<ModelPickPolicy>::Success({});
    if (!policy.Succeeded()) return {std::nullopt, std::move(policy.diagnostics)};
    // getEditorCompletedFrame只接受“最新提交已经完成”，持续播放时可能永远追不上。
    // 完成指标环保留同次Draw/Present的不可变身份；仅查询最后四帧并绑定拥有的CPU姿势。
    War3RenderFrameMetricsPageV2 page{}; page.structSize = sizeof(page);
    const auto indexResult = api_->getFrameMetrics(renderer_, 0, nullptr, 0, &page);
    if (indexResult != 0) ReportPluginFailure("getFrameMetrics", indexResult);
    if (indexResult != 0 || page.structSize < sizeof(page))
        return fail("WORLD_MODEL_PICK_FRAME_FAILED", "无法读取完成帧索引，点选已取消。");
    if (!page.latestFrame) return fail("WORLD_MODEL_PICK_FRAME_PENDING", "等待首个实际完成的模型画面。");
    std::array<War3RenderFrameMetricsV2, 4> metrics{};
    const auto afterFrame = page.latestFrame > metrics.size() ? page.latestFrame - metrics.size() : 0;
    const auto pageResult = api_->getFrameMetrics(renderer_, afterFrame, metrics.data(), static_cast<std::uint32_t>(metrics.size()), &page);
    if (pageResult != 0) ReportPluginFailure("getFrameMetrics", pageResult);
    if (pageResult != 0 ||
        page.structSize < sizeof(page) || page.count > metrics.size())
        return fail("WORLD_MODEL_PICK_FRAME_FAILED", "无法读取同次绘制完成身份，点选已取消。");
    const RetainedAnimationPickFrame* retained = nullptr;
    const War3RenderEditorCompletedFrameV2* accepted = nullptr;
    const War3RenderFrameMetricsV2* acceptedMetric = nullptr;
    for (std::size_t i = page.count; i > 0 && !retained; --i) {
        const auto& metric = metrics[i - 1]; const auto& candidate = metric.completed;
        const bool occluded = (metric.flags & WAR3_RENDER_FRAME_METRICS_PRESENT_OCCLUDED) != 0;
        // 只有CPU准备允许明确遮挡的非失败Present；未知非零状态、负HRESULT仍拒绝。
        // 默认精确点选继续要求成功且未遮挡；不能从准备计划推导方块已可见。
        const bool presentAccepted = requireSuccessfulPresent ? !metric.presentResult && !occluded
            : !metric.presentResult || (occluded && metric.presentResult >= 0);
        if (metric.structSize < sizeof(metric) || metric.result || !presentAccepted ||
            (metric.flags & (WAR3_RENDER_FRAME_METRICS_DRAW_COMPLETED | WAR3_RENDER_FRAME_METRICS_UPLOAD_VALID)) !=
                (WAR3_RENDER_FRAME_METRICS_DRAW_COMPLETED | WAR3_RENDER_FRAME_METRICS_UPLOAD_VALID) ||
            (!requireSuccessfulPresent && !(metric.flags & WAR3_RENDER_FRAME_METRICS_WINDOW_ATTACHED))) continue;
        Render::ViewProjectionFrame candidateView;
        candidateView.identity = {candidate.view.surfaceEpoch, candidate.view.sceneEpoch, candidate.view.viewEpoch, candidate.view.frameCount};
        candidateView.physicalWidth = candidate.view.width; candidateView.physicalHeight = candidate.view.height;
        std::copy_n(candidate.view.viewProjection, candidateView.rowMajorViewProjection.size(), candidateView.rowMajorViewProjection.begin());
        if (!SameView(candidateView, *currentView.value) || candidate.view.documentRevision != revision_) continue;
        for (auto history = animationPickHistory_.rbegin(); history != animationPickHistory_.rend(); ++history) {
            if (!history->frame || history->rendererEpoch != measurementRendererEpoch_ || history->policyRevision != pickPolicyRevision_ ||
                history->frame->sceneVersion != animationScene_->sceneVersion || history->frame->documentRevision != animationScene_->documentRevision ||
                history->frame->contentVersion != candidate.skin.contentVersion ||
                history->presentation.modelEpoch != editorPresentationTarget_->modelEpoch ||
                history->presentation.modelEpoch != candidate.presentation.modelEpoch ||
                history->presentation.mode != candidate.presentation.mode ||
                history->presentation.sceneVersion != candidate.presentation.sceneVersion ||
                history->presentation.contentVersion != candidate.presentation.contentVersion ||
                history->presentationRevision != candidate.presentation.presentationRevision) continue;
            retained = &*history; accepted = &candidate; acceptedMetric = &metric; break;
        }
    }
    if (!retained || !accepted)
        return fail("WORLD_MODEL_PICK_FRAME_PENDING", "当前视图尚无拥有姿势与显隐的实际完成帧。");
    const auto& completed = *accepted;
    const auto& view = completed.view; const auto& skin = completed.skin; const auto& presentation = completed.presentation;
    const auto& target = retained->presentation; const auto& frame = *retained->frame; const auto size = PhysicalSize();
    const float cameraFields[]{completed.camera.targetX, completed.camera.targetY, completed.camera.targetZ,
        completed.camera.distance, completed.camera.yawRadians, completed.camera.pitchRadians};
    // 逐项核对并报告第一项不一致，便于定位；任何一项失败都不使用该回执。
    const char* mismatch = nullptr;
    const auto check = [&](bool ok, const char* name) { if (!ok && !mismatch) mismatch = name; };
    check(completed.structSize >= sizeof(completed) && view.structSize >= sizeof(view) && skin.structSize >= sizeof(skin) &&
        completed.camera.structSize >= sizeof(completed.camera) && presentation.structSize >= sizeof(presentation) && !presentation.reserved, "结构版本");
    check(view.frameCount && view.surfaceEpoch && view.sceneEpoch && view.viewEpoch, "视图代次");
    check(view.documentRevision == revision_, "视图地图修订");
    check(view.width == static_cast<std::uint32_t>(size.width()) && view.height == static_cast<std::uint32_t>(size.height()), "视口尺寸");
    check(skin.active == 1 && skin.result == 0, "姿势上传状态");
    check(skin.frameCount == view.frameCount, "姿势帧号");
    check(skin.sceneVersion == animationScene_->sceneVersion && skin.sceneVersion == frame.sceneVersion, "姿势场景版本");
    check(skin.contentVersion == frame.contentVersion, "姿势内容版本");
    // revision_ 是视口的地形显示计数，动画场景携带的是地图编辑修订：两者不是同一个编号，不能互相比较
    // （真实地图上二者几乎总不相等，点选因此从未成功）。场景随地图修订的失效由世界模块负责：
    // 修订变化时先 InvalidateAnimationPicking 再换场景；这里只核对帧与场景属于同一修订。
    check(frame.documentRevision == animationScene_->documentRevision, "动画地图修订");
    check(presentation.mode == target.mode && presentation.modelEpoch == target.modelEpoch, "显隐模式/模型代次");
    check(presentation.sceneVersion == skin.sceneVersion && presentation.contentVersion == skin.contentVersion, "显隐与姿势版本");
    check(target.sceneVersion == skin.sceneVersion && target.contentVersion == skin.contentVersion, "显隐目标版本");
    check(retained->presentationRevision && presentation.presentationRevision == retained->presentationRevision, "显隐修订");
    check(presentation.instanceCount == frame.instances.size(), "显隐实例数");
    check(presentation.particleDrawCount == retained->particleDrawCount, "显隐粒子批次数");
    check(presentation.visibleInstanceCount <= presentation.instanceCount && presentation.visibleParticleDrawCount <= presentation.particleDrawCount
        && presentation.drawCallCount <= 262144, "显隐计数范围");
    check(std::all_of(std::begin(view.viewProjection), std::end(view.viewProjection), [](float v) { return std::isfinite(v); }) &&
        std::all_of(std::begin(cameraFields), std::end(cameraFields), [](float v) { return std::isfinite(v); }), "视图矩阵");
    if (mismatch)
        return Output::Failure("World", "WORLD_MODEL_PICK_FRAME_STALE",
            std::string("绘制回执与当前目标不一致（") + mismatch + "）。");
    if (animationScene_->particlesEnabled && animationScene_->particleInstanceCount) {
        const auto& particle = completed.particles;
        if (particle.structSize < sizeof(particle) || particle.reserved || particle.sceneVersion != skin.sceneVersion ||
            particle.contentVersion != skin.contentVersion || particle.vertexCount != retained->particleVertexCount ||
            particle.drawCount != retained->particleDrawCount)
            return fail("WORLD_MODEL_PICK_FRAME_STALE", "粒子与模型不属于同一次绘制回执。");
    }
    CompletedModelPickFrame result;
    result.metric = *acceptedMetric;
    result.frame = retained->frame;
    if (forGeometryPick) result.picking = Render::AnimationPickingScene(*animationScene_, frame);
    result.view.identity = {view.surfaceEpoch, view.sceneEpoch, view.viewEpoch, view.frameCount};
    result.view.physicalWidth = view.width; result.view.physicalHeight = view.height;
    std::copy_n(view.viewProjection, result.view.rowMajorViewProjection.size(), result.view.rowMajorViewProjection.begin());
    // 绑定使用动画场景自身的地图修订（不是视口地形显示计数 revision_），与 PickAnimatedModelInstances 的核对同一编号。
    result.visibility.binding = {animationScene_->documentRevision, animationScene_->generation, skin.sceneVersion, skin.contentVersion,
        presentation.modelEpoch, presentation.presentationRevision, result.view.identity};
    result.visibility.visible = std::move(policy.value->visible); result.visibility.selectable = std::move(policy.value->selectable);
    return Output::Success(std::move(result));
} catch (...) {
    return Result<CompletedModelPickFrame>::Failure("World", "WORLD_MODEL_PICK_OWNER_FAILED", "无法拥有实际完成帧的点选数据，未改用最新提交姿势。");
}

Result<std::vector<Render::ModelPickHit>> WorldViewport::PickInstancesAt(QPointF point,
    const Render::ViewProjectionFrame& requestedView) const {
    using Output = Result<std::vector<Render::ModelPickHit>>;
    if (!models_) {
        if (!animationScene_) return Output::Success({});
        return Output::Failure("World", "WORLD_MODEL_PICK_UNAVAILABLE", "当前动画帧没有摆放映射。");
    }
    if (animationScene_ ? animationScene_->specs.empty() : std::all_of(models_->instanceRequests.begin(),
        models_->instanceRequests.end(), [](auto request) { return request == Render::kBakedInstance; })) return Output::Success({});
    const auto completed = ReadCompletedModelPickFrame();
    if (!completed.Succeeded()) return {std::nullopt, completed.diagnostics};
    const auto& pick = *completed.value;
    const auto policyRevision = pickPolicyRevision_;
    const auto rendererEpoch = measurementRendererEpoch_;
    const auto scene = animationScene_;
    if (!SameView(requestedView, pick.view) || (pressModelPolicy_ &&
        (pressModelPolicy_->generation != pick.visibility.binding.generation || pressModelPolicy_->sceneVersion != pick.visibility.binding.sceneVersion ||
        pressModelPolicy_->visible != pick.visibility.visible || pressModelPolicy_->selectable != pick.visibility.selectable)))
        return Output::Failure("World", "WORLD_MODEL_PICK_FRAME_STALE", "按下后相机、场景或显隐/锁定标志已改变，点选已取消。");
    const auto ray = Render::MakeViewRayFromLogical(pick.view,
        {static_cast<std::uint32_t>(width()), static_cast<std::uint32_t>(height()), devicePixelRatioF()}, {point.x(), point.y()});
    if (!ray.Succeeded()) return {std::nullopt, ray.diagnostics};
    Render::ModelPickLimits limits;
    if (animationScene_->specs.size() > std::numeric_limits<std::uint32_t>::max())
        return Output::Failure("World", "WORLD_MODEL_PICK_BUDGET", "实例请求编号无法由当前点选接口表达。");
    limits.maxInstances = static_cast<std::uint32_t>(animationScene_->specs.size());
    const auto queried = Render::PickAnimatedModelInstances(*scene, *pick.frame, pick.visibility,
        pick.visibility.binding, ray.value->ray, ray.value->maxDistance, limits);
    if (!queried.Succeeded()) return {std::nullopt, queried.diagnostics};
    const auto afterView = ReadPickingFrame();
    if (!afterView.Succeeded()) return {std::nullopt, afterView.diagnostics};
    const auto afterPolicy = ReadModelPickPolicy();
    if (!afterPolicy.Succeeded()) return {std::nullopt, afterPolicy.diagnostics};
    // 查询拥有真正完成的姿势。异步重画下一姿势不使它失效；来源、模型槽、相机或策略改变则拒绝。
    if (!ModelPresentationCurrent() || scene != animationScene_ || rendererEpoch != measurementRendererEpoch_ ||
        policyRevision != pickPolicyRevision_ || editorVisibilityDirty_ || !editorPresentationTarget_ ||
        editorPresentationTarget_->modelEpoch != pick.visibility.binding.modelEpoch ||
        !SameView(*afterView.value, pick.view) || afterPolicy.value->visible != pick.visibility.visible ||
        afterPolicy.value->selectable != pick.visibility.selectable || queried.value->binding != pick.visibility.binding)
        return Output::Failure("World", "WORLD_MODEL_PICK_FRAME_STALE", "查询期间绘制帧或显隐/锁定已改变，本次结果已丢弃。");
    const auto& result = *queried.value;
    // 个别模型无法精确判定（坏资源、未知选择标志、预算）只影响那些模型：其余已证实的网格/碰撞命中照常使用，
    // 不能让一个坏模型使整张图都点不中。未命中时如实说明覆盖不完整，不冒称确定未命中。
    std::vector<Render::ModelPickHit> hits; hits.reserve(result.hits.size());
    for (const auto& hit : result.hits) {
        if ((hit.grade != 8 && hit.grade != 4) || hit.renderedInstance >= pick.frame->instances.size() ||
            hit.requestIndex >= scene->specs.size() || scene->specs[hit.requestIndex].identity != hit.identity ||
            std::find(pick.picking->instanceRequests.begin(), pick.picking->instanceRequests.end(), hit.requestIndex) == pick.picking->instanceRequests.end())
            return Output::Failure("World", "WORLD_MODEL_PICK_IDENTITY", "精确命中的实例身份与当前请求不一致。");
        const auto found = std::find(models_->instanceRequests.begin(), models_->instanceRequests.end(), hit.requestIndex);
        if (found == models_->instanceRequests.end())
            return Output::Failure("World", "WORLD_MODEL_PICK_IDENTITY", "精确命中没有当前摆放映射，选择已取消。");
        hits.push_back({static_cast<std::uint32_t>(found - models_->instanceRequests.begin()), hit.distance});
    }
    const bool incomplete = result.budgetExceeded || !result.meshComplete || !result.collisionComplete;
    const bool empty = hits.empty();
    auto output = Output::Success(std::move(hits));
    if (incomplete && empty) output.diagnostics.push_back({DiagnosticSeverity::Info, "Render.Picking",
        "WORLD_MODEL_PICK_INCOMPLETE", "此处没有可证实的模型命中；部分模型无法精确判定（坏资源或预算）。", std::nullopt});
    if (result.hitsTruncated) output.diagnostics.push_back({DiagnosticSeverity::Info, "Render.Picking",
        "WORLD_MODEL_PICK_HITS_TRUNCATED", "重叠命中超过显示上限，只在返回的前 16 项之间轮换。", std::nullopt});
    return output;
}

Result<std::shared_ptr<const Render::AnimationFrame>> WorldViewport::ReadCompletedAnimationFrameForBugPlaceholder(
    const std::shared_ptr<const Render::AnimatedModelScene>& expected) const {
    if (!expected || expected != animationScene_)
        return Result<std::shared_ptr<const Render::AnimationFrame>>::Failure("World.BugPlaceholder",
            "WORLD_BUG_PLACEHOLDER_SOURCE_STALE", "占位原始动画来源已改变。");
    auto completed = ReadCompletedModelPickFrame(false, false);
    if (!completed.Succeeded()) return {std::nullopt, std::move(completed.diagnostics)};
    return Result<std::shared_ptr<const Render::AnimationFrame>>::Success(std::move(completed.value->frame));
}

Result<std::optional<WorldViewport::BugPlaceholderPick>> WorldViewport::PickBugPlaceholderAt(QPointF point,
    const Render::ViewProjectionFrame& requestedView) const try {
    using Output = Result<std::optional<BugPlaceholderPick>>;
    const auto owned = bugPlaceholderPicking_;
    if (!owned) return Output::Success(std::nullopt);
    const auto stale = [] { return Output::Failure("World.BugPlaceholder", "WORLD_BUG_PLACEHOLDER_PICK_STALE",
        "方块来源、显隐、锁定或同次完成层已改变，本次点选已取消。"); };
    if (!owned->plan || !owned->scene || !owned->frame || !owned->live || !owned->flags || !owned->live()
        || owned->plan->sourceScene != animationScene_ || owned->rendererEpoch != measurementRendererEpoch_
        || modelLayerBridgeHandle_ != renderer_ || !HasModelLayerSupport()) return stale();
    const auto layer = modelLayerBridges_.find(owned->layer);
    if (layer == modelLayerBridges_.end() || layer->second.scene != owned->scene
        || !layer->second.renderer || layer->second.renderer->Target() != owned->frame) return stale();
    War3RenderModelLayersTargetV2 before{sizeof(before)};
    if (api_->getModelLayersTarget(renderer_, &before) != 0 || !before.parentModelEpoch || !before.layersRevision) return stale();
    auto completed = ReadCompletedModelPickFrame(false);
    if (!completed.Succeeded()) return {std::nullopt, std::move(completed.diagnostics)};
    if (!SameView(completed.value->view, requestedView)
        || before.parentModelEpoch != completed.value->visibility.binding.modelEpoch) return stale();
    // ReadPickingFrame只接收当前sceneEpoch实际完成的画面；ReadCompletedModelPickFrame同时核
    // 同次Draw/Present成功及全层UPLOAD_VALID聚合。无需RGBA读回，也不将待上传目标当成完成。
    auto current = Services::PlanWorldModelBugPlaceholders(animationScene_, *completed.value->frame,
        owned->plan->registry, owned->owners, owned->flags);
    if (!current.Succeeded() || current.value->binding != owned->plan->binding
        || current.value->markers != owned->plan->markers) return stale();
    const auto ray = Render::MakeViewRayFromLogical(completed.value->view,
        {static_cast<std::uint32_t>(width()), static_cast<std::uint32_t>(height()), devicePixelRatioF()}, {point.x(), point.y()});
    if (!ray.Succeeded()) return {std::nullopt, ray.diagnostics};
    Render::ModelPickVisibilitySnapshot masks;
    masks.binding = {owned->scene->documentRevision, owned->scene->generation, owned->scene->sceneVersion,
        owned->frame->contentVersion, before.parentModelEpoch, layer->second.renderer->Revision(), completed.value->view.identity};
    masks.visible.assign(owned->plan->markers.size(), 1);
    for (const auto& marker : owned->plan->markers) masks.selectable.push_back(marker.selectable ? 1 : 0);
    auto queried = Render::PickAnimatedModelInstances(*owned->scene, *owned->frame, masks, masks.binding,
        ray.value->ray, ray.value->maxDistance);
    if (!queried.Succeeded()) return {std::nullopt, std::move(queried.diagnostics)};
    War3RenderModelLayersTargetV2 after{sizeof(after)};
    const auto view = ReadPickingFrame();
    const auto actual = ReadCompletedModelPickFrame(false);
    if (!actual.Succeeded()) return {std::nullopt, actual.diagnostics};
    auto endPlan = Services::PlanWorldModelBugPlaceholders(animationScene_, *actual.value->frame,
        owned->plan->registry, owned->owners, owned->flags);
    if (bugPlaceholderPicking_ != owned || !owned->live() || !view.Succeeded() || !SameView(*view.value, requestedView)
        || api_->getModelLayersTarget(renderer_, &after) != 0 || before.parentModelEpoch != after.parentModelEpoch
        || before.layersRevision != after.layersRevision || before.layerCount != after.layerCount
        || !endPlan.Succeeded() || endPlan.value->binding != owned->plan->binding
        || endPlan.value->markers != owned->plan->markers) return stale();
    if (!Services::WorldModelBugLayerFrameQualified(before, after, completed.value->view, completed.value->metric)
        || !Services::WorldModelBugLayerFrameQualified(before, after, actual.value->view, actual.value->metric)) return stale();
    if (queried.value->hits.empty()) return Output::Success(std::nullopt);
    const auto& hit = queried.value->hits.front();
    if (hit.grade != 8 || hit.requestIndex >= owned->plan->markers.size()
        || hit.identity != owned->plan->markers[hit.requestIndex].identity) return stale();
    return Output::Success(BugPlaceholderPick{owned->plan->markers[hit.requestIndex].owner, hit.distance});
} catch (...) {
    return Result<std::optional<BugPlaceholderPick>>::Failure("World.BugPlaceholder", "WORLD_BUG_PLACEHOLDER_PICK_FAILED",
        "无法拥有当前方块点选数据，未使用原模型实例下标兜底。");
}

void WorldViewport::ScheduleHover() {
    const auto pixels = PhysicalSize();
    if (pointerInside_ && dragButton_ == Qt::NoButton && CanRender() && renderer_ && !failed_ && !destroying_ && !pickingSuspended_ &&
        pixels.width() <= 4096 && pixels.height() <= 4096 &&
        terrain_ && terrainStride_ == 1 && HasViewInfo()) {
        if (!hoverTimer_.isActive()) {
            // 第一次有效落点立即核对实际完成视图，后续移动仍按50ms合并。
            // 先启动周期以避免信号回调重复进入；没有完成帧时由原路径继续等待。
            hoverTimer_.start();
            if (placing_) UpdateHover();
        }
    } else hoverTimer_.stop();
}

void WorldViewport::UpdateHover() {
    if (!pointerInside_ || dragButton_ != Qt::NoButton || !CanRender() || failed_ || destroying_ || pickingSuspended_) {
        hoverTimer_.stop(); ClearCoordinates(tr("悬停坐标已清除")); return;
    }
    const auto frame = ReadPickingFrame();
    if (!frame.Succeeded()) {
        const bool pending = !frame.diagnostics.empty() && frame.diagnostics.front().code == "WORLD_PICK_FRAME_PENDING";
        ClearCoordinates(PickDiagnostic(frame.diagnostics), !pending, pending); return;
    }
    if (hoverView_ && SameView(*hoverView_, frame.value->identity) && sampledPosition_ == hoverPosition_) return;
    const auto picked = PickAt(hoverPosition_, *frame.value);
    if (!picked.Succeeded()) {
        ClearCoordinates(PickDiagnostic(picked.diagnostics), true); return;
    }
    hoverCoordinate_ = picked.value->hit;
    hoverView_ = frame.value->identity; sampledPosition_ = hoverPosition_;
    SetCoordinateTexts(hoverCoordinate_ ? tr("悬停：%1").arg(CoordinateText(*hoverCoordinate_)) : tr("鼠标处未命中地形"), selectionText_);
    if (placing_) {
        placementHoverActive_ = hoverCoordinate_.has_value();
        if (hoverCoordinate_) emit PlacementHovered(hoverCoordinate_->position[0],
            hoverCoordinate_->position[1], hoverCoordinate_->position[2],
            hoverModifiers_.testFlag(Qt::ControlModifier));
        else emit PlacementHoverLost();
    }
}

bool WorldViewport::DeferSelection(QPointF point, Qt::KeyboardModifiers modifiers, bool box) {
    if (!pressView_ || selectionRetries_ >= 90 || placing_ || brushMode_) return false;
    ++selectionRetries_;
    const auto serial = selectionGestureSerial_, revision = pressRevision_;
    const auto view = pressView_; const auto policy = pressModelPolicy_; const auto anchor = pressPosition_;
    QTimer::singleShot(16, this, [this, serial, revision, view, policy, anchor, point, modifiers, box] {
        if (serial != selectionGestureSerial_ || revision != revision_ || !isVisible() ||
            destroying_ || pickingSuspended_ || failed_ || dragButton_ != Qt::NoButton || placing_ || brushMode_) return;
        // 只重试同一手势；新相机/来源/显隐仍由完成帧核对拒绝，不混用旧姿势。
        const auto oldView = pressView_; const auto oldPolicy = pressModelPolicy_;
        const auto oldAnchor = pressPosition_; const auto oldModifiers = selectionModifiers_;
        pressView_ = view; pressRevision_ = revision; pressModelPolicy_ = policy;
        pressPosition_ = anchor; selectionModifiers_ = modifiers;
        if (box) FinishSelectionBox(point); else FinishClick(point, modifiers);
        pressView_ = oldView; pressModelPolicy_ = oldPolicy; pressPosition_ = oldAnchor;
        selectionModifiers_ = oldModifiers;
    });
    ClearCoordinates(tr("等待当前姿势完成绘制后选取…"));
    return true;
}

void WorldViewport::FinishClick(QPointF point, Qt::KeyboardModifiers modifiers) {
    if (!pressView_ || pressRevision_ != revision_) {
        const auto available = ReadPickingFrame();
        ClearCoordinates(available.Succeeded() ? tr("视图已改变，请再次单击") : PickDiagnostic(available.diagnostics)); return;
    }
    const auto frame = ReadPickingFrame();
    if (!frame.Succeeded()) {
        if (PickPending(frame.diagnostics) && DeferSelection(point, modifiers, false)) return;
        ClearCoordinates(PickDiagnostic(frame.diagnostics), true); return;
    }
    if (!SameView(*pressView_, frame.value->identity)) {
        ClearCoordinates(tr("按下后视图已改变，本次坐标选择已取消")); return;
    }
    if (placing_) {
        const auto ground = PickAt(point, *frame.value);
        if (ground.Succeeded() && ground.value->hit)
            emit PlaceRequested(ground.value->hit->position[0], ground.value->hit->position[1],
                modifiers.testFlag(Qt::ControlModifier));
        return;
    }
    const auto picked = PickAt(point, *frame.value);
    if (!picked.Succeeded()) { ClearCoordinates(PickDiagnostic(picked.diagnostics), true); return; }
    // 摆放点选：同一位置再次单击时在重叠的实例间轮换。
    // 地面查询先完成；模型查询最后核对 release 的同帧身份，不将旧按下姿势拼接到新相机。
    const auto hits = PickInstancesAt(point, *frame.value);
    if (!hits.Succeeded()) {
        if (PickPending(hits.diagnostics) && DeferSelection(point, modifiers, false)) return;
        ClearCoordinates(PickDiagnostic(hits.diagnostics), true); return;
    }
    const auto bug = PickBugPlaceholderAt(point, *frame.value);
    if (!bug.Succeeded()) {
        if (PickPending(bug.diagnostics) && DeferSelection(point, modifiers, false)) return;
        ClearCoordinates(PickDiagnostic(bug.diagnostics), true); return;
    }
    if (*bug.value && (hits.value->empty() || (*bug.value)->distance <= hits.value->front().distance)) {
        selectedCoordinate_ = picked.value->hit;
        SetCoordinateTexts(hoverText_, tr("命中：模型无法绘制的紫黑占位；真实原因见模型诊断。"));
        const auto owner = (*bug.value)->owner; // signal回调可重建视口，不借用绑定内存。
        emit BugPlaceholderClicked(owner, static_cast<int>(modifiers)); return;
    }
    int chosen = -1;
    if (!hits.value->empty()) {
        std::size_t next = 0;
        const auto again = point - lastClickPoint_;
        if (selectedInstance_ && std::abs(again.x()) + std::abs(again.y()) < 3) {
            const auto current = std::find_if(hits.value->begin(), hits.value->end(), [&](const auto& hit) { return hit.instance == *selectedInstance_; });
            if (current != hits.value->end()) next = static_cast<std::size_t>(current - hits.value->begin() + 1) % hits.value->size();
        }
        chosen = static_cast<int>((*hits.value)[next].instance);
    }
    lastClickPoint_ = point;
    selectedCoordinate_ = picked.value->hit;
    auto selection = selectedCoordinate_ ? tr("已选：%1").arg(CoordinateText(*selectedCoordinate_)) : tr("未选择地形坐标");
    if (!hits.diagnostics.empty()) selection += QStringLiteral(" · ") + PickDiagnostic(hits.diagnostics);
    SetCoordinateTexts(hoverText_, std::move(selection));
    if (chosen >= 0) emit InstanceClicked(chosen, static_cast<int>(modifiers));
    else if (selectedCoordinate_)
        emit GroundClicked(selectedCoordinate_->position[0], selectedCoordinate_->position[1], static_cast<int>(modifiers));
    else emit InstanceClicked(-1, static_cast<int>(modifiers));
}

void WorldViewport::FinishSelectionBox(QPointF point) {
    if (!pressView_ || pressRevision_ != revision_ || !models_) return;
    const auto frame = ReadPickingFrame();
    if (!frame.Succeeded()) {
        if (PickPending(frame.diagnostics)) (void)DeferSelection(point, selectionModifiers_, true);
        return;
    }
    if (!SameView(*pressView_, frame.value->identity)) {
        ClearCoordinates(tr("按下后视图已改变，本次框选已取消")); return;
    }
    // 框选按摆放中心的屏幕投影判定（与原版一致），只依赖当前完成帧的视图矩阵与显隐/锁定标志；
    // 不要求动画姿势回执，动画失败或静态回退时同样可用。
    const QRectF rectangle = QRectF(pressPosition_, point).normalized();
    QVector<int> selected;
    for (std::size_t index = 0; index < models_->instances.size(); ++index) {
        if (!IsInstanceSelectable(static_cast<std::uint32_t>(index))) continue;
        const auto center = ProjectInstanceCenter(static_cast<std::uint32_t>(index), *frame.value);
        if (center && rectangle.contains(QPointF((*center)[0], (*center)[1])))
            selected.push_back(static_cast<int>(index));
    }
    emit InstancesBoxSelected(selected, static_cast<int>(selectionModifiers_));
}

} // namespace War3Map::Modules
