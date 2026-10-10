#pragma once

#include "War3MapCore/ProjectStore.h"

#include <string_view>

namespace War3Map::QtProject {

// 仅自动恢复调用：成功发布后删除本次 Store 返回的、SHA 已确认的上一恢复副本。
// 手动工程保存的历史备份不调用此入口。失败保留文件，并返回警告。
[[nodiscard]] Status CleanupConfirmedRecoveryBackup(
    const ProjectSaveResult& written, std::string_view previousSha256);

} // namespace War3Map::QtProject
