#pragma once

#include "McpMapDataTools.h"

#include <functional>

namespace War3Map::QtProject { class EditorProject; }

namespace War3Map::QtApp {

// 借用唯一活动工程和原有身份/快照入口，不建立独立 MCP 工作区。
[[nodiscard]] QJsonObject InvokeLiveAuxiliaryTool(QtProject::EditorProject& project,
    const QString& name, const QJsonObject& arguments,
    const std::function<QJsonObject()>& identity,
    const std::function<Result<Mcp::MapDataToolContext>()>& context);

} // namespace War3Map::QtApp
