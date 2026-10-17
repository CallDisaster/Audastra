#include "RealTerrainPairAudit.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/VerifiedFile.h"
#include "War3MapCore/Render/TerrainGroundMesh.h"
#include <DirectXMath.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <set>
#include <stdexcept>

namespace War3Map::Tests {
namespace {
using Json = nlohmann::json;
using namespace MapData;
using namespace Render;
constexpr std::size_t kJsonLimit = 1024 * 1024;
constexpr std::uint32_t kStripRows = 32;
void Check(bool value, const char* text) { if (!value) throw std::runtime_error(text); }
std::string Digest(std::span<const std::byte> bytes) {
    auto result = Sha256(bytes); Check(result.Succeeded(), "pair SHA failed"); return *result.value;
}
std::uint32_t UInt(const Json& value, std::uint32_t maximum = UINT32_MAX) {
    Check(value.is_number_integer(), "pair integer required");
    const auto number = value.get<std::int64_t>();
    Check(number >= 0 && static_cast<std::uint64_t>(number) <= maximum, "pair integer outside range");
    return static_cast<std::uint32_t>(number);
}
float Number(const Json& value) {
    Check(value.is_number(), "pair number required");
    const double number = value.get<double>();
    Check(std::isfinite(number) && std::abs(number) <= 10000000, "pair number outside range");
    return static_cast<float>(number);
}
std::array<float, 3> Triple(const Json& value) {
    Check(value.is_array() && value.size() == 3, "pair triple required");
    return {Number(value[0]), Number(value[1]), Number(value[2])};
}
bool IsSha(std::string_view value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
std::string Rawcode(const Json& value) {
    const auto hex = value.get<std::string>();
    Check(hex.size() == 8, "pair rawcode requires four bytes");
    std::string result;
    for (std::size_t i = 0; i < 8; i += 2) {
        const auto digit = [](char c) -> unsigned {
            if (c >= '0' && c <= '9') return static_cast<unsigned>(c - '0');
            if (c >= 'a' && c <= 'f') return static_cast<unsigned>(c - 'a' + 10);
            throw std::runtime_error("pair rawcode hex invalid");
        };
        result.push_back(static_cast<char>(digit(hex[i]) * 16 + digit(hex[i + 1])));
    }
    return result;
}
std::filesystem::path Path(const Json& value) {
    const auto path = std::filesystem::u8path(value.get<std::string>());
    Check(path.is_absolute(), "pair input paths must be absolute"); return path;
}
Json ReadJson(const std::filesystem::path& path, std::string_view sha) {
    Check(IsSha(sha), "pair expected SHA invalid");
    auto bytes = ReadVerifiedFile(path, sha, kJsonLimit);
    Check(bytes.Succeeded(), "pair verified JSON read failed");
    return Json::parse(reinterpret_cast<const char*>(bytes.value->data()),
        reinterpret_cast<const char*>(bytes.value->data() + bytes.value->size()));
}
Json ReadRef(const Json& reference) {
    return ReadJson(Path(reference.at("path")), reference.at("sha256").get<std::string>());
}
TerrainVertex Vertex(const Json& j, const TerrainHeader& original) {
    TerrainVertex v;
    v.groundHeightRaw = static_cast<std::uint16_t>(UInt(j.at("ground_height_raw"), 65535));
    v.waterHeightAndFlagsRaw = static_cast<std::uint16_t>(UInt(j.at("water_height_and_flags_raw"), 65535));
    v.textureAndFlagsRaw = static_cast<std::uint8_t>(UInt(j.at("texture_and_flags_raw"), 255));
    v.textureFlagsExtensionRaw = static_cast<std::uint8_t>(UInt(j.at("texture_flags_extension_raw"), 255));
    Check(v.textureFlagsExtensionRaw == 0, "classic pair cannot carry a v12 extension byte");
    v.variationRaw = static_cast<std::uint8_t>(UInt(j.at("variation_raw"), 255));
    v.cliffAndLayerRaw = static_cast<std::uint8_t>(UInt(j.at("cliff_and_layer_raw"), 255));
    v.position = Triple(j.at("position_world")); v.waterBaseHeight = Number(j.at("water_base_height"));
    const auto x = UInt(j.at("x"), original.width - 1), y = UInt(j.at("y"), original.height - 1);
    Check(UInt(j.at("source_index")) == y * original.width + x, "pair source vertex identity mismatch");
    Check(v.position[0] == original.offset[0] + static_cast<float>(x) * 128 &&
        v.position[1] == original.offset[1] + static_cast<float>(y) * 128, "pair absolute world position mismatch");
    Check((v.groundHeightRaw & 0x8000u) == 0 &&
        v.position[2] == (static_cast<float>(v.groundHeightRaw)-8192.0f)/4.0f +
            128.0f*(static_cast<float>(v.CliffLevel())-2.0f) &&
        v.waterBaseHeight == (static_cast<float>(v.waterHeightAndFlagsRaw & 0x3fffu)-8192.0f)/4.0f,
        "pair packed ground/water height mismatch");
    Check(UInt(j.at("ground_texture_index")) == v.GroundTextureIndex() &&
        UInt(j.at("ground_variation")) == v.GroundVariation() && UInt(j.at("cliff_variation")) == v.CliffVariation() &&
        UInt(j.at("cliff_texture_index")) == v.CliffTextureIndex() && UInt(j.at("cliff_level")) == v.CliffLevel(),
        "pair raw/derived echo mismatch");
    Check(j.at("is_ramp") == v.IsRamp() && j.at("is_blight") == v.IsBlight() &&
        j.at("has_water") == v.HasWater() && j.at("is_boundary") == v.IsBoundary() &&
        j.at("is_map_edge") == v.IsMapEdge() && UInt(j.at("unknown_texture_flags")) == v.UnknownTextureFlags() &&
        UInt(j.at("unknown_water_flags")) == v.UnknownWaterFlags(), "pair raw flag echo mismatch");
    Check(!v.IsRamp() && !v.IsBlight() && !v.HasWater() && !v.IsBoundary() && !v.IsMapEdge() &&
        !v.UnknownTextureFlags() && !v.UnknownWaterFlags(), "pair requires clean flat halo flags");
    return v;
}
struct Context {
    TerrainGridSnapshot terrain;
    TerrainHeader original;
    std::array<std::uint32_t, 2> tiles{5, 6};
    std::array<std::uint32_t, 2> materials{};
    std::uint32_t x = 0, y = 0;
    Json source;
};
Context ValidateContext(const Json& selected) {
    Context c; c.source = selected;
    Check(selected.at("axis") == "vertical-shared-edge" &&
        selected.at("preview_diagonal") == "northwest-southeast", "pair orientation unsupported");
    const auto& header = selected.at("header");
    Check(UInt(header.at("version")) == 11 && UInt(header.at("vertex_record_bytes")) == 7 &&
        Number(header.at("tile_size_world")) == 128, "pair requires classic original grid");
    c.original.width = UInt(header.at("vertex_width"), 2049);
    c.original.height = UInt(header.at("vertex_height"), 2049);
    Check(c.original.width >= 5 && c.original.height >= 4 &&
        UInt(header.at("tile_width")) == c.original.width - 1 &&
        UInt(header.at("tile_height")) == c.original.height - 1, "pair source dimensions invalid");
    const auto& offset = header.at("offset_world_xy"); Check(offset.size() == 2, "pair offset invalid");
    c.original.offset = {Number(offset[0]), Number(offset[1])};
    c.original.mainTileset = static_cast<std::uint8_t>(UInt(header.at("main_tileset_byte"), 255));
    c.original.usesCustomTileset = header.at("uses_custom_tileset").get<bool>();
    Check(header.at("ground_rawcodes_hex").size() == 16 && header.at("cliff_rawcodes_hex").size() == 2,
        "pair retains complete sixteen/two original palettes");
    for (const auto& id : header.at("ground_rawcodes_hex")) c.original.groundTilesets.push_back(Rawcode(id));
    for (const auto& id : header.at("cliff_rawcodes_hex")) c.original.cliffTilesets.push_back(Rawcode(id));
    const auto& cells = selected.at("cells"); Check(cells.size() == 2, "pair requires two original cells");
    c.x = UInt(cells[0].at("x"),c.original.width - 4); c.y = UInt(cells[0].at("y"),c.original.height - 3);
    Check(c.x > 0 && c.y > 0 && c.x + 3 < c.original.width && c.y + 2 < c.original.height &&
        UInt(cells[1].at("x")) == c.x + 1 && UInt(cells[1].at("y")) == c.y, "pair cells are not interior neighbors");
    Check(selected.at("vertices").size() == 6 && selected.at("halo_vertices").size() == 20 &&
        selected.at("materials").size() == 2, "pair exact six vertices and twenty halo required");
    c.materials = {UInt(selected.at("materials")[0], 15), UInt(selected.at("materials")[1], 15)};
    Check(c.materials[0] < c.materials[1], "pair material order invalid");
    c.terrain.header = c.original; c.terrain.header.width = 5; c.terrain.header.height = 4;
    c.terrain.header.offset = {c.original.offset[0] + static_cast<float>(c.x - 1) * 128,
        c.original.offset[1] + static_cast<float>(c.y - 1) * 128};
    c.terrain.vertices.resize(20); std::array<bool, 20> found{};
    for (const auto& j : selected.at("halo_vertices")) {
        const auto x = UInt(j.at("x")), y = UInt(j.at("y"));
        Check(x >= c.x - 1 && x <= c.x + 3 && y >= c.y - 1 && y <= c.y + 2, "pair halo coordinate outside exact window");
        const auto local = (y - (c.y - 1)) * 5 + x - (c.x - 1);
        Check(!found[local], "pair duplicate halo vertex"); found[local] = true;
        c.terrain.vertices[local] = Vertex(j, c.original);
    }
    Check(std::all_of(found.begin(), found.end(), [](bool v) { return v; }), "pair incomplete halo");
    const auto z = c.terrain.vertices[0].position[2]; const auto level = c.terrain.vertices[0].CliffLevel();
    for (const auto& v : c.terrain.vertices) {
        Check(v.position[2] == z && v.CliffLevel() == level, "pair halo is not flat at one cliff level");
        Check(v.GroundTextureIndex() == c.materials[0] || v.GroundTextureIndex() == c.materials[1],
            "pair halo introduces another material");
    }
    std::set<std::uint32_t> selectedVertices;
    for (const auto& j : selected.at("vertices")) {
        const auto x = UInt(j.at("x")), y = UInt(j.at("y"));
        Check(x >= c.x && x <= c.x + 2 && y >= c.y && y <= c.y + 1, "pair core vertex outside pair");
        Check(selectedVertices.insert(UInt(j.at("source_index"))).second, "pair duplicate core vertex");
        const auto v = Vertex(j, c.original);
        const auto& h = c.terrain.vertices[(y - c.y + 1) * 5 + x - c.x + 1];
        Check(v.groundHeightRaw == h.groundHeightRaw && v.waterHeightAndFlagsRaw == h.waterHeightAndFlagsRaw &&
            v.textureAndFlagsRaw == h.textureAndFlagsRaw && v.variationRaw == h.variationRaw &&
            v.cliffAndLayerRaw == h.cliffAndLayerRaw && v.textureFlagsExtensionRaw == h.textureFlagsExtensionRaw &&
            v.position == h.position && v.waterBaseHeight == h.waterBaseHeight, "pair core/halo raw mismatch");
    }
    for (std::uint32_t y = 0; y < 3; ++y) for (std::uint32_t x = 0; x < 4; ++x) {
        const auto sw = y * 5 + x; c.terrain.tiles.push_back({{sw, sw + 1, sw + 5, sw + 6}});
    }
    c.terrain.minimum = c.terrain.vertices.front().position;
    c.terrain.maximum = c.terrain.vertices.back().position;
    for (std::size_t side = 0; side < 2; ++side) {
        const auto x = c.x + static_cast<std::uint32_t>(side), sw = c.y * c.original.width + x;
        Check(UInt(cells[side].at("source_index")) == c.y * (c.original.width - 1) + x &&
            cells[side].at("source_vertices_sw_se_nw_ne") == Json::array({sw, sw + 1, sw + c.original.width, sw + c.original.width + 1}),
            "pair original cell/vertex identity mismatch");
    }
    const auto south = c.y * c.original.width + c.x + 1;
    Check(selected.at("shared_source_vertices") == Json::array({south, south + c.original.width}), "pair shared identity mismatch");
    const auto& a = c.terrain.vertices[7]; const auto& b = c.terrain.vertices[12];
    Check(a.GroundTextureIndex() != b.GroundTextureIndex(), "pair shared edge must transition");
    return c;
}

Json Fixture() {
    Json j; j["axis"] = "vertical-shared-edge"; j["preview_diagonal"] = "northwest-southeast";
    j["header"] = {{"version",11},{"vertex_record_bytes",7},{"main_tileset_byte",65},{"uses_custom_tileset",true},
        {"vertex_width",9},{"vertex_height",8},{"tile_width",8},{"tile_height",7},{"tile_size_world",128},
        {"offset_world_xy",Json::array({-128,-256})},{"ground_rawcodes_hex",Json::array()},{"cliff_rawcodes_hex",Json::array({"41424344","45464748"})}};
    for (unsigned i = 0; i < 16; ++i) j["header"]["ground_rawcodes_hex"].push_back("41424344");
    j["materials"] = Json::array({1,3}); j["halo_vertices"] = Json::array(); j["vertices"] = Json::array();
    for (unsigned y = 1; y <= 4; ++y) for (unsigned x = 1; x <= 5; ++x) {
        const unsigned ground = y >= 3 && x >= 3 ? 1u : 3u;
        Json v = {{"x",x},{"y",y},{"source_index",y*9+x},{"position_world",Json::array({-128+static_cast<int>(x)*128,-256+static_cast<int>(y)*128,0})},
            {"ground_height_raw",8192},{"water_height_and_flags_raw",8192},{"water_base_height",0},
            {"texture_and_flags_raw",ground},{"texture_flags_extension_raw",0},{"variation_raw",4},{"cliff_and_layer_raw",18},
            {"ground_texture_index",ground},{"ground_variation",4},{"cliff_variation",0},{"cliff_texture_index",1},{"cliff_level",2},
            {"is_ramp",false},{"is_blight",false},{"has_water",false},{"is_boundary",false},{"is_map_edge",false},
            {"unknown_texture_flags",0},{"unknown_water_flags",0}};
        j["halo_vertices"].push_back(v); if (x >= 2 && x <= 4 && y >= 2 && y <= 3) j["vertices"].push_back(v);
    }
    j["cells"] = Json::array({{{"x",2},{"y",2},{"source_index",18},{"source_vertices_sw_se_nw_ne",Json::array({20,21,29,30})}},
        {{"x",3},{"y",2},{"source_index",19},{"source_vertices_sw_se_nw_ne",Json::array({21,22,30,31})}}});
    j["shared_source_vertices"] = Json::array({21,30}); return j;
}
void WriteBytes(const std::filesystem::path& path, std::span<const std::uint8_t> bytes) {
    std::ofstream stream(path, std::ios::binary); Check(stream.good(), "pair output open failed");
    stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    stream.close(); Check(stream.good(), "pair output write failed");
}
Json FileReceipt(const std::filesystem::path& path) {
    auto sha = Sha256File(path); Check(sha.Succeeded(), "pair output SHA failed");
    return {{"path",path.generic_string()},{"bytes",std::filesystem::file_size(path)},{"sha256",*sha.value}};
}
} // namespace

void RunRealTerrainPairValidationTests() {
    const auto original = Fixture(); const auto good = ValidateContext(original);
    Check(good.terrain.vertices.size() == 20 && good.terrain.tiles.size() == 12, "pair original fixture failed");
    const auto reject = [](Json j) { bool rejected = false; try { (void)ValidateContext(j); } catch (const std::exception&) { rejected = true; }
        Check(rejected, "pair malformed original fixture accepted"); };
    auto j = original; j["halo_vertices"].erase(0); reject(j);
    j = original; j["halo_vertices"][1] = j["halo_vertices"][0]; reject(j);
    j = original; j["cells"][1]["x"] = 4; reject(j);
    j = original; j["cells"][0]["x"] = UINT32_MAX; reject(j);
    j = original; j["vertices"][0]["source_index"] = 1; reject(j);
    j = original; j["vertices"][0]["ground_variation"] = 9; reject(j);
    j = original; j["halo_vertices"][0]["position_world"][2] = 1; reject(j);
    j = original; j["halo_vertices"][0]["ground_height_raw"] = 8196; reject(j);
    j = original; j["halo_vertices"][0]["ground_height_raw"] = 40960; reject(j);
    j = original; j["halo_vertices"][0]["water_base_height"] = 1; reject(j);
    j = original; j["halo_vertices"][0]["position_world"][0] = 99; reject(j);
    j = original; j["halo_vertices"][0]["is_ramp"] = true; reject(j);
    j = original; j["header"]["ground_rawcodes_hex"].erase(0); reject(j);
    j = original; j["shared_source_vertices"][0] = 999; reject(j);
    Check(!IsSha("../invalid") && IsSha(std::string(64,'a')), "pair SHA fixture gate failed");
}

void RunRealTerrainPairAudit(const std::filesystem::path& manifestPath, std::string_view manifestSha,
    const std::filesystem::path& output, const RealTerrainPairCapture& capture, const RealTerrainPairSaveBmp& saveBmp) {
    RunRealTerrainPairValidationTests();
    const auto manifest = ReadJson(manifestPath, manifestSha);
    Check(manifest.at("schema") == "terrain-real-pair-audit-v1" &&
        manifest.at("environment") == "neutral-no-shadow-no-fog", "pair manifest contract unsupported");
    Check(output.is_absolute() && (!std::filesystem::exists(output) ||
        (std::filesystem::is_directory(output) && std::filesystem::is_empty(output))), "pair output must be new or empty absolute directory");
    const auto selected = ReadRef(manifest.at("selected_neighbors")); auto context = ValidateContext(selected);
    const auto sampling = ReadRef(manifest.at("sampling_manifest"));
    const auto& binding = manifest.at("terrain_binding");
    Check(binding == selected.at("terrain_binding") && binding.at("source_sha256") == sampling.at("map_source_sha256"),
        "pair source binding mismatch");
    Check(IsSha(binding.at("source_sha256").get<std::string>()) && IsSha(binding.at("terrain_sha256").get<std::string>()) &&
        binding.at("document_id") == "archive-readonly:" + binding.at("source_sha256").get<std::string>(), "pair source identity invalid");
    const auto revisionText = binding.at("revision").get<std::string>();
    Check(!revisionText.empty() && revisionText.size() <= 19 && std::all_of(revisionText.begin(), revisionText.end(),
        [](char ch) { return ch >= '0' && ch <= '9'; }), "pair revision invalid");
    const auto revision = std::stoull(revisionText);
    Check(sampling.at("schema") == "terrain-alpha-sampling-input-v1" && sampling.at("textures").size() <= 16,
        "pair sampling manifest unsupported");
    std::vector<std::shared_ptr<const TerrainAtlasSnapshot>> atlases(context.original.groundTilesets.size());
    Json resourceReceipts = Json::array();
    for (const auto index : context.materials) {
        const Json* audited = nullptr;
        for (const auto& item : selected.at("audited_materials")) if (UInt(item.at("ground_texture_index")) == index) {
            Check(!audited, "pair duplicate audited material"); audited = &item;
        }
        Check(audited && Rawcode(audited->at("rawcode_hex")) == context.original.groundTilesets[index], "pair palette material mismatch");
        const Json* texture = nullptr;
        for (const auto& item : sampling.at("textures")) if (item.at("id") == audited->at("atlas_id")) {
            Check(!texture, "pair duplicate sampling atlas"); texture = &item;
        }
        Check(texture && texture->at("virtual_path") == audited->at("virtual_path") &&
            texture->at("source_sha256") == audited->at("source_sha256"), "pair material source mismatch");
        const auto sourcePath = Path(texture->at("source_file"));
        auto source = ReadVerifiedFile(sourcePath, texture->at("source_sha256").get<std::string>(), 16 * 1024 * 1024);
        Check(source.Succeeded(), "pair original BLP verified read failed");
        Resources::ResolvedResource resource;
        resource.location = {texture->at("virtual_path").get<std::string>(), Resources::ResourceLayer::Other, 0,
            texture->at("id").get<std::string>(), sourcePath.generic_string()};
        resource.bytes = std::move(*source.value);
        auto decoded = DecodeTerrainTextureAtlas(resource); Check(decoded.Succeeded(), "pair production BLP decode failed");
        atlases[index] = *decoded.value; const auto& atlas = *atlases[index];
        Check(atlas.sourceSha256 == texture->at("source_sha256").get<std::string>() && atlas.columns == UInt(texture->at("columns")),
            "pair decoded atlas identity mismatch");
        const auto& levels = texture->at("levels"); Check(levels.size() == atlas.additionalMips.size() + 1 && levels.size() <= 12,
            "pair original mip count mismatch");
        Json verifiedLevels = Json::array();
        for (std::size_t i = 0; i < levels.size(); ++i) {
            const auto& mip = i ? atlas.additionalMips[i - 1] : atlas.image;
            const auto sha = Digest(mip.rgba);
            Check(UInt(levels[i].at("level")) == i && UInt(levels[i].at("width")) == mip.width &&
                UInt(levels[i].at("height")) == mip.height && levels[i].at("rgba_sha256") == sha, "pair decoded original mip SHA mismatch");
            verifiedLevels.push_back({{"level",i},{"width",mip.width},{"height",mip.height},{"rgba_sha256",sha}});
        }
        resourceReceipts.push_back({{"palette_index",index},{"source_sha256",atlas.sourceSha256},
            {"source_path",sourcePath.generic_string()},{"columns",atlas.columns},{"mips",verifiedLevels}});
    }
    TerrainLighting lighting; const auto& light = manifest.at("lighting");
    lighting.enabled = light.at("enabled").get<bool>(); lighting.towardLight = Triple(light.at("toward_light"));
    lighting.ambient = Triple(light.at("ambient")); lighting.diffuse = Triple(light.at("diffuse"));
    auto built = BuildTerrainGroundMesh(context.terrain, atlases, {}, lighting);
    Check(built.Succeeded(), "pair production mesh rejected"); const auto& mesh = *built.value;
    Check(mesh.fallback.totalTiles == 0 && mesh.texturedTiles == 12 && mesh.hiddenTileCount == 0 &&
        mesh.waterTiles.empty() && mesh.blightTexturedTiles == 0 && mesh.blightApproximationTiles == 0,
        "pair halo requires fully textured flat production mesh");
    std::vector<War3RenderTerrainVertexV2> vertices;
    for (const auto& v : mesh.vertices) vertices.push_back({v.x,v.y,v.z,v.red,v.green,v.blue,v.u,v.v});
    std::vector<std::uint32_t> indices; std::vector<War3RenderTerrainBatchV2> batches;
    Json quadReceipts = Json::array(); unsigned baseCount = 0, overlayCount = 0;
    for (const auto& batch : mesh.batches) {
        Check(!batch.translucent && batch.indexCount % 6 == 0 && batch.textureIndex < mesh.textures.size(), "pair unexpected production batch");
        const auto firstIndex = static_cast<std::uint32_t>(indices.size());
        for (std::uint32_t at = batch.firstIndex; at < batch.firstIndex + batch.indexCount; at += 6) {
            const auto first = mesh.indices.at(at); Check(first % 4 == 0, "pair production quad identity invalid");
            const auto quad = first / 4;
            const auto tile = quad < 12 ? quad : mesh.overlayTiles.at(quad - 12);
            if (tile != context.tiles[0] && tile != context.tiles[1]) continue;
            const auto& corners = context.terrain.tiles[tile].vertices;
            unsigned mask = 0; const auto material = batch.alphaBlend ? context.materials[1] : context.materials[0];
            for (unsigned i = 0; i < 4; ++i) if (context.terrain.vertices[corners[i]].GroundTextureIndex() == material)
                mask |= std::array<unsigned,4>{2,1,8,4}[i];
            Check(mesh.textures[batch.textureIndex] == atlases[material], "pair production layer material mismatch");
            const auto& atlas = *mesh.textures[batch.textureIndex];
            const auto& v = mesh.vertices.at(first);
            const auto column = static_cast<unsigned>(std::floor(v.u * static_cast<float>(atlas.columns)));
            const auto row = static_cast<unsigned>(std::floor(v.v * 4.0f));
            Check(column < atlas.columns && row < 4, "pair production UV cell invalid");
            if (batch.alphaBlend) Check(column == mask % 4 && row == mask / 4, "pair production mask/cell mismatch");
            else {
                // 这里只核结构合同，不参与任何像素采样；UV 本身仍完全取生产 mesh。
                const auto variation = context.terrain.vertices[corners[0]].GroundVariation();
                std::array<unsigned,2> expectedCell;
                if (atlas.columns == 4) expectedCell = variation == 0 ? std::array<unsigned,2>{0,0} : std::array<unsigned,2>{3,3};
                else if (variation < 16) expectedCell = {4u+variation%4u,variation/4u};
                else expectedCell = variation == 16 ? std::array<unsigned,2>{3,3} : std::array<unsigned,2>{0,0};
                Check(column == expectedCell[0] && row == expectedCell[1], "pair production base variation cell mismatch");
            }
            Json shades = Json::array(), uvs = Json::array();
            for (unsigned i = 0; i < 4; ++i) {
                const auto& original = context.terrain.vertices[corners[i]]; const auto& emitted = mesh.vertices.at(first+i);
                const auto shade = TerrainVertexShade(context.terrain, corners[i] % 5, corners[i] / 5, lighting);
                Check(std::array<float,3>{emitted.x,emitted.y,emitted.z} == original.position &&
                    std::array<float,3>{emitted.red,emitted.green,emitted.blue} == shade, "pair production geometry/shade mismatch");
                shades.push_back(shade);
                uvs.push_back(Json::array({emitted.u,emitted.v}));
            }
            const auto side = tile == context.tiles[0] ? 0u : 1u;
            quadReceipts.push_back({{"local_tile",tile},{"source_tile",selected.at("cells")[side].at("source_index")},
                {"source_vertices",selected.at("cells")[side].at("source_vertices_sw_se_nw_ne")},
                {"alpha_blend",batch.alphaBlend},{"palette_index",material},{"corner_membership_mask",mask},
                {"transition_mask",batch.alphaBlend ? Json(mask) : Json(nullptr)},
                {"cell",Json::array({column,row})},{"sw_variation_raw",context.terrain.vertices[corners[0]].variationRaw},
                {"sw_ground_variation",context.terrain.vertices[corners[0]].GroundVariation()},{"shade",shades},{"uv_sw_se_nw_ne",uvs}});
            const std::array<std::uint32_t,6> expected{first,first+1,first+2,first+1,first+3,first+2};
            Check(std::equal(expected.begin(), expected.end(), mesh.indices.begin()+at), "pair production diagonal changed");
            indices.insert(indices.end(), mesh.indices.begin()+at, mesh.indices.begin()+at+6);
            if (batch.alphaBlend) ++overlayCount; else ++baseCount;
        }
        if (indices.size() != firstIndex) batches.push_back({firstIndex,static_cast<std::uint32_t>(indices.size()-firstIndex),
            batch.textureIndex | (batch.alphaBlend ? WAR3_RENDER_TERRAIN_ALPHA_BLEND : 0u)});
    }
    Check(baseCount == 2 && overlayCount == 2 && indices.size() == 24 && batches.size() == 2,
        "pair requires both real base and transition quads");
    std::vector<std::vector<War3RenderMipRgbaV2>> mipLists(mesh.textures.size());
    std::vector<War3RenderTextureMipChainV2> chains;
    for (std::size_t i = 0; i < mesh.textures.size(); ++i) {
        const auto& texture = *mesh.textures[i]; auto& mips = mipLists[i];
        const auto append = [&](const Image::DecodedImage& mip) { mips.push_back({sizeof(War3RenderMipRgbaV2),
            mip.width,mip.height,mip.rgba.size(),mip.rgba.data()}); };
        append(texture.image); for (const auto& mip : texture.additionalMips) append(mip);
        std::uint32_t maximum = 0;
        for (std::size_t level = 1; level < mips.size(); ++level) {
            if (mips[level].width < texture.columns || mips[level].height < texture.rows ||
                mips[level].width % texture.columns || mips[level].height % texture.rows) break;
            maximum = static_cast<std::uint32_t>(level);
        }
        chains.push_back({sizeof(War3RenderTextureMipChainV2),static_cast<std::uint32_t>(mips.size()),maximum,
            texture.columns,texture.rows,mips.data()});
    }
    const auto& cameraJson = manifest.at("camera"); const auto target = Triple(cameraJson.at("target_world"));
    const auto width = UInt(cameraJson.at("width"), 2048), height = UInt(cameraJson.at("height"),2048);
    Check(width >= 64 && height >= 64, "pair viewport outside bounded audit");
    const War3RenderCameraV2 camera{sizeof(camera),target[0],target[1],target[2],Number(cameraJson.at("distance")),
        Number(cameraJson.at("yaw_radians")),Number(cameraJson.at("pitch_radians"))};
    Check(camera.distance > 0 && camera.pitchRadians > 0 && camera.pitchRadians < 1.5707964f, "pair camera qualification invalid");
    const War3RenderTerrainMipSceneV2 scene{sizeof(scene),static_cast<std::uint32_t>(vertices.size()),
        static_cast<std::uint32_t>(indices.size()),static_cast<std::uint32_t>(chains.size()),static_cast<std::uint32_t>(batches.size()),
        revision,vertices.data(),indices.data(),chains.data(),batches.data()};
    const auto frame = capture(scene,camera,width,height); const auto& receipt = frame.receipt;
    Check(frame.pixels.size() == static_cast<std::size_t>(width)*height*4 && frame.status.software == 1 &&
        frame.status.result == 0 && frame.status.deviceReady == 1 && frame.status.width == width && frame.status.height == height &&
        frame.status.documentRevision == revision && frame.status.frameCount >= receipt.capture.frameCount &&
        receipt.capture.width == width && receipt.capture.height == height &&
        receipt.capture.documentRevision == revision && receipt.view.documentRevision == revision && receipt.capture.ticket &&
        receipt.capture.frameCount && receipt.capture.frameCount == receipt.view.frameCount && receipt.view.surfaceEpoch &&
        receipt.view.sceneEpoch && receipt.view.viewEpoch && std::memcmp(&camera,&receipt.camera,sizeof(camera)) == 0,
        "pair completed atomic WARP identity mismatch");
    using namespace DirectX;
    XMFLOAT4X4 matrix; std::memcpy(&matrix,receipt.view.viewProjection,sizeof(matrix));
    XMVECTOR determinant; const auto inverse = XMMatrixInverse(&determinant,XMLoadFloat4x4(&matrix));
    Check(std::isfinite(XMVectorGetX(determinant)) && XMVectorGetX(determinant) != 0, "pair capture matrix singular");
    struct Sample { bool present = false; double score = std::numeric_limits<double>::infinity(); Json value; std::array<std::uint8_t,4> rgba{}; };
    std::array<std::array<Sample,2>,kStripRows> samples{};
    const float edgeX = context.terrain.vertices[7].position[0], south = context.terrain.vertices[7].position[1];
    const float planeZ = context.terrain.vertices[7].position[2];
    for (std::uint32_t py = 0; py < height; ++py) for (std::uint32_t px = 0; px < width; ++px) {
        const float nx = (static_cast<float>(px)+0.5f)*2/static_cast<float>(width)-1;
        const float ny = 1-(static_cast<float>(py)+0.5f)*2/static_cast<float>(height);
        XMFLOAT3 nearPoint,farPoint;
        XMStoreFloat3(&nearPoint,XMVector3TransformCoord(XMVectorSet(nx,ny,0,1),inverse));
        XMStoreFloat3(&farPoint,XMVector3TransformCoord(XMVectorSet(nx,ny,1,1),inverse));
        const double dz = static_cast<double>(farPoint.z)-nearPoint.z; if (!std::isfinite(dz) || std::abs(dz) < 1e-12) continue;
        const double ray = (static_cast<double>(planeZ)-nearPoint.z)/dz; if (!(ray >= 0 && ray <= 1)) continue;
        const double wx = nearPoint.x + ray*(static_cast<double>(farPoint.x)-nearPoint.x);
        const double wy = nearPoint.y + ray*(static_cast<double>(farPoint.y)-nearPoint.y);
        if (!std::isfinite(wx) || !std::isfinite(wy) || wy <= south || wy >= south+128 ||
            std::abs(wx-edgeX) < 1e-5 || std::abs(wx-edgeX) >= 4) continue;
        const auto bin = std::min(kStripRows-1,static_cast<std::uint32_t>((wy-south)/128*kStripRows));
        const auto side = wx < edgeX ? 0u : 1u; auto& sample = samples[bin][side];
        const double wantedY = south+(static_cast<double>(bin)+0.5)*128/kStripRows;
        const double score = std::abs(wx-edgeX)+std::abs(wy-wantedY);
        if (score >= sample.score) continue;
        const auto offset = (static_cast<std::size_t>(py)*width+px)*4;
        std::copy_n(frame.pixels.begin()+static_cast<std::ptrdiff_t>(offset),4,sample.rgba.begin());
        Check(sample.rgba[3] == 255, "pair covered sample target alpha is not opaque");
        sample.present = true; sample.score = score;
        sample.value = {{"pixel",Json::array({px,py})},{"world_pixel_center",Json::array({wx,wy,planeZ})},
            {"rgba",sample.rgba},{"distance_to_edge_world",std::abs(wx-edgeX)}};
    }
    Json edge = Json::array(); std::vector<std::uint8_t> strip; strip.reserve(kStripRows*2*4);
    std::array<unsigned,4> maximum{}; std::array<double,4> totals{};
    for (std::uint32_t bin = 0; bin < kStripRows; ++bin) {
        Check(samples[bin][0].present && samples[bin][1].present, "pair finite strip lacks qualified samples on both sides");
        Check(samples[bin][0].value.at("pixel") != samples[bin][1].value.at("pixel"), "pair sides alias one frame pixel");
        std::array<unsigned,4> difference{};
        for (unsigned ch = 0; ch < 4; ++ch) {
            difference[ch] = static_cast<unsigned>(std::abs(static_cast<int>(samples[bin][0].rgba[ch])-samples[bin][1].rgba[ch]));
            maximum[ch] = std::max(maximum[ch],difference[ch]); totals[ch] += difference[ch];
        }
        for (const auto& sample : samples[bin]) strip.insert(strip.end(),sample.rgba.begin(),sample.rgba.end());
        edge.push_back({{"bin",bin},{"t_center",(static_cast<double>(bin)+0.5)/kStripRows},
            {"left",samples[bin][0].value},{"right",samples[bin][1].value},{"absolute_difference",difference}});
    }
    for (auto& total : totals) total /= kStripRows;
    if (!std::filesystem::exists(output)) Check(std::filesystem::create_directories(output), "pair output directory creation failed");
    WriteBytes(output/"rgba.bin",frame.pixels); saveBmp(output/"frame.bmp",frame.pixels,width,height);
    WriteBytes(output/"edge-strip.rgba",strip); saveBmp(output/"edge-strip.bmp",strip,2,kStripRows);
    Json result = {{"schema","terrain-real-pair-audit-result-v1"},{"completed",true},{"manifest_sha256",std::string(manifestSha)},
        {"terrain_binding",binding},{"camera",cameraJson},{"lighting",light},{"environment",manifest.at("environment")},
        {"selected_neighbors",manifest.at("selected_neighbors")},{"sampling_manifest",manifest.at("sampling_manifest")},
        {"resources",resourceReceipts},{"renderer",{{"software",frame.status.software},{"result",frame.status.result},
            {"device_ready",frame.status.deviceReady},{"width",frame.status.width},{"height",frame.status.height},
            {"document_revision",frame.status.documentRevision},{"frame_count",frame.status.frameCount},{"message",frame.status.message}}},
        {"captured_camera",{{"target_world",Json::array({receipt.camera.targetX,receipt.camera.targetY,receipt.camera.targetZ})},
            {"distance",receipt.camera.distance},{"yaw_radians",receipt.camera.yawRadians},{"pitch_radians",receipt.camera.pitchRadians}}},
        {"capture",{{"width",width},{"height",height},{"ticket",receipt.capture.ticket},{"frame_count",receipt.capture.frameCount},
            {"document_revision",revision},{"surface_epoch",receipt.view.surfaceEpoch},{"scene_epoch",receipt.view.sceneEpoch},
            {"view_epoch",receipt.view.viewEpoch},{"view_projection",std::vector<float>(std::begin(receipt.view.viewProjection),std::end(receipt.view.viewProjection))}}},
        {"geometry",{{"original_vertex_dimensions",Json::array({context.original.width,context.original.height})},
            {"local_vertex_dimensions",Json::array({5,4})},{"local_offset",context.terrain.header.offset},
            {"halo_vertices",20},{"source_pair_cells",selected.at("cells")},{"quads",quadReceipts},
            {"full_vertex_count",vertices.size()},{"full_index_count",mesh.indices.size()},
            {"submitted_index_count",indices.size()},{"submitted_batch_count",batches.size()},{"fallback_tiles",0}}},
        {"files",{{"rgba",FileReceipt(output/"rgba.bin")},{"bmp",FileReceipt(output/"frame.bmp")},
            {"edge_strip_rgba",FileReceipt(output/"edge-strip.rgba")},{"edge_strip_bmp",FileReceipt(output/"edge-strip.bmp")}}},
        {"edge_strip",{{"width",2},{"height",kStripRows},{"row_order","south-to-north"},{"columns",Json::array({"left","right"})}}},
        {"edge_samples",edge},{"edge_statistics",{{"absolute_difference_max_rgba",maximum},{"absolute_difference_mean_rgba",totals},
            {"interpretation","observation only; no RGB seam equality threshold"}}},
        {"scope","one real flat pair, production original-mip WARP sampling at the explicit audit camera; not full-map/editor/game proof"},
        {"caveats",Json::array({"Source texture art and opposite cell borders may differ legitimately.",
            "Verified/uploaded mip chains do not prove every mip or fractional LOD was sampled by this one camera.",
            "Target alpha=255 preserves destination opacity and does not report overlay source alpha.",
            "Each side is a distinct covered pixel center near the edge, not two fragments at the exact shared coordinate.",
            "Source file lifetime is owned by the caller read leases; verified reads produce independent byte snapshots."})}};
    const auto encoded = result.dump(2)+"\n";
    WriteBytes(output/"result.json.tmp",std::span(reinterpret_cast<const std::uint8_t*>(encoded.data()),encoded.size()));
    std::filesystem::rename(output/"result.json.tmp",output/"result.json");
}
} // namespace War3Map::Tests
