#pragma once

#include "War3MapCore/Feedback/ErrorReport.h"

#include <QString>

#include <filesystem>
#include <vector>

namespace War3Map::QtFeedback {

// 程序、系统、处理器、内存、显卡、显示器、磁盘。只读查询，不改系统设置。
[[nodiscard]] std::vector<Feedback::ErrorReportSection> CollectErrorReportEnvironment(const QString& buildStamp);
// 文件的版本号（如 war3.exe 的 1.27.0.52240）；没有版本信息时为空。
[[nodiscard]] QString FileVersion(const std::filesystem::path& path);
// 最近的崩溃转储（新的在前），最多 count 个。
[[nodiscard]] std::vector<std::filesystem::path> RecentCrashDumps(const std::filesystem::path& directory, std::size_t count);
// 崩溃时写转储和日志：未处理的结构化异常与 std::terminate。只装一次。
void InstallCrashCapture(const std::filesystem::path& directory);

} // namespace War3Map::QtFeedback
