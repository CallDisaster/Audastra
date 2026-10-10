#pragma once

#include <filesystem>

// 只读标准物编目录并导出供 war3-mdx-scan --list 使用的虚拟模型路径。
int RunObjectModelList(const std::filesystem::path& gameRoot, const std::filesystem::path& outputList);
