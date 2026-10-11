#pragma once

#include "War3MapCore/Script/LuaTriggerUnit.h"
#include "War3MapCore/Lua/LuaSyntax.h"

namespace War3Map::Script {

enum class LuaTestReloadChangeKind : std::uint8_t { Functions, Recreate, Remove, Add };
struct LuaTestReloadChange { StableId triggerId = 0; LuaTestReloadChangeKind kind{}; };
struct LuaTestReloadPatch {
    std::string source;
    std::string baselineScriptSha256, nextScriptSha256;
    std::vector<LuaTestReloadChange> changes;
    std::vector<StableId> newGlobals;
};

// binding 必须来自本局启动快照与当前 owned 地图事实（物编、地形、资源、设置），不能用修订号代替。
// 本函数仅准备、编译文本；不执行 Lua，不提交地图，不前推基线。邮箱对应成功 ACK 后才能前推。
[[nodiscard]] Result<LuaTestReloadPatch> PrepareLuaTestReload(const LuaTriggerUnit& baseline,
    const LuaTriggerUnit& current, std::string_view baselineMapBinding, std::string_view currentMapBinding,
    const Lua::SyntaxOptions& syntax = {});

} // namespace War3Map::Script
