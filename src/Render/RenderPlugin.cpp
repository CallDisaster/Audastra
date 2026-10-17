#include "War3MapCore/RenderPluginV2.h"
#include "War3MapCore/RenderPluginFrameMetrics.h"
#include "War3MapCore/RenderPluginInstanceRoster.h"
#include "War3MapCore/RenderPluginMaterialAlpha.h"
#include "War3MapCore/RenderPluginMaterialState.h"
#include "Backend/D3D11/D3D11Scene.h"
#include "Scene/ModelSkinInput.h"
#include "Particles/ParticleInput.h"
#include "Scene/ModelSceneLayerData.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <memory>
#include <limits>
#include <mutex>
#include <thread>

namespace {
using War3Map::Render::D3D11Scene;
using War3Map::Render::Mesh;
using War3Map::Render::ModelScene;
using War3Map::Render::SkinPose;
using War3Map::Render::ParticleSceneData;
using War3Map::Render::ParticleFrameData;
using War3Map::Render::Environment;
using War3Map::Render::EditorVisibility;
using War3Map::Render::ModelSceneLayerData;
using War3Map::Render::ModelSceneLayers;
constexpr std::uint32_t kMaxSize = 4096, kMaxVertices = 262144, kMaxIndices = 1572864;
// 地表入口含过渡层，单独放宽；普通网格入口不变。
constexpr std::uint32_t kMaxTerrainVertices = 1048576, kMaxTerrainIndices = 6291456;
constexpr std::uint32_t kMaxTerrainTextures = 16, kMaxTerrainBatches = 4096, kMaxTextureSize = 2048;
constexpr std::uint64_t kMaxTextureBytes = 16ull * 1024 * 1024, kMaxTextureTotal = 64ull * 1024 * 1024;
constexpr std::uint64_t kMaxTerrainBytes = 128ull * 1024 * 1024;
constexpr std::uint32_t kMaxMipLevels = 12;
constexpr std::uint64_t kMaxMipChainBytes = 24ull * 1024 * 1024;
// 模型场景预算（见 RenderPluginV2.h）。
constexpr std::uint32_t kMaxModelVertices = 2097152, kMaxModelIndices = 6291456, kMaxModelLayers = 131072,
    kMaxModelMeshes = 65536, kMaxModels = 16384, kMaxModelTextures = 4096;
constexpr std::uint64_t kMaxModelTextureTotal = 192ull * 1024 * 1024, kMaxModelBytes = 320ull * 1024 * 1024;
constexpr std::int32_t kInvalid = 1, kTimeout = 2;
std::uint64_t CpuNanoseconds(std::chrono::steady_clock::duration duration) noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count());
}
struct RosterCandidate {
    std::uint64_t ticket = 0, assetIdentity = 0, expectedPresentationRevision = 0, expectedLayersRevision = 0;
    std::uint64_t candidateAssetIdentity = 0;
    War3RenderEditorPresentationTargetV2 expected{};
    std::shared_ptr<const ModelScene> baseModels, baseLayout, layout;
    std::shared_ptr<const ModelScene> models; // 非空=静态suffix追加；空保持旧驻留库。
    std::shared_ptr<const SkinPose> basePose, pose;
    std::shared_ptr<const ParticleSceneData> baseParticles;
    std::shared_ptr<const ParticleSceneData> particleAssets;
    std::shared_ptr<const ParticleFrameData> baseParticleFrame, particleFrame;
    std::shared_ptr<const EditorVisibility> baseVisibility, visibility;
    std::shared_ptr<const ModelSceneLayers> layers;
};
struct FrameState {
    HWND window = nullptr;
    std::uint32_t width = 640, height = 480;
    War3RenderCameraV2 camera{sizeof(War3RenderCameraV2), 0, 0, 0, 6000, -1.1f, 0.8f};
    std::shared_ptr<const Mesh> mesh = std::make_shared<Mesh>();
    std::shared_ptr<const ModelScene> models = std::make_shared<ModelScene>();
    std::shared_ptr<const ModelScene> skinLayout; // roster baseline元数据，静态models拥有者保持驻留。
    std::shared_ptr<const SkinPose> skinPose;
    std::shared_ptr<const ParticleSceneData> particles;
    std::shared_ptr<const ParticleFrameData> particleFrame;
    std::shared_ptr<const EditorVisibility> visibility;
    std::uint64_t modelEpoch = 1, lastPresentationRevision = 0;
    std::uint64_t lastSkinSceneVersion = 0;
    std::uint64_t assetIdentity = 0;
    std::shared_ptr<const RosterCandidate> roster;
    std::shared_ptr<const ModelSceneLayers> modelLayers = std::make_shared<ModelSceneLayers>();
    std::uint64_t layersRevision = 1;
    std::shared_ptr<const Environment> environment = std::make_shared<Environment>(War3Map::Render::DefaultEnvironment());
    // 实例变换覆盖（空 = 使用 models 自带实例）；随 models 重新提交而清空。
    std::shared_ptr<const std::vector<War3RenderModelInstanceV2>> instances;
    std::shared_ptr<const std::vector<War3RenderVertexV2>> overlay = std::make_shared<std::vector<War3RenderVertexV2>>();
    std::uint64_t request = 1, capture = 0;
    std::uint64_t surfaceEpoch = 1, sceneEpoch = 1, viewEpoch = 1;
};
War3RenderEditorPresentationTargetV2 PresentationTarget(const FrameState& state) noexcept {
    return {sizeof(War3RenderEditorPresentationTargetV2),
        state.particles ? WAR3_EDITOR_PRESENTATION_PARTICLES : state.skinPose ? WAR3_EDITOR_PRESENTATION_SKINNED : WAR3_EDITOR_PRESENTATION_STATIC,
        static_cast<std::uint32_t>(state.skinPose ? state.skinPose->instances.size() : state.models->instances.size()),
        state.particleFrame ? static_cast<std::uint32_t>(state.particleFrame->draws.size()) : 0,
        state.modelEpoch, state.skinPose ? state.skinPose->sceneVersion : 0, state.skinPose ? state.skinPose->contentVersion : 0};
}
const ModelScene& SkinLayout(const FrameState& state) noexcept {
    return state.skinLayout ? *state.skinLayout : *state.models;
}
std::uint64_t PresentationRevision(const FrameState& state) noexcept {
    return state.visibility ? state.visibility->status.presentationRevision : 0;
}
bool SamePresentationTarget(const War3RenderEditorPresentationTargetV2& a,
    const War3RenderEditorPresentationTargetV2& b) noexcept {
    return a.mode == b.mode && a.instanceCount == b.instanceCount && a.particleDrawCount == b.particleDrawCount
        && a.modelEpoch == b.modelEpoch && a.sceneVersion == b.sceneVersion && a.contentVersion == b.contentVersion;
}
bool RosterBaseMatches(const FrameState& state, const RosterCandidate& roster) noexcept {
    return SamePresentationTarget(PresentationTarget(state), roster.expected)
        && state.assetIdentity == roster.assetIdentity
        && PresentationRevision(state) == roster.expectedPresentationRevision
        && state.layersRevision == roster.expectedLayersRevision
        && state.models == roster.baseModels && state.skinLayout == roster.baseLayout
        && state.skinPose == roster.basePose && state.particles == roster.baseParticles
        && state.particleFrame == roster.baseParticleFrame && state.visibility == roster.baseVisibility;
}
struct Renderer {
    std::mutex mutex;
    std::condition_variable changed;
    std::atomic<bool> closing{false}, workerExited{false};
    bool warp = false, captureActive = false;
    FrameState state;
    War3RenderStatusV2 status{sizeof(War3RenderStatusV2)};
    std::vector<std::uint8_t> pixels;
    War3RenderCaptureInfoV2 captureInfo{sizeof(War3RenderCaptureInfoV2)};
    War3RenderViewInfoV2 viewInfo{sizeof(War3RenderViewInfoV2)};
    War3RenderCapturedFrameV2 capturedFrame{sizeof(War3RenderCapturedFrameV2)};
    War3RenderSkinStatusV2 skinStatus{sizeof(War3RenderSkinStatusV2)};
    War3RenderSkinnedCapturedFrameV2 capturedSkinFrame{sizeof(War3RenderSkinnedCapturedFrameV2)};
    War3RenderParticleCapturedFrameV2 capturedParticleFrame{sizeof(War3RenderParticleCapturedFrameV2)};
    War3RenderEditorCapturedFrameV2 capturedPresentationFrame{sizeof(War3RenderEditorCapturedFrameV2)};
    War3RenderEditorCompletedFrameV2 completedEditorFrame{sizeof(War3RenderEditorCompletedFrameV2)};
    War3RenderLayeredCapturedFrameV2 capturedLayeredFrame{sizeof(War3RenderLayeredCapturedFrameV2)};
    std::shared_ptr<const ModelSceneLayers> completedLayers;
    std::vector<War3RenderModelLayerStatusV2> layerStatuses;
    std::shared_ptr<const ModelScene> completedModelRequest;
    std::shared_ptr<const SkinPose> completedPoseRequest;
    std::shared_ptr<const ParticleSceneData> completedParticleScene;
    std::shared_ptr<const ParticleFrameData> completedParticleFrame;
    std::shared_ptr<const EditorVisibility> completedVisibilityRequest;
    std::shared_ptr<const std::vector<War3RenderModelInstanceV2>> completedInstanceRequest;
    std::uint64_t completedModelEpoch = 0;
    std::uint64_t nextRosterTicket = 0;
    std::array<War3RenderInstanceRosterResultV2, 64> rosterResults{};
    std::array<War3RenderFrameMetricsV2, WAR3_RENDER_FRAME_METRICS_RING_CAPACITY> frameMetrics{};
    std::size_t metricsFirst = 0, metricsCount = 0;
    std::array<char, 256> metricsAdapterName{};
    std::uint32_t metricsFeatureLevel = 0, metricsSoftware = 0;
#ifdef WAR3_RENDER_TEST_FAULTS
    std::atomic<std::uint32_t> testFault{0};
    std::atomic<bool> testRosterGateReached{false};
    std::array<std::uint64_t, 7> completedAssetResources{};
#endif
    std::int32_t skinUploadResult = 0;
    std::int32_t visibilityUploadResult = 0;
    std::uint64_t captureDone = 0;
    std::thread thread;
    explicit Renderer(const War3RenderCreateInfoV2& info) {
        state.width = info.width; state.height = info.height; warp = info.forceWarp != 0;
        status.width = info.width; status.height = info.height;
        status.software = warp ? 1u : 0u;
        thread = std::thread([this] { Run(); });
    }
    ~Renderer() {
        { std::lock_guard lock(mutex); closing = true; }
        changed.notify_all();
        if (thread.joinable()) {
            {
                // DXGI 可同步 SendMessage 到 HWND 所属线程。只处理 sent messages，
                // 不取走/派发 Qt posted events、用户输入或 DeferredDelete，避免普通 join 互等。
                // 即使最新请求已 detach，worker 仍可能处理上一 HWND，不能只检查 state.window。
                // https://learn.microsoft.com/en-us/windows/win32/direct3darticles/dxgi-best-practices
                const HANDLE worker = static_cast<HANDLE>(thread.native_handle());
                while (!workerExited.load(std::memory_order_acquire)) {
                    const DWORD waited = MsgWaitForMultipleObjectsEx(1, &worker, INFINITE, QS_SENDMESSAGE, MWMO_INPUTAVAILABLE);
                    if (waited == WAIT_OBJECT_0) break;
                    MSG message{};
                    (void)PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
                    if (waited == WAIT_FAILED) Sleep(1); // 使用退出标记兜底，不能无限空转或卸载仍在执行的 DLL。
                }
            }
            thread.join();
        }
    }
    void ReportFailure(HRESULT error, std::uint64_t captured) {
        std::lock_guard lock(mutex);
        status.result = static_cast<std::int32_t>(error);
        status.deviceReady = 0;
        std::snprintf(status.message, sizeof(status.message), "D3D11 failed: 0x%08lX", static_cast<unsigned long>(error));
        pixels.clear(); captureDone = captured;
        for (auto& ticket : rosterResults) if (ticket.state == WAR3_RENDER_ROSTER_QUEUED) {
            ticket.state = WAR3_RENDER_ROSTER_REJECTED; ticket.result = static_cast<std::int32_t>(error);
        }
        state.roster.reset();
        changed.notify_all();
    }
    void Run() noexcept {
        struct Exited { std::atomic<bool>& value; ~Exited() { value.store(true, std::memory_order_release); } } exited{workerExited};
        try {
            D3D11Scene scene;
#ifdef WAR3_RENDER_TEST_FAULTS
            scene.SetTestFaultSource(&testFault);
#endif
            HRESULT failure = scene.Initialize(warp);
            {
                std::lock_guard lock(mutex);
                metricsAdapterName = scene.AdapterName();
                metricsFeatureLevel = scene.FeatureLevel();
                metricsSoftware = scene.SoftwareDevice() ? 1u : 0u;
            }
            std::uint64_t lastRequest = 0;
            std::shared_ptr<const Mesh> mesh = std::make_shared<Mesh>();
            std::shared_ptr<const ModelScene> models = std::make_shared<ModelScene>();
            HRESULT modelError = S_OK;
            std::shared_ptr<const ModelScene> attemptedModels;
            std::shared_ptr<const SkinPose> attemptedPose;
            std::shared_ptr<const ParticleSceneData> particles, attemptedParticles;
            std::shared_ptr<const ParticleFrameData> attemptedParticleFrame;
            std::shared_ptr<const Environment> environment; // 设备初始化已写入默认环境
            HRESULT environmentError = S_OK;
            HRESULT overlayError = S_OK;
            std::shared_ptr<const std::vector<War3RenderModelInstanceV2>> instances;
            std::shared_ptr<const std::vector<War3RenderVertexV2>> overlay = std::make_shared<std::vector<War3RenderVertexV2>>();
            std::uint64_t appliedSceneEpoch = 0;
            HRESULT meshError = S_OK;
            for (;;) {
                FrameState next;
                {
                    std::unique_lock lock(mutex);
                    changed.wait(lock, [&] { return closing || state.request != lastRequest; });
                    if (closing) break;
                    next = state; lastRequest = next.request;
                }
                if (FAILED(failure)) { ReportFailure(failure, next.capture); continue; }
                const auto uploadStarted = std::chrono::steady_clock::now();
                auto hr = scene.SetTarget(next.window, next.width, next.height);
                if (SUCCEEDED(hr) && mesh != next.mesh) {
                    const auto upload = scene.SetMesh(*next.mesh);
                    if (SUCCEEDED(upload)) {
                        mesh = next.mesh; appliedSceneEpoch = next.sceneEpoch; meshError = S_OK;
                    } else {
                        // SetMesh 的临时资源全部丢弃，仍绘制上一完整场景。保留错误，
                        // 不把旧图的帧回执当新提交成功；后续有效提交可再次尝试。
                        meshError = upload;
                        {
                            std::lock_guard lock(mutex);
                            if (state.mesh == next.mesh) {
                                state.mesh = mesh; ++state.sceneEpoch; ++state.request;
                            }
                        }
                        next.mesh = mesh; next.sceneEpoch = appliedSceneEpoch;
                    }
                }
                if (SUCCEEDED(hr) && next.particles) {
                    if (attemptedParticles != next.particles || attemptedParticleFrame != next.particleFrame) {
                        // 两类候选资源全部就绪才共同发布，姿势成功不能掩盖粒子失败。
                        modelError = models == next.models && particles == next.particles
                            ? scene.SetParticleFrame(*next.skinPose, *next.particleFrame)
                            : scene.SetParticleModels(*next.models, *next.skinPose, *next.particles, *next.particleFrame);
                        attemptedModels = next.models; attemptedPose = next.skinPose;
                        attemptedParticles = next.particles; attemptedParticleFrame = next.particleFrame;
                        if (SUCCEEDED(modelError)) { models = next.models; particles = next.particles; }
                    }
                } else if (SUCCEEDED(hr) && (attemptedModels != next.models || attemptedParticles)) {
                    // 目标与实际安装分开记录；上传失败不把新CPU指针当作已画出的模型。
                    modelError = scene.SetModels(*next.models, next.skinPose.get());
                    attemptedModels = next.models; attemptedPose = next.skinPose;
                    attemptedParticles.reset(); attemptedParticleFrame.reset();
                    if (SUCCEEDED(modelError)) { models = next.models; particles.reset(); scene.ClearParticles(); }
                }
                if (SUCCEEDED(hr) && !next.particles && models == next.models && next.skinPose && attemptedPose != next.skinPose) {
                    modelError = scene.SetSkinPose(*next.skinPose);
                    attemptedPose = next.skinPose;
                    if (SUCCEEDED(modelError)) { particles.reset(); scene.ClearParticles(); }
                }
                if (SUCCEEDED(hr) && models == next.models && instances != next.instances) {
                    if (next.instances && SUCCEEDED(modelError)) modelError = scene.SetModelInstances(*next.instances);
                    instances = next.instances;
                }
                // 纯呈现状态不写实例/矩阵/粒子缓冲；只有整对模型与粒子成功安装后才采用本掩码。
                HRESULT visibilityError = modelError;
                if (SUCCEEDED(hr) && SUCCEEDED(modelError) && models == next.models)
                    visibilityError = scene.SetEditorVisibility(next.visibility);
                if (SUCCEEDED(hr) && overlay != next.overlay) {
                    overlayError = scene.SetOverlay(*next.overlay); // 失败时不画叠加，不影响场景
                    overlay = next.overlay;
                }
                if (SUCCEEDED(hr) && environment != next.environment) {
                    // 阴影图上传失败时保留上一组环境继续绘制，状态行给出原因。
                    environmentError = scene.SetEnvironment(*next.environment);
                    environment = next.environment;
                }
                std::shared_ptr<const RosterCandidate> installedRoster;
                if (SUCCEEDED(hr) && next.roster) {
                    const auto roster = next.roster;
                    std::unique_ptr<D3D11Scene::PreparedInstanceRoster> prepared;
                    const auto oldSkin = scene.SkinStatus();
                    auto rosterError = models == roster->baseModels && SUCCEEDED(modelError)
                        && SUCCEEDED(visibilityError) && oldSkin.sceneVersion == roster->expected.sceneVersion
                        && oldSkin.contentVersion == roster->expected.contentVersion
                        ? roster->models
                            ? scene.PrepareAssetRoster(roster->models, roster->pose, roster->particleAssets,
                                roster->particleFrame, roster->visibility, prepared)
                            : scene.PrepareInstanceRoster(roster->pose, *roster->baseModels,
                                roster->particleFrame, roster->visibility, prepared) : E_INVALIDARG;
#ifdef WAR3_RENDER_TEST_FAULTS
                    if (testFault.load() == 6) {
                        testRosterGateReached.store(true);
                        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
                        while (testFault.load() == 6 && !closing && std::chrono::steady_clock::now() < deadline)
                            std::this_thread::sleep_for(std::chrono::milliseconds(1));
                        testRosterGateReached.store(false);
                    }
#endif
                    std::lock_guard lock(mutex);
                    auto& ticket = rosterResults[(roster->ticket - 1) % rosterResults.size()];
                    if (state.roster != roster || !RosterBaseMatches(state, *roster)) {
                        if (ticket.ticket == roster->ticket && ticket.state == WAR3_RENDER_ROSTER_QUEUED) {
                            ticket.state = WAR3_RENDER_ROSTER_SUPERSEDED; ticket.result = kTimeout;
                        }
                        if (state.roster == roster) { state.roster.reset(); ++state.request; }
                    } else if (FAILED(rosterError)) {
                        ticket.state = WAR3_RENDER_ROSTER_REJECTED; ticket.result = static_cast<std::int32_t>(rosterError);
                        state.roster.reset(); ++state.request;
                    } else {
                        // 唯一发布点：CAS之后只有noexcept GPU拥有者swap及CPU shared_ptr/标量替换。
                        scene.PublishInstanceRoster(*prepared);
                        if (roster->models) {
                            state.models = roster->models; state.particles = roster->particleAssets;
                            state.assetIdentity = roster->candidateAssetIdentity;
                        }
                        state.skinLayout = roster->layout; state.skinPose = roster->pose;
                        state.particleFrame = roster->particleFrame; state.visibility = roster->visibility;
                        state.instances.reset(); ++state.modelEpoch;
                        state.lastSkinSceneVersion = roster->pose->sceneVersion;
                        state.lastPresentationRevision = roster->visibility->status.presentationRevision;
                        state.modelLayers = roster->layers; ++state.layersRevision;
                        state.roster.reset(); ++state.request;
                        // 相机/表面可能已更新；本次Draw仍明确使用next中实际安装的视图身份。
                        next.skinLayout = state.skinLayout; next.skinPose = state.skinPose;
                        next.models = state.models; next.particles = state.particles; next.assetIdentity = state.assetIdentity;
                        next.particleFrame = state.particleFrame; next.visibility = state.visibility;
                        next.instances.reset(); next.modelEpoch = state.modelEpoch;
                        next.modelLayers = state.modelLayers; next.layersRevision = state.layersRevision;
                        models = attemptedModels = next.models; particles = attemptedParticles = next.particles;
                        attemptedPose = next.skinPose; attemptedParticleFrame = next.particleFrame;
                        modelError = visibilityError = S_OK; instances.reset();
                        installedRoster = roster;
                    }
                    next.roster.reset();
                }
                std::vector<std::uint8_t> captured;
                if (SUCCEEDED(hr)) scene.SetModelLayers(*next.modelLayers);
                const auto uploadCpuNs = CpuNanoseconds(std::chrono::steady_clock::now() - uploadStarted);
                std::uint64_t completed;
                { std::lock_guard lock(mutex); completed = captureDone; }
                const bool capture = next.capture > completed;
                float viewProjection[16]{};
                D3D11Scene::DrawTimings drawTimings;
                if (SUCCEEDED(hr)) hr = scene.Draw(next.camera, capture ? &captured : nullptr, closing, viewProjection, &drawTimings);
                if (FAILED(hr)) { failure = hr; ReportFailure(hr, next.capture); continue; }
                {
                    std::lock_guard lock(mutex);
                    status.deviceReady = 1;
                    // 较新场景已在排队时，不让被取代提交的上传错误提前终止新 capture。
                    status.result = state.mesh == next.mesh ? static_cast<std::int32_t>(meshError) : 0;
                    status.width = next.width; status.height = next.height;
                    status.documentRevision = next.mesh->revision;
                    ++status.frameCount;
                    completedLayers = next.modelLayers;
                    layerStatuses = scene.ModelLayerStatuses();
                    for (auto& layer : layerStatuses) layer.skin.frameCount = status.frameCount;
                    skinStatus = scene.SkinStatus(); skinStatus.frameCount = status.frameCount;
                    completedModelRequest = next.models; completedPoseRequest = next.skinPose;
                    completedParticleScene = next.particles; completedParticleFrame = next.particleFrame;
                    completedVisibilityRequest = next.visibility; completedInstanceRequest = next.instances;
                    completedModelEpoch = next.modelEpoch; visibilityUploadResult = static_cast<std::int32_t>(visibilityError);
#ifdef WAR3_RENDER_TEST_FAULTS
                    completedAssetResources = scene.TestAssetResources();
#endif
                    skinUploadResult = static_cast<std::int32_t>(modelError);
                    skinStatus.result = skinUploadResult;
                    viewInfo = {sizeof(War3RenderViewInfoV2), next.width, next.height, status.frameCount,
                        next.mesh->revision, next.surfaceEpoch, next.sceneEpoch, next.viewEpoch, {}};
                    std::copy_n(viewProjection, 16, viewInfo.viewProjection);
                    completedEditorFrame = {sizeof(War3RenderEditorCompletedFrameV2), viewInfo, next.camera,
                        skinStatus, scene.ParticleStatus(), next.visibility && SUCCEEDED(visibilityError)
                            ? next.visibility->status : War3RenderEditorPresentationStatusV2{sizeof(War3RenderEditorPresentationStatusV2)}};
                    if (installedRoster) {
                        auto& ticket = rosterResults[(installedRoster->ticket - 1) % rosterResults.size()];
                        if (ticket.ticket == installedRoster->ticket && ticket.state == WAR3_RENDER_ROSTER_QUEUED) {
                            if (state.models != next.models || state.skinPose != next.skinPose
                                || state.particleFrame != next.particleFrame || state.visibility != next.visibility
                                || state.modelEpoch != next.modelEpoch || state.modelLayers != next.modelLayers
                                || state.layersRevision != next.layersRevision) {
                                ticket.state = WAR3_RENDER_ROSTER_SUPERSEDED; ticket.result = kTimeout;
                            } else {
                                ticket.state = WAR3_RENDER_ROSTER_COMPLETED; ticket.result = 0;
                                ticket.completed = completedEditorFrame;
                            }
                        }
                    }
                    War3RenderFrameMetricsV2 metrics{};
                    metrics.structSize = sizeof(metrics);
                    metrics.flags = WAR3_RENDER_FRAME_METRICS_DRAW_COMPLETED;
                    metrics.result = FAILED(meshError) ? static_cast<std::int32_t>(meshError)
                        : FAILED(modelError) ? static_cast<std::int32_t>(modelError)
                        : FAILED(visibilityError) ? static_cast<std::int32_t>(visibilityError)
                        : FAILED(environmentError) ? static_cast<std::int32_t>(environmentError)
                        : static_cast<std::int32_t>(overlayError);
                    for (const auto& layer : layerStatuses)
                        if (!metrics.result && layer.result < 0) metrics.result = layer.result;
                    if (!metrics.result) metrics.flags |= WAR3_RENDER_FRAME_METRICS_UPLOAD_VALID;
                    if (next.window) metrics.flags |= WAR3_RENDER_FRAME_METRICS_WINDOW_ATTACHED;
                    if (drawTimings.presentResult == DXGI_STATUS_OCCLUDED)
                        metrics.flags |= WAR3_RENDER_FRAME_METRICS_PRESENT_OCCLUDED;
                    if (capture) metrics.flags |= WAR3_RENDER_FRAME_METRICS_READBACK;
                    metrics.presentResult = static_cast<std::int32_t>(drawTimings.presentResult);
                    metrics.completedSteadyNs = drawTimings.completedSteadyNs;
                    metrics.uploadCpuNs = uploadCpuNs; metrics.drawCpuNs = drawTimings.drawCpuNs;
                    metrics.presentCpuNs = drawTimings.presentCpuNs; metrics.readbackCpuNs = drawTimings.readbackCpuNs;
                    metrics.completed = completedEditorFrame;
                    // 指针/目标可已被更新；统计只从当前实际安装的 GPU 场景与显隐掩码取得。
                    metrics.completed.presentation = scene.EditorVisibilityStatus();
                    metrics.modelInstanceCount = scene.ModelInstanceCount();
                    metrics.particleDrawCount = metrics.completed.particles.drawCount;
                    metrics.visibleModelInstanceCount = metrics.completed.presentation.presentationRevision
                        ? metrics.completed.presentation.visibleInstanceCount : metrics.modelInstanceCount;
                    metrics.visibleParticleDrawCount = metrics.completed.presentation.presentationRevision
                        ? metrics.completed.presentation.visibleParticleDrawCount : metrics.particleDrawCount;
                    const auto metricIndex = (metricsFirst + metricsCount) % frameMetrics.size();
                    frameMetrics[metricIndex] = metrics;
                    if (metricsCount < frameMetrics.size()) ++metricsCount;
                    else metricsFirst = (metricsFirst + 1) % frameMetrics.size();
                    if (FAILED(meshError)) std::snprintf(status.message, sizeof(status.message),
                        "Scene upload failed: 0x%08lX; previous scene retained", static_cast<unsigned long>(meshError));
                    else if (FAILED(modelError)) std::snprintf(status.message, sizeof(status.message),
                        "Model upload failed: 0x%08lX; previous models retained", static_cast<unsigned long>(modelError));
                    else if (FAILED(environmentError)) std::snprintf(status.message, sizeof(status.message),
                        "Environment upload failed: 0x%08lX; previous environment retained",
                        static_cast<unsigned long>(environmentError));
                    else std::snprintf(status.message, sizeof(status.message), "%s / Feature Level 11.0 / %s",
                        warp ? "D3D11 WARP" : "D3D11 hardware", next.mesh->terrain ? "terrain base layer" : "height colors");
                    if (capture) {
                        pixels = std::move(captured); captureDone = next.capture;
                        captureInfo = {sizeof(War3RenderCaptureInfoV2), next.width, next.height,
                            status.frameCount, next.mesh->revision, next.capture};
                        // 和 pixels 在同一临界区保存。普通后续帧只能更新 viewInfo，
                        // 不能把新相机/矩阵贴到上一次读回像素的身份上。
                        capturedFrame = {sizeof(War3RenderCapturedFrameV2), captureInfo, viewInfo, next.camera};
                        capturedSkinFrame = {sizeof(War3RenderSkinnedCapturedFrameV2), capturedFrame, skinStatus};
                        capturedParticleFrame = {sizeof(War3RenderParticleCapturedFrameV2), capturedSkinFrame, scene.ParticleStatus()};
                        capturedPresentationFrame = {sizeof(War3RenderEditorCapturedFrameV2), capturedFrame, skinStatus,
                            scene.ParticleStatus(), next.visibility && SUCCEEDED(visibilityError)
                                ? next.visibility->status : War3RenderEditorPresentationStatusV2{sizeof(War3RenderEditorPresentationStatusV2)}};
                        capturedLayeredFrame = {};
                        capturedLayeredFrame.structSize = sizeof(capturedLayeredFrame);
                        capturedLayeredFrame.layerCount = static_cast<std::uint32_t>(layerStatuses.size());
                        capturedLayeredFrame.layersRevision = next.layersRevision;
                        capturedLayeredFrame.world = capturedPresentationFrame;
                        std::copy(layerStatuses.begin(), layerStatuses.end(), capturedLayeredFrame.layers);
                    }
                }
                changed.notify_all();
            }
        } catch (...) {
            std::uint64_t captured;
            { std::lock_guard lock(mutex); captured = state.capture; }
            ReportFailure(E_FAIL, captured);
        }
    }
};
bool SizeValid(std::uint32_t width, std::uint32_t height) { return width > 0 && height > 0 && width <= kMaxSize && height <= kMaxSize; }
template<class Operation>
std::int32_t Update(War3RendererHandle handle, Operation operation) noexcept {
    if (!handle) return kInvalid;
    try {
        auto& renderer = *static_cast<Renderer*>(handle);
        { std::lock_guard lock(renderer.mutex); operation(renderer.state); ++renderer.state.request; }
        renderer.changed.notify_all();
        return 0;
    } catch (...) { return static_cast<std::int32_t>(E_OUTOFMEMORY); }
}
War3RendererHandle WAR3_RENDER_CALL Create(const War3RenderCreateInfoV2* info) noexcept {
    if (!info || info->structSize != sizeof(*info) || !SizeValid(info->width, info->height) || info->forceWarp > 1) return nullptr;
    try { return new Renderer(*info); } catch (...) { return nullptr; }
}
void WAR3_RENDER_CALL Destroy(War3RendererHandle renderer) noexcept { try { delete static_cast<Renderer*>(renderer); } catch (...) {} }
std::int32_t WAR3_RENDER_CALL Attach(War3RendererHandle renderer, void* window) noexcept {
    if (window && !IsWindow(static_cast<HWND>(window))) return kInvalid;
    return Update(renderer, [window](auto& state) { state.window = static_cast<HWND>(window); ++state.surfaceEpoch; });
}
std::int32_t WAR3_RENDER_CALL Resize(War3RendererHandle renderer, std::uint32_t width, std::uint32_t height) noexcept {
    if (!SizeValid(width, height)) return kInvalid;
    return Update(renderer, [=](auto& state) { state.width = width; state.height = height; ++state.surfaceEpoch; });
}
bool Coordinate(float value) { return std::isfinite(value) && std::abs(value) <= 1.0e7f; }
std::int32_t WAR3_RENDER_CALL SetCamera(War3RendererHandle renderer, const War3RenderCameraV2* camera) noexcept {
    if (!camera || camera->structSize != sizeof(*camera) || !Coordinate(camera->targetX) || !Coordinate(camera->targetY) ||
        !Coordinate(camera->targetZ) || !Coordinate(camera->yawRadians) || !std::isfinite(camera->pitchRadians) ||
        !std::isfinite(camera->distance) || camera->distance < 16 || camera->distance > 400000 ||
        camera->pitchRadians < 0.05f || camera->pitchRadians > 1.5f) return kInvalid;
    return Update(renderer, [camera](auto& state) { state.camera = *camera; ++state.viewEpoch; });
}
std::int32_t UpdateScene(War3RendererHandle handle, std::shared_ptr<const Mesh> mesh) noexcept {
    return Update(handle, [handle, mesh = std::move(mesh)](auto& state) {
        state.mesh = mesh; ++state.sceneEpoch;
        // 旧的可恢复上传错误不能阻止新提交后的显式 capture 等待。设备故障仍锁存；
        // sceneEpoch 已推进，所以此刻 getViewInfo 只能报告待完成，不能返回旧帧成功。
        auto& status = static_cast<Renderer*>(handle)->status;
        if (status.deviceReady) status.result = 0;
    });
}
std::int32_t WAR3_RENDER_CALL Submit(War3RendererHandle renderer, const War3RenderMeshV2* input) noexcept {
    if (!input || input->structSize != sizeof(*input) || input->vertexCount > kMaxVertices || input->indexCount > kMaxIndices ||
        input->indexCount % 3 != 0 || (input->vertexCount && !input->vertices) || (input->indexCount && !input->indices) ||
        (input->vertexCount == 0) != (input->indexCount == 0)) return kInvalid;
    try {
        auto mesh = std::make_shared<Mesh>();
        mesh->revision = input->documentRevision;
        if (input->vertexCount) {
            mesh->vertices.assign(input->vertices, input->vertices + input->vertexCount);
            mesh->indices.assign(input->indices, input->indices + input->indexCount);
        }
        for (const auto& vertex : mesh->vertices) if (!Coordinate(vertex.x) || !Coordinate(vertex.y) || !Coordinate(vertex.z) ||
            !std::isfinite(vertex.red) || !std::isfinite(vertex.green) || !std::isfinite(vertex.blue) ||
            vertex.red < 0 || vertex.red > 1 || vertex.green < 0 || vertex.green > 1 || vertex.blue < 0 || vertex.blue > 1) return kInvalid;
        for (const auto index : mesh->indices) if (index >= mesh->vertices.size()) return kInvalid;
        return UpdateScene(renderer, std::move(mesh));
    } catch (...) { return static_cast<std::int32_t>(E_OUTOFMEMORY); }
}
struct TextureBudget { std::uint64_t perTexture, perChain, total; };
constexpr TextureBudget kTerrainTextureBudget{kMaxTextureBytes, kMaxMipChainBytes, kMaxTextureTotal};
struct TextureChains {
    std::vector<War3RenderTextureMipChainV2> descriptions;
    std::vector<std::vector<War3RenderMipRgbaV2>> levels;
    std::uint64_t descriptorBytes = 0, pixelBytes = 0;
};
// 地表与模型共用的 mip 链校验：描述符先复制再逐级核尺寸/字节/预算，不读取像素。
bool ValidateTextureChains(const War3RenderTextureMipChainV2* input, std::uint32_t count, const TextureBudget& budget,
    TextureChains& output) {
    if (count && !input) return false;
    if (count) output.descriptions.assign(input, input + count);
    output.levels.assign(output.descriptions.size(), {});
    for (std::size_t i = 0; i < output.descriptions.size(); ++i) {
        const auto& texture = output.descriptions[i];
        if (texture.structSize != sizeof(texture) || texture.mipCount == 0 || texture.mipCount > kMaxMipLevels
            || texture.maxSampleMip >= texture.mipCount || !texture.mips
            || texture.atlasColumns == 0 || texture.atlasRows == 0) return false;
        auto& levels = output.levels[i];
        levels.assign(texture.mips, texture.mips + texture.mipCount);
        output.descriptorBytes += static_cast<std::uint64_t>(texture.mipCount) * sizeof(War3RenderMipRgbaV2);
        std::uint64_t chainBytes = 0;
        for (std::size_t level = 0; level < levels.size(); ++level) {
            const auto& mip = levels[level];
            if (mip.structSize != sizeof(mip) || mip.width == 0 || mip.height == 0
                || mip.width > kMaxTextureSize || mip.height > kMaxTextureSize || !mip.rgba
                || mip.rgbaByteCount != static_cast<std::uint64_t>(mip.width) * mip.height * 4
                || mip.rgbaByteCount > budget.perTexture || mip.rgbaByteCount > budget.perChain - chainBytes
                || mip.rgbaByteCount > budget.total - output.pixelBytes) return false;
            if (level != 0) {
                const auto& previous = levels[level - 1];
                if ((previous.width == 1 && previous.height == 1)
                    || mip.width != std::max(1u, previous.width / 2)
                    || mip.height != std::max(1u, previous.height / 2)) return false;
            }
            if (level <= texture.maxSampleMip &&
                (mip.width % texture.atlasColumns != 0 || mip.height % texture.atlasRows != 0)) return false;
            chainBytes += mip.rgbaByteCount; output.pixelBytes += mip.rgbaByteCount;
        }
    }
    return true;
}
std::vector<War3Map::Render::TextureRgba> CopyTextureChains(const TextureChains& chains) {
    std::vector<War3Map::Render::TextureRgba> textures;
    textures.reserve(chains.descriptions.size());
    for (std::size_t i = 0; i < chains.descriptions.size(); ++i) {
        const auto& description = chains.descriptions[i];
        War3Map::Render::TextureRgba texture;
        texture.maxSampleMip = description.maxSampleMip;
        texture.atlasColumns = description.atlasColumns; texture.atlasRows = description.atlasRows;
        texture.mips.reserve(chains.levels[i].size());
        for (const auto& level : chains.levels[i]) {
            War3Map::Render::TextureMipRgba mip;
            mip.width = level.width; mip.height = level.height;
            const auto* bytes = static_cast<const std::uint8_t*>(level.rgba);
            mip.rgba.assign(bytes, bytes + static_cast<std::size_t>(level.rgbaByteCount));
            texture.mips.push_back(std::move(mip));
        }
        textures.push_back(std::move(texture));
    }
    return textures;
}
template<class Scene> std::uint64_t ResidentTextureBytes(const Scene& scene) noexcept {
    std::uint64_t total = 0;
    for (const auto* page = &scene; page; page = page->staticBase.get())
        for (const auto& texture : page->StaticTextures()) for (const auto& mip : texture.mips) total += mip.rgba.size();
    return total;
}
std::uint64_t ResidentTextureBytes(const ModelScene& scene) noexcept { return scene.TextureBytes(); }
bool CopySkinnedScene(const War3RenderSkinnedModelSceneV2& input, ModelScene& scene) {
    std::uint64_t payload = 0;
    if (!War3Map::Render::CopySkinnedModelInput(input, scene, payload)) return false;
    TextureChains chains;
    if (!ValidateTextureChains(input.textures, input.textureCount,
        TextureBudget{kMaxTextureBytes, kMaxMipChainBytes, kMaxModelTextureTotal}, chains)) return false;
    for (const auto& texture : chains.descriptions) if (texture.atlasColumns != 1 || texture.atlasRows != 1) return false;
    if (payload + chains.descriptorBytes + chains.pixelBytes > kMaxModelBytes) return false;
    scene.textures = CopyTextureChains(chains);
    return true;
}
bool CopyParticleScene(const War3RenderParticleSceneV2& input, ModelSceneLayerData& output) {
    if (input.structSize != sizeof(input) || input.textureCount > 256 || !input.models || !input.initialFrame) return false;
    auto models = std::make_shared<ModelScene>();
    if (!CopySkinnedScene(*input.models, *models)) return false;
    TextureChains chains;
    if (!ValidateTextureChains(input.textures, input.textureCount, kTerrainTextureBudget, chains)) return false;
    for (const auto& texture : chains.descriptions) if (texture.atlasColumns != 1 || texture.atlasRows != 1) return false;
    auto frame = std::make_shared<ParticleFrameData>();
    auto pose = std::make_shared<SkinPose>();
    if (!War3Map::Render::CopyParticleInput(*input.initialFrame, input.textureCount, *frame)
        || !War3Map::Render::CopySkinUpdate(*input.initialFrame->modelPose, *models, *pose)
        || pose->contentVersion != models->initialSkin->contentVersion) return false;
    // 两条发布路径共用完整布局、动态 GEOA 与纹理校验，不能截断可选尾部。
    models->initialSkin = pose;
    auto particles = std::make_shared<ParticleSceneData>();
    particles->textures = CopyTextureChains(chains); particles->initial = frame;
    output.models = std::move(models); output.pose = std::move(pose);
    output.particles = std::move(particles); output.particleFrame = std::move(frame);
    return true;
}
std::int32_t WAR3_RENDER_CALL SubmitTerrainMips(War3RendererHandle renderer, const War3RenderTerrainMipSceneV2* input) noexcept {
    if (!renderer || !input || input->structSize != sizeof(*input) || input->vertexCount > kMaxTerrainVertices
        || input->indexCount > kMaxTerrainIndices || input->textureCount > kMaxTerrainTextures
        || input->batchCount > kMaxTerrainBatches || input->indexCount % 3 != 0
        || (input->vertexCount == 0) != (input->indexCount == 0)
        || (input->indexCount == 0) != (input->batchCount == 0)
        || (input->vertexCount && !input->vertices) || (input->indexCount && !input->indices)
        || (input->textureCount && !input->textures) || (input->batchCount && !input->batches)
        || (!input->indexCount && input->textureCount)) return kInvalid;
    try {
        // 描述符先复制/验证预算；任何失败都不进入 Update，不发布半份场景。
        TextureChains chains;
        if (!ValidateTextureChains(input->textures, input->textureCount, kTerrainTextureBudget, chains)) return kInvalid;
        const auto& descriptions = chains.descriptions;
        const auto mipDescriptorBytes = chains.descriptorBytes, textureBytes = chains.pixelBytes;
        const std::uint64_t payloadBytes = static_cast<std::uint64_t>(input->vertexCount) * sizeof(War3RenderTerrainVertexV2)
            + static_cast<std::uint64_t>(input->indexCount) * sizeof(std::uint32_t)
            + static_cast<std::uint64_t>(input->batchCount) * sizeof(War3RenderTerrainBatchV2)
            + static_cast<std::uint64_t>(input->textureCount) * sizeof(War3RenderTextureMipChainV2)
            + mipDescriptorBytes + textureBytes;
        if (payloadBytes > kMaxTerrainBytes) return kInvalid;
        auto mesh = std::make_shared<Mesh>();
        mesh->terrain = true; mesh->revision = input->documentRevision;
        if (input->vertexCount) {
            mesh->terrainVertices.assign(input->vertices, input->vertices + input->vertexCount);
            mesh->indices.assign(input->indices, input->indices + input->indexCount);
            mesh->batches.assign(input->batches, input->batches + input->batchCount);
        }
        for (const auto& vertex : mesh->terrainVertices) {
            if (!Coordinate(vertex.x) || !Coordinate(vertex.y) || !Coordinate(vertex.z)
                || !std::isfinite(vertex.red) || !std::isfinite(vertex.green) || !std::isfinite(vertex.blue)
                || vertex.red < 0 || vertex.red > 1 || vertex.green < 0 || vertex.green > 1 || vertex.blue < 0 || vertex.blue > 1
                || !std::isfinite(vertex.u) || !std::isfinite(vertex.v) || vertex.u < 0 || vertex.u > 1 || vertex.v < 0 || vertex.v > 1)
                return kInvalid;
        }
        for (const auto index : mesh->indices) if (index >= mesh->terrainVertices.size()) return kInvalid;
        std::uint32_t nextIndex = 0;
        for (const auto& batch : mesh->batches) {
            if (batch.firstIndex != nextIndex || batch.indexCount == 0 || batch.indexCount % 3 != 0
                || batch.indexCount > input->indexCount - nextIndex
                || (batch.textureIndex != WAR3_RENDER_HEIGHT_COLOR_TEXTURE
                    && (batch.textureIndex & ~(WAR3_RENDER_TERRAIN_ALPHA_BLEND | WAR3_RENDER_TERRAIN_TRANSLUCENT)) >= descriptions.size()))
                return kInvalid;
            nextIndex += batch.indexCount;
        }
        if (nextIndex != input->indexCount) return kInvalid;
        mesh->textures = CopyTextureChains(chains);
        return UpdateScene(renderer, std::move(mesh));
    } catch (...) { return static_cast<std::int32_t>(E_OUTOFMEMORY); }
}
bool Unit(float value) { return std::isfinite(value) && value >= 0 && value <= 1; }
std::uint64_t PosePayloadBytes(const SkinPose& pose) noexcept {
    return std::uint64_t(pose.instances.size()) * sizeof(War3RenderSkinnedModelInstanceV2)
        + std::uint64_t(pose.groups.size()) * sizeof(War3RenderGroupMatrixV2)
        + std::uint64_t(pose.alphas.size()) * sizeof(float) + std::uint64_t(pose.colors.size()) * sizeof(War3RenderMeshColorV2)
        + std::uint64_t(pose.layerAlphas.size()) * sizeof(float) + std::uint64_t(pose.layerSlices.size()) * sizeof(War3RenderMaterialLayerSliceV2)
        + std::uint64_t(pose.materialStates.size()) * sizeof(War3RenderMaterialStateV2)
        + std::uint64_t(pose.materialStateSlices.size()) * sizeof(War3RenderMaterialLayerSliceV2);
}
bool ValidateUnmaskedMaterialWork(const ModelScene& models, const SkinPose* pose, const ParticleFrameData* particles) {
    if (!models.dynamicMaterialStates) return true;
    if (!pose) return false;
    const auto maskBytes = std::uint64_t(models.instances.size()) + (particles ? particles->draws.size() : 0);
    const auto bytes = PosePayloadBytes(*pose)
        + (particles ? particles->PayloadBytes() : 0) + maskBytes;
    const auto limit = (particles ? 128ull : 64ull) * 1024 * 1024;
    if (bytes > limit) {
        char message[192]{};
        std::snprintf(message, sizeof(message), "[Render.MaterialState] MATERIAL_STATE_UNMASKED_BUDGET required=%llu limit=%llu\n",
            static_cast<unsigned long long>(bytes), static_cast<unsigned long long>(limit));
        (void)std::fputs(message, stderr); OutputDebugStringA(message); return false;
    }
    const std::vector<std::uint8_t> instances(models.instances.size(), 1);
    const std::vector<std::uint8_t> draws(particles ? particles->draws.size() : 0, 1);
    const War3RenderEditorPresentationTargetV2 target{sizeof(target),
        particles ? WAR3_EDITOR_PRESENTATION_PARTICLES : WAR3_EDITOR_PRESENTATION_SKINNED,
        static_cast<std::uint32_t>(instances.size()), static_cast<std::uint32_t>(draws.size()),
        1, pose->sceneVersion, pose->contentVersion};
    const War3RenderEditorVisibilityUpdateV2 mask{sizeof(mask), target.instanceCount, target.particleDrawCount,
        0, target, 1, instances.data(), draws.data(), nullptr, nullptr, nullptr};
    EditorVisibility checked; // 仅预算预检；不安装掩码或改变原呈现身份。
    return War3Map::Render::CopyEditorVisibility(models, particles, target, mask, checked, pose);
}
std::int32_t PublishModels(Renderer& renderer, std::shared_ptr<const ModelScene> models,
    std::shared_ptr<const SkinPose> pose, std::shared_ptr<const ParticleSceneData> particles,
    std::shared_ptr<const ParticleFrameData> frame, const War3RenderEditorVisibilityUpdateV2* input) {
    const auto emptyLayers = std::make_shared<ModelSceneLayers>();
    FrameState frozen;
    {
        std::lock_guard lock(renderer.mutex);
        frozen = renderer.state;
    }
    if (frozen.modelEpoch == std::numeric_limits<std::uint64_t>::max()) return kInvalid;
    std::shared_ptr<const EditorVisibility> visibility;
    if (input) {
        if (input->structSize != sizeof(*input) || input->reserved || input->expected.structSize < sizeof(input->expected)
            || input->modelPose || input->particleFrame || input->staticInstances || !input->presentationRevision
            || input->presentationRevision <= frozen.lastPresentationRevision
            || !SamePresentationTarget(input->expected, PresentationTarget(frozen))) return kInvalid;
        const War3RenderEditorPresentationTargetV2 target{sizeof(target),
            particles ? WAR3_EDITOR_PRESENTATION_PARTICLES : pose ? WAR3_EDITOR_PRESENTATION_SKINNED : WAR3_EDITOR_PRESENTATION_STATIC,
            static_cast<std::uint32_t>(models->instances.size()), frame ? static_cast<std::uint32_t>(frame->draws.size()) : 0,
            frozen.modelEpoch + 1, pose ? pose->sceneVersion : 0, pose ? pose->contentVersion : 0};
        auto candidate = std::make_shared<EditorVisibility>();
        if (!War3Map::Render::CopyEditorVisibility(*models, frame.get(), target, *input, *candidate, pose.get())) return kInvalid;
        visibility = std::move(candidate);
    } else if (!ValidateUnmaskedMaterialWork(*models, pose.get(), frame.get())) {
        return kInvalid;
    }
    {
        std::lock_guard lock(renderer.mutex);
        if (renderer.state.modelEpoch == std::numeric_limits<std::uint64_t>::max()
            || (pose && pose->sceneVersion <= renderer.state.lastSkinSceneVersion)) return kInvalid;
        if (input && (!SamePresentationTarget(input->expected, PresentationTarget(renderer.state))
            || input->presentationRevision <= renderer.state.lastPresentationRevision)) return kInvalid;
        renderer.state.models = std::move(models); renderer.state.skinPose = std::move(pose);
        renderer.state.skinLayout.reset();
        renderer.state.assetIdentity = renderer.state.skinPose ? renderer.state.skinPose->sceneVersion : 0;
        renderer.state.particles = std::move(particles); renderer.state.particleFrame = std::move(frame);
        renderer.state.instances.reset(); renderer.state.visibility = std::move(visibility);
        if (renderer.state.skinPose) renderer.state.lastSkinSceneVersion = renderer.state.skinPose->sceneVersion;
        ++renderer.state.modelEpoch;
        // 附加层只对建立时的主槽有效。正式替换主模型后不能复活旧来源的预览。
        renderer.state.modelLayers = emptyLayers;
        ++renderer.state.layersRevision;
        if (input) renderer.state.lastPresentationRevision = input->presentationRevision;
        ++renderer.state.request;
    }
    renderer.changed.notify_all();
    return 0;
}
std::int32_t SubmitModelsImpl(War3RendererHandle renderer, const War3RenderModelSceneV2* input,
    const War3RenderEditorVisibilityUpdateV2* visibility) noexcept {
    if (!renderer || !input || input->structSize != sizeof(*input) || input->vertexCount > kMaxModelVertices
        || input->indexCount > kMaxModelIndices || input->layerCount > kMaxModelLayers || input->meshCount > kMaxModelMeshes
        || input->modelCount > kMaxModels || input->textureCount > kMaxModelTextures
        || (input->vertexCount && !input->vertices) || (input->indexCount && !input->indices)
        || (input->layerCount && !input->layers) || (input->meshCount && !input->meshes)
        || (input->modelCount && !input->models) || (input->instanceCount && !input->instances)) return kInvalid;
    try {
        TextureChains chains;
        if (!ValidateTextureChains(input->textures, input->textureCount,
            TextureBudget{kMaxTextureBytes, kMaxMipChainBytes, kMaxModelTextureTotal}, chains)) return kInvalid;
        for (const auto& description : chains.descriptions)
            if (description.atlasColumns != 1 || description.atlasRows != 1) return kInvalid;
        const std::uint64_t payloadBytes = static_cast<std::uint64_t>(input->vertexCount) * sizeof(War3RenderModelVertexV2)
            + static_cast<std::uint64_t>(input->indexCount) * sizeof(std::uint16_t)
            + static_cast<std::uint64_t>(input->layerCount) * sizeof(War3RenderModelLayerV2)
            + static_cast<std::uint64_t>(input->meshCount) * sizeof(War3RenderModelMeshV2)
            + static_cast<std::uint64_t>(input->modelCount) * sizeof(War3RenderModelV2)
            + static_cast<std::uint64_t>(input->instanceCount) * sizeof(War3RenderModelInstanceV2)
            + chains.descriptorBytes + chains.pixelBytes;
        if (payloadBytes > kMaxModelBytes) return kInvalid;
        auto scene = std::make_shared<ModelScene>();
        if (input->vertexCount) scene->vertices.assign(input->vertices, input->vertices + input->vertexCount);
        if (input->indexCount) scene->indices.assign(input->indices, input->indices + input->indexCount);
        if (input->layerCount) scene->layers.assign(input->layers, input->layers + input->layerCount);
        if (input->meshCount) scene->meshes.assign(input->meshes, input->meshes + input->meshCount);
        if (input->modelCount) scene->models.assign(input->models, input->models + input->modelCount);
        if (input->instanceCount) scene->instances.assign(input->instances, input->instances + input->instanceCount);
        if (!War3Map::Render::ValidateModelGeometry(*scene, input->textureCount)) return kInvalid;
        scene->textures = CopyTextureChains(chains);
        return PublishModels(*static_cast<Renderer*>(renderer), std::move(scene), {}, {}, {}, visibility);
    } catch (...) { return static_cast<std::int32_t>(E_OUTOFMEMORY); }
}
std::int32_t WAR3_RENDER_CALL SubmitModels(War3RendererHandle renderer, const War3RenderModelSceneV2* input) noexcept {
    return SubmitModelsImpl(renderer, input, nullptr);
}
std::int32_t SubmitSkinnedModelsImpl(War3RendererHandle handle, const War3RenderSkinnedModelSceneV2* input,
    const War3RenderEditorVisibilityUpdateV2* visibility) noexcept {
    if (!handle || !input) return kInvalid;
    try {
        auto scene = std::make_shared<ModelScene>();
        if (!CopySkinnedScene(*input, *scene)) return kInvalid;
        const auto pose = scene->initialSkin;
        return PublishModels(*static_cast<Renderer*>(handle), std::move(scene), pose, {}, {}, visibility);
    } catch (...) { return static_cast<std::int32_t>(E_OUTOFMEMORY); }
}
std::int32_t WAR3_RENDER_CALL SubmitSkinnedModels(War3RendererHandle handle, const War3RenderSkinnedModelSceneV2* input) noexcept {
    return SubmitSkinnedModelsImpl(handle, input, nullptr);
}
std::int32_t WAR3_RENDER_CALL UpdateSkinMatrices(War3RendererHandle handle, const War3RenderSkinUpdateV2* input) noexcept {
    if (!handle || !input) return kInvalid;
    try {
        auto& renderer = *static_cast<Renderer*>(handle);
        FrameState frozen;
        { std::lock_guard lock(renderer.mutex); frozen = renderer.state; }
        if (!frozen.skinPose) return kInvalid;
        auto pose = std::make_shared<SkinPose>();
        if (!War3Map::Render::CopySkinUpdate(*input, SkinLayout(frozen), *pose)) return kInvalid;
        if (!ValidateUnmaskedMaterialWork(SkinLayout(frozen), pose.get(), nullptr)) return kInvalid;
        {
            std::lock_guard lock(renderer.mutex);
            if (renderer.state.models != frozen.models || renderer.state.skinLayout != frozen.skinLayout
                || renderer.state.skinPose != frozen.skinPose || !renderer.state.skinPose
                || pose->contentVersion <= renderer.state.skinPose->contentVersion) return kInvalid;
            renderer.state.skinPose = std::move(pose);
            renderer.state.visibility.reset();
            renderer.state.particles.reset(); renderer.state.particleFrame.reset(); ++renderer.state.request;
        }
        renderer.changed.notify_all();
        return 0;
    } catch (...) { return static_cast<std::int32_t>(E_OUTOFMEMORY); }
}
std::int32_t WAR3_RENDER_CALL GetSkinStatus(War3RendererHandle handle, War3RenderSkinStatusV2* output) noexcept {
    if (!handle || !output || output->structSize < sizeof(*output)) return kInvalid;
    try {
        auto& renderer = *static_cast<Renderer*>(handle); std::lock_guard lock(renderer.mutex);
        auto result = renderer.skinStatus;
        if (renderer.status.result < 0) result.result = renderer.status.result;
        else if (renderer.completedModelRequest != renderer.state.models || renderer.completedPoseRequest != renderer.state.skinPose
            || renderer.completedParticleScene != renderer.state.particles || renderer.completedParticleFrame != renderer.state.particleFrame)
            result.result = kTimeout;
        else result.result = renderer.skinUploadResult;
        const auto capacity = output->structSize;
        *output = result;
        if (capacity >= sizeof(War3RenderSkinColorCapabilitiesV2)) {
            auto& extended = *reinterpret_cast<War3RenderSkinColorCapabilitiesV2*>(output);
            extended.colorFormat = WAR3_RENDER_STATIC_MESH_RGB_V1;
            extended.reserved = 0;
        }
        if (capacity >= sizeof(War3RenderSkinDynamicColorCapabilitiesV2)) {
            auto& extended = *reinterpret_cast<War3RenderSkinDynamicColorCapabilitiesV2*>(output);
            extended.dynamicColorFormat = WAR3_RENDER_DYNAMIC_MESH_RGB_V1;
            extended.reserved = 0;
        }
        if (capacity >= sizeof(War3RenderSkinSharedRangeCapabilitiesV2)) {
            auto& extended = *reinterpret_cast<War3RenderSkinSharedRangeCapabilitiesV2*>(output);
            extended.sharedRangeMatrixFormat = WAR3_RENDER_GROUP_LOCAL_COLUMN_3X4_SHARED_RANGES;
            extended.reserved = 0;
        }
        if (capacity >= sizeof(War3RenderSkinMaterialCapabilitiesV2)) {
            auto& extended = *reinterpret_cast<War3RenderSkinMaterialCapabilitiesV2*>(output);
            extended.layerAlphaFormat = WAR3_RENDER_DYNAMIC_LAYER_ALPHA_V1;
            extended.reserved = 0;
        }
        if (capacity >= sizeof(War3RenderSkinMaterialStateCapabilitiesV2)) {
            auto& extended = *reinterpret_cast<War3RenderSkinMaterialStateCapabilitiesV2*>(output);
            extended.materialStateFormat = WAR3_RENDER_DYNAMIC_MATERIAL_STATE_V1; extended.reserved = 0;
        }
        if (capacity >= sizeof(War3RenderParticleMaterialCapabilitiesV2)) {
            auto& extended = *reinterpret_cast<War3RenderParticleMaterialCapabilitiesV2*>(output);
            extended.particleMaterialFlagsFormat = WAR3_RENDER_PARTICLE_MATERIAL_FLAGS_V1;
            extended.particleMaterialFlagsMask = WAR3_PARTICLE_UNFOGGED;
        }
        return 0;
    } catch (...) { return static_cast<std::int32_t>(E_FAIL); }
}
std::int32_t SubmitParticleModelsImpl(War3RendererHandle handle, const War3RenderParticleSceneV2* input,
    const War3RenderEditorVisibilityUpdateV2* visibility) noexcept {
    if (!handle || !input) return kInvalid;
    try {
        ModelSceneLayerData copied;
        if (!CopyParticleScene(*input, copied)) return kInvalid;
        return PublishModels(*static_cast<Renderer*>(handle), std::move(copied.models), std::move(copied.pose),
            std::move(copied.particles), std::move(copied.particleFrame), visibility);
    } catch (...) { return static_cast<std::int32_t>(E_OUTOFMEMORY); }
}
std::int32_t WAR3_RENDER_CALL SubmitParticleModels(War3RendererHandle handle, const War3RenderParticleSceneV2* input) noexcept {
    return SubmitParticleModelsImpl(handle, input, nullptr);
}
// 附加层和主槽共用所有模型/矩阵/粒子校验；只改变发布目标，不改变资产语义。
std::int32_t WAR3_RENDER_CALL SubmitModelSceneLayer(War3RendererHandle handle,
    const War3RenderModelSceneLayerV2* input) noexcept {
    if (!handle || !input || (input->structSize != WAR3_RENDER_MODEL_SCENE_LAYER_PREFIX_SIZE && input->structSize != sizeof(*input))
        || input->reserved || !input->layerId
        || !input->parentModelEpoch || !input->revision || input->revision <= input->expectedRevision
        || (static_cast<unsigned>(input->models != nullptr) + static_cast<unsigned>(input->particles != nullptr)) != 1) return kInvalid;
    const auto role = input->structSize == WAR3_RENDER_MODEL_SCENE_LAYER_PREFIX_SIZE
        ? WAR3_RENDER_MODEL_LAYER_MODEL : input->role;
    if (input->structSize != WAR3_RENDER_MODEL_SCENE_LAYER_PREFIX_SIZE && input->roleReserved) return kInvalid;
    if (role != WAR3_RENDER_MODEL_LAYER_MODEL && role != WAR3_RENDER_MODEL_LAYER_GROUND_DECAL_PREVIEW) return kInvalid;
    if (role == WAR3_RENDER_MODEL_LAYER_GROUND_DECAL_PREVIEW && input->particles) return kInvalid;
    try {
        auto layer = std::make_shared<ModelSceneLayerData>();
        layer->id = input->layerId; layer->revision = input->revision; layer->parentModelEpoch = input->parentModelEpoch;
        layer->role = role;
        if (input->particles) { if (!CopyParticleScene(*input->particles, *layer)) return kInvalid; }
        else {
            auto models = std::make_shared<ModelScene>();
            if (!CopySkinnedScene(*input->models, *models)) return kInvalid;
            layer->pose = models->initialSkin; layer->models = std::move(models);
        }
        // 角色由调用方明确声明，但不能用它将不透明/写深度材质偷偷变成贴画。
        if (role == WAR3_RENDER_MODEL_LAYER_GROUND_DECAL_PREVIEW
            && std::any_of(layer->models->layers.begin(), layer->models->layers.end(),
                [](const auto& value) { return value.filterMode < 2 || !(value.flags & 4u); })) return kInvalid;
        if (!ValidateUnmaskedMaterialWork(*layer->models, layer->pose.get(), layer->particleFrame.get())) return kInvalid;
        auto& renderer = *static_cast<Renderer*>(handle);
        {
            std::lock_guard lock(renderer.mutex);
            if (renderer.state.modelEpoch != input->parentModelEpoch) return kInvalid;
            auto layers = std::make_shared<ModelSceneLayers>(*renderer.state.modelLayers);
            const auto found = std::find_if(layers->begin(), layers->end(), [&](const auto& value) { return value->id == layer->id; });
            if (found == layers->end()) {
                if (input->expectedRevision || layers->size() >= WAR3_RENDER_MAX_MODEL_SCENE_LAYERS) return kInvalid;
                layers->push_back(std::move(layer));
            } else {
                if ((*found)->revision != input->expectedRevision || (*found)->role != role) return kInvalid;
                *found = std::move(layer);
            }
            renderer.state.modelLayers = std::move(layers);
            ++renderer.state.layersRevision; ++renderer.state.sceneEpoch; ++renderer.state.request;
        }
        renderer.changed.notify_all(); return 0;
    } catch (...) { return static_cast<std::int32_t>(E_OUTOFMEMORY); }
}
std::int32_t WAR3_RENDER_CALL UpdateModelSceneLayer(War3RendererHandle handle,
    const War3RenderModelLayerUpdateV2* input) noexcept {
    if (!handle || !input || input->structSize != sizeof(*input) || input->reserved || !input->layerId
        || !input->expectedRevision || input->revision <= input->expectedRevision
        || (static_cast<unsigned>(input->pose != nullptr) + static_cast<unsigned>(input->particles != nullptr)) != 1) return kInvalid;
    try {
        auto& renderer = *static_cast<Renderer*>(handle);
        std::shared_ptr<const ModelSceneLayerData> previous;
        {
            std::lock_guard lock(renderer.mutex);
            if (renderer.state.modelEpoch != input->parentModelEpoch) return kInvalid;
            for (const auto& value : *renderer.state.modelLayers) if (value->id == input->layerId) previous = value;
        }
        if (!previous || previous->revision != input->expectedRevision || (!!previous->particles != !!input->particles)) return kInvalid;
        auto layer = std::make_shared<ModelSceneLayerData>(*previous);
        auto pose = std::make_shared<SkinPose>();
        if (input->particles) {
            auto frame = std::make_shared<ParticleFrameData>();
            if (!War3Map::Render::CopyParticleInput(*input->particles, previous->particles->TextureCount(), *frame)) return kInvalid;
            layer->particleFrame = frame;
        }
        if (!War3Map::Render::CopySkinUpdate(input->particles ? *input->particles->modelPose : *input->pose, *previous->models, *pose)
            || pose->contentVersion <= previous->pose->contentVersion) return kInvalid;
        if (!ValidateUnmaskedMaterialWork(*previous->models, pose.get(), layer->particleFrame.get())) return kInvalid;
        layer->pose = std::move(pose); layer->revision = input->revision;
        {
            std::lock_guard lock(renderer.mutex);
            if (renderer.state.modelEpoch != input->parentModelEpoch) return kInvalid;
            auto layers = std::make_shared<ModelSceneLayers>(*renderer.state.modelLayers);
            const auto found = std::find(layers->begin(), layers->end(), previous);
            if (found == layers->end()) return kInvalid;
            *found = std::move(layer); renderer.state.modelLayers = std::move(layers);
            ++renderer.state.layersRevision; ++renderer.state.sceneEpoch; ++renderer.state.request;
        }
        renderer.changed.notify_all(); return 0;
    } catch (...) { return static_cast<std::int32_t>(E_OUTOFMEMORY); }
}
std::int32_t WAR3_RENDER_CALL RemoveModelSceneLayer(War3RendererHandle handle, std::uint64_t id,
    std::uint64_t parentEpoch, std::uint64_t expectedRevision) noexcept {
    if (!handle || !id || !parentEpoch || !expectedRevision) return kInvalid;
    try {
        auto& renderer = *static_cast<Renderer*>(handle);
        {
            std::lock_guard lock(renderer.mutex);
            if (renderer.state.modelEpoch != parentEpoch) return kInvalid;
            auto layers = std::make_shared<ModelSceneLayers>(*renderer.state.modelLayers);
            const auto found = std::find_if(layers->begin(), layers->end(), [=](const auto& value) { return value->id == id; });
            if (found == layers->end() || (*found)->revision != expectedRevision) return kInvalid;
            layers->erase(found); renderer.state.modelLayers = std::move(layers);
            ++renderer.state.layersRevision; ++renderer.state.sceneEpoch; ++renderer.state.request;
        }
        renderer.changed.notify_all(); return 0;
    } catch (...) { return static_cast<std::int32_t>(E_OUTOFMEMORY); }
}
std::int32_t WAR3_RENDER_CALL GetModelLayersTarget(War3RendererHandle handle, War3RenderModelLayersTargetV2* output) noexcept {
    if (!handle || !output || output->structSize < sizeof(*output)) return kInvalid;
    try {
        auto& renderer = *static_cast<Renderer*>(handle); std::lock_guard lock(renderer.mutex);
        *output = {sizeof(*output), static_cast<std::uint32_t>(renderer.state.modelLayers->size()),
            renderer.state.modelEpoch, renderer.state.layersRevision}; return 0;
    } catch (...) { return static_cast<std::int32_t>(E_FAIL); }
}
std::int32_t WAR3_RENDER_CALL GetModelLayerCapabilities(War3RendererHandle handle, std::uint32_t* output) noexcept {
    if (!handle || !output) return kInvalid;
    *output = WAR3_RENDER_MODEL_LAYER_CAP_GROUND_DECAL_PREVIEW;
    return 0;
}
std::int32_t WAR3_RENDER_CALL SubmitEditorModelScene(War3RendererHandle handle,
    const War3RenderEditorModelSceneV2* input) noexcept {
    if (!input || input->structSize != sizeof(*input) || input->reserved || !input->visibility
        || (static_cast<unsigned>(input->staticModels != nullptr) + static_cast<unsigned>(input->models != nullptr)
            + static_cast<unsigned>(input->particles != nullptr)) != 1) return kInvalid;
    if (input->staticModels) return SubmitModelsImpl(handle, input->staticModels, input->visibility);
    if (input->models) return SubmitSkinnedModelsImpl(handle, input->models, input->visibility);
    return SubmitParticleModelsImpl(handle, input->particles, input->visibility);
}
std::int32_t WAR3_RENDER_CALL UpdateParticleFrame(War3RendererHandle handle, const War3RenderParticleUpdateV2* input) noexcept {
    if (!handle || !input) return kInvalid;
    try {
        auto& renderer = *static_cast<Renderer*>(handle);
        FrameState frozen;
        { std::lock_guard lock(renderer.mutex); frozen = renderer.state; }
        if (!frozen.particles) return kInvalid;
        auto frame = std::make_shared<ParticleFrameData>();
        if (!War3Map::Render::CopyParticleInput(*input, frozen.particles->TextureCount(), *frame)) return kInvalid;
        auto pose = std::make_shared<SkinPose>();
        if (!War3Map::Render::CopySkinUpdate(*input->modelPose, SkinLayout(frozen), *pose)) return kInvalid;
        if (!ValidateUnmaskedMaterialWork(SkinLayout(frozen), pose.get(), frame.get())) return kInvalid;
        {
            std::lock_guard lock(renderer.mutex);
            if (renderer.state.models != frozen.models || renderer.state.particles != frozen.particles
                || renderer.state.skinLayout != frozen.skinLayout || renderer.state.skinPose != frozen.skinPose
                || renderer.state.particleFrame != frozen.particleFrame || !renderer.state.skinPose
                || pose->contentVersion <= renderer.state.skinPose->contentVersion) return kInvalid;
            renderer.state.skinPose = std::move(pose); renderer.state.particleFrame = std::move(frame);
            renderer.state.visibility.reset();
            ++renderer.state.request;
        }
        renderer.changed.notify_all(); return 0;
    } catch (...) { return static_cast<std::int32_t>(E_OUTOFMEMORY); }
}
std::int32_t WAR3_RENDER_CALL GetEditorPresentationTarget(War3RendererHandle handle,
    War3RenderEditorPresentationTargetV2* output) noexcept {
    if (!handle || !output || output->structSize < sizeof(*output)) return kInvalid;
    try {
        auto& renderer = *static_cast<Renderer*>(handle);
        std::lock_guard lock(renderer.mutex);
        *output = PresentationTarget(renderer.state);
        return 0;
    } catch (...) { return static_cast<std::int32_t>(E_FAIL); }
}
std::int32_t WAR3_RENDER_CALL SubmitEditorVisibility(War3RendererHandle handle,
    const War3RenderEditorVisibilityUpdateV2* input) noexcept {
    if (!handle || !input || input->structSize != sizeof(*input) || input->reserved
        || input->expected.structSize < sizeof(input->expected) || !input->presentationRevision
        || (static_cast<unsigned>(input->modelPose != nullptr) + static_cast<unsigned>(input->particleFrame != nullptr)
            + static_cast<unsigned>(input->staticInstances != nullptr)) > 1) return kInvalid;
    try {
        auto& renderer = *static_cast<Renderer*>(handle);
        FrameState frozen;
        {
            std::lock_guard lock(renderer.mutex);
            if (!SamePresentationTarget(input->expected, PresentationTarget(renderer.state))
                || input->presentationRevision <= renderer.state.lastPresentationRevision) return kInvalid;
            frozen = renderer.state;
        }
        std::shared_ptr<const SkinPose> pose = frozen.skinPose;
        std::shared_ptr<const ParticleFrameData> particles = frozen.particleFrame;
        std::shared_ptr<const std::vector<War3RenderModelInstanceV2>> instances = frozen.instances;
        if (input->staticInstances) {
            if (frozen.skinPose || input->instanceCount != frozen.models->instances.size()
                || frozen.modelEpoch == std::numeric_limits<std::uint64_t>::max()) return kInvalid;
            auto nextInstances = std::make_shared<std::vector<War3RenderModelInstanceV2>>();
            if (input->instanceCount) nextInstances->assign(input->staticInstances, input->staticInstances + input->instanceCount);
            for (std::size_t i = 0; i < nextInstances->size(); ++i) {
                if ((*nextInstances)[i].model != frozen.models->instances[i].model) return kInvalid;
                for (const auto value : (*nextInstances)[i].transform) if (!Coordinate(value)) return kInvalid;
                for (const auto value : (*nextInstances)[i].color) if (!Unit(value)) return kInvalid;
            }
            instances = std::move(nextInstances);
        }
        if (input->particleFrame) {
            if (!frozen.particles || !frozen.skinPose) return kInvalid;
            auto nextParticles = std::make_shared<ParticleFrameData>();
            if (!War3Map::Render::CopyParticleInput(*input->particleFrame,
                frozen.particles->TextureCount(), *nextParticles)) return kInvalid;
            auto nextPose = std::make_shared<SkinPose>();
            if (!War3Map::Render::CopySkinUpdate(*input->particleFrame->modelPose, SkinLayout(frozen), *nextPose)) return kInvalid;
            pose = std::move(nextPose); particles = std::move(nextParticles);
        } else if (input->modelPose) {
            if (!frozen.skinPose || frozen.particles) return kInvalid;
            auto nextPose = std::make_shared<SkinPose>();
            if (!War3Map::Render::CopySkinUpdate(*input->modelPose, SkinLayout(frozen), *nextPose)) return kInvalid;
            pose = std::move(nextPose);
        }
        if ((input->modelPose || input->particleFrame) && pose->contentVersion <= frozen.skinPose->contentVersion) return kInvalid;
        auto target = PresentationTarget(frozen);
        if (input->staticInstances) ++target.modelEpoch;
        if (pose) { target.sceneVersion = pose->sceneVersion; target.contentVersion = pose->contentVersion; }
        target.particleDrawCount = particles ? static_cast<std::uint32_t>(particles->draws.size()) : 0;
        if (particles && (!pose || PosePayloadBytes(*pose) + particles->PayloadBytes()
            + input->instanceCount + input->particleDrawCount > 128ull * 1024 * 1024)) return kInvalid;
        auto visibility = std::make_shared<EditorVisibility>();
        if (!War3Map::Render::CopyEditorVisibility(SkinLayout(frozen), particles.get(), target, *input, *visibility, pose.get())) return kInvalid;
        {
            std::lock_guard lock(renderer.mutex);
            // CPU预检期间任何模型、姿势、静态实例覆盖或粒子槽变化均拒绝迟到请求。
            if (!SamePresentationTarget(input->expected, PresentationTarget(renderer.state))
                || renderer.state.models != frozen.models || renderer.state.skinPose != frozen.skinPose
                || renderer.state.particles != frozen.particles || renderer.state.particleFrame != frozen.particleFrame
                || renderer.state.instances != frozen.instances
                || input->presentationRevision <= renderer.state.lastPresentationRevision) return kInvalid;
            renderer.state.skinPose = std::move(pose); renderer.state.particleFrame = std::move(particles);
            renderer.state.instances = std::move(instances); renderer.state.modelEpoch = target.modelEpoch;
            renderer.state.visibility = std::move(visibility);
            renderer.state.lastPresentationRevision = input->presentationRevision;
            ++renderer.state.request;
        }
        renderer.changed.notify_all();
        return 0;
    } catch (...) { return static_cast<std::int32_t>(E_OUTOFMEMORY); }
}
std::int32_t WAR3_RENDER_CALL SubmitEditorInstanceRoster(War3RendererHandle handle,
    const War3RenderEditorInstanceRosterV2* input, std::uint64_t* outputTicket) noexcept {
    if (!handle || !input || !outputTicket
        || (input->structSize != sizeof(*input) && input->structSize != sizeof(War3RenderEditorAssetRosterV2))
        || (input->flags != 0 && input->flags != WAR3_RENDER_ROSTER_APPEND_ASSETS)
        || (input->flags == WAR3_RENDER_ROSTER_APPEND_ASSETS) != (input->structSize == sizeof(War3RenderEditorAssetRosterV2))
        || input->reserved[0] || input->reserved[1] || !input->candidatePose || !input->visibility
        || input->expected.structSize < sizeof(input->expected) || !input->expectedAssetIdentity) return kInvalid;
    const auto* assets = input->flags ? reinterpret_cast<const War3RenderEditorAssetRosterV2*>(input)->assets : nullptr;
    if (input->flags && (!assets || assets->structSize != sizeof(*assets) || assets->reserved)) return kInvalid;
    try {
        auto& renderer = *static_cast<Renderer*>(handle);
        FrameState frozen;
        { std::lock_guard lock(renderer.mutex); frozen = renderer.state; }
        const auto& mask = *input->visibility;
        if (!frozen.skinPose || renderer.closing || frozen.assetIdentity != input->expectedAssetIdentity
            || !SamePresentationTarget(PresentationTarget(frozen), input->expected)
            || PresentationRevision(frozen) != input->expectedPresentationRevision
            || frozen.layersRevision != input->expectedLayersRevision
            || frozen.modelEpoch == std::numeric_limits<std::uint64_t>::max()
            || frozen.layersRevision == std::numeric_limits<std::uint64_t>::max()
            || (assets && frozen.assetIdentity == std::numeric_limits<std::uint64_t>::max())
            || input->candidatePose->sceneVersion <= frozen.lastSkinSceneVersion
            || mask.structSize != sizeof(mask) || mask.reserved
            || mask.expected.structSize < sizeof(mask.expected) || !SamePresentationTarget(mask.expected, input->expected)
            || mask.modelPose || mask.particleFrame || mask.staticInstances
            || mask.presentationRevision <= frozen.lastPresentationRevision
            || (!assets && (!!input->candidateParticles != !!frozen.particles))
            || (assets && frozen.particles && !input->candidateParticles)
            || (assets && assets->particleTextureCount && !input->candidateParticles)) return kInvalid;
        const auto& poseInput = *input->candidatePose;
        std::uint64_t poseBytes = std::uint64_t(poseInput.instanceCount) * sizeof(War3RenderSkinnedModelInstanceV2)
            + std::uint64_t(poseInput.groupCount) * sizeof(War3RenderGroupMatrixV2)
            + std::uint64_t(poseInput.alphaCount) * (sizeof(float) + sizeof(War3RenderMeshColorV2));
        const War3RenderMaterialSkinUpdateV2* material = nullptr;
        if (poseInput.structSize == sizeof(War3RenderMaterialStateSkinUpdateV2)) {
            const auto& state = *reinterpret_cast<const War3RenderMaterialStateSkinUpdateV2*>(&poseInput);
            material = &state.base;
            poseBytes += std::uint64_t(state.materialStateCount) * sizeof(War3RenderMaterialStateV2)
                + std::uint64_t(state.materialInstanceSliceCount) * sizeof(War3RenderMaterialLayerSliceV2);
        } else if (poseInput.structSize == sizeof(War3RenderMaterialSkinUpdateV2)) material = reinterpret_cast<const War3RenderMaterialSkinUpdateV2*>(&poseInput);
        if (material) poseBytes += std::uint64_t(material->layerAlphaCount) * sizeof(float)
            + std::uint64_t(material->instanceSliceCount) * sizeof(War3RenderMaterialLayerSliceV2);
        const auto particleBytes = input->candidateParticles
            ? War3Map::Render::ParticleInputPayloadBytes(*input->candidateParticles) : std::optional<std::uint64_t>(0);
        if (!particleBytes) return kInvalid;
        if (poseBytes + *particleBytes + mask.instanceCount + mask.particleDrawCount > 128ull * 1024 * 1024) return kInvalid;
        auto roster = std::make_shared<RosterCandidate>();
        roster->assetIdentity = frozen.assetIdentity; roster->expected = input->expected;
        roster->candidateAssetIdentity = assets ? frozen.assetIdentity + 1 : frozen.assetIdentity;
        roster->expectedPresentationRevision = input->expectedPresentationRevision;
        roster->expectedLayersRevision = input->expectedLayersRevision;
        roster->baseModels = frozen.models; roster->baseLayout = frozen.skinLayout; roster->basePose = frozen.skinPose;
        roster->baseParticles = frozen.particles; roster->baseParticleFrame = frozen.particleFrame;
        roster->baseVisibility = frozen.visibility; roster->layers = std::make_shared<ModelSceneLayers>();
        auto layout = std::make_shared<ModelScene>();
        if (assets) {
            if (assets->prefix.particleTextureCount != (frozen.particles ? frozen.particles->TextureCount() : 0)) return kInvalid;
            std::uint64_t payload = 0;
            if (!War3Map::Render::CopySkinAssetAppend(*assets, poseInput, frozen.models, *layout, payload)) return kInvalid;
            const auto modelBytes = ResidentTextureBytes(*frozen.models);
            const auto particleTextureBytes = frozen.particles ? ResidentTextureBytes(*frozen.particles) : 0;
            if (modelBytes > kMaxModelTextureTotal || particleTextureBytes > kMaxTextureTotal) return kInvalid;
            TextureChains modelChains, particleChains;
            if (!ValidateTextureChains(assets->modelTextures, assets->modelTextureCount,
                    {kMaxTextureBytes, kMaxMipChainBytes, kMaxModelTextureTotal - modelBytes}, modelChains)
                || !ValidateTextureChains(assets->particleTextures, assets->particleTextureCount,
                    {kMaxTextureBytes, kMaxMipChainBytes, kMaxTextureTotal - particleTextureBytes}, particleChains)
                || payload + modelBytes + modelChains.pixelBytes + modelChains.descriptorBytes > kMaxModelBytes) return kInvalid;
            for (const auto& texture : modelChains.descriptions) if (texture.atlasColumns != 1 || texture.atlasRows != 1) return kInvalid;
            for (const auto& texture : particleChains.descriptions) if (texture.atlasColumns != 1 || texture.atlasRows != 1) return kInvalid;
            layout->textures = CopyTextureChains(modelChains);
            layout->SealStaticSuffix();
            roster->models = layout;
            if (input->candidateParticles) {
                auto library = std::make_shared<ParticleSceneData>();
                // 空suffix不增加拥有链深度；最多只保留真正上传过的粒子纹理页。
                library->staticBase = frozen.particles && frozen.particles->textures.empty()
                    ? frozen.particles->staticBase : frozen.particles;
                library->prefixTextures = assets->prefix.particleTextureCount;
                library->textures = CopyTextureChains(particleChains); roster->particleAssets = std::move(library);
            }
        } else if (!War3Map::Render::CopySkinRoster(poseInput, *frozen.models, *layout)) return kInvalid;
        roster->pose = layout->initialSkin; roster->layout = std::move(layout);
        if (input->candidateParticles) {
            if (input->candidateParticles->modelPose != input->candidatePose) return kInvalid;
            auto particles = std::make_shared<ParticleFrameData>();
            if (!War3Map::Render::CopyParticleInput(*input->candidateParticles,
                assets ? roster->particleAssets->TextureCount() : frozen.particles->TextureCount(), *particles)) return kInvalid;
            roster->particleFrame = std::move(particles);
        }
        auto target = input->expected;
        target.mode = roster->particleFrame ? WAR3_EDITOR_PRESENTATION_PARTICLES : WAR3_EDITOR_PRESENTATION_SKINNED;
        ++target.modelEpoch; target.sceneVersion = roster->pose->sceneVersion; target.contentVersion = roster->pose->contentVersion;
        target.instanceCount = static_cast<std::uint32_t>(roster->pose->instances.size());
        target.particleDrawCount = roster->particleFrame ? static_cast<std::uint32_t>(roster->particleFrame->draws.size()) : 0;
        auto visibility = std::make_shared<EditorVisibility>();
        if (!War3Map::Render::CopyEditorVisibility(*roster->layout, roster->particleFrame.get(), target, mask, *visibility)) return kInvalid;
        roster->visibility = std::move(visibility);
        {
            std::lock_guard lock(renderer.mutex);
            if (!RosterBaseMatches(renderer.state, *roster) || renderer.closing
                || mask.presentationRevision <= renderer.state.lastPresentationRevision
                || renderer.nextRosterTicket == std::numeric_limits<std::uint64_t>::max()) return kInvalid;
            if (renderer.state.roster) {
                auto& previous = renderer.rosterResults[(renderer.state.roster->ticket - 1) % renderer.rosterResults.size()];
                if (previous.ticket == renderer.state.roster->ticket && previous.state == WAR3_RENDER_ROSTER_QUEUED) {
                    previous.state = WAR3_RENDER_ROSTER_SUPERSEDED; previous.result = kTimeout;
                }
            }
            roster->ticket = ++renderer.nextRosterTicket;
            auto& ticket = renderer.rosterResults[(roster->ticket - 1) % renderer.rosterResults.size()];
            ticket = {}; ticket.structSize = sizeof(ticket); ticket.state = WAR3_RENDER_ROSTER_QUEUED;
            ticket.result = kTimeout; ticket.ticket = roster->ticket; ticket.assetIdentity = roster->candidateAssetIdentity;
            ticket.layersRevision = frozen.layersRevision + 1; ticket.presentationRevision = mask.presentationRevision;
            ticket.target = target;
            renderer.state.roster = std::move(roster); ++renderer.state.request;
            *outputTicket = ticket.ticket;
        }
        renderer.changed.notify_all(); return 0;
    } catch (...) { return static_cast<std::int32_t>(E_OUTOFMEMORY); }
}
std::int32_t WAR3_RENDER_CALL GetEditorRosterCapabilities(War3RendererHandle handle, std::uint32_t* flags) noexcept {
    if (!handle || !flags) return kInvalid;
    *flags = WAR3_RENDER_ROSTER_APPEND_ASSETS; return 0;
}
std::int32_t WAR3_RENDER_CALL GetEditorInstanceRosterResult(War3RendererHandle handle,
    std::uint64_t ticket, War3RenderInstanceRosterResultV2* output) noexcept {
    if (!handle || !output || output->structSize < sizeof(*output)) return kInvalid;
    try {
        auto& renderer = *static_cast<Renderer*>(handle); std::lock_guard lock(renderer.mutex);
        if (!ticket) {
            War3RenderInstanceRosterResultV2 value{}; value.structSize = sizeof(value);
            value.state = WAR3_RENDER_ROSTER_TARGET; value.assetIdentity = renderer.state.assetIdentity;
            value.target = PresentationTarget(renderer.state);
            value.layersRevision = renderer.state.layersRevision; value.presentationRevision = PresentationRevision(renderer.state);
            *output = value; return 0;
        }
        const auto& value = renderer.rosterResults[(ticket - 1) % renderer.rosterResults.size()];
        if (value.ticket != ticket || !value.structSize) return kInvalid;
        *output = value; return 0;
    } catch (...) { return static_cast<std::int32_t>(E_FAIL); }
}
std::int32_t WAR3_RENDER_CALL CancelEditorInstanceRoster(War3RendererHandle handle,
    std::uint64_t ticket, War3RenderInstanceRosterResultV2* output) noexcept {
    if (!handle || !ticket || !output || output->structSize < sizeof(*output)) return kInvalid;
    try {
        auto& renderer = *static_cast<Renderer*>(handle); std::lock_guard lock(renderer.mutex);
        auto& value = renderer.rosterResults[(ticket - 1) % renderer.rosterResults.size()];
        if (value.ticket != ticket || !value.structSize) return kInvalid;
        if (value.state == WAR3_RENDER_ROSTER_QUEUED && renderer.state.roster
            && renderer.state.roster->ticket == ticket) {
            value.state = WAR3_RENDER_ROSTER_SUPERSEDED; value.result = kTimeout;
            renderer.state.roster.reset(); ++renderer.state.request;
            renderer.changed.notify_all();
        }
        // 已Publish但尚未Draw/Present的票据仍为QUEUED；不能把它假记成撤销或回滚。
        // 已COMPLETED/REJECTED/SUPERSEDED只返回真实终态，不改GPU/完成帧。
        *output = value; return 0;
    } catch (...) { return static_cast<std::int32_t>(E_FAIL); }
}
bool Finite(const float* values, std::size_t count, float minimum, float maximum) {
    return std::all_of(values, values + count, [&](float value) { return std::isfinite(value) && value >= minimum && value <= maximum; });
}
std::int32_t WAR3_RENDER_CALL SetEnvironment(War3RendererHandle renderer, const War3RenderEnvironmentV2* input) noexcept {
    if (!renderer || !input || input->structSize != sizeof(*input)) return kInvalid;
    const auto& e = *input;
    const auto lengthSquared = e.towardLight[0] * e.towardLight[0] + e.towardLight[1] * e.towardLight[1]
        + e.towardLight[2] * e.towardLight[2];
    if (!Finite(e.towardLight, 3, -1.0e6f, 1.0e6f) || !(lengthSquared > 1.0e-12f) || !Finite(e.lightColor, 3, 0, 16)
        || !Finite(e.ambientColor, 3, 0, 16) || e.fogMode > WAR3_RENDER_FOG_EXP2 || !Finite(e.fogColor, 3, 0, 1)) return kInvalid;
    if (e.fogMode == WAR3_RENDER_FOG_LINEAR && !(Coordinate(e.fogStart) && Coordinate(e.fogEnd) && e.fogStart < e.fogEnd)) return kInvalid;
    if (e.fogMode >= WAR3_RENDER_FOG_EXP && !(std::isfinite(e.fogDensity) && e.fogDensity >= 0 && e.fogDensity <= 1)) return kInvalid;
    if (e.shadowWidth != 0) {
        if (e.shadowWidth > 8192 || e.shadowHeight == 0 || e.shadowHeight > 8192 || !e.shadow || !Coordinate(e.shadowOriginX)
            || !Coordinate(e.shadowOriginY) || !(std::isfinite(e.shadowCellSize) && e.shadowCellSize > 0 && e.shadowCellSize <= 1.0e5f)
            || !Unit(e.shadowDarkness)) return kInvalid;
    } else if (e.shadowHeight != 0) return kInvalid;
    try {
        auto environment = std::make_shared<Environment>();
        environment->values = e;
        environment->values.shadow = nullptr;
        if (e.shadowWidth != 0) environment->shadow.assign(e.shadow, e.shadow + static_cast<std::size_t>(e.shadowWidth) * e.shadowHeight);
        std::shared_ptr<const Environment> published = std::move(environment);
        return Update(renderer, [&published](auto& state) { state.environment = std::move(published); });
    } catch (...) { return static_cast<std::int32_t>(E_OUTOFMEMORY); }
}
std::int32_t WAR3_RENDER_CALL UpdateModelInstances(War3RendererHandle handle, const War3RenderModelInstanceV2* input,
    std::uint32_t count) noexcept {
    if (!handle || (count && !input)
        || static_cast<std::uint64_t>(count) * sizeof(War3RenderModelInstanceV2) > kMaxModelBytes) return kInvalid;
    try {
        auto copy = std::make_shared<std::vector<War3RenderModelInstanceV2>>();
        if (count) copy->assign(input, input + count);
        for (const auto& instance : *copy) {
            for (const auto value : instance.transform) if (!Coordinate(value)) return kInvalid;
            for (const auto value : instance.color) if (!Unit(value)) return kInvalid;
        }
        auto& renderer = *static_cast<Renderer*>(handle);
        {
            std::lock_guard lock(renderer.mutex);
            if (renderer.state.models->initialSkin) return kInvalid; // 蒙皮必须走带内容版本的入口
            if (renderer.state.modelEpoch == std::numeric_limits<std::uint64_t>::max()) return kInvalid;
            const auto& current = renderer.state.models->instances;
            if (current.size() != copy->size()) return kInvalid;
            for (std::size_t i = 0; i < copy->size(); ++i) if ((*copy)[i].model != current[i].model) return kInvalid;
            renderer.state.instances = std::move(copy);
            renderer.state.visibility.reset(); ++renderer.state.modelEpoch;
            ++renderer.state.request;
        }
        renderer.changed.notify_all();
        return 0;
    } catch (...) { return static_cast<std::int32_t>(E_OUTOFMEMORY); }
}
std::int32_t WAR3_RENDER_CALL SetOverlayLines(War3RendererHandle renderer, const War3RenderOverlayLineV2* input,
    std::uint32_t count) noexcept {
    constexpr std::uint32_t kMaxOverlayLines = 262144;
    if (!renderer || (count && !input) || count > kMaxOverlayLines) return kInvalid;
    try {
        auto vertices = std::make_shared<std::vector<War3RenderVertexV2>>();
        vertices->reserve(static_cast<std::size_t>(count) * 2);
        for (std::uint32_t i = 0; i < count; ++i) {
            const auto& line = input[i];
            for (int axis = 0; axis < 3; ++axis)
                if (!Coordinate(line.from[axis]) || !Coordinate(line.to[axis]) || !Unit(line.color[axis])) return kInvalid;
            vertices->push_back({line.from[0], line.from[1], line.from[2], line.color[0], line.color[1], line.color[2]});
            vertices->push_back({line.to[0], line.to[1], line.to[2], line.color[0], line.color[1], line.color[2]});
        }
        std::shared_ptr<const std::vector<War3RenderVertexV2>> published = std::move(vertices);
        return Update(renderer, [&published](auto& state) { state.overlay = std::move(published); });
    } catch (...) { return static_cast<std::int32_t>(E_OUTOFMEMORY); }
}
std::int32_t WAR3_RENDER_CALL SubmitTerrain(War3RendererHandle renderer, const War3RenderTerrainSceneV2* input) noexcept {
    if (!input || input->structSize != sizeof(*input) || input->textureCount > kMaxTerrainTextures
        || (input->textureCount && !input->textures)) return kInvalid;
    // 旧 ABI 仅包装为一层、一个 cell，复用全部预检和原子发布，不改变其布局或 mip0 行为。
    std::array<War3RenderMipRgbaV2, kMaxTerrainTextures> mips{};
    std::array<War3RenderTextureMipChainV2, kMaxTerrainTextures> textures{};
    for (std::uint32_t i = 0; i < input->textureCount; ++i) {
        const auto old = input->textures[i];
        if (old.structSize != sizeof(old)) return kInvalid;
        mips[i] = {sizeof(War3RenderMipRgbaV2), old.width, old.height, old.rgbaByteCount, old.rgba};
        textures[i] = {sizeof(War3RenderTextureMipChainV2), 1, 0, 1, 1, &mips[i]};
    }
    const War3RenderTerrainMipSceneV2 scene{sizeof(War3RenderTerrainMipSceneV2), input->vertexCount,
        input->indexCount, input->textureCount, input->batchCount, input->documentRevision,
        input->vertices, input->indices, textures.data(), input->batches};
    return SubmitTerrainMips(renderer, &scene);
}
void WAR3_RENDER_CALL Frame(War3RendererHandle renderer) noexcept { (void)Update(renderer, [](auto&) {}); }
std::int32_t WAR3_RENDER_CALL Status(War3RendererHandle handle, War3RenderStatusV2* output) noexcept {
    if (!handle || !output || output->structSize != sizeof(*output)) return kInvalid;
    try { auto& renderer = *static_cast<Renderer*>(handle); std::lock_guard lock(renderer.mutex); *output = renderer.status; return 0; }
    catch (...) { return static_cast<std::int32_t>(E_FAIL); }
}
std::int32_t CapturePixels(War3RendererHandle handle, void* data, std::uint64_t size,
    War3RenderCaptureInfoV2* legacyReceipt, War3RenderCapturedFrameV2* frameReceipt,
    War3RenderSkinnedCapturedFrameV2* skinReceipt = nullptr, War3RenderParticleCapturedFrameV2* particleReceipt = nullptr,
    War3RenderEditorCapturedFrameV2* presentationReceipt = nullptr, std::uint64_t expectedModelEpoch = 0,
    std::uint64_t expectedPresentationRevision = 0, War3RenderLayeredCapturedFrameV2* layerReceipt = nullptr,
    std::uint64_t expectedLayersRevision = 0) noexcept {
    try {
        auto& renderer = *static_cast<Renderer*>(handle);
        std::unique_lock lock(renderer.mutex);
        if (renderer.status.result != 0) return renderer.status.result;
        const auto wantedLayers = renderer.state.modelLayers;
        if (layerReceipt && renderer.state.layersRevision != expectedLayersRevision) return kTimeout;
        const auto wantedVisibility = presentationReceipt ? renderer.state.visibility : nullptr;
        if (presentationReceipt && (!wantedVisibility || wantedVisibility->status.modelEpoch != expectedModelEpoch
            || wantedVisibility->status.presentationRevision != expectedPresentationRevision)) return kTimeout;
        const bool needsSkin = skinReceipt || particleReceipt || (wantedVisibility && wantedVisibility->status.mode != WAR3_EDITOR_PRESENTATION_STATIC);
        const bool needsParticles = particleReceipt || (wantedVisibility && wantedVisibility->status.mode == WAR3_EDITOR_PRESENTATION_PARTICLES);
        if (renderer.captureActive || (needsSkin && !renderer.state.skinPose)
            || (needsParticles && !renderer.state.particles)) return kInvalid;
        if (size < static_cast<std::uint64_t>(renderer.state.width) * renderer.state.height * 4) return kInvalid;
        renderer.captureActive = true;
        struct ResetCapture { Renderer& renderer; ~ResetCapture() { renderer.captureActive = false; } } reset{renderer};
        const auto ticket = ++renderer.state.capture;
        ++renderer.state.request;
        renderer.changed.notify_all();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        const HANDLE worker = static_cast<HANDLE>(renderer.thread.native_handle());
        while (renderer.captureDone < ticket && !renderer.closing && renderer.status.result == 0) {
            if (std::chrono::steady_clock::now() >= deadline) return kTimeout;
            lock.unlock();
            // 显式截图也可能由 HWND 所属线程调用；不可持锁等待同步窗口消息。
            const DWORD waited = MsgWaitForMultipleObjectsEx(1, &worker, 10, QS_SENDMESSAGE, MWMO_INPUTAVAILABLE);
            MSG message{};
            (void)PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
            if (waited == WAIT_FAILED) Sleep(1);
            lock.lock();
        }
        if (renderer.status.result != 0) return renderer.status.result;
        if (renderer.closing || renderer.pixels.empty() || size < renderer.pixels.size()) return kInvalid;
        if (frameReceipt || skinReceipt || particleReceipt || presentationReceipt || layerReceipt) {
            const auto& captured = renderer.capturedFrame;
            const auto& view = captured.view;
            if (captured.capture.ticket != ticket || !view.frameCount ||
                captured.capture.frameCount != view.frameCount ||
                captured.capture.documentRevision != view.documentRevision ||
                captured.capture.width != view.width || captured.capture.height != view.height ||
                view.documentRevision != renderer.state.mesh->revision ||
                view.width != renderer.state.width || view.height != renderer.state.height ||
                view.surfaceEpoch != renderer.state.surfaceEpoch || view.sceneEpoch != renderer.state.sceneEpoch ||
                view.viewEpoch != renderer.state.viewEpoch) return kTimeout;
            if (renderer.pixels.size() != static_cast<std::uint64_t>(view.width) * view.height * 4) {
                return static_cast<std::int32_t>(E_FAIL);
            }
        }
        if (presentationReceipt) {
            if (renderer.state.visibility != wantedVisibility || renderer.completedVisibilityRequest != wantedVisibility
                || renderer.completedModelEpoch != renderer.state.modelEpoch || renderer.state.modelEpoch != expectedModelEpoch
                || renderer.completedModelRequest != renderer.state.models || renderer.completedPoseRequest != renderer.state.skinPose
                || renderer.completedInstanceRequest != renderer.state.instances
                || renderer.completedParticleScene != renderer.state.particles || renderer.completedParticleFrame != renderer.state.particleFrame)
                return kTimeout;
            if (renderer.visibilityUploadResult < 0) return renderer.visibilityUploadResult;
            const auto& presentation = renderer.capturedPresentationFrame.presentation;
            if (presentation.modelEpoch != expectedModelEpoch || presentation.presentationRevision != expectedPresentationRevision
                || presentation.sceneVersion != wantedVisibility->status.sceneVersion
                || presentation.contentVersion != wantedVisibility->status.contentVersion
                // 实例增量保留静态models资产；当前名册来自已验证的skinPose，不能拿首次上传的实例数校验新完成帧。
                || presentation.instanceCount != PresentationTarget(renderer.state).instanceCount
                || presentation.particleDrawCount != (renderer.state.particleFrame ? renderer.state.particleFrame->draws.size() : 0))
                return kTimeout;
        }
        if (needsSkin) {
            if (renderer.completedModelRequest == renderer.state.models && renderer.completedPoseRequest == renderer.state.skinPose
                && renderer.completedParticleScene == renderer.state.particles && renderer.completedParticleFrame == renderer.state.particleFrame
                && renderer.skinUploadResult < 0) return renderer.skinUploadResult;
            const auto& skin = renderer.capturedSkinFrame.skin;
            if (!renderer.state.skinPose || !skin.active || skin.result != 0
                || skin.frameCount != renderer.capturedFrame.capture.frameCount
                || skin.sceneVersion != renderer.state.skinPose->sceneVersion
                || skin.contentVersion != renderer.state.skinPose->contentVersion) return kTimeout;
        }
        if (needsParticles) {
            const auto& particles = renderer.capturedParticleFrame.particles;
            if (!renderer.state.particles || !renderer.state.particleFrame
                || renderer.completedParticleScene != renderer.state.particles || renderer.completedParticleFrame != renderer.state.particleFrame
                || particles.sceneVersion != renderer.state.particleFrame->sceneVersion
                || particles.contentVersion != renderer.state.particleFrame->contentVersion
                || particles.vertexCount != renderer.state.particleFrame->vertices.size()
                || particles.drawCount != renderer.state.particleFrame->draws.size()) return kTimeout;
        }
        if (layerReceipt) {
            const auto& layered = renderer.capturedLayeredFrame;
            if (renderer.state.layersRevision != expectedLayersRevision || renderer.state.modelLayers != wantedLayers
                || renderer.completedLayers != wantedLayers || layered.layersRevision != expectedLayersRevision
                || layered.layerCount != wantedLayers->size() || renderer.completedModelEpoch != renderer.state.modelEpoch
                || renderer.completedModelRequest != renderer.state.models || renderer.completedPoseRequest != renderer.state.skinPose
                || renderer.completedParticleScene != renderer.state.particles || renderer.completedParticleFrame != renderer.state.particleFrame
                || renderer.completedInstanceRequest != renderer.state.instances) return kTimeout;
            if (renderer.skinUploadResult < 0) return renderer.skinUploadResult;
            for (std::size_t i = 0; i < wantedLayers->size(); ++i) {
                const auto& wanted = *(*wantedLayers)[i]; const auto& actual = layered.layers[i];
                if (actual.result != 0) return actual.result;
                if (actual.layerId != wanted.id || actual.parentModelEpoch != wanted.parentModelEpoch
                    || actual.parentModelEpoch != renderer.state.modelEpoch || actual.revision != wanted.revision
                    || actual.skin.sceneVersion != wanted.pose->sceneVersion || actual.skin.contentVersion != wanted.pose->contentVersion
                    || actual.skin.frameCount != layered.world.frame.capture.frameCount) return kTimeout;
                if (wanted.particleFrame && (actual.particles.sceneVersion != wanted.particleFrame->sceneVersion
                    || actual.particles.contentVersion != wanted.particleFrame->contentVersion
                    || actual.particles.vertexCount != wanted.particleFrame->vertices.size()
                    || actual.particles.drawCount != wanted.particleFrame->draws.size())) return kTimeout;
            }
        }
        std::memcpy(data, renderer.pixels.data(), renderer.pixels.size());
        if (layerReceipt) *layerReceipt = renderer.capturedLayeredFrame;
        else if (presentationReceipt) *presentationReceipt = renderer.capturedPresentationFrame;
        else if (particleReceipt) *particleReceipt = renderer.capturedParticleFrame;
        else if (skinReceipt) *skinReceipt = renderer.capturedSkinFrame;
        else if (frameReceipt) *frameReceipt = renderer.capturedFrame;
        else *legacyReceipt = renderer.captureInfo;
        return 0;
    } catch (...) { return static_cast<std::int32_t>(E_OUTOFMEMORY); }
}
std::int32_t WAR3_RENDER_CALL Capture(War3RendererHandle handle, void* data, std::uint64_t size,
    War3RenderCaptureInfoV2* receipt) noexcept {
    if (!handle || !data || !receipt || receipt->structSize != sizeof(*receipt)) return kInvalid;
    return CapturePixels(handle, data, size, receipt, nullptr);
}
std::int32_t WAR3_RENDER_CALL CaptureFrame(War3RendererHandle handle, void* data, std::uint64_t size,
    War3RenderCapturedFrameV2* receipt) noexcept {
    // 只读取前四字节，先拒绝短结构；接受未来更大的尾部，但只赋值已知结构前缀。
    if (!handle || !data || !receipt || receipt->structSize < sizeof(*receipt)) return kInvalid;
    return CapturePixels(handle, data, size, nullptr, receipt);
}
std::int32_t WAR3_RENDER_CALL CaptureSkinnedFrame(War3RendererHandle handle, void* data, std::uint64_t size,
    War3RenderSkinnedCapturedFrameV2* receipt) noexcept {
    if (!handle || !data || !receipt || receipt->structSize < sizeof(*receipt)) return kInvalid;
    return CapturePixels(handle, data, size, nullptr, nullptr, receipt);
}
std::int32_t WAR3_RENDER_CALL CaptureParticleFrame(War3RendererHandle handle, void* data, std::uint64_t size,
    War3RenderParticleCapturedFrameV2* receipt) noexcept {
    if (!handle || !data || !receipt || receipt->structSize < sizeof(*receipt)) return kInvalid;
    return CapturePixels(handle, data, size, nullptr, nullptr, nullptr, receipt);
}
std::int32_t WAR3_RENDER_CALL CaptureEditorPresentationFrame(War3RendererHandle handle,
    std::uint64_t expectedModelEpoch, std::uint64_t expectedPresentationRevision,
    void* data, std::uint64_t size, War3RenderEditorCapturedFrameV2* receipt) noexcept {
    if (!handle || !data || !receipt || receipt->structSize < sizeof(*receipt)
        || !expectedModelEpoch || !expectedPresentationRevision) return kInvalid;
    return CapturePixels(handle, data, size, nullptr, nullptr, nullptr, nullptr, receipt,
        expectedModelEpoch, expectedPresentationRevision);
}
std::int32_t WAR3_RENDER_CALL CaptureLayeredFrame(War3RendererHandle handle, std::uint64_t expectedLayersRevision,
    void* data, std::uint64_t size, War3RenderLayeredCapturedFrameV2* receipt) noexcept {
    if (!handle || !data || !receipt || receipt->structSize < sizeof(*receipt) || !expectedLayersRevision) return kInvalid;
    try {
        auto& renderer = *static_cast<Renderer*>(handle);
        std::uint64_t epoch = 0, presentation = 0;
        {
            std::lock_guard lock(renderer.mutex);
            if (renderer.state.layersRevision != expectedLayersRevision) return kTimeout;
            if (renderer.state.visibility) {
                epoch = renderer.state.modelEpoch; presentation = renderer.state.visibility->status.presentationRevision;
            }
        }
        War3RenderCapturedFrameV2 frame{sizeof(frame)};
        War3RenderEditorCapturedFrameV2 world{sizeof(world)};
        return CapturePixels(handle, data, size, nullptr, &frame, nullptr, nullptr,
            presentation ? &world : nullptr, epoch, presentation, receipt, expectedLayersRevision);
    } catch (...) { return static_cast<std::int32_t>(E_FAIL); }
}
std::int32_t WAR3_RENDER_CALL ViewInfo(War3RendererHandle handle, War3RenderViewInfoV2* output) noexcept {
    if (!handle || !output || output->structSize != sizeof(*output)) return kInvalid;
    try {
        auto& renderer = *static_cast<Renderer*>(handle);
        std::lock_guard lock(renderer.mutex);
        if (renderer.status.result != 0) return renderer.status.result;
        const auto& frame = renderer.viewInfo;
        if (!frame.frameCount || frame.surfaceEpoch != renderer.state.surfaceEpoch ||
            frame.sceneEpoch != renderer.state.sceneEpoch || frame.viewEpoch != renderer.state.viewEpoch) return kTimeout;
        *output = frame;
        return 0;
    } catch (...) { return static_cast<std::int32_t>(E_FAIL); }
}
std::int32_t WAR3_RENDER_CALL GetEditorCompletedFrame(War3RendererHandle handle,
    War3RenderEditorCompletedFrameV2* output) noexcept {
    if (!handle || !output || output->structSize < sizeof(*output)) return kInvalid;
    try {
        auto& renderer = *static_cast<Renderer*>(handle);
        std::lock_guard lock(renderer.mutex);
        if (renderer.status.result != 0) return renderer.status.result;
        const auto& state = renderer.state;
        const auto& frame = renderer.completedEditorFrame;
        const auto& view = frame.view;
        if (renderer.closing || !state.visibility || !view.frameCount
            || view.surfaceEpoch != state.surfaceEpoch || view.sceneEpoch != state.sceneEpoch || view.viewEpoch != state.viewEpoch
            || view.documentRevision != state.mesh->revision || view.width != state.width || view.height != state.height
            || renderer.completedModelEpoch != state.modelEpoch || renderer.completedModelRequest != state.models
            || renderer.completedPoseRequest != state.skinPose || renderer.completedInstanceRequest != state.instances
            || renderer.completedParticleScene != state.particles || renderer.completedParticleFrame != state.particleFrame
            || renderer.completedVisibilityRequest != state.visibility) return kTimeout;
        if (renderer.skinUploadResult < 0) return renderer.skinUploadResult;
        if (renderer.visibilityUploadResult < 0) return renderer.visibilityUploadResult;
        if (frame.presentation.modelEpoch != state.modelEpoch
            || frame.presentation.presentationRevision != state.visibility->status.presentationRevision) return kTimeout;
        *output = frame;
        return 0;
    } catch (...) { return static_cast<std::int32_t>(E_FAIL); }
}
std::int32_t WAR3_RENDER_CALL GetFrameMetrics(War3RendererHandle handle, std::uint64_t afterFrame,
    War3RenderFrameMetricsV2* records, std::uint32_t capacity, War3RenderFrameMetricsPageV2* output) noexcept {
    if (!handle || !output || output->structSize < sizeof(*output)
        || capacity > WAR3_RENDER_FRAME_METRICS_PAGE_LIMIT || (capacity && !records)) return kInvalid;
    try {
        auto& renderer = *static_cast<Renderer*>(handle);
        std::lock_guard lock(renderer.mutex);
        if (renderer.closing) return kTimeout;
        War3RenderFrameMetricsPageV2 page{};
        page.structSize = sizeof(page); page.capacity = WAR3_RENDER_FRAME_METRICS_RING_CAPACITY;
        page.software = renderer.metricsSoftware; page.featureLevel = renderer.metricsFeatureLevel;
        std::copy(renderer.metricsAdapterName.begin(), renderer.metricsAdapterName.end(), page.adapterName);
        if (renderer.metricsCount) {
            page.oldestFrame = renderer.frameMetrics[renderer.metricsFirst].completed.view.frameCount;
            const auto last = (renderer.metricsFirst + renderer.metricsCount - 1) % renderer.frameMetrics.size();
            page.latestFrame = renderer.frameMetrics[last].completed.view.frameCount;
            if (page.oldestFrame > afterFrame) page.lostBeforeCount = page.oldestFrame - afterFrame - 1;
            for (std::size_t i = 0; i < renderer.metricsCount && page.count < capacity; ++i) {
                const auto& metric = renderer.frameMetrics[(renderer.metricsFirst + i) % renderer.frameMetrics.size()];
                if (metric.completed.view.frameCount > afterFrame) records[page.count++] = metric;
            }
        }
        *output = page;
        return 0;
    } catch (...) { return static_cast<std::int32_t>(E_FAIL); }
}
const War3RenderPluginApiV2 api{sizeof(War3RenderPluginApiV2), WAR3_RENDER_PLUGIN_API_V2,
    &Create, &Destroy, &Attach, &Resize, &Submit, &SetCamera, &Frame, &Status, &Capture, &ViewInfo, &SubmitTerrain, &SubmitTerrainMips,
    &CaptureFrame, &SubmitModels, &SetEnvironment, &UpdateModelInstances, &SetOverlayLines,
    &SubmitSkinnedModels, &UpdateSkinMatrices, &GetSkinStatus, &CaptureSkinnedFrame,
    &SubmitParticleModels, &UpdateParticleFrame, &CaptureParticleFrame,
    &GetEditorPresentationTarget, &SubmitEditorVisibility, &CaptureEditorPresentationFrame, &SubmitEditorModelScene, &GetEditorCompletedFrame,
    &SubmitModelSceneLayer, &UpdateModelSceneLayer, &RemoveModelSceneLayer, &GetModelLayersTarget, &CaptureLayeredFrame, &GetFrameMetrics,
    &SubmitEditorInstanceRoster, &GetEditorInstanceRosterResult, &GetEditorRosterCapabilities, &CancelEditorInstanceRoster,
    &GetModelLayerCapabilities};
}
extern "C" WAR3_RENDER_EXPORT const War3RenderPluginApiV2* WAR3_RENDER_CALL
War3MapEditor_GetRenderPluginApiV2(std::uint32_t version) { return version == WAR3_RENDER_PLUGIN_API_V2 ? &api : nullptr; }
#ifdef WAR3_RENDER_TEST_FAULTS
extern "C" WAR3_RENDER_EXPORT std::int32_t WAR3_RENDER_CALL
War3RenderTest_SetFault(War3RendererHandle handle, std::uint32_t stage) {
    if (!handle || stage > 8) return kInvalid;
    if (stage == 6) static_cast<Renderer*>(handle)->testRosterGateReached.store(false);
    static_cast<Renderer*>(handle)->testFault.store(stage);
    return 0;
}
extern "C" WAR3_RENDER_EXPORT std::int32_t WAR3_RENDER_CALL
War3RenderTest_RosterGateReached(War3RendererHandle handle) {
    if (!handle) return kInvalid;
    return static_cast<Renderer*>(handle)->testRosterGateReached.load() ? 1 : 0;
}
extern "C" WAR3_RENDER_EXPORT std::int32_t WAR3_RENDER_CALL
War3RenderTest_GetAssetResources(War3RendererHandle handle, std::uint64_t* output, std::uint32_t capacity) {
    if (!handle || !output || capacity < 7) return kInvalid;
    auto& renderer = *static_cast<Renderer*>(handle); std::lock_guard lock(renderer.mutex);
    if (!renderer.completedEditorFrame.view.frameCount) return kTimeout;
    std::copy(renderer.completedAssetResources.begin(), renderer.completedAssetResources.end(), output); return 0;
}
#endif
