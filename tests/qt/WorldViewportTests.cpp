#include "WorldViewport.h"
#include "WorldAnimationPlayer.h"
#include "WorldAnimationViewContractChecks.h"
#include "WorldAnimationPlaybackContractChecks.h"
#include "../AnimationTestModel.h"
#include "EditorPresentationFixture.h"

#include <QApplication>
#include <QMouseEvent>
#include <QSignalSpy>
#include <QTest>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string_view>
#include <vector>

using namespace War3Map;
using namespace War3Map::Modules;

namespace {
struct BackendState {
    bool rejectCreate = false;
    std::int32_t attachResult = 0, meshResult = 0, resizeResult = 0, statusCallResult = 0, statusResult = 0;
    std::int32_t viewInfoResult = 0;
    bool emptyStatus = false, unterminatedStatus = false;
    EditorPresentationState editor;
    std::uint32_t width = 0, height = 0;
    std::uint64_t revision = 0, frameCount = 0;
    std::uint64_t surfaceEpoch = 1, sceneEpoch = 1, viewEpoch = 1;
    War3RenderCameraV2 camera{sizeof(War3RenderCameraV2), 0, 0, 0, 2048, 0, 0.8f};
    bool rejectCamera = false, rejectModels = false, staleCaptureEpoch = false, staleCaptureScene = false;
    bool staleCaptureCamera = false, timeoutCapture = false, modelUploadFailed = false;
    int captures = 0;
    std::uint32_t modelInstances = 0, updatedModelInstances = 0;
    std::uint64_t skinScene = 0, skinContent = 0, skinUploads = 0, matrixUploads = 0;
    bool staleSkin = false;
    bool redrawOnCompleted = false;
    bool freezeCompletedDraw = false;
    War3RenderParticleStatusV2 particles{sizeof(War3RenderParticleStatusV2)};
    bool staleParticles = false;
    int nextSkinUpdateDelayMs = 0;
    std::uint64_t layersRevision = 1;
    std::vector<War3RenderModelLayerStatusV2> layers;
    int layeredCaptures = 0;
    bool shortLayerWorld = false, shortLayerSkin = false, inactiveLayerSkin = false;
};
BackendState* active = nullptr;

War3RendererHandle WAR3_RENDER_CALL Create(const War3RenderCreateInfoV2* info) {
    if (active->rejectCreate) return nullptr;
    active->width = info->width; active->height = info->height;
    return active;
}
void WAR3_RENDER_CALL Destroy(War3RendererHandle) {}
std::int32_t WAR3_RENDER_CALL Attach(War3RendererHandle handle, void* window) {
    if (static_cast<BackendState*>(handle)->attachResult) return static_cast<BackendState*>(handle)->attachResult;
    ++static_cast<BackendState*>(handle)->surfaceEpoch;
    return window ? 0 : 1;
}
std::int32_t WAR3_RENDER_CALL Resize(War3RendererHandle handle, std::uint32_t width, std::uint32_t height) {
    auto& state = *static_cast<BackendState*>(handle);
    if (state.resizeResult) return state.resizeResult;
    state.width = width; state.height = height; ++state.surfaceEpoch;
    return 0;
}
std::int32_t WAR3_RENDER_CALL Mesh(War3RendererHandle handle, const War3RenderMeshV2* mesh) {
    auto& state = *static_cast<BackendState*>(handle);
    if (state.meshResult) return state.meshResult;
    state.revision = mesh->documentRevision; ++state.sceneEpoch;
    return 0;
}
std::int32_t WAR3_RENDER_CALL Camera(War3RendererHandle handle, const War3RenderCameraV2* camera) {
    auto& state = *static_cast<BackendState*>(handle);
    if (state.rejectCamera) return 1;
    state.camera = *camera; ++state.viewEpoch;
    return 0;
}
std::int32_t WAR3_RENDER_CALL ModelScene(War3RendererHandle handle, const War3RenderModelSceneV2* scene) {
    auto& state = *static_cast<BackendState*>(handle);
    if (state.rejectModels) return 1;
    state.modelInstances = scene->instanceCount;
    return 0;
}
std::int32_t WAR3_RENDER_CALL ModelInstances(War3RendererHandle handle,
    const War3RenderModelInstanceV2*, std::uint32_t count) {
    static_cast<BackendState*>(handle)->updatedModelInstances = count;
    return 0;
}
void WAR3_RENDER_CALL Frame(War3RendererHandle handle) {
    auto& state = *static_cast<BackendState*>(handle); ++state.frameCount;
    if (!state.freezeCompletedDraw) EditorPresentationFixture<BackendState>::Draw(handle);
}
std::int32_t WAR3_RENDER_CALL Status(War3RendererHandle handle, War3RenderStatusV2* output) {
    const auto& state = *static_cast<BackendState*>(handle);
    output->result = state.statusResult; output->deviceReady = 1; output->width = state.width; output->height = state.height;
    output->frameCount = state.frameCount; output->documentRevision = state.revision;
    constexpr char success[] = "D3D11 WARP / Feature Level 11.0 / model ready";
    constexpr char failure[] = "Model upload failed: previous models retained";
    if (state.emptyStatus) output->message[0] = '\0';
    else if (state.unterminatedStatus) std::memset(output->message, 'x', sizeof(output->message));
    else if (state.modelUploadFailed) std::memcpy(output->message, failure, sizeof(failure));
    else std::memcpy(output->message, success, sizeof(success));
    return state.statusCallResult;
}
std::int32_t WAR3_RENDER_CALL LegacyCapture(War3RendererHandle, void*, std::uint64_t, War3RenderCaptureInfoV2*) { return 1; }
War3RenderViewInfoV2 View(const BackendState& state) {
    War3RenderViewInfoV2 view{sizeof(War3RenderViewInfoV2), state.width, state.height,
        state.frameCount, state.revision, state.surfaceEpoch, state.sceneEpoch, state.viewEpoch, {}};
    view.viewProjection[0] = view.viewProjection[5] = view.viewProjection[10] = view.viewProjection[15] = 1;
    return view;
}
std::int32_t WAR3_RENDER_CALL ViewInfo(War3RendererHandle handle, War3RenderViewInfoV2* output) {
    const auto& state = *static_cast<BackendState*>(handle);
    if (state.viewInfoResult != 0) return state.viewInfoResult;
    *output = View(state);
    return 0;
}
std::int32_t WAR3_RENDER_CALL CaptureFrame(War3RendererHandle handle, void* rgba, std::uint64_t bytes,
    War3RenderCapturedFrameV2* output) {
    auto& state = *static_cast<BackendState*>(handle);
    ++state.captures;
    if (state.timeoutCapture) return 2;
    const auto needed = static_cast<std::uint64_t>(state.width) * state.height * 4;
    if (bytes < needed) return 1;
    std::memset(rgba, 42, static_cast<std::size_t>(needed));
    ++state.frameCount;
    output->structSize = sizeof(*output);
    output->capture = {sizeof(War3RenderCaptureInfoV2), state.width, state.height, state.frameCount, state.revision, 1};
    output->view = View(state);
    if (state.staleCaptureEpoch) ++output->view.viewEpoch;
    if (state.staleCaptureScene) ++output->view.sceneEpoch;
    output->camera = state.camera;
    if (state.staleCaptureCamera) ++output->camera.targetX;
    return 0;
}
// 仅验证 UI/服务/同帧回执接线，不把这些填充像素当成 WARP 画面证据。
std::int32_t WAR3_RENDER_CALL SkinScene(War3RendererHandle h, const War3RenderSkinnedModelSceneV2* input) {
    auto& s = *static_cast<BackendState*>(h);
    if (s.rejectModels || !input || !input->initialPose) return 1;
    s.skinScene = input->initialPose->sceneVersion; s.skinContent = input->initialPose->contentVersion;
    ++s.skinUploads; ++s.matrixUploads; ++s.sceneEpoch;
    return 0;
}
std::int32_t WAR3_RENDER_CALL SkinUpdate(War3RendererHandle h, const War3RenderSkinUpdateV2* input) {
    auto& s = *static_cast<BackendState*>(h);
    if (s.rejectModels || input->sceneVersion != s.skinScene || input->contentVersion <= s.skinContent) return 1;
    const auto delay = s.nextSkinUpdateDelayMs; s.nextSkinUpdateDelayMs = 0;
    if (delay) QTest::qSleep(delay); // 一次受控慢提交，覆盖 GUI 预算暂停而非依赖机器速度。
    // 生产 UpdateSkinMatrices 只推进 pose/content 和 request；动态姿势不改变地形 sceneEpoch。
    // 完成帧可以落后 CPU 一个内容版本，但仍属于同一静态视图和拥有的场景。
    s.skinContent = input->contentVersion; ++s.matrixUploads;
    return 0;
}
std::int32_t WAR3_RENDER_CALL SkinStatus(War3RendererHandle h, War3RenderSkinStatusV2* out) {
    const auto& s = *static_cast<BackendState*>(h);
    *out = {sizeof(*out), 0, s.skinScene ? 1u : 0u, 0, s.frameCount,
        s.skinScene, s.skinContent, s.skinUploads, s.matrixUploads};
    return 0;
}
std::int32_t WAR3_RENDER_CALL SkinCapture(War3RendererHandle h, void* rgba, std::uint64_t bytes,
    War3RenderSkinnedCapturedFrameV2* out) {
    const auto code = CaptureFrame(h, rgba, bytes, &out->frame);
    if (code) return code;
    out->structSize = sizeof(*out);
    (void)SkinStatus(h, &out->skin);
    if (static_cast<BackendState*>(h)->staleSkin) ++out->skin.contentVersion;
    return 0;
}
std::int32_t WAR3_RENDER_CALL ParticleScene(War3RendererHandle h, const War3RenderParticleSceneV2* input) {
    if (SkinScene(h,input->models)) return 1;
    auto& p=static_cast<BackendState*>(h)->particles; const auto& f=*input->initialFrame;
    p={sizeof(p),f.vertexCount,f.drawCount,0,f.sceneVersion,f.contentVersion,input->textureCount?1u:0u,1}; return 0;
}
std::int32_t WAR3_RENDER_CALL ParticleUpdate(War3RendererHandle h, const War3RenderParticleUpdateV2* input) {
    if (SkinUpdate(h,input->modelPose)) return 1;
    auto& p=static_cast<BackendState*>(h)->particles;
    p.vertexCount=input->vertexCount; p.drawCount=input->drawCount; p.contentVersion=input->contentVersion;
    ++p.dynamicUploadCount; return 0;
}
std::int32_t WAR3_RENDER_CALL ParticleCapture(War3RendererHandle h, void* rgba, std::uint64_t bytes,
    War3RenderParticleCapturedFrameV2* out) {
    if (SkinCapture(h,rgba,bytes,&out->models)) return 1;
    const auto& s=*static_cast<BackendState*>(h); out->structSize=sizeof(*out); out->particles=s.particles;
    if (s.staleParticles) ++out->particles.contentVersion;
    return 0;
}
std::int32_t WAR3_RENDER_CALL LayersTarget(War3RendererHandle h, War3RenderModelLayersTargetV2* out) {
    const auto& s = *static_cast<BackendState*>(h);
    *out = {sizeof(*out), static_cast<std::uint32_t>(s.layers.size()), s.editor.target.modelEpoch, s.layersRevision};
    return 0;
}
std::int32_t WAR3_RENDER_CALL SubmitLayer(War3RendererHandle h, const War3RenderModelSceneLayerV2* in) {
    auto& s = *static_cast<BackendState*>(h);
    const auto* pose = in->models ? in->models->initialPose : in->particles->models->initialPose;
    War3RenderModelLayerStatusV2 layer{};
    layer.structSize = sizeof(layer); layer.layerId = in->layerId;
    layer.parentModelEpoch = in->parentModelEpoch; layer.revision = in->revision;
    layer.skin = {sizeof(layer.skin), 0, 1, 0, 0, pose->sceneVersion, pose->contentVersion, 1, 1};
    s.layers.push_back(layer); ++s.layersRevision; return 0;
}
std::int32_t WAR3_RENDER_CALL UpdateLayer(War3RendererHandle, const War3RenderModelLayerUpdateV2*) { return 1; }
std::int32_t WAR3_RENDER_CALL RemoveLayer(War3RendererHandle, std::uint64_t, std::uint64_t, std::uint64_t) { return 1; }
std::int32_t WAR3_RENDER_CALL CaptureLayers(War3RendererHandle h, std::uint64_t revision,
    void* rgba, std::uint64_t bytes, War3RenderLayeredCapturedFrameV2* out) {
    auto& s = *static_cast<BackendState*>(h); ++s.layeredCaptures;
    if (revision != s.layersRevision) return 2;
    out->structSize = sizeof(*out); out->layerCount = static_cast<std::uint32_t>(s.layers.size());
    out->layersRevision = revision; out->world.structSize = sizeof(out->world);
    const auto result = s.editor.api ? EditorPresentationFixture<BackendState>::Capture(h,
        s.editor.target.modelEpoch, s.editor.presentation.presentationRevision, rgba, bytes, &out->world)
        : CaptureFrame(h, rgba, bytes, &out->world.frame);
    if (result) return result;
    for (std::size_t i = 0; i < s.layers.size(); ++i) {
        out->layers[i] = s.layers[i]; out->layers[i].skin.frameCount = out->world.frame.capture.frameCount;
        if (s.shortLayerSkin) out->layers[i].skin.structSize = 4;
        if (s.inactiveLayerSkin) out->layers[i].skin.active = 0;
    }
    if (s.shortLayerWorld) out->world.structSize = 4;
    return 0;
}
void EnableLayers(War3RenderPluginApiV2& api) {
    api.submitModelSceneLayer = SubmitLayer; api.updateModelSceneLayer = UpdateLayer;
    api.removeModelSceneLayer = RemoveLayer; api.getModelLayersTarget = LayersTarget;
    api.captureLayeredFrameRgba = CaptureLayers;
}
const War3RenderPluginApiV2 backend{sizeof(War3RenderPluginApiV2), WAR3_RENDER_PLUGIN_API_V2,
    Create, Destroy, Attach, Resize, Mesh, Camera, Frame, Status, LegacyCapture, ViewInfo,
    nullptr, nullptr, CaptureFrame};

bool Bind(WorldViewport& viewport, std::uint64_t revision) {
    const std::vector<War3RenderVertexV2> vertices{{-64, -64, 0, 1, 1, 1}, {64, -64, 0, 1, 1, 1},
        {-64, 64, 0, 1, 1, 1}, {64, 64, 0, 1, 1, 1}};
    return viewport.SetMesh(vertices, {0, 1, 2, 1, 3, 2}, revision);
}
} // namespace

namespace War3Map::Modules {
// 只验证旁路历史的拥有权/预算；不创建模型回执，也不向生产接口开放可写帧。
class WorldViewportPickHistoryTestAccess final {
public:
    static void Append(WorldViewport& viewport, const std::shared_ptr<const Render::AnimationFrame>& frame) {
        if (!viewport.animationScene_) viewport.animationScene_ = std::make_shared<Render::AnimatedModelScene>();
        viewport.animationFrame_ = frame;
        viewport.editorPresentationTarget_.emplace();
        viewport.editorPresentationTarget_->sceneVersion = frame->sceneVersion;
        viewport.editorPresentationTarget_->contentVersion = frame->contentVersion;
        viewport.editorPresentationRevision_ = 1;
        viewport.RetainAnimationPickFrame();
    }
    static std::size_t Count(const WorldViewport& viewport) { return viewport.animationPickHistory_.size(); }
    static std::size_t Bytes(const WorldViewport& viewport) {
        std::size_t total = 0;
        for (const auto& retained : viewport.animationPickHistory_) total += retained.ownedBytes;
        return total;
    }
    static std::size_t LastBytes(const WorldViewport& viewport) { return viewport.animationPickHistory_.back().ownedBytes; }
    static std::shared_ptr<const Render::AnimationFrame> Borrow(const WorldViewport& viewport, std::size_t index) {
        return viewport.animationPickHistory_.at(index).frame;
    }
    static std::weak_ptr<const Render::AnimationFrame> Observe(const WorldViewport& viewport, std::size_t index) {
        return viewport.animationPickHistory_.at(index).frame;
    }
};
} // namespace War3Map::Modules

class WorldViewportTests : public QObject {
    Q_OBJECT
private slots:
    void PluginFailuresPreserveOperationAndBoundedRawStatus_data();
    void PluginFailuresPreserveOperationAndBoundedRawStatus();
    void OptionalCapabilitiesAndPhysicalLimitDoNotEmitPluginFailure();
    void CaptureViewPendingKeepsStaleResultWithoutProgramFailure_data();
    void CaptureViewPendingKeepsStaleResultWithoutProgramFailure();
    void CameraAndCompletedFrameStayBound();
    void CaptureRejectsUnsupportedStaleAndBudget();
    void LayeredCaptureBindsAdditionalLayersAndRejectsMalformedReceipts_data();
    void LayeredCaptureBindsAdditionalLayersAndRejectsMalformedReceipts();
    void ModelOnlyCompletedFrameKeepsSceneAndCameraBinding();
    void RejectedModelSceneKeepsCpuSceneAndSelection();
    void AnimationPauseStepAndCaptureStayBound();
    void AnimationPickHistoryReusesOnlyRetiredUnborrowedStorage();
    void AnimationPickHistoryChargesRetainedCapacity();
};

void WorldViewportTests::PluginFailuresPreserveOperationAndBoundedRawStatus_data() {
    QTest::addColumn<QString>("operation");
    QTest::addColumn<bool>("specificMessage");
    for (const auto* operation : {"createRenderer", "attachNativeWindow", "submitMesh", "getStatus", "resize", "setCamera"})
        QTest::newRow(operation) << QString::fromLatin1(operation) << false;
    QTest::newRow("bounded-unterminated-message") << QStringLiteral("getStatus") << true;
}

void WorldViewportTests::PluginFailuresPreserveOperationAndBoundedRawStatus() {
    QFETCH(QString, operation);
    QFETCH(bool, specificMessage);
    BackendState state; active = &state;
    state.emptyStatus = !specificMessage;
    state.unterminatedStatus = specificMessage;
    if (operation == QStringLiteral("createRenderer")) state.rejectCreate = true;
    if (operation == QStringLiteral("attachNativeWindow")) state.attachResult = -11;
    if (operation == QStringLiteral("submitMesh")) state.meshResult = -12;
    if (operation == QStringLiteral("getStatus")) { state.statusCallResult = -13; state.statusResult = -14; }
    WorldViewport viewport(nullptr, &backend);
    QSignalSpy failures(&viewport, &WorldViewport::PluginFailed);
    viewport.resize(320, 240);
    QVERIFY(Bind(viewport, 17));
    viewport.show(); QApplication::processEvents();
    if (operation == QStringLiteral("resize")) {
        state.resizeResult = -15;
        viewport.resize(400, 260); QApplication::processEvents();
    }
    if (operation == QStringLiteral("setCamera")) {
        state.rejectCamera = true;
        auto camera = viewport.Camera(); camera.targetX = 30;
        QVERIFY(!viewport.SetCamera(camera, 17).Succeeded());
    }
    QVERIFY(!failures.isEmpty());
    bool found = false;
    for (const auto& signal : failures) {
        const auto failure = qvariant_cast<RenderPluginFailure>(signal[0]);
        if (failure.operation != operation) continue;
        found = true;
        QCOMPARE(failure.result, operation == QStringLiteral("createRenderer") ? -1
            : operation == QStringLiteral("attachNativeWindow") ? -11
            : operation == QStringLiteral("submitMesh") ? -12
            : operation == QStringLiteral("getStatus") ? -13
            : operation == QStringLiteral("resize") ? -15 : 1);
        QCOMPARE(failure.statusAvailable, operation != QStringLiteral("createRenderer"));
        if (operation == QStringLiteral("getStatus")) {
            QCOMPARE(failure.statusCallResult, -13);
            QCOMPARE(failure.statusResult, -14);
        }
        if (specificMessage) QCOMPARE(failure.message.size(), qsizetype{sizeof(War3RenderStatusV2{}.message)});
        else QVERIFY(failure.message.isEmpty());
    }
    QVERIFY(found);
}

void WorldViewportTests::OptionalCapabilitiesAndPhysicalLimitDoNotEmitPluginFailure() {
    BackendState state; active = &state;
    auto unsupported = backend;
    unsupported.setEnvironment = nullptr;
    WorldViewport viewport(nullptr, &unsupported);
    QSignalSpy failures(&viewport, &WorldViewport::PluginFailed);
    viewport.resize(5000, 200); QVERIFY(Bind(viewport, 17));
    viewport.show(); QApplication::processEvents();
    QVERIFY(!viewport.RendererReady());
    QCOMPARE(failures.count(), 0);
    viewport.resize(320, 240); QApplication::processEvents();
    QVERIFY(viewport.RendererReady());
    QVERIFY(!viewport.SetEnvironment(std::make_shared<Render::SceneEnvironment>()));
    QCOMPARE(failures.count(), 0);
}

void WorldViewportTests::CaptureViewPendingKeepsStaleResultWithoutProgramFailure_data() {
    QTest::addColumn<int>("viewResult");
    QTest::addColumn<bool>("captured");
    QTest::addColumn<bool>("programFailure");
    QTest::newRow("pending-view-after-readback") << 2 << false << false;
    QTest::newRow("real-view-failure-after-readback") << -19 << false << true;
    QTest::newRow("matching-completed-view") << 0 << true << false;
}

void WorldViewportTests::CaptureViewPendingKeepsStaleResultWithoutProgramFailure() {
    QFETCH(int, viewResult);
    QFETCH(bool, captured);
    QFETCH(bool, programFailure);
    BackendState state; active = &state; // 状态接口仍给真实 stub 的旧成功状态串。
    WorldViewport viewport(nullptr, &backend);
    viewport.resize(320, 240); QVERIFY(Bind(viewport, 17));
    viewport.show(); QApplication::processEvents();
    QVERIFY(viewport.RendererReady());
    QSignalSpy failures(&viewport, &WorldViewport::PluginFailed);
    state.viewInfoResult = viewResult;
    const auto result = viewport.CaptureCompletedFrame(17);
    QCOMPARE(result.Succeeded(), captured);
    QCOMPARE(state.captures, 1); // 确实在完整图像读回之后进入最后的 getViewInfo 复核。
    if (!captured) QCOMPARE(result.diagnostics.front().code, std::string("WORLD_CAPTURE_FRAME_STALE"));
    QCOMPARE(failures.count(), programFailure ? 1 : 0);
    if (programFailure) {
        const auto failure = qvariant_cast<RenderPluginFailure>(failures.front()[0]);
        QCOMPARE(failure.operation, QStringLiteral("getViewInfo"));
        QCOMPARE(failure.result, viewResult);
        QVERIFY(failure.message.startsWith(QStringLiteral("D3D11 WARP / Feature Level 11.0 / ")));
    }
}

void WorldViewportTests::AnimationPickHistoryReusesOnlyRetiredUnborrowedStorage() {
    using History = WorldViewportPickHistoryTestAccess;
    BackendState state; active = &state;
    WorldViewport viewport(nullptr, &backend);
    auto source = std::make_shared<Render::AnimationFrame>();
    source->sceneVersion = 7; source->documentRevision = 11;
    source->states.resize(8); source->poses.resize(8); source->particleLinks.resize(8);
    source->particleVertices.resize(12); source->particleDraws.resize(1);
    const auto append = [&](std::uint64_t version) {
        source->contentVersion = version;
        source->states[0].elapsedMs = version;
        source->poses[0].rejectionMessage.assign(128, static_cast<char>('a' + version));
        source->particleLinks[0].resetReason.assign(128, static_cast<char>('a' + version));
        History::Append(viewport, source);
    };
    append(1);
    auto oldest = History::Observe(viewport, 0);
    const auto* oldAddress = oldest.lock().get();
    const auto* oldStates = oldest.lock()->states.data();
    const auto* oldPoseText = oldest.lock()->poses[0].rejectionMessage.data();
    const auto* oldParticleText = oldest.lock()->particleLinks[0].resetReason.data();
    for (std::uint64_t version = 2; version <= 4; ++version) append(version);
    QCOMPARE(History::Count(viewport), std::size_t{4});
    QCOMPARE(oldest.lock()->contentVersion, std::uint64_t{1});
    QCOMPARE(oldest.lock()->states[0].elapsedMs, std::uint64_t{1}); // 拷贝拥有，不借当前源帧。
    append(5);
    auto reused = History::Borrow(viewport, 3);
    QCOMPARE(reused.get(), oldAddress);
    QCOMPARE(reused->states.data(), oldStates);
    QVERIFY(reused->poses[0].rejectionMessage.data() == oldPoseText);
    QVERIFY(reused->particleLinks[0].resetReason.data() == oldParticleText);
    QCOMPARE(reused->contentVersion, std::uint64_t{5});
    QCOMPARE(reused->states[0].elapsedMs, std::uint64_t{5});
    QVERIFY(reused->particleVertices.empty()); QVERIFY(reused->particleDraws.empty());
    QVERIFY(!reused->particleCheckpoint); // 不通过保留源帧间接保留粒子状态。
    reused.reset();

    const auto borrowed = History::Borrow(viewport, 0); // 实际点选的 const 借用尚未归还。
    const auto oldMessage = borrowed->poses[0].rejectionMessage;
    append(6);
    QCOMPARE(History::Count(viewport), std::size_t{4});
    QVERIFY(History::Borrow(viewport, 3).get() != borrowed.get());
    QCOMPARE(borrowed->contentVersion, std::uint64_t{2});
    QCOMPARE(borrowed->states[0].elapsedMs, std::uint64_t{2});
    QCOMPARE(borrowed->poses[0].rejectionMessage, oldMessage);
    append(6); QCOMPARE(History::Count(viewport), std::size_t{4}); // 同身份不添加重复历史。
    viewport.InvalidateAnimationPicking();
    QCOMPARE(History::Count(viewport), std::size_t{0});
    QVERIFY(oldest.expired()); // 没有脱离失效机制的备用池。
    append(7); QCOMPARE(History::Count(viewport), std::size_t{0});
    QCOMPARE(borrowed->contentVersion, std::uint64_t{2}); // 失效不改已借出的拥有数据。
}

void WorldViewportTests::AnimationPickHistoryChargesRetainedCapacity() {
    using History = WorldViewportPickHistoryTestAccess;
    constexpr std::size_t mib = 1024 * 1024, budget = 64 * mib;
    BackendState state; active = &state;
    WorldViewport viewport(nullptr, &backend);
    auto source = std::make_shared<Render::AnimationFrame>();
    source->sceneVersion = 7; source->documentRevision = 11;
    source->groups.resize(32 * mib / sizeof(War3RenderGroupMatrixV2));
    source->poses.resize(1); source->poses[0].rejectionMessage.assign(256, 'x');
    const auto append = [&](std::uint64_t version) { source->contentVersion = version; History::Append(viewport, source); };
    append(1);
    auto largeOwner = History::Observe(viewport, 0);
    const auto oldGroupCapacity = largeOwner.lock()->groups.capacity();
    const auto oldStringCapacity = largeOwner.lock()->poses[0].rejectionMessage.capacity();
    source->groups.resize(1); source->poses[0].rejectionMessage.assign(1, 's');
    for (std::uint64_t version = 2; version <= 5; ++version) append(version);
    auto smallWithCapacity = History::Borrow(viewport, History::Count(viewport) - 1);
    QCOMPARE(smallWithCapacity.get(), largeOwner.lock().get());
    QCOMPARE(smallWithCapacity->groups.size(), std::size_t{1});
    QCOMPARE(smallWithCapacity->groups.capacity(), oldGroupCapacity);
    QCOMPARE(smallWithCapacity->poses[0].rejectionMessage.size(), std::size_t{1});
    QCOMPARE(smallWithCapacity->poses[0].rejectionMessage.capacity(), oldStringCapacity);
    QVERIFY(History::LastBytes(viewport) >= sizeof(Render::AnimationFrame) +
        oldGroupCapacity * sizeof(War3RenderGroupMatrixV2) +
        smallWithCapacity->poses.capacity() * sizeof(Render::AnimationPoseLink) + oldStringCapacity + 1);
    smallWithCapacity.reset();
    source->groups.resize(20 * mib / sizeof(War3RenderGroupMatrixV2));
    append(6); append(7);
    QVERIFY(History::Bytes(viewport) <= budget);
    QVERIFY(History::Count(viewport) < 4); // 32MiB备用容量不能按当前一个矩阵漏算。
    QVERIFY(largeOwner.expired());
    source->groups.resize(budget / sizeof(War3RenderGroupMatrixV2) + 1);
    append(8);
    QCOMPARE(History::Count(viewport), std::size_t{0});
    QCOMPARE(viewport.AnimationTarget().get(), source.get()); // 拒绝旁路历史，不丢绘制源帧。
}

void WorldViewportTests::CameraAndCompletedFrameStayBound() {
    BackendState state; active = &state;
    WorldViewport viewport(nullptr, &backend);
    viewport.resize(320, 240);
    QVERIFY(Bind(viewport, 17));
    viewport.show(); QApplication::processEvents();
    auto requested = viewport.Camera();
    requested.targetX = 72; requested.targetY = -16; requested.distance = 1400;
    const auto set = viewport.SetCamera(requested, 17);
    QVERIFY(set.Succeeded());
    QCOMPARE(viewport.Camera().targetX, 72.0f);
    const auto read = viewport.ReadCamera(17);
    QVERIFY(read.Succeeded()); QCOMPARE(read.value->targetX, requested.targetX);
    QVERIFY(!viewport.ReadCamera(18).Succeeded());
    const auto captured = viewport.CaptureCompletedFrame(17);
    QVERIFY(captured.Succeeded());
    QCOMPARE(captured.value->documentRevision, std::uint64_t{17});
    QCOMPARE(captured.value->view.physicalWidth, state.width);
    QCOMPARE(captured.value->view.physicalHeight, state.height);
    QCOMPARE(captured.value->view.identity.sceneEpoch, state.sceneEpoch);
    QCOMPARE(captured.value->view.identity.viewEpoch, state.viewEpoch);
    QCOMPARE(captured.value->camera.targetX, requested.targetX);
    QCOMPARE(captured.value->camera.distance, requested.distance);
    QCOMPARE(captured.value->rgba.size(), static_cast<std::size_t>(state.width) * state.height * 4);
    QVERIFY(std::all_of(captured.value->rgba.begin(), captured.value->rgba.end(), [](std::byte pixel) {
        return pixel == std::byte{42};
    }));
    QCOMPARE(state.captures, 1);
    auto invalid = requested;
    invalid.distance = std::numeric_limits<float>::infinity();
    QVERIFY(!viewport.SetCamera(invalid, 17).Succeeded());
    QCOMPARE(viewport.Camera().distance, requested.distance);
    QVERIFY(!viewport.SetCamera(requested, 18).Succeeded());
    state.rejectCamera = true;
    requested.targetX = 90;
    const auto rejected = viewport.SetCamera(requested, 17);
    QVERIFY(!rejected.Succeeded());
    QCOMPARE(rejected.diagnostics.front().code, std::string("WORLD_CAMERA_REJECTED"));
    QCOMPARE(viewport.Camera().targetX, 72.0f);
}

void WorldViewportTests::CaptureRejectsUnsupportedStaleAndBudget() {
    BackendState state; active = &state;
    auto legacy = backend;
    legacy.structSize = static_cast<std::uint32_t>(offsetof(War3RenderPluginApiV2, captureFrameRgba));
    {
        WorldViewport viewport(nullptr, &legacy);
        viewport.resize(320, 240);
        QVERIFY(Bind(viewport, 5));
        viewport.show(); QApplication::processEvents();
        const auto unsupported = viewport.CaptureCompletedFrame(5);
        QVERIFY(!unsupported.Succeeded());
        QCOMPARE(unsupported.diagnostics.front().code, std::string("WORLD_CAPTURE_UNSUPPORTED"));
    }
    WorldViewport viewport(nullptr, &backend);
    viewport.resize(320, 240);
    QVERIFY(Bind(viewport, 5));
    viewport.show(); QApplication::processEvents();
    const auto wrongRevision = viewport.CaptureCompletedFrame(6);
    QVERIFY(!wrongRevision.Succeeded());
    QCOMPARE(wrongRevision.diagnostics.front().code, std::string("WORLD_CAPTURE_REVISION"));
    const auto overBudget = viewport.CaptureCompletedFrame(5, 100);
    QVERIFY(!overBudget.Succeeded());
    QCOMPARE(overBudget.diagnostics.front().code, std::string("WORLD_CAPTURE_BUDGET"));
    QCOMPARE(state.captures, 0);
    state.timeoutCapture = true;
    const auto timeout = viewport.CaptureCompletedFrame(5);
    QVERIFY(!timeout.Succeeded());
    QCOMPARE(timeout.diagnostics.front().code, std::string("WORLD_CAPTURE_STALE_OR_TIMEOUT"));
    state.timeoutCapture = false; state.staleCaptureEpoch = true;
    const auto stale = viewport.CaptureCompletedFrame(5);
    QVERIFY(!stale.Succeeded());
    QCOMPARE(stale.diagnostics.front().code, std::string("WORLD_CAPTURE_FRAME_STALE"));
}

void WorldViewportTests::LayeredCaptureBindsAdditionalLayersAndRejectsMalformedReceipts_data() {
    QTest::addColumn<qulonglong>("sourceRevision");
    QTest::newRow("viewport23-source0") << qulonglong{0};
    QTest::newRow("viewport23-source23") << qulonglong{23};
    QTest::newRow("viewport23-source32") << qulonglong{32};
}
void WorldViewportTests::LayeredCaptureBindsAdditionalLayersAndRejectsMalformedReceipts() {
    QFETCH(qulonglong, sourceRevision);
    BackendState state; active = &state;
    auto api = backend; EnableLayers(api);
    WorldViewport viewport(nullptr, &api);
    viewport.resize(320, 240); QVERIFY(Bind(viewport, 23));
    viewport.show(); QApplication::processEvents();
    auto scene = std::make_shared<Render::AnimatedModelScene>();
    scene->sceneVersion = 37; scene->documentRevision = sourceRevision;
    scene->generation = 5; scene->sourceIdentity = "fixture-source-sha";
    auto pose = std::make_shared<Render::AnimationFrame>();
    pose->sceneVersion = 37; pose->contentVersion = 41; pose->documentRevision = sourceRevision;
    QVERIFY(viewport.SetModelLayer(7, scene, pose).Succeeded());
    const auto captured = viewport.CaptureCompletedFrame(23);
    QVERIFY(captured.Succeeded()); QCOMPARE(state.layeredCaptures, 1);
    QCOMPARE(captured.value->modelLayersRevision, state.layersRevision);
    QCOMPARE(captured.value->modelLayers.size(), std::size_t{1});
    QCOMPARE(captured.value->modelLayers.front().layerId, std::uint64_t{7});
    QCOMPARE(captured.value->modelLayers.front().skin.frameCount, captured.value->view.identity.frameCount);
    QCOMPARE(captured.value->modelLayerFrames.front().get(), pose.get());
    QCOMPARE(captured.value->documentRevision, std::uint64_t{23});
    QCOMPARE(captured.value->modelLayerFrames.front()->documentRevision, std::uint64_t{sourceRevision});
    // 不同编号空间合法，但同一来源的错误修订不能进入桥；失败保留原绑定。
    auto wrongSource = std::make_shared<Render::AnimationFrame>(*pose);
    ++wrongSource->documentRevision; ++wrongSource->contentVersion;
    QVERIFY(!viewport.SetModelLayer(7, scene, wrongSource).Succeeded());
    QVERIFY(!viewport.UpdateModelLayer(7, wrongSource).Succeeded());
    // 畸形回执必须拒绝，不能退回不含选择圈的普通帧。
    const auto rejected = [&] {
        const auto result = viewport.CaptureCompletedFrame(23);
        return !result.Succeeded() && result.diagnostics.front().code == "WORLD_CAPTURE_LAYER_STALE";
    };
    state.shortLayerWorld = true; QVERIFY(rejected()); state.shortLayerWorld = false;
    state.shortLayerSkin = true; QVERIFY(rejected()); state.shortLayerSkin = false;
    state.inactiveLayerSkin = true; QVERIFY(rejected()); state.inactiveLayerSkin = false;
    ++state.layers.front().skin.contentVersion; QVERIFY(rejected()); --state.layers.front().skin.contentVersion;
    state.layers.push_back(state.layers.front()); QVERIFY(rejected()); state.layers.pop_back();
    ++scene->documentRevision; QVERIFY(rejected()); --scene->documentRevision;
    ++pose->documentRevision; QVERIFY(rejected()); --pose->documentRevision;
    ++scene->generation; QVERIFY(rejected()); --scene->generation;
    scene->sourceIdentity = "different-source-sha"; QVERIFY(rejected()); scene->sourceIdentity = "fixture-source-sha";
    state.staleCaptureCamera = true;
    QVERIFY(!viewport.CaptureCompletedFrame(23).Succeeded()); state.staleCaptureCamera = false;
    QVERIFY(viewport.CaptureCompletedFrame(23).Succeeded());
    QCOMPARE(state.layeredCaptures, state.captures); // 每次均经过层读回，无失败回退。
    // 父场景变化后旧层必须拒绝；插件清空该父的层后，宿主移除也释放拥有的来源。
    ++state.editor.target.modelEpoch;
    QVERIFY(rejected());
    state.layers.clear(); ++state.layersRevision;
    std::weak_ptr<const Render::AnimatedModelScene> oldSource = scene;
    scene.reset();
    QVERIFY(viewport.RemoveModelLayer(7).Succeeded());
    QVERIFY(oldSource.expired());
    const auto cleared = viewport.CaptureCompletedFrame(23);
    QVERIFY(cleared.Succeeded()); QVERIFY(cleared.value->modelLayers.empty());
}

void WorldViewportTests::ModelOnlyCompletedFrameKeepsSceneAndCameraBinding() {
    BackendState state; active = &state;
    auto modelBackend = backend;
    modelBackend.submitModelScene = ModelScene;
    WorldViewport viewport(nullptr, &modelBackend);
    viewport.resize(320, 240);
    viewport.ClearMesh(31);
    viewport.show(); QApplication::processEvents();
    QCOMPARE(viewport.VertexCount(), std::size_t{0});
    const auto empty = viewport.CaptureCompletedFrame(31);
    QVERIFY(!empty.Succeeded());
    QCOMPARE(empty.diagnostics.front().code, std::string("WORLD_CAPTURE_UNAVAILABLE"));
    QCOMPARE(state.captures, 0);

    auto scene = std::make_shared<Render::WorldModelScene>();
    scene->vertices = {{-1, -1, 0, 0, 0, 1, 0, 0}, {1, -1, 0, 0, 0, 1, 1, 0},
        {0, 1, 0, 0, 0, 1, 0.5f, 1}};
    scene->indices = {0, 1, 2};
    scene->layers = {{WAR3_RENDER_MODEL_NO_TEXTURE, 0, 0, 1}};
    scene->meshes = {{0, 3, 0, 0, 1}};
    scene->models = {{0, 1}};
    scene->instances = {{0, {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}, {1, 1, 1}}};
    QVERIFY(viewport.SetModelScene(scene));
    QCOMPARE(state.modelInstances, std::uint32_t{1});
    auto requested = viewport.Camera();
    requested.targetX = 9;
    QVERIFY(viewport.SetCamera(requested, 31).Succeeded());

    const auto captured = viewport.CaptureCompletedFrame(31);
    QVERIFY(captured.Succeeded());
    QCOMPARE(captured.value->documentRevision, std::uint64_t{31});
    QCOMPARE(captured.value->view.identity.sceneEpoch, state.sceneEpoch);
    QCOMPARE(captured.value->view.identity.viewEpoch, state.viewEpoch);
    QCOMPARE(captured.value->camera.targetX, requested.targetX);
    QCOMPARE(state.captures, 1);

    state.modelUploadFailed = true;
    const auto failedUpload = viewport.CaptureCompletedFrame(31);
    QVERIFY(!failedUpload.Succeeded());
    QCOMPARE(failedUpload.diagnostics.front().code, std::string("WORLD_CAPTURE_MODEL_UPLOAD"));
    state.modelUploadFailed = false;

    const auto wrongRevision = viewport.CaptureCompletedFrame(32);
    QVERIFY(!wrongRevision.Succeeded());
    QCOMPARE(wrongRevision.diagnostics.front().code, std::string("WORLD_CAPTURE_REVISION"));
    QCOMPARE(state.captures, 2);
    state.staleCaptureScene = true;
    const auto staleScene = viewport.CaptureCompletedFrame(31);
    QVERIFY(!staleScene.Succeeded());
    QCOMPARE(staleScene.diagnostics.front().code, std::string("WORLD_CAPTURE_FRAME_STALE"));
    state.staleCaptureScene = false;
    state.staleCaptureCamera = true;
    const auto staleCamera = viewport.CaptureCompletedFrame(31);
    QVERIFY(!staleCamera.Succeeded());
    QCOMPARE(staleCamera.diagnostics.front().code, std::string("WORLD_CAPTURE_FRAME_STALE"));
    state.staleCaptureCamera = false;

    QVERIFY(viewport.SetModelScene(nullptr));
    const auto cleared = viewport.CaptureCompletedFrame(31);
    QVERIFY(!cleared.Succeeded());
    QCOMPARE(cleared.diagnostics.front().code, std::string("WORLD_CAPTURE_UNAVAILABLE"));
}

void WorldViewportTests::RejectedModelSceneKeepsCpuSceneAndSelection() {
    BackendState state; active = &state;
    auto modelBackend = backend;
    modelBackend.submitModelScene = ModelScene;
    modelBackend.updateModelInstances = ModelInstances;
    auto original = std::make_shared<Render::WorldModelScene>();
    original->instances.resize(1);
    auto replacement = std::make_shared<Render::WorldModelScene>();
    replacement->instances.resize(2);

    WorldViewport viewport(nullptr, &modelBackend);
    viewport.resize(320, 240);
    QVERIFY(Bind(viewport, 19));
    viewport.show(); QApplication::processEvents();
    QVERIFY(viewport.SetModelScene(original));
    QCOMPARE(state.modelInstances, std::uint32_t{1});
    viewport.SetSelectedInstance(0);

    state.rejectModels = true;
    QVERIFY(!viewport.SetModelScene(replacement));
    QVERIFY(viewport.HasModelScene());
    QVERIFY(viewport.SelectedInstance().has_value());
    QCOMPARE(*viewport.SelectedInstance(), std::uint32_t{0});
    QCOMPARE(state.modelInstances, std::uint32_t{1});
    QVERIFY(!viewport.SetModelScene(nullptr));
    QVERIFY(viewport.HasModelScene());
    QVERIFY(viewport.SelectedInstance().has_value());

    state.rejectModels = false;
    QVERIFY(viewport.UpdateModelInstances());
    QCOMPARE(state.updatedModelInstances, std::uint32_t{1}); // 仍从旧 CPU 场景取实例。
    QVERIFY(viewport.SetModelScene(replacement));
    QCOMPARE(state.modelInstances, std::uint32_t{2});
    QVERIFY(!viewport.SelectedInstance().has_value());
    viewport.SetSelectedInstance(1);
    QVERIFY(viewport.SetModelScene(nullptr));
    QVERIFY(!viewport.HasModelScene());
    QVERIFY(!viewport.SelectedInstance().has_value());
    QCOMPARE(state.modelInstances, std::uint32_t{0});

    WorldViewport unsupported(nullptr, &backend);
    unsupported.SetSelectedInstance(0);
    QVERIFY(!unsupported.SetModelScene(original));
    QVERIFY(!unsupported.HasModelScene());
    QVERIFY(unsupported.SelectedInstance().has_value());
}

void WorldViewportTests::AnimationPauseStepAndCaptureStayBound() {
    using namespace War3Map::Render;
    BackendState state; active = &state;
    auto api = backend;
    api.submitModelScene = ModelScene;
    api.submitSkinnedModelScene = SkinScene; api.updateSkinMatrices = SkinUpdate;
    api.getSkinStatus = SkinStatus; api.captureSkinnedFrameRgba = SkinCapture;
    EditorPresentationFixture<BackendState>::Enable(api, state.editor);
    WorldViewport viewport(nullptr, &api);
    viewport.resize(320, 240); QVERIFY(Bind(viewport, 99));
    auto terrain = std::make_shared<MapData::TerrainGridSnapshot>();
    terrain->header.width = terrain->header.height = 2; terrain->header.offset = {-64, -64};
    terrain->minimum = {-64, -64, 0}; terrain->maximum = {64, 64, 0};
    terrain->vertices.resize(4); terrain->tiles.push_back({{0, 1, 2, 3}});
    for (std::size_t i = 0; i < 4; ++i)
        terrain->vertices[i].position = {i & 1 ? 64.0f : -64.0f, i & 2 ? 64.0f : -64.0f, 0};
    QVERIFY(viewport.SetPickingTerrain(terrain, 1, 99));
    viewport.show(); QApplication::processEvents();
    {
        QSignalSpy hovered(&viewport, &WorldViewport::PlacementHovered);
        QSignalSpy lost(&viewport, &WorldViewport::PlacementHoverLost);
        QEvent firstLeave(QEvent::Leave); QApplication::sendEvent(&viewport, &firstLeave);
        viewport.SetPlacing(true);
        QMouseEvent firstMove(QEvent::MouseMove, QPointF(160, 120), QPointF(160, 120),
            Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(&viewport, &firstMove);
        QVERIFY(!hovered.isEmpty()); // 首次落点不等待50ms周期，仍来自已完成视图。
        QVERIFY(viewport.HoverCoordinate());
        auto camera = viewport.Camera(); camera.targetX += 1;
        QVERIFY(viewport.SetCamera(camera, 99).Succeeded());
        QVERIFY(!viewport.HoverCoordinate()); // 旧坐标不可用于新镜头的正式摆放。
        QCOMPARE(lost.size(), 0); // 等待新回执时保留最后有效预览。
        QEvent leave(QEvent::Leave); QApplication::sendEvent(&viewport, &leave);
        QCOMPARE(lost.size(), 1); // 即使精确坐标已清空，真离开仍移除预览。
        viewport.SetPlacing(false);
    }
    auto bytes = AnimationTest::Bytes();
    const auto track = std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()).find("KGTR");
    QVERIFY(track != std::string_view::npos);
    // 有限输入的局部奇异继承：首段零缩放，200ms后恢复单位缩放。
    // 原版首末缺键合法；NaN则会被parser提前拒绝，均不能充当此运行期夹具。
    const auto patch = [&](std::size_t at, std::uint32_t value) { std::memcpy(bytes.data() + at, &value, 4); };
    patch(track + 8, 0); patch(track + 32, 200); patch(track + 48, 1000); patch(track + 20, 0);
    for (const auto offset : {24u, 28u, 36u, 40u, 44u, 52u, 56u, 60u, 68u, 72u, 76u}) patch(track + offset, 0x3f800000u);
    const auto bone = std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()).find("BONE");
    QVERIFY(bone != std::string_view::npos); patch(bone + 100, 2); patch(track, 0x4353474bu);
    AnimationInstanceSpec spec; spec.identity = {1, 8}; spec.hostKey = "preview-1";
    spec.model.modelPath = "synthetic.mdx"; spec.action = "Stand"; spec.elapsedMs = 500;
    spec.model.scale = {0.1f, 0.1f, 0.1f}; spec.model.position = {-0.8f, -0.5f, 0.1f}; // 真三角覆盖顶视中心。
    std::array specs{spec, spec}; specs[1].identity.value = 2; specs[1].hostKey = "preview-2";
    specs[0].elapsedMs = 100;
    AnimationLimits limits; limits.retainUnsupportedStatic = true;
    auto service = ModelAnimationService::Create({8, 99, "Qt-synthetic"}, specs,
        [&](auto path, auto) -> Result<AnimationResource> {
            if (path != "synthetic.mdx") return Result<AnimationResource>::Failure("Test", "RESOURCE_NOT_FOUND", "原创模型没有游戏贴图。");
            return Result<AnimationResource>::Success({bytes, "synthetic", std::string(64, 'a')});
        }, limits);
    QVERIFY2(service.Succeeded(), service.diagnostics.empty() ? "Animation service has no diagnostics" : service.diagnostics.front().code.c_str());
    AnimationAdvance initial;
    initial.worldView = viewport.AnimationViewSnapshot();
    initial.worldCamera = AnimationWorldCamera(initial.worldView->camera);
    auto first = (*service.value)->Prepare(nullptr, initial); QVERIFY(first.Succeeded());
    bool current = true;
    WorldAnimationPlayer player(viewport);
    QVERIFY(player.Install(*service.value, *first.value, {}, [&] { return current; }));
    QVERIFY(player.Playing()); player.SetPlaying(false); // 首次可见安装自动播放；下方精确帧断言显式暂停。
    const auto capture = viewport.CaptureCompletedFrame(99);
    QVERIFY(capture.Succeeded()); QVERIFY(capture.value->skin.has_value()); QVERIFY(capture.value->animation);
    QCOMPARE(capture.value->skin->contentVersion, capture.value->animation->contentVersion);
    QCOMPARE(capture.value->animation->poses.front().identity.value, std::uint64_t{1});
    QCOMPARE(capture.value->animation->hiddenInstances, std::size_t{1});
    QCOMPARE(player.Picking()->instances.size(), std::size_t{1});
    QCOMPARE(player.Picking()->instanceRequests.front(), std::uint32_t{1});
    QSignalSpy picked(&viewport, &WorldViewport::InstanceClicked);
    const auto fullApiSize = api.structSize;
    api.structSize = static_cast<std::uint32_t>(offsetof(War3RenderPluginApiV2, getEditorCompletedFrame));
    QTest::mouseClick(&viewport, Qt::LeftButton, Qt::NoModifier, QPoint(160, 120));
    QCOMPARE(picked.size(), qsizetype{0});
    QVERIFY(viewport.HoverText().contains(QStringLiteral("缺少同帧姿势/显隐绘制回执"))); // 旧尾部不回退 AABB/像素辅助。
    api.structSize = fullApiSize; Frame(&state);
    QTest::mouseClick(&viewport, Qt::LeftButton, Qt::NoModifier, QPoint(160, 120));
    QCOMPARE(picked.size(), qsizetype{1}); QCOMPARE(picked.first()[0].toInt(), 0);
    api.getFrameMetrics = [](War3RendererHandle h, std::uint64_t after, War3RenderFrameMetricsV2* records,
        std::uint32_t capacity, War3RenderFrameMetricsPageV2* page) -> std::int32_t {
        if (static_cast<BackendState*>(h)->redrawOnCompleted) Frame(h);
        return EditorPresentationFixture<BackendState>::Metrics(h, after, records, capacity, page);
    };
    state.redrawOnCompleted = true; picked.clear();
    QTest::mouseClick(&viewport, Qt::LeftButton, Qt::NoModifier, QPoint(160, 120));
    QCOMPARE(picked.size(), qsizetype{1}); // 相同内容的重绘不能废弃查询。
    state.redrawOnCompleted = false; picked.clear();
    state.freezeCompletedDraw = true;
    auto waitingCamera = viewport.Camera(); waitingCamera.targetX += 1;
    QVERIFY(viewport.SetCamera(waitingCamera, 99).Succeeded()); // 旧完成的相机确实失效，不能只设置pending布尔。
    QTest::mouseClick(&viewport, Qt::LeftButton, Qt::NoModifier, QPoint(160, 120));
    QCOMPARE(picked.size(), qsizetype{0}); state.freezeCompletedDraw = false; Frame(&state);
    QTRY_COMPARE(picked.size(), qsizetype{1}); // 不必再点一次，等待真实完成回执。
    state.freezeCompletedDraw = true;
    const auto visibleContent = state.editor.completed.skin.contentVersion;
    player.Step(); QTRY_VERIFY(player.Frame()->contentVersion > visibleContent);
    QCOMPARE(state.editor.completed.skin.contentVersion, visibleContent); // 最新CPU已经推进，真正显示的姿势仍保留。
    picked.clear(); QTest::mouseClick(&viewport, Qt::LeftButton, Qt::NoModifier, QPoint(160, 120));
    QCOMPARE(picked.size(), qsizetype{1}); QCOMPARE(picked.first()[0].toInt(), 0);
    state.freezeCompletedDraw = false; Frame(&state);
    QSignalSpy boxed(&viewport, &WorldViewport::InstancesBoxSelected);
    QTest::mousePress(&viewport, Qt::LeftButton, Qt::NoModifier, QPoint(20, 20));
    QMouseEvent boxMove(QEvent::MouseMove, QPointF(300, 220), QPointF(300, 220), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&viewport, &boxMove);
    QTest::mouseRelease(&viewport, Qt::LeftButton, Qt::NoModifier, QPoint(300, 220));
    QCOMPARE(boxed.size(), qsizetype{1}); QVERIFY(!qvariant_cast<QVector<int>>(boxed.first()[0]).empty());
    state.staleSkin = true;
    QVERIFY(!viewport.CaptureCompletedFrame(99).Succeeded());
    state.staleSkin = false;
    AnimationEdit edit; edit.identity = specs[0].identity; edit.elapsedMs = 500;
    player.Edit(edit);
    QTRY_VERIFY(player.Frame() && player.Frame()->hiddenInstances == 0);
    const auto beforePlay = state.matrixUploads;
    player.SetPlaying(true);
    QTRY_VERIFY(state.matrixUploads > beforePlay);
    player.SetPlaying(false);
    const auto paused = player.Frame(); const auto uploads = state.matrixUploads;
    QTest::qWait(120); // 正在返回的 worker 也不能在暂停后提交。
    QCOMPARE(state.matrixUploads, uploads);
    player.Step();
    QTRY_VERIFY(player.Frame()->contentVersion > paused->contentVersion);
    QCOMPARE(player.Frame()->states.front().elapsedMs, paused->states.front().elapsedMs + 33);
    QCOMPARE(state.skinUploads, std::uint64_t{1});

    const auto picking = player.Picking();
    viewport.SetSelectedInstance(1);
    edit.elapsedMs = 100;
    player.Edit(edit);
    QTRY_VERIFY(player.Frame() && player.Frame()->hiddenInstances == 1);
    QCOMPARE(player.Frame()->animatedInstances, std::size_t{1});
    QCOMPARE(player.Picking(), picking); // 宿主持有的同一轻量场景必须同步过滤。
    QCOMPARE(picking->instances.size(), std::size_t{1});
    QCOMPARE(picking->instanceRequests.front(), std::uint32_t{1});
    QCOMPARE(viewport.SelectedInstance(), std::optional<std::uint32_t>{0});
    const auto hiddenFrame = player.Frame();
    QVERIFY(hiddenFrame->poses.front().hidden);
    QCOMPARE(hiddenFrame->poses.front().rejection.code, War3RenderCore::Code::PoseInheritanceSingular);
    QVERIFY(!viewport.UpdateAnimationFrame(hiddenFrame, picking)); // 旧内容版本仍被拒绝。

    edit.elapsedMs = 500;
    player.Edit(edit);
    QTRY_VERIFY(player.Frame() && player.Frame()->hiddenInstances == 0);
    QCOMPARE(picking->instances.size(), std::size_t{2});
    QCOMPARE(viewport.SelectedInstance(), std::optional<std::uint32_t>{1});

    // 宿主拖动适配：临时变换经服务接受；隐藏取消按稳定身份恢复，不能倒退到旧时间。
    const auto dragOrigin = player.Frame()->states[0].transform;
    int cancelled = 0;
    std::shared_ptr<const AnimationFrame> cancelledFrame;
    const auto cancelConnection = connect(&viewport, &WorldViewport::MoveCancelled, &player, [&] {
        ++cancelled;
        cancelledFrame = viewport.AnimationTarget();
        AnimationEdit rollback; rollback.identity = specs[0].identity; rollback.transform = dragOrigin;
        player.Edit(std::move(rollback));
    });
    viewport.SetSelectedInstance(0);
    Frame(&state); // 只有 Draw 接受当前姿势/显隐后，拖动才可精确命中。
    QTest::mousePress(&viewport, Qt::LeftButton, Qt::NoModifier, QPoint(160, 120));
    QMouseEvent move(QEvent::MouseMove, QPointF(180, 120), QPointF(180, 120), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&viewport, &move);
    QVERIFY(viewport.Moving());
    picking->instances[0].transform[3] += 25;
    QVERIFY(viewport.UpdateModelInstances());
    QTRY_VERIFY(player.Frame()->states[0].transform[3] == dragOrigin[3] + 25);
    const auto dragged = player.Frame();
    edit.elapsedMs = 100;
    AnimationAdvance hide; hide.edits.push_back(edit);
    const auto candidate = (*service.value)->Prepare(dragged.get(), hide);
    QVERIFY(candidate.Succeeded());
    state.rejectModels = true;
    QVERIFY(!viewport.UpdateAnimationFrame(*candidate.value, picking));
    QCOMPARE(viewport.LastAnimationFailure().diagnostics.front().code, std::string("ANIMATION_UPDATE_REJECTED"));
    QVERIFY(viewport.Moving()); QCOMPARE(cancelled, 0);
    QCOMPARE(viewport.AnimationTarget(), dragged); QCOMPARE(picking->instances.size(), std::size_t{2});
    QVERIFY(!player.Install(*service.value, *first.value, {}, [&] { return current; }));
    QVERIFY(player.Ready()); QCOMPARE(player.Frame(), dragged); QCOMPARE(player.Picking(), picking);
    QVERIFY(viewport.ModelPresentationCurrent()); // 同来源新场景提交失败不能先 Clear 原播放器。
    QSignalSpy animationMessage(&player, &WorldAnimationPlayer::Message);
    player.Edit(edit);
    QTRY_VERIFY(!player.Busy());
    QVERIFY(player.Ready()); QVERIFY(!player.Playing()); QCOMPARE(player.Frame(), dragged);
    QVERIFY(viewport.ModelPresentationCurrent());
    QVERIFY(std::any_of(animationMessage.begin(), animationMessage.end(), [](const auto& row) {
        return row.front().toString().contains(QStringLiteral("ANIMATION_UPDATE_REJECTED"));
    }));
    state.rejectModels = false;
    state.nextSkinUpdateDelayMs = 550;
    player.SetPlaying(true);
    player.Edit(edit);
    QTRY_COMPARE(cancelled, 1);
    QTRY_VERIFY(!player.Busy());
    QVERIFY(player.Playing()); // 合法慢帧只降频，拖动补偿仍继续提交。
    player.SetPlaying(false); QTRY_VERIFY(!player.Busy());
    // 550ms 提交后的实时时钟允许继续推进；隐藏必须绑定实际取消拖动的帧，不能要求更晚的当前帧仍隐藏。
    QVERIFY(cancelledFrame && cancelledFrame->poses[0].hidden);
    QCOMPARE(cancelledFrame->states[0].elapsedMs, std::uint64_t{100});
    QVERIFY(player.Frame()->states[0].elapsedMs >= cancelledFrame->states[0].elapsedMs);
    QVERIFY(player.Frame()->states[0].elapsedMs >= std::uint64_t{100});
    QCOMPARE(player.Frame()->states[0].transform, dragOrigin);
    QVERIFY(!player.Playing());
    QVERIFY(!viewport.Moving());
    disconnect(cancelConnection);
    QTest::mouseRelease(&viewport, Qt::LeftButton, Qt::NoModifier, QPoint(180, 120));
    // 550ms 慢提交后两实例时钟已独立推进；恢复共同合法姿势须显式绑定两者。
    edit.elapsedMs = 500; auto restorePeer = edit; restorePeer.identity = specs[1].identity;
    player.EditMany({edit, restorePeer});
    QTRY_VERIFY(player.Frame()->states[0].elapsedMs == 500 && player.Frame()->states[1].elapsedMs == 500);
    QCOMPARE(player.Frame()->hiddenInstances, std::size_t{0});
    QCOMPARE(picking->instances[0].transform[3], dragOrigin[3]);

    edit.elapsedMs = 100; player.Edit(edit);
    edit.identity = specs[1].identity; player.Edit(edit);
    QTRY_VERIFY(player.Frame() && player.Frame()->hiddenInstances == 2);
    QVERIFY(picking->instances.empty()); QVERIFY(picking->instanceRequests.empty());
    QVERIFY(!viewport.SelectedInstance().has_value());
    player.SetPlaying(true);
    QTRY_VERIFY(player.Frame() && player.Frame()->hiddenInstances == 0); // 全隐藏仍推进到合法区间。
    player.SetPlaying(false); QTRY_VERIFY(!player.Busy());
    QCOMPARE(picking->instances.size(), std::size_t{2});
    QCOMPARE(state.skinUploads, std::uint64_t{1});
    const auto pausedBeforeHide=player.Frame();
    viewport.hide(); QTest::qWait(100); viewport.show(); QApplication::processEvents();
    QVERIFY(!player.Playing()); QCOMPARE(player.Frame(),pausedBeforeHide);
    player.Clear();
    QVERIFY(player.Install(*service.value,*first.value,{},[&]{return current;}));
    QVERIFY(!player.Playing()); // 用户暂停也跨场景重建保留。
    player.SetPlaying(true); QTRY_VERIFY(player.Frame()->contentVersion>1);
    viewport.hide(); QVERIFY(!player.Playing());
    const auto hidden=player.Frame(); QTest::qWait(100); QCOMPARE(player.Frame(),hidden);
    player.Clear();
    QVERIFY(player.Install(*service.value,*first.value,{},[&]{return current;}));
    QVERIFY(!player.Playing()); // 隐藏安装不推进时间，保留播放意图。
    viewport.show(); QTRY_VERIFY(player.Playing());
    QTRY_VERIFY(player.Frame()->contentVersion>1);
    player.SetPlaying(false); QTRY_VERIFY(!player.Busy());
    const auto phase=WorldStandStartMs("units:0:hfoo:17",1000);
    QVERIFY(phase<1000); QCOMPARE(WorldStandStartMs("units:0:hfoo:17",1000),phase);
    QVERIFY(WorldStandStartMs("units:1:hfoo:18",1000)!=phase);
    QCOMPARE(WorldStandStartMs("units:0:hfoo:17",0),std::uint64_t{0});
    WorldAnimationViewContractChecks::PausedViewReplay(viewport,player);
    // 离屏调度用可证明边界的原始模型；继承剥离夹具必须保持未知边界。
    auto bounded = ModelAnimationService::Create({8,99,"Qt-bounded-offscreen"}, specs,
        [](auto,auto) { return Result<AnimationResource>::Success({AnimationTest::Bytes(),"synthetic",std::string(64,'b')}); }, limits);
    QVERIFY(bounded.Succeeded());
    initial.worldView=viewport.AnimationViewSnapshot(); initial.worldCamera=AnimationWorldCamera(initial.worldView->camera);
    auto boundedFirst=(*bounded.value)->Prepare(nullptr,initial); QVERIFY(boundedFirst.Succeeded());
    QCOMPARE((*boundedFirst.value)->visibilityUnknownInstances,std::size_t{0});
    QVERIFY(player.Install(*bounded.value,*boundedFirst.value,{},[&]{return current;}));
    WorldAnimationViewContractChecks::MovingViewKeepsScheduling(viewport,player);
    WorldAnimationPlaybackContractChecks::ResumeDuringPausedWork(player);
    WorldAnimationViewContractChecks::OffscreenKeepsScheduling(viewport,player);
    const auto frozen = viewport.AnimationTarget(); const auto frozenUploads = state.skinUploads;
    current = false; player.SetPlaying(true);
    QVERIFY(!player.Ready()); QVERIFY(viewport.HasAnimationScene());
    QCOMPARE(viewport.AnimationTarget(), frozen); QCOMPARE(state.skinUploads, frozenUploads);
    QVERIFY(!viewport.ModelPresentationCurrent()); picked.clear(); Frame(&state);
    QTest::mouseClick(&viewport, Qt::LeftButton, Qt::NoModifier, QPoint(160, 120));
    QVERIFY(picked.isEmpty()); QVERIFY(viewport.HoverText().contains(QStringLiteral("只读")));

    // 同帧粒子接线：无地形/mesh 的合法零发射帧；不是实际GPU像素或模拟正确性证明。
    auto particles=std::make_shared<AnimatedModelScene>(); particles->sceneVersion=NextAnimationSerial();
    particles->documentRevision=99; particles->particlesEnabled=true; particles->particleInstanceCount=1;
    particles->specs.push_back(spec); particles->particleCapabilities.push_back({1,0,0,true,{}});
    auto zero=std::make_shared<AnimationFrame>(); zero->sceneVersion=particles->sceneVersion;
    zero->contentVersion=1; zero->documentRevision=99;
    auto emptyPicking=AnimationPickingScene(*particles,*zero); viewport.ClearMesh(99);
    zero->worldCamera=AnimationWorldCamera(viewport.Camera());
    QVERIFY(!viewport.SetAnimationScene(particles,zero,emptyPicking)); // 旧尾部不能伪造纯粒子空图。
    api.submitParticleModelScene=ParticleScene; api.updateParticleModelFrame=ParticleUpdate;
    api.captureParticleFrameRgba=ParticleCapture;
    QVERIFY(viewport.SetAnimationScene(particles,zero,emptyPicking));
    const auto firstParticle=viewport.CaptureCompletedFrame(99);
    QVERIFY(firstParticle.Succeeded()); QVERIFY(firstParticle.value->particles);
    QCOMPARE(firstParticle.value->particles->vertexCount,std::uint32_t{0});
    auto emitted=std::make_shared<AnimationFrame>(*zero); emitted->contentVersion=2;
    emitted->particleVertices.resize(3); emitted->particleDraws.push_back({0,3,WAR3_RENDER_MODEL_NO_TEXTURE,WAR3_PARTICLE_BLEND});
    state.rejectModels=true;
    QVERIFY(!viewport.UpdateAnimationFrame(emitted,emptyPicking)); QVERIFY(viewport.AnimationTarget()==zero);
    QCOMPARE(state.particles.contentVersion,std::uint64_t{1}); state.rejectModels=false;
    QVERIFY(viewport.UpdateAnimationFrame(emitted,emptyPicking));
    const auto sameFrame=viewport.CaptureCompletedFrame(99); QVERIFY(sameFrame.Succeeded());
    QCOMPARE(sameFrame.value->particles->contentVersion,sameFrame.value->skin->contentVersion);
    QCOMPARE(sameFrame.value->particles->vertexCount,std::uint32_t{3});
    state.staleParticles=true; QVERIFY(!viewport.CaptureCompletedFrame(99).Succeeded()); state.staleParticles=false;
    auto particleCamera=viewport.Camera(); particleCamera.targetX+=1;
    QVERIFY(viewport.SetCamera(particleCamera,99).Succeeded()); QVERIFY(!viewport.CaptureCompletedFrame(99).Succeeded());
    auto cleared=std::make_shared<AnimationFrame>(*zero); cleared->contentVersion=3;
    cleared->worldCamera=AnimationWorldCamera(viewport.Camera());
    QVERIFY(viewport.UpdateAnimationFrame(cleared,emptyPicking));
    const auto emptyAgain=viewport.CaptureCompletedFrame(99); QVERIFY(emptyAgain.Succeeded());
    QCOMPARE(emptyAgain.value->particles->vertexCount,std::uint32_t{0});
    QCOMPARE(emptyAgain.value->particles->dynamicUploadCount,std::uint64_t{3});
    // 新插件优先走layered路径时仍必须保留AnimationRenderer相同的粒子/相机门禁。
    EnableLayers(api);
    const auto layeredParticle = viewport.CaptureCompletedFrame(99);
    QVERIFY(layeredParticle.Succeeded()); QVERIFY(layeredParticle.value->particles);
    QCOMPARE(layeredParticle.value->particles->contentVersion, cleared->contentVersion);
    state.staleParticles = true; QVERIFY(!viewport.CaptureCompletedFrame(99).Succeeded()); state.staleParticles = false;
    particleCamera.targetX += 1;
    QVERIFY(viewport.SetCamera(particleCamera, 99).Succeeded());
    QVERIFY(!viewport.CaptureCompletedFrame(99).Succeeded());
}

QTEST_MAIN(WorldViewportTests)
#include "WorldViewportTests.moc"
