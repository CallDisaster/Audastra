#pragma once

#include "War3MapCore/Services/MapSaveService.h"

namespace War3Map::Services::LuaMapSave {

struct SettingsInput {
    std::vector<std::byte> source;
    std::vector<std::byte> final;
};

// 只取得W3I；不读取导入载荷或脚本正文。保存准备与执行共用同一身份规则。
[[nodiscard]] Result<SettingsInput> ReadSettings(const MapSaveTask& task);
[[nodiscard]] Status VerifyTarget(const MapSaveTask& task);
// 成功脚本与明确语言选择一次合入任务，失败不修改原待保存成员。
[[nodiscard]] Status MergeCompletedScript(MapSaveTask& task,
    std::vector<MemberReplacement>& scriptMembers);

} // namespace War3Map::Services::LuaMapSave
