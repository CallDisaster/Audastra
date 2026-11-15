#pragma once

#include "War3MapCore/Services/MapTestService.h"
#include "War3MapCore/Types.h"

#include <filesystem>
#include <string>
#include <vector>

namespace War3Map::Script {

// KKWE 等新版 YDWE 的运行库：bin/ydbase.dll 加 plugin/warcraft3/config.cfg（[Enable] 段列出要装的插件）。
// 没有老 YDWE 的 lua53.dll / ydwar3.dll，不能按六件套装入。
[[nodiscard]] bool IsPluginConfigRuntime(const std::filesystem::path& root);

struct PluginConfigRuntimePlan {
    // 装入顺序：每个插件前面先放它要用、放在 bin 里的依赖（依赖的依赖更靠前）。
    std::vector<Services::MapTestRuntimeModule> modules;
    std::vector<std::string> plugins;          // 本次装入的插件（config.cfg 顺序）
    std::vector<std::string> missing;          // config.cfg 启用了、plugin/warcraft3 里却没有的插件
    std::vector<std::string> unmatchedNatives; // 地图要的扩展接口，在所有模块文件里都没找到名字
};

// 只读：读 config.cfg 与各 DLL 的导入表、导出表，不装入、不执行。与原平台装载器相同，
// 启用的插件逐个装入并调用 Initialize；yd_loader.dll 本身由 Audastra 的启动组件代替，不装入。
[[nodiscard]] Result<PluginConfigRuntimePlan> PlanPluginConfigRuntime(
    const std::filesystem::path& root, const std::vector<std::string>& natives);

} // namespace War3Map::Script
