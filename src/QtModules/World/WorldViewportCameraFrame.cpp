#include "WorldViewport.h"
#include "WorldViewportInternals.h"

#include <QThread>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <new>
#include <numbers>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace War3Map::Modules {
namespace {
bool ValidCamera(const War3RenderCameraV2& camera) {
    const auto coordinate = [](float value) { return std::isfinite(value) && std::abs(value) <= kMaximumCoordinate; };
    return camera.structSize == sizeof(camera) && coordinate(camera.targetX) && coordinate(camera.targetY) &&
        coordinate(camera.targetZ) && coordinate(camera.yawRadians) && std::isfinite(camera.distance) &&
        camera.distance >= kMinimumDistance && camera.distance <= kMaximumDistance &&
        std::isfinite(camera.pitchRadians) && camera.pitchRadians >= kMinimumPitch && camera.pitchRadians <= kMaximumPitch;
}
bool SameCamera(const War3RenderCameraV2& first, const War3RenderCameraV2& second) {
    return first.targetX == second.targetX && first.targetY == second.targetY && first.targetZ == second.targetZ &&
        first.distance == second.distance && first.yawRadians == second.yawRadians && first.pitchRadians == second.pitchRadians;
}
bool SameParticleFrame(const War3RenderParticleStatusV2& particles, const Render::AnimationFrame& frame,
    const War3RenderCameraV2& camera) {
    const auto actualCamera = Render::AnimationWorldCamera(camera);
    return particles.structSize >= sizeof(particles) && !particles.reserved && particles.dynamicUploadCount
        && particles.sceneVersion == frame.sceneVersion && particles.contentVersion == frame.contentVersion
        && particles.vertexCount == frame.particleVertices.size() && particles.drawCount == frame.particleDraws.size()
        && frame.worldCamera && frame.worldCamera->eye == actualCamera.eye && frame.worldCamera->up == actualCamera.up;
}
} // namespace

void WorldViewport::SubmitCamera() {
    ++cameraSerial_;
    pressView_.reset(); ClearCoordinates(tr("视图更新中，等待完成帧"), false, true);
    pickingSuspended_ = false;
    const auto result = renderer_ ? api_->setCamera(renderer_, &camera_) : 0;
    if (result != 0) {
        ReportPluginFailure("setCamera", result);
        pickingSuspended_ = true; hoverTimer_.stop();
        SetStatus(tr("渲染器拒绝了视角参数"));
        ClearCoordinates(tr("视角更新失败，无法选取坐标"), true);
    }
    ScheduleHover();
    if (!pickingSuspended_ && animationScene_)
        emit AnimationCameraChanged();
}

void WorldViewport::ResetCamera() {
    camera_ = homeCamera_; SubmitCamera();
}

Result<War3RenderCameraV2> WorldViewport::ReadCamera(std::uint64_t expectedRevision) const {
    if (QThread::currentThread() != thread()) return Result<War3RenderCameraV2>::Failure("World", "WORLD_CAMERA_THREAD",
        "视角读取必须由世界视口所属线程执行。");
    if (expectedRevision != revision_) return Result<War3RenderCameraV2>::Failure("World", "WORLD_CAMERA_REVISION",
        "地图修订已变化，当前视角未读取。");
    return Result<War3RenderCameraV2>::Success(camera_);
}

Status WorldViewport::CheckCameraChange(const War3RenderCameraV2& requested, std::uint64_t expectedRevision) const {
    if (QThread::currentThread() != thread()) return Status::Failure("World", "WORLD_CAMERA_THREAD",
        "视角命令必须由世界视口所属线程执行。");
    if (expectedRevision != revision_) return Status::Failure("World", "WORLD_CAMERA_REVISION",
        "地图修订已变化，视角命令未执行。");
    if (!ValidCamera(requested)) return Status::Failure("World", "WORLD_CAMERA_INVALID", "视角参数无效或超出范围。");
    if (strokeActive_ || moving_ || dragButton_ != Qt::NoButton)
        return Status::Failure("World", "WORLD_CAMERA_BUSY", "正在交互编辑，暂不能从服务层改动视角。");
    return Status::Ok();
}

Status WorldViewport::SetCamera(const War3RenderCameraV2& requested, std::uint64_t expectedRevision) {
    const auto check = CheckCameraChange(requested, expectedRevision);
    if (!check.Succeeded()) return check;
    auto candidate = requested;
    candidate.yawRadians = std::remainder(candidate.yawRadians, 2.0f * std::numbers::pi_v<float>);
    const auto previous = camera_;
    camera_ = candidate;
    SubmitCamera();
    if (pickingSuspended_) {
        camera_ = previous;
        return Status::Failure("World", "WORLD_CAMERA_REJECTED", "渲染器拒绝视角更新，原请求状态已恢复。");
    }
    return Status::Ok();
}

Result<Render::CapturedFrameRgba> WorldViewport::CaptureCompletedFrame(std::uint64_t expectedRevision,
    std::size_t maxBytes) const {
    const auto fail = [](std::string code, std::string message) {
        return Result<Render::CapturedFrameRgba>::Failure("World", std::move(code), std::move(message));
    };
    if (QThread::currentThread() != thread()) return fail("WORLD_CAPTURE_THREAD", "完成帧读取必须由世界视口所属线程执行。");
    if (expectedRevision != revision_) return fail("WORLD_CAPTURE_REVISION", "地图修订已变化，完成帧未读取。");
    if (maxBytes == 0 || maxBytes > Render::kMaximumFrameCaptureBytes)
        return fail("WORLD_CAPTURE_BUDGET", "完成帧字节上限无效或超过 64 MiB。");
    // 初始零发射是合法粒子帧；能力来自已接收场景，不能以空顶点冒充支持。
    const bool hasModelGeometry = models_ && HasModelSupport() && !models_->vertices.empty() &&
        models_->indices.size() >= 3 && !models_->layers.empty() && !models_->meshes.empty() &&
        !models_->models.empty() && !models_->instances.empty();
    const bool hasAnimatedGeometry = animationScene_ && animationFrame_ && HasAnimationSupport() &&
        ((!animationScene_->vertices.empty() && !animationFrame_->instances.empty()) ||
            (HasParticleSupport() && animationScene_->particlesEnabled && animationScene_->particleInstanceCount));
    if (!renderer_ || failed_ || destroying_ || !CanRender() ||
        (vertices_.empty() && !hasModelGeometry && !hasAnimatedGeometry) || pickingSuspended_)
        return fail("WORLD_CAPTURE_UNAVAILABLE", "世界视口尚未就绪，无法读取真实完成帧。");
    if (!api_ || api_->structSize < offsetof(War3RenderPluginApiV2, captureFrameRgba) + sizeof(api_->captureFrameRgba) ||
        !api_->captureFrameRgba || !HasViewInfo())
        return fail("WORLD_CAPTURE_UNSUPPORTED", "当前渲染插件缺少同帧图像、视图和相机回执。");
    if (editorVisibilityDirty_ || (HasEditorVisibilitySupport() && !editorPresentationTarget_))
        return fail("WORLD_CAPTURE_PRESENTATION_PENDING", "显隐尚未绑定当前模型或姿势，完成帧未读取。");
    const auto size = PhysicalSize();
    if (size.width() <= 0 || size.height() <= 0 || size.width() > 4096 || size.height() > 4096)
        return fail("WORLD_CAPTURE_SIZE", "物理视口尺寸超出截图范围。");
    const auto width = static_cast<std::uint32_t>(size.width()), height = static_cast<std::uint32_t>(size.height());
    const auto byteCount = static_cast<std::uint64_t>(width) * height * 4;
    if (byteCount > maxBytes) return fail("WORLD_CAPTURE_BUDGET", "物理视口 RGBA 超出本次字节上限。");
    Render::CapturedFrameRgba output;
    try { output.rgba.resize(static_cast<std::size_t>(byteCount)); }
    catch (const std::bad_alloc&) { return fail("WORLD_CAPTURE_ALLOCATION", "无法为完成帧分配有界 RGBA 缓冲。"); }
    catch (const std::length_error&) { return fail("WORLD_CAPTURE_ALLOCATION", "完成帧 RGBA 缓冲超出容器容量。"); }
    War3RenderCapturedFrameV2 receipt{};
    receipt.structSize = sizeof(receipt);
    if (HasModelLayerSupport()) {
        War3RenderModelLayersTargetV2 target{sizeof(target)};
        const auto targetResult = api_->getModelLayersTarget(renderer_, &target);
        if (targetResult != 0) ReportPluginFailure("getModelLayersTarget", targetResult);
        if (targetResult != 0 || target.structSize < sizeof(target)
            || !target.layersRevision || target.layerCount > WAR3_RENDER_MAX_MODEL_SCENE_LAYERS)
            return fail("WORLD_CAPTURE_LAYER_TARGET", "无法读取附加模型层的当前修订。");
        War3RenderLayeredCapturedFrameV2 layered{sizeof(layered)};
        const auto result = api_->captureLayeredFrameRgba(renderer_, target.layersRevision,
            output.rgba.data(), byteCount, &layered);
        if (result == 2) return fail("WORLD_CAPTURE_LAYER_STALE", "附加层、模型或相机在读回期间已被替代。");
        if (result != 0) {
            ReportPluginFailure("captureLayeredFrameRgba", result);
            return fail("WORLD_CAPTURE_LAYER_FAILED", "附加模型层没有成功安装的同次完成帧。");
        }
        if (layered.structSize < sizeof(layered) || layered.world.structSize < sizeof(layered.world)
            || layered.layersRevision != target.layersRevision
            || layered.layerCount != target.layerCount || layered.layerCount > WAR3_RENDER_MAX_MODEL_SCENE_LAYERS)
            return fail("WORLD_CAPTURE_LAYER_STALE", "附加层回执数量或修订不符。");
        receipt = layered.world.frame;
        output.modelLayersRevision = layered.layersRevision;
        output.modelLayers.assign(layered.layers, layered.layers + layered.layerCount);
        std::vector<std::uint64_t> receivedIds;
        for (const auto& layer : output.modelLayers) {
            if (layer.structSize < sizeof(layer) || layer.result != 0 || layer.parentModelEpoch != target.parentModelEpoch
                || !layer.layerId || !layer.revision || layer.skin.structSize < sizeof(layer.skin)
                || layer.skin.active != 1 || layer.skin.result || layer.skin.frameCount != receipt.capture.frameCount
                || std::find(receivedIds.begin(), receivedIds.end(), layer.layerId) != receivedIds.end())
                return fail("WORLD_CAPTURE_LAYER_STALE", "附加层身份与读回帧不符。");
            receivedIds.push_back(layer.layerId);
            const auto bridge = modelLayerBridges_.find(layer.layerId);
            const auto* binding = bridge == modelLayerBridges_.end() ? nullptr : &bridge->second;
            const auto hostFrame = binding && binding->renderer ? binding->renderer->Target() : nullptr;
            if (!hostFrame || !binding->scene || binding->renderer->Revision() != layer.revision
                || hostFrame->sceneVersion != layer.skin.sceneVersion || hostFrame->contentVersion != layer.skin.contentVersion
                || binding->scene->sceneVersion != hostFrame->sceneVersion
                || binding->scene->generation != binding->source.generation
                || binding->scene->sourceIdentity != binding->source.sourceIdentity
                || binding->scene->documentRevision != binding->source.documentRevision
                || hostFrame->documentRevision != binding->source.documentRevision)
                return fail("WORLD_CAPTURE_LAYER_STALE", "附加层读回无法绑定到当前实例与姿势帧。");
            if ((layer.particles.sceneVersion || !hostFrame->particleVertices.empty() || !hostFrame->particleDraws.empty())
                && !SameParticleFrame(layer.particles, *hostFrame, receipt.camera))
                return fail("WORLD_CAPTURE_LAYER_STALE", "附加层粒子与完成帧的内容、数量或相机不符。");
            output.modelLayerFrames.push_back(hostFrame);
        }
        if (editorPresentationTarget_) output.presentation = layered.world.presentation;
        if (animationFrame_) {
            const auto& skin = layered.world.skin;
            if (skin.structSize < sizeof(skin) || skin.active != 1 || skin.result || skin.sceneVersion != animationFrame_->sceneVersion
                || skin.contentVersion != animationFrame_->contentVersion || skin.frameCount != receipt.capture.frameCount)
                return fail("WORLD_CAPTURE_FRAME_STALE", "主世界动画与附加层读回不属于同一次完成帧。");
            output.skin = skin; output.animation = animationFrame_;
            if (animationScene_->particlesEnabled && animationScene_->particleInstanceCount) {
                if (!SameParticleFrame(layered.world.particles, *animationFrame_, receipt.camera))
                    return fail("WORLD_CAPTURE_FRAME_STALE", "主世界粒子与完成帧的内容、数量或相机不符。");
                output.particles = layered.world.particles;
            }
        }
    } else if (animationFrame_) {
        if (!animationRenderer_) return fail("WORLD_CAPTURE_UNSUPPORTED", "动画场景没有完成帧桥接。 ");
        auto captured = animationRenderer_->Capture(output.rgba, expectedRevision);
        if (!captured.Succeeded()) return {std::nullopt, std::move(captured.diagnostics)};
        receipt = captured.value->frame;
        output.skin = captured.value->skin;
        output.particles = captured.value->particles;
        output.presentation = captured.value->presentation;
        output.animation = animationFrame_;
    } else if (editorPresentationTarget_) {
        if (!HasEditorVisibilitySupport()) return fail("WORLD_CAPTURE_UNSUPPORTED", "渲染插件缺少编辑器显隐同次回执。");
        War3RenderEditorCapturedFrameV2 presented{}; presented.structSize = sizeof(presented);
        const auto result = api_->captureEditorPresentationFrameRgba(renderer_, editorPresentationTarget_->modelEpoch,
            editorPresentationRevision_, output.rgba.data(), byteCount, &presented);
        if (result == 2) return fail("WORLD_CAPTURE_STALE_OR_TIMEOUT", "显隐、模型或视图已被新状态取代，未取得指定完成帧。");
        if (result != 0) {
            ReportPluginFailure("captureEditorPresentationFrameRgba", result);
            return fail("WORLD_CAPTURE_FAILED", "渲染器未能读取显隐组合完成帧。");
        }
        if (presented.structSize < sizeof(presented)) return fail("WORLD_CAPTURE_FRAME_STALE", "显隐组合回执结构过短。");
        receipt = presented.frame; output.presentation = presented.presentation;
    } else {
        const auto result = api_->captureFrameRgba(renderer_, output.rgba.data(), byteCount, &receipt);
        if (result == 2) return fail("WORLD_CAPTURE_STALE_OR_TIMEOUT", "等待完成帧超时或场景、视角已被新状态替代。");
        if (result != 0) {
            ReportPluginFailure("captureFrameRgba", result);
            return fail("WORLD_CAPTURE_FAILED", "渲染器未能读取真实完成帧。");
        }
    }
    if (editorPresentationTarget_) {
        if (!output.presentation) return fail("WORLD_CAPTURE_PRESENTATION_STALE", "完成帧缺少已绑定的显隐身份。");
        const auto& presentation = *output.presentation;
        const auto& target = *editorPresentationTarget_;
        if (editorVisibilityDirty_ || presentation.structSize < sizeof(presentation) || presentation.reserved
            || presentation.mode != target.mode || presentation.modelEpoch != target.modelEpoch
            || presentation.presentationRevision != editorPresentationRevision_ || presentation.sceneVersion != target.sceneVersion
            || presentation.contentVersion != target.contentVersion || presentation.instanceCount != target.instanceCount
            || presentation.particleDrawCount != target.particleDrawCount || presentation.visibleInstanceCount > presentation.instanceCount
            || presentation.visibleParticleDrawCount > presentation.particleDrawCount || presentation.drawCallCount > 262144)
            return fail("WORLD_CAPTURE_PRESENTATION_STALE", "显隐、模型或姿势修订与同次完成帧不一致。");
    }
    const auto& capture = receipt.capture;
    const auto& view = receipt.view;
    if (receipt.structSize != sizeof(receipt) || capture.structSize != sizeof(capture) ||
        view.structSize != sizeof(view) || !ValidCamera(receipt.camera) ||
        capture.width != width || capture.height != height || view.width != width || view.height != height ||
        capture.width != view.width || capture.height != view.height ||
        capture.documentRevision != expectedRevision || view.documentRevision != expectedRevision ||
        capture.frameCount == 0 || capture.frameCount != view.frameCount || capture.ticket == 0 ||
        view.surfaceEpoch == 0 || view.sceneEpoch == 0 || view.viewEpoch == 0 ||
        !SameCamera(receipt.camera, camera_) || revision_ != expectedRevision || PhysicalSize() != size ||
        !std::all_of(std::begin(view.viewProjection), std::end(view.viewProjection), [](float value) { return std::isfinite(value); }))
        return fail("WORLD_CAPTURE_FRAME_STALE", "图像回执与当前地图、物理视口或相机不一致，截图已丢弃。");
    War3RenderViewInfoV2 current{};
    current.structSize = sizeof(current);
    const auto viewResult = api_->getViewInfo(renderer_, &current);
    // 2 是插件约定的首帧/场景/视角待完成，不是无因失败；保持旧的 STALE 回执。
    if (viewResult != 0 && viewResult != 2) ReportPluginFailure("getViewInfo", viewResult);
    if (viewResult != 0 || current.structSize != sizeof(current) ||
        current.documentRevision != expectedRevision || current.width != width || current.height != height ||
        current.frameCount < view.frameCount || current.surfaceEpoch != view.surfaceEpoch ||
        current.sceneEpoch != view.sceneEpoch || current.viewEpoch != view.viewEpoch)
        return fail("WORLD_CAPTURE_FRAME_STALE", "读回后视图 epoch 已变化，截图已丢弃。");
    if (vertices_.empty() && !output.animation) {
        War3RenderStatusV2 status{sizeof(status)};
        if (!api_->getStatus || api_->getStatus(renderer_, &status) != 0 || status.structSize != sizeof(status) ||
            status.result != 0 || status.deviceReady != 1 || status.width != width || status.height != height ||
            status.frameCount != capture.frameCount || status.documentRevision != expectedRevision ||
            // 模型上传失败时插件仍可能返回背景帧和 result=0；成功状态必须绑定到同一完成帧。
            !(std::string_view(status.message, sizeof(status.message)).starts_with("D3D11 WARP / Feature Level 11.0 / ") ||
                std::string_view(status.message, sizeof(status.message)).starts_with("D3D11 hardware / Feature Level 11.0 / ")))
            return fail("WORLD_CAPTURE_MODEL_UPLOAD", "模型未在本次完成帧成功上传，截图已丢弃。");
    }
    output.documentRevision = expectedRevision;
    output.view.identity = {view.surfaceEpoch, view.sceneEpoch, view.viewEpoch, view.frameCount};
    output.view.physicalWidth = width; output.view.physicalHeight = height;
    std::copy_n(view.viewProjection, output.view.rowMajorViewProjection.size(), output.view.rowMajorViewProjection.begin());
    output.camera = receipt.camera;
    return Result<Render::CapturedFrameRgba>::Success(std::move(output));
}

QJsonObject WorldViewport::SaveCamera() const {
    return {{"x", camera_.targetX}, {"y", camera_.targetY}, {"z", camera_.targetZ}, {"distance", camera_.distance},
        {"yaw", camera_.yawRadians}, {"pitch", camera_.pitchRadians}};
}

bool WorldViewport::RestoreCamera(const QJsonObject& state) {
    for (const auto* name : {"x", "y", "z", "distance", "yaw", "pitch"}) {
        const auto value = state.value(QLatin1String(name));
        if (!value.isDouble() || !std::isfinite(value.toDouble()) || std::abs(value.toDouble()) > kMaximumCoordinate) return false;
    }
    const float distance = static_cast<float>(state["distance"].toDouble());
    const float pitch = static_cast<float>(state["pitch"].toDouble());
    if (distance < kMinimumDistance || distance > kMaximumDistance || pitch < kMinimumPitch || pitch > kMaximumPitch) return false;
    const War3RenderCameraV2 camera{sizeof(War3RenderCameraV2), static_cast<float>(state["x"].toDouble()), static_cast<float>(state["y"].toDouble()),
        static_cast<float>(state["z"].toDouble()), distance,
        std::remainder(static_cast<float>(state["yaw"].toDouble()), 2.0f * std::numbers::pi_v<float>), pitch};
    return SetCamera(camera, revision_).Succeeded();
}

} // namespace War3Map::Modules
