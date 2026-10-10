#pragma once

#include "War3MapCore/Services/MapSaveService.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace War3Map::Services {

// 兼容旧 CLI/MCP 的单笔荒芜请求。坐标是源 W3E 顶点索引；事务由
// SaveMapEditCandidate 执行，源归档只读，目标必须是未存在的独立候选文件。
struct TerrainBlightSaveRequest {
    std::filesystem::path sourceMap;
    std::filesystem::path targetCandidate;
    std::filesystem::path toolRoot;
    std::filesystem::path gameRoot;
    std::string expectedSourceSha256;
    std::string expectedTerrainSha256;
    // 如界面使用本机覆盖层序，调用方必须提供同一显式序列；省略仅允许 config 默认层序。
    std::optional<std::vector<std::string>> effectiveLayers;
    std::uint32_t vertexX = 0;
    std::uint32_t vertexY = 0;
    std::uint32_t radius = 0;
    bool setBlight = true;
};

struct TerrainBlightSaveReceipt {
    std::filesystem::path candidate;
    std::string sourceSha256;
    std::string candidateSha256;
    std::string originalTerrainSha256;
    std::string savedTerrainSha256;
    std::string savedPathingSha256;
    std::size_t memberCount = 0;
    double scriptSeconds = 0;
    std::vector<std::string> effectiveLayers;
};

// 旧命令薄包装；只组装一笔圆形荒芜，不另建脚本、通行性或地图保存路径。
// 若写出后回读/会话建立失败，诊断会明确指出可能已存在已验证候选，调用方不切基线。
[[nodiscard]] Result<TerrainBlightSaveReceipt> SaveTerrainBlightCandidate(
    const TerrainBlightSaveRequest& request, const MapSaveService::Progress& progress = {});

} // namespace War3Map::Services
