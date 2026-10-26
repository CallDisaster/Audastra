#pragma once
#include <filesystem>

namespace War3Map {
// 安装根的唯一规则：便携包bin旁有发布清单时取父目录，否则取可执行文件目录。
[[nodiscard]] std::filesystem::path InstallationRootFromExecutableDirectory(const std::filesystem::path& directory);
[[nodiscard]] std::filesystem::path CurrentInstallationRoot();
} // namespace War3Map
