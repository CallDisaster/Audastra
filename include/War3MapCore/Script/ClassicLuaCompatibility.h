#pragma once

#include "War3MapCore/Script/LuaTriggerUnit.h"

namespace War3Map::Script {

struct ClassicLuaCompatibilityResult {
    // 地图内模块返回 factory(common)，factory 返回独立兼容表，不修改 jass.common。
    std::string moduleText;
    std::vector<LuaApiSignature> providedApis;
    std::vector<std::string> requiredNativeApis, requiredConstants;
    std::vector<Diagnostic> diagnostics;
    [[nodiscard]] bool Succeeded() const noexcept;
};

// 纯T经典档案的最小兼容层；只根据可信目标核对依赖，不加载外部引擎。
// 玩家配置、环境声效与InitBlizzard由JASS入口按原阶段顺序执行；不伪装成native。
[[nodiscard]] ClassicLuaCompatibilityResult BuildClassicLuaCompatibility(const LuaTargetProfile& target);

} // namespace War3Map::Script
