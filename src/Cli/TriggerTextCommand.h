#pragma once
#include <filesystem>
#include <string_view>

// 紧凑 T 写法：给出触发器名时打印该触发器的导出文本；不给时对全部触发器做
// “导出 → 整体替换 → 再导出”往返检查并汇总（只在内存中进行，不写任何文件）。
// mapDirectory 是 archive-expand 展开的目录；gameRoot 提供标准单位/物品分类。
int RunTriggerText(const std::filesystem::path& uiRoot, const std::filesystem::path& mapDirectory,
    const std::filesystem::path& gameRoot, std::string_view triggerName = {});
