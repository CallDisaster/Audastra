#pragma once

#include "TerrainBoundsFixture.h"
#include "../TerrainCornerRampFixtures.h"
#include "War3MapCore/IO/FileBytes.h"

#include <bit>

namespace AudastraTest::TerrainResize {
using namespace War3Map;
using namespace War3Map::MapData;
using TerrainBounds::Bytes;
using TerrainBounds::Check;
using TerrainBounds::HasCode;
using TerrainBounds::Require;
using TerrainBounds::Take;
using TerrainBounds::Text;
using War3Map::TerrainCornerRampFixtures::Slk;

inline void Put(Bytes& bytes, std::uint32_t value, unsigned count = 4) {
    for (unsigned i = 0; i < count; ++i) bytes.push_back(std::byte((value >> (i * 8)) & 255));
}

// 原创32格地图；完整expected独立写出，不调用resize、地形writer或通行生成器。
// 原可玩区始终为世界[-1920,1792)²；四边参数相对原32格canvas。
inline Bytes TerrainLiteral(int west = 0, int south = 0, int east = 32, int north = 32) {
    Require(west < east && south < north, "nonempty independent canvas");
    const auto width = unsigned(east - west), height = unsigned(north - south);
    Bytes bytes = Text("W3E!"); Put(bytes, 11); Put(bytes, 'L', 1); Put(bytes, 0);
    Put(bytes, 1); const auto ground = Text("Tg01"); bytes.insert(bytes.end(), ground.begin(), ground.end());
    Put(bytes, 1); const auto cliff = Text("Cl01"); bytes.insert(bytes.end(), cliff.begin(), cliff.end());
    Put(bytes, width + 1); Put(bytes, height + 1);
    Put(bytes, std::bit_cast<std::uint32_t>(float(-2048 + west * 128)));
    Put(bytes, std::bit_cast<std::uint32_t>(float(-2048 + south * 128)));
    for (unsigned y = 0; y <= height; ++y) for (unsigned x = 0; x <= width; ++x) {
        const auto sx = std::clamp(int(x) + west, 0, 32), sy = std::clamp(int(y) + south, 0, 32);
        bool edge = sx < 32 && sy < 32 && (sx < 1 || sx >= 30 || sy < 1 || sy >= 30);
        if (x < width && y < height)
            edge = int(x) + west < 1 || int(x) + west >= 30 || int(y) + south < 1 || int(y) + south >= 30;
        Put(bytes, 8192, 2); Put(bytes, 8192u | (edge ? 0x4000u : 0), 2);
        Put(bytes, 0, 1); Put(bytes, unsigned((sx + sy * 3) % 16), 1); Put(bytes, 2, 1);
    }
    return bytes;
}

inline Bytes PathingLiteral(int west = 0, int south = 0, int east = 32, int north = 32) {
    Require(west < east && south < north, "nonempty independent pathing canvas");
    const auto width = unsigned(east - west) * 4, height = unsigned(north - south) * 4;
    Bytes bytes = Text("MP3W"); Put(bytes, 0); Put(bytes, width); Put(bytes, height);
    for (unsigned y = 0; y < height; ++y) for (unsigned x = 0; x < width; ++x) {
        const auto oldX = int(x) + west * 4, oldY = int(y) + south * 4;
        const bool authored = oldX >= 0 && oldX < 128 && oldY >= 0 && oldY < 128;
        const bool outside = oldX < 4 || oldX >= 120 || oldY < 4 || oldY >= 120;
        bytes.push_back(std::byte(0x40 | (authored ? 0x11 : 0) | (outside ? 0x8e : 0)));
    }
    return bytes;
}

inline Bytes ShadowLiteral(int west = 0, int south = 0, int east = 32, int north = 32, bool resized = false) {
    Require(west < east && south < north, "nonempty independent shadow canvas");
    const auto width = unsigned(east - west) * 4, height = unsigned(north - south) * 4;
    Bytes bytes; bytes.reserve(std::size_t(width) * height);
    constexpr std::array<unsigned, 5> raw{0, 1, 2, 0xff, 0x80};
    for (unsigned y = 0; y < height; ++y) for (unsigned x = 0; x < width; ++x) {
        const auto oldX = int(x) + west * 4, oldY = int(y) + south * 4;
        const bool overlap = oldX >= 0 && oldX < 128 && oldY >= 0 && oldY < 128;
        unsigned value = overlap ? raw[unsigned(oldX + oldY * 3) % raw.size()] : 0;
        // 原版磁盘loader先将任何非零字节置为runtime bit0，writer再输出00/FF。
        if (resized) value = value ? 0xff : 0;
        bytes.push_back(std::byte(value));
    }
    return bytes;
}

struct Fixture {
    std::filesystem::path directory, gameRoot;
    TerrainGridSnapshot grid;
    MapSettings settings;
    UnitPlacements units;
    DoodadPlacements doodads;
    RegionsDocument regions;
    CamerasDocument cameras;
    std::vector<MapArchiveMember> members;
    std::unique_ptr<MapArchiveSession> archive;
    std::unique_ptr<MapDataEditSession> edits;

    explicit Fixture(bool spatial = false, bool shadow = true) {
        static std::atomic<unsigned> next{0};
        directory = std::filesystem::temp_directory_path() / ("Audastra-resize-"
            + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()) + "-" + std::to_string(next++));
        Require(std::filesystem::create_directory(directory), "private resize fixture directory");
        gameRoot = directory / "game";
        Require(std::filesystem::create_directory(gameRoot), "private resize startup directory");
        Check(IO::WriteFileBytes(gameRoot / "WorldEditData.txt", Text(
            "[WorldEditMisc]\nMinimumMapSize=32\nMaximumMapSize=480\n"
            "[CameraMargins]\nLeft=1\nRight=2\nBottom=1\nTop=2\n")));
        grid = *Take(ParseMapTerrain(TerrainLiteral())).Snapshot();
        settings.formatVersion = 25; settings.custom.emplace(); settings.tileset = 'L';
        settings.mapName = "Synthetic terrain canvas"; settings.author = "Audastra tests";
        settings.flags = 0x400; settings.cameraMargins = {1, 2, 1, 2};
        settings.playableWidth = settings.playableHeight = 29;
        settings.cameraBounds = {-1792, -1792, 1536, 1536, -1792, 1536, 1536, -1792};
        cameras.layout = CameraLayout::Classic;
        if (spatial) {
            UnitPlacement unit; unit.rawcode = "u001"; unit.creationId = 17;
            unit.position = {0, 64, 7}; unit.scale = {1, 1, 1}; units.units.push_back(unit);
            DoodadPlacement doodad; doodad.rawcode = "Dst0"; doodad.creationId = 29;
            doodad.position = {128, -64, 11}; doodad.scale = {1, 1, 1}; doodads.doodads.push_back(doodad);
            RegionRecord region{}; region.name = "retained region"; region.creationNumber = 41;
            region.left = -256; region.bottom = -128; region.right = 384; region.top = 256;
            region.colorBgra = {13, 23, 37, 0xff}; regions.entries.push_back(region);
            CameraRecord camera{}; camera.name = "retained camera"; camera.targetX = 64; camera.targetY = 128;
            camera.rotation = 37; camera.angleOfAttack = 304; camera.targetDistance = 1650;
            camera.fieldOfView = 70; camera.farClippingPlane = 5000; camera.nearClippingPlane = 100;
            cameras.entries.push_back(camera);
        }
        members = {{"war3map.w3e", TerrainLiteral()}, {"war3map.wpm", PathingLiteral()},
            {"war3map.w3i", Take(settings.Serialize())}, {"war3mapUnits.doo", Take(units.Serialize())},
            {"war3map.doo", Take(doodads.Serialize())}, {"war3map.w3r", Take(regions.Serialize())},
            {"war3map.w3c", Take(cameras.Serialize())},
            {"war3map.j", Text("function config takes nothing returns nothing\nendfunction\n"
                "function main takes nothing returns nothing\nendfunction\n")},
            {"war3mapMap.tga", Text("authored minimap kept verbatim")},
            {"TerrainArt/Terrain.slk", Slk({"tileID", "walkable", "flyable", "buildable"}, {{"Tg01", "1", "1", "1"}})},
            {"TerrainArt/Water.slk", Slk({"waterID", "height", "impassable"}, {{"LSha", "0", "0"}})},
            {"TerrainArt/CliffTypes.slk", Slk({"cliffID"}, {{"Cl01"}})},
            {"UI/MiscData.txt", Text("[Terrain]\nMaxSlope=90\nMinHeight=-512\nMaxHeight=3328\n"
                "[Water]\nDeepLevelPathing=128\n[CameraMargins]\nLeft=1\nRight=2\nBottom=1\nTop=2\n")},
            {"UI/WorldEditData.txt", Text("[Terrain]\nMaxHeight=3328\nMinHeight=-512\n")},
            {"Units/DestructableData.slk", Slk({"destructableID", "code", "walkable", "pathTex"}, {{"Dst0", "Dst0", "0", "_"}})},
            {"Doodads/Doodads.slk", Slk({"doodID", "code"}, {{"Doo0", "Doo0"}})},
            {"Units/DestructableMetaData.slk", Slk({"ID", "field", "slk", "type", "repeat", "data", "index"},
                {{"dwlk", "walkable", "DestructableData", "bool", "0", "0", "-1"}})}};
        if (shadow) members.push_back({"war3map.shd", ShadowLiteral()});
        Seal();
    }

    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;
    ~Fixture() {
        archive.reset(); edits.reset();
        std::error_code error;
        const auto temporary = std::filesystem::temp_directory_path(error);
        if (!error && directory.parent_path() == temporary && directory.filename().string().starts_with("Audastra-resize-"))
            std::filesystem::remove_all(directory, error);
    }

    Bytes& Member(std::string_view name) {
        const auto it = std::find_if(members.begin(), members.end(), [&](const auto& item) { return item.name == name; });
        Require(it != members.end(), "fixture member exists"); return it->data;
    }
    void PutMember(std::string_view name, Bytes bytes) {
        const auto it = std::find_if(members.begin(), members.end(), [&](const auto& item) { return item.name == name; });
        if (it == members.end()) members.push_back({std::string(name), std::move(bytes)});
        else it->data = std::move(bytes);
    }
    void EraseMember(std::string_view name) {
        std::erase_if(members, [&](const auto& item) { return item.name == name; });
    }
    [[nodiscard]] std::unique_ptr<MapDataEditSession> FreshSession() const {
        auto session = std::make_unique<MapDataEditSession>(MapDocument::FromMembers(members));
        const auto read = [&](std::string_view name) -> const Bytes& {
            const auto it = std::find_if(members.begin(), members.end(), [&](const auto& item) { return item.name == name; });
            Require(it != members.end(), "source terrain/pathing member exists"); return it->data;
        };
        Check(session->AttachTerrain(read("war3map.w3e")));
        Check(session->AttachPathing(read("war3map.wpm"))); return session;
    }
    // 每次独立归档名，既不覆盖作者文件，也不覆盖以前的原创输入。
    void Seal() {
        const auto path = directory / ("source-" + std::to_string(nextArchive_++) + ".w3x");
        archive = std::make_unique<MapArchiveSession>(Take(MapArchiveSession::Open(Take(WriteVerifiedArchive(path, members)).path)));
        edits = FreshSession();
    }
private:
    unsigned nextArchive_ = 0;
};

} // namespace AudastraTest::TerrainResize
