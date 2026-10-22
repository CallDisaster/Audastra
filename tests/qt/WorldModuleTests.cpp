#include "EditorProject.h"
#include "MapSaveJob.h"
#include "ProjectSaveJob.h"
#include "WorldModule.h"
#include "MapEntitiesModule.h"
#include "MapSettingsModule.h"
#include "WorldAnimationPlayer.h"
#include "WorldAnimationPlaybackContractChecks.h"
#include "WorldViewport.h"
#include "WorldOutlinerModel.h"
#include "WorldNavigationBookmark.h"
#include "WorldAssetBrowser.h"
#include "WorldTerrainAssets.h"
#include "WorldTerrainBuild.h"
#include "WorldCliffSourceFailureChecks.h"
#include "WorldShadowRelationMemo.h"
#include "WorldDiagnosticJson.h"
#include "WorldSelectionPresenter.h"
#include "WorldPlacementPreview.h"
#include "WorldPlacementModels.h"
#include "WorldAuxiliaryDialog.h"
#include "WorldStartLocations.h"
#include "ProjectAuxiliaryEdits.h"
#include "TerrainWaterEditFixture.h"
#include "War3MapCore/Trigger/TriggerDocument.h"
#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/MapData/MapAuxiliary.h"
#include "War3MapCore/MapData/PlacementEditing.h"
#include "War3MapCore/MapData/TerrainEditing.h"
#include "War3MapCore/MapData/TerrainSavePlan.h"
#include "War3MapCore/MapData/MapShadow.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/Workspace.h"
#include "War3MapCore/Game/GameDataSource.h"
#include "War3MapCore/ObjectData/StandardObjectCatalog.h"
#include "War3MapCore/ObjectData/LniObjectCodec.h"
#include "Widgets/ObjectCatalogWidgets.h"
#include "Fonts/FontLoader.h"
#include "../MapDataFixtures.h"
#include "../services/ObjectCommandsFixture.h"
#include "../AnimationParticleContinuityTests.h"
#include "MdxTestModel.h"
#include "EditorPresentationFixture.h"
#include "SyntheticObjectCatalogFixture.h"
#include "../../src/Render/Scene/ModelSkinInput.h"
#include "../../src/Render/Particles/ParticleInput.h"

#include <QApplication>
#include <QAction>
#include <QCheckBox>
#include <QDockWidget>
#include <QMainWindow>
#include <QListWidget>
#include "Theme/ThemeManager.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QInputDialog>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QScrollBar>
#include <QMouseEvent>
#include <QPushButton>
#include <QSignalSpy>
#include <QStandardItemModel>
#include <QSemaphore>
#include <QSpinBox>
#include <QToolButton>
#include <QTreeView>
#include <QTableWidget>
#include <QItemSelectionModel>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QTimer>
#include <QUndoStack>
#include <QWheelEvent>

#include <bit>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <memory>

using namespace War3Map;
using namespace War3Map::Modules;

namespace {
struct BackendState {
    EditorPresentationState editor;
    std::uint64_t skinScene = 0, skinContent = 0, skinUploads = 0, matrixUploads = 0;
    std::shared_ptr<const Render::ModelScene> skinModels, skinLayout, layerModels;
    std::shared_ptr<const Render::SkinPose> skinPose, layerPose;
    std::shared_ptr<const Render::ParticleFrameData> layerParticles;
    std::vector<Render::TextureRgba> layerParticleTextures;
    std::uint64_t layerParent = 7, layerRevision = 0, layersRevision = 1, layerId = 0;
    int layerSubmits = 0, layerUpdates = 0, layerRemoves = 0;
    std::uint32_t layerParticleVertices = 0;
    std::uint64_t layerScene = 0, layerContent = 0;
    std::uint64_t rosterSubmits = 0;
    std::uint64_t rosterAssetIdentity = 0, rosterAssetAppends = 0;
    War3RenderAssetPrefixV2 rosterAssets{};
    War3RenderInstanceRosterResultV2 roster{sizeof(roster)};
    float layerX = 0;
    int creates = 0, destroys = 0, frames = 0;
    std::uint32_t width = 0, height = 0;
    std::uint64_t revision = 0;
    std::size_t vertices = 0;
    WorldViewport* viewport = nullptr;
    bool destroyedBeforeWindow = true;
    bool stoppedBeforeDestroy = true;
    War3RenderCameraV2 camera{};
    int viewReads = 0, captures = 0;
    int terrainSubmits = 0;
    int mipSubmits = 0;
    std::uint32_t mipCount = 0, maxSampleMip = 0, atlasColumns = 0, atlasRows = 0;
    std::vector<std::byte> mipOneBytes;
    int blightQueries = 0, blightNullQueries = 0, blightSubmits = 0;
    std::int32_t blightQueryResult = 0;
    std::uint32_t blightFormat = 1, blightMaximum = 32, blightReserved = 0, blightCapsSize = 16;
    std::uint32_t blightBaseQuads = 0, blightBaseTextures = 0, blightMipCount = 0, blightMaxMip = 0;
    std::vector<std::uint32_t> blightIndices;
    std::vector<War3RenderTerrainBlightBatchV2> blightBatches;
    std::vector<std::byte> blightMipOneBytes;
    int gridQueries = 0, gridNullQueries = 0, gridSubmits = 0;
    std::int32_t gridQueryResult = 0;
    War3RenderTerrainGridCapabilitiesV2 gridCaps{sizeof(War3RenderTerrainGridCapabilitiesV2), 1, 16, 32, 1, 0};
    std::uint32_t gridVertexCount = 0, gridBaseCount = 0, gridWaterCount = 0, gridTransitionCount = 0;
    const War3RenderTerrainGridGroundVertexV2* gridGroundAddress = nullptr;
    const War3RenderTerrainGridWaterVertexV2* gridWaterAddress = nullptr;
    std::vector<War3RenderOverlayLineV2> overlayLines;
    bool rejectTerrain = false;
    bool autoComplete = true, pending = false, staleRevision = false, staleSize = false;
    std::int32_t statusResult = 0, viewResult = 0;
    std::uint64_t surfaceEpoch = 1, sceneEpoch = 1, viewEpoch = 1;
    War3RenderViewInfoV2 completed{sizeof(War3RenderViewInfoV2)};
    // 明确的原创顶视正交 MVP。射线从 Z=100 到 -100，覆盖 X±128、Y±64。
    std::array<float, 16> viewProjection{1.0f / 128, 0, 0, 0, 0, 1.0f / 64, 0, 0, 0, 0, -0.005f, 0, 0, 0, 0.5f, 1};
};
BackendState* activeBackend = nullptr;
War3RendererHandle WAR3_RENDER_CALL Create(const War3RenderCreateInfoV2* info) {
    ++activeBackend->creates;
    activeBackend->width = info->width; activeBackend->height = info->height;
    return activeBackend;
}
void WAR3_RENDER_CALL Destroy(War3RendererHandle handle) {
    auto& state = *static_cast<BackendState*>(handle);
    ++state.destroys;
    state.destroyedBeforeWindow = state.destroyedBeforeWindow && state.viewport && state.viewport->internalWinId() != 0;
    state.stoppedBeforeDestroy = state.stoppedBeforeDestroy && state.viewport && !state.viewport->FrameTimerActive();
}
std::int32_t WAR3_RENDER_CALL Attach(War3RendererHandle handle, void* native) {
    ++static_cast<BackendState*>(handle)->surfaceEpoch; return native ? 0 : 1;
}
std::int32_t WAR3_RENDER_CALL Resize(War3RendererHandle handle, std::uint32_t width, std::uint32_t height) {
    auto& state = *static_cast<BackendState*>(handle); state.width = width; state.height = height; ++state.surfaceEpoch; return 0;
}
std::int32_t WAR3_RENDER_CALL Mesh(War3RendererHandle handle, const War3RenderMeshV2* mesh) {
    auto& state = *static_cast<BackendState*>(handle); state.revision = mesh->documentRevision; state.vertices = mesh->vertexCount; ++state.sceneEpoch; return 0;
}
std::int32_t WAR3_RENDER_CALL Camera(War3RendererHandle handle, const War3RenderCameraV2* camera) {
    auto& state = *static_cast<BackendState*>(handle); state.camera = *camera; ++state.viewEpoch; return 0;
}
void WAR3_RENDER_CALL Frame(War3RendererHandle handle) {
    auto& state = *static_cast<BackendState*>(handle);
    ++state.frames;
    if (state.autoComplete) {
        state.completed = {sizeof(War3RenderViewInfoV2), state.width, state.height, static_cast<std::uint64_t>(state.frames),
            state.revision, state.surfaceEpoch, state.sceneEpoch, state.viewEpoch, {}};
        std::copy(state.viewProjection.begin(), state.viewProjection.end(), state.completed.viewProjection);
        EditorPresentationFixture<BackendState>::Draw(handle);
    }
}
std::int32_t WAR3_RENDER_CALL Status(War3RendererHandle handle, War3RenderStatusV2* status) {
    auto& state = *static_cast<BackendState*>(handle);
    status->result = state.statusResult;
    status->deviceReady = 1; status->width = state.width; status->height = state.height;
    status->frameCount = state.frames; status->documentRevision = state.revision; return 0;
}
std::int32_t WAR3_RENDER_CALL Capture(War3RendererHandle handle, void*, std::uint64_t, War3RenderCaptureInfoV2*) {
    ++static_cast<BackendState*>(handle)->captures; return 1;
}
std::int32_t WAR3_RENDER_CALL ViewInfo(War3RendererHandle handle, War3RenderViewInfoV2* output) {
    auto& state = *static_cast<BackendState*>(handle); ++state.viewReads;
    if (state.viewResult != 0) return state.viewResult;
    if (state.pending || state.completed.frameCount == 0 || state.completed.surfaceEpoch != state.surfaceEpoch ||
        state.completed.sceneEpoch != state.sceneEpoch || state.completed.viewEpoch != state.viewEpoch) return 2;
    *output = state.completed;
    if (state.staleRevision) ++output->documentRevision;
    if (state.staleSize) ++output->width;
    return 0;
}
std::int32_t WAR3_RENDER_CALL TerrainScene(War3RendererHandle handle, const War3RenderTerrainSceneV2* scene) {
    auto& state = *static_cast<BackendState*>(handle);
    ++state.terrainSubmits;
    if (state.rejectTerrain) return 1;
    state.revision = scene->documentRevision; state.vertices = scene->vertexCount; ++state.sceneEpoch;
    return 0;
}
const War3RenderPluginApiV2 backend{sizeof(War3RenderPluginApiV2), WAR3_RENDER_PLUGIN_API_V2,
    Create, Destroy, Attach, Resize, Mesh, Camera, Frame, Status, Capture, ViewInfo, TerrainScene};
std::int32_t WAR3_RENDER_CALL BoundaryOverlay(War3RendererHandle handle,
    const War3RenderOverlayLineV2* lines, std::uint32_t count) {
    auto& state = *static_cast<BackendState*>(handle);
    state.overlayLines.clear();
    if (count != 0) state.overlayLines.assign(lines, lines + count);
    return 0;
}
struct EnvironmentRecord { int submits = 0; std::uint32_t fogMode = 0; float fogEnd = 0; };
EnvironmentRecord environmentRecord;
std::int32_t WAR3_RENDER_CALL Environment(War3RendererHandle, const War3RenderEnvironmentV2* environment) {
    ++environmentRecord.submits;
    environmentRecord.fogMode = environment->fogMode;
    environmentRecord.fogEnd = environment->fogEnd;
    return 0;
}
const War3RenderPluginApiV2 environmentBackend = [] {
    auto api = backend;
    api.setEnvironment = Environment;
    return api;
}();
std::int32_t WAR3_RENDER_CALL ModelScene(War3RendererHandle handle, const War3RenderModelSceneV2*) {
    ++static_cast<BackendState*>(handle)->sceneEpoch;
    return 0;
}
const War3RenderPluginApiV2 modelBackend = [] {
    auto api = backend;
    api.submitModelScene = ModelScene;
    return api;
}();
// UI 调度夹具仍不绘制像素；保存完整 owned 输入，让能力声明有真实的 CPU 负载校验。
bool CopyFixtureTextures(const War3RenderTextureMipChainV2* input, std::uint32_t count,
    std::vector<Render::TextureRgba>& output) {
    if (count > 4096 || (count && !input)) return false;
    std::vector<Render::TextureRgba> candidate;
    std::uint64_t total = 0;
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto& chain = input[i];
        if (chain.structSize != sizeof(chain) || !chain.mipCount || chain.mipCount > 12
            || !chain.mips || chain.maxSampleMip >= chain.mipCount || chain.atlasColumns != 1 || chain.atlasRows != 1) return false;
        Render::TextureRgba texture;
        texture.maxSampleMip = chain.maxSampleMip;
        std::uint64_t chainBytes = 0;
        for (std::uint32_t level = 0; level < chain.mipCount; ++level) {
            const auto& mip = chain.mips[level];
            if (mip.structSize != sizeof(mip) || !mip.width || !mip.height || mip.width > 2048 || mip.height > 2048
                || !mip.rgba || mip.rgbaByteCount != std::uint64_t(mip.width) * mip.height * 4
                || mip.rgbaByteCount > 16ull * 1024 * 1024 || mip.rgbaByteCount > 24ull * 1024 * 1024 - chainBytes
                || mip.rgbaByteCount > 192ull * 1024 * 1024 - total) return false;
            if (level) {
                const auto& previous = chain.mips[level - 1];
                if ((previous.width == 1 && previous.height == 1) || mip.width != std::max(1u, previous.width / 2)
                    || mip.height != std::max(1u, previous.height / 2)) return false;
            }
            const auto* pixels = static_cast<const std::uint8_t*>(mip.rgba);
            texture.mips.push_back({mip.width, mip.height, {pixels, pixels + static_cast<std::size_t>(mip.rgbaByteCount)}});
            chainBytes += mip.rgbaByteCount; total += mip.rgbaByteCount;
        }
        candidate.push_back(std::move(texture));
    }
    output = std::move(candidate); return true;
}
bool CopyFixtureModels(const War3RenderSkinnedModelSceneV2& input, Render::ModelScene& output) {
    std::uint64_t payload = 0;
    return Render::CopySkinnedModelInput(input, output, payload)
        && CopyFixtureTextures(input.textures, input.textureCount, output.textures);
}
std::int32_t WAR3_RENDER_CALL SkinScene(War3RendererHandle h, const War3RenderSkinnedModelSceneV2* input) {
    auto& s = *static_cast<BackendState*>(h);
    auto models = std::make_shared<Render::ModelScene>();
    if (!input || !CopyFixtureModels(*input, *models)) return 1;
    s.skinPose = models->initialSkin; s.skinModels = s.skinLayout = std::move(models);
    s.skinScene = input->initialPose->sceneVersion; s.skinContent = input->initialPose->contentVersion;
    s.rosterAssets = {input->vertexCount, input->indexCount, input->layerCount, input->meshCount,
        input->modelCount, input->textureCount, 0};
    ++s.rosterAssetIdentity;
    ++s.skinUploads; ++s.matrixUploads; ++s.sceneEpoch; return 0;
}
std::int32_t WAR3_RENDER_CALL SkinUpdate(War3RendererHandle h, const War3RenderSkinUpdateV2* input) {
    auto& s = *static_cast<BackendState*>(h);
    auto pose = std::make_shared<Render::SkinPose>();
    if (!input || !s.skinLayout || input->sceneVersion != s.skinScene || input->contentVersion <= s.skinContent
        || !Render::CopySkinUpdate(*input, *s.skinLayout, *pose)) return 1;
    s.skinPose = std::move(pose);
    // 与生产插件一致：姿势更新推进content，不替换静态场景/视图sceneEpoch。
    s.skinContent = input->contentVersion; ++s.matrixUploads; return 0;
}
std::int32_t WAR3_RENDER_CALL SkinStatus(War3RendererHandle h, War3RenderSkinStatusV2* out) {
    const auto& s = *static_cast<BackendState*>(h);
    if (!out || out->structSize < sizeof(*out)) return 1;
    const auto capacity = out->structSize;
    *out = {sizeof(*out), 0, s.skinScene ? 1u : 0u, 0, static_cast<std::uint64_t>(s.frames),
        s.skinScene, s.skinContent, s.skinUploads, s.matrixUploads};
    if (capacity >= sizeof(War3RenderSkinColorCapabilitiesV2)) {
        auto& capabilities = *reinterpret_cast<War3RenderSkinColorCapabilitiesV2*>(out);
        capabilities.colorFormat = WAR3_RENDER_STATIC_MESH_RGB_V1; capabilities.reserved = 0;
    }
    if (capacity >= sizeof(War3RenderSkinDynamicColorCapabilitiesV2)) {
        auto& capabilities = *reinterpret_cast<War3RenderSkinDynamicColorCapabilitiesV2*>(out);
        capabilities.dynamicColorFormat = WAR3_RENDER_DYNAMIC_MESH_RGB_V1; capabilities.reserved = 0;
    }
    if (capacity >= sizeof(War3RenderSkinSharedRangeCapabilitiesV2)) {
        auto& capabilities = *reinterpret_cast<War3RenderSkinSharedRangeCapabilitiesV2*>(out);
        capabilities.sharedRangeMatrixFormat = 0; capabilities.reserved = 0;
    }
    if (capacity >= sizeof(War3RenderSkinMaterialCapabilitiesV2)) {
        auto& capabilities = *reinterpret_cast<War3RenderSkinMaterialCapabilitiesV2*>(out);
        capabilities.layerAlphaFormat = WAR3_RENDER_DYNAMIC_LAYER_ALPHA_V1; capabilities.reserved = 0;
    }
    if (capacity >= sizeof(War3RenderSkinMaterialStateCapabilitiesV2)) {
        auto& capabilities = *reinterpret_cast<War3RenderSkinMaterialStateCapabilitiesV2*>(out);
        capabilities.materialStateFormat = WAR3_RENDER_DYNAMIC_MATERIAL_STATE_V1; capabilities.reserved = 0;
    }
    return 0;
}
std::int32_t WAR3_RENDER_CALL SkinCapture(War3RendererHandle, void*, std::uint64_t, War3RenderSkinnedCapturedFrameV2*) { return 1; }
std::int32_t WAR3_RENDER_CALL LayerTarget(War3RendererHandle h, War3RenderModelLayersTargetV2* out) {
    const auto& s = *static_cast<BackendState*>(h);
    *out = {sizeof(*out), s.layerId ? 1u : 0u, s.layerParent, s.layersRevision}; return 0;
}
std::int32_t WAR3_RENDER_CALL LayerSubmit(War3RendererHandle h, const War3RenderModelSceneLayerV2* in) {
    auto& s = *static_cast<BackendState*>(h);
    if (!in) return 1;
    const auto* models = in->particles ? in->particles->models : in->models;
    if (!models || !models->initialPose || in->parentModelEpoch != s.layerParent
        || in->expectedRevision != s.layerRevision || in->revision != s.layerRevision + 1) return 1;
    const auto& pose = *models->initialPose;
    if (!pose.instanceCount && !in->particles) return 1;
    auto ownedModels = std::make_shared<Render::ModelScene>();
    if (!CopyFixtureModels(*models, *ownedModels)) return 1;
    std::shared_ptr<const Render::ParticleFrameData> particles;
    std::vector<Render::TextureRgba> particleTextures;
    if (in->particles) {
        const auto& scene = *in->particles;
        auto frame = std::make_shared<Render::ParticleFrameData>();
        auto particlePose = std::make_shared<Render::SkinPose>();
        if (scene.structSize != sizeof(scene) || !scene.initialFrame || !scene.initialFrame->modelPose
            || !CopyFixtureTextures(scene.textures, scene.textureCount, particleTextures)
            || !Render::CopyParticleInput(*scene.initialFrame, scene.textureCount, *frame)
            || !Render::CopySkinUpdate(*scene.initialFrame->modelPose, *ownedModels, *particlePose)
            || particlePose->sceneVersion != ownedModels->initialSkin->sceneVersion
            || particlePose->contentVersion != ownedModels->initialSkin->contentVersion) return 1;
        ownedModels->initialSkin = std::move(particlePose); particles = std::move(frame);
    }
    s.layerPose = ownedModels->initialSkin; s.layerModels = std::move(ownedModels);
    s.layerParticles = std::move(particles); s.layerParticleTextures = std::move(particleTextures);
    s.layerId = in->layerId; s.layerRevision = in->revision; ++s.layersRevision; ++s.layerSubmits;
    s.layerScene = pose.sceneVersion; s.layerContent = pose.contentVersion;
    s.layerParticleVertices = in->particles ? in->particles->initialFrame->vertexCount : 0;
    s.layerX = pose.instanceCount ? pose.instances[0].instance.transform[3] : 0; return 0;
}
std::int32_t WAR3_RENDER_CALL LayerUpdate(War3RendererHandle h, const War3RenderModelLayerUpdateV2* in) {
    auto& s = *static_cast<BackendState*>(h);
    if (!in) return 1;
    const auto* pose = in->particles ? in->particles->modelPose : in->pose;
    if (!pose || (!pose->instanceCount && !in->particles) || in->layerId != s.layerId || in->parentModelEpoch != s.layerParent
        || in->expectedRevision != s.layerRevision || in->revision != s.layerRevision + 1) return 1;
    if (pose->sceneVersion != s.layerScene || pose->contentVersion <= s.layerContent) return 1;
    auto ownedPose = std::make_shared<Render::SkinPose>();
    if (!s.layerModels || bool(s.layerParticles) != bool(in->particles)
        || !Render::CopySkinUpdate(*pose, *s.layerModels, *ownedPose)) return 1;
    auto particles = std::make_shared<Render::ParticleFrameData>();
    if (in->particles && !Render::CopyParticleInput(*in->particles,
        static_cast<std::uint32_t>(s.layerParticleTextures.size()), *particles)) return 1;
    s.layerPose = std::move(ownedPose);
    if (in->particles) s.layerParticles = std::move(particles);
    s.layerRevision = in->revision; ++s.layersRevision; ++s.layerUpdates;
    s.layerContent = pose->contentVersion;
    s.layerParticleVertices = in->particles ? in->particles->vertexCount : 0;
    s.layerX = pose->instanceCount ? pose->instances[0].instance.transform[3] : 0; return 0;
}
std::int32_t WAR3_RENDER_CALL LayerRemove(War3RendererHandle h, std::uint64_t id,
    std::uint64_t parent, std::uint64_t expected) {
    auto& s = *static_cast<BackendState*>(h);
    if (id != s.layerId || parent != s.layerParent || expected != s.layerRevision) return 1;
    s.layerModels.reset(); s.layerPose.reset(); s.layerParticles.reset(); s.layerParticleTextures.clear();
    s.layerId = s.layerRevision = 0; ++s.layersRevision; ++s.layerRemoves; return 0;
}
std::int32_t WAR3_RENDER_CALL LayerCapture(War3RendererHandle, std::uint64_t, void*, std::uint64_t,
    War3RenderLayeredCapturedFrameV2*) { return 1; }
const War3RenderPluginApiV2 layerBackend = [] {
    auto api = backend;
    api.submitModelSceneLayer = LayerSubmit; api.updateModelSceneLayer = LayerUpdate;
    api.removeModelSceneLayer = LayerRemove; api.getModelLayersTarget = LayerTarget;
    api.captureLayeredFrameRgba = LayerCapture; return api;
}();
const War3RenderPluginApiV2 particleLayerBackend = [] {
    auto api = layerBackend;
    api.submitModelScene = ModelScene;
    api.submitSkinnedModelScene = SkinScene; api.updateSkinMatrices = SkinUpdate;
    api.getSkinStatus = SkinStatus; api.captureSkinnedFrameRgba = SkinCapture;
    api.submitParticleModelScene = [](War3RendererHandle h, const War3RenderParticleSceneV2* in) -> std::int32_t {
        return SkinScene(h, in->models);
    };
    api.updateParticleModelFrame = [](War3RendererHandle h, const War3RenderParticleUpdateV2* in) -> std::int32_t {
        return SkinUpdate(h, in->modelPose);
    };
    api.captureParticleFrameRgba = [](War3RendererHandle, void*, std::uint64_t,
        War3RenderParticleCapturedFrameV2*) -> std::int32_t { return 1; };
    return api;
}();
// 调度夹具只核验提交/Draw身份；不产生模型像素，也不代替生产插件的GPU事务测试。
std::int32_t WAR3_RENDER_CALL SubmitRoster(War3RendererHandle handle,
    const War3RenderEditorInstanceRosterV2* input, std::uint64_t* ticket) {
    auto& state = *static_cast<BackendState*>(handle);
    auto& editor = state.editor;
    if (!input || !ticket || !input->candidatePose || input->candidateParticles || !input->visibility
        || (input->flags != 0 && input->flags != WAR3_RENDER_ROSTER_APPEND_ASSETS)
        || input->structSize != (input->flags ? sizeof(War3RenderEditorAssetRosterV2) : sizeof(*input))
        || input->reserved[0] || input->reserved[1]
        || !EditorPresentationFixture<BackendState>::Matches(input->expected, editor.target)
        || input->expectedAssetIdentity != state.rosterAssetIdentity || input->expectedLayersRevision != state.layersRevision
        || input->expectedPresentationRevision != editor.presentation.presentationRevision) return 1;
    if (!state.skinModels || !state.skinLayout) return 1;
    auto layout = std::make_shared<Render::ModelScene>();
    auto models = state.skinModels;
    auto assets = state.rosterAssets;
    if (input->flags == WAR3_RENDER_ROSTER_APPEND_ASSETS) {
        const auto* append = reinterpret_cast<const War3RenderEditorAssetRosterV2*>(input)->assets;
        if (!append || append->structSize != sizeof(*append) || append->reserved || append->particleTextureCount
            || (append->vertexCount && !append->vertices) || (append->indexCount && !append->indices)
            || (append->layerCount && !append->layers) || (append->meshCount && !append->meshes)
            || (append->modelCount && !append->models) || (append->modelTextureCount && !append->modelTextures)
            || (append->colorCount && (append->colorCount != append->meshCount || !append->colors))) return 1;
        const auto& before = state.rosterAssets;
        const auto& prefix = append->prefix;
        if (prefix.vertexCount != before.vertexCount || prefix.indexCount != before.indexCount
            || prefix.layerCount != before.layerCount || prefix.meshCount != before.meshCount
            || prefix.modelCount != before.modelCount || prefix.modelTextureCount != before.modelTextureCount
            || prefix.particleTextureCount != before.particleTextureCount) return 1;
        std::uint64_t payload = 0;
        if (!Render::CopySkinAssetAppend(*append, *input->candidatePose, state.skinModels, *layout, payload)
            || !CopyFixtureTextures(append->modelTextures, append->modelTextureCount, layout->textures)) return 1;
        layout->SealStaticSuffix(); models = layout;
        // 只核验本调度夹具的资产前缀/后缀和身份；不实现GPU上传，也不伪造模型像素。
        assets = {prefix.vertexCount + append->vertexCount, prefix.indexCount + append->indexCount,
            prefix.layerCount + append->layerCount, prefix.meshCount + append->meshCount,
            prefix.modelCount + append->modelCount, prefix.modelTextureCount + append->modelTextureCount,
            prefix.particleTextureCount};
    } else if (!Render::CopySkinRoster(*input->candidatePose, *state.skinLayout, *layout)) return 1;
    state.skinModels = std::move(models); state.skinLayout = std::move(layout);
    state.skinPose = state.skinLayout->initialSkin; state.rosterAssets = assets;
    if (input->flags == WAR3_RENDER_ROSTER_APPEND_ASSETS) { ++state.rosterAssetIdentity; ++state.rosterAssetAppends; }
    const auto& pose = *input->candidatePose;
    state.skinScene = pose.sceneVersion; state.skinContent = pose.contentVersion;
    ++state.matrixUploads; ++state.sceneEpoch; ++state.layersRevision;
    ++editor.target.modelEpoch;
    editor.target.sceneVersion = pose.sceneVersion; editor.target.contentVersion = pose.contentVersion;
    editor.target.instanceCount = pose.instanceCount;
    EditorPresentationFixture<BackendState>::Publish(editor, *input->visibility);
    state.roster = {}; state.roster.structSize = sizeof(state.roster);
    state.roster.state = WAR3_RENDER_ROSTER_QUEUED; state.roster.ticket = ++state.rosterSubmits;
    state.roster.assetIdentity = state.rosterAssetIdentity; state.roster.layersRevision = state.layersRevision;
    state.roster.target = editor.target; state.roster.presentationRevision = editor.presentation.presentationRevision;
    *ticket = state.roster.ticket; return 0;
}
std::int32_t WAR3_RENDER_CALL RosterResult(War3RendererHandle handle, std::uint64_t ticket,
    War3RenderInstanceRosterResultV2* output) {
    auto& state = *static_cast<BackendState*>(handle);
    if (!output) return 1;
    if (!ticket) {
        *output = {}; output->structSize = sizeof(*output); output->state = WAR3_RENDER_ROSTER_TARGET;
        output->assetIdentity = state.rosterAssetIdentity; output->layersRevision = state.layersRevision;
        output->target = state.editor.target;
        output->presentationRevision = state.editor.presentation.presentationRevision; return 0;
    }
    if (ticket != state.roster.ticket) return 1;
    const auto& completed = state.editor.completed;
    if (state.roster.state == WAR3_RENDER_ROSTER_QUEUED && !state.editor.pending
        && completed.presentation.modelEpoch == state.roster.target.modelEpoch
        && completed.skin.sceneVersion == state.roster.target.sceneVersion
        && completed.skin.contentVersion == state.roster.target.contentVersion) {
        state.roster.state = WAR3_RENDER_ROSTER_COMPLETED; state.roster.completed = completed;
    }
    *output = state.roster; return 0;
}
struct CatalogWorkerGate {
    QSemaphore entered, release, finished;
};
struct ReleaseCatalogWorker {
    std::shared_ptr<CatalogWorkerGate> gate;
    ~ReleaseCatalogWorker() { gate->release.release(); }
};
std::int32_t WAR3_RENDER_CALL MipScene(War3RendererHandle handle, const War3RenderTerrainMipSceneV2* scene) {
    auto& state = *static_cast<BackendState*>(handle);
    ++state.mipSubmits;
    if (state.rejectTerrain) return 1;
    state.blightIndices.clear(); state.blightBatches.clear(); state.blightMipOneBytes.clear();
    if (scene->textureCount) {
        const auto& chain = scene->textures[0];
        state.mipCount = chain.mipCount; state.maxSampleMip = chain.maxSampleMip;
        state.atlasColumns = chain.atlasColumns; state.atlasRows = chain.atlasRows;
        if (chain.mipCount > 1) {
            const auto* pixels = static_cast<const std::byte*>(chain.mips[1].rgba);
            state.mipOneBytes.assign(pixels, pixels + chain.mips[1].rgbaByteCount);
        }
    }
    state.revision = scene->documentRevision; state.vertices = scene->vertexCount; ++state.sceneEpoch;
    return 0;
}
const War3RenderPluginApiV2 mipBackend{sizeof(War3RenderPluginApiV2), WAR3_RENDER_PLUGIN_API_V2,
    Create, Destroy, Attach, Resize, Mesh, Camera, Frame, Status, Capture, ViewInfo, TerrainScene, MipScene};
std::int32_t WAR3_RENDER_CALL BlightCapabilities(War3RendererHandle handle, War3RenderTerrainBlightCapabilitiesV2* output) {
    auto& state = handle ? *static_cast<BackendState*>(handle) : *activeBackend;
    ++state.blightQueries;
    state.blightNullQueries += handle == nullptr;
    if (state.blightQueryResult) return state.blightQueryResult;
    *output = {state.blightCapsSize, state.blightFormat, state.blightMaximum, state.blightReserved};
    return 0;
}
std::int32_t WAR3_RENDER_CALL BlightFrame(War3RendererHandle handle, const War3RenderTerrainBlightFrameV2* frame) {
    auto& state = *static_cast<BackendState*>(handle);
    ++state.blightSubmits;
    if (state.rejectTerrain) return 1;
    state.blightBaseQuads = frame->baseQuadCount; state.blightBaseTextures = frame->baseScene.textureCount;
    state.blightIndices.clear(); state.blightBatches.clear(); state.blightMipOneBytes.clear();
    state.blightMipCount = state.blightMaxMip = 0;
    if (frame->blightIndexCount) state.blightIndices.assign(frame->blightIndices, frame->blightIndices + frame->blightIndexCount);
    if (frame->blightBatchCount) state.blightBatches.assign(frame->blightBatches, frame->blightBatches + frame->blightBatchCount);
    if (frame->blightTexture) {
        state.blightMipCount = frame->blightTexture->mipCount; state.blightMaxMip = frame->blightTexture->maxSampleMip;
        if (frame->blightTexture->mipCount > 1) {
            const auto& mip = frame->blightTexture->mips[1];
            const auto* bytes = static_cast<const std::byte*>(mip.rgba);
            state.blightMipOneBytes.assign(bytes, bytes + mip.rgbaByteCount);
        }
    }
    state.revision = frame->baseScene.documentRevision; state.vertices = frame->baseScene.vertexCount; ++state.sceneEpoch;
    return 0;
}
const War3RenderPluginApiV2 legacyBackend{static_cast<std::uint32_t>(offsetof(War3RenderPluginApiV2, getViewInfo)), WAR3_RENDER_PLUGIN_API_V2,
    Create, Destroy, Attach, Resize, Mesh, Camera, Frame, Status, Capture, ViewInfo}; // 尾部虽在内存中，旧 size 禁止读取/调用。

std::int32_t WAR3_RENDER_CALL GridCapabilities(War3RendererHandle handle, War3RenderTerrainGridCapabilitiesV2* output) {
    auto& state = handle ? *static_cast<BackendState*>(handle) : *activeBackend;
    ++state.gridQueries; state.gridNullQueries += handle == nullptr;
    if (state.gridQueryResult) return state.gridQueryResult;
    *output = state.gridCaps; return 0;
}
std::int32_t WAR3_RENDER_CALL GridFrame(War3RendererHandle handle, const War3RenderTerrainGridFrameV2* frame) {
    auto& state = *static_cast<BackendState*>(handle); ++state.gridSubmits;
    if (state.rejectTerrain) return 1;
    state.gridVertexCount = frame->gridVertexCount; state.gridBaseCount = frame->baseQuadCount;
    state.gridWaterCount = frame->waterQuadCount; state.gridTransitionCount = frame->groundScene.vertexCount;
    state.gridGroundAddress = frame->groundGrid; state.gridWaterAddress = frame->waterGrid;
    state.revision = frame->groundScene.documentRevision; state.vertices = frame->gridVertexCount; ++state.sceneEpoch;
    return 0;
}
std::int32_t WAR3_RENDER_CALL GridFacts(War3RendererHandle, const War3RenderViewInfoV2*, War3RenderTerrainGridCaptureFactsV2*) { return 2; }

void Append32(QByteArray& bytes, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) bytes.append(static_cast<char>((value >> shift) & 0xffu));
}
void AppendFloat(QByteArray& bytes, float value) { Append32(bytes, std::bit_cast<std::uint32_t>(value)); }
void Append16(QByteArray& bytes, std::uint16_t value) {
    bytes.append(static_cast<char>(value & 0xffu)); bytes.append(static_cast<char>(value >> 8));
}
QByteArray TerrainBytes(float offsetX = -128) {
    QByteArray bytes("W3E!", 4);
    Append32(bytes, 11); bytes.append('A'); Append32(bytes, 0);
    Append32(bytes, 1); bytes.append("Ldrt", 4);
    Append32(bytes, 1); bytes.append("Cldi", 4);
    Append32(bytes, 3); Append32(bytes, 2); AppendFloat(bytes, offsetX); AppendFloat(bytes, -64);
    for (std::uint16_t index = 0; index < 6; ++index) {
        Append16(bytes, static_cast<std::uint16_t>(8192 + 4 * index)); Append16(bytes, 8192);
        bytes.append('\0'); bytes.append('\0'); bytes.append('\2');
    }
    return bytes;
}
bool Write(const QString& path, const QByteArray& bytes) {
    QFile file(path); return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
void Mouse(WorldViewport& viewport, QEvent::Type type, QPointF position, Qt::MouseButton button,
    Qt::MouseButtons buttons, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    QMouseEvent event(type, position, position, button, buttons, modifiers);
    QApplication::sendEvent(&viewport, &event);
}
std::shared_ptr<const MapData::TerrainGridSnapshot> TestTerrain() {
    const auto bytes = TerrainBytes();
    const auto parsed = MapData::ParseMapTerrain(std::span<const std::byte>(reinterpret_cast<const std::byte*>(bytes.constData()), static_cast<std::size_t>(bytes.size())));
    return parsed.Succeeded() ? parsed.value->Snapshot() : nullptr;
}
std::shared_ptr<MapData::TerrainGridSnapshot> FlatGrid(std::uint32_t width, std::uint32_t height) {
    auto grid = std::make_shared<MapData::TerrainGridSnapshot>();
    grid->header.width = width; grid->header.height = height; grid->header.offset = {-128, -64};
    grid->minimum = {-128, -64, 0};
    grid->maximum = {-128 + static_cast<float>(width - 1) * 128, -64 + static_cast<float>(height - 1) * 128, 0};
    for (std::uint32_t y = 0; y < height; ++y) for (std::uint32_t x = 0; x < width; ++x) {
        MapData::TerrainVertex vertex;
        vertex.groundHeightRaw = 8192; vertex.waterHeightAndFlagsRaw = 8192; vertex.cliffAndLayerRaw = 2;
        vertex.position = {-128 + static_cast<float>(x) * 128, -64 + static_cast<float>(y) * 128, 0};
        grid->vertices.push_back(vertex);
        if (x + 1 < width && y + 1 < height) {
            const auto sw = y * width + x;
            grid->tiles.push_back({{sw, sw + 1, sw + width, sw + width + 1}});
        }
    }
    return grid;
}

bool SameShadowDiagnostics(const std::vector<Diagnostic>& left, const std::vector<Diagnostic>& right) {
    if (left.size() != right.size()) return false;
    for (std::size_t i = 0; i < left.size(); ++i) {
        const auto& a = left[i]; const auto& b = right[i];
        if (a.severity != b.severity || a.subsystem != b.subsystem || a.code != b.code || a.message != b.message
            || a.origin.has_value() != b.origin.has_value()) return false;
        if (a.origin && (a.origin->source != b.origin->source || a.origin->offset != b.origin->offset
            || a.origin->length != b.origin->length || a.origin->fingerprint != b.origin->fingerprint)) return false;
    }
    return true;
}

std::vector<MapArchiveMember> ShadowObservationMembers() {
    auto grid = FlatGrid(5, 5); grid->header.mainTileset = 'A';
    grid->header.groundTilesets = {"Ldrt"}; grid->header.cliffTilesets = {"Cldi"};
    const auto terrain = MapData::SerializeMapTerrain(*grid);
    if (!terrain.Succeeded()) return {};
    const auto text = [](std::string_view value) {
        const auto bytes = std::as_bytes(std::span(value.data(), value.size())); return std::vector<std::byte>(bytes.begin(), bytes.end());
    };
    Image::DecodedImage image{16, 16, std::vector<std::byte>(16 * 16 * 4, std::byte{255})};
    const auto texture = Image::EncodeBlpPaletted(image); if (!texture.Succeeded()) return {};
    return {{"war3map.w3i", MapDataFixtures::FullMapSettingsV25().bytes}, {"war3map.w3e", *terrain.value},
        {"war3mapUnits.doo", MapDataFixtures::EmptyUnitsV8Sub11()}, {"war3map.doo", MapDataFixtures::EmptyDoodadsV8Sub11()},
        {"TerrainArt/Terrain.slk", text("ID;PWXL;N;E\nC;X1;Y1;K\"tileID\"\nC;X2;Y1;K\"dir\"\nC;X3;Y1;K\"file\"\n"
            "C;X1;Y2;K\"Ldrt\"\nC;X2;Y2;K\"Terrain/Test\"\nC;X3;Y2;K\"stone\"\nE\n")},
        {"TerrainArt/CliffTypes.slk", text("ID;PWXL;N;E\nC;X1;Y1;K\"cliffID\"\nE\n")},
        {"TerrainArt/Water.slk", text("ID;PWXL;N;E\nC;X1;Y1;K\"waterID\"\nE\n")}, {"Terrain/Test/stone.blp", *texture.value}};
}
bool BindTerrain(WorldViewport& viewport, std::shared_ptr<const MapData::TerrainGridSnapshot> terrain, std::uint64_t revision = 12) {
    if (!terrain) return false;
    auto mesh = BuildWorldTerrainMesh(*terrain);
    if (!mesh.Succeeded()) return false;
    const auto stride = mesh.value->stride;
    return viewport.SetMesh(std::move(mesh.value->vertices), std::move(mesh.value->indices), revision)
        && viewport.SetPickingTerrain(std::move(terrain), stride, revision);
}

bool InstallAuxiliaryEditFixture(QtProject::EditorProject& project, const QString& root,
    const MapData::RegionRecord& region, const MapData::CameraRecord& camera) {
    auto grid = FlatGrid(5, 5);
    grid->header.mainTileset = 'A';
    grid->header.groundTilesets = {"Ldrt"}; grid->header.cliffTilesets = {"Cldi"};
    const auto terrain = MapData::SerializeMapTerrain(*grid);
    const auto regions = MapData::RegionsDocument{5, {region}, {}}.Serialize();
    const auto cameras = MapData::CamerasDocument{0, {camera}, {}}.Serialize();
    if (!terrain.Succeeded() || !regions.Succeeded() || !cameras.Succeeded()) return false;
    if (!Write(root + "/war3map.w3e", QByteArray(reinterpret_cast<const char*>(terrain.value->data()),
        static_cast<qsizetype>(terrain.value->size())))) return false;
    project.projectRoot_ = root.toStdWString();
    auto opened = DirectoryWorkspace::Open(project.projectRoot_);
    if (!opened.Succeeded()) return false;
    project.directorySnapshot_ = std::make_unique<DirectoryWorkspace>(std::move(*opened.value));
    const std::vector<MapArchiveMember> members{{"war3map.w3r", *regions.value}, {"war3map.w3c", *cameras.value}};
    project.mapDocument_ = std::make_unique<MapDocument>(MapDocument::FromMembers(members));
    project.mapEdits_ = std::make_unique<MapData::MapDataEditSession>(*project.mapDocument_);
    if (!project.mapEdits_->AttachTerrain(*terrain.value).Succeeded()) return false;
    project.triggerDocument_ = std::make_unique<Trigger::TriggerDocument>(); // 原创完整空 T，允许新符号检查。
    project.ResetEditTracking();
    return true;
}

bool SelectAuxiliaryRow(QWidget& page, const Services::WorldObjectId& id, Qt::KeyboardModifiers modifiers = {}) {
    auto* outliner = page.findChild<QTreeView*>("worldOutliner");
    auto* model = outliner ? dynamic_cast<WorldOutlinerModel*>(outliner->model()) : nullptr;
    if (!model) return false;
    const auto index = model->Find(id);
    if (!index.isValid()) return false;
    outliner->expandAll(); outliner->scrollTo(index); outliner->doItemsLayout();
    const auto rect = outliner->visualRect(index);
    if (!rect.isValid()) return false;
    QTest::mouseClick(outliner->viewport(), Qt::LeftButton, modifiers, rect.center());
    return true;
}
bool InstallStartLocationEditFixture(QtProject::EditorProject& project, const QString& root,
    bool duplicateOwner = false, bool ordinaryUnit = false) {
    auto grid = FlatGrid(5, 5);
    grid->header.mainTileset = 'A'; grid->header.groundTilesets = {"Ldrt"}; grid->header.cliffTilesets = {"Cldi"};
    const auto terrain = MapData::SerializeMapTerrain(*grid);
    auto units = MapData::ParseUnitPlacements(MapDataFixtures::EmptyUnitsV8Sub11());
    if (!terrain.Succeeded() || !units.Succeeded()) return false;
    MapData::UnitPlacement unit;
    unit.rawcode = "sloc"; unit.creationId = 10; unit.owner = 0; unit.position = {64, 64, 5}; unit.scale = {1, 1, 1};
    units.value->units.push_back(unit);
    if (duplicateOwner) { unit.creationId = 20; unit.position = {192, 192, 5}; units.value->units.push_back(unit); }
    if (ordinaryUnit) { unit.rawcode = "hfoo"; unit.creationId = 30; unit.owner = 1; unit.position = {128, 64, 0}; units.value->units.push_back(unit); }
    const auto encoded = units.value->Serialize(); if (!encoded.Succeeded()) return false;
    if (!Write(root + "/war3map.w3e", QByteArray(reinterpret_cast<const char*>(terrain.value->data()),
        static_cast<qsizetype>(terrain.value->size())))) return false;
    project.projectRoot_ = root.toStdWString();
    auto opened = DirectoryWorkspace::Open(project.projectRoot_); if (!opened.Succeeded()) return false;
    project.directorySnapshot_ = std::make_unique<DirectoryWorkspace>(std::move(*opened.value));
    const std::vector<MapArchiveMember> members{{"war3map.w3i", MapDataFixtures::FullMapSettingsV25().bytes},
        {"war3mapUnits.doo", *encoded.value}, {"war3map.doo", MapDataFixtures::EmptyDoodadsV8Sub11()}};
    project.mapDocument_ = std::make_unique<MapDocument>(MapDocument::FromMembers(members));
    project.mapEdits_ = std::make_unique<MapData::MapDataEditSession>(*project.mapDocument_);
    if (!project.mapEdits_->AttachTerrain(*terrain.value).Succeeded()) return false;
    project.ResetEditTracking(); return true;
}
QPointF AuxiliaryScreenPoint(const WorldViewport& viewport, const BackendState& state, QPointF world, float span = 192) {
    // 拾取使用完成帧的物理尺寸；1.25 DPR 会将尺寸取整，不能用逻辑宽高直接反投影。
    const auto scale = viewport.devicePixelRatioF();
    return {((world.x() - 64) / span + 1) * state.completed.width / (2 * scale),
        (1 - (world.y() - 64) / span) * state.completed.height / (2 * scale)};
}
QPointF MoveAuxiliaryPointer(WorldViewport& viewport, const BackendState& state, QPointF world) {
    QPointF point;
    // 首次悬停会更新页脚并触发布局。只在本次指针所借完成帧的几何保持不变时开始手势，
    // 不把旧尺寸反投影出的点交给新尺寸帧，也不放宽世界坐标断言。
    for (int attempt = 0; attempt < 4; ++attempt) {
        const auto before = state.completed;
        const auto ratio = viewport.devicePixelRatioF();
        point = AuxiliaryScreenPoint(viewport, state, world);
        Mouse(viewport, QEvent::MouseMove, point, Qt::NoButton, Qt::NoButton);
        QTest::qWait(75); // 覆盖生产的 50 ms 悬停合并及其后布局事件。
        const auto& after = state.completed;
        if (ratio == viewport.devicePixelRatioF() && before.width == after.width && before.height == after.height
            && before.surfaceEpoch == after.surfaceEpoch && before.sceneEpoch == after.sceneEpoch
            && before.viewEpoch == after.viewEpoch) return point;
    }
    return point; // 仍由调用方的精确坐标和事务断言拒绝不稳定的夹具。
}
QModelIndex FindWorldObject(WorldOutlinerModel& model, const Services::WorldObjectId& id) {
    for (int page = 0; page < 64 && !model.Find(id).isValid() && model.canFetchMore({}); ++page) model.fetchMore({});
    return model.Find(id);
}
std::vector<MapArchiveMember> PlacementPreviewMembers(bool pureParticle = false) {
    auto model = AnimationParticleContinuityTest::Model();
    if (pureParticle) {
        std::vector<std::byte> particleModel(model.begin(), model.begin() + 4);
        for (std::size_t offset = 4; offset + 8 <= model.size();) {
            std::uint32_t size = 0;
            for (unsigned i = 0; i < 4; ++i) size |= std::to_integer<std::uint32_t>(model[offset + 4 + i]) << (8 * i);
            const std::string_view tag(reinterpret_cast<const char*>(model.data() + offset), 4);
            if (tag != "GEOS" && tag != "GEOA") particleModel.insert(particleModel.end(),
                model.begin() + static_cast<std::ptrdiff_t>(offset), model.begin() + static_cast<std::ptrdiff_t>(offset + 8 + size));
            offset += 8 + size;
        }
        model = std::move(particleModel);
    }
    const auto terrain = TerrainBytes();
    const auto* terrainBegin = reinterpret_cast<const std::byte*>(terrain.constData());
    return {{"war3map.w3i", MapDataFixtures::FullMapSettingsV25().bytes},
        {"war3map.w3e", {terrainBegin, terrainBegin + terrain.size()}},
        {"war3mapUnits.doo", MapDataFixtures::EmptyUnitsV8Sub11()},
        {"war3map.doo", MapDataFixtures::EmptyDoodadsV8Sub11()},
        {"Synthetic\\Arcane.mdx", std::move(model)},
        {"Textures\\Crate.blp", AnimationParticleContinuityTest::Texture()},
        {"ReplaceableTextures\\TeamColor\\TeamColor00.blp", AnimationParticleContinuityTest::Texture()}};
}
bool InstallPlacementCatalog(QtProject::EditorProject& project) {
    Tests::ObjectCommandsFixture fields;
    project.objectFieldSchema_ = std::make_unique<ObjectData::ObjectFieldSchema>(std::move(fields.schema));
    const std::string slk = "ID;PWXL\nC;X1;Y1;K\"alias\"\nC;X2;K\"Name\"\n"
        "C;X1;Y2;K\"hsan\"\nC;X2;K\"Synthetic sanctuary\"\nE\n";
    ObjectData::CatalogLoadOptions options; options.stringFiles.clear();
    options.sources = {{ObjectData::ObjectType::Unit, {"unit.slk"}, {}, {"name"}, {"art"}}};
    auto catalog = ObjectData::StandardObjectCatalog::Load([&](const std::string&) {
        const auto bytes = std::as_bytes(std::span(slk.data(), slk.size()));
        return Result<std::vector<std::byte>>::Success({bytes.begin(), bytes.end()});
    }, options);
    if (!catalog.Succeeded()) return false;
    if (!Tests::InstallSyntheticGameCatalog(project, *catalog.value)) return false;
    const std::string lni = "[hsan]\nunam=\"神秘圣地合成夹具\"\nfile=\"Synthetic/Arcane.mdx\"\n";
    auto parsed = ObjectData::LniObjectCodec(ObjectData::ObjectType::Unit).Parse(std::as_bytes(std::span(lni.data(), lni.size())));
    if (!parsed.Succeeded()) return false;
    project.objectDocuments_[0].reset(dynamic_cast<ObjectData::LniObjectDocument*>(parsed.document.release()));
    return project.objectDocuments_[0] != nullptr;
}
std::optional<MapData::PlacementId> AttachTrackedResourcePlacement(QtProject::EditorProject& project) {
    if (!project.archiveSession_) return {};
    project.mapDocument_ = std::make_unique<MapDocument>(MapDocument::FromArchive(*project.archiveSession_));
    project.mapEdits_ = std::make_unique<MapData::MapDataEditSession>(*project.mapDocument_);
    const auto terrain = TerrainBytes();
    if (!project.mapEdits_->AttachTerrain(std::as_bytes(std::span(terrain.constData(),
        static_cast<std::size_t>(terrain.size())))).Succeeded()) return {};
    MapData::UnitPlacement unit; unit.rawcode = "hsan"; unit.flags = 2;
    unit.scale = {1, 1, 1}; unit.position = {-32, 0, 0};
    const auto added = MapData::AddUnitPlacement(*project.mapEdits_, unit);
    if (!added.Succeeded()) return {};
    project.projectRoot_ = project.sourceArchivePath_.parent_path();
    project.ResetEditTracking();
    return project.editTrackingReady_ ? added.value : std::nullopt;
}
} // namespace

class WorldModuleTests final : public QObject {
    Q_OBJECT
private slots:
    void HeroLevelEditsOnlyHeroes();
    void TerrainFogFollowsMapSettingsFlag();
    void SourceShadowObservationKeepsUnknownAndCodecFailures_data();
    void SourceShadowObservationKeepsUnknownAndCodecFailures();
    void SourceShadowObservationSurvivesRebuildWithoutRawOrRepeatedDiagnostics();
    void CliffSourceFailuresKeepWinnerAndReobserveReadFailures_data();
    void CliffSourceFailuresKeepWinnerAndReobserveReadFailures();
    void CliffSamplerFailureAfterMissingDoesNotInventModelContext();
    void MeshBoundsAndSampling();
    void ViewportLifecycleAndInput();
    void MapSwitchDoesNotEdit();
    void PlaybackSnapshotKeepsServiceAndRetainedPictureDistinct();
    void TerrainLoadSnapshotPreservesFailureAndClearsOnRefresh_data();
    void TerrainLoadSnapshotPreservesFailureAndClearsOnRefresh();
    void TerrainOnlySceneRequiresKnownEmptyPlacements_data();
    void TerrainOnlySceneRequiresKnownEmptyPlacements();
    void TerrainOnlySceneRejectsStaleSourceAndRefresh();
    void InstanceCatalogStartsBesidePose_data();
    void InstanceCatalogStartsBesidePose();
    void AmbiguousTerrainAndDeferredLoad();
    void OversizedViewportDoesNotCreate();
    void HoverClickAndPendingFrames();
    void ResizeDprAndLegacyFallback();
    void SimplifiedAndMismatchedPickingDisabled();
    void GroundSceneKeepsPickingAndRejectsDifferentGeometry();
    void TerrainResourceOverlayAndSourceIdentity();
    void ReadOnlyArchiveLoadsTerrainAndRejectsIdenticalReplacement();
    void SelectionLayerWaitsForRosterTerminal();
    void MipSceneUsesOriginalChainAndClampsAtlas();
    void SharedBaseBlightRequiresCapabilitiesAndKeepsAtomicFrame();
    void SharedGridRequiresCompleteCapabilitiesAndKeepsAtomicFrame();
    void PlacementClickDragAndKeys();
    void TerrainBrushPreservesCameraAndModels();
    void TerrainBrushUsesSharedTransactionAndUndo();
    void TerrainWaterBrushUsesFrozenPrefixAndSharedUndo_data();
    void TerrainWaterBrushUsesFrozenPrefixAndSharedUndo();
    void TerrainWaterBrushRejectsChangedInputs();
    void TerrainWaterBrushRejectsChangedGameResources_data();
    void TerrainWaterBrushRejectsChangedGameResources();
    void TerrainWaterBrushDropsCancelledCommit_data();
    void TerrainWaterBrushDropsCancelledCommit();
    void TerrainTextureSlotUsesSharedTransaction();
    void TerrainRampKeepsCellAndWholeStroke();
    void TerrainBoundaryMarksAndRestores_data();
    void TerrainBoundaryMarksAndRestores();
    void TerrainPathingUsesCellsAndPreservesSource_data();
    void TerrainPathingUsesCellsAndPreservesSource();
    void InspectorEditsPreserveOtherFloatBits();
    void AuxiliaryFormPreservesNativeBytes();
    void AuxiliaryResultRejectsStaleSessionAndCoverage();
    void WorldAuxiliaryCrudUsesSharedService();
    void OutlinerPagingRestoresAuxiliarySelection_data();
    void OutlinerPagingRestoresAuxiliarySelection();
    void WorldAuxiliaryDialogRejectsChangedMap();
    void StartLocationMarkerCaptureIsBounded();
    void StartLocationDragLinksSettingsAndUndo();
    void StartLocationAmbiguityRejectsWholeSelection();
    void RegionEdgesUseSharedTableTransaction_data();
    void RegionEdgesUseSharedTableTransaction();
    void AuxiliaryDragCancellationAndIdentity();
    void CameraDragUsesSharedTableTransaction();
    void TypedMultiSelectionWithoutModelsUsesOneUndo();
    void NavigationTypedBookmarkCodecKeepsAllIdentityBytes();
    void NavigationBookmarkRestoresPagedOutlinePrimaryAndCamera();
    void PlacementCaptureFileScopeMatchesFullCapture();
    void BrowserSelectionSynchronizesPlacementTool();
    void HiddenOutlinerKeepsTypedEditsAndRefreshesOnce();
    void PlacementParticleClock_data();
    void PlacementParticleClock();
    void NativePlacementFrames();
    void DockWorkspaceUsesLeftTabsAndRestoresBottomAssets();
    void ShadowRelationMemoRejectsForeignSourcesAndReusesRevision();
    void ShadowRelationMemoTracksHeightUndoRedoAndSavedBaseline();
    void ShadowRelationMemoTracksAttachGenerationsAndSessionIdentity();
};

void WorldModuleTests::PlacementCaptureFileScopeMatchesFullCapture() {
    QtProject::EditorProject project;
    const auto install = [&](ObjectData::ObjectType type, const std::string& text) {
        auto parsed = ObjectData::LniObjectCodec(type).Parse(
            std::span(reinterpret_cast<const std::byte*>(text.data()), text.size()));
        if (!parsed.document) return false;
        auto& target = project.objectDocuments_[static_cast<std::size_t>(type)];
        target.reset(dynamic_cast<ObjectData::LniObjectDocument*>(parsed.document.release()));
        return target != nullptr;
    };
    QVERIFY(install(ObjectData::ObjectType::Unit, "[u001]\nfile=\"Unit.mdl\"\nusca=2\n"));
    QVERIFY(install(ObjectData::ObjectType::Item, "[i001]\nfile=\"Item.mdl\"\nisca=1.5\n"));
    QVERIFY(install(ObjectData::ObjectType::Doodad, "[d001]\ndfil=\"Doodad.mdl\"\n"));
    QVERIFY(install(ObjectData::ObjectType::Destructable, "[b001]\nbcat=\"P\"\nbuch=1\nbfil=\"\"\nbptx=\"_\"\n"));
    auto units = MapData::ParseUnitPlacements(MapDataFixtures::EmptyUnitsV8Sub11());
    auto doodads = MapData::ParseDoodadPlacements(MapDataFixtures::EmptyDoodadsV8Sub11());
    QVERIFY(units.Succeeded() && doodads.Succeeded());
    MapData::UnitPlacement unit; unit.rawcode = "u001"; unit.creationId = 1;
    unit.position = {10, 20, 30}; unit.scale = {1, 2, 3}; unit.owner = 4;
    units.value->units.push_back(unit);
    unit.rawcode = "i001"; unit.creationId = 2; units.value->units.push_back(unit);
    unit.rawcode = "zzzz"; unit.creationId = 3; units.value->units.push_back(unit);
    MapData::DoodadPlacement doodad; doodad.rawcode = "d001"; doodad.creationId = 4;
    doodad.position = {40, 50, 60}; doodad.scale = {3, 2, 1}; doodads.value->doodads.push_back(doodad);
    doodad.rawcode = "b001"; doodad.creationId = 5; doodads.value->doodads.push_back(doodad);
    auto unitBytes = units.value->Serialize(); auto doodadBytes = doodads.value->Serialize();
    QVERIFY(unitBytes.Succeeded() && doodadBytes.Succeeded());
    const std::vector<MapArchiveMember> members{{"war3mapUnits.doo", *unitBytes.value}, {"war3map.doo", *doodadBytes.value}};
    const auto document = MapDocument::FromMembers(members);
    const auto full = CaptureWorldPlacements(project, document);
    const auto unitCapture = CaptureWorldPlacements(project, document, MapData::PlacementFile::Units);
    const auto doodadCapture = CaptureWorldPlacements(project, document, MapData::PlacementFile::Doodads);
    QCOMPARE(full.requests.size(), std::size_t{4}); QCOMPARE(full.unresolved, std::size_t{1});
    QCOMPARE(unitCapture.requests.size(), std::size_t{2}); QCOMPARE(unitCapture.units, std::size_t{1});
    QCOMPARE(unitCapture.items, std::size_t{1}); QCOMPARE(unitCapture.unresolved, std::size_t{1});
    QCOMPARE(unitCapture.doodads + unitCapture.destructibles, std::size_t{0});
    QCOMPARE(doodadCapture.requests.size(), std::size_t{2}); QCOMPARE(doodadCapture.doodads, std::size_t{1});
    QCOMPARE(doodadCapture.destructibles, std::size_t{1}); QCOMPARE(doodadCapture.unresolved, std::size_t{0});
    QCOMPARE(doodadCapture.units + doodadCapture.items, std::size_t{0});
    const auto sameRequest = [](const Render::ModelInstanceRequest& left, const Render::ModelInstanceRequest& right) {
        return left.modelPath == right.modelPath && left.replaceableTexture == right.replaceableTexture
            && left.position == right.position && left.angle == right.angle && left.scale == right.scale
            && left.tint == right.tint && left.teamColor == right.teamColor && left.previewGhost == right.previewGhost
            && left.animationProperties == right.animationProperties && left.selection.has_value() == right.selection.has_value();
    };
    for (std::size_t index = 0; index < doodadCapture.requests.size(); ++index) {
        QVERIFY(sameRequest(full.requests[index], doodadCapture.requests[index]));
        QVERIFY(full.ids[index] == doodadCapture.ids[index]);
    }
    for (std::size_t index = 0; index < unitCapture.requests.size(); ++index) {
        QVERIFY(sameRequest(full.requests[index + 2], unitCapture.requests[index]));
        QVERIFY(full.ids[index + 2] == unitCapture.ids[index]);
    }
    QVERIFY(unitCapture.presentations.empty()); QCOMPARE(doodadCapture.presentations.size(), std::size_t{1});
    QCOMPARE(doodadCapture.presentations[0].requestIndex, std::uint32_t{1});
    QVERIFY(doodadCapture.presentations[0].owner == doodadCapture.ids[1]);
    QVERIFY(doodadCapture.presentations[0].owner == full.presentations[0].owner);
    QCOMPARE(doodadCapture.presentations[0].fields->modelPath.value, full.presentations[0].fields->modelPath.value);

    // 只捕获被改成员时，仍由真实回执和两个成员SHA证明另一个成员可以保留。
    MapData::MapDataEditSession edits(document);
    const Services::WorldInstanceBinding binding{7, 0, edits.SessionIdentity(), 0, "capture-scope-source"};
    const auto registry = Services::WorldInstanceRegistry::Create(document, binding, full.requests, full.ids);
    QVERIFY(registry.Succeeded());
    unit.rawcode = "u001"; unit.position = {70, 80, 90};
    QVERIFY(MapData::AddUnitPlacement(edits, unit).Succeeded());
    const auto changed = edits.Snapshot();
    const auto changedUnits = CaptureWorldPlacements(project, changed, MapData::PlacementFile::Units);
    auto requests = doodadCapture.requests; requests.insert(requests.end(), changedUnits.requests.begin(), changedUnits.requests.end());
    auto ids = doodadCapture.ids; ids.insert(ids.end(), changedUnits.ids.begin(), changedUnits.ids.end());
    const auto prepared = (*registry.value)->Prepare(edits.LastAppliedEdit(), binding, edits.Revision(), changed, requests, ids);
    QVERIFY(prepared.Succeeded()); QCOMPARE(prepared.value->delta.additions.size(), std::size_t{1});
    QVERIFY(prepared.value->delta.removals.empty());
    auto tamperedDoodads = *changed.Doodads(); tamperedDoodads.doodads[0].position[2] += 1;
    const auto tamperedBytes = tamperedDoodads.Serialize(); const auto changedUnitBytes = changed.Units()->Serialize();
    QVERIFY(tamperedBytes.Succeeded() && changedUnitBytes.Succeeded());
    const std::vector<MapArchiveMember> tamperedMembers{{"war3mapUnits.doo", *changedUnitBytes.value}, {"war3map.doo", *tamperedBytes.value}};
    const auto tampered = MapDocument::FromMembers(tamperedMembers);
    const auto rejected = (*registry.value)->Prepare(edits.LastAppliedEdit(), binding, edits.Revision(), tampered, requests, ids);
    QVERIFY(!rejected.Succeeded());
    QVERIFY(std::any_of(rejected.diagnostics.begin(), rejected.diagnostics.end(), [](const auto& diagnostic) {
        return diagnostic.code == "WORLD_INSTANCE_MEMBER_STALE";
    }));

    // capture没有暗藏跨物编版本缓存；目录刷新后必须取得实际新字段。
    QVERIFY(install(ObjectData::ObjectType::Unit, "[u001]\nfile=\"Changed.mdl\"\nusca=3\n"));
    const auto refreshed = CaptureWorldPlacements(project, document, MapData::PlacementFile::Units);
    QCOMPARE(refreshed.requests[0].modelPath, std::string("Changed.mdx"));
    QVERIFY(refreshed.requests[0].scale != unitCapture.requests[0].scale);
}

void WorldModuleTests::BrowserSelectionSynchronizesPlacementTool() {
    QTemporaryDir directory; QVERIFY(directory.isValid());
    const auto mapPath = directory.filePath(QStringLiteral("browser-placement.w3x"));
    QVERIFY(WriteVerifiedArchive(mapPath.toStdWString(), PlacementPreviewMembers()).Succeeded());
    auto archive = MapArchiveSession::Open(mapPath.toStdWString()); QVERIFY(archive.Succeeded());
    QtProject::EditorProject project; QVERIFY(InstallPlacementCatalog(project));
    project.sourceArchivePath_ = mapPath.toStdWString();
    project.archiveSession_ = std::make_unique<MapArchiveSession>(std::move(*archive.value));
    project.mapEdits_ = std::make_unique<MapData::MapDataEditSession>(MapDocument::FromArchive(*project.archiveSession_));
    const auto editableTerrain = TerrainBytes();
    QVERIFY(project.mapEdits_->AttachTerrain(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(editableTerrain.constData()), static_cast<std::size_t>(editableTerrain.size()))).Succeeded());
    project.ResetEditTracking(); QVERIFY(project.editTrackingReady_);
    BackendState state; activeBackend = &state;
    auto api = particleLayerBackend; EditorPresentationFixture<BackendState>::Enable(api, state.editor);
    WorldModule module(project, &api); auto* page = module.Widget(); state.viewport = module.Viewport();
    page->resize(1280, 800); page->show();
    auto* viewport = module.Viewport();
    auto* select = page->findChild<QPushButton*>("worldSelectTool");
    auto* place = page->findChild<QPushButton*>("worldPlaceToggle");
    auto* terrain = page->findChild<QPushButton*>("worldTerrainToggle");
    auto* rows = page->findChild<QListWidget*>("worldAssetRows");
    auto* assets = page->findChild<QDockWidget*>("worldAssetDock");
    QVERIFY(select && place && terrain && rows && assets);
    assets->show(); assets->raise(); // 切资源标签不等于提前切换为摆放工具。
    QTRY_VERIFY(viewport->RendererReady() && viewport->VertexCount() > 0);
    QListWidgetItem* sanctuary = nullptr;
    const auto findItem = [&] {
        for (int i = 0; i < rows->count(); ++i) if (rows->item(i)->data(Qt::UserRole).toString() == "hsan") {
            sanctuary = rows->item(i); return true;
        }
        return false;
    };
    QTRY_VERIFY(findItem());
    const auto choose = [&] {
        assets->raise();
        rows->scrollToItem(sanctuary);
        QTest::mouseClick(rows->viewport(), Qt::LeftButton, Qt::NoModifier, rows->visualItemRect(sanctuary).center());
    };
    QVERIFY(select->isChecked() && !place->isChecked() && !viewport->Placing());
    choose(); // 从选择工具直接选目录，不提前勾选摆放来绕开作者实际入口。
    QVERIFY(place->isChecked() && place->property("current").toBool() && !select->isChecked());
    QVERIFY(viewport->Placing() && !viewport->BrushMode());
    auto* preview = module.findChild<WorldPlacementPreview*>(); QVERIFY(preview);
    viewport->PlacementHovered(0, 0, 0, false);
    QTRY_VERIFY(preview->ModelSubmitted() && state.layerId != 0);
    QVERIFY(state.layerPose && state.layerPose->dynamicMaterialStates && !state.layerPose->materialStates.empty());
    QTRY_VERIFY(preview->CurrentFrame() && !preview->CurrentFrame()->particleVertices.empty());
    QTest::mouseClick(select, Qt::LeftButton);
    QVERIFY(select->isChecked() && !place->isChecked() && !viewport->Placing());
    QVERIFY(!preview->PlaybackActive());
    QTRY_VERIFY(!preview->Busy() && state.layerId == 0);
    QVERIFY(terrain->isEnabled());
    QTest::mouseClick(terrain, Qt::LeftButton);
    QVERIFY(terrain->isChecked());
    choose();
    QVERIFY(place->isChecked() && !terrain->isChecked() && !select->isChecked());
    QVERIFY(viewport->Placing() && !viewport->BrushMode());
    viewport->PlacementHovered(0, 0, 0, false);
    QTRY_VERIFY(preview->ModelSubmitted());
    QTest::keyClick(viewport, Qt::Key_Escape);
    QVERIFY(select->isChecked() && !place->isChecked() && !preview->PlaybackActive());
    QTRY_VERIFY(!preview->Busy() && state.layerId == 0);
    QVERIFY(!project.mapEdits_->IsDirty());
}

void WorldModuleTests::HiddenOutlinerKeepsTypedEditsAndRefreshesOnce() {
    QTemporaryDir directory; QVERIFY(directory.isValid());
    auto members = PlacementPreviewMembers();
    auto units = MapData::ParseUnitPlacements(MapDataFixtures::EmptyUnitsV8Sub11()); QVERIFY(units.Succeeded());
    MapData::UnitPlacement initial; initial.rawcode = "hsan"; initial.flags = 2;
    initial.creationId = 7; initial.scale = {1, 1, 1}; initial.position = {-32, 0, 0};
    units.value->units.push_back(initial);
    const auto unitBytes = units.value->Serialize(); QVERIFY(unitBytes.Succeeded());
    for (auto& member : members) if (member.name == "war3mapUnits.doo") member.data = *unitBytes.value;
    const auto mapPath = directory.filePath(QStringLiteral("hidden-outliner.w3x"));
    QVERIFY(WriteVerifiedArchive(mapPath.toStdWString(), members).Succeeded());
    auto archive = MapArchiveSession::Open(mapPath.toStdWString()); QVERIFY(archive.Succeeded());
    QtProject::EditorProject project; QVERIFY(InstallPlacementCatalog(project));
    project.sourceArchivePath_ = mapPath.toStdWString();
    project.archiveSession_ = std::make_unique<MapArchiveSession>(std::move(*archive.value));
    project.mapDocument_ = std::make_unique<MapDocument>(MapDocument::FromArchive(*project.archiveSession_));
    project.mapEdits_ = std::make_unique<MapData::MapDataEditSession>(*project.mapDocument_);
    const auto terrainBytes = TerrainBytes();
    QVERIFY(project.mapEdits_->AttachTerrain(std::as_bytes(std::span(terrainBytes.constData(),
        static_cast<std::size_t>(terrainBytes.size())))).Succeeded());
    project.ResetEditTracking(); QVERIFY(project.editTrackingReady_);
    QVERIFY(QtProject::EnsureProjectObjectCatalog(project).Succeeded());
    const auto* capturedType = project.standardCatalog_->Find(ObjectData::ObjectType::Unit, "hsan");
    QVERIFY(capturedType && capturedType->nativeOverridesSupported);
    BackendState state; activeBackend = &state;
    WorldModule module(project, &modelBackend); auto* page = module.Widget(); state.viewport = module.Viewport();
    page->resize(1280, 800); page->show();
    auto* viewport = module.Viewport();
    auto* select = page->findChild<QPushButton*>("worldSelectTool");
    auto* tree = page->findChild<QTreeView*>("worldOutliner");
    auto* model = tree ? dynamic_cast<WorldOutlinerModel*>(tree->model()) : nullptr;
    auto* search = page->findChild<QLineEdit*>("worldOutlinerFilter");
    auto* category = page->findChild<QComboBox*>("worldOutlinerCategory");
    auto* toolDock = page->findChild<QDockWidget*>("worldToolDock");
    auto* assets = page->findChild<QDockWidget*>("worldAssetDock");
    auto* rows = page->findChild<QListWidget*>("worldAssetRows");
    QVERIFY(select && tree && model && search && category && toolDock && assets && rows);
    QTRY_VERIFY(viewport->RendererReady() && viewport->VertexCount() > 0);
    const Services::WorldObjectId initialId = MapData::MakePlacementId(MapData::PlacementFile::Units, 0, initial);
    QTRY_VERIFY(FindWorldObject(*model, initialId).isValid());
    category->setCurrentIndex(category->findData(static_cast<int>(Services::WorldCategory::Unit)));
    search->setText(QStringLiteral("hsan"));
    QTRY_VERIFY(FindWorldObject(*model, initialId).isValid());
    assets->show(); assets->raise();
    QListWidgetItem* sanctuary = nullptr;
    const auto findAsset = [&] {
        for (int i = 0; i < rows->count(); ++i) if (rows->item(i)->data(Qt::UserRole).toString() == "hsan") {
            sanctuary = rows->item(i); return true;
        }
        return false;
    };
    QTRY_VERIFY(findAsset());
    rows->scrollToItem(sanctuary);
    QTest::mouseClick(rows->viewport(), Qt::LeftButton, Qt::NoModifier, rows->visualItemRect(sanctuary).center());
    QVERIFY(viewport->Placing()); QVERIFY(!tree->isVisible());
    QSignalSpy resets(model, &QAbstractItemModel::modelReset);
    viewport->PlaceRequested(0, 0, false);
    QTRY_COMPARE(project.mapDataUndoStack_->count(), 1);
    QCOMPARE(project.mapEdits_->Snapshot().Units()->units.size(), std::size_t{2});
    project.mapDataUndoStack_->undo();
    QCOMPARE(project.mapEdits_->Snapshot().Units()->units.size(), std::size_t{1});
    project.mapDataUndoStack_->redo();
    QCOMPARE(project.mapEdits_->Snapshot().Units()->units.size(), std::size_t{2});
    // 隐藏时只保留最终筛选，不查询中间目录；撤销/重做仍由同一地图事务执行。
    search->setText(QStringLiteral("神秘"));
    QCOMPARE(resets.size(), 0);
    QVERIFY(model->Version() && model->Version()->document.revision != project.mapEdits_->Revision());
    QTest::mouseClick(select, Qt::LeftButton); toolDock->show(); toolDock->raise();
    QTRY_VERIFY(tree->isVisible() && model->Version()
        && model->Version()->document.revision == project.mapEdits_->Revision());
    QCOMPARE(resets.size(), 1);
    QCOMPARE(search->text(), QStringLiteral("神秘"));
    QCOMPARE(category->currentData().toInt(), static_cast<int>(Services::WorldCategory::Unit));
    const auto placedDocument = project.mapEdits_->Snapshot();
    const auto placed = placedDocument.Units()->units.back();
    const Services::WorldObjectId placedId = MapData::MakePlacementId(MapData::PlacementFile::Units, 1, placed);
    QTRY_VERIFY(FindWorldObject(*model, placedId).isValid());
    tree->expandAll(); tree->scrollTo(model->Find(placedId)); tree->doItemsLayout();
    QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier, tree->visualRect(model->Find(placedId)).center());
    QTRY_COMPARE(tree->selectionModel()->selectedRows().size(), 1);
    auto* x = page->findChild<QDoubleSpinBox*>("worldFieldX"); QVERIFY(x);
    QTRY_VERIFY(x->isEnabled());
    // 隐藏目录后连续两次编辑可用，证明第二次使用的是已同步的新 typed 修订。
    toolDock->hide(); QVERIFY(!tree->isVisible()); resets.clear();
    viewport->NudgeRequested(1, 0); viewport->NudgeRequested(1, 0);
    QCOMPARE(project.mapEdits_->Snapshot().Units()->units[1].position[0], placed.position[0] + 2.0f);
    project.mapDataUndoStack_->undo(); project.mapDataUndoStack_->undo();
    QCOMPARE(project.mapEdits_->Snapshot().Units()->units[1].position[0], placed.position[0]);
    project.mapDataUndoStack_->redo(); project.mapDataUndoStack_->redo();
    QCOMPARE(project.mapEdits_->Snapshot().Units()->units[1].position[0], placed.position[0] + 2.0f);
    QCOMPARE(resets.size(), 0);
    toolDock->show(); toolDock->raise();
    QTRY_VERIFY(tree->isVisible() && model->Version()
        && model->Version()->document.revision == project.mapEdits_->Revision());
    QCOMPARE(resets.size(), 1);
    QTRY_COMPARE(tree->selectionModel()->selectedRows().size(), 1);
    const auto* selected = model->RowAt(tree->selectionModel()->selectedRows().front());
    QVERIFY(selected && selected->id == std::optional(placedId));
    QCOMPARE(search->text(), QStringLiteral("神秘"));
    // 没有地图或过滤变化时重复显示不重建，也不丢失当前选择。
    toolDock->hide(); toolDock->show(); toolDock->raise(); QApplication::processEvents();
    QCOMPARE(resets.size(), 1);
    QCOMPARE(tree->selectionModel()->selectedRows().size(), 1);
}

void WorldModuleTests::PlacementParticleClock_data() {
    QTest::addColumn<bool>("pureParticle");
    QTest::newRow("mesh-and-particles") << false;
    QTest::newRow("particles-without-mesh") << true;
}
void WorldModuleTests::PlacementParticleClock() {
    QFETCH(bool, pureParticle);
    QTemporaryDir directory; QVERIFY(directory.isValid());
    const auto mapPath = directory.filePath(QStringLiteral("particle-preview.w3x"));
    QVERIFY(WriteVerifiedArchive(mapPath.toStdWString(), PlacementPreviewMembers(pureParticle)).Succeeded());
    auto archive = MapArchiveSession::Open(mapPath.toStdWString()); QVERIFY(archive.Succeeded());
    QtProject::EditorProject project; project.sourceArchivePath_ = mapPath.toStdWString();
    project.archiveSession_ = std::make_unique<MapArchiveSession>(std::move(*archive.value));
    const auto trackedPlacement = AttachTrackedResourcePlacement(project); QVERIFY(trackedPlacement);
    const auto resourceBaseline = project.editRevision_.Baseline(EditSaveTarget::Map); QVERIFY(resourceBaseline);
    QVERIFY(resourceBaseline->scopes.contains("source"));
    QCOMPARE(resourceBaseline->scopes.at("source"), "archive:" + project.archiveSession_->SourceSha256());
    QVERIFY(resourceBaseline->scopes.contains("map/war3mapUnits.doo"));
    auto sources = CaptureWorldTerrainSources(project); QVERIFY(sources.Succeeded());
    BackendState state; activeBackend = &state;
    WorldViewport viewport(nullptr, &particleLayerBackend); state.viewport = &viewport;
    viewport.resize(320, 240); QVERIFY(BindTerrain(viewport, TestTerrain())); viewport.show();
    QTRY_VERIFY(viewport.RendererReady() && state.frames > 0);
    WorldPlacementPreview preview(viewport, &viewport);
    QSignalSpy messages(&preview, &WorldPlacementPreview::Message);
    QString previewMessage;
    connect(&preview, &WorldPlacementPreview::Message, &preview, [&](const QString& text) {
        previewMessage = text; qInfo().noquote() << "Placement preview:" << text;
    });
    Render::ModelInstanceRequest request; request.modelPath = "Synthetic\\Arcane.mdx";
    auto transform = Render::ModelInstanceTransform(request);
    preview.Show({request}, *sources.value, 12, [] { return true; }, {transform});
    QTRY_VERIFY2(preview.ModelSubmitted() && preview.CurrentFrame(), qPrintable(previewMessage));
    QTRY_VERIFY(preview.PreloadedAssets(*sources.value));
    const auto resourceSnapshot = preview.PreloadedAssets(*sources.value);
    const auto revisionBeforeMove = project.editRevision_.Revision();
    const MapData::PlacementMove resourceMove{*trackedPlacement, {32, 0, 0}, {}};
    QVERIFY(MapData::MovePlacements(*project.mapEdits_, std::span(&resourceMove, 1)).Succeeded());
    QVERIFY(project.ObserveMapDataEdit("移动真实夹具单位"));
    QVERIFY(project.editRevision_.Revision() > revisionBeforeMove);
    const auto afterMove = CaptureWorldTerrainSources(project); QVERIFY(afterMove.Succeeded());
    QVERIFY(SameWorldTerrainSources(*sources.value, *afterMove.value));
    QCOMPARE(preview.PreloadedAssets(*afterMove.value), resourceSnapshot);
    const auto scene = preview.CurrentScene(); const auto first = preview.CurrentFrame();
    QVERIFY(scene && first->particleCheckpoint && scene->particleInstanceCount == 1);
    QVERIFY(state.layerPose && state.layerPose->dynamicMaterialStates == !pureParticle);
    if (!pureParticle) {
        QVERIFY(state.layerPose->dynamicMaterialAlphas && !state.layerPose->materialStates.empty());
        QCOMPARE(state.layerPose->layerAlphas, first->layerAlphas);
    }
    QCOMPARE(first->states.size(), std::size_t{1});
    QCOMPARE(first->states[0].sequenceIndex, std::uint32_t{0}); // 共享夹具按时间顺序，Stand 为第 0 个动作
    QCOMPARE(first->states[0].elapsedMs - first->states[0].globalMs, std::uint64_t{500});
    if (pureParticle) QVERIFY(first->instances.empty()); else QVERIFY(!first->instances.empty());
    QVERIFY(preview.PlaybackActive());
    QElapsedTimer actualElapsed; actualElapsed.start();
    const auto previousGlobal = first->states[0].globalMs;
    QTRY_VERIFY(preview.CurrentFrame()->states[0].globalMs >= previousGlobal + 100
        && state.layerParticleVertices > 0);
    const auto running = preview.CurrentFrame();
    QVERIFY(running->states[0].globalMs - previousGlobal <= static_cast<std::uint64_t>(actualElapsed.elapsed()) + 150);
    QVERIFY(running->particleLinks[0].resetReason.empty());
    QCOMPARE(preview.CurrentScene(), scene); // 鼠标不动：同一资产和实例继续求值，不能靠鼠标伪造时间。
    QCOMPARE(state.layerSubmits, 1);
    QVERIFY(state.layerUpdates >= 1);
    const auto beforeCamera = running->states[0].globalMs;
    auto camera = viewport.SaveCamera(); camera["yaw"] = camera["yaw"].toDouble() + 0.2;
    QVERIFY(viewport.RestoreCamera(camera));
    QTRY_VERIFY(preview.CurrentFrame()->states[0].globalMs > beforeCamera);
    QCOMPARE(preview.CurrentScene(), scene);
    QVERIFY(preview.CurrentFrame()->particleLinks[0].resetReason.empty());
    for (int i = 1; i <= 20; ++i) { transform[3] = static_cast<float>(i); preview.SetTransforms({transform}); }
    QTRY_VERIFY(preview.ModelSubmitted() && preview.CurrentFrame()->states[0].transform[3] == 20);
    QVERIFY(preview.CurrentFrame()->particleCheckpoint);
    viewport.hide();
    QVERIFY(!preview.PlaybackActive());
    QTRY_VERIFY(!preview.Busy());
    QCOMPARE(state.layerId, std::uint64_t{0});
    const auto hiddenGlobal = preview.CurrentFrame()->states[0].globalMs;
    QTest::qWait(80);
    QCOMPARE(preview.CurrentFrame()->states[0].globalMs, hiddenGlobal);
    viewport.show();
    QTRY_VERIFY(preview.ModelSubmitted() && preview.PlaybackActive());
    QTRY_VERIFY(preview.CurrentFrame()->states[0].globalMs > hiddenGlobal);
    QCOMPARE(preview.CurrentScene(), scene);
    transform[3] = 40; preview.SetTransforms({transform}); preview.CancelRequest();
    QVERIFY(!preview.PlaybackActive());
    QTRY_VERIFY(!preview.Busy());
    QCOMPARE(state.layerId, std::uint64_t{0});
    QVERIFY(!preview.ModelSubmitted());
    const auto resident = preview.CurrentFrame(); QVERIFY(resident && resident->particleCheckpoint);
    transform = resident->states[0].transform;
    const auto submitsBeforeRestore = state.layerSubmits;
    preview.Show({request}, *sources.value, 12, [] { return true; }, {transform});
    // 热选相同落点直接提交完整旧帧，不等待worker，也不改检查点/版本来伪造一个新姿势。
    QVERIFY(preview.ModelSubmitted());
    QCOMPARE(preview.CurrentFrame(), resident);
    QCOMPARE(preview.CurrentFrame()->particleCheckpoint, resident->particleCheckpoint);
    QCOMPARE(state.layerContent, resident->contentVersion);
    QCOMPARE(state.layerSubmits, submitsBeforeRestore + 1);
    QTRY_VERIFY(preview.CurrentFrame()->states[0].globalMs > resident->states[0].globalMs);
    QVERIFY(preview.CurrentFrame()->particleLinks[0].resetReason.empty());
    preview.CancelRequest(); QTRY_VERIFY(!preview.Busy());
    camera["yaw"] = camera["yaw"].toDouble() + 0.3;
    QVERIFY(viewport.RestoreCamera(camera));
    preview.Show({request}, *sources.value, 12, [] { return true; }, {transform});
    QVERIFY(!preview.ModelSubmitted()); // 镜头不同：旧朝向的粒子几何不能通过热缓存快路径。
    QTRY_VERIFY(preview.ModelSubmitted());
    preview.CancelRequest(); QTRY_VERIFY(!preview.Busy());
    QCOMPARE(state.layerId, std::uint64_t{0});
    for (const auto& entry : messages) {
        const auto text = entry.front().toString();
        QVERIFY2(!text.contains("PARTICLE_GLOBAL_REPLAY_UNSUPPORTED") && !text.contains("CHECKPOINT"), qPrintable(text));
    }
}

// 显式运行的本机画面验收；常规 CTest 不读本机游戏，也不占用用户窗口。
// 仅用独立地图副本、游戏根和输出目录；窗口移出桌面可见范围并禁止抢焦点。
void WorldModuleTests::NativePlacementFrames() {
    const auto output = qEnvironmentVariable("FORGE_H1_CAPTURE_DIR");
    if (output.isEmpty()) return;
    const auto gamePath = qEnvironmentVariable("FORGE_H1_GAME_ROOT");
    QVERIFY(!gamePath.isEmpty() && QDir().mkpath(output));
    QTemporaryDir synthetic;
    QVERIFY(synthetic.isValid());
    auto units = MapData::ParseUnitPlacements(MapDataFixtures::EmptyUnitsV8Sub11());
    auto doodads = MapData::ParseDoodadPlacements(MapDataFixtures::EmptyDoodadsV8Sub11());
    QVERIFY(units.Succeeded() && doodads.Succeeded());
    MapData::UnitPlacement footman;
    footman.rawcode = "hfoo"; footman.scale = {1, 1, 1}; footman.owner = 0;
    footman.creationId = 1; footman.position = {-48, 0, 0};
    units.value->units.push_back(footman);
    footman.creationId = 2; footman.position = {48, 0, 0};
    units.value->units.push_back(footman);
    MapData::DoodadPlacement tree;
    tree.rawcode = "LTlt"; tree.scale = {1, 1, 1}; tree.flags = 2;
    tree.creationId = 3; tree.position = {-120, -60, 0};
    doodads.value->doodads.push_back(tree);
    auto unitBytes = units.value->Serialize();
    auto doodadBytes = doodads.value->Serialize();
    QVERIFY(unitBytes.Succeeded() && doodadBytes.Succeeded());
    const auto terrain = TerrainBytes();
    const auto asBytes = [](const QByteArray& value) {
        return std::vector<std::byte>(reinterpret_cast<const std::byte*>(value.constData()),
            reinterpret_cast<const std::byte*>(value.constData() + value.size()));
    };
    const auto mapPath = synthetic.filePath(QStringLiteral("h1-synthetic.w3x"));
    const std::vector<MapArchiveMember> members{{"war3map.w3i", MapDataFixtures::FullMapSettingsV25().bytes},
        {"war3map.w3e", asBytes(terrain)}, {"war3mapUnits.doo", *unitBytes.value},
        {"war3map.doo", *doodadBytes.value}};
    QVERIFY(WriteVerifiedArchive(mapPath.toStdWString(), members).Succeeded());
    auto archive = MapArchiveSession::Open(mapPath.toStdWString());
    QVERIFY2(archive.Succeeded(), "independent map copy could not be opened");
    auto game = Game::GameDataSource::Mount(gamePath.toStdWString());
    QVERIFY2(game.Succeeded(), "game assets could not be mounted");
    auto catalog = ObjectData::StandardObjectCatalog::Load(*game.value);
    QVERIFY2(catalog.Succeeded(), "standard model fields could not be loaded");
    QtProject::EditorProject project;
    project.sourceArchivePath_ = mapPath.toStdWString();
    project.war3GameRoot_ = gamePath.toStdWString();
    project.archiveSession_ = std::make_unique<MapArchiveSession>(std::move(*archive.value));
    project.standardCatalog_ = std::make_unique<ObjectData::StandardObjectCatalog>(std::move(*catalog.value));
    project.mapEdits_ = std::make_unique<MapData::MapDataEditSession>(MapDocument::FromArchive(*project.archiveSession_));
    project.ResetEditTracking();
    QVERIFY(project.editTrackingReady_);
    WorldModule module(project);
    auto* page = module.Widget();
    page->resize(1440, 900);
    page->setWindowFlag(Qt::Tool);
    page->setWindowFlag(Qt::WindowDoesNotAcceptFocus);
    page->setAttribute(Qt::WA_ShowWithoutActivating);
    page->move(-3000, -3000); // 自有原生窗口留在可见桌面之外，不抢用户焦点。
    page->show();
    auto* viewport = module.Viewport();
    QTRY_VERIFY_WITH_TIMEOUT(viewport->RendererReady() && viewport->VertexCount() > 0
        && viewport->HasAnimationScene(), 60000);
    auto camera = viewport->SaveCamera(); camera["distance"] = 600;
    QVERIFY(viewport->RestoreCamera(camera));
    const auto revision = viewport->DocumentRevision();
    const auto save = [&](const QString& name, std::size_t expectedStates, std::uint64_t minimumVersion,
        std::size_t previewInstances = 0) {
        for (int attempt = 0; attempt < 15; ++attempt) {
            auto frame = viewport->CaptureCompletedFrame(revision);
            if (frame.Succeeded() && frame.value) {
                const auto& image = *frame.value;
                if (!image.animation || image.animation->states.size() != expectedStates ||
                    image.animation->contentVersion < minimumVersion) {
                    QTest::qWait(300);
                    continue;
                }
                if (previewInstances && (image.modelLayerFrames.size() != 1 || image.modelLayerFrames.front()->states.size() != previewInstances)) {
                    QTest::qWait(300); continue;
                }
                const auto width = static_cast<int>(image.view.physicalWidth);
                const auto height = static_cast<int>(image.view.physicalHeight);
                if (width <= 0 || height <= 0 || image.rgba.size() != static_cast<std::size_t>(width) * height * 4)
                    return false;
                const QImage pixels(reinterpret_cast<const uchar*>(image.rgba.data()), width, height,
                    width * 4, QImage::Format_RGBA8888);
                return pixels.save(QDir(output).filePath(name));
            }
            QTest::qWait(300);
        }
        return false;
    };
    auto* place = page->findChild<QPushButton*>("worldPlaceToggle");
    auto* objects = page->findChild<WorldAssetBrowser*>("worldAssetBrowser");
    QVERIFY(place && objects);
    const auto baselineInstances = viewport->AnimationTarget()->states.size();
    place->setChecked(true);
    auto* rows = objects->findChild<QListWidget*>("worldAssetRows"); QVERIFY(rows);
    QListWidgetItem* footmanItem = nullptr;
    const auto findFootman = [&] {
        for (int i = 0; i < rows->count(); ++i) if (rows->item(i)->data(Qt::UserRole).toString() == QStringLiteral("hfoo")) {
            footmanItem = rows->item(i); return true;
        }
        return false;
    };
    QTRY_VERIFY_WITH_TIMEOUT(findFootman(), 20000);
    rows->setCurrentItem(footmanItem); rows->scrollToItem(footmanItem);
    QTest::mouseClick(rows->viewport(), Qt::LeftButton, Qt::NoModifier, rows->visualItemRect(footmanItem).center());
    QTest::qWait(6000);
    const auto afterChoose = viewport->AnimationTarget();
    const auto* modelStatus = page->findChild<QLabel*>("worldModelStatus");
    qInfo().noquote() << "H1 preview state" << baselineInstances
        << (afterChoose ? afterChoose->states.size() : 0)
        << "viewport" << viewport->StatusText()
        << "model" << (modelStatus ? modelStatus->text() : QStringLiteral("missing"))
        << "detail" << (modelStatus ? modelStatus->toolTip() : QStringLiteral("missing"));
    QVERIFY(afterChoose && afterChoose->states.size() == baselineInstances);
    const auto previewReady = [&](std::size_t count) {
        const auto captured = viewport->CaptureCompletedFrame(revision);
        if (!captured.Succeeded()) return false;
        const auto& layers = captured.value->modelLayerFrames;
        if (!count) return layers.empty();
        if (layers.size() != 1 || layers.front()->states.size() != count) return false;
        return std::all_of(layers.front()->states.begin(), layers.front()->states.end(), [](const auto& state) {
            return state.transform[11] > -64 && state.transform[11] < 64;
        });
    };
    const auto center = QPointF(viewport->width() / 2.0, viewport->height() / 2.0);
    Mouse(*viewport, QEvent::MouseMove, center, Qt::NoButton, Qt::NoButton);
    QTRY_VERIFY_WITH_TIMEOUT(viewport->HoverCoordinate().has_value(), 15000);
    const auto placeHover = *viewport->HoverCoordinate();
    viewport->PlacementHovered(placeHover.position[0], placeHover.position[1], placeHover.position[2], false);
    QTRY_VERIFY_WITH_TIMEOUT(previewReady(1), 15000);
    QVERIFY2(save(QStringLiteral("placement-ghost.png"), baselineInstances,
        viewport->AnimationTarget()->contentVersion, 1), "placement ghost completed frame unavailable");
    viewport->PlacingCancelled();
    QTRY_VERIFY_WITH_TIMEOUT(viewport->AnimationTarget()
        && viewport->AnimationTarget()->states.size() == baselineInstances, 20000);
    QTRY_VERIFY_WITH_TIMEOUT(previewReady(0), 15000);
    int first = -1;
    for (int index = 1; index < 3 && first < 0; ++index) {
        viewport->InstanceClicked(index, 0);
        if (viewport->SelectedInstance()) first = index;
    }
    QVERIFY(first >= 0);
    QVERIFY(viewport->SelectedInstance());
    const auto beforeRotationVersion = viewport->AnimationTarget()->contentVersion;
    viewport->RotateStarted(0, 0);
    viewport->RotateDragged(256, 128);
    QTRY_VERIFY_WITH_TIMEOUT(viewport->AnimationTarget()
        && viewport->AnimationTarget()->contentVersion > beforeRotationVersion, 15000);
    QVERIFY2(save(QStringLiteral("rotation-draft.png"), baselineInstances,
        viewport->AnimationTarget()->contentVersion), "rotation completed frame unavailable");
    viewport->RotateCancelled();
    int second = -1;
    for (int index = 1; index < 3 && second < 0; ++index) {
        if (index == first) continue;
        viewport->InstanceClicked(index, static_cast<int>(Qt::ShiftModifier));
        if (viewport->SelectedInstance() && *viewport->SelectedInstance() != static_cast<std::uint32_t>(first))
            second = index;
    }
    QVERIFY(second >= 0);
    QTest::keyClick(viewport, Qt::Key_C, Qt::ControlModifier);
    QTest::keyClick(viewport, Qt::Key_V, Qt::ControlModifier);
    QVERIFY(viewport->Placing());
    QCOMPARE(viewport->AnimationTarget()->states.size(), baselineInstances);
    // 让组预览与原单位错开，截图中能同时辨认两份半透明实例。
    viewport->PlacementHovered(0, -40, 0, false);
    QTRY_VERIFY_WITH_TIMEOUT(previewReady(2), 15000);
    QVERIFY2(save(QStringLiteral("group-paste-ghost.png"), baselineInstances,
        viewport->AnimationTarget()->contentVersion, 2), "group paste completed frame unavailable");
    QVERIFY(!project.mapEdits_->IsDirty()); // 三张图都只是预览，尚未提交地图修改。
    viewport->PlacingCancelled();
    QTRY_VERIFY_WITH_TIMEOUT(previewReady(0), 15000);
}

// 显式运行（需本机游戏根）：雾只由“使用地形迷雾”启用，样式 0 为线性；
// 地图设置的应用/撤销/重做只重算环境，不重新读取地形。
void WorldModuleTests::TerrainFogFollowsMapSettingsFlag() {
    const auto gamePath = qEnvironmentVariable("FORGE_WORLD_GAME_ROOT");
    if (gamePath.isEmpty()) QSKIP("需要 FORGE_WORLD_GAME_ROOT 指向本机游戏根目录");
    QTemporaryDir synthetic;
    QVERIFY(synthetic.isValid());
    auto settings = MapData::MapSettings::Parse(MapDataFixtures::FullMapSettingsV25().bytes);
    QVERIFY(settings.Succeeded() && settings.value);
    settings.value->flags &= ~MapData::kMapFlagTerrainFog;
    settings.value->environment.fogStyle = 0;
    settings.value->environment.fogStart = 1000;
    settings.value->environment.fogEnd = 5000;
    const auto settingsBytes = settings.value->Serialize();
    QVERIFY(settingsBytes.Succeeded() && settingsBytes.value);
    const auto terrain = TerrainBytes();
    const std::vector<MapArchiveMember> members{{"war3map.w3i", *settingsBytes.value},
        {"war3map.w3e", std::vector<std::byte>(reinterpret_cast<const std::byte*>(terrain.constData()),
            reinterpret_cast<const std::byte*>(terrain.constData() + terrain.size()))}};
    const auto mapPath = synthetic.filePath(QStringLiteral("fog-synthetic.w3x"));
    QVERIFY(WriteVerifiedArchive(mapPath.toStdWString(), members).Succeeded());
    auto archive = MapArchiveSession::Open(mapPath.toStdWString());
    QVERIFY(archive.Succeeded());
    QtProject::EditorProject project;
    project.sourceArchivePath_ = mapPath.toStdWString();
    project.war3GameRoot_ = gamePath.toStdWString();
    project.archiveSession_ = std::make_unique<MapArchiveSession>(std::move(*archive.value));
    project.mapEdits_ = std::make_unique<MapData::MapDataEditSession>(MapDocument::FromArchive(*project.archiveSession_));
    project.ResetEditTracking();
    BackendState state; activeBackend = &state; environmentRecord = {};
    WorldModule module(project, &environmentBackend); auto* page = module.Widget(); state.viewport = module.Viewport();
    page->resize(640, 480); page->show();
    QTRY_VERIFY_WITH_TIMEOUT(environmentRecord.submits > 0, 60000);
    QCOMPARE(environmentRecord.fogMode, std::uint32_t{WAR3_RENDER_FOG_NONE});
    const auto terrainSubmits = state.terrainSubmits;
    const auto revision = state.revision;

    Modules::MapSettingsModule settingsPage(project);
    auto* fog = settingsPage.Widget()->findChild<QCheckBox*>("mapSettingsFlag_use_terrain_fog");
    QVERIFY(fog && !fog->isChecked());
    fog->setChecked(true);
    QVERIFY(settingsPage.ApplyDraft());
    QCOMPARE(environmentRecord.fogMode, std::uint32_t{WAR3_RENDER_FOG_LINEAR});
    QCOMPARE(environmentRecord.fogEnd, 5000.0f);
    project.mapDataUndoStack_->undo();
    QCOMPARE(environmentRecord.fogMode, std::uint32_t{WAR3_RENDER_FOG_NONE});
    project.mapDataUndoStack_->redo();
    QCOMPARE(environmentRecord.fogMode, std::uint32_t{WAR3_RENDER_FOG_LINEAR});
    QTest::qWait(200);
    QCOMPARE(state.terrainSubmits, terrainSubmits);
    QCOMPARE(state.revision, revision);
}

void WorldModuleTests::SourceShadowObservationKeepsUnknownAndCodecFailures_data() {
    QTest::addColumn<QString>("mode");
    for (const auto* mode : {"not-observed", "missing-unknown", "read-failure", "empty", "short", "trailing", "zero", "nonbinary"})
        QTest::newRow(mode) << QString::fromLatin1(mode);
}

void WorldModuleTests::SourceShadowObservationKeepsUnknownAndCodecFailures() {
    QFETCH(QString, mode);
    QTemporaryDir directory; QVERIFY(directory.isValid()); auto members = ShadowObservationMembers(); QVERIFY(!members.empty());
    std::vector<std::byte> raw(256);
    if (mode == "nonbinary") for (unsigned i = 0; i < 256; ++i) raw[i] = static_cast<std::byte>(i);
    if (mode == "empty") raw.clear();
    else if (mode == "short") raw.pop_back();
    else if (mode == "trailing") raw.push_back(std::byte{7});
    if (mode != "not-observed" && mode != "missing-unknown" && mode != "read-failure") members.push_back({"war3map.shd", raw});
    const auto path = std::filesystem::path(directory.filePath("shadow-observation.w3x").toStdWString());
    QVERIFY(WriteVerifiedArchive(path, members).Succeeded());
    auto source = MapArchiveSession::Open(path); QVERIFY(source.Succeeded());
    const auto sourceSha = source.value->SourceSha256();
    QtProject::EditorProject project; project.sourceArchivePath_ = path; project.projectRoot_ = path.parent_path();
    project.archiveSession_ = std::make_unique<MapArchiveSession>(std::move(*source.value)); project.LoadMapDocument(); project.ResetEditTracking();
    QVERIFY(project.mapEdits_ && project.mapEdits_->Terrain());
    // 游戏内同名合法SHD不能补入map缺失、读取失败或坏正文；材质仍正常使用现资源链。
    const auto game = std::filesystem::path(directory.filePath("game").toStdWString()); QVERIFY(std::filesystem::create_directory(game));
    const std::vector<MapArchiveMember> gameMembers{{"war3map.shd", std::vector<std::byte>(256, std::byte{255})}};
    QVERIFY(WriteVerifiedArchive(game / "war3.mpq", gameMembers).Succeeded());
    QVERIFY(WriteVerifiedArchive(game / "war3x.mpq", gameMembers).Succeeded()); project.war3GameRoot_ = game;
    const auto sources = CaptureWorldTerrainSources(project); QVERIFY(sources.Succeeded());
    WorldShadowInput input;
    input.context = {WorldShadowTerrainRelation::TerrainAndPathingUnchanged, project.mapEdits_->SessionIdentity(),
        project.mapEdits_->Revision(), project.mapEdits_->TerrainSourceRevision(), false};
    if (mode != "not-observed") input.read = project.ReadMapMember("war3map.shd");
    if (mode == "read-failure") {
        input.read = Result<std::vector<std::byte>>::Failure("OwnedShadowRead", "OWNED_SHD_READ_FAILURE", "原创读取故障，存在性未知");
        input.read->diagnostics.front().origin = OriginSpan{"war3map.shd", 7, 3, "owned-shadow-origin"};
    }
    const auto readDiagnostics = input.read ? input.read->diagnostics : std::vector<Diagnostic>{};
    std::shared_ptr<WorldTerrainCache> cache;
    const auto loaded = LoadWorldTerrain(members[1].data, sources, {}, input, &cache);
    QVERIFY2(loaded.Succeeded(), loaded.diagnostics.empty() ? "shadow load failed" : loaded.diagnostics.front().message.c_str());
    QVERIFY(cache && loaded.value->environment && loaded.value->ground);
    const auto& observed = loaded.value->shadowObservation; const auto& environment = *loaded.value->environment;
    QCOMPARE(observed.sourceArchiveSha, sourceSha);
    QCOMPARE(observed.context.sessionIdentity, project.mapEdits_->SessionIdentity());
    const auto summary = WorldShadowSummary(observed, &environment), details = WorldShadowDetails(observed, &environment);
    if (mode == "not-observed") {
        QVERIFY(observed.readState == WorldShadowReadState::NotObserved && observed.readDiagnostics.empty());
        QVERIFY(summary.contains(QStringLiteral("未观测")));
        QVERIFY(environment.shadowInput.state == Render::SceneShadowInputState::NotProvided && environment.shadow.empty());
    } else if (mode == "missing-unknown" || mode == "read-failure") {
        QVERIFY(input.read && !input.read->Succeeded() && observed.readState == WorldShadowReadState::ReadFailed);
        QVERIFY(SameShadowDiagnostics(observed.readDiagnostics, readDiagnostics));
        QVERIFY(summary.contains(QStringLiteral("存在性未确认")) && !summary.contains(QStringLiteral("无静态阴影")));
        QVERIFY(details.contains(QString::fromStdString(readDiagnostics.front().code)) && !observed.sourceBytes);
        QVERIFY(environment.shadow.empty() && environment.shadowInput.state == Render::SceneShadowInputState::NotProvided);
    } else {
        QVERIFY(observed.readState == WorldShadowReadState::Present && observed.sourceBytes == raw.size());
        QVERIFY(input.read && input.read->Succeeded() && *input.read->value == raw);
        const auto codec = MapData::ParseMapShadow(raw, 16, 16);
        QVERIFY(SameShadowDiagnostics(environment.shadowInput.diagnostics, codec.diagnostics));
        QCOMPARE(environment.shadowInput.sourceBytes, raw.size());
        if (mode == "empty") {
            QVERIFY(environment.shadowInput.state == Render::SceneShadowInputState::PresentEmpty);
            QVERIFY(summary.contains(QStringLiteral("为空")) && environment.shadow.empty());
        } else if (mode == "short" || mode == "trailing") {
            QVERIFY(environment.shadowInput.state == Render::SceneShadowInputState::Rejected && environment.shadow.empty());
            QCOMPARE(environment.shadowInput.diagnostics.front().code, std::string(mode == "short" ? "SHADOW_TRUNCATED" : "SHADOW_TRAILING_DATA"));
            QVERIFY(details.contains(QString::fromStdString(environment.shadowInput.diagnostics.front().code)));
        } else {
            QVERIFY(codec.Succeeded() && environment.shadowInput.state == Render::SceneShadowInputState::Parsed);
            QCOMPARE(environment.shadow.size(), raw.size());
            for (std::size_t i = 0; i < raw.size(); ++i) QCOMPARE(environment.shadow[i], raw[i] == std::byte{0} ? std::uint8_t{0} : std::uint8_t{255});
        }
    }
    QCOMPARE(cache->shadowObservation.sourceArchiveSha, observed.sourceArchiveSha);
    QVERIFY(SameShadowDiagnostics(cache->shadowObservation.readDiagnostics, observed.readDiagnostics));
    const auto globalRows = project.diagnosticsModel_->rowCount(); const auto notesCount = cache->notes.size();
    input.read.reset(); // 重建只借cache里的事实及environment，原Result/raw已释放。
    const auto repeated = RebuildWorldTerrain(*project.mapEdits_->Terrain(), *cache, {});
    QVERIFY(repeated.Succeeded() && repeated.value->environment == loaded.value->environment);
    QVERIFY(SameShadowDiagnostics(repeated.value->shadowObservation.readDiagnostics, readDiagnostics));
    for (unsigned i = 0; i < 3; ++i) {
        QCOMPARE(WorldShadowSummary(repeated.value->shadowObservation, repeated.value->environment.get()), summary);
        QCOMPARE(WorldShadowDetails(repeated.value->shadowObservation, repeated.value->environment.get()), details);
    }
    QCOMPARE(project.diagnosticsModel_->rowCount(), globalRows); QCOMPARE(cache->notes.size(), notesCount);
    QCOMPARE(*Sha256File(path).value, sourceSha); QVERIFY(!project.mapEdits_->IsDirty());
}

void WorldModuleTests::SourceShadowObservationSurvivesRebuildWithoutRawOrRepeatedDiagnostics() {
    QTemporaryDir directory; QVERIFY(directory.isValid()); auto members = ShadowObservationMembers(); QVERIFY(!members.empty());
    const std::vector<std::byte> originalShadow(256, std::byte{7}); members.push_back({"war3map.shd", originalShadow});
    const auto path = std::filesystem::path(directory.filePath("shadow-rebuild.w3x").toStdWString());
    QVERIFY(WriteVerifiedArchive(path, members).Succeeded()); auto source = MapArchiveSession::Open(path); QVERIFY(source.Succeeded());
    const auto sourceSha = source.value->SourceSha256();
    QtProject::EditorProject project; project.sourceArchivePath_ = path; project.projectRoot_ = path.parent_path();
    project.archiveSession_ = std::make_unique<MapArchiveSession>(std::move(*source.value)); project.LoadMapDocument(); project.ResetEditTracking();
    QVERIFY(project.mapEdits_ && project.mapEdits_->Terrain()); const auto sources = CaptureWorldTerrainSources(project); QVERIFY(sources.Succeeded());
    std::shared_ptr<WorldTerrainCache> cache;
    {
        WorldShadowInput input; input.read = project.ReadMapMember("war3map.shd"); QVERIFY(input.read->Succeeded());
        input.context = {WorldShadowTerrainRelation::TerrainAndPathingUnchanged, project.mapEdits_->SessionIdentity(),
            project.mapEdits_->Revision(), project.mapEdits_->TerrainSourceRevision(), false};
        const auto loaded = LoadWorldTerrain(members[1].data, sources, {}, input, &cache); QVERIFY(loaded.Succeeded());
    } // 输入Result及raw销毁，cache只拥有事实/诊断和既有environment。
    QVERIFY(cache && cache->environment); const auto originalEnvironment = cache->environment;
    const auto diagnosticsRows = project.diagnosticsModel_->rowCount(); const auto cachedNotes = cache->notes.size();
    MapData::TerrainBrush brush; brush.kind = MapData::TerrainBrushKind::Raise; brush.radius = 0; brush.strength = 1;
    const MapData::TerrainStrokePoint point{128, 192};
    QVERIFY(project.mapEdits_->EditTerrain(brush, std::span(&point, 1), false).Succeeded());
    WorldShadowContext context{WorldShadowTerrainRelation::TerrainOrPathingEdited, project.mapEdits_->SessionIdentity(),
        project.mapEdits_->Revision(), project.mapEdits_->TerrainSourceRevision(), true};
    const auto rebuilt = RebuildWorldTerrain(*project.mapEdits_->Terrain(), *cache, {}, {},
        Render::TerrainGroundBlightPolicy::Legacy, Render::TerrainGroundRepresentation::Expanded, context);
    QVERIFY(rebuilt.Succeeded() && rebuilt.value->environment == originalEnvironment);
    QVERIFY(rebuilt.value->shadowObservation.context.terrainRelation == WorldShadowTerrainRelation::TerrainOrPathingEdited);
    QVERIFY(cache->shadowObservation.context.terrainRelation == WorldShadowTerrainRelation::TerrainAndPathingUnchanged);
    const auto summary = WorldShadowSummary(rebuilt.value->shadowObservation, rebuilt.value->environment.get());
    const auto details = WorldShadowDetails(rebuilt.value->shadowObservation, rebuilt.value->environment.get());
    QVERIFY(summary.contains(QStringLiteral("地形或通行性已变化")) && details.contains(QStringLiteral("草稿未用于")));
    for (unsigned i = 0; i < 4; ++i) {
        QCOMPARE(WorldShadowSummary(rebuilt.value->shadowObservation, originalEnvironment.get()), summary);
        QCOMPARE(WorldShadowDetails(rebuilt.value->shadowObservation, originalEnvironment.get()), details);
    }
    QCOMPARE(project.diagnosticsModel_->rowCount(), diagnosticsRows); QCOMPARE(cache->notes.size(), cachedNotes);
    QVERIFY(project.mapEdits_->Undo()); context.terrainRelation = WorldShadowTerrainRelation::TerrainAndPathingUnchanged;
    context.revision = project.mapEdits_->Revision(); context.pendingSourceChanges = false;
    const auto undone = RebuildWorldTerrain(*project.mapEdits_->Terrain(), *cache, {}, {},
        Render::TerrainGroundBlightPolicy::Legacy, Render::TerrainGroundRepresentation::Expanded, context);
    QVERIFY(undone.Succeeded() && undone.value->environment == originalEnvironment);
    QVERIFY(undone.value->shadowObservation.context.terrainRelation == WorldShadowTerrainRelation::TerrainAndPathingUnchanged);
    QVERIFY(WorldShadowDetails(undone.value->shadowObservation, originalEnvironment.get()).contains(QStringLiteral("不证明源阴影")));
    const auto raw = project.ReadMapMember("war3map.shd"); QVERIFY(raw.Succeeded()); QCOMPARE(*raw.value, originalShadow);
    QCOMPARE(*Sha256File(path).value, sourceSha);
}

void WorldModuleTests::CliffSourceFailuresKeepWinnerAndReobserveReadFailures_data() {
    QTest::addColumn<QString>("mode");
    for (const auto* mode : {"missing", "malformed-owned", "read-failure", "valid-owned", "sampler-unsupported"})
        QTest::newRow(mode) << QString::fromLatin1(mode);
}

void WorldModuleTests::CliffSourceFailuresKeepWinnerAndReobserveReadFailures() {
    namespace Cliff = Tests::WorldCliffSourceFailure;
    QFETCH(QString, mode);
    QTemporaryDir directory; QVERIFY(directory.isValid());
    auto members = Cliff::Members(ShadowObservationMembers(), *FlatGrid(2, 2)); QVERIFY(!members.empty());
    // 请求路径在原地图也有效；会话winner损坏/不可读时不能改取原地图正文或低变体。
    if (mode != "missing") members.push_back({Cliff::storedRequested, Cliff::Model(11)});
    const auto path = std::filesystem::path(directory.filePath("cliff-source.w3x").toStdWString());
    QVERIFY(WriteVerifiedArchive(path, members).Succeeded()); auto archive = MapArchiveSession::Open(path); QVERIFY(archive.Succeeded());
    const auto sourceSha = archive.value->SourceSha256();
    QtProject::EditorProject project; project.sourceArchivePath_ = path; project.projectRoot_ = path.parent_path();
    project.archiveSession_ = std::make_unique<MapArchiveSession>(std::move(*archive.value)); project.LoadMapDocument(); project.ResetEditTracking();
    QVERIFY(project.mapEdits_ && project.mapEdits_->Terrain());
    const auto requestedName = QString::fromStdString(Cliff::storedRequested);
    const auto diskPath = directory.filePath("explicit-next-rebuild.mdx");
    auto winner = Cliff::Model(37, mode == "sampler-unsupported" ? 8u : 0u);
    if (mode == "malformed-owned") winner.resize(winner.size() - 6);
    if (mode == "read-failure") project.pendingImports_.push_back({requestedName, diskPath, static_cast<qint64>(winner.size())});
    else if (mode != "missing") project.pendingImports_.push_back({requestedName, {}, static_cast<qint64>(winner.size()),
        std::make_shared<const std::vector<std::byte>>(winner)});
    const auto sources = CaptureWorldTerrainSources(project); QVERIFY(sources.Succeeded());
    auto independent = WorldResourceReader::Open(*sources.value); QVERIFY(independent.Succeeded());
    const auto requestedLocation = (*independent.value)->Resolver().Locate(Cliff::requested);
    std::vector<Diagnostic> expected;
    if (mode == "read-failure") {
        const auto read = (*independent.value)->Resolver().Read(Cliff::requested); QVERIFY(!read.Succeeded()); expected = read.diagnostics;
        QVERIFY(std::any_of(expected.begin(), expected.end(), [](const auto& diagnostic) { return diagnostic.code == "WORLD_ASSET_READ"; }));
    } else if (mode == "malformed-owned") {
        const auto parsed = Render::ParseMdx(winner); QVERIFY(!parsed.Succeeded()); expected = parsed.diagnostics;
        QVERIFY(std::all_of(expected.begin(), expected.end(), [](const auto& diagnostic) {
            return diagnostic.message.find(Cliff::requested) == std::string::npos && (!diagnostic.origin || diagnostic.origin->source.empty());
        })); // codec不接路径；请求与真实来源必须由独立事实补足，不能改写这些原诊断。
    }
    const auto globalRows = project.diagnosticsModel_->rowCount();
    auto expectedGlobalRows = globalRows;
    std::shared_ptr<WorldTerrainCache> cache;
    const auto loaded = LoadWorldTerrain(members[1].data, sources, {}, {}, &cache);
    QVERIFY2(loaded.Succeeded(), loaded.diagnostics.empty() ? "cliff fixture load failed" : loaded.diagnostics.front().message.c_str());
    QVERIFY(cache && cache->reader && loaded.value->ground);
    if (mode == "missing") {
        QVERIFY(!requestedLocation.Succeeded() && requestedLocation.diagnostics.size() == 1
            && requestedLocation.diagnostics.front().code == "RESOURCE_NOT_FOUND");
        QVERIFY(Cliff::SelectsGeometry(*loaded.value, 0));
        const auto lowerLocation = cache->reader->Resolver().Locate(Cliff::lower); QVERIFY(lowerLocation.Succeeded());
        QVERIFY(Cliff::CachesResource(*cache, Cliff::lower, Cliff::Model(0), *lowerLocation.value));
        QVERIFY(!cache->modelFiles.contains(Cliff::requested));
    } else {
        QVERIFY(requestedLocation.Succeeded());
        if (mode == "valid-owned") {
            QVERIFY(Cliff::SelectsGeometry(*loaded.value, 37));
            QVERIFY(Cliff::CachesResource(*cache, Cliff::requested, winner, *requestedLocation.value));
        } else if (mode == "sampler-unsupported") {
            QVERIFY(!loaded.value->cliffs && !loaded.value->cliffFailure && loaded.value->ground->hiddenTileCount == 0);
            QVERIFY(loaded.value->cliffDiagnostics.size() == 1
                && loaded.value->cliffDiagnostics.front().code == "TERRAIN_CLIFF_SAMPLER_UNSUPPORTED");
            QVERIFY(!loaded.value->materialNote.contains(QString::fromStdString(std::string(Render::kTerrainCliffModelFailureContextCode))));
            QVERIFY(Cliff::CachesResource(*cache, Cliff::requested, winner, *requestedLocation.value));
            // Qt成功正文cache复用后仍是Bake失败；最后reader回调不能被当成确定的失败模型归因。
            const auto repeated = RebuildWorldTerrain(*project.mapEdits_->Terrain(), *cache, {});
            QVERIFY(repeated.Succeeded() && !repeated.value->cliffs && !repeated.value->cliffFailure && repeated.value->ground
                && repeated.value->ground->hiddenTileCount == 0);
            QVERIFY(Cliff::SameDiagnostics(repeated.value->cliffDiagnostics, loaded.value->cliffDiagnostics));
            QVERIFY(!repeated.value->materialNote.contains(QString::fromStdString(std::string(Render::kTerrainCliffModelFailureContextCode))));
        } else {
            QVERIFY(Cliff::RetainsFailure(*loaded.value, expected));
            if (mode == "read-failure") QVERIFY(cache->modelFiles.empty() && !loaded.value->cliffFailure->location);
            else {
                QVERIFY(Cliff::CachesResource(*cache, Cliff::requested, winner, *requestedLocation.value));
                QVERIFY(loaded.value->cliffFailure->location && Cliff::SameLocation(*loaded.value->cliffFailure->location, *requestedLocation.value));
                QVERIFY(loaded.value->materialNote.contains(QString::fromStdString(requestedLocation.value->sourceId))
                    && loaded.value->materialNote.contains(QString::fromStdString(requestedLocation.value->sourcePath)));
            }
        }
        QVERIFY(!cache->modelFiles.contains(Cliff::lower));
    }
    if (mode == "read-failure") {
        // 原失败没有跨job固定为nullopt；写入同一声明disk来源后，只有显式重建才重新读取。
        const auto repeated = RebuildWorldTerrain(*project.mapEdits_->Terrain(), *cache, {});
        QVERIFY(repeated.Succeeded() && Cliff::RetainsFailure(*repeated.value, expected));
        QVERIFY(cache->modelFiles.empty());
        QVERIFY(Write(diskPath, QByteArray(reinterpret_cast<const char*>(winner.data()), static_cast<qsizetype>(winner.size()))));
        QVERIFY(cache->modelFiles.empty());
        const auto recovered = RebuildWorldTerrain(*project.mapEdits_->Terrain(), *cache, {});
        QVERIFY(recovered.Succeeded() && Cliff::SelectsGeometry(*recovered.value, 37));
        QVERIFY(Cliff::CachesResource(*cache, Cliff::requested, winner, *requestedLocation.value));
        QVERIFY(!cache->modelFiles.contains(Cliff::lower));
    } else if (mode == "malformed-owned") {
        // 成功读到的坏正文属于原immutable owner；更换owner并显式通知导入，沿原来源失效链新建cache。
        QCOMPARE(project.diagnosticsModel_->rowCount(), globalRows); // 原失败及重建没有发布全局诊断。
        const auto repaired = Cliff::Model(37);
        project.pendingImports_[0].ownedPayload = std::make_shared<const std::vector<std::byte>>(repaired);
        project.pendingImports_[0].size = static_cast<qint64>(repaired.size()); project.NotifyImportsChanged(true);
        // 导入信号会调用既有物编目录刷新；其独立诊断不属于下方地形加载的副作用。
        expectedGlobalRows = project.diagnosticsModel_->rowCount();
        const auto changed = CaptureWorldTerrainSources(project); QVERIFY(changed.Succeeded());
        QVERIFY(!SameWorldTerrainSources(*sources.value, *changed.value) && !cache->reader->MatchesSources(*changed.value));
        std::shared_ptr<WorldTerrainCache> repairedCache;
        const auto reloaded = LoadWorldTerrain(members[1].data, changed, {}, {}, &repairedCache);
        QVERIFY(reloaded.Succeeded() && repairedCache && Cliff::SelectsGeometry(*reloaded.value, 37));
        QVERIFY(Cliff::CachesResource(*repairedCache, Cliff::requested, repaired, *requestedLocation.value));
        QVERIFY(Cliff::CachesResource(*cache, Cliff::requested, winner, *requestedLocation.value));
        QVERIFY(!repairedCache->modelFiles.contains(Cliff::lower));
    }
    QCOMPARE(project.diagnosticsModel_->rowCount(), expectedGlobalRows);
    const auto terrain = project.ReadMapMember("war3map.w3e"); QVERIFY(terrain.Succeeded()); QCOMPARE(*terrain.value, members[1].data);
    const auto finalSha = Sha256File(path); QVERIFY(finalSha.Succeeded()); QCOMPARE(*finalSha.value, sourceSha);
    QVERIFY(!project.mapEdits_->IsDirty());
}

void WorldModuleTests::CliffSamplerFailureAfterMissingDoesNotInventModelContext() {
    namespace Cliff = Tests::WorldCliffSourceFailure;
    QTemporaryDir directory; QVERIFY(directory.isValid());
    auto members = Cliff::Members(ShadowObservationMembers(), *FlatGrid(3, 2)); QVERIFY(!members.empty());
    std::erase_if(members, [](const auto& member) { return member.name == Cliff::storedLower; });
    const std::string second = "Doodads\\Terrain\\CLIFFS\\CLIFFSBAAA0.mdx";
    const std::string storedSecond = "Doodads\\Terrain\\Cliffs\\CliffsBAAA0.mdx";
    const auto sampler = Cliff::Model(37, 8); members.push_back({storedSecond, sampler});
    const auto path = std::filesystem::path(directory.filePath("cliff-missing-then-bake.w3x").toStdWString());
    QVERIFY(WriteVerifiedArchive(path, members).Succeeded()); auto source = MapArchiveSession::Open(path); QVERIFY(source.Succeeded());
    const auto sourceSha = source.value->SourceSha256();
    QtProject::EditorProject project; project.sourceArchivePath_ = path; project.projectRoot_ = path.parent_path();
    project.archiveSession_ = std::make_unique<MapArchiveSession>(std::move(*source.value)); project.LoadMapDocument(); project.ResetEditTracking();
    QVERIFY(project.mapEdits_ && project.mapEdits_->Terrain());
    const auto sources = CaptureWorldTerrainSources(project); QVERIFY(sources.Succeeded());
    std::shared_ptr<WorldTerrainCache> cache;
    const auto loaded = LoadWorldTerrain(members[1].data, sources, {}, {}, &cache);
    QVERIFY(loaded.Succeeded() && cache && cache->reader && loaded.value->ground);
    QVERIFY(!loaded.value->cliffs && !loaded.value->cliffFailure && loaded.value->ground->hiddenTileCount == 0);
    QVERIFY(loaded.value->cliffDiagnostics.size() == 1
        && loaded.value->cliffDiagnostics.front().code == "TERRAIN_CLIFF_SAMPLER_UNSUPPORTED");
    QVERIFY(!cache->modelFiles.contains(Cliff::requested) && !cache->modelFiles.contains(Cliff::lower) && cache->modelFiles.size() == 1);
    const auto location = cache->reader->Resolver().Locate(second); QVERIFY(location.Succeeded());
    QVERIFY(Cliff::CachesResource(*cache, second, sampler, *location.value));
    // 再次显式重建：先读明确Missing，再复用Qt成功正文；Bake失败仍不能借前一Missing或最后callback归因。
    const auto repeated = RebuildWorldTerrain(*project.mapEdits_->Terrain(), *cache, {});
    QVERIFY(repeated.Succeeded() && !repeated.value->cliffs && !repeated.value->cliffFailure && repeated.value->ground
        && repeated.value->ground->hiddenTileCount == 0);
    QVERIFY(Cliff::SameDiagnostics(repeated.value->cliffDiagnostics, loaded.value->cliffDiagnostics));
    QVERIFY(!repeated.value->materialNote.contains(QString::fromStdString(std::string(Render::kTerrainCliffModelFailureContextCode)))
        && !repeated.value->materialNote.contains(QStringLiteral("实际模型来源：")));
    const auto finalSha = Sha256File(path); QVERIFY(finalSha.Succeeded()); QCOMPARE(*finalSha.value, sourceSha);
    QVERIFY(!project.mapEdits_->IsDirty());
}

void WorldModuleTests::DockWorkspaceUsesLeftTabsAndRestoresBottomAssets() {
    QtProject::EditorProject project; BackendState state; activeBackend = &state;
    WorldModule module(project, &backend); auto* page = module.Widget(); state.viewport = module.Viewport();
    page->resize(640, 480); page->show();
    auto* host = page->findChild<QMainWindow*>("worldDockHost");
    auto* tool = page->findChild<QDockWidget*>("worldToolDock");
    auto* assets = page->findChild<QDockWidget*>("worldAssetDock");
    auto* browser = page->findChild<WorldAssetBrowser*>("worldAssetBrowser");
    QVERIFY(host && tool && assets && browser);
    QVERIFY(!host->isWindow());
    QVERIFY(host->isVisible());
    QVERIFY(module.Viewport()->isVisible());
    QVERIFY(module.Viewport()->width() > 0 && module.Viewport()->height() > 0);
    QCOMPARE(host->dockWidgetArea(assets), Qt::LeftDockWidgetArea);
    QVERIFY(host->tabifiedDockWidgets(tool).contains(assets)); QVERIFY(browser->Compact());
    host->addDockWidget(Qt::BottomDockWidgetArea, assets);
    QCOMPARE(host->dockWidgetArea(assets), Qt::BottomDockWidgetArea); QVERIFY(!browser->Compact());
    const auto dockState = host->saveState(1);
    host->addDockWidget(Qt::LeftDockWidgetArea, assets); host->tabifyDockWidget(tool, assets);
    module.RestoreViewState({{"version", 2}, {"docks", QString::fromLatin1(dockState.toBase64())}});
    QCOMPARE(host->dockWidgetArea(assets), Qt::BottomDockWidgetArea);
    QCOMPARE(project.mapDataUndoStack_->count(), 0);
}

void WorldModuleTests::MeshBoundsAndSampling() {
    auto owned = FlatGrid(513, 513);
    auto& grid = *owned;
    const auto mesh = BuildWorldTerrainMesh(grid);
    QVERIFY(mesh.Succeeded());
    QCOMPARE(mesh.value->stride, 2u);
    QCOMPARE(mesh.value->vertices.size(), std::size_t{257 * 257});
    QCOMPARE(mesh.value->indices.size(), std::size_t{256 * 256 * 6});
    QCOMPARE(mesh.value->vertices.front().x, grid.minimum[0]);
    QCOMPARE(mesh.value->vertices.back().x, grid.maximum[0]);
    QCOMPARE(mesh.value->vertices.back().y, grid.maximum[1]);
    // 非整除边界仍包含最后一行和一列。
    grid.header.width = 512; grid.vertices.resize(512 * 513);
    grid.vertices.back().position = {65408, 65472, 0};
    const auto uneven = BuildWorldTerrainMesh(grid);
    QVERIFY(uneven.Succeeded());
    QCOMPARE(uneven.value->vertices.back().x, 65408.0f);
    grid.header.width = 1;
    QVERIFY(!BuildWorldTerrainMesh(grid).Succeeded());
}

void WorldModuleTests::ViewportLifecycleAndInput() {
    BackendState state; activeBackend = &state;
    QWidget first, second; first.resize(640, 480); second.resize(640, 480);
    auto viewport = std::make_unique<WorldViewport>(&first, &backend);
    state.viewport = viewport.get(); viewport->resize(320, 240);
    QVERIFY(viewport->SetMesh({{0, 0, 0, 0, 1, 0}, {128, 0, 0, 0, 1, 0}, {0, 128, 64, 0, 1, 0}}, {0, 1, 2}, 9));
    first.show(); viewport->show();
    QTRY_VERIFY(state.frames > 0);
    QCOMPARE(state.creates, 1);
    QCOMPARE(state.width, static_cast<std::uint32_t>(qRound(viewport->width() * viewport->devicePixelRatioF())));
    QCOMPARE(state.revision, std::uint64_t{9});
    const auto home = viewport->SaveCamera();
    Mouse(*viewport, QEvent::MouseButtonPress, {80, 80}, Qt::LeftButton, Qt::LeftButton, Qt::AltModifier);
    Mouse(*viewport, QEvent::MouseMove, {120, 95}, Qt::NoButton, Qt::LeftButton, Qt::AltModifier);
    Mouse(*viewport, QEvent::MouseButtonRelease, {120, 95}, Qt::LeftButton, Qt::NoButton, Qt::AltModifier);
    QVERIFY(viewport->SaveCamera()["yaw"] != home["yaw"]);
    Mouse(*viewport, QEvent::MouseButtonPress, {80, 80}, Qt::MiddleButton, Qt::MiddleButton);
    Mouse(*viewport, QEvent::MouseMove, {100, 110}, Qt::NoButton, Qt::MiddleButton);
    Mouse(*viewport, QEvent::MouseButtonRelease, {100, 110}, Qt::MiddleButton, Qt::NoButton);
    QVERIFY(viewport->SaveCamera()["x"] != home["x"] || viewport->SaveCamera()["y"] != home["y"]);
    const auto distance = viewport->Camera().distance;
    QWheelEvent wheel({100, 100}, {100, 100}, {}, {0, 120}, Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(viewport.get(), &wheel);
    QVERIFY(viewport->Camera().distance < distance);
    viewport->ResetCamera(); QCOMPARE(viewport->SaveCamera(), home);
    // 默认是原版游戏视角：对准地图中心、朝北、俯角 56°，距离不随视口尺寸变化（不按全图取景）。
    QCOMPARE(home["x"].toDouble(), 64.0); QCOMPARE(home["y"].toDouble(), 64.0);
    QVERIFY(std::abs(home["yaw"].toDouble() + 1.5707963) < 1e-5 && std::abs(home["pitch"].toDouble() - 0.9773844) < 1e-5);
    viewport->resize(160, 480); viewport->ResetCamera(); QCOMPARE(viewport->SaveCamera(), home);
    viewport->resize(320, 240); viewport->ResetCamera(); QCOMPARE(viewport->SaveCamera(), home);
    auto invalid = home; invalid["distance"] = 500000;
    QVERIFY(!viewport->RestoreCamera(invalid)); QCOMPARE(viewport->SaveCamera(), home);
    viewport->hide(); const int frames = state.frames;
    QTest::qWait(90); QCOMPARE(state.frames, frames); QVERIFY(!viewport->FrameTimerActive());
    viewport->setParent(&second); second.show(); viewport->show();
    QTRY_COMPARE(state.creates, 2);
    QCOMPARE(state.destroys, 1);
    QVERIFY(state.destroyedBeforeWindow && state.stoppedBeforeDestroy);
    viewport.reset();
    QCOMPARE(state.destroys, 2);
    QVERIFY(state.destroyedBeforeWindow && state.stoppedBeforeDestroy);
}

void WorldModuleTests::MapSwitchDoesNotEdit() {
    QTemporaryDir directory; QVERIFY(directory.isValid());
    QVERIFY(Write(directory.filePath("war3map.w3e"), TerrainBytes()));
    QtProject::EditorProject project;
    project.projectRoot_ = directory.path().toStdWString();
    auto snapshot = DirectoryWorkspace::Open(project.projectRoot_); QVERIFY(snapshot.Succeeded());
    project.directorySnapshot_ = std::make_unique<DirectoryWorkspace>(std::move(*snapshot.value));
    project.ResetEditTracking(); QVERIFY(project.editTrackingReady_);
    BackendState state; activeBackend = &state;
    WorldModule module(project, &backend);
    auto* page = module.Widget(); state.viewport = module.Viewport(); page->resize(800, 600); page->show();
    // 该后端既无模型接口，也无动画尾部：控件可发现但不可用，模型准备须留下明确失败。
    auto* play = page->findChild<QPushButton*>("worldAnimationPlay");
    auto* pause = page->findChild<QPushButton*>("worldAnimationPause");
    auto* step = page->findChild<QPushButton*>("worldAnimationStep");
    QVERIFY(play && pause && step);
    QVERIFY(!play->isEnabled() && !pause->isEnabled() && !step->isEnabled());
    QVERIFY(!module.Viewport()->HasAnimationSupport());
    QTRY_COMPARE(module.Viewport()->VertexCount(), std::size_t{6});
    QTRY_VERIFY(module.ModelLoadFailed());
    const auto playback = module.PlaybackDiagnosticSnapshot();
    const auto modelLoad = playback["model_load"].toObject();
    QCOMPARE(modelLoad["stage"].toString(), QStringLiteral("failed"));
    const auto modelDiagnostics = modelLoad["diagnostics"].toArray();
    QCOMPARE(modelDiagnostics.size(), 1);
    QCOMPARE(modelDiagnostics.first().toObject()["code"].toString(), QStringLiteral("WORLD_MODEL_BACKEND_UNAVAILABLE"));
    QVERIFY(playback["player"].toObject()["last_diagnostic"].toString().contains(QStringLiteral("渲染插件没有模型显示能力")));
    QVERIFY(!playback["player"].toObject()["ready"].toBool());
    for (const auto* key : {"play", "pause", "step"}) {
        const auto button = playback["buttons"].toObject()[key].toObject();
        QVERIFY(!button["effective_enabled"].toBool()); QVERIFY(button["locally_disabled"].toBool());
    }
    QCOMPARE(module.Viewport()->TriangleCount(), std::size_t{4});
    QCOMPARE(module.Viewport()->Camera().targetX, 0.0f);
    Mouse(*module.Viewport(), QEvent::MouseMove, {module.Viewport()->width() / 4.0, module.Viewport()->height() / 2.0}, Qt::NoButton, Qt::NoButton);
    QTRY_VERIFY(module.Viewport()->HoverCoordinate().has_value());
    const QPointF selectedPoint(module.Viewport()->width() / 4.0, module.Viewport()->height() / 2.0);
    Mouse(*module.Viewport(), QEvent::MouseButtonPress, selectedPoint, Qt::LeftButton, Qt::LeftButton);
    Mouse(*module.Viewport(), QEvent::MouseButtonRelease, selectedPoint, Qt::LeftButton, Qt::NoButton);
    QVERIFY(module.Viewport()->SelectedCoordinate().has_value());
    QCOMPARE(page->findChild<QLabel*>("worldSelectedCoordinates")->text(), module.Viewport()->SelectionText());
    auto camera = module.Viewport()->SaveCamera(); camera["distance"] = 1024;
    QVERIFY(module.Viewport()->RestoreCamera(camera));
    (void)module.SaveViewState(); module.Viewport()->ResetCamera();
    QVERIFY(project.ObserveEditState("只读地形视角"));
    QCOMPARE(project.editRevision_.Revision(), std::uint64_t{0});
    QVERIFY(!project.editRevision_.IsDirty(EditSaveTarget::Map));
    QVERIFY(!project.editRevision_.IsDirty(EditSaveTarget::Project));
    QCOMPARE(project.mapDataUndoStack_->count(), 0); QVERIFY(module.UndoStack() == project.mapDataUndoStack_);
    // 连续地图切换合并到最新 generation，不允许旧解码覆盖新地图；同一张地图重新读取（如保存后）保留当前视角。
    QVERIFY(module.Viewport()->RestoreCamera(camera));
    QVERIFY(Write(directory.filePath("war3map.w3e"), TerrainBytes(800)));
    const auto stale = project.ReadMapMember("war3map.w3e");
    QVERIFY(!stale.Succeeded() && !stale.diagnostics.empty()
        && stale.diagnostics.front().code == "VERIFIED_FILE_SHA_MISMATCH");
    // 模拟重新打开已变化的工作区；旧快照不能被用于读取新的地图字节。
    auto switched = DirectoryWorkspace::Open(project.projectRoot_); QVERIFY(switched.Succeeded());
    project.directorySnapshot_ = std::make_unique<DirectoryWorkspace>(std::move(*switched.value));
    project.MapDocumentChanged(); project.MapDocumentChanged();
    QVERIFY(!module.Viewport()->HoverCoordinate() && !module.Viewport()->SelectedCoordinate());
    QTRY_VERIFY(module.Viewport()->VertexCount() == 6 && module.Viewport()->Camera().distance == 1024.0f);
    QCOMPARE(module.Viewport()->Camera().targetX, 0.0f);
    // 视图状态记录所属地图：其他地图与旧版本（不分地图）的状态不恢复。
    const auto saved = module.SaveViewState();
    QCOMPARE(saved["map"].toString(), QString::fromStdWString(project.projectRoot_.lexically_normal().wstring()));
    module.Viewport()->ResetCamera(); QCOMPARE(module.Viewport()->Camera().targetX, 928.0f);
    auto other = saved; other["map"] = QStringLiteral("other.w3x"); module.RestoreViewState(other);
    QCOMPARE(module.Viewport()->Camera().targetX, 928.0f);
    auto legacy = saved; legacy.remove("version"); module.RestoreViewState(legacy);
    QCOMPARE(module.Viewport()->Camera().targetX, 928.0f);
    module.RestoreViewState(saved);
    QVERIFY(module.Viewport()->Camera().targetX == 0.0f && module.Viewport()->Camera().distance == 1024.0f);
    QVERIFY(Write(directory.filePath("war3map.w3e"), QByteArray("broken")));
    project.MapDocumentChanged();
    QTRY_COMPARE(module.Viewport()->VertexCount(), std::size_t{0});
    QTRY_VERIFY(!page->findChild<QLabel*>("worldTerrainStatus")->text().contains(QStringLiteral("正在")));
}

void WorldModuleTests::TerrainLoadSnapshotPreservesFailureAndClearsOnRefresh_data() {
    QTest::addColumn<bool>("brokenTerrain");
    QTest::addColumn<QString>("failureStage");
    QTest::addColumn<QString>("failureCode");
    QTest::newRow("missing-member") << false << QStringLiteral("source-read-failed")
        << QStringLiteral("MAP_MEMBER_UNAVAILABLE");
    QTest::newRow("malformed-member") << true << QStringLiteral("terrain-load-failed")
        << QStringLiteral("TERRAIN_MAGIC");
}

void WorldModuleTests::TerrainLoadSnapshotPreservesFailureAndClearsOnRefresh() {
    QFETCH(bool, brokenTerrain);
    QFETCH(QString, failureStage);
    QFETCH(QString, failureCode);
    QTemporaryDir directory; QVERIFY(directory.isValid());
    QVERIFY(Write(directory.filePath("owned-note.txt"), QByteArray("owned terrain diagnostic fixture")));
    if (brokenTerrain) QVERIFY(Write(directory.filePath("war3map.w3e"), QByteArray("broken")));
    QtProject::EditorProject project; project.projectRoot_ = directory.path().toStdWString();
    auto snapshot = DirectoryWorkspace::Open(project.projectRoot_); QVERIFY(snapshot.Succeeded());
    project.directorySnapshot_ = std::make_unique<DirectoryWorkspace>(std::move(*snapshot.value));
    BackendState state; activeBackend = &state;
    WorldModule module(project, &backend);
    auto* page = module.Widget(); state.viewport = module.Viewport(); page->resize(800, 600);
    const auto loadSnapshot = [&] { return module.PlaybackDiagnosticSnapshot()["terrain_load"].toObject(); };
    QCOMPARE(loadSnapshot()["stage"].toString(), QStringLiteral("deferred-hidden"));
    QVERIFY(!module.TerrainLoadFailed());
    QVERIFY(!loadSnapshot()["failed"].toBool());
    QVERIFY(!loadSnapshot()["worker_running"].toBool());
    QVERIFY(loadSnapshot()["diagnostics"].toArray().isEmpty());
    QCOMPARE(state.creates, 0);

    page->show();
    QTRY_VERIFY(module.TerrainLoadFailed());
    const auto failed = loadSnapshot();
    QVERIFY(failed["failed"].toBool());
    QCOMPARE(failed["scope"].toString(), QStringLiteral("source-load"));
    QCOMPARE(failed["stage"].toString(), failureStage);
    QCOMPARE(failed["status_text"].toString(), page->findChild<QLabel*>("worldTerrainStatus")->text());
    QVERIFY(!failed["worker_running"].toBool());
    QVERIFY(!failed["has_terrain"].toBool());
    QVERIFY(!failed["completion_proven"].toBool());
    QCOMPARE(failed["vertex_count"].toString(), QStringLiteral("0"));
    const auto diagnostics = failed["diagnostics"].toArray();
    QVERIFY(!diagnostics.isEmpty());
    const auto first = diagnostics.first().toObject();
    QCOMPARE(first["code"].toString(), failureCode);
    QCOMPARE(first["severity"].toString(), QStringLiteral("error"));
    QVERIFY(!first["subsystem"].toString().isEmpty());
    QCOMPARE(first["message"].toString(), failed["status_text"].toString());
    QVERIFY(!first["message_truncated"].toBool());
    QCOMPARE(failed["diagnostics_omitted"].toString(), QStringLiteral("0"));
    if (brokenTerrain) QCOMPARE(first["origin"].toObject()["offset"].toString(), QStringLiteral("0"));
    const auto failedGeneration = failed["generation"].toString().toULongLong();

    // 重开自有新来源后旧错误必须清掉；后台准备成功仍不是GPU完成帧证据。
    QVERIFY(Write(directory.filePath("war3map.w3e"), TerrainBytes()));
    auto reopened = DirectoryWorkspace::Open(project.projectRoot_); QVERIFY(reopened.Succeeded());
    project.directorySnapshot_ = std::make_unique<DirectoryWorkspace>(std::move(*reopened.value));
    project.MapDocumentChanged();
    QVERIFY(!module.TerrainLoadFailed());
    QVERIFY(!loadSnapshot()["failed"].toBool());
    QVERIFY(loadSnapshot()["diagnostics"].toArray().isEmpty());
    QTRY_COMPARE(loadSnapshot()["stage"].toString(), QStringLiteral("prepared"));
    const auto prepared = loadSnapshot();
    QVERIFY(prepared["generation"].toString().toULongLong() > failedGeneration);
    QVERIFY(prepared["has_terrain"].toBool());
    QVERIFY(prepared["has_raw_terrain"].toBool());
    QCOMPARE(prepared["vertex_count"].toString(), QStringLiteral("6"));
    QVERIFY(!prepared["completion_proven"].toBool());
    QCOMPARE(project.mapDataUndoStack_->count(), 0);
}

void WorldModuleTests::TerrainOnlySceneRequiresKnownEmptyPlacements_data() {
    QTest::addColumn<QByteArray>("unitBytes");
    QTest::addColumn<QByteArray>("doodadBytes");
    QTest::addColumn<int>("unitState");
    QTest::addColumn<int>("doodadState");
    QTest::addColumn<int>("unitCount");
    QTest::addColumn<int>("doodadCount");
    QTest::addColumn<bool>("ready");
    QTest::addColumn<int>("unresolved");
    QTest::addColumn<int>("startLocations");
    const auto bytes = [](const std::vector<std::byte>& value) {
        return QByteArray(reinterpret_cast<const char*>(value.data()), static_cast<qsizetype>(value.size()));
    };
    const auto emptyUnits = bytes(MapDataFixtures::EmptyUnitsV8Sub11());
    const auto emptyDoodads = bytes(MapDataFixtures::EmptyDoodadsV8Sub11());
    const auto add = [&](const char* name, QByteArray units, MapDataState unitStatus, int unitsCount,
        QByteArray doodads, MapDataState doodadStatus, int doodadsCount, bool expected, int unresolvedCount,
        int startLocations = 0) {
        QTest::newRow(name) << units << doodads << static_cast<int>(unitStatus) << static_cast<int>(doodadStatus)
            << unitsCount << doodadsCount << expected << unresolvedCount << startLocations;
    };
    add("both-missing", {}, MapDataState::Missing, -1, {}, MapDataState::Missing, -1, true, 0);
    add("empty-units", emptyUnits, MapDataState::Ready, 0, {}, MapDataState::Missing, -1, true, 0);
    add("empty-doodads", {}, MapDataState::Missing, -1, emptyDoodads, MapDataState::Ready, 0, true, 0);
    add("both-empty", emptyUnits, MapDataState::Ready, 0, emptyDoodads, MapDataState::Ready, 0, true, 0);
    add("broken-units", QByteArray("W3do"), MapDataState::Invalid, -1,
        emptyDoodads, MapDataState::Ready, 0, false, 0);
    add("broken-doodads", emptyUnits, MapDataState::Ready, 0,
        QByteArray("W3do"), MapDataState::Invalid, -1, false, 0);

    auto units = MapData::ParseUnitPlacements(MapDataFixtures::EmptyUnitsV8Sub11());
    auto doodads = MapData::ParseDoodadPlacements(MapDataFixtures::EmptyDoodadsV8Sub11());
    QVERIFY(units.Succeeded() && doodads.Succeeded());
    MapData::UnitPlacement unit; unit.rawcode = "sloc"; unit.creationId = 1; unit.scale = {1, 1, 1};
    units.value->units = {unit};
    auto serializedUnits = units.value->Serialize(); QVERIFY(serializedUnits.Succeeded());
    // 原版起点逐条验证后可以排除模型；回执仍保留数量，不假称已显示起点标记。
    add("start-location-without-model", bytes(*serializedUnits.value), MapDataState::Ready, 1,
        emptyDoodads, MapDataState::Ready, 0, true, 0, 1);
    units.value->units.push_back(unit);
    serializedUnits = units.value->Serialize(); QVERIFY(serializedUnits.Succeeded());
    add("partial-duplicate-unit-identity", bytes(*serializedUnits.value), MapDataState::Partial, 2,
        emptyDoodads, MapDataState::Ready, 0, false, 0);
    units.value->units.back().creationId = 2;
    serializedUnits = units.value->Serialize(); QVERIFY(serializedUnits.Succeeded());
    add("two-start-locations-without-model", bytes(*serializedUnits.value), MapDataState::Ready, 2,
        emptyDoodads, MapDataState::Ready, 0, true, 0, 2);
    units.value->units.back().rawcode = "zzzz";
    serializedUnits = units.value->Serialize(); QVERIFY(serializedUnits.Succeeded());
    add("start-location-and-unknown-unit", bytes(*serializedUnits.value), MapDataState::Ready, 2,
        emptyDoodads, MapDataState::Ready, 0, false, 1);
    unit.rawcode = "zzzz"; units.value->units = {unit};
    serializedUnits = units.value->Serialize(); QVERIFY(serializedUnits.Succeeded());
    add("unresolved-unit", bytes(*serializedUnits.value), MapDataState::Ready, 1,
        emptyDoodads, MapDataState::Ready, 0, false, 1);
    for (std::uint32_t id = 2; id <= 41; ++id) {
        unit.creationId = id; units.value->units.push_back(unit);
    }
    serializedUnits = units.value->Serialize(); QVERIFY(serializedUnits.Succeeded());
    add("unresolved-units-paged", bytes(*serializedUnits.value), MapDataState::Ready, 41,
        emptyDoodads, MapDataState::Ready, 0, false, 41);

    MapData::DoodadPlacement doodad; doodad.rawcode = "zzzz"; doodad.creationId = 2; doodad.scale = {1, 1, 1};
    doodads.value->doodads = {doodad};
    auto serializedDoodads = doodads.value->Serialize(); QVERIFY(serializedDoodads.Succeeded());
    add("unresolved-doodad", emptyUnits, MapDataState::Ready, 0,
        bytes(*serializedDoodads.value), MapDataState::Ready, 1, false, 1);
    doodads.value->doodads.push_back(doodad);
    serializedDoodads = doodads.value->Serialize(); QVERIFY(serializedDoodads.Succeeded());
    add("partial-duplicate-doodad-identity", emptyUnits, MapDataState::Ready, 0,
        bytes(*serializedDoodads.value), MapDataState::Partial, 2, false, 2);
    doodads.value->doodads.clear();
    doodads.value->specialDoodads.push_back({"zzzz", 0, {0, 0}});
    serializedDoodads = doodads.value->Serialize(); QVERIFY(serializedDoodads.Succeeded());
    add("special-doodad-without-model", emptyUnits, MapDataState::Ready, 0,
        bytes(*serializedDoodads.value), MapDataState::Ready, 1, false, 0);
}

void WorldModuleTests::TerrainOnlySceneRequiresKnownEmptyPlacements() {
    QFETCH(QByteArray, unitBytes); QFETCH(QByteArray, doodadBytes);
    QFETCH(int, unitState); QFETCH(int, doodadState);
    QFETCH(int, unitCount); QFETCH(int, doodadCount);
    QFETCH(bool, ready); QFETCH(int, unresolved); QFETCH(int, startLocations);
    QTemporaryDir directory; QVERIFY(directory.isValid());
    QVERIFY(Write(directory.filePath("war3map.w3e"), TerrainBytes()));
    if (!unitBytes.isEmpty()) QVERIFY(Write(directory.filePath("war3mapUnits.doo"), unitBytes));
    if (!doodadBytes.isEmpty()) QVERIFY(Write(directory.filePath("war3map.doo"), doodadBytes));
    QtProject::EditorProject project; project.projectRoot_ = directory.path().toStdWString();
    QVERIFY(project.war3GameRoot_.empty());
    auto source = DirectoryWorkspace::Open(project.projectRoot_); QVERIFY(source.Succeeded());
    project.directorySnapshot_ = std::make_unique<DirectoryWorkspace>(std::move(*source.value));
    project.LoadMapDocument(); project.ResetEditTracking();
    QVERIFY(project.mapDocument_ && project.mapEdits_);
    const auto& actualUnits = project.mapDocument_->Member(MapDataKind::Units);
    const auto& actualDoodads = project.mapDocument_->Member(MapDataKind::Doodads);
    QCOMPARE(static_cast<int>(actualUnits.state), unitState);
    QCOMPARE(static_cast<int>(actualDoodads.state), doodadState);
    QCOMPARE(actualUnits.recordCount.has_value(), unitCount >= 0);
    QCOMPARE(actualDoodads.recordCount.has_value(), doodadCount >= 0);
    if (actualUnits.recordCount) QCOMPARE(*actualUnits.recordCount, static_cast<std::size_t>(unitCount));
    if (actualDoodads.recordCount) QCOMPARE(*actualDoodads.recordCount, static_cast<std::size_t>(doodadCount));

    BackendState state; activeBackend = &state;
    auto api = modelBackend; api.submitSkinnedModelScene = SkinScene; api.updateSkinMatrices = SkinUpdate;
    api.getSkinStatus = SkinStatus; api.captureSkinnedFrameRgba = SkinCapture;
    WorldModule module(project, &api);
    QVERIFY(!module.TerrainOnlySceneReady());
    auto* page = module.Widget(); state.viewport = module.Viewport(); page->resize(800, 600);
    QVERIFY(!module.TerrainOnlySceneReady()); // 隐藏页尚未读取，空容器不能充当完成证据。
    page->show();
    QTRY_VERIFY(module.ViewportForTools() && module.Viewport()->VertexCount() == 6);
    QVERIFY(module.Viewport()->HasAnimationSupport());
    const auto diagnostic = module.ModelDiagnosticSnapshot();
    QCOMPARE(diagnostic["unresolvedPlacementCount"].toInt(), unresolved);
    if (ready) QTRY_VERIFY(module.TerrainOnlySceneReady());
    else QVERIFY(!module.TerrainOnlySceneReady());
    QTRY_COMPARE(module.ModelLoadFailed(), !ready);
    const auto terrainOnly = module.TerrainOnlySceneDiagnosticSnapshot();
    QCOMPARE(terrainOnly["source_start_location_count"].toInt(), startLocations);
    QVERIFY(terrainOnly["start_location_marker_implemented"].toBool());
    QVERIFY(!terrainOnly["start_location_marker_visual_verified"].toBool());
    QVERIFY(!terrainOnly["completion_proven"].toBool());
    const auto progress = module.ModelDiagnosticProgressSnapshot();
    QCOMPARE(progress["unresolvedPlacementCount"].toInt(), unresolved);
    QCOMPARE(progress["unresolvedPlacementRows"].toArray().size(), std::min(16, unresolved));
    QCOMPARE(progress["unresolvedPlacementPage"].toObject()["omitted"].toInt(), std::max(0, unresolved - 16));
    QVERIFY(progress["unresolvedPlacementPage"].toObject()["counts_consistent"].toBool());
    QVERIFY(QJsonDocument(progress).toJson(QJsonDocument::Compact).size() < 128 * 1024);
    const auto later = module.ModelDiagnosticProgressSnapshot(32, 8);
    QCOMPARE(later["unresolvedPlacementRows"].toArray().size(), std::clamp(unresolved - 32, 0, 8));
    const auto past = module.ModelDiagnosticProgressSnapshot(UINT32_MAX, UINT32_MAX);
    QVERIFY(past["unresolvedPlacementRows"].toArray().isEmpty());
    QCOMPARE(past["unresolvedPlacementPage"].toObject()["remaining"].toInt(), 0);
    QVERIFY(past["unresolvedPlacementPage"].toObject()["counts_consistent"].toBool());
    // 调用者数量错误也应显式标记，而不是无符号下溢成一个巨大剩余数。
    const auto inconsistent = WorldDiagnosticJson::Page(3, 2, 4, 4);
    QVERIFY(!inconsistent["counts_consistent"].toBool());
    QCOMPARE(inconsistent["remaining"].toInt(), 0);
    // 无Game目录，也未准备UberSplat目录；不放宽原有模型诊断门来让纯地形探针通过。
    QVERIFY(!module.ModelDiagnosticsReady());
    QVERIFY(!module.TerrainLoadFailed());
    QVERIFY(!module.AnimationPlayer()->Ready());
    QCOMPARE(state.skinUploads, std::uint64_t{0});
}

void WorldModuleTests::TerrainOnlySceneRejectsStaleSourceAndRefresh() {
    QTemporaryDir first, second; QVERIFY(first.isValid() && second.isValid());
    QVERIFY(Write(first.filePath("war3map.w3e"), TerrainBytes()));
    QVERIFY(Write(second.filePath("war3map.w3e"), TerrainBytes(800)));
    const auto emptyRegions = MapData::RegionsDocument{5, {}, {}}.Serialize(); QVERIFY(emptyRegions.Succeeded());
    QVERIFY(Write(first.filePath("war3map.w3r"), QByteArray(reinterpret_cast<const char*>(emptyRegions.value->data()),
        static_cast<qsizetype>(emptyRegions.value->size()))));
    QtProject::EditorProject project; project.projectRoot_ = first.path().toStdWString();
    QVERIFY(project.war3GameRoot_.empty());
    auto source = DirectoryWorkspace::Open(project.projectRoot_); QVERIFY(source.Succeeded());
    project.directorySnapshot_ = std::make_unique<DirectoryWorkspace>(std::move(*source.value));
    project.LoadMapDocument(); project.ResetEditTracking(); QVERIFY(project.editTrackingReady_);
    BackendState state; activeBackend = &state;
    auto api = modelBackend; api.submitSkinnedModelScene = SkinScene; api.updateSkinMatrices = SkinUpdate;
    api.getSkinStatus = SkinStatus; api.captureSkinnedFrameRgba = SkinCapture;
    WorldModule module(project, &api); auto* page = module.Widget(); state.viewport = module.Viewport();
    page->resize(800, 600); page->show();
    QTRY_VERIFY(module.TerrainOnlySceneReady());
    const auto binding = module.ModelDiagnosticSnapshot()["binding"].toObject();
    QVERIFY(!binding["documentId"].toString().isEmpty() && !binding["sourceSha256"].toString().isEmpty());
    QCOMPARE(binding["generation"].toString().toULongLong(), project.editRevision_.Generation());
    QCOMPARE(binding["revision"].toString().toULongLong(), project.mapEdits_->Revision());

    project.sourceChangeSuspected_ = true;
    QVERIFY(!module.TerrainOnlySceneReady()); QVERIFY(!module.ModelDiagnosticsReady());
    QVERIFY(!module.ModelDiagnosticProgressSnapshot()["captureCurrent"].toBool());
    QVERIFY(module.ModelDiagnosticProgressSnapshot()["unresolvedPlacementCount"].isNull());
    project.sourceChangeSuspected_ = false; QVERIFY(module.TerrainOnlySceneReady());
    project.sourceChangeConfirmed_ = true;
    QVERIFY(!module.TerrainOnlySceneReady()); QVERIFY(!module.ModelDiagnosticsReady());
    project.sourceChangeConfirmed_ = false; QVERIFY(module.TerrainOnlySceneReady());

    // 真正的代际变化先使旧捕获失效，再由Refresh读取新代际，不能借旧空场景通行。
    const auto generation = project.editRevision_.Generation();
    project.ResetEditTracking(); QVERIFY(project.editRevision_.Generation() > generation);
    QVERIFY(!module.TerrainOnlySceneReady());
    module.Refresh(); QVERIFY(!module.TerrainOnlySceneReady());
    module.Refresh(); QVERIFY(!module.TerrainOnlySceneReady());
    QTRY_VERIFY(module.TerrainOnlySceneReady());
    MapData::RegionsDocument regions{5, {{-16, -16, 16, 16, "Owned region", 7, {}, "", {0, 0, 0, 255}}}, {}};
    const auto revision = project.mapEdits_->Revision();
    QVERIFY(project.mapEdits_->EditRegions(regions).Succeeded());
    QVERIFY(project.mapEdits_->Revision() > revision);
    QVERIFY(!module.TerrainOnlySceneReady());
    module.Refresh(); QVERIFY(!module.TerrainOnlySceneReady());
    QTRY_VERIFY(module.TerrainOnlySceneReady());

    // MapKey先切换、文档仍旧的过渡期也必须拒绝；最终接受第二份真实来源。
    project.projectRoot_ = second.path().toStdWString();
    QVERIFY(!module.TerrainOnlySceneReady()); QVERIFY(!module.ViewportForTools());
    source = DirectoryWorkspace::Open(project.projectRoot_); QVERIFY(source.Succeeded());
    project.directorySnapshot_ = std::make_unique<DirectoryWorkspace>(std::move(*source.value));
    project.LoadMapDocument(); project.ResetEditTracking();
    QVERIFY(!module.TerrainOnlySceneReady());
    QTRY_VERIFY(module.TerrainOnlySceneReady());
    QCOMPARE(module.Viewport()->Camera().targetX, 928.0f);
    QVERIFY(!module.ModelDiagnosticsReady());
    QVERIFY(!project.mapEdits_->IsDirty());
    QCOMPARE(project.mapDataUndoStack_->count(), 0);
}

void WorldModuleTests::PlaybackSnapshotKeepsServiceAndRetainedPictureDistinct() {
    QtProject::EditorProject project; BackendState state; activeBackend = &state;
    auto api = modelBackend; api.submitSkinnedModelScene = SkinScene; api.updateSkinMatrices = SkinUpdate;
    api.getSkinStatus = SkinStatus; api.captureSkinnedFrameRgba = SkinCapture;
    WorldModule module(project, &api); auto* page = module.Widget(); state.viewport = module.Viewport();
    QVERIFY(BindTerrain(*state.viewport, TestTerrain())); page->resize(800, 600); page->show();
    Render::AnimationInstanceSpec spec; spec.identity = {1, 8}; spec.hostKey = "snapshot-model"; spec.model.modelPath = "synthetic.mdx";
    const auto service = Render::ModelAnimationService::Create({8, 12, "Qt-playback-snapshot"}, std::span(&spec, 1),
        [](auto, auto) { return Result<Render::AnimationResource>::Success({
            AnimationTest::WithoutSyntheticConstantKmtf(MdxTest::BuildTestMdx()), "synthetic", std::string(64, 'a')}); });
    QVERIFY(service.Succeeded()); const auto first = (*service.value)->Prepare(nullptr); QVERIFY(first.Succeeded());
    bool current = true; auto* player = module.AnimationPlayer();
    QVERIFY(player->Install(*service.value, *first.value, {}, [&] { return current; }));
    const auto matchesButtons = [&] {
        const auto snapshot = module.PlaybackDiagnosticSnapshot()["buttons"].toObject();
        for (const auto& key : {QStringLiteral("play"), QStringLiteral("pause"), QStringLiteral("step")}) {
            const auto* button = page->findChild<QPushButton*>("worldAnimation" + key.left(1).toUpper() + key.mid(1));
            if (!button || snapshot[key].toObject()["effective_enabled"].toBool() != button->isEnabled()) return false;
        }
        return true;
    };
    QVERIFY(player->Ready()); QVERIFY(matchesButtons());
    const auto installed = module.PlaybackDiagnosticSnapshot();
    QVERIFY(installed["player"].toObject()["playing"].toBool());
    QCOMPARE(installed["service_scene"].toObject()["animated_instances"].toString(), QStringLiteral("1"));
    QCOMPARE(installed["accepted_frame"].toObject()["scene_version"].toString(), QString::number((*first.value)->sceneVersion));
    page->findChild<QPushButton*>("worldAnimationPause")->click();
    QVERIFY(!player->Playing()); QVERIFY(matchesButtons()); QVERIFY(page->findChild<QPushButton*>("worldAnimationPlay")->isEnabled());
    current = false; page->findChild<QPushButton*>("worldAnimationPlay")->click();
    const auto failed = module.PlaybackDiagnosticSnapshot();
    QVERIFY(!failed["player"].toObject()["ready"].toBool()); QVERIFY(state.viewport->HasAnimationScene());
    QVERIFY(failed["player"].toObject()["last_diagnostic"].toString().contains("身份已变化"));
    QVERIFY(!failed["accepted_frame"].toObject()["present"].toBool()); QVERIFY(matchesButtons());
    for (const auto* key : {"play", "pause", "step"}) QVERIFY(!failed["buttons"].toObject()[key].toObject()["effective_enabled"].toBool());
}

void WorldModuleTests::InstanceCatalogStartsBesidePose_data() {
    QTest::addColumn<bool>("cancelAndReplace");
    QTest::addColumn<bool>("planBeforePose");
    QTest::addColumn<QString>("prefetchDisposition");
    QTest::newRow("latest-pose-and-next-roster") << false << false << QString{};
    QTest::newRow("clear-replace-and-late-catalog") << true << false << QString{};
    QTest::newRow("ready-roster-overlaps-accepted-pose-notification") << false << true << QString{};
    QTest::newRow("prefetch-completed-before-ui-dispatch") << false << false << QStringLiteral("consume");
    QTest::newRow("prefetch-cancel-completed-before-ui-dispatch") << false << false << QStringLiteral("cancel");
    QTest::newRow("prefetch-invalidate-completed-before-ui-dispatch") << false << false << QStringLiteral("invalidate");
    QTest::newRow("prefetch-replace-variant-before-ui-dispatch") << false << false << QStringLiteral("variant");
}
void WorldModuleTests::InstanceCatalogStartsBesidePose() {
    QFETCH(bool, cancelAndReplace);
    QFETCH(bool, planBeforePose);
    QFETCH(QString, prefetchDisposition);
    QTemporaryDir directory; QVERIFY(directory.isValid());
    const auto mapPath = directory.filePath(QStringLiteral("catalog-scheduling.w3x"));
    auto members = PlacementPreviewMembers();
    // 此夹具验证目录/预取调度；保留 TEXS0 的真实材质模式，并由后端共用 Copy 校验完整负载。
    const auto skeletonOnly = AnimationTest::WithoutSyntheticConstantKmtf(MdxTest::BuildTestMdx());
    for (auto& member : members) if (member.name == "Synthetic\\Arcane.mdx") member.data = skeletonOnly;
    if (!prefetchDisposition.isEmpty()) members.push_back({"Synthetic\\Prefetched.mdx", skeletonOnly});
    QVERIFY(WriteVerifiedArchive(mapPath.toStdWString(), members).Succeeded());
    auto archive = MapArchiveSession::Open(mapPath.toStdWString()); QVERIFY(archive.Succeeded());
    QtProject::EditorProject project; project.sourceArchivePath_ = mapPath.toStdWString();
    project.archiveSession_ = std::make_unique<MapArchiveSession>(std::move(*archive.value));
    const bool trackedMovement = !cancelAndReplace && !planBeforePose && prefetchDisposition.isEmpty();
    std::optional<MapData::PlacementId> trackedPlacement;
    if (trackedMovement) {
        trackedPlacement = AttachTrackedResourcePlacement(project); QVERIFY(trackedPlacement);
        const auto resourceBaseline = project.editRevision_.Baseline(EditSaveTarget::Map); QVERIFY(resourceBaseline);
        QVERIFY(resourceBaseline->scopes.contains("source"));
        QCOMPARE(resourceBaseline->scopes.at("source"), "archive:" + project.archiveSession_->SourceSha256());
        QVERIFY(resourceBaseline->scopes.contains("map/war3mapUnits.doo"));
    }
    const auto captured = CaptureWorldTerrainSources(project); QVERIFY(captured.Succeeded());
    auto opened = WorldResourceReader::Open(*captured.value); QVERIFY(opened.Succeeded());
    auto source = std::shared_ptr<WorldResourceReader>(std::move(*opened.value));
    BackendState state; activeBackend = &state;
    auto api = modelBackend; api.submitSkinnedModelScene = SkinScene; api.updateSkinMatrices = SkinUpdate;
    api.getSkinStatus = SkinStatus; api.captureSkinnedFrameRgba = SkinCapture;
    EditorPresentationFixture<BackendState>::Enable(api, state.editor);
    api.submitEditorInstanceRoster = SubmitRoster; api.getEditorInstanceRosterResult = RosterResult;
    api.getEditorRosterCapabilities = [](War3RendererHandle, std::uint32_t* flags) -> std::int32_t {
        if (!flags) return 1;
        *flags = WAR3_RENDER_ROSTER_APPEND_ASSETS; return 0;
    };
    WorldViewport viewport(nullptr, &api); state.viewport = &viewport;
    viewport.resize(320, 240); QVERIFY(BindTerrain(viewport, TestTerrain())); viewport.show();
    QTRY_VERIFY(viewport.RendererReady() && state.frames > 0);
    WorldAnimationPlayer player(viewport); player.SetPlaying(false);
    Render::AnimationInstanceSpec spec; spec.identity = {1, 77}; spec.hostKey = "catalog-survivor";
    spec.model.modelPath = "Synthetic\\Arcane.mdx"; spec.elapsedMs = 0;
    const auto install = [&](std::uint64_t revision) {
        const auto service = Render::ModelAnimationService::Create({spec.identity.generation, revision, captured.value->archiveSha},
            std::span(&spec, 1), [&](auto path, auto budget) { return source->ReadAnimationResource(path, budget); });
        if (!service.Succeeded()) return false;
        Render::AnimationAdvance advance; advance.playAll = false; advance.worldView = viewport.AnimationViewSnapshot();
        const auto first = (*service.value)->Prepare(nullptr, advance);
        return first.Succeeded() && player.Install(*service.value, *first.value, source, [] { return true; });
    };
    QVERIFY(install(12)); QVERIFY(!player.Busy());
    QVERIFY(state.skinPose && state.skinPose->dynamicMaterialStates && state.skinPose->dynamicMaterialAlphas);
    QVERIFY(!state.skinPose->materialStates.empty() && !state.skinPose->layerAlphas.empty());
    const auto originalScene = player.Scene()->sceneVersion;
    auto gate = std::make_shared<CatalogWorkerGate>(); ReleaseCatalogWorker release{gate};
    struct Completion { int calls = 0; bool accepted = false; War3Map::Status status; };
    auto completion = std::make_shared<Completion>();
    const auto makeChange = [&](std::uint64_t revision, std::shared_ptr<Completion> result) {
        WorldAnimationPlayer::InstanceChange change;
        change.delta.session = {spec.identity.generation, revision, captured.value->archiveSha};
        change.timing.generation = spec.identity.generation; change.timing.editSessionIdentity = 2307;
        change.timing.documentRevision = revision; change.timing.commandIdentity = revision + 1000;
        change.sources = *captured.value; change.valid = [] { return true; };
        change.completed = [result](bool accepted, const War3Map::Status& status) {
            ++result->calls; result->accepted = accepted; result->status = status;
        };
        return change;
    };
    auto change = makeChange(13, completion);
    auto added = spec; added.identity.value = 2; added.hostKey = "catalog-added";
    WorldInstanceTiming completedPrefetch;
    auto firstInstanceReader = std::make_shared<std::shared_ptr<WorldResourceReader>>();
    if (!prefetchDisposition.isEmpty()) {
        // 冷模型不属于原目录，不能用已驻留模型的NOT_NEEDED路径假装预取命中。
        added.model.modelPath = "Synthetic\\Prefetched.mdx";
        auto prefetchValid = std::make_shared<bool>(true);
        const auto valid = [prefetchValid] { return *prefetchValid; };
        const auto framesBeforePrefetch = state.frames;
        const auto contentBeforePrefetch = player.Frame()->contentVersion;
        player.PrefetchPlacementAsset(added.model, *captured.value, valid);
        QElapsedTimer elapsed; elapsed.start();
        do {
            completedPrefetch = player.PlacementAssetPrefetchTiming();
            if (completedPrefetch.prefetchFinishedNs) break;
            QThread::msleep(1); // 不泵Qt：finished回调必须仍在UI队列中，worker结果已经独立发布。
        } while (elapsed.elapsed() < 2000);
        QVERIFY2(completedPrefetch.prefetchFinishedNs, "formal prefetch worker did not finish within the bounded no-UI-dispatch wait");
        QVERIFY(completedPrefetch.prefetchRequestedNs && completedPrefetch.prefetchStartedNs);
        QVERIFY(completedPrefetch.prefetchRequestedNs <= completedPrefetch.prefetchStartedNs);
        QVERIFY(completedPrefetch.prefetchStartedNs <= completedPrefetch.prefetchFinishedNs);
        QCOMPARE(completedPrefetch.prefetchState, std::string("ready"));
        QCOMPARE(state.frames, framesBeforePrefetch);
        QCOMPARE(player.Frame()->contentVersion, contentBeforePrefetch);
        // 只读查询不能消费/取消结果；正式QueueInstances仍必须拿到同一份已完成的静态包。
        QCOMPARE(player.PlacementAssetPrefetchTiming().prefetchFinishedNs, completedPrefetch.prefetchFinishedNs);
        if (prefetchDisposition == QStringLiteral("cancel")) {
            player.CancelAssetPrefetch();
            QCOMPARE(player.PlacementAssetPrefetchTiming().prefetchRequestedNs, std::uint64_t{0});
        } else if (prefetchDisposition == QStringLiteral("invalidate")) {
            *prefetchValid = false;
            QCOMPARE(player.PlacementAssetPrefetchTiming().prefetchState, std::string("stale"));
        } else if (prefetchDisposition == QStringLiteral("variant")) {
            // 旧worker已经完成但其UI清理回调未跑，新变体仍排队；不得把旧包当成新包。
            added.model.teamColor = 5;
            player.PrefetchPlacementAsset(added.model, *captured.value, valid);
            const auto replacement = player.PlacementAssetPrefetchTiming();
            QVERIFY(replacement.prefetchRequestedNs >= completedPrefetch.prefetchFinishedNs);
            QCOMPARE(replacement.prefetchFinishedNs, std::uint64_t{0});
        }
        QCOMPARE(state.frames, framesBeforePrefetch);
    }
    change.delta.additions.push_back(added);
    change.prepareOwnedAssets = [gate, firstInstanceReader](auto&, auto reader, auto&) {
        *firstInstanceReader = std::move(reader);
        auto* thread = QThread::currentThread();
        QObject::connect(thread, &QThread::finished, thread, [gate] { gate->finished.release(); }, Qt::DirectConnection);
        gate->entered.release(); gate->release.acquire(); return War3Map::Status::Ok();
    };
    const auto operationIdentity = change.timing;
    const auto beforeEdit = player.LatestTimingSequence();
    std::unique_ptr<WorldAnimationPlaybackContractChecks::ReadyRosterNotificationProbe> notification;
    if (planBeforePose) notification = std::make_unique<WorldAnimationPlaybackContractChecks::ReadyRosterNotificationProbe>(player);
    bool intercepted = false, queued = false, startedBesidePose = false, catalogFinished = false;
    const auto connection = connect(&player, &WorldAnimationPlayer::Changed, &player, [&] {
        if (intercepted || !player.Busy()) return;
        intercepted = true;
        // Changed在旧pose worker启动之前同步发出；此时等待资源worker的真执行信号，
        // 不处理GUI事件，因而旧pose完成回调不可能先替我们释放调度门。
        queued = player.QueueInstances(std::move(change));
        if (queued) startedBesidePose = gate->entered.tryAcquire(1, 2000);
        if (planBeforePose && startedBesidePose) {
            gate->release.release();
            catalogFinished = gate->finished.tryAcquire(1, 2000);
            // catalog先完成并消费其plan，旧pose线程尚未start；不靠睡眠碰巧制造次序。
            if (catalogFinished) QCoreApplication::sendPostedEvents(&player, QEvent::MetaCall);
        }
    });
    Render::AnimationEdit edit; edit.identity = spec.identity; edit.elapsedMs = 500; edit.globalMs = 500;
    player.Edit(std::move(edit)); disconnect(connection);
    QVERIFY(intercepted && queued); QVERIFY(startedBesidePose);
    if (planBeforePose) QVERIFY(catalogFinished);
    // 排队后UI补录目录耗时，不允许旧会话/旧修订把统计写到另一次实例事务。
    auto staleIdentity = operationIdentity; ++staleIdentity.editSessionIdentity;
    QVERIFY(!player.RecordInstanceOutlinerTiming(staleIdentity, 9000));
    staleIdentity = operationIdentity; ++staleIdentity.documentRevision;
    QVERIFY(!player.RecordInstanceOutlinerTiming(staleIdentity, 9000));
    QVERIFY(player.RecordInstanceOutlinerTiming(operationIdentity, 1234567));
    if (!planBeforePose) {
        QTRY_VERIFY(!player.Busy());
        QCOMPARE(player.Frame()->states[0].elapsedMs, std::uint64_t{500});
        QCOMPARE(state.rosterSubmits, std::uint64_t{0}); // catalog等待期间旧pose已被接受，尚未重基/提交名单。
        QVERIFY(player.InstancesBusy());
    }
    if (cancelAndReplace) {
        player.Clear(); QCOMPARE(completion->calls, 1); QVERIFY(!completion->accepted);
        QVERIFY(!completion->status.diagnostics.empty());
        QCOMPARE(completion->status.diagnostics.front().code, std::string("WORLD_INSTANCE_CANCELLED"));
        QVERIFY(!player.Ready()); QVERIFY(player.InstancesBusy()); // 被取消worker仍持有自己的来源和档案。
        spec.identity.generation = 88;
        QVERIFY(install(100)); QVERIFY(player.Scene()->sceneVersion != originalScene);
    }
    const auto acceptedBeforeCatalog = player.LatestTimingSequence();
    if (!planBeforePose) {
        gate->release.release();
        // finished直连运行于资源线程，确保其函数已经退出，但故意不处理GUI完成回调。
        // 在这个窄窗口再次排入姿势编辑，必须等名单重基，不能多求值一遍旧场景。
        QVERIFY(gate->finished.tryAcquire(1, 2000));
    }
    if (!cancelAndReplace && !planBeforePose) {
        QSignalSpy startedPose(&player, &WorldAnimationPlayer::Changed);
        Render::AnimationEdit pendingEdit; pendingEdit.identity = spec.identity;
        pendingEdit.elapsedMs = 500; pendingEdit.globalMs = 500;
        player.Edit(std::move(pendingEdit));
        QCOMPARE(startedPose.count(), 0);
    }
    QTRY_VERIFY(!player.InstancesBusy());
    QTRY_VERIFY(!player.Busy());
    QCOMPARE(completion->calls, 1);
    const auto instanceHistory = player.InstanceTimingHistory();
    QCOMPARE(instanceHistory.size(), std::size_t{1});
    QCOMPARE(instanceHistory.front().refresh.outlinerNs, std::uint64_t{1234567});
    QVERIFY(instanceHistory.front().refresh.outlinerAfterQueue);
    if (!prefetchDisposition.isEmpty()) {
        const auto& timing = instanceHistory.front();
        const bool expectedHit = prefetchDisposition == QStringLiteral("consume");
        QCOMPARE(timing.catalogDetail.preparedAssetHit, expectedHit);
        QCOMPARE(timing.resourceReaderReused, expectedHit);
        QVERIFY(completedPrefetch.prefetchFinishedNs <= timing.queuedNs);
        if (expectedHit) {
            QCOMPARE(timing.prefetchState, std::string("ready"));
            QCOMPARE(timing.prefetchRequestedNs, completedPrefetch.prefetchRequestedNs);
            QCOMPARE(timing.prefetchStartedNs, completedPrefetch.prefetchStartedNs);
            QCOMPARE(timing.prefetchFinishedNs, completedPrefetch.prefetchFinishedNs);
            QVERIFY(timing.catalogDetail.preparedAssetMissReason.empty());
        }
        // 取消、失效和变体切换均走正常正式准备，旧queued finished不能重新发布被丢弃的包。
        std::string failure = timing.diagnosticCode;
        for (const auto& diagnostic : completion->status.diagnostics)
            failure += "\n" + diagnostic.code + ": " + diagnostic.message;
        QVERIFY2(timing.accepted, failure.c_str());
        QVERIFY(timing.assetsChanged);
        QCOMPARE(state.rosterAssetAppends, std::uint64_t{1});
        QCOMPARE(state.rosterAssets.modelCount, std::uint32_t{2});
        QCOMPARE(state.roster.assetIdentity, state.rosterAssetIdentity);
        QCOMPARE(player.Scene()->specs[0].hostKey, std::string("catalog-survivor"));
        QCOMPARE(player.Scene()->specs[1].model.modelPath, added.model.modelPath);
        QCOMPARE(player.Scene()->specs[1].model.teamColor, added.model.teamColor);
        QCOMPARE(player.Frame()->states[0].identity.value, std::uint64_t{1});
        QCOMPARE(player.Frame()->states[0].elapsedMs, std::uint64_t{500});
    }
    if (notification) {
        notification->VerifyOverlap(instanceHistory.front());
        const auto attempts = player.TimingHistory(beforeEdit);
        QVERIFY(std::any_of(attempts.begin(), attempts.end(), [&](const auto& attempt) {
            return attempt.sceneVersion == originalScene && attempt.contentVersion == notification->AcceptedContent()
                && attempt.outcome == WorldAnimationTimingOutcome::Accepted;
        })); // 允许合法旧pose先接收；它的最新500ms状态才是名单重基来源。
    }
    // 同步目录通知可重入完成回调；终态补录也只更新同一身份的历史，不能污染下一笔。
    QVERIFY(player.RecordInstanceOutlinerTiming(operationIdentity, 11));
    QCOMPARE(player.InstanceTimingHistory().front().refresh.outlinerNs, std::uint64_t{1234578});
    if (cancelAndReplace) {
        QVERIFY(!completion->accepted); QCOMPARE(state.rosterSubmits, std::uint64_t{0});
        QCOMPARE(player.Scene()->documentRevision, std::uint64_t{100});
        QCOMPARE(player.Frame()->states.size(), std::size_t{1});
        QCOMPARE(player.Frame()->states[0].identity.generation, std::uint64_t{88});
    } else {
        QVERIFY2(completion->accepted, completion->status.diagnostics.empty() ? "no roster completion" : completion->status.diagnostics.front().message.c_str());
        QCOMPARE(player.Scene()->documentRevision, std::uint64_t{13});
        QCOMPARE(player.Frame()->states.size(), std::size_t{2});
        QCOMPARE(player.Frame()->states[0].elapsedMs, std::uint64_t{500}); // 重基必须使用catalog准备期间接受的最新帧。
        QCOMPARE(state.rosterSubmits, std::uint64_t{1});
        const auto attempts = player.TimingHistory(acceptedBeforeCatalog);
        QVERIFY(!attempts.empty());
        for (const auto& attempt : attempts) {
            if (planBeforePose && attempt.sceneVersion == originalScene) continue;
            QCOMPARE(attempt.sceneVersion, player.Scene()->sceneVersion);
        }
        if (planBeforePose) {
            Render::AnimationEdit nextPose; nextPose.identity = spec.identity;
            nextPose.elapsedMs = 600; nextPose.globalMs = 600;
            player.Edit(std::move(nextPose));
            QTRY_VERIFY(!player.Busy() && player.Frame()->states[0].elapsedMs == 600);
        }
    }
    // 成功roster和Clear后替换都须发布各自的新档案；旧revision/旧代次不能用于下一笔。
    auto nextCompletion = std::make_shared<Completion>();
    auto next = makeChange(cancelAndReplace ? 101 : 14, nextCompletion);
    if (trackedMovement) {
        const auto revisionBeforeMove = project.editRevision_.Revision();
        const MapData::PlacementMove move{*trackedPlacement, {32, 0, 0}, {}};
        QVERIFY(MapData::MovePlacements(*project.mapEdits_, std::span(&move, 1)).Succeeded());
        QVERIFY(project.ObserveMapDataEdit("移动真实夹具单位"));
        QVERIFY(project.editRevision_.Revision() > revisionBeforeMove);
        const auto afterMove = CaptureWorldTerrainSources(project); QVERIFY(afterMove.Succeeded());
        QVERIFY(SameWorldTerrainSources(*captured.value, *afterMove.value));
        next.sources = *afterMove.value;
        auto movedRequest = spec.model; movedRequest.position = {32, 0, 0};
        Render::AnimationEdit edit; edit.identity = spec.identity; edit.transform = Render::ModelInstanceTransform(movedRequest);
        next.delta.edits.push_back(std::move(edit));
    }
    auto nextInstanceReader = std::make_shared<std::shared_ptr<WorldResourceReader>>();
    next.prepareOwnedAssets = [nextInstanceReader](auto&, auto reader, auto&) {
        *nextInstanceReader = std::move(reader); return War3Map::Status::Ok();
    };
    if (cancelAndReplace) {
        added.identity.generation = spec.identity.generation;
        next.delta.additions.push_back(added);
    } else next.delta.removals.push_back(added.identity);
    QVERIFY(player.QueueInstances(std::move(next)));
    QTRY_COMPARE(nextCompletion->calls, 1);
    QVERIFY2(nextCompletion->accepted, nextCompletion->status.diagnostics.empty() ? "no next roster completion" : nextCompletion->status.diagnostics.front().message.c_str());
    QCOMPARE(player.InstanceTimingHistory().back().refresh.outlinerNs, std::uint64_t{0});
    QCOMPARE(player.InstanceTimingHistory().back().resourceReaderReused, !cancelAndReplace);
    QVERIFY(*firstInstanceReader && *nextInstanceReader);
    QCOMPARE(*firstInstanceReader == *nextInstanceReader, !cancelAndReplace);
    QCOMPARE(player.Scene()->documentRevision, std::uint64_t{cancelAndReplace ? 101u : 14u});
    QCOMPARE(player.Frame()->states.size(), std::size_t{cancelAndReplace ? 2u : 1u});
    // 同一归档SHA仍不足以复用解析器：主地形/覆盖配置变化必须取得独立的新reader。
    auto changedSourceCompletion = std::make_shared<Completion>();
    auto changedSource = makeChange(cancelAndReplace ? 102 : 15, changedSourceCompletion);
    changedSource.sources.mainTileset = static_cast<std::uint8_t>(
        changedSource.sources.mainTileset == static_cast<std::uint8_t>('L') ? 'A' : 'L');
    QVERIFY(!(*nextInstanceReader)->MatchesSources(changedSource.sources));
    auto changedInstanceReader = std::make_shared<std::shared_ptr<WorldResourceReader>>();
    changedSource.prepareOwnedAssets = [changedInstanceReader](auto&, auto reader, auto&) {
        *changedInstanceReader = std::move(reader); return War3Map::Status::Ok();
    };
    QVERIFY(player.QueueInstances(std::move(changedSource)));
    QTRY_COMPARE(changedSourceCompletion->calls, 1);
    QVERIFY(changedSourceCompletion->accepted);
    QVERIFY(!player.InstanceTimingHistory().back().resourceReaderReused);
    QVERIFY(*changedInstanceReader && *changedInstanceReader != *nextInstanceReader);
    if (trackedMovement) for (const bool changeGeneration : {false, true}) {
        // 资源可复用不授予旧地图任务发布资格：阻住真实worker后改变原任务绑定。
        const auto beforeSources = CaptureWorldTerrainSources(project); QVERIFY(beforeSources.Succeeded());
        const auto generation = project.editRevision_.Generation(), revision = project.mapEdits_->Revision();
        const auto sceneBefore = player.Scene()->sceneVersion;
        auto staleCompletion = std::make_shared<Completion>();
        auto stale = makeChange(player.Scene()->documentRevision + 1, staleCompletion);
        stale.sources = *beforeSources.value;
        stale.valid = [&project, generation, revision] {
            return project.editRevision_.Generation() == generation && project.mapEdits_->Revision() == revision;
        };
        auto staleGate = std::make_shared<CatalogWorkerGate>(); ReleaseCatalogWorker releaseStale{staleGate};
        stale.prepareOwnedAssets = [staleGate](auto&, auto, auto&) {
            staleGate->entered.release(); staleGate->release.acquire(); return War3Map::Status::Ok();
        };
        QVERIFY(player.QueueInstances(std::move(stale)));
        QVERIFY(staleGate->entered.tryAcquire(1, 2000));
        if (changeGeneration) {
            project.ResetEditTracking(); QVERIFY(project.editTrackingReady_);
            QVERIFY(project.editRevision_.Generation() != generation);
        } else {
            const MapData::PlacementMove move{*trackedPlacement, {64, 0, 0}, {}};
            QVERIFY(MapData::MovePlacements(*project.mapEdits_, std::span(&move, 1)).Succeeded());
            QVERIFY(project.ObserveMapDataEdit("使旧worker地图修订过期"));
            QVERIFY(project.mapEdits_->Revision() != revision);
        }
        const auto afterChange = CaptureWorldTerrainSources(project); QVERIFY(afterChange.Succeeded());
        QVERIFY(SameWorldTerrainSources(*beforeSources.value, *afterChange.value));
        staleGate->release.release();
        QTRY_COMPARE(staleCompletion->calls, 1);
        QVERIFY(!staleCompletion->accepted);
        QVERIFY(!staleCompletion->status.diagnostics.empty());
        QCOMPARE(staleCompletion->status.diagnostics.front().code, std::string("WORLD_INSTANCE_SOURCE_STALE"));
        QCOMPARE(player.Scene()->sceneVersion, sceneBefore);
        QTRY_VERIFY(!player.InstancesBusy());
    }
    player.Clear(); QVERIFY(!player.Ready()); QVERIFY(!player.Busy()); QVERIFY(!player.InstancesBusy());
}

void WorldModuleTests::AmbiguousTerrainAndDeferredLoad() {
    QTemporaryDir directory; QVERIFY(directory.isValid());
    QVERIFY(QDir(directory.path()).mkpath("map"));
    QVERIFY(Write(directory.filePath("war3map.w3e"), TerrainBytes()));
    QVERIFY(Write(directory.filePath("map/war3map.w3e"), TerrainBytes(512)));
    QtProject::EditorProject project; project.projectRoot_ = directory.path().toStdWString();
    auto snapshot = DirectoryWorkspace::Open(project.projectRoot_); QVERIFY(snapshot.Succeeded());
    project.directorySnapshot_ = std::make_unique<DirectoryWorkspace>(std::move(*snapshot.value));
    BackendState state; activeBackend = &state;
    WorldModule module(project, &backend);
    auto* page = module.Widget(); state.viewport = module.Viewport();
    project.MapDocumentChanged(); QTest::qWait(50);
    QCOMPARE(module.Viewport()->VertexCount(), std::size_t{0}); QCOMPARE(state.creates, 0);
    QCOMPARE(module.PlaybackDiagnosticSnapshot()["terrain_load"].toObject()["stage"].toString(),
        QStringLiteral("deferred-hidden"));
    page->show();
    QTRY_VERIFY(page->findChild<QLabel*>("worldTerrainStatus")->text().contains(QStringLiteral("来源不明确")));
    QCOMPARE(module.Viewport()->VertexCount(), std::size_t{0});
    const auto failed = module.PlaybackDiagnosticSnapshot()["terrain_load"].toObject();
    QVERIFY(failed["failed"].toBool());
    QCOMPARE(failed["stage"].toString(), QStringLiteral("source-read-failed"));
    QCOMPARE(failed["diagnostics"].toArray().first().toObject()["code"].toString(), QStringLiteral("MAP_MEMBER_UNAVAILABLE"));
}

void WorldModuleTests::OversizedViewportDoesNotCreate() {
    BackendState state; activeBackend = &state;
    WorldViewport viewport(nullptr, &backend); state.viewport = &viewport;
    viewport.resize(4200, 240); viewport.show();
    QTest::qWait(80); QCOMPARE(state.creates, 0); QVERIFY(!viewport.FrameTimerActive());
    QVERIFY(viewport.StatusText().contains("4096"));
    viewport.resize(320, 240);
    QTRY_COMPARE(state.creates, 1);
    QTRY_VERIFY(state.frames > 0);
}

void WorldModuleTests::HoverClickAndPendingFrames() {
    BackendState state; activeBackend = &state;
    WorldViewport viewport(nullptr, &backend); state.viewport = &viewport;
    viewport.resize(320, 240);
    QVERIFY(BindTerrain(viewport, TestTerrain()));
    viewport.show(); QTRY_VERIFY(state.frames > 0);
    const QPointF point(viewport.width() / 4.0, viewport.height() / 2.0);
    // 鼠标事件只更新最新点；这一批不能每次都读取回执/求交。
    const auto reads = state.viewReads;
    for (int i = 0; i < 50; ++i) Mouse(viewport, QEvent::MouseMove, {10.0 + i, 50}, Qt::NoButton, Qt::NoButton);
    Mouse(viewport, QEvent::MouseMove, point, Qt::NoButton, Qt::NoButton);
    QCOMPARE(state.viewReads, reads);
    QTRY_VERIFY(viewport.HoverCoordinate().has_value());
    QCOMPARE(viewport.HoverCoordinate()->tileIndex, std::size_t{0});
    QVERIFY(std::abs(viewport.HoverCoordinate()->position[0] + 64) < 1e-5);
    QVERIFY(std::abs(viewport.HoverCoordinate()->position[1]) < 1e-5);
    QVERIFY(std::abs(viewport.HoverCoordinate()->position[2] - 2) < 1e-5);
    const auto camera = viewport.SaveCamera();
    Mouse(viewport, QEvent::MouseButtonPress, point, Qt::LeftButton, Qt::LeftButton);
    Mouse(viewport, QEvent::MouseMove, point + QPointF(1, 1), Qt::NoButton, Qt::LeftButton);
    Mouse(viewport, QEvent::MouseButtonRelease, point + QPointF(1, 1), Qt::LeftButton, Qt::NoButton);
    QVERIFY(viewport.SelectedCoordinate().has_value());
    QCOMPARE(viewport.SaveCamera(), camera); // 短点不能先产生微小旋转。
    QVERIFY(viewport.SelectionText().startsWith(QStringLiteral("已选：X")));
    QEvent leave(QEvent::Leave); QApplication::sendEvent(&viewport, &leave);
    QVERIFY(!viewport.HoverCoordinate()); QVERIFY(viewport.SelectedCoordinate().has_value());
    QTest::keyClick(&viewport, Qt::Key_Escape); QVERIFY(!viewport.SelectedCoordinate());
    Mouse(viewport, QEvent::MouseButtonPress, point, Qt::LeftButton, Qt::LeftButton);
    Mouse(viewport, QEvent::MouseButtonRelease, point + QPointF(60, 40), Qt::LeftButton, Qt::NoButton);
    QVERIFY(!viewport.SelectedCoordinate()); // 缺失中间 move 事件也不能将远处松开当短点。

    Mouse(viewport, QEvent::MouseMove, point, Qt::NoButton, Qt::NoButton);
    QTRY_VERIFY(viewport.HoverCoordinate().has_value());
    state.pending = true;
    QTRY_VERIFY(!viewport.HoverCoordinate() && viewport.HoverText().contains(QStringLiteral("等待完成帧")));
    Mouse(viewport, QEvent::MouseButtonPress, point, Qt::LeftButton, Qt::LeftButton);
    Mouse(viewport, QEvent::MouseButtonRelease, point, Qt::LeftButton, Qt::NoButton);
    QVERIFY(!viewport.SelectedCoordinate());
    state.pending = false;
    QTRY_VERIFY(viewport.HoverCoordinate().has_value());
    state.staleRevision = true;
    QTRY_VERIFY(!viewport.HoverCoordinate() && viewport.HoverText().contains(QStringLiteral("不匹配")));
    state.staleRevision = false;
    QTRY_VERIFY(viewport.HoverCoordinate().has_value());
    state.staleSize = true;
    QTRY_VERIFY(!viewport.HoverCoordinate() && viewport.HoverText().contains(QStringLiteral("不匹配")));
    state.staleSize = false;
    QTRY_VERIFY(viewport.HoverCoordinate().has_value());

    Mouse(viewport, QEvent::MouseButtonPress, point, Qt::LeftButton, Qt::LeftButton);
    ++state.viewEpoch; Frame(&state); // 模拟按下/松开之间已有另一完成视角。
    Mouse(viewport, QEvent::MouseButtonRelease, point, Qt::LeftButton, Qt::NoButton);
    QVERIFY(!viewport.SelectedCoordinate()); QVERIFY(viewport.HoverText().contains(QStringLiteral("取消")));
    Mouse(viewport, QEvent::MouseButtonPress, point, Qt::LeftButton, Qt::LeftButton, Qt::AltModifier);
    Mouse(viewport, QEvent::MouseMove, point + QPointF(40, 20), Qt::NoButton, Qt::LeftButton, Qt::AltModifier);
    QVERIFY(!viewport.HoverCoordinate());
    Mouse(viewport, QEvent::MouseButtonRelease, point + QPointF(40, 20), Qt::LeftButton, Qt::NoButton, Qt::AltModifier);
    QVERIFY(viewport.SaveCamera()["yaw"] != camera["yaw"]); QVERIFY(!viewport.SelectedCoordinate());
    QTRY_VERIFY(viewport.HoverCoordinate().has_value());
    state.viewResult = 7;
    QTRY_VERIFY(!viewport.HoverCoordinate() && viewport.HoverText().contains(QStringLiteral("暂停")));
    state.viewResult = 0;
    QTRY_VERIFY(viewport.HoverCoordinate().has_value());
    state.statusResult = 7;
    QTRY_VERIFY(!viewport.FrameTimerActive() && !viewport.HoverCoordinate() && !viewport.SelectedCoordinate());
    QCOMPARE(state.captures, 0);
}

void WorldModuleTests::ResizeDprAndLegacyFallback() {
    const auto terrain = TestTerrain(); QVERIFY(terrain != nullptr);
    {
        BackendState state; activeBackend = &state;
        WorldViewport viewport(nullptr, &backend); state.viewport = &viewport;
        viewport.resize(321, 241); QVERIFY(BindTerrain(viewport, terrain)); viewport.show();
        QTRY_VERIFY(state.frames > 0);
        QVERIFY(viewport.devicePixelRatioF() > 1); // 本测试进程使用真实 Qt 1.25 倍缩放。
        QCOMPARE(state.width, static_cast<std::uint32_t>(qRound(viewport.width() * viewport.devicePixelRatioF())));
        const auto center = [&] { return QPointF(state.width * 0.5 / viewport.devicePixelRatioF(), state.height * 0.5 / viewport.devicePixelRatioF()); };
        Mouse(viewport, QEvent::MouseMove, center(), Qt::NoButton, Qt::NoButton);
        QTRY_VERIFY(viewport.HoverCoordinate().has_value());
        QVERIFY(std::abs(viewport.HoverCoordinate()->position[0]) < 1e-5);
        QVERIFY(std::abs(viewport.HoverCoordinate()->position[1]) < 1e-5);
        state.autoComplete = false;
        viewport.resize(423, 277);
        QVERIFY(!viewport.HoverCoordinate());
        Mouse(viewport, QEvent::MouseMove, center(), Qt::NoButton, Qt::NoButton);
        QTRY_VERIFY(viewport.HoverText().contains(QStringLiteral("等待完成帧")));
        state.autoComplete = true; Frame(&state);
        QTRY_VERIFY(viewport.HoverCoordinate().has_value());
        QVERIFY(std::abs(viewport.HoverCoordinate()->position[0]) < 1e-5);
        state.autoComplete = false;
        const auto surface = state.surfaceEpoch;
        QEvent dprChanged(QEvent::DevicePixelRatioChange); QApplication::sendEvent(&viewport, &dprChanged);
        QVERIFY(state.surfaceEpoch > surface); QVERIFY(!viewport.HoverCoordinate());
        QTRY_VERIFY(viewport.HoverText().contains(QStringLiteral("等待完成帧")));
        state.autoComplete = true; Frame(&state);
        QTRY_VERIFY(viewport.HoverCoordinate().has_value());
        Mouse(viewport, QEvent::MouseButtonPress, center(), Qt::LeftButton, Qt::LeftButton);
        Mouse(viewport, QEvent::MouseButtonRelease, center(), Qt::LeftButton, Qt::NoButton);
        QVERIFY(viewport.SelectedCoordinate().has_value());
        QVERIFY(std::abs(viewport.SelectedCoordinate()->position[0]) < 1e-5);
        QCOMPARE(state.captures, 0);
    }
    {
        BackendState state; activeBackend = &state;
        WorldViewport viewport(nullptr, &legacyBackend); state.viewport = &viewport;
        viewport.resize(320, 240); QVERIFY(BindTerrain(viewport, terrain)); viewport.show();
        QTRY_VERIFY(state.frames > 0);
        Mouse(viewport, QEvent::MouseMove, {160, 120}, Qt::NoButton, Qt::NoButton);
        QTest::qWait(80);
        QVERIFY(viewport.FrameTimerActive() && viewport.VertexCount() == 6);
        QVERIFY(!viewport.HoverCoordinate() && !viewport.SelectedCoordinate());
        QVERIFY(viewport.HoverText().contains(QStringLiteral("缺少完成帧回执")));
        QCOMPARE(state.viewReads, 0); QCOMPARE(state.captures, 0);
    }
}

void WorldModuleTests::SimplifiedAndMismatchedPickingDisabled() {
    BackendState state; activeBackend = &state;
    WorldViewport viewport(nullptr, &backend); state.viewport = &viewport;
    viewport.resize(320, 240);
    QVERIFY(BindTerrain(viewport, FlatGrid(513, 513), 14)); viewport.show();
    QTRY_VERIFY(state.frames > 0);
    Mouse(viewport, QEvent::MouseMove, {160, 120}, Qt::NoButton, Qt::NoButton);
    Mouse(viewport, QEvent::MouseButtonPress, {160, 120}, Qt::LeftButton, Qt::LeftButton);
    Mouse(viewport, QEvent::MouseButtonRelease, {160, 120}, Qt::LeftButton, Qt::NoButton);
    QVERIFY(!viewport.HoverCoordinate() && !viewport.SelectedCoordinate());
    QVERIFY(viewport.HoverText().contains(QStringLiteral("精确拾取")));
    QCOMPARE(state.viewReads, 0);
    const auto terrain = TestTerrain(); QVERIFY(terrain != nullptr);
    QVERIFY(BindTerrain(viewport, terrain, 14));
    auto changed = std::make_shared<MapData::TerrainGridSnapshot>(*terrain);
    changed->vertices[0].position[2] += 10;
    QVERIFY(!viewport.SetPickingTerrain(changed, 1, 14));
    QVERIFY(viewport.HoverText().contains(QStringLiteral("不一致")));
    QVERIFY(!viewport.SetPickingTerrain(terrain, 1, 13));
    QVERIFY(!viewport.HoverCoordinate() && !viewport.SelectedCoordinate());
    QCOMPARE(state.captures, 0);
}

void WorldModuleTests::GroundSceneKeepsPickingAndRejectsDifferentGeometry() {
    BackendState state; activeBackend = &state;
    WorldViewport viewport(nullptr, &backend); state.viewport = &viewport;
    const auto terrain = TestTerrain(); QVERIFY(terrain);
    viewport.resize(320, 240); QVERIFY(BindTerrain(viewport, terrain)); viewport.show();
    QTRY_VERIFY(state.frames > 0);
    const std::vector<std::shared_ptr<const Render::TerrainAtlasSnapshot>> missing(terrain->header.groundTilesets.size());
    auto built = Render::BuildTerrainGroundMesh(*terrain, missing);
    QVERIFY(built.Succeeded());
    auto ground = std::make_shared<const Render::TerrainGroundMesh>(std::move(*built.value));
    const auto camera = viewport.Camera();
    QVERIFY(viewport.SetGroundMesh(ground)); QVERIFY(viewport.HasGroundMesh());
    QCOMPARE(state.terrainSubmits, 1); QCOMPARE(viewport.Camera().distance, camera.distance);
    QTRY_VERIFY(state.completed.sceneEpoch == state.sceneEpoch);
    Mouse(viewport, QEvent::MouseButtonPress, {240, 120}, Qt::LeftButton, Qt::LeftButton);
    Mouse(viewport, QEvent::MouseButtonRelease, {240, 120}, Qt::LeftButton, Qt::NoButton);
    QVERIFY(viewport.SelectedCoordinate()); QCOMPARE(viewport.SelectedCoordinate()->tileX, 1u);
    auto changed = std::make_shared<Render::TerrainGroundMesh>(*ground);
    changed->vertices[0].z += 1;
    QVERIFY(!viewport.SetGroundMesh(changed)); QCOMPARE(state.terrainSubmits, 1);
    changed = std::make_shared<Render::TerrainGroundMesh>(*ground);
    std::copy_n(changed->indices.begin(), 6, changed->indices.begin() + 6);
    QVERIFY(!viewport.SetGroundMesh(changed)); QCOMPARE(state.terrainSubmits, 1);
    state.rejectTerrain = true;
    QVERIFY(!viewport.SetGroundMesh(ground)); QVERIFY(viewport.HasGroundMesh());
    state.rejectTerrain = false;
    QVERIFY(BindTerrain(viewport, terrain, 15)); QVERIFY(!viewport.HasGroundMesh());
    const auto count = state.terrainSubmits;
    WorldViewport legacy(nullptr, &legacyBackend);
    QVERIFY(BindTerrain(legacy, terrain)); QVERIFY(!legacy.SetGroundMesh(ground));
    QCOMPARE(state.terrainSubmits, count);
}

void WorldModuleTests::SelectionLayerWaitsForRosterTerminal() {
    QTemporaryDir directory; QVERIFY(directory.isValid());
    const auto bytes = [](std::string_view text) { return std::vector<std::byte>(reinterpret_cast<const std::byte*>(text.data()),
        reinterpret_cast<const std::byte*>(text.data() + text.size())); };
    Image::DecodedImage image{2, 2, std::vector<std::byte>(16, std::byte{255})};
    const auto texture = Image::EncodeBlpPaletted(image); QVERIFY(texture.Succeeded());
    const auto path = std::filesystem::path(directory.filePath(QStringLiteral("circles.w3x")).toStdWString());
    const std::vector<MapArchiveMember> members{
        {"UI/MiscData.txt", bytes("[SelectionCircle]\nNumSizes=1\nScaleFactor=72\nTexture00=Original/Circle\nColorFriend=255,0,255,0\n")},
        {"Units/MiscData.txt", bytes("[Misc]\nSelectionCircleBaseZ=16\n")}, {"Original/Circle.blp", *texture.value}};
    QVERIFY(WriteVerifiedArchive(path, members).Succeeded());
    const auto sha = Sha256File(path); QVERIFY(sha.Succeeded());
    WorldTerrainSources sources; sources.archivePath = path; sources.archiveSha = *sha.value;
    for (const auto& member : members) sources.entries.push_back({member.name, member.name, {}});
    BackendState state; activeBackend = &state;
    WorldViewport viewport(nullptr, &layerBackend); state.viewport = &viewport;
    viewport.resize(320, 240); viewport.show(); QTRY_VERIFY(viewport.RendererReady());
    WorldSelectionPresenter presenter(viewport);
    Render::ModelInstanceRequest selected; selected.position = {10, 20, 25};
    selected.selection = Render::ModelSelectionDescription{1, 0, true};
    const std::vector<MapData::PlacementId> ids{{MapData::PlacementFile::Units, 0, "hfoo", 1}};
    presenter.SetSource(sources, {1, 7, "selection-wait-fixture"}, [] { return true; });
    QVERIFY(presenter.Preparing()); // 真实MPQ素材读取已经启动，尚未投递finished。
    presenter.Show({selected}, nullptr, {}, ids);
    presenter.SetSubmissionDeferred(true);
    QTRY_VERIFY(!presenter.Preparing()); // 素材worker实际finished，不能在此改layersRevision。
    QCOMPARE(state.layerSubmits + state.layerUpdates + state.layerRemoves, 0);
    presenter.SetSubmissionDeferred(false);
    QTRY_COMPARE(state.layerSubmits, 1); QCOMPARE(state.layerX, 10.0f);
    const auto frozenLayers = state.layersRevision;
    selected.position[0] = 30;
    presenter.Show({selected}, nullptr, {}, ids); QVERIFY(presenter.Preparing());
    presenter.SetSubmissionDeferred(true);
    QTRY_VERIFY(!presenter.Preparing());
    QCOMPARE(state.layersRevision, frozenLayers);
    presenter.SetSubmissionDeferred(false);
    QTRY_COMPARE(state.layerUpdates, 1); QCOMPARE(state.layerX, 30.0f); QCOMPARE(state.layerSubmits, 1);
    const auto unchangedParentLayers = state.layersRevision;
    presenter.SetSubmissionDeferred(true);
    presenter.Hide(); selected.position[0] = 50; presenter.Show({selected}, nullptr, {}, ids);
    QCOMPARE(state.layersRevision, unchangedParentLayers); // 取消/改选均延期，旧完成层仍驻留。
    presenter.SetSubmissionDeferred(false); // 拒绝终态：在原parent处理延期删除和最新选择。
    QTRY_COMPARE(state.layerSubmits, 2); QCOMPARE(state.layerRemoves, 1); QCOMPARE(state.layerX, 50.0f);
    const auto oldParent = state.layerParent;
    presenter.SetSubmissionDeferred(true); selected.position[0] = 70; presenter.Show({selected}, nullptr, {}, ids);
    // 成功roster已在同一完成帧清掉旧parent的层；宿主此时才RebindParent。
    ++state.layerParent; state.layerId = state.layerRevision = 0; ++state.layersRevision;
    const auto completedLayers = state.layersRevision;
    presenter.RebindParent(); QCOMPARE(state.layersRevision, completedLayers);
    presenter.SetSubmissionDeferred(false);
    QTRY_COMPARE(state.layerSubmits, 3); QCOMPARE(state.layerParent, oldParent + 1);
    QCOMPARE(state.layerRemoves, 1); QCOMPARE(state.layerX, 70.0f);
}

void WorldModuleTests::ReadOnlyArchiveLoadsTerrainAndRejectsIdenticalReplacement() {
    QTemporaryDir directory; QVERIFY(directory.isValid());
    QVERIFY(QDir(directory.path()).mkdir(QStringLiteral("workspace")));
    const auto path = std::filesystem::path(directory.filePath(QStringLiteral("readonly-source.w3x")).toStdWString());
    const auto bytes = [](std::string_view text) {
        const auto* begin = reinterpret_cast<const std::byte*>(text.data());
        return std::vector<std::byte>(begin, begin + text.size());
    };
    const auto terrain = TerrainBytes();
    const auto* terrainBegin = reinterpret_cast<const std::byte*>(terrain.constData());
    Image::DecodedImage image{16, 16, std::vector<std::byte>(16 * 16 * 4)};
    for (std::size_t i = 0; i < image.rgba.size(); i += 4) {
        image.rgba[i] = std::byte{64}; image.rgba[i + 1] = std::byte{192}; image.rgba[i + 3] = std::byte{255};
    }
    const auto texture = Image::EncodeBlpPaletted(image); QVERIFY(texture.Succeeded());
    const auto marker = bytes("original imported marker");
    const std::vector<MapArchiveMember> members{
        {"war3map.w3i", MapDataFixtures::FullMapSettingsV25().bytes},
        {"war3map.w3e", {terrainBegin, terrainBegin + terrain.size()}},
        {"war3mapUnits.doo", MapDataFixtures::EmptyUnitsV8Sub11()},
        {"war3map.doo", MapDataFixtures::EmptyDoodadsV8Sub11()},
        {"war3map.wts", bytes("STRING 42\n{\nOriginal fixture\n}\n")},
        {"TerrainArt/Terrain.slk", bytes("ID;PWXL;N;E\nC;X1;Y1;K\"tileID\"\nC;X2;Y1;K\"dir\"\nC;X3;Y1;K\"file\"\n"
            "C;X1;Y2;K\"Ldrt\"\nC;X2;Y2;K\"Terrain/Test\"\nC;X3;Y2;K\"stone\"\nE\n")},
        {"TerrainArt/CliffTypes.slk", bytes("ID;PWXL;N;E\nC;X1;Y1;K\"cliffID\"\nE\n")},
        {"TerrainArt/Water.slk", bytes("ID;PWXL;N;E\nC;X1;Y1;K\"waterID\"\nE\n")},
        {"Terrain/Test/stone.blp", *texture.value}, {"Imported/marker.bin", marker}};
    QVERIFY(WriteVerifiedArchive(path, members).Succeeded());
    {
        // NGIS只表示此原创尾部的形态；用例不伪造可信签名，也不放宽严格保存检查。
        QFile file(QString::fromStdWString(path.wstring())); QVERIFY(file.open(QIODevice::Append));
        const auto trailer = QByteArray("NGIS", 4) + QByteArray(256, '\0');
        QCOMPARE(file.write(trailer), trailer.size());
    }
    const auto digest = Sha256File(path); QVERIFY(digest.Succeeded());
    const auto strict = MapArchiveSession::Open(path); QVERIFY(!strict.Succeeded());
    QVERIFY(std::any_of(strict.diagnostics.begin(), strict.diagnostics.end(), [](const auto& note) {
        return note.code == "ARCHIVE_TRAILING_BYTES_REJECTED";
    }));
    auto opened = MpqWorkspace::OpenReadOnly(path); QVERIFY(opened.Succeeded());
    QtProject::EditorProject project;
    project.projectRoot_ = directory.filePath(QStringLiteral("workspace")).toStdWString();
    project.sourceArchivePath_ = path;
    project.readOnlyArchive_ = std::make_shared<MpqWorkspace>(std::move(*opened.value));
    QVERIFY(!project.archiveSession_ && !project.directorySnapshot_);
    project.LoadMapDocument(); project.ResetEditTracking();
    QVERIFY(project.mapDocument_ && project.mapEdits_ && project.mapEdits_->Terrain());
    QVERIFY(project.mapDocument_->InitializationSourceStatus().Succeeded());
    QVERIFY(project.CheckProjectSources().Succeeded());
    QVERIFY(!project.sourceChangeConfirmed_ && !project.sourceChangeSuspected_);
    const auto terrainRead = project.ReadMapMember("war3map.w3e");
    QVERIFY(terrainRead.Succeeded()); QCOMPARE(*terrainRead.value, members[1].data);
    const auto entries = project.AssetEntries();
    const auto imported = std::find_if(entries.begin(), entries.end(), [](const auto& entry) {
        return QtProject::EditorProject::AssetKey(entry.path) == QStringLiteral("imported\\marker.bin");
    });
    QVERIFY(imported != entries.end()); QVERIFY(imported->diskPath.isEmpty());
    QCOMPARE(imported->size, static_cast<qint64>(marker.size()));
    const auto captured = CaptureWorldTerrainSources(project); QVERIFY(captured.Succeeded());
    QCOMPARE(captured.value->archiveSha, *digest.value);
    QVERIFY(captured.value->readOnlyArchive == project.readOnlyArchive_);
    QVERIFY(std::filesystem::equivalent(captured.value->archivePath, path));
    const auto secondCapture = CaptureWorldTerrainSources(project); QVERIFY(secondCapture.Succeeded());
    QVERIFY(SameWorldTerrainSources(*captured.value, *secondCapture.value));
    const auto overBudget = CaptureWorldTerrainSources(project, 1); QVERIFY(!overBudget.Succeeded());
    QCOMPARE(overBudget.diagnostics.front().code, std::string("WORLD_ASSET_METADATA_BUDGET"));
    auto reader = WorldResourceReader::Open(*captured.value); QVERIFY(reader.Succeeded());
    const auto importedRead = (*reader.value)->Resolver().Read("Imported/marker.bin");
    QVERIFY(importedRead.Succeeded()); QCOMPARE(importedRead.value->bytes, marker);
    const auto textureRead = (*reader.value)->Resolver().Read("Terrain/Test/stone.blp");
    QVERIFY(textureRead.Succeeded()); QCOMPARE(textureRead.value->bytes, *texture.value);
    QVERIFY((*reader.value)->VerifyCurrentSource().Succeeded());
    const auto terrainAtlases = LoadWorldTerrainAtlases(*captured.value, project.mapEdits_->Terrain()->header);
    QVERIFY(terrainAtlases.Succeeded()); QCOMPARE(terrainAtlases.value->size(), std::size_t{1});
    QVERIFY(terrainAtlases.value->front()); QCOMPARE(terrainAtlases.value->front()->image.rgba, image.rgba);

    // 只走已有fake backend，真实产品的后台读图/贴图路径仍必须完成；不创建GPU设备。
    {
        BackendState state; activeBackend = &state;
        auto api = modelBackend; api.submitSkinnedModelScene = SkinScene; api.updateSkinMatrices = SkinUpdate;
        api.getSkinStatus = SkinStatus; api.captureSkinnedFrameRgba = SkinCapture;
        WorldModule module(project, &api);
        auto* page = module.Widget(); state.viewport = module.Viewport(); page->resize(800, 600); page->show();
        QTRY_VERIFY(module.ViewportForTools() && module.Viewport()->VertexCount() == 6);
        QTRY_VERIFY(state.terrainSubmits > 0 && state.frames > 0);
        QVERIFY(module.Viewport()->HasGroundMesh()); QVERIFY(!module.TerrainLoadFailed());
        const auto facts = module.PlaybackDiagnosticSnapshot().value("scene_facts").toObject();
        const auto textureSlotFacts = facts.value("ground_budget").toObject().value("resource_state").toObject();
        QVERIFY(textureSlotFacts.contains("requested_ground_texture_slots"));
        QCOMPARE(textureSlotFacts.value("requested_ground_texture_slots").toInteger(), qint64{1});
        QCOMPARE(textureSlotFacts.value("ground_texture_slots").toInteger(), qint64{1});
        QCOMPARE(textureSlotFacts.value("final_texture_slots").toInteger(), qint64{1});
        const auto groundDetails = facts.value("ground").toObject().value("details").toString();
        QVERIFY(groundDetails.contains(QStringLiteral("请求地表预检 1")));
        QVERIFY(groundDetails.contains(QStringLiteral("保留地表 1")));
        QVERIFY(!project.mapEdits_->IsDirty()); QCOMPARE(project.mapDataUndoStack_->count(), 0);
    }

    // 能读地图并不意味着有完整成员清单或可重建签名，地图/工程/恢复准备均须拒绝。
    const auto mapTarget = std::filesystem::path(directory.filePath(QStringLiteral("copy.w3x")).toStdWString());
    const auto projectTarget = std::filesystem::path(directory.filePath(QStringLiteral("copy.forge")).toStdWString());
    QVERIFY(!project.PrepareMapSave(mapTarget, false));
    QVERIFY(!project.PrepareMapSave({}, true));
    QVERIFY(!project.PrepareProjectSave(projectTarget).Succeeded());
    QVERIFY(!project.PrepareProjectSave(projectTarget, true).Succeeded());
    QVERIFY(!std::filesystem::exists(mapTarget) && !std::filesystem::exists(projectTarget));
    QVERIFY(!project.archiveSession_ && !project.directorySnapshot_);
    const auto afterRead = Sha256File(path); QVERIFY(afterRead.Succeeded()); QCOMPARE(*afterRead.value, *digest.value);

    const auto retired = std::filesystem::path(directory.filePath(QStringLiteral("retired.w3x")).toStdWString());
    std::error_code replacementError;
    std::filesystem::rename(path, retired, replacementError);
    QVERIFY2(!replacementError, replacementError.message().c_str());
    QVERIFY(std::filesystem::copy_file(retired, path, replacementError));
    QVERIFY2(!replacementError, replacementError.message().c_str());
    const auto replacementSha = Sha256File(path); QVERIFY(replacementSha.Succeeded()); QCOMPARE(*replacementSha.value, *digest.value);
    QVERIFY(!project.readOnlyArchive_->VerifyUnchanged().Succeeded());
    QVERIFY(!project.CheckProjectSources().Succeeded());
    QVERIFY(project.sourceChangeConfirmed_ && project.sourceChangeSuspected_);
    QVERIFY(!project.ReadMapMember("war3map.w3e").Succeeded());
    QVERIFY(!CaptureWorldTerrainSources(project).Succeeded());
    QVERIFY(!(*reader.value)->VerifyCurrentSource().Succeeded());
    QVERIFY(!WorldResourceReader::Open(*captured.value).Succeeded()); // 不借旧reader也必须核对捕获时的文件身份。
    QVERIFY(!WorldResourceReader::Open(*captured.value, reader.value->get()).Succeeded());
    std::shared_ptr<WorldTerrainCache> staleCache;
    const auto staleTerrain = LoadWorldTerrain(members[1].data, captured, {}, {}, &staleCache);
    QVERIFY(!staleTerrain.Succeeded()); QVERIFY(!staleCache);
    QCOMPARE(staleTerrain.diagnostics.front().code, std::string("MPQ_SOURCE_CHANGED"));
    const auto recapturedTerrain = LoadWorldTerrain(members[1].data, CaptureWorldTerrainSources(project), {}, {}, &staleCache);
    QVERIFY(!recapturedTerrain.Succeeded()); QVERIFY(!staleCache);
    QCOMPARE(recapturedTerrain.diagnostics.front().code, std::string("MPQ_SOURCE_CHANGED"));
    // 上层世界页不能把仍在内存中的 W3E 降为一次成功的高度预览。
    {
        BackendState state; activeBackend = &state;
        WorldModule module(project, &backend);
        auto* page = module.Widget(); state.viewport = module.Viewport(); page->resize(800, 600); page->show();
        QTRY_VERIFY(module.TerrainLoadFailed());
        QCOMPARE(module.Viewport()->VertexCount(), std::size_t{0});
        QCOMPARE(state.terrainSubmits, 0);
    }
    project.LoadMapDocument(); QVERIFY(!project.mapDocument_ && !project.mapEdits_);

    // 显式重新打开才可接受新文件；旧捕获由自身的绑定维持失效，不随Project重新绑定复活。
    auto replacement = MpqWorkspace::OpenReadOnly(path); QVERIFY(replacement.Succeeded());
    project.readOnlyArchive_ = std::make_shared<MpqWorkspace>(std::move(*replacement.value));
    project.LoadMapDocument(); project.ResetEditTracking();
    QVERIFY(project.mapDocument_ && project.mapEdits_ && project.CheckProjectSources().Succeeded());
    const auto rebound = CaptureWorldTerrainSources(project); QVERIFY(rebound.Succeeded());
    QCOMPARE(rebound.value->archiveSha, captured.value->archiveSha);
    QVERIFY(!SameWorldTerrainSources(*captured.value, *rebound.value));
    QVERIFY(WorldResourceReader::Open(*rebound.value).Succeeded());
    QVERIFY(!WorldResourceReader::Open(*captured.value).Succeeded());
    QVERIFY(!project.archiveSession_ && !project.PrepareMapSave(mapTarget, false));
}

void WorldModuleTests::TerrainResourceOverlayAndSourceIdentity() {
    QTemporaryDir directory; QVERIFY(directory.isValid());
    const auto bytes = [](std::string_view text) { return std::vector<std::byte>(reinterpret_cast<const std::byte*>(text.data()),
        reinterpret_cast<const std::byte*>(text.data() + text.size())); };
    const auto path = std::filesystem::path(directory.filePath(QStringLiteral("source.w3x")).toStdWString());
    Image::DecodedImage image{16, 16, std::vector<std::byte>(16 * 16 * 4)};
    for (std::size_t i = 0; i < image.rgba.size(); i += 4) { image.rgba[i] = std::byte{255}; image.rgba[i + 3] = std::byte{255}; }
    const auto red = Image::EncodeBlpPaletted(image); QVERIFY(red.Succeeded());
    const std::vector<MapArchiveMember> members{{"TerrainArt/Terrain.slk", bytes("broken-original")},
        {"TerrainArt/CliffTypes.slk", bytes("ID;PWXL;N;E\nC;X1;Y1;K\"cliffID\"\nE\n")},
        {"TerrainArt/Water.slk", bytes("ID;PWXL;N;E\nC;X1;Y1;K\"waterID\"\nE\n")},
        {"Terrain/Test/stone.blp", *red.value}};
    const auto written = WriteVerifiedArchive(path, members); QVERIFY(written.Succeeded());
    auto opened = MapArchiveSession::Open(path); QVERIFY(opened.Succeeded());
    QtProject::EditorProject project;
    project.archiveSession_ = std::make_unique<MapArchiveSession>(std::move(*opened.value));
    project.war3GameRoot_ = directory.filePath(QStringLiteral("nonexistent-game-root")).toStdWString();
    const auto pending = directory.filePath(QStringLiteral("pending.slk"));
    QVERIFY(Write(pending, "ID;PWXL;N;E\nC;X1;Y1;K\"tileID\"\nC;X2;Y1;K\"dir\"\nC;X3;Y1;K\"file\"\nC;X1;Y2;K\"Ldrt\"\nC;X2;Y2;K\"Terrain/Test\"\nC;X3;Y2;K\"stone\"\nE\n"));
    for (std::size_t i = 0; i < image.rgba.size(); i += 4) { image.rgba[i] = std::byte{0}; image.rgba[i + 2] = std::byte{255}; }
    const auto blue = Image::EncodeBlpPaletted(image); QVERIFY(blue.Succeeded());
    const auto pendingTexture = directory.filePath(QStringLiteral("pending.blp"));
    QVERIFY(Write(pendingTexture, QByteArray(reinterpret_cast<const char*>(blue.value->data()), static_cast<qsizetype>(blue.value->size()))));
    const auto worldEditData = directory.filePath(QStringLiteral("WorldEditData.txt"));
    QVERIFY(Write(worldEditData, "[TileSets]\nA=Name,TerrainArt/Blight/Ashen_Blight\n"));
    Image::DecodedImage blightImage{32, 16, std::vector<std::byte>(32 * 16 * 4, std::byte{0})};
    for (std::size_t pixel = 0; pixel < 32 * 16; ++pixel) blightImage.rgba[pixel * 4 + 3] = std::byte{255};
    const auto opaqueBlight = Image::EncodeBlpPaletted(blightImage); QVERIFY(opaqueBlight.Succeeded());
    const auto opaquePath = directory.filePath(QStringLiteral("opaque-blight.blp"));
    QVERIFY(Write(opaquePath, QByteArray(reinterpret_cast<const char*>(opaqueBlight.value->data()),
        static_cast<qsizetype>(opaqueBlight.value->size()))));
    for (std::uint32_t y = 0; y < 16; ++y) for (std::uint32_t x = 0; x < 16; ++x) {
        const auto mask = (y / 4) * 4 + x / 4;
        if (mask > 0 && mask < 15 && x % 4 < 2)
            blightImage.rgba[(static_cast<std::size_t>(y) * 32 + x) * 4 + 3] = std::byte{0};
    }
    const auto maskedBlight = Image::EncodeBlpPaletted(blightImage); QVERIFY(maskedBlight.Succeeded());
    const auto maskedPath = directory.filePath(QStringLiteral("masked-blight.blp"));
    QVERIFY(Write(maskedPath, QByteArray(reinterpret_cast<const char*>(maskedBlight.value->data()),
        static_cast<qsizetype>(maskedBlight.value->size()))));
    project.pendingImports_ = {{QStringLiteral("terrainart/terrain.slk"), pending, QFileInfo(pending).size()},
        {QStringLiteral("terrain/test/stone.blp"), pendingTexture, QFileInfo(pendingTexture).size()},
        {QStringLiteral("ui/worldeditdata.txt"), worldEditData, QFileInfo(worldEditData).size()},
        {QStringLiteral("terrainart/blight/ashen_blight.blp"), opaquePath, QFileInfo(opaquePath).size()}};
    const auto captured = CaptureWorldTerrainSources(project); QVERIFY(captured.Succeeded());
    auto sources = *captured.value;
    const auto terrain = TestTerrain(); QVERIFY(terrain);
    const auto pendingResult = LoadWorldTerrainAtlases(sources, terrain->header);
    QVERIFY2(pendingResult.Succeeded(), pendingResult.diagnostics.empty() ? "no result" : pendingResult.diagnostics.front().message.c_str());
    QCOMPARE(pendingResult.value->size(), std::size_t{1});
    QCOMPARE(pendingResult.value->front()->image.rgba, image.rgba);
    const auto blueSha = Sha256(*blue.value); QVERIFY(blueSha.Succeeded());
    QCOMPARE(pendingResult.value->front()->sourceSha256, *blueSha.value);
    std::shared_ptr<const Render::TerrainAtlasSnapshot> blightAtlas;
    const auto opaqueResult = LoadWorldTerrainAtlases(sources, terrain->header, nullptr, &blightAtlas);
    QVERIFY(opaqueResult.Succeeded()); QVERIFY(!blightAtlas);
    QVERIFY(std::any_of(opaqueResult.diagnostics.begin(), opaqueResult.diagnostics.end(), [](const auto& note) {
        return note.code == "WORLD_BLIGHT_MASK_ALPHA";
    }));
    const auto blightEntry = std::find_if(sources.entries.begin(), sources.entries.end(), [](const auto& entry) {
        return entry.path == "terrainart/blight/ashen_blight.blp";
    });
    QVERIFY(blightEntry != sources.entries.end());
    blightEntry->diskPath = maskedPath.toStdWString();
    const auto maskedResult = LoadWorldTerrainAtlases(sources, terrain->header, nullptr, &blightAtlas);
    QVERIFY(maskedResult.Succeeded()); QVERIFY(blightAtlas);
    QCOMPARE(blightAtlas->columns, 8u);
    const auto maskedSha = Sha256(*maskedBlight.value); QVERIFY(maskedSha.Succeeded());
    QCOMPARE(blightAtlas->sourceSha256, *maskedSha.value);
    std::erase_if(sources.entries, [](const auto& entry) { return entry.path == "Terrain/Test/stone.blp"; });
    const auto reimported = LoadWorldTerrainAtlases(sources, terrain->header);
    QVERIFY(reimported.Succeeded());
    QCOMPARE(reimported.value->front()->sourceSha256, *blueSha.value);
    const auto pendingEntry = std::find_if(sources.entries.begin(), sources.entries.end(), [](const auto& entry) { return !entry.diskPath.empty(); });
    QVERIFY(pendingEntry != sources.entries.end());
    sources.entries.push_back(*pendingEntry);
    const auto conflict = LoadWorldTerrainAtlases(sources, terrain->header);
    QVERIFY(!conflict.Succeeded()); QCOMPARE(conflict.diagnostics.front().code, std::string("WORLD_ASSET_CONFLICT"));
    sources.entries.pop_back();
    // 部分材质损坏不能隐藏已成功的材质，也不能改用原归档的低优先级材质。
    const auto brokenTexture = directory.filePath(QStringLiteral("broken.blp"));
    QVERIFY(Write(brokenTexture, "not-a-blp"));
    const auto stone = std::find_if(sources.entries.begin(), sources.entries.end(), [](const auto& e) {
        return e.path == "terrain/test/stone.blp";
    });
    QVERIFY(stone != sources.entries.end());
    const auto savedPending = stone->diskPath;
    stone->diskPath = brokenTexture.toStdWString();
    const auto broken = LoadWorldTerrainAtlases(sources, terrain->header);
    QVERIFY(broken.Succeeded()); QVERIFY(!broken.value->front()); QVERIFY(!broken.diagnostics.empty());
    stone->diskPath = savedPending;
    auto partialGrid = FlatGrid(4, 2);
    partialGrid->header.groundTilesets = {"Ldrt", "Lrok"};
    for (std::uint32_t y = 0; y < 2; ++y) for (std::uint32_t x = 2; x < 4; ++x)
        partialGrid->vertices[y * 4 + x].textureAndFlagsRaw = 1;
    const auto partial = LoadWorldTerrainAtlases(sources, partialGrid->header);
    QVERIFY(partial.Succeeded()); QCOMPARE(partial.value->size(), std::size_t{2});
    QVERIFY(partial.value->front()); QVERIFY(!partial.value->back()); QVERIFY(!partial.diagnostics.empty());
    const auto ground = Render::BuildTerrainGroundMesh(*partialGrid, *partial.value);
    // 中间格一角用到缺失材质：其过渡层画不出，整格回退高度色，不只画基础层冒充完整。
    QVERIFY(ground.Succeeded()); QCOMPARE(ground.value->texturedTiles, std::size_t{1});
    QCOMPARE(ground.value->fallback.missingAtlasTiles, std::size_t{2});
    sources.archiveSha.assign(64, '0');
    const auto changed = LoadWorldTerrainAtlases(sources, terrain->header);
    QVERIFY(!changed.Succeeded()); QCOMPARE(changed.diagnostics.front().code, std::string("WORLD_ASSET_SOURCE_CHANGED"));
    const auto after = Sha256File(path); QVERIFY(after.Succeeded()); QCOMPARE(*after.value, written.value->sha256);
}

void WorldModuleTests::MipSceneUsesOriginalChainAndClampsAtlas() {
    Image::DecodedImage image{16, 16, std::vector<std::byte>(16 * 16 * 4, std::byte{127})};
    const auto encoded = Image::EncodeBlpPaletted(image); QVERIFY(encoded.Succeeded());
    Resources::ResolvedResource resource; resource.bytes = *encoded.value;
    const auto decoded = Render::DecodeTerrainTextureAtlas(resource); QVERIFY(decoded.Succeeded());
    const auto terrain = TestTerrain(); QVERIFY(terrain);
    const std::vector<std::shared_ptr<const Render::TerrainAtlasSnapshot>> atlases{*decoded.value};
    const auto built = Render::BuildTerrainGroundMesh(*terrain, atlases); QVERIFY(built.Succeeded());
    auto ground = std::make_shared<const Render::TerrainGroundMesh>(*built.value);
    BackendState state; activeBackend = &state;
    WorldViewport viewport(nullptr, &mipBackend); state.viewport = &viewport;
    viewport.resize(320, 240); QVERIFY(BindTerrain(viewport, terrain)); viewport.show();
    QTRY_VERIFY(state.frames > 0);
    QVERIFY(viewport.HasTerrainMips()); QVERIFY(viewport.SetGroundMesh(ground));
    QCOMPARE(state.mipSubmits, 1); QCOMPARE(state.terrainSubmits, 0);
    QCOMPARE(state.mipCount, 5u); QCOMPARE(state.maxSampleMip, 2u);
    QCOMPARE(state.atlasColumns, 4u); QCOMPARE(state.atlasRows, 4u);
    QCOMPARE(state.mipOneBytes, (*decoded.value)->additionalMips[0].rgba);
    state.rejectTerrain = true; QVERIFY(!viewport.SetGroundMesh(ground));
    QVERIFY(viewport.HasGroundMesh()); QCOMPARE(state.terrainSubmits, 0);
    // 可选尾入口不存在时继续使用旧 mip0，绝不读取 size 之外的函数指针。
    state.rejectTerrain = false;
    auto old = mipBackend;
    old.structSize = static_cast<std::uint32_t>(offsetof(War3RenderPluginApiV2, submitTerrainMipScene));
    {
        struct RestoreViewport {
            BackendState& state;
            WorldViewport* previous;
            ~RestoreViewport() { state.viewport = previous; }
        } restore{state, &viewport};
        WorldViewport legacy(nullptr, &old); state.viewport = &legacy;
        legacy.resize(320, 240); QVERIFY(BindTerrain(legacy, terrain)); legacy.show();
        QTRY_VERIFY(state.creates == 2);
        QVERIFY(!legacy.HasTerrainMips()); QVERIFY(legacy.SetGroundMesh(ground)); QCOMPARE(state.terrainSubmits, 1);
    }
}

void WorldModuleTests::SharedBaseBlightRequiresCapabilitiesAndKeepsAtomicFrame() {
    auto api = mipBackend;
    api.getTerrainBlightCapabilities = BlightCapabilities; api.submitTerrainBlightFrame = BlightFrame;
    BackendState state; activeBackend = &state;
    WorldViewport viewport(nullptr, &api); state.viewport = &viewport;
    QVERIFY(viewport.HasTerrainBlightSupport()); QCOMPARE(state.creates, 0); QCOMPARE(state.blightNullQueries, 1);
    state.blightQueryResult = 1; QVERIFY(!viewport.HasTerrainBlightSupport()); state.blightQueryResult = 0;
    state.blightFormat = 2; QVERIFY(!viewport.HasTerrainBlightSupport()); state.blightFormat = 1;
    state.blightMaximum = 31; QVERIFY(!viewport.HasTerrainBlightSupport()); state.blightMaximum = 32;
    state.blightReserved = 1; QVERIFY(!viewport.HasTerrainBlightSupport()); state.blightReserved = 0;
    state.blightCapsSize = 12; QVERIFY(!viewport.HasTerrainBlightSupport()); state.blightCapsSize = 16;
    {
        auto prefix = api;
        prefix.structSize = static_cast<std::uint32_t>(offsetof(War3RenderPluginApiV2, getTerrainBlightCapabilities));
        const auto queries = state.blightQueries;
        WorldViewport old(nullptr, &prefix); QVERIFY(!old.HasTerrainBlightSupport());
        QCOMPARE(state.blightQueries, queries);
        prefix.structSize = sizeof(prefix); prefix.getTerrainBlightCapabilities = nullptr;
        QVERIFY(!old.HasTerrainBlightSupport());
        prefix.getTerrainBlightCapabilities = BlightCapabilities; prefix.submitTerrainBlightFrame = nullptr;
        QVERIFY(!old.HasTerrainBlightSupport()); QCOMPARE(state.blightQueries, queries);
    }
    auto terrain = FlatGrid(3, 2); terrain->header.groundTilesets = {"Ldrt"};
    for (auto& vertex : terrain->vertices) vertex.textureAndFlagsRaw = 0x20;
    terrain->vertices[1].variationRaw = 1;
    Image::DecodedImage image{16, 16, std::vector<std::byte>(16 * 16 * 4, std::byte{127})};
    const auto encoded = Image::EncodeBlpPaletted(image); QVERIFY(encoded.Succeeded());
    Resources::ResolvedResource resource; resource.bytes = *encoded.value;
    const auto decoded = Render::DecodeTerrainTextureAtlas(resource); QVERIFY(decoded.Succeeded());
    Image::DecodedImage blightImage{32, 16, std::vector<std::byte>(32 * 16 * 4, std::byte{211})};
    const auto encodedBlight = Image::EncodeBlpPaletted(blightImage); QVERIFY(encodedBlight.Succeeded());
    Resources::ResolvedResource blightResource; blightResource.bytes = *encodedBlight.value;
    const auto decodedBlight = Render::DecodeTerrainTextureAtlas(blightResource); QVERIFY(decodedBlight.Succeeded());
    const std::vector<std::shared_ptr<const Render::TerrainAtlasSnapshot>> atlases{*decoded.value};
    constexpr auto preview = Render::TerrainGroundBudgetPolicy::PreserveBasePreview;
    constexpr auto shared = Render::TerrainGroundBlightPolicy::SharedBase;
    const auto built = Render::BuildTerrainGroundMesh(*terrain, atlases, {}, Render::TerrainLighting{false},
        nullptr, {}, *decodedBlight.value, {}, preview, shared);
    QVERIFY(built.Succeeded());
    const auto ground = std::make_shared<const Render::TerrainGroundMesh>(*built.value);
    viewport.resize(320, 240); QVERIFY(BindTerrain(viewport, terrain)); viewport.show();
    QTRY_VERIFY(state.frames > 0);
    QVERIFY(viewport.SetGroundMesh(ground)); QCOMPARE(state.blightSubmits, 1); QCOMPARE(state.mipSubmits, 0);
    QCOMPARE(state.blightBaseQuads, 2u); QCOMPARE(state.blightBaseTextures, 1u); QCOMPARE(state.vertices, std::size_t{8});
    QCOMPARE(state.blightIndices, ground->blightIndices); QCOMPARE(state.blightBatches.size(), ground->blightBatches.size());
    QCOMPARE(state.blightBatches[0].uvLeft, ground->blightBatches[0].uvLeft);
    QCOMPARE(state.blightBatches[1].uvBottom, ground->blightBatches[1].uvBottom);
    QCOMPARE(state.blightMipCount, 6u); QCOMPARE(state.blightMaxMip, 2u);
    QCOMPARE(state.blightMipOneBytes, (*decodedBlight.value)->additionalMips[0].rgba);
    const auto epoch = state.sceneEpoch, revision = state.revision;
    const auto submits = state.blightSubmits;
    const auto invalid = [&](const std::function<void(Render::TerrainGroundMesh&)>& change) {
        auto candidate = std::make_shared<Render::TerrainGroundMesh>(*ground); change(*candidate);
        return viewport.SetGroundMesh(std::move(candidate));
    };
    QVERIFY(!invalid([](auto& m) { m.blightIndices[0] = 8; })); // base前缀外
    QVERIFY(!invalid([](auto& m) { m.blightIndices[1] = 0; })); // 错对角线/重复顶点
    QVERIFY(!invalid([](auto& m) { m.blightIndices[6] = 0; m.blightIndices[7] = 1; m.blightIndices[8] = 2;
        m.blightIndices[9] = 1; m.blightIndices[10] = 3; m.blightIndices[11] = 2; })); // 跨cell重复quad
    QVERIFY(!invalid([](auto& m) { m.blightBatches[1].uvLeft = m.blightBatches[0].uvLeft;
        m.blightBatches[1].uvRight = m.blightBatches[0].uvRight; m.blightBatches[1].uvTop = m.blightBatches[0].uvTop;
        m.blightBatches[1].uvBottom = m.blightBatches[0].uvBottom; }));
    QVERIFY(!invalid([](auto& m) { m.batches[0].alphaBlend = true; }));
    QVERIFY(!invalid([](auto& m) {
        auto atlas = std::make_shared<Render::TerrainAtlasSnapshot>(*m.blightTexture);
        while (atlas->additionalMips.size() < 12) atlas->additionalMips.push_back(atlas->additionalMips.back());
        m.blightTexture = std::move(atlas);
    })); // 超12总mip须在reserve/复制ABI描述符前拒绝，不交provider兜底。
    QCOMPARE(state.blightSubmits, submits); QCOMPARE(state.sceneEpoch, epoch); QCOMPARE(state.revision, revision);
    state.rejectTerrain = true;
    auto mesh = BuildWorldTerrainMesh(*terrain); QVERIFY(mesh.Succeeded());
    QVERIFY(!viewport.ReplaceTerrainPresentation(std::move(mesh.value->vertices), std::move(mesh.value->indices),
        terrain, 1, ground, revision + 1));
    QCOMPARE(state.sceneEpoch, epoch); QCOMPARE(state.revision, revision); QVERIFY(viewport.HasGroundMesh());
    state.rejectTerrain = false;
    state.blightQueryResult = 1; QVERIFY(!viewport.SetGroundMesh(ground)); state.blightQueryResult = 0;
    QCOMPARE(state.sceneEpoch, epoch); QCOMPARE(state.revision, revision); QCOMPARE(state.mipSubmits, 0);
    auto clean = std::make_shared<MapData::TerrainGridSnapshot>(*terrain);
    for (auto& vertex : clean->vertices) vertex.textureAndFlagsRaw = 0;
    const auto empty = Render::BuildTerrainGroundMesh(*clean, atlases, {}, Render::TerrainLighting{false},
        nullptr, {}, *decodedBlight.value, {}, preview, shared);
    QVERIFY(empty.Succeeded());
    auto cleanMesh = BuildWorldTerrainMesh(*clean); QVERIFY(cleanMesh.Succeeded());
    QVERIFY(viewport.ReplaceTerrainPresentation(std::move(cleanMesh.value->vertices), std::move(cleanMesh.value->indices),
        clean, 1, std::make_shared<const Render::TerrainGroundMesh>(*empty.value), revision + 2));
    QVERIFY(state.blightIndices.empty()); QVERIFY(state.blightBatches.empty()); QCOMPARE(state.blightMipCount, 0u);
    const auto oldGround = Render::BuildTerrainGroundMesh(*clean, atlases); QVERIFY(oldGround.Succeeded());
    QVERIFY(viewport.SetGroundMesh(std::make_shared<const Render::TerrainGroundMesh>(*oldGround.value)));
    QCOMPARE(state.mipSubmits, 1); QVERIFY(state.blightIndices.empty());
}

void WorldModuleTests::SharedGridRequiresCompleteCapabilitiesAndKeepsAtomicFrame() {
    auto api = mipBackend;
    api.getTerrainGridCapabilities = GridCapabilities; api.submitTerrainGridFrame = GridFrame; api.getTerrainGridCaptureFacts = GridFacts;
    BackendState state; activeBackend = &state;
    WorldViewport viewport(nullptr, &api); state.viewport = &viewport;
    QVERIFY(viewport.HasTerrainGridSupport()); QCOMPARE(state.creates, 0); QCOMPARE(state.gridNullQueries, 1);
    state.gridQueryResult = 1; QVERIFY(!viewport.HasTerrainGridSupport()); state.gridQueryResult = 0;
    const auto good = state.gridCaps;
    state.gridCaps.gridFormat = 2; QVERIFY(!viewport.HasTerrainGridSupport()); state.gridCaps = good;
    state.gridCaps.maxGroundTextures = 17; QVERIFY(!viewport.HasTerrainGridSupport()); state.gridCaps = good;
    state.gridCaps.maxBlightBatches = 31; QVERIFY(!viewport.HasTerrainGridSupport()); state.gridCaps = good;
    state.gridCaps.maxWaterTextures = 2; QVERIFY(!viewport.HasTerrainGridSupport()); state.gridCaps = good;
    state.gridCaps.reserved = 1; QVERIFY(!viewport.HasTerrainGridSupport()); state.gridCaps = good;
    state.gridCaps.structSize = 20; QVERIFY(!viewport.HasTerrainGridSupport()); state.gridCaps = good;
    {
        auto prefix = api;
        prefix.structSize = static_cast<std::uint32_t>(offsetof(War3RenderPluginApiV2, getTerrainGridCaptureFacts));
        WorldViewport old(nullptr, &prefix); const auto queries = state.gridQueries;
        QVERIFY(!old.HasTerrainGridSupport()); QCOMPARE(state.gridQueries, queries);
        prefix.structSize = sizeof(prefix); prefix.getTerrainGridCapabilities = nullptr;
        QVERIFY(!old.HasTerrainGridSupport()); prefix.getTerrainGridCapabilities = GridCapabilities;
        prefix.submitTerrainGridFrame = nullptr; QVERIFY(!old.HasTerrainGridSupport()); prefix.submitTerrainGridFrame = GridFrame;
        prefix.getTerrainGridCaptureFacts = nullptr; QVERIFY(!old.HasTerrainGridSupport()); QCOMPARE(state.gridQueries, queries);
    }
    auto terrain = FlatGrid(3, 2); terrain->header.groundTilesets = {"Ldrt"};
    for (auto& vertex : terrain->vertices) { vertex.textureAndFlagsRaw = 0x60; vertex.waterBaseHeight = 40; }
    const auto atlas = [](std::uint32_t columns) {
        auto value = std::make_shared<Render::TerrainAtlasSnapshot>();
        value->columns = columns; value->sourceWidth = value->image.width = columns;
        value->sourceHeight = value->image.height = 4; value->image.rgba.assign(columns * 4 * 4, std::byte{255});
        for (std::uint32_t row = 0; row < 4; ++row) for (std::uint32_t column = 0; column < columns; ++column)
            value->cells.push_back({column, row, column, row, 1, 1});
        return value;
    };
    const std::vector<std::shared_ptr<const Render::TerrainAtlasSnapshot>> atlases{atlas(4)};
    Render::TerrainWaterStyle water{0, {30, 60, 100, 100}, {80, 110, 150, 220}, {20, 50, 90, 180}, {10, 30, 70, 240}};
    const auto built = Render::BuildTerrainGroundMesh(*terrain, atlases, {}, Render::TerrainLighting{false}, &water, {}, atlas(8), {},
        Render::TerrainGroundBudgetPolicy::PreserveBasePreview, Render::TerrainGroundBlightPolicy::SharedBase,
        Render::TerrainGroundRepresentation::SharedGrid);
    QVERIFY(built.Succeeded()); const auto ground = std::make_shared<const Render::TerrainGroundMesh>(*built.value);
    viewport.resize(320, 240); QVERIFY(BindTerrain(viewport, terrain)); viewport.show(); QTRY_VERIFY(state.frames > 0);
    QVERIFY(viewport.SetGroundMesh(ground)); QCOMPARE(state.gridSubmits, 1); QCOMPARE(state.mipSubmits, 0); QCOMPARE(state.blightSubmits, 0);
    QCOMPARE(state.gridVertexCount, 6u); QCOMPARE(state.gridBaseCount, 2u); QCOMPARE(state.gridWaterCount, 2u); QCOMPARE(state.gridTransitionCount, 0u);
    QCOMPARE(state.gridGroundAddress, ground->groundGrid.data()); QCOMPARE(state.gridWaterAddress, ground->waterGrid.data());
    const auto epoch = state.sceneEpoch, revision = state.revision;
    state.gridQueryResult = 1; QVERIFY(!viewport.SetGroundMesh(ground)); state.gridQueryResult = 0;
    QCOMPARE(state.gridSubmits, 1); QCOMPARE(state.mipSubmits, 0); QCOMPARE(state.sceneEpoch, epoch);
    auto invalid = std::make_shared<Render::TerrainGroundMesh>(*ground); invalid->representation = Render::TerrainGroundRepresentation::Expanded;
    QVERIFY(!viewport.SetGroundMesh(invalid)); QCOMPARE(state.mipSubmits, 0); QCOMPARE(state.gridSubmits, 1);
    invalid = std::make_shared<Render::TerrainGroundMesh>(*ground); invalid->baseQuads[1].sourceTile = 0;
    QVERIFY(!viewport.SetGroundMesh(invalid)); QCOMPARE(state.gridSubmits, 1);
    state.rejectTerrain = true; auto mesh = BuildWorldTerrainMesh(*terrain); QVERIFY(mesh.Succeeded());
    QVERIFY(!viewport.ReplaceTerrainPresentation(std::move(mesh.value->vertices), std::move(mesh.value->indices), terrain, 1, ground, revision + 1));
    QCOMPARE(state.sceneEpoch, epoch); QCOMPARE(state.revision, revision); QCOMPARE(state.gridSubmits, 2);
    QCOMPARE(state.mipSubmits, 0); QCOMPARE(state.blightSubmits, 0); QVERIFY(viewport.HasGroundMesh());
}

// 摆放点选与拖动：单击命中实例、按在选中实例上拖动发出目标 XY 且不转镜头、Esc 取消、Delete 请求删除。
void WorldModuleTests::PlacementClickDragAndKeys() {
    using namespace Render;
    BackendState state; activeBackend = &state;
    auto api = modelBackend;
    api.submitSkinnedModelScene = SkinScene; api.updateSkinMatrices = SkinUpdate;
    api.getSkinStatus = SkinStatus; api.captureSkinnedFrameRgba = SkinCapture;
    EditorPresentationFixture<BackendState>::Enable(api, state.editor);
    WorldViewport viewport(nullptr, &api); state.viewport = &viewport;
    viewport.resize(320, 240);
    QVERIFY(BindTerrain(viewport, TestTerrain()));
    std::vector<AnimationInstanceSpec> specs(38);
    for (std::size_t i = 0; i < specs.size(); ++i) {
        specs[i].identity = {i + 1, 8}; specs[i].hostKey = std::to_string(i);
        specs[i].model.modelPath = i == 37 ? "synthetic.mdx" : "missing.mdx";
    }
    specs[37].model.scale = {3.2f, 3.2f, 3.2f}; specs[37].model.position = {-16, -16, 0};
    AnimationLimits limits; limits.retainUnsupportedStatic = true;
    const auto service = ModelAnimationService::Create({8, 12, "Qt-placement-synthetic"}, specs,
        [](auto path, auto) -> Result<AnimationResource> {
            if (path != "synthetic.mdx") return Result<AnimationResource>::Failure("Test", "RESOURCE_NOT_FOUND", "原创夹具无此资源。");
            return Result<AnimationResource>::Success({
                AnimationTest::WithoutSyntheticConstantKmtf(MdxTest::BuildTestMdx()), "synthetic", std::string(64, 'a')});
        }, limits);
    QVERIFY(service.Succeeded());
    auto prepared = (*service.value)->Prepare(nullptr); QVERIFY(prepared.Succeeded());
    auto current = *prepared.value;
    auto scene = AnimationPickingScene(*(*service.value)->Scene(), *current);
    QVERIFY(viewport.SetAnimationScene((*service.value)->Scene(), current, scene));
    const auto transform = [&](std::array<float, 12> value) {
        AnimationAdvance advance; AnimationEdit edit; edit.identity = specs[37].identity; edit.transform = value;
        advance.edits.push_back(edit); const auto next = (*service.value)->Prepare(current.get(), advance);
        if (!next.Succeeded() || !viewport.UpdateAnimationFrame(*next.value, scene)) return false;
        current = *next.value; Frame(&state); return true;
    };
    viewport.show(); QTRY_VERIFY(state.frames > 0);
    QSignalSpy clicked(&viewport, &WorldViewport::InstanceClicked);
    QSignalSpy started(&viewport, &WorldViewport::MoveStarted);
    QSignalSpy dragged(&viewport, &WorldViewport::MoveDragged);
    QSignalSpy finished(&viewport, &WorldViewport::MoveFinished);
    QSignalSpy cancelled(&viewport, &WorldViewport::MoveCancelled);
    QSignalSpy deleted(&viewport, &WorldViewport::DeleteRequested);
    QSignalSpy copy(&viewport, &WorldViewport::CopyRequested);
    QSignalSpy paste(&viewport, &WorldViewport::PasteRequested);
    const QPointF center(160, 120), right(240, 120), empty(20, 120);
    Mouse(viewport, QEvent::MouseMove, center, Qt::NoButton, Qt::NoButton);
    QTRY_VERIFY(viewport.HoverCoordinate().has_value()); // 等到当前视图的完成帧
    Mouse(viewport, QEvent::MouseButtonPress, center, Qt::LeftButton, Qt::LeftButton);
    Mouse(viewport, QEvent::MouseButtonRelease, center, Qt::LeftButton, Qt::NoButton);
    QCOMPARE(clicked.count(), 1); QCOMPARE(clicked.takeFirst().at(0).toInt(), 0);
    viewport.SetSelectedInstance(0u);
    const auto camera = viewport.SaveCamera();
    Mouse(viewport, QEvent::MouseButtonPress, center, Qt::LeftButton, Qt::LeftButton);
    Mouse(viewport, QEvent::MouseMove, right, Qt::NoButton, Qt::LeftButton);
    QVERIFY(viewport.Moving());
    Mouse(viewport, QEvent::MouseButtonRelease, right, Qt::LeftButton, Qt::NoButton);
    QVERIFY(started.count() == 1 && dragged.count() >= 1 && finished.count() == 1 && cancelled.isEmpty());
    QVERIFY(std::abs(started.first().at(0).toDouble()) < 1e-3 && std::abs(started.first().at(1).toDouble()) < 1e-3);
    QVERIFY(std::abs(dragged.last().at(0).toDouble() - 64) < 1e-3 && std::abs(dragged.last().at(1).toDouble()) < 1e-3);
    QCOMPARE(viewport.SaveCamera(), camera); // 拖动物体不旋转镜头
    Mouse(viewport, QEvent::MouseButtonPress, center, Qt::LeftButton, Qt::LeftButton);
    Mouse(viewport, QEvent::MouseMove, right, Qt::NoButton, Qt::LeftButton);
    QTest::keyClick(&viewport, Qt::Key_Escape);
    QCOMPARE(cancelled.count(), 1); QVERIFY(!viewport.Moving());
    Mouse(viewport, QEvent::MouseButtonRelease, right, Qt::LeftButton, Qt::NoButton);
    QCOMPARE(finished.count(), 1);
    QSignalSpy rotateStart(&viewport, &WorldViewport::RotateStarted);
    QSignalSpy rotateDrag(&viewport, &WorldViewport::RotateDragged);
    QSignalSpy rotateFinish(&viewport, &WorldViewport::RotateFinished);
    QSignalSpy rotateCancel(&viewport, &WorldViewport::RotateCancelled);
    Mouse(viewport, QEvent::MouseButtonPress, center, Qt::LeftButton, Qt::LeftButton, Qt::AltModifier);
    Mouse(viewport, QEvent::MouseMove, right, Qt::NoButton, Qt::LeftButton, Qt::AltModifier);
    Mouse(viewport, QEvent::MouseButtonRelease, right, Qt::LeftButton, Qt::NoButton, Qt::AltModifier);
    QCOMPARE(rotateStart.count(), 1); QVERIFY(rotateDrag.count() >= 1); QCOMPARE(rotateFinish.count(), 1);
    QCOMPARE(started.count(), 2); QCOMPARE(finished.count(), 1); // Alt 手势绝不走平移信号。
    QCOMPARE(viewport.SaveCamera(), camera);
    Mouse(viewport, QEvent::MouseButtonPress, center, Qt::LeftButton, Qt::LeftButton, Qt::AltModifier);
    Mouse(viewport, QEvent::MouseMove, right, Qt::NoButton, Qt::LeftButton, Qt::AltModifier);
    QTest::keyClick(&viewport, Qt::Key_Escape);
    QCOMPARE(rotateCancel.count(), 1);
    Mouse(viewport, QEvent::MouseButtonRelease, right, Qt::LeftButton, Qt::NoButton, Qt::AltModifier);
    QCOMPARE(rotateFinish.count(), 1);
    QTest::keyClick(&viewport, Qt::Key_Delete);
    QCOMPARE(deleted.count(), 1);
    QTest::keyClick(&viewport, Qt::Key_C, Qt::ControlModifier);
    QTest::keyClick(&viewport, Qt::Key_V, Qt::ControlModifier);
    QCOMPARE(copy.count(), 1); QCOMPARE(paste.count(), 1);
    QSignalSpy duplicate(&viewport, &WorldViewport::DuplicateRequested), all(&viewport, &WorldViewport::SelectAllRequested);
    QSignalSpy focus(&viewport, &WorldViewport::FocusSelectionRequested), nudge(&viewport, &WorldViewport::NudgeRequested);
    QTest::keyClick(&viewport, Qt::Key_D, Qt::ControlModifier); QTest::keyClick(&viewport, Qt::Key_A, Qt::ControlModifier);
    QTest::keyClick(&viewport, Qt::Key_F); QTest::keyClick(&viewport, Qt::Key_Right); QTest::keyClick(&viewport, Qt::Key_Up, Qt::ShiftModifier);
    QCOMPARE(duplicate.count(), 1); QCOMPARE(all.count(), 1); QCOMPARE(focus.count(), 1); QCOMPARE(nudge.count(), 2);
    QCOMPARE(nudge[0][0].toDouble(), 16.0); QCOMPARE(nudge[1][1].toDouble(), 64.0);
    // Alt + 空处拖动旋转镜头；普通空处单击清除选择。
    Mouse(viewport, QEvent::MouseButtonPress, empty, Qt::LeftButton, Qt::LeftButton, Qt::AltModifier);
    Mouse(viewport, QEvent::MouseMove, empty + QPointF(40, 20), Qt::NoButton, Qt::LeftButton, Qt::AltModifier);
    Mouse(viewport, QEvent::MouseButtonRelease, empty + QPointF(40, 20), Qt::LeftButton, Qt::NoButton, Qt::AltModifier);
    QVERIFY(viewport.SaveCamera()["yaw"] != camera["yaw"]);
    viewport.ResetCamera();
    Mouse(viewport, QEvent::MouseMove, empty, Qt::NoButton, Qt::NoButton);
    QTRY_VERIFY(viewport.HoverCoordinate().has_value());
    clicked.clear();
    QSignalSpy groundClicked(&viewport, &WorldViewport::GroundClicked);
    Mouse(viewport, QEvent::MouseButtonPress, empty, Qt::LeftButton, Qt::LeftButton);
    Mouse(viewport, QEvent::MouseButtonRelease, empty, Qt::LeftButton, Qt::NoButton);
    QVERIFY(clicked.isEmpty()); QCOMPARE(groundClicked.count(), 1); QCOMPARE(groundClicked[0][2].toInt(), 0);
    // 地面手柄：判断通过时空处拖动也改为移动。
    viewport.SetSelectedInstance(std::nullopt);
    viewport.SetHandleTest([](double x, double) { return x < -80; });
    started.clear(); finished.clear();
    Mouse(viewport, QEvent::MouseButtonPress, empty, Qt::LeftButton, Qt::LeftButton);
    Mouse(viewport, QEvent::MouseMove, empty + QPointF(40, 0), Qt::NoButton, Qt::LeftButton);
    Mouse(viewport, QEvent::MouseButtonRelease, empty + QPointF(40, 0), Qt::LeftButton, Qt::NoButton);
    QVERIFY(started.count() == 1 && finished.count() == 1);
    // 摆放模式：单击地面发出摆放请求，不做实例点选；Esc 退出。
    QSignalSpy place(&viewport, &WorldViewport::PlaceRequested);
    QSignalSpy placingCancelled(&viewport, &WorldViewport::PlacingCancelled);
    viewport.SetPlacing(true); clicked.clear();
    Mouse(viewport, QEvent::MouseMove, center, Qt::NoButton, Qt::NoButton);
    QTRY_VERIFY(viewport.HoverCoordinate().has_value());
    Mouse(viewport, QEvent::MouseButtonPress, center, Qt::LeftButton, Qt::LeftButton);
    Mouse(viewport, QEvent::MouseButtonRelease, center, Qt::LeftButton, Qt::NoButton);
    QVERIFY(place.count() == 1 && clicked.isEmpty());
    // 预览模型/动作在按下与松开之间推进场景编号，且松开时仍在等待完成帧：同一单击等到完成帧后摆放，不要求再点。
    state.autoComplete = false;
    Mouse(viewport, QEvent::MouseButtonPress, center, Qt::LeftButton, Qt::LeftButton);
    ++state.sceneEpoch;
    Mouse(viewport, QEvent::MouseButtonRelease, center, Qt::LeftButton, Qt::NoButton);
    QCOMPARE(place.count(), 1);
    state.autoComplete = true;
    QTRY_COMPARE(place.count(), 2);
    // 镜头在按下后改变则取消，不按新镜头落点。
    state.autoComplete = false;
    Mouse(viewport, QEvent::MouseButtonPress, center, Qt::LeftButton, Qt::LeftButton);
    viewport.ResetCamera();
    Mouse(viewport, QEvent::MouseButtonRelease, center, Qt::LeftButton, Qt::NoButton);
    state.autoComplete = true;
    QTest::qWait(100);
    QCOMPARE(place.count(), 2);
    QTest::keyClick(&viewport, Qt::Key_Escape);
    QVERIFY(placingCancelled.count() == 1 && !viewport.Placing());
    viewport.SetPlacing(true);
    Mouse(viewport, QEvent::MouseButtonPress, center, Qt::RightButton, Qt::RightButton);
    QCOMPARE(placingCancelled.count(), 2); QVERIFY(!viewport.Placing());
    QSignalSpy box(&viewport, &WorldViewport::InstancesBoxSelected);
    Mouse(viewport, QEvent::MouseButtonPress, {100, 70}, Qt::LeftButton, Qt::LeftButton, Qt::ShiftModifier);
    Mouse(viewport, QEvent::MouseMove, {210, 170}, Qt::NoButton, Qt::LeftButton, Qt::ShiftModifier);
    Mouse(viewport, QEvent::MouseButtonRelease, {210, 170}, Qt::LeftButton, Qt::NoButton, Qt::ShiftModifier);
    QCOMPARE(box.count(), 1);
    QCOMPARE(box.first().at(0).value<QVector<int>>(), QVector<int>{0});
    QCOMPARE(box.first().at(1).toInt(), static_cast<int>(Qt::ShiftModifier));
    // 巨大模型盒只擦到矩形边不算框选；摆放中心在矩形外。
    scene->modelBounds[0] = {-128, -64, 0, 128, 64, 32};
    auto moved = current->states[37].transform; moved[3] = 80; QVERIFY(transform(moved)); box.clear();
    Mouse(viewport, QEvent::MouseButtonPress, {100, 70}, Qt::LeftButton, Qt::LeftButton, Qt::ShiftModifier);
    Mouse(viewport, QEvent::MouseMove, {210, 170}, Qt::NoButton, Qt::LeftButton, Qt::ShiftModifier);
    Mouse(viewport, QEvent::MouseButtonRelease, {210, 170}, Qt::LeftButton, Qt::NoButton, Qt::ShiftModifier);
    QCOMPARE(box.count(), 1); QVERIFY(box[0][0].value<QVector<int>>().isEmpty());
    // 小模型只按真实三角点选；predicate 收到来源编号37，不以 6px 扩大盒制造命中。
    QVERIFY(transform({0.02f, 0, 0, -0.1f, 0, 0.02f, 0, -0.1f, 0, 0, 0.02f, 0}));
    const auto contentBeforePolicy = viewport.AnimationTarget()->contentVersion;
    const auto uploadsBeforePolicy = state.matrixUploads;
    const auto framesBeforePolicy = state.frames;
    std::uint32_t request = 0; viewport.SetInstanceSelectable([&](std::uint32_t value) { request = value; return value == 37; });
    clicked.clear(); groundClicked.clear();
    Mouse(viewport, QEvent::MouseButtonPress, center, Qt::LeftButton, Qt::LeftButton);
    Mouse(viewport, QEvent::MouseButtonRelease, center, Qt::LeftButton, Qt::NoButton);
    QCOMPARE(request, std::uint32_t{37}); QCOMPARE(clicked.count(), 1); QCOMPARE(clicked[0][0].toInt(), 0);
    // 锁定/选择规则改变后仍能取同一完成姿势，不靠新pose上传或伪造Draw恢复点选。
    QCOMPARE(viewport.AnimationTarget()->contentVersion, contentBeforePolicy);
    QCOMPARE(state.matrixUploads, uploadsBeforePolicy); QCOMPARE(state.frames, framesBeforePolicy);
    Mouse(viewport, QEvent::MouseButtonPress, center + QPointF(5, 0), Qt::LeftButton, Qt::LeftButton, Qt::ShiftModifier);
    Mouse(viewport, QEvent::MouseButtonRelease, center + QPointF(5, 0), Qt::LeftButton, Qt::NoButton, Qt::ShiftModifier);
    QCOMPARE(clicked.count(), 1); QCOMPARE(groundClicked.count(), 1); QCOMPARE(groundClicked[0][2].toInt(), int(Qt::ShiftModifier));
    viewport.SetInstanceSelectable([](std::uint32_t) { return false; }); clicked.clear(); groundClicked.clear();
    Mouse(viewport, QEvent::MouseButtonPress, center, Qt::LeftButton, Qt::LeftButton);
    Mouse(viewport, QEvent::MouseButtonRelease, center, Qt::LeftButton, Qt::NoButton);
    QVERIFY(clicked.isEmpty()); QCOMPARE(groundClicked.count(), 1);
}

void WorldModuleTests::TerrainBrushPreservesCameraAndModels() {
    BackendState state; activeBackend = &state;
    WorldViewport viewport(nullptr, &modelBackend); state.viewport = &viewport;
    viewport.resize(320, 240);
    auto terrain = TestTerrain(); QVERIFY(BindTerrain(viewport, terrain));
    auto scene = std::make_shared<Render::WorldModelScene>();
    QVERIFY(viewport.SetModelScene(scene));
    auto camera = viewport.SaveCamera(); camera["distance"] = 1024;
    QVERIFY(viewport.RestoreCamera(camera));
    viewport.show(); QTRY_VERIFY(state.frames > 0);
    QSignalSpy started(&viewport, &WorldViewport::TerrainStrokeStarted);
    QSignalSpy moved(&viewport, &WorldViewport::TerrainStrokeMoved);
    QSignalSpy finished(&viewport, &WorldViewport::TerrainStrokeFinished);
    QSignalSpy cancelled(&viewport, &WorldViewport::TerrainStrokeCancelled);
    viewport.SetBrushMode(true);
    const QPointF center(160, 120), right(170, 120);
    Mouse(viewport, QEvent::MouseMove, center, Qt::NoButton, Qt::NoButton);
    QTRY_VERIFY(viewport.HoverCoordinate().has_value());
    Mouse(viewport, QEvent::MouseButtonPress, center, Qt::LeftButton, Qt::LeftButton);
    QCOMPARE(started.count(), 1); QVERIFY(viewport.StrokeActive());
    auto edited = std::make_shared<MapData::TerrainGridSnapshot>(*terrain);
    edited->vertices[1].groundHeightRaw += 16;
    MapData::RefreshTerrainDerived(*edited, 0, 0, edited->header.width, edited->header.height);
    auto mesh = BuildWorldTerrainMesh(*edited); QVERIFY(mesh.Succeeded());
    QVERIFY(viewport.ReplaceTerrainPresentation(std::move(mesh.value->vertices), std::move(mesh.value->indices),
        edited, mesh.value->stride, nullptr, 13));
    QCOMPARE(viewport.SaveCamera(), camera);
    QVERIFY(viewport.HasModelScene());
    QCOMPARE(viewport.DocumentRevision(), std::uint64_t{13});
    QTRY_VERIFY(state.frames > 1);
    Mouse(viewport, QEvent::MouseButtonRelease, center, Qt::LeftButton, Qt::NoButton);
    QCOMPARE(finished.count(), 1); QCOMPARE(cancelled.count(), 0);
    QCOMPARE(moved.count(), 0); // 预览抬高后同一屏幕像素不能沿新表面产生额外笔刷点。
    QVERIFY(!viewport.StrokeActive());
    Mouse(viewport, QEvent::MouseMove, center, Qt::NoButton, Qt::NoButton);
    QTRY_VERIFY(viewport.HoverCoordinate().has_value());
    Mouse(viewport, QEvent::MouseButtonPress, center, Qt::LeftButton, Qt::LeftButton);
    QTRY_VERIFY(viewport.StrokeActive());
    Mouse(viewport, QEvent::MouseMove, right, Qt::NoButton, Qt::LeftButton);
    QTest::keyClick(&viewport, Qt::Key_Escape);
    QCOMPARE(cancelled.count(), 1);
    Mouse(viewport, QEvent::MouseButtonRelease, right, Qt::LeftButton, Qt::NoButton);
    QCOMPARE(finished.count(), 1);
    QCOMPARE(viewport.SaveCamera(), camera);
}

void WorldModuleTests::TerrainWaterBrushUsesFrozenPrefixAndSharedUndo_data() {
    QTest::addColumn<bool>("deep");
    QTest::newRow("shallow") << false;
    QTest::newRow("deep") << true;
}

void WorldModuleTests::TerrainWaterBrushUsesFrozenPrefixAndSharedUndo() {
    QFETCH(bool, deep);
    QTemporaryDir directory; QVERIFY(directory.isValid());
    QtProject::EditorProject project;
    Tests::InstallTerrainWaterProject(project, directory.path().toStdWString(), false);
    BackendState state; activeBackend = &state;
    WorldModule module(project, &backend);
    auto* page = module.Widget(); auto* viewport = module.Viewport(); state.viewport = viewport;
    page->resize(900, 600); page->show(); QTRY_COMPARE(viewport->VertexCount(), std::size_t{81});
    auto* brush = page->findChild<QToolButton*>(deep ? "worldTerrainBrushDeepWater" : "worldTerrainBrushShallowWater");
    auto* toggle = page->findChild<QPushButton*>("worldTerrainToggle");
    auto* status = page->findChild<QLabel*>("worldTerrainWaterBrushStatus");
    auto* radius = page->findChild<QSpinBox*>("worldTerrainRadius");
    QVERIFY(brush && toggle && status && radius); radius->setValue(1);
    brush->click();
    QTRY_VERIFY(status->property("ready").toBool());
    const auto captures = status->property("captureCount").toULongLong();
    toggle->click(); QVERIFY(viewport->BrushMode());
    const auto original = Tests::WaterTake(MapData::SerializeMapTerrain(*project.mapEdits_->Terrain()));
    const MapData::TerrainStrokePoint first{384, 384};
    auto request = std::make_shared<QtProject::TerrainWaterEditRequest>(
        Tests::WaterTake(QtProject::CaptureTerrainWaterEditRequest(project, true)));
    auto resources = Tests::WaterTake(QtProject::LoadTerrainWaterEditResources(request));
    const MapData::TerrainWaterBrush expectedBrush{deep ? MapData::TerrainWaterBrushKind::Deep : MapData::TerrainWaterBrushKind::Shallow,
        MapData::TerrainBrushShape::Circle, 1, 0, static_cast<std::uint32_t>(project.mapEdits_->Revision()),
        Tests::WaterTake(MapData::CaptureTerrainWaterStrokeAnchor(*request->terrain, first))};
    const std::array points{first, MapData::TerrainStrokePoint{512, 384}, MapData::TerrainStrokePoint{640, 384}};
    const auto expected = Tests::WaterTake(MapData::PrepareTerrainWaterStroke(*request->terrain,
        *resources.resources.WaterBrushRules(), expectedBrush, points, request->doodads));
    const auto frame = viewport->DocumentRevision();
    viewport->TerrainStrokeStarted(first.x, first.y);
    viewport->TerrainStrokeMoved(points[1].x, points[1].y);
    viewport->TerrainStrokeMoved(points[2].x, points[2].y);
    QTRY_VERIFY(viewport->DocumentRevision() > frame);
    QCOMPARE(Tests::WaterTake(MapData::SerializeMapTerrain(*project.mapEdits_->Terrain())), original);
    QCOMPARE(status->property("captureCount").toULongLong(), captures); // 预览不重新打开来源。
    viewport->TerrainStrokeFinished();
    QTRY_COMPARE(project.mapDataUndoStack_->count(), 1);
    const auto changed = Tests::WaterTake(MapData::SerializeMapTerrain(*project.mapEdits_->Terrain()));
    QCOMPARE(changed, Tests::WaterTake(MapData::SerializeMapTerrain(expected.terrain)));
    QVERIFY(changed != original);
    QVERIFY(status->property("prepareMilliseconds").isValid());
    QTRY_VERIFY(toggle->isChecked()); QTRY_VERIFY(status->property("ready").toBool());
    project.mapDataUndoStack_->undo();
    QCOMPARE(Tests::WaterTake(MapData::SerializeMapTerrain(*project.mapEdits_->Terrain())), original);
    project.mapDataUndoStack_->redo();
    QCOMPARE(Tests::WaterTake(MapData::SerializeMapTerrain(*project.mapEdits_->Terrain())), changed);
    QCOMPARE(project.mapDataUndoStack_->count(), 1);
    QTRY_VERIFY(status->property("ready").toBool());
}

void WorldModuleTests::TerrainWaterBrushRejectsChangedInputs() {
    QTemporaryDir directory; QVERIFY(directory.isValid());
    QtProject::EditorProject project;
    Tests::InstallTerrainWaterProject(project, directory.path().toStdWString(), false);
    BackendState state; activeBackend = &state;
    WorldModule module(project, &backend);
    auto* page = module.Widget(); auto* viewport = module.Viewport(); state.viewport = viewport;
    page->resize(900, 600); page->show(); QTRY_COMPARE(viewport->VertexCount(), std::size_t{81});
    auto* brush = page->findChild<QToolButton*>("worldTerrainBrushShallowWater");
    auto* toggle = page->findChild<QPushButton*>("worldTerrainToggle");
    auto* status = page->findChild<QLabel*>("worldTerrainWaterBrushStatus");
    QVERIFY(brush && toggle && status); brush->click();
    // queued规则捕获完成前改变来源状态；旧成功回调不能重新启用笔刷。
    project.sourceChangeSuspected_ = true;
    project.SourceStateChanged();
    QTRY_VERIFY(!status->property("ready").toBool());
    QVERIFY(!viewport->BrushMode());
    project.sourceChangeSuspected_ = false; project.SourceStateChanged();
    QTRY_VERIFY(status->property("ready").toBool());
    toggle->click(); QVERIFY(viewport->BrushMode());
    viewport->TerrainStrokeStarted(384, 384);
    MapData::TerrainBrush external; external.kind = MapData::TerrainBrushKind::Raise;
    external.size = 1; external.strength = 1;
    const MapData::TerrainStrokePoint elsewhere{128, 128};
    QVERIFY(project.mapEdits_->EditTerrain(external, std::span(&elsewhere, 1), false).Succeeded());
    const auto changed = Tests::WaterTake(MapData::SerializeMapTerrain(*project.mapEdits_->Terrain()));
    viewport->TerrainStrokeFinished();
    QCOMPARE(Tests::WaterTake(MapData::SerializeMapTerrain(*project.mapEdits_->Terrain())), changed);
    QCOMPARE(project.mapDataUndoStack_->count(), 0);
    QTRY_VERIFY(status->property("ready").toBool());
    QTRY_VERIFY(toggle->isChecked());
    project.pendingImports_.push_back({QStringLiteral("TerrainArt/Terrain.slk"), {}, 1,
        std::make_shared<const std::vector<std::byte>>(Tests::WaterText("pending"))});
    viewport->TerrainStrokeStarted(384, 384); viewport->TerrainStrokeFinished();
    QCOMPARE(Tests::WaterTake(MapData::SerializeMapTerrain(*project.mapEdits_->Terrain())), changed);
    QCOMPARE(project.mapDataUndoStack_->count(), 0);
    QVERIFY(!status->property("ready").toBool());
}

void WorldModuleTests::TerrainWaterBrushRejectsChangedGameResources_data() {
    QTest::addColumn<bool>("patch");
    QTest::newRow("loose-after-ready") << false;
    QTest::newRow("patch-after-ready") << true;
}

void WorldModuleTests::TerrainWaterBrushRejectsChangedGameResources() {
    QFETCH(bool, patch);
    QTemporaryDir directory; QVERIFY(directory.isValid());
    QtProject::EditorProject project;
    Tests::InstallTerrainWaterProject(project, directory.path().toStdWString(), false);
    BackendState state; activeBackend = &state;
    WorldModule module(project, &backend);
    auto* page = module.Widget(); auto* viewport = module.Viewport(); state.viewport = viewport;
    page->resize(900, 600); page->show(); QTRY_COMPARE(viewport->VertexCount(), std::size_t{81});
    auto* brush = page->findChild<QToolButton*>("worldTerrainBrushDeepWater");
    auto* toggle = page->findChild<QPushButton*>("worldTerrainToggle");
    auto* status = page->findChild<QLabel*>("worldTerrainWaterBrushStatus");
    QVERIFY(brush && toggle && status); brush->click();
    QTRY_VERIFY(status->property("ready").toBool()); toggle->click();
    const auto original = Tests::WaterTake(MapData::SerializeMapTerrain(*project.mapEdits_->Terrain()));
    if (patch) {
        const std::array members{MapArchiveMember{"UI/WorldEditGameStrings.txt", Tests::WaterText("[Terrain]\nMaxSlope=12\n")}};
        QVERIFY(WriteVerifiedArchive(project.war3GameRoot_ / "war3patch.mpq", members).Succeeded());
    } else {
        std::filesystem::create_directory(project.war3GameRoot_ / "UI");
        QVERIFY(IO::WriteFileBytes(project.war3GameRoot_ / "UI/MiscData.txt", Tests::WaterText("[Terrain]\nMaxSlope=12\n")).Succeeded());
    }
    // 没有地图来源通知，Session和project修订也没变；实际游戏规则仍必须重新核对。
    viewport->TerrainStrokeStarted(384, 384); viewport->TerrainStrokeFinished();
    QTRY_VERIFY(status->property("verifyMilliseconds").isValid());
    QCOMPARE(project.mapDataUndoStack_->count(), 0);
    QCOMPARE(Tests::WaterTake(MapData::SerializeMapTerrain(*project.mapEdits_->Terrain())), original);
    QTRY_VERIFY(toggle->isEnabled());
    QVERIFY(!toggle->isChecked()); // 旧规则的一笔被拒绝；新规则重新捕获不恢复这笔。
}

void WorldModuleTests::TerrainWaterBrushDropsCancelledCommit_data() {
    QTest::addColumn<int>("mode");
    QTest::newRow("escape-pending-commit") << 0;
    QTest::newRow("select-tool-pending-commit") << 1;
    QTest::newRow("place-tool-pending-commit") << 2;
    QTest::newRow("refresh-pending-commit") << 3;
    QTest::newRow("switch-map-pending-commit") << 4;
}

void WorldModuleTests::TerrainWaterBrushDropsCancelledCommit() {
    QFETCH(int, mode);
    QTemporaryDir directory, replacement; QVERIFY(directory.isValid() && replacement.isValid());
    QtProject::EditorProject project;
    Tests::InstallTerrainWaterProject(project, directory.path().toStdWString(), false);
    BackendState state; activeBackend = &state;
    WorldModule module(project, &backend);
    auto* page = module.Widget(); auto* viewport = module.Viewport(); state.viewport = viewport;
    page->resize(900, 600); page->show(); QTRY_COMPARE(viewport->VertexCount(), std::size_t{81});
    auto* brush = page->findChild<QToolButton*>("worldTerrainBrushShallowWater");
    auto* toggle = page->findChild<QPushButton*>("worldTerrainToggle");
    auto* select = page->findChild<QPushButton*>("worldSelectTool");
    auto* place = page->findChild<QPushButton*>("worldPlaceToggle");
    auto* status = page->findChild<QLabel*>("worldTerrainWaterBrushStatus");
    QVERIFY(brush && toggle && select && place && status); brush->click();
    QTRY_VERIFY(status->property("ready").toBool()); toggle->click();
    const auto original = Tests::WaterTake(MapData::SerializeMapTerrain(*project.mapEdits_->Terrain()));
    const auto frame = viewport->DocumentRevision();
    viewport->TerrainStrokeStarted(384, 384);
    QTRY_VERIFY(viewport->DocumentRevision() > frame);
    // 预览已完成，松手启动后台复核；在允许回调执行前取消或切图。
    viewport->TerrainStrokeFinished();
    QVERIFY(!toggle->isEnabled());
    QVERIFY(!module.PrepareNavigationLeave());
    if (mode == 0) {
        // 同步走真实键盘入口；不让QTest配置的键盘延迟先派发后台finished。
        QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QApplication::sendEvent(viewport, &escape);
    } else if (mode == 1) select->click();
    else if (mode == 2) place->click();
    else {
        if (mode == 4)
            Tests::InstallTerrainWaterProject(project, replacement.path().toStdWString(), false);
        module.Refresh();
    }
    const auto expectedIdentity = project.mapEdits_->SessionIdentity();
    QTRY_VERIFY(module.ViewportForTools() != nullptr);
    QTRY_VERIFY(toggle->isEnabled());
    QCOMPARE(project.mapEdits_->SessionIdentity(), expectedIdentity);
    QCOMPARE(project.mapEdits_->Revision(), std::uint64_t{0});
    QCOMPARE(project.mapDataUndoStack_->count(), 0);
    QCOMPARE(Tests::WaterTake(MapData::SerializeMapTerrain(*project.mapEdits_->Terrain())), original);
    QVERIFY(!toggle->isChecked());
    QCOMPARE(place->isChecked(), mode == 2);
    QVERIFY(module.PrepareNavigationLeave());
}

void WorldModuleTests::TerrainBrushUsesSharedTransactionAndUndo() {
    QTemporaryDir directory; QVERIFY(directory.isValid());
    const auto source = TerrainBytes();
    QVERIFY(Write(directory.filePath("war3map.w3e"), source));
    QtProject::EditorProject project;
    project.projectRoot_ = directory.path().toStdWString();
    auto opened = DirectoryWorkspace::Open(project.projectRoot_); QVERIFY(opened.Succeeded());
    project.directorySnapshot_ = std::make_unique<DirectoryWorkspace>(std::move(*opened.value));
    project.mapDocument_ = std::make_unique<MapDocument>(MapDocument::FromMembers(std::span<const MapArchiveMember>{}));
    project.mapEdits_ = std::make_unique<MapData::MapDataEditSession>(*project.mapDocument_);
    QVERIFY(project.mapEdits_->AttachTerrain(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(source.constData()), static_cast<std::size_t>(source.size()))).Succeeded());
    project.ResetEditTracking();
    BackendState state; activeBackend = &state;
    WorldModule module(project, &backend);
    auto* page = module.Widget(); state.viewport = module.Viewport(); page->resize(900, 600); page->show();
    QTRY_COMPARE(module.Viewport()->VertexCount(), std::size_t{6});
    auto* toggle = page->findChild<QPushButton*>("worldTerrainToggle"); QVERIFY(toggle && toggle->isEnabled());
    toggle->click(); QVERIFY(module.Viewport()->BrushMode());
    auto* raise = page->findChild<QToolButton*>("worldTerrainBrushRaise"); QVERIFY(raise);
    raise->click(); QVERIFY(raise->isChecked());
    auto* radius = page->findChild<QSpinBox*>("worldTerrainRadius"); QVERIFY(radius);
    QCOMPARE(radius->minimum(), 1); QCOMPARE(radius->maximum(), 8); radius->setValue(1);
    QVERIFY(page->findChild<QToolButton*>("worldTerrainBrushNoise"));
    QVERIFY(page->findChild<QToolButton*>("worldTerrainBrushCliffTexture"));
    QVERIFY(page->findChild<QToolButton*>("worldTerrainBrushWaterHeight"));
    auto* variation = page->findChild<QComboBox*>("worldTerrainVariation"); QVERIFY(variation);
    QCOMPARE(variation->count(), 19); // 随机和 0..17 共 18 种显式变体。
    auto* waterHeight = page->findChild<QDoubleSpinBox*>("worldTerrainWaterHeight"); QVERIFY(waterHeight);
    QCOMPARE(waterHeight->minimum(), -2048.0); QCOMPARE(waterHeight->maximum(), 2047.75);
    auto* strength = page->findChild<QDoubleSpinBox*>("worldTerrainStrength"); QVERIFY(strength); strength->setValue(6.25);
    auto original = MapData::SerializeMapTerrain(*project.mapEdits_->Terrain()); QVERIFY(original.Succeeded());
    const QPointF center(module.Viewport()->width() / 2.0, module.Viewport()->height() / 2.0);
    const auto overlayBeforeHover = module.Viewport()->OverlayLineCount();
    Mouse(*module.Viewport(), QEvent::MouseMove, center, Qt::NoButton, Qt::NoButton);
    QTRY_VERIFY(module.Viewport()->HoverCoordinate().has_value());
    QTRY_VERIFY(module.Viewport()->OverlayLineCount() > overlayBeforeHover); // 悬停时画出笔刷落点
    const auto revisionBeforeStroke = module.Viewport()->DocumentRevision();
    Mouse(*module.Viewport(), QEvent::MouseButtonPress, center, Qt::LeftButton, Qt::LeftButton);
    Mouse(*module.Viewport(), QEvent::MouseButtonRelease, center, Qt::LeftButton, Qt::NoButton);
    QCOMPARE(project.mapDataUndoStack_->count(), 1);
    QVERIFY(!toggle->isChecked()); // 提交后等待最终地形，不允许继续在旧视图下笔。
    QVERIFY(!module.Viewport()->BrushMode());
    QTRY_VERIFY(module.Viewport()->DocumentRevision() > revisionBeforeStroke);
    QTRY_VERIFY(toggle->isChecked()); // push() 的 indexChanged 不能吞掉提交后的恢复请求。
    QVERIFY(module.Viewport()->BrushMode());
    QCOMPARE(radius->value(), 1);
    QCOMPARE(strength->value(), 6.25); // 最终地形刷新不能把用户强度重置为 32。
    QVERIFY(project.mapEdits_->IsDirty());
    const auto changed = MapData::SerializeMapTerrain(*project.mapEdits_->Terrain()); QVERIFY(changed.Succeeded());
    QVERIFY(*changed.value != *original.value);
    auto* pathingWarning = page->findChild<QLabel*>("worldTerrainPathingWarning"); QVERIFY(pathingWarning);
    QTRY_VERIFY(pathingWarning->isVisible());
    project.mapDataUndoStack_->undo();
    QVERIFY(!toggle->isChecked()); // 外部 Undo 也暂停笔刷，直到回退地形展示完毕。
    const auto restored = MapData::SerializeMapTerrain(*project.mapEdits_->Terrain()); QVERIFY(restored.Succeeded());
    QVERIFY(*restored.value == *original.value);
    const auto revisionBeforeUndoRender = module.Viewport()->DocumentRevision();
    QTRY_VERIFY(module.Viewport()->DocumentRevision() > revisionBeforeUndoRender);
    QTRY_VERIFY(toggle->isChecked());
    QVERIFY(module.Viewport()->BrushMode());
    QCOMPARE(radius->value(), 1);
    QCOMPARE(strength->value(), 6.25);
    QTRY_VERIFY(!pathingWarning->isVisible());
    QTRY_VERIFY(module.Viewport()->HoverCoordinate().has_value());
    // 第二次提交尚未展示时立即从外部 Undo；已过时的提交不得随后恢复笔刷。
    Mouse(*module.Viewport(), QEvent::MouseButtonPress, center, Qt::LeftButton, Qt::LeftButton);
    Mouse(*module.Viewport(), QEvent::MouseButtonRelease, center, Qt::LeftButton, Qt::NoButton);
    QCOMPARE(project.mapDataUndoStack_->count(), 1);
    QVERIFY(!toggle->isChecked());
    project.mapDataUndoStack_->undo();
    const auto revisionBeforeInterruptedUndo = module.Viewport()->DocumentRevision();
    QTRY_VERIFY(module.Viewport()->DocumentRevision() > revisionBeforeInterruptedUndo);
    QVERIFY(!toggle->isChecked());
    QVERIFY(!module.Viewport()->BrushMode());
    toggle->click(); QVERIFY(module.Viewport()->BrushMode());
    Mouse(*module.Viewport(), QEvent::MouseButtonPress, center, Qt::LeftButton, Qt::LeftButton);
    QTest::keyClick(module.Viewport(), Qt::Key_Escape);
    Mouse(*module.Viewport(), QEvent::MouseButtonRelease, center, Qt::LeftButton, Qt::NoButton);
    QCOMPARE(project.mapDataUndoStack_->count(), 1);
    const auto cancelled = MapData::SerializeMapTerrain(*project.mapEdits_->Terrain()); QVERIFY(cancelled.Succeeded());
    QVERIFY(*cancelled.value == *original.value);
    auto* smooth = page->findChild<QToolButton*>("worldTerrainBrushSmooth"); QVERIFY(smooth);
    smooth->click(); QVERIFY(smooth->isChecked());
    QCOMPARE(radius->value(), 1);
    QCOMPARE(strength->value(), 0.5); // 切换到不同强度域时才应用平滑默认值。
}

void WorldModuleTests::TerrainTextureSlotUsesSharedTransaction() {
    QTemporaryDir directory; QVERIFY(directory.isValid());
    const auto source = TerrainBytes(); QVERIFY(Write(directory.filePath("war3map.w3e"), source));
    QtProject::EditorProject project; project.projectRoot_ = directory.path().toStdWString();
    auto opened = DirectoryWorkspace::Open(project.projectRoot_); QVERIFY(opened.Succeeded());
    project.directorySnapshot_ = std::make_unique<DirectoryWorkspace>(std::move(*opened.value));
    project.mapDocument_ = std::make_unique<MapDocument>(MapDocument::FromMembers(std::span<const MapArchiveMember>{}));
    project.mapEdits_ = std::make_unique<MapData::MapDataEditSession>(*project.mapDocument_);
    QVERIFY(project.mapEdits_->AttachTerrain(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(source.constData()), static_cast<std::size_t>(source.size()))).Succeeded());
    project.ResetEditTracking();
    BackendState state; activeBackend = &state;
    WorldModule module(project, &backend);
    auto* page = module.Widget(); state.viewport = module.Viewport(); page->resize(900, 600); page->show();
    QTRY_COMPARE(module.Viewport()->VertexCount(), std::size_t{6});
    auto* replace = page->findChild<QPushButton*>("worldTerrainReplaceTexture"); QVERIFY(replace);
    const auto original = *MapData::SerializeMapTerrain(*project.mapEdits_->Terrain()).value;
    bool accepted = false;
    QTimer::singleShot(0, page, [&] {
        if (auto* dialog = qobject_cast<QInputDialog*>(QApplication::activeModalWidget())) {
            dialog->setTextValue(QStringLiteral("Lgrs")); accepted = true; dialog->accept();
        }
    });
    replace->click(); QVERIFY(accepted);
    QCOMPARE(project.mapEdits_->Terrain()->header.groundTilesets.front(), std::string("Lgrs"));
    QCOMPARE(project.mapDataUndoStack_->count(), 1);
    project.mapDataUndoStack_->undo();
    QCOMPARE(*MapData::SerializeMapTerrain(*project.mapEdits_->Terrain()).value, original);
    project.mapDataUndoStack_->redo();
    QCOMPARE(project.mapEdits_->Terrain()->header.groundTilesets.front(), std::string("Lgrs"));
    QTRY_VERIFY(page->findChild<QPushButton*>("worldTerrainToggle")->isEnabled());
    const auto before = project.mapEdits_->Revision();
    accepted = false;
    bool externalChanged = false;
    QTimer::singleShot(0, page, [&] {
        if (auto* dialog = qobject_cast<QInputDialog*>(QApplication::activeModalWidget())) {
            externalChanged = project.mapEdits_->EditTerrainTextureSlot(MapData::TerrainTextureTable::Ground, 0, "Lrok").Succeeded();
            dialog->setTextValue(QStringLiteral("Ldrt")); accepted = true; dialog->accept();
        }
    });
    replace->click(); QVERIFY(accepted && externalChanged);
    QCOMPARE(project.mapEdits_->Revision(), before + 1);
    QCOMPARE(project.mapEdits_->Terrain()->header.groundTilesets.front(), std::string("Lrok"));
    QCOMPARE(project.mapDataUndoStack_->count(), 1); // 过期对话框不能覆盖外部编辑或增加撤销项。
}

void WorldModuleTests::TerrainBoundaryMarksAndRestores_data() {
    QTest::addColumn<bool>("overlaySupported");
    QTest::newRow("overlay-supported") << true;
    QTest::newRow("overlay-unavailable") << false;
}

void WorldModuleTests::TerrainBoundaryMarksAndRestores() {
    QFETCH(bool, overlaySupported);
    QTemporaryDir directory; QVERIFY(directory.isValid());
    const auto source = TerrainBytes();
    QVERIFY(Write(directory.filePath("war3map.w3e"), source));
    QtProject::EditorProject project; project.projectRoot_ = directory.path().toStdWString();
    auto opened = DirectoryWorkspace::Open(project.projectRoot_); QVERIFY(opened.Succeeded());
    project.directorySnapshot_ = std::make_unique<DirectoryWorkspace>(std::move(*opened.value));
    project.mapDocument_ = std::make_unique<MapDocument>(MapDocument::FromMembers(std::span<const MapArchiveMember>{}));
    project.mapEdits_ = std::make_unique<MapData::MapDataEditSession>(*project.mapDocument_);
    const std::span<const std::byte> bytes(reinterpret_cast<const std::byte*>(source.constData()), static_cast<std::size_t>(source.size()));
    QVERIFY(project.mapEdits_->AttachTerrain(bytes).Succeeded());
    project.ResetEditTracking();
    BackendState state; activeBackend = &state;
    auto api = backend;
    if (overlaySupported) api.setOverlayLines = BoundaryOverlay;
    WorldModule module(project, &api); auto* page = module.Widget(); auto* viewport = module.Viewport();
    state.viewport = viewport; page->resize(900, 600); page->show();
    QTRY_COMPARE(viewport->VertexCount(), std::size_t{6});
    auto* toggle = page->findChild<QPushButton*>("worldTerrainToggle"); QVERIFY(toggle && toggle->isEnabled()); toggle->click();
    auto* boundary = page->findChild<QToolButton*>("worldTerrainBrushBoundary"); QVERIFY(boundary); boundary->click();
    auto* radius = page->findChild<QSpinBox*>("worldTerrainRadius"); QVERIFY(radius); radius->setValue(1);
    auto* status = page->findChild<QLabel*>("worldTerrainBoundaryStatus"); QVERIFY(status);
    QCOMPARE(viewport->HasOverlaySupport(), overlaySupported);
    if (!overlaySupported) QVERIFY(status->text().contains(QStringLiteral("不支持边界标记")));
    QCOMPARE(status->property("boundaryVertexCount").toULongLong(), qulonglong{0});
    viewport->TerrainStrokeStarted(0, -64); viewport->TerrainStrokeFinished();
    QVERIFY(project.mapEdits_->Terrain()->vertices[1].IsBoundary());
    QCOMPARE(project.mapDataUndoStack_->count(), 1);
    QVERIFY(!project.mapEdits_->VerifyTerrainPathingForSave().Succeeded());
    QTRY_COMPARE(status->property("boundaryMarkerCount").toULongLong(), overlaySupported ? qulonglong{1} : qulonglong{0});
    QTRY_VERIFY(toggle->isEnabled() && toggle->isChecked());
    QCOMPARE(state.overlayLines.size(), overlaySupported ? std::size_t{2} : std::size_t{0});
    project.mapDataUndoStack_->undo();
    QTRY_COMPARE(status->property("boundaryMarkerCount").toULongLong(), qulonglong{0});
    QCOMPARE(*MapData::SerializeMapTerrain(*project.mapEdits_->Terrain()).value, std::vector<std::byte>(bytes.begin(), bytes.end()));
    QTRY_VERIFY(toggle->isEnabled() && toggle->isChecked()); project.mapDataUndoStack_->redo();
    QTRY_COMPARE(status->property("boundaryMarkerCount").toULongLong(), overlaySupported ? qulonglong{1} : qulonglong{0});
    QTRY_VERIFY(toggle->isEnabled() && toggle->isChecked());
    auto* clear = page->findChild<QPushButton*>("worldTerrainBoundaryMode1"); QVERIFY(clear); clear->click();
    viewport->TerrainStrokeStarted(0, -64); viewport->TerrainStrokeFinished();
    QCOMPARE(*MapData::SerializeMapTerrain(*project.mapEdits_->Terrain()).value, std::vector<std::byte>(bytes.begin(), bytes.end()));
    QTRY_COMPARE(status->property("boundaryMarkerCount").toULongLong(), qulonglong{0});
}

void WorldModuleTests::TerrainPathingUsesCellsAndPreservesSource_data() {
    QTest::addColumn<bool>("overlaySupported");
    QTest::addColumn<bool>("hasPathing");
    QTest::newRow("pathing-and-overlay") << true << true;
    QTest::newRow("overlay-unavailable") << false << true;
    QTest::newRow("pathing-unavailable") << true << false;
}

void WorldModuleTests::TerrainPathingUsesCellsAndPreservesSource() {
    QFETCH(bool, overlaySupported); QFETCH(bool, hasPathing);
    QTemporaryDir directory; QVERIFY(directory.isValid());
    const auto source = TerrainBytes(); QVERIFY(Write(directory.filePath("war3map.w3e"), source));
    QByteArray wpm("MP3W", 4); Append32(wpm, 0); Append32(wpm, 8); Append32(wpm, 4);
    wpm.append(QByteArray(32, static_cast<char>(0x82))); // 原图不可走和未知位都须保留。
    if (hasPathing) QVERIFY(Write(directory.filePath("war3map.wpm"), wpm));
    QtProject::EditorProject project; project.projectRoot_ = directory.path().toStdWString();
    auto opened = DirectoryWorkspace::Open(project.projectRoot_); QVERIFY(opened.Succeeded());
    project.directorySnapshot_ = std::make_unique<DirectoryWorkspace>(std::move(*opened.value));
    project.mapDocument_ = std::make_unique<MapDocument>(MapDocument::FromMembers(std::span<const MapArchiveMember>{}));
    project.mapEdits_ = std::make_unique<MapData::MapDataEditSession>(*project.mapDocument_);
    const std::span<const std::byte> bytes(reinterpret_cast<const std::byte*>(source.constData()), static_cast<std::size_t>(source.size()));
    QVERIFY(project.mapEdits_->AttachTerrain(bytes).Succeeded());
    if (hasPathing) QVERIFY(project.mapEdits_->AttachPathing(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(wpm.constData()), static_cast<std::size_t>(wpm.size()))).Succeeded());
    project.ResetEditTracking();
    BackendState state; activeBackend = &state;
    auto api = backend; if (overlaySupported) api.setOverlayLines = BoundaryOverlay;
    WorldModule module(project, &api); auto* page = module.Widget(); auto* viewport = module.Viewport();
    state.viewport = viewport; page->resize(900, 600); page->show();
    QTRY_COMPARE(viewport->VertexCount(), std::size_t{6});
    auto* toggle = page->findChild<QPushButton*>("worldTerrainToggle"); QVERIFY(toggle && toggle->isEnabled()); toggle->click();
    auto* tool = page->findChild<QToolButton*>("worldTerrainBrushPathingRestriction"); QVERIFY(tool); tool->click();
    auto* radius = page->findChild<QSpinBox*>("worldTerrainRadius"); QVERIFY(radius); radius->setValue(1);
    auto* fly = page->findChild<QPushButton*>("worldTerrainPathingType1"); QVERIFY(fly); fly->click();
    auto* status = page->findChild<QLabel*>("worldTerrainPathingStatus"); QVERIFY(status);
    if (!hasPathing) {
        QVERIFY(status->text().contains(QStringLiteral("不可读")));
        viewport->TerrainStrokeStarted(-112, -48); viewport->TerrainStrokeFinished();
        QCOMPARE(project.mapDataUndoStack_->count(), 0); QCOMPARE(project.mapEdits_->Revision(), std::uint64_t{0});
        return;
    }
    if (!overlaySupported) QVERIFY(status->text().contains(QStringLiteral("不支持通行格标记")));
    const auto originalTerrain = project.mapEdits_->Terrain();
    const auto flags = [&](double x) { return project.mapEdits_->InspectTerrainPoint({x, -48}).value->pathingFlags; };
    // 两格都靠近同一 W3E 顶点；必须按 32 单位 WPM 格连续落笔，不能吸附到 128 单位顶点。
    viewport->TerrainStrokeStarted(-112, -48); viewport->TerrainStrokeMoved(-80, -48); viewport->TerrainStrokeFinished();
    QCOMPARE(project.mapDataUndoStack_->count(), 1);
    QCOMPARE(flags(-112), std::optional<std::uint8_t>{0x86}); QCOMPARE(flags(-80), std::optional<std::uint8_t>{0x86});
    QCOMPARE(flags(-48), std::optional<std::uint8_t>{0x82}); QCOMPARE(project.mapEdits_->Terrain().get(), originalTerrain.get());
    QVERIFY(toggle->isChecked() && viewport->BrushMode());
    // 笔刷声明的新增限制可正式保存；完整输出须保留头部、其它格和所有来源位。
    QVERIFY(project.mapEdits_->VerifyTerrainPathingForSave().Succeeded());
    const auto replacements = project.mapEdits_->Replacements();
    QCOMPARE(replacements.size(), std::size_t{1});
    QCOMPARE(replacements.front().name, std::string("war3map.wpm"));
    const auto* sourcePathing = reinterpret_cast<const std::byte*>(wpm.constData());
    std::vector<std::byte> expectedPathing(sourcePathing, sourcePathing + wpm.size());
    expectedPathing[16] = std::byte{0x86}; expectedPathing[17] = std::byte{0x86};
    QCOMPARE(replacements.front().data, expectedPathing);
    QTRY_COMPARE(status->property("pathingMarkerCount").toULongLong(), overlaySupported ? qulonglong{2} : qulonglong{0});
    if (overlaySupported) { QCOMPARE(status->property("pathingAddedCellCount").toULongLong(), qulonglong{2}); QVERIFY(!state.overlayLines.empty()); }
    project.mapDataUndoStack_->undo();
    QCOMPARE(flags(-112), std::optional<std::uint8_t>{0x82}); QCOMPARE(flags(-80), std::optional<std::uint8_t>{0x82});
    QVERIFY(project.mapEdits_->Replacements().empty());
    QVERIFY(project.mapEdits_->VerifyTerrainPathingForSave().Succeeded());
    QTRY_COMPARE(status->property("pathingMarkerCount").toULongLong(), qulonglong{0});
    project.mapDataUndoStack_->redo();
    QCOMPARE(flags(-112), std::optional<std::uint8_t>{0x86});
    const auto redone = project.mapEdits_->Replacements();
    QCOMPARE(redone.size(), std::size_t{1}); QCOMPARE(redone.front().data, expectedPathing);
    QVERIFY(project.mapEdits_->VerifyTerrainPathingForSave().Succeeded());
    QTRY_COMPARE(status->property("pathingMarkerCount").toULongLong(), overlaySupported ? qulonglong{2} : qulonglong{0});
    auto* clear = page->findChild<QPushButton*>("worldTerrainPathingMode1"); QVERIFY(clear); clear->click();
    auto* walk = page->findChild<QPushButton*>("worldTerrainPathingType0"); QVERIFY(walk); walk->click();
    const auto beforeOriginalClear = project.mapEdits_->Revision();
    viewport->TerrainStrokeStarted(-112, -48); viewport->TerrainStrokeFinished();
    QCOMPARE(project.mapEdits_->Revision(), beforeOriginalClear); QCOMPARE(flags(-112), std::optional<std::uint8_t>{0x86});
    fly->click();
    viewport->TerrainStrokeStarted(-112, -48); viewport->TerrainStrokeMoved(-80, -48); viewport->TerrainStrokeFinished();
    QCOMPARE(project.mapDataUndoStack_->count(), 2); QCOMPARE(flags(-112), std::optional<std::uint8_t>{0x82});
    QCOMPARE(flags(-80), std::optional<std::uint8_t>{0x82});
    QVERIFY(project.mapEdits_->Replacements().empty());
    QVERIFY(project.mapEdits_->VerifyTerrainPathingForSave().Succeeded());
    QTRY_COMPARE(status->property("pathingMarkerCount").toULongLong(), qulonglong{0});
    project.mapDataUndoStack_->undo(); QCOMPARE(flags(-112), std::optional<std::uint8_t>{0x86});
    viewport->TerrainStrokeStarted(-112, -48);
    MapData::TerrainBrush external; external.kind = MapData::TerrainBrushKind::PathingRestriction;
    external.pathingRestrictions = 0x08; external.size = 1;
    const MapData::TerrainStrokePoint elsewhere{-16, -48};
    QVERIFY(project.mapEdits_->EditTerrain(external, std::span(&elsewhere, 1)).Succeeded());
    const auto externallyChanged = project.mapEdits_->Revision();
    viewport->TerrainStrokeFinished();
    QCOMPARE(project.mapEdits_->Revision(), externallyChanged); QCOMPARE(flags(-112), std::optional<std::uint8_t>{0x86});
    QCOMPARE(flags(-16), std::optional<std::uint8_t>{0x8a}); // 拒绝过期笔划，保留外部编辑。
    viewport->TerrainStrokeStarted(-112, -48); viewport->TerrainStrokeMoved(-112, 64); viewport->TerrainStrokeFinished();
    QCOMPARE(project.mapEdits_->Revision(), externallyChanged); // 北边界半开，坏后续点整笔取消。
    QCOMPARE(*MapData::SerializeMapTerrain(*project.mapEdits_->Terrain()).value, std::vector<std::byte>(bytes.begin(), bytes.end()));
    QFile originalWpm(directory.filePath("war3map.wpm")); QVERIFY(originalWpm.open(QIODevice::ReadOnly));
    QCOMPARE(originalWpm.readAll(), wpm);
    QFile originalW3e(directory.filePath("war3map.w3e")); QVERIFY(originalW3e.open(QIODevice::ReadOnly));
    QCOMPARE(originalW3e.readAll(), source);
}

void WorldModuleTests::TerrainRampKeepsCellAndWholeStroke() {
    QTemporaryDir directory; QVERIFY(directory.isValid());
    auto terrain = FlatGrid(17, 17);
    terrain->header.version = 11; terrain->header.mainTileset = 'L'; terrain->header.offset = {-1024, -1024};
    terrain->header.groundTilesets = {"Ldrt"}; terrain->header.cliffTilesets = {"CLdi"};
    for (std::uint32_t y = 0; y < 17; ++y) for (std::uint32_t x = 0; x < 17; ++x)
        terrain->vertices[y * 17 + x].cliffAndLayerRaw = static_cast<std::uint8_t>(x >= 8 ? 3 : 2);
    MapData::RefreshTerrainDerived(*terrain, 0, 0, 17, 17);
    const auto original = MapData::SerializeMapTerrain(*terrain); QVERIFY(original.Succeeded());
    const QByteArray source(reinterpret_cast<const char*>(original.value->data()), static_cast<qsizetype>(original.value->size()));
    QVERIFY(Write(directory.filePath("war3map.w3e"), source));
    QtProject::EditorProject project; project.projectRoot_ = directory.path().toStdWString();
    auto opened = DirectoryWorkspace::Open(project.projectRoot_); QVERIFY(opened.Succeeded());
    project.directorySnapshot_ = std::make_unique<DirectoryWorkspace>(std::move(*opened.value));
    project.mapDocument_ = std::make_unique<MapDocument>(MapDocument::FromMembers(std::span<const MapArchiveMember>{}));
    project.mapEdits_ = std::make_unique<MapData::MapDataEditSession>(*project.mapDocument_);
    QVERIFY(project.mapEdits_->AttachTerrain(*original.value).Succeeded());
    project.ResetEditTracking();
    BackendState state; activeBackend = &state;
    WorldModule module(project, &backend);
    auto* page = module.Widget(); auto* viewport = module.Viewport(); state.viewport = viewport;
    page->resize(900, 600); page->show();
    QTRY_COMPARE(viewport->VertexCount(), std::size_t{289});
    auto* toggle = page->findChild<QPushButton*>("worldTerrainToggle"); QVERIFY(toggle && toggle->isEnabled());
    toggle->click(); QVERIFY(viewport->BrushMode());
    auto* ramp = page->findChild<QToolButton*>("worldTerrainBrushRamp"); QVERIFY(ramp); ramp->click();
    auto* size = page->findChild<QSpinBox*>("worldTerrainRadius"); QVERIFY(size && !size->isEnabled());
    // x=-32 位于悬崖格的高侧半格；旧顶点吸附会错误落到 x=0 的平地格。
    viewport->TerrainStrokeStarted(-32, 32);
    viewport->TerrainStrokeMoved(-32, 160);
    viewport->TerrainStrokeFinished();
    const auto changed = *MapData::SerializeMapTerrain(*project.mapEdits_->Terrain()).value;
    QVERIFY(changed != *original.value);
    QCOMPARE(std::count_if(project.mapEdits_->Terrain()->vertices.begin(), project.mapEdits_->Terrain()->vertices.end(),
        [](const auto& vertex) { return vertex.IsRamp(); }), 6);
    QCOMPARE(project.mapDataUndoStack_->count(), 1);
    QVERIFY(!project.mapEdits_->VerifyTerrainPathingForSave().Succeeded());
    QTRY_VERIFY(toggle->isEnabled() && toggle->isChecked());
    project.mapDataUndoStack_->undo();
    QCOMPARE(*MapData::SerializeMapTerrain(*project.mapEdits_->Terrain()).value, *original.value);
    QTRY_VERIFY(toggle->isEnabled() && toggle->isChecked());
    project.mapDataUndoStack_->redo();
    QCOMPARE(*MapData::SerializeMapTerrain(*project.mapEdits_->Terrain()).value, changed);
    QTRY_VERIFY(toggle->isEnabled() && toggle->isChecked());
    auto* remove = page->findChild<QPushButton*>("worldTerrainRampMode1"); QVERIFY(remove); remove->click();
    viewport->TerrainStrokeStarted(-32, 32);
    viewport->TerrainStrokeMoved(-32, 160);
    viewport->TerrainStrokeFinished();
    QCOMPARE(*MapData::SerializeMapTerrain(*project.mapEdits_->Terrain()).value, *original.value);
    QTRY_VERIFY(toggle->isEnabled() && toggle->isChecked());
    auto* create = page->findChild<QPushButton*>("worldTerrainRampMode0"); QVERIFY(create); create->click();
    const auto beforeRejected = project.mapEdits_->Revision();
    viewport->TerrainStrokeStarted(-32, 32);
    viewport->TerrainStrokeMoved(-640, 32); // 后续坏点使整笔拒绝，不能留下最初的坡道。
    viewport->TerrainStrokeFinished();
    QCOMPARE(project.mapEdits_->Revision(), beforeRejected);
    QCOMPARE(*MapData::SerializeMapTerrain(*project.mapEdits_->Terrain()).value, *original.value);
}

void WorldModuleTests::InspectorEditsPreserveOtherFloatBits() {
    QTemporaryDir directory; QVERIFY(directory.isValid());
    const auto terrain = TerrainBytes(); QVERIFY(Write(directory.filePath("war3map.w3e"), terrain));
    const MapData::RegionRecord region{-64.25f, -32.125f, 32.375f, 32.875f,
        "RegionOne", 1, {}, "", {0, 0, 0, 255}};
    const MapData::CameraRecord camera{0.125f, 1.125f, 0, 0, 0, 1650, 0, 70, 5000, 100, "CameraOne"};
    const auto w3r = MapData::RegionsDocument{5, {region}, {}}.Serialize();
    const auto w3c = MapData::CamerasDocument{0, {camera}, {}}.Serialize();
    QVERIFY(w3r.Succeeded() && w3r.value && w3c.Succeeded() && w3c.value);
    const std::vector<MapArchiveMember> members{{"war3map.w3r", *w3r.value}, {"war3map.w3c", *w3c.value}};
    QtProject::EditorProject project; project.projectRoot_ = directory.path().toStdWString();
    auto opened = DirectoryWorkspace::Open(project.projectRoot_); QVERIFY(opened.Succeeded());
    project.directorySnapshot_ = std::make_unique<DirectoryWorkspace>(std::move(*opened.value));
    project.mapDocument_ = std::make_unique<MapDocument>(MapDocument::FromMembers(members));
    project.mapEdits_ = std::make_unique<MapData::MapDataEditSession>(*project.mapDocument_);
    project.triggerDocument_ = std::make_unique<Trigger::TriggerDocument>();
    QVERIFY(project.mapEdits_->AttachTerrain(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(terrain.constData()), static_cast<std::size_t>(terrain.size()))).Succeeded());
    project.ResetEditTracking();
    BackendState state; activeBackend = &state;
    WorldModule module(project, &modelBackend);
    auto* page = module.Widget(); state.viewport = module.Viewport(); page->resize(1000, 650); page->show();
    auto* outliner = page->findChild<QTreeView*>("worldOutliner"); QVERIFY(outliner);
    auto* model = dynamic_cast<WorldOutlinerModel*>(outliner->model()); QVERIFY(model);
    const Services::WorldObjectId regionId = MapData::MakeRegionId(0, region), cameraId = MapData::MakeCameraId(0, camera);
    QTRY_VERIFY(FindWorldObject(*model, regionId).isValid() && FindWorldObject(*model, cameraId).isValid()); outliner->expandAll();
    QTest::mouseClick(outliner->viewport(), Qt::LeftButton, {},
        outliner->visualRect(model->Find(regionId)).center());
    auto* left = page->findChild<QDoubleSpinBox*>("worldRegionLeft");
    auto* top = page->findChild<QDoubleSpinBox*>("worldRegionTop");
    QVERIFY(left && top);
    left->setFocus(); left->clearFocus();
    QVERIFY(!project.mapEdits_->IsDirty());
    top->setFocus(); top->setValue(50.0); QTest::keyClick(top, Qt::Key_Return);
    const auto afterRegion = project.mapEdits_->Snapshot(); QVERIFY(afterRegion.Regions());
    const auto& savedRegion = afterRegion.Regions()->entries.front();
    QCOMPARE(savedRegion.top, 50.0f);
    QCOMPARE(std::bit_cast<std::uint32_t>(savedRegion.left), std::bit_cast<std::uint32_t>(region.left));
    QCOMPARE(std::bit_cast<std::uint32_t>(savedRegion.bottom), std::bit_cast<std::uint32_t>(region.bottom));
    QCOMPARE(std::bit_cast<std::uint32_t>(savedRegion.right), std::bit_cast<std::uint32_t>(region.right));
    const auto cameraIndex = FindWorldObject(*model, cameraId);
    QVERIFY(cameraIndex.isValid()); outliner->expandAll(); outliner->scrollTo(cameraIndex); outliner->doItemsLayout();
    QVERIFY(outliner->visualRect(cameraIndex).isValid());
    QTest::mouseClick(outliner->viewport(), Qt::LeftButton, {}, outliner->visualRect(cameraIndex).center());
    auto* x = page->findChild<QDoubleSpinBox*>("worldCameraX"); QVERIFY(x);
    QVERIFY(x->isVisible() && x->isEnabled());
    x->setFocus(); x->setValue(10.0); QTest::keyClick(x, Qt::Key_Return);
    const auto afterCamera = project.mapEdits_->Snapshot(); QVERIFY(afterCamera.Cameras());
    QCOMPARE(afterCamera.Cameras()->entries.front().targetX, 10.0f);
    QCOMPARE(std::bit_cast<std::uint32_t>(afterCamera.Cameras()->entries.front().targetY),
        std::bit_cast<std::uint32_t>(camera.targetY));
    auto* duplicate = page->findChild<QPushButton*>("worldDuplicateAuxiliary"); QVERIFY(duplicate);
    duplicate->click();
    QTRY_VERIFY(project.mapEdits_->Snapshot().Cameras()->entries.size() == 2);
    QCOMPARE(project.mapEdits_->Snapshot().Cameras()->entries[1].name, std::string("CameraOne_copy1"));
    QCOMPARE(project.mapEdits_->Snapshot().Cameras()->entries[1].targetX, 10.0f);
    project.mapDataUndoStack_->undo();
    QCOMPARE(project.mapEdits_->Snapshot().Cameras()->entries.size(), std::size_t{1});
    project.mapDataUndoStack_->redo();
    QTRY_VERIFY(project.mapEdits_->Snapshot().Cameras()->entries.size() == 2);
    const auto regionIndex = FindWorldObject(*model, regionId);
    QVERIFY(regionIndex.isValid()); outliner->expandAll(); outliner->scrollTo(regionIndex); outliner->doItemsLayout();
    QVERIFY(outliner->visualRect(regionIndex).isValid());
    QTest::mouseClick(outliner->viewport(), Qt::LeftButton, {}, outliner->visualRect(regionIndex).center());
    duplicate->click();
    QTRY_VERIFY(project.mapEdits_->Snapshot().Regions()->entries.size() == 2);
    QCOMPARE(project.mapEdits_->Snapshot().Regions()->entries[1].name, std::string("RegionOne_copy1"));
    QCOMPARE(project.mapEdits_->Snapshot().Regions()->entries[1].creationNumber, 2);
    QCOMPARE(project.mapEdits_->Snapshot().Regions()->entries[1].top, 50.0f);
}

void WorldModuleTests::AuxiliaryFormPreservesNativeBytes() {
    const MapData::RegionRecord region{-64.12345f, -32.125f, 32.375f, 32.875f,
        std::string("native\xff", 7), 17, {std::byte{'?'}, std::byte{0}, std::byte{0xff}, std::byte{'x'}},
        std::string("sound\xff", 6), {10, 20, 30, 77}};
    const auto source = MapDocument::FromMembers({});
    WorldAuxiliaryDialog regionDialog(region, source, false, nullptr);
    auto unedited = regionDialog.Record(); QVERIFY(unedited.Succeeded());
    const auto before = MapData::RegionsDocument{5, {region}, {}}.Serialize(); QVERIFY(before.Succeeded());
    const auto unchanged = MapData::RegionsDocument{5, {std::get<MapData::RegionRecord>(*unedited.value)}, {}}.Serialize();
    QVERIFY(unchanged.Succeeded()); QVERIFY(*before.value == *unchanged.value);
    regionDialog.findChild<QLineEdit*>("worldAuxRegionTop")->setText("96.5");
    const auto changed = regionDialog.Record(); QVERIFY(changed.Succeeded());
    const auto& edited = std::get<MapData::RegionRecord>(*changed.value);
    QCOMPARE(edited.top, 96.5f); QCOMPARE(edited.name, region.name); QCOMPARE(edited.ambientSound, region.ambientSound);
    QCOMPARE(edited.weatherId, region.weatherId); QCOMPARE(edited.colorBgra, region.colorBgra);
    QCOMPARE(std::bit_cast<std::uint32_t>(edited.left), std::bit_cast<std::uint32_t>(region.left));
    regionDialog.findChild<QLineEdit*>("worldAuxRegionTop")->setText("not-a-number");
    QVERIFY(!regionDialog.Record().Succeeded());
    const MapData::CameraRecord camera{0.125f, 1.234567f, -0.0f, 45.125f, 304.75f, 1649.875f, 5.25f, 70.125f,
        5000.5f, std::bit_cast<float>(0x42c80001u), std::string("camera\xff", 7)};
    WorldAuxiliaryDialog cameraDialog(camera, source, false, nullptr);
    cameraDialog.findChild<QLineEdit*>("worldAuxCameraRoll")->setText("30.25");
    const auto cameraEdit = cameraDialog.Record(); QVERIFY(cameraEdit.Succeeded());
    const auto& result = std::get<MapData::CameraRecord>(*cameraEdit.value);
    QCOMPARE(result.roll, 30.25f); QCOMPARE(result.name, camera.name);
    QCOMPARE(std::bit_cast<std::uint32_t>(result.zOffset), std::bit_cast<std::uint32_t>(camera.zOffset));
    QCOMPARE(std::bit_cast<std::uint32_t>(result.targetY), std::bit_cast<std::uint32_t>(camera.targetY));
    QCOMPARE(std::bit_cast<std::uint32_t>(result.nearClippingPlane), std::bit_cast<std::uint32_t>(camera.nearClippingPlane));
}

void WorldModuleTests::AuxiliaryResultRejectsStaleSessionAndCoverage() {
    QtProject::EditorProject project;
    const std::vector<MapArchiveMember> members{{"war3map.w3i", MapDataFixtures::FullMapSettingsV25().bytes}};
    project.mapDocument_ = std::make_unique<MapDocument>(MapDocument::FromMembers(members));
    project.mapEdits_ = std::make_unique<MapData::MapDataEditSession>(*project.mapDocument_);
    project.triggerDocument_ = std::make_unique<Trigger::TriggerDocument>();
    Services::AuxiliaryEditRequest request;
    request.action = Services::AuxiliaryEditAction::Create; request.expectedMemberSha256 = "missing";
    const MapData::RegionRecord region{0, 0, 128, 128, "Created", 1, {}, "", {1, 2, 3, 255}};
    request.record = region;
    auto binding = QtProject::CaptureAuxiliaryEditBinding(project); QVERIFY(binding.Succeeded());
    // 字节相同的新会话不能继承旧授权。
    project.mapEdits_ = std::make_unique<MapData::MapDataEditSession>(*project.mapDocument_);
    const auto stale = QtProject::ApplyAuxiliaryEditWithResult(project, *binding.value, request, "create");
    QVERIFY(!stale.Succeeded()); QCOMPARE(stale.diagnostics.front().code, std::string("AUX_PROJECT_STALE"));
    QVERIFY(project.mapEdits_->Replacements().empty()); QCOMPARE(project.mapDataUndoStack_->count(), 0);
    binding = QtProject::CaptureAuxiliaryEditBinding(project); QVERIFY(binding.Succeeded());
    project.triggerDocument_.reset();
    const auto unknown = QtProject::ApplyAuxiliaryEditWithResult(project, *binding.value, request, "create");
    QVERIFY(!unknown.Succeeded()); QCOMPARE(unknown.diagnostics.front().code, std::string("AUX_REFERENCE_COVERAGE"));
    project.triggerDocument_ = std::make_unique<Trigger::TriggerDocument>();
    const auto created = QtProject::ApplyAuxiliaryEditWithResult(project, *binding.value, request, "create");
    QVERIFY(created.Succeeded()); QVERIFY(created.value->changed); QVERIFY(!created.value->rowKey.empty());
    QVERIFY(QtProject::AuxiliaryEditBindingCurrent(project, created.value->binding));
    QCOMPARE(std::get<MapData::RegionId>(created.value->object).name, region.name);
    auto unchangedRequest = request; unchangedRequest.action = Services::AuxiliaryEditAction::Update;
    unchangedRequest.expectedMemberSha256 = created.value->memberSha256; unchangedRequest.expectedRowKey = created.value->rowKey;
    const auto noChange = QtProject::ApplyAuxiliaryEditWithResult(project, created.value->binding, unchangedRequest, "unchanged");
    QVERIFY(noChange.Succeeded()); QVERIFY(!noChange.value->changed);
    QCOMPARE(project.mapEdits_->Revision(), created.value->binding.revision); QCOMPARE(project.mapDataUndoStack_->count(), 1);
    const auto replacements = project.mapEdits_->Replacements(); QCOMPARE(replacements.size(), std::size_t{1});
    QCOMPARE(replacements.front().name, std::string("war3map.w3r"));
    auto rereadMembers = members; rereadMembers.insert(rereadMembers.end(), replacements.begin(), replacements.end());
    const auto reread = MapDocument::FromMembers(rereadMembers);
    QVERIFY(reread.Regions()); QCOMPARE(reread.Regions()->entries.front().name, region.name);
    project.mapDataUndoStack_->undo(); QVERIFY(project.mapEdits_->Replacements().empty());
    QCOMPARE(project.mapEdits_->Snapshot().Member(MapDataKind::Regions).state, MapDataState::Missing);
    project.mapDataUndoStack_->redo(); QCOMPARE(project.mapEdits_->Replacements().front().data, replacements.front().data);
    // 缺席镜头成员同样输出真实原生成员，不借用字段密封。
    request.kind = MapDataKind::Cameras;
    request.record = MapData::CameraRecord{64, 64, 0, 90, 304, 1650, 0, 70, 5000, 100, "CreatedCamera"};
    binding = QtProject::CaptureAuxiliaryEditBinding(project); QVERIFY(binding.Succeeded());
    const auto camera = QtProject::ApplyAuxiliaryEditWithResult(project, *binding.value, request, "camera");
    QVERIFY(camera.Succeeded()); QCOMPARE(std::get<MapData::CameraId>(camera.value->object).name, std::string("CreatedCamera"));
    QCOMPARE(project.mapEdits_->Replacements().size(), std::size_t{2});
    const auto revision = project.mapEdits_->Revision();
    project.savingMap_ = true;
    QVERIFY(!QtProject::ApplyAuxiliaryEditWithResult(project, camera.value->binding, request, "save race").Succeeded());
    QCOMPARE(project.mapEdits_->Revision(), revision); project.savingMap_ = false;
    project.sourceChangeSuspected_ = true;
    QVERIFY(!QtProject::CaptureAuxiliaryEditBinding(project).Succeeded());
    QVERIFY(!QtProject::ApplyAuxiliaryEditWithResult(project, camera.value->binding, request, "source race").Succeeded());
    project.sourceChangeSuspected_ = false; project.sourceChangeConfirmed_ = true;
    QVERIFY(!QtProject::AuxiliaryEditBindingCurrent(project, camera.value->binding));
    QCOMPARE(project.mapEdits_->Revision(), revision); project.sourceChangeConfirmed_ = false;
}

void WorldModuleTests::WorldAuxiliaryCrudUsesSharedService() {
    QTemporaryDir directory; QVERIFY(directory.isValid());
    const MapData::RegionRecord region{0, 0, 128, 128, "RegionOne", 17, {}, "", {10, 20, 30, 77}};
    const MapData::CameraRecord camera{256, 256, 7, 0, 0, 1650, 0, 70, 5000, 100, "CameraOne"};
    QtProject::EditorProject project; QVERIFY(InstallAuxiliaryEditFixture(project, directory.path(), region, camera));
    BackendState state; activeBackend = &state; WorldModule module(project, &modelBackend);
    auto* page = module.Widget(); state.viewport = module.Viewport(); page->resize(1200, 760); page->show();
    auto* outliner = page->findChild<QTreeView*>("worldOutliner"); QVERIFY(outliner);
    auto* model = dynamic_cast<WorldOutlinerModel*>(outliner->model()); QVERIFY(model);
    QTRY_VERIFY(FindWorldObject(*model, MapData::MakeRegionId(0, region)).isValid());
    auto* create = page->findChild<QAction*>("worldNewRegion"); QVERIFY(create); create->trigger();
    auto* dialog = dynamic_cast<WorldAuxiliaryDialog*>(page->findChild<QDialog*>("worldAuxiliaryDialog")); QVERIFY(dialog);
    dialog->findChild<QLineEdit*>("worldAuxName")->setText("WorldCreated"); dialog->accept();
    QTRY_COMPARE(project.mapEdits_->Snapshot().Regions()->entries.size(), std::size_t{2});
    QCOMPARE(project.mapDataUndoStack_->count(), 1);
    const auto created = project.mapEdits_->Snapshot().Regions()->entries.back();
    const Services::WorldObjectId createdId = MapData::MakeRegionId(1, created);
    QTRY_VERIFY(model->Find(createdId).isValid());
    QTRY_VERIFY(outliner->selectionModel()->isSelected(model->Find(createdId)));
    QTRY_VERIFY(page->findChild<QDialog*>("worldAuxiliaryDialog") == nullptr);
    auto* properties = page->findChild<QPushButton*>("worldAuxiliaryProperties"); QVERIFY(properties && properties->isEnabled());
    properties->click(); dialog = dynamic_cast<WorldAuxiliaryDialog*>(page->findChild<QDialog*>("worldAuxiliaryDialog")); QVERIFY(dialog);
    dialog->findChild<QLineEdit*>("worldAuxName")->setText("WorldRenamed");
    dialog->findChild<QLineEdit*>("worldAuxRegionTop")->setText("320"); dialog->accept();
    QTRY_COMPARE(project.mapEdits_->Snapshot().Regions()->entries.back().name, std::string("WorldRenamed"));
    const auto renamed = project.mapEdits_->Snapshot().Regions()->entries.back();
    QCOMPARE(renamed.top, 320.0f); QCOMPARE(renamed.creationNumber, created.creationNumber);
    QTRY_VERIFY(model->Find(MapData::MakeRegionId(1, renamed)).isValid());
    QTRY_VERIFY(outliner->selectionModel()->isSelected(model->Find(MapData::MakeRegionId(1, renamed))));
    const auto savedBytes = project.mapEdits_->Snapshot().Regions()->Serialize(); QVERIFY(savedBytes.Succeeded());
    auto* remove = page->findChild<QPushButton*>("worldDeleteAuxiliary"); QVERIFY(remove && remove->isEnabled()); remove->click();
    QCOMPARE(project.mapEdits_->Snapshot().Regions()->entries.size(), std::size_t{1});
    QCOMPARE(project.mapDataUndoStack_->count(), 3);
    project.mapDataUndoStack_->undo();
    QCOMPARE(*project.mapEdits_->Snapshot().Regions()->Serialize().value, *savedBytes.value);
    project.mapDataUndoStack_->redo(); QCOMPARE(project.mapEdits_->Snapshot().Regions()->entries.size(), std::size_t{1});
    QTRY_VERIFY(page->findChild<QDialog*>("worldAuxiliaryDialog") == nullptr);
    auto* createCamera = page->findChild<QAction*>("worldNewCamera"); QVERIFY(createCamera); createCamera->trigger();
    dialog = dynamic_cast<WorldAuxiliaryDialog*>(page->findChild<QDialog*>("worldAuxiliaryDialog")); QVERIFY(dialog);
    dialog->findChild<QLineEdit*>("worldAuxName")->setText("WorldCamera");
    dialog->findChild<QLineEdit*>("worldAuxCameraFar")->setText("6000"); dialog->accept();
    QTRY_COMPARE(project.mapEdits_->Snapshot().Cameras()->entries.size(), std::size_t{2});
    const auto createdCamera = project.mapEdits_->Snapshot().Cameras()->entries.back();
    QCOMPARE(createdCamera.farClippingPlane, 6000.0f);
    const Services::WorldObjectId createdCameraId = MapData::MakeCameraId(1, createdCamera);
    QTRY_VERIFY(model->Find(createdCameraId).isValid());
    QTRY_VERIFY(outliner->selectionModel()->isSelected(model->Find(createdCameraId)));
    QTRY_VERIFY(page->findChild<QDialog*>("worldAuxiliaryDialog") == nullptr);
    properties->click(); dialog = dynamic_cast<WorldAuxiliaryDialog*>(page->findChild<QDialog*>("worldAuxiliaryDialog")); QVERIFY(dialog);
    dialog->findChild<QLineEdit*>("worldAuxCameraRoll")->setText("12.5"); dialog->accept();
    const auto editedCamera = project.mapEdits_->Snapshot().Cameras()->entries.back();
    QCOMPARE(editedCamera.roll, 12.5f);
    QCOMPARE(std::bit_cast<std::uint32_t>(editedCamera.nearClippingPlane), std::bit_cast<std::uint32_t>(createdCamera.nearClippingPlane));
    remove->click(); QCOMPARE(project.mapEdits_->Snapshot().Cameras()->entries.size(), std::size_t{1});
    QCOMPARE(project.mapDataUndoStack_->count(), 6);
    QTRY_VERIFY(FindWorldObject(*model, MapData::MakeCameraId(0, camera)).isValid());
    QVERIFY(SelectAuxiliaryRow(*page, MapData::MakeCameraId(0, camera)));
    project.triggerDocument_.reset(); // 缺少引用覆盖，副本也不能绕过共用服务。
    const auto before = project.mapEdits_->Revision(); const auto count = project.mapDataUndoStack_->count();
    auto* duplicate = page->findChild<QPushButton*>("worldDuplicateAuxiliary"); QVERIFY(duplicate); duplicate->click();
    remove->click(); // 删除也必须命中同一引用覆盖门。
    QCOMPARE(project.mapEdits_->Revision(), before); QCOMPARE(project.mapDataUndoStack_->count(), count);
    QCOMPARE(project.mapEdits_->Snapshot().Cameras()->entries.size(), std::size_t{1});
}

void WorldModuleTests::OutlinerPagingRestoresAuxiliarySelection_data() {
    QTest::addColumn<bool>("selectCamera");
    QTest::newRow("region") << false;
    QTest::newRow("camera") << true;
}

void WorldModuleTests::OutlinerPagingRestoresAuxiliarySelection() {
    QFETCH(bool, selectCamera);
    QTemporaryDir directory; QVERIFY(directory.isValid());
    const MapData::RegionRecord region{0, 0, 128, 128, "PagedRegion", 17, {}, "", {0, 0, 0, 255}};
    const MapData::CameraRecord camera{256, 256, 0, 0, 0, 1650, 0, 70, 5000, 100, "PagedCamera"};
    QtProject::EditorProject project; QVERIFY(InstallAuxiliaryEditFixture(project, directory.path(), region, camera));
    BackendState state; activeBackend = &state; WorldModule module(project, &modelBackend);
    auto* page = module.Widget(); state.viewport = module.Viewport(); page->resize(1200, 760); page->show();
    auto* outliner = page->findChild<QTreeView*>("worldOutliner"); QVERIFY(outliner);
    auto* model = dynamic_cast<WorldOutlinerModel*>(outliner->model()); QVERIFY(model);
    const Services::WorldObjectId selectedId = selectCamera
        ? Services::WorldObjectId(MapData::MakeCameraId(0, camera))
        : Services::WorldObjectId(MapData::MakeRegionId(0, region));
    QTRY_VERIFY(FindWorldObject(*model, selectedId).isValid());
    QVERIFY(SelectAuxiliaryRow(*page, selectedId));
    QTRY_VERIFY(outliner->selectionModel()->isSelected(model->Find(selectedId)));
    const auto revision = project.mapEdits_->Revision();
    QSignalSpy resets(model, &QAbstractItemModel::modelReset);
    QSignalSpy pages(model, &WorldOutlinerModel::PageLoaded);
    // 正常地图刷新先读空的 Units 页。测试随后只等待 QTreeView 自动分页，
    // 不调用 FindWorldObject / fetchMore / “继续读取”，也不重选该对象。
    project.MapDocumentChanged();
    QVERIFY(!resets.empty()); QVERIFY(!pages.empty());
    QVERIFY(!model->Find(selectedId).isValid());
    QTRY_VERIFY(model->Find(selectedId).isValid());
    QTRY_VERIFY(outliner->selectionModel()->isSelected(model->Find(selectedId)));
    QCOMPARE(outliner->selectionModel()->selectedRows().size(), 1);
    QCOMPARE(outliner->selectionModel()->currentIndex(), model->Find(selectedId));
    QVERIFY(pages.size() > 1);
    QCOMPARE(project.mapEdits_->Revision(), revision);
    QCOMPARE(project.mapDataUndoStack_->count(), 0);
}

void WorldModuleTests::WorldAuxiliaryDialogRejectsChangedMap() {
    QTemporaryDir directory; QVERIFY(directory.isValid());
    const MapData::RegionRecord region{0, 0, 128, 128, "RegionOne", 17, {}, "", {0, 0, 0, 255}};
    const MapData::CameraRecord camera{256, 256, 0, 0, 0, 1650, 0, 70, 5000, 100, "CameraOne"};
    QtProject::EditorProject project; QVERIFY(InstallAuxiliaryEditFixture(project, directory.path(), region, camera));
    BackendState state; activeBackend = &state; WorldModule module(project, &modelBackend);
    auto* page = module.Widget(); state.viewport = module.Viewport(); page->resize(1200, 760); page->show();
    auto* outliner = page->findChild<QTreeView*>("worldOutliner"); QVERIFY(outliner);
    auto* model = dynamic_cast<WorldOutlinerModel*>(outliner->model()); QVERIFY(model);
    QTRY_VERIFY(FindWorldObject(*model, MapData::MakeRegionId(0, region)).isValid());
    QVERIFY(SelectAuxiliaryRow(*page, MapData::MakeRegionId(0, region)));
    auto* properties = page->findChild<QPushButton*>("worldAuxiliaryProperties"); QVERIFY(properties); properties->click();
    auto* dialog = dynamic_cast<WorldAuxiliaryDialog*>(page->findChild<QDialog*>("worldAuxiliaryDialog")); QVERIFY(dialog);
    dialog->findChild<QLineEdit*>("worldAuxRegionTop")->setText("999");
    // 保持同样成员字节与行键，只更换实际编辑会话，旧弹窗仍须拒绝。
    project.mapEdits_ = std::make_unique<MapData::MapDataEditSession>(*project.mapDocument_);
    dialog->accept();
    QCOMPARE(project.mapEdits_->Snapshot().Regions()->entries.front().top, region.top);
    QCOMPARE(project.mapDataUndoStack_->count(), 0);
}

void WorldModuleTests::StartLocationMarkerCaptureIsBounded() {
    auto units = MapData::ParseUnitPlacements(MapDataFixtures::EmptyUnitsV8Sub11()); QVERIFY(units.Succeeded());
    MapData::UnitPlacement start; start.rawcode = "sloc"; start.scale = {1, 1, 1}; start.owner = 0;
    for (int index = 0; index < 4; ++index) { start.creationId = index + 1; start.position = {float(index * 64), 0, 0}; units.value->units.push_back(start); }
    const auto bytes = units.value->Serialize(); QVERIFY(bytes.Succeeded());
    const std::vector<MapArchiveMember> members{{"war3mapUnits.doo", *bytes.value}};
    const auto source = MapDocument::FromMembers(members);
    const auto capture = CaptureWorldStartLocations(source, 2); QVERIFY(capture.Succeeded());
    QCOMPARE(capture.value->sourceCount, std::size_t{4}); QCOMPARE(capture.value->omittedCount, std::size_t{2});
    QCOMPARE(capture.value->markers.size(), std::size_t{2});
    QVERIFY(capture.value->markers[1].id == MapData::MakePlacementId(MapData::PlacementFile::Units, 1, units.value->units[1]));
    QCOMPARE(capture.value->markers[1].position, units.value->units[1].position);
    QVERIFY(!CaptureWorldStartLocations(source, 0).Succeeded());
    auto ambiguous = members; ambiguous.push_back(members.front());
    QVERIFY(!CaptureWorldStartLocations(MapDocument::FromMembers(ambiguous)).Succeeded());
    const auto empty = CaptureWorldStartLocations(MapDocument::FromMembers({})); QVERIFY(empty.Succeeded());
    QVERIFY(empty.value->markers.empty());
}

void WorldModuleTests::StartLocationDragLinksSettingsAndUndo() {
    QTemporaryDir directory; QVERIFY(directory.isValid()); QtProject::EditorProject project;
    QVERIFY(InstallStartLocationEditFixture(project, directory.path()));
    const auto before = project.mapEdits_->Snapshot();
    const auto id = MapData::MakePlacementId(MapData::PlacementFile::Units, 0, before.Units()->units[0]);
    const auto unitsBefore = before.Member(MapDataKind::Units).sources.front().data;
    const auto settingsBefore = before.Member(MapDataKind::Settings).sources.front().data;
    BackendState state; activeBackend = &state;
    state.viewProjection[0] = state.viewProjection[5] = 1 / 192.0f;
    state.viewProjection[12] = state.viewProjection[13] = -64 / 192.0f;
    auto api = modelBackend; api.setOverlayLines = BoundaryOverlay;
    WorldModule module(project, &api); auto* page = module.Widget(); auto* viewport = module.Viewport(); state.viewport = viewport;
    page->resize(1200, 760); page->show();
    auto* outliner = page->findChild<QTreeView*>("worldOutliner"); QVERIFY(outliner);
    auto* model = dynamic_cast<WorldOutlinerModel*>(outliner->model()); QVERIFY(model);
    QTRY_VERIFY(viewport->VertexCount() > 0 && state.frames > 0 && !state.overlayLines.empty());
    QTRY_VERIFY(FindWorldObject(*model, id).isValid());
    const auto start = MoveAuxiliaryPointer(*viewport, state, {64, 64});
    const auto end = AuxiliaryScreenPoint(*viewport, state, {96, 80});
    Mouse(*viewport, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
    Mouse(*viewport, QEvent::MouseButtonRelease, start, Qt::LeftButton, Qt::NoButton);
    QTRY_VERIFY(outliner->selectionModel()->isSelected(model->Find(id)));
    QSignalSpy started(viewport, &WorldViewport::MoveStarted);
    const auto camera = viewport->SaveCamera(); const auto revision = project.mapEdits_->Revision();
    Mouse(*viewport, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
    Mouse(*viewport, QEvent::MouseMove, end, Qt::NoButton, Qt::LeftButton);
    QCOMPARE(started.count(), 1); QCOMPARE(project.mapEdits_->Revision(), revision);
    QTest::keyClick(viewport, Qt::Key_Escape);
    Mouse(*viewport, QEvent::MouseButtonRelease, end, Qt::LeftButton, Qt::NoButton);
    QCOMPARE(project.mapDataUndoStack_->count(), 0);
    QCOMPARE(project.mapEdits_->Snapshot().Member(MapDataKind::Units).sources.front().data, unitsBefore);
    const auto stableStart = MoveAuxiliaryPointer(*viewport, state, {64, 64});
    const auto stableEnd = AuxiliaryScreenPoint(*viewport, state, {96, 80});
    Mouse(*viewport, QEvent::MouseButtonPress, stableStart, Qt::LeftButton, Qt::LeftButton);
    Mouse(*viewport, QEvent::MouseMove, stableEnd, Qt::NoButton, Qt::LeftButton);
    Mouse(*viewport, QEvent::MouseButtonRelease, stableEnd, Qt::LeftButton, Qt::NoButton);
    QCOMPARE(project.mapDataUndoStack_->count(), 1); QCOMPARE(project.mapEdits_->Revision(), revision + 1);
    const auto after = project.mapEdits_->Snapshot(); const auto moved = after.Units()->units.front();
    QCOMPARE(moved.position[0], 96.0f); QCOMPARE(moved.position[1], 80.0f); QCOMPARE(moved.position[2], 5.0f);
    QCOMPARE(after.Settings()->players.front().startX, moved.position[0]);
    QCOMPARE(after.Settings()->players.front().startY, moved.position[1]);
    QCOMPARE(viewport->SaveCamera(), camera);
    QCOMPARE(module.TerrainOnlySceneDiagnosticSnapshot()["model_requests"].toInt(), 0);
    QCOMPARE(module.TerrainOnlySceneDiagnosticSnapshot()["start_location_marker_count"].toInt(), 1);
    project.mapDataUndoStack_->undo();
    QCOMPARE(project.mapEdits_->Snapshot().Member(MapDataKind::Units).sources.front().data, unitsBefore);
    QCOMPARE(project.mapEdits_->Snapshot().Member(MapDataKind::Settings).sources.front().data, settingsBefore);
    project.mapDataUndoStack_->redo();
    QCOMPARE(project.mapEdits_->Snapshot().Units()->units.front().position, moved.position);
    QVERIFY(model->setData(model->Find(id).siblingAtColumn(1), Qt::Unchecked, Qt::CheckStateRole));
    QTRY_VERIFY(state.overlayLines.empty());
    const auto beforeHidden = project.mapEdits_->Revision();
    viewport->MoveStarted(96, 80); viewport->MoveDragged(128, 96); viewport->MoveFinished();
    QCOMPARE(project.mapEdits_->Revision(), beforeHidden);
}

void WorldModuleTests::StartLocationAmbiguityRejectsWholeSelection() {
    QTemporaryDir directory; QVERIFY(directory.isValid()); QtProject::EditorProject project;
    QVERIFY(InstallStartLocationEditFixture(project, directory.path(), true, true));
    const auto before = project.mapEdits_->Snapshot();
    const auto start = MapData::MakePlacementId(MapData::PlacementFile::Units, 0, before.Units()->units[0]);
    const auto ordinary = MapData::MakePlacementId(MapData::PlacementFile::Units, 2, before.Units()->units[2]);
    BackendState state; activeBackend = &state; auto api = modelBackend; api.setOverlayLines = BoundaryOverlay;
    WorldModule module(project, &api); auto* page = module.Widget(); state.viewport = module.Viewport(); page->resize(1200, 760); page->show();
    auto* outliner = page->findChild<QTreeView*>("worldOutliner"); QVERIFY(outliner);
    auto* model = dynamic_cast<WorldOutlinerModel*>(outliner->model()); QVERIFY(model);
    QTRY_VERIFY(module.Viewport()->VertexCount() > 0 && FindWorldObject(*model, ordinary).isValid());
    QVERIFY(SelectAuxiliaryRow(*page, start)); QVERIFY(SelectAuxiliaryRow(*page, ordinary, Qt::ShiftModifier));
    const auto revision = project.mapEdits_->Revision();
    module.Viewport()->MoveStarted(64, 64); module.Viewport()->MoveDragged(80, 80); module.Viewport()->MoveFinished();
    QCOMPARE(project.mapEdits_->Revision(), revision); QCOMPARE(project.mapDataUndoStack_->count(), 0);
    QCOMPARE(project.mapEdits_->Snapshot().Member(MapDataKind::Units).sources.front().data,
        before.Member(MapDataKind::Units).sources.front().data);
    QCOMPARE(project.mapEdits_->Snapshot().Member(MapDataKind::Settings).sources.front().data,
        before.Member(MapDataKind::Settings).sources.front().data);
}

void WorldModuleTests::RegionEdgesUseSharedTableTransaction_data() {
    QTest::addColumn<QPointF>("start");
    QTest::addColumn<QPointF>("end");
    QTest::addColumn<QList<float>>("expected");
    QTest::addColumn<float>("right");
    QTest::addColumn<float>("span");
    const auto row = [](const char* name, QPointF start, QPointF end, QList<float> expected, float right = 128) {
        // 两份真实拾取矩阵的缩放比例，不把只改相机字段当成缩放回归。
        for (const float span : {192.0f, 384.0f}) {
            const auto label = QByteArray(name) + '-' + QByteArray::number(span);
            QTest::newRow(label.constData()) << start << end << expected << right << span;
        }
    };
    row("left", {0, 64}, {45, 77}, {32, 0, 128, 128});
    row("near-left", {4, 64}, {49, 77}, {32, 0, 128, 128});
    row("bottom", {64, 0}, {77, 45}, {0, 32, 128, 128});
    row("right", {128, 64}, {83, 77}, {0, 0, 96, 128});
    row("top", {64, 128}, {77, 83}, {0, 0, 128, 96});
    row("left-cross", {0, 64}, {192, 77}, {96, 0, 128, 128});
    row("bottom-cross", {64, 0}, {77, 192}, {0, 96, 128, 128});
    row("right-cross", {128, 64}, {-32, 77}, {0, 0, 32, 128});
    row("top-cross", {64, 128}, {77, -32}, {0, 0, 128, 32});
    row("southwest-priority", {0, 0}, {45, 45}, {32, 32, 128, 128});
    row("southeast-priority", {128, 0}, {83, 45}, {0, 32, 96, 128});
    row("northeast-priority", {128, 128}, {83, 83}, {0, 0, 96, 96});
    row("northwest-priority", {0, 128}, {45, 83}, {32, 0, 128, 96});
    row("fractional-opposite", {0, 64}, {192, 77}, {96, 0, 129.25f, 128}, 129.25f);
    row("parallel-noop", {0, 64}, {0, 96}, {0, 0, 128, 128});
}

void WorldModuleTests::RegionEdgesUseSharedTableTransaction() {
    QFETCH(QPointF, start); QFETCH(QPointF, end); QFETCH(QList<float>, expected);
    QFETCH(float, right); QFETCH(float, span);
    QTemporaryDir directory; QVERIFY(directory.isValid());
    const MapData::RegionRecord region{0, 0, right, 128, "EdgeRegion", 17, {}, "", {10, 20, 30, 255}};
    const MapData::CameraRecord camera{256, 256, 7, 0, 0, 1650, 0, 70, 5000, 100, "CameraOne"};
    QtProject::EditorProject project;
    QVERIFY(InstallAuxiliaryEditFixture(project, directory.path(), region, camera));
    const auto original = project.mapEdits_->Snapshot().Regions()->Serialize(); QVERIFY(original.Succeeded());
    const auto cameraOriginal = project.mapEdits_->Snapshot().Cameras()->Serialize(); QVERIFY(cameraOriginal.Succeeded());
    BackendState state; activeBackend = &state;
    state.viewProjection[0] = state.viewProjection[5] = 1 / span;
    state.viewProjection[12] = state.viewProjection[13] = -64 / span;
    WorldModule module(project, &modelBackend);
    MapEntitiesModule tableModule(project);
    auto* table = tableModule.Widget()->findChild<QTableWidget*>("mapEntitiesTable2"); QVERIFY(table);
    auto* page = module.Widget(); auto* viewport = module.Viewport(); state.viewport = viewport;
    page->resize(1200, 760); page->show();
    auto* outliner = page->findChild<QTreeView*>("worldOutliner"); QVERIFY(outliner);
    auto* model = dynamic_cast<WorldOutlinerModel*>(outliner->model()); QVERIFY(model);
    const Services::WorldObjectId id = MapData::MakeRegionId(0, region);
    QTRY_VERIFY(FindWorldObject(*model, id).isValid());
    QVERIFY(SelectAuxiliaryRow(*page, id));
    QTRY_VERIFY(viewport->VertexCount() > 0 && state.frames > 0);
    const auto screen = [&](QPointF world) {
        return AuxiliaryScreenPoint(*viewport, state, world, span);
    };
    Mouse(*viewport, QEvent::MouseMove, screen(start), Qt::NoButton, Qt::NoButton);
    QTRY_VERIFY(viewport->HoverCoordinate().has_value());
    const auto beforeCamera = viewport->SaveCamera();
    const auto beforeRevision = project.mapEdits_->Revision();
    QSignalSpy started(viewport, &WorldViewport::MoveStarted);
    QSignalSpy finished(viewport, &WorldViewport::MoveFinished);
    QSignalSpy cancelled(viewport, &WorldViewport::MoveCancelled);
    Mouse(*viewport, QEvent::MouseButtonPress, screen(start), Qt::LeftButton, Qt::LeftButton);
    Mouse(*viewport, QEvent::MouseMove, screen(end), Qt::NoButton, Qt::LeftButton);
    QTest::keyClick(viewport, Qt::Key_Escape);
    Mouse(*viewport, QEvent::MouseButtonRelease, screen(end), Qt::LeftButton, Qt::NoButton);
    QCOMPARE(cancelled.count(), 1); QCOMPARE(finished.count(), 0);
    QCOMPARE(project.mapEdits_->Revision(), beforeRevision); QCOMPARE(project.mapDataUndoStack_->count(), 0);
    started.clear();
    Mouse(*viewport, QEvent::MouseButtonPress, screen(start), Qt::LeftButton, Qt::LeftButton);
    Mouse(*viewport, QEvent::MouseMove, screen(end), Qt::NoButton, Qt::LeftButton);
    QCOMPARE(started.count(), 1);
    QCOMPARE(project.mapEdits_->Revision(), beforeRevision);
    QCOMPARE(project.mapDataUndoStack_->count(), 0);
    QCOMPARE(table->item(0, 1)->text().toFloat(), region.left); // 表格仍是已提交数据
    auto* draftLeft = page->findChild<QDoubleSpinBox*>("worldRegionLeft"); QVERIFY(draftLeft);
    QCOMPARE(draftLeft->value(), double(expected[0]));
    Mouse(*viewport, QEvent::MouseButtonRelease, screen(end), Qt::LeftButton, Qt::NoButton);
    QCOMPARE(finished.count(), 1);
    QCOMPARE(viewport->SaveCamera(), beforeCamera);
    const auto after = project.mapEdits_->Snapshot().Regions()->Serialize(); QVERIFY(after.Succeeded());
    const auto currentCamera = project.mapEdits_->Snapshot().Cameras()->Serialize(); QVERIFY(currentCamera.Succeeded());
    QVERIFY(*currentCamera.value == *cameraOriginal.value);
    const auto saved = project.mapEdits_->Snapshot().Regions()->entries.front();
    const QList<float> actual{saved.left, saved.bottom, saved.right, saved.top};
    QCOMPARE(actual, expected);
    for (int i = 0; i < 4; ++i) QCOMPARE(table->item(0, i + 1)->text().toFloat(), expected[i]);
    QVERIFY(saved.right - saved.left >= 32 && saved.top - saved.bottom >= 32);
    const bool changed = *after.value != *original.value;
    QCOMPARE(project.mapDataUndoStack_->count(), changed ? 1 : 0);
    if (!changed) {
        // 零变更释放不能残留草稿，从而阻止下一次正常属性提交。
        draftLeft->setFocus(); draftLeft->setValue(32); QTest::keyClick(draftLeft, Qt::Key_Return);
        QCOMPARE(project.mapDataUndoStack_->count(), 1);
        QCOMPARE(project.mapEdits_->Snapshot().Regions()->entries.front().left, 32.0f);
        return;
    }
    project.mapDataUndoStack_->undo();
    const auto undone = project.mapEdits_->Snapshot().Regions()->Serialize(); QVERIFY(undone.Succeeded());
    QVERIFY(*undone.value == *original.value);
    QCOMPARE(table->item(0, 3)->text().toFloat(), right);
    project.mapDataUndoStack_->redo();
    const auto redone = project.mapEdits_->Snapshot().Regions()->Serialize(); QVERIFY(redone.Succeeded());
    QVERIFY(*redone.value == *after.value);
    project.mapDataUndoStack_->undo();
    constexpr std::array<std::string_view, 4> fields{"left", "bottom", "right", "top"};
    const QList<float> before{region.left, region.bottom, region.right, region.top};
    for (int i = 0; i < 4; ++i) if (before[i] != expected[i])
        QVERIFY(tableModule.EditCell(MapDataKind::Regions, 0, fields[i], QString::number(expected[i]).toStdString()).Succeeded());
    const auto fromTable = project.mapEdits_->Snapshot().Regions()->Serialize(); QVERIFY(fromTable.Succeeded());
    QVERIFY(*fromTable.value == *after.value); // 两入口最终原生成员字节完全相同
}

void WorldModuleTests::AuxiliaryDragCancellationAndIdentity() {
    QTemporaryDir directory; QVERIFY(directory.isValid());
    const MapData::RegionRecord region{0, 0, 128, 128, "EdgeRegion", 17, {}, "", {0, 0, 0, 255}};
    const MapData::CameraRecord camera{256, 256, 0, 0, 0, 1650, 0, 70, 5000, 100, "CameraOne"};
    QtProject::EditorProject project;
    QVERIFY(InstallAuxiliaryEditFixture(project, directory.path(), region, camera));
    BackendState state; activeBackend = &state;
    state.viewProjection[0] = state.viewProjection[5] = 1.0f / 192;
    state.viewProjection[12] = state.viewProjection[13] = -64.0f / 192;
    WorldModule module(project, &modelBackend); MapEntitiesModule tableModule(project);
    (void)tableModule.Widget();
    auto* page = module.Widget(); auto* viewport = module.Viewport(); state.viewport = viewport;
    page->resize(1200, 760); page->show();
    auto* model = dynamic_cast<WorldOutlinerModel*>(page->findChild<QTreeView*>("worldOutliner")->model()); QVERIFY(model);
    const Services::WorldObjectId regionId = MapData::MakeRegionId(0, region), cameraId = MapData::MakeCameraId(0, camera);
    QTRY_VERIFY(FindWorldObject(*model, regionId).isValid());
    QVERIFY(SelectAuxiliaryRow(*page, regionId));
    QTRY_VERIFY(viewport->VertexCount() > 0 && state.frames > 0);
    const auto before = project.mapEdits_->Revision();
    const auto original = project.mapEdits_->Snapshot().Regions()->Serialize(); QVERIFY(original.Succeeded());
    // 外角方形内、圆形手柄外的点没有可拖动边，不能退化成整区域移动。
    const auto screen = [&](QPointF world) {
        return AuxiliaryScreenPoint(*viewport, state, world);
    };
    QSignalSpy started(viewport, &WorldViewport::MoveStarted);
    const auto cornerPoint = MoveAuxiliaryPointer(*viewport, state, {-24, -24});
    QTRY_VERIFY(viewport->HoverCoordinate().has_value());
    QVERIFY(std::abs(viewport->HoverCoordinate()->position[0] + 24) < 0.01);
    QVERIFY2(std::abs(viewport->HoverCoordinate()->position[1] + 24) < 0.01,
        qPrintable(QStringLiteral("actual y=%1; viewport=%2x%3; completed=%4x%5; dpr=%6")
            .arg(viewport->HoverCoordinate()->position[1], 0, 'g', 12).arg(viewport->width()).arg(viewport->height())
            .arg(state.completed.width).arg(state.completed.height).arg(viewport->devicePixelRatioF(), 0, 'g', 12)));
    Mouse(*viewport, QEvent::MouseButtonPress, cornerPoint, Qt::LeftButton, Qt::LeftButton);
    Mouse(*viewport, QEvent::MouseMove, screen({21, 21}), Qt::NoButton, Qt::LeftButton);
    QTest::keyClick(viewport, Qt::Key_Escape);
    Mouse(*viewport, QEvent::MouseButtonRelease, screen({21, 21}), Qt::LeftButton, Qt::NoButton);
    QCOMPARE(started.count(), 0);
    QCOMPARE(project.mapEdits_->Revision(), before); QCOMPARE(project.mapDataUndoStack_->count(), 0);
    QVERIFY(SelectAuxiliaryRow(*page, regionId));
    viewport->MoveStarted(0, 64); viewport->MoveDragged(64, 64); viewport->MoveCancelled(); viewport->MoveFinished();
    QCOMPARE(project.mapEdits_->Revision(), before); QCOMPARE(project.mapDataUndoStack_->count(), 0);
    // 选择身份在草稿期间变化，不允许把旧区域草稿提交给新选择。
    viewport->MoveStarted(0, 64); viewport->MoveDragged(64, 64);
    QVERIFY(SelectAuxiliaryRow(*page, cameraId)); viewport->MoveFinished();
    QCOMPARE(project.mapEdits_->Revision(), before); QCOMPARE(project.mapDataUndoStack_->count(), 0);
    QVERIFY(SelectAuxiliaryRow(*page, regionId));
    QVERIFY(SelectAuxiliaryRow(*page, cameraId, Qt::ShiftModifier));
    QVERIFY(page->findChild<QTreeView*>("worldOutliner")->selectionModel()->selectedRows().size() >= 2);
    viewport->MoveStarted(0, 64); viewport->MoveDragged(64, 64); viewport->MoveFinished();
    QCOMPARE(project.mapEdits_->Revision(), before); QCOMPARE(project.mapDataUndoStack_->count(), 0);
    // 地图身份变化时拒绝旧草稿，即便文档指针尚未替换。
    QVERIFY(SelectAuxiliaryRow(*page, regionId));
    viewport->MoveStarted(0, 64); viewport->MoveDragged(64, 64);
    const auto mapRoot = project.projectRoot_; project.projectRoot_ /= L"changed";
    viewport->MoveFinished(); project.projectRoot_ = mapRoot;
    QCOMPARE(project.mapEdits_->Revision(), before); QCOMPARE(project.mapDataUndoStack_->count(), 0);
    // 表格在拖动期间已发布新修订，释放不能覆盖它，也不能追加第二项Undo。
    QVERIFY(SelectAuxiliaryRow(*page, regionId));
    viewport->MoveStarted(0, 64); viewport->MoveDragged(64, 64);
    QVERIFY(tableModule.EditCell(MapDataKind::Regions, 0, "right", "160").Succeeded());
    const auto newerRevision = project.mapEdits_->Revision();
    viewport->MoveFinished();
    QCOMPARE(project.mapEdits_->Revision(), newerRevision); QCOMPARE(project.mapDataUndoStack_->count(), 1);
    QCOMPARE(project.mapEdits_->Snapshot().Regions()->entries.front().left, 0.0f);
    QCOMPARE(project.mapEdits_->Snapshot().Regions()->entries.front().right, 160.0f);
    project.mapDataUndoStack_->undo();
    const auto restored = project.mapEdits_->Snapshot().Regions()->Serialize(); QVERIFY(restored.Succeeded());
    QVERIFY(*restored.value == *original.value);
}

void WorldModuleTests::CameraDragUsesSharedTableTransaction() {
    QTemporaryDir directory; QVERIFY(directory.isValid());
    const MapData::RegionRecord region{0, 0, 128, 128, "EdgeRegion", 17, {}, "", {0, 0, 0, 255}};
    const MapData::CameraRecord camera{64, 64, 7, 0, 0, 1650, 0, 70, 5000, 100, "CameraOne"};
    QtProject::EditorProject project;
    QVERIFY(InstallAuxiliaryEditFixture(project, directory.path(), region, camera));
    BackendState state; activeBackend = &state;
    state.viewProjection[0] = state.viewProjection[5] = 1.0f / 192;
    state.viewProjection[12] = state.viewProjection[13] = -64.0f / 192;
    WorldModule module(project, &modelBackend); MapEntitiesModule tableModule(project);
    auto* table = tableModule.Widget()->findChild<QTableWidget*>("mapEntitiesTable3"); QVERIFY(table);
    auto* page = module.Widget(); auto* viewport = module.Viewport(); state.viewport = viewport;
    page->resize(1200, 760); page->show();
    auto* model = dynamic_cast<WorldOutlinerModel*>(page->findChild<QTreeView*>("worldOutliner")->model()); QVERIFY(model);
    const Services::WorldObjectId id = MapData::MakeCameraId(0, camera);
    QTRY_VERIFY(FindWorldObject(*model, id).isValid()); QVERIFY(SelectAuxiliaryRow(*page, id));
    QTRY_VERIFY(viewport->VertexCount() > 0 && state.frames > 0);
    const auto start = MoveAuxiliaryPointer(*viewport, state, {64, 64});
    const auto end = AuxiliaryScreenPoint(*viewport, state, {96, 80});
    QTRY_VERIFY(viewport->HoverCoordinate().has_value());
    const auto original = project.mapEdits_->Snapshot().Cameras()->Serialize(); QVERIFY(original.Succeeded());
    const auto originalRegion = project.mapEdits_->Snapshot().Regions()->Serialize(); QVERIFY(originalRegion.Succeeded());
    const auto viewCamera = viewport->SaveCamera();
    QSignalSpy started(viewport, &WorldViewport::MoveStarted);
    QSignalSpy dragged(viewport, &WorldViewport::MoveDragged);
    Mouse(*viewport, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
    Mouse(*viewport, QEvent::MouseMove, end, Qt::NoButton, Qt::LeftButton);
    QCOMPARE(started.count(), 1); QCOMPARE(project.mapDataUndoStack_->count(), 0);
    QVERIFY(std::abs(started.front().at(0).toDouble() - 64) < 0.001);
    QVERIFY2(std::abs(started.front().at(1).toDouble() - 64) < 0.001,
        qPrintable(QStringLiteral("actual start y=%1; viewport=%2x%3; completed=%4x%5; dpr=%6; mouse=(%7,%8)")
            .arg(started.front().at(1).toDouble(), 0, 'g', 12).arg(viewport->width()).arg(viewport->height())
            .arg(state.completed.width).arg(state.completed.height).arg(viewport->devicePixelRatioF(), 0, 'g', 12)
            .arg(start.x(), 0, 'g', 12).arg(start.y(), 0, 'g', 12)));
    QVERIFY(!dragged.empty());
    QVERIFY(std::abs(dragged.back().at(0).toDouble() - 96) < 0.001);
    QVERIFY(std::abs(dragged.back().at(1).toDouble() - 80) < 0.001);
    QTest::keyClick(viewport, Qt::Key_Escape);
    Mouse(*viewport, QEvent::MouseButtonRelease, end, Qt::LeftButton, Qt::NoButton);
    QCOMPARE(project.mapDataUndoStack_->count(), 0);
    Mouse(*viewport, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
    Mouse(*viewport, QEvent::MouseMove, end, Qt::NoButton, Qt::LeftButton);
    Mouse(*viewport, QEvent::MouseButtonRelease, end, Qt::LeftButton, Qt::NoButton);
    QCOMPARE(project.mapDataUndoStack_->count(), 1);
    QCOMPARE(viewport->SaveCamera(), viewCamera);
    const auto saved = project.mapEdits_->Snapshot().Cameras()->entries.front();
    QVERIFY(std::abs(saved.targetX - 96) < 0.001f && std::abs(saved.targetY - 80) < 0.001f);
    QCOMPARE(table->item(0, 1)->text().toFloat(), saved.targetX);
    QCOMPARE(table->item(0, 2)->text().toFloat(), saved.targetY);
    const auto after = project.mapEdits_->Snapshot().Cameras()->Serialize(); QVERIFY(after.Succeeded());
    const auto unchangedRegion = project.mapEdits_->Snapshot().Regions()->Serialize(); QVERIFY(unchangedRegion.Succeeded());
    QVERIFY(*unchangedRegion.value == *originalRegion.value);
    project.mapDataUndoStack_->undo();
    const auto undone = project.mapEdits_->Snapshot().Cameras()->Serialize(); QVERIFY(undone.Succeeded());
    QVERIFY(*undone.value == *original.value);
    QCOMPARE(table->item(0, 1)->text().toFloat(), camera.targetX);
    project.mapDataUndoStack_->redo();
    const auto redone = project.mapEdits_->Snapshot().Cameras()->Serialize(); QVERIFY(redone.Succeeded());
    QVERIFY(*redone.value == *after.value);
    project.mapDataUndoStack_->undo();
    QVERIFY(tableModule.EditCell(MapDataKind::Cameras, 0, "targetX", QString::number(saved.targetX, 'g', 9).toStdString()).Succeeded());
    QVERIFY(tableModule.EditCell(MapDataKind::Cameras, 0, "targetY", QString::number(saved.targetY, 'g', 9).toStdString()).Succeeded());
    const auto fromTable = project.mapEdits_->Snapshot().Cameras()->Serialize(); QVERIFY(fromTable.Succeeded());
    QVERIFY(*fromTable.value == *after.value);
}

// 显式运行（需本机游戏根以识别英雄）：选中英雄可改等级并撤销；混选非英雄时不可改。
void WorldModuleTests::HeroLevelEditsOnlyHeroes() {
    const auto gamePath = qEnvironmentVariable("FORGE_WORLD_GAME_ROOT");
    if (gamePath.isEmpty()) QSKIP("需要 FORGE_WORLD_GAME_ROOT 指向本机游戏根目录");
    QTemporaryDir directory; QVERIFY(directory.isValid());
    const auto terrain = TerrainBytes(); QVERIFY(Write(directory.filePath("war3map.w3e"), terrain));
    auto units = MapData::ParseUnitPlacements(MapDataFixtures::EmptyUnitsV8Sub11()); QVERIFY(units.Succeeded());
    MapData::UnitPlacement hero; hero.rawcode = "Hpal"; hero.flags = 2; hero.scale = {1, 1, 1};
    hero.creationId = 7; hero.position = {-32, -16, 0}; hero.heroLevel = 1; hero.hitPoints = -1; hero.mana = -1;
    auto footman = hero; footman.rawcode = "hfoo"; footman.creationId = 8; footman.position = {32, 16, 0};
    units.value->units = {hero, footman}; const auto unitBytes = units.value->Serialize(); QVERIFY(unitBytes.Succeeded());
    const std::array<MapArchiveMember, 1> members{{{"war3mapUnits.doo", *unitBytes.value}}};
    auto game = Game::GameDataSource::Mount(gamePath.toStdWString()); QVERIFY(game.Succeeded());
    auto catalog = ObjectData::StandardObjectCatalog::Load(*game.value); QVERIFY(catalog.Succeeded());
    QtProject::EditorProject project; project.projectRoot_ = directory.path().toStdWString();
    project.war3GameRoot_ = gamePath.toStdWString();
    project.standardCatalog_ = std::make_unique<ObjectData::StandardObjectCatalog>(std::move(*catalog.value));
    auto workspace = DirectoryWorkspace::Open(project.projectRoot_); QVERIFY(workspace.Succeeded());
    project.directorySnapshot_ = std::make_unique<DirectoryWorkspace>(std::move(*workspace.value));
    project.mapDocument_ = std::make_unique<MapDocument>(MapDocument::FromMembers(members));
    project.mapEdits_ = std::make_unique<MapData::MapDataEditSession>(*project.mapDocument_);
    QVERIFY(project.mapEdits_->AttachTerrain(std::span<const std::byte>(reinterpret_cast<const std::byte*>(terrain.constData()),
        static_cast<std::size_t>(terrain.size()))).Succeeded()); project.ResetEditTracking();
    BackendState state; activeBackend = &state; WorldModule module(project, &modelBackend);
    auto* page = module.Widget(); state.viewport = module.Viewport(); page->resize(1200, 760); page->show();
    auto* outliner = page->findChild<QTreeView*>("worldOutliner"); QVERIFY(outliner);
    auto* model = dynamic_cast<WorldOutlinerModel*>(outliner->model()); QVERIFY(model);
    const Services::WorldObjectId heroId = MapData::MakePlacementId(MapData::PlacementFile::Units, 0, hero);
    const Services::WorldObjectId footmanId = MapData::MakePlacementId(MapData::PlacementFile::Units, 1, footman);
    QTRY_VERIFY(FindWorldObject(*model, heroId).isValid() && FindWorldObject(*model, footmanId).isValid());
    QTRY_VERIFY(state.frames > 0); outliner->expandAll();
    QTest::mouseClick(outliner->viewport(), Qt::LeftButton, {}, outliner->visualRect(model->Find(heroId)).center());
    auto* level = page->findChild<QLineEdit*>("worldSelectionLevel"); QVERIFY(level);
    QTRY_VERIFY(level->isVisible() && level->isEnabled());
    QCOMPARE(level->text(), QStringLiteral("1"));
    level->setText(QStringLiteral("0")); level->setProperty("worldEdited", true); emit level->editingFinished();
    QCOMPARE(project.mapEdits_->Snapshot().Units()->units[0].heroLevel, 1);
    QTRY_COMPARE(level->text(), QStringLiteral("1"));
    level->setText(QStringLiteral("5")); level->setProperty("worldEdited", true); emit level->editingFinished();
    QCOMPARE(project.mapEdits_->Snapshot().Units()->units[0].heroLevel, 5);
    QCOMPARE(project.mapEdits_->Snapshot().Units()->units[1].heroLevel, 1);
    QTRY_COMPARE(level->text(), QStringLiteral("5"));
    project.mapDataUndoStack_->undo();
    QCOMPARE(project.mapEdits_->Snapshot().Units()->units[0].heroLevel, 1);
    QTest::mouseClick(outliner->viewport(), Qt::LeftButton, Qt::ShiftModifier, outliner->visualRect(model->Find(footmanId)).center());
    QTRY_COMPARE(outliner->selectionModel()->selectedRows().size(), 2);
    QTRY_VERIFY(!level->isEnabled());
}

void WorldModuleTests::TypedMultiSelectionWithoutModelsUsesOneUndo() {
    QTemporaryDir directory; QVERIFY(directory.isValid());
    const auto terrain = TerrainBytes(); QVERIFY(Write(directory.filePath("war3map.w3e"), terrain));
    auto units = MapData::ParseUnitPlacements(MapDataFixtures::EmptyUnitsV8Sub11()); QVERIFY(units.Succeeded());
    MapData::UnitPlacement first; first.rawcode = "hfoo"; first.flags = 2; first.scale = {1, 1, 1};
    first.creationId = 7; first.position = {-32, -16, 0};
    auto second = first; second.creationId = 8; second.position = {32, 16, 0}; second.rotation = 0.5f;
    units.value->units = {first, second}; const auto unitBytes = units.value->Serialize(); QVERIFY(unitBytes.Succeeded());
    const QByteArray sourceBytes(reinterpret_cast<const char*>(unitBytes.value->data()), static_cast<qsizetype>(unitBytes.value->size()));
    const auto sourcePath = directory.filePath("war3mapUnits.doo"); QVERIFY(Write(sourcePath, sourceBytes));
    const std::array<MapArchiveMember, 1> members{{{"war3mapUnits.doo", *unitBytes.value}}};
    QtProject::EditorProject project; project.projectRoot_ = directory.path().toStdWString();
    auto workspace = DirectoryWorkspace::Open(project.projectRoot_); QVERIFY(workspace.Succeeded());
    project.directorySnapshot_ = std::make_unique<DirectoryWorkspace>(std::move(*workspace.value));
    project.mapDocument_ = std::make_unique<MapDocument>(MapDocument::FromMembers(members));
    project.mapEdits_ = std::make_unique<MapData::MapDataEditSession>(*project.mapDocument_);
    QVERIFY(project.mapEdits_->AttachTerrain(std::span<const std::byte>(reinterpret_cast<const std::byte*>(terrain.constData()),
        static_cast<std::size_t>(terrain.size()))).Succeeded()); project.ResetEditTracking();
    BackendState state; activeBackend = &state; WorldModule module(project, &modelBackend);
    auto* page = module.Widget(); state.viewport = module.Viewport(); page->resize(1200, 760); page->show();
    auto* outliner = page->findChild<QTreeView*>("worldOutliner"); QVERIFY(outliner);
    auto* model = dynamic_cast<WorldOutlinerModel*>(outliner->model()); QVERIFY(model);
    const Services::WorldObjectId firstId = MapData::MakePlacementId(MapData::PlacementFile::Units, 0, first);
    const Services::WorldObjectId secondId = MapData::MakePlacementId(MapData::PlacementFile::Units, 1, second);
    QTRY_VERIFY(FindWorldObject(*model, firstId).isValid() && FindWorldObject(*model, secondId).isValid());
    QTRY_VERIFY(state.frames > 0); outliner->expandAll();
    QTest::mouseClick(outliner->viewport(), Qt::LeftButton, {}, outliner->visualRect(model->Find(firstId)).center());
    auto* x = page->findChild<QDoubleSpinBox*>("worldFieldX"); QVERIFY(x);
    QTRY_VERIFY(x->isEnabled()); QVERIFY(x->isVisible()); QCOMPARE(x->value(), -32.0);
    QVERIFY(!module.Viewport()->SelectedInstance()); // 没有游戏目录/模型路径，typed 属性仍可选择编辑。
    auto* note = page->findChild<QLabel*>("worldSelectionNote"); QVERIFY(note && note->text().contains("模型"));
    QTest::mouseClick(outliner->viewport(), Qt::LeftButton, Qt::ShiftModifier, outliner->visualRect(model->Find(secondId)).center());
    QTRY_COMPARE(outliner->selectionModel()->selectedRows().size(), 2);
    auto* input = x->findChild<QLineEdit*>(); QVERIFY(input);
    QTRY_VERIFY(input->placeholderText().contains("多个值"));
    const auto output = qEnvironmentVariable("FORGE_R08_WORLD_SCREENSHOT_DIR");
    if (!output.isEmpty()) {
        QVERIFY(QDir().mkpath(output));
        for (const auto size : {QSize(1920, 1080), QSize(2560, 1440)}) {
            const qreal ratio = page->devicePixelRatioF(); page->resize(qRound(size.width() / ratio), qRound(size.height() / ratio));
            QApplication::processEvents(); const auto image = page->grab().toImage(); QCOMPARE(image.size(), size);
            QVERIFY(image.save(QDir(output).filePath(QStringLiteral("world-stub-layout-%1x%2.png").arg(size.width()).arg(size.height()))));
        } // 仅真实 Qt 布局截图，桩后端没有 D3D 游戏画面。
    }
    x->setFocus(); QTest::keyClick(x, Qt::Key_A, Qt::ControlModifier); QTest::keyClicks(x, "64");
    QCOMPARE(input->text(), QStringLiteral("64"));
    QTest::keyClick(x, Qt::Key_Return); QTRY_COMPARE(project.mapDataUndoStack_->count(), 1);
    QCOMPARE(project.mapEdits_->Snapshot().Units()->units[0].position[0], 64.0f);
    QCOMPARE(project.mapEdits_->Snapshot().Units()->units[1].position[0], 64.0f);
    QCOMPARE(project.mapEdits_->Snapshot().Units()->units[0].position[1], first.position[1]);
    QCOMPARE(project.mapEdits_->Snapshot().Units()->units[1].position[1], second.position[1]);
    project.mapDataUndoStack_->undo(); QCOMPARE(project.mapDataUndoStack_->index(), 0); QVERIFY(!project.mapEdits_->IsDirty());
    QCOMPARE(project.mapEdits_->Snapshot().Units()->units[0].position[0], first.position[0]);
    QCOMPARE(project.mapEdits_->Snapshot().Units()->units[1].position[0], second.position[0]);
    project.mapDataUndoStack_->redo(); QCOMPARE(project.mapDataUndoStack_->index(), 1);
    QCOMPARE(project.mapEdits_->Snapshot().Units()->units[0].position[0], 64.0f);
    QCOMPARE(project.mapEdits_->Snapshot().Units()->units[1].position[0], 64.0f);
    const auto revision = project.mapEdits_->Revision(); const auto history = project.mapDataUndoStack_->count();
    QVERIFY(FindWorldObject(*model, firstId).isValid() && FindWorldObject(*model, secondId).isValid());
    QVERIFY(model->setData(model->Find(firstId).siblingAtColumn(1), Qt::Unchecked, Qt::CheckStateRole));
    QTRY_VERIFY(model->RowAt(FindWorldObject(*model, firstId)) && !model->RowAt(model->Find(firstId))->flags.visible);
    QTRY_VERIFY(!(model->flags(model->Find(firstId)) & Qt::ItemIsSelectable));
    QVERIFY(model->setData(model->Find(secondId).siblingAtColumn(2), Qt::Checked, Qt::CheckStateRole));
    QTRY_VERIFY(model->RowAt(FindWorldObject(*model, secondId)) && model->RowAt(model->Find(secondId))->flags.locked);
    QTRY_VERIFY(outliner->selectionModel()->selectedRows().isEmpty());
    QCOMPARE(project.mapEdits_->Revision(), revision); QCOMPARE(project.mapDataUndoStack_->count(), history);
    QCOMPARE(project.mapDataUndoStack_->index(), 1); QVERIFY(project.mapEdits_->IsDirty());
    QFile source(sourcePath); QVERIFY(source.open(QIODevice::ReadOnly)); QCOMPARE(source.readAll(), sourceBytes);
}

void WorldModuleTests::NavigationTypedBookmarkCodecKeepsAllIdentityBytes() {
    MapData::RegionRecord region{}; region.name = std::string("same\xff", 5); region.creationNumber = -9;
    MapData::CameraRecord camera{}; camera.name = region.name;
    MapData::UnitPlacement unit; unit.rawcode = std::string("h\xff\0o", 4); unit.creationId = -5;
    for (const Services::WorldObjectId& id : std::array<Services::WorldObjectId, 3>{
        MapData::MakePlacementId(MapData::PlacementFile::Units, 4, unit),
        MapData::MakeRegionId(7, region), MapData::MakeCameraId(8, camera)}) {
        const auto encoded = WorldNavigation::Encode(id);
        const auto decoded = WorldNavigation::Decode(encoded);
        QVERIFY(decoded && *decoded == id);
        auto malformed = encoded; malformed.insert("index", 0.5); QVERIFY(!WorldNavigation::Decode(malformed));
        malformed = encoded; malformed.insert("index", QStringLiteral("7")); QVERIFY(!WorldNavigation::Decode(malformed));
        malformed = encoded; malformed.insert("index", 4294967296.0); QVERIFY(!WorldNavigation::Decode(malformed));
        malformed = encoded; malformed.remove("kind"); QVERIFY(!WorldNavigation::Decode(malformed));
        malformed = encoded; malformed.insert(encoded.contains("rawcodeBytes") ? "rawcodeBytes" : "nameBytes", "zz");
        QVERIFY(!WorldNavigation::Decode(malformed));
    }
    auto record = WorldNavigation::Encode(MapData::MakeRegionId(0, region));
    record.remove("creation"); QVERIFY(!WorldNavigation::Decode(record));
    record = WorldNavigation::Encode(MapData::MakePlacementId(MapData::PlacementFile::Units, 0, unit));
    record.insert("file", 2); QVERIFY(!WorldNavigation::Decode(record));
    record.insert("file", 0); record.insert("rawcodeBytes", "00"); QVERIFY(!WorldNavigation::Decode(record));
    QJsonObject oversized{{"padding", QString(512 * 1024, QLatin1Char('x'))}};
    QVERIFY(!WorldNavigation::Bounded(oversized));
}

void WorldModuleTests::NavigationBookmarkRestoresPagedOutlinePrimaryAndCamera() {
    QTemporaryDir directory; QVERIFY(directory.isValid());
    MapData::RegionRecord first{0, 0, 128, 128, "Region000", 100, {}, "", {0, 0, 0, 255}};
    const MapData::CameraRecord camera{256, 256, 0, 0, 0, 1650, 0, 70, 5000, 100, "CameraOne"};
    QtProject::EditorProject project; QVERIFY(InstallAuxiliaryEditFixture(project, directory.path(), first, camera));
    auto regions = *project.mapEdits_->Snapshot().Regions();
    for (int index = 1; index < 160; ++index) {
        auto region = first; region.name = QStringLiteral("Region%1").arg(index, 3, 10, QLatin1Char('0')).toStdString();
        region.creationNumber = 100 + index; regions.entries.push_back(std::move(region));
    }
    QVERIFY(project.mapEdits_->EditRegions(regions).Succeeded());
    const auto revision = project.mapEdits_->Revision();
    const auto regionBytes = project.mapEdits_->Snapshot().Regions()->Serialize(); QVERIFY(regionBytes.Succeeded());
    BackendState state; activeBackend = &state;
    WorldModule module(project, &modelBackend); auto* page = module.Widget(); auto* viewport = module.Viewport(); state.viewport = viewport;
    page->resize(1200, 760); page->show();
    auto* tree = page->findChild<QTreeView*>("worldOutliner");
    auto* model = tree ? dynamic_cast<WorldOutlinerModel*>(tree->model()) : nullptr;
    auto* search = page->findChild<QLineEdit*>("worldOutlinerFilter");
    auto* category = page->findChild<QComboBox*>("worldOutlinerCategory");
    QVERIFY(tree && model && search && category);
    QTRY_VERIFY(viewport->VertexCount() > 0 && state.frames > 0);
    const Services::WorldObjectId regionId = MapData::MakeRegionId(128, regions.entries[128]);
    const Services::WorldObjectId cameraId = MapData::MakeCameraId(0, camera);
    QTRY_VERIFY(model->Version().has_value());
    for (int count = 0; !model->Find(regionId).isValid() && model->canFetchMore({}) && count < 16; ++count) model->fetchMore({});
    QVERIFY(model->Find(regionId).isValid()); QVERIFY(SelectAuxiliaryRow(*page, regionId));
    auto cameraPose = viewport->SaveCamera(); cameraPose.insert("yaw", 0.75); cameraPose.insert("distance", 1700.0);
    QVERIFY(viewport->RestoreCamera(cameraPose));
    tree->expandAll(); tree->verticalScrollBar()->setValue(tree->verticalScrollBar()->maximum() / 2);
    const auto saved = module.CaptureNavigationBookmark(); QVERIFY(saved);
    QVERIFY(saved->value("loadedRows").toInt() > 64);
    auto multi = *saved; multi.insert("selection", QJsonArray{WorldNavigation::Encode(regionId), WorldNavigation::Encode(cameraId)});
    multi.insert("primary", WorldNavigation::Encode(regionId)); // primary 不必等于集合最后一项。
    search->setText("Region000"); category->setCurrentIndex(category->findData(static_cast<int>(Services::WorldCategory::Region)));
    QTRY_VERIFY(model->Find(MapData::MakeRegionId(0, first)).isValid());
    auto otherCamera = viewport->SaveCamera(); otherCamera.insert("yaw", 1.25); QVERIFY(viewport->RestoreCamera(otherCamera));
    QCOMPARE(module.RestoreNavigationBookmark(multi), Shell::NavigationRestoreResult::Restored);
    const auto restored = module.CaptureNavigationBookmark(); QVERIFY(restored);
    QCOMPARE(restored->value("selection"), multi.value("selection")); QCOMPARE(restored->value("primary"), multi.value("primary"));
    QCOMPARE(restored->value("camera"), saved->value("camera")); QCOMPARE(search->text(), saved->value("search").toString());
    QCOMPARE(category->currentData().toInt(), saved->value("category").toInt());
    QVERIFY(restored->value("loadedRows").toInt() >= saved->value("loadedRows").toInt());
    QCOMPARE(restored->value("vertical").toInt(), saved->value("vertical").toInt());
    QCOMPARE(restored->value("expanded"), saved->value("expanded"));
    QCOMPARE(project.mapEdits_->Revision(), revision); QCOMPARE(project.mapDataUndoStack_->count(), 0);
    const auto after = project.mapEdits_->Snapshot().Regions()->Serialize(); QVERIFY(after.Succeeded()); QVERIFY(*after.value == *regionBytes.value);
    for (const auto* field : {"creation", "index"}) {
        auto invalid = multi; auto selected = invalid.value("selection").toArray(); auto id = selected[0].toObject();
        id.insert(QLatin1String(field), field == std::string_view("creation") ? 999 : 0); selected[0] = id; invalid.insert("selection", selected);
        const auto before = module.CaptureNavigationBookmark(); QVERIFY(before);
        QCOMPARE(module.RestoreNavigationBookmark(invalid), Shell::NavigationRestoreResult::Invalid);
        QVERIFY(module.CaptureNavigationBookmark() == before);
    }
    auto invalid = multi; auto invalidCamera = saved->value("camera").toObject(); invalidCamera.insert("distance", 0.0); invalid.insert("camera", invalidCamera);
    const auto beforeCamera = module.CaptureNavigationBookmark();
    QCOMPARE(module.RestoreNavigationBookmark(invalid), Shell::NavigationRestoreResult::Invalid);
    QVERIFY(module.CaptureNavigationBookmark() == beforeCamera);
    QCOMPARE(module.RestoreNavigationBookmark(*saved), Shell::NavigationRestoreResult::Restored);
    viewport->MoveStarted(0, 64); viewport->MoveDragged(32, 64);
    QVERIFY(!module.PrepareNavigationLeave());
    const auto duringDraft = module.CaptureNavigationBookmark();
    QCOMPARE(module.RestoreNavigationBookmark(multi), Shell::NavigationRestoreResult::Blocked);
    QVERIFY(module.CaptureNavigationBookmark() == duringDraft);
    viewport->MoveCancelled(); viewport->MoveFinished();
    QVERIFY(module.PrepareNavigationLeave()); QCOMPARE(project.mapEdits_->Revision(), revision);
    regions.entries[128].creationNumber = 999; QVERIFY(project.mapEdits_->EditRegions(regions).Succeeded());
    QCOMPARE(module.RestoreNavigationBookmark(*saved), Shell::NavigationRestoreResult::Invalid);
}

void WorldModuleTests::ShadowRelationMemoRejectsForeignSourcesAndReusesRevision() {
    auto grid = FlatGrid(5, 5); grid->header.mainTileset = 'A';
    grid->header.groundTilesets = {"Ldrt"}; grid->header.cliffTilesets = {"Cldi"};
    const auto bytes = MapData::SerializeMapTerrain(*grid); QVERIFY(bytes.Succeeded());
    MapData::MapDataEditSession edits(MapDocument::FromMembers({}));
    WorldShadowRelationMemo memo;
    int comparisons = 0;
    const auto compare = [&](const auto& current) {
        ++comparisons;
        return MapData::TerrainSavePlan::HasTerrainChanges(current);
    };
    QVERIFY(!memo.Query(edits, nullptr, compare));
    QVERIFY(!memo.Query(edits, grid.get(), compare));
    QCOMPARE(comparisons, 0);
    const auto sourceRevision = edits.TerrainSourceRevision();
    QVERIFY(edits.AttachTerrain(*bytes.value).Succeeded());
    QCOMPARE(edits.Revision(), std::uint64_t{0});
    QVERIFY(edits.TerrainSourceRevision() > sourceRevision);
    const auto source = edits.Terrain(); QVERIFY(source && source.get() != grid.get());
    QVERIFY(memo.Query(edits, source.get(), compare) == std::optional<bool>{false});
    QCOMPARE(comparisons, 1);
    // 同修订水帧和预览只取已有关系；字段相同的外来快照也不能借memo通行。
    for (int repeated = 0; repeated < 3; ++repeated)
        QVERIFY(memo.Query(edits, source.get(), compare) == std::optional<bool>{false});
    QVERIFY(!memo.Query(edits, grid.get(), compare));
    QVERIFY(!memo.Query(edits, nullptr, compare));
    QVERIFY(memo.Query(edits, source.get(), compare) == std::optional<bool>{false});
    QCOMPARE(comparisons, 1);
    QVERIFY(!edits.IsDirty());
}

void WorldModuleTests::ShadowRelationMemoTracksHeightUndoRedoAndSavedBaseline() {
    auto grid = FlatGrid(5, 5); grid->header.mainTileset = 'A';
    grid->header.groundTilesets = {"Ldrt"}; grid->header.cliffTilesets = {"Cldi"};
    const auto bytes = MapData::SerializeMapTerrain(*grid); QVERIFY(bytes.Succeeded());
    MapData::MapDataEditSession edits(MapDocument::FromMembers({}));
    QVERIFY(edits.AttachTerrain(*bytes.value).Succeeded());
    WorldShadowRelationMemo memo;
    int comparisons = 0;
    const auto compare = [&](const auto& current) {
        ++comparisons;
        return MapData::TerrainSavePlan::HasTerrainChanges(current);
    };
    const auto original = edits.Terrain();
    QVERIFY(memo.Query(edits, original.get(), compare) == std::optional<bool>{false});
    QCOMPARE(comparisons, 1);
    MapData::TerrainBrush brush; brush.kind = MapData::TerrainBrushKind::Raise; brush.radius = 0; brush.strength = 1;
    const MapData::TerrainStrokePoint point{128, 192};
    QVERIFY(edits.EditTerrain(brush, std::span(&point, 1), false).Succeeded());
    QVERIFY(edits.Terrain().get() != original.get());
    // 新修订的旧请求先拒绝，不能用它刷新key或覆盖上一份已核关系。
    QVERIFY(!memo.Query(edits, original.get(), compare)); QCOMPARE(comparisons, 1);
    QVERIFY(memo.Query(edits, edits.Terrain().get(), compare) == std::optional<bool>{true});
    QVERIFY(memo.Query(edits, edits.Terrain().get(), compare) == std::optional<bool>{true});
    QCOMPARE(comparisons, 2);
    const auto raisedRevision = edits.Revision();
    QVERIFY(edits.Undo()); QVERIFY(edits.Revision() > raisedRevision);
    const auto undone = MapData::SerializeMapTerrain(*edits.Terrain()); QVERIFY(undone.Succeeded());
    QCOMPARE(*undone.value, *bytes.value); // Undo是实际净同，不以Revision或指针相同代替比较。
    QVERIFY(memo.Query(edits, edits.Terrain().get(), compare) == std::optional<bool>{false});
    QCOMPARE(comparisons, 3);
    const auto undoneRevision = edits.Revision();
    QVERIFY(edits.Redo()); QVERIFY(edits.Revision() > undoneRevision);
    QVERIFY(memo.Query(edits, edits.Terrain().get(), compare) == std::optional<bool>{true});
    QCOMPARE(comparisons, 4);
    const auto savedTerrain = edits.Terrain();
    const auto revision = edits.Revision(), sourceRevision = edits.TerrainSourceRevision();
    edits.CommitSaved(edits.Snapshot());
    QCOMPARE(edits.Revision(), revision); QCOMPARE(edits.Terrain().get(), savedTerrain.get());
    QVERIFY(edits.TerrainSourceRevision() > sourceRevision);
    QVERIFY(memo.Query(edits, savedTerrain.get(), compare) == std::optional<bool>{false});
    QVERIFY(memo.Query(edits, savedTerrain.get(), compare) == std::optional<bool>{false});
    QCOMPARE(comparisons, 5);
    // 保存后Undo反而偏离新基线，Redo回到新基线；不能复用保存前的布尔结论。
    QVERIFY(edits.Undo());
    QVERIFY(memo.Query(edits, edits.Terrain().get(), compare) == std::optional<bool>{true});
    QCOMPARE(comparisons, 6);
    QVERIFY(edits.Redo());
    QVERIFY(memo.Query(edits, edits.Terrain().get(), compare) == std::optional<bool>{false});
    QCOMPARE(comparisons, 7);
}

void WorldModuleTests::ShadowRelationMemoTracksAttachGenerationsAndSessionIdentity() {
    auto grid = FlatGrid(5, 5); grid->header.mainTileset = 'A';
    grid->header.groundTilesets = {"Ldrt"}; grid->header.cliffTilesets = {"Cldi"};
    const auto bytes = MapData::SerializeMapTerrain(*grid); QVERIFY(bytes.Succeeded());
    MapData::MapDataEditSession edits(MapDocument::FromMembers({}));
    QVERIFY(edits.AttachTerrain(*bytes.value).Succeeded());
    WorldShadowRelationMemo memo;
    int comparisons = 0;
    const auto compare = [&](const auto& current) {
        ++comparisons;
        return MapData::TerrainSavePlan::HasTerrainChanges(current);
    };
    const auto source = edits.Terrain();
    QVERIFY(memo.Query(edits, source.get(), compare) == std::optional<bool>{false});
    QCOMPARE(comparisons, 1);
    QByteArray wpm("MP3W", 4); Append32(wpm, 0); Append32(wpm, 16); Append32(wpm, 16);
    wpm.append(QByteArray(16 * 16, static_cast<char>(0x82)));
    const auto pathing = std::as_bytes(std::span(wpm.constData(), static_cast<std::size_t>(wpm.size())));
    const auto revision = edits.Revision(), sourceRevision = edits.TerrainSourceRevision();
    QVERIFY(edits.AttachPathing(pathing).Succeeded());
    QCOMPARE(edits.Revision(), revision); QCOMPARE(edits.Terrain().get(), source.get());
    QVERIFY(edits.TerrainSourceRevision() > sourceRevision);
    QVERIFY(memo.Query(edits, source.get(), compare) == std::optional<bool>{false});
    QCOMPARE(comparisons, 2);
    MapData::TerrainBrush brush; brush.kind = MapData::TerrainBrushKind::PathingRestriction;
    brush.radius = 0; brush.pathingRestrictions = 0x04;
    const MapData::TerrainStrokePoint point{144, 208};
    QVERIFY(edits.EditTerrain(brush, std::span(&point, 1), false).Succeeded());
    QCOMPARE(edits.Terrain().get(), source.get()); // 仅WPM变化也须重新计算，不能按W3E指针缓存。
    QVERIFY(memo.Query(edits, source.get(), compare) == std::optional<bool>{true});
    QVERIFY(memo.Query(edits, source.get(), compare) == std::optional<bool>{true});
    QCOMPARE(comparisons, 3);
    MapData::MapDataEditSession other(MapDocument::FromMembers({}));
    QVERIFY(other.AttachTerrain(*bytes.value).Succeeded() && other.AttachPathing(pathing).Succeeded());
    QVERIFY(other.EditRegions(MapData::RegionsDocument{5, {}, {}}).Succeeded());
    QVERIFY(other.IsDirty() && !MapData::TerrainSavePlan::HasTerrainChanges(other));
    QCOMPARE(other.Revision(), edits.Revision()); QCOMPARE(other.TerrainSourceRevision(), edits.TerrainSourceRevision());
    QVERIFY(other.SessionIdentity() != edits.SessionIdentity());
    QVERIFY(!memo.Query(other, source.get(), compare)); QCOMPARE(comparisons, 3);
    QVERIFY(memo.Query(other, other.Terrain().get(), compare) == std::optional<bool>{false});
    QVERIFY(memo.Query(other, other.Terrain().get(), compare) == std::optional<bool>{false});
    QCOMPARE(comparisons, 4);
    QVERIFY(memo.Query(edits, source.get(), compare) == std::optional<bool>{true});
    QCOMPARE(comparisons, 5);
}

int main(int argc, char** argv) {
    if (qEnvironmentVariable("FORGE_H1_CAPTURE_DIR").isEmpty()) qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_SCALE_FACTOR", "1.25"); // 仅当前测试进程，覆盖真实 Qt 分数 DPR，不修改机器配置。
    QApplication application(argc, argv);
    if (!qEnvironmentVariable("FORGE_R08_WORLD_SCREENSHOT_DIR").isEmpty())
        UiKit::ApplyApplicationFont(UiKit::LoadFonts(UiKit::DefaultFontDirectory()));
    UiKit::ThemeManager theme;
    if (!qEnvironmentVariable("FORGE_R08_WORLD_SCREENSHOT_DIR").isEmpty()) theme.ApplyTheme(QStringLiteral("graphite"));
    WorldModuleTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "WorldModuleTests.moc"
