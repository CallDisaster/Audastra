#pragma once

#include "War3MapCore/MapData/TerrainPathingResources.h"

namespace War3Map::QtProject {
class EditorProject;

// GUI线程冻结一次；后台仅借用这些owned值，不借用EditorProject或可变Session。
struct TerrainWaterEditRequest {
    // 保留用户打开路径和来源快照路径；别名等价性在worker核对，GUI不做文件I/O。
    std::filesystem::path projectRoot, archivePath, sourcePath, gameRoot;
    std::string sourceSha256;
    // MapArchiveSession是不可变来源快照；const读取各自使用局部只读句柄。
    // 共享原快照，避免每笔再Open并完整解码整张地图，不借用可变MapDataEditSession。
    std::shared_ptr<const MapArchiveSession> archive;
    std::optional<WorkspaceSnapshot> directory;
    std::shared_ptr<const MapData::TerrainGridSnapshot> terrain;
    MapData::DoodadPlacements doodads;
    std::uint64_t sessionIdentity = 0, revision = 0, terrainSourceRevision = 0;
    std::uint64_t projectGeneration = 0, projectRevision = 0;
    bool enforceWaterLimits = true;
    bool doodadsCaptured = false;
    std::size_t maximumBytes = 64u * 1024u * 1024u;
};

struct TerrainWaterEditCapture {
    std::shared_ptr<const TerrainWaterEditRequest> request;
    MapData::TerrainPathingResources resources;
};

[[nodiscard]] Result<TerrainWaterEditRequest> CaptureTerrainWaterEditRequest(
    const EditorProject& project, bool enforceWaterLimits,
    std::size_t maximumBytes = 64u * 1024u * 1024u, bool captureDoodads = true);
// 只比较已捕获身份，不在鼠标事件里重新打开、读取或散列整张地图。
[[nodiscard]] bool TerrainWaterEditRequestCurrent(const EditorProject& project,
    const TerrainWaterEditRequest& request, bool enforceWaterLimits);
// 后台入口：归档和目录使用同一个Core规则捕获，不读取渲染缓存或注册表。
[[nodiscard]] Result<TerrainWaterEditCapture> LoadTerrainWaterEditResources(
    std::shared_ptr<const TerrainWaterEditRequest> request);
// 每笔松手时在后台复核实际map/game winner、缺失项与loose覆盖，再回GUI提交。
[[nodiscard]] Status VerifyTerrainWaterEditResources(const TerrainWaterEditCapture& capture);
} // namespace War3Map::QtProject
