#pragma once

#include "McpServer.h"
#include "War3MapCore/Services/ProjectSearch.h"

#include <functional>
#include <vector>

namespace War3Map::Mcp {

// identity 返回 project_identity 与覆盖所有当前来源的 context_revision。
// query 在文档所属线程内同步组装借用 context，并调用 SearchProjectPage。
// 返回值拥有所有字符串；不得把 context/span 捕获到异步任务或跨调用保存。
// 本宿主只有查询接口；没有 apply、保存、导航或打开文件的副作用。
struct ProjectSearchToolHost {
    std::function<QJsonObject()> identity;
    std::function<Result<Services::ProjectSearchPage>(std::string_view expectedContextRevision,
        std::string_view query, const Services::ProjectSearchOptions& options,
        std::size_t pageLimit, const Services::ProjectSearchCursor* cursor)> query;
};

[[nodiscard]] std::vector<ToolDefinition> ProjectSearchToolDefinitions();
[[nodiscard]] QJsonObject InvokeProjectSearchTool(const QString& name,
    const QJsonObject& arguments, const ProjectSearchToolHost& host);

} // namespace War3Map::Mcp
