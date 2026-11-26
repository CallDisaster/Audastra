#include "WorldAnimationPlayer.h"
#include <QEvent>
#include <QScopeGuard>
#include <QScopedValueRollback>
#include <QScreen>
#include "WorldFrameRelease.h"
#include "WorldPlaybackClock.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <limits>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <Windows.h>

namespace War3Map::Modules {
namespace {
// 待接收帧数上限（含刚算完的一帧）：求值线程直接接续与界面线程接续共用；默认 2，测量对照可改。
std::atomic<std::uint32_t> chainBacklogLimit{2};
}
void SetAnimationChainBacklogOverride(std::uint32_t frames) noexcept {
    chainBacklogLimit.store(std::clamp(frames, 1u, 4u), std::memory_order_relaxed);
}
// 常驻动画计算线程：串行执行界面线程投递的求值作业，不再每帧新建线程。
// 线程只持有共享队列；播放器析构时通知退出、丢弃未开始的作业，不等待在途作业，线程结束后自行回收。
// 完成通知投递给线程自带的界面线程对象，它在线程结束后才删除，所以在途作业总能安全投递；
// 通知本身再判断播放器是否还在。
class WorldAnimationJobThread final {
public:
    WorldAnimationJobThread() : state_(std::make_shared<State>()) {
        state_->receiver = new QObject;
        auto* thread = QThread::create([state = state_] { Run(*state); });
        QObject::connect(thread, &QThread::finished, state_->receiver, &QObject::deleteLater);
        QObject::connect(thread, &QThread::finished, thread, &QObject::deleteLater);
        thread->start(QThread::LowPriority);
    }
    ~WorldAnimationJobThread() {
        std::deque<std::function<void()>> dropped; // 未开始作业持有的帧在锁外释放
        { std::lock_guard lock(state_->mutex); state_->stop = true; dropped.swap(state_->jobs); }
        state_->wake.notify_all();
    }
    WorldAnimationJobThread(const WorldAnimationJobThread&) = delete;
    WorldAnimationJobThread& operator=(const WorldAnimationJobThread&) = delete;
    // 只投递作业：完成通知由作业自己投给 Receiver()（接续求值时一次作业连出多帧，每帧各投一次）。
    void Post(std::function<void()> job) {
        { std::lock_guard lock(state_->mutex); state_->jobs.push_back(std::move(job)); }
        state_->wake.notify_one();
    }
    // 线程自带的界面线程对象：线程结束后才删除，在途作业可随时向它投递完成通知。
    [[nodiscard]] QObject* Receiver() const noexcept { return state_->receiver; }
    // job 在常驻线程执行；之后 done 回到界面线程执行。
    void Post(std::function<void()> job, std::function<void()> done) {
        auto task = [job = std::move(job), done = std::move(done), receiver = state_->receiver]() mutable {
            job();
            QMetaObject::invokeMethod(receiver, std::move(done), Qt::QueuedConnection);
        };
        { std::lock_guard lock(state_->mutex); state_->jobs.push_back(std::move(task)); }
        state_->wake.notify_one();
    }
private:
    struct State {
        std::mutex mutex;
        std::condition_variable wake;
        std::deque<std::function<void()>> jobs;
        QObject* receiver = nullptr;
        bool stop = false;
    };
    static void Run(State& state) noexcept {
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock lock(state.mutex);
                state.wake.wait(lock, [&] { return state.stop || !state.jobs.empty(); });
                if (state.stop) return;
                job = std::move(state.jobs.front()); state.jobs.pop_front();
            }
            try { job(); } catch (...) {} // 作业自行把失败写进结果；这里只保证线程不因异常退出。
        }
    }
    std::shared_ptr<State> state_;
};
// 一帧求值作业。启动方（界面线程，或接续时的求值线程）写好命令与身份后交给求值线程；求值线程写结果，
// 第二段线程收尾与接收准备，界面线程接收。
struct WorldAnimationWork {
    Result<std::shared_ptr<const Render::AnimationFrame>> result;
    // 帧的收尾（指纹、运动摘要、点选包围盒）：第一段算完即可作下一帧的基，收尾在第二段线程、接收准备之前做。
    std::function<Status()> finalize;
    WorldAnimationTiming timing;
    WorldViewport::AnimationFramePreparation prepared; // 点选列表与占位快照在后台算好，界面线程只交换
    // 启动时写好、之后只读（接续帧的 command 由求值线程在等到刷新间隔后写入，界面线程只读 planned）：
    Render::AnimationAdvance command, planned;
    std::shared_ptr<const Render::AnimatedModelScene> scene;
    std::uint64_t version = 0; // 本帧所属的输入代号；接收时与界面线程当前代号不同即作废
    bool chainable = false, activatesPlayback = false;
    bool skipped = false; // 接续帧在等刷新间隔时许可被收回：未求值，按作废结束
    // 求值线程在投递本帧完成之前写好：凭常驻许可以本帧为基直接接续的下一帧；空 = 未接续。
    std::shared_ptr<WorldAnimationWork> next;
    // 只由界面线程读写：本帧的在途令牌。接续帧计算期间由界面线程写入，求值线程不读此成员。
    QPointer<QObject> token;
};
// 接续帧的起步等待：高精度可等待计时器（不改系统计时器精度）等到截止时刻，收回许可时由界面线程唤醒。
// 计时器不可用时退回普通睡眠。唤醒只是提示，调用方醒来后总要重新核对许可与时钟。
class WorldAnimationPacer final {
public:
    WorldAnimationPacer() noexcept
        : timer_(CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS)),
          wake_(CreateEventW(nullptr, FALSE, FALSE, nullptr)) {}
    ~WorldAnimationPacer() { if (timer_) CloseHandle(timer_); if (wake_) CloseHandle(wake_); }
    WorldAnimationPacer(const WorldAnimationPacer&) = delete;
    WorldAnimationPacer& operator=(const WorldAnimationPacer&) = delete;
    void Wake() noexcept { if (wake_) SetEvent(wake_); }
    // 等 remainingNs 纳秒，或被 Wake 提前唤醒。
    void Wait(std::int64_t remainingNs) noexcept {
        if (remainingNs <= 0) return;
        if (timer_ && wake_) {
            LARGE_INTEGER due; due.QuadPart = -std::max<std::int64_t>(remainingNs / 100, 1); // 相对时间，100 ns 单位
            if (SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE)) {
                const HANDLE handles[2] = {timer_, wake_};
                (void)WaitForMultipleObjects(2, handles, FALSE, INFINITE);
                return;
            }
        }
        if (wake_) (void)WaitForSingleObject(wake_, static_cast<DWORD>((remainingNs + 999'999) / 1'000'000));
        else std::this_thread::sleep_for(std::chrono::nanoseconds(remainingNs));
    }
private:
    HANDLE timer_ = nullptr, wake_ = nullptr;
};
// 常驻接续许可。界面线程在稳定播放时授予，任何输入变化（编辑、拖动时间、调速、暂停、显隐、实例增删、重播）
// 之前先收回；求值线程算完一帧即凭它以本帧为基直接接下一帧，不经界面线程往返。接续帧带着授予时的输入代号，
// 接收时与当前代号不符即作废。经过时间取自同一播放时钟；相机与物理视口、粒子预算、刷新间隔由界面线程随变化
// 发布，接续时读最新一份。全部成员受 mutex 保护。
struct WorldAnimationChain {
    std::mutex mutex;
    bool granted = false;
    std::uint64_t version = 0;
    std::optional<Render::AnimationView> view;
    std::optional<Render::AnimationParticleFrameBudget> budget;
    std::int64_t intervalNs = 33'000'000;
    WorldPlaybackClock clock; // 播放时钟（steady 纳秒）
    WorldChainGrid grid; // 接续帧固定网格；界面线程消耗时钟或时钟重启时重置
    std::uint64_t attempts = 0; // 求值尝试序号：界面线程启动与求值线程接续共用，跨 Clear 保留
    // 已算完、界面线程尚未接收或丢弃的帧数；积压超过一帧时不接续，界面线程卡住时不越算越多。
    std::uint32_t outstanding = 0;
    bool waiting = false; // 求值线程正在为接续帧等刷新间隔（尚未开算）
    // 诊断计数（只增不减）：算完投递 / 界面收到，交给收尾线程 / 收尾开始 / 收尾结束 / 收尾完成界面收到。
    std::atomic<std::uint64_t> computedPosted{0}, computedDelivered{0}, finisherPosted{0}, finisherStarted{0},
        finisherEnded{0}, finishedDelivered{0};
    WorldAnimationPacer pacer; // 接续帧等刷新间隔；收回许可时唤醒（不受 mutex 保护，自身线程安全）
    // 消耗播放时钟（调用方持锁），并把这次消耗时的实际经过与累计扣除记入本帧计时。
    std::uint64_t ConsumeMs(std::int64_t now, WorldAnimationTiming* timing) noexcept {
        const auto ms = clock.Consume(now);
        if (timing && clock.Valid()) {
            timing->clockElapsedNs = static_cast<std::uint64_t>(clock.ElapsedNs(now)); timing->clockConsumedNs = clock.ConsumedNs();
        }
        return ms;
    }
};
namespace {
std::uint64_t CpuStampNs() noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
// retainedBytes：是否在此统计帧驻留字节（遍历全部姿势链）。求值线程不统计，改由后台接收准备算好后记入。
void FrameTiming(WorldAnimationTiming& timing, const Render::AnimationFrame& frame, bool retainedBytes) {
    timing.sceneVersion=frame.sceneVersion; timing.contentVersion=frame.contentVersion;
    timing.documentRevision=frame.documentRevision; timing.frame=frame.timings;
    timing.animatedInstances=frame.animatedInstances; timing.retainedStaticInstances=frame.retainedStaticInstances;
    timing.hiddenInstances=frame.hiddenInstances; timing.budgetStaticInstances=frame.budgetStaticInstances;
    timing.budgetParticleInstances=frame.budgetParticleInstances;
    timing.offscreenInstances=frame.offscreenInstances; timing.offscreenPoseInstances=frame.offscreenPoseInstances;
    timing.offscreenParticleInstances=frame.offscreenParticleInstances; timing.visibilityUnknownInstances=frame.visibilityUnknownInstances;
    timing.frozenParticleInstances=std::count_if(frame.particleLinks.begin(),frame.particleLinks.end(),[](const auto& p) { return p.frozen; });
    timing.frameInstances=frame.instances.size(); timing.frameGroups=frame.groups.size();
    timing.frameParticleVertices=frame.particleVertices.size();
    if (retainedBytes) timing.frameRetainedBytes=Render::AnimationFrameRetainedBytes(frame);
    timing.frameChangedInstances=frame.changedInstances.size(); timing.frameChangeBase=frame.changeBaseContentVersion!=0;
}
std::string DiagnosticCode(const std::vector<Diagnostic>& diagnostics) {
    const auto first=std::find_if(diagnostics.begin(),diagnostics.end(),[](const auto& d) { return d.severity==DiagnosticSeverity::Error; });
    return first==diagnostics.end() ? std::string{} : first->code;
}
QString Error(const std::vector<Diagnostic>& d) {
    const auto first = std::find_if(d.begin(), d.end(), [](const auto& item) { return item.severity == DiagnosticSeverity::Error; });
    const auto* diagnostic = first != d.end() ? &*first : (d.empty() ? nullptr : &d.front());
    return diagnostic ? QString::fromUtf8(diagnostic->code + "：" + diagnostic->message) : QObject::tr("动画操作失败");
}
bool SameView(const Render::AnimationView& first, const Render::AnimationView& second) {
    const auto& a=first.camera; const auto& b=second.camera;
    return first.physicalWidth==second.physicalWidth && first.physicalHeight==second.physicalHeight
        && first.conservativeMargin==second.conservativeMargin && a.targetX==b.targetX && a.targetY==b.targetY
        && a.targetZ==b.targetZ && a.distance==b.distance && a.yawRadians==b.yawRadians && a.pitchRadians==b.pitchRadians;
}
bool NeedsView(const Render::AnimationFrame& frame, const Render::AnimationView& current) {
    return !frame.worldView || !SameView(*frame.worldView,current);
}
std::int64_t SteadyNs() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
// 第一段（求值线程）：来源复核与整帧求值，写进 output。算完即可作下一帧的基；收尾与接收准备交给第二段线程。
void ComputeAnimationWork(Render::ModelAnimationService& service, const Render::AnimationFrame* previous,
    WorldResourceReader* source, WorldAnimationWork& output) {
    auto& timing=output.timing;
    timing.workerStartedNs=CpuStampNs(); timing.queueWaitNs=timing.workerStartedNs-timing.queuedNs;
    try {
        bool sourceValid=true;
        if (source) {
            const auto started=CpuStampNs();
            // 逐帧只在来源目录有变化通知或满 1 秒时完整核对（见 VerifyCurrentSourceForFrame）。
            auto valid = source->VerifyCurrentSourceForFrame();
            timing.sourceBeforeNs=CpuStampNs()-started;
            timing.sourceBefore=valid.Succeeded() ? WorldAnimationSourceCheck::Passed : WorldAnimationSourceCheck::Rejected;
            if (!valid.Succeeded()) {
                output.result.diagnostics = std::move(valid.diagnostics);
                timing.outcome=WorldAnimationTimingOutcome::SourceRejected;
                sourceValid=false;
            }
        }
        if (sourceValid) {
            auto staged = service.PrepareStaged(previous, output.command);
            if (staged.Succeeded()) {
                output.finalize = std::move(staged.value->finalize);
                output.result = Result<std::shared_ptr<const Render::AnimationFrame>>::Success(std::move(staged.value->frame));
            } else output.result = {std::nullopt, std::move(staged.diagnostics)};
        }
        if (output.result.Succeeded()) FrameTiming(timing,**output.result.value,false);
        if (source && output.result.Succeeded()) {
            const auto started=CpuStampNs();
            const auto valid = source->VerifyCurrentSourceForFrame();
            timing.sourceAfterNs=CpuStampNs()-started;
            timing.sourceAfter=valid.Succeeded() ? WorldAnimationSourceCheck::Passed : WorldAnimationSourceCheck::Rejected;
            if (!valid.Succeeded()) {
                output.result = {std::nullopt, valid.diagnostics};
                output.finalize = nullptr;
                timing.outcome=WorldAnimationTimingOutcome::SourceRejected;
            }
        }
    } catch (const std::exception&) {
        output.result = Result<std::shared_ptr<const Render::AnimationFrame>>::Failure(
            "World", "WORLD_ASSET_ANIMATION_WORK_FAILED", "动画来源复核或后台计算异常，旧帧未提交。");
    }
    timing.computeFinishedNs=CpuStampNs();
}
// 求值线程：本帧算完后凭常驻许可计划下一帧——先定下身份（输入代号、尝试序号），随本帧完成通知交给界面线程接替
// 在途令牌；命令到开算前才定。许可已收回、代号已变、时钟未启动、界面积压或本帧失败时不接续，交回界面线程照常安排。
std::shared_ptr<WorldAnimationWork> PlanAnimationChain(WorldAnimationChain& chain,
    const Render::ModelAnimationService& service, const WorldAnimationWork& done) {
    if (!done.chainable || !done.result.Succeeded()) return {};
    auto next = std::make_shared<WorldAnimationWork>();
    next->scene = service.Scene();
    {
        std::lock_guard lock(chain.mutex);
        // outstanding 不含刚算完的这一帧（随后才计入）：连同它不超过上限才接续。
        if (!chain.granted || chain.version != done.version || !chain.clock.Valid() || !chain.view ||
            chain.outstanding + 1 > chainBacklogLimit.load(std::memory_order_relaxed)) return {};
        next->planned.worldView = chain.view;
        next->version = chain.version;
        next->timing.attemptSerial = ++chain.attempts;
    }
    next->planned.worldCamera = Render::AnimationWorldCamera(next->planned.worldView->camera);
    next->chainable = true;
    auto& timing = next->timing;
    timing.directChained = true;
    timing.sceneVersion = next->scene->sceneVersion; timing.generation = next->scene->generation;
    timing.documentRevision = next->scene->documentRevision;
    timing.previousContentVersion = (*done.result.value)->contentVersion;
    return next;
}
// 求值线程：接续帧开算之前等到播放时钟满一个刷新间隔（许可收回时立即醒来），与界面线程稳定播放时的节奏相同。
// 仍获许可则消耗播放时钟、取最新发布的相机与预算定下命令并返回 true；否则返回 false，本帧不求值、按作废结束。
bool AwaitAnimationChain(WorldAnimationChain& chain, WorldAnimationWork& work) {
    for (;;) {
        std::int64_t remaining = 0;
        {
            std::lock_guard lock(chain.mutex);
            chain.waiting = false;
            if (!chain.granted || chain.version != work.version || !chain.view) return false;
            const auto now = SteadyNs();
            const auto due = chain.grid.DueNs(chain.clock.clockNs + chain.intervalNs, chain.intervalNs);
            remaining = due - now;
            chain.waiting = remaining > 0;
            if (remaining <= 0) {
                const auto late = chain.grid.Start(due, now, chain.intervalNs);
                work.timing.chainGridLateNs = static_cast<std::uint64_t>(late);
                work.timing.chainGridReset = late >= chain.intervalNs;
                work.command.deltaMs = chain.ConsumeMs(now, &work.timing);
                work.command.worldView = chain.view;
                work.command.previewParticleBudget = chain.budget;
                break;
            }
        }
        chain.pacer.Wait(remaining);
    }
    work.command.worldCamera = Render::AnimationWorldCamera(work.command.worldView->camera);
    work.timing.queuedNs = CpuStampNs();
    return true;
}
} // namespace
std::string_view WorldAnimationTimingOutcomeName(WorldAnimationTimingOutcome outcome) noexcept {
    switch (outcome) {
    case WorldAnimationTimingOutcome::Installed: return "installed";
    case WorldAnimationTimingOutcome::Accepted: return "accepted";
    case WorldAnimationTimingOutcome::Cancelled: return "cancelled";
    case WorldAnimationTimingOutcome::Stale: return "stale";
    case WorldAnimationTimingOutcome::SourceRejected: return "source_rejected";
    case WorldAnimationTimingOutcome::PrepareRejected: return "prepare_rejected";
    case WorldAnimationTimingOutcome::ViewportRejected: return "viewport_rejected";
    }
    return "unknown";
}
std::uint64_t WorldStandStartMs(std::string_view hostKey, std::uint64_t duration) noexcept {
    std::uint64_t hash = 14695981039346656037ull;
    for (const unsigned char value : hostKey) { hash ^= value; hash *= 1099511628211ull; }
    return duration ? hash % duration : 0;
}
WorldAnimationPlayer::WorldAnimationPlayer(WorldViewport& viewport, QObject* parent)
    : QObject(parent), viewport_(viewport), chain_(std::make_shared<WorldAnimationChain>()) {
    timer_.setInterval(interval_);
    timer_.setTimerType(Qt::PreciseTimer);
    viewport_.installEventFilter(this);
    connect(&timer_, &QTimer::timeout, this, &WorldAnimationPlayer::Tick);
    connect(&viewport_, &WorldViewport::AnimationTransformsChanged, this,
            &WorldAnimationPlayer::RefreshTransforms);
    connect(&viewport_, &WorldViewport::AnimationCameraChanged, this, [this] {
        RefreshView();
    });
    connect(&viewport_, &WorldViewport::StatusChanged, this, [this](const QString& text) {
        if (!installing_ && service_ && (viewport_.RendererFailed() || text.startsWith(tr("模型上传失败："))))
            Fail(text);
    });
    connect(this, &WorldAnimationPlayer::Message, this, [this](const QString& text) {
        lastDiagnostic_ = text.left(4096);
        PublishChanged(); // Clear后的控件也须保留原因，不能只剩无解释的禁用。
    });
}
WorldAnimationPlayer::~WorldAnimationPlayer() {
    timer_.stop();
    CancelAssetPrefetch();
    RevokeChain();
    ++serial_;
}
void WorldAnimationPlayer::SetEffectQuality(Render::AnimationPreviewQuality quality) {
    if (Render::AnimationPreviewQualityName(quality).empty() || EffectQuality() == quality) return;
    effectBudget_.SetQuality(quality);
    if (accepted_) effectBudget_.SetOmissions(accepted_->budgetParticleInstances, accepted_->budgetParticleEmitters);
    RefreshEffectBudget();
}
void WorldAnimationPlayer::SetTemporaryFullEffects(bool enabled) {
    if (TemporaryFullEffects() == enabled) return;
    effectBudget_.SetTemporaryFull(enabled); RefreshEffectBudget();
}
int WorldAnimationPlayer::PlaybackIntervalMs() const {
    const auto status = effectBudget_.Status();
    auto interval = static_cast<int>(status.minimumIntervalMs);
    // 省电档保持其最低间隔；其余档按所在屏幕的刷新节奏出姿势，不再按计算耗时拉长间隔。
    // 计算慢于刷新时由“算完即接下一帧”决定实际更新率。
    if (status.quality != Render::AnimationPreviewQuality::PowerSaving || status.temporaryFull) {
        const auto* screen = viewport_.screen();
        const double rate = screen ? screen->refreshRate() : 60.0;
        if (std::isfinite(rate) && rate >= 30.0)
            interval = std::min(interval, std::max(4, static_cast<int>(std::floor(1000.0 / rate))));
    }
    return interval;
}
void WorldAnimationPlayer::RefreshEffectBudget() {
    interval_ = PlaybackIntervalMs();
    timer_.setInterval(interval_);
    if (Current()) { Render::AnimationAdvance refresh; refresh.playAll = playing_; Queue(std::move(refresh)); }
    PublishChanged();
}
void WorldAnimationPlayer::PollEffectBudget() {
    if (!accepted_ || !viewport_.isVisible()) return;
    const auto page = viewport_.ReadFrameMetrics(effectMetricsCursor_);
    if (!page.Succeeded()) return; // 能力不可用不补造计时，播放器仍可使用明确的初始档位。
    if (page.value->page.latestFrame < effectMetricsCursor_) {
        effectMetricsCursor_ = 0;
        const bool full = TemporaryFullEffects();
        effectBudget_.ResetMeasurements(); effectBudget_.SetTemporaryFull(full);
        effectBudget_.SetOmissions(accepted_->budgetParticleInstances, accepted_->budgetParticleEmitters);
        return;
    }
    const auto add = [](std::uint64_t a, std::uint64_t b) {
        return b > std::numeric_limits<std::uint64_t>::max() - a ? std::numeric_limits<std::uint64_t>::max() : a + b;
    };
    bool changed = false, statusChanged = false;
    for (const auto& record : page.value->records) {
        const auto& completed = record.completed;
        if (record.structSize < sizeof(record) || completed.structSize < sizeof(completed)) continue;
        effectMetricsCursor_ = std::max(effectMetricsCursor_, completed.view.frameCount);
        if (completed.view.structSize < sizeof(completed.view) || completed.skin.structSize < sizeof(completed.skin) ||
            completed.skin.sceneVersion != accepted_->sceneVersion || completed.skin.contentVersion != accepted_->contentVersion ||
            completed.view.documentRevision != accepted_->documentRevision || record.result != 0 || record.presentResult != 0 ||
            (record.flags & (WAR3_RENDER_FRAME_METRICS_DRAW_COMPLETED | WAR3_RENDER_FRAME_METRICS_UPLOAD_VALID |
                WAR3_RENDER_FRAME_METRICS_WINDOW_ATTACHED)) !=
                (WAR3_RENDER_FRAME_METRICS_DRAW_COMPLETED | WAR3_RENDER_FRAME_METRICS_UPLOAD_VALID | WAR3_RENDER_FRAME_METRICS_WINDOW_ATTACHED) ||
            (record.flags & WAR3_RENDER_FRAME_METRICS_PRESENT_OCCLUDED)) continue;
        if (service_->Scene()->particlesEnabled && service_->Scene()->particleInstanceCount &&
            (completed.particles.structSize < sizeof(completed.particles) ||
                completed.particles.sceneVersion != accepted_->sceneVersion || completed.particles.contentVersion != accepted_->contentVersion)) continue;
        Render::AnimationPreviewBudgetSample sample;
        sample.frameNumber = completed.view.frameCount; sample.contentVersion = completed.skin.contentVersion;
        sample.completedSteadyNs = record.completedSteadyNs; sample.completed = sample.visible = true;
        sample.completedCpuNs = add(add(record.uploadCpuNs, record.drawCpuNs), record.presentCpuNs);
        sample.animationCpuNs = accepted_->timings.prepareNs;
        if (const auto timing = Timing(accepted_->sceneVersion, accepted_->contentVersion))
            sample.animationCpuNs = add(timing->workerTotalNs, timing->uiAcceptNs);
        if (record.flags & WAR3_RENDER_FRAME_METRICS_GPU_TIME_AVAILABLE) sample.gpuDrawNs = record.gpuDrawNs;
        const auto before = effectBudget_.Status();
        changed |= effectBudget_.Observe(sample);
        const auto after = effectBudget_.Status();
        // 普通帧接受已有Changed；只为证据从未知变已知或档位改变补一次通知。
        statusChanged |= after.hasCompletedFrameSample != before.hasCompletedFrameSample ||
            after.gpuDrawNs.has_value() != before.gpuDrawNs.has_value();
    }
    if (changed && !playing_) RefreshEffectBudget();
    else if (changed || statusChanged) PublishChanged();
}
std::optional<WorldAnimationTiming> WorldAnimationPlayer::Timing(std::uint64_t sceneVersion, std::uint64_t contentVersion) const {
    for (auto it=timings_.rbegin();it!=timings_.rend();++it)
        if (it->sceneVersion==sceneVersion && it->contentVersion==contentVersion &&
            (it->outcome==WorldAnimationTimingOutcome::Installed || it->outcome==WorldAnimationTimingOutcome::Accepted)) return *it;
    return std::nullopt;
}
std::vector<WorldAnimationTiming> WorldAnimationPlayer::TimingHistory(std::uint64_t afterSequence) const {
    std::vector<WorldAnimationTiming> result;
    for (const auto& timing:timings_) if (timing.sequence>afterSequence) result.push_back(timing);
    return result;
}
void WorldAnimationPlayer::RecordTiming(WorldAnimationTiming timing) {
    timing.sequence=++timingSequence_;
    timings_.push_back(std::move(timing));
    if (timings_.size()>512) timings_.pop_front();
}
void WorldAnimationPlayer::PublishChanged() {
    if (changeBatch_) { changeDeferred_ = true; return; }
    if (!timingSink_) { emit Changed(); return; }
    const auto begin=CpuStampNs();
    emit Changed();
    timingSink_->changedSlotsNs+=CpuStampNs()-begin;
}
bool WorldAnimationPlayer::Install(std::shared_ptr<Render::ModelAnimationService> service,
                                   std::shared_ptr<const Render::AnimationFrame> first,
                                   std::shared_ptr<WorldResourceReader> source,
                                   std::function<bool()> current) {
    if (!service || !first || (current && !current()))
        return false;
    if (clearPending_ || instanceTicket_) {
        emit Message(tr("实例事务仍在等待终态，暂缓安装另一个场景"));
        return false;
    }
    WorldAnimationTiming timing;
    QScopedValueRollback<WorldAnimationTiming*> timingScope(timingSink_,&timing);
    timing.uiStartedNs=CpuStampNs(); timing.generation=service->Scene()->generation;
    FrameTiming(timing,*first,true);
    // 新服务尚未启动求值；仅在此边界捕获一次，后续摆放不再等待旧pose worker。
    auto catalog = service != service_ ? service->CaptureInstanceCatalog()
        : catalog_ ? Result<std::shared_ptr<const Render::ModelAnimationService>>::Success(catalog_)
        : Result<std::shared_ptr<const Render::ModelAnimationService>>::Failure("World.Instance",
            "WORLD_INSTANCE_CATALOG_STALE", "当前服务没有安装边界的只读档案，未在求值中捕获。");
    if (!catalog.Succeeded()) {
        timing.outcome=WorldAnimationTimingOutcome::PrepareRejected;
        timing.diagnosticCode=DiagnosticCode(catalog.diagnostics);
        emit Message(tr("动画实例档案准备失败：%1").arg(Error(catalog.diagnostics)));
        timing.uiAcceptNs=CpuStampNs()-timing.uiStartedNs;
        RecordTiming(std::move(timing));
        return false;
    }
    // 提交新场景成功之前不撤销同来源的已接收服务；失败不把整片世界变为只读。
    if (service_ && !CanRetainCurrentScene()) Fail(tr("地图或动画来源已变化，旧场景已失效"));
    auto picking = Render::AnimationPickingScene(*service->Scene(), *first);
    installing_ = true;
    const bool accepted = viewport_.SetAnimationScene(service->Scene(), first, picking);
    installing_ = false;
    if (!accepted) {
        timing.outcome=WorldAnimationTimingOutcome::ViewportRejected;
        timing.diagnosticCode=DiagnosticCode(viewport_.LastAnimationFailure().diagnostics);
        emit Message(tr("动画场景接收失败：%1").arg(Error(viewport_.LastAnimationFailure().diagnostics)));
        timing.uiAcceptNs=CpuStampNs()-timing.uiStartedNs;
        RecordTiming(std::move(timing));
        return false;
    }
    Clear(); // 此时视口已原子接收新场景；旧在途任务只会因 serial 不同被丢弃。
    service_ = std::move(service);
    catalog_ = std::move(*catalog.value);
    source_ = std::move(source);
    accepted_ = std::move(first);
    picking_ = std::move(picking);
    isCurrent_ = std::move(current);
    lastDiagnostic_.clear();
    effectBudget_.SetOmissions(accepted_->budgetParticleInstances, accepted_->budgetParticleEmitters);
    interval_ = PlaybackIntervalMs();
    PublishChanged();
    if (playRequested_) SetPlayback(true);
    if (Current() && accepted_ && NeedsView(*accepted_,viewport_.AnimationViewSnapshot())) {
        Render::AnimationAdvance refresh; refresh.playAll=playing_;
        Queue(std::move(refresh)); // 后台加载期间可能已换相机，安装后补一次当前视角。
    }
    timing.outcome=WorldAnimationTimingOutcome::Installed;
    timing.uiAcceptNs=CpuStampNs()-timing.uiStartedNs;
    RecordTiming(std::move(timing));
    return true;
}
void WorldAnimationPlayer::Clear() {
    timer_.stop();
    CancelAssetPrefetch();
    catalog_.reset(); // 即使GPU终态尚待drain，也不再向新请求暴露旧档案。
    instanceSource_.reset();
    playing_ = false;
    RevokeChain();
    ++serial_;
    pending_.reset(); pendingAdvancePlayback_ = false; pendingQueuedNs_ = 0; inFlight_.reset();
    pendingEditNs_ = inFlightEditNs_ = 0;
    if (instanceTicket_) {
        const bool firstClear = !clearPending_;
        clearPending_ = true;
        const auto cancelled = viewport_.CancelPendingAnimationRoster(instanceTicket_);
        if (!cancelled.Succeeded() || cancelled.value->state == WAR3_RENDER_ROSTER_QUEUED) {
            // 旧插件/查询错误/已Publish待Present都继续拥有候选；不让Install替换桥或晚提交悬空。
            instancePollTimer_.start();
            if (firstClear) emit Message(tr("正在等待实例事务取消或完成，旧画面与拥有者保留"));
            PublishChanged();
            return;
        }
        const bool completed = cancelled.value->state == WAR3_RENDER_ROSTER_COMPLETED && instanceResult_;
        if (completed && instanceChange_) instanceChange_->timing.completed = cancelled.value->completed;
        if (completed) {
            service_ = std::move(instanceResult_->service); accepted_ = std::move(instanceResult_->frame);
            catalog_ = std::move(instanceCatalog_);
            picking_ = std::move(instancePicking_);
        }
        FinishInstanceWork(completed, completed ? Status{} : Status::Failure("World.Instance",
            "WORLD_INSTANCE_CANCELLED", "实例事务已明确取消，未发布候选。"));
    } else if (instanceChange_) {
        // 仅CPU准备阶段没有GPU票据；失效代号会拒绝随后到达的worker结果。
        clearPending_ = true;
        FinishInstanceWork(false, Status::Failure("World.Instance", "WORLD_INSTANCE_CANCELLED", "实例CPU准备已取消。"));
    }
    clearPending_ = false; instancePollFailed_ = false;
    ++instanceSerial_; instancePollTimer_.stop();
    instanceChange_.reset(); instancePlan_.reset(); instanceCatalog_.reset(); instanceResult_.reset(); instancePicking_.reset(); instanceTicket_ = 0;
    pending_.reset();
    pendingAdvancePlayback_ = false;
    pendingQueuedNs_=0;
    inFlight_.reset();
    pendingEditNs_ = inFlightEditNs_ = 0;
    resumePending_ = false;
    service_.reset();
    catalog_.reset();
    source_.reset();
    accepted_.reset();
    picking_.reset();
    isCurrent_ = {};
    effectBudget_.ResetMeasurements(); effectMetricsCursor_ = 0;
    emit InstanceWorkWaitingChanged(false); // owner终态消费和来源清理之后才解除附加层延期。
    PublishChanged();
}
bool WorldAnimationPlayer::Current() const {
    return service_ && (!isCurrent_ || isCurrent_());
}
bool WorldAnimationPlayer::CanRetainCurrentScene() const try {
    return Current() && accepted_ && picking_ && viewport_.AnimationTarget() == accepted_ && viewport_.ModelPresentationCurrent()
        && !viewport_.RendererFailed() && (!source_ || source_->VerifyCurrentSource().Succeeded());
} catch (const std::exception&) {
    return false;
}
void WorldAnimationPlayer::Fail(const QString& text) {
    Clear();
    // 保留已经接收的悬崖/模型；旧姿势只读，宿主按同代来源安排静态回退。
    viewport_.InvalidateAnimationPicking();
    emit Message(text);
}
void WorldAnimationPlayer::SetPlaying(bool value) {
    if (value) RevokeChain(); // 用户点播放：下一帧带重播意图，求值线程不再按旧输入接续
    playRequested_ = value;
    restartPending_ = value;
    SetPlayback(value);
}
bool WorldAnimationPlayer::eventFilter(QObject* watched, QEvent* event) {
    if (watched == &viewport_) {
        if (event->type() == QEvent::Hide) SetPlayback(false);
        else if (event->type() == QEvent::Show)
            QTimer::singleShot(0, this, [this] { if (playRequested_) SetPlayback(true); RefreshView(); });
        else if (event->type() == QEvent::Resize || event->type() == QEvent::DevicePixelRatioChange)
            QTimer::singleShot(0, this, &WorldAnimationPlayer::RefreshView);
    }
    return QObject::eventFilter(watched, event);
}
void WorldAnimationPlayer::RefreshView() {
    if (!Current() || installing_ || !accepted_ || !viewport_.isVisible()) return;
    const auto current=viewport_.AnimationViewSnapshot();
    // Resize/屏幕事件可能重复；只合并真正不同的完整相机/物理视口。
    if (pending_ && pending_->worldView && SameView(*pending_->worldView,current)) return;
    if (!pending_ && inFlight_ && inFlight_->worldView && SameView(*inFlight_->worldView,current)) return;
    if (!pending_ && !inFlight_ && !NeedsView(*accepted_,current)) return;
    Render::AnimationAdvance command; command.playAll=playing_;
    Queue(std::move(command), true);
}
void WorldAnimationPlayer::SetPlayback(bool value) {
    if (value && clearPending_) { emit Message(tr("实例事务仍在等待终态，暂缓播放")); return; }
    if (!value) {
        const bool cancelledWork=pending_.has_value() || inFlight_.has_value();
        timer_.stop();
        playing_ = false;
        RevokeChain();
        ++serial_;
        pending_.reset();
        pendingAdvancePlayback_ = false;
        pendingQueuedNs_=0;
        inFlight_.reset();
        pendingEditNs_ = inFlightEditNs_ = 0;
        resumePending_ = false;
        restartPending_ = false;
        PublishChanged();
        // 用户暂停可能取消在途相机重放；内部失败回调已清 inFlight，不自动无限重试。
        if (cancelledWork && viewport_.isVisible() && Current() && accepted_ && NeedsView(*accepted_,viewport_.AnimationViewSnapshot())) {
            Render::AnimationAdvance refresh; refresh.playAll=false;
            Queue(std::move(refresh));
        }
        return;
    }
    if (!Current()) {
        if (service_)
            Fail(tr("地图或实例身份已变化，动画已停止"));
        return;
    }
    if (!service_->Scene()->animatedInstances && !service_->Scene()->particleInstanceCount) {
        emit Message(tr("当前场景只有静态模型"));
        return;
    }
    if (!viewport_.isVisible())
        return;
    // 重复Show/恢复通知不重置已运行的时钟，也不再次改写实例播放状态。
    if (playing_ && timer_.isActive())
        return;
    RevokeChain(); // 恢复播放：下一帧须激活全部实例
    playing_ = true;
    resumePending_ = true;
    StartPlaybackClock();
    timer_.setInterval(interval_);
    timer_.start();
    PublishChanged();
}
void WorldAnimationPlayer::Step() {
    if (Busy()) {
        emit Message(tr("动画计算中，单步未执行"));
        return;
    }
    SetPlaying(false);
    if (!Current())
        return;
    if (!service_->Scene()->animatedInstances && !service_->Scene()->particleInstanceCount) {
        emit Message(tr("当前场景只有静态模型"));
        return;
    }
    Render::AnimationAdvance command;
    command.deltaMs = 33;
    command.stepPaused = true;
    command.playAll = false;
    Queue(std::move(command));
}
void WorldAnimationPlayer::Edit(Render::AnimationEdit edit) {
    EditMany({std::move(edit)});
}
void WorldAnimationPlayer::EditMany(std::vector<Render::AnimationEdit> edits) {
    if (!Current())
        return;
    Render::AnimationAdvance command;
    command.edits = std::move(edits);
    command.playAll = playing_;
    Queue(std::move(command));
}
void WorldAnimationPlayer::RefreshTransforms() {
    if (!Current() || !picking_ || !accepted_)
        return;
    Render::AnimationAdvance command;
    command.playAll = playing_;
    for (std::size_t i = 0; i < picking_->instances.size(); ++i) {
        const auto r = picking_->instanceRequests[i];
        if (r == Render::kBakedInstance)
            continue;
        const auto& source = picking_->instances[i];
        const auto& old = accepted_->states[r];
        if (std::memcmp(source.transform, old.transform.data(), sizeof(source.transform)) == 0 &&
            std::memcmp(source.color, old.tint.data(), sizeof(source.color)) == 0)
            continue;
        Render::AnimationEdit e;
        e.identity = old.identity;
        e.transform.emplace();
        e.tint.emplace();
        std::copy(std::begin(source.transform), std::end(source.transform), e.transform->begin());
        std::copy(std::begin(source.color), std::end(source.color), e.tint->begin());
        command.edits.push_back(std::move(e));
    }
    if (!command.edits.empty())
        Queue(std::move(command));
}
void WorldAnimationPlayer::Tick() {
    if (!playing_)
        return;
    if (!viewport_.isVisible()) {
        SetPlayback(false);
        return;
    }
    if (!Current()) {
        Fail(tr("地图或实例身份已变化，动画已停止"));
        return;
    }
    PollEffectBudget();
    if (!playing_ || !Current()) return; // 同步状态观察者可能清场景或暂停。
    PublishChainInputs(nullptr); // 预算可能刚调整；常驻接续读最新一份
    if (worker_ || pending_)
        return; // 同时一个求值；无队列堆积
    Render::AnimationAdvance command;
    if (resumePending_)
        command.playAll = true;
    Queue(std::move(command), true);
}
void WorldAnimationPlayer::Queue(Render::AnimationAdvance command, bool advancePlayback) {
    if (clearPending_) return;
    command.worldView=viewport_.AnimationViewSnapshot();
    command.worldCamera=Render::AnimationWorldCamera(command.worldView->camera);
    // 常驻接续进行中：求值线程接下一帧时读最新相机，播放推进与相机变化不另排一帧。
    if (advancePlayback && chainGranted_) { PublishChainInputs(&*command.worldView); return; }
    if (!pending_) pendingQueuedNs_=CpuStampNs();
    if (advancePlayback) {
        // 相机与普通播放只合并最新视图。正在计算的同身份帧仍可接受，不能被每次鼠标移动取消。
        if (pending_) {
            pending_->worldView = command.worldView;
            pending_->worldCamera = command.worldCamera;
        } else {
            pending_ = std::move(command);
            pendingAdvancePlayback_ = true;
        }
        StartNext();
        return;
    }
    // 输入变化：先收回常驻许可，再推进代号；求值线程按旧代号接续的帧在接收时作废。
    RevokeChain();
    // 编辑到首帧计时取尚未显示的最早一笔编辑：在途帧随代号推进作废、其编辑并入这次时，沿用它的排队时刻。
    if (!pendingEditNs_) {
        if ((!pending_ || pendingAdvancePlayback_) && inFlight_ && inFlightEditNs_) pendingEditNs_ = inFlightEditNs_;
        else if (!command.edits.empty()) pendingEditNs_ = CpuStampNs();
    }
    // 快速 UI 编辑合并最后值，包括将被取消的在途命令；不继承尚未显示的 dt。
    if ((!pending_ || pendingAdvancePlayback_) && inFlight_) {
        pending_ = *inFlight_;
        pending_->deltaMs = 0;
        pending_->stepPaused = false;
    }
    if (pending_) {
        for (auto& e : command.edits) {
            auto found = std::find_if(pending_->edits.begin(), pending_->edits.end(),
                                      [&](const auto& old) { return old.identity == e.identity; });
            if (found == pending_->edits.end())
                pending_->edits.push_back(std::move(e));
            else {
                if (e.sequenceIndex)
                    found->sequenceIndex = e.sequenceIndex;
                if (e.elapsedMs)
                    found->elapsedMs = e.elapsedMs;
                if (e.globalMs)
                    found->globalMs = e.globalMs;
                if (e.transform)
                    found->transform = e.transform;
                if (e.tint)
                    found->tint = e.tint;
                if (e.speed)
                    found->speed = e.speed;
                if (e.playing)
                    found->playing = e.playing;
            }
        }
        pending_->deltaMs = command.deltaMs;
        pending_->stepPaused = command.stepPaused;
        pending_->playAll = command.playAll;
        pending_->worldView = command.worldView;
        pending_->worldCamera = command.worldCamera;
    } else
        pending_ = std::move(command);
    pendingAdvancePlayback_ = false;
    ++serial_;
    StartNext();
}
void WorldAnimationPlayer::StartNext(std::shared_ptr<const Render::AnimationFrame> base) {
    if (clearPending_ || !Current())
        return;
    // 资源准备只读安装时档案，可与旧pose并行；重基和普通求值仍共用唯一worker。
    if (StartInstanceWork() || worker_ || !pending_) return;
    // 资源线程可能已经结束，而Qt完成信号仍排在当前UI回调之后。此时不再多启动
    // 一轮旧pose；只等正常完成回调消费结果，不在UI线程读取worker结果或join。
    if (instanceChange_ && catalogWorker_ && catalogFinished_
        && catalogFinished_->load(std::memory_order_acquire)) return;
    auto command = std::move(*pending_);
    const bool advancePlayback = pendingAdvancePlayback_;
    const auto queuedNs=pendingQueuedNs_;
    pending_.reset();
    pendingAdvancePlayback_ = false;
    pendingQueuedNs_=0;
    const auto editQueuedNs = std::exchange(pendingEditNs_, 0);
    WorldAnimationTiming clockTiming; // 只取本次消耗播放时钟的记录
    if (advancePlayback && playing_ && PlaybackClockValid()) {
        // 求值排队期间相机可以任意变化；只有实际启动一个 worker 时消耗一次经过时间。
        command.deltaMs = ConsumePlaybackClockMs(&clockTiming);
        if (resumePending_) command.playAll = true;
        if (restartPending_) { command.restartFinished = true; restartPending_ = false; }
    }
    command.worldView = viewport_.AnimationViewSnapshot();
    command.worldCamera = Render::AnimationWorldCamera(command.worldView->camera);
    if (service_->PlaybackPolicy() == Render::AnimationPlaybackPolicy::InteractivePreview)
        command.previewParticleBudget = effectBudget_.ParticleBudget();
    inFlight_ = command; inFlightEditNs_ = editQueuedNs;
    auto output = std::make_shared<WorldAnimationWork>();
    output->activatesPlayback = command.playAll.value_or(false);
    // 只有普通播放推进可在算完后立即接下一帧；编辑、单步与暂停下的相机刷新照旧等界面接收。
    output->chainable = advancePlayback && playing_ && command.edits.empty() && !command.stepPaused;
    // 稳定播放（无恢复、重播与实例事务）时授予常驻接续许可：求值线程算完即以本帧为基接下一帧。其余情况收回。
    if (output->chainable && !resumePending_ && !restartPending_ && !instanceChange_ && !instanceTicket_ && !clearPending_)
        GrantChain(command);
    else RevokeChain();
    output->version = serial_;
    output->scene = service_->Scene();
    auto previous = base ? std::move(base) : accepted_;
    auto& timing = output->timing;
    timing.attemptSerial = NextAttemptSerial(); timing.queuedNs = queuedNs;
    timing.sceneVersion = output->scene->sceneVersion; timing.generation = output->scene->generation;
    timing.documentRevision = output->scene->documentRevision;
    timing.previousContentVersion = previous ? previous->contentVersion : 0;
    timing.editQueuedNs = editQueuedNs;
    timing.clockElapsedNs = clockTiming.clockElapsedNs; timing.clockConsumedNs = clockTiming.clockConsumedNs;
    output->command = std::move(command);
    auto* token = new QObject(this);
    output->token = token;
    worker_ = token;
    PublishChanged();
    if (!jobs_) jobs_ = std::make_unique<WorldAnimationJobThread>();
    // 求值线程：算完一帧先看常驻许可，可接续就以本帧为基直接算下一帧，每帧算完各自投递完成通知回界面线程。
    auto job = [service = service_, previous = std::move(previous), source = source_, output, chain = chain_,
                receiver = jobs_->Receiver(), self = QPointer<WorldAnimationPlayer>(this)]() mutable {
        for (auto current = std::move(output);;) {
            if (current->timing.directChained && !AwaitAnimationChain(*chain, *current)) {
                current->skipped = true; current->timing.chainSkipped = true;
                current->timing.workerStartedNs = current->timing.computeFinishedNs = CpuStampNs();
            } else ComputeAnimationWork(*service, previous.get(), source.get(), *current);
            std::shared_ptr<WorldAnimationWork> next;
            if (!current->skipped) try { next = PlanAnimationChain(*chain, *service, *current); } catch (...) { next.reset(); }
            current->next = next;
            { std::lock_guard lock(chain->mutex); ++chain->outstanding; }
            // 旧帧在求值线程放手：界面接收新帧后它常是最后持有者，析构数十 MB 不应落在界面线程。
            if (next) previous = *current->result.value; else previous.reset();
            chain->computedPosted.fetch_add(1, std::memory_order_relaxed);
            QMetaObject::invokeMethod(receiver, [self, current, chain] {
                chain->computedDelivered.fetch_add(1, std::memory_order_relaxed);
                if (self) self->OnComputed(current);
            }, Qt::QueuedConnection);
            if (!next) return;
            current = std::move(next);
        }
    };
    jobs_->Post(std::move(job));
}
void WorldAnimationPlayer::OnComputed(std::shared_ptr<WorldAnimationWork> output) {
    const QPointer<QObject> completedWorker = output->token;
    if (output->next) {
        // 求值线程已凭常驻许可以本帧为基直接接续：下一帧（已在计算）接替在途令牌，界面线程不另起求值。
        auto* token = new QObject(this);
        output->next->token = token;
        if (!worker_ || worker_ == completedWorker) worker_ = token;
        inFlight_ = output->next->planned; inFlightEditNs_ = 0;
    } else if (chainGranted_ && worker_ == completedWorker) {
        // 求值线程没有接续（许可已收回、间隔未到、界面积压或本帧失败）：许可到此结束，之后由界面线程照常安排。
        RevokeChain();
    }
    auto& timing = output->timing;
    if (output->skipped || !output->result.Succeeded() || output->version != serial_ || !service_) {
        // 失败或已作废（输入代号已变、场景已清）：不做第二段收尾与插件复制，直接按原路径结束本帧。
        // 作废帧未收尾即放手（回收时不作沿用来源）；在途令牌随即释放，排队中的编辑不必再等它收尾。
        timing.finishStartedNs = timing.workerFinishedNs = timing.computeFinishedNs;
        timing.workerTotalNs = timing.computeFinishedNs - timing.workerStartedNs;
        if (!output->skipped && !output->result.Succeeded()) timing.diagnosticCode = DiagnosticCode(output->result.diagnostics);
        OnFinished(std::move(output));
        return;
    }
    // 第二段：按帧顺序在第二个常驻线程收尾、做界面接收准备与插件复制，只读本帧，与下一帧的求值重叠。
    // 渲染器可预备时借用它把插件复制挪到后台；代次不同或渲染器已收回则跳过。
    if (!finisher_) finisher_ = std::make_unique<WorldAnimationJobThread>();
    chain_->finisherPosted.fetch_add(1, std::memory_order_relaxed);
    finisher_->Post([output, chain = chain_, lease = viewport_.AnimationPrepareLease(), prepareMode = viewport_.AnimationPrepareMode()] {
        chain->finisherStarted.fetch_add(1, std::memory_order_relaxed);
        const auto ended = qScopeGuard([&] { chain->finisherEnded.fetch_add(1, std::memory_order_relaxed); });
        auto& timing = output->timing;
        timing.finishStartedNs = CpuStampNs();
        try {
            if (output->result.Succeeded() && output->finalize) {
                // 收尾只写本帧收尾字段；此时下一帧可能正以本帧为基求值，它不读这些字段。
                auto status = output->finalize();
                output->finalize = nullptr;
                if (status.Succeeded()) timing.frame = (**output->result.value).timings;
                else output->result = {std::nullopt, std::move(status.diagnostics)};
            }
            if (output->result.Succeeded()) {
                const auto started = CpuStampNs();
                WorldViewport::PrepareAnimationFrame(output->scene, **output->result.value, output->prepared);
                timing.acceptPreparationNs = CpuStampNs() - started;
                timing.acceptPickingNs = output->prepared.pickingNs; timing.acceptBugSnapshotNs = output->prepared.bugSnapshotNs;
                timing.acceptRetainedBytesNs = output->prepared.retainedBytesNs; timing.acceptCheck = output->prepared.check;
                timing.frameRetainedBytes = output->prepared.retainedBytes;
                if (output->prepared.check) {
                    timing.memorySampled = true; timing.memory = Render::AnimationFrameRetainedMemory(**output->result.value);
                    timing.framesAlive = Render::AnimationFramesAlive();
                }
                if (lease && prepareMode && output->scene) {
                    const auto renderStarted = CpuStampNs();
                    std::shared_lock lock(lease->mutex);
                    if (lease->handle && lease->prepare && lease->epoch == prepareMode->second) {
                        output->prepared.renderTicket = Render::AnimationRenderer::PrepareUpdate(lease->prepare, lease->prepareChanges,
                            lease->handle, *output->scene, **output->result.value, prepareMode->first);
                        output->prepared.renderEpoch = lease->epoch;
                    }
                    timing.renderPreparationNs = CpuStampNs() - renderStarted;
                }
            }
        } catch (const std::exception&) {
            output->result = Result<std::shared_ptr<const Render::AnimationFrame>>::Failure(
                "World", "WORLD_ASSET_ANIMATION_WORK_FAILED", "动画来源复核或后台计算异常，旧帧未提交。");
        }
        timing.workerFinishedNs = CpuStampNs();
        timing.workerTotalNs = (timing.computeFinishedNs - timing.workerStartedNs) + (timing.workerFinishedNs - timing.finishStartedNs);
        if (!output->result.Succeeded()) timing.diagnosticCode = DiagnosticCode(output->result.diagnostics);
    }, [self = QPointer<WorldAnimationPlayer>(this), output, chain = chain_] {
        chain->finishedDelivered.fetch_add(1, std::memory_order_relaxed);
        if (self) self->OnFinished(output);
    });
    // 求值线程没有接续时，排队中的若只是播放推进或相机变化（不含编辑），由界面线程以本帧为基接着启动下一帧
    // （照样读取最新相机并消耗经过时间，与接收后再启动等价），并重新授予常驻许可。
    if (!output->next && output->chainable && output->version == serial_ && service_ && playing_ &&
        (!pending_ || pendingAdvancePlayback_) && !resumePending_ && !restartPending_ && !instanceChange_ &&
        !instanceTicket_ && !clearPending_ && worker_ == completedWorker && PlaybackClockDue() && ChainBacklogAllows() && Current()) {
        worker_ = nullptr;
        if (!pending_) { pending_ = Render::AnimationAdvance{}; pendingAdvancePlayback_ = true; pendingQueuedNs_ = CpuStampNs(); }
        StartNext(*output->result.value);
    }
}
void WorldAnimationPlayer::OnFinished(std::shared_ptr<WorldAnimationWork> output) {
    { std::lock_guard lock(chain_->mutex); if (chain_->outstanding) --chain_->outstanding; }
    const auto serial = output->version;
    const bool activatesPlayback = output->activatesPlayback, chainable = output->chainable;
    const QPointer<QObject> completedWorker = output->token;
    // GUI 接受帧期间仍保持忙碌；隐藏触发的同步拖动取消须从新接受帧恢复变换。
    // 第一段算完时可能已接着启动下一帧；那时在途命令属于新作业，不能清掉。
    if (worker_==completedWorker) { inFlight_.reset(); inFlightEditNs_=0; }
    auto timing=output->timing;
    QScopedValueRollback<WorldAnimationTiming*> timingScope(timingSink_,&timing);
    timing.uiStartedNs=CpuStampNs(); timing.uiQueueWaitNs=timing.uiStartedNs-timing.workerFinishedNs;
    ++changeBatch_;
    // 稳定播放时这一帧一算完就以它为基接着算下一帧，下一帧的计算与这里的接收、绘制重叠。
    // 这一帧若被拒收，播放会暂停或清场景，代号随之改变，在途的下一帧作废。
    if (chainable && serial == serial_ && service_ && output->result.Succeeded() && playing_ && !pending_ &&
        !resumePending_ && !restartPending_ && !instanceChange_ && !instanceTicket_ && !clearPending_ &&
        worker_ == completedWorker && PlaybackClockDue() && Current()) {
        worker_ = nullptr;
        pending_ = Render::AnimationAdvance{}; pendingAdvancePlayback_ = true; pendingQueuedNs_ = CpuStampNs();
        StartNext(*output->result.value);
    }
    std::shared_ptr<const Render::AnimationFrame> retired;
    if (serial == serial_ && service_ && !output->skipped) {
        if (!Current()) {
            timing.outcome=WorldAnimationTimingOutcome::Stale;
            Fail(tr("地图或实例身份已变化，旧动画帧已丢弃"));
        } else if (!output->result.Succeeded()) {
            if (timing.outcome!=WorldAnimationTimingOutcome::SourceRejected)
                timing.outcome=WorldAnimationTimingOutcome::PrepareRejected;
            // 数值边界拒绝时暂停，保留最后合法帧；来源错误则撤销显示。
            const auto text = Error(output->result.diagnostics);
            if (!output->result.diagnostics.empty() &&
                output->result.diagnostics.front().code.starts_with("WORLD_ASSET_"))
                Fail(text);
            else {
                SetPlaying(false);
                emit Message(text);
            }
        } else if (!viewport_.UpdateAnimationFrame(*output->result.value, picking_, &output->prepared)) {
            timing.outcome=WorldAnimationTimingOutcome::ViewportRejected;
            timing.diagnosticCode=DiagnosticCode(viewport_.LastAnimationFailure().diagnostics);
            const auto& candidate = **output->result.value;
            const auto text = tr("动画帧接收失败（场景 %1，帧 %2）：%3")
                .arg(candidate.sceneVersion).arg(candidate.contentVersion)
                .arg(Error(viewport_.LastAnimationFailure().diagnostics));
            if (!CanRetainCurrentScene()) Fail(text);
            else {
                SetPlaying(false);
                emit Message(text + tr("；保留最后有效帧，可播放或单步重试"));
            }
        } else {
            timing.outcome=WorldAnimationTimingOutcome::Accepted;
            if (timing.editQueuedNs) timing.editLatencyNs=CpuStampNs()-timing.editQueuedNs;
            retired = std::exchange(accepted_, std::move(*output->result.value));
            effectBudget_.SetOmissions(accepted_->budgetParticleInstances, accepted_->budgetParticleEmitters);
            // 暂停时排出的相机/单步帧可能在用户恢复后返回；它未处理playAll=true，
            // 不能吞掉新的恢复意图。下一次实际worker仍负责激活全部实例并消费经过时间。
            if (activatesPlayback) resumePending_ = false;
            // 视野外、预算冻结和局部拒绝均不结束播放；时钟继续才能在回镜头/合法区间恢复。
            if (playing_ && !resumePending_ && std::none_of(accepted_->states.begin(),accepted_->states.end(),
                [](const auto& state) { return state.playing; })) {
                // 下一帧须重新激活全部实例（或停止播放）：先收回常驻许可，求值线程不再按旧输入接续。
                RevokeChain();
                if (loopFinished_) {
                    // 时钟继续：下一次推进重新激活全部实例，并让已结束的不循环动作从 0 开始。
                    resumePending_ = true; restartPending_ = true;
                } else {
                    playing_ = false;
                    timer_.stop();
                }
            }
            // 旧pose已真实接收且其线程已finished：就绪名单现在即可用最新accepted重基。
            // 重基只使用独立候选求值器，与下面同步状态/诊断通知重叠；不取消合法旧帧。
            if (instanceChange_ && instancePlan_ && worker_==completedWorker) {
                worker_=nullptr;
                StartNext();
            }
            // 间隔固定跟随屏幕刷新（省电档除外），不再按计算耗时拉长；窗口换屏时在这里跟上。
            if (const auto next = PlaybackIntervalMs(); next != interval_) {
                interval_ = next; timer_.setInterval(interval_); PublishChainInputs(nullptr);
            }
            PublishChanged();
        }
    } else timing.outcome=WorldAnimationTimingOutcome::Cancelled;
    // 未被接收的帧在启动下一帧之前交回：否则要等完成回调对象析构才放回回收处，新一帧开算时回收处常是空的，
    // 只能新建整帧。回收处有空位时放回只移指针（仍有别的持有者时只减引用）；备用位已满时交后台释放。
    if (timing.outcome != WorldAnimationTimingOutcome::Accepted && output->result.Succeeded()) {
        auto dropped = std::exchange(*output->result.value, nullptr);
        if (dropped && service_ && service_->FramePoolHasRoom()) { dropped.reset(); timing.droppedFrameReturned = true; }
        else ReleaseAnimationFrameInBackground(std::move(dropped));
    }
    // 旧完成回调只能释放自己，不能擦掉上面重基或接续已启动的新任务。
    if (worker_==completedWorker) worker_=nullptr;
    StartNext();
    if (playing_ && !worker_ && !pending_ && PlaybackClockDue())
        QTimer::singleShot(0,this,&WorldAnimationPlayer::Tick);
    PublishChanged();
    // 本次接收的新目标、预算与忙碌状态合并为一次 Changed；接收者可重入并启动新 worker。
    if (--changeBatch_ == 0 && std::exchange(changeDeferred_, false)) PublishChanged();
    ReleaseAnimationFrameInBackground(std::move(retired));
    if (timing.memorySampled) {
        if (service_) {
            const auto counts = service_->FrameCounts();
            timing.serviceFramesLive = counts.live; timing.serviceFramesSpare = counts.spares;
        }
        timing.pickHistoryFrames = viewport_.AnimationPickHistoryFrames();
        const auto history = viewport_.AnimationHistoryMemoryUse();
        timing.pickHistoryBytes = history.pickBytes; timing.pickHistoryExtraFrames = history.extraFrames;
        timing.pickHistoryExtraBytes = history.extraBytes; timing.bugHistoryBytes = history.bugBytes;
    }
    timing.uiAcceptNs=CpuStampNs()-timing.uiStartedNs;
    RecordTiming(std::move(timing));
    if (completedWorker) completedWorker->deleteLater();
}
void WorldAnimationPlayer::GrantChain(const Render::AnimationAdvance& command) {
    std::lock_guard lock(chain_->mutex);
    chain_->granted = true; chain_->version = serial_;
    chain_->view = command.worldView; chain_->budget = command.previewParticleBudget;
    chain_->intervalNs = static_cast<std::int64_t>(interval_) * 1'000'000;
    chainGranted_ = true;
}
void WorldAnimationPlayer::RevokeChain() {
    {
        std::lock_guard lock(chain_->mutex);
        chain_->granted = false;
        chainGranted_ = false;
    }
    chain_->pacer.Wake(); // 正在等刷新间隔的接续帧立即醒来、放弃，排队中的编辑不必等它
}
void WorldAnimationPlayer::PublishChainInputs(const Render::AnimationView* view) {
    if (!chainGranted_ || !service_) return;
    std::optional<Render::AnimationParticleFrameBudget> budget;
    if (service_->PlaybackPolicy() == Render::AnimationPlaybackPolicy::InteractivePreview) budget = effectBudget_.ParticleBudget();
    std::lock_guard lock(chain_->mutex);
    if (view) chain_->view = *view;
    chain_->budget = std::move(budget);
    chain_->intervalNs = static_cast<std::int64_t>(interval_) * 1'000'000;
}
void WorldAnimationPlayer::StartPlaybackClock() {
    std::lock_guard lock(chain_->mutex);
    chain_->clock.Start(SteadyNs()); chain_->grid.Reset();
}
bool WorldAnimationPlayer::PlaybackClockValid() const {
    std::lock_guard lock(chain_->mutex);
    return chain_->clock.Valid();
}
bool WorldAnimationPlayer::PlaybackClockDue() const {
    std::lock_guard lock(chain_->mutex);
    return chain_->clock.Valid() && SteadyNs() - chain_->clock.clockNs >= static_cast<std::int64_t>(interval_) * 1'000'000;
}
std::uint64_t WorldAnimationPlayer::ConsumePlaybackClockMs(WorldAnimationTiming* timing) {
    std::lock_guard lock(chain_->mutex);
    chain_->grid.Reset(); // 界面线程开算：之后的接续帧从这次消耗起按间隔排
    return chain_->ConsumeMs(SteadyNs(), timing);
}
// 界面线程接续：刚算完的这一帧已计入 outstanding。
bool WorldAnimationPlayer::ChainBacklogAllows() const {
    std::lock_guard lock(chain_->mutex);
    return chain_->outstanding <= chainBacklogLimit.load(std::memory_order_relaxed);
}
bool WorldAnimationPlayer::Busy() const {
    if (pending_) return true;
    if (!worker_) return false;
    std::lock_guard lock(chain_->mutex);
    return !(chain_->waiting && chain_->outstanding == 0);
}
QString WorldAnimationPlayer::BusyReason() const {
    const auto flags = [this] {
        return QStringLiteral(" playing=%1 resume=%2 restart=%3 instanceChange=%4 instanceTicket=%5 clear=%6 current=%7")
            .arg(playing_).arg(resumePending_).arg(restartPending_).arg(instanceChange_.has_value())
            .arg(instanceTicket_).arg(clearPending_).arg(Current());
    };
    if (pending_) return QStringLiteral("pending advance=%1 worker=%2").arg(pendingAdvancePlayback_).arg(!worker_.isNull()) + flags();
    if (!worker_) return {};
    std::lock_guard lock(chain_->mutex);
    if (chain_->waiting && chain_->outstanding == 0) return {};
    const auto count = [](const std::atomic<std::uint64_t>& value) { return value.load(std::memory_order_relaxed); };
    return QStringLiteral("worker inFlight=%1 chainWaiting=%2 outstanding=%3 granted=%4 computed=%5/%6 finisher=%7/%8/%9/%10")
        .arg(inFlight_.has_value()).arg(chain_->waiting).arg(chain_->outstanding).arg(chain_->granted)
        .arg(count(chain_->computedPosted)).arg(count(chain_->computedDelivered)).arg(count(chain_->finisherPosted))
        .arg(count(chain_->finisherStarted)).arg(count(chain_->finisherEnded)).arg(count(chain_->finishedDelivered)) + flags();
}
std::uint64_t WorldAnimationPlayer::NextAttemptSerial() {
    std::lock_guard lock(chain_->mutex);
    return ++chain_->attempts;
}
} // namespace War3Map::Modules
