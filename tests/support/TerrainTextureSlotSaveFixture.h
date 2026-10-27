#pragma once
#include "../AuxiliarySaveFixtures.h"
#include "War3MapCore/MapData/MapTerrain.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/Services/MapEditSaveService.h"
#include "War3MapCore/Resources/ImportTable.h"
#include <Windows.h>
#include <algorithm>

namespace AudastraTest::TextureSlot {
using namespace War3Map;
using namespace AuxiliarySaveFixtures;
inline std::vector<std::byte> Slk(const std::vector<std::string>& columns,
    std::initializer_list<std::vector<std::string>> rows) {
    std::string text = "ID;PWXL;N;E\n";
    const auto row = [&](const auto& values, unsigned y) {
        for (unsigned x = 0; x < values.size(); ++x)
            text += "C;X" + std::to_string(x + 1) + ";Y" + std::to_string(y) + ";K\"" + values[x] + "\"\n";
    };
    row(columns, 1); unsigned y = 2; for (const auto& values : rows) row(values, y++);
    return Bytes(text + "E\n");
}
struct Fixture {
    std::filesystem::path root;
    OriginalInputs inputs;
    ArchiveSaveResult source;
    std::string terrainSha;
    Fixture() {
        root = std::filesystem::temp_directory_path() / ("Audastra-palette-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
        Require(std::filesystem::create_directory(root), "exclusive palette fixture root");
        inputs = CreateOriginalInputs(root);
        MapData::TerrainGridSnapshot grid;
        grid.header.width = grid.header.height = 9; grid.header.mainTileset = 'L';
        grid.header.offset = {-512, -512}; grid.header.groundTilesets = {"Tg01"}; grid.header.cliffTilesets = {"Cl01"}; grid.vertices.resize(81);
        for (auto& v : grid.vertices) { v.groundHeightRaw = 8192; v.waterHeightAndFlagsRaw = 7936; v.cliffAndLayerRaw = 2; }
        MapData::RefreshTerrainDerived(grid, 0, 0, 9, 9);
        const auto terrain = Take(MapData::SerializeMapTerrain(grid)); terrainSha = Take(Sha256(terrain));
        auto members = inputs.members;
        MapData::MapSettings settings; settings.formatVersion = 25; settings.tileset = 'L'; settings.custom.emplace();
        settings.playableWidth = settings.playableHeight = 8; settings.cameraBounds = {-512, -512, 512, 512, -512, 512, 512, -512};
        std::find_if(members.begin(), members.end(), [](const auto& m) { return m.name == "war3map.w3i"; })->data = Take(settings.Serialize());
        auto wpm = Bytes("MP3W");
        for (const unsigned v : {0u, 32u, 32u}) for (unsigned shift = 0; shift < 32; shift += 8) wpm.push_back(std::byte((v >> shift) & 255));
        wpm.resize(16 + 32 * 32, std::byte{0x40});
        members.push_back({"war3map.w3e", terrain}); members.push_back({"war3map.wpm", wpm});
        // 原创1x1索引BLP及真实导入记录；这组用例验证槽表保存，不要求缺图生成器猜测材质。
        std::vector<std::byte> minimap(1181);
        const auto magic = Bytes("BLP1"); std::copy(magic.begin(), magic.end(), minimap.begin());
        const auto put32 = [&](unsigned at, unsigned value) {
            for (unsigned shift = 0; shift < 32; shift += 8) minimap[at++] = std::byte((value >> shift) & 255);
        };
        put32(4, 1); put32(12, 1); put32(16, 1); put32(20, 5); put32(24, 1); put32(28, 1180); put32(92, 1);
        minimap[157] = std::byte{128}; members.push_back({"war3mapMap.blp", std::move(minimap)});
        auto imports = Resources::ImportTable::CreateEmpty(); Require(imports.Upsert("war3mapMap.blp").Succeeded(), "owned minimap import");
        members.push_back({"war3map.imp", Take(imports.Serialize())});
        const std::vector<MapArchiveMember> resources{
            {"TerrainArt/Terrain.slk", Slk({"tileID", "walkable", "flyable", "buildable"},
                {{"Tg01", "1", "1", "1"}, {"Tg02", "1", "1", "1"}, {"Tg03", "1", "1", "1"}, {"Tg04", "1", "1", "1"}})},
            {"TerrainArt/CliffTypes.slk", Slk({"cliffID"}, {{"Cl01"}, {"Cl02"}, {"Cl03"}, {"Cl04"}})},
            {"TerrainArt/Water.slk", Slk({"waterID", "height", "impassable"}, {{"LSha", "0", "0"}})},
            {"UI/MiscData.txt", Bytes("[Terrain]\nMaxSlope=90\nMinHeight=-256\nMaxHeight=1536\n[Water]\nDeepLevelPathing=128\n[CameraMargins]\nLeft=0\nRight=0\nBottom=0\nTop=0\n")},
            {"UI/WorldEditData.txt", Bytes("[Other]\nValue=1\n")},
            {"Units/DestructableData.slk", Slk({"destructableID", "code", "walkable", "pathTex", "fixedRot"}, {})},
            {"Units/DestructableMetaData.slk", Slk({"ID", "field", "slk", "type", "repeat", "data", "index"},
                {{"bptx", "pathTex", "DestructableData", "pathingTexture", "0", "0", "0"}})},
            {"Doodads/Doodads.slk", Slk({"doodID", "code", "pathTex", "category", "fixedRot"}, {})},
            {"Doodads/DoodadMetaData.slk", Slk({"ID", "field", "slk", "type", "repeat", "index"},
                {{"dpt0", "pathTex", "DoodadData", "pathingTexture", "0", "0"}})}};
        members.insert(members.end(), resources.begin(), resources.end());
        auto game = GameFiles(JassDeclarations()); for (const auto& resource : resources) game.push_back({resource.name, resource.data, false});
        inputs.game = root / "native-game"; Require(std::filesystem::create_directory(inputs.game), "owned resource directory");
        Require(Mpq::WriteMpqArchive(inputs.game / "war3.mpq", game).Succeeded(), "owned synthetic resources");
        source = Take(WriteVerifiedArchive(root / "native-source.w3x", members));
    }
    ~Fixture() { std::error_code error; std::filesystem::remove_all(root, error); }
    Services::MapEditSaveRequest Request(std::string_view name) const {
        Services::MapEditSaveRequest request; request.sourceMap = source.path; request.expectedSourceSha256 = source.sha256;
        request.expectedTerrainSha256 = terrainSha; request.targetCandidate = root / (std::string(name) + ".w3x");
        request.toolRoot = inputs.tool; request.gameRoot = inputs.game; request.minimapChoice = Services::TerrainMinimapSaveChoice::Preserve;
        return request;
    }
};
inline Services::MapEditTerrainStroke Command(bool append, MapData::TerrainTextureTable table,
    unsigned slot, std::string oldValue, std::string newValue) {
    Services::MapEditTerrainStroke stroke;
    stroke.textureSlot = Services::TerrainTextureSlotCommand{append ? Services::TerrainTextureSlotOperation::Append : Services::TerrainTextureSlotOperation::Replace,
        table, slot, std::move(oldValue), std::move(newValue)}; return stroke;
}
} // namespace AudastraTest::TextureSlot
