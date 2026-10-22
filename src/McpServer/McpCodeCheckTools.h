#pragma once

#include "McpServer.h"
#include "War3MapCore/Services/CodeCheck.h"
#include "War3MapCore/Trigger/TriggerCodeEditing.h"

#include <functional>
#include <memory>

namespace War3Map::Mcp {

struct CodeCheckToolInput {
    bool block = false;
    Services::CodeLanguage language = Services::CodeLanguage::Jass;
    Trigger::CodeBlockTarget target;
    std::string text;
    std::string sourceName = "MCP/input.j";
    bool hasText = false;
    std::uint64_t generation = 0;
};
struct CodeCheckToolWork {
    std::string binding;
    std::string targetKey;
    std::function<Services::CodeCheckResult(std::stop_token)> run;
};
struct CodeCheckToolHost {
    // 宿主线程只做身份核对/冻结，不运行完整检查；回调只拥有不可变来源。
    std::function<Result<CodeCheckToolWork>(CodeCheckToolInput)> capture;
    std::function<std::string()> currentBinding;
};

class CodeCheckToolSession final {
public:
    CodeCheckToolSession();
    ~CodeCheckToolSession();
    CodeCheckToolSession(const CodeCheckToolSession&) = delete;
    CodeCheckToolSession& operator=(const CodeCheckToolSession&) = delete;
    struct Impl;
    [[nodiscard]] QJsonObject Invoke(const QJsonObject& arguments, const CodeCheckToolHost& host);
    // 其他检查工具（如触发器整图检查）共用同一后台队列：先取请求序号，宿主冻结工作项后入队；
    // status/cancel 由 Report 按同一结果格式分页返回。
    [[nodiscard]] Result<std::uint64_t> ReserveGeneration();
    [[nodiscard]] QJsonObject Enqueue(CodeCheckToolWork work, std::uint64_t generation);
    [[nodiscard]] QJsonObject Report(const QJsonObject& arguments, const std::string& currentBinding);
    // 请求已完成、失败、取消或不存在时为假。
    [[nodiscard]] bool Pending(const std::string& requestId) const;
private:
    std::shared_ptr<Impl> impl_;
};

[[nodiscard]] ToolDefinition CodeCheckToolDefinition();
void RegisterCodeCheckTool(McpServer& server);

} // namespace War3Map::Mcp
