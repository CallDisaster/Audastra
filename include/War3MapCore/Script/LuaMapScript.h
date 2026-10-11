#pragma once

#include "War3MapCore/Script/LuaTriggerUnit.h"
#include "War3MapCore/Script/MapInitializationPlan.h"

namespace War3Map::Script {

struct LuaMapObjectSpan {
    MapInitSymbol source;
    std::size_t byteOffset = 0, byteLength = 0;
};

struct LuaMapScriptResult {
    // 完整config/main、地图对象初始化和T单元；失败不返回部分脚本。
    std::string text;
    std::string targetIdentity, targetSha256, scriptSha256, sourceMapSha256;
    LuaTriggerUnit triggers;
    std::vector<LuaSourceSpan> sourceSpans;
    std::vector<LuaMapObjectSpan> objectSpans;
    std::vector<std::string> requiredApis, requiredGlobals, requiredConstants;
    std::vector<Diagnostic> diagnostics;
    [[nodiscard]] bool Succeeded() const noexcept;
};

// 直接从owned地图事实和类型化T生成；不把生成JASS或WCT代码转译成Lua。
[[nodiscard]] LuaMapScriptResult GenerateLuaMapScript(const MapInitializationPlan& plan,
    const Trigger::TriggerDocument& document, const Trigger::UiCatalog& catalog,
    const LuaTargetProfile& target, std::span<const LuaCallbackBinding> callbacks = {}, LuaTriggerBuildOptions options = {});

} // namespace War3Map::Script
