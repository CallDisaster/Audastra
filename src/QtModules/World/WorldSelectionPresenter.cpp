#include "WorldSelectionPresenter.h"
#include "WorldViewport.h"

#include <QEvent>
#include <QThread>

#include <algorithm>
#include <exception>

namespace War3Map::Modules {
namespace {
bool SameSession(const Render::AnimationSession& a, const Render::AnimationSession& b) {
    return a.generation == b.generation && a.documentRevision == b.documentRevision
        && a.sourceIdentity == b.sourceIdentity;
}
bool SameCircle(const std::optional<Render::ModelSelectionDescription>& a,
    const std::optional<Render::ModelSelectionDescription>& b) {
    if (a.has_value() != b.has_value()) return false;
    return !a || (a->circleScale == b->circleScale && a->circleHeight == b->circleHeight
        && a->circleScaleKnown == b->circleScaleKnown && a->circleOnWater == b->circleOnWater
        && a->flyHeightOffset == b->flyHeightOffset
        && a->heightPreviewApproximate == b->heightPreviewApproximate);
}
bool SameRequests(const std::vector<Render::ModelInstanceRequest>& a,
    const std::vector<Render::ModelInstanceRequest>& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](const auto& x, const auto& y) {
        return x.position == y.position && SameCircle(x.selection, y.selection);
    });
}
bool SameLayout(const WorldSelectionCircleFrame& a, const WorldSelectionCircleFrame& b) {
    if (!a.frame || !b.frame || a.requestIndices != b.requestIndices
        || a.frame->instances.size() != b.frame->instances.size()
        || a.frame->groups.size() != b.frame->groups.size() || a.frame->alphas.size() != b.frame->alphas.size()) return false;
    return std::equal(a.frame->instances.begin(), a.frame->instances.end(), b.frame->instances.begin(),
        [](const auto& x, const auto& y) {
            return x.instance.model == y.instance.model
                && x.firstGroup == y.firstGroup && x.firstAlpha == y.firstAlpha;
        });
}
QString FailureText(const std::vector<Diagnostic>& diagnostics) {
    if (diagnostics.empty()) return QStringLiteral("选取圈准备失败，未提交新层");
    return QString::fromUtf8(diagnostics.front().code + "：" + diagnostics.front().message);
}
}
struct WorldSelectionPresenter::Work {
    std::shared_ptr<WorldResourceReader> reader;
    std::optional<WorldSelectionCircleAssets> assets;
    std::optional<WorldSelectionCircleFrame> prepared;
    std::vector<Diagnostic> diagnostics;
    bool sourceRejected = false;
};
WorldSelectionPresenter::WorldSelectionPresenter(WorldViewport& viewport, QObject* parent)
    : QObject(parent), viewport_(&viewport), layerId_(Render::NextAnimationSerial()) {
    viewport.installEventFilter(this);
    parentTimer_.setInterval(250);
    connect(&parentTimer_, &QTimer::timeout, this, &WorldSelectionPresenter::CheckParent);
    connect(&viewport, &WorldViewport::StatusChanged, this, [this] {
        if (!assets_ || needsSubmit_) BeginNext();
    }, Qt::QueuedConnection);
}
WorldSelectionPresenter::~WorldSelectionPresenter() {
    parentTimer_.stop();
    if (worker_) disconnect(worker_, nullptr, this, nullptr);
    if (viewport_) {
        viewport_->removeEventFilter(this);
        (void)viewport_->RemoveModelLayer(layerId_);
    }
    // worker只拥有快照和独立reader，finished自删；不在共享UI线程等待。
}
void WorldSelectionPresenter::SetSource(WorldTerrainSources sources, Render::AnimationSession session,
    std::function<bool()> sourceCurrent) {
    if (sources_ && SameWorldTerrainSources(*sources_, sources) && SameSession(session_, session)) {
        sourceCurrent_ = std::move(sourceCurrent);
        BeginNext();
        return;
    }
    Clear();
    sources_ = std::move(sources); session_ = std::move(session); sourceCurrent_ = std::move(sourceCurrent);
    observedParent_ = viewport_ ? viewport_->MeasurementModelEpoch() : 0;
    BeginNext(); // 可见页预载真实素材；没有选择时不会创建显示帧或开轮询。
}
void WorldSelectionPresenter::Show(std::vector<Render::ModelInstanceRequest> requests,
    std::shared_ptr<const MapData::TerrainGridSnapshot> terrain,
    std::function<std::optional<float>(double, double)> waterHeight,
    std::vector<MapData::PlacementId> selectedIds) {
    if (!selectedIds.empty() && selectedIds.size() != requests.size()) {
        Hide(); emit Message(tr("选取圈的对象身份与请求数量不符，未提交新层")); return;
    }
    if (requests.empty()) { Hide(); return; }
    const bool sameRequests = SameRequests(requests_, requests);
    const bool sameTerrain = terrain_ == terrain;
    const bool sameIds = !selectedIds.empty() && selectedIds == selectedIds_;
    const bool sameWater = !waterHeight && !waterHeight_;
    if (sameRequests && sameTerrain && sameWater && selectedIds == selectedIds_) {
        UpdateTimer(); BeginNext(); return;
    }
    // 有可信行身份时，移动可接收稍旧的有效位置；对象替换或地形更换须拒绝旧帧。
    if (!sameIds || !sameTerrain) { ++rosterSerial_; RemoveLayer(); prepared_.reset(); }
    requests_ = std::move(requests); selectedIds_ = std::move(selectedIds);
    terrain_ = std::move(terrain); waterHeight_ = std::move(waterHeight); ++requestSerial_;
    needsSubmit_ = needsSubmit_ || !submitted_;
    UpdateTimer(); BeginNext();
}
void WorldSelectionPresenter::Hide() {
    ++rosterSerial_; ++requestSerial_;
    requests_.clear(); selectedIds_.clear(); terrain_.reset(); waterHeight_ = {};
    prepared_.reset(); needsSubmit_ = false;
    parentTimer_.stop(); RemoveLayer();
}
void WorldSelectionPresenter::Clear() {
    ++sourceSerial_; ++bindingSerial_; Hide();
    sources_.reset(); session_ = {}; sourceCurrent_ = {}; reader_.reset(); assets_.reset();
    failed_.reset(); assetFailed_ = false; lastMessage_.clear(); preparedRequestSerial_ = 0; observedParent_ = 0;
}
void WorldSelectionPresenter::RemoveLayer() {
    if (submissionDeferred_) { removalPending_ = true; return; }
    removalPending_ = false;
    if (viewport_ && submitted_) {
        const auto removed = viewport_->RemoveModelLayer(layerId_);
        if (!removed.Succeeded()) {
            const auto text = FailureText(removed.diagnostics);
            if (lastMessage_ != text) { lastMessage_ = text; emit Message(text); }
        }
    }
    submitted_ = false; installed_.reset();
}
void WorldSelectionPresenter::RebindParent() {
    ++bindingSerial_; needsSubmit_ = !requests_.empty();
    observedParent_ = viewport_ ? viewport_->MeasurementModelEpoch() : 0;
    // 主roster事务已清掉旧parentEpoch层；不能用本地submitted猜测它仍然存在。
    submitted_ = false; installed_.reset(); removalPending_ = false;
    UpdateTimer(); BeginNext();
}
void WorldSelectionPresenter::SetSubmissionDeferred(bool deferred) {
    if (submissionDeferred_ == deferred) return;
    submissionDeferred_ = deferred;
    if (deferred) { parentTimer_.stop(); return; }
    // 明确失败终态保留旧parent，延期删除在该parent上执行；成功由RebindParent清掉旧层。
    if (removalPending_) RemoveLayer();
    UpdateTimer(); BeginNext();
}
WorldSelectionPresenter::Signature WorldSelectionPresenter::CurrentSignature() const {
    return {sourceSerial_, rosterSerial_, requestSerial_, bindingSerial_,
        viewport_ ? viewport_->MeasurementModelEpoch() : 0};
}
void WorldSelectionPresenter::UpdateTimer() {
    if (!submissionDeferred_ && viewport_ && viewport_->isVisible() && !viewport_->RendererFailed()
        && !requests_.empty() && sources_ && !assetFailed_) parentTimer_.start();
    else parentTimer_.stop();
}
void WorldSelectionPresenter::CheckParent() {
    if (!viewport_ || !viewport_->isVisible()) { parentTimer_.stop(); return; }
    if (sourceCurrent_ && !sourceCurrent_()) { Clear(); return; }
    const auto parent = viewport_->MeasurementModelEpoch();
    if (parent != observedParent_) { RebindParent(); return; }
    BeginNext(); // unchanged/failed签名会立即返回，不创建worker、不读MPQ、不重提交。
}
bool WorldSelectionPresenter::eventFilter(QObject* watched, QEvent* event) {
    if (watched == viewport_) {
        if (event->type() == QEvent::Hide) {
            parentTimer_.stop(); needsSubmit_ = !requests_.empty(); RemoveLayer();
        } else if (event->type() == QEvent::Show) {
            // eventFilter先于WorldViewport::showEvent创建后端，不能把启动中的api判成不支持。
            QTimer::singleShot(0, this, [this] { if (viewport_ && viewport_->isVisible()) RebindParent(); });
        }
    }
    return QObject::eventFilter(watched, event);
}
void WorldSelectionPresenter::BeginNext() {
    if (submissionDeferred_ || worker_ || !viewport_ || !viewport_->isVisible() || !sources_ || !sourceCurrent_ || assetFailed_) return;
    if (!sourceCurrent_()) { Clear(); return; }
    if (!viewport_->RendererReady()) { UpdateTimer(); return; }
    if (!viewport_->HasModelLayerSupport()) {
        assetFailed_ = true; UpdateTimer(); emit Message(tr("渲染插件不支持独立选取圈层")); return;
    }
    const auto signature = CurrentSignature();
    if (failed_ && *failed_ == signature) return;
    if (assets_ && prepared_ && preparedRequestSerial_ == requestSerial_ && !requests_.empty()
        && (needsSubmit_ || !submitted_ || !installed_ || prepared_->frame != installed_->frame)) {
        SubmitPrepared(signature);
        return;
    }
    if (assets_ && (requests_.empty() || (!needsSubmit_ && preparedRequestSerial_ == requestSerial_))) return;
    auto work = std::make_shared<Work>(); work->reader = reader_; work->assets = assets_;
    const auto sources = *sources_; const auto session = session_;
    const auto requests = requests_; const auto terrain = terrain_; const auto water = waterHeight_;
    const auto content = Render::NextAnimationSerial();
    auto* worker = QThread::create([work, sources, session, requests, terrain, water, content] {
        try {
            if (!work->reader) {
                auto reader = WorldResourceReader::Open(sources);
                if (!reader.Succeeded()) { work->diagnostics = std::move(reader.diagnostics); work->sourceRejected = true; return; }
                work->reader = std::shared_ptr<WorldResourceReader>(std::move(*reader.value));
            }
            const auto valid = work->reader->VerifyCurrentSource();
            if (!valid.Succeeded()) { work->diagnostics = valid.diagnostics; work->sourceRejected = true; return; }
            if (!work->assets) {
                auto assets = LoadWorldSelectionCircleAssets(session,
                    [&](auto path, auto maximum) { return work->reader->ReadAnimationResource(path, maximum); });
                if (!assets.Succeeded()) { work->diagnostics = std::move(assets.diagnostics); return; }
                work->assets = std::move(*assets.value);
            }
            if (!requests.empty()) {
                auto prepared = BuildWorldSelectionCircleFrame(*work->assets, requests, terrain.get(), content, water);
                if (!prepared.Succeeded()) { work->diagnostics = std::move(prepared.diagnostics); return; }
                work->prepared = std::move(*prepared.value);
            }
            const auto stillCurrent = work->reader->VerifyCurrentSource();
            if (!stillCurrent.Succeeded()) {
                work->diagnostics = stillCurrent.diagnostics; work->sourceRejected = true; work->prepared.reset();
            }
        } catch (const std::exception& error) {
            work->diagnostics.push_back({DiagnosticSeverity::Error, "World.Selection", "WORLD_SELECTION_BUILD", error.what(), std::nullopt});
        }
    });
    worker_ = worker;
    connect(worker, &QThread::finished, this, [this, work, signature] {
        worker_ = nullptr;
        if (signature.source != sourceSerial_ || !viewport_ || !sourceCurrent_) { BeginNext(); return; }
        if (!sourceCurrent_()) { Clear(); return; }
        if (work->sourceRejected || !work->assets) {
            assetFailed_ = true; failed_ = signature; RemoveLayer(); UpdateTimer();
            const auto text = FailureText(work->diagnostics);
            if (lastMessage_ != text) { lastMessage_ = text; emit Message(text); }
            return;
        }
        // 选择移动/取消不使已核验的素材失效；始终只在当前来源代次内保存CPU缓存。
        reader_ = work->reader; assets_ = std::move(work->assets);
        if (signature.roster != rosterSerial_ || requests_.empty()) { BeginNext(); return; }
        if (!work->prepared) {
            // 原请求可能在素材预载期间新增；这不是失败，下一次直接用驻留素材准备。
            if (work->diagnostics.empty()) { BeginNext(); return; }
            failed_ = signature;
            const auto text = FailureText(work->diagnostics);
            if (lastMessage_ != text) { lastMessage_ = text; emit Message(text); }
            BeginNext(); return;
        }
        prepared_ = std::move(work->prepared); preparedRequestSerial_ = signature.request;
        if (submissionDeferred_) return;
        if (!viewport_->isVisible() || signature.binding != bindingSerial_) { BeginNext(); return; }
        if (signature.parent != viewport_->MeasurementModelEpoch()) { RebindParent(); return; }
        SubmitPrepared(signature);
        BeginNext(); // 同一对象持续拖动时接收有效位置，再合并到最新位置，不饿死显示。
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start(QThread::LowPriority);
}
void WorldSelectionPresenter::SubmitPrepared(const Signature& signature) {
    if (submissionDeferred_ || !viewport_ || !prepared_ || !assets_) return;
    if (prepared_->frame->instances.empty()) {
        RemoveLayer(); needsSubmit_ = false;
    } else {
        const bool submit = needsSubmit_ || !submitted_ || !installed_ || !SameLayout(*installed_, *prepared_);
        auto result = submit ? viewport_->SetModelLayer(layerId_, assets_->scene, prepared_->frame)
            : viewport_->UpdateModelLayer(layerId_, prepared_->frame);
        // 旧插件的观测epoch可能为零；仅一次重新取得真实parent身份，绝不循环重试。
        if (!submit && !result.Succeeded()) result = viewport_->SetModelLayer(layerId_, assets_->scene, prepared_->frame);
        if (!result.Succeeded()) {
            failed_ = signature; needsSubmit_ = false;
            const auto text = FailureText(result.diagnostics);
            if (lastMessage_ != text) { lastMessage_ = text; emit Message(text); }
            return; // 后端拒绝保留旧层；不删除它以制造假成功。
        }
        submitted_ = true; needsSubmit_ = false; installed_ = prepared_; failed_.reset();
    }
    const auto text = tr("选取圈：预览近似；未知尺寸 %1，地表未采样 %2，水面待验证 %3")
        .arg(static_cast<qulonglong>(prepared_->unknownScale))
        .arg(static_cast<qulonglong>(prepared_->terrainUnavailable))
        .arg(static_cast<qulonglong>(prepared_->waterSemanticsPending));
    if (lastMessage_ != text) { lastMessage_ = text; emit Message(text); }
}
} // namespace War3Map::Modules
