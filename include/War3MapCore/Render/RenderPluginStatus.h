#pragma once

#include <initializer_list>
#include <string_view>

namespace War3Map::Render {

// 渲染插件成功完成一帧时，状态消息写成“<后端> / <画面内容>[<附注>]”；上传失败、沿用上一份等说明不以这些开头。
// 编辑器靠它区分 result=0 的成功帧和上传失败后仍返回的背景帧，所以各后端的描述只在这里定义。
inline constexpr char kRenderStatusD3D11Warp[] = "D3D11 WARP / Feature Level 11.0";
inline constexpr char kRenderStatusD3D11Hardware[] = "D3D11 hardware / Feature Level 11.0";
inline constexpr char kRenderStatusVulkanHardware[] = "Vulkan hardware";

// message 可以带插件定长缓冲末尾的空字符。
[[nodiscard]] constexpr bool IsRenderSuccessStatus(std::string_view message) noexcept {
    for (const std::string_view backend : {std::string_view(kRenderStatusD3D11Warp),
             std::string_view(kRenderStatusD3D11Hardware), std::string_view(kRenderStatusVulkanHardware)})
        if (message.starts_with(backend) && message.substr(backend.size()).starts_with(" / ")) return true;
    return false;
}

} // namespace War3Map::Render
