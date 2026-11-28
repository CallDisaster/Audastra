#pragma once

#include "EditorProject.h"

#include <QJsonArray>
#include <QJsonObject>

#include <optional>
#include <vector>

namespace War3Map::QtApp {
// Agent 命令里的本机文件导入：调用时把 {disk_path, archive_path} 读入内存，之后磁盘文件再变也不影响本次导入和撤销。
// 单个文件与一次合计的上限取 ImportPayloadPolicy。imports_add 与材质工具共用。失败时返回 MCP 失败对象。
[[nodiscard]] std::optional<QJsonObject> ReadDiskImports(const QJsonArray& files,
    std::vector<QtProject::EditorProject::ImportPayload>& payloads);
} // namespace War3Map::QtApp
