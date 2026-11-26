#pragma once

#include "War3MapCore/RenderPluginV2.h"

#include <cstdint>

namespace War3Map::Render {

// 编辑器渲染插件扩展：截图推迟取像素。宿主开启后，本渲染器之后的各截图入口（captureFrameRgba 及分层、显隐、
// 蒙皮、粒子变体）照常等到同一次完成帧、照常核对并写出全部回执，但不写像素缓冲，也不等 GPU 把这一帧拷完：
// 插件把这一帧拷进暂存纹理即返回。像素随后凭回执里的 capture.ticket 取回；未拷完时返回“未就绪”，不阻塞调用方。
// 同一渲染器只保留最近一次推迟截图：新的推迟截图使旧票据失效。调用方仍按该帧 RGBA 字节数传入缓冲
// （插件照常核对容量）；开启失败（返回非零）时按同步截图处理。enabled 只接受 0 / 1。
using War3SetCapturePixelsDeferredV1 = std::int32_t (WAR3_RENDER_CALL*)(War3RendererHandle handle, std::uint32_t enabled);
// state：0 未就绪（稍后再取）；1 已写入 data，票据随即失效；2 失效（被新的推迟截图取代、读回失败或超时、已取过，
// 或这张票据从未推迟）。返回非零表示参数无效、size 小于该帧 RGBA 字节数（不写），或渲染器已失败。
using War3CollectDeferredCapturePixelsV1 = std::int32_t (WAR3_RENDER_CALL*)(War3RendererHandle handle,
    std::uint64_t ticket, void* data, std::uint64_t size, std::uint32_t* state);
inline constexpr char kSetCapturePixelsDeferredExportV1[] = "War3MapEditor_SetCapturePixelsDeferredV1";
inline constexpr char kCollectDeferredCapturePixelsExportV1[] = "War3MapEditor_CollectDeferredCapturePixelsV1";

} // namespace War3Map::Render
