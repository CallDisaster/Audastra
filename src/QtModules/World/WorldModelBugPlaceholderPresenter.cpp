#include "WorldModelBugPlaceholderPresenter.h"
#include "WorldViewport.h"
#include <QScopedValueRollback>

namespace War3Map::Modules {
WorldModelBugPlaceholderPresenter::WorldModelBugPlaceholderPresenter(WorldViewport* viewport, QObject* parent)
    : QObject(parent), viewport_(viewport) {
    timer_.setInterval(100);
    connect(&timer_, &QTimer::timeout, this, [this] { Refresh(); });
}
WorldModelBugPlaceholderPresenter::~WorldModelBugPlaceholderPresenter() { Clear(); }
void WorldModelBugPlaceholderPresenter::Bind(std::shared_ptr<const Render::AnimatedModelScene> scene,
    std::shared_ptr<const Services::WorldInstanceRegistry> registry, std::vector<MapData::PlacementId> owners,
    std::function<bool()> live, Services::WorldModelBugFlagsReader flags) {
    if (source_ != scene || registry_ != registry || owners_ != owners) Clear();
    source_ = std::move(scene); registry_ = std::move(registry); owners_ = std::move(owners);
    live_ = std::move(live); flags_ = std::move(flags);
    if (source_ && registry_ && live_ && flags_) { timer_.start(); Refresh(); }
}
void WorldModelBugPlaceholderPresenter::Clear() {
    timer_.stop();
    if (viewport_) {
        viewport_->ClearBugPlaceholderPicking();
        status_ = viewport_->RemoveModelLayer(LayerId);
    }
    plan_.reset(); source_.reset(); registry_.reset(); owners_.clear(); live_ = {}; flags_ = {}; rendererEpoch_ = 0;
}
void WorldModelBugPlaceholderPresenter::Refresh() try {
    if (refreshing_ || !viewport_ || !source_ || !registry_ || !live_ || !flags_) return;
    QScopedValueRollback<bool> guard(refreshing_, true);
    if (!live_()) { Clear(); return; }
    const auto completed = viewport_->ReadCompletedAnimationFrameForBugPlaceholder(source_);
    if (!completed.Succeeded()) return; // 尚在上传时保留显示，拾取仍独立拒绝旧身份。
    auto planned = Services::PlanWorldModelBugPlaceholders(source_, **completed.value, registry_, owners_, flags_);
    if (!planned.Succeeded()) { auto failure = Status{std::move(planned.diagnostics)}; Clear(); status_ = std::move(failure); return; }
    const auto epoch = viewport_->MeasurementRendererEpoch();
    if (plan_ && rendererEpoch_ == epoch && plan_->markers == planned.value->markers
        && (plan_->markers.empty() || viewport_->HasBugPlaceholderPicking(plan_))) return;
    if (planned.value->markers.empty()) {
        viewport_->ClearBugPlaceholderPicking(); status_ = viewport_->RemoveModelLayer(LayerId);
        if (status_.Succeeded()) plan_ = std::make_shared<const Services::WorldModelBugPlaceholderPlan>(std::move(*planned.value));
        rendererEpoch_ = epoch; return;
    }
    if (!asset_) {
        auto asset = Render::CreateCheckerBlockerAsset(std::nullopt);
        if (!asset.Succeeded()) { status_ = {std::move(asset.diagnostics)}; return; }
        asset_ = std::move(*asset.value);
    }
    std::vector<Render::AnimationInstanceSpec> specs;
    specs.reserve(planned.value->markers.size());
    for (const auto& marker : planned.value->markers) {
        Render::AnimationInstanceSpec spec; spec.identity = marker.identity;
        spec.hostKey = "bug-placeholder:" + std::to_string(marker.identity.value);
        spec.model.modelPath = marker.modelPath; spec.model.ownedStaticAsset = asset_;
        spec.model.position = marker.position; spec.model.scale = {8, 8, 8}; specs.push_back(std::move(spec));
    }
    const Render::AnimationResourceReader noRead = [](std::string_view, std::uint64_t) {
        return Result<Render::AnimationResource>::Failure("World.BugPlaceholder", "WORLD_BUG_PLACEHOLDER_UNEXPECTED_READ",
            "原创方块不读取或替换原模型资源。");
    };
    auto service = Render::ModelAnimationService::Create({source_->generation, source_->documentRevision, source_->sourceIdentity},
        specs, noRead);
    if (!service.Succeeded()) { status_ = {std::move(service.diagnostics)}; return; }
    auto frame = (*service.value)->Prepare(nullptr, {});
    if (!frame.Succeeded()) { status_ = {std::move(frame.diagnostics)}; return; }
    auto next = std::make_shared<const Services::WorldModelBugPlaceholderPlan>(std::move(*planned.value));
    status_ = viewport_->SetModelLayer(LayerId, (*service.value)->Scene(), *frame.value);
    if (!status_.Succeeded()) { viewport_->ClearBugPlaceholderPicking(); return; }
    viewport_->SetBugPlaceholderPicking(LayerId, next, (*service.value)->Scene(), *frame.value, owners_, live_, flags_);
    plan_ = std::move(next); rendererEpoch_ = epoch;
} catch (...) {
    status_ = Status::Failure("World.BugPlaceholder", "WORLD_BUG_PLACEHOLDER_PREPARE_FAILED",
        "占位准备失败；原模型与地图保持原位，不使用猜测点选。");
    if (viewport_) viewport_->ClearBugPlaceholderPicking();
}
}
