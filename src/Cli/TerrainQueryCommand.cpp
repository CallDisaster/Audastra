#include "TerrainQueryCommand.h"

#include "War3MapCore/Hash.h"
#include "War3MapCore/MapData/MapTerrain.h"
#include "War3MapCore/Render/TerrainInspection.h"
#include "War3MapCore/Workspace.h"

#include <charconv>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <locale>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace {

using Binding = War3Map::Render::TerrainInspectionBinding;

struct OpenTerrain {
    War3Map::MpqWorkspace archive;
    War3Map::MapData::MapTerrain terrain;
    Binding binding;
};

void PrintDiagnostics(const std::vector<War3Map::Diagnostic>& diagnostics) {
    for (const auto& diagnostic : diagnostics) {
        std::cerr << diagnostic.subsystem << ": " << diagnostic.code << ": "
                  << diagnostic.message << '\n';
    }
}

std::optional<OpenTerrain> Open(const std::filesystem::path& path) {
    // 这里只按已知名字只读查询 W3E，不要求可编辑/可保存的完整成员清单。
    auto archive = War3Map::MpqWorkspace::OpenReadOnly(path);
    if (!archive.Succeeded() || !archive.value) {
        PrintDiagnostics(archive.diagnostics);
        return std::nullopt;
    }
    if (!archive.value->Archive().HasFile("war3map.w3e")) {
        std::cerr << "Terrain: TERRAIN_MEMBER_MISSING: war3map.w3e\n";
        return std::nullopt;
    }
    auto bytes = archive.value->ReadMember("war3map.w3e", War3Map::MapData::TerrainReadLimits{}.maxBytes);
    if (!bytes.Succeeded() || !bytes.value) {
        PrintDiagnostics(bytes.diagnostics);
        return std::nullopt;
    }
    auto terrainHash = War3Map::Sha256(*bytes.value);
    if (!terrainHash.Succeeded() || !terrainHash.value) {
        PrintDiagnostics(terrainHash.diagnostics);
        return std::nullopt;
    }
    auto terrain = War3Map::MapData::ParseMapTerrain(*bytes.value);
    if (!terrain.Succeeded() || !terrain.value) {
        PrintDiagnostics(terrain.diagnostics);
        return std::nullopt;
    }
    const auto sourceHash = archive.value->SourceSha256();
    Binding binding{"archive-readonly:" + sourceHash, 1, 0, sourceHash, *terrainHash.value};
    return OpenTerrain{std::move(*archive.value), std::move(*terrain.value), std::move(binding)};
}

bool SourceStillCurrent(const OpenTerrain& opened) {
    const auto status = opened.archive.VerifyUnchanged();
    if (status.Succeeded()) return true;
    PrintDiagnostics(status.diagnostics);
    return false;
}

std::optional<std::uint32_t> ParseCoordinate(std::string_view input) {
    std::uint32_t value = 0;
    const auto [end, error] = std::from_chars(input.data(), input.data() + input.size(), value);
    if (input.empty() || error != std::errc{} || end != input.data() + input.size()) {
        return std::nullopt;
    }
    return value;
}

void WriteBinding(std::ostream& out, const Binding& binding) {
    out << "{\"document_id\":\"" << binding.documentId
        << "\",\"document_generation\":\"" << binding.documentGeneration
        << "\",\"revision\":\"" << binding.revision
        << "\",\"source_sha256\":\"" << binding.sourceSha256
        << "\",\"terrain_sha256\":\"" << binding.terrainSha256 << "\"}";
}

void WriteVector(std::ostream& out, const auto& values) {
    out << '[';
    bool first = true;
    for (const auto value : values) {
        if (!first) out << ',';
        out << value;
        first = false;
    }
    out << ']';
}

void WriteRawcodes(std::ostream& out, const std::vector<std::string>& ids) {
    constexpr char digits[] = "0123456789abcdef";
    out << '[';
    for (std::size_t index = 0; index < ids.size(); ++index) {
        if (index) out << ',';
        out << '"';
        for (unsigned char byte : ids[index]) {
            out << digits[byte >> 4] << digits[byte & 15];
        }
        out << '"';
    }
    out << ']';
}

void WriteHeader(std::ostream& out, const War3Map::MapData::TerrainHeader& header) {
    out << "{\"version\":" << header.version
        << ",\"vertex_record_bytes\":" << header.VertexRecordBytes()
        << ",\"main_tileset_byte\":" << static_cast<unsigned>(header.mainTileset)
        << ",\"uses_custom_tileset\":" << (header.usesCustomTileset ? "true" : "false")
        << ",\"vertex_width\":" << header.width << ",\"vertex_height\":" << header.height
        << ",\"tile_width\":" << header.width - 1 << ",\"tile_height\":" << header.height - 1
        << ",\"tile_size_world\":128,\"offset_world_xy\":";
    WriteVector(out, header.offset);
    out << ",\"ground_rawcodes_hex\":";
    WriteRawcodes(out, header.groundTilesets);
    out << ",\"cliff_rawcodes_hex\":";
    WriteRawcodes(out, header.cliffTilesets);
    out << '}';
}

std::ostringstream NewOutput() {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setprecision(9) << std::boolalpha;
    return out;
}

bool EmitBounded(const OpenTerrain& opened, const std::ostringstream& out) {
    // 同目录 MCP 采用相同的 1 MiB JSON 门禁。完整结果要么输出，要么拒绝。
    if (out.view().size() > 1024 * 1024) {
        std::cerr << "Terrain: TERRAIN_JSON_BUDGET: complete result exceeds 1 MiB\n";
        return false;
    }
    if (!SourceStillCurrent(opened)) return false;
    std::cout << out.view() << '\n';
    return true;
}

void WriteCommon(std::ostream& out, const Binding& binding) {
    out << "{\"ok\":true,\"schema_version\":1,\"workspace_kind\":\"map-archive-readonly\""
        << ",\"content_scope\":\"committed-source\",\"coordinate_system\":"
        << "\"southwest-origin-x-east-y-north-z-up\",\"surface_semantics\":"
        << "\"w3e-raw-layer-height-grid\",\"preview_diagonal\":\"northwest-southeast\""
        << ",\"is_render_capture\":false"
        << ",\"includes_cliff_ramp_geometry\":false,\"includes_water_material_offset\":false"
        << ",\"includes_entities\":false,\"includes_pathing\":false,\"terrain_binding\":";
    WriteBinding(out, binding);
}

} // namespace

int RunTerrainSnapshot(const std::filesystem::path& archivePath) {
    auto opened = Open(archivePath);
    if (!opened) return 1;
    const auto grid = opened->terrain.Snapshot();
    auto out = NewOutput();
    WriteCommon(out, opened->binding);
    out << ",\"header\":";
    WriteHeader(out, grid->header);
    out << ",\"ground_minimum\":";
    WriteVector(out, grid->minimum);
    out << ",\"ground_maximum\":";
    WriteVector(out, grid->maximum);
    out << ",\"query_limits\":{\"max_tiles\":256,\"max_vertices\":1024,\"max_json_bytes\":1048576}}";
    return EmitBounded(*opened, out) ? 0 : 1;
}

int RunTerrainBlock(const std::filesystem::path& archivePath,
    std::string_view expectedSourceSha256, std::string_view expectedTerrainSha256,
    std::string_view x, std::string_view y, std::string_view width, std::string_view height) {
    const auto tileX = ParseCoordinate(x), tileY = ParseCoordinate(y);
    const auto tileWidth = ParseCoordinate(width), tileHeight = ParseCoordinate(height);
    if (!tileX || !tileY || !tileWidth || !tileHeight) {
        std::cerr << "Terrain: TERRAIN_RECTANGLE_INVALID: expected four unsigned decimal integers\n";
        return 2;
    }
    auto opened = Open(archivePath);
    if (!opened) return 1;
    auto expected = opened->binding;
    expected.sourceSha256 = expectedSourceSha256;
    expected.terrainSha256 = expectedTerrainSha256;
    const War3Map::Render::TerrainInspectionRequest request{expected,
        {*tileX, *tileY, *tileWidth, *tileHeight}};
    auto queried = War3Map::Render::QueryTerrainBlock(opened->terrain, opened->binding, request,
        {256, 1024, 2 * 1024 * 1024});
    if (!queried.Succeeded() || !queried.value) {
        PrintDiagnostics(queried.diagnostics);
        return 1;
    }
    const auto& block = *queried.value;
    auto out = NewOutput();
    WriteCommon(out, block.binding);
    out << ",\"header\":";
    WriteHeader(out, block.sourceHeader);
    out << ",\"tiles\":{\"x\":" << block.tiles.x << ",\"y\":" << block.tiles.y
        << ",\"width\":" << block.tiles.width << ",\"height\":" << block.tiles.height << '}';
    out << ",\"ground_minimum\":";
    WriteVector(out, block.minimum);
    out << ",\"ground_maximum\":";
    WriteVector(out, block.maximum);
    out << ",\"unknown_water_flag_vertices\":" << block.unknownWaterFlagVertices
        << ",\"unknown_texture_flag_vertices\":" << block.unknownTextureFlagVertices
        << ",\"unresolved_ground_texture_vertices\":" << block.unresolvedGroundTextureVertices
        << ",\"unresolved_cliff_texture_vertices\":" << block.unresolvedCliffTextureVertices
        << ",\"core_logical_output_bytes\":" << block.outputBytes << ",\"vertices\":[";
    for (std::size_t index = 0; index < block.vertices.size(); ++index) {
        if (index) out << ',';
        const auto& vertex = block.vertices[index];
        const auto& sample = vertex.sample;
        out << "{\"x\":" << vertex.x << ",\"y\":" << vertex.y
            << ",\"source_index\":" << vertex.sourceIndex << ",\"position_world\":";
        WriteVector(out, sample.position);
        out << ",\"water_base_height\":" << sample.waterBaseHeight
            << ",\"ground_height_raw\":" << sample.groundHeightRaw
            << ",\"water_height_and_flags_raw\":" << sample.waterHeightAndFlagsRaw
            << ",\"texture_and_flags_raw\":" << static_cast<unsigned>(sample.textureAndFlagsRaw)
            << ",\"texture_flags_extension_raw\":" << static_cast<unsigned>(sample.textureFlagsExtensionRaw)
            << ",\"unknown_texture_flags\":" << static_cast<unsigned>(sample.UnknownTextureFlags())
            << ",\"variation_raw\":" << static_cast<unsigned>(sample.variationRaw)
            << ",\"cliff_and_layer_raw\":" << static_cast<unsigned>(sample.cliffAndLayerRaw)
            << ",\"ground_texture_index\":" << static_cast<unsigned>(sample.GroundTextureIndex())
            << ",\"ground_variation\":" << static_cast<unsigned>(sample.GroundVariation())
            << ",\"cliff_variation\":" << static_cast<unsigned>(sample.CliffVariation())
            << ",\"cliff_texture_index\":" << static_cast<unsigned>(sample.CliffTextureIndex())
            << ",\"cliff_level\":" << static_cast<unsigned>(sample.CliffLevel())
            << ",\"is_ramp\":" << sample.IsRamp() << ",\"is_blight\":" << sample.IsBlight()
            << ",\"has_water\":" << sample.HasWater() << ",\"is_boundary\":" << sample.IsBoundary()
            << ",\"is_map_edge\":" << sample.IsMapEdge()
            << ",\"unknown_water_flags\":" << sample.UnknownWaterFlags() << '}';
    }
    out << "],\"cells\":[";
    for (std::size_t index = 0; index < block.cells.size(); ++index) {
        if (index) out << ',';
        const auto& cell = block.cells[index];
        out << "{\"x\":" << cell.x << ",\"y\":" << cell.y
            << ",\"source_index\":" << cell.sourceIndex
            << ",\"source_vertices_sw_se_nw_ne\":";
        WriteVector(out, cell.source.vertices);
        out << ",\"local_vertices_sw_se_nw_ne\":";
        WriteVector(out, cell.localVertices);
        out << '}';
    }
    out << "]}";
    return EmitBounded(*opened, out) ? 0 : 1;
}
