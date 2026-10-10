#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace War3Map::Render {

enum class FrameMeasurementVisibility : std::uint8_t {
    Visible, Minimized, Occluded, Hidden, Unknown, Count
};
enum class FrameMeasurementResult : std::uint8_t { Completed, Rejected, Stale };
enum class FrameMeasurementStage : std::uint8_t {
    PoseEvaluation, ParticleEvaluation, UiReceive, InputUpload, DrawCpu,
    PresentCpu, CaptureReadback, Gpu, Count
};
inline constexpr std::size_t kFrameMeasurementStageCount =
    static_cast<std::size_t>(FrameMeasurementStage::Count);

// 地图编辑修订、视口修订和完成帧号属于不同编号空间，不参与相互比较。
struct FrameMeasurementIdentity {
    std::string sourceIdentity;
    std::uint64_t mapEditRevision = 0;
    std::uint64_t viewportIdentity = 0;
    std::uint64_t viewportRevision = 0;
    std::uint64_t measurementEpoch = 0;
    std::uint64_t completedFrameNumber = 0;
    std::uint64_t contentVersion = 0;
    std::uint64_t poseVersion = 0;
    std::uint64_t cameraFingerprint = 0;
    std::uint32_t physicalWidth = 0;
    std::uint32_t physicalHeight = 0;
    std::uint64_t surfaceEpoch = 0;
    std::uint64_t renderSceneEpoch = 0;
    std::uint64_t viewEpoch = 0;
};

struct FrameMeasurementSample {
    FrameMeasurementIdentity identity;
    std::uint64_t monotonicNs = 0;
    FrameMeasurementResult result = FrameMeasurementResult::Completed;
    FrameMeasurementVisibility visibility = FrameMeasurementVisibility::Unknown;
    bool playing = false;
    // 只有可证明在本完成帧中可见的活动对象才能填写 true，未知保持空。
    std::optional<bool> visibleDynamicWitness;
    // 来自明确可见见证对象的实际姿势矩阵/时间或粒子数据；全场景指纹只能
    // 配 unknown witness 作诊断，不能以视野外变化抬高可见有效动画帧率。
    std::optional<std::uint64_t> poseFingerprint;
    std::optional<std::uint64_t> particleFingerprint;
    // 可见见证对象集合改变时不比较两个不同集合的聚合指纹。
    std::optional<std::uint64_t> witnessIdentityFingerprint;
    std::array<std::optional<std::uint64_t>, kFrameMeasurementStageCount> timingsNs{};
    std::optional<std::uint64_t> omittedInstances;
    std::optional<std::uint64_t> downgradedInstances;
};

struct FrameScreenshotSample {
    FrameMeasurementIdentity identity;
    std::uint64_t monotonicNs = 0;
    FrameMeasurementResult result = FrameMeasurementResult::Completed;
    FrameMeasurementVisibility visibility = FrameMeasurementVisibility::Unknown;
    bool playing = false;
    std::optional<bool> visibleDynamicWitness;
    // 调用方完成同帧相机/像素/完成回执绑定校验后才置 true。
    bool completionBound = false;
    std::string pixelSha256;
    std::optional<std::uint64_t> readbackNs;
};

enum class FrameScreenshotClassification : std::uint8_t {
    Dynamic, Stall, Paused, Static, WitnessUnknown, Minimized, Occluded,
    Hidden, VisibilityUnknown, Rejected, Stale, IdentityMismatch
};
struct FrameScreenshotObservation {
    FrameScreenshotClassification classification = FrameScreenshotClassification::Rejected;
    std::uint64_t equalPixelRun = 0;
    std::uint64_t stallBeginNs = 0;
    std::uint64_t stallEndNs = 0;
};

struct FrameTimingDistribution {
    std::uint64_t sampleCount = 0;
    std::uint64_t retainedCount = 0;
    std::uint64_t omittedFromQuantiles = 0;
    std::optional<std::uint64_t> minimumNs;
    std::optional<std::uint64_t> maximumNs;
    std::optional<std::uint64_t> p50Ns;
    std::optional<std::uint64_t> p95Ns;
    std::optional<std::uint64_t> p99Ns;
};

struct FrameMeasurementReport {
    std::uint64_t completedFrames = 0;
    std::uint64_t duplicateFrames = 0;
    std::uint64_t rejectedSamples = 0;
    std::uint64_t staleSamples = 0;
    std::uint64_t missingCompletedFrames = 0;
    std::uint64_t externallyLostRecords = 0;
    std::uint64_t clockRegressions = 0;
    std::uint64_t sourceTransitions = 0;
    std::uint64_t continuityBreaks = 0;
    std::uint64_t poseAdvancedFrames = 0;
    std::uint64_t particleAdvancedFrames = 0;
    std::uint64_t effectiveAnimationFrames = 0;
    std::uint64_t animationEligibleIntervals = 0;
    std::uint64_t animationEligibleNs = 0;
    std::uint64_t unknownPayloadIntervals = 0;
    std::uint64_t observedNs = 0;
    std::uint64_t pausedSamples = 0;
    std::uint64_t staticSamples = 0;
    std::uint64_t unknownWitnessSamples = 0;
    std::array<std::uint64_t, static_cast<std::size_t>(FrameMeasurementVisibility::Count)> visibilitySamples{};
    std::optional<double> completedFps;
    std::optional<double> effectiveAnimationFps;
    // 仅表示记录完整且明确可见，不是实窗/硬件或 >=20 FPS 的验收结论。
    bool visibleRecordComplete = false;
    std::optional<std::uint64_t> maximumOmittedInstances;
    std::optional<std::uint64_t> maximumDowngradedInstances;
    std::array<FrameTimingDistribution, kFrameMeasurementStageCount> timings{};
    std::uint64_t screenshots = 0;
    std::uint64_t acceptedScreenshots = 0;
    std::uint64_t stallObservations = 0;
};

// 无 Qt、无渲染副作用的共用分析器。每阶段分位数保留最近 capacity 个样本，
// 未保留数单列；总数和极值覆盖全部输入。默认足够容纳十分钟 20 FPS。
class FrameMeasurementAnalyzer {
public:
    explicit FrameMeasurementAnalyzer(std::size_t timingSampleCapacity = 16384);
    void AddCompletedFrame(const FrameMeasurementSample& sample);
    FrameScreenshotObservation AddScreenshot(const FrameScreenshotSample& sample);
    void AddLostRecords(std::uint64_t count);
    // 页面隐藏/最小化等没有新完成帧时仍可断开比较，不制造完成帧或丢帧。
    void ResetComparisons();
    FrameMeasurementReport Report() const;

private:
    struct TimingSeries {
        std::vector<std::uint64_t> values;
        std::size_t next = 0;
        std::uint64_t count = 0;
        std::optional<std::uint64_t> minimum;
        std::optional<std::uint64_t> maximum;
    };
    void AddTiming(FrameMeasurementStage stage, std::uint64_t ns);
    void ResetScreenshotRun();
    FrameMeasurementReport report_;
    std::size_t timingSampleCapacity_;
    std::array<TimingSeries, kFrameMeasurementStageCount> timings_;
    std::optional<FrameMeasurementSample> previous_;
    std::optional<FrameScreenshotSample> previousScreenshot_;
    std::uint64_t equalPixelRun_ = 0;
    std::uint64_t equalPixelBeginNs_ = 0;
    std::uint64_t completedIntervals_ = 0;
};

// 稳定的字节指纹仅用于相邻完成帧的实际数据变化检测，不作为资源身份/SHA。
std::uint64_t HashFrameMeasurementBytes(std::span<const std::byte> bytes);

} // namespace War3Map::Render
