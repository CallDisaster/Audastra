#include "TerrainGroundBrushRealTests.h"

#include "War3MapCore/Hash.h"
#include "War3MapCore/MapData/MapMinimapMarkers.h"
#include "War3MapCore/MapData/MapShadow.h"
#include "War3MapCore/MapData/TerrainCliffEditing.h"
#include "War3MapCore/Services/TerrainPathingSave.h"
#include "War3MapCore/Text/SlkCells.h"
#include "War3MapCore/Text/TextUtil.h"

#include <algorithm>
#include <array>
#include <iostream>
#include <iterator>
#include <map>
#include <stdexcept>

namespace War3Map::TerrainSaveTests {
namespace {
using namespace MapData;
using namespace Services;
constexpr std::size_t kBytes = 64u * 1024u * 1024u;
using Properties = std::array<bool, 3>;

void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
template<class T> T Take(Result<T> result) {
    if (result.Succeeded() && result.value) return std::move(*result.value);
    std::string message;
    for (const auto& item : result.diagnostics) message += item.code + ": " + item.message + "\n";
    throw std::runtime_error(message.empty() ? "missing real ground test result" : message);
}

std::map<std::string, Properties> GroundProperties(const Game::GameFileContents& file) {
    const auto text = std::string_view(reinterpret_cast<const char*>(file.bytes.data()), file.bytes.size());
    const auto cells = Take(Text::ParseSlkCells(text, Text::SlkCellMode::CatalogUnique));
    Require(!cells.empty(), "actual terrain table has a header");
    std::map<std::string, int> columns;
    for (const auto& [column, name] : cells.begin()->second)
        Require(columns.emplace(Text::LowerAscii(name), column).second, "terrain headers are unique");
    for (const auto* name : {"tileid", "walkable", "flyable", "buildable"})
        Require(columns.contains(name), "actual terrain table exposes all pathing columns");
    std::map<std::string, Properties> result;
    for (auto row = std::next(cells.begin()); row != cells.end(); ++row) {
        const auto id = row->second.find(columns.at("tileid"));
        if (id == row->second.end()) continue;
        Properties properties{};
        std::size_t index = 0;
        for (const auto* name : {"walkable", "flyable", "buildable"}) {
            const auto value = row->second.find(columns.at(name));
            Require(value != row->second.end() && (value->second == "0" || value->second == "1"),
                "actual terrain pathing fields are explicit booleans");
            properties[index++] = value->second == "1";
        }
        Require(result.emplace(id->second, properties).second, "actual terrain IDs are unique");
    }
    return result;
}

// 独立的局部干地字节预期，不调用产品通行生成器。样例只改地图中心，
// 这里不重做可玩区投影、水或对象规则；这些位置由夹具明确排除。
std::byte DryMask(const TerrainGridSnapshot& terrain, std::size_t px, std::size_t py,
    const std::map<std::string, Properties>& properties) {
    const auto columns = terrain.header.width;
    const auto tile = (py / 4) * columns + px / 4;
    const auto level = terrain.vertices[tile].CliffLevel();
    bool cliff = false;
    for (const auto offset : {std::size_t{0}, std::size_t{1}, std::size_t{columns}, std::size_t{columns + 1}}) {
        const auto& vertex = terrain.vertices[tile + offset];
        Require(!vertex.HasWater() && !vertex.IsRamp() && !vertex.IsBoundary() && !vertex.IsMapEdge(),
            "independent dry oracle only covers interior dry non-ramp tiles");
        cliff = cliff || vertex.CliffLevel() != level;
    }
    const auto& vertex = terrain.vertices[((py + 2) / 4) * columns + (px + 2) / 4];
    const auto& flags = properties.at(terrain.header.groundTilesets.at(vertex.GroundTextureIndex()));
    unsigned mask = 0x40 | (vertex.IsBlight() ? 0x20 : 0);
    if (cliff || !flags[0]) mask |= 0x02;
    if (!flags[1]) mask |= 0x04;
    if (cliff || !flags[2]) mask |= 0x08;
    if (cliff) mask |= 0x80;
    return static_cast<std::byte>(mask);
}
} // namespace

void CheckRealGroundBrushSave(const MapArchiveSession& original,
    const std::filesystem::path& directory, const std::filesystem::path& tool,
    const std::filesystem::path& game, const Game::GameDataSource& mounted) {
    const auto table = Take(mounted.ReadFileBoundedWithSource("TerrainArt/Terrain.slk", 2u * 1024u * 1024u));
    Require(!original.HasMember("TerrainArt/Terrain.slk"), "ground test uses the actual game-winning table");
    const auto properties = GroundProperties(table);
    const auto beforeBytes = Take(original.ReadMember("war3map.w3e"));
    const auto before = *Take(ParseMapTerrain(beforeBytes)).Snapshot();
    Require(before.header.width == 65 && before.header.height == 65, "manual ground fixtures use a 64-cell map");
    const auto center = std::size_t{32} * 65 + 32;
    const auto& oldProperties = properties.at(before.header.groundTilesets.at(before.vertices[center].GroundTextureIndex()));
    // 优先使用原palette；其余实际材质只在表内确有不同行为时作为候选。
    std::string texture;
    for (const auto& id : before.header.groundTilesets)
        if (properties.at(id) != oldProperties) { texture = id; break; }
    if (texture.empty()) for (const auto& [id, fields] : properties)
        if (fields != oldProperties) { texture = id; break; }
    Require(!texture.empty(), "actual terrain table contains a material with different pathing");
    std::cout << "ground_brush_table winner=" << table.archiveName << " sha256=" << Take(Sha256(table.bytes)) << '\n';
    const auto rules = Take(TerrainPathingSave::CaptureNative(original, before, before, game, kBytes));
    const auto oldNative = Take(rules.GenerateNative(before, kBytes));
    auto baseline = Take(original.ReadMember("war3map.wpm"));
    Require(baseline.size() == 16 + 256 * 256 && oldNative.size() == 256 * 256, "manual ground pathing dimensions");
    for (std::size_t i = 0; i < oldNative.size(); ++i) baseline[i + 16] = (baseline[i + 16] & std::byte{0x11}) | oldNative[i];
    // 完整二值非零影图与多条v0图标只验证现有保原字合同，不冒充原版烘焙/图标生成。
    ShadowGridSnapshot shadow;
    shadow.width = (before.header.width - 1) * 4; shadow.height = (before.header.height - 1) * 4;
    shadow.intensities.resize(std::size_t{shadow.width} * shadow.height, 0);
    for (std::size_t y = 29; y < 34; ++y) for (std::size_t x = 17; x < 24; ++x)
        shadow.intensities[y * shadow.width + x] = 255;
    shadow.intensities[(shadow.height - 31) * shadow.width + shadow.width - 19] = 255;
    shadow.intensities[(shadow.height / 2) * shadow.width + shadow.width / 3] = 255;
    const auto shadowBytes = Take(SerializeMapShadow(shadow));
    MinimapMarkersSnapshot markers;
    markers.records = {{0, {31, 43}, {0x11, 0x44, 0xaa, 0xff}},
        {1, {177, 67}, {0xaa, 0xbb, 0x44, 0xff}}, {2, {91, 213}, {0x44, 0x11, 0xcc, 0xff}}};
    const auto markerBytes = Take(SerializeMapMinimapMarkers(markers));
    const auto verifyDerived = [&](const MapArchiveSession& archive, const char* phase, const char* kind) {
        Require(archive.HasMember("war3map.shd") && archive.HasMember("war3map.mmp"),
            "ground derived fixture retains both populated members");
        const auto actualShadowBytes = Take(archive.ReadMember("war3map.shd"));
        const auto actualMarkerBytes = Take(archive.ReadMember("war3map.mmp"));
        Require(actualShadowBytes.size() == std::size_t{shadow.width} * shadow.height
            && actualShadowBytes == shadowBytes && actualMarkerBytes.size() == 8 + 3 * 16
            && actualMarkerBytes == markerBytes, "ground derived members retain complete sizes and exact fixture bytes");
        const auto actualShadow = Take(ParseMapShadow(actualShadowBytes, shadow.width, shadow.height));
        const auto nonzero = std::count(actualShadow.intensities.begin(), actualShadow.intensities.end(), std::uint8_t{255});
        Require(actualShadow.width == shadow.width && actualShadow.height == shadow.height && nonzero == 37
            && actualShadow.intensities == shadow.intensities
            && std::all_of(actualShadow.intensities.begin(), actualShadow.intensities.end(),
                [](auto value) { return value == 0 || value == 255; }),
            "full-size asymmetric shadow contains exactly the authored nonzero binary mask");
        const auto actualMarkers = Take(ParseMapMinimapMarkers(actualMarkerBytes));
        Require(actualMarkers.version == 0 && actualMarkers.records.size() == 3,
            "ground marker fixture is complete v0 with three records");
        for (std::size_t i = 0; i < markers.records.size(); ++i) {
            const auto& expectedMarker = markers.records[i]; const auto& actualMarker = actualMarkers.records[i];
            Require(actualMarker.typeRaw == expectedMarker.typeRaw && actualMarker.positionRaw == expectedMarker.positionRaw
                && actualMarker.colorBgra == expectedMarker.colorBgra,
                "ground marker records retain their exact order, type, coordinates and colors");
        }
        std::cout << "ground_derived_members kind=" << kind << " phase=" << phase << " archive_sha256=" << archive.SourceSha256()
            << " shd_present=true shd_bytes=" << actualShadowBytes.size() << " shd_sha256=" << Take(Sha256(actualShadowBytes))
            << " shd_nonzero=" << nonzero << " mmp_present=true mmp_bytes=" << actualMarkerBytes.size()
            << " mmp_sha256=" << Take(Sha256(actualMarkerBytes)) << " mmp_version=0 mmp_records=3"
            << " native_editor_verified=false game_verified=false\n";
    };
    for (const bool cliff : {false, true}) {
        const auto scenario = directory / (cliff ? "playable-cliff" : "playable-pathing-texture");
        Require(std::filesystem::create_directory(scenario), "exclusive ground brush scenario");
        const std::array<MemberReplacement, 3> seeded{{{"war3map.wpm", baseline},
            {"war3map.shd", shadowBytes}, {"war3map.mmp", markerBytes}}};
        const auto source = Take(MapArchiveSession::Open(Take(original.SaveCandidate(scenario / "source.w3x", seeded)).path));
        verifyDerived(source, "source", cliff ? "cliff" : "texture");
        MapEditSaveRequest request;
        request.sourceMap = source.SourcePath(); request.targetCandidate = scenario / "saved.w3x";
        request.toolRoot = tool; request.gameRoot = game;
        request.expectedSourceSha256 = source.SourceSha256(); request.expectedTerrainSha256 = Take(Sha256(beforeBytes));
        MapEditTerrainStroke stroke;
        stroke.pointSpace = MapEditPointSpace::TerrainVertex; stroke.points = {{32, 32}};
        stroke.brush.shape = TerrainBrushShape::Square; stroke.brush.radius = 2;
        stroke.brush.kind = cliff ? TerrainBrushKind::CliffLevel : TerrainBrushKind::Texture;
        if (cliff) { stroke.brush.levelDelta = 1; stroke.brush.cliffTexture = 0; }
        else { stroke.brush.groundTexture = texture; stroke.brush.seed = 42; }
        request.terrainStrokes = {stroke};
        const TerrainStrokePoint world{before.header.offset[0] + 32 * 128, before.header.offset[1] + 32 * 128};
        auto planned = Take(PrepareTerrainStroke(before, stroke.brush, std::span(&world, 1)));
        if (cliff) {
            const auto doodads = Take(ParseDoodadPlacements(Take(source.ReadMember("war3map.doo"))));
            const auto resources = Take(TerrainPathingSave::CaptureWaterBrushRules(source, before, before, doodads, game, true, kBytes));
            Require(resources.WaterBrushRules() != nullptr, "native cliff captures actual structural brush rules");
            const TerrainCliffBrush brush{stroke.brush.shape, stroke.brush.radius + 1, stroke.brush.cliffTexture,
                stroke.brush.seed, Take(CaptureTerrainStrokeAnchor(before, world)), stroke.brush.levelDelta};
            auto native = Take(PrepareTerrainCliffStroke(before, *resources.WaterBrushRules(), brush,
                std::span(&world, 1), doodads, resources.WaterSpecialShapes()));
            planned.terrain = std::move(native.terrain); planned.result = native.result.changed;
        }
        Require(!planned.result.Empty() && planned.result.x0 >= 25 && planned.result.y0 >= 25
            && planned.result.x1 <= 39 && planned.result.y1 <= 39, "ground edit stays inside the independent oracle window");
        const auto saved = Take(SaveMapEditCandidate(request));
        const auto readback = Take(MapArchiveSession::Open(saved.candidate));
        const auto afterBytes = Take(readback.ReadMember("war3map.w3e"));
        Require(afterBytes != beforeBytes && afterBytes == Take(SerializeMapTerrain(planned.terrain)),
            "public save reads back the complete intended ground brush result");
        auto expected = baseline;
        for (std::size_t py = 24 * 4; py < 40 * 4; ++py) for (std::size_t px = 24 * 4; px < 40 * 4; ++px) {
            const auto offset = 16 + py * 256 + px;
            Require((baseline[offset] & std::byte{0xee}) == DryMask(before, px, py, properties),
                "source pathing independently matches the interior dry ground");
            expected[offset] = (baseline[offset] & std::byte{0x11}) | DryMask(planned.terrain, px, py, properties);
        }
        Require(expected != baseline && Take(readback.ReadMember("war3map.wpm")) == expected,
            "complete saved WPM matches independent local dry bytes and unchanged exterior");
        Require(saved.scriptPreserved, "ground save preserves the compiled playable initialization");
        for (const auto& member : source.Members()) {
            const auto name = Text::LowerAscii(member.name);
            if (name == "war3map.w3e" || name == "war3map.wpm" || name == "war3mapmap.blp") continue;
            Require(Take(readback.ReadMember(member.name)) == member.data, "ground save preserves every unrelated member");
        }
        verifyDerived(readback, "saved", cliff ? "cliff" : "texture");
        Require(Take(Sha256File(source.SourcePath())) == source.SourceSha256(), "ground save leaves source untouched");
        std::cout << "playable_ground_save_passed kind=" << (cliff ? "cliff" : "texture")
            << " texture=" << (cliff ? "unchanged" : texture) << " source_sha256=" << source.SourceSha256()
            << " candidate_sha256=" << saved.candidateSha256 << " native_editor_verified=false game_verified=false\n";
    }
}
} // namespace War3Map::TerrainSaveTests
