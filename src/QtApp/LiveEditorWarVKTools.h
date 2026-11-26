#pragma once

#include "McpServer.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include <functional>
#include <vector>

namespace War3Map::QtProject { class EditorProject; }

namespace War3Map::QtApp {

// WarVK 渲染设置的 Agent 命令；身份/修订核对与写锁由活动编辑器工具宿主统一处理，这里只做参数与结果转换。
struct LiveWarVKTool {
    QString name, description;
    Mcp::ToolPermission permission = Mcp::ToolPermission::Query;
    QJsonObject properties; // 不含 project_identity / expected_revision，由宿主补上。
    QJsonArray required;
    std::function<QJsonObject(QtProject::EditorProject&)> query;
    std::function<QJsonObject(QtProject::EditorProject&, const QJsonObject&)> handler;
};
[[nodiscard]] std::vector<LiveWarVKTool> LiveWarVKTools();

} // namespace War3Map::QtApp
