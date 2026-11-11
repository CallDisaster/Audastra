#pragma once

#include "War3MapCore/Trigger/TriggerEditing.h"

#include <span>
#include <string>
#include <string_view>

namespace War3Map::Trigger::detail {

// 在不改动的代码里找“按旧名称使用这个触发器”的证据，只读、纯词法。
// 识别标识符、字符串（含 JASS 四字符整数）和注释（JASS `//`，Lua `--` 与 `--[[ ]]`）：
// - 名称使用：标识符或字符串里出现 gg_trg_<脚本名>、Trig_<脚本名>、InitTrig_<脚本名>，
//   或字符串里出现触发器名原文；ExecuteFunc 的固定文字参数按同一规则判断。
// - 动态按名查找：ExecuteFunc 的参数不是一个固定文字；## 拼接紧跟 gg_trg_/Trig_/InitTrig_；
//   Lua 的 _G[非固定文字]、_ENV、rawget。按标识符边界判断，udg_X_G[1] 之类不算。
// 先返回第一处名称使用，没有再返回第一处动态查找。
struct TriggerRenameEvidence {
    TriggerDocumentEditor::RenameEvidence kind = TriggerDocumentEditor::RenameEvidence::None;
    std::size_t line = 0;
    std::string excerpt;
};

[[nodiscard]] TriggerRenameEvidence FindTriggerRenameEvidence(std::string_view code,
    std::string_view oldName, std::span<const std::string> scriptNames);

} // namespace War3Map::Trigger::detail
