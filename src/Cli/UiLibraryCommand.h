#pragma once

#include <filesystem>

// wtgPath / wctPath 同时给出时统计本图用到的函数库包。
int RunUiLibraryInspect(const std::filesystem::path& toolRoot,
    const std::filesystem::path* wtgPath, const std::filesystem::path* wctPath);
