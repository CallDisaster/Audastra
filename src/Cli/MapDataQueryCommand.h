#pragma once

#include <filesystem>
#include <string_view>

// 仅查询本次打开的归档快照；generation=1、revision=0 只在单次命令作用域有效。
// sourceSha256 由调用者从先前快照取得，命令与本次真实归档 SHA 精确比对。
int RunMapDataQuery(const std::filesystem::path& archivePath,
    std::string_view expectedSourceSha256, std::string_view kind,
    std::string_view startIndex, std::string_view limit);

// 打印字段目录（类别、字段标识、中文名、类型、是否可写与写入约束）。
int RunMapFieldCatalog();
