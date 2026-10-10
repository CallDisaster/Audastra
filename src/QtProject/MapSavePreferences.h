#pragma once

#include "War3MapCore/MapArchiveSession.h"

class QSettings;
class QString;

namespace War3Map::QtProject {

// UI 与本机 Agent 保存共用同一份备份偏好；Core/独立 CLI 不读取 Qt 设置。
[[nodiscard]] Result<InPlaceSaveOptions> ReadMapInPlaceSaveOptions();
// 只读的显式来源入口；测试不修改作者设置或系统用户数据根。
[[nodiscard]] Result<InPlaceSaveOptions> ReadMapInPlaceSaveOptions(
    const QSettings& settings, const QString& genericDataRoot);

} // namespace War3Map::QtProject
