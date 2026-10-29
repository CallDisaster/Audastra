#pragma once
#include "War3MapCore/Script/MapRuntimeBoot.h"

namespace War3Map::Script::Detail {
Result<std::optional<ScriptProfileModuleLoader>> SelectModuleLoader(std::string_view script,
    std::span<const ScriptProfileModuleLoader> loaders);
Result<std::string> RemoveModuleBoot(std::string_view script,
    const std::optional<ScriptProfileModuleLoader>& owned);
Result<std::string> AddModuleBoot(std::string_view script, const ScriptProfileModuleLoader& loader);
Result<std::string> BindModuleExpected(std::string_view script, bool expected);
Status VerifyModuleBoot(std::string_view script, const std::optional<ScriptProfileModuleLoader>& loader);
}
