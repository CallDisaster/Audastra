#pragma once

#include <QObject>
#include <QString>
#include <memory>

class QJsonObject;
namespace War3Map::Render { struct FrameScreenshotSample; }
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

signals:
    void StatusChanged(const QString& text);
    void Finished(const QString& directory);

private:
    struct State;
    std::unique_ptr<State> state_;
    void Tick();
    void ObserveAccepted();
    void Drain();
    void Capture();
    void CollectCapture(bool wait);
    void RecordCapture(const Render::FrameScreenshotSample& sample, QJsonObject json);
    bool WriteSummary(const QString& reason);
};

} // namespace War3Map::Modules
