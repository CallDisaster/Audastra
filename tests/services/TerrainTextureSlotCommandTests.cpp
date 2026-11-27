#include "../support/TerrainTextureSlotSaveFixture.h"
#include <iostream>

using namespace AudastraTest::TextureSlot;
using namespace War3Map::Services;
using War3Map::MapData::TerrainTextureTable;

int main() {
    try {
        MapData::TerrainHeader header; header.groundTilesets = {"Tg01"}; header.cliffTilesets = {"Cl01"};
        TerrainTextureSlotCommand typed{TerrainTextureSlotOperation::Replace, TerrainTextureTable::Ground, 0, "Tg01", "Tg02"};
        Require(NormalizeTerrainTextureSlotCommand(typed, &header).Succeeded(), "replacement CAS accepts actual old value");
        TerrainTextureSlotCommand remap{TerrainTextureSlotOperation::Remap, TerrainTextureTable::Ground, 0, "Tg01", "Tg02"};
        MapData::TerrainHeader two = header; two.groundTilesets.push_back("Tg02");
        const auto normalizedRemap = NormalizeTerrainTextureSlotCommand(remap, &two);
        Require(normalizedRemap.Succeeded() && normalizedRemap.value && normalizedRemap.value->algorithm == 4, "remap normalizes to algorithm 4");
        Require(!NormalizeTerrainTextureSlotCommand(remap, &header).Succeeded(), "remap target must already be in the table");
        auto remapBad = remap; remapBad.table = TerrainTextureTable::Cliff;
        Require(!NormalizeTerrainTextureSlotCommand(remapBad).Succeeded(), "remap is for the ground table only");
        remapBad = remap; remapBad.rawcode = "Tg01";
        Require(!NormalizeTerrainTextureSlotCommand(remapBad).Succeeded(), "remap onto itself rejected");
        auto invalid = typed; invalid.expectedRawcode = "Tg02";
        Require(!NormalizeTerrainTextureSlotCommand(invalid, &header).Succeeded(), "replacement requires matching old value");
        invalid = typed; invalid.operation = static_cast<TerrainTextureSlotOperation>(99);
        Require(!NormalizeTerrainTextureSlotCommand(invalid).Succeeded(), "unknown operation rejected");
        invalid = typed; invalid.table = static_cast<TerrainTextureTable>(99);
        Require(!NormalizeTerrainTextureSlotCommand(invalid).Succeeded(), "unknown table rejected");
        invalid = typed; invalid.rawcode = std::string("ab\0d", 4);
        Require(!NormalizeTerrainTextureSlotCommand(invalid).Succeeded(), "embedded NUL rejected");
        invalid = typed; invalid.operation = TerrainTextureSlotOperation::Append; invalid.slot = 1;
        Require(!NormalizeTerrainTextureSlotCommand(invalid).Succeeded(), "append cannot carry replacement expectation");
        invalid.expectedRawcode.clear();
        Require(NormalizeTerrainTextureSlotCommand(invalid, &header).Succeeded(), "append CAS accepts current length");
        header.groundTilesets.push_back("Tg03");
        Require(!NormalizeTerrainTextureSlotCommand(invalid, &header).Succeeded(), "append expectation expires after preceding append");
        Fixture fixture;
        for (const auto table : {TerrainTextureTable::Ground, TerrainTextureTable::Cliff}) {
            const bool ground = table == TerrainTextureTable::Ground;
            const std::string prefix = ground ? "Tg" : "Cl";
            auto request = fixture.Request(ground ? "ground" : "cliff");
            request.terrainStrokes = {Command(true, table, 1, "", prefix + "02"), Command(true, table, 2, "", prefix + "03"),
                Command(false, table, 1, prefix + "02", prefix + "04"), Command(false, table, 0, prefix + "01", prefix + "01")};
            (void)Take(SaveMapEditCandidate(request));
            auto reopened = Take(MapArchiveSession::Open(request.targetCandidate));
            const auto actual = Take(MapData::ParseMapTerrain(Take(reopened.ReadMember("war3map.w3e"))));
            Require((ground ? actual.Snapshot()->header.groundTilesets : actual.Snapshot()->header.cliffTilesets) == std::vector<std::string>{prefix + "01", prefix + "04", prefix + "03"},
                "save/reopen preserves execution-order append and replacement CAS");
            auto original = Take(MapArchiveSession::Open(fixture.source.path));
            Require(Take(reopened.ReadMember("war3map.wpm")) == Take(original.ReadMember("war3map.wpm")), "palette-only edit preserves native pathing");
            Require(Take(reopened.ReadMember("unchanged.bin")) == Take(original.ReadMember("unchanged.bin")), "unrelated member preserved");
            for (unsigned mode = 0; mode < 6; ++mode) {
                request.targetCandidate = fixture.root / ((ground ? "g-bad-" : "c-bad-") + std::to_string(mode) + ".w3x");
                request.terrainStrokes = {Command(true, table, 1, "", prefix + "02")};
                if (mode == 0) request.terrainStrokes.push_back(Command(true, table, 1, "", prefix + "03"));
                if (mode == 1) request.terrainStrokes.push_back(Command(false, table, 1, prefix + "01", prefix + "03"));
                if (mode == 2) request.terrainStrokes.push_back(Command(true, table, 2, "", prefix + "01"));
                if (mode == 3) request.terrainStrokes.push_back(Command(true, table, 2, "", "Miss"));
                if (mode == 4) request.terrainStrokes = {Command(false, table, 0, prefix + "01", prefix + "01")};
                if (mode == 5) request.terrainStrokes.push_back(Command(true, table, ground ? 16 : 15, "", prefix + "03"));
                const auto rejected = SaveMapEditCandidate(request);
                Require(!rejected.Succeeded() && !rejected.value && !std::filesystem::exists(request.targetCandidate), "failed suffix cannot publish any prefix");
                if (mode <= 1 || mode == 4) Require(std::any_of(rejected.diagnostics.begin(), rejected.diagnostics.end(), [&](const auto& d) {
                    return d.code == (mode == 4 ? "SAVE_NO_TERRAIN_CHANGE" : "TERRAIN_JOURNAL_PALETTE_DRIFT");
                }), "CAS and no-op preserve explicit diagnostics");
            }
        }
        {
            // 同一批：先追加 Tg02，再把 Tg01 槽上的全部顶点改用 Tg02；表不变，只改顶点的地面索引。
            auto request = fixture.Request("remap");
            Services::MapEditTerrainStroke remapStroke; remapStroke.textureSlot = remap;
            request.terrainStrokes = {Command(true, TerrainTextureTable::Ground, 1, "", "Tg02"), remapStroke};
            (void)Take(SaveMapEditCandidate(request));
            auto reopened = Take(MapArchiveSession::Open(request.targetCandidate));
            auto original = Take(MapArchiveSession::Open(fixture.source.path));
            const auto after = Take(MapData::ParseMapTerrain(Take(reopened.ReadMember("war3map.w3e"))));
            const auto before = Take(MapData::ParseMapTerrain(Take(original.ReadMember("war3map.w3e"))));
            const auto& a = after.Snapshot()->vertices; const auto& b = before.Snapshot()->vertices;
            Require(after.Snapshot()->header.groundTilesets == std::vector<std::string>{"Tg01", "Tg02"} && a.size() == b.size(),
                "remap save keeps the table");
            for (std::size_t i = 0; i < a.size(); ++i)
                Require(a[i].GroundTextureIndex() == 1 && (a[i].textureAndFlagsRaw & 0xf0) == (b[i].textureAndFlagsRaw & 0xf0)
                    && a[i].variationRaw == b[i].variationRaw && a[i].groundHeightRaw == b[i].groundHeightRaw
                    && a[i].cliffAndLayerRaw == b[i].cliffAndLayerRaw, "remap save changes only the ground index");
            request.targetCandidate = fixture.root / "remap-missing.w3x";
            request.terrainStrokes = {remapStroke};
            const auto missing = SaveMapEditCandidate(request);
            Require(!missing.Succeeded() && !missing.value && !std::filesystem::exists(request.targetCandidate)
                && std::any_of(missing.diagnostics.begin(), missing.diagnostics.end(), [](const auto& d) {
                    return d.code == "TERRAIN_TILESET_REMAP_TARGET_MISSING"; }), "remap to a tile outside the table saves nothing");
        }
        Require(Take(Sha256File(fixture.source.path)) == fixture.source.sha256, "source bytes unchanged throughout");
        std::cout << "Typed palette CAS, ground remap, atomic save, readback and no-op passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
