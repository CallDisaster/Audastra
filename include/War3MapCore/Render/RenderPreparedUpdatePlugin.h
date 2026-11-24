#pragma once

#include "War3MapCore/RenderPluginV2.h"
#include "War3MapCore/RenderPluginEditorVisibility.h"

#include <cstdint>

namespace War3Map::Render {

// 编辑器渲染插件扩展：姿势/粒子更新的复制与校验可以提前在任意线程完成，界面线程随后提交
// 同一更新时凭票据直接采用，不必在界面线程里整份复制。
//
// 预备：input 只需填 modelPose 或 particleFrame 之一（显隐掩码与目标忽略）；成功返回 0 并写出非零票据。
// 提交：与 submitEditorVisibility 相同的输入与检查，多一个票据。票据只是加速——驻留布局、输入数组地址/数量
// 或场景/内容版本任一与预备时不同，插件照常完整复制；结果与直接提交相同。票据用过即失效，插件只保留最近两份。
// 描述结构本身可以是新建的副本，身份只看其中的数组地址、数量与版本。used 非空时写 1 表示用上了预备副本、0 表示照常复制。
// 调用方保证：同一 (sceneVersion, contentVersion) 的输入数组内容在提交前不再改变。
using War3PrepareEditorUpdateV1 = std::int32_t (WAR3_RENDER_CALL*)(War3RendererHandle handle,
    const War3RenderEditorVisibilityUpdateV2* input, std::uint64_t* ticket);
using War3SubmitPreparedEditorVisibilityV1 = std::int32_t (WAR3_RENDER_CALL*)(War3RendererHandle handle,
    const War3RenderEditorVisibilityUpdateV2* input, std::uint64_t ticket, std::uint32_t* used);
inline constexpr char kPrepareEditorUpdateExportV1[] = "War3MapEditor_PrepareEditorUpdateV1";
inline constexpr char kSubmitPreparedEditorVisibilityExportV1[] = "War3MapEditor_SubmitPreparedEditorVisibilityV1";

} // namespace War3Map::Render
