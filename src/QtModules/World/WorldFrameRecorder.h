#pragma once

#include <QObject>
#include <QString>
#include <cstdint>
#include <memory>

class QJsonObject;
namespace War3Map::Render { struct FrameScreenshotSample; struct CapturedFrameRgba; }
namespace War3Map::Modules {
class WorldModule;

// 被动采集完成帧；不控制鼠标/相机、不触发编辑或主动要求动画帧。
class WorldFrameRecorder final : public QObject {
    Q_OBJECT
public:
    explicit WorldFrameRecorder(WorldModule& world, QObject* parent = nullptr);
    ~WorldFrameRecorder() override;
    [[nodiscard]] bool Start(const QString& newOutputDir, const QString& sourceMapPath, bool background = false);
    void Stop(const QString& reason = QStringLiteral("作者停止"));
    [[nodiscard]] bool Running() const noexcept;
    void SetPhase(const QString& phase);
    void SetWitnessConfirmed(bool confirmed);
    void SetUnobscured(bool unobscured);
    [[nodiscard]] QString OutputDirectory() const;
    // 测量用：截图在途占用——推迟读回待取的一份像素缓冲及其绑定的动画帧、编码器是否在编。
    struct InFlightCapture { std::uint64_t readbackBytes = 0; bool readbackHoldsFrame = false, encoderBusy = false; };
    [[nodiscard]] InFlightCapture InFlightCaptureUse() const noexcept;

signals:
    void StatusChanged(const QString& text);
    void Finished(const QString& directory);

private:
    struct State;
    std::unique_ptr<State> state_;
    void Tick();
    void ObserveAccepted();
    // final=false 时不消费刚完成、GPU 时间尚未回填的帧（最多等 kGpuSettleNs），停止或读回时 final=true 全部读出。
    void Drain(bool final = false);
    void Capture();
    // 推迟取像素的截图：像素就绪后交编码；wait 时最多等 5 秒（停止测量时用），否则未就绪即返回。
    void CollectReadback(bool wait);
    void SubmitCapture(Render::CapturedFrameRgba&& frame, const Render::FrameScreenshotSample& sample,
        QJsonObject json, std::uint64_t comparisonEpoch);
    void CollectCapture(bool wait);
    void RecordCapture(const Render::FrameScreenshotSample& sample, QJsonObject json);
    bool WriteSummary(const QString& reason);
};

} // namespace War3Map::Modules
