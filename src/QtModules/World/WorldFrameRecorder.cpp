#include "WorldFrameRecorder.h"
#include "WorldModule.h"
#include "WorldViewport.h"
#include "WorldAnimationPlayer.h"
#include "WorldScreenshotEncoder.h"
#include "War3MapCore/Render/FrameMeasurement.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMatrix4x4>
#include <QPointer>
#include <QSaveFile>
#include <QScreen>
#include <QTimer>
#include <QVector3D>
#include <QVector4D>
#include <QWindow>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <deque>
#include <limits>

namespace War3Map::Modules {
namespace {
using namespace Render;
constexpr std::uint64_t kMaximumBytes = 1024ull * 1024 * 1024;
constexpr std::uint64_t kSummaryReserve = 1024ull * 1024;
constexpr std::uint64_t kCaptureIntervalNs = 2300000000ull;
constexpr std::uint64_t kMaximumDurationNs = 3600ull * 1000000000;
std::uint64_t NowNs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
QString Text(const std::string& value) { return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size())); }
QJsonValue Number(std::uint64_t value) { return QString::number(value); }
QJsonValue Optional(const std::optional<std::uint64_t>& value) { return value ? Number(*value) : QJsonValue(); }
QString FileSha(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    constexpr qint64 maximum = 512ll * 1024 * 1024;
    const auto expectedSize = file.size();
    if (expectedSize < 0 || expectedSize > maximum) return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    qint64 readBytes = 0;
    while (!file.atEnd()) {
        const auto chunk = file.read(512 * 1024);
        if (chunk.isEmpty() || readBytes + chunk.size() > maximum) return {};
        readBytes += chunk.size(); hash.addData(chunk);
    }
    if (readBytes != expectedSize || file.error() != QFile::NoError) return {};
    return QString::fromLatin1(hash.result().toHex());
}
std::uint64_t CameraHash(const War3RenderCameraV2& camera) {
    const std::array<float, 6> values{camera.targetX, camera.targetY, camera.targetZ,
        camera.distance, camera.yawRadians, camera.pitchRadians};
    return HashFrameMeasurementBytes(std::as_bytes(std::span(values)));
}
QJsonObject CameraJson(const War3RenderCameraV2& c) {
    return {{"target_x", c.targetX}, {"target_y", c.targetY}, {"target_z", c.targetZ},
        {"distance", c.distance}, {"yaw_radians", c.yawRadians}, {"pitch_radians", c.pitchRadians},
        {"fingerprint", Number(CameraHash(c))}};
}
QJsonObject IdentityJson(const FrameMeasurementIdentity& id) {
    return {{"source_identity", Text(id.sourceIdentity)}, {"map_edit_revision", Number(id.mapEditRevision)},
        {"viewport_identity", Number(id.viewportIdentity)}, {"viewport_revision", Number(id.viewportRevision)},
        {"measurement_epoch", Number(id.measurementEpoch)}, {"completed_frame", Number(id.completedFrameNumber)},
        {"content_version", Number(id.contentVersion)}, {"pose_version", Number(id.poseVersion)},
        {"camera_fingerprint", Number(id.cameraFingerprint)}, {"physical_width", static_cast<int>(id.physicalWidth)},
        {"physical_height", static_cast<int>(id.physicalHeight)}, {"surface_epoch", Number(id.surfaceEpoch)},
        {"render_scene_epoch", Number(id.renderSceneEpoch)}, {"view_epoch", Number(id.viewEpoch)}};
}
QString VisibilityName(FrameMeasurementVisibility visibility) {
    constexpr const char* names[]{"visible", "minimized", "occluded", "hidden", "unknown"};
    const auto index = static_cast<std::size_t>(visibility);
    return QString::fromLatin1(index < std::size(names) ? names[index] : "unknown");
}
QString ClassificationName(FrameScreenshotClassification classification) {
    constexpr const char* names[]{"dynamic", "stall", "paused", "static", "witness_unknown", "minimized",
        "occluded", "hidden", "visibility_unknown", "rejected", "stale", "identity_mismatch"};
    return QString::fromLatin1(names[static_cast<std::size_t>(classification)]);
}
QJsonObject TimingJson(const AnimationFrameTimings& t) {
    return {{"prepare_ns", Number(t.prepareNs)}, {"state_advance_ns", Number(t.stateAdvanceNs)},
        {"pose_evaluation_ns", Number(t.poseEvaluationNs)}, {"pose_packing_ns", Number(t.posePackingNs)},
        {"visibility_ns", Number(t.visibilityNs)}, {"visibility_nodes_visited", Number(t.visibilityNodesVisited)},
        {"visibility_bounds_tests", Number(t.visibilityBoundsTests)},
        {"particles_total_ns", Number(t.particlesTotalNs)}, {"particle_simulation_ns", Number(t.particleSimulationNs)},
        {"particle_geometry_ns", Number(t.particleGeometryNs)}, {"pick_bounds_ns", Number(t.pickBoundsNs)},
        {"fingerprint_ns", Number(t.fingerprintNs)}, {"candidate_instances", Number(t.candidateInstances)},
        {"eligible_pose_instances", Number(t.eligiblePoseInstances)}, {"pose_evaluation_requests", Number(t.poseEvaluationRequests)},
        {"particle_simulation_requests", Number(t.particleSimulationRequests)},
        {"particle_anchor_requests", Number(t.particleAnchorRequests)}, {"particle_anchor_node_visits", Number(t.particleAnchorNodeVisits)},
        {"particle_skipped_timeline_ms", Number(t.particleSkippedTimelineMs)},
        {"whole_scene_motion_hash_diagnostic", Number(t.outputMotionHash)},
        {"whole_scene_particle_state_hash_diagnostic", Number(t.particleStateHash)},
        {"min_elapsed_ms", Number(t.minElapsedMs)}, {"max_elapsed_ms", Number(t.maxElapsedMs)},
        {"min_global_ms", Number(t.minGlobalMs)}, {"max_global_ms", Number(t.maxGlobalMs)}};
}
QJsonObject AttemptJson(const WorldAnimationTiming& t) {
    return {{"sequence", Number(t.sequence)}, {"attempt_serial", Number(t.attemptSerial)},
        {"scene_version", Number(t.sceneVersion)}, {"content_version", Number(t.contentVersion)},
        {"previous_content_version", Number(t.previousContentVersion)}, {"generation", Number(t.generation)},
        {"map_edit_revision", Number(t.documentRevision)}, {"outcome", Text(std::string(WorldAnimationTimingOutcomeName(t.outcome)))},
        {"queued_ns", Number(t.queuedNs)}, {"worker_started_ns", Number(t.workerStartedNs)},
        {"worker_finished_ns", Number(t.workerFinishedNs)}, {"ui_started_ns", Number(t.uiStartedNs)},
        {"queue_wait_ns", Number(t.queueWaitNs)}, {"worker_total_ns", Number(t.workerTotalNs)},
        {"source_before_ns", Number(t.sourceBeforeNs)}, {"source_after_ns", Number(t.sourceAfterNs)},
        {"source_before_result", static_cast<int>(t.sourceBefore)}, {"source_after_result", static_cast<int>(t.sourceAfter)},
        {"ui_queue_wait_ns", Number(t.uiQueueWaitNs)}, {"ui_accept_ns", Number(t.uiAcceptNs)},
        {"changed_slots_ns", Number(t.changedSlotsNs)}, {"animated_instances", Number(t.animatedInstances)},
        {"retained_static_instances", Number(t.retainedStaticInstances)}, {"hidden_instances", Number(t.hiddenInstances)},
        {"budget_static_instances", Number(t.budgetStaticInstances)}, {"budget_particle_instances", Number(t.budgetParticleInstances)},
        {"offscreen_instances", Number(t.offscreenInstances)}, {"offscreen_pose_instances", Number(t.offscreenPoseInstances)},
        {"offscreen_particle_instances", Number(t.offscreenParticleInstances)}, {"visibility_unknown_instances", Number(t.visibilityUnknownInstances)},
        {"frozen_particle_instances", Number(t.frozenParticleInstances)},
        {"diagnostic_code", Text(t.diagnosticCode)}, {"frame_timings", TimingJson(t.frame)}};
}
QJsonObject InstanceDetailJson(const Render::AnimationInstanceDeltaTimings& t) {
    return {{"clone_ns", Number(t.cloneNs)}, {"identity_ns", Number(t.identityNs)},
        {"asset_ns", Number(t.assetNs)}, {"ordering_ns", Number(t.orderingNs)}, {"storage_ns", Number(t.storageNs)},
        {"prepared_source_verify_ns", Number(t.preparedSourceVerifyNs)}, {"prepared_geometry_apply_ns", Number(t.preparedGeometryApplyNs)},
        {"storage_layout_ns", Number(t.storageLayoutNs)}, {"storage_evaluator_ns", Number(t.storageEvaluatorNs)},
        {"storage_stand_ns", Number(t.storageStandNs)}, {"storage_particles_ns", Number(t.storageParticlesNs)},
        {"state_remap_ns", Number(t.stateRemapNs)}, {"particle_remap_ns", Number(t.particleRemapNs)},
        {"first_frame_ns", Number(t.firstFrameNs)}, {"rendered_remap_ns", Number(t.renderedRemapNs)},
        {"total_ns", Number(t.totalNs)}, {"prepared_asset_hit", t.preparedAssetHit},
        {"prepared_asset_miss_reason", Text(t.preparedAssetMissReason)}};
}
QJsonObject InstanceTimingJson(const WorldInstanceTiming& t) {
    QJsonArray commands;
    for (const auto& command : t.commands) commands.append(QJsonObject{
        {"command_identity", Number(command.commandIdentity)}, {"map_edit_revision", Number(command.documentRevision)},
        {"placements", Number(command.placements)}, {"retained_placements", Number(command.retainedPlacements)},
        {"input_started_ns", Number(command.inputStartedNs)},
        {"transaction_completed_ns", Number(command.transactionCompletedNs)}, {"capture_started_ns", Number(command.captureStartedNs)},
        {"capture_ns", Number(command.captureNs)}, {"registry_prepare_ns", Number(command.registryPrepareNs)},
        {"input_to_roster_observed_ns", t.accepted && command.inputStartedNs && t.finishedNs >= command.inputStartedNs
            ? Number(t.finishedNs - command.inputStartedNs) : QJsonValue{}}});
    QJsonObject completed;
    if (t.accepted && t.completed.structSize == sizeof(t.completed)) completed = {
        {"frame_count", Number(t.completed.view.frameCount)}, {"surface_epoch", Number(t.completed.view.surfaceEpoch)},
        {"scene_epoch", Number(t.completed.view.sceneEpoch)}, {"view_epoch", Number(t.completed.view.viewEpoch)},
        {"model_scene_version", Number(t.completed.skin.sceneVersion)}, {"content_version", Number(t.completed.skin.contentVersion)},
        {"presentation_revision", Number(t.completed.presentation.presentationRevision)},
        {"model_epoch", Number(t.completed.presentation.modelEpoch)}, {"camera", CameraJson(t.completed.camera)}};
    return {{"event", "instance_roster"}, {"sequence", Number(t.sequence)}, {"operation", Number(t.operation)},
        {"command_identity", Number(t.commandIdentity)}, {"generation", Number(t.generation)},
        {"edit_session_identity", Number(t.editSessionIdentity)}, {"map_edit_revision", Number(t.documentRevision)},
        {"input_started_ns", Number(t.inputStartedNs)}, {"transaction_completed_ns", Number(t.transactionCompletedNs)},
        {"capture_started_ns", Number(t.captureStartedNs)}, {"capture_ns", Number(t.captureNs)},
        {"registry_prepare_ns", Number(t.registryPrepareNs)}, {"source_snapshot_ns", Number(t.sourceSnapshotNs)},
        {"placement_refresh", QJsonObject{{"snapshot_ns", Number(t.refresh.snapshotNs)},
            {"auxiliary_ns", Number(t.refresh.auxiliaryNs)}, {"building_splats_ns", Number(t.refresh.buildingSplatsNs)},
            {"outliner_ns", Number(t.refresh.outlinerNs)}, {"stage_snapshot_ns", Number(t.refresh.stageSnapshotNs)},
            {"reused_snapshot", t.refresh.reusedSnapshot}, {"splats_deferred_to_roster", t.refresh.splatsDeferredToRoster},
            {"outliner_after_queue", t.refresh.outlinerAfterQueue}}},
        {"queued_ns", Number(t.queuedNs)}, {"catalog_capture_ns", Number(t.catalogCaptureNs)},
        {"catalog_worker_started_ns", Number(t.catalogWorkerStartedNs)}, {"source_open_ns", Number(t.sourceOpenNs)},
        {"source_hash_ns", Number(t.sourceHashNs)}, {"archive_open_ns", Number(t.archiveOpenNs)},
        {"source_verify_ns", Number(t.sourceVerifyNs)}, {"hashed_bytes", Number(t.hashedBytes)},
        {"verified_archive_reused", t.verifiedArchiveReused}, {"resource_reader_reused", t.resourceReaderReused},
        {"owned_assets_ns", Number(t.ownedAssetsNs)},
        {"owned_helpers", QJsonObject{{"mapping_ns", Number(t.helpers.mappingNs)}, {"cache_ns", Number(t.helpers.cacheNs)},
            {"classify_ns", Number(t.helpers.classifyNs)}, {"query_ns", Number(t.helpers.queryNs)},
            {"query_validate_ns", Number(t.helpers.queryValidateNs)}, {"query_row_copy_ns", Number(t.helpers.queryRowCopyNs)},
            {"query_canonical_ns", Number(t.helpers.queryCanonicalNs)}, {"query_hash_ns", Number(t.helpers.queryHashNs)},
            {"query_resource_ns", Number(t.helpers.queryResourceNs)}, {"query_footprint_ns", Number(t.helpers.queryFootprintNs)},
            {"asset_ns", Number(t.helpers.assetNs)}, {"verify_ns", Number(t.helpers.verifyNs)}, {"total_ns", Number(t.helpers.totalNs)},
            {"candidates", Number(t.helpers.candidateCount)}, {"helpers", Number(t.helpers.helperCount)},
            {"queries", Number(t.helpers.queryCount)}, {"resource_reads", Number(t.helpers.resourceReads)},
            {"resource_cache_hits", Number(t.helpers.resourceCacheHits)}, {"assets_created", Number(t.helpers.assetsCreated)},
            {"assets_reused", Number(t.helpers.assetsReused)}}},
        {"catalog_prepare_ns", Number(t.catalogPrepareNs)}, {"catalog_worker_finished_ns", Number(t.catalogWorkerFinishedNs)},
        {"catalog_detail", InstanceDetailJson(t.catalogDetail)}, {"rebase_detail", InstanceDetailJson(t.rebaseDetail)},
        {"formal_asset_prefetch", QJsonObject{{"requested_ns", Number(t.prefetchRequestedNs)},
            {"started_ns", Number(t.prefetchStartedNs)}, {"finished_ns", Number(t.prefetchFinishedNs)},
            {"state", Text(t.prefetchState)}, {"diagnostic", Text(t.prefetchDiagnostic)}, {"model", Text(t.prefetchModel)}}},
        {"first_frame_detail", TimingJson(t.firstFrameDetail)},
        {"rebase_started_ns", Number(t.rebaseStartedNs)}, {"rebase_ns", Number(t.rebaseNs)}, {"picking_ns", Number(t.pickingNs)},
        {"roster_queue_ns", Number(t.rosterQueueNs)}, {"submitted_ns", Number(t.submittedNs)}, {"finished_ns", Number(t.finishedNs)},
        {"additions", Number(t.additions)}, {"removals", Number(t.removals)}, {"edits", Number(t.edits)},
        {"roster_ticket", Number(t.rosterTicket)}, {"assets_changed", t.assetsChanged}, {"roster_completed", t.accepted},
        {"diagnostic_code", Text(t.diagnosticCode)}, {"completed", completed.isEmpty() ? QJsonValue{} : QJsonValue(completed)},
        {"commands", commands}};
}
QMatrix4x4 CameraProjection(const War3RenderCameraV2& c, const QSize& size) {
    const QVector3D target(c.targetX, c.targetY, c.targetZ);
    const QVector3D eye = target + QVector3D(c.distance * std::cos(c.pitchRadians) * std::cos(c.yawRadians),
        c.distance * std::cos(c.pitchRadians) * std::sin(c.yawRadians), c.distance * std::sin(c.pitchRadians));
    QMatrix4x4 view; view.lookAt(eye, target, QVector3D(0, 0, 1));
    QMatrix4x4 projection; projection.perspective(45.0f, static_cast<float>(size.width()) /
        static_cast<float>(std::max(1, size.height())), 1.0f, 1000000.0f);
    return projection * view; // XY 与后端 RH 45度透视一致；深度只做正面/裁剪资格判断。
}
bool Inside(const AnimationPickBounds& bounds, const QMatrix4x4& projection) {
    if (!bounds.available) return false;
    const auto& box = bounds.box;
    const auto point = projection * QVector4D(static_cast<float>((box[0] + box[3]) * 0.5),
        static_cast<float>((box[1] + box[4]) * 0.5), static_cast<float>((box[2] + box[5]) * 0.5), 1);
    if (!std::isfinite(point.w()) || point.w() <= 0) return false;
    const auto normalized = point / point.w();
    return std::isfinite(normalized.x()) && std::isfinite(normalized.y()) && std::isfinite(normalized.z())
        && std::abs(normalized.x()) < 1 && std::abs(normalized.y()) < 1 && std::abs(normalized.z()) <= 1;
}
bool ParticleInside(const AnimationFrame& frame, const AnimationParticleLink& particles, const QMatrix4x4& projection) {
    if (particles.hidden || particles.outsideView || particles.frozen || !particles.simulated || !particles.vertexCount || !particles.drawCount
        || particles.firstVertex >= frame.particleVertices.size()) return false;
    const auto end = std::min<std::size_t>(frame.particleVertices.size(),
        static_cast<std::size_t>(particles.firstVertex) + std::min<std::uint32_t>(particles.vertexCount, 12));
    for (std::size_t i = particles.firstVertex; i < end; ++i) {
        const auto& vertex = frame.particleVertices[i];
        if (vertex.color[3] <= 0) continue;
        const auto point = projection * QVector4D(vertex.position[0], vertex.position[1], vertex.position[2], 1);
        if (!std::isfinite(point.w()) || point.w() <= 0) continue;
        const auto normalized = point / point.w();
        if (std::isfinite(normalized.x()) && std::isfinite(normalized.y()) && std::isfinite(normalized.z())
            && std::abs(normalized.x()) < 1 && std::abs(normalized.y()) < 1 && std::abs(normalized.z()) <= 1) return true;
    }
    return false;
}
std::uint64_t HashWords(const std::vector<std::uint64_t>& words) {
    return HashFrameMeasurementBytes(std::as_bytes(std::span(words)));
}
} // namespace

struct WorldFrameRecorder::State {
    explicit State(WorldModule& module) : world(module), worldGuard(&module) {}
    struct Candidate { std::uint32_t request = 0; std::size_t pose = SIZE_MAX; std::size_t particle = SIZE_MAX; };
    struct Accepted {
        std::uint64_t scene = 0, content = 0, mapRevision = 0, generation = 0, cameraHash = 0, poseVersion = 0;
        std::uint64_t presentationRevision = 0, modelEpoch = 0;
        std::string sourceIdentity;
        QSize physical;
        bool playing = false, witness = false;
        std::optional<std::uint64_t> poseHash, particleHash, witnessHash;
        QJsonArray witnesses;
        QJsonObject simulationView, visibilityCounts;
    };
    WorldModule& world;
    QPointer<WorldModule> worldGuard;
    QPointer<WorldViewport> viewport;
    QMetaObject::Connection acceptedConnection;
    QTimer timer;
    QFile frames, captures, attempts;
    FrameMeasurementAnalyzer analyzer;
    WorldScreenshotEncoder encoder;
    std::optional<FrameScreenshotSample> pendingCaptureComparison;
    std::uint64_t captureComparisonEpoch = 0;
    std::deque<Accepted> accepted;
    std::vector<Candidate> selected;
    std::uint64_t selectedScene = 0, selectedCamera = 0;
    QSize selectedPhysical;
    QString directory, sourcePath, sourceBefore, sourceAfter, phase = QStringLiteral("基线");
    QJsonArray phaseEvents;
    War3RenderFrameMetricsPageV2 device{};
    std::uint64_t cursor = 0, rendererEpoch = 0, cpuCursor = 0, startedNs = 0;
    std::uint64_t instanceCursor = 0;
    std::uint64_t lastMeasuredCpuSequence = 0, witnessConfirmedSinceNs = 0, unobscuredSinceNs = 0;
    std::uint64_t nextCaptureNs = 0, nextStatusNs = 0, bytesWritten = 0, captureIndex = 0;
    std::uint64_t endedNs = 0, statusNs = 0, statusFrames = 0;
    std::uint64_t metadataMisses = 0, cpuLost = 0, phaseEventsOmitted = 0, screenshotsFailed = 0, screenshotsSkipped = 0;
    std::optional<FrameMeasurementVisibility> lastVisibility;
    bool running = false, background = false, witnessConfirmed = false, unobscured = false, writeFailed = false;
    QString lastFailure;
    void ResetComparisons() {
        ++captureComparisonEpoch; pendingCaptureComparison.reset(); analyzer.ResetComparisons();
    }
    void CheckCaptureInterval(const FrameMeasurementSample& next) {
        if (!pendingCaptureComparison || next.monotonicNs <= pendingCaptureComparison->monotonicNs) return;
        const auto& original = *pendingCaptureComparison;
        // PNG返回前若中间发生暂停/遮挡/镜头或来源变化，不能由较早截图重新接上停滞比较。
        // 像素和原拍摄身份仍保留；仅把这份截图的比较资格标为过期。
        if (original.visibility != FrameMeasurementVisibility::Visible || !original.playing
            || !original.visibleDynamicWitness.value_or(false)) return;
        const auto& a = original.identity; const auto& b = next.identity;
        if (next.result != FrameMeasurementResult::Completed || next.visibility != FrameMeasurementVisibility::Visible
            || !next.playing || !next.visibleDynamicWitness.value_or(false)
            || a.sourceIdentity != b.sourceIdentity || a.measurementEpoch != b.measurementEpoch
            || a.mapEditRevision != b.mapEditRevision || a.viewportIdentity != b.viewportIdentity
            || a.viewportRevision != b.viewportRevision || a.surfaceEpoch != b.surfaceEpoch
            || a.renderSceneEpoch != b.renderSceneEpoch || a.viewEpoch != b.viewEpoch
            || a.cameraFingerprint != b.cameraFingerprint || a.physicalWidth != b.physicalWidth
            || a.physicalHeight != b.physicalHeight) {
            ++captureComparisonEpoch; pendingCaptureComparison.reset();
        }
    }
    bool Write(QFile& file, const QJsonObject& object) {
        if (writeFailed) return false;
        const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
        if (bytesWritten + static_cast<std::uint64_t>(bytes.size()) > kMaximumBytes - kSummaryReserve
            || file.write(bytes) != bytes.size()) {
            writeFailed = true; lastFailure = QStringLiteral("记录预算或磁盘写入失败"); return false;
        }
        bytesWritten += static_cast<std::uint64_t>(bytes.size()); return true;
    }
    const Accepted* Find(std::uint64_t scene, std::uint64_t content) const {
        const auto found = std::find_if(accepted.rbegin(), accepted.rend(), [&](const auto& item) {
            return item.scene == scene && item.content == content;
        });
        return found == accepted.rend() ? nullptr : &*found;
    }
    FrameMeasurementVisibility Visibility(std::uint32_t flags) const {
        if (!viewport) return FrameMeasurementVisibility::Hidden;
        if (viewport->window()->isMinimized()) return FrameMeasurementVisibility::Minimized;
        if (!viewport->isVisible()) return FrameMeasurementVisibility::Hidden;
        if ((flags & WAR3_RENDER_FRAME_METRICS_PRESENT_OCCLUDED) != 0) return FrameMeasurementVisibility::Occluded;
        if (background || !unobscured || (flags & WAR3_RENDER_FRAME_METRICS_WINDOW_ATTACHED) == 0)
            return FrameMeasurementVisibility::Unknown;
        return FrameMeasurementVisibility::Visible;
    }
    FrameMeasurementIdentity Identity(const War3RenderViewInfoV2& view, const War3RenderCameraV2& camera,
        std::uint64_t scene, std::uint64_t content) const {
        FrameMeasurementIdentity id;
        const auto* meta = Find(scene, content);
        id.sourceIdentity = meta ? meta->sourceIdentity : sourceBefore.toStdString();
        id.mapEditRevision = meta ? meta->mapRevision : 0;
        id.viewportIdentity = rendererEpoch; id.viewportRevision = view.documentRevision;
        id.measurementEpoch = startedNs; id.completedFrameNumber = view.frameCount;
        id.surfaceEpoch = view.surfaceEpoch; id.renderSceneEpoch = view.sceneEpoch; id.viewEpoch = view.viewEpoch;
        id.contentVersion = content; id.poseVersion = meta ? meta->poseVersion : 0;
        id.cameraFingerprint = CameraHash(camera); id.physicalWidth = view.width; id.physicalHeight = view.height;
        return id;
    }
};

WorldFrameRecorder::WorldFrameRecorder(WorldModule& world, QObject* parent)
    : QObject(parent), state_(std::make_unique<State>(world)) {
    state_->timer.setInterval(20);
    connect(&state_->timer, &QTimer::timeout, this, &WorldFrameRecorder::Tick);
}
WorldFrameRecorder::~WorldFrameRecorder() { if (Running()) Stop(QStringLiteral("窗口关闭")); }
bool WorldFrameRecorder::Running() const noexcept { return state_->running; }
QString WorldFrameRecorder::OutputDirectory() const { return state_->directory; }
void WorldFrameRecorder::SetWitnessConfirmed(bool confirmed) {
    if (Running() && state_->witnessConfirmed != confirmed) state_->ResetComparisons();
    state_->witnessConfirmed = confirmed; state_->witnessConfirmedSinceNs = NowNs();
}
void WorldFrameRecorder::SetUnobscured(bool unobscured) {
    if (Running() && state_->unobscured != unobscured) state_->ResetComparisons();
    state_->unobscured = unobscured; state_->unobscuredSinceNs = NowNs();
}
void WorldFrameRecorder::SetPhase(const QString& phase) {
    state_->phase = phase.left(128);
    if (!Running()) return;
    if (state_->phaseEvents.size() < 512) state_->phaseEvents.append(QJsonObject{{"phase", state_->phase}, {"steady_ns", Number(NowNs())}});
    else ++state_->phaseEventsOmitted;
}

bool WorldFrameRecorder::Start(const QString& newOutputDir, const QString& sourceMapPath, bool background) {
    if (Running()) { emit StatusChanged(tr("已有测量正在运行")); return false; }
    if (state_->encoder.Busy()) { emit StatusChanged(tr("上一轮截图尚未收取，不能混入新的测量目录")); return false; }
    auto* viewport = state_->worldGuard ? state_->world.Viewport() : nullptr;
    const auto source = QFileInfo(sourceMapPath);
    const auto output = QFileInfo(newOutputDir);
    if (!viewport || !source.isFile() || newOutputDir.isEmpty() || output.exists()) {
        emit StatusChanged(tr("测量需要已创建的世界视口、源地图及不存在的新输出目录")); return false;
    }
    const auto sourceSha = FileSha(source.absoluteFilePath());
    if (sourceSha.isEmpty() || !QDir().mkpath(output.absoluteFilePath() + QStringLiteral("/screenshots"))) {
        emit StatusChanged(tr("无法读取地图身份或创建测量目录")); return false;
    }
    const bool witness = state_->witnessConfirmed, unobscured = state_->unobscured;
    const auto phase = state_->phase;
    if (state_->viewport) state_->viewport->SetMeasurementEnabled(false);
    disconnect(state_->acceptedConnection);
    state_ = std::make_unique<State>(state_->world);
    state_->witnessConfirmed = witness; state_->unobscured = unobscured; state_->phase = phase;
    state_->witnessConfirmedSinceNs = NowNs(); state_->unobscuredSinceNs = state_->witnessConfirmedSinceNs;
    state_->directory = output.absoluteFilePath(); state_->sourcePath = source.absoluteFilePath();
    state_->sourceBefore = sourceSha; state_->background = background; state_->viewport = viewport;
    state_->frames.setFileName(state_->directory + QStringLiteral("/frames.jsonl"));
    state_->captures.setFileName(state_->directory + QStringLiteral("/captures.jsonl"));
    state_->attempts.setFileName(state_->directory + QStringLiteral("/cpu-attempts.jsonl"));
    if (!state_->frames.open(QIODevice::WriteOnly | QIODevice::NewOnly)
        || !state_->captures.open(QIODevice::WriteOnly | QIODevice::NewOnly)
        || !state_->attempts.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        emit StatusChanged(tr("无法独占创建测量日志；已创建的证据保留")); return false;
    }
    state_->acceptedConnection = connect(viewport, &WorldViewport::MeasurementFrameAccepted, this, &WorldFrameRecorder::ObserveAccepted);
    state_->rendererEpoch = viewport->MeasurementRendererEpoch();
    if (const auto page = viewport->ReadFrameMetrics(0); page.Succeeded()) {
        state_->device = page.value->page; state_->cursor = page.value->page.latestFrame;
    }
    if (const auto* player = state_->world.AnimationPlayer()) {
        state_->cpuCursor = player->LatestTimingSequence();
        const auto instances = player->InstanceTimingHistory();
        if (!instances.empty()) state_->instanceCursor = instances.back().sequence;
    }
    state_->startedNs = NowNs(); state_->nextCaptureNs = state_->startedNs + kCaptureIntervalNs;
    state_->statusNs = state_->startedNs;
    state_->running = true; viewport->SetMeasurementEnabled(true); ObserveAccepted();
    state_->timer.setInterval(20); connect(&state_->timer, &QTimer::timeout, this, &WorldFrameRecorder::Tick);
    SetPhase(state_->phase); state_->timer.start(); WriteSummary(QStringLiteral("测量进行中"));
    emit StatusChanged(tr("已开始被动完成帧测量，每 2.3 秒读取截图；不操作地图或相机")); return true;
}

void WorldFrameRecorder::ObserveAccepted() {
    if (!Running() || !state_->viewport || !state_->worldGuard) return;
    auto* player = state_->world.AnimationPlayer();
    const auto scene = player ? player->Scene() : nullptr;
    const auto frame = state_->viewport->AnimationTarget();
    if (!scene || !frame || scene->sceneVersion != frame->sceneVersion) return;
    // worker 的视图与当前 UI 可能不同，必须绑定 Prepare 实際使用的视图，不能替旧帧盖上新相机身份。
    const auto camera = frame->worldView ? frame->worldView->camera : state_->viewport->Camera();
    const auto cameraHash = CameraHash(camera);
    const auto physical = frame->worldView ? QSize(static_cast<int>(frame->worldView->physicalWidth),
        static_cast<int>(frame->worldView->physicalHeight)) : state_->viewport->MeasurementPhysicalSize();
    const auto projection = CameraProjection(camera, physical);
    auto usablePose = [&](std::size_t i) {
        if (i >= frame->poses.size() || i >= frame->pickBounds.size()) return false;
        const auto& pose = frame->poses[i];
        return pose.requestIndex < frame->states.size() && frame->states[pose.requestIndex].playing
            && pose.animated && !pose.hidden && !pose.outsideView && !pose.noVisibleGeometry && !pose.staticForBudget
            && state_->viewport->MeasurementRequestVisible(pose.requestIndex) && Inside(frame->pickBounds[i], projection);
    };
    auto usableParticle = [&](std::size_t i) {
        if (i >= frame->particleLinks.size()) return false;
        const auto& particles = frame->particleLinks[i];
        return particles.requestIndex < frame->states.size() && frame->states[particles.requestIndex].playing
            && state_->viewport->MeasurementRequestVisible(particles.requestIndex) && ParticleInside(*frame, particles, projection);
    };
    if (state_->selected.empty() || state_->selectedScene != frame->sceneVersion || state_->selectedCamera != cameraHash || state_->selectedPhysical != physical) {
        state_->selected.clear();
        for (std::size_t i = 0; i < frame->poses.size() && state_->selected.size() < 8; ++i) {
            if (!usablePose(i)) continue;
            const auto request = frame->poses[i].requestIndex;
            const auto particle = std::find_if(frame->particleLinks.begin(), frame->particleLinks.end(), [&](const auto& p) { return p.requestIndex == request; });
            state_->selected.push_back({request, i, particle == frame->particleLinks.end() ? SIZE_MAX : static_cast<std::size_t>(particle - frame->particleLinks.begin())});
        }
        for (std::size_t i = 0; i < frame->particleLinks.size() && state_->selected.size() < 8; ++i) {
            if (!usableParticle(i)) continue;
            const auto request = frame->particleLinks[i].requestIndex;
            if (std::none_of(state_->selected.begin(), state_->selected.end(), [&](const auto& c) { return c.request == request; }))
                state_->selected.push_back({request, SIZE_MAX, i});
        }
        state_->selectedScene = frame->sceneVersion; state_->selectedCamera = cameraHash; state_->selectedPhysical = physical;
    }
    State::Accepted meta;
    meta.scene = frame->sceneVersion; meta.content = frame->contentVersion; meta.mapRevision = frame->documentRevision;
    meta.generation = scene->generation; meta.cameraHash = cameraHash; meta.physical = physical;
    meta.sourceIdentity = scene->sourceIdentity; meta.playing = player->Playing();
    meta.presentationRevision = state_->viewport->MeasurementPresentationRevision();
    meta.modelEpoch = state_->viewport->MeasurementModelEpoch();
    meta.simulationView={{"known",frame->worldView.has_value()},{"camera",CameraJson(camera)},
        {"physical_width",physical.width()},{"physical_height",physical.height()}};
    meta.visibilityCounts={{"offscreen_instances",Number(frame->offscreenInstances)},
        {"offscreen_pose_instances",Number(frame->offscreenPoseInstances)},{"offscreen_particle_instances",Number(frame->offscreenParticleInstances)},
        {"visibility_unknown_instances",Number(frame->visibilityUnknownInstances)},
        {"frozen_particle_instances",Number(static_cast<std::uint64_t>(std::count_if(frame->particleLinks.begin(),frame->particleLinks.end(),
            [](const auto& link) { return link.frozen; })))} };
    std::vector<std::uint64_t> poseWords, particleWords, identities;
    for (const auto& candidate : state_->selected) {
        const bool showPose = usablePose(candidate.pose) && frame->poses[candidate.pose].requestIndex == candidate.request;
        const bool showParticle = usableParticle(candidate.particle) && frame->particleLinks[candidate.particle].requestIndex == candidate.request;
        if (!showPose && !showParticle) continue;
        const auto identity = showPose ? frame->poses[candidate.pose].identity : frame->particleLinks[candidate.particle].identity;
        const auto& instance = frame->states[candidate.request];
        identities.insert(identities.end(), {identity.value, identity.generation, showPose ? 1ull : 0ull, showParticle ? 1ull : 0ull});
        QJsonObject witness{{"request_index", static_cast<int>(candidate.request)}, {"instance_id", Number(identity.value)},
            {"instance_generation", Number(identity.generation)},
            {"elapsed_ms", Number(instance.elapsedMs)}, {"global_ms", Number(instance.globalMs)}};
        if (showPose) {
            const auto& pose = frame->poses[candidate.pose];
            poseWords.insert(poseWords.end(), {pose.identity.value, pose.identity.generation, pose.matrixHash, pose.alphaHash, pose.colorHash});
            meta.poseVersion = std::max(meta.poseVersion, pose.poseVersion);
            witness.insert("pose_version", Number(pose.poseVersion)); witness.insert("matrix_hash", Number(pose.matrixHash));
            witness.insert("alpha_hash", Number(pose.alphaHash)); witness.insert("color_hash", Number(pose.colorHash));
        }
        if (showParticle) {
            const auto& particles = frame->particleLinks[candidate.particle];
            particleWords.insert(particleWords.end(), {particles.identity.value, particles.identity.generation, particles.geometryHash});
            witness.insert("particle_geometry_hash", Number(particles.geometryHash));
            witness.insert("particle_count", static_cast<int>(particles.particleCount));
            witness.insert("particle_draw_count", static_cast<int>(particles.drawCount));
        }
        meta.witnesses.append(witness);
    }
    if (!poseWords.empty()) meta.poseHash = HashWords(poseWords);
    if (!particleWords.empty()) meta.particleHash = HashWords(particleWords);
    if (!identities.empty()) meta.witnessHash = HashWords(identities);
    meta.witness = !meta.witnesses.empty();
    state_->accepted.push_back(std::move(meta));
    if (state_->accepted.size() > 512) state_->accepted.pop_front();
}

void WorldFrameRecorder::Drain() {
    if (!Running() || !state_->viewport || !state_->worldGuard) return;
    auto* viewport = state_->viewport.data();
    const auto epoch = viewport->MeasurementRendererEpoch();
    if (epoch != state_->rendererEpoch) { state_->rendererEpoch = epoch; state_->cursor = 0; }
    for (unsigned pageIndex = 0; pageIndex < 8; ++pageIndex) {
        const auto result = viewport->ReadFrameMetrics(state_->cursor);
        if (!result.Succeeded()) {
            const auto code = result.diagnostics.empty() ? QStringLiteral("FRAME_METRICS_UNAVAILABLE") : Text(result.diagnostics.front().code);
            if (state_->lastFailure != code) {
                state_->lastFailure = code; state_->Write(state_->frames, {{"event", "metrics_unavailable"}, {"code", code}, {"steady_ns", Number(NowNs())}});
            }
            break;
        }
        state_->device = result.value->page;
        if (state_->device.lostBeforeCount) {
            ++state_->captureComparisonEpoch; state_->pendingCaptureComparison.reset();
            state_->analyzer.AddLostRecords(state_->device.lostBeforeCount);
            state_->Write(state_->frames, {{"event", "ring_gap"}, {"lost_records", Number(state_->device.lostBeforeCount)}, {"after_frame", Number(state_->cursor)}});
        }
        if (result.value->records.empty()) break;
        for (const auto& record : result.value->records) {
            const auto& completed = record.completed; const auto& view = completed.view;
            const auto* meta = state_->Find(completed.skin.sceneVersion, completed.skin.contentVersion);
            FrameMeasurementSample sample;
            sample.identity = state_->Identity(view, completed.camera, completed.skin.sceneVersion, completed.skin.contentVersion);
            sample.monotonicNs = record.completedSteadyNs; sample.visibility = state_->Visibility(record.flags);
            sample.result = (record.flags & WAR3_RENDER_FRAME_METRICS_DRAW_COMPLETED) && record.result == 0
                && (record.flags & WAR3_RENDER_FRAME_METRICS_UPLOAD_VALID) ? FrameMeasurementResult::Completed : FrameMeasurementResult::Rejected;
            const bool bound = meta && meta->cameraHash == sample.identity.cameraFingerprint
                && meta->physical.width() == static_cast<int>(view.width) && meta->physical.height() == static_cast<int>(view.height)
                && meta->presentationRevision == completed.presentation.presentationRevision
                && meta->modelEpoch == completed.presentation.modelEpoch;
            const auto* playing = state_->world.AnimationPlayer();
            sample.playing = meta && meta->playing && playing && playing->Playing();
            if (bound) {
                sample.poseFingerprint = meta->poseHash; sample.particleFingerprint = meta->particleHash;
                sample.witnessIdentityFingerprint = meta->witnessHash;
                if (state_->witnessConfirmed && !state_->background && record.completedSteadyNs >= state_->witnessConfirmedSinceNs)
                    sample.visibleDynamicWitness = meta->witness;
            } else ++state_->metadataMisses;
            if (record.completedSteadyNs < state_->unobscuredSinceNs && sample.visibility == FrameMeasurementVisibility::Visible)
                sample.visibility = FrameMeasurementVisibility::Unknown;
            sample.timingsNs[static_cast<std::size_t>(FrameMeasurementStage::InputUpload)] = record.uploadCpuNs;
            sample.timingsNs[static_cast<std::size_t>(FrameMeasurementStage::DrawCpu)] = record.drawCpuNs;
            sample.timingsNs[static_cast<std::size_t>(FrameMeasurementStage::PresentCpu)] = record.presentCpuNs;
            if (record.flags & WAR3_RENDER_FRAME_METRICS_GPU_TIME_AVAILABLE) sample.timingsNs[static_cast<std::size_t>(FrameMeasurementStage::Gpu)] = record.gpuDrawNs;
            QJsonObject json{{"identity", IdentityJson(sample.identity)}, {"steady_ns", Number(sample.monotonicNs)},
                {"phase", state_->phase}, {"renderer_epoch", Number(epoch)}, {"result", record.result}, {"present_result", record.presentResult},
                {"flags", static_cast<int>(record.flags)}, {"visibility", VisibilityName(sample.visibility)}, {"playing", sample.playing},
                {"visible_dynamic_witness", sample.visibleDynamicWitness ? QJsonValue(*sample.visibleDynamicWitness) : QJsonValue()},
                {"pose_fingerprint", Optional(sample.poseFingerprint)}, {"particle_geometry_fingerprint", Optional(sample.particleFingerprint)},
                {"witness_identity_fingerprint", Optional(sample.witnessIdentityFingerprint)}, {"witness_binding_valid", bound},
                {"map_edit_revision_known", meta != nullptr},
                {"camera", CameraJson(completed.camera)}, {"surface_epoch", Number(view.surfaceEpoch)}, {"scene_epoch", Number(view.sceneEpoch)},
                {"view_epoch", Number(view.viewEpoch)}, {"model_scene_version", Number(completed.skin.sceneVersion)},
                {"presentation_revision", Number(completed.presentation.presentationRevision)}, {"model_epoch", Number(completed.presentation.modelEpoch)},
                {"visible_models", static_cast<int>(record.visibleModelInstanceCount)}, {"total_models", static_cast<int>(record.modelInstanceCount)},
                {"visible_particle_draws", static_cast<int>(record.visibleParticleDrawCount)}, {"total_particle_draws", static_cast<int>(record.particleDrawCount)},
                {"logical_width", viewport->width()}, {"logical_height", viewport->height()}, {"device_pixel_ratio", viewport->devicePixelRatioF()},
                {"upload_cpu_ns", Number(record.uploadCpuNs)}, {"draw_cpu_ns", Number(record.drawCpuNs)}, {"present_cpu_ns", Number(record.presentCpuNs)},
                {"gpu_draw_ns", sample.timingsNs[static_cast<std::size_t>(FrameMeasurementStage::Gpu)] ? Number(record.gpuDrawNs) : QJsonValue()},
                {"witnesses", bound ? meta->witnesses : QJsonArray{}},
                {"simulation_view",meta ? QJsonValue(meta->simulationView) : QJsonValue()},
                {"simulation_visibility_counts",meta ? QJsonValue(meta->visibilityCounts) : QJsonValue()}};
            if (auto* player = state_->world.AnimationPlayer()) {
                if (const auto timing = player->Timing(completed.skin.sceneVersion, completed.skin.contentVersion)) {
                    const bool newCpuTask = timing->sequence != state_->lastMeasuredCpuSequence
                        && timing->uiStartedNs >= state_->startedNs;
                    if (newCpuTask) {
                        sample.timingsNs[static_cast<std::size_t>(FrameMeasurementStage::PoseEvaluation)] = timing->frame.poseEvaluationNs;
                        sample.timingsNs[static_cast<std::size_t>(FrameMeasurementStage::ParticleEvaluation)] = timing->frame.particlesTotalNs;
                        sample.timingsNs[static_cast<std::size_t>(FrameMeasurementStage::UiReceive)] = timing->uiAcceptNs;
                        state_->lastMeasuredCpuSequence = timing->sequence;
                    }
                    json.insert("cpu_task_charged_this_completion", newCpuTask);
                    sample.omittedInstances = timing->hiddenInstances;
                    sample.downgradedInstances = timing->budgetStaticInstances + timing->budgetParticleInstances;
                    json.insert("cpu", AttemptJson(*timing));
                }
            }
            if (const auto receive = viewport->ReceiveTiming(completed.skin.sceneVersion, completed.skin.contentVersion)) {
                json.insert("ui_receive_parts", QJsonObject{{"picking_ns", Number(receive->pickingNs)}, {"visibility_ns", Number(receive->visibilityNs)},
                    {"submit_ns", Number(receive->submitNs)}, {"install_ns", Number(receive->installNs)}, {"measurement_observer_ns", Number(receive->observerNs)}});
            }
            state_->CheckCaptureInterval(sample);
            state_->analyzer.AddCompletedFrame(sample); state_->Write(state_->frames, json); state_->cursor = view.frameCount;
        }
        if (state_->cursor >= state_->device.latestFrame) break;
    }
    if (auto* player = state_->world.AnimationPlayer()) {
        const auto history = player->TimingHistory(state_->cpuCursor);
        if (!history.empty() && history.front().sequence > state_->cpuCursor + 1) {
            const auto lost = history.front().sequence - state_->cpuCursor - 1;
            state_->cpuLost += lost; state_->analyzer.AddLostRecords(lost);
            state_->Write(state_->attempts, {{"event", "cpu_history_gap"}, {"lost_records", Number(lost)}});
        }
        for (const auto& attempt : history) {
            auto json = AttemptJson(attempt); json.insert("phase", state_->phase);
            state_->Write(state_->attempts, json); state_->cpuCursor = attempt.sequence;
        }
        const auto instances = player->InstanceTimingHistory(state_->instanceCursor);
        if (!instances.empty() && instances.front().sequence > state_->instanceCursor + 1)
            state_->Write(state_->attempts, {{"event", "instance_history_gap"},
                {"lost_records", Number(instances.front().sequence - state_->instanceCursor - 1)}});
        for (const auto& attempt : instances) {
            auto json = InstanceTimingJson(attempt); json.insert("phase", state_->phase);
            state_->Write(state_->attempts, json); state_->instanceCursor = attempt.sequence;
        }
    }
}

void WorldFrameRecorder::Capture() {
    if (!Running() || !state_->viewport) return;
    auto* viewport = state_->viewport.data();
    FrameScreenshotSample sample;
    sample.monotonicNs = NowNs(); sample.visibility = state_->Visibility(WAR3_RENDER_FRAME_METRICS_WINDOW_ATTACHED);
    if (state_->encoder.Busy()) {
        ++state_->screenshotsSkipped;
        state_->Write(state_->captures, {{"event", "capture_skipped"}, {"reason", "WORLD_CAPTURE_ENCODER_BUSY"},
            {"steady_ns", Number(sample.monotonicNs)}, {"visibility", VisibilityName(sample.visibility)},
            {"phase", state_->phase}, {"measurement_epoch", Number(state_->startedNs)}});
        return; // 不做读回，不积累第二份RGBA。
    }
    if (!viewport->isVisible() || viewport->window()->isMinimized()) {
        state_->ResetComparisons(); ++state_->screenshotsSkipped;
        state_->Write(state_->captures, {{"event", "capture_skipped"}, {"steady_ns", Number(sample.monotonicNs)},
            {"visibility", VisibilityName(sample.visibility)}, {"phase", state_->phase}}); return;
    }
    const auto begun = NowNs(); auto captured = viewport->CaptureCompletedFrame(viewport->DocumentRevision(), kMaximumFrameCaptureBytes);
    sample.readbackNs = NowNs() - begun; sample.monotonicNs = NowNs();
    Drain(); // 读回可能完成新帧；先接收其不可变完成回执，不将未来帧当旧身份。
    QJsonObject json{{"steady_ns", Number(sample.monotonicNs)}, {"phase", state_->phase}, {"readback_ns", Optional(sample.readbackNs)}};
    if (!captured.Succeeded()) {
        ++state_->screenshotsFailed; sample.result = FrameMeasurementResult::Rejected;
        const auto diagnostic = captured.diagnostics.empty() ? std::string("CAPTURE_FAILED") : captured.diagnostics.front().code;
        json.insert("diagnostic_code", Text(diagnostic));
        RecordCapture(sample, std::move(json)); return;
    }
    auto& frame = *captured.value;
    War3RenderViewInfoV2 view{}; view.width = frame.view.physicalWidth; view.height = frame.view.physicalHeight;
    view.frameCount = frame.view.identity.frameCount; view.documentRevision = frame.documentRevision;
    view.surfaceEpoch = frame.view.identity.surfaceEpoch;
    view.sceneEpoch = frame.view.identity.sceneEpoch; view.viewEpoch = frame.view.identity.viewEpoch;
    const auto scene = frame.skin ? frame.skin->sceneVersion : 0, content = frame.skin ? frame.skin->contentVersion : 0;
    sample.identity = state_->Identity(view, frame.camera, scene, content); sample.completionBound = true;
    const auto* meta = state_->Find(scene, content);
    const bool bound = meta && meta->cameraHash == sample.identity.cameraFingerprint && meta->physical.width() == static_cast<int>(view.width)
        && meta->physical.height() == static_cast<int>(view.height) && frame.presentation
        && meta->presentationRevision == frame.presentation->presentationRevision
        && meta->modelEpoch == frame.presentation->modelEpoch;
    const auto* playing = state_->world.AnimationPlayer();
    sample.playing = meta && meta->playing && playing && playing->Playing();
    if (bound && state_->witnessConfirmed && !state_->background) sample.visibleDynamicWitness = meta->witness;
    json.insert("identity", IdentityJson(sample.identity)); json.insert("camera", CameraJson(frame.camera));
    json.insert("witnesses", bound ? meta->witnesses : QJsonArray{});
    json.insert("simulation_view", meta ? QJsonValue(meta->simulationView) : QJsonValue());
    json.insert("simulation_visibility_counts", meta ? QJsonValue(meta->visibilityCounts) : QJsonValue());
    json.insert("model_layer_count", static_cast<int>(frame.modelLayers.size()));
    auto input = MakeWorldScreenshotEncodeInput(std::move(frame));
    for (auto it = json.constBegin(); it != json.constEnd(); ++it) input.metadata.insert(it.key(), it.value());
    input.sample = sample;
    input.captureIndex = ++state_->captureIndex; input.comparisonEpoch = state_->captureComparisonEpoch;
    state_->pendingCaptureComparison = sample;
    if (!state_->encoder.Submit(std::move(input))) {
        state_->pendingCaptureComparison.reset(); ++state_->screenshotsSkipped;
        state_->Write(state_->captures, {{"event", "capture_skipped"}, {"reason", "WORLD_CAPTURE_ENCODER_BUSY"},
            {"steady_ns", Number(sample.monotonicNs)}, {"identity", IdentityJson(sample.identity)},
            {"phase", state_->phase}, {"readback_ns", Optional(sample.readbackNs)}});
    }
}

void WorldFrameRecorder::CollectCapture(bool wait) {
    auto encoded = wait ? state_->encoder.Finish() : state_->encoder.Poll();
    if (!encoded) return;
    state_->pendingCaptureComparison.reset();
    auto& sample = encoded->sample; auto json = std::move(encoded->metadata);
    const auto collectedNs = NowNs();
    if (sample.identity.measurementEpoch != state_->startedNs) {
        ++state_->screenshotsFailed;
        state_->Write(state_->captures, {{"event", "capture_session_mismatch"},
            {"expected_epoch", Number(state_->startedNs)}, {"captured_epoch", Number(sample.identity.measurementEpoch)}});
        state_->writeFailed = true; state_->lastFailure = tr("截图属于另一测量会话，未写入当前目录"); return;
    }
    json.insert("encode_queued_ns", Number(encoded->queuedNs));
    json.insert("encode_worker_started_ns", Number(encoded->workerStartedNs));
    json.insert("encode_worker_finished_ns", Number(encoded->workerFinishedNs));
    json.insert("encode_queue_wait_ns", encoded->workerStartedNs ? Number(encoded->workerStartedNs - encoded->queuedNs) : QJsonValue());
    json.insert("pixel_sha_ns", Number(encoded->pixelHashNs)); json.insert("png_encode_ns", Number(encoded->pngEncodeNs));
    json.insert("ui_collected_ns", Number(collectedNs));
    json.insert("encode_result_wait_ns", Number(collectedNs - encoded->workerFinishedNs));
    json.insert("pixel_sha256", Text(sample.pixelSha256)); json.insert("encoding_thread", "worker");
    json.insert("disk_write_thread", "ui");
    QString failure;
    if (!encoded->Succeeded()) {
        failure = tr("截图后台编码失败"); json.insert("diagnostic_code", encoded->diagnostic);
    } else {
        const auto fileName = QStringLiteral("screenshots/frame-%1.png").arg(encoded->captureIndex, 6, 10, QLatin1Char('0'));
        const auto writeBegan = NowNs(); QFile file(state_->directory + '/' + fileName);
        if (state_->bytesWritten + static_cast<std::uint64_t>(encoded->png.size()) > kMaximumBytes - kSummaryReserve
            || !file.open(QIODevice::WriteOnly | QIODevice::NewOnly) || file.write(encoded->png) != encoded->png.size()
            || !file.flush()) {
            failure = tr("截图预算或磁盘写入失败"); json.insert("diagnostic_code", "WORLD_CAPTURE_PNG_WRITE_FAILED");
        } else state_->bytesWritten += static_cast<std::uint64_t>(encoded->png.size());
        json.insert("png_disk_write_ns", Number(NowNs() - writeBegan)); json.insert("file", fileName);
    }
    if (!failure.isEmpty()) {
        ++state_->screenshotsFailed; sample.result = FrameMeasurementResult::Rejected;
    } else if (encoded->comparisonEpoch != state_->captureComparisonEpoch
        && sample.visibility == FrameMeasurementVisibility::Visible && sample.playing && sample.visibleDynamicWitness.value_or(false)) {
        sample.result = FrameMeasurementResult::Stale;
        json.insert("diagnostic_code", "WORLD_CAPTURE_COMPARISON_CHANGED_WHILE_ENCODING");
    }
    RecordCapture(sample, std::move(json));
    if (!failure.isEmpty()) { state_->writeFailed = true; state_->lastFailure = failure; }
}

void WorldFrameRecorder::RecordCapture(const FrameScreenshotSample& sample, QJsonObject json) {
    const auto observation = state_->analyzer.AddScreenshot(sample);
    json.insert("classification", ClassificationName(observation.classification));
    json.insert("visibility", VisibilityName(sample.visibility)); json.insert("playing", sample.playing);
    json.insert("equal_pixel_run", Number(observation.equalPixelRun));
    json.insert("stall_begin_ns", Number(observation.stallBeginNs)); json.insert("stall_end_ns", Number(observation.stallEndNs));
    json.insert("visible_dynamic_witness", sample.visibleDynamicWitness ? QJsonValue(*sample.visibleDynamicWitness) : QJsonValue());
    state_->Write(state_->captures, json);
}

void WorldFrameRecorder::Tick() {
    if (!Running()) return;
    const auto now = NowNs();
    if (now - state_->startedNs >= kMaximumDurationNs) { Stop(tr("已达到一小时测量上限")); return; }
    if (!state_->viewport || !state_->worldGuard) { Stop(tr("世界视口或模块已销毁")); return; }
    if (state_->viewport->RendererFailed()) {
        state_->Write(state_->frames, {{"event", "renderer_failed"}, {"steady_ns", Number(now)}, {"status", state_->viewport->StatusText()}});
        Stop(tr("渲染器失败：%1").arg(state_->viewport->StatusText())); return;
    }
    const auto visibility = state_->Visibility(WAR3_RENDER_FRAME_METRICS_WINDOW_ATTACHED);
    if (state_->lastVisibility && *state_->lastVisibility != visibility) {
        state_->ResetComparisons();
        state_->Write(state_->frames, {{"event", "window_state_changed"}, {"steady_ns", Number(now)},
            {"before", VisibilityName(*state_->lastVisibility)}, {"after", VisibilityName(visibility)}});
    }
    state_->lastVisibility = visibility;
    Drain();
    CollectCapture(false);
    if (now >= state_->nextCaptureNs) { Capture(); state_->nextCaptureNs = NowNs() + kCaptureIntervalNs; }
    if (state_->writeFailed) { Stop(state_->lastFailure); return; }
    if (now >= state_->nextStatusNs) {
        const auto report = state_->analyzer.Report();
        const auto statusNow = NowNs();
        const auto elapsed = statusNow - state_->statusNs;
        const double liveFps = elapsed ? static_cast<double>(report.completedFrames - state_->statusFrames)
            * 1.0e9 / static_cast<double>(elapsed) : 0.0;
        emit StatusChanged(tr("已记录 %1 个完成帧；最近区间绘制 %2 FPS；资格区间有效动画 %3 FPS；截图 %4；停滞 %5")
            .arg(report.completedFrames).arg(QString::number(liveFps, 'f', 1))
            .arg(report.effectiveAnimationFps ? QString::number(*report.effectiveAnimationFps, 'f', 1) : tr("不可用"))
            .arg(report.acceptedScreenshots).arg(report.stallObservations));
        state_->statusNs = statusNow; state_->statusFrames = report.completedFrames;
        state_->nextStatusNs = statusNow + 1000000000;
    }
}

bool WorldFrameRecorder::WriteSummary(const QString& reason) {
    const auto r = state_->analyzer.Report();
    const auto recordingNs = (state_->endedNs ? state_->endedNs : NowNs()) - state_->startedNs;
    QJsonObject timings;
    constexpr const char* names[]{"pose_evaluation", "particle_evaluation", "ui_receive", "input_upload", "draw_cpu", "present_cpu", "capture_readback", "gpu"};
    for (std::size_t i = 0; i < r.timings.size(); ++i) {
        const auto& t = r.timings[i];
        timings.insert(QString::fromLatin1(names[i]), QJsonObject{{"sample_count", Number(t.sampleCount)}, {"retained_count", Number(t.retainedCount)},
            {"omitted_from_quantiles", Number(t.omittedFromQuantiles)}, {"minimum_ns", Optional(t.minimumNs)}, {"maximum_ns", Optional(t.maximumNs)},
            {"p50_ns", Optional(t.p50Ns)}, {"p95_ns", Optional(t.p95Ns)}, {"p99_ns", Optional(t.p99Ns)}});
    }
    QJsonObject visibility;
    for (std::size_t i = 0; i < r.visibilitySamples.size(); ++i) visibility.insert(VisibilityName(static_cast<FrameMeasurementVisibility>(i)), Number(r.visibilitySamples[i]));
    const auto adapterLength = std::find(std::begin(state_->device.adapterName), std::end(state_->device.adapterName), '\0') - std::begin(state_->device.adapterName);
    QJsonObject json{{"schema_version", 1}, {"running", Running()}, {"reason", reason}, {"background", state_->background},
        {"acceptance", "pending_author_visible_acceptance"}, {"source_map", state_->sourcePath}, {"source_sha256_before", state_->sourceBefore},
        {"source_sha256_after", state_->sourceAfter.isEmpty() ? QJsonValue() : QJsonValue(state_->sourceAfter)},
        {"source_unchanged", state_->sourceAfter.isEmpty() ? QJsonValue() : QJsonValue(state_->sourceAfter == state_->sourceBefore)},
        {"started_steady_ns", Number(state_->startedNs)}, {"exported_utc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {"adapter", QString::fromUtf8(state_->device.adapterName, static_cast<qsizetype>(adapterLength))},
        {"renderer_device_observed", state_->device.structSize >= sizeof(state_->device)},
        {"software_renderer", state_->device.structSize >= sizeof(state_->device) ? QJsonValue(state_->device.software != 0) : QJsonValue()},
        {"feature_level", static_cast<int>(state_->device.featureLevel)},
        {"completed_frames", Number(r.completedFrames)}, {"duplicate_frames", Number(r.duplicateFrames)},
        {"rejected_samples", Number(r.rejectedSamples)}, {"stale_samples", Number(r.staleSamples)},
        {"missing_completed_frames", Number(r.missingCompletedFrames)}, {"lost_records", Number(r.externallyLostRecords)},
        {"cpu_history_lost", Number(state_->cpuLost)}, {"metadata_binding_misses", Number(state_->metadataMisses)},
        {"clock_regressions", Number(r.clockRegressions)}, {"source_transitions", Number(r.sourceTransitions)},
        {"continuity_breaks", Number(r.continuityBreaks)},
        {"completed_fps", r.completedFps ? QJsonValue(*r.completedFps) : QJsonValue()},
        {"completed_fps_scope", "consecutive completed-frame intervals; see whole recording window separately"},
        {"recording_duration_ns", Number(recordingNs)},
        {"completed_fps_over_recording_window", recordingNs ? QJsonValue(static_cast<double>(r.completedFrames)
            * 1.0e9 / static_cast<double>(recordingNs)) : QJsonValue()},
        {"effective_visible_animation_fps", r.effectiveAnimationFps ? QJsonValue(*r.effectiveAnimationFps) : QJsonValue()},
        {"effective_visible_animation_fps_scope", "qualified same-camera visible witness intervals; unknown intervals cannot establish acceptance"},
        {"effective_animation_fps_recording_lower_bound", recordingNs && r.effectiveAnimationFps
            ? QJsonValue(static_cast<double>(r.effectiveAnimationFrames) * 1.0e9 / static_cast<double>(recordingNs)) : QJsonValue()},
        {"pose_advanced_frames", Number(r.poseAdvancedFrames)}, {"particle_advanced_frames", Number(r.particleAdvancedFrames)},
        {"effective_animation_frames", Number(r.effectiveAnimationFrames)}, {"animation_eligible_intervals", Number(r.animationEligibleIntervals)},
        {"animation_eligible_ns", Number(r.animationEligibleNs)}, {"unknown_payload_intervals", Number(r.unknownPayloadIntervals)},
        {"observed_ns", Number(r.observedNs)}, {"visibility", visibility}, {"paused_samples", Number(r.pausedSamples)},
        {"static_samples", Number(r.staticSamples)}, {"unknown_witness_samples", Number(r.unknownWitnessSamples)},
        {"completion_record_visible_and_complete", r.visibleRecordComplete}, {"maximum_omitted_instances", Optional(r.maximumOmittedInstances)},
        {"maximum_downgraded_instances", Optional(r.maximumDowngradedInstances)}, {"screenshots", Number(r.screenshots)},
        {"accepted_screenshots", Number(r.acceptedScreenshots)}, {"screenshots_failed", Number(state_->screenshotsFailed)},
        {"screenshots_skipped", Number(state_->screenshotsSkipped)},
        {"stall_observations", Number(r.stallObservations)}, {"timing_distributions", timings},
        {"quantiles_scope", "latest_retained_samples_per_stage; minima/maxima cover all inputs"},
        {"gpu_timing", "unavailable unless backend timestamp-query flag explicitly present"},
        {"readback_interval_ns", Number(kCaptureIntervalNs)}, {"duration_limit_ns", Number(kMaximumDurationNs)},
        {"byte_limit", Number(kMaximumBytes)}, {"bytes_written", Number(state_->bytesWritten)},
        {"write_failed", state_->writeFailed}, {"last_failure", state_->lastFailure}, {"phase_events", state_->phaseEvents},
        {"phase_events_omitted", Number(state_->phaseEventsOmitted)}, {"witness_confirmation_at_export", state_->witnessConfirmed},
        {"unobscured_confirmation_at_export", state_->unobscured},
        {"capture_overhead", "UI readback and disk write; one worker owns pixel SHA and PNG encode; queue/worker/result wait logged separately; recording FPS includes contention"},
        {"capture_encoder_capacity", 1}, {"capture_encoder_busy", state_->encoder.Busy()},
        {"witness_limit", 8}, {"cpu_metadata_history_limit", 512},
        {"source_sha_read_limit_bytes", Number(512ull * 1024 * 1024)},
        {"witness_qualification", "author region confirmation + unobscured confirmation + projected playing successful pose or drawable particles + matching same-frame camera and viewport"}};
    QSaveFile file(state_->directory + QStringLiteral("/summary.json"));
    const auto bytes = QJsonDocument(json).toJson(QJsonDocument::Indented);
    return static_cast<std::uint64_t>(bytes.size()) <= kSummaryReserve && file.open(QIODevice::WriteOnly)
        && file.write(bytes) == bytes.size() && file.commit();
}

void WorldFrameRecorder::Stop(const QString& reason) {
    if (!Running()) return;
    state_->timer.stop(); Drain();
    // 停止不丢已读回的截图；旧会话在日志关闭/State更换前同步取回唯一在途结果。
    // 最后的等待和UI落盘也属于这轮测量开销，不从记录时长中扣除。
    CollectCapture(true); state_->endedNs = NowNs();
    if (state_->viewport) state_->viewport->SetMeasurementEnabled(false);
    disconnect(state_->acceptedConnection); state_->running = false;
    state_->sourceAfter = FileSha(state_->sourcePath);
    const bool framesFlushed = state_->frames.flush(), capturesFlushed = state_->captures.flush(), attemptsFlushed = state_->attempts.flush();
    if (!framesFlushed || !capturesFlushed || !attemptsFlushed) { state_->writeFailed = true; state_->lastFailure = tr("测量日志 flush 失败"); }
    state_->frames.close(); state_->captures.close(); state_->attempts.close();
    const bool summary = WriteSummary(reason);
    emit StatusChanged(summary ? tr("测量已导出：%1").arg(state_->directory) : tr("测量结束，但汇总写出失败：%1").arg(state_->directory));
    if (summary && !state_->writeFailed) emit Finished(state_->directory);
}

} // namespace War3Map::Modules
