#pragma once

#include "McpServer.h"
#include "War3MapCore/MapData/MapDataFieldEditing.h"
#include "War3MapCore/Services/MapDataQuery.h"

#include <functional>
#include <memory>
#include <vector>

namespace War3Map::Mcp {

struct MapDataToolContext {
    std::shared_ptr<const MapDocument> original;
    std::shared_ptr<const MapDocument> document;
    Services::MapDataQueryBinding binding;
    bool hasOpaqueArchiveMembers = false;
    bool includesActiveTransactionOverlay = false;
    bool includesUnsavedUiRevision = false;
    std::shared_ptr<const Services::MapDataQuerySnapshot> preparedSnapshot;
    // 显式 typed Capture 的拥有值回交，不由全局搜索发起准备。
    std::function<Status(std::shared_ptr<const Services::MapDataQuerySnapshot>)> acceptSnapshot;
};

[[nodiscard]] Result<Services::MapDataQuerySnapshot> CaptureMapDataToolSnapshot(
    const MapDataToolContext& context, const std::shared_ptr<const MapDocument>& document);

// 宿主在原 Undo/EditorSession 覆盖发布之前调用；changes 是原改动面板的完整内容。
using MapDataPrepublishValidator = std::function<Status(const MapDocument& before, const MapDocument& after,
    const MapData::MapDataEditSession::EditReceipt& receipt, const QJsonObject& changes)>;

// 身份与环境门禁由可信宿主负责。独立事务累计原来源差异最多 32 项；GUI 每条命令最多
// 32 项，保留作者已有的其他编辑和原完整保存。facade 不嵌套 Write/保存。
struct MapDataToolHost {
    std::function<QJsonObject()> identity;
    std::function<Result<MapDataToolContext>()> context;
    std::function<Result<MapData::MapDataEditSession::EditReceipt>(
        const MapData::PreparedMapDataFields&, const MapDataPrepublishValidator&)> apply;
};

[[nodiscard]] std::vector<ToolDefinition> MapDataToolDefinitions();
[[nodiscard]] QJsonObject InvokeMapDataTool(
    const QString& name, const QJsonObject& arguments, const MapDataToolHost& host);
void RegisterMapDataTools(McpServer& server);

} // namespace War3Map::Mcp
