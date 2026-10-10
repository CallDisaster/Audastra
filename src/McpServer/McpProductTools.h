#pragma once

#include "McpServer.h"
#include "War3MapCore/Feedback/Queue.h"

namespace War3Map::Mcp {

struct ProductToolHost {
    std::function<Result<std::shared_ptr<Feedback::FeedbackQueue>>()> feedback;
    std::function<std::int64_t()> nowUnixSeconds;
    // 可信宿主提供的只读更新状态；协议不能启动安装或修改设置。
    std::function<QJsonObject()> updateStatus;
};

[[nodiscard]] std::vector<ToolDefinition> ProductToolDefinitions();
[[nodiscard]] QJsonObject InvokeProductTool(const QString& name,
    const QJsonObject& arguments, const ProductToolHost& host);
[[nodiscard]] Result<std::shared_ptr<Feedback::FeedbackQueue>> ApplicationFeedbackQueue();
[[nodiscard]] ProductToolHost DefaultProductToolHost();
// 可信宿主发布实际检查结果，协议只能读取；不触发网络或安装。
void PublishApplicationUpdateStatus(QJsonObject status);
void RegisterProductTools(McpServer& server, ProductToolHost host = DefaultProductToolHost());

} // namespace War3Map::Mcp
