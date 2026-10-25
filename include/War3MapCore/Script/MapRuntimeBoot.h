#pragma once
#include "War3MapCore/Types.h"
#include <string_view>
#include <functional>
#include <span>

namespace War3Map::Script {
struct MapRuntimeBootPlan {
    bool enabled = true;
    bool ownsExisting = false;
    bool onlyIfUsed = false;
    std::string payloadSha256;
    std::function<Result<std::string>()> resolvePayloadIdentity;
    std::vector<std::string> injectedSourceSha256;
    std::function<Status(std::span<const std::string>)> verifyScriptInputs;
};
struct MapRuntimeUsage {
    bool required = false;
    bool unresolvedDynamicCalls = false;
};
enum class MapRuntimeScriptLanguage { Jass, Lua };
// 仅处理已翻译的地图主脚本，复用JASS词法；不读取导入引导成员。
[[nodiscard]] Result<std::string> ApplyMapRuntimeBoot(std::string_view script, const MapRuntimeBootPlan& plan);
[[nodiscard]] Status VerifyMapRuntimeBoot(std::string_view script, bool enabled);
[[nodiscard]] Status VerifyMapRuntimeIdentity(std::string_view script, std::string_view payloadSha256);
[[nodiscard]] Result<bool> UsesMapRuntime(std::string_view script);
[[nodiscard]] Result<MapRuntimeUsage> AnalyzeMapRuntimeUsage(std::string_view script,
    MapRuntimeScriptLanguage language = MapRuntimeScriptLanguage::Jass);
} // namespace War3Map::Script
