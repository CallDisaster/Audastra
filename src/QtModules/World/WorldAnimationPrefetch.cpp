#include "WorldAnimationPlayer.h"

#include <algorithm>
#include <utility>

namespace War3Map::Modules {
namespace {
std::string Variant(const Render::ModelInstanceRequest& request) {
    return Render::ModelAssetKey(request) + "|team:" + std::to_string(request.teamColor)
        + "|replacement:" + Render::AnimationPathKey(request.replaceableTexture);
}
}

// 仅由UI线程管理生命周期；worker只拥有输入、独立reader和静态结果。
// 一个在途任务加一个最新候选，取消只失效结果，绝不等待或重置世界播放。
class WorldAnimationAssetPrefetch final : public QObject {
public:
    ~WorldAnimationAssetPrefetch() override { Cancel(); }

    void Request(std::shared_ptr<const Render::ModelAnimationService> catalog,
        std::shared_ptr<WorldResourceReader> verifiedSource, const Render::ModelInstanceRequest& request,
        const WorldTerrainSources& sources, std::function<bool()> valid,
        std::shared_ptr<const Render::AnimationAssetSnapshot> preloaded) {
        if (!catalog || !verifiedSource || request.previewGhost || !valid || !valid()) { Cancel(); return; }
        const auto key = Variant(request);
        if (catalog_ && catalog_->Scene() == catalog->Scene() && variant_ == key && SameWorldTerrainSources(sources_, sources)
            && valid_ && valid_()) {
            valid_ = std::move(valid);
            return;
        }
        Cancel();
        catalog_ = std::move(catalog); sources_ = sources; variant_ = key; valid_ = std::move(valid);
        requestedNs_ = WorldInstanceNowNs(); model_ = request.modelPath;
        state_ = "pending";
        pending_ = RequestData{catalog_, std::move(verifiedSource), request, sources,
            std::move(preloaded), serial_->load(std::memory_order_relaxed)};
        Start();
    }

    void Cancel() {
        serial_->fetch_add(1, std::memory_order_relaxed);
        pending_.reset(); work_.reset(); ready_.reset(); catalog_.reset(); valid_ = {}; variant_.clear();
        state_ = "none"; diagnostic_.clear(); model_.clear(); sources_ = {};
        requestedNs_ = startedNs_ = finishedNs_ = 0;
    }

    std::shared_ptr<const Render::AnimationPreparedAssets> Ready(
        const std::shared_ptr<const Render::ModelAnimationService>& catalog, WorldInstanceTiming& timing) {
        // 正式点击可以先于queued finished通知。仅消费release发布的只读结果，UI绝不等待线程。
        PublishCompleted();
        timing.prefetchRequestedNs = requestedNs_; timing.prefetchStartedNs = startedNs_;
        timing.prefetchFinishedNs = finishedNs_; timing.prefetchDiagnostic = diagnostic_;
        timing.prefetchModel = model_; timing.prefetchState = state_;
        if (!catalog_ || !catalog || catalog_->Scene() != catalog->Scene() || !valid_ || !valid_()) {
            if (requestedNs_) timing.prefetchState = "stale";
            return {};
        }
        return ready_;
    }

    std::shared_ptr<WorldResourceReader> TakeReader(const WorldTerrainSources& sources) {
        // Ready已完成acquire发布；只移交一次，worker不再读写，后续诊断也不借用它。
        if (!ready_ || !work_ || !work_->completed.load(std::memory_order_acquire)
            || work_->serial != serial_->load(std::memory_order_relaxed) || !valid_ || !valid_()
            || !SameWorldTerrainSources(sources_, sources)) return {};
        return std::move(work_->reader);
    }

private:
    struct Work {
        Result<std::shared_ptr<const Render::AnimationPreparedAssets>> result;
        std::shared_ptr<WorldResourceReader> reader;
        std::atomic<std::uint64_t> startedNs{0}, finishedNs{0};
        std::atomic<bool> completed{false};
        std::uint64_t serial = 0;
    };
    void PublishCompleted() {
        if (!work_ || work_->serial != serial_->load(std::memory_order_relaxed)) return;
        startedNs_ = work_->startedNs.load(std::memory_order_relaxed);
        if (!work_->completed.load(std::memory_order_acquire)) return;
        // completed之后worker不再改result；回调与提前读取都只复制shared_ptr，不能移动两次。
        startedNs_ = work_->startedNs.load(std::memory_order_relaxed);
        finishedNs_ = work_->finishedNs.load(std::memory_order_relaxed);
        if (work_->result.Succeeded()) { ready_ = *work_->result.value; state_ = "ready"; }
        else {
            state_ = "failed";
            if (!work_->result.diagnostics.empty()) diagnostic_ = work_->result.diagnostics.front().code;
        }
    }
    struct RequestData {
        std::shared_ptr<const Render::ModelAnimationService> catalog;
        std::shared_ptr<WorldResourceReader> verifiedSource;
        Render::ModelInstanceRequest request;
        WorldTerrainSources sources;
        std::shared_ptr<const Render::AnimationAssetSnapshot> preloaded;
        std::uint64_t serial = 0;
    };
    void Start() {
        if (worker_ || !pending_) return;
        if (!valid_ || !valid_()) { Cancel(); return; }
        auto input = std::move(*pending_); pending_.reset();
        state_ = "pending";
        auto work = std::make_shared<Work>();
        const auto operation = input.serial;
        work->serial = operation; work_ = work;
        const auto serial = serial_;
        auto* thread = QThread::create([input = std::move(input), work, serial] {
            work->startedNs.store(WorldInstanceNowNs(), std::memory_order_relaxed);
            const auto current = [&] { return serial->load(std::memory_order_relaxed) == input.serial; };
            try {
                if (current()) {
                    auto opened = WorldResourceReader::Open(input.sources, input.verifiedSource.get());
                    if (!opened.Succeeded()) work->result.diagnostics = std::move(opened.diagnostics);
                    else {
                        work->reader = std::shared_ptr<WorldResourceReader>(std::move(*opened.value));
                        auto& reader = *work->reader;
                        auto verified = reader.VerifyCurrentSource();
                        if (!verified.Succeeded()) work->result.diagnostics = std::move(verified.diagnostics);
                        else if (current()) {
                            work->result = input.catalog->PrepareStaticAssets(input.request,
                                [&](auto path, auto budget) -> Result<Render::AnimationResource> {
                                    if (!current()) return Result<Render::AnimationResource>::Failure(
                                        "World.Prefetch", "WORLD_PREFETCH_CANCELLED", "预取类型已改变，丢弃未发布资产。");
                                    return reader.ReadAnimationResource(path, budget);
                                }, input.preloaded);
                            verified = reader.VerifyCurrentSource();
                            if (!verified.Succeeded()) work->result = {std::nullopt, std::move(verified.diagnostics)};
                        }
                    }
                }
            } catch (const std::exception&) {
                work->result = decltype(work->result)::Failure("World.Prefetch", "WORLD_PREFETCH_FAILED",
                    "正式静态资产预取异常；实际摆放仍按正常路径准备。");
            }
            work->finishedNs.store(WorldInstanceNowNs(), std::memory_order_relaxed);
            work->completed.store(true, std::memory_order_release);
        });
        worker_ = thread;
        connect(thread, &QThread::finished, this, [this, operation] {
            worker_ = nullptr;
            if (operation == serial_->load(std::memory_order_relaxed) && valid_ && valid_()) {
                PublishCompleted();
            }
            Start();
        });
        connect(thread, &QThread::finished, thread, &QObject::deleteLater);
        thread->start(QThread::LowPriority);
    }
    QPointer<QThread> worker_;
    std::optional<RequestData> pending_;
    std::shared_ptr<Work> work_;
    std::shared_ptr<std::atomic<std::uint64_t>> serial_ = std::make_shared<std::atomic<std::uint64_t>>(0);
    std::shared_ptr<const Render::ModelAnimationService> catalog_;
    std::shared_ptr<const Render::AnimationPreparedAssets> ready_;
    WorldTerrainSources sources_;
    std::function<bool()> valid_;
    std::string variant_, model_, state_ = "none", diagnostic_;
    std::uint64_t requestedNs_ = 0, startedNs_ = 0, finishedNs_ = 0;
};

void WorldAnimationPlayer::PrefetchPlacementAsset(const Render::ModelInstanceRequest& request,
    const WorldTerrainSources& sources, std::function<bool()> valid,
    std::shared_ptr<const Render::AnimationAssetSnapshot> preloaded) {
    if (!Current() || !catalog_ || !source_ || clearPending_) { CancelAssetPrefetch(); return; }
    if (!assetPrefetch_) assetPrefetch_ = std::make_shared<WorldAnimationAssetPrefetch>();
    assetPrefetch_->Request(catalog_, source_, request, sources, std::move(valid), std::move(preloaded));
}
void WorldAnimationPlayer::CancelAssetPrefetch() {
    if (assetPrefetch_) assetPrefetch_->Cancel();
}
WorldInstanceTiming WorldAnimationPlayer::PlacementAssetPrefetchTiming() const {
    WorldInstanceTiming timing;
    if (assetPrefetch_) (void)assetPrefetch_->Ready(catalog_, timing);
    return timing;
}
std::shared_ptr<const Render::AnimationPreparedAssets> WorldAnimationPlayer::PreparedPlacementAssets(
    WorldInstanceTiming& timing, const WorldTerrainSources& sources, std::shared_ptr<WorldResourceReader>& reader) const {
    if (!assetPrefetch_) return {};
    auto assets = assetPrefetch_->Ready(catalog_, timing);
    if (assets && !reader) reader = assetPrefetch_->TakeReader(sources);
    if (!assets && (timing.prefetchState == "pending" || timing.prefetchState == "stale")) {
        // 不让未完成的低优先级预取与已点击的正式准备重复争用资源；保留本次计时证据。
        if (timing.prefetchState == "pending") timing.prefetchState = "cancelled_for_placement";
        assetPrefetch_->Cancel();
    }
    return assets;
}
} // namespace War3Map::Modules
