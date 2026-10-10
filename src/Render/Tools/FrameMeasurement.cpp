#include "War3MapCore/Render/FrameMeasurement.h"

#include <algorithm>
#include <limits>

namespace War3Map::Render {
namespace {

bool SameStream(const FrameMeasurementIdentity& a, const FrameMeasurementIdentity& b) {
    return a.sourceIdentity == b.sourceIdentity && a.viewportIdentity == b.viewportIdentity
        && a.measurementEpoch == b.measurementEpoch;
}

bool SameSurface(const FrameMeasurementIdentity& a, const FrameMeasurementIdentity& b) {
    return SameStream(a, b) && a.mapEditRevision == b.mapEditRevision && a.viewportRevision == b.viewportRevision
        && a.surfaceEpoch == b.surfaceEpoch && a.renderSceneEpoch == b.renderSceneEpoch && a.viewEpoch == b.viewEpoch
        && a.physicalWidth == b.physicalWidth && a.physicalHeight == b.physicalHeight;
}

bool AnimationEligible(const FrameMeasurementSample& sample) {
    return sample.visibility == FrameMeasurementVisibility::Visible && sample.playing
        && sample.visibleDynamicWitness == true;
}

bool Changed(const std::optional<std::uint64_t>& a, const std::optional<std::uint64_t>& b) {
    return a && b && *a != *b;
}

void Maximum(std::optional<std::uint64_t>& target, const std::optional<std::uint64_t>& value) {
    if (value && (!target || *value > *target)) target = value;
}

// 输入预算通常远小于 UINT64_MAX，仍不让审计计数溢出成零。
void Accumulate(std::uint64_t& target, std::uint64_t value) {
    target = value > std::numeric_limits<std::uint64_t>::max() - target
        ? std::numeric_limits<std::uint64_t>::max() : target + value;
}

} // namespace

FrameMeasurementAnalyzer::FrameMeasurementAnalyzer(std::size_t timingSampleCapacity)
    : timingSampleCapacity_(std::min<std::size_t>(timingSampleCapacity, 65536)) {
}

void FrameMeasurementAnalyzer::AddTiming(FrameMeasurementStage stage, std::uint64_t ns) {
    const auto index = static_cast<std::size_t>(stage);
    if (index >= timings_.size()) return;
    auto& series = timings_[index];
    Accumulate(series.count, 1);
    if (!series.minimum || ns < *series.minimum) series.minimum = ns;
    if (!series.maximum || ns > *series.maximum) series.maximum = ns;
    if (timingSampleCapacity_ == 0) return;
    if (series.values.size() < timingSampleCapacity_) {
        series.values.push_back(ns);
    } else {
        series.values[series.next] = ns;
        series.next = (series.next + 1) % timingSampleCapacity_;
    }
}

void FrameMeasurementAnalyzer::ResetScreenshotRun() {
    previousScreenshot_.reset();
    equalPixelRun_ = 0;
    equalPixelBeginNs_ = 0;
}

void FrameMeasurementAnalyzer::AddCompletedFrame(const FrameMeasurementSample& sample) {
    if (sample.result != FrameMeasurementResult::Completed) {
        Accumulate(sample.result == FrameMeasurementResult::Stale
            ? report_.staleSamples : report_.rejectedSamples, 1);
        previous_.reset();
        ResetScreenshotRun();
        return;
    }
    if (sample.identity.sourceIdentity.empty() || sample.identity.sourceIdentity.size() > 4096
        || sample.identity.completedFrameNumber == 0
        || sample.identity.physicalWidth == 0 || sample.identity.physicalHeight == 0) {
        Accumulate(report_.rejectedSamples, 1);
        previous_.reset();
        ResetScreenshotRun();
        return;
    }
    const bool sameStream = previous_ && SameStream(previous_->identity, sample.identity);
    if (sameStream && sample.identity.completedFrameNumber <= previous_->identity.completedFrameNumber) {
        if (sample.identity.completedFrameNumber == previous_->identity.completedFrameNumber) {
            Accumulate(report_.duplicateFrames, 1);
        } else {
            Accumulate(report_.staleSamples, 1);
            ResetScreenshotRun();
        }
        return;
    }
    if (previous_ && !sameStream) {
        Accumulate(report_.sourceTransitions, 1);
        ResetScreenshotRun();
    }
    Accumulate(report_.completedFrames, 1);
    const auto visibility = static_cast<std::size_t>(sample.visibility);
    if (visibility < report_.visibilitySamples.size()) Accumulate(report_.visibilitySamples[visibility], 1);
    else Accumulate(report_.visibilitySamples[static_cast<std::size_t>(FrameMeasurementVisibility::Unknown)], 1);
    if (!sample.playing) Accumulate(report_.pausedSamples, 1);
    else if (!sample.visibleDynamicWitness) Accumulate(report_.unknownWitnessSamples, 1);
    else if (!*sample.visibleDynamicWitness) Accumulate(report_.staticSamples, 1);
    Maximum(report_.maximumOmittedInstances, sample.omittedInstances);
    Maximum(report_.maximumDowngradedInstances, sample.downgradedInstances);
    for (std::size_t i = 0; i < sample.timingsNs.size(); ++i) {
        if (sample.timingsNs[i]) AddTiming(static_cast<FrameMeasurementStage>(i), *sample.timingsNs[i]);
    }
    if (sameStream) {
        const auto frameDelta = sample.identity.completedFrameNumber - previous_->identity.completedFrameNumber;
        if (frameDelta > 1) Accumulate(report_.missingCompletedFrames, frameDelta - 1);
        if (sample.monotonicNs <= previous_->monotonicNs) {
            Accumulate(report_.clockRegressions, 1);
            ResetScreenshotRun();
        } else {
            const auto elapsed = sample.monotonicNs - previous_->monotonicNs;
            Accumulate(report_.observedNs, elapsed);
            Accumulate(completedIntervals_, 1);
            // 丢帧区间不猜中间姿势，也不以两个端点抬高有效动画帧率。
            if (frameDelta == 1 && SameSurface(previous_->identity, sample.identity)
                && AnimationEligible(*previous_) && AnimationEligible(sample)
                && previous_->witnessIdentityFingerprint == sample.witnessIdentityFingerprint) {
                if ((previous_->poseFingerprint && sample.poseFingerprint)
                    || (previous_->particleFingerprint && sample.particleFingerprint)) {
                    Accumulate(report_.animationEligibleNs, elapsed);
                    Accumulate(report_.animationEligibleIntervals, 1);
                    const bool poseChanged = Changed(previous_->poseFingerprint, sample.poseFingerprint);
                    const bool particleChanged = Changed(previous_->particleFingerprint, sample.particleFingerprint);
                    if (poseChanged) Accumulate(report_.poseAdvancedFrames, 1);
                    if (particleChanged) Accumulate(report_.particleAdvancedFrames, 1);
                    if (poseChanged || particleChanged) Accumulate(report_.effectiveAnimationFrames, 1);
                } else Accumulate(report_.unknownPayloadIntervals, 1);
            }
        }
    }
    if (!AnimationEligible(sample)) ResetScreenshotRun();
    previous_ = sample;
}

FrameScreenshotObservation FrameMeasurementAnalyzer::AddScreenshot(const FrameScreenshotSample& sample) {
    Accumulate(report_.screenshots, 1);
    if (sample.readbackNs) AddTiming(FrameMeasurementStage::CaptureReadback, *sample.readbackNs);
    FrameScreenshotObservation result;
    auto reject = [&](FrameScreenshotClassification classification) {
        ResetScreenshotRun();
        result.classification = classification;
        return result;
    };
    if (sample.result == FrameMeasurementResult::Rejected) return reject(FrameScreenshotClassification::Rejected);
    if (sample.result == FrameMeasurementResult::Stale) return reject(FrameScreenshotClassification::Stale);
    if (!sample.completionBound || sample.pixelSha256.size() != 64 || sample.identity.completedFrameNumber == 0
        || !std::all_of(sample.pixelSha256.begin(), sample.pixelSha256.end(), [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        })) {
        return reject(FrameScreenshotClassification::Rejected);
    }
    if (!previous_ || !SameSurface(previous_->identity, sample.identity)) {
        return reject(FrameScreenshotClassification::IdentityMismatch);
    }
    if (sample.identity.completedFrameNumber > previous_->identity.completedFrameNumber
        || (previousScreenshot_ && (sample.monotonicNs <= previousScreenshot_->monotonicNs
            || sample.identity.completedFrameNumber < previousScreenshot_->identity.completedFrameNumber))) {
        return reject(FrameScreenshotClassification::Stale);
    }
    Accumulate(report_.acceptedScreenshots, 1);
    switch (sample.visibility) {
    case FrameMeasurementVisibility::Minimized: return reject(FrameScreenshotClassification::Minimized);
    case FrameMeasurementVisibility::Occluded: return reject(FrameScreenshotClassification::Occluded);
    case FrameMeasurementVisibility::Hidden: return reject(FrameScreenshotClassification::Hidden);
    case FrameMeasurementVisibility::Unknown: return reject(FrameScreenshotClassification::VisibilityUnknown);
    case FrameMeasurementVisibility::Visible: break;
    case FrameMeasurementVisibility::Count: return reject(FrameScreenshotClassification::VisibilityUnknown);
    }
    if (!sample.playing) return reject(FrameScreenshotClassification::Paused);
    if (!sample.visibleDynamicWitness) return reject(FrameScreenshotClassification::WitnessUnknown);
    if (!*sample.visibleDynamicWitness) return reject(FrameScreenshotClassification::Static);
    const bool unchanged = previousScreenshot_ && SameSurface(previousScreenshot_->identity, sample.identity)
        && previousScreenshot_->identity.cameraFingerprint == sample.identity.cameraFingerprint
        && previousScreenshot_->pixelSha256 == sample.pixelSha256;
    if (unchanged) Accumulate(equalPixelRun_, 1);
    else {
        equalPixelRun_ = 1;
        equalPixelBeginNs_ = sample.monotonicNs;
    }
    result.equalPixelRun = equalPixelRun_;
    result.classification = equalPixelRun_ >= 3
        ? FrameScreenshotClassification::Stall : FrameScreenshotClassification::Dynamic;
    if (result.classification == FrameScreenshotClassification::Stall) {
        result.stallBeginNs = equalPixelBeginNs_;
        result.stallEndNs = sample.monotonicNs;
        Accumulate(report_.stallObservations, 1);
    }
    previousScreenshot_ = sample;
    return result;
}

void FrameMeasurementAnalyzer::AddLostRecords(std::uint64_t count) {
    Accumulate(report_.externallyLostRecords, count);
    if (count != 0) {
        previous_.reset();
        ResetScreenshotRun();
    }
}

void FrameMeasurementAnalyzer::ResetComparisons() {
    Accumulate(report_.continuityBreaks, 1);
    previous_.reset();
    ResetScreenshotRun();
}

FrameMeasurementReport FrameMeasurementAnalyzer::Report() const {
    auto result = report_;
    if (result.observedNs != 0) result.completedFps = static_cast<double>(completedIntervals_) * 1.0e9
        / static_cast<double>(result.observedNs);
    if (result.animationEligibleNs != 0) result.effectiveAnimationFps =
        static_cast<double>(result.effectiveAnimationFrames) * 1.0e9 / static_cast<double>(result.animationEligibleNs);
    const auto visible = result.visibilitySamples[static_cast<std::size_t>(FrameMeasurementVisibility::Visible)];
    result.visibleRecordComplete = result.completedFrames > 1 && visible == result.completedFrames
        && result.rejectedSamples == 0 && result.staleSamples == 0 && result.missingCompletedFrames == 0
        && result.externallyLostRecords == 0 && result.clockRegressions == 0 && result.sourceTransitions == 0
        && result.continuityBreaks == 0;
    for (std::size_t i = 0; i < timings_.size(); ++i) {
        const auto& series = timings_[i];
        auto& distribution = result.timings[i];
        distribution.sampleCount = series.count;
        distribution.retainedCount = static_cast<std::uint64_t>(series.values.size());
        distribution.omittedFromQuantiles = series.count - distribution.retainedCount;
        distribution.minimumNs = series.minimum;
        distribution.maximumNs = series.maximum;
        if (series.values.empty()) continue;
        auto sorted = series.values;
        std::sort(sorted.begin(), sorted.end());
        auto quantile = [&](std::size_t percentile) {
            const auto rank = (sorted.size() * percentile + 99) / 100;
            return sorted[rank - 1];
        };
        distribution.p50Ns = quantile(50);
        distribution.p95Ns = quantile(95);
        distribution.p99Ns = quantile(99);
    }
    return result;
}

std::uint64_t HashFrameMeasurementBytes(std::span<const std::byte> bytes) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const auto value : bytes) {
        hash ^= static_cast<std::uint64_t>(std::to_integer<unsigned char>(value));
        hash *= 1099511628211ULL;
    }
    return hash;
}

} // namespace War3Map::Render
