#pragma once
#include "War3MapCore/Services/WorldModelBugPlaceholderService.h"
#include <QObject>
#include <QPointer>
#include <QTimer>

namespace War3Map::Modules {
class WorldViewport;
class WorldModelBugPlaceholderPresenter final : public QObject {
public:
    explicit WorldModelBugPlaceholderPresenter(WorldViewport* viewport, QObject* parent = nullptr);
    ~WorldModelBugPlaceholderPresenter() override;
    // live必须复核当前地图、目录、会话、编辑修订及同一registry；flags只读当前typed对象。
    void Bind(std::shared_ptr<const Render::AnimatedModelScene> scene,
        std::shared_ptr<const Services::WorldInstanceRegistry> registry,
        std::vector<MapData::PlacementId> owners, std::function<bool()> live,
        Services::WorldModelBugFlagsReader flags);
    void BindReadOnly(std::shared_ptr<const Render::AnimatedModelScene> scene,
        std::shared_ptr<const Services::WorldModelBugReadOnlySource> source, std::function<bool()> live,
        Services::WorldModelBugFlagsReader flags);
    void Clear();
    void Refresh();
    [[nodiscard]] const Status& LastStatus() const noexcept { return status_; }
    [[nodiscard]] std::size_t Count() const noexcept { return plan_ ? plan_->markers.size() : 0; }
    // 最后完成原动画帧所准备的CPU计划；不是方块GPU绘制签收，保留completedContentVersion。
    [[nodiscard]] std::shared_ptr<const Services::WorldModelBugPlaceholderPlan> Snapshot() const {
        return plan_ && live_ && live_() ? plan_ : nullptr;
    }
private:
    static constexpr std::uint64_t LayerId = 0x4255474d4f44454cull;
    QPointer<WorldViewport> viewport_;
    QTimer timer_;
    std::shared_ptr<const Render::AnimatedModelScene> source_;
    std::shared_ptr<const Services::WorldInstanceRegistry> registry_;
    std::shared_ptr<const Services::WorldModelBugReadOnlySource> readOnlySource_;
    std::vector<MapData::PlacementId> owners_;
    std::function<bool()> live_;
    Services::WorldModelBugFlagsReader flags_;
    std::shared_ptr<const Services::WorldModelBugPlaceholderPlan> plan_;
    std::shared_ptr<const Render::OwnedStaticModelAsset> asset_;
    std::uint64_t rendererEpoch_ = 0;
    bool refreshing_ = false;
    Status status_;
};
}
