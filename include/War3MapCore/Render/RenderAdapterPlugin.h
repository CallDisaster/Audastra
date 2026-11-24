#pragma once

#include "War3MapCore/RenderPluginV2.h"

#include <cstdint>

namespace War3Map::Render {

// 渲染插件导出：之后新建的渲染器在这块显卡上建设备；luid 为 0 表示跟随系统默认。
// 返回 0：已采用；1：本机没有这块卡，已退回系统默认。
using War3SetRenderAdapterV1 = std::int32_t (WAR3_RENDER_CALL*)(std::uint64_t luid);
inline constexpr char kSetRenderAdapterExportV1[] = "War3MapEditor_SetRenderAdapterV1";
// 选定的显卡在建设备时已不存在、退回系统默认时，插件在成功状态消息末尾附上这一句。
inline constexpr char kRenderAdapterFallbackMessage[] = "; requested adapter unavailable, system default used";

} // namespace War3Map::Render
