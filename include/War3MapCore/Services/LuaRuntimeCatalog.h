#pragma once

#include "War3MapCore/Lua/LuaApiCatalog.h"
#include "War3MapCore/Lua/LuaSemantic.h"
#include "War3MapCore/Types.h"

#include <filesystem>
#include <memory>
#include <stop_token>
#include <string>
#include <vector>

namespace War3Map::Services {

// 编辑器自带 Lua 运行环境与测试地图使用同一份游戏 common.j / blizzard.j。
struct LuaRuntimeScripts {
    std::string common, blizzard;
    std::string source; // 来源说明
};

// 读取所选游戏目录里的 scripts\common.j 与 scripts\blizzard.j（测试地图运行时的接口来源）。
[[nodiscard]] Result<LuaRuntimeScripts> ReadGameLuaRuntimeScripts(const std::filesystem::path& gameRoot);

// 按 JASS 声明生成接口目录；mapGlobals 为地图变量的 JASS 名（udg_…）。
// scripts 为空时只含 Lua 标准库与 jass.runtime / jass.console，jass.common 成员未知。
[[nodiscard]] std::shared_ptr<const Lua::ApiCatalog> BuildLuaRuntimeCatalog(const LuaRuntimeScripts* scripts,
    const std::vector<std::pair<std::string, std::string>>& mapGlobals, std::stop_token cancel = {});

// 用目录填写字节码检查所需的运行库事实（模块清单与 native 参数个数）。
[[nodiscard]] Lua::SemanticRuntime OwnedRuntimeSemantics(const Lua::ApiCatalog& catalog);

} // namespace War3Map::Services
