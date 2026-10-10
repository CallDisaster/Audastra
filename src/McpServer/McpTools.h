#pragma once

namespace War3Map::Mcp {
class McpServer;

/// 注册安全事务写工具及其只读发现工具。旧物编直写、自动删除与松散
/// export_candidate 永不注册。
void RegisterAllTools(McpServer& server);

} // namespace War3Map::Mcp
