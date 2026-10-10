#pragma once

#include "War3MapCore/Render/TerrainInspection.h"

#include <QJsonObject>

namespace War3Map::Mcp {

class McpWorkspace;

inline constexpr std::size_t TerrainMcpMaxTiles = 256;
inline constexpr std::size_t TerrainMcpMaxVertices = 1024;
inline constexpr std::size_t TerrainMcpMaxJsonBytes = 1024 * 1024;

struct McpTerrainSnapshot {
    ProjectRevision revision;
    Render::TerrainInspectionBinding binding;
    MapData::TerrainHeader header;
    std::array<float, 3> minimum{}, maximum{};
    std::string memberPath;
};

// 只服务同一个已打开的目录 MCP 工作区；由单线程 stdio dispatcher 串行调用。
// 不打开 MPQ、不连接 Qt 编辑器，也不访问活动事务 overlay 或任何保存入口。
class McpTerrain {
public:
    McpTerrain(McpWorkspace& workspace, std::string documentId, std::uint64_t generation);
    [[nodiscard]] Result<McpTerrainSnapshot> Snapshot(const ProjectRevision& expectedRevision);
    [[nodiscard]] Result<Render::TerrainInspectionBlock> Query(
        const ProjectRevision& expectedRevision, const Render::TerrainInspectionRequest& request,
        std::size_t maxTiles = TerrainMcpMaxTiles,
        std::size_t maxVertices = TerrainMcpMaxVertices);

private:
    [[nodiscard]] Status CheckCurrent(const ProjectRevision& expectedRevision) const;
    [[nodiscard]] Status EnsureLoaded(const ProjectRevision& expectedRevision);
    [[nodiscard]] Render::TerrainInspectionBinding Binding(const ProjectRevision& revision) const;

    McpWorkspace& workspace_;
    std::string documentId_;
    std::uint64_t generation_;
    std::optional<MapData::MapTerrain> terrain_;
    std::string terrainSha256_, memberPath_;
    std::vector<Diagnostic> terrainDiagnostics_;
};

[[nodiscard]] QJsonObject TerrainBindingJson(const Render::TerrainInspectionBinding& binding);
[[nodiscard]] QJsonObject TerrainSnapshotJson(const McpTerrainSnapshot& snapshot);
[[nodiscard]] QJsonObject TerrainBlockJson(const Render::TerrainInspectionBlock& block);

} // namespace War3Map::Mcp
