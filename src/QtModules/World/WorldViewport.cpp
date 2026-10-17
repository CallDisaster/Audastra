#include "WorldViewport.h"
#include "WorldDiagnosticJson.h"
#include "WorldSelectionPresentation.h"
#include "WorldViewportInternals.h"

#include <QCoreApplication>
#include <QApplication>
#include <QDir>
#include <QHideEvent>
#include <QJsonArray>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPlatformSurfaceEvent>
#include <QResizeEvent>
#include <QShowEvent>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <string_view>
#include <type_traits>

namespace War3Map::Modules {
namespace {
// 原版游戏默认镜头：俯角 56°（攻击角 304°）、距离 1650、垂直 FOV 70°。后端固定 FOV 45°，
// 按同样的地面视野换算距离：1650 × tan(35°) / tan(22.5°) ≈ 2789。yaw −90° 表示镜头在目标南侧、朝北看。
constexpr War3RenderCameraV2 kGameCamera{sizeof(War3RenderCameraV2), 0, 0, 0, 2789.0f, -1.5707963f, 0.9773844f};

bool ValidApi(const War3RenderPluginApiV2* api) {
    // getViewInfo 是可选尾部。旧 V2 前缀仍能显示地形，只禁用缺乏回执的坐标选取。
    return api && api->structSize >= offsetof(War3RenderPluginApiV2, getViewInfo) && api->apiVersion == WAR3_RENDER_PLUGIN_API_V2 &&
        api->createRenderer && api->destroyRenderer && api->attachNativeWindow && api->resize &&
        api->submitMesh && api->setCamera && api->requestFrame && api->getStatus && api->captureRgba;
}
bool Finite(const War3RenderVertexV2& vertex) {
    return std::isfinite(vertex.x) && std::isfinite(vertex.y) && std::isfinite(vertex.z) &&
        std::abs(vertex.x) <= kMaximumCoordinate && std::abs(vertex.y) <= kMaximumCoordinate && std::abs(vertex.z) <= kMaximumCoordinate &&
        std::isfinite(vertex.red) && std::isfinite(vertex.green) && std::isfinite(vertex.blue) &&
        vertex.red >= 0 && vertex.red <= 1 && vertex.green >= 0 && vertex.green <= 1 && vertex.blue >= 0 && vertex.blue <= 1;
}
bool ValidMesh(const std::vector<War3RenderVertexV2>& vertices, const std::vector<std::uint32_t>& indices) {
    return vertices.size() <= 262144 && indices.size() <= 1572864 && vertices.empty() == indices.empty() &&
        indices.size() % 3 == 0 && std::all_of(vertices.begin(), vertices.end(), Finite) &&
        std::none_of(indices.begin(), indices.end(), [&](std::uint32_t index) { return index >= vertices.size(); });
}
bool MatchesTerrain(const MapData::TerrainGridSnapshot& terrain, std::uint32_t stride,
    const std::vector<War3RenderVertexV2>& vertices, const std::vector<std::uint32_t>& indices) {
    if (stride == 0 || terrain.header.width < 2 || terrain.header.height < 2) return false;
    if (stride != 1) return true; // 降采样只读预览，不能执行精确拾取或笔刷。
    if (terrain.vertices.size() != vertices.size() || terrain.tiles.size() != indices.size() / 6) return false;
    for (std::size_t i = 0; i < vertices.size(); ++i) {
        const auto& p = terrain.vertices[i].position;
        if (p[0] != vertices[i].x || p[1] != vertices[i].y || p[2] != vertices[i].z) return false;
    }
    for (std::size_t i = 0; i < terrain.tiles.size(); ++i) {
        const auto& t = terrain.tiles[i].vertices;
        const std::array<std::uint32_t, 6> expected{t[0], t[1], t[2], t[1], t[3], t[2]};
        if (!std::equal(expected.begin(), expected.end(), indices.begin() + static_cast<std::ptrdiff_t>(i * 6))) return false;
    }
    return true;
}
} // namespace

WorldViewport::WorldViewport(QWidget* parent, const War3RenderPluginApiV2* backend)
    : QWidget(parent), api_(backend), injected_(backend != nullptr) {
    qRegisterMetaType<RenderPluginFailure>();
    setObjectName(QStringLiteral("worldViewport"));
    setAttribute(Qt::WA_NativeWindow);
    setAttribute(Qt::WA_PaintOnScreen);
    setAttribute(Qt::WA_NoSystemBackground);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setAcceptDrops(true);
    setMinimumSize(160, 120);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    frameTimer_.setInterval(33);
    frameTimer_.setTimerType(Qt::PreciseTimer);
    connect(&frameTimer_, &QTimer::timeout, this, &WorldViewport::Tick);
    hoverTimer_.setInterval(50);
    connect(&hoverTimer_, &QTimer::timeout, this, &WorldViewport::UpdateHover);
    SetStatus(tr("渲染器尚未启动"));
    ClearCoordinates(tr("尚未载入可选取的地形"), true);
}

WorldViewport::~WorldViewport() {
    destroying_ = true;
    DestroyRenderer(); // 必须先回收插件线程，QWidget 基类随后才销毁 HWND。
    if (!injected_) {
        api_ = nullptr;
        library_.unload();
    }
}

QPaintEngine* WorldViewport::paintEngine() const { return nullptr; }

QSize WorldViewport::PhysicalSize() const {
    const qreal scale = devicePixelRatioF();
    return {std::max(1, qRound(width() * scale)), std::max(1, qRound(height() * scale))};
}

bool WorldViewport::CanRender() const {
    return isVisible() && !window()->isMinimized() && width() > 0 && height() > 0;
}

void WorldViewport::SetStatus(QString text) {
    if (text == status_) return;
    status_ = std::move(text);
    emit StatusChanged(status_);
}

QJsonObject WorldViewport::BackendDiagnosticSnapshot() const {
    const bool valid = ValidApi(api_);
    const auto number = [](std::uint64_t value) { return QString::number(value); };
    QJsonObject result{{"schema_version", 1}, {"injected", injected_}, {"library_loaded", library_.isLoaded()},
        {"api_present", api_ != nullptr}, {"api_valid", valid},
        {"renderer_ready", RendererReady()}, {"renderer_failed", failed_}, {"renderer_initializing", initializing_},
        {"frame_timer_active", frameTimer_.isActive()},
        {"renderer_epoch", number(measurementRendererEpoch_)}, {"viewport_document_revision", number(revision_)},
        {"has_animation_scene", HasAnimationScene()}, {"model_presentation_current", ModelPresentationCurrent()},
        {"gpu_timing_observed", false}, {"diagnostic_only", true}};
    WorldDiagnosticJson::Text(result, "library_path", library_.fileName());
    WorldDiagnosticJson::Text(result, "status_text", status_);
    result.insert("api_struct_size", api_ ? QJsonValue(number(api_->structSize)) : QJsonValue{});
    result.insert("api_version", api_ && api_->structSize >= offsetof(War3RenderPluginApiV2, apiVersion) + sizeof(api_->apiVersion)
        ? QJsonValue(number(api_->apiVersion)) : QJsonValue{});
    result.insert("capabilities", QJsonObject{{"models", valid && HasModelSupport()},
        {"animation", valid && HasAnimationSupport()}, {"particles", valid && HasParticleSupport()},
        {"model_layers", valid && HasModelLayerSupport()}, {"instance_roster", valid && HasInstanceRosterSupport()}});
    result.insert("attach_result", attachResult_ ? QJsonValue(*attachResult_) : QJsonValue{});
    result.insert("status_call_result", statusCallResult_ ? QJsonValue(*statusCallResult_) : QJsonValue{});
    QJsonObject status{{"observed", observedStatus_.has_value()}};
    if (observedStatus_) {
        const auto& observed = *observedStatus_;
        status.insert("renderer_epoch", number(observedStatusRendererEpoch_));
        const auto messageLength = std::find(std::begin(observed.message), std::end(observed.message), '\0')
            - std::begin(observed.message);
        status.insert("result", observed.result);
        status.insert("device_ready", observed.deviceReady != 0);
        status.insert("software", observed.software != 0);
        status.insert("frame_count", number(observed.frameCount));
        status.insert("document_revision", number(observed.documentRevision));
        status.insert("message", QString::fromUtf8(observed.message, static_cast<qsizetype>(messageLength)));
    }
    result.insert("observed_renderer_status", status);
    const auto failures = WorldDiagnosticJson::Diagnostics(animationFailure_.diagnostics);
    result.insert("animation_failure", failures);
    result.insert("animation_failure_page", WorldDiagnosticJson::Page(animationFailure_.diagnostics.size(),
        0, static_cast<std::size_t>(failures.size()), 64));
    const auto groundFailures = WorldDiagnosticJson::Diagnostics(groundFailure_.diagnostics);
    result.insert("ground_failure", groundFailures);
    result.insert("ground_failure_page", WorldDiagnosticJson::Page(groundFailure_.diagnostics.size(),
        0, static_cast<std::size_t>(groundFailures.size()), 64));
    return result;
}

void WorldViewport::EnsureRenderer() {
    if (renderer_ || initializing_ || destroying_ || failed_ || !CanRender()) return;
    const QSize pixels = PhysicalSize();
    if (pixels.width() > 4096 || pixels.height() > 4096) {
        SetStatus(tr("视口物理尺寸超过 4096 × 4096，请缩小窗口"));
        return;
    }
    initializing_ = true;
    attachResult_.reset(); statusCallResult_.reset(); observedStatus_.reset();
    if (!api_ && !injected_) {
        library_.setFileName(QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("war3-render-d3d11.dll")));
        if (library_.load()) {
            const auto getApi = reinterpret_cast<War3GetRenderPluginApiV2>(library_.resolve("War3MapEditor_GetRenderPluginApiV2"));
            if (getApi) api_ = getApi(WAR3_RENDER_PLUGIN_API_V2);
        }
    }
    if (!ValidApi(api_)) {
        failed_ = true;
        SetStatus(tr("无法加载兼容的 D3D11 渲染插件：%1").arg(library_.errorString()));
    } else {
        const War3RenderCreateInfoV2 info{sizeof(War3RenderCreateInfoV2), static_cast<std::uint32_t>(pixels.width()),
            static_cast<std::uint32_t>(pixels.height()), 0};
        renderer_ = api_->createRenderer(&info);
        if (renderer_) attachResult_ = api_->attachNativeWindow(renderer_, reinterpret_cast<void*>(winId()));
        if (!renderer_ || *attachResult_ != 0) {
            ReportPluginFailure(renderer_ ? "attachNativeWindow" : "createRenderer",
                renderer_ ? *attachResult_ : -1);
            failed_ = true;
            DestroyRenderer();
            SetStatus(tr("D3D11 渲染窗口初始化失败"));
        } else if (!SubmitMesh()) {
            failed_ = true;
            DestroyRenderer();
        } else {
            (void)SubmitModels(); // 模型失败不影响地形视图，状态行给出原因
            (void)SubmitEnvironment();
            (void)SubmitOverlay();
            SubmitCamera();
            SetStatus(tr("D3D11 正在初始化"));
            frameTimer_.start();
            Tick();
        }
    }
    initializing_ = false;
}

void WorldViewport::DestroyRenderer() {
    ++measurementRendererEpoch_;
    animationPickHistory_.clear();
    bugPreparationHistory_.clear(); bugPreparationFailure_ = {};
    selectionOutline_.clear();
    CancelTerrainStroke();
    CancelMove();
    frameTimer_.stop();
    hoverTimer_.stop();
    dragButton_ = Qt::NoButton;
    dragging_ = false; pressView_.reset();
    ClearCoordinates(tr("视口未就绪"), true);
    animationRenderer_.reset();
    modelLayerBridges_.clear(); modelLayerBridgeHandle_ = nullptr;
    editorPresentationTarget_.reset(); editorPresentationRevision_ = 0;
    editorVisibilityDirty_ = static_cast<bool>(instanceVisible_);
    if (renderer_) {
        api_->destroyRenderer(renderer_);
        renderer_ = nullptr;
    }
}

void WorldViewport::Tick() {
    if (!renderer_ || !CanRender()) return;
    War3RenderStatusV2 status{};
    status.structSize = sizeof(status);
    statusCallResult_ = api_->getStatus(renderer_, &status);
    observedStatus_ = status;
    observedStatusRendererEpoch_ = measurementRendererEpoch_;
    if (*statusCallResult_ != 0 || status.result != 0) {
        ReportPluginFailure("getStatus", *statusCallResult_ != 0 ? *statusCallResult_ : status.result,
            &status, *statusCallResult_);
        frameTimer_.stop();
        failed_ = true;
        const auto end = std::find(std::begin(status.message), std::end(status.message), '\0');
        SetStatus(tr("D3D11 渲染失败：%1").arg(QString::fromUtf8(status.message, end - std::begin(status.message))));
        hoverTimer_.stop(); ClearCoordinates(tr("渲染失败，无法选取地形坐标"), true);
        return;
    }
    if (status.deviceReady && status.frameCount && status.documentRevision == revision_) {
        if (std::string_view(status.message, sizeof(status.message)).starts_with("Model upload failed:"))
            SetStatus(tr("模型上传失败：%1").arg(QString::fromUtf8(status.message,
                std::find(std::begin(status.message), std::end(status.message), '\0') - std::begin(status.message))));
        else SetStatus(status.software ? tr("D3D11 · 软件渲染") : tr("D3D11 · 硬件渲染"));
    }
    if (animationRenderer_) {
        const auto skin = animationRenderer_->StatusNow();
        if (!skin.Succeeded()) {
            animationFailure_.diagnostics = skin.diagnostics;
            SetStatus(tr("模型上传失败：动画 GPU 上传或绘制失败"));
            return;
        }
    }
    api_->requestFrame(renderer_);
    ScheduleHover();
}

void WorldViewport::ReportPluginFailure(const char* operation, std::int32_t result,
    const War3RenderStatusV2* observed, std::optional<std::int32_t> statusCall) const {
    if (result == 0 || destroying_) return;
    RenderPluginFailure failure;
    failure.operation = QString::fromLatin1(operation);
    failure.result = result;
    War3RenderStatusV2 status{sizeof(status)};
    if (observed) {
        status = *observed;
        failure.statusCallResult = statusCall.value_or(0);
        failure.statusAvailable = status.structSize >= sizeof(status);
    } else if (renderer_ && api_ && api_->getStatus) {
        failure.statusCallResult = api_->getStatus(renderer_, &status);
        failure.statusAvailable = status.structSize >= sizeof(status);
    }
    if (failure.statusAvailable) {
        failure.statusResult = status.result;
        const auto end = std::find(std::begin(status.message), std::end(status.message), '\0');
        failure.message = QString::fromUtf8(status.message, end - std::begin(status.message));
    }
    emit const_cast<WorldViewport*>(this)->PluginFailed(failure);
}

bool WorldViewport::SubmitMesh() {
    if (!renderer_) return true;
    if (ground_ && api_->structSize >= offsetof(War3RenderPluginApiV2, submitTerrainScene) + sizeof(api_->submitTerrainScene) && api_->submitTerrainScene) {
        std::vector<War3RenderTerrainVertexV2> vertices;
        vertices.reserve(ground_->vertices.size());
        for (const auto& v : ground_->vertices) vertices.push_back({v.x, v.y, v.z, v.red, v.green, v.blue, v.u, v.v});
        std::vector<War3RenderTerrainBatchV2> batches;
        for (const auto& b : ground_->batches) batches.push_back({b.firstIndex, b.indexCount,
            b.textureIndex | (b.alphaBlend ? WAR3_RENDER_TERRAIN_ALPHA_BLEND : 0u) | (b.translucent ? WAR3_RENDER_TERRAIN_TRANSLUCENT : 0u)});
        if (HasTerrainMips()) {
            std::vector<std::vector<War3RenderMipRgbaV2>> mipLists(ground_->textures.size());
            std::vector<War3RenderTextureMipChainV2> chains;
            chains.reserve(ground_->textures.size());
            for (std::size_t index = 0; index < ground_->textures.size(); ++index) {
                const auto& texture = *ground_->textures[index];
                auto& mips = mipLists[index];
                const auto append = [&](const Image::DecodedImage& mip) {
                    mips.push_back({sizeof(War3RenderMipRgbaV2), mip.width, mip.height, mip.rgba.size(), mip.rgba.data()});
                };
                append(texture.image);
                for (const auto& mip : texture.additionalMips) append(mip);
                std::uint32_t maximum = 0;
                for (std::size_t level = 1; level < mips.size(); ++level) {
                    if (mips[level].width < texture.columns || mips[level].height < texture.rows
                        || mips[level].width % texture.columns != 0 || mips[level].height % texture.rows != 0) break;
                    maximum = static_cast<std::uint32_t>(level);
                }
                chains.push_back({sizeof(War3RenderTextureMipChainV2), static_cast<std::uint32_t>(mips.size()),
                    maximum, texture.columns, texture.rows, mips.data()});
            }
            const War3RenderTerrainMipSceneV2 scene{sizeof(War3RenderTerrainMipSceneV2), static_cast<std::uint32_t>(vertices.size()),
                static_cast<std::uint32_t>(ground_->indices.size()), static_cast<std::uint32_t>(chains.size()),
                static_cast<std::uint32_t>(batches.size()), revision_, vertices.data(), ground_->indices.data(), chains.data(), batches.data()};
            const auto result = api_->submitTerrainMipScene(renderer_, &scene);
            if (result == 0) return true;
            ReportPluginFailure("submitTerrainMipScene", result);
            SetStatus(tr("渲染器拒绝了地表 mip 场景，保留此前视图"));
            return false;
        }
        std::vector<War3RenderTextureRgbaV2> textures;
        for (const auto& texture : ground_->textures) textures.push_back({sizeof(War3RenderTextureRgbaV2),
            texture->image.width, texture->image.height, texture->image.rgba.size(), texture->image.rgba.data()});
        const War3RenderTerrainSceneV2 scene{sizeof(War3RenderTerrainSceneV2), static_cast<std::uint32_t>(vertices.size()),
            static_cast<std::uint32_t>(ground_->indices.size()), static_cast<std::uint32_t>(textures.size()),
            static_cast<std::uint32_t>(batches.size()), revision_, vertices.data(), ground_->indices.data(), textures.data(), batches.data()};
        const auto result = api_->submitTerrainScene(renderer_, &scene);
        if (result == 0) return true;
        ReportPluginFailure("submitTerrainScene", result);
        SetStatus(tr("渲染器拒绝了地表贴图，保留高度预览"));
        return false;
    }
    const War3RenderMeshV2 mesh{sizeof(War3RenderMeshV2), static_cast<std::uint32_t>(vertices_.size()),
        static_cast<std::uint32_t>(indices_.size()), revision_, vertices_.data(), indices_.data()};
    const auto result = api_->submitMesh(renderer_, &mesh);
    if (result == 0) return true;
    ReportPluginFailure("submitMesh", result);
    SetStatus(tr("渲染器拒绝了地形网格"));
    return false;
}

bool WorldViewport::HasModelSupport() const noexcept {
    return api_ && api_->structSize >= offsetof(War3RenderPluginApiV2, submitModelScene)
        + sizeof(api_->submitModelScene) && api_->submitModelScene;
}

bool WorldViewport::SubmitModels() {
    if (animationScene_) return SubmitAnimation();
    return SubmitModels(models_.get());
}

bool WorldViewport::SubmitModels(const Render::WorldModelScene* models) {
    if (!renderer_) return true;
    if (!HasModelSupport()) return models == nullptr;
    static const Render::WorldModelScene empty;
    if (const auto status = Render::CheckStaticModelSamplerSupport(api_, renderer_, models ? *models : empty); !status.Succeeded()) {
        SetStatus(QString::fromStdString(status.diagnostics.front().code + ": " + status.diagnostics.front().message));
        return false;
    }
    const auto descriptor = Render::DescribeModelScene(models ? *models : empty);
    const auto* input = descriptor.StaticScene();
    if (!input) return false;
    if (HasEditorVisibilitySupport()) {
        if (SubmitStaticVisibility(models, *input)) return true;
        SetStatus(tr("渲染器拒绝了模型与显隐组合场景，未替换当前视图"));
        return false;
    }
    if (instanceVisible_) {
        EditorVisibilityBuffers masks;
        if (!BuildEditorVisibility(models, nullptr, masks)
            || std::find(masks.instances.begin(), masks.instances.end(), 0) != masks.instances.end()) {
            SetStatus(tr("当前渲染插件不支持编辑器显隐，未显示被隐藏的模型"));
            return false;
        }
    }
    const auto result = api_->submitModelScene(renderer_, input);
    if (result == 0) {
        editorPresentationTarget_.reset(); editorVisibilityDirty_ = false;
        return true;
    }
    ReportPluginFailure("submitModelScene", result);
    SetStatus(tr("渲染器拒绝了模型场景，未替换当前视图"));
    return false;
}

bool WorldViewport::HasEnvironmentSupport() const noexcept {
    return api_ && api_->structSize >= offsetof(War3RenderPluginApiV2, setEnvironment)
        + sizeof(api_->setEnvironment) && api_->setEnvironment;
}

Status WorldViewport::CheckCliffSamplerSupport(std::span<const Render::BakedModelMesh> baked, bool layered) const {
    if (std::any_of(baked.begin(), baked.end(), [](const auto& mesh) { return mesh.samplerFlags > 3; }))
        return Status::Failure("Render.World", "WORLD_STATIC_SAMPLER_INVALID", "悬崖来源 sampler 含未知标志。");
    if (std::none_of(baked.begin(), baked.end(), [](const auto& mesh) { return mesh.samplerFlags != 3; })) return {};
    if (layered || animationScene_) {
        War3RenderSkinMaterialStateCapabilitiesV2 capabilities{};
        capabilities.base.base.base.base.base.structSize = sizeof(capabilities);
        if (!api_ || api_->structSize < offsetof(War3RenderPluginApiV2, getSkinStatus) + sizeof(api_->getSkinStatus)
            || !api_->getSkinStatus || api_->getSkinStatus(renderer_, &capabilities.base.base.base.base.base) != 0
            || capabilities.materialStateFormat != WAR3_RENDER_DYNAMIC_MATERIAL_STATE_V1 || capabilities.reserved
            || capabilities.base.layerAlphaFormat != WAR3_RENDER_DYNAMIC_LAYER_ALPHA_V1 || capabilities.base.reserved
            || (layered && !HasModelLayerSupport()))
            return Status::Failure("Render.World", "WORLD_STATIC_SAMPLER_UNSUPPORTED",
                "渲染插件不支持悬崖材质 sampler，未替换配对地面与悬崖。");
        return {};
    }
    return Render::CheckStaticModelSamplerCapability(api_, renderer_);
}

bool WorldViewport::SubmitEnvironment() {
    if (!renderer_ || !environment_) return true;
    if (!HasEnvironmentSupport()) return false;
    const auto descriptor = Render::DescribeSceneEnvironment(*environment_);
    const auto result = api_->setEnvironment(renderer_, &descriptor);
    if (result == 0) return true;
    ReportPluginFailure("setEnvironment", result);
    SetStatus(tr("渲染器拒绝了光照/雾/阴影设置，按默认光照显示"));
    return false;
}

bool WorldViewport::SetEnvironment(std::shared_ptr<const Render::SceneEnvironment> environment) {
    environment_ = std::move(environment);
    if (!renderer_) return !api_ || HasEnvironmentSupport();
    return SubmitEnvironment();
}

bool WorldViewport::UpdateModelInstances() {
    if (animationScene_) { emit AnimationTransformsChanged(); return true; }
    if (!renderer_ || !models_) return true;
    if (HasEditorVisibilitySupport()) {
        editorVisibilityDirty_ = true;
        if (UpdateStaticVisibility(true)) return true;
        return SubmitModels(); // 实例数量/模型布局变化复用完整提交；仍携同次初始掩码
    }
    if (instanceVisible_) {
        EditorVisibilityBuffers masks;
        if (!BuildEditorVisibility(models_.get(), nullptr, masks)
            || std::find(masks.instances.begin(), masks.instances.end(), 0) != masks.instances.end()) return false;
    }
    const bool partial = api_->structSize >= offsetof(War3RenderPluginApiV2, updateModelInstances)
        + sizeof(api_->updateModelInstances) && api_->updateModelInstances;
    if (partial) {
        const auto result = api_->updateModelInstances(renderer_, models_->instances.data(),
            static_cast<std::uint32_t>(models_->instances.size()));
        if (result == 0) {
            editorPresentationTarget_.reset(); editorVisibilityDirty_ = false;
            return true;
        }
        ReportPluginFailure("updateModelInstances", result);
    }
    return SubmitModels();
}

bool WorldViewport::HasOverlaySupport() const noexcept {
    return api_ && api_->structSize >= offsetof(War3RenderPluginApiV2, setOverlayLines)
        + sizeof(api_->setOverlayLines) && api_->setOverlayLines;
}

bool WorldViewport::SubmitOverlay() {
    if (!renderer_) return true;
    if (!HasOverlaySupport()) return overlay_.empty() && selectionOutline_.empty();
    auto lines = overlay_;
    lines.insert(lines.end(), selectionOutline_.begin(), selectionOutline_.end());
    const auto result = api_->setOverlayLines(renderer_, lines.data(), static_cast<std::uint32_t>(lines.size()));
    if (result != 0) ReportPluginFailure("setOverlayLines", result);
    return result == 0;
}

void WorldViewport::UpdateSelectionOutline(QPointF point) {
    const auto completed = ReadPickingFrame();
    if (!completed.Succeeded()) { ClearSelectionOutline(); return; }
    auto outline = BuildWorldSelectionOutline(*completed.value,
        {static_cast<std::uint32_t>(width()), static_cast<std::uint32_t>(height()), devicePixelRatioF()},
        {pressPosition_.x(), pressPosition_.y()}, {point.x(), point.y()});
    if (!outline.Succeeded()) { ClearSelectionOutline(); return; }
    selectionOutline_ = std::move(*outline.value);
    (void)SubmitOverlay();
}

void WorldViewport::ClearSelectionOutline() {
    if (selectionOutline_.empty()) return;
    selectionOutline_.clear();
    (void)SubmitOverlay();
}

void WorldViewport::RetainBugPreparationFrame() try {
    if (!animationScene_ || !animationFrame_ || !editorPresentationTarget_ || !animationPickingCurrent_) return;
    if (!bugPreparationHistory_.empty()) {
        const auto& last = bugPreparationHistory_.back();
        if (last.snapshot->sourceScene.lock() == animationScene_ && last.rendererEpoch == measurementRendererEpoch_
            && last.policyRevision == pickPolicyRevision_ && last.contentVersion == animationFrame_->contentVersion
            && last.sceneVersion == animationFrame_->sceneVersion && last.presentationRevision == editorPresentationRevision_) return;
    }
    auto captured = Services::CaptureWorldModelBugPlaceholderFrame(animationScene_, *animationFrame_);
    if (!captured.Succeeded()) {
        bugPreparationHistory_.clear(); bugPreparationFailure_ = {std::move(captured.diagnostics)}; return;
    }
    auto snapshot = std::make_shared<const Services::WorldModelBugPlaceholderFrameSnapshot>(std::move(*captured.value));
    const auto bytes = snapshot->ownedBytes;
    std::size_t retained = 0; for (const auto& owned : bugPreparationHistory_) retained += owned.ownedBytes;
    while (!bugPreparationHistory_.empty() && (bugPreparationHistory_.size() >= 4
        || retained > Services::kMaximumWorldModelBugSnapshotBytes - bytes)) {
        retained -= bugPreparationHistory_.front().ownedBytes; bugPreparationHistory_.pop_front();
    }
    const AnimationCompletionIdentity identity{*editorPresentationTarget_, editorPresentationRevision_, measurementRendererEpoch_,
        pickPolicyRevision_, animationFrame_->sceneVersion, animationFrame_->contentVersion, animationFrame_->documentRevision,
        animationFrame_->instances.size(), static_cast<std::uint32_t>(animationFrame_->particleVertices.size()),
        static_cast<std::uint32_t>(animationFrame_->particleDraws.size())};
    bugPreparationHistory_.push_back({identity, std::move(snapshot), bytes}); bugPreparationFailure_ = {};
} catch (...) {
    bugPreparationHistory_.clear();
    bugPreparationFailure_ = Status::Failure("World.BugPlaceholder", "WORLD_BUG_PLACEHOLDER_SNAPSHOT_FAILED",
        "无法拥有有界占位来源快照，未借用几何拾取历史或伪造完成帧。");
}

void WorldViewport::RetainAnimationPickFrame() try {
    if (!animationScene_ || !animationFrame_ || !editorPresentationTarget_ || !animationPickingCurrent_) return;
    RetainBugPreparationFrame(); // 独立稀疏来源；完整几何历史超预算不能阻止CPU占位准备。
    constexpr std::size_t budget = 64ull * 1024 * 1024;
    const auto& source = *animationFrame_;
    if (!animationPickHistory_.empty() && animationPickHistory_.back().frame->contentVersion == source.contentVersion &&
        animationPickHistory_.back().frame->sceneVersion == source.sceneVersion &&
        animationPickHistory_.back().presentationRevision == editorPresentationRevision_) return;
    const auto measure = [budget](const Render::AnimationFrame& candidate, bool capacity) -> std::optional<std::size_t> {
        std::size_t bytes = sizeof(Render::AnimationFrame);
        const auto charge = [&](const auto& values) {
            const auto count = capacity ? values.capacity() : values.size();
            constexpr auto stride = sizeof(typename std::decay_t<decltype(values)>::value_type);
            if (count > (budget - bytes) / stride) return false;
            bytes += count * stride; return true;
        };
        if (!charge(candidate.states) || !charge(candidate.requestVisibility) || !charge(candidate.instances) ||
            !charge(candidate.groups) || !charge(candidate.alphas) || !charge(candidate.poses) ||
            !charge(candidate.collisionMatrices) || !charge(candidate.pickBounds) || !charge(candidate.particleLinks)) return std::nullopt;
        const auto chargeString = [&](const std::string& value) {
            // 包含终止字节；SSO 也保守计费，不依赖特定 STL 的短串实现。
            const auto count = capacity ? value.capacity() : value.size();
            if (count >= budget - bytes) return false;
            bytes += count + 1; return true;
        };
        for (const auto& pose : candidate.poses)
            if (!chargeString(pose.rejectionCode) || !chargeString(pose.rejectionMessage)) return std::nullopt;
        for (const auto& link : candidate.particleLinks)
            if (!chargeString(link.rejectionCode) || !chargeString(link.rejectionMessage) || !chargeString(link.resetReason)) return std::nullopt;
        return bytes;
    };
    // 拷贝前先拒绝本身已超预算的输入；源帧的备用容量不会被复制。
    const auto minimum = measure(source, false);
    if (!minimum) { animationPickHistory_.clear(); return; }
    std::size_t retained = 0;
    for (const auto& owner : animationPickHistory_) retained += owner.ownedBytes;
    std::shared_ptr<Render::AnimationFrame> frame;
    while (!animationPickHistory_.empty() && (animationPickHistory_.size() >= 4 || retained > budget - *minimum)) {
        auto retired = std::move(animationPickHistory_.front());
        animationPickHistory_.pop_front(); retained -= retired.ownedBytes;
        // 同一 UI 线程只在实际点选期间借用 const 帧；从历史移除后且唯一拥有才可写。
        // 无独立退休池：所有复用容量仍计入这次候选和至多四帧的历史预算。
        if (!frame && retired.frame.use_count() == 1) frame = std::move(retired.frame);
    }
    // 仅保留精确点选所需的owned姿势；粒子顶点/检查点不在历史中重复驻留。
    if (!frame) frame = std::make_shared<Render::AnimationFrame>();
    frame->sceneVersion = source.sceneVersion; frame->contentVersion = source.contentVersion; frame->documentRevision = source.documentRevision;
    frame->states = source.states; frame->requestVisibility = source.requestVisibility;
    frame->instances = source.instances; frame->groups = source.groups; frame->alphas = source.alphas; frame->poses = source.poses;
    frame->collisionMatrices = source.collisionMatrices; frame->pickBounds = source.pickBounds; frame->particleLinks = source.particleLinks;
    frame->sharedPoseRanges = source.sharedPoseRanges;
    // 复用后 size 变小也可能仍保留大容量；按实际分配重新核算，不把备用容量漏算。
    const auto bytes = measure(*frame, true);
    if (!bytes) { animationPickHistory_.clear(); return; }
    while (!animationPickHistory_.empty() && retained > budget - *bytes) {
        retained -= animationPickHistory_.front().ownedBytes; animationPickHistory_.pop_front();
    }
    const AnimationCompletionIdentity identity{*editorPresentationTarget_, editorPresentationRevision_, measurementRendererEpoch_,
        pickPolicyRevision_, source.sceneVersion, source.contentVersion, source.documentRevision, source.instances.size(),
        static_cast<std::uint32_t>(source.particleVertices.size()), static_cast<std::uint32_t>(source.particleDraws.size())};
    animationPickHistory_.push_back({identity, std::move(frame), *bytes});
} catch (...) {
    // 点选历史是有界旁路；分配失败时拒绝点选，不能中止仍有效的播放/绘制。
    animationPickHistory_.clear();
}

bool WorldViewport::SetOverlayLines(std::vector<War3RenderOverlayLineV2> lines) {
    overlay_ = std::move(lines);
    if (!renderer_) return !api_ || HasOverlaySupport() || overlay_.empty();
    return SubmitOverlay();
}

void WorldViewport::SetSelectedInstance(std::optional<std::uint32_t> instance) {
    if (instance != selectedInstance_) CancelMove();
    selectedInstance_ = instance;
}

void WorldViewport::SetPlacing(bool placing) {
    if (placing_ == placing) return;
    placing_ = placing;
    placementHoverActive_ = placing && hoverCoordinate_.has_value();
    hoverView_.reset();
    if (placing_) ScheduleHover();
    else emit PlacementHoverLost();
}

void WorldViewport::BeginMove(QPointF point, const Render::ViewProjectionFrame& frame, bool rotate) {
    moving_ = moved_ = rotating_ = false;
    if (placing_) return;
    // 拖动按地面点差值移动：按下处必须能拾取到地面。
    const auto ground = PickAt(point, frame);
    if (!ground.Succeeded() || !ground.value->hit) return;
    pressGround_ = {ground.value->hit->position[0], ground.value->hit->position[1]};
    if (selectedInstance_ && models_ && *selectedInstance_ < models_->instances.size()) {
        const auto hits = PickInstancesAt(point, frame);
        if (hits.Succeeded()) moving_ = std::any_of(hits.value->begin(), hits.value->end(),
            [&](const auto& hit) { return hit.instance == *selectedInstance_; });
        else ClearCoordinates(PickDiagnostic(hits.diagnostics));
        rotating_ = moving_ && rotate;
    }
    if (!moving_ && handleTest_) moving_ = handleTest_(pressGround_[0], pressGround_[1]);
}

void WorldViewport::CancelMove() {
    const bool notify = moving_ && moved_;
    const bool rotate = rotating_;
    moving_ = moved_ = rotating_ = false;
    if (notify) {
        if (rotate) emit RotateCancelled();
        else emit MoveCancelled();
    }
}

bool WorldViewport::SetModelScene(std::shared_ptr<const Render::WorldModelScene> models) {
    // submitModelScene 的返回值只证明插件已接收 CPU 数据；完成帧另以回执为准。
    // 在该接收门禁前不能换掉拾取所用场景，也不能取消正在进行的选择/拖动。
    if (renderer_) {
        if (!SubmitModels(models.get())) return false;
    } else if (models && api_ && !HasModelSupport()) {
        return false;
    }
    animationRenderer_.reset(); animationScene_.reset(); animationFrame_.reset();
    if (models != models_) { CancelMove(); selectedInstance_.reset(); }
    models_ = std::move(models);
    return true;
}

bool WorldViewport::SetMesh(std::vector<War3RenderVertexV2> vertices, std::vector<std::uint32_t> indices,
    std::uint64_t documentRevision) {
    CancelTerrainStroke();
    groundFailure_ = {};
    ground_.reset();
    animationRenderer_.reset(); animationScene_.reset(); animationFrame_.reset();
    if (models_) { models_.reset(); (void)SubmitModels(); } // 地形换代时旧模型一并清除
    terrain_.reset(); terrainStride_ = 0; pressView_.reset();
    ClearCoordinates(tr("正在更新地形坐标数据…"), true);
    if (!ValidMesh(vertices, indices)) {
        SetStatus(tr("地形网格无效"));
        ClearCoordinates(tr("地形网格无效，无法选取坐标"), true);
        return false;
    }
    vertices_ = std::move(vertices); indices_ = std::move(indices); revision_ = documentRevision;
    homeCamera_ = kGameCamera;
    if (!vertices_.empty()) {
        // 与原版编辑器一致：对准地图中心、朝北的游戏视角，不把全图塞进视口（大地图不会一次铺满屏幕）。
        float minX = vertices_.front().x, maxX = minX, minY = vertices_.front().y, maxY = minY;
        for (const auto& vertex : vertices_) {
            minX = std::min(minX, vertex.x); maxX = std::max(maxX, vertex.x);
            minY = std::min(minY, vertex.y); maxY = std::max(maxY, vertex.y);
        }
        homeCamera_.targetX = minX * 0.5f + maxX * 0.5f;
        homeCamera_.targetY = minY * 0.5f + maxY * 0.5f;
        // 目标高度取离中心最近的地形顶点，镜头落在地面上。
        const auto nearest = std::min_element(vertices_.begin(), vertices_.end(), [&](const auto& a, const auto& b) {
            return std::hypot(a.x - homeCamera_.targetX, a.y - homeCamera_.targetY)
                < std::hypot(b.x - homeCamera_.targetX, b.y - homeCamera_.targetY);
        });
        homeCamera_.targetZ = nearest->z;
    }
    const bool accepted = SubmitMesh();
    ResetCamera();
    return accepted;
}

void WorldViewport::ClearMesh(std::uint64_t documentRevision) { (void)SetMesh({}, {}, documentRevision); }

bool WorldViewport::SetPickingTerrain(std::shared_ptr<const MapData::TerrainGridSnapshot> terrain,
    std::uint32_t stride, std::uint64_t documentRevision) {
    terrain_.reset(); terrainStride_ = 0; pressView_.reset();
    ClearCoordinates(tr("正在更新地形坐标数据…"), true);
    const bool matches = terrain && documentRevision == revision_ && MatchesTerrain(*terrain, stride, vertices_, indices_);
    if (!matches) {
        ClearCoordinates(tr("地形拾取数据与显示网格不一致，无法选取坐标"), true); return false;
    }
    terrain_ = std::move(terrain); terrainStride_ = stride;
    if (stride > 1) ClearCoordinates(tr("当前每 %1 格取样，暂不可精确拾取地形坐标").arg(stride), true);
    else if (api_ && !HasViewInfo()) ClearCoordinates(tr("当前渲染插件缺少完成帧回执，暂不可拾取坐标"), true);
    else ClearCoordinates(tr("悬停查看地形坐标，左键单击固定所选坐标"), true);
    ScheduleHover();
    return true;
}

bool WorldViewport::HasViewInfo() const {
    return api_ && api_->structSize >= offsetof(War3RenderPluginApiV2, getViewInfo) + sizeof(api_->getViewInfo) && api_->getViewInfo;
}

bool WorldViewport::HasTerrainMips() const noexcept {
    return api_ && api_->structSize >= offsetof(War3RenderPluginApiV2, submitTerrainMipScene)
        + sizeof(api_->submitTerrainMipScene) && api_->submitTerrainMipScene;
}

bool WorldViewport::HasGroundSupport() const noexcept {
    return api_ && api_->structSize >= offsetof(War3RenderPluginApiV2, submitTerrainScene)
        + sizeof(api_->submitTerrainScene) && api_->submitTerrainScene;
}

bool WorldViewport::ReplaceTerrainPresentation(std::vector<War3RenderVertexV2> vertices,
    std::vector<std::uint32_t> indices, std::shared_ptr<const MapData::TerrainGridSnapshot> terrain,
    std::uint32_t stride, std::shared_ptr<const Render::TerrainGroundMesh> ground, std::uint64_t documentRevision,
    std::span<const Render::BakedModelMesh> cliffs, bool layeredCliffs) {
    if (ground) {
        const auto support = CheckCliffSamplerSupport(cliffs, layeredCliffs);
        if (!support.Succeeded()) {
            groundFailure_ = support;
            SetStatus(QString::fromStdString(support.diagnostics.front().code + ": " + support.diagnostics.front().message));
            return false; // 不提交/回滚任何新ground，不触碰旧CPU来源和完成帧。
        }
    }
    if (!terrain || !ValidMesh(vertices, indices) || vertices.empty() ||
        !MatchesTerrain(*terrain, stride, vertices, indices) || (ground && !HasGroundSupport())) return false;
    auto oldVertices = std::move(vertices_);
    auto oldIndices = std::move(indices_);
    auto oldTerrain = std::move(terrain_);
    auto oldGround = std::move(ground_);
    const auto oldStride = terrainStride_;
    const auto oldRevision = revision_;
    vertices_ = std::move(vertices); indices_ = std::move(indices);
    terrain_ = std::move(terrain); terrainStride_ = stride; revision_ = documentRevision;
    ground_.reset();
    const bool accepted = ground ? SetGroundMesh(std::move(ground)) : SubmitMesh();
    if (!accepted) {
        vertices_ = std::move(oldVertices); indices_ = std::move(oldIndices);
        terrain_ = std::move(oldTerrain); ground_ = std::move(oldGround);
        terrainStride_ = oldStride; revision_ = oldRevision;
        (void)SubmitMesh(); // 插件若曾接受临时场景，恢复先前已确认的地形。
        return false;
    }
    pressView_.reset();
    ClearCoordinates(tr("地表视图更新中…"), true);
    ScheduleHover();
    return true;
}

void WorldViewport::SetBrushMode(bool enabled) {
    if (brushMode_ == enabled) return;
    CancelTerrainStroke();
    CancelMove();
    brushMode_ = enabled;
    if (enabled) { placing_ = false; selectedInstance_.reset(); }
}

void WorldViewport::CancelTerrainStroke() {
    if (!strokeActive_) return;
    strokeActive_ = false;
    strokeTerrain_.reset();
    dragButton_ = Qt::NoButton;
    dragging_ = false;
    pressView_.reset();
    emit TerrainStrokeCancelled();
}

bool WorldViewport::SetGroundMesh(std::shared_ptr<const Render::TerrainGroundMesh> ground) {
    const auto reject = [this](const char* code, const char* message) {
        groundFailure_ = Status::Failure("World", code, message);
        return false;
    };
    if (!ground || !terrain_ || terrainStride_ != 1)
        return reject("WORLD_GROUND_SOURCE", "地表贴图网格未绑定完整分辨率的地形来源。");
    if (!HasGroundSupport())
        return reject("WORLD_GROUND_UNSUPPORTED", "当前渲染插件未提供地表贴图接口。");
    const auto tiles = terrain_->tiles.size();
    // 悬崖覆盖表仍对应原格；紧缩基础层映射必须恰好为它的有序补集。
    if (!ground->hiddenTiles.empty() && ground->hiddenTiles.size() != tiles)
        return reject("WORLD_GROUND_HIDDEN_LAYOUT", "悬崖覆盖表与地形格数不一致。");
    if (std::any_of(ground->hiddenTiles.begin(), ground->hiddenTiles.end(), [](std::uint8_t hidden) { return hidden > 1; })
        || static_cast<std::size_t>(std::count_if(ground->hiddenTiles.begin(), ground->hiddenTiles.end(),
        [](std::uint8_t hidden) { return hidden != 0; })) != ground->hiddenTileCount)
        return reject("WORLD_GROUND_HIDDEN_COUNT", "悬崖覆盖计数与覆盖表不一致。");
    const auto baseQuads = tiles - ground->hiddenTileCount;
    if (ground->hiddenTileCount ? ground->baseTiles.size() != baseQuads : !ground->baseTiles.empty())
        return reject("WORLD_GROUND_BASE_LAYOUT", "基础层原格映射与悬崖覆盖数量不一致。");
    if (ground->hiddenTileCount) {
        std::size_t base = 0;
        for (std::size_t tile = 0; tile < tiles; ++tile) if (!ground->hiddenTiles[tile]) {
            if (ground->baseTiles[base++] != tile)
                return reject("WORLD_GROUND_BASE_LAYOUT", "基础层原格映射必须按原格顺序完整覆盖所有未隐藏格。");
        }
    }
    const auto layered = baseQuads + ground->overlayTiles.size();
    const auto quads = layered + ground->waterTiles.size();
    if (ground->vertices.size() != quads * 4 || ground->indices.size() != quads * 6 ||
        ground->vertices.size() > Render::TerrainGroundMeshLimits{}.maxVertices || ground->sourceWidth != terrain_->header.width ||
        ground->sourceHeight != terrain_->header.height)
        return reject("WORLD_GROUND_LAYOUT", "地表网格尺寸、顶点数或索引数与当前地形不一致，或已超出预算。");
    // 基础层通过完整原格或显式 baseTiles 映射，过渡层对应 overlayTiles，坐标必须与原始高度网格一致；
    // 水面对应 waterTiles，XY 一致、Z 为水面高度。
    for (std::size_t quad = 0; quad < quads; ++quad) {
        const auto tile = quad < baseQuads ? (ground->hiddenTileCount ? ground->baseTiles[quad] : quad)
            : quad < layered ? ground->overlayTiles[quad - baseQuads]
            : ground->waterTiles[quad - layered];
        if (tile >= tiles)
            return reject("WORLD_GROUND_TILE_RANGE", "地表过渡层或水面引用的地形格越界。");
        for (std::size_t corner = 0; corner < 4; ++corner) {
            const auto& p = terrain_->vertices[terrain_->tiles[tile].vertices[corner]].position;
            const auto& v = ground->vertices[quad * 4 + corner];
            if (v.x != p[0] || v.y != p[1] || (quad < layered ? v.z != p[2] : !std::isfinite(v.z)))
                return reject("WORLD_GROUND_POSITION", "地表顶点坐标与当前地形不一致，或水面高度无效。");
        }
    }
    // 分组只能重排整 quad，不能换对角线、遗漏或重复显示面；拾取仍共用原始高度网格。
    std::vector<bool> seen(quads);
    for (std::size_t i = 0; i < ground->indices.size(); i += 6) {
        const auto first = ground->indices[i];
        const auto quad = first / 4;
        if (first % 4 != 0 || quad >= quads || seen[quad])
            return reject("WORLD_GROUND_FACE_LAYOUT", "地表索引重复、越界或包含应由悬崖遮住的面。");
        const std::array<std::uint32_t, 6> expected{first, first + 1, first + 2, first + 1, first + 3, first + 2};
        if (!std::equal(expected.begin(), expected.end(), ground->indices.begin() + static_cast<std::ptrdiff_t>(i)))
            return reject("WORLD_GROUND_FACE_ORDER", "地表三角形与拾取网格的对角线不一致。");
        seen[quad] = true;
    }
    if (!std::all_of(seen.begin(), seen.end(), [](bool visible) { return visible; }))
        return reject("WORLD_GROUND_FACE_LAYOUT", "地表索引没有完整覆盖全部基础层、过渡层和水面。");
    if (std::any_of(ground->textures.begin(), ground->textures.end(), [](const auto& p) {
        return !p || (p->columns != 4 && p->columns != 8) || !p->image.Valid() || p->additionalMips.size() > 11;
    })) return reject("WORLD_GROUND_TEXTURE", "地表图集尺寸、像素或 mip 层数不符合提交要求。");
    const auto previous = ground_;
    ground_ = std::move(ground);
    if (!SubmitMesh()) {
        ground_ = previous;
        return reject("WORLD_GROUND_PLUGIN_REJECTED", "渲染插件拒绝了地表贴图提交；保留此前地形，插件返回值见同次诊断。");
    }
    groundFailure_ = {};
    pressView_.reset(); ClearCoordinates(tr("地表视图更新中…"), true);
    return true;
}

bool WorldViewport::event(QEvent* event) {
    if (event->type() == QEvent::FocusOut || event->type() == QEvent::WindowDeactivate) {
        CancelTerrainStroke();
        CancelMove();
        pointerInside_ = false; dragButton_ = Qt::NoButton; dragging_ = false; pressView_.reset();
        hoverTimer_.stop(); ClearCoordinates(tr("悬停坐标已清除"));
    }
    if (event->type() == QEvent::ParentAboutToChange) DestroyRenderer();
    if (event->type() == QEvent::PlatformSurface &&
        static_cast<QPlatformSurfaceEvent*>(event)->surfaceEventType() == QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed) {
        DestroyRenderer();
    }
    const bool handled = QWidget::event(event);
    if (!destroying_ && event->type() == QEvent::WinIdChange)
        QTimer::singleShot(0, this, [this] { EnsureRenderer(); });
    if (event->type() == QEvent::DevicePixelRatioChange) ResizeRenderer();
    return handled;
}

void WorldViewport::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    EnsureRenderer();
    if (renderer_ && !failed_) ResizeRenderer();
}

void WorldViewport::hideEvent(QHideEvent* event) {
    CancelTerrainStroke();
    CancelMove();
    frameTimer_.stop(); hoverTimer_.stop(); dragButton_ = Qt::NoButton;
    dragging_ = false; pointerInside_ = false; pressView_.reset(); ClearCoordinates(tr("视口已隐藏，悬停坐标已清除"));
    QWidget::hideEvent(event);
}

void WorldViewport::ResizeRenderer() {
    pressView_.reset(); ClearCoordinates(tr("视口尺寸更新中，等待完成帧"), false, true);
    const auto pixels = PhysicalSize();
    if (pixels.width() > 4096 || pixels.height() > 4096) {
        frameTimer_.stop(); hoverTimer_.stop(); SetStatus(tr("视口物理尺寸超过 4096 × 4096，请缩小窗口"));
        ClearCoordinates(tr("物理视口尺寸超过上限，无法选取坐标"), true); return;
    }
    if (!renderer_) { EnsureRenderer(); return; }
    const auto result = api_->resize(renderer_, static_cast<std::uint32_t>(pixels.width()), static_cast<std::uint32_t>(pixels.height()));
    if (result != 0) {
        ReportPluginFailure("resize", result);
        frameTimer_.stop(); failed_ = true;
        SetStatus(tr("渲染窗口尺寸更新失败"));
        hoverTimer_.stop(); ClearCoordinates(tr("视口尺寸更新失败，无法选取坐标"), true);
    } else if (CanRender() && !failed_) frameTimer_.start();
    ScheduleHover();
}

void WorldViewport::resizeEvent(QResizeEvent* event) { QWidget::resizeEvent(event); ResizeRenderer(); }

void WorldViewport::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton || event->button() == Qt::MiddleButton || event->button() == Qt::RightButton) {
        if (placing_ && event->button() == Qt::RightButton) {
            placing_ = false; emit PlacingCancelled(); event->accept(); return;
        }
        setFocus();
        ++selectionGestureSerial_; selectionRetries_ = 0;
        selectionBox_ = false;
        selectionModifiers_ = hoverModifiers_ = event->modifiers();
        ClearSelectionOutline();
        pressCameraSerial_ = cameraSerial_;
        dragButton_ = event->button(); mousePosition_ = pressPosition_ = hoverPosition_ = event->position();
        pointerInside_ = true; dragging_ = dragButton_ != Qt::LeftButton; pressView_.reset(); pressModelPolicy_.reset();
        hoverTimer_.stop(); ClearCoordinates(tr("拖动时暂停悬停坐标"), false, true);
        CancelMove();
        if (!dragging_) {
            const auto frame = ReadPickingFrame();
            if (frame.Succeeded()) {
                pressView_ = frame.value->identity; pressRevision_ = revision_;
                if (!brushMode_ && !placing_ && animationScene_) {
                    const auto policy = ReadModelPickPolicy();
                    if (!policy.Succeeded()) { ClearCoordinates(PickDiagnostic(policy.diagnostics)); event->accept(); return; }
                    pressModelPolicy_ = *policy.value;
                }
                if (dragButton_ == Qt::LeftButton && brushMode_) {
                    const auto picked = PickAt(pressPosition_, *frame.value);
                    if (picked.Succeeded() && picked.value->hit) {
                        strokeActive_ = true;
                        strokeTerrain_ = terrain_;
                        strokeLastPickPixel_ = pressPosition_;
                        dragging_ = true;
                        emit TerrainStrokeStarted(picked.value->hit->position[0], picked.value->hit->position[1]);
                    } else ClearCoordinates(picked.Succeeded() ? tr("鼠标处未命中地形") : PickDiagnostic(picked.diagnostics));
                } else if (dragButton_ == Qt::LeftButton &&
                    !(event->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier)))
                    BeginMove(pressPosition_, *frame.value, event->modifiers().testFlag(Qt::AltModifier));
            } else ClearCoordinates(PickDiagnostic(frame.diagnostics), false, placing_);
        }
        event->accept(); return;
    }
    QWidget::mousePressEvent(event);
}

void WorldViewport::mouseMoveEvent(QMouseEvent* event) {
    pointerInside_ = true; hoverPosition_ = event->position(); hoverModifiers_ = event->modifiers();
    if (dragButton_ == Qt::NoButton || !(event->buttons() & dragButton_)) {
        CancelTerrainStroke();
        selectionBox_ = false;
        ClearSelectionOutline();
        dragButton_ = Qt::NoButton; dragging_ = false; pressView_.reset();
        if (event->buttons() == Qt::NoButton) {
            if (!terrain_ || terrainStride_ != 1 || !HasViewInfo()) {
                const auto frame = ReadPickingFrame();
                if (!frame.Succeeded()) ClearCoordinates(PickDiagnostic(frame.diagnostics));
            }
            ScheduleHover();
        } else { hoverTimer_.stop(); ClearCoordinates(tr("拖动时暂停悬停坐标"), false, true); }
        event->accept(); return;
    }
    if (strokeActive_) {
        mousePosition_ = event->position();
        const auto frame = ReadPickingFrame();
        if (frame.Succeeded()) {
            const auto picked = PickAt(event->position(), *frame.value, strokeTerrain_.get());
            if (picked.Succeeded() && picked.value->hit) {
                strokeLastPickPixel_ = event->position();
                emit TerrainStrokeMoved(picked.value->hit->position[0], picked.value->hit->position[1]);
            }
        }
        event->accept(); return;
    }
    if (brushMode_ && dragButton_ == Qt::LeftButton) { event->accept(); return; }
    if (!dragging_) {
        const auto displacement = event->position() - pressPosition_;
        if (std::abs(displacement.x()) + std::abs(displacement.y()) < QApplication::startDragDistance()) {
            event->accept(); return;
        }
        dragging_ = true;
        if (dragButton_ == Qt::LeftButton && !moving_ && !placing_ && !brushMode_
            && !selectionModifiers_.testFlag(Qt::AltModifier)) {
            selectionBox_ = true;
        } else pressView_.reset();
    }
    if (selectionBox_) {
        UpdateSelectionOutline(event->position());
        event->accept(); return;
    }
    if (moving_) {
        // 拖动选中对象：给出光标下的地面点；视图未完成时跳过本次，不改镜头。
        if (!moved_) {
            moved_ = true;
            if (rotating_) emit RotateStarted(pressGround_[0], pressGround_[1]);
            else emit MoveStarted(pressGround_[0], pressGround_[1]);
        }
        mousePosition_ = event->position();
        const auto frame = ReadPickingFrame();
        if (frame.Succeeded()) {
            const auto ground = PickAt(event->position(), *frame.value);
            if (ground.Succeeded() && ground.value->hit)
                if (rotating_) emit RotateDragged(ground.value->hit->position[0], ground.value->hit->position[1]);
                else emit MoveDragged(ground.value->hit->position[0], ground.value->hit->position[1]);
        }
        event->accept(); return;
    }
    const QPointF delta = event->position() - mousePosition_;
    mousePosition_ = event->position();
    const float dx = static_cast<float>(std::clamp(delta.x(), -10000.0, 10000.0));
    const float dy = static_cast<float>(std::clamp(delta.y(), -10000.0, 10000.0));
    // 原版 WE：Ctrl+右键拖动旋转视角；Alt+左键拖动空处同样旋转。其余右键/中键拖动为平移。
    const bool rotateCamera = dragButton_ == Qt::LeftButton
        || (dragButton_ == Qt::RightButton && selectionModifiers_.testFlag(Qt::ControlModifier));
    if (rotateCamera) {
        camera_.yawRadians = std::remainder(camera_.yawRadians - dx * 0.006f, 2.0f * std::numbers::pi_v<float>);
        camera_.pitchRadians = std::clamp(camera_.pitchRadians + dy * 0.006f, kMinimumPitch, kMaximumPitch);
    } else {
        const float scale = camera_.distance / static_cast<float>(std::max(height(), 1));
        const float sine = std::sin(camera_.yawRadians), cosine = std::cos(camera_.yawRadians);
        // 抓取式平移：地面跟着鼠标走。屏幕右方在地面上为 (−sin, cos)，屏幕下方为 (cos, sin)。
        camera_.targetX = std::clamp(camera_.targetX + (dx * sine - dy * cosine) * scale, -kMaximumCoordinate, kMaximumCoordinate);
        camera_.targetY = std::clamp(camera_.targetY + (-dx * cosine - dy * sine) * scale, -kMaximumCoordinate, kMaximumCoordinate);
    }
    SubmitCamera(); event->accept();
}

void WorldViewport::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == dragButton_) {
        hoverPosition_ = event->position();
        if (selectionBox_) {
            selectionBox_ = false;
            ClearSelectionOutline();
            FinishSelectionBox(event->position());
            dragButton_ = Qt::NoButton; dragging_ = false; pressView_.reset();
            ScheduleHover(); event->accept(); return;
        }
        if (strokeActive_) {
            if (event->position() != strokeLastPickPixel_) {
                const auto frame = ReadPickingFrame();
                if (frame.Succeeded()) {
                    const auto picked = PickAt(event->position(), *frame.value, strokeTerrain_.get());
                    if (picked.Succeeded() && picked.value->hit)
                        emit TerrainStrokeMoved(picked.value->hit->position[0], picked.value->hit->position[1]);
                }
            }
            strokeActive_ = false;
            strokeTerrain_.reset();
            emit TerrainStrokeFinished();
            dragButton_ = Qt::NoButton; dragging_ = false; pressView_.reset();
            ScheduleHover(); event->accept(); return;
        }
        if (moving_ && moved_) {
            const bool rotate = rotating_;
            rotating_ = false;
            moving_ = moved_ = false;
            if (rotate) emit RotateFinished();
            else emit MoveFinished();
            dragButton_ = Qt::NoButton; dragging_ = false; pressView_.reset();
            ScheduleHover(); event->accept(); return;
        }
        moving_ = rotating_ = false;
        const auto displacement = event->position() - pressPosition_;
        if (dragButton_ == Qt::LeftButton && !brushMode_ && !dragging_ &&
            std::abs(displacement.x()) + std::abs(displacement.y()) < QApplication::startDragDistance()) {
            if (placing_) FinishPlacement(event->position(), event->modifiers().testFlag(Qt::ControlModifier),
                selectionGestureSerial_, pressCameraSerial_, 0);
            else FinishClick(event->position(), event->modifiers());
        }
        dragButton_ = Qt::NoButton; dragging_ = false; pressView_.reset();
        ScheduleHover(); event->accept(); return;
    }
    QWidget::mouseReleaseEvent(event);
}

void WorldViewport::wheelEvent(QWheelEvent* event) {
    ++selectionGestureSerial_;
    pressView_.reset(); ClearCoordinates(tr("视图缩放中，等待完成帧"), false, true);
    const double steps = std::clamp(static_cast<double>(event->angleDelta().y()) / 120.0, -10.0, 10.0);
    camera_.distance = std::clamp(camera_.distance * static_cast<float>(std::exp(-steps * 0.15)), kMinimumDistance, kMaximumDistance);
    SubmitCamera(); event->accept();
}

void WorldViewport::leaveEvent(QEvent* event) {
    pointerInside_ = false; hoverTimer_.stop(); ClearCoordinates(tr("鼠标已离开地形视口"));
    QWidget::leaveEvent(event);
}

void WorldViewport::keyReleaseEvent(QKeyEvent* event) {
    if (placing_ && event->key() == Qt::Key_Control) {
        hoverModifiers_ &= ~Qt::ControlModifier; hoverView_.reset(); ScheduleHover();
    }
    QWidget::keyReleaseEvent(event);
}

} // namespace War3Map::Modules
