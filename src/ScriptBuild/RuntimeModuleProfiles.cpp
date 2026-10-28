#include "War3MapCore/Script/ScriptToolchainProfile.h"
#include "War3MapCore/Hash.h"
#include <algorithm>

namespace War3Map::Script {
#include "RuntimeModuleProfiles.generated.h"

Result<std::string> ScriptModuleLoaderIdentity(const ScriptProfileModuleLoader& loader) {
    const auto valid = [](std::string_view value, bool symbol) {
        if (value.empty() || value.size() > 96 || (value.front() >= '0' && value.front() <= '9')) return false;
        return std::all_of(value.begin(), value.end(), [symbol](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '_' || (!symbol && c == '-');
        });
    };
    if (!valid(loader.id, false) || !valid(loader.functionName, true) ||
        loader.availability != ScriptModuleLoaderAvailability::AfterMain)
        return Result<std::string>::Failure("Script.Profile", "SCRIPT_PROFILE_MODULE_LOADER_INVALID",
            "模块加载声明需要有界标识符、固定参数返回类型及主初始化完成后的可用阶段。");
    return Sha256Text("archive-module-v1\n" + loader.id + "\n" + loader.functionName +
        "\nstring,boolean->boolean\nafter-main\n" + (loader.anonymous ? "true" : "false"));
}
}
