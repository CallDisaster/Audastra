#pragma once

#include "War3MapCore/Services/MapSaveService.h"
#include "War3MapCore/Script/MapScriptBuild.h"

#include <optional>
#include <span>
#include <vector>

namespace War3Map::Services::ScriptPreservation {

// 本次保存的改动不影响游戏脚本时返回源地图脚本原字节。判定方式：触发器与脚本成员未变、
// 用源地图状态重新生成的脚本文本与本次相同；运行了模板时再用源状态完整构建，最终脚本也须相同。
// 任何一项不能证明时返回空，由调用方照常写入重建结果；不产生错误。
[[nodiscard]] std::optional<std::vector<std::byte>> UnaffectedSourceScript(const MapSaveTask& task,
    const Script::MapScriptBuildRequest& request, const Script::MapScriptBuildResult& built,
    std::span<const MemberReplacement> prepared, std::string_view scriptMember);

// 比较源脚本和本次脚本的函数签名（返回类型与参数类型），把不同之处写入诊断。
// 沿用源脚本时为消息，写入重建脚本时为警告。
void ReportSignatureChanges(std::span<const std::byte> source, std::span<const std::byte> saved,
    bool preserved, std::vector<Diagnostic>& diagnostics);

} // namespace War3Map::Services::ScriptPreservation
