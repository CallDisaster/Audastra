#pragma once

#include "War3MapCore/Render/ViewRay.h"
#include "War3MapCore/RenderPluginV2.h"
#include "War3MapCore/RenderPluginEditorVisibility.h"
#include "War3MapCore/RenderPluginModelLayers.h"

#include <cstddef>
#include <cstdint>
#include <vector>
#include <memory>
#include <optional>

namespace War3Map::Render {
struct AnimationFrame;

// 不依赖 Qt 的真实完成帧数据。view 和 camera 来自与 rgba 同一次渲染/读回的回执，
// 不是由当前请求相机或截图后另一次状态查询重建。每行紧密 RGBA8，首行在图像顶部。
struct CapturedFrameRgba {
    std::uint64_t documentRevision = 0;
    ViewProjectionFrame view;
    War3RenderCameraV2 camera{};
    std::vector<std::byte> rgba;
    // 仅动画读回填写：同帧 skin 回执绑定到拥有实例/poseVersion 表的宿主帧包。
    // 静态调用方保持原字段和行为；这是进程内 C++ 数据，不改变插件 ABI。
    std::optional<War3RenderSkinStatusV2> skin;
    std::optional<War3RenderParticleStatusV2> particles;
    std::optional<War3RenderEditorPresentationStatusV2> presentation;
    std::shared_ptr<const AnimationFrame> animation;
    // 和 RGBA 同一次完成帧的附加层身份；空列表也绑定 layersRevision。
    std::uint64_t modelLayersRevision = 0;
    std::vector<War3RenderModelLayerStatusV2> modelLayers;
    std::vector<std::shared_ptr<const AnimationFrame>> modelLayerFrames; // 与 modelLayers 同序，绑定实例/姿势身份。
};

inline constexpr std::size_t kDefaultFrameCaptureBytes = 16 * 1024 * 1024;
inline constexpr std::size_t kMaximumFrameCaptureBytes = 64 * 1024 * 1024;

} // namespace War3Map::Render
