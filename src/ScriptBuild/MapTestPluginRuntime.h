#pragma once

#include "War3MapCore/Services/MapTestService.h"
#include "War3MapCore/Types.h"

#include <filesystem>
#include <string>
#include <vector>

namespace War3Map::Script {

// KKWE 等新版 YDWE 的运行库：bin/ydbase.dll 加 plugin/warcraft3 插件目录，并且有 config.cfg 或 bin 里没有 lua53.dll。
// 没有老 YDWE 的 lua53.dll / ydwar3.dll，不能按六件套装入。
[[nodiscard]] bool IsPluginConfigRuntime(const std::filesystem::path& root);

struct PluginConfigRuntimePlan {
    // 装入顺序：每个插件前面先放它要用、放在 bin 里的依赖（依赖的依赖更靠前）。
    std::vector<Services::MapTestRuntimeModule> modules;
    std::vector<std::string> plugins;          // 本次装入的插件（目录顺序）
    std::vector<std::string> missing;          // config.cfg 写了启用、plugin/warcraft3 里却没有的插件
    std::vector<std::string> unmatchedNatives; // 地图要的扩展接口，在所有模块文件里都没找到名字
};

// 只读：读 config.cfg 与各 DLL 的导入表、导出表，不装入、不执行。与原平台装载器相同：
// plugin/warcraft3 下的 DLL 按目录顺序全部装入，只跳过 config.cfg [Enable] 里写成 0 的；没有 config.cfg 时全装。
// 全部装入后再逐个调用 Initialize。yd_loader.dll 本身由 Audastra 的启动组件代替，不装入。
// 计划内的文件彼此缺函数（不同版本混放）时拒绝，不让游戏装到一半失败。
[[nodiscard]] Result<PluginConfigRuntimePlan> PlanPluginConfigRuntime(
    const std::filesystem::path& root, const std::vector<std::string>& natives);

} // namespace War3Map::Script
