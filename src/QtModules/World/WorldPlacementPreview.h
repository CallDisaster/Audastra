#pragma once
#include "WorldTerrainAssets.h"
#include "WorldPathingHelperPresenter.h"
#include <QObject>
#include <QPointer>
#include <QElapsedTimer>
#include <QTimer>
#include <functional>

class QThread;
namespace War3Map::Modules {
class WorldViewport;
// 单个附加层承载摆放或粘贴组；worker 只准备该层，不捕获世界摆放表。
class WorldPlacementPreview final : public QObject {
    Q_OBJECT
public:
    explicit WorldPlacementPreview(WorldViewport& viewport, QObject* parent);
    ~WorldPlacementPreview() override;
    void Show(std::vector<Render::ModelInstanceRequest> requests, WorldTerrainSources sources,
        std::uint64_t revision, std::function<bool()> current,
        std::vector<std::array<float, 12>> initialTransforms = {},
        std::vector<WorldPlacementPresentationCandidate> presentations = {},
        Services::WorldSelectionVersion presentationVersion = {}, std::string helperResourceIdentity = {});
    void SetTransforms(std::vector<std::array<float, 12>> transforms);
    // 与世界roster排队信号同步连接；锁定期间不得改变主layersRevision。
    void SetSubmissionDeferred(bool deferred);
    // 真正离开/未命中时移除附加层，保留经验证的 CPU 驻留资产；不制造地下实例。
    void Hide();
    void CancelRequest(); // 取消交互/在途接收；保留可验证的 CPU 驻留内容供再次选择。
    void Clear();
    [[nodiscard]] bool ModelSubmitted() const noexcept {
        return submitted_ && !transforms_.empty() && acceptedTransformSerial_ == transformSerial_;
    }
    // 当前CPU已接受的层，仅供诊断；实际可见证据仍须从viewport完成帧查询。
    [[nodiscard]] std::shared_ptr<const Render::AnimationFrame> CurrentFrame() const noexcept { return frame_; }
    [[nodiscard]] std::shared_ptr<const Render::AnimatedModelScene> CurrentScene() const noexcept {
        return service_ ? service_->Scene() : nullptr;
    }
    [[nodiscard]] bool Busy() const noexcept { return worker_ != nullptr; }
    [[nodiscard]] bool PlaybackActive() const noexcept { return playbackTimer_.isActive(); }
    [[nodiscard]] std::shared_ptr<const Render::AnimationAssetSnapshot> PreloadedAssets(
        const WorldTerrainSources& sources) const;
signals:
    void Message(const QString& text);
    void Prepared();
private:
    struct Pending {
        std::vector<Render::ModelInstanceRequest> requests;
        WorldTerrainSources sources;
        std::uint64_t revision = 0;
        std::vector<WorldPlacementPresentationCandidate> presentations;
        Services::WorldSelectionVersion presentationVersion;
        std::string helperResourceIdentity;
    };
    struct Work;
    void BeginNext();
    void RemoveLayer();
    bool PresentCachedTransforms();
    bool eventFilter(QObject* watched, QEvent* event) override;
    void UpdatePlaybackClock();
    void PausePlaybackClock();
    [[nodiscard]] std::uint64_t PlaybackTimeMs() const;
    QPointer<WorldViewport> viewport_;
    QPointer<QThread> worker_;
    std::optional<Pending> pending_;
    std::function<bool()> current_;
    std::vector<std::array<float, 12>> transforms_;
    std::shared_ptr<Render::ModelAnimationService> service_;
    std::shared_ptr<const Render::AnimationFrame> frame_;
    std::shared_ptr<const Render::AnimationAssetSnapshot> preloadedAssets_;
    std::shared_ptr<WorldResourceReader> source_;
    std::optional<WorldTerrainSources> residentSources_;
    std::vector<Render::ModelInstanceRequest> residentRequests_;
    std::string residentPresentationIdentity_;
    std::shared_ptr<const WorldPathingHelperCache> helperCache_;
    std::optional<WorldPathingHelperPrepared> residentHelpers_;
    bool submitted_ = false, needsSubmit_ = false, submissionDeferred_ = false, removePending_ = false;
    bool cameraDependent_ = false;
    bool playbackFaulted_ = false;
    QTimer playbackTimer_;
    QElapsedTimer playbackClock_;
    std::uint64_t clockElapsedMs_ = 0, preparedClockMs_ = 0, requestedClockMs_ = 0;
    std::optional<War3RenderCore::LocalCamera> preparedCamera_;
    std::uint64_t preparedTransformSerial_ = 0;
    std::uint64_t serial_ = 0, transformSerial_ = 0, acceptedTransformSerial_ = 0;
    const std::uint64_t layerId_;
};
}
