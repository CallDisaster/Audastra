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
#include <memory>
#include <string_view>

namespace War3Map::Modules {
class WorldAnimationAssetPrefetch;
class WorldAnimationJobThread;
struct WorldAnimationWork;
struct WorldAnimationChain;
// 测量对照用：接续下一帧时允许的待接收帧数上限（含刚算完的一帧，1–4，默认 2），本进程有效。
void SetAnimationChainBacklogOverride(std::uint32_t frames) noexcept;
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
    std::uint64_t acceptPreparationNs = 0; // 后台为界面接收预先整理点选列表与占位快照的耗时
    // 其中点选列表、占位快照、帧驻留字节各自的耗时；acceptCheck：0 未核对，1 与整份扫描一致，2 不一致。
    std::uint64_t acceptPickingNs = 0, acceptBugSnapshotNs = 0, acceptRetainedBytesNs = 0;
    std::uint8_t acceptCheck = 0;
    // 与等价核对同频取样：帧驻留字节按缓冲类型拆账、进程内存活帧数（memorySampled 为 false 时未取样）。
    bool memorySampled = false;
    Render::AnimationFrameMemory memory;
    std::uint64_t framesAlive = 0;
    // 同次取样、界面接收之后：本服务交出未放回的帧、回收处备用帧、点选历史帧数。
    std::uint64_t serviceFramesLive = 0, serviceFramesSpare = 0, pickHistoryFrames = 0;
    // 点选历史按整帧计费的字节、其中不是当前帧的帧数与字节；占位快照历史字节。
    std::uint64_t pickHistoryBytes = 0, pickHistoryExtraFrames = 0, pickHistoryExtraBytes = 0, bugHistoryBytes = 0;
    std::uint64_t renderPreparationNs = 0; // 后台为插件预先复制姿势/粒子的耗时（插件支持时）
    // 两段流水：求值结束时刻、第二段（接收准备与插件复制）开始时刻。workerTotalNs 是两段实际计算之和。
    std::uint64_t computeFinishedNs = 0, finishStartedNs = 0;
    // 编辑到首帧：本帧所含最早一笔尚未显示的编辑进入队列的时刻，及本帧接收完成时距它的时长；不含编辑为 0。
    std::uint64_t editQueuedNs = 0, editLatencyNs = 0;
    bool directChained = false; // 求值线程凭常驻接续许可、以上一帧为基直接启动的帧
    bool chainSkipped = false; // 接续帧在等刷新间隔时许可被收回，未求值即作废
    // 接续帧开算时晚于固定网格的量；晚一个间隔以上时网格从这次开算重排（chainGridReset）。
    std::uint64_t chainGridLateNs = 0;
    bool chainGridReset = false;
    bool droppedFrameReturned = false; // 未被接收的帧在启动下一帧之前同步交回回收处
    // 本帧消耗播放时钟时：自时钟启动的实际经过，与累计扣除（含落后超过 1 秒时丢弃的部分）；未消耗为 0。
    std::uint64_t clockElapsedNs = 0, clockConsumedNs = 0;
    std::uint64_t animatedInstances = 0, retainedStaticInstances = 0, hiddenInstances = 0;
    std::uint64_t budgetStaticInstances = 0, budgetParticleInstances = 0;
    std::uint64_t offscreenInstances = 0, offscreenPoseInstances = 0, offscreenParticleInstances = 0;
    std::uint64_t visibilityUnknownInstances = 0, frozenParticleInstances = 0;
    // 每次更新整份复制的数据量：实例、组矩阵、粒子顶点与整帧占用字节。
    std::uint64_t frameInstances = 0, frameGroups = 0, frameParticleVertices = 0, frameRetainedBytes = 0;
    // 本帧相对其基准帧的变化实例数；没有基准（整份提交）时 frameChangeBase 为 false。
    std::uint64_t frameChangedInstances = 0;
    bool frameChangeBase = false;
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
    // 预览卡用：全部实例的不循环动作自然播完时从头再来，而不是停在结尾。用户暂停、单步不受影响。
    void SetLoopFinishedActions(bool value) noexcept { loopFinished_ = value; }
    [[nodiscard]] bool Ready() const noexcept { return service_ != nullptr; }
    // 有排队请求或在途求值即忙。接续帧只是在等刷新间隔、且之前各帧都已接收时不算忙（与两帧之间等计时器相同）。
    [[nodiscard]] bool Busy() const;
    // 诊断：仍忙的原因（排队命令或在途求值及接续状态）；不忙时为空。供探针与测试在等待超时时说明卡在哪一步。
    [[nodiscard]] QString BusyReason() const;
    [[nodiscard]] QString LastDiagnostic() const { return lastDiagnostic_; }
    [[nodiscard]] bool CanRetainCurrentScene() const;
    [[nodiscard]] std::shared_ptr<const Render::AnimationFrame> Frame() const noexcept { return accepted_; }
    // 测量用：当前动画服务的帧池计数（在用 / 备用）。
    [[nodiscard]] Render::AnimationFrameCounts ServiceFrameCounts() const noexcept {
        return service_ ? service_->FrameCounts() : Render::AnimationFrameCounts{};
    }
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
    // base 为空时以已接收帧为基；稳定播放时可传入刚算完、尚未接收的帧，使下一帧计算与界面接收重叠。
    void StartNext(std::shared_ptr<const Render::AnimationFrame> base = {});
    // 第一段算完（界面线程）：接替接续帧的在途令牌，作废帧直接结束，其余交第二段线程收尾与接收准备。
    void OnComputed(std::shared_ptr<WorldAnimationWork> output);
    // 第二段完成（界面线程）：按输入代号接收或丢弃本帧。
    void OnFinished(std::shared_ptr<WorldAnimationWork> output);
    // 常驻接续许可：稳定播放时授予；任何输入变化之前收回；相机、预算与间隔变化时发布最新值。
    void GrantChain(const Render::AnimationAdvance& command);
    void RevokeChain();
    void PublishChainInputs(const Render::AnimationView* view);
    // 播放时钟与求值线程接续共用：消耗时只扣整毫秒，余数留给下一帧。
    void StartPlaybackClock();
    [[nodiscard]] bool PlaybackClockValid() const;
    [[nodiscard]] bool PlaybackClockDue() const;
    [[nodiscard]] bool ChainBacklogAllows() const; // 已算完待接收的帧（含刚算完的）未超过上限
    [[nodiscard]] std::uint64_t ConsumePlaybackClockMs(WorldAnimationTiming* timing = nullptr);
    [[nodiscard]] std::uint64_t NextAttemptSerial();
    [[nodiscard]] int PlaybackIntervalMs() const;
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
    // 常驻接续许可、播放时钟与尝试序号：界面线程与求值线程共用，跨 Clear 保留。
    std::shared_ptr<WorldAnimationChain> chain_;
    bool chainGranted_ = false; // 界面线程所见：许可已授予且尚未得知求值线程停止接续
    // 在途求值的身份：常驻线程上的姿势作业是一个令牌对象，实例重基仍是独立线程；非空即忙。
    QPointer<QObject> worker_;
    std::unique_ptr<WorldAnimationJobThread> jobs_; // 首次求值时建立，之后常驻。
    // 第二段常驻线程：按帧顺序做界面接收准备与插件复制，与下一帧的求值重叠。
    std::unique_ptr<WorldAnimationJobThread> finisher_;
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
    std::uint64_t pendingEditNs_ = 0, inFlightEditNs_ = 0; // 尚未显示的最早一笔编辑进入队列的时刻（排队中 / 在途帧）
    std::deque<WorldAnimationTiming> timings_; // 最多 512 个紧凑尝试；跨 Clear 保留取消证据。
    std::deque<WorldInstanceTiming> instanceTimings_;
    std::uint64_t instanceTimingSequence_ = 0;
    WorldAnimationTiming* timingSink_ = nullptr; // 仅 GUI 栈内有效；统计同步 Changed 接收者。
    int changeBatch_ = 0; // 接收一帧期间的多次状态变化合并为结束时一次 Changed。
    bool changeDeferred_ = false;
    bool resumePending_ = false;
    bool restartPending_ = false; // 用户点播放：下一次实际推进让已结束的不循环动作从头开始
    bool instanceActivatesPlayback_ = false; // 仅已接受的名单候选实际包含playAll=true时确认恢复。
    bool clearPending_ = false, instancePollFailed_ = false;
    std::uint64_t serial_ = 0; // 显式状态变更的取消代号；相机合并不改变它。
    bool playing_ = false;
    bool playRequested_ = true; // 用户意图不随清场景、隐藏或异步取消而重置。
    bool loopFinished_ = false;
    bool installing_ = false;
    QString lastDiagnostic_;
    int interval_ = 33;
    Render::AnimationPreviewBudgetController effectBudget_;
    std::uint64_t effectMetricsCursor_ = 0;
};
} // namespace War3Map::Modules
