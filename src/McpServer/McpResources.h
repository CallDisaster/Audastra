#pragma once

#include <filesystem>

namespace War3Map::Mcp {
class McpServer;

/// 注册所有 MCP 资源到服务器
// root 未指定时只使用当前便携程序旁的 skills，不扫描任意地图/父目录。
void RegisterAllResources(McpServer& server, const std::filesystem::path& skillRoot = {});
void RegisterSkillResources(McpServer& server, const std::filesystem::path& skillRoot);

} // namespace War3Map::Mcp
