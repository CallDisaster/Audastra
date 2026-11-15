#pragma once

#include "War3MapCore/Types.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

// 错误报告：作者出问题时导出一个 ZIP 发给开发者。包含经过描述、电脑配置、运行环境、版本、运行时长、
// 编辑器设置、诊断列表、本次与上次异常退出的运行日志和崩溃转储。导出前可预览 report.txt。
// 本机用户目录在文本里替换成 %USERPROFILE%；崩溃转储是二进制，原样放入。
namespace War3Map::Feedback {

struct ErrorReportSection {
    std::string title;
    std::vector<std::pair<std::string, std::string>> rows;
};

struct ErrorReportFile {
    std::string name;            // ZIP 里的路径，只用 ASCII（资源管理器按旧编码显示 ZIP 内文件名），如 logs/current.log
    std::filesystem::path path;  // 本机文件
    bool text = true;            // 文本文件替换用户目录；二进制原样
};

struct ErrorReportInput {
    std::string description;
    std::vector<ErrorReportSection> sections;
    std::vector<std::string> diagnostics;
    std::vector<ErrorReportFile> files;
};

// 每个文件最多读 16 MiB，整包压缩后最多 64 MiB。
inline constexpr std::uint64_t kErrorReportFileLimit = 16ull * 1024 * 1024;
inline constexpr std::uint64_t kErrorReportZipLimit = 64ull * 1024 * 1024;

// 把本机用户目录（USERPROFILE 指向的目录）换成 %USERPROFILE%；profile 为空时取当前用户目录。
[[nodiscard]] std::string MaskUserProfile(std::string text, const std::filesystem::path& profile = {});
// report.txt 的内容（不含日志正文），预览与导出共用。
[[nodiscard]] std::string RenderErrorReportText(const ErrorReportInput& input);
[[nodiscard]] Result<std::vector<std::byte>> BuildErrorReportZip(const ErrorReportInput& input);
// 写到 target（不存在或允许覆盖的普通文件）。
[[nodiscard]] Status WriteErrorReport(const std::filesystem::path& target, const ErrorReportInput& input);

} // namespace War3Map::Feedback
