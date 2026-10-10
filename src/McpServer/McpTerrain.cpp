#include "McpTerrain.h"
#include "McpWorkspace.h"

#include "War3MapCore/IO/FileBytes.h"
#include "War3MapCore/VerifiedFile.h"
#include "War3MapCore/Workspace.h"

#include <QJsonArray>

#include <algorithm>

namespace War3Map::Mcp {
namespace {

template <typename T>
Result<T> Failed(const Status& status) {
    Result<T> result;
    result.diagnostics = status.diagnostics;
    return result;
}

QJsonArray VectorJson(const auto& values) {
    QJsonArray result;
    for (const auto value : values) result.append(static_cast<double>(value));
    return result;
}

QJsonArray RawcodesJson(const std::vector<std::string>& values) {
    QJsonArray result;
    for (const auto& value : values) {
        // rawcode 不保证 UTF-8；hex 保留大小写、NUL 和所有原始字节。
        result.append(QString::fromLatin1(QByteArray(value.data(),
            static_cast<qsizetype>(value.size())).toHex()));
    }
    return result;
}

QJsonObject HeaderJson(const MapData::TerrainHeader& header) {
    return {{"version", header.version}, {"main_tileset_byte", header.mainTileset},
        {"vertex_record_bytes", static_cast<qint64>(header.VertexRecordBytes())},
        {"uses_custom_tileset", header.usesCustomTileset},
        {"vertex_width", static_cast<qint64>(header.width)},
        {"vertex_height", static_cast<qint64>(header.height)},
        {"tile_width", static_cast<qint64>(header.width - 1)},
        {"tile_height", static_cast<qint64>(header.height - 1)},
        {"offset_world_xy", VectorJson(header.offset)}, {"tile_size_world", 128},
        {"ground_rawcodes_hex", RawcodesJson(header.groundTilesets)},
        {"cliff_rawcodes_hex", RawcodesJson(header.cliffTilesets)}};
}

QJsonObject RectangleJson(const Render::TerrainTileRectangle& tiles) {
    return {{"x", static_cast<qint64>(tiles.x)}, {"y", static_cast<qint64>(tiles.y)},
        {"width", static_cast<qint64>(tiles.width)}, {"height", static_cast<qint64>(tiles.height)}};
}

QJsonObject CommonJson(const Render::TerrainInspectionBinding& binding) {
    return {{"ok", true}, {"schema_version", 1}, {"workspace_kind", "expanded-directory"},
        {"terrain_binding", TerrainBindingJson(binding)}, {"content_scope", "committed-source"},
        {"coordinate_system", "southwest-origin-x-east-y-north-z-up"},
        {"surface_semantics", "w3e-raw-layer-height-grid"},
        {"preview_diagonal", "northwest-southeast"},
        {"includes_cliff_ramp_geometry", false}, {"includes_water_material_offset", false},
        {"includes_entities", false}, {"is_render_capture", false}};
}

} // namespace

McpTerrain::McpTerrain(McpWorkspace& workspace, std::string documentId, std::uint64_t generation)
    : workspace_(workspace), documentId_(std::move(documentId)), generation_(generation) {}

Status McpTerrain::CheckCurrent(const ProjectRevision& expectedRevision) const {
    if (workspace_.Revision() != expectedRevision) {
        return Status::Failure("MCP", "MCP_REVISION_CONFLICT", "地形请求的 expected_revision 已过期。");
    }
    if (workspace_.HasActiveTransaction()) {
        return Status::Failure("MCP", "MCP_TERRAIN_TRANSACTION_ACTIVE",
            "当前存在活动编辑事务；请先完成导出或回滚，再读取已提交来源的地形。");
    }
    return workspace_.EnvironmentStatus();
}

Status McpTerrain::EnsureLoaded(const ProjectRevision& expectedRevision) {
    const auto before = CheckCurrent(expectedRevision);
    if (!before.Succeeded()) return before;
    if (terrain_) return Status::Ok();

    // EditorSession 没有公开原始成员读取入口。复用同一种目录清单服务，且必须与
    // 已打开 session 的 aggregate 身份精确相等，不能把新磁盘内容悄悄绑定到旧 revision。
    auto directory = DirectoryWorkspace::Open(workspace_.SourceRoot());
    if (!directory.Succeeded()) return {std::move(directory.diagnostics)};
    const auto& snapshot = directory.value->Snapshot();
    if (snapshot.aggregateSha256 != expectedRevision.sourceSha256) {
        return Status::Failure("MCP", "MCP_TERRAIN_SOURCE_CHANGED", "目录身份与当前会话来源不一致。");
    }
    const WorkspaceFile* selected = nullptr;
    for (const auto& file : snapshot.files) {
        auto name = IO::GenericPathUtf8(file.relativePath);
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) {
            return static_cast<char>(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
        });
        if (name != "war3map.w3e" && name != "map/war3map.w3e") continue;
        if (selected != nullptr) {
            return Status::Failure("MCP", "MCP_TERRAIN_AMBIGUOUS",
                "目录存在多个 root/map 地形成员，不能猜测有效来源。");
        }
        selected = &file;
    }
    if (selected == nullptr) {
        return Status::Failure("MCP", "MCP_TERRAIN_MISSING", "目录工作区没有 war3map.w3e 地形成员。");
    }
    auto bytes = ReadVerifiedFile(snapshot.root / selected->relativePath, selected->sha256,
        MapData::TerrainReadLimits{}.maxBytes);
    if (!bytes.Succeeded()) return {std::move(bytes.diagnostics)};
    auto parsed = MapData::ParseMapTerrain(*bytes.value);
    if (!parsed.Succeeded()) return {std::move(parsed.diagnostics)};
    const auto after = CheckCurrent(expectedRevision);
    if (!after.Succeeded()) return after;

    terrainSha256_ = selected->sha256;
    memberPath_ = IO::GenericPathUtf8(selected->relativePath);
    terrainDiagnostics_ = std::move(parsed.diagnostics);
    terrain_ = std::move(*parsed.value);
    return Status::Ok();
}

Render::TerrainInspectionBinding McpTerrain::Binding(const ProjectRevision& revision) const {
    return {documentId_, generation_, revision.generation, revision.sourceSha256, terrainSha256_};
}

Result<McpTerrainSnapshot> McpTerrain::Snapshot(const ProjectRevision& expectedRevision) {
    const auto loaded = EnsureLoaded(expectedRevision);
    if (!loaded.Succeeded()) return Failed<McpTerrainSnapshot>(loaded);
    const auto grid = terrain_->Snapshot();
    McpTerrainSnapshot snapshot{expectedRevision, Binding(expectedRevision), grid->header,
        grid->minimum, grid->maximum, memberPath_};
    const auto after = CheckCurrent(expectedRevision);
    if (!after.Succeeded()) return Failed<McpTerrainSnapshot>(after);
    auto result = Result<McpTerrainSnapshot>::Success(std::move(snapshot));
    result.diagnostics = terrainDiagnostics_;
    return result;
}

Result<Render::TerrainInspectionBlock> McpTerrain::Query(const ProjectRevision& expectedRevision,
    const Render::TerrainInspectionRequest& request, std::size_t maxTiles, std::size_t maxVertices) {
    if (maxTiles == 0 || maxTiles > TerrainMcpMaxTiles || maxVertices == 0 || maxVertices > TerrainMcpMaxVertices) {
        return Result<Render::TerrainInspectionBlock>::Failure("MCP", "MCP_TERRAIN_LIMITS_INVALID",
            "MCP 地形预算必须为正且只能收紧到最多 256 格、1024 顶点。");
    }
    const auto loaded = EnsureLoaded(expectedRevision);
    if (!loaded.Succeeded()) return Failed<Render::TerrainInspectionBlock>(loaded);
    auto result = Render::QueryTerrainBlock(*terrain_, Binding(expectedRevision), request,
        Render::TerrainInspectionLimits{maxTiles, maxVertices, 2 * 1024 * 1024});
    if (!result.Succeeded()) return result;
    const auto after = CheckCurrent(expectedRevision);
    if (!after.Succeeded()) return Failed<Render::TerrainInspectionBlock>(after);
    return result;
}

QJsonObject TerrainBindingJson(const Render::TerrainInspectionBinding& binding) {
    return {{"document_id", QString::fromStdString(binding.documentId)},
        {"document_generation", QString::number(binding.documentGeneration)},
        {"revision", QString::number(binding.revision)},
        {"source_sha256", QString::fromStdString(binding.sourceSha256)},
        {"terrain_sha256", QString::fromStdString(binding.terrainSha256)}};
}

QJsonObject TerrainSnapshotJson(const McpTerrainSnapshot& snapshot) {
    auto result = CommonJson(snapshot.binding);
    result["header"] = HeaderJson(snapshot.header);
    result["member_path"] = QString::fromStdString(snapshot.memberPath);
    result["ground_minimum"] = VectorJson(snapshot.minimum);
    result["ground_maximum"] = VectorJson(snapshot.maximum);
    result["query_limits"] = QJsonObject{
        {"max_tiles", static_cast<qint64>(TerrainMcpMaxTiles)},
        {"max_vertices", static_cast<qint64>(TerrainMcpMaxVertices)},
        {"max_json_bytes", static_cast<qint64>(TerrainMcpMaxJsonBytes)}};
    return result;
}

QJsonObject TerrainBlockJson(const Render::TerrainInspectionBlock& block) {
    auto result = CommonJson(block.binding);
    result["header"] = HeaderJson(block.sourceHeader);
    result["tiles"] = RectangleJson(block.tiles);
    result["ground_minimum"] = VectorJson(block.minimum);
    result["ground_maximum"] = VectorJson(block.maximum);
    result["unknown_water_flag_vertices"] = static_cast<qint64>(block.unknownWaterFlagVertices);
    result["unknown_texture_flag_vertices"] = static_cast<qint64>(block.unknownTextureFlagVertices);
    result["unresolved_ground_texture_vertices"] = static_cast<qint64>(block.unresolvedGroundTextureVertices);
    result["unresolved_cliff_texture_vertices"] = static_cast<qint64>(block.unresolvedCliffTextureVertices);
    result["core_logical_output_bytes"] = static_cast<qint64>(block.outputBytes);
    QJsonArray vertices, cells;
    for (const auto& vertex : block.vertices) {
        const auto& sample = vertex.sample;
        vertices.append(QJsonObject{
            {"x", static_cast<qint64>(vertex.x)}, {"y", static_cast<qint64>(vertex.y)},
            {"source_index", static_cast<qint64>(vertex.sourceIndex)},
            {"position_world", VectorJson(sample.position)}, {"water_base_height", sample.waterBaseHeight},
            {"ground_height_raw", sample.groundHeightRaw},
            {"water_height_and_flags_raw", sample.waterHeightAndFlagsRaw},
            {"texture_and_flags_raw", sample.textureAndFlagsRaw},
            {"texture_flags_extension_raw", sample.textureFlagsExtensionRaw},
            {"unknown_texture_flags", sample.UnknownTextureFlags()},
            {"variation_raw", sample.variationRaw}, {"cliff_and_layer_raw", sample.cliffAndLayerRaw},
            {"ground_texture_index", sample.GroundTextureIndex()},
            {"ground_variation", sample.GroundVariation()}, {"cliff_variation", sample.CliffVariation()},
            {"cliff_texture_index", sample.CliffTextureIndex()}, {"cliff_level", sample.CliffLevel()},
            {"is_ramp", sample.IsRamp()}, {"is_blight", sample.IsBlight()},
            {"has_water", sample.HasWater()}, {"is_boundary", sample.IsBoundary()},
            {"is_map_edge", sample.IsMapEdge()}, {"unknown_water_flags", sample.UnknownWaterFlags()}});
    }
    for (const auto& cell : block.cells) {
        cells.append(QJsonObject{{"x", static_cast<qint64>(cell.x)}, {"y", static_cast<qint64>(cell.y)},
            {"source_index", static_cast<qint64>(cell.sourceIndex)},
            {"source_vertices_sw_se_nw_ne", VectorJson(cell.source.vertices)},
            {"local_vertices_sw_se_nw_ne", VectorJson(cell.localVertices)}});
    }
    result["vertices"] = vertices;
    result["cells"] = cells;
    return result;
}

} // namespace War3Map::Mcp
