#pragma once

#include "War3MapCore/Script/ScriptPipeline.h"
#include "War3MapCore/Script/LuaMapScript.h"

namespace War3Map::Script {

// 经典 Lua 地图仍由原生 war3map.j 执行 config/main 和原版 BJ 初始化。
// 这里只接受显式 MapGameOnly / Post124 冻结基础库，并复用保存链的完整 JASS 检查器；
// 不运行预处理、代码注入、外部编译器或地图代码，也不改变普通 JASS 的 layout 门禁。
// 任何来源的 //# 检查开关均拒绝，避免库或入口自行放宽最终语法/类型检查。
// 成功时保留入口原字节及本次实际检查的基础库；失败时不返回可保存脚本。
[[nodiscard]] PipelineResult CheckClassicLuaEntryScript(std::string_view generatedJass,
    std::shared_ptr<const ScriptBaseInputs> bases, const Jass::JassCheckOptions& limits = {},
    const std::function<void(std::string_view)>& onStage = {});

// 从同一冻结 common.j 的真实语义索引核对 Lua 所需 native、常量、标量全局与句柄父链。
// 六个原创兼容函数以明确签名放行，并核对它们的 native/常量依赖；不把任意 BJ 当 native。
// plan 自己声明的对象全局必须与 generated 的来源范围精确一致；数组和 code 全局未实现。
[[nodiscard]] Status ValidateClassicLuaDependencies(const MapInitializationPlan& plan,
    const LuaMapScriptResult& generated, const LuaTargetProfile& target, const ScriptBaseInputs& bases,
    const Jass::JassCheckOptions& limits = {});

} // namespace War3Map::Script
