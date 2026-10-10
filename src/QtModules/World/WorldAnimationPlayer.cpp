#include "WorldAnimationPlayer.h"
#include <QEvent>
#include <QScopedValueRollback>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <exception>
#include <limits>

namespace War3Map::Modules {
namespace {
std::uint64_t CpuStampNs() noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
void FrameTiming(WorldAnimationTiming& timing, const Render::AnimationFrame& frame) {
    timing.sceneVersion=frame.sceneVersion; timing.contentVersion=frame.contentVersion;
    timing.documentRevision=frame.documentRevision; timing.frame=frame.timings;
    timing.animatedInstances=frame.animatedInstances; timing.retainedStaticInstances=frame.retainedStaticInstances;
    timing.hiddenInstances=frame.hiddenInstances; timing.budgetStaticInstances=frame.budgetStaticInstances;
    timing.budgetParticleInstances=frame.budgetParticleInstances;
    timing.offscreenInstances=frame.offscreenInstances; timing.offscreenPoseInstances=frame.offscreenPoseInstances;
    timing.offscreenParticleInstances=frame.offscreenParticleInstances; timing.visibilityUnknownInstances=frame.visibilityUnknownInstances;
    timing.frozenParticleInstances=std::count_if(frame.particleLinks.begin(),frame.particleLinks.end(),[](const auto& p) { return p.frozen; });
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
    : QObject(parent), viewport_(viewport) {
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
void WorldAnimationPlayer::RefreshEffectBudget() {
    interval_ = static_cast<int>(std::clamp<std::uint64_t>(recentCostMs_, effectBudget_.Status().minimumIntervalMs, 250));
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
    FrameTiming(timing,*first);
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
    interval_ = static_cast<int>(effectBudget_.Status().minimumIntervalMs);
    recentCostMs_ = static_cast<std::uint64_t>(interval_);
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
    ++serial_;
    pending_.reset(); pendingAdvancePlayback_ = false; pendingQueuedNs_ = 0; inFlight_.reset();
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
        ++serial_;
        pending_.reset();
        pendingAdvancePlayback_ = false;
        pendingQueuedNs_=0;
        inFlight_.reset();
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
    playing_ = true;
    resumePending_ = true;
    elapsed_.start();
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
void WorldAnimationPlayer::StartNext() {
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
    if (advancePlayback && playing_ && elapsed_.isValid()) {
        // 求值排队期间相机可以任意变化；只有实际启动一个 worker 时消耗一次经过时间。
        command.deltaMs = static_cast<std::uint64_t>(std::clamp<qint64>(elapsed_.restart(), 0, 1000));
        if (resumePending_) command.playAll = true;
        if (restartPending_) { command.restartFinished = true; restartPending_ = false; }
    }
    command.worldView = viewport_.AnimationViewSnapshot();
    command.worldCamera = Render::AnimationWorldCamera(command.worldView->camera);
    if (service_->PlaybackPolicy() == Render::AnimationPlaybackPolicy::InteractivePreview)
        command.previewParticleBudget = effectBudget_.ParticleBudget();
    inFlight_ = command;
    const bool activatesPlayback = command.playAll.value_or(false);
    const auto serial = serial_;
    auto service = service_;
    auto previous = accepted_;
    auto source = source_;
    struct Work {
        Result<std::shared_ptr<const Render::AnimationFrame>> result;
        std::uint64_t milliseconds = 0;
        WorldAnimationTiming timing;
    };
    auto output = std::make_shared<Work>();
    output->timing.attemptSerial=++attemptSerial_; output->timing.queuedNs=queuedNs;
    output->timing.sceneVersion=service->Scene()->sceneVersion; output->timing.generation=service->Scene()->generation;
    output->timing.documentRevision=service->Scene()->documentRevision;
    output->timing.previousContentVersion=previous ? previous->contentVersion : 0;
    auto* thread = QThread::create([service, previous, source, command = std::move(command), output] {
        const auto begin = std::chrono::steady_clock::now();
        auto& timing=output->timing;
        timing.workerStartedNs=CpuStampNs(); timing.queueWaitNs=timing.workerStartedNs-timing.queuedNs;
        try {
            bool sourceValid=true;
            if (source) {
                const auto started=CpuStampNs();
                auto valid = source->VerifyCurrentSource();
                timing.sourceBeforeNs=CpuStampNs()-started;
                timing.sourceBefore=valid.Succeeded() ? WorldAnimationSourceCheck::Passed : WorldAnimationSourceCheck::Rejected;
                if (!valid.Succeeded()) {
                    output->result.diagnostics = std::move(valid.diagnostics);
                    timing.outcome=WorldAnimationTimingOutcome::SourceRejected;
                    sourceValid=false;
                }
            }
            if (sourceValid) output->result = service->Prepare(previous.get(), command);
            if (output->result.Succeeded()) FrameTiming(timing,**output->result.value);
            if (source && output->result.Succeeded()) {
                const auto started=CpuStampNs();
                const auto valid = source->VerifyCurrentSource();
                timing.sourceAfterNs=CpuStampNs()-started;
                timing.sourceAfter=valid.Succeeded() ? WorldAnimationSourceCheck::Passed : WorldAnimationSourceCheck::Rejected;
                if (!valid.Succeeded()) {
                    output->result = {std::nullopt, valid.diagnostics};
                    timing.outcome=WorldAnimationTimingOutcome::SourceRejected;
                }
            }
        } catch (const std::exception&) {
            output->result = Result<std::shared_ptr<const Render::AnimationFrame>>::Failure(
                "World", "WORLD_ASSET_ANIMATION_WORK_FAILED", "动画来源复核或后台计算异常，旧帧未提交。");
        }
        timing.workerFinishedNs=CpuStampNs(); timing.workerTotalNs=timing.workerFinishedNs-timing.workerStartedNs;
        if (!output->result.Succeeded()) timing.diagnosticCode=DiagnosticCode(output->result.diagnostics);
        output->milliseconds = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - begin)
                .count());
    });
    worker_ = thread;
    PublishChanged();
    connect(thread, &QThread::finished, this, [this, serial, activatesPlayback, output, completedWorker=QPointer<QThread>(thread)] {
        // GUI 接受帧期间仍保持忙碌；隐藏触发的同步拖动取消须从新接受帧恢复变换。
        inFlight_.reset();
        const auto guiStarted = std::chrono::steady_clock::now();
        auto timing=output->timing;
        QScopedValueRollback<WorldAnimationTiming*> timingScope(timingSink_,&timing);
        timing.uiStartedNs=CpuStampNs(); timing.uiQueueWaitNs=timing.uiStartedNs-timing.workerFinishedNs;
        if (serial == serial_ && service_) {
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
            } else if (!viewport_.UpdateAnimationFrame(*output->result.value, picking_)) {
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
                accepted_ = std::move(*output->result.value);
                effectBudget_.SetOmissions(accepted_->budgetParticleInstances, accepted_->budgetParticleEmitters);
                // 暂停时排出的相机/单步帧可能在用户恢复后返回；它未处理playAll=true，
                // 不能吞掉新的恢复意图。下一次实际worker仍负责激活全部实例并消费经过时间。
                if (activatesPlayback) resumePending_ = false;
                // 视野外、预算冻结和局部拒绝均不结束播放；时钟继续才能在回镜头/合法区间恢复。
                if (playing_ && !resumePending_ && std::none_of(accepted_->states.begin(),accepted_->states.end(),
                    [](const auto& state) { return state.playing; })) {
                    playing_ = false;
                    timer_.stop();
                }
                // 旧pose已真实接收且其线程已finished：就绪名单现在即可用最新accepted重基。
                // 重基只使用独立候选求值器，与下面同步状态/诊断通知重叠；不取消合法旧帧。
                if (instanceChange_ && instancePlan_ && worker_==completedWorker) {
                    worker_=nullptr;
                    StartNext();
                }
                PublishChanged(); // 先公布新目标，再发预算状态，避免控件刷新覆盖警告。
                const auto guiMs =
                    static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                   std::chrono::steady_clock::now() - guiStarted)
                                                   .count());
                const auto cost = output->milliseconds + guiMs;
                // 单 worker 已禁止重叠；周期不再乘二或追加整段空等。启动慢帧的影响有界并快速衰减。
                recentCostMs_=(recentCostMs_+std::clamp<std::uint64_t>(cost,1,250)+1)/2;
                const auto nextInterval=static_cast<int>(std::clamp<std::uint64_t>(recentCostMs_,effectBudget_.Status().minimumIntervalMs,250));
                if (nextInterval!=interval_) { interval_=nextInterval; timer_.setInterval(interval_); }
                PublishChanged();
            }
        } else timing.outcome=WorldAnimationTimingOutcome::Cancelled;
        // Changed可以重入并启动新worker；旧完成回调只能释放自己，不能擦掉新重基/求值任务。
        if (worker_==completedWorker) worker_=nullptr;
        StartNext();
        if (playing_ && !worker_ && !pending_ && elapsed_.isValid() && elapsed_.elapsed()>=interval_)
            QTimer::singleShot(0,this,&WorldAnimationPlayer::Tick);
        PublishChanged();
        timing.uiAcceptNs=CpuStampNs()-timing.uiStartedNs;
        RecordTiming(std::move(timing));
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start(QThread::LowPriority);
}
} // namespace War3Map::Modules
