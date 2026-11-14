#pragma once

#include "War3MapCore/Trigger/TriggerEditing.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace War3Map::Trigger::detail {

// 在不改动的代码里找“按名称使用某个声明”的证据，只读、纯词法。
// 识别标识符、字符串（含 JASS 四字符整数）和注释（JASS `//`，Lua `--` 与 `--[[ ]]`）：
// - 名称使用：标识符等于完整符号（gg_trg_<脚本名>、InitTrig_<脚本名>、udg_<变量名>）或生成函数
//   Trig_<脚本名>_Actions/_Conditions/_FuncNNN；字符串里按标识符边界出现这些符号；
//   给了显示名时整个字符串等于显示名也算。ExecuteFunc 的固定文字参数按同一规则判断。
// - 动态按名查找：ExecuteFunc 的参数不是一个固定文字；## 拼接紧跟 gg_trg_/Trig_/InitTrig_；
//   Lua 的 _G[非固定文字]、_ENV、rawget。按标识符边界判断，udg_X_G[1] 之类不算。
struct TriggerRenameEvidence {
    TriggerDocumentEditor::RenameEvidence kind = TriggerDocumentEditor::RenameEvidence::None;
    std::size_t line = 0;
    std::string excerpt;
};

struct CodeSymbolTarget {
    std::vector<std::string> exact;     // 完整符号
    std::vector<std::string> generated; // 生成函数前缀 Trig_<脚本名>_
    std::string displayName;            // 非空时整串相等的字符串也算
};

// 触发器的代码符号：按写出字节和按 UTF-8 正文换算的脚本名都查。
[[nodiscard]] CodeSymbolTarget TriggerCodeSymbols(std::span<const std::string> scriptNames, std::string displayName = {});

// 改名用：先返回第一处名称使用，没有再返回第一处动态查找。
[[nodiscard]] TriggerRenameEvidence FindTriggerRenameEvidence(std::string_view code,
    std::string_view oldName, std::span<const std::string> scriptNames);

// 删除用：一次扫描多个目标，每个目标返回本段代码里第一处名称使用；动态查找不算引用。
[[nodiscard]] std::vector<TriggerRenameEvidence> FindSymbolUses(std::string_view code,
    std::span<const CodeSymbolTarget> targets);

} // namespace War3Map::Trigger::detail
