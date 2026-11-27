#pragma once
#include "WorldTerrainAssets.h"
#include "War3MapCore/Render/ModelAnimation.h"
#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QThread>
#include <QTimer>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace War3Map::Modules {
class WorldViewport;

// 地图天空模型（WarVK 设置的 sky.model，游戏里由 SetSkyModel 设置）单独成一个天空附加层：
// 插件以镜头为原点最先画、不受雾影响、不遮挡场景。模型在后台线程读取；有动作时按 10 帧/秒播放。
// 附加层挂在主模型场景上，主场景换代后重新提交；地图没有任何模型（没有主场景）时不显示。
class WorldSkyLayer final : public QObject {
    Q_OBJECT
public:
    explicit WorldSkyLayer(WorldViewport& viewport, QObject* parent = nullptr);
    ~WorldSkyLayer() override;
    // 显示指定天空模型；模型、来源和会话都相同时只重挂。current 为假时丢弃迟到的读取结果。
    void Show(std::string modelPath, WorldTerrainSources sources, Render::AnimationSession session,
        std::function<bool()> current);
    void RebindParent(); // 主场景或实例名单安装后由宿主调用
    // 主名单事务进行中不得增删改附加层；结束后补交。
    void SetSubmissionDeferred(bool deferred);
    void Clear();
    [[nodiscard]] const std::string& ModelPath() const noexcept { return path_; }
    [[nodiscard]] const std::vector<Diagnostic>& LastDiagnostics() const noexcept { return lastDiagnostics_; }
signals:
    void Message(const QString& text);
protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
private:
    struct Work;
    void Submit();
    void Tick();
    void Remove();
    void UpdateTimer();
    void Report(QString text, std::vector<Diagnostic> diagnostics);
    QPointer<WorldViewport> viewport_;
    QPointer<QThread> worker_;
    QTimer timer_;
    QElapsedTimer clock_;
    std::string path_;
    std::optional<WorldTerrainSources> sources_;
    Render::AnimationSession session_;
    std::function<bool()> current_;
    std::shared_ptr<Render::ModelAnimationService> service_;
    std::shared_ptr<const Render::AnimationFrame> frame_;
    std::uint64_t serial_ = 0, parentEpoch_ = 0, failedEpoch_ = 0;
    bool animated_ = false, submitted_ = false, deferred_ = false, removalPending_ = false;
    QString lastMessage_;
    std::vector<Diagnostic> lastDiagnostics_;
    const std::uint64_t layerId_;
};
} // namespace War3Map::Modules
