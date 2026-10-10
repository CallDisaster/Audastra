#include "WorldAnimationPlayer.h"
#include <QThread>
#include <exception>
#include <algorithm>

namespace War3Map::Modules {
namespace {
struct MeasureDuration {
    std::uint64_t& destination;
    std::uint64_t started = WorldInstanceNowNs();
    ~MeasureDuration() { destination += WorldInstanceNowNs() - started; }
};
struct FinishStamp {
    std::uint64_t& destination;
    std::atomic<bool>& ready;
    ~FinishStamp() { destination = WorldInstanceNowNs(); ready.store(true, std::memory_order_release); }
};
}
std::vector<WorldInstanceTiming> WorldAnimationPlayer::InstanceTimingHistory(std::uint64_t afterSequence) const {
    std::vector<WorldInstanceTiming> result;
    for (const auto& timing : instanceTimings_) if (timing.sequence > afterSequence) result.push_back(timing);
    return result;
}
bool WorldAnimationPlayer::InstancesBusy() const noexcept {
    return instanceChange_.has_value() || catalogWorker_ || instanceTicket_;
}
bool WorldAnimationPlayer::RecordInstanceOutlinerTiming(const WorldInstanceTiming& identity, std::uint64_t elapsedNs) {
    const auto matches = [&](const WorldInstanceTiming& target) {
        return target.generation == identity.generation && target.editSessionIdentity == identity.editSessionIdentity
            && target.documentRevision == identity.documentRevision && target.commandIdentity == identity.commandIdentity;
    };
    auto record = [&](WorldInstanceTiming& target) {
        target.refresh.outlinerNs += elapsedNs; target.refresh.outlinerAfterQueue = true;
    };
    if (instanceChange_ && matches(instanceChange_->timing)) { record(instanceChange_->timing); return true; }
    // Qt模型通知可以同步重入；若本笔已在通知中结束，只修改UI独占的同身份历史回执。
    for (auto it = instanceTimings_.rbegin(); it != instanceTimings_.rend(); ++it)
        if (matches(*it)) { record(*it); return true; }
    return false;
}
bool WorldAnimationPlayer::QueueInstances(InstanceChange change) {
    if (clearPending_ || !Current() || !catalog_ || !accepted_ || !source_ || InstancesBusy() || !viewport_.HasInstanceRosterSupport()
        || !change.valid || !change.valid() || !change.completed) return false;
    instanceChange_ = std::move(change); ++instanceSerial_; instancePollFailed_ = false;
    auto& timing = instanceChange_->timing;
    timing.operation = instanceSerial_; timing.queuedNs = WorldInstanceNowNs();
    timing.additions = instanceChange_->delta.additions.size(); timing.removals = instanceChange_->delta.removals.size();
    timing.edits = instanceChange_->delta.edits.size();
    emit InstanceWorkWaitingChanged(true);
    StartNext();
    return true;
}
bool WorldAnimationPlayer::StartInstanceWork() {
    if (instanceTicket_) return true;
    if (!instanceChange_) return false;
    if (!instanceChange_->valid()) {
        FinishInstanceWork(false, Status::Failure("World.Instance", "WORLD_INSTANCE_SOURCE_STALE", "摆放候选来源已失效。"));
        return false;
    }
    if (catalogWorker_) return false; // 原求值worker可继续；新增资源读取不借它的MPQ句柄。
    const auto operation = instanceSerial_;
    if (!instancePlan_) {
        const auto scene = service_->Scene();
        const auto snapshot = catalog_ ? catalog_->Scene() : nullptr;
        if (!snapshot || snapshot->sceneVersion != scene->sceneVersion
            || snapshot->documentRevision != scene->documentRevision || snapshot->generation != scene->generation
            || snapshot->sourceIdentity != scene->sourceIdentity) {
            FinishInstanceWork(false, Status::Failure("World.Instance", "WORLD_INSTANCE_CATALOG_STALE",
                "实例档案不属于当前已安装场景，未准备新增资产。"));
            return false;
        }
        auto catalog = catalog_;
        auto delta = instanceChange_->delta;
        auto preloadedAssets = instanceChange_->preloadedAssets;
        auto sources = instanceChange_->sources;
        auto preparedSource = instanceSource_ && instanceSource_->MatchesSources(sources)
            ? std::move(instanceSource_) : std::shared_ptr<WorldResourceReader>{};
        instanceSource_.reset();
        auto preparedAssets = PreparedPlacementAssets(instanceChange_->timing, sources, preparedSource);
        auto prepareOwnedAssets = instanceChange_->prepareOwnedAssets;
        auto verifiedSource = source_;
        struct Work {
            Result<std::shared_ptr<const Render::AnimationInstanceDeltaPlan>> plan;
            std::shared_ptr<WorldResourceReader> reader;
            WorldInstanceTiming timing;
        };
        auto work = std::make_shared<Work>();
        work->timing = instanceChange_->timing;
        auto completedFlag = std::make_shared<std::atomic<bool>>(false);
        catalogFinished_ = completedFlag;
        auto* thread = QThread::create([catalog, delta = std::move(delta), preloadedAssets = std::move(preloadedAssets), preparedAssets = std::move(preparedAssets), sources = std::move(sources), work,
            prepareOwnedAssets = std::move(prepareOwnedAssets), verifiedSource = std::move(verifiedSource),
            preparedSource = std::move(preparedSource), completedFlag]() mutable {
            auto& timing = work->timing; timing.catalogWorkerStartedNs = WorldInstanceNowNs();
            FinishStamp finished{timing.catalogWorkerFinishedNs, *completedFlag};
            try {
                auto reader = std::move(preparedSource);
                if (!reader) {
                    WorldResourceReader::OpenTimings openTiming;
                    auto opened = WorldResourceReader::Open(sources, verifiedSource.get(), &openTiming);
                    timing.sourceOpenNs = openTiming.totalNs; timing.sourceHashNs = openTiming.hashNs;
                    timing.archiveOpenNs = openTiming.archiveNs; timing.hashedBytes = openTiming.hashedBytes;
                    timing.verifiedArchiveReused = openTiming.verifiedArchiveReused;
                    if (!opened.Succeeded()) { work->plan.diagnostics = std::move(opened.diagnostics); return; }
                    reader = std::shared_ptr<WorldResourceReader>(std::move(*opened.value));
                } else {
                    timing.resourceReaderReused = true;
                    timing.verifiedArchiveReused = true;
                }
                // reader由已结束的预取/目录任务移交，仍逐项重读正负来源及验证租约；
                // 复用的是独占解析器/游戏包挂载，不是上次来源检查结论。
                work->reader = reader;
                Status source;
                { MeasureDuration duration{timing.sourceVerifyNs}; source = reader->VerifyCurrentSource(); }
                if (!source.Succeeded()) { work->plan.diagnostics = std::move(source.diagnostics); return; }
                if (prepareOwnedAssets) {
                    MeasureDuration duration{timing.ownedAssetsNs};
                    source = prepareOwnedAssets(delta, reader, timing.helpers);
                    if (!source.Succeeded()) { work->plan.diagnostics = std::move(source.diagnostics); return; }
                }
                {
                    MeasureDuration duration{timing.catalogPrepareNs};
                    work->plan = catalog->PrepareInstanceCatalog(std::move(delta),
                        [&](auto path, auto budget) { return reader->ReadAnimationResource(path, budget); },
                        preloadedAssets, &timing.catalogDetail, preparedAssets);
                }
                { MeasureDuration duration{timing.sourceVerifyNs}; source = reader->VerifyCurrentSource(); }
                if (!source.Succeeded()) work->plan = {std::nullopt, std::move(source.diagnostics)};
            } catch (const std::exception&) {
                work->plan = decltype(work->plan)::Failure("World.Instance", "WORLD_INSTANCE_CATALOG_FAILED", "实例资源准备异常，旧世界保留。");
            }
        });
        catalogWorker_ = thread;
        connect(thread, &QThread::finished, this, [this, operation, work] {
            catalogWorker_ = nullptr; catalogFinished_.reset();
            if (operation != instanceSerial_ || !instanceChange_) { PublishChanged(); StartNext(); return; }
            const auto refresh = instanceChange_->timing.refresh;
            instanceChange_->timing = std::move(work->timing);
            instanceChange_->timing.refresh = refresh;
            if (!work->plan.Succeeded()) { FinishInstanceWork(false, {work->plan.diagnostics}); return; }
            instanceSource_ = std::move(work->reader);
            instancePlan_ = std::move(*work->plan.value);
            StartNext();
        });
        connect(thread, &QThread::finished, thread, &QObject::deleteLater);
        thread->start(QThread::LowPriority);
        return false;
    }
    if (worker_) return true; // 已准备的plan只在旧pose接受后，以最新accepted帧串行重基。
    auto previous = accepted_; auto service = service_; auto source = source_; auto plan = instancePlan_;
    Render::AnimationAdvance advance;
    advance.playAll = playing_; advance.worldView = viewport_.AnimationViewSnapshot();
    advance.worldCamera = Render::AnimationWorldCamera(advance.worldView->camera);
    if (service_->PlaybackPolicy() == Render::AnimationPlaybackPolicy::InteractivePreview)
        advance.previewParticleBudget = effectBudget_.ParticleBudget();
    instanceActivatesPlayback_ = advance.playAll.value_or(false);
    if (playing_ && elapsed_.isValid()) advance.deltaMs = static_cast<std::uint64_t>(std::clamp<qint64>(elapsed_.restart(), 0, 1000));
    struct Work {
        Result<Render::AnimationInstanceDeltaResult> result;
        std::shared_ptr<const Render::ModelAnimationService> catalog;
        WorldInstanceTiming timing;
    };
    auto work = std::make_shared<Work>();
    work->timing = instanceChange_->timing;
    auto* thread = QThread::create([service, previous, source, plan, advance, work] {
        auto& timing = work->timing; timing.rebaseStartedNs = WorldInstanceNowNs();
        try {
            Status status;
            { MeasureDuration duration{timing.sourceVerifyNs}; status = source->VerifyCurrentSource(); }
            if (!status.Succeeded()) { work->result.diagnostics = std::move(status.diagnostics); return; }
            { MeasureDuration duration{timing.rebaseNs}; work->result = service->PrepareInstanceDelta(
                *previous, *plan, advance, &timing.rebaseDetail); }
            if (work->result.Succeeded()) timing.firstFrameDetail = work->result.value->frame->timings;
            { MeasureDuration duration{timing.sourceVerifyNs}; status = source->VerifyCurrentSource(); }
            if (!status.Succeeded()) work->result = {std::nullopt, std::move(status.diagnostics)};
            if (work->result.Succeeded()) {
                MeasureDuration duration{timing.catalogCaptureNs};
                auto catalog = work->result.value->service->CaptureInstanceCatalog();
                if (!catalog.Succeeded()) work->result = {std::nullopt, std::move(catalog.diagnostics)};
                else work->catalog = std::move(*catalog.value);
            }
        } catch (const std::exception&) {
            work->result = decltype(work->result)::Failure("World.Instance", "WORLD_INSTANCE_REBASE_FAILED", "实例最新帧重基异常，旧世界保留。");
        }
    });
    worker_ = thread;
    connect(thread, &QThread::finished, this, [this, operation, work] {
        worker_ = nullptr;
        if (operation != instanceSerial_ || !instanceChange_) { StartNext(); return; }
        const auto refresh = instanceChange_->timing.refresh;
        instanceChange_->timing = std::move(work->timing);
        instanceChange_->timing.refresh = refresh;
        if (!work->result.Succeeded()) { FinishInstanceWork(false, {work->result.diagnostics}); return; }
        if (!Current() || !instanceChange_->valid()) {
            FinishInstanceWork(false, Status::Failure("World.Instance", "WORLD_INSTANCE_SOURCE_STALE", "重基完成时地图来源已变化。")); return;
        }
        if (instanceChange_->validateOwnedAssets) {
            const auto current = instanceChange_->validateOwnedAssets();
            if (!current.Succeeded()) { FinishInstanceWork(false, current); return; }
        }
        auto& timing = instanceChange_->timing;
        const auto pickingStarted = WorldInstanceNowNs();
        instancePicking_ = Render::AnimationPickingScene(*work->result.value->service->Scene(), *work->result.value->frame);
        timing.pickingNs = WorldInstanceNowNs() - pickingStarted;
        const auto queueStarted = WorldInstanceNowNs();
        auto ticket = viewport_.QueueAnimationRoster(work->result.value->service->Scene(), work->result.value->frame,
            instancePicking_, work->result.value->assetsChanged, instanceChange_->visible);
        timing.rosterQueueNs = WorldInstanceNowNs() - queueStarted;
        if (!ticket.Succeeded()) { FinishInstanceWork(false, {ticket.diagnostics}); return; }
        instanceResult_ = std::move(*work->result.value); instanceCatalog_ = std::move(work->catalog); instanceTicket_ = *ticket.value;
        timing.assetsChanged = instanceResult_->assetsChanged; timing.rosterTicket = instanceTicket_;
        timing.submittedNs = WorldInstanceNowNs();
        if (!instancePollTimer_.interval()) {
            // 仅名单事务在途时短轮询真实完成票据；完成/取消即停，不把提交当成完成。
            // 16ms观察间隔会把已经绘制的新实例额外阻挡近一帧，连带延迟点选身份发布。
            instancePollTimer_.setInterval(4);
            instancePollTimer_.setTimerType(Qt::PreciseTimer);
            connect(&instancePollTimer_, &QTimer::timeout, this, &WorldAnimationPlayer::PollInstanceWork);
        }
        instancePollTimer_.start();
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start(QThread::LowPriority);
    return true;
}
void WorldAnimationPlayer::PollInstanceWork() {
    if (!instanceTicket_ || !instanceChange_) { instancePollTimer_.stop(); return; }
    auto receipt = viewport_.PollAnimationRoster(instanceTicket_);
    if (!receipt.Succeeded()) {
        // 查询错误不能证明GPU尚未发布；保留全部owner并继续drain，绝不超时释放。
        if (!instancePollFailed_) {
            instancePollFailed_ = true;
            emit Message(receipt.diagnostics.empty() ? tr("实例票据暂不可核验，保持等待终态")
                : QString::fromUtf8(receipt.diagnostics.front().message));
        }
        return;
    }
    if (receipt.value->state == WAR3_RENDER_ROSTER_QUEUED) return;
    const bool clearAfter = clearPending_;
    if (receipt.value->state != WAR3_RENDER_ROSTER_COMPLETED || !instanceResult_) {
        FinishInstanceWork(false, Status::Failure("World.Instance", "WORLD_INSTANCE_GPU_REJECTED", "GPU实例事务未完成，驻留场景保持。"));
        if (clearAfter) Clear();
        return;
    }
    service_ = std::move(instanceResult_->service); accepted_ = std::move(instanceResult_->frame);
    effectBudget_.SetOmissions(accepted_->budgetParticleInstances, accepted_->budgetParticleEmitters);
    catalog_ = std::move(instanceCatalog_);
    picking_ = std::move(instancePicking_);
    instanceChange_->timing.completed = receipt.value->completed;
    if (instanceActivatesPlayback_) resumePending_ = false;
    FinishInstanceWork(true, {});
    if (clearAfter) Clear();
}
void WorldAnimationPlayer::FinishInstanceWork(bool accepted, Status status) {
    instancePollTimer_.stop(); instanceTicket_ = 0;
    if (instanceChange_) {
        auto timing = std::move(instanceChange_->timing); timing.finishedNs = WorldInstanceNowNs();
        timing.accepted = accepted; timing.sequence = ++instanceTimingSequence_;
        if (!status.diagnostics.empty()) timing.diagnosticCode = status.diagnostics.front().code;
        instanceTimings_.push_back(std::move(timing));
        if (instanceTimings_.size() > 512) instanceTimings_.pop_front();
    }
    auto completed = instanceChange_ ? std::move(instanceChange_->completed) : std::function<void(bool,const Status&)>{};
    instanceChange_.reset(); instancePlan_.reset(); instanceCatalog_.reset(); instanceResult_.reset(); instancePicking_.reset();
    instanceActivatesPlayback_ = false;
    if (completed) completed(accepted, status);
    // 成功回调先绑定已完成的主parent，失败仍绑定旧parent；再合并等待期间的附加层请求。
    // completed可能同步排入下一笔，不能短暂解锁它。
    emit InstanceWorkWaitingChanged(clearPending_ || InstancesBusy());
    if (!accepted && !status.diagnostics.empty()) emit Message(QString::fromUtf8(status.diagnostics.front().message));
    PublishChanged(); StartNext();
}
} // namespace War3Map::Modules
