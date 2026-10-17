#include "WorldScreenshotEncoder.h"
#include "War3MapCore/Render/ModelAnimation.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>

using namespace War3Map;
using namespace War3Map::Modules;
namespace {
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
WorldScreenshotEncodeInput Fixture(std::uint64_t epoch) {
    WorldScreenshotEncodeInput input;
    input.sample.identity.sourceIdentity = "frozen-map-source";
    input.sample.identity.measurementEpoch = epoch;
    input.sample.identity.mapEditRevision = 17;
    input.sample.identity.completedFrameNumber = 83;
    input.sample.identity.contentVersion = 79;
    input.sample.identity.cameraFingerprint = 71;
    input.sample.identity.physicalWidth = 2; input.sample.identity.physicalHeight = 2;
    input.sample.monotonicNs = 61; input.sample.readbackNs = 59;
    input.sample.playing = true; input.sample.completionBound = true;
    input.sample.visibility = Render::FrameMeasurementVisibility::Visible;
    input.sample.visibleDynamicWitness = true;
    input.captureIndex = 7; input.comparisonEpoch = 5;
    input.metadata = {{"phase", "captured-before-edit"}, {"camera", QJsonObject{{"yaw", 1.25}}}};
    const std::array<std::uint8_t, 16> pixels{255,0,0,255, 0,255,0,255, 0,0,255,255, 255,255,0,255};
    input.rgba.assign(reinterpret_cast<const std::byte*>(pixels.data()), reinterpret_cast<const std::byte*>(pixels.data() + pixels.size()));
    return input;
}
void OwnsPixelsAndMetadataWithSingleSlotBackpressure() {
    WorldScreenshotEncoder encoder;
    auto input = Fixture(41); const auto original = input;
    Require(!encoder.Busy() && !encoder.Poll() && !encoder.Finish(), "empty worker must have no pending result");
    Require(encoder.Submit(input) && encoder.Busy(), "first owned capture must be accepted");
    // 即使worker此时已经结束，没有取回前也不得接受第二份RGBA。
    Require(!encoder.Submit(Fixture(99)), "busy worker must not accumulate a second capture");
    std::fill(input.rgba.begin(), input.rgba.end(), std::byte{0});
    input.metadata.insert("phase", "new-ui-phase"); input.sample.identity.measurementEpoch = 99;
    input.sample.identity.completedFrameNumber = 1000; input.sample.monotonicNs = 999;
    input.sample.playing = false; input.comparisonEpoch = 100;
    const auto result = encoder.Finish();
    Require(result && result->Succeeded() && !encoder.Busy(), "stop must drain the actual result");
    auto expectedMetadata = original.metadata;
    expectedMetadata.insert("pixel_sha256", QString::fromStdString(result->sample.pixelSha256));
    Require(result->sample.identity.measurementEpoch == 41 && result->sample.identity.mapEditRevision == 17
        && result->sample.identity.completedFrameNumber == 83 && result->sample.identity.contentVersion == 79
        && result->sample.identity.cameraFingerprint == 71 && result->sample.identity.sourceIdentity == "frozen-map-source"
        && result->sample.monotonicNs == 61 && result->sample.readbackNs == 59 && result->sample.playing
        && result->captureIndex == 7 && result->comparisonEpoch == 5 && result->metadata == expectedMetadata,
        "encoding result must retain acquisition identity/time/metadata rather than latest UI values");
    const auto hash = QCryptographicHash::hash(QByteArrayView(reinterpret_cast<const char*>(original.rgba.data()),
        static_cast<qsizetype>(original.rgba.size())), QCryptographicHash::Sha256).toHex().toStdString();
    Require(result->sample.pixelSha256 == hash, "SHA must describe original owned RGBA");
    Require(result->metadata.value("pixel_sha256").toString().toStdString() == hash,
        "metadata must bind the exact acquired RGBA hash");
    const auto image = QImage::fromData(result->png, "PNG").convertToFormat(QImage::Format_RGBA8888);
    Require(!image.isNull() && image.width() == 2 && image.height() == 2, "owned image must decode to original dimensions");
    for (int y = 0; y < 2; ++y)
        Require(std::memcmp(image.constScanLine(y), original.rgba.data() + y * 8, 8) == 0,
            "caller mutation must not affect encoded pixels");
    Require(result->queuedNs > 0 && result->workerStartedNs >= result->queuedNs
        && result->workerFinishedNs >= result->workerStartedNs && result->pngEncodeNs > 0 && result->pixelHashNs > 0,
        "queue/hash/encode timestamps must be separate from acquisition time");
    Require(!encoder.Poll() && !encoder.Finish(), "drained capture must not be delivered twice");
}
void CompletedFrameMetadataSurvivesPngRoundTrip() {
    auto pixels = Fixture(1).rgba;
    Render::CapturedFrameRgba frame;
    frame.documentRevision = 9007199254740993ull; // 超过double精确整数范围，不能序列化为浮点数。
    frame.view.identity = {31, 41, 51, 9007199254740995ull};
    frame.view.physicalWidth = frame.view.physicalHeight = 2;
    frame.view.rowMajorViewProjection[0] = 1.25f;
    frame.camera = {sizeof(War3RenderCameraV2), 10, 20, 30, 1400, 0.25f, 0.75f};
    frame.rgba = pixels;
    Render::TerrainGridCaptureReceipt terrain;
    terrain.sourceWidth = 3; terrain.sourceHeight = 2; terrain.hiddenTileCount = 1;
    terrain.accepted = {sizeof(terrain.accepted),1,
        {sizeof(War3RenderViewInfoV2),2,2,frame.view.identity.frameCount,frame.documentRevision,31,41,51},6,1,4,6,1,2,2,3};
    terrain.budget.base = {2,1,0,1}; terrain.budget.transitions = {3,1,2,0};
    terrain.budget.blight = {1,1,0,0}; terrain.budget.water = {4,2,1,1};
    frame.terrainGrid = terrain;
    auto animation = std::make_shared<Render::AnimationFrame>();
    animation->documentRevision = 32; animation->sceneVersion = 61; animation->contentVersion = 71;
    frame.animation = animation;
    frame.skin = War3RenderSkinStatusV2{sizeof(War3RenderSkinStatusV2), 0, 1, 0,
        frame.view.identity.frameCount, 61, 71, 1, 1};
    frame.modelLayersRevision = 83;
    War3RenderModelLayerStatusV2 layer{};
    layer.structSize = sizeof(layer); layer.layerId = 91; layer.revision = 101; layer.parentModelEpoch = 111;
    layer.skin = {sizeof(layer.skin), 0, 1, 0, frame.view.identity.frameCount, 121, 131, 1, 1};
    frame.modelLayers.push_back(layer);
    auto layerSource = std::make_shared<Render::AnimationFrame>();
    layerSource->documentRevision = 0; layerSource->sceneVersion = 121; layerSource->contentVersion = 131;
    frame.modelLayerFrames.push_back(layerSource);
    auto input = MakeWorldScreenshotEncodeInput(std::move(frame));
    Require(input.sample.completionBound && input.sample.identity.mapEditRevision == 32
        && input.sample.identity.viewportRevision == 9007199254740993ull
        && input.sample.identity.completedFrameNumber == 9007199254740995ull
        && input.sample.identity.surfaceEpoch == 31 && input.sample.identity.renderSceneEpoch == 41
        && input.sample.identity.viewEpoch == 51 && input.rgba == pixels,
        "common screenshot input must retain completed frame identity and pixels");
    const auto completion = input.metadata.value("completion").toObject();
    const auto grid = completion.value("terrain").toObject();
    Require(grid.value("representation").toString() == QStringLiteral("shared-grid-v1")
        && grid.value("completion_bound").toBool()
        && grid.value("counts_meaning").toString() == QStringLiteral("accepted-geometry-not-visible-pixel-contribution")
        && grid.value("grid_vertex_count").toString() == QStringLiteral("6")
        && grid.value("base_quad_count").toString() == QStringLiteral("1")
        && grid.value("water_quad_count").toString() == QStringLiteral("2")
        && grid.value("hidden_base_tiles").toString() == QStringLiteral("1")
        && grid.value("water").toObject().value("requested").toString() == QStringLiteral("4")
        && grid.value("water").toObject().value("displayed").toString() == QStringLiteral("2")
        && grid.value("water").toObject().value("omitted").toString() == QStringLiteral("1")
        && grid.value("water").toObject().value("no_contribution").toString() == QStringLiteral("1"),
        "terrain metadata must retain the frozen accepted counts and separate candidate omissions");
    Require(completion.value("document_revision").toString() == QStringLiteral("9007199254740993")
        && completion.value("document_revision_space").toString() == QStringLiteral("viewport_presentation")
        && completion.value("viewport_revision").toString() == QStringLiteral("9007199254740993")
        && completion.value("map_edit_revision").toString() == QStringLiteral("32")
        && completion.value("map_edit_revision_source").toString() == QStringLiteral("accepted_main_animation")
        && completion.value("animation").toObject().value("source_document_revision").toString() == QStringLiteral("32")
        && completion.value("frame_number").toString() == QStringLiteral("9007199254740995")
        && completion.value("camera").toObject().value("target_x").toDouble() == 10
        && completion.value("row_major_view_projection").toArray().first().toDouble() == 1.25
        && completion.value("model_layers").toArray().first().toObject().value("id").toString() == QStringLiteral("91"),
        "metadata must retain exact frame, camera, projection and additional layer identity");
    const auto layerMetadata = completion.value("model_layers").toArray().first().toObject();
    Require(layerMetadata.value("source_document_revision").toString() == QStringLiteral("0")
        && layerMetadata.value("source_document_revision_space").toString() == QStringLiteral("animation_session"),
        "layer revision zero is a real source revision, not the viewport or main animation revision");
    Render::CapturedFrameRgba staticFrame;
    staticFrame.documentRevision = 23;
    const auto noSource = MakeWorldScreenshotEncodeInput(std::move(staticFrame));
    Require(noSource.sample.identity.viewportRevision == 23 && noSource.sample.identity.mapEditRevision == 0
        && !noSource.metadata.value("completion").toObject().contains("terrain")
        && noSource.metadata.value("completion").toObject().value("map_edit_revision").isNull()
        && noSource.metadata.value("completion").toObject().value("map_edit_revision_source").toString() == QStringLiteral("unknown"),
        "a static frame without document-source evidence must not invent a map edit revision");
    WorldScreenshotEncoder encoder;
    Require(encoder.Submit(std::move(input)), "completed frame encoding must be accepted");
    const auto encoded = encoder.Finish();
    Require(encoded && encoded->Succeeded(), "completed frame must encode successfully");
    const auto image = QImage::fromData(encoded->png, "PNG");
    const auto embedded = QJsonDocument::fromJson(image.text(QStringLiteral("Audastra.CompletedFrame")).toUtf8()).object();
    Require(embedded == encoded->metadata && embedded.value("completion").toObject() == completion,
        "exported PNG must carry the same immutable receipt as the sidecar metadata");
    const auto rgba = image.convertToFormat(QImage::Format_RGBA8888);
    for (int y = 0; y < 2; ++y)
        Require(std::memcmp(rgba.constScanLine(y), pixels.data() + y * 8, 8) == 0,
            "embedding completion metadata must preserve acquired pixels");
}
void InvalidPixelsReturnFailureAndNextSessionStartsOnlyAfterDrain() {
    WorldScreenshotEncoder encoder;
    auto invalid = Fixture(200); invalid.rgba.pop_back();
    Require(encoder.Submit(std::move(invalid)), "invalid input must return an owned diagnostic result");
    Require(!encoder.Submit(Fixture(201)), "failed but uncollected result must retain the slot");
    const auto failed = encoder.Finish();
    Require(failed && !failed->Succeeded() && failed->png.isEmpty()
        && failed->diagnostic == QStringLiteral("WORLD_CAPTURE_PIXELS_INVALID")
        && failed->sample.result == Render::FrameMeasurementResult::Rejected
        && failed->sample.identity.measurementEpoch == 200 && !encoder.Busy(),
        "stop must collect a failure with the original session identity");
    auto oversized = Fixture(202);
    oversized.sample.identity.physicalWidth = std::numeric_limits<std::uint32_t>::max();
    oversized.sample.identity.physicalHeight = std::numeric_limits<std::uint32_t>::max();
    Require(encoder.Submit(std::move(oversized)), "bounded validation must accept ownership before diagnosis");
    const auto rejected = encoder.Finish();
    Require(rejected && !rejected->Succeeded() && rejected->diagnostic == QStringLiteral("WORLD_CAPTURE_PIXELS_INVALID"),
        "dimension overflow must reject before hashing or image allocation");
    Require(encoder.Submit(Fixture(300)), "new session may start after old result is drained");
    std::optional<WorldScreenshotEncodeResult> next;
    for (int attempt = 0; attempt < 1000 && !next; ++attempt) {
        next = encoder.Poll();
        if (!next) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    Require(next && next->Succeeded() && next->sample.identity.measurementEpoch == 300 && !encoder.Poll(),
        "old success/failure must not be relabeled or returned in a later session");
}
} // namespace
int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    try {
        OwnsPixelsAndMetadataWithSingleSlotBackpressure();
        InvalidPixelsReturnFailureAndNextSessionStartsOnlyAfterDrain();
        CompletedFrameMetadataSurvivesPngRoundTrip();
        std::cout << "world screenshot encoder checks passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
