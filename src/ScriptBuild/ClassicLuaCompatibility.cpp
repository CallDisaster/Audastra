#include "War3MapCore/Script/ClassicLuaCompatibility.h"

#include <algorithm>
#include <map>
#include <string_view>

namespace War3Map::Script {
namespace {
// 原创实现；只提供已核对的纯Lua辅助行为，不代替拥有JASS全局状态的BJ。
constexpr std::string_view kModule = R"lua(-- Audastra: bounded classic Lua helpers.
return function(common)
    local api = {}
    local raise, kind, byte = error, type, string.byte
    api.FourCC = function(text)
        if kind(text) ~= 'string' or #text ~= 4 then
            raise('FourCC: expected exactly four bytes')
        end
        local value = 0
        for index = 1, 4 do value = value * 256 + byte(text, index) end
        if value >= 2147483648 then value = value - 4294967296 end
        return value
    end
    api.ConditionalTriggerExecute = function(trigger)
        if common.TriggerEvaluate(trigger) then common.TriggerExecute(trigger) end
    end
    local function alliance(flags)
        return function(source, other, enabled)
            for i = 1, #flags do
                common.SetPlayerAlliance(source, other, flags[i], enabled)
            end
        end
    end
    api.SetPlayerAllianceStateAllyBJ = alliance({
        common.ALLIANCE_PASSIVE, common.ALLIANCE_HELP_REQUEST,
        common.ALLIANCE_HELP_RESPONSE, common.ALLIANCE_SHARED_XP,
        common.ALLIANCE_SHARED_SPELLS
    })
    api.SetPlayerAllianceStateVisionBJ = alliance({common.ALLIANCE_SHARED_VISION})
    api.SetPlayerAllianceStateControlBJ = alliance({common.ALLIANCE_SHARED_CONTROL})
    api.SetPlayerAllianceStateFullControlBJ = alliance({common.ALLIANCE_SHARED_ADVANCED_CONTROL})
    return api
end
)lua";

const std::vector<LuaApiSignature> kNatives{
    {"TriggerEvaluate", "boolean", {"trigger"}}, {"TriggerExecute", "nothing", {"trigger"}},
    {"SetPlayerAlliance", "nothing", {"player", "player", "alliancetype", "boolean"}}
};
const std::vector<LuaApiSignature> kProvided{
    {"FourCC", "integer", {"string"}}, {"ConditionalTriggerExecute", "nothing", {"trigger"}},
    {"SetPlayerAllianceStateAllyBJ", "nothing", {"player", "player", "boolean"}},
    {"SetPlayerAllianceStateVisionBJ", "nothing", {"player", "player", "boolean"}},
    {"SetPlayerAllianceStateControlBJ", "nothing", {"player", "player", "boolean"}},
    {"SetPlayerAllianceStateFullControlBJ", "nothing", {"player", "player", "boolean"}}
};
const std::vector<LuaEngineConstant> kConstants{
    {"ALLIANCE_PASSIVE", "alliancetype"}, {"ALLIANCE_HELP_REQUEST", "alliancetype"},
    {"ALLIANCE_HELP_RESPONSE", "alliancetype"}, {"ALLIANCE_SHARED_XP", "alliancetype"},
    {"ALLIANCE_SHARED_SPELLS", "alliancetype"}, {"ALLIANCE_SHARED_VISION", "alliancetype"},
    {"ALLIANCE_SHARED_CONTROL", "alliancetype"}, {"ALLIANCE_SHARED_ADVANCED_CONTROL", "alliancetype"}
};
bool Same(const LuaApiSignature& first, const LuaApiSignature& second) {
    return first.name == second.name && first.returns == second.returns && first.parameters == second.parameters;
}
}

bool ClassicLuaCompatibilityResult::Succeeded() const noexcept {
    return !moduleText.empty() && std::none_of(diagnostics.begin(), diagnostics.end(), [](const auto& value) {
        return value.severity == DiagnosticSeverity::Error;
    });
}
ClassicLuaCompatibilityResult BuildClassicLuaCompatibility(const LuaTargetProfile& target) {
    ClassicLuaCompatibilityResult result;
    const auto fail = [&](std::string code, std::string message) {
        result.diagnostics.push_back({DiagnosticSeverity::Error, "Script.ClassicLua", std::move(code), std::move(message), {}});
    };
    if (target.identity.empty() || target.identity.size() > 256 || target.apis.size() > 4096 || target.constants.size() > 4096) {
        fail("CLASSIC_LUA_COMPAT_TARGET", "经典Lua兼容层缺少明确目标或超出声明预算。"); return result;
    }
    std::map<std::string, const LuaApiSignature*, std::less<>> apis;
    std::map<std::string, std::string, std::less<>> constants;
    for (const auto& api : target.apis) if (!apis.emplace(api.name, &api).second)
        fail("CLASSIC_LUA_COMPAT_DUPLICATE", "目标API声明重复：" + api.name);
    for (const auto& constant : target.constants) if (!constants.emplace(constant.name, constant.type).second)
        fail("CLASSIC_LUA_COMPAT_DUPLICATE", "目标常量声明重复：" + constant.name);
    for (const auto& expected : kNatives) {
        const auto found = apis.find(expected.name);
        if (found == apis.end() || !Same(*found->second, expected))
            fail("CLASSIC_LUA_COMPAT_NATIVE", "经典初始化所需native缺失或签名不符：" + expected.name);
        result.requiredNativeApis.push_back(expected.name);
    }
    for (const auto& expected : kProvided) {
        const auto found = apis.find(expected.name);
        if (found != apis.end() && !Same(*found->second, expected))
            fail("CLASSIC_LUA_COMPAT_API", "经典初始化兼容API与目标声明冲突：" + expected.name);
    }
    for (const auto& expected : kConstants) {
        const auto found = constants.find(expected.name);
        if (found == constants.end() || found->second != expected.type)
            fail("CLASSIC_LUA_COMPAT_CONSTANT", "经典初始化所需常量缺失或类型不符：" + expected.name);
        result.requiredConstants.push_back(expected.name);
    }
    if (!result.diagnostics.empty()) return result;
    result.providedApis = kProvided;
    result.moduleText = kModule;
    return result;
}
} // namespace War3Map::Script
