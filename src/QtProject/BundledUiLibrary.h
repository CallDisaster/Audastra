#pragma once

#include <QString>

#include <filesystem>
#include <string_view>

// 编辑器自带的触发器函数库包（AudastraUI）：随编辑器发布，作者确认后写进数据根 ui/<包名>，
// 与 ui/WarVK 这类包同样由函数库两层启用管理。不修改 config，不执行包内程序。
// 数据根在 YDWE/KKWE 安装目录里时，包内 jass 再写一份到安装根 jass/<包名>：它们保存地图时只从那里扫描注入代码。
namespace War3Map::QtProject {

inline constexpr std::string_view kBundledUiLibrary = "AudastraUI";

enum class BundledUiLibraryState { Missing, Same, Different };

struct BundledUiLibraryCheck {
    BundledUiLibraryState state = BundledUiLibraryState::Missing;
    std::filesystem::path target;           // 数据根里的包目录
    std::filesystem::path injectionTarget;  // 安装根 jass/<包名>；数据根不在带 jass 目录的安装根里时为空
    QString error;                          // 数据根无效等，非空时 state 无意义
};

// uiDataRoot 可为含 ui/config 的安装根，也可为数据根本身（同 UiCatalog::ResolveUiRoot）。
[[nodiscard]] BundledUiLibraryCheck CheckBundledUiLibrary(const std::filesystem::path& uiDataRoot);
// 写入包内全部文件（逐个原子替换，包目录里的其他文件不动）；返回空字符串表示成功。
[[nodiscard]] QString InstallBundledUiLibrary(const std::filesystem::path& uiDataRoot);

} // namespace War3Map::QtProject
