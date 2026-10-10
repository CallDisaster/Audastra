#pragma once

#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/Script/ScriptPipeline.h"
#include "War3MapCore/Script/SlkMapSourceSnapshot.h"

#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// YDWE Lua 模板（<? ... ?>，含 require 'slk' 物编生成）的生产执行适配器。
// 与 YDWE 保存时相同：整份预处理脚本编译为一个 chunk、只执行一次，SLK 会话读取本次保存的最终地图成员，
// 结束时 refresh 写出有变化的七类物编。执行在独立 lua.exe 进程中（YDWE 自带），受 Job 内存/超时/输出限制；
// 模板代码只在 LuaTemplateHost.lua 的受限环境内运行。任何失败都不返回部分结果。
namespace War3Map::Script {

struct LuaTemplateToolchain {
    std::filesystem::path ydweRoot;
    std::string dataId;           // w3x2lni core/config.ini [global] data
    std::string locale = "zh-CN"; // share/<locale>/mpq
};

// 核对 YDWE 目录结构与已适配的 template.lua / slk_lib.lua 版本（宿主依赖其内部结构，其他版本拒绝）。
[[nodiscard]] Result<LuaTemplateToolchain> ResolveLuaTemplateToolchain(const std::filesystem::path& ydweRoot);

struct LuaTemplateLimits {
    std::chrono::milliseconds timeout{300000};
    std::size_t jobMemoryBytes = 2ull * 1024 * 1024 * 1024;
    std::size_t resultBytes = 160ull * 1024 * 1024;
};

// script 尚未经过 vJass/JASS 检查；objectMembers 为 refresh 实际写出的七类 native 成员。
using LuaTemplateOutput = TemplateTransactionOutput;

// workDir 必须是本次保存独占的已存在目录；运行文件与请求/结果均写在其中并保留供诊断。
[[nodiscard]] Result<LuaTemplateOutput> RunLuaTemplateTransaction(std::string_view preprocessed,
    const SlkMapSourceSnapshot& sources, const LuaTemplateToolchain& toolchain,
    const std::filesystem::path& workDir, const LuaTemplateLimits& limits = {});

} // namespace War3Map::Script
