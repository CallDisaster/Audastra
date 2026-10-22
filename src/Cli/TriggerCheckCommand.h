#pragma once
#include <filesystem>
#include <string_view>

// 触发器整图检查：按保存链生成整图脚本并做同一 JASS 检查，报错落在 GUI 语句上时给出紧凑 T 行列。
// 给出 triggerName 与 textFile 时，先在内存中用该文件的紧凑 T 文本整体替换这个触发器再检查。
// 只读打开地图，不写任何文件。
int RunTriggerCheck(const std::filesystem::path& ydweRoot, const std::filesystem::path& mapPath,
    const std::filesystem::path& gameRoot, std::string_view triggerName = {}, const std::filesystem::path& textFile = {});
