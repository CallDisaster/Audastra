#include "WorldScreenshotEncoder.h"
#include "War3MapCore/Render/FrameCapture.h"
#include "War3MapCore/Render/ModelAnimation.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <chrono>
#include <limits>
#include <memory>
#include <utility>

namespace War3Map::Modules {
namespace {
std::uint64_t NowNs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
WorldScreenshotEncodeResult Metadata(const WorldScreenshotEncodeInput& input, std::uint64_t queuedNs) {
    WorldScreenshotEncodeResult output;
    output.sample = input.sample; output.metadata = input.metadata;
    output.captureIndex = input.captureIndex; output.comparisonEpoch = input.comparisonEpoch; output.queuedNs = queuedNs;
    return output;
}
WorldScreenshotEncodeResult Encode(const WorldScreenshotEncodeInput& input, std::uint64_t queuedNs) {
    auto output = Metadata(input, queuedNs); output.workerStartedNs = NowNs();
    try {
        const auto width = input.sample.identity.physicalWidth, height = input.sample.identity.physicalHeight;
        constexpr auto maximumPixels = Render::kMaximumFrameCaptureBytes / 4;
        if (!width || !height || width > maximumPixels || height > maximumPixels / width
            || width > static_cast<std::uint32_t>(std::numeric_limits<int>::max() / 4)
            || height > static_cast<std::uint32_t>(std::numeric_limits<int>::max())
            || input.rgba.size() != static_cast<std::size_t>(width) * height * 4) {
            output.diagnostic = QStringLiteral("WORLD_CAPTURE_PIXELS_INVALID");
        } else {
            const auto hashStarted = NowNs();
            output.sample.pixelSha256 = QCryptographicHash::hash(
                QByteArrayView(reinterpret_cast<const char*>(input.rgba.data()), static_cast<qsizetype>(input.rgba.size())),
                QCryptographicHash::Sha256).toHex().toStdString();
            output.pixelHashNs = NowNs() - hashStarted;
            const auto encodeStarted = NowNs();
            QImage image(reinterpret_cast<const uchar*>(input.rgba.data()), static_cast<int>(width), static_cast<int>(height),
                static_cast<int>(width * 4), QImage::Format_RGBA8888);
            output.metadata.insert("pixel_sha256", QString::fromStdString(output.sample.pixelSha256));
            image.setText(QStringLiteral("Audastra.CompletedFrame"),
                QString::fromUtf8(QJsonDocument(output.metadata).toJson(QJsonDocument::Compact)));
            QBuffer buffer(&output.png);
            if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG")) {
                output.png.clear(); output.diagnostic = QStringLiteral("WORLD_CAPTURE_PNG_ENCODE_FAILED");
            }
            output.pngEncodeNs = NowNs() - encodeStarted;
        }
    } catch (...) {
        output.png.clear(); output.diagnostic = QStringLiteral("WORLD_CAPTURE_ENCODE_EXCEPTION");
    }
    if (!output.Succeeded()) output.sample.result = Render::FrameMeasurementResult::Rejected;
    output.workerFinishedNs = NowNs();
    return output;
}
} // namespace

WorldScreenshotEncodeInput MakeWorldScreenshotEncodeInput(Render::CapturedFrameRgba frame) {
    // 使用十进制字符串保存64位身份，避免JSON消费者按double解析时丢失精度。
    const auto number = [](std::uint64_t value) { return QString::number(value); };
    WorldScreenshotEncodeInput input;
    auto& identity = input.sample.identity;
    identity.viewportRevision = frame.documentRevision;
    // 没有主动画来源时保留默认值，元数据明确标为未知，不从视口序号猜地图修订。
    if (frame.animation) identity.mapEditRevision = frame.animation->documentRevision;
    identity.completedFrameNumber = frame.view.identity.frameCount;
    identity.surfaceEpoch = frame.view.identity.surfaceEpoch;
    identity.renderSceneEpoch = frame.view.identity.sceneEpoch;
    identity.viewEpoch = frame.view.identity.viewEpoch;
    identity.physicalWidth = frame.view.physicalWidth; identity.physicalHeight = frame.view.physicalHeight;
    identity.contentVersion = frame.skin ? frame.skin->contentVersion : 0;
    input.sample.completionBound = true;
    QJsonArray matrix;
    for (const auto value : frame.view.rowMajorViewProjection) matrix.append(value);
    const auto& camera = frame.camera;
    QJsonObject completion{{"document_revision", number(frame.documentRevision)},
        {"document_revision_space", "viewport_presentation"}, {"viewport_revision", number(frame.documentRevision)},
        {"map_edit_revision", frame.animation ? QJsonValue(number(frame.animation->documentRevision)) : QJsonValue(QJsonValue::Null)},
        {"map_edit_revision_source", frame.animation ? "accepted_main_animation" : "unknown"},
        {"frame_number", number(identity.completedFrameNumber)}, {"surface_epoch", number(identity.surfaceEpoch)},
        {"scene_epoch", number(identity.renderSceneEpoch)}, {"view_epoch", number(identity.viewEpoch)},
        {"width", static_cast<int>(identity.physicalWidth)}, {"height", static_cast<int>(identity.physicalHeight)},
        {"camera", QJsonObject{{"target_x", camera.targetX}, {"target_y", camera.targetY}, {"target_z", camera.targetZ},
            {"distance", camera.distance}, {"yaw_radians", camera.yawRadians}, {"pitch_radians", camera.pitchRadians}}},
        {"row_major_view_projection", matrix}, {"model_layers_revision", number(frame.modelLayersRevision)}};
    if (frame.terrainGrid) {
        const auto& terrain = *frame.terrainGrid; const auto& accepted = terrain.accepted;
        const auto layer = [&](const Render::TerrainGroundLayerCounts& value) {
            return QJsonObject{{"requested", number(value.requested)}, {"displayed", number(value.displayed)},
                {"omitted", number(value.omitted)}, {"no_contribution", number(value.noContribution)},
                {"omission_reason_bits", static_cast<int>(value.omissionReasons)},
                {"resource_admission_checked", value.resourceAdmissionChecked},
                {"texture_slots_before_admission", number(value.textureSlotsBeforeAdmission)}};
        };
        completion.insert("terrain", QJsonObject{{"representation", "shared-grid-v1"}, {"completion_bound", true},
            {"counts_meaning", "accepted-geometry-not-visible-pixel-contribution"},
            {"grid_width", static_cast<int>(terrain.sourceWidth)}, {"grid_height", static_cast<int>(terrain.sourceHeight)},
            {"grid_vertex_count", number(accepted.gridVertexCount)}, {"base_quad_count", number(accepted.baseQuadCount)},
            {"transition_vertex_count", number(accepted.transitionVertexCount)}, {"blight_index_count", number(accepted.blightIndexCount)},
            {"blight_batch_count", number(accepted.blightBatchCount)}, {"water_quad_count", number(accepted.waterQuadCount)},
            {"ground_texture_count", static_cast<int>(accepted.groundTextureCount)}, {"pass_bits", static_cast<int>(accepted.passBits)},
            {"hidden_base_tiles", number(terrain.hiddenTileCount)}, {"base", layer(terrain.budget.base)},
            {"transitions", layer(terrain.budget.transitions)}, {"blight", layer(terrain.budget.blight)}, {"water", layer(terrain.budget.water)}});
    }
    if (frame.skin) completion.insert("animation", QJsonObject{{"scene_version", number(frame.skin->sceneVersion)},
        {"content_version", number(frame.skin->contentVersion)}, {"frame_number", number(frame.skin->frameCount)},
        {"source_document_revision", frame.animation ? QJsonValue(number(frame.animation->documentRevision)) : QJsonValue(QJsonValue::Null)},
        {"source_document_revision_space", "animation_session"}});
    if (frame.presentation) completion.insert("presentation", QJsonObject{
        {"model_epoch", number(frame.presentation->modelEpoch)},
        {"revision", number(frame.presentation->presentationRevision)},
        {"instance_count", static_cast<int>(frame.presentation->instanceCount)},
        {"visible_instance_count", static_cast<int>(frame.presentation->visibleInstanceCount)},
        {"visible_particle_draw_count", static_cast<int>(frame.presentation->visibleParticleDrawCount)}});
    QJsonArray layers;
    for (std::size_t i = 0; i < frame.modelLayers.size(); ++i) {
        const auto& layer = frame.modelLayers[i];
        const auto source = i < frame.modelLayerFrames.size() ? frame.modelLayerFrames[i] : nullptr;
        layers.append(QJsonObject{{"id", number(layer.layerId)},
            {"revision", number(layer.revision)}, {"parent_model_epoch", number(layer.parentModelEpoch)},
            {"result", layer.result}, {"active", static_cast<int>(layer.skin.active)},
            {"upload_result", layer.skin.result},
            {"static_upload_count", number(layer.skin.staticUploadCount)},
            {"matrix_upload_count", number(layer.skin.matrixUploadCount)},
            {"scene_version", number(layer.skin.sceneVersion)}, {"content_version", number(layer.skin.contentVersion)},
            {"frame_number", number(layer.skin.frameCount)},
            {"source_document_revision", source ? QJsonValue(number(source->documentRevision)) : QJsonValue(QJsonValue::Null)},
            {"source_document_revision_space", "animation_session"}});
    }
    completion.insert("model_layers", layers);
    input.metadata = {{"capture_source", "renderer_completed_frame"}, {"completion", completion}};
    input.rgba = std::move(frame.rgba);
    return input;
}

bool WorldScreenshotEncoder::Busy() const noexcept { return active_.valid() || launchFailure_.has_value(); }
bool WorldScreenshotEncoder::Submit(WorldScreenshotEncodeInput input) {
    if (Busy()) return false;
    const auto queuedNs = NowNs();
    // 独占value拥有像素；只读shared owner也让线程创建失败保留原身份回执。
    auto owned = std::make_shared<const WorldScreenshotEncodeInput>(std::move(input));
    try {
        active_ = std::async(std::launch::async, [owned, queuedNs] { return Encode(*owned, queuedNs); });
    } catch (...) {
        launchFailure_ = Metadata(*owned, queuedNs);
        launchFailure_->diagnostic = QStringLiteral("WORLD_CAPTURE_WORKER_START_FAILED");
        launchFailure_->sample.result = Render::FrameMeasurementResult::Rejected;
        launchFailure_->workerFinishedNs = NowNs();
    }
    return true;
}
std::optional<WorldScreenshotEncodeResult> WorldScreenshotEncoder::Poll() {
    if (launchFailure_) return std::exchange(launchFailure_, std::nullopt);
    if (!active_.valid() || active_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return std::nullopt;
    return active_.get();
}
std::optional<WorldScreenshotEncodeResult> WorldScreenshotEncoder::Finish() {
    if (launchFailure_) return std::exchange(launchFailure_, std::nullopt);
    return active_.valid() ? std::optional(active_.get()) : std::nullopt;
}

} // namespace War3Map::Modules
