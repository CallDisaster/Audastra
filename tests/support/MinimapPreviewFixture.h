#pragma once

#include "War3MapCore/MapData/TerrainEditing.h"
#include "War3MapCore/MapArchiveSession.h"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

namespace War3Map::Tests::MinimapPreviewFixtures {
using namespace MapData;
using Bytes = std::vector<std::byte>;
inline void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
template<class T> T Take(Result<T> result) {
    Require(result.Succeeded() && result.value.has_value(), "原创小地图夹具未能构建");
    return std::move(*result.value);
}

inline Bytes Text(std::string_view text) {
    Bytes bytes;
    for (const auto value : text) bytes.push_back(static_cast<std::byte>(value));
    return bytes;
}
inline void Put32(Bytes& bytes, std::size_t offset, std::uint32_t value) {
    Require(offset <= bytes.size() && bytes.size() - offset >= 4, "原创样例字段有界");
    for (unsigned index = 0; index < 4; ++index) bytes[offset + index] = static_cast<std::byte>((value >> (index * 8)) & 255);
}
inline Bytes Slk(const std::vector<std::string>& headers, const std::vector<std::vector<std::string>>& rows = {}) {
    std::string text = "ID;PWXL;N;E\n";
    const auto row = [&](const std::vector<std::string>& values, std::size_t y) {
        for (std::size_t x = 0; x < values.size(); ++x)
            text += "C;X" + std::to_string(x + 1) + ";Y" + std::to_string(y) + ";K\"" + values[x] + "\"\n";
    };
    row(headers, 1);
    for (std::size_t index = 0; index < rows.size(); ++index) row(rows[index], index + 2);
    return Text(text + "E\n");
}
inline Bytes Atlas(std::uint8_t red = 80) {
    // 手写原创 BLP1 palette+mips；输入不由被测JPEG编码器生成。
    Bytes bytes(1180 + 341);
    const auto magic = Text("BLP1"); std::copy(magic.begin(), magic.end(), bytes.begin());
    Put32(bytes, 4, 1); Put32(bytes, 12, 16); Put32(bytes, 16, 16); Put32(bytes, 20, 5); Put32(bytes, 24, 1);
    bytes[156] = std::byte{180}; bytes[157] = std::byte{120}; bytes[158] = static_cast<std::byte>(red);
    std::size_t offset = 1180;
    for (std::uint32_t level = 0, side = 16;; ++level, side /= 2) {
        Put32(bytes, 28 + level * 4, static_cast<std::uint32_t>(offset)); Put32(bytes, 92 + level * 4, side * side);
        offset += static_cast<std::size_t>(side) * side;
        if (side == 1) break;
    }
    return bytes;
}
inline std::vector<MapArchiveMember> Materials() {
    return {{"TerrainArt\\Terrain.slk", Slk({"tileID", "dir", "file"}, {{"Tg01", "Fixture", "Ground"}})},
        {"TerrainArt\\CliffTypes.slk", Slk({"cliffID", "texDir", "texFile", "cliffModelDir", "rampModelDir", "groundTile", "cliffClass"},
            {{"CLdi", "Fixture", "Cliff", "Cliff", "Ramp", "Tg01", "0"}})},
        {"TerrainArt\\Water.slk", Slk({"waterID"})}, {"Fixture\\Ground.blp", Atlas()},
        {"Fixture\\Cliff.blp", Atlas()}, {"UI\\MiscData.txt", Text("[Water]\nDeepLevelPathing=64\n")},
        {"UI\\WorldEditData.txt", Text("[CameraMargins]\nLeft=0\nRight=0\nBottom=0\nTop=0\n")}};
}
inline std::vector<MapArchiveMember> MapMembers() {
    TerrainGridSnapshot grid;
    grid.header.width = grid.header.height = 9; grid.header.mainTileset = 'L';
    grid.header.groundTilesets = {"Tg01"}; grid.header.cliffTilesets = {"CLdi"}; grid.vertices.resize(81);
    for (auto& vertex : grid.vertices) {
        vertex.groundHeightRaw = 8192; vertex.waterHeightAndFlagsRaw = 7936; vertex.cliffAndLayerRaw = 2;
    }
    RefreshTerrainDerived(grid, 0, 0, 9, 9);
    Bytes settings(147); Put32(settings, 0, 25); Put32(settings, 8, 6061);
    Put32(settings, 48, 1); Put32(settings, 52, 3); Put32(settings, 56, 2); Put32(settings, 60, 1);
    Put32(settings, 64, 4); Put32(settings, 68, 5); Put32(settings, 72, 0x400); settings[76] = std::byte{'L'};
    auto pathing = Text("MP3W"); pathing.resize(16 + 32 * 32, std::byte{0x82});
    Put32(pathing, 4, 0); Put32(pathing, 8, 32); Put32(pathing, 12, 32);
    auto members = Materials();
    members.push_back({"war3map.w3e", Take(SerializeMapTerrain(grid))});
    members.push_back({"war3map.wpm", std::move(pathing)});
    members.push_back({"war3map.w3i", std::move(settings)});
    Bytes native(12); Put32(native, 0, 1); members.push_back({"war3map.w3u", std::move(native)});
    members.push_back({"unrelated.bin", Text("unrelated first value")});
    return members;
}
} // namespace War3Map::Tests::MinimapPreviewFixtures
