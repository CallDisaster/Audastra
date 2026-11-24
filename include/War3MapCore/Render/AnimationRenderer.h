#pragma once
#include "War3MapCore/Render/ModelAnimation.h"
#include "War3MapCore/RenderPluginEditorVisibility.h"
#include "War3MapCore/RenderPluginInstanceRoster.h"
#include "War3MapCore/Render/RenderPreparedUpdatePlugin.h"

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
// 后台预备插件更新时需要的提交方式：粒子组合帧还是纯姿势，粒子是否带材质尾部。
struct AnimationPreparedMode {
    bool particleMode = false, particleMaterial = false;
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
    // preparedTicket：后台已为这一帧预备的插件票据（见 PrepareUpdate）；插件不支持或票据对不上时照常完整复制。
    [[nodiscard]] Status Update(std::shared_ptr<const AnimationFrame> frame, const EditorVisibilityMasks* visibility = nullptr,
        std::uint64_t preparedTicket = 0);
    // 当前目标可在后台预备时返回提交方式（编辑器显隐呈现、无待定实例事务）；否则为空。
    [[nodiscard]] std::optional<AnimationPreparedMode> PreparedMode() const;
    void SetPreparedSubmit(War3SubmitPreparedEditorVisibilityV1 submit) noexcept { preparedSubmit_ = submit; }
    // 最近一次 Update 是否直接用上了后台预备的副本（测量用）。
    [[nodiscard]] bool LastUpdateUsedPrepared() const noexcept { return lastUpdateUsedPrepared_; }
    // 任意线程：按与 Update 相同的描述预备一帧，返回票据；0 表示未预备。调用方保证 handle 在调用期间有效。
    // prepareChanges 可用时附上帧的变化清单（帧没有基准时不附），插件据此只复制、只上传变化实例。
    [[nodiscard]] static std::uint64_t PrepareUpdate(War3PrepareEditorUpdateV1 prepare,
        War3PrepareEditorUpdateChangesV1 prepareChanges, War3RendererHandle handle,
        const AnimatedModelScene& scene, const AnimationFrame& frame, AnimationPreparedMode mode) noexcept;
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
    War3SubmitPreparedEditorVisibilityV1 preparedSubmit_ = nullptr;
    bool lastUpdateUsedPrepared_ = false;
    std::uint64_t pendingRosterTicket_ = 0, pendingAssetIdentity_ = 0, pendingPresentationRevision_ = 0, pendingLayersRevision_ = 0;
    War3RenderEditorPresentationTargetV2 pendingTarget_{};
    std::shared_ptr<const AnimatedModelScene> pendingScene_;
    std::shared_ptr<const AnimationFrame> pendingFrame_;
};
} // namespace War3Map::Render
