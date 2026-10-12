#pragma once

#include "War3MapCore/Script/LuaTriggerUnit.h"
#include "War3MapCore/Script/MapInitializationPlan.h"
#include <string_view>

namespace War3Map::Script {

inline constexpr std::string_view kClassicLuaObjectsContinuation = "AudastraMainObjects";
inline constexpr std::string_view kClassicLuaMainContinuation = "AudastraMainAfterBlizzard";
struct LuaMapScriptOptions {
    // 经典JASS入口在环境、对象与T三段之间保留原版环境声效和InitBlizzard；默认v31不变。
    bool splitClassicMainForJass = false;
};

struct LuaMapObjectSpan {
    MapInitSymbol source;
    std::size_t byteOffset = 0, byteLength = 0;
};

struct LuaMapScriptResult {
    // 完整地图对象初始化和T单元；经典档案的config由JASS保存入口负责，失败不返回部分脚本。
    std::string text;
    std::string targetIdentity, targetSha256, scriptSha256, sourceMapSha256;
    std::string classicObjectsContinuation, classicMainContinuation;
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
    const LuaTargetProfile& target, std::span<const LuaCallbackBinding> callbacks = {},
    LuaTriggerBuildOptions options = {}, const LuaMapScriptOptions& mapOptions = {});

} // namespace War3Map::Script
