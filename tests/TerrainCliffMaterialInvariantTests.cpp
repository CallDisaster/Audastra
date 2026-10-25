#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/MapData/MapPlacements.h"
#include "War3MapCore/MapData/TerrainPathingResources.h"
#include "War3MapCore/MapData/TerrainSavePlan.h"
#include "War3MapCore/Mpq/MpqArchive.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace {
using namespace War3Map;
using namespace War3Map::MapData;
using Bytes = std::vector<std::byte>;

void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
template<class T> T Take(Result<T> result) {
    if (!result.Succeeded() || !result.value) {
        std::string message = "fixture operation failed";
        for (const auto& diagnostic : result.diagnostics)
            message += " | " + diagnostic.code + ": " + diagnostic.message;
        throw std::runtime_error(message);
    }
    return std::move(*result.value);
}
void Check(Status result) {
    if (!result.Succeeded()) {
        std::string message = "fixture status failed";
        for (const auto& diagnostic : result.diagnostics)
            message += " | " + diagnostic.code + ": " + diagnostic.message;
        throw std::runtime_error(message);
    }
}
Bytes Text(std::string_view text) {
    return {reinterpret_cast<const std::byte*>(text.data()), reinterpret_cast<const std::byte*>(text.data() + text.size())};
}
class Directory {
public:
    Directory() {
        static std::atomic<unsigned> sequence{0};
        path = std::filesystem::temp_directory_path() / ("Audastra-cliff-material-"
            + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64())
            + "-" + std::to_string(sequence.fetch_add(1)));
        Require(std::filesystem::create_directory(path), "private fixture directory");
    }
    ~Directory() { std::error_code error; std::filesystem::remove_all(path, error); }
    std::filesystem::path path;
};
Bytes CliffTable(bool first = true, bool second = true) {
    std::string text = "ID;PWXL;N;E\nB;X1;Y3;D0\nC;X1;Y1;K\"cliffID\"\n";
    if (first) text += "C;X1;Y2;K\"Cl01\"\n";
    if (second) text += "C;X1;Y3;K\"Cl02\"\n";
    return Text(text + "E\n");
}
Bytes GroundTable() {
    return Text("ID;PWXL;N;E\nB;X4;Y3;D0\n"
        "C;X1;Y1;K\"tileID\"\nC;X2;K\"walkable\"\nC;X3;K\"flyable\"\nC;X4;K\"buildable\"\n"
        "C;X1;Y2;K\"Tg01\"\nC;X2;K1\nC;X3;K1\nC;X4;K1\n"
        "C;X1;Y3;K\"Tg02\"\nC;X2;K0\nC;X3;K1\nC;X4;K0\nE\n");
}
struct Fixture {
    static constexpr std::size_t target = 4 * 9 + 4;
    TerrainGridSnapshot grid;
    Bytes w3e, wpm;
    std::vector<MapArchiveMember> members;
    Fixture() {
        grid.header.width = grid.header.height = 9;
        grid.header.mainTileset = 'L';
        grid.header.groundTilesets = {"Tg01", "Tg02"};
        grid.header.cliffTilesets = {"Cl01", "Cl02"};
        grid.vertices.resize(81);
        for (std::size_t i = 0; i < grid.vertices.size(); ++i) {
            auto& vertex = grid.vertices[i];
            vertex.groundHeightRaw = 8192;
            vertex.waterHeightAndFlagsRaw = 7936;
            vertex.cliffAndLayerRaw = i % 9 >= 4 ? 3 : 2;
        }
        wpm = Text("MP3W");
        for (const auto value : {0u, 32u, 32u}) for (unsigned shift = 0; shift < 32; shift += 8)
            wpm.push_back(static_cast<std::byte>((value >> shift) & 0xff));
        wpm.resize(16 + 32 * 32);
        // 故意保留原生、作者限制和未知位；此测试不能靠重建清零获得通过。
        for (std::size_t i = 16; i < wpm.size(); ++i) wpm[i] = static_cast<std::byte>((i * 53 + 137) & 0xff);
        DoodadPlacements doodads;
        DoodadPlacement object;
        object.rawcode = "Dx01"; object.scale = {1, 1, 1}; object.position = {320, 320, 0}; object.creationId = 19;
        doodads.doodads.push_back(object);
        UnitPlacements units;
        UnitPlacement unit;
        unit.rawcode = "Ux01"; unit.scale = {1, 1, 1}; unit.position = {256, 256, 0}; unit.creationId = 23;
        units.units.push_back(unit);
        members = {{"war3map.w3e", {}}, {"war3map.wpm", wpm}, {"war3map.doo", Take(doodads.Serialize())},
            {"war3mapUnits.doo", Take(units.Serialize())},
            {"war3map.w3d", Text("opaque custom object bytes: no material-only reinterpretation")},
            {"TerrainArt/CliffTypes.slk", CliffTable()}, {"TerrainArt/Terrain.slk", GroundTable()},
            {"war3mapMap.tga", Text("author minimap")}};
        Sync();
    }
    void Sync() {
        RefreshTerrainDerived(grid, 0, 0, 9, 9);
        w3e = Take(SerializeMapTerrain(grid)); members[0].data = w3e;
    }
    MapArchiveSession Write(const std::filesystem::path& path) const {
        return Take(MapArchiveSession::Open(Take(WriteVerifiedArchive(path, members)).path));
    }
    void Attach(MapDataEditSession& edits) const { Check(edits.AttachTerrain(w3e)); Check(edits.AttachPathing(wpm)); }
    TerrainGridSnapshot Changed(bool material = true, bool variation = false) const {
        auto after = grid;
        if (material) after.vertices[target].cliffAndLayerRaw = static_cast<std::uint8_t>((after.vertices[target].cliffAndLayerRaw & 0x0f) | 0x10);
        if (variation) after.vertices[target].variationRaw |= 0x40;
        return after;
    }
    void Set(MapDataEditSession& edits, const TerrainGridSnapshot& after) const {
        Check(edits.SetTerrain(Take(SerializeMapTerrain(after))));
    }
};
bool Refused(const Result<TerrainSavePlan>& result) {
    return !result.Succeeded() || !result.value || !result.value->SaveGate().Succeeded();
}

void MaterialAndVariationSaveWithEveryOriginalPathingByte() {
    Directory directory;
    for (unsigned mode = 1; mode <= 3; ++mode) {
        Fixture fixture;
        const auto source = fixture.Write(directory.path / (std::to_string(mode) + ".w3x"));
        MapDataEditSession edits(MapDocument::FromMembers(fixture.members)); fixture.Attach(edits);
        const auto after = fixture.Changed((mode & 1) != 0, (mode & 2) != 0);
        fixture.Set(edits, after);
        Require(TerrainPathingResources::Needed(fixture.grid, after), "material and same-slot variation require actual resource capture");
        Require(!edits.VerifyTerrainPathingForSave().Succeeded(), "resource-free gate cannot bless a cliff row");
        const auto resources = Take(TerrainPathingResources::Capture(source, fixture.grid, after, {}, 1024 * 1024));
        Require(resources.CapturedCliffMaterials("Cl01", (mode & 1) ? "Cl02" : "Cl01"), "both actual requested rows are captured");
        Require(!resources.CapturedCliffMaterials("Cl01", "ZZZZ"), "uncaptured rawcode has no qualification");
        const auto plan = Take(TerrainSavePlan::PrepareForSave(source, edits, {})); Check(plan.SaveGate());
        Require(!plan.RebuiltPathing() && plan.Replacements().size() == 1, "material-only save must not rebuild or replace WPM");
        Check(plan.VerifyForSave(edits, source.SourceSha256(), edits.Replacements()));
        const auto saved = Take(MapArchiveSession::Open(Take(source.SaveCandidate(
            directory.path / (std::to_string(mode) + "-saved.w3x"), plan.Replacements())).path));
        Check(plan.VerifyReadback(saved));
        Require(Take(saved.ReadMember("war3map.w3e")) == Take(SerializeMapTerrain(after)), "saved W3E equals independent exact target");
        for (const auto& member : fixture.members) if (member.name != "war3map.w3e")
            Require(Take(saved.ReadMember(member.name)) == member.data, "complete WPM and unchanged object/resource members survive byte for byte");
        Require(Take(source.ReadMember("war3map.w3e")) == fixture.w3e, "source terrain stays read only");
    }
}

void MixedGeometryFlagsGroundAndInvalidMaterialsCannotBorrowTheProof() {
    Directory directory;
    enum class Change { Level, Ramp, Boundary, MapEdge, UnknownWater, Water, Ground, GroundVariation,
        OldVariation, NewVariation, OldSlot15, NewSlot15, OldOutOfBounds, NewOutOfBounds, Palette };
    unsigned ordinal = 0;
    for (const auto change : {Change::Level, Change::Ramp, Change::Boundary, Change::MapEdge, Change::UnknownWater,
        Change::Water, Change::Ground, Change::GroundVariation, Change::OldVariation, Change::NewVariation,
        Change::OldSlot15, Change::NewSlot15, Change::OldOutOfBounds, Change::NewOutOfBounds, Change::Palette}) {
        Fixture fixture;
        if (change == Change::OldVariation) fixture.grid.vertices[Fixture::target].variationRaw = 0x60;
        if (change == Change::OldSlot15) fixture.grid.vertices[Fixture::target].cliffAndLayerRaw |= 0xf0;
        if (change == Change::OldOutOfBounds) fixture.grid.vertices[Fixture::target].cliffAndLayerRaw |= 0x20;
        fixture.Sync();
        auto after = fixture.Changed(); auto& vertex = after.vertices[Fixture::target];
        if (change == Change::Level) vertex.cliffAndLayerRaw = 0x14;
        if (change == Change::Ramp) vertex.textureAndFlagsRaw |= 0x10;
        if (change == Change::Boundary) vertex.textureAndFlagsRaw |= 0x80;
        if (change == Change::MapEdge) vertex.waterHeightAndFlagsRaw |= 0x4000;
        if (change == Change::UnknownWater) vertex.waterHeightAndFlagsRaw |= 0x8000;
        if (change == Change::Water) vertex.textureAndFlagsRaw |= 0x40;
        if (change == Change::Ground) vertex.textureAndFlagsRaw |= 1;
        if (change == Change::GroundVariation) vertex.variationRaw = 18;
        if (change == Change::OldVariation) vertex.variationRaw = 0;
        if (change == Change::NewVariation) vertex.variationRaw = 0x60;
        if (change == Change::NewSlot15) vertex.cliffAndLayerRaw |= 0xf0;
        if (change == Change::NewOutOfBounds) vertex.cliffAndLayerRaw = 0x23;
        if (change == Change::Palette) after.header.cliffTilesets[1] = "ZZZZ";
        const auto source = fixture.Write(directory.path / (std::to_string(ordinal++) + ".w3x"));
        MapDataEditSession edits(MapDocument::FromMembers(fixture.members)); fixture.Attach(edits); fixture.Set(edits, after);
        Require(Refused(TerrainSavePlan::PrepareForSave(source, edits, {})), "mixed semantic or unsupported cliff change must not preserve arbitrary old WPM");
    }
}

void BothCliffRowsAreMandatoryEvenForSameSlotVariation() {
    Directory directory;
    unsigned ordinal = 0;
    for (const bool variationOnly : {false, true}) for (const bool missingOld : {false, true}) {
        Fixture fixture;
        fixture.members[5].data = CliffTable(!missingOld, missingOld);
        if (variationOnly && !missingOld) fixture.members.erase(fixture.members.begin() + 5);
        const auto source = fixture.Write(directory.path / (std::to_string(ordinal++) + ".w3x"));
        const auto after = fixture.Changed(!variationOnly, variationOnly);
        Require(!TerrainPathingResources::Capture(source, fixture.grid, after, {}, 1024 * 1024).Succeeded(),
            "missing old/new actual cliff row cannot qualify the invariant");
        MapDataEditSession edits(MapDocument::FromMembers(fixture.members)); fixture.Attach(edits); fixture.Set(edits, after);
        Require(Refused(TerrainSavePlan::PrepareForSave(source, edits, {})), "save does not silently default missing cliff data");
    }
    Fixture fixture; const auto source = fixture.Write(directory.path / "budget.w3x");
    Require(!TerrainPathingResources::Capture(source, fixture.grid, fixture.Changed(), {}, 1).Succeeded(),
        "cliff capture obeys actual source budget");
}

void PlacementAndRevisionChecksRemainRequiredForSlotOnlyChanges() {
    Directory directory;
    for (const bool unit : {false, true}) {
        Fixture fixture; const auto source = fixture.Write(directory.path / (unit ? "units.w3x" : "doodads.w3x"));
        MapDataEditSession edits(MapDocument::FromMembers(fixture.members)); fixture.Attach(edits); fixture.Set(edits, fixture.Changed());
        if (unit) {
            auto placements = Take(ParseUnitPlacements(fixture.members[3].data)); placements.units[0].position[0] += 32;
            Check(edits.EditUnits(placements));
        } else {
            auto placements = Take(ParseDoodadPlacements(fixture.members[2].data)); placements.doodads[0].position[0] += 32;
            Check(edits.EditDoodads(placements));
        }
        Require(Refused(TerrainSavePlan::PrepareForSave(source, edits, {})), "material-only proof cannot hide changed XY placement");
    }
    Fixture fixture; const auto source = fixture.Write(directory.path / "revision.w3x");
    MapDataEditSession edits(MapDocument::FromMembers(fixture.members)); fixture.Attach(edits); fixture.Set(edits, fixture.Changed());
    const auto plan = Take(TerrainSavePlan::PrepareForSave(source, edits, {})); Check(plan.SaveGate());
    Require(!plan.VerifyForSave(edits, std::string(64, 'f'), edits.Replacements()).Succeeded(), "different source identity cannot reuse material proof");
    auto after = fixture.Changed(); after.header.cliffTilesets[1] = "ZZZZ"; fixture.Set(edits, after);
    Require(!plan.VerifyForSave(edits, source.SourceSha256(), edits.Replacements()).Succeeded(), "later palette/revision cannot reuse prior captured rows");
}

void FinalMembersReadbackAndActualGameWinnerStayBound() {
    Directory directory; Fixture fixture;
    const auto source = fixture.Write(directory.path / "source.w3x");
    MapDataEditSession edits(MapDocument::FromMembers(fixture.members)); fixture.Attach(edits); fixture.Set(edits, fixture.Changed());
    const auto plan = Take(TerrainSavePlan::PrepareForSave(source, edits, {})); Check(plan.SaveGate());
    Check(plan.VerifySourceInputs(source));
    auto output = edits.Replacements(); output.push_back({"TerrainArt/CliffTypes.slk", CliffTable(false, true)});
    Require(!plan.VerifyFrozenOutput(source.SourceSha256(), output).Succeeded(), "changed final cliff table is rejected");
    const auto altered = Take(MapArchiveSession::Open(Take(source.SaveCandidate(directory.path / "altered.w3x", output)).path));
    Require(!plan.VerifyReadback(altered).Succeeded(), "independent readback rejects altered cliff row bytes");
    const std::array removals{std::string("TerrainArt/CliffTypes.slk")};
    Require(!plan.VerifyFrozenOutput(source.SourceSha256(), edits.Replacements(), removals).Succeeded(), "removing captured cliff resource is rejected");
    const auto missing = Take(MapArchiveSession::Open(Take(source.SaveCandidate(
        directory.path / "missing.w3x", edits.Replacements(), removals)).path));
    Require(!plan.VerifyReadback(missing).Succeeded(), "independent readback binds resource presence");
    for (const auto& name : {"war3map.w3d", "war3map.doo", "war3mapUnits.doo"}) {
        auto mixed = edits.Replacements(); mixed.push_back({name, Text("different final dependency")});
        Require(!plan.VerifyFrozenOutput(source.SourceSha256(), mixed).Succeeded(), "mixed objects cannot reuse unchanged-contribution proof");
    }
    fixture.members.erase(fixture.members.begin() + 5);
    const auto gameSource = fixture.Write(directory.path / "game-source.w3x");
    const auto gameRoot = directory.path / "game"; Require(std::filesystem::create_directory(gameRoot), "private game resource directory");
    Take(WriteVerifiedArchive(gameRoot / "war3.mpq", std::vector<MapArchiveMember>{{"TerrainArt/CliffTypes.slk", CliffTable()}}));
    MapDataEditSession gameEdits(MapDocument::FromMembers(fixture.members)); fixture.Attach(gameEdits); fixture.Set(gameEdits, fixture.Changed());
    const auto gamePlan = Take(TerrainSavePlan::PrepareForSave(gameSource, gameEdits, gameRoot)); Check(gamePlan.SaveGate());
    Check(gamePlan.VerifyFrozenOutput(gameSource.SourceSha256(), gameEdits.Replacements()));
    auto overlay = gameEdits.Replacements(); overlay.push_back({"TerrainArt/CliffTypes.slk", CliffTable()});
    Require(!gamePlan.VerifyFrozenOutput(gameSource.SourceSha256(), overlay).Succeeded(), "same-byte new map overlay still changes winning source");
    Take(WriteVerifiedArchive(gameRoot / "war3patch.mpq", std::vector<MapArchiveMember>{{"TerrainArt/CliffTypes.slk", CliffTable()}}));
    Require(!gamePlan.VerifyFrozenOutput(gameSource.SourceSha256(), gameEdits.Replacements()).Succeeded(), "same-byte changed winning game archive is stale");
    Require(!gamePlan.VerifySourceInputs(gameSource).Succeeded(), "source check revalidates game winner and absence");
}
} // namespace

int main() {
    try {
        MaterialAndVariationSaveWithEveryOriginalPathingByte();
        MixedGeometryFlagsGroundAndInvalidMaterialsCannotBorrowTheProof();
        BothCliffRowsAreMandatoryEvenForSameSlotVariation();
        PlacementAndRevisionChecksRemainRequiredForSlotOnlyChanges();
        FinalMembersReadbackAndActualGameWinnerStayBound();
        std::cout << "cliff material pathing invariants passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
