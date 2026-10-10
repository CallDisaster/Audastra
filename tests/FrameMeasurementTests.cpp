#include "War3MapCore/Render/FrameMeasurement.h"

#include <cmath>
#include <iostream>
#include <stdexcept>

using namespace War3Map::Render;
namespace {
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
FrameMeasurementSample Frame(std::uint64_t number) {
    FrameMeasurementSample sample;
    sample.identity = {"source-sha", 4, 3, 12, 9, number, number, number, 5, 800, 600};
    sample.monotonicNs = number * 50000000;
    sample.visibility = FrameMeasurementVisibility::Visible;
    sample.playing = true; sample.visibleDynamicWitness = true; sample.poseFingerprint = 42;
    sample.timingsNs[static_cast<std::size_t>(FrameMeasurementStage::DrawCpu)] = number;
    return sample;
}
FrameScreenshotSample Shot(std::uint64_t number) {
    const auto frame = Frame(number);
    FrameScreenshotSample shot;
    shot.identity = frame.identity; shot.monotonicNs = frame.monotonicNs;
    shot.visibility = frame.visibility; shot.playing = true; shot.visibleDynamicWitness = true;
    shot.completionBound = true; shot.pixelSha256.assign(64, 'a');
    return shot;
}
void ActualDataAndLoss() {
    FrameMeasurementAnalyzer analyzer(2);
    analyzer.AddCompletedFrame(Frame(1)); analyzer.AddCompletedFrame(Frame(2));
    auto third = Frame(3); third.poseFingerprint = 43; analyzer.AddCompletedFrame(third);
    analyzer.AddCompletedFrame(third);
    auto report = analyzer.Report();
    Require(report.completedFrames == 3 && report.duplicateFrames == 1, "重复完成帧不能再次计数");
    Require(report.effectiveAnimationFrames == 1 && report.poseAdvancedFrames == 1, "内容版本不能冒充姿势推进");
    Require(report.completedFps == 20.0 && report.effectiveAnimationFps == 10.0, "绘制/动画帧率须分开");
    const auto timing = report.timings[static_cast<std::size_t>(FrameMeasurementStage::DrawCpu)];
    Require(timing.sampleCount == 3 && timing.omittedFromQuantiles == 1 && timing.p50Ns == 2
        && timing.p95Ns == 3 && timing.minimumNs == 1, "有界分位数须声明未保留样本");
    auto fifth = Frame(5); fifth.poseFingerprint = 44; analyzer.AddCompletedFrame(fifth);
    Require(analyzer.Report().missingCompletedFrames == 1 && !analyzer.Report().visibleRecordComplete,
        "完成帧缺口不能签成可见记录完整");
    analyzer.AddLostRecords(1); Require(analyzer.Report().externallyLostRecords == 1, "页环丢记录须保留");
    FrameMeasurementAnalyzer unknown;
    auto first = Frame(1); first.poseFingerprint.reset(); unknown.AddCompletedFrame(first);
    auto second = Frame(2); second.poseFingerprint.reset(); unknown.AddCompletedFrame(second);
    Require(!unknown.Report().effectiveAnimationFps && unknown.Report().unknownPayloadIntervals == 1,
        "未知数据不能当零帧动画或有效推进");
}
void ScreenshotQualification() {
    FrameMeasurementAnalyzer analyzer;
    for (std::uint64_t n = 1; n <= 3; ++n) {
        analyzer.AddCompletedFrame(Frame(n)); const auto observation = analyzer.AddScreenshot(Shot(n));
        Require(observation.classification == (n == 3 ? FrameScreenshotClassification::Stall
            : FrameScreenshotClassification::Dynamic), "三个同像素同相机截图才是停滞");
    }
    auto changedCamera = Frame(4); changedCamera.identity.cameraFingerprint = 6;
    analyzer.AddCompletedFrame(changedCamera); auto shot = Shot(4); shot.identity.cameraFingerprint = 6;
    Require(analyzer.AddScreenshot(shot).equalPixelRun == 1, "移动相机须重置停滞计数");
    const FrameScreenshotClassification expected[] = {FrameScreenshotClassification::Paused,
        FrameScreenshotClassification::WitnessUnknown, FrameScreenshotClassification::Static,
        FrameScreenshotClassification::Minimized, FrameScreenshotClassification::Occluded,
        FrameScreenshotClassification::Stale, FrameScreenshotClassification::IdentityMismatch};
    for (std::size_t i = 0; i < std::size(expected); ++i) {
        const auto number = static_cast<std::uint64_t>(5 + i); analyzer.AddCompletedFrame(Frame(number));
        auto sample = Shot(number);
        if (i == 0) sample.playing = false;
        if (i == 1) sample.visibleDynamicWitness.reset();
        if (i == 2) sample.visibleDynamicWitness = false;
        if (i == 3) sample.visibility = FrameMeasurementVisibility::Minimized;
        if (i == 4) sample.visibility = FrameMeasurementVisibility::Occluded;
        if (i == 5) sample.result = FrameMeasurementResult::Stale;
        if (i == 6) ++sample.identity.viewportRevision;
        Require(analyzer.AddScreenshot(sample).classification == expected[i], "非资格截图须明确区分");
    }
    auto otherSource = Frame(12); otherSource.identity.sourceIdentity = "other";
    analyzer.AddCompletedFrame(otherSource);
    Require(analyzer.Report().sourceTransitions == 1 && !analyzer.Report().visibleRecordComplete,
        "来源切换不能复用资格或混合帧身份");
    auto stale = otherSource; --stale.identity.completedFrameNumber; analyzer.AddCompletedFrame(stale);
    Require(analyzer.Report().staleSamples == 1, "旧完成帧须拒绝");
    auto paused = otherSource; ++paused.identity.completedFrameNumber; paused.monotonicNs += 50000000;
    paused.playing = false; analyzer.AddCompletedFrame(paused);
    Require(analyzer.Report().pausedSamples == 1, "暂停独立统计");
    auto wrongEpoch = Shot(13); wrongEpoch.identity.sourceIdentity = "other"; ++wrongEpoch.identity.viewEpoch;
    Require(analyzer.AddScreenshot(wrongEpoch).classification == FrameScreenshotClassification::IdentityMismatch,
        "不同实际视图 epoch 不能绑定截图");
}
} // namespace

int main() {
    try {
        ActualDataAndLoss(); ScreenshotQualification();
        const std::byte a[]{std::byte{1}, std::byte{2}}, b[]{std::byte{1}, std::byte{3}};
        Require(HashFrameMeasurementBytes(a) == HashFrameMeasurementBytes(a)
            && HashFrameMeasurementBytes(a) != HashFrameMeasurementBytes(b), "实际字节指纹须稳定并识别变化");
        std::cout << "FrameMeasurementTests passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
