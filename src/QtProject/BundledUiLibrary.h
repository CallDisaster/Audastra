#pragma once

#include <QString>

#include <filesystem>
#include <string_view>

// 编辑器自带的触发器函数库包（AudastraUI）：随编辑器发布，作者确认后写进数据根 ui/<包名>，
// 与 ui/WarVK 这类包同样由函数库两层启用管理。不修改 config，不执行包内程序。
namespace War3Map::QtProject {

inline constexpr std::string_view kBundledUiLibrary = "AudastraUI";

enum class BundledUiLibraryState { Missing, Same, Different };

struct BundledUiLibraryCheck {
    BundledUiLibraryState state = BundledUiLibraryState::Missing;
    std::filesystem::path target;  // 数据根里的包目录
    QString error;                 // 数据根无效等，非空时 state 无意义
};

// uiDataRoot 可为含 ui/config 的安装根，也可为数据根本身（同 UiCatalog::ResolveUiRoot）。
[[nodiscard]] BundledUiLibraryCheck CheckBundledUiLibrary(const std::filesystem::path& uiDataRoot);
// 写入包内全部文件（逐个原子替换，包目录里的其他文件不动）；返回空字符串表示成功。
[[nodiscard]] QString InstallBundledUiLibrary(const std::filesystem::path& uiDataRoot);

} // namespace War3Map::QtProject
