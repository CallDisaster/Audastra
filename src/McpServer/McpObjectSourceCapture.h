#pragma once

#include "McpWorkspaceObjectsState.h"

#include "War3MapCore/Services/ObjectCatalogSources.h"
#include "War3MapCore/Services/ObjectCommands.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <vector>

namespace War3Map::Mcp {

// 物编来源的成员清单与读取器。展开目录与只读归档共用同一套七类物编捕获，
// 宿主只负责列出成员和按名字读取，不在这里判断来源类型。
struct ObjectMemberSource {
    std::vector<std::filesystem::path> files; // 来源内全部成员的相对路径
    std::function<Result<std::vector<std::byte>>(const std::filesystem::path&, std::size_t maximumBytes)> read;
};

// 读取有效目录、游戏字段元数据、七类地图物编与主脚本提示，建立来源快照。
// sources 提供有效目录的来源（目录或归档）与游戏根；map 是同一来源的地图文档。
[[nodiscard]] Result<std::unique_ptr<McpObjectWorkspaceState>> CaptureObjectWorkspaceState(
    const Services::ObjectCatalogSources& sources, const ObjectMemberSource& members, MapDocument map);

// 以快照里已提交的七类文档组装物编命令上下文；宿主有活动事务时再替换 documents/nameMap。
[[nodiscard]] Result<Services::ObjectCommandContext> CommittedObjectContext(
    const McpObjectWorkspaceState& state, ObjectData::ObjectType type);

} // namespace War3Map::Mcp
