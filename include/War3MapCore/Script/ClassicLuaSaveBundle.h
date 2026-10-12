#pragma once

#include "War3MapCore/Script/LuaMapScript.h"

namespace War3Map::Script {

struct ClassicLuaModule {
    std::string member, text, sha256;
};

struct ClassicLuaSaveBundle {
    std::string jassEntry, jassSha256;
    std::string generatedScriptSha256, sourceMapSha256;
    std::vector<ClassicLuaModule> modules;
    std::vector<Diagnostic> diagnostics;
    [[nodiscard]] bool Succeeded() const noexcept;
};

// 经典v25设置逐字保留。大厅配置复用JASS生产输出，Lua只承接明确分开的main阶段。
// 仅生成并检查Lua语法；JASS检查、归档事务与真实游戏签收仍由调用方分别完成。
[[nodiscard]] ClassicLuaSaveBundle BuildClassicLuaSaveBundle(const MapInitializationPlan& plan,
    const LuaMapScriptResult& generated, const LuaTargetProfile& target);

} // namespace War3Map::Script
