#include "TerrainGroundBrushRealTests.h"

#include "War3MapCore/Hash.h"
#include "War3MapCore/MapData/MapMinimapMarkers.h"
#include "War3MapCore/MapData/MapShadow.h"
#include "War3MapCore/MapData/TerrainCliffEditing.h"
#include "War3MapCore/MapData/TerrainRampEditing.h"
#include "War3MapCore/ObjectData/ObjectImport.h"
#include "War3MapCore/Script/MapScriptBuild.h"
#include "War3MapCore/Services/ObjectTransactionSnapshot.h"
#include "War3MapCore/Services/TerrainMinimapMarkers.h"
#include "War3MapCore/Services/TerrainPathingSave.h"
#include "War3MapCore/Text/SlkCells.h"
#include "War3MapCore/Text/TextUtil.h"

#include <algorithm>
#include <array>
#include <iostream>
#include <iterator>
#include <limits>
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
    // SHD保原字；MMP源哨兵在正常保存时须被实际来源生成的完整结果替换。
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
    const auto verifyDerived = [&](const MapArchiveSession& archive, const char* phase, const char* kind,
        const std::vector<std::byte>& expectedMarkers) {
        Require(archive.HasMember("war3map.shd") && archive.HasMember("war3map.mmp"),
            "ground derived fixture retains both populated members");
        const auto actualShadowBytes = Take(archive.ReadMember("war3map.shd"));
        const auto actualMarkerBytes = Take(archive.ReadMember("war3map.mmp"));
        Require(actualShadowBytes.size() == std::size_t{shadow.width} * shadow.height
            && actualShadowBytes == shadowBytes && actualMarkerBytes == expectedMarkers,
            "ground derived members retain SHD and contain the exact expected complete MMP");
        const auto actualShadow = Take(ParseMapShadow(actualShadowBytes, shadow.width, shadow.height));
        const auto nonzero = std::count(actualShadow.intensities.begin(), actualShadow.intensities.end(), std::uint8_t{255});
        Require(actualShadow.width == shadow.width && actualShadow.height == shadow.height && nonzero == 37
            && actualShadow.intensities == shadow.intensities
            && std::all_of(actualShadow.intensities.begin(), actualShadow.intensities.end(),
                [](auto value) { return value == 0 || value == 255; }),
            "full-size asymmetric shadow contains exactly the authored nonzero binary mask");
        const auto actualMarkers = Take(ParseMapMinimapMarkers(actualMarkerBytes));
        const auto expected = Take(ParseMapMinimapMarkers(expectedMarkers));
        Require(actualMarkers.version == 0 && actualMarkers.records.size() == expected.records.size(),
            "ground marker result is complete v0 with expected record count");
        for (std::size_t i = 0; i < expected.records.size(); ++i) {
            const auto& expectedMarker = expected.records[i]; const auto& actualMarker = actualMarkers.records[i];
            Require(actualMarker.typeRaw == expectedMarker.typeRaw && actualMarker.positionRaw == expectedMarker.positionRaw
                && actualMarker.colorBgra == expectedMarker.colorBgra,
                "ground marker records have their exact expected order, type, coordinates and colors");
        }
        std::cout << "ground_derived_members kind=" << kind << " phase=" << phase << " archive_sha256=" << archive.SourceSha256()
            << " shd_present=true shd_bytes=" << actualShadowBytes.size() << " shd_sha256=" << Take(Sha256(actualShadowBytes))
            << " shd_nonzero=" << nonzero << " mmp_present=true mmp_bytes=" << actualMarkerBytes.size()
            << " mmp_sha256=" << Take(Sha256(actualMarkerBytes)) << " mmp_version=0 mmp_records=" << actualMarkers.records.size()
            << " native_editor_verified=false game_verified=false\n";
    };
    for (const unsigned mode : {0u, 1u, 2u, 3u}) {
        const bool cliff = mode == 1, mixed = mode == 2, renamed = mode == 3;
        const auto scenario = directory / (renamed ? "playable-mixed-settings-terrain"
            : mixed ? "playable-mixed-unit-terrain" : cliff ? "playable-cliff" : "playable-pathing-texture");
        Require(std::filesystem::create_directory(scenario), "exclusive ground brush scenario");
        std::vector<MemberReplacement> seeded{{"war3map.wpm", baseline},
            {"war3map.shd", shadowBytes}, {"war3map.mmp", markerBytes}};
        std::string authorReference;
        std::vector<std::byte> expectedStrings;
        constexpr std::string_view originalAuthor = "Original terrain author reference";
        constexpr std::string_view finalAuthor = "Final terrain author body";
        if (renamed) {
            // 在同一原创来源上加唯一引用和未改哨兵；WTS期望直接替换已知正文，
            // 不调用产品字段层或字符串writer来生成保存后的oracle。
            const auto captured = Take(original.CaptureExactMembers(std::array<std::string, 1>{"war3map.wts"}));
            Require(captured.size() == 1, "capture exact original string-table presence");
            const auto& originalStrings = captured.front().bytes;
            const auto parsed = Take(ParseStringTable(originalStrings));
            std::uint32_t largest = 0;
            for (const auto& entry : parsed.entries) largest = (std::max)(largest, entry.id);
            Require(largest <= (std::numeric_limits<std::uint32_t>::max)() - 2,
                "original fixture has two unused string IDs");
            const auto authorId = largest + 1;
            authorReference = "TRIGSTR_000" + std::to_string(authorId);
            std::string strings(originalStrings.empty() ? "" : reinterpret_cast<const char*>(originalStrings.data()),
                originalStrings.size());
            if (!strings.empty() && strings.back() != '\n') strings += "\r\n";
            strings += "// terrain author reference: preserve formatting\r\nSTRING " + std::to_string(authorId)
                + "\r\n{\r\n";
            const auto authorOffset = strings.size();
            strings += originalAuthor;
            strings += "\r\n}\r\n// untouched terrain sentinel\r\nSTRING " + std::to_string(authorId + 1)
                + "\r\n{\r\nUntouched terrain string body\r\n}\r\n";
            auto expected = strings;
            expected.replace(authorOffset, originalAuthor.size(), finalAuthor);
            const auto wire = [](std::string_view text) {
                const auto bytes = std::as_bytes(std::span(text.data(), text.size()));
                return std::vector<std::byte>(bytes.begin(), bytes.end());
            };
            seeded.push_back({"war3map.wts", wire(strings)});
            expectedStrings = wire(expected);
            auto settings = Take(MapSettings::Parse(Take(original.ReadMember("war3map.w3i"))));
            settings.author = authorReference;
            seeded.push_back({"war3map.w3i", Take(settings.Serialize())});
        }
        if (mixed) {
            using namespace ObjectData;
            auto units = Take(NativeObjectDocument::CreateEmpty(ObjectType::Unit));
            Require(units.AddCustomObject("uM01", "hfoo").Succeeded(), "seed original custom unit for mixed save");
            Require(units.AddModification({ObjectType::Unit, "uM01"}, {"uhpm"}, NativeValueType::Int32, std::int32_t{0}).Succeeded(),
                "seed unit HP zero before editing");
            seeded.push_back({"war3map.w3u", Take(NativeObjectCodec(ObjectType::Unit).Serialize(units))});
        }
        const auto source = Take(MapArchiveSession::Open(Take(original.SaveCandidate(scenario / "source.w3x", seeded)).path));
        verifyDerived(source, "source", cliff ? "cliff" : "texture", markerBytes);
        MapEditSaveRequest request;
        request.sourceMap = source.SourcePath(); request.targetCandidate = scenario / "saved.w3x";
        request.toolRoot = tool; request.gameRoot = game;
        request.expectedSourceSha256 = source.SourceSha256(); request.expectedTerrainSha256 = Take(Sha256(beforeBytes));
        std::vector<std::byte> expectedUnits;
        std::vector<std::byte> expectedSettings;
        if (renamed) {
            const auto settingsBytes = Take(source.ReadMember("war3map.w3i"));
            const auto stringBytes = Take(source.ReadMember("war3map.wts"));
            const std::array<MapArchiveMember, 2> settingsMembers{{{"war3map.w3i", settingsBytes}, {"war3map.wts", stringBytes}}};
            const auto settingsMap = MapDocument::FromMembers(settingsMembers);
            Require(settingsMap.Settings() && settingsMap.Settings()->author == authorReference
                && Take(settingsMap.ResolveString(authorReference)) == originalAuthor,
                "original mixed source binds the exact zero-padded author reference to its unique WTS body");
            const auto* field = FindMapField(MapDataKind::Settings, "map_name");
            Require(field != nullptr, "shared map-name field is writable");
            const auto beforeName = Take(ReadMapField(settingsMap, MapDataKind::Settings, 0, *field));
            const auto afterName = Take(ParseMapFieldInput(*field, "Terrain texture and final map name"));
            request.fieldEdits = {{Take(Sha256(settingsBytes)), {MapDataKind::Settings, 0,
                Take(MapFieldRowKey(settingsMap, MapDataKind::Settings, 0)), "map_name", beforeName, afterName}}};
            const auto* author = FindMapField(MapDataKind::Settings, "author");
            Require(author != nullptr, "shared author field is writable through its existing WTS reference");
            request.fieldEdits.push_back({Take(Sha256(settingsBytes)), {MapDataKind::Settings, 0,
                Take(MapFieldRowKey(settingsMap, MapDataKind::Settings, 0)), "author",
                Take(ReadMapField(settingsMap, MapDataKind::Settings, 0, *author)),
                Take(ParseMapFieldInput(*author, finalAuthor))}});
            auto expected = Take(MapSettings::Parse(settingsBytes));
            expected.mapName = "Terrain texture and final map name";
            expectedSettings = Take(expected.Serialize());
        }
        if (mixed) {
            using namespace ObjectData;
            const auto originalUnits = Take(source.ReadMember("war3map.w3u"));
            auto parsed = LniObjectCodec(ObjectType::Unit).Parse(Take(ConvertNativeObjectDataToLni(ObjectType::Unit, originalUnits)));
            const auto* lni = parsed.Succeeded() ? dynamic_cast<const LniObjectDocument*>(parsed.document.get()) : nullptr;
            Require(lni != nullptr, "project actual baseline into shared editable unit document");
            auto edited = *lni;
            Require(edited.SetModification({ObjectType::Unit, "uM01"}, {"uhpm"}, std::int32_t{1}).Succeeded(), "edit unit HP zero to one");
            auto transaction = Take(ObjectTransactionSnapshot::Freeze(edited, originalUnits, source.SourceSha256()));
            expectedUnits.assign(transaction->EncodedNative().begin(), transaction->EncodedNative().end());
            request.objectTransactions.push_back(std::move(transaction));
        }
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
        std::vector<MemberReplacement> markerInputs{{"war3map.w3e", Take(SerializeMapTerrain(planned.terrain))}};
        if (mixed) markerInputs.push_back({"war3map.w3u", expectedUnits});
        if (renamed) markerInputs.push_back({"war3map.w3i", expectedSettings});
        const auto expectedMarkerOwner = Take(TerrainMinimapMarkers::Capture(source, game, markerInputs));
        Require(expectedMarkerOwner->Bytes() != markerBytes, "normal save must replace synthetic stale marker sentinels");
        if (mixed || renamed) {
            const auto reject = [&](MapEditSaveRequest bad, std::string_view code, const char* filename) {
                bad.targetCandidate = scenario / filename;
                bool progressed = false;
                const auto rejected = SaveMapEditCandidate(bad, [&](std::string_view) { progressed = true; });
                Require(!rejected.Succeeded() && !rejected.value && !progressed
                    && !std::filesystem::exists(bad.targetCandidate)
                    && std::any_of(rejected.diagnostics.begin(), rejected.diagnostics.end(), [&](const auto& diagnostic) {
                        return diagnostic.code == code;
                    }), "invalid mixed input rejects before script preparation or candidate publication");
            };
            auto stale = request;
            stale.expectedTerrainSha256[0] = stale.expectedTerrainSha256[0] == '0' ? '1' : '0';
            reject(stale, "SAVE_TERRAIN_STALE", "stale-terrain-must-not-exist.w3x");
            auto missing = request; missing.expectedTerrainSha256.clear();
            reject(missing, "SAVE_REQUEST_INVALID", "missing-terrain-sha-must-not-exist.w3x");
            auto invalidStroke = request; invalidStroke.terrainStrokes.front().points.clear();
            reject(invalidStroke, "SAVE_STROKE_POINTS", "empty-stroke-must-not-exist.w3x");
            auto extraDomain = request; extraDomain.placementEdit = MapEditPlacementRotation{};
            reject(extraDomain, "SAVE_OPERATION_SCOPE_INVALID", "extra-domain-must-not-exist.w3x");
            if (renamed) {
                auto range = request; range.fieldEdits.front().edit.field = "playable_width";
                reject(range, "SAVE_TERRAIN_FIELD_SCOPE_INVALID", "mixed-range-must-not-exist.w3x");
            }
        }
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
        if (!mixed && !renamed) Require(saved.scriptPreserved, "ground save preserves the compiled playable initialization");
        if (mixed) Require(Take(readback.ReadMember("war3map.w3u")) == expectedUnits, "public mixed save reads back HP zero to one exactly");
        if (renamed) Require(Take(readback.ReadMember("war3map.w3i")) == expectedSettings
            && Take(readback.ReadMember("war3map.wts")) == expectedStrings
            && saved.appliedFieldEditCount == 2 && saved.appliedStrokeCount == 1 && saved.generatedMissingMinimap.has_value(),
            "real script preparation and automatic minimap consume the final typed settings alongside terrain");
        if (renamed) {
            const auto map = MapDocument::FromArchive(readback);
            Require(map.Settings() && map.Settings()->author == authorReference
                && Take(map.ResolveString(authorReference)) == finalAuthor,
                "independent reopen preserves the author reference and only replaces the exact requested WTS body");
            const auto script = Take(readback.ReadMember(Script::MapScriptMemberName(readback)));
            const std::string_view scriptText(reinterpret_cast<const char*>(script.data()), script.size());
            Require(!saved.scriptPreserved && scriptText.find("Terrain texture and final map name") != std::string_view::npos,
                "compiled final script contains the new map name instead of preserving the source script");
            const auto minimap = Take(readback.ReadMember("war3mapMap.blp"));
            Require(minimap.size() == saved.generatedMissingMinimap->encodedBlpBytes
                && Take(Sha256(minimap)) == saved.generatedMissingMinimap->encodedBlpSha256
                && saved.generatedMissingMinimap->width == 256 && saved.generatedMissingMinimap->height == 256
                && saved.generatedMissingMinimap->encodedMipCount == 1,
                "actual generated single-mip BLP independently reads back with the helper receipt identity");
        }
        for (const auto& member : source.Members()) {
            const auto name = Text::LowerAscii(member.name);
            if (name == "war3map.w3e" || name == "war3map.wpm" || name == "war3mapmap.blp" || name == "war3map.mmp") continue;
            if ((mixed || renamed) && (name == "war3map.j" || name == "scripts/war3map.j")) continue;
            if (mixed && name == "war3map.w3u") continue;
            if (renamed && (name == "war3map.w3i" || name == "war3map.wts")) continue;
            Require(Take(readback.ReadMember(member.name)) == member.data, "ground save preserves every unrelated member");
        }
        verifyDerived(readback, "saved", cliff ? "cliff" : "texture", expectedMarkerOwner->Bytes());
        Require(expectedMarkerOwner->VerifyReadback(readback).Succeeded(), "actual final marker dependencies survive public save");
        Require(Take(Sha256File(source.SourcePath())) == source.SourceSha256(), "ground save leaves source untouched");
        std::cout << "playable_ground_save_passed kind=" << (cliff ? "cliff" : "texture")
            << " texture=" << (cliff ? "unchanged" : texture) << " source_sha256=" << source.SourceSha256()
            << " candidate_sha256=" << saved.candidateSha256 << " native_editor_verified=false game_verified=false\n";
    }
}

void CheckRealExistingRampCliffSave(const MapArchiveSession& original,
    const std::filesystem::path& directory, const std::filesystem::path& tool,
    const std::filesystem::path& game) {
    const auto originalTerrain = *Take(ParseMapTerrain(Take(original.ReadMember("war3map.w3e")))).Snapshot();
    Require(originalTerrain.header.width == 65 && originalTerrain.header.height == 65,
        "existing ramp public save uses the playable 64-cell original fixture");
    for (const bool wet : {false, true}) {
        const auto scenario = directory / (wet ? "playable-wet-ramp-cliff" : "playable-dry-ramp-cliff");
        Require(std::filesystem::create_directory(scenario), "exclusive existing ramp cliff save scenario");
        auto before = originalTerrain;
        for (std::uint32_t y = 0; y < before.header.height; ++y) for (std::uint32_t x = 0; x < before.header.width; ++x) {
            auto& vertex = before.vertices[std::size_t{y} * before.header.width + x];
            Require(!vertex.HasWater() && !vertex.IsRamp(), "existing ramp seed begins with dry original terrain");
            vertex.cliffAndLayerRaw = static_cast<std::uint8_t>((vertex.cliffAndLayerRaw & 0xf0) | (x <= 31 ? 3 : 2));
        }
        for (const auto x : {31u, 32u, 33u}) before.vertices[32 * 65 + x].textureAndFlagsRaw |= 0x10;
        if (wet) for (std::uint32_t y = 29; y <= 35; ++y) for (std::uint32_t x = 29; x <= 35; ++x) {
            auto& vertex = before.vertices[std::size_t{y} * 65 + x];
            vertex.textureAndFlagsRaw |= 0x40;
            vertex.waterHeightAndFlagsRaw = static_cast<std::uint16_t>((vertex.groundHeightRaw & 0x3fff) + 1200);
        }
        RefreshTerrainDerived(before, 0, 0, 65, 65);
        const auto beforeBytes = Take(SerializeMapTerrain(before));
        const TerrainStrokePoint point{before.header.offset[0] + 32 * 128, before.header.offset[1] + 32 * 128};
        const auto seeded = Take(MapArchiveSession::Open(Take(original.SaveCandidate(scenario / "seeded.w3x",
            std::array<MemberReplacement, 1>{{{"war3map.w3e", beforeBytes}}})).path));
        const auto doodads = Take(ParseDoodadPlacements(Take(seeded.ReadMember("war3map.doo"))));
        const auto resources = Take(TerrainPathingSave::CaptureWaterBrushRules(seeded, before, before, doodads, game, true, kBytes));
        const auto* loaded = resources.WaterBrushRules();
        Require(loaded != nullptr, "existing ramp cliff captures actual terrain rules");
        Require(TerrainRampHeightOffset(before, 32, 32, loaded->maximumCliffLevel) == 64.0f,
            "independent three-bit straight ramp midpoint has exactly 64 units of effective ground lift");
        TerrainBrush options;
        options.kind = TerrainBrushKind::CliffLevel; options.shape = TerrainBrushShape::Square;
        options.radius = 0; options.levelDelta = 1; options.cliffTexture = 0; options.seed = 73;
        const TerrainCliffBrush brush{options.shape, options.radius + 1, options.cliffTexture,
            options.seed, Take(CaptureTerrainStrokeAnchor(before, point)), options.levelDelta};
        const auto planned = Take(PrepareTerrainCliffStroke(before, *loaded, brush, std::span(&point, 1),
            doodads, resources.WaterSpecialShapes()));
        const auto afterBytes = Take(SerializeMapTerrain(planned.terrain));
        Require(afterBytes != beforeBytes && !planned.result.changed.Empty(), "actual existing ramp cliff stroke changes terrain");
        const auto rules = Take(TerrainPathingSave::CaptureNative(seeded, before, planned.terrain, game, kBytes));
        const auto oldNative = Take(rules.GenerateNative(before, kBytes));
        const auto newNative = Take(rules.GenerateNative(planned.terrain, kBytes));
        auto oldPathing = Take(seeded.ReadMember("war3map.wpm"));
        Require(oldPathing.size() == 16 + oldNative.size() && oldNative.size() == newNative.size(),
            "existing ramp pathing uses the complete original grid dimensions");
        auto expectedPathing = oldPathing;
        // 此专项复用实际资源生成器检查公共保存编排；独立坡道算法预期在Core测试。
        // 不把这份可操作原创地图的保存回读当作原版或游戏通行验收。
        for (std::size_t i = 0; i < oldNative.size(); ++i) {
            oldPathing[i + 16] = (oldPathing[i + 16] & std::byte{0x11}) | oldNative[i];
            expectedPathing[i + 16] = (oldPathing[i + 16] & std::byte{0x11}) | newNative[i];
        }
        Require(oldPathing != expectedPathing, "existing ramp cliff stroke changes complete native pathing");
        const auto source = Take(MapArchiveSession::Open(Take(seeded.SaveCandidate(scenario / "source.w3x",
            std::array<MemberReplacement, 1>{{{"war3map.wpm", oldPathing}}})).path));
        MapEditSaveRequest request;
        request.sourceMap = source.SourcePath(); request.targetCandidate = scenario / "saved.w3x";
        request.toolRoot = tool; request.gameRoot = game; request.expectedSourceSha256 = source.SourceSha256();
        request.expectedTerrainSha256 = Take(Sha256(beforeBytes));
        MapEditTerrainStroke stroke;
        stroke.brush = options; stroke.points = {point}; request.terrainStrokes = {stroke};
        const auto saved = Take(SaveMapEditCandidate(request));
        const auto readback = Take(MapArchiveSession::Open(saved.candidate));
        Require(Take(readback.ReadMember("war3map.w3e")) == afterBytes
            && Take(readback.ReadMember("war3map.wpm")) == expectedPathing,
            "existing dry/wet ramp public save reads back the complete intended W3E and WPM");
        Require(saved.scriptPreserved, "existing ramp cliff save preserves the checked playable script");
        const auto expectedMarkers = Take(TerrainMinimapMarkers::Capture(source, game,
            std::array<MemberReplacement, 1>{{{"war3map.w3e", afterBytes}}}));
        Require(expectedMarkers->VerifyReadback(readback).Succeeded(), "existing ramp markers use final terrain and actual dependencies");
        for (const auto& member : source.Members()) {
            const auto name = Text::LowerAscii(member.name);
            if (name == "war3map.w3e" || name == "war3map.wpm" || name == "war3mapmap.blp" || name == "war3map.mmp") continue;
            Require(Take(readback.ReadMember(member.name)) == member.data, "existing ramp cliff save preserves unrelated members and prior shadows");
        }
        Require(Take(Sha256File(source.SourcePath())) == source.SourceSha256(), "existing ramp cliff save leaves source untouched");
        Require(rules.VerifyReadback(readback, kBytes).Succeeded(), "existing ramp final actual terrain dependencies remain unchanged");
        std::cout << "playable_existing_ramp_cliff_save_passed wet=" << wet
            << " source_sha256=" << source.SourceSha256() << " candidate_sha256=" << saved.candidateSha256
            << " native_editor_verified=false game_verified=false\n";
    }
}
} // namespace War3Map::TerrainSaveTests
