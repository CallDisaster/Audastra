#pragma once
#include "War3MapCore/Render/ModelAnimation.h"
#include "War3MapCore/RenderPluginEditorVisibility.h"
#include "War3MapCore/RenderPluginInstanceRoster.h"

namespace War3Map::Render {
// 进程内完成帧回执；粒子状态只能由同一次组合读回填入，不通过随后查询拼接。
struct AnimationCaptureReceipt {
    War3RenderCapturedFrameV2 frame{};
    War3RenderSkinStatusV2 skin{};
    std::optional<War3RenderParticleStatusV2> particles;
    std::optional<War3RenderEditorPresentationStatusV2> presentation;
    AnimationCaptureReceipt() = default;
    AnimationCaptureReceipt(const War3RenderSkinnedCapturedFrameV2& value) : frame(value.frame), skin(value.skin) {}
};
// 借用掩码只活到提交返回；插件深拷贝。编号来自实际AnimationFrame实例/粒子draw映射。
struct EditorVisibilityMasks {
    std::span<const std::uint8_t> instances;
    std::span<const std::uint8_t> particleDraws;
    std::uint64_t presentationRevision = 0;
};
// 一份插件实例由外部宿主管理生命周期；该薄层不拥有 handle，不在析构时触碰 UI/GPU。
// 记录 CPU 已接收目标。Capture 只接受同一目标的实际完成帧，不拿当前状态拼旧像素。
class AnimationRenderer {
public:
    AnimationRenderer(const War3RenderPluginApiV2* api, War3RendererHandle handle) : api_(api), handle_(handle) {}
    [[nodiscard]] static bool Supported(const War3RenderPluginApiV2* api) noexcept;
    [[nodiscard]] static bool ParticleSupported(const War3RenderPluginApiV2* api) noexcept;
    [[nodiscard]] static bool PresentationSupported(const War3RenderPluginApiV2* api) noexcept;
    [[nodiscard]] static bool RosterSupported(const War3RenderPluginApiV2* api) noexcept;
    [[nodiscard]] static bool AssetAppendSupported(const War3RenderPluginApiV2* api, War3RendererHandle handle) noexcept;
    // queued时不替换Target；完成帧通过Poll后才发布新场景/帧，调用方再发布Player/点选身份。
    [[nodiscard]] Result<std::uint64_t> QueueRoster(std::shared_ptr<const AnimatedModelScene> scene,
        std::shared_ptr<const AnimationFrame> frame, const EditorVisibilityMasks& visibility, bool assetsChanged = false);
    [[nodiscard]] Result<War3RenderInstanceRosterResultV2> PollRoster(std::uint64_t ticket);
    // 取消只是请求；QUEUED/失败时仍保留拥有的候选，只有确切终态才释放。
    [[nodiscard]] Result<War3RenderInstanceRosterResultV2> CancelRoster(std::uint64_t ticket);
    [[nodiscard]] Status Submit(std::shared_ptr<const AnimatedModelScene> scene,
        std::shared_ptr<const AnimationFrame> frame, const EditorVisibilityMasks* visibility = nullptr);
    [[nodiscard]] Status Update(std::shared_ptr<const AnimationFrame> frame, const EditorVisibilityMasks* visibility = nullptr);
    [[nodiscard]] Status Present(const EditorVisibilityMasks& visibility); // 同姿势显隐，不重新上传
    [[nodiscard]] Result<War3RenderSkinStatusV2> StatusNow() const;
    [[nodiscard]] Result<AnimationCaptureReceipt> Capture(
        std::span<std::byte> rgba, std::uint64_t viewRevision) const;
    [[nodiscard]] std::shared_ptr<const AnimationFrame> Target() const noexcept { return frame_; }
    [[nodiscard]] std::optional<War3RenderEditorPresentationTargetV2> PresentationTarget() const noexcept { return presentationTarget_; }
    [[nodiscard]] std::uint64_t PresentationRevision() const noexcept { return presentationRevision_; }
private:
    const War3RenderPluginApiV2* api_ = nullptr;
    War3RendererHandle handle_ = nullptr;
    std::shared_ptr<const AnimatedModelScene> scene_;
    std::shared_ptr<const AnimationFrame> frame_;
    std::optional<War3RenderEditorPresentationTargetV2> presentationTarget_;
    std::uint64_t presentationRevision_ = 0;
    bool particleMode_ = false;
    std::uint64_t pendingRosterTicket_ = 0, pendingAssetIdentity_ = 0, pendingPresentationRevision_ = 0, pendingLayersRevision_ = 0;
    War3RenderEditorPresentationTargetV2 pendingTarget_{};
    std::shared_ptr<const AnimatedModelScene> pendingScene_;
    std::shared_ptr<const AnimationFrame> pendingFrame_;
};
} // namespace War3Map::Render
