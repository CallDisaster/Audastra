#include "WorldPlacementPreview.h"
#include "WorldPlacementPending.h"
#include "WorldViewport.h"
#include <QEvent>
#include <QThread>
#include <algorithm>
#include <exception>

namespace War3Map::Modules {
namespace {
bool SameSources(const WorldTerrainSources& a, const WorldTerrainSources& b) {
    if (a.archivePath != b.archivePath || a.archiveSha != b.archiveSha || a.gameRoot != b.gameRoot
        || a.mainTileset != b.mainTileset || a.entries.size() != b.entries.size()) return false;
    return std::equal(a.entries.begin(), a.entries.end(), b.entries.begin(), [](const auto& x, const auto& y) {
        return x.path == y.path && x.archivePath == y.archivePath && x.diskPath == y.diskPath;
    });
}
bool SameRequests(const std::vector<Render::ModelInstanceRequest>& a,
    const std::vector<Render::ModelInstanceRequest>& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](const auto& x, const auto& y) {
        // 落点由完整 transforms 提供；类型/材质/动画属性改变不能借用上一类型的资产。
        return x.modelPath == y.modelPath && x.replaceableTexture == y.replaceableTexture
            && x.teamColor == y.teamColor && x.angle == y.angle && x.scale == y.scale
            && x.tint == y.tint && x.previewGhost == y.previewGhost && x.animationProperties == y.animationProperties
            && static_cast<bool>(x.ownedStaticAsset) == static_cast<bool>(y.ownedStaticAsset)
            && (!x.ownedStaticAsset || x.ownedStaticAsset->IdentitySha256() == y.ownedStaticAsset->IdentitySha256());
    });
}
}
struct WorldPlacementPreview::Work {
    std::shared_ptr<Render::ModelAnimationService> service;
    std::shared_ptr<WorldResourceReader> source;
    std::shared_ptr<const Render::AnimationFrame> frame;
    std::shared_ptr<const Render::AnimationAssetSnapshot> preloadedAssets;
    std::vector<Diagnostic> diagnostics;
    std::optional<WorldPathingHelperPrepared> helpers;
};
WorldPlacementPreview::WorldPlacementPreview(WorldViewport& viewport, QObject* parent)
    : QObject(parent), viewport_(&viewport), layerId_(Render::NextAnimationSerial()) {
    playbackTimer_.setInterval(50); // 幽灵层至多20次求值/秒，与主世界共用UI但不争抢每次鼠标事件。
    connect(&playbackTimer_, &QTimer::timeout, this, [this] { UpdatePlaybackClock(); BeginNext(); });
    viewport.installEventFilter(this);
    connect(&viewport, &WorldViewport::AnimationCameraChanged, this, [this] { if (cameraDependent_) BeginNext(); });
}
WorldPlacementPreview::~WorldPlacementPreview() {
    PausePlaybackClock();
    if (viewport_) viewport_->removeEventFilter(this);
    if (worker_) disconnect(worker_, nullptr, this, nullptr);
    if (viewport_) (void)viewport_->RemoveModelLayer(layerId_);
    // 线程只拥有已捕获值，finished 自删；这里不阻塞共享 UI 线程。
}
void WorldPlacementPreview::Clear() {
    PausePlaybackClock(); clockElapsedMs_ = preparedClockMs_ = requestedClockMs_ = 0;
    playbackFaulted_ = false;
    ++serial_; pending_.reset(); current_ = {}; transforms_.clear();
    service_.reset(); frame_.reset(); source_.reset(); preloadedAssets_.reset();
    residentSources_.reset(); residentRequests_.clear(); needsSubmit_ = false;
    residentPresentationIdentity_.clear(); helperCache_.reset(); residentHelpers_.reset();
    cameraDependent_ = false; preparedCamera_.reset(); preparedTransformSerial_ = 0;
    RemoveLayer();
}
void WorldPlacementPreview::RemoveLayer() {
    if (submissionDeferred_) { removePending_ = true; return; }
    if (viewport_) {
        const auto result = viewport_->RemoveModelLayer(layerId_);
        if (result.Succeeded()) { submitted_ = false; removePending_ = false; }
        else if (!result.diagnostics.empty()) emit Message(QString::fromUtf8(result.diagnostics.front().message));
    }
}
void WorldPlacementPreview::Hide() {
    PausePlaybackClock();
    if (!transforms_.empty()) { transforms_.clear(); ++transformSerial_; }
    needsSubmit_ = true;
    RemoveLayer();
}
void WorldPlacementPreview::CancelRequest() {
    ++serial_; pending_.reset(); current_ = {};
    Hide();
}
std::shared_ptr<const Render::AnimationAssetSnapshot> WorldPlacementPreview::PreloadedAssets(
    const WorldTerrainSources& sources) const {
    return residentSources_ && SameSources(*residentSources_, sources) ? preloadedAssets_ : nullptr;
}
void WorldPlacementPreview::Show(std::vector<Render::ModelInstanceRequest> requests, WorldTerrainSources sources,
    std::uint64_t revision, std::function<bool()> current, std::vector<std::array<float, 12>> initialTransforms,
    std::vector<WorldPlacementPresentationCandidate> presentations,
    Services::WorldSelectionVersion presentationVersion, std::string helperResourceIdentity) {
    ++serial_; pending_.reset();
    if (!viewport_ || !viewport_->HasModelLayerSupport()) { Clear(); emit Message(tr("渲染插件不支持独立摆放预览层")); return; }
    if (requests.empty()) { Clear(); return; }
    std::string presentationIdentity;
    if (!presentations.empty()) {
        auto identity = WorldPathingHelperCandidateIdentity(presentations, presentationVersion, helperResourceIdentity);
        if (!identity.Succeeded()) { Clear(); emit Message(QString::fromUtf8(identity.diagnostics.front().message)); return; }
        presentationIdentity = std::move(*identity.value);
    }
    for (auto& request : requests) request.previewGhost = true;
    const bool sameSource = residentSources_ && SameSources(*residentSources_, sources);
    const bool reuse = sameSource && service_ && frame_ && SameRequests(residentRequests_, requests)
        && residentPresentationIdentity_ == presentationIdentity;
    if (!reuse) {
        PausePlaybackClock(); clockElapsedMs_ = preparedClockMs_ = requestedClockMs_ = 0;
        RemoveLayer();
        service_.reset(); frame_.reset(); preloadedAssets_.reset(); residentRequests_.clear();
        cameraDependent_ = false; preparedCamera_.reset(); preparedTransformSerial_ = 0;
        if (!sameSource) { source_.reset(); helperCache_.reset(); }
        pending_ = Pending{std::move(requests), std::move(sources), revision,
            std::move(presentations), presentationVersion, std::move(helperResourceIdentity)};
    }
    playbackFaulted_ = false;
    residentPresentationIdentity_ = std::move(presentationIdentity);
    current_ = std::move(current); transforms_ = std::move(initialTransforms);
    // Show 也用于主场景安装后的重新挂接。CPU 可以复用，但父 model epoch 可能已变；
    // 通过 Submit 重新取得真实父身份，不能以本地 submitted_ 猜插件仍持有旧层。
    ++transformSerial_; needsSubmit_ = true;
    if (transforms_.empty()) Hide();
    if (transforms_.empty()) emit Message(tr("已选择摆放类型，等待有效地形落点"));
    else if (!reuse) emit Message(tr("正在准备摆放模型，落点标记不是物体占地"));
    if (reuse && source_) {
        const auto valid = source_->VerifyCurrentSource();
        if (!valid.Succeeded()) { Clear(); emit Message(QString::fromUtf8(valid.diagnostics.front().message)); return; }
    }
    UpdatePlaybackClock();
    if (reuse && PresentCachedTransforms() && !cameraDependent_) return;
    BeginNext();
}
void WorldPlacementPreview::SetTransforms(std::vector<std::array<float, 12>> transforms) {
    if (transforms.empty()) { Hide(); return; }
    if (transforms_ == transforms && !needsSubmit_) return;
    transforms_ = std::move(transforms); ++transformSerial_;
    UpdatePlaybackClock();
    if (!PresentCachedTransforms() || cameraDependent_) BeginNext();
}
void WorldPlacementPreview::SetSubmissionDeferred(bool deferred) {
    if (submissionDeferred_ == deferred) return;
    submissionDeferred_ = deferred;
    if (deferred) { PausePlaybackClock(); return; }
    // 完成主roster可能清除了父epoch中的附加层，须重新Submit取得真实父身份。
    if (removePending_) RemoveLayer();
    needsSubmit_ = true;
    playbackFaulted_ = false;
    UpdatePlaybackClock();
    if (!transforms_.empty() && (!PresentCachedTransforms() || cameraDependent_)) BeginNext();
}
bool WorldPlacementPreview::PresentCachedTransforms() {
    if (submissionDeferred_ || pending_ || !viewport_ || !viewport_->isVisible() || !current_ || !current_() || transforms_.empty()
        || !service_ || !frame_) return false;
    if (frame_->particleCheckpoint) {
        // 再次选同一类型或返回同一落点时，可原样挂回完整已接受帧。这里只接受完全相同
        // 的变换和相机；绝不平移粒子顶点、改版本、丢检查点，或在UI线程重新求值。
        if (frame_->states.size() != transforms_.size() || !frame_->worldCamera) return false;
        for (std::size_t i = 0; i < transforms_.size(); ++i)
            if (frame_->states[i].transform != transforms_[i]) return false;
        const auto camera = Render::AnimationWorldCamera(viewport_->Camera());
        if (frame_->worldCamera->eye != camera.eye || frame_->worldCamera->up != camera.up) return false;
        if (needsSubmit_ || !submitted_) {
            if (!source_) return false;
            const auto valid = source_->VerifyCurrentSource();
            if (!valid.Succeeded()) {
                Clear();
                if (!valid.diagnostics.empty()) emit Message(QString::fromUtf8(valid.diagnostics.front().message));
                return false;
            }
            const auto result = viewport_->SetModelLayer(layerId_, service_->Scene(), frame_);
            if (!result.Succeeded()) return false;
        }
        const bool changed = needsSubmit_ || !submitted_ || acceptedTransformSerial_ != transformSerial_;
        acceptedTransformSerial_ = preparedTransformSerial_ = transformSerial_;
        submitted_ = true; needsSubmit_ = false; removePending_ = false;
        if (changed) emit Prepared();
        return true;
    }
    auto translated = TranslatePlacementPreviewFrame(*service_->Scene(), *frame_, transforms_, Render::NextAnimationSerial());
    if (!translated.Succeeded()) return false;
    const auto result = needsSubmit_ || !submitted_ ? viewport_->SetModelLayer(layerId_, service_->Scene(), *translated.value)
                                                  : viewport_->UpdateModelLayer(layerId_, *translated.value);
    if (!result.Succeeded()) return false;
    frame_ = std::move(*translated.value); acceptedTransformSerial_ = transformSerial_;
    submitted_ = true; needsSubmit_ = false; removePending_ = false;
    emit Prepared();
    return true;
}
std::uint64_t WorldPlacementPreview::PlaybackTimeMs() const {
    return clockElapsedMs_ + (playbackClock_.isValid() ? static_cast<std::uint64_t>(playbackClock_.elapsed()) : 0);
}
void WorldPlacementPreview::PausePlaybackClock() {
    clockElapsedMs_ = PlaybackTimeMs(); playbackClock_.invalidate(); playbackTimer_.stop();
}
void WorldPlacementPreview::UpdatePlaybackClock() {
    const bool active = !playbackFaulted_ && !submissionDeferred_ && viewport_ && viewport_->isVisible()
        && !viewport_->RendererFailed() && current_ && current_() && !transforms_.empty() && frame_ && service_;
    if (!active) { PausePlaybackClock(); return; }
    if (!playbackClock_.isValid()) playbackClock_.start();
    if (!playbackTimer_.isActive()) playbackTimer_.start();
}
bool WorldPlacementPreview::eventFilter(QObject* watched, QEvent* event) {
    if (watched == viewport_) {
        if (event->type() == QEvent::Hide) { PausePlaybackClock(); needsSubmit_ = true; RemoveLayer(); }
        else if (event->type() == QEvent::Show) QTimer::singleShot(0, this, [this] {
            if (viewport_ && viewport_->isVisible()) { UpdatePlaybackClock(); BeginNext(); }
        });
    }
    return QObject::eventFilter(watched, event);
}
void WorldPlacementPreview::BeginNext() {
    if (worker_ || !viewport_ || !current_ || (transforms_.empty() && !pending_)) return;
    if (playbackFaulted_ || !viewport_->isVisible()) return;
    if (submissionDeferred_ && !pending_) return; // 镜头/鼠标移动只合并落点，不为冻结层重复求值。
    if (!current_()) { Clear(); return; }
    const bool build = pending_.has_value();
    const auto camera = Render::AnimationWorldCamera(viewport_->Camera());
    const bool updateCamera = cameraDependent_ && (preparedTransformSerial_ != transformSerial_ || !preparedCamera_
        || preparedCamera_->eye != camera.eye || preparedCamera_->up != camera.up);
    const auto clockMs = PlaybackTimeMs();
    const bool updateTime = frame_ && clockMs > preparedClockMs_;
    if (!build && (!service_ || !frame_ || (!needsSubmit_ && transformSerial_ == acceptedTransformSerial_ && !updateCamera && !updateTime))) return;
    if (!build && !needsSubmit_ && submitted_ && playbackClock_.isValid()
        && clockMs - requestedClockMs_ < static_cast<std::uint64_t>(playbackTimer_.interval())) return;
    const auto count = build ? pending_->requests.size() : service_->Scene()->specs.size();
    if (!transforms_.empty() && transforms_.size() != count) { Hide(); emit Message(tr("摆放预览的落点变换与类型数量不符")); return; }
    auto work = std::make_shared<Work>();
    auto pending = std::move(pending_); pending_.reset();
    const auto serial = serial_, transformSerial = transformSerial_;
    const auto deltaMs = clockMs - preparedClockMs_;
    requestedClockMs_ = clockMs;
    const auto epoch = Render::NextAnimationSerial();
    const bool particles = viewport_->HasParticleSupport();
    const bool submit = needsSubmit_ || build;
    const auto previous = frame_; auto transforms = transforms_;
    // 选类型即在后台预备资产；尚无有效落点时这些CPU变换绝不提交到GPU。
    // 第一次有效地形命中可直接平移已有缓存，不用从这时才开始模型/贴图解码。
    if (build && transforms.empty()) {
        transforms.reserve(count);
        for (const auto& request : pending->requests) transforms.push_back(Render::ModelInstanceTransform(request));
    }
    const auto sources = build ? std::optional<WorldTerrainSources>(pending->sources) : residentSources_;
    const auto requests = build ? pending->requests : residentRequests_;
    work->service = service_; work->source = source_;
    const auto helperCache = helperCache_;
    auto* worker = QThread::create([work, pending = std::move(pending), previous, transforms, epoch, camera, particles, helperCache, deltaMs]() mutable {
        try {
            if (pending) {
                if (!work->source) {
                    auto source = WorldResourceReader::Open(pending->sources);
                    if (!source.Succeeded()) { work->diagnostics = std::move(source.diagnostics); return; }
                    work->source = std::shared_ptr<WorldResourceReader>(std::move(*source.value));
                }
                const auto valid = work->source->VerifyCurrentSource();
                if (!valid.Succeeded()) { work->diagnostics = valid.diagnostics; return; }
                if (!pending->presentations.empty()) {
                    Services::PlacementPresentationContext context{pending->presentationVersion, pending->helperResourceIdentity,
                        WorldPathingHelperResourceReader(work->source)};
                    auto helpers = PrepareWorldPathingHelpers(pending->requests, pending->presentations, context, work->source, helperCache);
                    if (!helpers.Succeeded()) { work->diagnostics = std::move(helpers.diagnostics); return; }
                    work->helpers = std::move(*helpers.value);
                }
                std::vector<Render::AnimationInstanceSpec> specs;
                specs.reserve(pending->requests.size());
                for (std::size_t i = 0; i < pending->requests.size(); ++i) {
                    Render::AnimationInstanceSpec spec; spec.identity = {i + 1, epoch};
                    spec.hostKey = "placement-preview:" + std::to_string(i); spec.model = pending->requests[i];
                    spec.playing = true;
                    specs.push_back(std::move(spec));
                }
                auto limits = Render::InteractiveAnimationPreviewLimits();
                limits.retainUnsupportedStatic = true; limits.enableParticles = particles;
                auto service = Render::ModelAnimationService::Create({epoch, pending->revision, pending->sources.archiveSha}, specs,
                    [&](auto path, auto maximum) { return work->source->ReadAnimationResource(path, maximum); }, limits);
                if (!service.Succeeded()) { work->diagnostics = std::move(service.diagnostics); return; }
                work->service = std::move(*service.value);
                // 优化缓存失败不改变预览资格；正式摆放仍可正常读取/解析。
                auto cached = work->service->CaptureAssetSnapshot();
                if (cached.Succeeded()) work->preloadedAssets = std::move(*cached.value);
            }
            const auto valid = work->source->VerifyCurrentSource();
            if (!valid.Succeeded()) { work->diagnostics = valid.diagnostics; return; }
            Render::AnimationAdvance advance; advance.worldCamera = camera;
            const auto& specs = work->service->Scene()->specs;
            for (std::size_t i = 0; i < specs.size(); ++i) {
                Render::AnimationEdit edit; edit.identity = specs[i].identity; edit.transform = transforms[i];
                if (!pending && previous && deltaMs && previous->states[i].playing) {
                    // 时钟来自独立单调计时器；鼠标/镜头只改变展示，不重置Stand相位或伪造delta。
                    const auto& state = previous->states[i];
                    edit.elapsedMs = state.elapsedMs + deltaMs; edit.globalMs = state.globalMs + deltaMs;
                    const auto& sequences = work->service->Scene()->sequences[i];
                    if (state.sequenceIndex < sequences.size()) {
                        const auto& sequence = sequences[state.sequenceIndex];
                        if (sequence.flags == 1 && sequence.end >= sequence.start
                            && *edit.elapsedMs >= static_cast<std::uint64_t>(sequence.end) - sequence.start) {
                            edit.elapsedMs = static_cast<std::uint64_t>(sequence.end) - sequence.start;
                            edit.playing = false;
                        }
                    }
                }
                advance.edits.push_back(std::move(edit));
            }
            auto frame = work->service->Prepare(pending ? nullptr : previous.get(), advance);
            if (!frame.Succeeded()) { work->diagnostics = std::move(frame.diagnostics); return; }
            const auto stillCurrent = work->source->VerifyCurrentSource();
            if (!stillCurrent.Succeeded()) { work->diagnostics = stillCurrent.diagnostics; return; }
            work->frame = std::move(*frame.value);
        } catch (const std::exception& error) {
            work->diagnostics.push_back({DiagnosticSeverity::Error, "World", "WORLD_PREVIEW_BUILD", error.what(), std::nullopt});
        }
    });
    worker_ = worker;
    connect(worker, &QThread::finished, this, [this, work, serial, transformSerial, submit, sources, requests, clockMs] {
        worker_ = nullptr;
        if (serial == serial_ && viewport_ && current_ && current_()) {
            if (work->frame && work->source) {
                const auto current = work->source->VerifyCurrentSource();
                if (!current.Succeeded()) { work->frame.reset(); work->diagnostics = current.diagnostics; }
            }
            if (!work->frame) {
                playbackFaulted_ = true; PausePlaybackClock();
                acceptedTransformSerial_ = transformSerial; // 同一坏请求不自动重试。
                needsSubmit_ = false;
                RemoveLayer();
                service_.reset(); frame_.reset(); source_.reset(); preloadedAssets_.reset();
                residentSources_.reset(); residentRequests_.clear();
                helperCache_.reset(); residentHelpers_.reset();
                cameraDependent_ = false; preparedCamera_.reset(); preparedTransformSerial_ = 0;
                if (!work->diagnostics.empty()) emit Message(QString::fromUtf8(work->diagnostics.front().code + "：" + work->diagnostics.front().message));
            } else {
                service_ = work->service; source_ = work->source; frame_ = work->frame;
                if (work->preloadedAssets) preloadedAssets_ = std::move(work->preloadedAssets);
                preparedClockMs_ = clockMs;
                residentSources_ = sources; residentRequests_ = requests;
                if (work->helpers) {
                    helperCache_ = work->helpers->cache;
                    if (work->helpers->unknownFootprints)
                        emit Message(tr("路径阻断器 · 占地来源未知，显示小标记；不是原版占地。"));
                    residentHelpers_ = std::move(work->helpers);
                }
                else residentHelpers_.reset();
                preparedTransformSerial_ = transformSerial; preparedCamera_ = work->frame->worldCamera;
                cameraDependent_ = service_->Scene()->particleInstanceCount != 0;
                for (const auto& model : service_->Scene()->pickModels) if (model.source)
                    Render::ForEachMdxNode(*model.source, [this](const auto& node) { cameraDependent_ |= (node.flags & 0xF8u) != 0; });
                // 构建期间真正离开视口：只接收 CPU 驻留内容，不安装迟到的幽灵预览。
                if (transforms_.empty() || !viewport_->isVisible()) { UpdatePlaybackClock(); BeginNext(); return; }
                if (submissionDeferred_) {
                    // CPU驻留可以完成；等待同一主roster终态后才触碰附加层。
                    acceptedTransformSerial_ = transformSerial; needsSubmit_ = true;
                    return;
                }
                UpdatePlaybackClock();
                // 回调落地时使用最新鼠标落点；缓存快路径不提交后台任务的旧位置。
                // 新粒子帧尚未提交，不能把它误判成之前已驻留的完整帧；仍走下面的Update。
                if (!frame_->particleCheckpoint && PresentCachedTransforms()) { BeginNext(); return; }
                // 有粒子检查点的帧不能只改contentVersion；其检查点必须保持服务发出的同一身份。
                // 无粒子缓存平移可能已推进显示版本，只有此时需要新的显示序号。
                if (!frame_->particleCheckpoint) {
                    auto presented = std::make_shared<Render::AnimationFrame>(*frame_);
                    presented->contentVersion = Render::NextAnimationSerial(); frame_ = std::move(presented);
                }
                // 移动期间允许接收这一份有效落点；下面继续合并到最新 transforms，避免鼠标持续移动时饿死预览。
                const auto result = submit || !submitted_ ? viewport_->SetModelLayer(layerId_, service_->Scene(), frame_)
                                                        : viewport_->UpdateModelLayer(layerId_, frame_);
                if (result.Succeeded()) {
                    const bool placementChanged = submit || !submitted_ || acceptedTransformSerial_ != transformSerial;
                    acceptedTransformSerial_ = transformSerial;
                    submitted_ = true; needsSubmit_ = false;
                    // 时间推进不改变地面叠加线；只在首次挂接/落点改变时唤起界面刷新。
                    if (placementChanged) emit Prepared();
                } else {
                    playbackFaulted_ = true; PausePlaybackClock();
                    acceptedTransformSerial_ = transformSerial;
                    needsSubmit_ = false;
                    RemoveLayer();
                    if (!result.diagnostics.empty()) emit Message(QString::fromUtf8(result.diagnostics.front().code + "：" + result.diagnostics.front().message));
                }
            }
        }
        BeginNext();
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start(QThread::LowPriority);
}
}
