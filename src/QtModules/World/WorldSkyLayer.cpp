#include "WorldSkyLayer.h"
#include "WorldViewport.h"
#include "War3MapCore/RenderPluginModelLayers.h"

#include <QEvent>

#include <algorithm>
#include <exception>

namespace War3Map::Modules {
namespace {
bool SameSession(const Render::AnimationSession& a, const Render::AnimationSession& b) {
    return a.generation == b.generation && a.documentRevision == b.documentRevision && a.sourceIdentity == b.sourceIdentity;
}
QString DiagnosticText(const std::vector<Diagnostic>& diagnostics) {
    if (diagnostics.empty()) return QStringLiteral("天空模型没有显示");
    return QString::fromUtf8(diagnostics.front().code + "：" + diagnostics.front().message);
}
}
struct WorldSkyLayer::Work {
    std::shared_ptr<Render::ModelAnimationService> service;
    std::shared_ptr<const Render::AnimationFrame> frame;
    std::vector<Diagnostic> diagnostics;
    bool animated = false;
};
WorldSkyLayer::WorldSkyLayer(WorldViewport& viewport, QObject* parent)
    : QObject(parent), viewport_(&viewport), layerId_(Render::NextAnimationSerial()) {
    viewport.installEventFilter(this);
    // 同一个定时器检查主场景换代（换代后重交）并推进动作。
    timer_.setInterval(100);
    connect(&timer_, &QTimer::timeout, this, &WorldSkyLayer::Tick);
}
WorldSkyLayer::~WorldSkyLayer() {
    timer_.stop();
    if (worker_) disconnect(worker_, nullptr, this, nullptr);
    if (viewport_) {
        viewport_->removeEventFilter(this);
        (void)viewport_->RemoveModelLayer(layerId_);
    }
}
void WorldSkyLayer::Show(std::string modelPath, WorldTerrainSources sources, Render::AnimationSession session,
    std::function<bool()> current) {
    if (modelPath.empty()) { Clear(); return; }
    if (modelPath == path_ && sources_ && SameWorldTerrainSources(*sources_, sources) && SameSession(session_, session)) {
        current_ = std::move(current);
        RebindParent();
        return;
    }
    Clear();
    path_ = std::move(modelPath); sources_ = std::move(sources); session_ = std::move(session); current_ = std::move(current);
    const auto serial = serial_;
    const auto epoch = Render::NextAnimationSerial();
    auto work = std::make_shared<Work>();
    auto* worker = QThread::create([work, path = path_, sources = *sources_, session = session_, epoch] {
        try {
            auto reader = WorldResourceReader::Open(sources);
            if (!reader.Succeeded()) { work->diagnostics = std::move(reader.diagnostics); return; }
            const std::shared_ptr<WorldResourceReader> source(std::move(*reader.value));
            Render::AnimationInstanceSpec spec;
            spec.identity = {1, epoch}; spec.hostKey = "world-sky";
            spec.model.modelPath = path; spec.elapsedMs = 0; spec.playing = true;
            auto limits = Render::InteractiveAnimationPreviewLimits();
            limits.retainUnsupportedStatic = true; limits.enableParticles = false; limits.poseWorkers = 1;
            limits.geometry.maxModels = 1; limits.geometry.maxInstances = 1;
            limits.geometry.maxTextureDimension = 1024; // 天空铺满画面，贴图按 1024 读取
            auto service = Render::ModelAnimationService::Create(session, std::span(&spec, 1),
                [&](auto resource, auto maximum) { return source->ReadAnimationResource(resource, maximum); }, limits);
            if (!service.Succeeded()) { work->diagnostics = std::move(service.diagnostics); return; }
            const auto scene = (*service.value)->Scene();
            if (!scene->unavailableInstances.empty() || scene->models.empty()) {
                const auto code = scene->unavailableInstances.empty() ? std::string("没有可画的网格")
                                                                       : scene->unavailableInstances.front().code;
                work->diagnostics.push_back({DiagnosticSeverity::Warning, "World.Sky", "WORLD_SKY_MODEL_UNAVAILABLE",
                    "天空模型“" + path + "”无法显示（" + code + "）。", std::nullopt});
                return;
            }
            auto frame = (*service.value)->Prepare(nullptr);
            if (!frame.Succeeded()) { work->diagnostics = std::move(frame.diagnostics); return; }
            const auto& states = (*frame.value)->states;
            const auto& sequences = scene->sequences[0];
            work->animated = !states.empty() && states[0].sequenceIndex < sequences.size()
                && sequences[states[0].sequenceIndex].end > sequences[states[0].sequenceIndex].start;
            const auto valid = source->VerifyCurrentSource();
            if (!valid.Succeeded()) { work->diagnostics = valid.diagnostics; return; }
            work->service = std::move(*service.value); work->frame = std::move(*frame.value);
        } catch (const std::exception& error) {
            work->diagnostics.push_back({DiagnosticSeverity::Error, "World.Sky", "WORLD_SKY_BUILD", error.what(), std::nullopt});
        }
    });
    worker_ = worker;
    connect(worker, &QThread::finished, this, [this, work, serial] {
        worker_ = nullptr;
        if (serial != serial_ || !current_ || !current_()) return;
        if (!work->frame) { Report(DiagnosticText(work->diagnostics), work->diagnostics); return; }
        service_ = std::move(work->service); frame_ = std::move(work->frame); animated_ = work->animated;
        clock_.start();
        Submit(); UpdateTimer();
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start(QThread::LowPriority);
}
void WorldSkyLayer::RebindParent() {
    // 主名单事务已清掉旧父场景上的层；不能用本地 submitted 猜它仍在。
    submitted_ = false; removalPending_ = false; parentEpoch_ = 0; failedEpoch_ = 0;
    Submit(); UpdateTimer();
}
void WorldSkyLayer::SetSubmissionDeferred(bool deferred) {
    if (deferred_ == deferred) return;
    deferred_ = deferred;
    if (deferred) return;
    if (removalPending_) Remove();
    Submit(); UpdateTimer();
}
void WorldSkyLayer::Clear() {
    ++serial_; timer_.stop();
    Remove();
    service_.reset(); frame_.reset(); path_.clear(); sources_.reset(); session_ = {}; current_ = {};
    animated_ = false; parentEpoch_ = failedEpoch_ = 0; lastMessage_.clear(); lastDiagnostics_.clear();
}
void WorldSkyLayer::Remove() {
    if (deferred_) { removalPending_ = submitted_ || removalPending_; submitted_ = false; return; }
    if (viewport_ && (submitted_ || removalPending_)) (void)viewport_->RemoveModelLayer(layerId_);
    submitted_ = false; removalPending_ = false;
}
void WorldSkyLayer::UpdateTimer() {
    if (frame_ && viewport_ && viewport_->isVisible()) { if (!timer_.isActive()) timer_.start(); }
    else timer_.stop();
}
void WorldSkyLayer::Submit() {
    if (deferred_ || !viewport_ || !service_ || !frame_ || !viewport_->isVisible() || !viewport_->RendererReady()) return;
    if (current_ && !current_()) { Clear(); return; }
    const auto epoch = viewport_->MeasurementModelEpoch();
    if (!epoch) return; // 主场景尚未就绪；附加层必须挂在有效的主场景上。
    const auto status = viewport_->SetModelLayer(layerId_, service_->Scene(), frame_, WAR3_RENDER_MODEL_LAYER_SKY);
    submitted_ = status.Succeeded();
    parentEpoch_ = submitted_ ? epoch : 0;
    failedEpoch_ = submitted_ ? 0 : epoch; // 同一主场景上失败不反复重试
    if (submitted_) Report(tr("天空模型：%1").arg(QString::fromStdString(path_)), {});
    else Report(DiagnosticText(status.diagnostics), status.diagnostics);
}
void WorldSkyLayer::Tick() {
    if (!viewport_ || !viewport_->isVisible() || !frame_) { UpdateTimer(); return; }
    if (deferred_) return;
    const auto epoch = viewport_->MeasurementModelEpoch();
    if (!submitted_ || epoch != parentEpoch_) {
        if (epoch != failedEpoch_) Submit();
        return;
    }
    if (!animated_ || !service_) return;
    Render::AnimationAdvance advance;
    advance.deltaMs = static_cast<std::uint64_t>(std::clamp<qint64>(clock_.restart(), 0, 1000));
    auto next = service_->Prepare(frame_.get(), advance);
    if (!next.Succeeded()) {
        animated_ = false; // 停在当前姿势，不反复报同一错误
        Report(DiagnosticText(next.diagnostics), next.diagnostics);
        return;
    }
    if (!viewport_->UpdateModelLayer(layerId_, *next.value).Succeeded()) { submitted_ = false; return; } // 下次重交
    frame_ = std::move(*next.value);
}
bool WorldSkyLayer::eventFilter(QObject* watched, QEvent* event) {
    if (watched == viewport_) {
        if (event->type() == QEvent::Hide) { timer_.stop(); Remove(); }
        else if (event->type() == QEvent::Show)
            QTimer::singleShot(0, this, [this] { if (viewport_ && viewport_->isVisible()) RebindParent(); });
    }
    return QObject::eventFilter(watched, event);
}
void WorldSkyLayer::Report(QString text, std::vector<Diagnostic> diagnostics) {
    if (text == lastMessage_) return;
    lastMessage_ = std::move(text); lastDiagnostics_ = std::move(diagnostics);
    emit Message(lastMessage_);
}
} // namespace War3Map::Modules
