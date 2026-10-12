#include "WorldCliffLayer.h"
#include "WorldViewport.h"

namespace War3Map::Modules {
Result<WorldCliffLayerFrame> PrepareWorldCliffLayer(std::shared_ptr<const Render::TerrainCliffs> cliffs,
    const Render::AnimationResourceReader& read, Render::AnimationSession session) {
    WorldCliffLayerFrame output; output.cliffs = cliffs;
    if (!cliffs || cliffs->meshes.empty()) return Result<WorldCliffLayerFrame>::Success(std::move(output));
    auto limits = Render::InteractiveAnimationPreviewLimits();
    limits.enableParticles = false;
    auto service = Render::ModelAnimationService::Create(std::move(session), {}, read, limits,
        std::span<const Render::BakedModelMesh>(cliffs->meshes));
    if (!service.Succeeded()) return {std::nullopt, std::move(service.diagnostics)};
    auto frame = (*service.value)->Prepare(nullptr, {});
    if (!frame.Succeeded()) return {std::nullopt, std::move(frame.diagnostics)};
    output.scene = (*service.value)->Scene(); output.frame = std::move(*frame.value);
    return Result<WorldCliffLayerFrame>::Success(std::move(output));
}

WorldCliffLayerPresenter::WorldCliffLayerPresenter(WorldViewport* viewport, QObject* parent)
    : QObject(parent), viewport_(viewport) {
    // 主场景换代（换图、目录变化、实例增量）会使附加层失效；按父场景身份检查并重提交。
    timer_.setInterval(250);
    connect(&timer_, &QTimer::timeout, this, [this] {
        if (!viewport_ || !shown_.scene) return;
        const auto epoch = viewport_->MeasurementModelEpoch();
        if (submitted_ ? epoch != parentEpoch_ : epoch != failedEpoch_) Submit(); // 同一父场景上失败不反复重试。
    });
}
WorldCliffLayerPresenter::~WorldCliffLayerPresenter() { Clear(); }

void WorldCliffLayerPresenter::Show(WorldCliffLayerFrame frame) {
    if (frame.cliffs == shown_.cliffs && frame.scene == shown_.scene && submitted_) return;
    shown_ = std::move(frame); submitted_ = false; failedEpoch_ = 0;
    if (!shown_.scene) {
        timer_.stop();
        if (viewport_) status_ = viewport_->RemoveModelLayer(LayerId);
        return;
    }
    timer_.start(); Submit();
}
void WorldCliffLayerPresenter::RebindParent() { submitted_ = false; failedEpoch_ = 0; if (shown_.scene) Submit(); }
void WorldCliffLayerPresenter::Clear() {
    timer_.stop();
    if (viewport_) status_ = viewport_->RemoveModelLayer(LayerId);
    shown_ = {}; submitted_ = false; parentEpoch_ = 0; failedEpoch_ = 0;
}
void WorldCliffLayerPresenter::Submit() {
    if (!viewport_ || !shown_.scene || !viewport_->RendererReady()) return;
    const auto epoch = viewport_->MeasurementModelEpoch();
    if (!epoch) return; // 主场景尚未就绪；附加层必须挂在有效父场景上。
    status_ = viewport_->SetModelLayer(LayerId, shown_.scene, shown_.frame);
    submitted_ = status_.Succeeded();
    parentEpoch_ = submitted_ ? epoch : 0;
    failedEpoch_ = submitted_ ? 0 : epoch;
}
} // namespace War3Map::Modules
