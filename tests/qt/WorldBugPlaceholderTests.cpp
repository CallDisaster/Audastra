#include "WorldViewport.h"
#include "WorldModelBugPlaceholderPresenter.h"
#include "../WorldModelBugPlaceholderFixture.h"
#include "EditorPresentationFixture.h"
#include <QApplication>
#include <QSignalSpy>
#include <QTest>

using namespace War3Map;
using namespace War3Map::Modules;
using namespace WorldModelBugPlaceholderTest;

namespace {
// 原创后端合同夹具：记录提交、完成身份及调用次数；不绘制，也不生成 GPU 像素。
// 点选仍必须走生产 WorldViewport 的完成帧门、原创方块三角形和 typed signal。
struct PlaceholderBackend {
    EditorPresentationState editor;
    War3RenderCameraV2 camera{sizeof(camera), 0, 0, 0, 2048, 0, 0.8f};
    std::uint32_t width = 0, height = 0;
    std::uint64_t revision = 0, frame = 0, surfaceEpoch = 1, sceneEpoch = 1, viewEpoch = 1;
    std::uint64_t skinScene = 0, skinContent = 0, layersRevision = 1;
    std::vector<War3RenderModelLayerStatusV2> layers;
    int rgbaCalls = 0;
    bool drawEnabled = true;
    static PlaceholderBackend* active;
    static PlaceholderBackend& Get(War3RendererHandle h) { return *static_cast<PlaceholderBackend*>(h); }
    static War3RendererHandle WAR3_RENDER_CALL Create(const War3RenderCreateInfoV2* in) {
        active->width = in->width; active->height = in->height; return active;
    }
    static void WAR3_RENDER_CALL Destroy(War3RendererHandle) {}
    static std::int32_t WAR3_RENDER_CALL Attach(War3RendererHandle h, void* window) {
        ++Get(h).surfaceEpoch; return window ? 0 : 1;
    }
    static std::int32_t WAR3_RENDER_CALL Resize(War3RendererHandle h, std::uint32_t w, std::uint32_t height) {
        auto& s = Get(h); s.width = w; s.height = height; ++s.surfaceEpoch; return 0;
    }
    static std::int32_t WAR3_RENDER_CALL Mesh(War3RendererHandle h, const War3RenderMeshV2* in) {
        auto& s = Get(h); s.revision = in->documentRevision; ++s.sceneEpoch; return 0;
    }
    static std::int32_t WAR3_RENDER_CALL Camera(War3RendererHandle h, const War3RenderCameraV2* in) {
        auto& s = Get(h); s.camera = *in; ++s.viewEpoch; return 0;
    }
    static std::int32_t WAR3_RENDER_CALL View(War3RendererHandle h, War3RenderViewInfoV2* out) {
        const auto& s = Get(h); *out = {sizeof(*out), s.width, s.height, s.frame, s.revision,
            s.surfaceEpoch, s.sceneEpoch, s.viewEpoch, {}};
        // 正交顶视：near 在 z=128，far 在 z=-128。中心射线真实经过原位方块与地形。
        out->viewProjection[0] = out->viewProjection[5] = 1.0f / 128;
        out->viewProjection[10] = -1.0f / 256; out->viewProjection[14] = 0.5f;
        out->viewProjection[15] = 1; return 0;
    }
    static std::int32_t WAR3_RENDER_CALL Status(War3RendererHandle h, War3RenderStatusV2* out) {
        const auto& s = Get(h); *out = {}; out->structSize = sizeof(*out); out->deviceReady = 1;
        out->width = s.width; out->height = s.height; out->frameCount = s.frame; out->documentRevision = s.revision;
        return 0;
    }
    static std::int32_t WAR3_RENDER_CALL SkinScene(War3RendererHandle h, const War3RenderSkinnedModelSceneV2* in) {
        if (!in || !in->initialPose) return 1;
        auto& s = Get(h); s.skinScene = in->initialPose->sceneVersion; s.skinContent = in->initialPose->contentVersion;
        ++s.sceneEpoch; return 0;
    }
    static std::int32_t WAR3_RENDER_CALL SkinUpdate(War3RendererHandle h, const War3RenderSkinUpdateV2* in) {
        auto& s = Get(h); if (!in || in->sceneVersion != s.skinScene) return 1;
        s.skinContent = in->contentVersion; return 0;
    }
    static std::int32_t WAR3_RENDER_CALL SkinStatus(War3RendererHandle h, War3RenderSkinStatusV2* out) {
        const auto& s = Get(h); *out = {sizeof(*out), 0, s.skinScene ? 1u : 0u, 0, s.frame,
            s.skinScene, s.skinContent, 1, 1}; return 0;
    }
    static std::int32_t WAR3_RENDER_CALL LegacyCapture(War3RendererHandle h, void*, std::uint64_t, War3RenderCaptureInfoV2*) {
        ++Get(h).rgbaCalls; return 1;
    }
    static std::int32_t WAR3_RENDER_CALL SkinCapture(War3RendererHandle h, void*, std::uint64_t, War3RenderSkinnedCapturedFrameV2*) {
        ++Get(h).rgbaCalls; return 1;
    }
    static std::int32_t WAR3_RENDER_CALL EditorCapture(War3RendererHandle h, std::uint64_t, std::uint64_t,
        void*, std::uint64_t, War3RenderEditorCapturedFrameV2*) { ++Get(h).rgbaCalls; return 1; }
    static std::int32_t WAR3_RENDER_CALL LayerCapture(War3RendererHandle h, std::uint64_t,
        void*, std::uint64_t, War3RenderLayeredCapturedFrameV2*) { ++Get(h).rgbaCalls; return 1; }
    static std::int32_t WAR3_RENDER_CALL LayerTarget(War3RendererHandle h, War3RenderModelLayersTargetV2* out) {
        const auto& s = Get(h); *out = {sizeof(*out), static_cast<std::uint32_t>(s.layers.size()),
            s.editor.target.modelEpoch, s.layersRevision}; return 0;
    }
    static std::int32_t WAR3_RENDER_CALL SubmitLayer(War3RendererHandle h, const War3RenderModelSceneLayerV2* in) {
        auto& s = Get(h); if (!in || !in->models || !in->models->initialPose
            || in->parentModelEpoch != s.editor.target.modelEpoch) return 1;
        const auto& pose = *in->models->initialPose;
        War3RenderModelLayerStatusV2 layer{}; layer.structSize = sizeof(layer); layer.layerId = in->layerId;
        layer.parentModelEpoch = in->parentModelEpoch; layer.revision = in->revision;
        layer.skin = {sizeof(layer.skin), 0, 1, 0, 0, pose.sceneVersion, pose.contentVersion, 1, 1};
        const auto found = std::find_if(s.layers.begin(), s.layers.end(), [&](const auto& old) { return old.layerId == in->layerId; });
        if (found == s.layers.end()) s.layers.push_back(layer); else *found = layer;
        ++s.layersRevision; ++s.sceneEpoch; return 0;
    }
    static std::int32_t WAR3_RENDER_CALL UpdateLayer(War3RendererHandle, const War3RenderModelLayerUpdateV2*) { return 1; }
    static std::int32_t WAR3_RENDER_CALL RemoveLayer(War3RendererHandle h, std::uint64_t id,
        std::uint64_t parent, std::uint64_t revision) {
        auto& s = Get(h); const auto found = std::find_if(s.layers.begin(), s.layers.end(),
            [&](const auto& layer) { return layer.layerId == id && layer.parentModelEpoch == parent && layer.revision == revision; });
        if (found == s.layers.end()) return 1;
        s.layers.erase(found); ++s.layersRevision; ++s.sceneEpoch; return 0;
    }
    static void WAR3_RENDER_CALL Frame(War3RendererHandle h) {
        auto& s = Get(h); if (!s.drawEnabled) return;
        ++s.frame; EditorPresentationFixture<PlaceholderBackend>::Draw(h);
        if (!s.editor.metrics.empty()) s.editor.metrics.back().flags |= WAR3_RENDER_FRAME_METRICS_WINDOW_ATTACHED;
    }
    War3RenderPluginApiV2 Api() {
        active = this; War3RenderPluginApiV2 api{}; api.structSize = sizeof(api); api.apiVersion = WAR3_RENDER_PLUGIN_API_V2;
        api.createRenderer = Create; api.destroyRenderer = Destroy; api.attachNativeWindow = Attach; api.resize = Resize;
        api.submitMesh = Mesh; api.setCamera = Camera; api.requestFrame = Frame; api.getStatus = Status; api.captureRgba = LegacyCapture;
        api.getViewInfo = View; api.submitSkinnedModelScene = SkinScene; api.updateSkinMatrices = SkinUpdate;
        api.getSkinStatus = SkinStatus; api.captureSkinnedFrameRgba = SkinCapture;
        api.submitModelSceneLayer = SubmitLayer; api.updateModelSceneLayer = UpdateLayer;
        api.removeModelSceneLayer = RemoveLayer; api.getModelLayersTarget = LayerTarget; api.captureLayeredFrameRgba = LayerCapture;
        return api;
    }
};
PlaceholderBackend* PlaceholderBackend::active = nullptr;
bool BindTerrain(WorldViewport& viewport) {
    auto terrain = std::make_shared<MapData::TerrainGridSnapshot>(); terrain->header.width = terrain->header.height = 2;
    terrain->header.offset = {-64, -64}; terrain->minimum = {-64, -64, 0}; terrain->maximum = {64, 64, 0};
    terrain->vertices.resize(4); terrain->tiles.push_back({{0, 1, 2, 3}});
    std::vector<War3RenderVertexV2> vertices;
    for (std::size_t i = 0; i < 4; ++i) {
        const float x = i & 1 ? 64.0f : -64.0f, y = i & 2 ? 64.0f : -64.0f;
        terrain->vertices[i].position = {x, y, 0}; vertices.push_back({x, y, 0, 1, 1, 1});
    }
    return viewport.SetMesh(std::move(vertices), {0, 1, 2, 1, 3, 2}, 1) && viewport.SetPickingTerrain(terrain, 1, 1);
}
}

class WorldBugPlaceholderTests final : public QObject {
    Q_OBJECT
private slots:
    void CompletedOwnedLayerUsesTypedSignalWithoutReadback_data() {
        QTest::addColumn<int>("refusal");
        QTest::newRow("same-completed-source-and-layer") << 0;
        QTest::newRow("old-map-revision") << 1;
        QTest::newRow("locked-live-owner") << 2;
        QTest::newRow("layer-not-completed") << 3;
        QTest::newRow("aggregate-upload-failed") << 4;
        QTest::newRow("source-invalidated") << 5;
        QTest::newRow("window-not-attached") << 6;
    }
    void CompletedOwnedLayerUsesTypedSignalWithoutReadback() {
        QFETCH(int, refusal);
        PlaceholderBackend state; auto backend = state.Api();
        EditorPresentationFixture<PlaceholderBackend>::Enable(backend, state.editor);
        backend.captureEditorPresentationFrameRgba = PlaceholderBackend::EditorCapture;
        WorldViewport viewport(nullptr, &backend); viewport.resize(320, 240); QVERIFY(BindTerrain(viewport));
        viewport.show(); QApplication::processEvents(); QVERIFY(viewport.RendererReady());
        Fixture source; source.PoseFailure(); const auto originalContent = source.frame->contentVersion;
        auto picking = Render::AnimationPickingScene(*source.scene, *source.frame);
        QVERIFY(viewport.SetAnimationScene(source.scene, source.frame, picking)); PlaceholderBackend::Frame(&state);
        QVERIFY(viewport.ReadCompletedAnimationFrameForBugPlaceholder(source.scene).Succeeded());
        bool current = true; WorldModelBugPlaceholderPresenter presenter(&viewport);
        presenter.Bind(source.scene, source.registry, source.owners, [&] { return current; }, source.Flags());
        QCOMPARE(presenter.Count(), std::size_t{1}); QVERIFY(presenter.Snapshot());
        QVERIFY(viewport.HasBugPlaceholderPicking(presenter.Snapshot())); QCOMPARE(state.layers.size(), std::size_t{1});
        // 主层完成后新提交附加层会推进sceneEpoch；必须再明确完成整个当前视图。
        if (refusal != 3) PlaceholderBackend::Frame(&state);
        state.drawEnabled = false; // 后续事件不能意外刷新故意设置的旧/失败回执。
        QVERIFY(!state.editor.metrics.empty());
        if (refusal == 1) ++state.editor.metrics.back().completed.view.documentRevision;
        if (refusal == 2) source.policy[0].locked = true;
        if (refusal == 4) state.editor.metrics.back().flags &= ~WAR3_RENDER_FRAME_METRICS_UPLOAD_VALID;
        if (refusal == 5) current = false;
        if (refusal == 6) state.editor.metrics.back().flags &= ~WAR3_RENDER_FRAME_METRICS_WINDOW_ATTACHED;
        QSignalSpy bugs(&viewport, &WorldViewport::BugPlaceholderClicked);
        QSignalSpy originals(&viewport, &WorldViewport::InstanceClicked);
        QVERIFY(bugs.isValid()); QVERIFY(originals.isValid());
        std::vector<MapData::PlacementId> signalled;
        connect(&viewport, &WorldViewport::BugPlaceholderClicked, &viewport,
            [&](const MapData::PlacementId& owner, int modifiers) {
                signalled.push_back(owner); QCOMPARE(modifiers, static_cast<int>(Qt::ShiftModifier));
            });
        // 真实press/release经过生产ray->owned checker triangles；不人工emit typed signal。
        QTest::mouseClick(&viewport, Qt::LeftButton, Qt::ShiftModifier, QPoint(160, 120));
        QCOMPARE(bugs.size(), refusal ? qsizetype{0} : qsizetype{1});
        QCOMPARE(signalled.size(), refusal ? std::size_t{0} : std::size_t{1});
        if (!refusal) {
            QVERIFY(signalled.front() == source.owners[0]); QCOMPARE(originals.size(), qsizetype{0});
            QVERIFY(viewport.SelectionText().startsWith(QStringLiteral("命中：")));
        }
        QCOMPARE(state.rgbaCalls, 0); // capture入口即使被误调用也计数并拒绝，不能伪造通过。
        QVERIFY(source.frame->poses[0].hidden); QCOMPARE(source.frame->contentVersion, originalContent);
    }
    void NoCompletedFrameCannotPrepareOrAuthorizePicking() {
        War3RenderPluginApiV2 backend{}; backend.structSize = sizeof(backend); backend.apiVersion = WAR3_RENDER_PLUGIN_API_V2;
        WorldViewport viewport(nullptr, &backend); WorldModelBugPlaceholderPresenter presenter(&viewport);
        Fixture source; source.PoseFailure();
        presenter.Bind(source.scene, source.registry, source.owners, [] { return true; }, source.Flags());
        QVERIFY(!viewport.ReadCompletedAnimationFrameForBugPlaceholder(source.scene).Succeeded());
        QCOMPARE(presenter.Count(), std::size_t(0)); QVERIFY(!presenter.Snapshot());
        QVERIFY(!viewport.HasBugPlaceholderPicking({}));
        presenter.Clear(); QVERIFY(!presenter.Snapshot());
    }
    void OccludedDrawCanPrepareCpuPlanWithoutPickAuthority_data() {
        QTest::addColumn<int>("refusal");
        QTest::newRow("occluded-success-prepares-cpu-only") << 0;
        QTest::newRow("failed-present-rejects-cpu") << 1;
        QTest::newRow("unmarked-nonzero-present-rejects-cpu") << 2;
        QTest::newRow("no-window-rejects-cpu") << 3;
        QTest::newRow("bad-upload-rejects-cpu") << 4;
        QTest::newRow("old-map-revision-rejects-cpu") << 5;
        QTest::newRow("old-view-epoch-rejects-cpu") << 6;
    }
    void OccludedDrawCanPrepareCpuPlanWithoutPickAuthority() {
        QFETCH(int, refusal);
        PlaceholderBackend state; auto backend = state.Api();
        EditorPresentationFixture<PlaceholderBackend>::Enable(backend, state.editor);
        backend.captureEditorPresentationFrameRgba = PlaceholderBackend::EditorCapture;
        WorldViewport viewport(nullptr, &backend); viewport.resize(320, 240); QVERIFY(BindTerrain(viewport));
        viewport.show(); QApplication::processEvents(); QVERIFY(viewport.RendererReady());
        Fixture source; source.PoseFailure();
        QVERIFY(viewport.SetAnimationScene(source.scene, source.frame, Render::AnimationPickingScene(*source.scene, *source.frame)));
        PlaceholderBackend::Frame(&state); QVERIFY(!state.editor.metrics.empty()); state.drawEnabled = false;
        const auto markOccluded = [](War3RenderFrameMetricsV2& metric) {
            metric.flags |= WAR3_RENDER_FRAME_METRICS_PRESENT_OCCLUDED;
            metric.presentResult = 0x087a0001; // DXGI_STATUS_OCCLUDED：非失败HRESULT，仍无可见验收资格。
        };
        auto& metric = state.editor.metrics.back(); markOccluded(metric);
        if (refusal == 1) metric.presentResult = -1;
        if (refusal == 2) metric.flags &= ~WAR3_RENDER_FRAME_METRICS_PRESENT_OCCLUDED;
        if (refusal == 3) metric.flags &= ~WAR3_RENDER_FRAME_METRICS_WINDOW_ATTACHED;
        if (refusal == 4) metric.flags &= ~WAR3_RENDER_FRAME_METRICS_UPLOAD_VALID;
        if (refusal == 5) ++metric.completed.view.documentRevision;
        if (refusal == 6) ++metric.completed.view.viewEpoch;
        QCOMPARE(viewport.ReadCompletedAnimationFrameForBugPlaceholder(source.scene).Succeeded(), !refusal);
        WorldModelBugPlaceholderPresenter presenter(&viewport);
        presenter.Bind(source.scene, source.registry, source.owners, [] { return true; }, source.Flags());
        QCOMPARE(presenter.Count(), refusal ? std::size_t{0} : std::size_t{1});
        QCOMPARE(static_cast<bool>(presenter.Snapshot()), !refusal);
        if (!refusal) {
            QCOMPARE(state.layers.size(), std::size_t{1}); state.drawEnabled = true;
            PlaceholderBackend::Frame(&state); state.drawEnabled = false; markOccluded(state.editor.metrics.back());
            QVERIFY(viewport.ReadCompletedAnimationFrameForBugPlaceholder(source.scene).Succeeded());
        }
        QSignalSpy bugs(&viewport, &WorldViewport::BugPlaceholderClicked); QVERIFY(bugs.isValid());
        QTest::mouseClick(&viewport, Qt::LeftButton, Qt::ShiftModifier, QPoint(160, 120));
        QCOMPARE(bugs.size(), qsizetype{0}); // 含已准备方块的遮挡完成帧仍不得通过严格点选门。
        QCOMPARE(state.rgbaCalls, 0); QVERIFY(source.frame->poses[0].hidden);
    }
    void FailedPlanRetainsDiagnosticAfterLayerCleanup() {
        PlaceholderBackend state; auto backend = state.Api();
        EditorPresentationFixture<PlaceholderBackend>::Enable(backend, state.editor);
        backend.captureEditorPresentationFrameRgba = PlaceholderBackend::EditorCapture;
        WorldViewport viewport(nullptr, &backend); viewport.resize(320, 240); QVERIFY(BindTerrain(viewport));
        viewport.show(); QApplication::processEvents(); QVERIFY(viewport.RendererReady());
        Fixture source; source.PoseFailure();
        QVERIFY(viewport.SetAnimationScene(source.scene, source.frame, Render::AnimationPickingScene(*source.scene, *source.frame)));
        PlaceholderBackend::Frame(&state);
        WorldModelBugPlaceholderPresenter presenter(&viewport);
        presenter.Bind(source.scene, source.registry, source.owners, [] { return true; }, source.Flags());
        QCOMPARE(presenter.Count(), std::size_t{1}); PlaceholderBackend::Frame(&state);
        source.scene->sourceIdentity = "synthetic-changed-source";
        presenter.Refresh(); QVERIFY(!presenter.Snapshot()); QCOMPARE(presenter.Count(), std::size_t{0});
        QVERIFY(state.layers.empty()); QVERIFY(!presenter.LastStatus().Succeeded());
        QVERIFY(!presenter.LastStatus().diagnostics.empty());
        QCOMPARE(QString::fromStdString(presenter.LastStatus().diagnostics.front().code), QStringLiteral("WORLD_BUG_PLACEHOLDER_SOURCE_STALE"));
        QCOMPARE(state.rgbaCalls, 0); // 清理成功不能将真实计划失败改称成功。
    }
    void LiveSourceInvalidatesWithoutCreatingRendererOrMutatingOriginal() {
        War3RenderPluginApiV2 backend{}; backend.structSize = sizeof(backend); backend.apiVersion = WAR3_RENDER_PLUGIN_API_V2;
        WorldViewport viewport(nullptr, &backend); WorldModelBugPlaceholderPresenter presenter(&viewport);
        Fixture source; source.PoseFailure(); const auto before = source.frame->contentVersion;
        bool current = true;
        presenter.Bind(source.scene, source.registry, source.owners, [&] { return current; }, source.Flags());
        current = false; presenter.Refresh();
        QVERIFY(!presenter.Snapshot()); QCOMPARE(presenter.Count(), std::size_t(0));
        QVERIFY(!viewport.RendererReady()); QVERIFY(!viewport.HasAnimationScene());
        QCOMPARE(source.frame->contentVersion, before); QVERIFY(source.frame->poses[0].hidden);
        QVERIFY(source.scene->unavailableInstances.empty());
    }
};
int main(int argc, char** argv) {
    QApplication application(argc, argv); WorldBugPlaceholderTests tests; return QTest::qExec(&tests, argc, argv);
}
#include "WorldBugPlaceholderTests.moc"
