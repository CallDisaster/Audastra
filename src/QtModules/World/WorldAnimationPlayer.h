#pragma once
#include "WorldViewport.h"
#include "WorldTerrainAssets.h"
#include "WorldInstanceTiming.h"
#include <QElapsedTimer>
#include <QPointer>
#include <QThread>
#include <QTimer>
#include <deque>
#include <atomic>
#include <string_view>

namespace War3Map::Modules {
class WorldAnimationAssetPrefetch;
// 世界实例的局部 Stand 相位；不改变共享的全局序列时钟。
[[nodiscard]] std::uint64_t WorldStandStartMs(std::string_view hostKey, std::uint64_t duration) noexcept;
enum class WorldAnimationTimingOutcome { Installed, Accepted, Cancelled, Stale, SourceRejected, PrepareRejected, ViewportRejected };
enum class WorldAnimationSourceCheck { NotRun, Passed, Rejected };
[[nodiscard]] std::string_view WorldAnimationTimingOutcomeName(WorldAnimationTimingOutcome outcome) noexcept;
// 紧凑 CPU 尝试回执，不保留巨大的 AnimationFrame；只有 Installed/Accepted 可绑定完成帧。
struct WorldAnimationTiming {
    std::uint64_t sequence = 0, attemptSerial = 0;
    std::uint64_t sceneVersion = 0, contentVersion = 0, previousContentVersion = 0;
    std::uint64_t generation = 0, documentRevision = 0;
    std::uint64_t queuedNs = 0, workerStartedNs = 0, workerFinishedNs = 0, uiStartedNs = 0;
    std::uint64_t queueWaitNs = 0, workerTotalNs = 0, sourceBeforeNs = 0, sourceAfterNs = 0;
    std::uint64_t uiQueueWaitNs = 0, uiAcceptNs = 0, changedSlotsNs = 0;
    std::uint64_t animatedInstances = 0, retainedStaticInstances = 0, hiddenInstances = 0;
    std::uint64_t budgetStaticInstances = 0, budgetParticleInstances = 0;
    std::uint64_t offscreenInstances = 0, offscreenPoseInstances = 0, offscreenParticleInstances = 0;
    std::uint64_t visibilityUnknownInstances = 0, frozenParticleInstances = 0;
    WorldAnimationTimingOutcome outcome = WorldAnimationTimingOutcome::Cancelled;
    WorldAnimationSourceCheck sourceBefore = WorldAnimationSourceCheck::NotRun;
    WorldAnimationSourceCheck sourceAfter = WorldAnimationSourceCheck::NotRun;
    Render::AnimationFrameTimings frame;
    std::string diagnosticCode;
};
// UI 适配层只安排 worker、显式 dt、来源门与按钮；状态/时钟/求值均委托 Qt-free 服务。
class WorldAnimationPlayer final : public QObject {
    Q_OBJECT
public:
    explicit WorldAnimationPlayer(WorldViewport& viewport, QObject* parent = nullptr);
    ~WorldAnimationPlayer() override;
    bool Install(std::shared_ptr<Render::ModelAnimationService> service,
        std::shared_ptr<const Render::AnimationFrame> first,
        std::shared_ptr<WorldResourceReader> source, std::function<bool()> isCurrent);
    void Clear();
    void SetPlaying(bool value);
    void Step();
    void Edit(Render::AnimationEdit edit);
    void EditMany(std::vector<Render::AnimationEdit> edits);
    void RefreshTransforms();
    struct InstanceChange {
        WorldInstanceTiming timing;
        Render::AnimationInstanceDelta delta;
        std::shared_ptr<const Render::AnimationAssetSnapshot> preloadedAssets;
        WorldTerrainSources sources;
        std::function<bool()> valid;
        std::function<bool(std::uint32_t)> visible;
        std::function<void(bool, const Status&)> completed;
        // owned资产在独立资源worker准备；不能捕获GUI对象或借用当前project父链。
        std::function<Status(Render::AnimationInstanceDelta&, std::shared_ptr<WorldResourceReader>,
            WorldPathingHelperTimings&)> prepareOwnedAssets;
        // 仅资源worker结束后的提交边界调用；不得在valid逐帧路径访问worker正在写的候选。
        std::function<Status()> validateOwnedAssets;
    };
    // 使用安装边界已发布的只读档案立即准备新增资产；最新帧仍在求值边界重基。
    bool QueueInstances(InstanceChange change);
    // 仅预取一个正式模型变体；不持有/移动ghost场景，正式事务仍核验来源与catalog。
    void PrefetchPlacementAsset(const Render::ModelInstanceRequest& request, const WorldTerrainSources& sources,
        std::function<bool()> valid, std::shared_ptr<const Render::AnimationAssetSnapshot> preloaded = {});
    void CancelAssetPrefetch();
    // UI线程的非阻塞只读诊断；worker真正结束与Qt完成通知到达分别处理，不取消在途预取。
    [[nodiscard]] WorldInstanceTiming PlacementAssetPrefetchTiming() const;
    // UI线程补录排队后的同步目录刷新；按完整操作身份匹配，不写worker持有的计时副本。
    bool RecordInstanceOutlinerTiming(const WorldInstanceTiming& identity, std::uint64_t elapsedNs);
    [[nodiscard]] bool InstancesBusy() const noexcept;
    [[nodiscard]] bool Playing() const noexcept { return playing_; }
    [[nodiscard]] bool Ready() const noexcept { return service_ != nullptr; }
    [[nodiscard]] bool Busy() const noexcept { return worker_ != nullptr || pending_.has_value(); }
    [[nodiscard]] QString LastDiagnostic() const { return lastDiagnostic_; }
    [[nodiscard]] bool CanRetainCurrentScene() const;
    [[nodiscard]] std::shared_ptr<const Render::AnimationFrame> Frame() const noexcept { return accepted_; }
    [[nodiscard]] std::shared_ptr<Render::WorldModelScene> Picking() const noexcept { return picking_; }
    [[nodiscard]] std::shared_ptr<const Render::AnimatedModelScene> Scene() const noexcept { return service_ ? service_->Scene() : nullptr; }
    [[nodiscard]] std::optional<WorldAnimationTiming> Timing(std::uint64_t sceneVersion, std::uint64_t contentVersion) const;
    [[nodiscard]] std::vector<WorldAnimationTiming> TimingHistory(std::uint64_t afterSequence = 0) const;
    [[nodiscard]] std::vector<WorldInstanceTiming> InstanceTimingHistory(std::uint64_t afterSequence = 0) const;
    [[nodiscard]] std::uint64_t LatestTimingSequence() const noexcept { return timingSequence_; }
    void SetEffectQuality(Render::AnimationPreviewQuality quality);
    [[nodiscard]] Render::AnimationPreviewQuality EffectQuality() const noexcept { return effectBudget_.Status().quality; }
    void SetTemporaryFullEffects(bool enabled);
    [[nodiscard]] bool TemporaryFullEffects() const noexcept { return effectBudget_.Status().temporaryFull; }
    [[nodiscard]] Render::AnimationPreviewBudgetStatus EffectBudgetStatus() const noexcept { return effectBudget_.Status(); }
signals:
    void Changed();
    // 宿主须同步延迟附加层写入，先于roster事务捕获layersRevision；终态回调之后恢复。
    void InstanceWorkWaitingChanged(bool waiting);
    void Message(const QString& text);
private:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void SetPlayback(bool value);
    void Tick();
    void Queue(Render::AnimationAdvance command, bool advancePlayback = false);
    void StartNext();
    bool StartInstanceWork();
    void PollInstanceWork();
    void FinishInstanceWork(bool accepted, Status status);
    bool Current() const;
    [[nodiscard]] std::shared_ptr<const Render::AnimationPreparedAssets> PreparedPlacementAssets(WorldInstanceTiming& timing,
        const WorldTerrainSources& sources, std::shared_ptr<WorldResourceReader>& reader) const;
    void Fail(const QString& text);
    void RecordTiming(WorldAnimationTiming timing);
    void PublishChanged();
    void RefreshView();
    void PollEffectBudget();
    void RefreshEffectBudget();
    WorldViewport& viewport_;
    QTimer timer_;
    QElapsedTimer elapsed_;
    QPointer<QThread> worker_;
    QPointer<QThread> catalogWorker_;
    std::shared_ptr<std::atomic<bool>> catalogFinished_; // 只读就绪提示；结果仍由Qt完成回调发布。
    std::optional<InstanceChange> instanceChange_;
    std::shared_ptr<const Render::AnimationInstanceDeltaPlan> instancePlan_;
    std::shared_ptr<const Render::ModelAnimationService> instanceCatalog_;
    std::optional<Render::AnimationInstanceDeltaResult> instanceResult_;
    std::shared_ptr<Render::WorldModelScene> instancePicking_;
    QTimer instancePollTimer_;
    std::uint64_t instanceTicket_ = 0, instanceSerial_ = 0;
    std::shared_ptr<Render::ModelAnimationService> service_;
    // 只在服务安装/roster完成时发布；不含可变求值scratch，与旧pose worker独立。
    std::shared_ptr<const Render::ModelAnimationService> catalog_;
    std::shared_ptr<WorldAnimationAssetPrefetch> assetPrefetch_;
    // 仅目录worker串行读取；预取线程不借用它。源配置/租约变化即拒绝复用。
    std::shared_ptr<WorldResourceReader> instanceSource_;
    std::shared_ptr<WorldResourceReader> source_;
    std::shared_ptr<const Render::AnimationFrame> accepted_;
    std::shared_ptr<Render::WorldModelScene> picking_;
    std::function<bool()> isCurrent_;
    std::optional<Render::AnimationAdvance> pending_;
    std::optional<Render::AnimationAdvance> inFlight_;
    bool pendingAdvancePlayback_ = false;
    std::uint64_t pendingQueuedNs_ = 0, timingSequence_ = 0;
    std::deque<WorldAnimationTiming> timings_; // 最多 512 个紧凑尝试；跨 Clear 保留取消证据。
    std::deque<WorldInstanceTiming> instanceTimings_;
    std::uint64_t instanceTimingSequence_ = 0;
    WorldAnimationTiming* timingSink_ = nullptr; // 仅 GUI 栈内有效；统计同步 Changed 接收者。
    bool resumePending_ = false;
    bool restartPending_ = false; // 用户点播放：下一次实际推进让已结束的不循环动作从头开始
    bool instanceActivatesPlayback_ = false; // 仅已接受的名单候选实际包含playAll=true时确认恢复。
    bool clearPending_ = false, instancePollFailed_ = false;
    std::uint64_t serial_ = 0; // 显式状态变更的取消代号；相机合并不改变它。
    std::uint64_t attemptSerial_ = 0; // 每次实际 worker 唯一，跨 Clear 保留测量身份。
    bool playing_ = false;
    bool playRequested_ = true; // 用户意图不随清场景、隐藏或异步取消而重置。
    bool installing_ = false;
    QString lastDiagnostic_;
    int interval_ = 33;
    std::uint64_t recentCostMs_ = 33;
    Render::AnimationPreviewBudgetController effectBudget_;
    std::uint64_t effectMetricsCursor_ = 0;
};
} // namespace War3Map::Modules
