#pragma once

#include "War3MapCore/Render/AnimationPremFrame.h"
#include "War3MapCore/Render/FrameCapture.h"
#include "War3MapCore/Services/WorldInstanceRegistry.h"

#include <QJsonObject>
#include <QObject>
#include <QPointer>

namespace War3Map::Modules {
class WorldViewport;

// 一个 pooled 子层；只消费同次父帧冻结的 PREM 包，没有地图摆放/点选/事务入口。
class WorldPremPresenter final : public QObject {
public:
    static constexpr std::uint64_t LayerId = 0x5052454d504f4f4cull;
    explicit WorldPremPresenter(WorldViewport* viewport, QObject* parent = nullptr);
    ~WorldPremPresenter() override;
    void Bind(std::shared_ptr<const Render::AnimatedModelScene> scene,
        std::shared_ptr<const Services::WorldInstanceRegistry> registry,
        std::vector<MapData::PlacementId> owners, std::function<bool()> live);
    void Clear();
    void Refresh();
    [[nodiscard]] const Status& LastStatus() const noexcept { return status_; }
    [[nodiscard]] QString Summary() const;
    [[nodiscard]] Result<QJsonObject> CaptureSnapshot(const Render::CapturedFrameRgba& capture) const;
    // 调用方先核对当前宿主来源。这里只证明该权威父计划在同次GPU回执中没有子槽/残层，
    // 不要求可编辑摆放谱系，不授予有子槽的PREM绘制资格。缺失CPU计划不是空计划。
    [[nodiscard]] static Result<QJsonObject> CaptureEmptySnapshot(const Render::CapturedFrameRgba& capture,
        const std::shared_ptr<const Render::AnimatedModelScene>& currentScene,
        const std::shared_ptr<const Render::AnimationFrame>& currentFrame, std::uint64_t viewportRevision);
private:
    void Accept(std::shared_ptr<const Render::AnimatedModelScene> scene,
        std::shared_ptr<const Render::AnimationFrame> frame);
    [[nodiscard]] Status CheckFrame(const std::shared_ptr<const Render::AnimatedModelScene>& scene,
        const std::shared_ptr<const Render::AnimationFrame>& frame) const;
    void Remove();
    QPointer<WorldViewport> viewport_;
    std::shared_ptr<const Render::AnimatedModelScene> source_;
    std::shared_ptr<const Services::WorldInstanceRegistry> registry_;
    std::vector<MapData::PlacementId> owners_;
    std::function<bool()> live_;
    std::shared_ptr<const Render::AnimationFrame> acceptedParent_;
    std::shared_ptr<const Render::AnimationPremFrame> packet_;
    std::uint64_t rendererEpoch_ = 0, parentModelEpoch_ = 0;
    bool submitted_ = false, accepting_ = false;
    Status status_;
};
} // namespace War3Map::Modules
