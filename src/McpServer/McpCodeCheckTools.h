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
private:
    std::shared_ptr<Impl> impl_;
};

[[nodiscard]] ToolDefinition CodeCheckToolDefinition();
void RegisterCodeCheckTool(McpServer& server);

} // namespace War3Map::Mcp
