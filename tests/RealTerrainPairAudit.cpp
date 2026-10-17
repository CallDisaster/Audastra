#include "RealTerrainPairAudit.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/VerifiedFile.h"
#include "War3MapCore/Render/TerrainGroundMesh.h"
#include "War3MapCore/Render/TerrainCliffs.h"
#include "War3MapCore/Resources/TerrainTable.h"
#include "MdxTestModel.h"
#include <DirectXMath.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>

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
    std::u8string utf8;
    for (const unsigned char byte : value.get<std::string>()) utf8.push_back(static_cast<char8_t>(byte));
    const std::filesystem::path path(utf8);
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
const Image::DecodedImage& Mip(const TerrainAtlasSnapshot& atlas, std::size_t level) {
    return level ? atlas.additionalMips.at(level - 1) : atlas.image;
}
bool CellSampleable(const Image::DecodedImage& mip, std::uint32_t columns) {
    return mip.width >= columns && mip.height >= TerrainAtlasSnapshot::rows &&
        mip.width % columns == 0 && mip.height % TerrainAtlasSnapshot::rows == 0;
}
std::uint32_t HostMaxSampleMip(const TerrainAtlasSnapshot& atlas) {
    // 与既有 TerrainPreviewFrame descriptor 相同；完整保留链与可采样上限分开。
    std::uint32_t maximum = 0;
    for (std::size_t level = 1; level <= atlas.additionalMips.size(); ++level) {
        if (!CellSampleable(Mip(atlas,level),atlas.columns)) break;
        maximum = static_cast<std::uint32_t>(level);
    }
    return maximum;
}
struct MipContract {
    Json audited = Json::array(), retained = Json::array();
    std::uint32_t auditedCount = 0, retainedCount = 0, validatedCount = 0, maxSampleMip = 0;
};
void CheckRequestedSampleMip(const MipContract& contract, std::uint32_t maximum) {
    Check(maximum == contract.maxSampleMip && maximum < contract.auditedCount,
        "pair sample range must exactly cover the audited cell-sampleable prefix, never a retained tail");
}
MipContract ValidateMipContract(const TerrainAtlasSnapshot& atlas, const Json& auditedLevels) {
    const TerrainAtlasLimits limits;
    Check((atlas.columns == 4 || atlas.columns == 8) && atlas.image.width == atlas.sourceWidth &&
        atlas.image.height == atlas.sourceHeight && atlas.image.height % atlas.rows == 0 &&
        atlas.image.width == atlas.image.height / atlas.rows * atlas.columns,
        "pair original atlas layout invalid");
    const auto count = atlas.additionalMips.size()+1;
    Check(count >= 1 && count <= 12 && atlas.validatedMipCount == count,
        "pair retained and validated original mip counts disagree");
    Check(auditedLevels.is_array() && !auditedLevels.empty() && auditedLevels.size() <= 12 && auditedLevels.size() <= count,
        "pair audited prefix count invalid");
    MipContract contract;
    contract.auditedCount = static_cast<std::uint32_t>(auditedLevels.size());
    contract.retainedCount = static_cast<std::uint32_t>(count); contract.validatedCount = atlas.validatedMipCount;
    std::size_t retainedBytes = 0, totalPixels = 0;
    for (std::size_t level = 0; level < count; ++level) {
        const auto& mip = Mip(atlas,level);
        Check(mip.width <= limits.maxDimension && mip.height <= limits.maxDimension && mip.Valid(),
            "pair retained mip dimensions or RGBA byte count invalid");
        if (level) {
            const auto& previous = Mip(atlas,level-1);
            Check((previous.width != 1 || previous.height != 1) && mip.width == std::max(1u,previous.width/2) &&
                mip.height == std::max(1u,previous.height/2), "pair retained mip chain is not continuous original half sizes");
        }
        const auto pixels = static_cast<std::size_t>(mip.width)*mip.height;
        Check(mip.rgba.size() <= limits.maxRgbaBytes && mip.rgba.size() <= limits.maxRetainedRgbaBytes-retainedBytes &&
            pixels <= limits.maxTotalMipPixels-totalPixels, "pair retained original mip chain exceeds existing limits");
        retainedBytes += mip.rgba.size(); totalPixels += pixels;
        const bool sampleable = CellSampleable(mip,atlas.columns), audited = level < auditedLevels.size();
        const auto sha = Digest(mip.rgba);
        if (audited) {
            const auto& expected = auditedLevels[level];
            Check(sampleable && UInt(expected.at("level")) == level && UInt(expected.at("width")) == mip.width &&
                UInt(expected.at("height")) == mip.height && expected.at("rgba_sha256").get<std::string>() == sha,
                "pair audited original mip identity mismatch");
            contract.audited.push_back({{"level",level},{"width",mip.width},{"height",mip.height},{"rgba_sha256",sha}});
        } else Check(!sampleable, "pair unaudited retained mip is still cell-sampleable");
        contract.retained.push_back({{"level",level},{"width",mip.width},{"height",mip.height},
            {"rgba_sha256",sha},{"cell_sampleable",sampleable},{"audited",audited}});
    }
    contract.maxSampleMip = HostMaxSampleMip(atlas);
    Check(contract.auditedCount == contract.maxSampleMip+1,
        "pair audited prefix must exactly equal the original host cell-sampleable range");
    CheckRequestedSampleMip(contract,contract.maxSampleMip);
    return contract;
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
    std::vector<std::uint32_t> materials;
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
        selected.at("materials").is_array() && selected.at("materials").size() >= 2 &&
        selected.at("materials").size() <= 3, "pair exact six vertices, twenty halo and two or three materials required");
    for (const auto& material : selected.at("materials")) c.materials.push_back(UInt(material,15));
    Check(std::is_sorted(c.materials.begin(),c.materials.end()) &&
        std::adjacent_find(c.materials.begin(),c.materials.end()) == c.materials.end(), "pair material order invalid");
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
        Check(std::binary_search(c.materials.begin(),c.materials.end(),v.GroundTextureIndex()),
            "pair halo introduces another material");
    }
    std::set<std::uint32_t> selectedVertices;
    std::set<std::uint32_t> coreMaterials;
    for (const auto& j : selected.at("vertices")) {
        const auto x = UInt(j.at("x")), y = UInt(j.at("y"));
        Check(x >= c.x && x <= c.x + 2 && y >= c.y && y <= c.y + 1, "pair core vertex outside pair");
        Check(selectedVertices.insert(UInt(j.at("source_index"))).second, "pair duplicate core vertex");
        const auto v = Vertex(j, c.original);
        coreMaterials.insert(v.GroundTextureIndex());
        const auto& h = c.terrain.vertices[(y - c.y + 1) * 5 + x - c.x + 1];
        Check(v.groundHeightRaw == h.groundHeightRaw && v.waterHeightAndFlagsRaw == h.waterHeightAndFlagsRaw &&
            v.textureAndFlagsRaw == h.textureAndFlagsRaw && v.variationRaw == h.variationRaw &&
            v.cliffAndLayerRaw == h.cliffAndLayerRaw && v.textureFlagsExtensionRaw == h.textureFlagsExtensionRaw &&
            v.position == h.position && v.waterBaseHeight == h.waterBaseHeight, "pair core/halo raw mismatch");
    }
    Check(std::vector<std::uint32_t>(coreMaterials.begin(),coreMaterials.end()) == c.materials,
        "pair declared materials must exactly equal the real core union");
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

using LayerKey = std::tuple<std::uint32_t,std::uint32_t,bool>; // local tile、原palette、是否overlay
std::set<LayerKey> RequiredLayers(const Context& context) {
    std::set<LayerKey> expected;
    for (const auto tile : context.tiles) {
        std::set<std::uint32_t> materials;
        for (const auto corner : context.terrain.tiles[tile].vertices)
            materials.insert(context.terrain.vertices[corner].GroundTextureIndex());
        for (const auto material : materials) expected.emplace(tile,material,material != *materials.begin());
    }
    return expected;
}
std::uint32_t PaletteForTexture(const Context& context,
    const std::vector<std::shared_ptr<const TerrainAtlasSnapshot>>& atlases,
    const std::shared_ptr<const TerrainAtlasSnapshot>& texture) {
    std::optional<std::uint32_t> found;
    for (const auto material : context.materials) if (texture && atlases.at(material) == texture) {
        Check(!found, "pair submitted atlas has ambiguous original palette identity"); found = material;
    }
    Check(found.has_value(), "pair submitted layer lacks an audited original palette source"); return *found;
}
struct LayerLayout { unsigned mask; std::array<unsigned,2> cell; };
LayerLayout ValidateLayerLayout(const Context& context, std::uint32_t tile, std::uint32_t material,
    bool alphaBlend, const TerrainAtlasSnapshot& atlas, std::span<const TerrainGroundVertex> quad) {
    Check(quad.size() == 4, "pair layer must contain four original corners");
    const auto& corners = context.terrain.tiles.at(tile).vertices;
    std::set<unsigned> materials; unsigned mask = 0;
    for (unsigned i = 0; i < 4; ++i) {
        const auto ground = context.terrain.vertices[corners[i]].GroundTextureIndex(); materials.insert(ground);
        if (ground == material) mask |= std::array<unsigned,4>{2,1,8,4}[i];
    }
    Check(materials.contains(material) && alphaBlend == (material != *materials.begin()),
        "pair layer is not the actual lowest base or a present higher overlay");
    std::array<unsigned,2> cell{mask%4,mask/4};
    if (!alphaBlend) {
        const auto variation = context.terrain.vertices[corners[0]].GroundVariation();
        if (atlas.columns == 4) cell = variation == 0 ? std::array<unsigned,2>{0,0} : std::array<unsigned,2>{3,3};
        else if (variation < 16) cell = {4u+variation%4u,variation/4u};
        else cell = variation == 16 ? std::array<unsigned,2>{3,3} : std::array<unsigned,2>{0,0};
    }
    const auto* rect = atlas.Cell(cell[0],cell[1]); Check(rect, "pair production layer cell invalid");
    const auto width = static_cast<float>(atlas.image.width), height = static_cast<float>(atlas.image.height);
    const float left = (static_cast<float>(rect->x)+0.5f)/width, right = (static_cast<float>(rect->x+rect->width)-0.5f)/width;
    const float top = (static_cast<float>(rect->y)+0.5f)/height, bottom = (static_cast<float>(rect->y+rect->height)-0.5f)/height;
    for (unsigned i = 0; i < 4; ++i)
        Check(quad[i].u == (i%2 == 0 ? left : right) && quad[i].v == (i < 2 ? bottom : top),
            "pair production layer four-corner UV/mask or SW variation mismatch");
    return {mask,cell};
}

void RunMipContractValidationTests() {
    // 原创小图仅验证身份、链结构和取样资格，不模拟真实纹理像素或过滤。
    TerrainAtlasSnapshot atlas; atlas.columns = 4; atlas.sourceWidth = atlas.sourceHeight = 16;
    for (std::uint32_t size = 16; size; size /= 2) {
        Image::DecodedImage image{size,size,std::vector<std::byte>(static_cast<std::size_t>(size)*size*4,
            static_cast<std::byte>(size))};
        if (size == 16) atlas.image = std::move(image); else atlas.additionalMips.push_back(std::move(image));
    }
    atlas.validatedMipCount = 5;
    Json levels = Json::array();
    for (std::size_t i = 0; i < 3; ++i) {
        const auto& mip = Mip(atlas,i);
        levels.push_back({{"level",i},{"width",mip.width},{"height",mip.height},{"rgba_sha256",Digest(mip.rgba)}});
    }
    const auto valid = ValidateMipContract(atlas,levels);
    Check(valid.auditedCount == 3 && valid.retainedCount == 5 && valid.validatedCount == 5 && valid.maxSampleMip == 2 &&
        !valid.retained[3].at("cell_sampleable").get<bool>() && !valid.retained[4].at("audited").get<bool>(),
        "pair original mip fixture failed");
    const auto reject = [](const TerrainAtlasSnapshot& candidate, const Json& prefix) {
        bool rejected = false; try { (void)ValidateMipContract(candidate,prefix); } catch (const std::exception&) { rejected = true; }
        Check(rejected, "pair malformed mip contract accepted");
    };
    auto prefix = levels; prefix[1]["rgba_sha256"] = std::string(64,'0'); reject(atlas,prefix);
    prefix = levels; prefix[1]["width"] = 4; reject(atlas,prefix);
    prefix = levels; prefix[1]["level"] = 2; reject(atlas,prefix);
    prefix = levels; prefix.erase(2); reject(atlas,prefix); // 仍可采样的第2级不得留作未审尾层。
    auto changed = atlas; changed.validatedMipCount = 4; reject(changed,levels);
    changed = atlas; changed.additionalMips[0].rgba.pop_back(); reject(changed,levels);
    changed = atlas; changed.additionalMips[2] = changed.additionalMips[1]; reject(changed,levels);
    changed = atlas; changed.additionalMips.push_back(changed.additionalMips.back()); changed.validatedMipCount = 6; reject(changed,levels);
    bool tailRejected = false;
    try { CheckRequestedSampleMip(valid,3); } catch (const std::exception&) { tailRejected = true; }
    Check(tailRejected, "pair retained tail was allowed into the requested sample range");
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
void RunLayerValidationTests() {
    auto original = Fixture(); original["materials"] = Json::array({1,2,3});
    for (const auto* field : {"vertices","halo_vertices"}) for (auto& v : original[field]) {
        const auto x = UInt(v.at("x")), y = UInt(v.at("y"));
        if (x >= 2 && x <= 4 && y >= 2 && y <= 3) {
            const unsigned ground = y == 2 ? (x == 3 ? 1u : 3u) : (x == 3 ? 3u : 2u);
            v["ground_texture_index"] = ground; v["texture_and_flags_raw"] = ground;
        }
    }
    const auto context = ValidateContext(original);
    std::vector<std::shared_ptr<const TerrainAtlasSnapshot>> atlases(16);
    for (const auto material : context.materials) {
        auto atlas = std::make_shared<TerrainAtlasSnapshot>();
        atlas->columns = 4; atlas->sourceWidth = atlas->sourceHeight = 16; atlas->validatedMipCount = 1;
        atlas->image = {16,16,std::vector<std::byte>(16*16*4,std::byte{255})};
        for (unsigned y = 0; y < 4; ++y) for (unsigned x = 0; x < 4; ++x) atlas->cells.push_back({x,y,x*4,y*4,4,4});
        atlases[material] = std::move(atlas);
    }
    TerrainLighting lighting; lighting.enabled = false;
    const auto built = BuildTerrainGroundMesh(context.terrain,atlases,{},lighting);
    Check(built.Succeeded(), "pair original three-material mesh fixture failed");
    const auto& mesh = *built.value; std::set<LayerKey> seen; std::vector<TerrainGroundVertex> middle;
    for (const auto& batch : mesh.batches) for (std::uint32_t at = batch.firstIndex; at < batch.firstIndex+batch.indexCount; at += 6) {
        const auto first = mesh.indices[at], quad = first/4;
        const auto tile = quad < 12 ? quad : mesh.overlayTiles.at(quad-12);
        if (tile != 5 && tile != 6) continue;
        const auto material = PaletteForTexture(context,atlases,mesh.textures[batch.textureIndex]);
        const auto vertices = std::span<const TerrainGroundVertex>(mesh.vertices).subspan(first,4);
        const auto layout = ValidateLayerLayout(context,tile,material,batch.alphaBlend,*atlases[material],vertices);
        Check(seen.emplace(tile,material,batch.alphaBlend).second, "pair three-material fixture duplicates a layer");
        if (tile == 5 && material == 2) {
            Check(layout.mask == 8, "pair three-material fixture middle equality mask changed");
            middle.assign(vertices.begin(),vertices.end());
        }
    }
    Check(seen == RequiredLayers(context) && seen.size() == 6 && middle.size() == 4,
        "pair three-material fixture omits an original layer");
    const auto reject = [](const auto& action) {
        bool rejected = false; try { action(); } catch (const std::exception&) { rejected = true; }
        Check(rejected, "pair malformed three-material layer fixture accepted");
    };
    reject([&] { (void)ValidateLayerLayout(context,5,3,true,*atlases[3],middle); }); // 中层误当最高材质。
    auto wrongUv = middle; wrongUv[3].v += 1.0f/16;
    reject([&] { (void)ValidateLayerLayout(context,5,2,true,*atlases[2],wrongUv); }); // 非SW角也必须核。
    auto missing = atlases; missing[2].reset();
    reject([&] { (void)PaletteForTexture(context,missing,atlases[2]); });
    auto pure = context;
    for (const auto corner : pure.terrain.tiles[5].vertices) pure.terrain.vertices[corner].textureAndFlagsRaw = 3;
    for (const auto corner : {8u,13u}) pure.terrain.vertices[corner].textureAndFlagsRaw = 1;
    Check(RequiredLayers(pure).size() == 3, "pair pure-cell fixture must not invent an overlay");
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
    RunMipContractValidationTests();
    RunLayerValidationTests();
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
    j = original; j["materials"] = Json::array({1,2,3}); reject(j);
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
    std::vector<std::pair<std::shared_ptr<const TerrainAtlasSnapshot>,MipContract>> mipContracts;
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
        const auto& levels = texture->at("levels");
        if (levels.size() != atlas.additionalMips.size() + 1) {
            std::cerr << "pair_mip_counts palette=" << index << " audited=" << levels.size()
                << " retained=" << atlas.additionalMips.size() + 1 << " validated=" << atlas.validatedMipCount << '\n';
            for (std::size_t level = 0; level <= atlas.additionalMips.size(); ++level) {
                const auto& mip = level ? atlas.additionalMips[level - 1] : atlas.image;
                const bool cellSampleable = mip.width >= atlas.columns && mip.height >= atlas.rows
                    && mip.width % atlas.columns == 0 && mip.height % atlas.rows == 0;
                std::cerr << "pair_mip_level level=" << level << " width=" << mip.width << " height=" << mip.height
                    << " cell_sampleable=" << cellSampleable << " rgba_sha256=" << Digest(mip.rgba) << '\n';
            }
        }
        auto mipContract = ValidateMipContract(atlas,levels);
        resourceReceipts.push_back({{"palette_index",index},{"source_sha256",atlas.sourceSha256},
            {"source_path",sourcePath.generic_string()},{"columns",atlas.columns},{"audited_mips",mipContract.audited},
            {"retained_mips",mipContract.retained},{"audited_sampleable_mip_count",mipContract.auditedCount},
            {"retained_mip_count",mipContract.retainedCount},{"validated_mip_count",mipContract.validatedCount},
            {"max_sample_mip",mipContract.maxSampleMip}});
        mipContracts.emplace_back(atlases[index],std::move(mipContract));
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
    const auto expectedLayers = RequiredLayers(context); std::set<LayerKey> observedLayers;
    std::set<std::pair<bool,std::uint32_t>> expectedBatches;
    for (const auto& [tile,material,alphaBlend] : expectedLayers) expectedBatches.emplace(alphaBlend,material);
    bool overlaysStarted = false; std::uint32_t previousOverlay = 0;
    for (const auto& batch : mesh.batches) {
        Check(!batch.translucent && batch.indexCount % 6 == 0 && batch.textureIndex < mesh.textures.size(), "pair unexpected production batch");
        const auto firstIndex = static_cast<std::uint32_t>(indices.size());
        for (std::uint32_t at = batch.firstIndex; at < batch.firstIndex + batch.indexCount; at += 6) {
            const auto first = mesh.indices.at(at); Check(first % 4 == 0, "pair production quad identity invalid");
            const auto quad = first / 4;
            const auto tile = quad < 12 ? quad : mesh.overlayTiles.at(quad - 12);
            if (tile != context.tiles[0] && tile != context.tiles[1]) continue;
            const auto& corners = context.terrain.tiles[tile].vertices;
            const auto material = PaletteForTexture(context,atlases,mesh.textures[batch.textureIndex]);
            const auto& atlas = *mesh.textures[batch.textureIndex];
            const auto layout = ValidateLayerLayout(context,tile,material,batch.alphaBlend,atlas,
                std::span<const TerrainGroundVertex>(mesh.vertices).subspan(first,4));
            const auto mask = layout.mask;
            Check(observedLayers.emplace(tile,material,batch.alphaBlend).second, "pair production repeated a layer");
            if (batch.alphaBlend) {
                Check(!overlaysStarted || material >= previousOverlay, "pair production overlay palette order changed");
                overlaysStarted = true; previousOverlay = material;
            } else {
                Check(!overlaysStarted, "pair production base appeared after an overlay");
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
                {"cell",layout.cell},{"sw_variation_raw",context.terrain.vertices[corners[0]].variationRaw},
                {"sw_ground_variation",context.terrain.vertices[corners[0]].GroundVariation()},{"shade",shades},{"uv_sw_se_nw_ne",uvs}});
            const std::array<std::uint32_t,6> expected{first,first+1,first+2,first+1,first+3,first+2};
            Check(std::equal(expected.begin(), expected.end(), mesh.indices.begin()+at), "pair production diagonal changed");
            indices.insert(indices.end(), mesh.indices.begin()+at, mesh.indices.begin()+at+6);
            if (batch.alphaBlend) ++overlayCount; else ++baseCount;
        }
        if (indices.size() != firstIndex) batches.push_back({firstIndex,static_cast<std::uint32_t>(indices.size()-firstIndex),
            batch.textureIndex | (batch.alphaBlend ? WAR3_RENDER_TERRAIN_ALPHA_BLEND : 0u)});
    }
    Check(observedLayers == expectedLayers && baseCount == context.tiles.size() &&
        overlayCount == expectedLayers.size()-context.tiles.size() && indices.size() == expectedLayers.size()*6 &&
        batches.size() == expectedBatches.size(), "pair production omitted or added an actual base/transition layer");
    std::vector<std::vector<War3RenderMipRgbaV2>> mipLists(mesh.textures.size());
    std::vector<War3RenderTextureMipChainV2> chains;
    for (std::size_t i = 0; i < mesh.textures.size(); ++i) {
        const auto& texture = *mesh.textures[i]; auto& mips = mipLists[i];
        const auto append = [&](const Image::DecodedImage& mip) { mips.push_back({sizeof(War3RenderMipRgbaV2),
            mip.width,mip.height,mip.rgba.size(),mip.rgba.data()}); };
        append(texture.image); for (const auto& mip : texture.additionalMips) append(mip);
        const auto maximum = HostMaxSampleMip(texture);
        const auto contract = std::find_if(mipContracts.begin(),mipContracts.end(),[&](const auto& item) {
            return item.first == mesh.textures[i];
        });
        Check(contract != mipContracts.end(), "pair submitted texture has no audited mip contract");
        CheckRequestedSampleMip(contract->second,maximum);
        Check(mips.size() == contract->second.retainedCount, "pair descriptor dropped retained mip data");
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
    Json result = {{"schema","terrain-real-pair-audit-result-v2"},{"completed",true},{"manifest_sha256",std::string(manifestSha)},
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
            {"materials",context.materials},{"expected_layer_count",expectedLayers.size()},
            {"observed_layer_count",observedLayers.size()},{"base_quad_count",baseCount},{"overlay_quad_count",overlayCount},
            {"full_vertex_count",vertices.size()},{"full_index_count",mesh.indices.size()},
            {"submitted_index_count",indices.size()},{"submitted_batch_count",batches.size()},{"fallback_tiles",0}}},
        {"files",{{"rgba",FileReceipt(output/"rgba.bin")},{"bmp",FileReceipt(output/"frame.bmp")},
            {"edge_strip_rgba",FileReceipt(output/"edge-strip.rgba")},{"edge_strip_bmp",FileReceipt(output/"edge-strip.bmp")}}},
        {"edge_strip",{{"width",2},{"height",kStripRows},{"row_order","south-to-north"},{"columns",Json::array({"left","right"})}}},
        {"edge_samples",edge},{"edge_statistics",{{"absolute_difference_max_rgba",maximum},{"absolute_difference_mean_rgba",totals},
            {"interpretation","observation only; no RGB seam equality threshold"}}},
        {"scope","one real flat pair, production original-mip WARP sampling at the explicit audit camera; not full-map/editor/game proof"},
        {"caveats",Json::array({"Source texture art and opposite cell borders may differ legitimately.",
            "The audited cell-sampleable prefix is exact; retained smaller tails are hashed/uploaded but cannot be sampled as atlas cells.",
            "Verified/uploaded mip chains do not prove every audited mip or fractional LOD was sampled by this one camera.",
            "Target alpha=255 preserves destination opacity and does not report overlay source alpha.",
            "Each side is a distinct covered pixel center near the edge, not two fragments at the exact shared coordinate.",
            "Source file lifetime is owned by the caller read leases; verified reads produce independent byte snapshots."})}};
    const auto encoded = result.dump(2)+"\n";
    WriteBytes(output/"result.json.tmp",std::span(reinterpret_cast<const std::uint8_t*>(encoded.data()),encoded.size()));
    std::filesystem::rename(output/"result.json.tmp",output/"result.json");
}
namespace {
// 此入口只消费已存block与明确资源；路径不能逃出manifest目录，也不接受地图/游戏入口。
std::filesystem::path RelativePathText(const std::string& text) {
    Check(!text.empty() && text.size() <= 1024 && text.find(':') == std::string::npos &&
        text.find('\\') == std::string::npos && text.find("//") == std::string::npos &&
        text.back() != '/' && std::none_of(text.begin(),text.end(),[](unsigned char c) {
            return c < 32 || c == '<' || c == '>' || c == '"' || c == '|' || c == '?' || c == '*';
        }), "cliff-ground input requires a plain relative path");
    std::u8string utf8; for (const unsigned char c : text) utf8.push_back(static_cast<char8_t>(c));
    const std::filesystem::path relative(utf8);
    Check(!relative.is_absolute() && !relative.has_root_path(), "cliff-ground absolute input denied");
    const auto extension=relative.extension().string();
    Check(extension==".json" || extension==".bin" || extension==".slk" || extension==".mdx" || extension==".blp",
        "cliff-ground map/archive/unlisted input type denied");
    for (const auto& component : relative) {
        const auto part=component.u8string();
        Check(component != "." && component != ".." && !part.empty() && part.back()!=u8'.' && part.back()!=u8' ',
            "cliff-ground input path has traversal or a Windows alias");
    }
    return relative;
}
std::filesystem::path RelativeInput(const std::filesystem::path& root, const Json& reference) {
    const auto relative=RelativePathText(reference.at("path").get<std::string>());
    auto resolved = root;
    for (const auto& component : relative) {
        resolved /= component;
        Check(!std::filesystem::is_symlink(std::filesystem::symlink_status(resolved)), "cliff-ground input symlink denied");
    }
    const auto canonical = std::filesystem::canonical(resolved);
    const auto inside = canonical.lexically_relative(std::filesystem::canonical(root));
    Check(!inside.empty() && !inside.is_absolute() && *inside.begin() != ".." &&
        std::filesystem::is_regular_file(canonical), "cliff-ground input is outside its manifest tree");
    return canonical;
}
std::vector<std::byte> RelativeBytes(const std::filesystem::path& root, const Json& reference,
    std::uint64_t limit, std::string_view extension = {}) {
    const auto path = RelativeInput(root,reference);
    Check(extension.empty() || path.extension() == extension, "cliff-ground input extension mismatch");
    const auto sha = reference.at("sha256").get<std::string>(); Check(IsSha(sha), "cliff-ground input SHA invalid");
    auto bytes = ReadVerifiedFile(path,sha,limit); Check(bytes.Succeeded(), "cliff-ground verified input failed");
    return std::move(*bytes.value);
}
Json RelativeJson(const std::filesystem::path& root, const Json& reference) {
    const auto bytes = RelativeBytes(root,reference,kJsonLimit);
    return Json::parse(reinterpret_cast<const char*>(bytes.data()),reinterpret_cast<const char*>(bytes.data()+bytes.size()));
}
std::string VirtualPath(std::string path) {
    Check(!path.empty() && path.size() <= 260 && path.front() != '/' && path.front() != '\\' &&
        path.find(':') == std::string::npos && std::none_of(path.begin(),path.end(),[](unsigned char c) {
            return c < 32 || c == '<' || c == '>' || c == '"' || c == '|' || c == '?' || c == '*';
        }), "cliff-ground virtual path invalid");
    std::replace(path.begin(),path.end(),'\\','/');
    Check(path.back()!='/' && path.find("//")==std::string::npos,"cliff-ground virtual empty component denied");
    const std::filesystem::path parts(path);
    for (const auto& part : parts) {
        const auto text=part.u8string();
        Check(part != "." && part != ".." && !text.empty() && text.back()!=u8'.' && text.back()!=u8' ',
            "cliff-ground virtual traversal/Windows alias denied");
    }
    std::replace(path.begin(),path.end(),'/','\\'); return path;
}
void RebuildGrid(TerrainGridSnapshot& grid) {
    grid.tiles.clear();
    for (std::uint32_t y=0;y+1<grid.header.height;++y) for (std::uint32_t x=0;x+1<grid.header.width;++x) {
        const auto at=y*grid.header.width+x; grid.tiles.push_back({{at,at+1,at+grid.header.width,at+grid.header.width+1}});
    }
    Check(!grid.vertices.empty(), "cliff-ground empty grid"); grid.minimum=grid.maximum=grid.vertices.front().position;
    for (const auto& v:grid.vertices) for (unsigned axis=0;axis<3;++axis) {
        grid.minimum[axis]=std::min(grid.minimum[axis],v.position[axis]);
        grid.maximum[axis]=std::max(grid.maximum[axis],v.position[axis]);
    }
}
TerrainHeader CliffHeader(const Json& j) {
    TerrainHeader header;
    Check(UInt(j.at("version"))==11 && UInt(j.at("vertex_record_bytes"))==7 && Number(j.at("tile_size_world"))==128,
        "cliff-ground requires original classic header");
    header.width=UInt(j.at("vertex_width"),2049); header.height=UInt(j.at("vertex_height"),2049);
    Check(header.width>=4 && header.height>=5 && UInt(j.at("tile_width"))==header.width-1 &&
        UInt(j.at("tile_height"))==header.height-1, "cliff-ground header dimensions invalid");
    header.mainTileset=static_cast<std::uint8_t>(UInt(j.at("main_tileset_byte"),255));
    header.usesCustomTileset=j.at("uses_custom_tileset").get<bool>();
    Check(j.at("offset_world_xy").size()==2,"cliff-ground source offset invalid");
    header.offset={Number(j.at("offset_world_xy")[0]),Number(j.at("offset_world_xy")[1])};
    for (const auto* field:{"ground_rawcodes_hex","cliff_rawcodes_hex"})
        Check(j.at(field).is_array() && !j.at(field).empty() && j.at(field).size()<=16,"cliff-ground palette bound invalid");
    for (const auto& id:j.at("ground_rawcodes_hex")) header.groundTilesets.push_back(Rawcode(id));
    for (const auto& id:j.at("cliff_rawcodes_hex")) header.cliffTilesets.push_back(Rawcode(id));
    return header;
}
struct CliffContext {
    TerrainHeader original;
    TerrainGridSnapshot halo, core;
    std::array<std::uint32_t,6> haloCore{5,6,9,10,13,14};
    std::array<std::uint32_t,6> sourceVertices{};
    std::uint32_t x=0,y=0,type=0;
    Json sourceCells, haloSource;
};
CliffContext CliffContextFromBlock(const Json& block, const Json& selection) {
    Check(block.at("ok")==true && UInt(block.at("schema_version"))==1 && block.at("is_render_capture")==false &&
        block.at("workspace_kind")=="map-archive-readonly" && block.at("content_scope")=="committed-source" &&
        block.at("coordinate_system")=="southwest-origin-x-east-y-north-z-up" &&
        block.at("surface_semantics")=="w3e-raw-layer-height-grid" &&
        block.at("preview_diagonal")=="northwest-southeast" && block.at("includes_cliff_ramp_geometry")==false &&
        block.at("includes_water_material_offset")==false && block.at("includes_entities")==false && block.at("includes_pathing")==false,
        "cliff-ground source block contract invalid");
    CliffContext c; c.original=CliffHeader(block.at("header"));
    const auto& ground=selection.at("ground_cell"); const auto& cliff=selection.at("cliff_cell");
    Check(ground.is_array() && ground.size()==2 && cliff.is_array() && cliff.size()==2,"cliff-ground selection needs two coordinates");
    c.x=UInt(ground[0],c.original.width-1); c.y=UInt(ground[1],c.original.height-1);
    Check(c.x>0 && c.y>0 && c.x+2<c.original.width && c.y+3<c.original.height &&
        UInt(cliff[0])==c.x && UInt(cliff[1])==c.y+1,"cliff-ground requires an interior ground cell immediately south of one cliff");
    const auto& rectangle=block.at("tiles"); const auto bx=UInt(rectangle.at("x"),c.original.width-1),by=UInt(rectangle.at("y"),c.original.height-1);
    const auto bw=UInt(rectangle.at("width"),256),bh=UInt(rectangle.at("height"),256);
    Check(bw && bh && std::uint64_t(bw)*bh<=256 && bx+bw<c.original.width && by+bh<c.original.height &&
        c.x-1>=bx && c.y-1>=by && c.x+2<=bx+bw && c.y+3<=by+bh &&
        block.at("vertices").size()==std::size_t(bw+1)*(bh+1) && block.at("cells").size()==std::size_t(bw)*bh,
        "cliff-ground source block does not contain its complete halo");
    c.halo.header=c.original; c.halo.header.width=4; c.halo.header.height=5;
    c.halo.header.offset={c.original.offset[0]+float(c.x-1)*128,c.original.offset[1]+float(c.y-1)*128};
    c.halo.vertices.resize(20); c.haloSource=Json::array(); std::array<bool,20> seen{};
    for (const auto& row:block.at("vertices")) {
        const auto x=UInt(row.at("x")),y=UInt(row.at("y"));
        if (x<c.x-1 || x>c.x+2 || y<c.y-1 || y>c.y+3) continue;
        const auto at=(y-(c.y-1))*4+x-(c.x-1); Check(!seen[at],"cliff-ground duplicate halo source"); seen[at]=true;
        c.halo.vertices[at]=Vertex(row,c.original); c.haloSource.push_back(row);
        Check(c.halo.vertices[at].GroundTextureIndex()<c.original.groundTilesets.size(),"cliff-ground unknown raw ground palette");
    }
    Check(std::all_of(seen.begin(),seen.end(),[](bool v){return v;}),"cliff-ground incomplete source halo");
    RebuildGrid(c.halo); c.core.header=c.original; c.core.header.width=2; c.core.header.height=3;
    c.core.header.offset={c.original.offset[0]+float(c.x)*128,c.original.offset[1]+float(c.y)*128};
    for (std::size_t i=0;i<c.haloCore.size();++i) {
        c.core.vertices.push_back(c.halo.vertices[c.haloCore[i]]);
        c.sourceVertices[i]=(c.y+static_cast<std::uint32_t>(i/2))*c.original.width+c.x+static_cast<std::uint32_t>(i%2);
    }
    RebuildGrid(c.core); c.sourceCells=Json::array();
    for (std::uint32_t side=0;side<2;++side) {
        const auto y=c.y+side; const Json* found=nullptr;
        for (const auto& row:block.at("cells")) if (UInt(row.at("x"))==c.x && UInt(row.at("y"))==y) {
            Check(!found,"cliff-ground duplicate source cell"); found=&row;
        }
        const auto sw=y*c.original.width+c.x;
        Check(found && UInt(found->at("source_index"))==y*(c.original.width-1)+c.x &&
            found->at("source_vertices_sw_se_nw_ne")==Json::array({sw,sw+1,sw+c.original.width,sw+c.original.width+1}),
            "cliff-ground source cell/corner identity invalid"); c.sourceCells.push_back(*found);
    }
    const auto low=c.core.vertices[0].CliffLevel(),high=c.core.vertices[4].CliffLevel();
    Check(high>low && high-low<=2 && c.core.vertices[1].CliffLevel()==low && c.core.vertices[2].CliffLevel()==low &&
        c.core.vertices[3].CliffLevel()==low && c.core.vertices[5].CliffLevel()==high,
        "cliff-ground source is not one ordinary straight north wall and flat south ground");
    c.type=c.core.vertices[2].CliffTextureIndex();
    Check(c.type<c.original.cliffTilesets.size() && c.type!=15,"cliff-ground source cliff palette unknown");
    for (const auto at:{2u,3u,4u,5u}) Check(c.core.vertices[at].CliffTextureIndex()==c.type,
        "cliff-ground narrow input requires unambiguous actual cliff type");
    return c;
}
void NoWarnings(std::span<const Diagnostic> diagnostics) {
    Check(std::none_of(diagnostics.begin(),diagnostics.end(),[](const auto& d){return d.severity!=DiagnosticSeverity::Info;}),
        "cliff-ground production warning/error must not become accepted partial geometry");
}
std::vector<std::optional<CliffTypeSource>> CliffTypesFromTable(const TerrainHeader& header,
    const Resources::TerrainTableSnapshot& table, Json& rows) {
    std::vector<std::optional<CliffTypeSource>> types(header.cliffTilesets.size()); rows=Json::array();
    const auto required=[](const auto& row,const char* key)->std::string {
        const auto at=row.fields.find(key); Check(at!=row.fields.end() && !at->second.empty(),"cliff-ground cliff row field missing"); return at->second;
    };
    for (std::size_t index=0;index<header.cliffTilesets.size();++index) for (const auto& row:table.Rows())
        if (row.id==header.cliffTilesets[index]) {
            Check(!types[index],"cliff-ground duplicate cliff rawcode row");
            const auto directory=required(row,"cliffmodeldir"),ramp=required(row,"rampmodeldir");
            for (const auto& part:{directory,ramp}) Check(part.size()<=64 && std::all_of(part.begin(),part.end(),[](unsigned char c){
                return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_';}),"cliff-ground cliff directory invalid");
            auto texture=VirtualPath(required(row,"texdir")+"\\"+required(row,"texfile"));
            if (!texture.ends_with(".blp")) texture+=".blp";
            const auto ground=required(row,"groundtile"),upper=required(row,"uppertile");
            Check(ground.size()==4 && (upper=="_" || upper.size()==4),"cliff-ground paired rawcodes invalid");
            types[index]=CliffTypeSource{directory,texture,ramp,ground,upper=="_"?"":upper};
            rows.push_back({{"palette_index",index},{"rawcode",row.id},{"sylk_row",row.row},{"fields",row.fields}});
        }
    return types;
}
Resources::TerrainTableSnapshot CaptureCliffTable(std::span<const std::byte> bytes) {
    Resources::ResourceResolver resolver;
    Resources::ResourceSource source;source.id="verified-audit-cliff-table";source.layer=Resources::ResourceLayer::Other;
    source.probe=[](std::string_view path) {
        return Result<Resources::ResourceProbe>::Success({path=="TERRAINART\\CLIFFTYPES.SLK"?
            Resources::ResourceProbeState::Found:Resources::ResourceProbeState::Missing,std::string(path)});
    };
    source.read=[&](std::string_view path) {
        if(path!="TERRAINART\\CLIFFTYPES.SLK")return Result<std::vector<std::byte>>::Failure("Audit","MISSING","unlisted table");
        return Result<std::vector<std::byte>>::Success(std::vector<std::byte>(bytes.begin(),bytes.end()));
    };
    Check(resolver.AddSource(std::move(source)).Succeeded(),"cliff-ground table source rejected");
    auto table=Resources::CaptureTerrainTable(resolver,"TerrainArt\\CliffTypes.slk","cliffid");
    Check(table.Succeeded(),"cliff-ground official table parse failed");NoWarnings(table.diagnostics);return std::move(*table.value);
}
struct CliffPrepared {
    TerrainGridSnapshot core;
    std::vector<std::uint8_t> selected;
    TerrainCliffs cliffs;
    TerrainGroundSelection selection;
    std::size_t rampAdjusted=0;
};
CliffPrepared PrepareCliffGround(const CliffContext& context,
    std::span<const std::optional<CliffTypeSource>> types,const ModelFileReader& read) {
    Check(types.size()==context.original.cliffTilesets.size() && types[context.type],"cliff-ground selected source row unavailable");
    auto halo=context.halo; CliffPrepared p; p.rampAdjusted=ApplyRampHeights(halo);
    Check(p.rampAdjusted==0,"cliff-ground clean ordinary halo unexpectedly needs ramp shaping");
    const auto selected=ResolveTerrainGroundSelection(halo,types); Check(selected.Succeeded(),"cliff-ground production ground selection failed");
    NoWarnings(selected.diagnostics); p.selection=*selected.value; p.core=context.core;
    for (std::size_t i=0;i<context.haloCore.size();++i) {
        p.core.vertices[i]=halo.vertices[context.haloCore[i]]; p.selected.push_back(p.selection.indices.at(context.haloCore[i]));
    }
    RebuildGrid(p.core);
    const auto built=BuildTerrainCliffs(p.core,types,read); Check(built.Succeeded(),"cliff-ground production cliff build failed");
    NoWarnings(built.diagnostics); p.cliffs=*built.value; NoWarnings(p.cliffs.warnings);
    Check(p.cliffs.cliffTiles==1 && p.cliffs.placedTiles==1 && !p.cliffs.rampPieces && !p.cliffs.rampSurfaceTiles &&
        !p.cliffs.tooHighTiles && !p.cliffs.unknownTypeTiles && !p.cliffs.missingModelTiles &&
        p.cliffs.coveredTiles==std::vector<std::uint8_t>({0,1}) && !p.cliffs.meshes.empty(),
        "cliff-ground cliff coverage is not one actual model and one visible ground cell");
    std::size_t visibleIndices=0;
    for (const auto& mesh:p.cliffs.meshes) {
        Check(!mesh.vertices.empty() && !mesh.indices.empty(),"cliff-ground empty visible cliff mesh"); visibleIndices+=mesh.indices.size();
    }
    Check(visibleIndices>0,"cliff-ground covered flag lacks actual visible geometry"); return p;
}
void ValidateCliffModel(const MdxModel& model,std::string_view texturePath) {
    Check(model.version==800 && !model.geosets.empty() && !model.skippedChunks && model.globalSequences.empty() &&
        model.textureAnimations.empty() && model.textureAnimationIssue.empty() && model.otherNodes.empty() && model.lights.empty(),
        "cliff-ground actual MDX format unsupported");
    ForEachMdxNode(model,[](const MdxNode& node) {
        Check(!node.translation && !node.rotation && !node.scaling &&
            (node.flags & ~MdxNodeNonTransformFlags(node.kind))==0,
            "cliff-ground static Bake cannot silently ignore a source node transform");
    });
    std::size_t visible=0;
    for (const auto& geoset:model.geosets) if (geoset.visibleInStand && !geoset.indices.empty()) {
        ++visible; Check(geoset.materialId<model.materials.size(),"cliff-ground visible material missing");
        const auto& material=model.materials[geoset.materialId];
        Check(material.layers.size()==1 && material.priorityPlane==0 && !material.flags && !material.hasUninterpretedData &&
            !geoset.animation && !geoset.skippedPrimitiveGroups && !geoset.uninterpretedTailBytes,
            "cliff-ground Bake cannot faithfully accept additional visible material/geoset semantics");
        const auto& layer=material.layers.front(); Check(layer.textureId<model.textures.size(),"cliff-ground source TEXS missing");
        const auto& texture=model.textures[layer.textureId];
        Check(layer.filterMode==MdxFilterMode::None && layer.shadingFlags==0 && layer.alpha==1 && texture.flags<=3 &&
            !layer.animatedTexture && !layer.animatedAlpha && !layer.hasUninterpretedData &&
            layer.textureAnimationId==UINT32_MAX && layer.coordId==0 &&
            (texture.replaceableId==11 || (texture.replaceableId==0 && VirtualPath(texture.path)==texturePath)),
            "cliff-ground unsupported original material must not be flattened by Bake");
        Check(geoset.normals.size()==geoset.positions.size() && geoset.uvs.size()==geoset.positions.size(),
            "cliff-ground requires actual source UV and normals");
    }
    Check(visible>0,"cliff-ground source has no Stand-visible triangles");
}
std::string ExpectedCliffPath(const CliffContext& c,const CliffTypeSource& type) {
    const auto& v=c.core.vertices;
    const auto shape=CliffShapeName(v[2].CliffLevel(),v[4].CliffLevel(),v[5].CliffLevel(),v[3].CliffLevel());
    Check(shape && v[2].CliffVariation()==0,"cliff-ground requires exact source variation0 without fallback search");
    return "Doodads\\Terrain\\"+type.modelDirectory+"\\"+type.modelDirectory+*shape+"0.mdx";
}
void CheckGroundGeometry(const TerrainGroundMesh& ground,const CliffPrepared& p) {
    Check(ground.hiddenTileCount==1 && ground.texturedTiles==1 && !ground.fallback.totalTiles &&
        ground.baseTiles==std::vector<std::uint32_t>({0}) && ground.vertices.size()>=4 && !ground.indices.empty() &&
        ground.waterTiles.empty() && ground.budget.policy==TerrainGroundBudgetPolicy::Strict && !ground.budget.reduced,
        "cliff-ground actual ground must preserve one textured cell and hide only the placed cliff");
    Check(ground.indices.size()%6==0 && ground.overlayTiles.size()+1==ground.vertices.size()/4,
        "cliff-ground output layer mapping invalid");
    for (const auto tile:ground.overlayTiles) Check(tile==0,"cliff-ground unexpected overlay on hidden cliff");
    for (const auto& batch:ground.batches) {
        Check(!batch.translucent && batch.textureIndex<ground.textures.size() && batch.indexCount%6==0,
            "cliff-ground untextured/water/fallback batch denied");
        for (std::uint32_t at=batch.firstIndex;at<batch.firstIndex+batch.indexCount;at+=6) {
            const auto first=ground.indices.at(at); Check(first%4==0 && first+3<ground.vertices.size(),"cliff-ground quad shape invalid");
            const std::array<std::uint32_t,6> expected{first,first+1,first+2,first+1,first+3,first+2};
            Check(std::equal(expected.begin(),expected.end(),ground.indices.begin()+at),"cliff-ground original diagonal changed");
            for (unsigned i=0;i<4;++i) {
                const auto& output=ground.vertices[first+i];
                Check(std::array<float,3>{output.x,output.y,output.z}==p.core.vertices[p.core.tiles[0].vertices[i]].position,
                    "cliff-ground actual ground XYZ is not the shaped source corner");
            }
        }
    }
}
Json CliffBlockFixture() {
    const auto original=Fixture(); Json block;
    block["ok"]=true; block["schema_version"]=1; block["is_render_capture"]=false;
    block["workspace_kind"]="map-archive-readonly";block["content_scope"]="committed-source";
    block["coordinate_system"]="southwest-origin-x-east-y-north-z-up";
    block["surface_semantics"]="w3e-raw-layer-height-grid"; block["preview_diagonal"]="northwest-southeast";
    block["includes_cliff_ramp_geometry"]=false; block["header"]=original.at("header");
    block["includes_water_material_offset"]=false;block["includes_entities"]=false;block["includes_pathing"]=false;
    for (unsigned i=0;i<16;++i) {
        // 唯一rawcode使正式GroundSelection必须真正找到第3项，不能把私有地图palette写死。
        std::string code="41414141"; code[7]="0123456789abcdef"[i]; block["header"]["ground_rawcodes_hex"][i]=code;
    }
    block["tiles"]={{"x",1},{"y",1},{"width",3},{"height",4}};
    block["vertices"]=Json::array(); block["cells"]=Json::array();
    for (unsigned y=1;y<=5;++y) for (unsigned x=1;x<=4;++x) {
        auto row=original.at("halo_vertices")[0]; const unsigned level=y>=4?3:2,material=y>=3?1:3;
        row["x"]=x; row["y"]=y; row["source_index"]=y*9+x;
        row["position_world"]=Json::array({-128+int(x)*128,-256+int(y)*128,int(level-2)*128});
        row["cliff_and_layer_raw"]=16+level; row["cliff_level"]=level;
        row["ground_texture_index"]=material; row["texture_and_flags_raw"]=material;
        block["vertices"].push_back(row);
    }
    for (unsigned y=1;y<5;++y) for (unsigned x=1;x<4;++x) {
        const auto sw=y*9+x; block["cells"].push_back({{"x",x},{"y",y},{"source_index",y*8+x},
            {"source_vertices_sw_se_nw_ne",Json::array({sw,sw+1,sw+9,sw+10})}});
    }
    return block;
}
std::vector<std::byte> CliffMdxFixture() {
    // 原创单层可见墙面，正式ParseMdx/BuildTerrainCliffs读取；没有私有Bake测试后门。
    MdxTest::Writer w; w.Tag("MDLX"); w.Tag("VERS"); w.U32(4); w.U32(800);
    w.Tag("TEXS"); w.U32(268); w.U32(11); w.Fixed("",260); w.U32(0);
    w.Tag("MTLS"); {
        const auto chunk=w.Begin(),material=w.Begin(); w.U32(0); w.U32(0); w.Tag("LAYS"); w.U32(1);
        const auto layer=w.Begin(); w.U32(0); w.U32(0); w.U32(0); w.U32(UINT32_MAX); w.U32(0); w.F32(1);
        w.EndInclusive(layer); w.EndInclusive(material); w.EndChunk(chunk);
    }
    w.Tag("GEOS"); {
        const auto chunk=w.Begin(),item=w.Begin(); w.Tag("VRTX"); w.U32(3);
        for (const auto& p:std::array<std::array<float,3>,3>{{{-128,0,0},{0,0,0},{-128,128,128}}}) for (float v:p) w.F32(v);
        w.Tag("NRMS"); w.U32(3); for (int i=0;i<3;++i) {w.F32(0);w.F32(-1);w.F32(0);}
        w.Tag("PTYP"); w.U32(1); w.U32(4); w.Tag("PCNT"); w.U32(1); w.U32(3);
        w.Tag("PVTX"); w.U32(3); w.U16(0); w.U16(1); w.U16(2);
        w.Tag("GNDX"); w.U32(3); w.Raw("\0\0\0",3);
        w.Tag("MTGC"); w.U32(1); w.U32(1); w.Tag("MATS"); w.U32(1); w.U32(0);
        w.U32(0);w.U32(0);w.U32(0); for (int i=0;i<7;++i) w.F32(0); w.U32(0);
        w.Tag("UVAS"); w.U32(1);w.Tag("UVBS");w.U32(3); for (int i=0;i<3;++i) {w.F32(float(i)/2);w.F32(float(i)/2);}
        w.EndInclusive(item); w.EndChunk(chunk);
    }
    w.Tag("BONE"); {
        const auto chunk=w.Begin(),node=w.Begin();w.Fixed("Original audit",80);w.U32(0);w.U32(UINT32_MAX);w.U32(0);
        w.EndInclusive(node);w.U32(UINT32_MAX);w.U32(UINT32_MAX);w.EndChunk(chunk);
    }
    w.Tag("PIVT");w.U32(12);w.F32(0);w.F32(0);w.F32(0); return std::move(w.bytes);
}
const char* CliffMipStopName(Image::BlpMipChainStopReason reason) {
    using Stop = Image::BlpMipChainStopReason;
    switch (reason) {
    case Stop::Complete: return "Complete";
    case Stop::UndeclaredMip: return "UndeclaredMip";
    case Stop::InvalidMipRange: return "InvalidMipRange";
    case Stop::TotalPixelBudget: return "TotalPixelBudget";
    case Stop::RetainedRgbaBudget: return "RetainedRgbaBudget";
    case Stop::WorkingBudget: return "WorkingBudget";
    case Stop::JpegBudget: return "JpegBudget";
    case Stop::DecodeFailure: return "DecodeFailure";
    }
    Check(false,"cliff-ground unknown model mip stop reason");return "Unknown";
}
Json CliffTextureMipFacts(const WorldModelScene& scene,const ModelSceneDescriptor& descriptor) {
    const auto* submitted=descriptor.StaticScene();
    Check(scene.textures.size()==1 && submitted && submitted->textureCount==scene.textures.size()
        && descriptor.chains.size()==scene.textures.size() && submitted->textures==descriptor.chains.data(),
        "cliff-ground submitted texture array does not match its descriptor owner/scene");
    const auto& texture=scene.textures[0];
    Check(ModelTextureValid(texture,WorldModelLimits{}.maxTextureDimension),"cliff-ground owned model mip chain invalid");
    const auto retained=texture.additionalMips.size()+1;
    const auto& chain=submitted->textures[0];
    const bool stableInterval=chain.mips==descriptor.mips.data() && descriptor.mips.size()==retained;
    Check(chain.structSize==sizeof(chain) && chain.mipCount==retained && chain.maxSampleMip==retained-1
        && chain.atlasColumns==1 && chain.atlasRows==1 && stableInterval,
        "cliff-ground submitted mip interval/count/LOD does not match the owned source chain");
    Json levels=Json::array(),diagnostics=Json::array();std::uint64_t descriptorRgbaBytes=0;
    bool pointersMatch=true,metadataMatch=true;
    for (std::size_t level=0;level<retained;++level) {
        const auto bytes=level==0 ? std::as_bytes(std::span(texture.rgba))
            : std::span<const std::byte>(texture.additionalMips[level-1].rgba);
        const auto width=level==0 ? texture.width : texture.additionalMips[level-1].width;
        const auto height=level==0 ? texture.height : texture.additionalMips[level-1].height;
        const auto& mip=chain.mips[level];
        const bool pointer=static_cast<const void*>(mip.rgba)==static_cast<const void*>(bytes.data());
        const bool metadata=mip.structSize==sizeof(mip) && mip.width==width && mip.height==height
            && mip.rgbaByteCount==bytes.size();
        Check(pointer && metadata,"cliff-ground submitted mip does not refer to the actual owned RGBA level");
        Check(mip.rgbaByteCount<=UINT64_MAX-descriptorRgbaBytes,"cliff-ground descriptor RGBA sum overflow");
        descriptorRgbaBytes+=mip.rgbaByteCount;pointersMatch&=pointer;metadataMatch&=metadata;
        levels.push_back({{"level",level},{"width",width},{"height",height},{"rgba_bytes",bytes.size()},
            {"rgba_sha256",Digest(bytes)}});
    }
    const auto rgbaBytes=ModelTextureRgbaBytes(texture),storageBytes=ModelTextureStorageBytes(texture);
    Check(rgbaBytes!=UINT64_MAX && storageBytes!=UINT64_MAX && descriptorRgbaBytes==rgbaBytes,
        "cliff-ground submitted/owned full-chain RGBA totals differ");
    for (const auto& diagnostic:texture.mipDiagnostics)
        diagnostics.push_back({{"severity",static_cast<int>(diagnostic.severity)},
            {"subsystem",diagnostic.subsystem},{"code",diagnostic.code}});
    // 这些是CPU拥有字节与即将提交的同一个描述符事实；没有逐级GPU读回或像素采样声明。
    return {{"schema","cliff-model-texture-mips-v1"},
        {"scope","cpu-owned-mips-and-submitted-host-descriptors; no per-mip GPU readback"},
        {"origin",texture.mipOrigin==ModelTextureMipOrigin::BlpSource ? "blp-source" : "generated-from-top"},
        {"policy_version",texture.mipPolicyVersion},{"source_width",texture.sourceWidth},{"source_height",texture.sourceHeight},
        {"source_base_mip",texture.sourceBaseMip},{"stop",{{"reason",CliffMipStopName(texture.mipStopReason)},
            {"level",texture.mipStopLevel},{"has_later_declarations",texture.mipHasLaterDeclarations}}},
        {"diagnostics",diagnostics},{"retained_mip_count",retained},{"rgba_bytes",rgbaBytes},
        {"owned_dynamic_storage_bytes",storageBytes},{"levels",levels},
        {"submitted_descriptor",{{"mip_count",chain.mipCount},{"max_sample_mip",chain.maxSampleMip},
            {"atlas_columns",chain.atlasColumns},{"atlas_rows",chain.atlasRows},{"rgba_bytes",descriptorRgbaBytes},
            {"stable_interval_matches_owner",stableInterval},{"rgba_pointers_match_scene",pointersMatch},
            {"level_metadata_match_scene",metadataMatch},{"rgba_total_matches_scene",descriptorRgbaBytes==rgbaBytes}}}};
}
} // namespace

void RunRealCliffGroundValidationTests() {
    const auto selection=Json{{"ground_cell",{2,2}},{"cliff_cell",{2,3}}};
    const auto original=CliffBlockFixture(); const auto context=CliffContextFromBlock(original,selection);
    Check(context.halo.vertices.size()==20 && context.core.vertices.size()==6 && context.core.tiles.size()==2 &&
        context.sourceVertices==std::array<std::uint32_t,6>{20,21,29,30,38,39},"cliff-ground original source mapping failed");
    const auto reject=[](const auto& action) {bool caught=false;try {action();}catch(const std::exception&){caught=true;}
        Check(caught,"cliff-ground malformed contract accepted");};
    WorldModelScene mipScene;ModelTextureRgba mipTexture;
    mipTexture.width=mipTexture.height=mipTexture.sourceWidth=mipTexture.sourceHeight=2;
    mipTexture.rgba.assign(16,255);mipTexture.additionalMips.push_back({1,1,std::vector<std::byte>(4,std::byte{127})});
    mipTexture.mipOrigin=ModelTextureMipOrigin::BlpSource;mipTexture.mipPolicyVersion=ModelTextureMipPolicyVersion;
    mipTexture.mipStopLevel=2;mipScene.textures.push_back(std::move(mipTexture));
    auto mipDescriptor=DescribeModelScene(mipScene);const auto mipFacts=CliffTextureMipFacts(mipScene,mipDescriptor);
    Check(mipFacts.at("retained_mip_count")==2 && mipFacts.at("rgba_bytes")==20
        && mipFacts.at("submitted_descriptor").at("rgba_total_matches_scene")==true,
        "cliff-ground facts omitted owned source levels or descriptor bytes");
    mipDescriptor.mips[1].rgba=mipScene.textures[0].rgba.data();
    reject([&]{CliffTextureMipFacts(mipScene,mipDescriptor);}); // 同尺寸/字节的错误像素指针也必须拒绝。
    Check(RelativePathText("inputs/original.bin")==std::filesystem::path("inputs/original.bin"),"cliff-ground relative input fixture failed");
    for (const auto* path:{"../original.bin","inputs/../original.bin","C:/original.bin","//host/original.bin",
        "inputs\\original.bin","inputs//original.bin","inputs./original.bin","inputs /original.bin","original.w3x","original.bin/"})
        reject([&]{(void)RelativePathText(path);});
    reject([&]{(void)RelativePathText(std::string("original\0.bin",13));});
    for (const auto* path:{"../Original.blp","Original//Audit.blp","Original/Audit.blp/","Original./Audit.blp"})
        reject([&]{(void)VirtualPath(path);});
    for (const auto field:{"is_ramp","has_water","is_boundary","is_blight","is_map_edge"}) {
        auto changed=original;changed["vertices"][5][field]=true;reject([&]{(void)CliffContextFromBlock(changed,selection);});
    }
    auto rawRamp=original;rawRamp["vertices"][5]["texture_and_flags_raw"]=UInt(rawRamp["vertices"][5]["ground_texture_index"])|0x10u;
    rawRamp["vertices"][5]["is_ramp"]=true;reject([&]{(void)CliffContextFromBlock(rawRamp,selection);});
    auto changed=original;changed["vertices"][5]["source_index"]=999;reject([&]{(void)CliffContextFromBlock(changed,selection);});
    changed=original;changed["vertices"][5]=changed["vertices"][6];reject([&]{(void)CliffContextFromBlock(changed,selection);});
    changed=original;changed["cells"][4]["source_vertices_sw_se_nw_ne"][2]=0;reject([&]{(void)CliffContextFromBlock(changed,selection);});
    changed=original;changed["vertices"].erase(3);reject([&]{(void)CliffContextFromBlock(changed,selection);});
    auto moved=selection;moved["cliff_cell"]={3,2};reject([&]{(void)CliffContextFromBlock(original,moved);});
    const std::vector<std::optional<CliffTypeSource>> types{std::nullopt,
        CliffTypeSource{"AuditCliffs","Original\\Audit.blp","AuditRamp",context.original.groundTilesets[3],""}};
    const std::string slk="ID;PWXL;N;E\nB;X7;Y2;D0\nC;X1;Y1;K\"cliffID\"\nC;X2;K\"cliffModelDir\"\n"
        "C;X3;K\"rampModelDir\"\nC;X4;K\"texDir\"\nC;X5;K\"texFile\"\nC;X6;K\"groundTile\"\nC;X7;K\"upperTile\"\n"
        "C;X1;Y2;K\"EFGH\"\nC;X2;K\"AuditCliffs\"\nC;X3;K\"AuditRamp\"\nC;X4;K\"Original\"\n"
        "C;X5;K\"Audit\"\nC;X6;K\"AAAC\"\nC;X7;K\"_\"\nE\n";
    const auto table=CaptureCliffTable(std::as_bytes(std::span(slk)));Json rows;
    const auto parsedTypes=CliffTypesFromTable(context.original,table,rows);
    Check(parsedTypes.size()==2 && !parsedTypes[0] && parsedTypes[1] && parsedTypes[1]->modelDirectory==types[1]->modelDirectory &&
        parsedTypes[1]->texturePath==types[1]->texturePath && parsedTypes[1]->groundTile==types[1]->groundTile && rows.size()==1,
        "cliff-ground official source table/raw palette binding failed");
    auto unsafeSlk=slk;unsafeSlk.replace(unsafeSlk.find("AuditCliffs"),11,"../Escape");
    reject([&]{const auto unsafeTable=CaptureCliffTable(std::as_bytes(std::span(unsafeSlk)));Json unsafeRows;
        (void)CliffTypesFromTable(context.original,unsafeTable,unsafeRows);});
    const auto bytes=CliffMdxFixture();const auto parsed=ParseMdx(bytes);Check(parsed.Succeeded(),"cliff-ground original MDX fixture invalid");
    ValidateCliffModel(*parsed.value,types[1]->texturePath);
    unsigned reads=0;const auto path=ExpectedCliffPath(context,*types[1]);
    const ModelFileReader reader=[&](std::string_view requested) {
        ++reads;Check(requested==path,"cliff-ground reader guessed another shape or source order");
        return Result<std::vector<std::byte>>::Success(bytes);
    };
    const auto prepared=PrepareCliffGround(context,types,reader);
    Check(reads==1 && prepared.rampAdjusted==0 && prepared.selected==std::vector<std::uint8_t>({3,3,3,3,3,3}) &&
        context.core.vertices[2].GroundTextureIndex()==1,"cliff-ground production derived indices were not separated from raw");
    const auto& contact=prepared.cliffs.meshes.front().vertices;
    Check(std::array<float,3>{contact[0].x,contact[0].y,contact[0].z}==prepared.core.vertices[2].position &&
        std::array<float,3>{contact[1].x,contact[1].y,contact[1].z}==prepared.core.vertices[3].position &&
        contact[0].u==parsed.value->geosets[0].uvs[0][0] && contact[1].v==parsed.value->geosets[0].uvs[1][1],
        "cliff-ground original wall contact/UV did not follow the actual source geometry");
    auto atlas=std::make_shared<TerrainAtlasSnapshot>();atlas->columns=4;atlas->sourceWidth=atlas->sourceHeight=16;
    atlas->image={16,16,std::vector<std::byte>(16*16*4,std::byte{255})};atlas->validatedMipCount=1;
    for (unsigned y=0;y<4;++y)for(unsigned x=0;x<4;++x)atlas->cells.push_back({x,y,x*4,y*4,4,4});
    std::vector<std::shared_ptr<const TerrainAtlasSnapshot>> atlases(16);atlases[3]=atlas;
    TerrainLighting lighting;lighting.enabled=false;
    const auto ground=BuildTerrainGroundMesh(prepared.core,atlases,{},lighting,nullptr,prepared.cliffs.coveredTiles,{},prepared.selected);
    Check(ground.Succeeded(),"cliff-ground production original ground fixture rejected");CheckGroundGeometry(*ground.value,prepared);
    auto emptyTypes=types;emptyTypes[1].reset();reject([&]{(void)PrepareCliffGround(context,emptyTypes,reader);});
    reject([&]{(void)PrepareCliffGround(context,types,[](std::string_view){return Result<std::vector<std::byte>>::Failure("Audit","MISSING","missing");});});
    auto unsupported=*parsed.value;unsupported.materials[0].layers.push_back(unsupported.materials[0].layers[0]);
    reject([&]{ValidateCliffModel(unsupported,types[1]->texturePath);});
    auto typedBone=*parsed.value;typedBone.bones[0].flags=0x100;
    ValidateCliffModel(typedBone,types[1]->texturePath); // 正式元数据中Bone类型位不表示变换。
    typedBone.bones[0].flags|=1;reject([&]{ValidateCliffModel(typedBone,types[1]->texturePath);});
    TerrainGroundMeshLimits budget;budget.maxVertices=3;
    Check(!BuildTerrainGroundMesh(prepared.core,atlases,budget,lighting,nullptr,prepared.cliffs.coveredTiles,{},prepared.selected).Succeeded(),
        "cliff-ground original production vertex budget was bypassed");
}

void RunRealCliffGroundAudit(const std::filesystem::path& manifestPath,std::string_view manifestSha,
    const std::filesystem::path& output,const RealCliffGroundCapture& capture,const RealTerrainPairSaveBmp& saveBmp) {
    RunRealCliffGroundValidationTests();
    Check(manifestPath.is_absolute() && std::filesystem::is_regular_file(manifestPath),"cliff-ground manifest must be an absolute regular file");
    const auto manifest=ReadJson(manifestPath,manifestSha);const auto root=std::filesystem::canonical(manifestPath.parent_path());
    Check(manifest.at("schema")=="terrain-cliff-ground-audit-v1" && manifest.at("environment")=="neutral-no-shadow-no-fog",
        "cliff-ground manifest schema/environment unsupported");
    const auto provenance=manifest.at("source_scope").get<std::string>();
    Check(!provenance.empty() && provenance.size()<=4096,"cliff-ground source scope must be explicit");
    Check(output.is_absolute() && std::filesystem::weakly_canonical(output)!=root && (!std::filesystem::exists(output) ||
        (!std::filesystem::is_symlink(std::filesystem::symlink_status(output)) && std::filesystem::is_directory(output) &&
        std::filesystem::is_empty(output))),"cliff-ground output must be a new/empty directory distinct from inputs");
    const auto block=RelativeJson(root,manifest.at("source_block"));
    const auto& binding=manifest.at("terrain_binding");
    Check(binding==block.at("terrain_binding") && IsSha(binding.at("source_sha256").get<std::string>()) &&
        IsSha(binding.at("terrain_sha256").get<std::string>()) && !binding.at("document_id").get<std::string>().empty(),
        "cliff-ground map/member source binding mismatch");
    const auto expectedProvider=manifest.at("expected_provider_sha256").get<std::string>();
    Check(IsSha(expectedProvider),"cliff-ground expected provider SHA invalid");
    const auto revisionText=binding.at("revision").get<std::string>();
    Check(!revisionText.empty() && revisionText.size()<=19 && std::all_of(revisionText.begin(),revisionText.end(),[](char c){return c>='0'&&c<='9';}),
        "cliff-ground revision invalid");const auto revision=std::stoull(revisionText);
    const auto context=CliffContextFromBlock(block,manifest.at("selection"));

    // 只给正式TerrainTable提供manifest已核字节，没有目录/game/地图Resolver来源。
    const auto tableBytes=RelativeBytes(root,manifest.at("cliff_table"),2*1024*1024,".slk");
    const auto table=CaptureCliffTable(tableBytes);
    Check(table.SourceSha256()==manifest.at("cliff_table").at("sha256").get<std::string>(),"cliff-ground official table SHA failed");
    Json sourceRows;const auto types=CliffTypesFromTable(context.original,table,sourceRows);
    Check(types[context.type].has_value(),"cliff-ground actual palette row missing from parsed source table");
    const auto& type=*types[context.type];const auto expectedModel=ExpectedCliffPath(context,type);
    Check(VirtualPath(manifest.at("cliff_model").at("virtual_path").get<std::string>())==expectedModel &&
        VirtualPath(manifest.at("cliff_texture").at("virtual_path").get<std::string>())==type.texturePath,
        "cliff-ground model/texture does not match actual source row, shape and variation");
    const auto modelBytes=RelativeBytes(root,manifest.at("cliff_model"),16*1024*1024,".mdx");
    const auto cliffBytes=RelativeBytes(root,manifest.at("cliff_texture"),16*1024*1024,".blp");
    const auto model=ParseMdx(modelBytes);Check(model.Succeeded(),"cliff-ground actual MDX parse failed");NoWarnings(model.diagnostics);
    ValidateCliffModel(*model.value,type.texturePath);
    unsigned modelReads=0;
    const ModelFileReader modelReader=[&](std::string_view requested) {
        Check(requested==expectedModel,"cliff-ground undeclared model/fallback read denied");++modelReads;
        return Result<std::vector<std::byte>>::Success(modelBytes);
    };
    const auto prepared=PrepareCliffGround(context,types,modelReader);Check(modelReads==1,"cliff-ground expected exactly one source model read");
    std::set<std::uint32_t> requiredGround;
    for (const auto at:prepared.core.tiles[0].vertices) requiredGround.insert(prepared.selected[at]);
    Check(manifest.at("ground_atlases").is_array() && manifest.at("ground_atlases").size()==requiredGround.size() &&
        requiredGround.size()<=4,"cliff-ground manifest must supply precisely actual visible ground dependencies");
    std::vector<std::shared_ptr<const TerrainAtlasSnapshot>> atlases(context.original.groundTilesets.size());
    Json resources=Json::array();std::set<std::uint32_t> loaded;
    std::size_t retainedAtlasBytes=0;
    for (const auto& entry:manifest.at("ground_atlases")) {
        const auto palette=UInt(entry.at("palette_index"));
        Check(requiredGround.contains(palette) && loaded.insert(palette).second && palette<atlases.size() &&
            Rawcode(entry.at("rawcode_hex"))==context.original.groundTilesets[palette],"cliff-ground atlas raw palette mismatch");
        Resources::ResolvedResource resource;
        resource.location={VirtualPath(entry.at("virtual_path").get<std::string>()),Resources::ResourceLayer::Other,0,
            "verified-audit-ground",entry.at("path").get<std::string>()};
        resource.bytes=RelativeBytes(root,entry,16*1024*1024,".blp");
        const auto decoded=DecodeTerrainTextureAtlas(resource);Check(decoded.Succeeded(),"cliff-ground production atlas decode failed");
        NoWarnings(decoded.diagnostics);atlases[palette]=*decoded.value;
        Check(atlases[palette]->sourceSha256==entry.at("sha256").get<std::string>() &&
            atlases[palette]->columns==UInt(entry.at("columns")),"cliff-ground actual atlas source/layout mismatch");
        const auto contract=ValidateMipContract(*atlases[palette],entry.at("audited_mips"));
        Check(atlases[palette]->RgbaByteCount()<=64*1024*1024-retainedAtlasBytes,"cliff-ground retained atlas aggregate budget exceeded");
        retainedAtlasBytes+=atlases[palette]->RgbaByteCount();
        resources.push_back({{"palette_index",palette},{"input",entry},{"audited_mips",contract.audited},
            {"retained_mips",contract.retained},{"max_sample_mip",contract.maxSampleMip}});
    }
    TerrainLighting lighting;lighting.enabled=false;
    const auto built=BuildTerrainGroundMesh(prepared.core,atlases,{},lighting,nullptr,prepared.cliffs.coveredTiles,{},prepared.selected);
    Check(built.Succeeded(),"cliff-ground production ground build failed");NoWarnings(built.diagnostics);
    const auto& ground=*built.value;CheckGroundGeometry(ground,prepared);
    // 此副本只供独立UV/mask检查，生产输入及回执始终保留原raw字段。
    Context layout;layout.terrain=prepared.core;
    for (std::size_t i=0;i<layout.terrain.vertices.size();++i)
        layout.terrain.vertices[i].textureAndFlagsRaw=prepared.selected[i];
    Json groundLayers=Json::array();std::set<std::uint32_t> emitted;bool overlayStarted=false;std::uint32_t previousOverlay=0;
    for (const auto& batch:ground.batches) {
        std::optional<std::uint32_t> palette;
        for (const auto index:requiredGround) if(atlases[index]==ground.textures.at(batch.textureIndex)) {
            Check(!palette,"cliff-ground ambiguous palette/atlas identity");palette=index;
        }
        Check(palette.has_value(),"cliff-ground emitted an undeclared atlas");
        for (std::uint32_t at=batch.firstIndex;at<batch.firstIndex+batch.indexCount;at+=6) {
            const auto first=ground.indices.at(at);
            const auto checked=ValidateLayerLayout(layout,0,*palette,batch.alphaBlend,*atlases[*palette],
                std::span<const TerrainGroundVertex>(ground.vertices).subspan(first,4));
            Check(emitted.insert(*palette).second,"cliff-ground duplicate actual ground layer");
            if(batch.alphaBlend) {Check(!overlayStarted||*palette>=previousOverlay,"cliff-ground overlay ordering changed");overlayStarted=true;previousOverlay=*palette;}
            else Check(!overlayStarted && *palette==*requiredGround.begin(),"cliff-ground original base layer ordering changed");
            Json uvs=Json::array();for(unsigned i=0;i<4;++i)uvs.push_back({ground.vertices[first+i].u,ground.vertices[first+i].v});
            groundLayers.push_back({{"source_tile",context.sourceCells[0].at("source_index")},{"palette_index",*palette},
                {"alpha_blend",batch.alphaBlend},{"exact_membership_mask",checked.mask},{"cell",checked.cell},{"uv_sw_se_nw_ne",uvs}});
        }
    }
    Check(emitted==requiredGround,"cliff-ground production omitted an actual derived layer");
    const ModelFileReader textureReader=[&](std::string_view requested) {
        Check(VirtualPath(std::string(requested))==type.texturePath,"cliff-ground undeclared material resource read denied");
        return Result<std::vector<std::byte>>::Success(cliffBytes);
    };
    const auto models=BuildWorldModelScene({},textureReader,{},prepared.cliffs.meshes);
    Check(models.Succeeded(),"cliff-ground production baked model scene failed");NoWarnings(models.diagnostics);NoWarnings(models.value->warnings);
    Check(models.value->models.size()==1 && models.value->instances.size()==1 && !models.value->untexturedLayers &&
        models.value->textures.size()==1 && !models.value->indices.empty(),"cliff-ground incomplete model material/geometry");
    const auto descriptor=DescribeModelScene(*models.value);Check(descriptor.StaticScene(),"cliff-ground source sampler descriptor invalid");
    const auto cliffMipFacts=CliffTextureMipFacts(*models.value,descriptor);
    for(std::size_t i=0;i<models.value->layers.size();++i) {
        const auto flags=models.value->layerSamplerFlags.empty()?3u:models.value->layerSamplerFlags[i];
        Check(flags==prepared.cliffs.meshes.at(i).samplerFlags,"cliff-ground baked source sampler transport changed");
    }
    std::vector<War3RenderTerrainVertexV2> vertices;vertices.reserve(ground.vertices.size());
    for(const auto& v:ground.vertices)vertices.push_back({v.x,v.y,v.z,v.red,v.green,v.blue,v.u,v.v});
    std::vector<std::vector<War3RenderMipRgbaV2>> mipLists(ground.textures.size());std::vector<War3RenderTextureMipChainV2> chains;
    for(std::size_t i=0;i<ground.textures.size();++i) {
        const auto& atlas=*ground.textures[i];auto& list=mipLists[i];
        const auto append=[&](const Image::DecodedImage& mip){list.push_back({sizeof(War3RenderMipRgbaV2),mip.width,mip.height,mip.rgba.size(),mip.rgba.data()});};
        append(atlas.image);for(const auto& mip:atlas.additionalMips)append(mip);
        chains.push_back({sizeof(War3RenderTextureMipChainV2),static_cast<std::uint32_t>(list.size()),HostMaxSampleMip(atlas),
            atlas.columns,atlas.rows,list.data()});
    }
    std::vector<War3RenderTerrainBatchV2> batches;
    for(const auto& batch:ground.batches)batches.push_back({batch.firstIndex,batch.indexCount,
        batch.textureIndex|(batch.alphaBlend?WAR3_RENDER_TERRAIN_ALPHA_BLEND:0u)});
    const War3RenderTerrainMipSceneV2 scene{sizeof(scene),static_cast<std::uint32_t>(vertices.size()),static_cast<std::uint32_t>(ground.indices.size()),
        static_cast<std::uint32_t>(chains.size()),static_cast<std::uint32_t>(batches.size()),revision,
        vertices.data(),ground.indices.data(),chains.data(),batches.data()};
    const auto& cameraInput=manifest.at("camera");const auto target=Triple(cameraInput.at("target_world"));
    const auto width=UInt(cameraInput.at("width"),1024),height=UInt(cameraInput.at("height"),1024);
    Check(width>=64 && height>=64,"cliff-ground viewport qualification invalid");
    const War3RenderCameraV2 camera{sizeof(camera),target[0],target[1],target[2],Number(cameraInput.at("distance")),
        Number(cameraInput.at("yaw_radians")),Number(cameraInput.at("pitch_radians"))};
    Check(camera.distance>0 && camera.pitchRadians>0 && camera.pitchRadians<1.5707964f,"cliff-ground camera invalid");
    if(!std::filesystem::exists(output))Check(std::filesystem::create_directory(output),"cliff-ground output create failed");
    const auto frame=capture(scene,*descriptor.StaticScene(),camera,width,height);const auto& receipt=frame.receipt;
    Check(frame.providerSha256==expectedProvider && frame.pixels.size()==std::size_t(width)*height*4 &&
        frame.status.result==0 && frame.status.deviceReady==1 && frame.status.software==1 &&
        frame.status.documentRevision==revision && frame.status.frameCount>=receipt.capture.frameCount &&
        receipt.capture.width==width && receipt.capture.height==height && receipt.capture.documentRevision==revision &&
        receipt.view.documentRevision==revision && receipt.capture.ticket && receipt.capture.frameCount &&
        receipt.view.frameCount==receipt.capture.frameCount && receipt.view.surfaceEpoch && receipt.view.sceneEpoch && receipt.view.viewEpoch &&
        receipt.view.width==width && receipt.view.height==height && receipt.camera.targetX==camera.targetX && receipt.camera.targetY==camera.targetY &&
        receipt.camera.targetZ==camera.targetZ && receipt.camera.distance==camera.distance && receipt.camera.yawRadians==camera.yawRadians &&
        receipt.camera.pitchRadians==camera.pitchRadians,"cliff-ground atomic two-packet completed frame identity failed");
    for(float value:receipt.view.viewProjection)Check(std::isfinite(value),"cliff-ground completed MVP nonfinite");
    DirectX::XMFLOAT4X4 completedMatrix;std::memcpy(&completedMatrix,receipt.view.viewProjection,sizeof(completedMatrix));
    const auto matrix=DirectX::XMLoadFloat4x4(&completedMatrix);
    const auto determinant=DirectX::XMMatrixDeterminant(matrix);
    Check(std::isfinite(DirectX::XMVectorGetX(determinant)) && DirectX::XMVectorGetX(determinant)!=0,
        "cliff-ground completed MVP singular");
    Json core=Json::array();for(std::size_t i=0;i<6;++i)core.push_back({{"source_index",context.sourceVertices[i]},
        {"raw_ground_index",context.core.vertices[i].GroundTextureIndex()},{"derived_ground_index",prepared.selected[i]},
        {"raw_xyz",context.core.vertices[i].position},{"shaped_xyz",prepared.core.vertices[i].position}});
    Json cliffMeshes=Json::array();
    for(const auto& mesh:prepared.cliffs.meshes)cliffMeshes.push_back({{"texture_path",mesh.texturePath},{"source_sampler_flags",mesh.samplerFlags},
        {"vertex_count",mesh.vertices.size()},{"index_count",mesh.indices.size()},
        {"vertices_sha256",Digest(std::as_bytes(std::span(mesh.vertices)))},{"indices_sha256",Digest(std::as_bytes(std::span(mesh.indices)))}});
    WriteBytes(output/"rgba.bin",frame.pixels);saveBmp(output/"frame.bmp",frame.pixels,width,height);
    const Json result{{"schema","terrain-cliff-ground-audit-result-v1"},{"completed",true},
        {"scope","one-source-footprint-and-adjacent-ground-warp-not-game-or-automatic-seam-acceptance"},
        {"manifest_sha256",manifestSha},{"source_scope",provenance},{"terrain_binding",binding},{"source_block",manifest.at("source_block")},
        {"provider_sha256",frame.providerSha256},{"winning_archive_verified_by_this_entry",false},
        {"selection",manifest.at("selection")},{"original_header",block.at("header")},{"source_cells",context.sourceCells},
        {"halo_source_vertices",context.haloSource},{"core_vertices",core},{"cliff_table",manifest.at("cliff_table")},
        {"official_table_identity_sha256",table.IdentitySha256()},{"cliff_rows",sourceRows},
        {"cliff_model",manifest.at("cliff_model")},{"cliff_texture",manifest.at("cliff_texture")},{"actual_model_path",expectedModel},
        {"ramp_adjusted_vertices",prepared.rampAdjusted},{"cliff_geometry",{{"covered_tiles",prepared.cliffs.coveredTiles},
            {"placed_tiles",prepared.cliffs.placedTiles},{"visible_meshes",cliffMeshes}}},
        {"ground_geometry",{{"textured_tiles",ground.texturedTiles},{"hidden_tiles",ground.hiddenTileCount},{"fallback_tiles",ground.fallback.totalTiles},
            {"base_tiles",ground.baseTiles},{"layers",groundLayers},{"vertices_sha256",Digest(std::as_bytes(std::span(ground.vertices)))},
            {"indices_sha256",Digest(std::as_bytes(std::span(ground.indices)))}}},{"resources",resources},
        {"cliff_decoded_mip0",{{"width",models.value->textures[0].width},{"height",models.value->textures[0].height},
            {"rgba_sha256",Digest(std::as_bytes(std::span(models.value->textures[0].rgba)))},
            {"mip_scope","CPU-decoded mip0; owned chain and submitted descriptor facts are in cliff_texture_mips; no per-mip GPU readback"}}},
        {"cliff_texture_mips",cliffMipFacts},
        {"camera",cameraInput},{"capture",{{"width",width},{"height",height},{"ticket",receipt.capture.ticket},{"frame_count",receipt.capture.frameCount},
            {"document_revision",revision},{"surface_epoch",receipt.view.surfaceEpoch},{"scene_epoch",receipt.view.sceneEpoch},{"view_epoch",receipt.view.viewEpoch},
            {"view_projection",std::vector<float>(std::begin(receipt.view.viewProjection),std::end(receipt.view.viewProjection))}}},
        {"files",{{"rgba",FileReceipt(output/"rgba.bin")},{"bmp",FileReceipt(output/"frame.bmp")}}},
        {"caveats",Json::array({"No map/W3E/game/YDWE resource reads; only SHA-bound relative manifest inputs.",
            "Table identity is the supplied captured source, not a claim of the map Resolver winning archive.",
            "Both production packets share one renderer/frame; actual geometry gaps and image quality require separate review.",
            "Neutral diffuse-zero lighting does not qualify relief-normal improvement or original game equivalence."})}};
    const auto text=result.dump(2)+"\n";WriteBytes(output/"result.json.tmp",std::span(reinterpret_cast<const std::uint8_t*>(text.data()),text.size()));
    std::filesystem::rename(output/"result.json.tmp",output/"result.json");
}

} // namespace War3Map::Tests
