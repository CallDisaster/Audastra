#pragma once

#include "War3MapCore/Hash.h"
#include "War3MapCore/MapData/TerrainRampEditing.h"
#include "War3MapCore/MapData/TerrainPathingResources.h"
#include "War3MapCore/MapData/MapSettings.h"
#include "War3MapCore/Services/TerrainEditService.h"
#include "War3MapCore/Services/TerrainPathingSave.h"
#include "War3MapCore/Services/MapSaveService.h"

#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <initializer_list>
#include <memory>
#include <source_location>
#include <stdexcept>

namespace War3Map::TerrainCornerRampFixtures {
using namespace MapData;
using namespace Services;
using Bytes = std::vector<std::byte>;
constexpr std::size_t kBudget = 8u * 1024u * 1024u;

inline void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class T> T Take(Result<T> result, const std::source_location& location = std::source_location::current()) {
    if (!result.Succeeded() || !result.value) {
        std::string message = std::string(location.function_name()) + ":" + std::to_string(location.line());
        for (const auto& diagnostic : result.diagnostics) message += "\n" + diagnostic.code + ": " + diagnostic.message;
        throw std::runtime_error(message);
    }
    return std::move(*result.value);
}
inline Bytes Text(std::string_view value) {
    const auto bytes = std::as_bytes(std::span(value.data(), value.size())); return {bytes.begin(), bytes.end()};
}
inline Bytes Slk(const std::vector<std::string>& columns, std::initializer_list<std::vector<std::string>> rows) {
    std::string text = "ID;PWXL;N;E\n";
    const auto append = [&](const auto& values, std::size_t y) {
        for (std::size_t x = 0; x < values.size(); ++x)
            text += "C;X" + std::to_string(x + 1) + ";Y" + std::to_string(y) + ";K\"" + values[x] + "\"\n";
    };
    append(columns, 1); std::size_t y = 2; for (const auto& row : rows) append(row, y++);
    return Text(text + "E\n");
}

inline Bytes PathingLiteral(bool created) {
    Bytes pathing{std::byte{'M'}, std::byte{'P'}, std::byte{'3'}, std::byte{'W'}};
    for (const auto value : {0u, 128u, 128u}) for (unsigned shift = 0; shift < 32; shift += 8)
        pathing.push_back(std::byte((value >> shift) & 255));
    pathing.resize(16 + 128 * 128);
    // 独立G0：高层西南象限的两条悬崖边，逐地格4x4展开，保护位11保持。
    // G1只打开(16,16)，新增(17,15)/(15,17)两肩；不调用产品生成器制造expected。
    for (unsigned y = 0; y < 128; ++y) for (unsigned x = 0; x < 128; ++x) {
        const auto cx = x / 4, cy = y / 4;
        bool blocked = (cx == 16 && cy <= 16) || (cy == 16 && cx <= 16);
        if (created && cx == 16 && cy == 16) blocked = false;
        if (created && ((cx == 17 && cy == 15) || (cx == 15 && cy == 17))) blocked = true;
        pathing[16 + y * 128 + x] = blocked ? std::byte{0xdb} : std::byte{0x51};
    }
    return pathing;
}

struct Fixture {
    std::filesystem::path root, game;
    TerrainGridSnapshot grid;
    std::vector<MapArchiveMember> members;
    UnitPlacements units;
    DoodadPlacements doodads;
    std::unique_ptr<MapArchiveSession> source;
    std::unique_ptr<MapDataEditSession> edits;
    TerrainStrokePoint point{2080, 2080};
    Fixture() {
        static std::atomic<unsigned> next{0};
        root = std::filesystem::temp_directory_path() / ("Audastra-corner-service-"
            + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()) + "-" + std::to_string(next++));
        Require(std::filesystem::create_directory(root), "private corner fixture directory");
        game = root / "game"; Require(std::filesystem::create_directory(game), "private corner game source");
        const std::vector<MapArchiveMember> gameMembers{{"fixture.txt", Text("independent corner fixture")}};
        Take(WriteVerifiedArchive(game / "war3.mpq", gameMembers));
        grid.header.version = 11; grid.header.width = grid.header.height = 33; grid.header.mainTileset = 'L';
        grid.header.groundTilesets = {"Tg01"}; grid.header.cliffTilesets = {"Cl01"}; grid.vertices.resize(33 * 33);
        for (unsigned y = 0; y < 33; ++y) for (unsigned x = 0; x < 33; ++x) {
            auto& vertex = grid.vertices[y * 33 + x]; vertex.groundHeightRaw = 8320;
            vertex.waterHeightAndFlagsRaw = 7936; vertex.variationRaw = 3;
            vertex.cliffAndLayerRaw = x <= 16 && y <= 16 ? 3 : 2;
        }
        RefreshTerrainDerived(grid, 0, 0, 33, 33);
        for (std::size_t row = 0; row < 3; ++row) {
            const float x = row < 2 ? 2176.0f : 0, y = row == 0 ? 2176.0f : row == 1 ? 2048.0f : 0;
            UnitPlacement unit; unit.rawcode = "u001"; unit.creationId = static_cast<std::int32_t>(10 + row);
            unit.position = {x, y, row == 0 ? 42.0f : row == 1 ? 72.0f : 52.0f}; unit.scale = {1, 1, 1}; units.units.push_back(unit);
            DoodadPlacement doodad; doodad.rawcode = "Dst0"; doodad.creationId = static_cast<std::int32_t>(20 + row);
            doodad.position = {x, y, row == 0 ? 37.0f : row == 1 ? 67.0f : 57.0f}; doodad.scale = {1, 1, 1}; doodads.doodads.push_back(doodad);
        }
        MapSettings settings; settings.formatVersion = 25; settings.custom.emplace(); settings.tileset = 'L';
        settings.playableWidth = settings.playableHeight = 32; settings.flags |= 0x400;
        members = {{"war3map.w3e", Take(SerializeMapTerrain(grid))}, {"war3map.wpm", PathingLiteral(false)},
            {"war3map.w3i", Take(settings.Serialize())}, {"war3mapUnits.doo", Take(units.Serialize())},
            {"war3map.doo", Take(doodads.Serialize())}, {"war3map.j", Text("// untouched authored script\n")},
            {"TerrainArt/Terrain.slk", Slk({"tileID", "walkable", "flyable", "buildable"}, {{"Tg01", "1", "1", "1"}})},
            {"TerrainArt/Water.slk", Slk({"waterID", "height", "impassable"}, {{"LSha", "0", "0"}})},
            {"TerrainArt/CliffTypes.slk", Slk({"cliffID"}, {{"Cl01"}})},
            {"UI/MiscData.txt", Text("[Terrain]\nMaxSlope=90\nMinHeight=-256\nMaxHeight=1536\n[Water]\nDeepLevelPathing=128\n"
                "[CameraMargins]\nLeft=0\nRight=0\nBottom=0\nTop=0\n")},
            {"UI/WorldEditData.txt", Text("[Other]\nValue=1\n")},
            {"Units/DestructableData.slk", Slk({"destructableID", "code", "walkable"}, {{"Dst0", "Dst0", "0"}})},
            {"Doodads/Doodads.slk", Slk({"doodID", "code"}, {{"Doo0", "Doo0"}})},
            {"Units/DestructableMetaData.slk", Slk({"ID", "field", "slk", "type", "repeat", "data", "index"},
                {{"dwlk", "walkable", "DestructableData", "bool", "0", "0", "-1"}})}};
        source = std::make_unique<MapArchiveSession>(Take(MapArchiveSession::Open(Take(WriteVerifiedArchive(root / "source.w3x", members)).path)));
        edits = FreshSession();
    }
    ~Fixture() { std::error_code error; std::filesystem::remove_all(root, error); }
    std::unique_ptr<MapDataEditSession> FreshSession() const {
        auto session = std::make_unique<MapDataEditSession>(MapDocument::FromMembers(members));
        Require(session->AttachTerrain(members[0].data).Succeeded() && session->AttachPathing(members[1].data).Succeeded(), "attach complete corner source");
        return session;
    }
    TerrainPathingResources Rules(bool enforce = true) const {
        const auto current = Take(edits->Snapshot(MapDataKind::Doodads, kBudget));
        return Take(TerrainPathingSave::CaptureWaterBrushRules(*source, grid, *edits->Terrain(), *current.Doodads(), game, enforce, kBudget));
    }
    MapDataQueryBinding Document() const { return {"corner-service", 1, 0, source->SourceSha256()}; }
    TerrainEditBinding Binding() const { return {Document(), edits->SessionIdentity(), edits->Revision(), edits->TerrainSourceRevision()}; }
    Bytes TerrainLiteral(bool created) const {
        auto expected = grid;
        if (created) for (unsigned y = 16; y <= 18; ++y) for (unsigned x = 16; x <= 18; ++x) expected.vertices[y * 33 + x].textureAndFlagsRaw = 0x10;
        return Take(SerializeMapTerrain(expected));
    }
    UnitPlacements UnitsLiteral(bool created) const {
        auto expected = units; if (created) { expected.units[0].position[2] += 32; expected.units[1].position[2] += 64; } return expected;
    }
    DoodadPlacements DoodadsLiteral(bool created) const {
        auto expected = doodads; if (created) { expected.doodads[0].position[2] += 32; expected.doodads[1].position[2] += 64; } return expected;
    }
    void CheckState(const MapDataEditSession& session, bool created) const {
        const auto snapshot = session.Snapshot();
        Require(Take(SerializeMapTerrain(*session.Terrain())) == TerrainLiteral(created)
            && Take(snapshot.Units()->Serialize()) == Take(UnitsLiteral(created).Serialize())
            && Take(snapshot.Doodads()->Serialize()) == Take(DoodadsLiteral(created).Serialize()),
            "complete terrain and placement streams match independent R/S32/R64 literals");
    }
};

} // namespace War3Map::TerrainCornerRampFixtures
