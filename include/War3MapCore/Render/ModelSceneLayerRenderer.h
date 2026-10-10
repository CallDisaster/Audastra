#pragma once
#include "War3MapCore/Render/ModelAnimation.h"
#include "War3MapCore/RenderPluginModelLayers.h"

namespace War3Map::Render {
// UI/Agent 共用附加模型层适配。只借用 renderer，析构不访问 GPU；宿主显式 Remove。
class ModelSceneLayerRenderer {
public:
    ModelSceneLayerRenderer(const War3RenderPluginApiV2* api, War3RendererHandle handle, std::uint64_t id)
        : api_(api), handle_(handle), id_(id) {}
    [[nodiscard]] static bool Supported(const War3RenderPluginApiV2* api) noexcept;
    [[nodiscard]] Status Submit(std::shared_ptr<const AnimatedModelScene> scene, std::shared_ptr<const AnimationFrame> frame,
        std::uint32_t role = WAR3_RENDER_MODEL_LAYER_MODEL);
    [[nodiscard]] Status Update(std::shared_ptr<const AnimationFrame> frame);
    [[nodiscard]] Status Remove();
    [[nodiscard]] std::uint64_t Revision() const noexcept { return revision_; }
    [[nodiscard]] std::shared_ptr<const AnimationFrame> Target() const noexcept { return frame_; }
private:
    const War3RenderPluginApiV2* api_ = nullptr;
    War3RendererHandle handle_ = nullptr;
    std::uint64_t id_ = 0, parentEpoch_ = 0, revision_ = 0;
    std::uint32_t role_ = WAR3_RENDER_MODEL_LAYER_MODEL;
    std::shared_ptr<const AnimatedModelScene> scene_;
    std::shared_ptr<const AnimationFrame> frame_;
};
}
