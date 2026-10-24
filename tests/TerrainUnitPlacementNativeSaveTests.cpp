#include "War3MapCore/Hash.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/MapData/PlacementEditing.h"
#include "War3MapCore/MapData/TerrainNativeUnitsProof.h"
#include "War3MapCore/MapData/TerrainSavePlan.h"

#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <iostream>
#include <stdexcept>
#include <type_traits>

namespace {
using namespace War3Map;
using namespace War3Map::MapData;
using Bytes = std::vector<std::byte>;
static_assert(!std::is_default_constructible_v<TerrainNativeUnitsProof>);

void Require(bool condition, const char* text) { if (!condition) throw std::runtime_error(text); }
void Check(Status status) {
    if (status.Succeeded()) return;
    std::string text;
    for (const auto& diagnostic : status.diagnostics) text += diagnostic.code + ": " + diagnostic.message + "\n";
    throw std::runtime_error(text);
}
template<class T> T Take(Result<T> value) {
    if (!value.Succeeded() || !value.value) { Check({value.diagnostics}); throw std::runtime_error("missing result"); }
    return std::move(*value.value);
}
bool HasCode(const std::vector<Diagnostic>& diagnostics, std::string_view code) {
    return std::any_of(diagnostics.begin(), diagnostics.end(), [&](const auto& diagnostic) { return diagnostic.code == code; });
}
Bytes Text(std::string_view text) {
    return {reinterpret_cast<const std::byte*>(text.data()), reinterpret_cast<const std::byte*>(text.data() + text.size())};
}

struct Fixture {
    std::filesystem::path directory;
    std::vector<MapArchiveMember> members;
    std::unique_ptr<MapArchiveSession> source;
    std::unique_ptr<MapDataEditSession> edits;
    Bytes originalTerrain, originalWpm, originalUnits;
    unsigned saved = 0;

    explicit Fixture(bool roc = false, bool sourceUnits = true) {
        static std::atomic<unsigned> sequence{0};
        directory = std::filesystem::temp_directory_path() / ("Audastra-terrain-units-save-"
            + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()) + "-" + std::to_string(sequence++));
        Require(std::filesystem::create_directory(directory), "create private units/terrain save directory");
        TerrainGridSnapshot terrain;
        terrain.header.width = terrain.header.height = 9; terrain.header.mainTileset = 'L';
        terrain.header.groundTilesets = {"Tg01"}; terrain.header.cliffTilesets = {"Cl01"};
        terrain.vertices.resize(81);
        for (auto& vertex : terrain.vertices) {
            vertex.groundHeightRaw = 8192; vertex.waterHeightAndFlagsRaw = 8192 + 48 * 4;
            vertex.textureAndFlagsRaw = 0x40; vertex.cliffAndLayerRaw = 2;
        }
        RefreshTerrainDerived(terrain, 0, 0, 9, 9);
        originalTerrain = Take(SerializeMapTerrain(terrain));
        originalWpm = Text("MP3W");
        for (const auto value : {0u, 32u, 32u}) for (unsigned shift = 0; shift < 32; shift += 8)
            originalWpm.push_back(static_cast<std::byte>((value >> shift) & 255));
        // 浅水G0=08，保留的非原生位为11，源图不含任何单位静态占地。
        originalWpm.resize(16 + 32 * 32, std::byte{0x19});
        UnitPlacements units;
        if (roc) { units.version = 7; units.subversion = 9; }
        UnitPlacement unit; unit.rawcode = "u001"; unit.creationId = 11;
        unit.position = {256.25f, 384.5f, 19.0f}; unit.rotation = 0.125f; unit.scale = {1, 1, 1};
        unit.flags = 2; unit.owner = 3; unit.unknownOwnerBytes = {0xab, 0x72};
        unit.hitPoints = 73; unit.mana = 33; unit.gold = 77; unit.targetAcquisition = 255;
        unit.heroLevel = 3; unit.itemSets.push_back({{{"I001", 37}, {"I002", 63}}});
        unit.inventory.push_back({2, "I003"}); unit.abilities.push_back({"A001", true, 2});
        unit.random = RandomCustomTable{{{"u010", 40}, {"u011", 60}}};
        if (!roc) { unit.heroStrength = 17; unit.heroAgility = 18; unit.heroIntelligence = 19; }
        units.units.push_back(unit);
        unit.rawcode = "b001"; unit.creationId = 12; unit.position = {512, 512, 0};
        units.units.push_back(unit);
        originalUnits = Take(units.Serialize());
        MapSettings settings;
        settings.formatVersion = 25; settings.custom.emplace(); settings.tileset = 'L';
        settings.playableWidth = settings.playableHeight = 8; settings.flags |= 0x400;
        members = {{"war3map.w3e", originalTerrain}, {"war3map.wpm", originalWpm},
            {"war3map.w3i", Take(settings.Serialize())}, {"war3map.doo", Take(DoodadPlacements{}.Serialize())},
            {"unrelated.bin", Bytes{std::byte{0x17}, std::byte{0x93}, std::byte{0}, std::byte{0xfe}}},
            {"TerrainArt/Terrain.slk", Text("ID;PWXL;N;E\nB;X4;Y2;D0\nC;X1;Y1;K\"tileID\"\n"
                "C;X2;K\"walkable\"\nC;X3;K\"flyable\"\nC;X4;K\"buildable\"\n"
                "C;X1;Y2;K\"Tg01\"\nC;X2;K1\nC;X3;K1\nC;X4;K1\nE\n")},
            {"TerrainArt/Water.slk", Text("ID;PWXL;N;E\nB;X3;Y2;D0\nC;X1;Y1;K\"waterID\"\n"
                "C;X2;K\"height\"\nC;X3;K\"impassable\"\nC;X1;Y2;K\"LSha\"\nC;X2;K0\nC;X3;K0\nE\n")},
            {"UI/MiscData.txt", Text("[Water]\nDeepLevelPathing=64\n[CameraMargins]\nLeft=0\nRight=0\nBottom=0\nTop=0\n")},
            {"UI/WorldEditData.txt", Text("[Other]\nValue=1\n")}};
        if (sourceUnits) members.push_back({"war3mapUnits.doo", originalUnits});
        Open();
    }
    ~Fixture() { std::error_code error; std::filesystem::remove_all(directory, error); }
    void Open() {
        source = std::make_unique<MapArchiveSession>(Take(MapArchiveSession::Open(
            Take(WriteVerifiedArchive(directory / ("source-" + std::to_string(saved++) + ".w3x"), members)).path)));
        edits = std::make_unique<MapDataEditSession>(MapDocument::FromMembers(members));
        Check(edits->AttachTerrain(Member("war3map.w3e"))); Check(edits->AttachPathing(Member("war3map.wpm")));
    }
    Bytes& Member(std::string_view name) {
        const auto found = std::find_if(members.begin(), members.end(), [&](const auto& member) { return member.name == name; });
        Require(found != members.end(), "fixture member exists"); return found->data;
    }
    void Water() {
        TerrainBrush brush; brush.kind = TerrainBrushKind::WaterHeight; brush.radius = 0; brush.waterHeight = 80;
        const TerrainStrokePoint point{512, 512};
        Take(edits->EditTerrain(brush, std::span(&point, 1), false));
    }
    TerrainSavePlan Plan(const TerrainSavePlanLimits& limits = {}) const {
        return Take(TerrainSavePlan::PrepareForSave(*source, *edits, {}, limits));
    }
    Bytes ExpectedDeepWpm() const {
        auto expected = originalWpm;
        // 最近顶点(4,4)的4x4通行格，从浅水08变深水0a；所有其它字节保持。
        for (std::size_t y = 14; y < 18; ++y) for (std::size_t x = 14; x < 18; ++x)
            expected[16 + y * 32 + x] = std::byte{0x1b};
        return expected;
    }
    std::vector<MemberReplacement> Outputs(const TerrainSavePlan& plan) const {
        auto outputs = edits->Replacements(); Check(plan.InstallPathingOutput(*edits, outputs));
        Check(plan.VerifyForSave(*edits, source->SourceSha256(), outputs)); return outputs;
    }
    void SaveAndCheck(const Bytes& expectedUnits, const Bytes& expectedWpm, const Bytes& expectedTerrain) {
        const auto plan = Plan(); Check(plan.SaveGate()); Require(plan.RebuiltPathing(), "mixed save uses complete native G0/G1");
        const auto outputs = Outputs(plan);
        auto savedMap = Take(MapArchiveSession::Open(Take(source->SaveCandidate(
            directory / ("saved-" + std::to_string(saved++) + ".w3x"), outputs)).path));
        Check(plan.VerifyReadback(savedMap));
        Require(Take(savedMap.ReadMember("war3mapUnits.doo")) == expectedUnits, "all final Units bytes are exact");
        Require(Take(savedMap.ReadMember("war3map.wpm")) == expectedWpm, "complete WPM matches independent shallow/deep literal");
        Require(Take(savedMap.ReadMember("war3map.w3e")) == expectedTerrain, "complete terrain matches independent raw change");
        Require(Take(savedMap.ReadMember("unrelated.bin")) == Member("unrelated.bin"), "unrelated opaque bytes survive mixed save");
        Check(plan.CommitSaved(*edits, MapDocument::FromArchive(savedMap)));
        Require(!edits->IsDirty(), "complete successful readback becomes clean baseline");
        source = std::make_unique<MapArchiveSession>(std::move(savedMap));
    }
};

void NativeSaveSupportsUnitTransformsAndCounts() {
    for (const bool roc : {false, true}) for (unsigned operation = 0; operation < 6; ++operation) {
        Fixture fixture(roc); fixture.Water();
        auto document = fixture.edits->Snapshot(); const auto& units = *document.Units();
        const auto id = MakePlacementId(PlacementFile::Units, 0, units.units[0]);
        if (operation == 0 || operation == 1) {
            PlacementMove move{id, units.units[0].position, {}};
            if (operation == 0) { move.position[0] += 67.25f; move.position[1] -= 25.5f; }
            else move.rotation = 1.125f;
            Check(MovePlacements(*fixture.edits, std::span(&move, 1)));
        } else if (operation == 2) {
            Take(CopyPlacements(*fixture.edits, std::span(&id, 1), {64, -128, 7}));
        } else if (operation == 3) {
            auto created = units.units[0]; created.rawcode = "uNew"; created.position = {700, 300, 0};
            Take(AddUnitPlacement(*fixture.edits, created));
        } else if (operation == 4) Check(DeletePlacements(*fixture.edits, std::span(&id, 1)));
        else { auto empty = units; empty.units.clear(); Check(fixture.edits->EditUnits(empty)); }
        const auto expectedUnits = Take(fixture.edits->Snapshot().Units()->Serialize());
        auto expectedTerrain = *Take(ParseMapTerrain(fixture.originalTerrain)).Snapshot();
        expectedTerrain.vertices[40].waterHeightAndFlagsRaw = 8192 + 80 * 4;
        const auto deepTerrain = Take(SerializeMapTerrain(expectedTerrain));
        Require(!fixture.edits->VerifyTerrainPathingForSave().Succeeded(), "old public invariant still rejects mixed transform/count changes");
        const auto sourceSha = fixture.source->SourceSha256(); const auto sourcePath = fixture.source->SourcePath();
        fixture.SaveAndCheck(expectedUnits, fixture.ExpectedDeepWpm(), deepTerrain);
        Require(Take(Sha256File(sourcePath)) == sourceSha, "mixed save does not modify source archive");
        Require(fixture.edits->Undo() && fixture.edits->Undo(), "undo the saved placement and water transactions");
        fixture.SaveAndCheck(fixture.originalUnits, fixture.originalWpm, fixture.originalTerrain);
        Require(fixture.edits->Redo() && fixture.edits->Redo(), "redo both transactions after another saved baseline");
        fixture.SaveAndCheck(expectedUnits, fixture.ExpectedDeepWpm(), deepTerrain);
    }
}

void FreezeReadbackAndCommitKeepWholeUnitsIdentity() {
    Fixture fixture; fixture.Water();
    auto units = *fixture.edits->Snapshot().Units(); units.units[0].rotation = 1.5f; Check(fixture.edits->EditUnits(units));
    const auto plan = fixture.Plan(); Check(plan.SaveGate());
    auto output = fixture.Outputs(plan);
    const auto unitsMember = std::find_if(output.begin(), output.end(), [](const auto& member) { return member.name == "war3mapUnits.doo"; });
    Require(unitsMember != output.end(), "mixed final output contains Units");
    auto omitted = output; std::erase_if(omitted, [](const auto& member) { return member.name == "war3mapUnits.doo"; });
    Require(!plan.VerifyFrozenOutput(fixture.source->SourceSha256(), omitted).Succeeded(), "omitted Units cannot publish");
    auto tampered = output; const auto index = static_cast<std::size_t>(unitsMember - output.begin());
    tampered[index].data = fixture.originalUnits;
    Require(!plan.VerifyFrozenOutput(fixture.source->SourceSha256(), tampered).Succeeded(), "old full Units cannot replace checked final output");
    const std::vector<std::string> removal{"war3mapUnits.doo"};
    Require(!plan.VerifyFrozenOutput(fixture.source->SourceSha256(), output, removal).Succeeded(), "whole Units removal remains rejected");
    Require(!plan.VerifyFrozenOutput(std::string(64, '0'), output).Succeeded(), "source mismatch remains rejected");
    const auto wrongSaved = Take(MapArchiveSession::Open(Take(fixture.source->SaveCandidate(fixture.directory / "wrong-units.w3x", tampered)).path));
    Require(!plan.VerifyReadback(wrongSaved).Succeeded(), "readback refuses an archive carrying old Units");
    const auto revision = fixture.edits->Revision(); const auto current = Take(fixture.edits->Snapshot().Units()->Serialize());
    Require(!plan.CommitSaved(*fixture.edits, MapDocument::FromArchive(wrongSaved)).Succeeded(), "commit refuses wrong readback Units before changing baseline");
    Require(fixture.edits->Revision() == revision && fixture.edits->IsDirty()
        && Take(fixture.edits->Snapshot().Units()->Serialize()) == current, "commit refusal preserves complete current edit and revision");
    Require(fixture.edits->Undo(), "undo remains usable after refused commit");
    Require(!plan.VerifyForSave(*fixture.edits, fixture.source->SourceSha256(), output).Succeeded(), "old proof cannot survive undo revision");
}

void OldWpmAndOrdinaryDoodadGatesRemainStrict() {
    Fixture unexplained;
    unexplained.Member("war3map.wpm")[16] ^= std::byte{0x04}; unexplained.Open(); unexplained.Water();
    auto units = *unexplained.edits->Snapshot().Units(); units.units[0].position[0] += 32;
    Check(unexplained.edits->EditUnits(units)); const auto rejected = unexplained.Plan();
    Require(!rejected.SaveGate().Succeeded() && HasCode(rejected.SaveGate().diagnostics, "TERRAIN_PATHING_SOURCE_UNEXPLAINED"),
        "unit proof does not bypass a single unexplained old WPM bit");
    Fixture doodad;
    DoodadPlacements placed; DoodadPlacement object; object.rawcode = "D001"; object.creationId = 1;
    object.position = {256, 256, 0}; object.scale = {1, 1, 1}; placed.doodads.push_back(object);
    doodad.Member("war3map.doo") = Take(placed.Serialize()); doodad.Open(); doodad.Water();
    units = *doodad.edits->Snapshot().Units(); units.units[0].rotation = 1; Check(doodad.edits->EditUnits(units));
    placed.doodads[0].position[0] += 32; Check(doodad.edits->EditDoodads(placed));
    const auto doodadRevision = doodad.edits->Revision();
    const auto changedDoodads = Take(doodad.edits->Snapshot().Doodads()->Serialize());
    const auto refused = doodad.Plan();
    Require(!refused.SaveGate().Succeeded() && !refused.RebuiltPathing()
        && HasCode(refused.SaveGate().diagnostics, "TERRAIN_NATIVE_OBJECT_ADAPTER_REQUIRED"),
        "Units proof cannot supply the actual object-category and footprint adapter for changed ordinary DOO");
    Require(doodad.edits->Revision() == doodadRevision && doodad.edits->IsDirty()
        && Take(doodad.edits->Snapshot().Doodads()->Serialize()) == changedDoodads,
        "missing ordinary-object sources retain the whole draft and its revision");
    Fixture missing(false, false); missing.Water();
    units = Take(ParseUnitPlacements(missing.originalUnits)); Check(missing.edits->EditUnits(units));
    const auto missingPlan = missing.Plan();
    Require(!missingPlan.SaveGate().Succeeded() && HasCode(missingPlan.SaveGate().diagnostics, "TERRAIN_NATIVE_UNITS_EXISTENCE"),
        "adding a missing whole Units member remains unsupported");
}

void BudgetFailureRetainsDraftAndOriginal() {
    Fixture fixture; fixture.Water();
    auto units = *fixture.edits->Snapshot().Units(); units.units[0].rotation = 1; Check(fixture.edits->EditUnits(units));
    const auto revision = fixture.edits->Revision();
    const auto sourceSha = fixture.source->SourceSha256();
    const auto succeeds = [&](std::size_t budget) {
        auto result = TerrainSavePlan::PrepareForSave(*fixture.source, *fixture.edits, {}, {budget});
        return result.Succeeded() && result.value && result.value->SaveGate().Succeeded();
    };
    std::size_t low = 0, high = TerrainSavePlanLimits{}.maximumBytes;
    Require(succeeds(high), "default cumulative budget supports fixture");
    while (high - low > 1) { const auto middle = low + (high - low) / 2; if (succeeds(middle)) high = middle; else low = middle; }
    Require(succeeds(high) && !succeeds(high - 1), "actual cumulative native save budget has a strict exact boundary");
    Require(fixture.edits->Revision() == revision && fixture.edits->IsDirty()
        && fixture.source->SourceSha256() == sourceSha
        && Take(fixture.source->ReadMember("war3mapUnits.doo")) == fixture.originalUnits,
        "rejected budgets preserve source, full Units draft and revision");
    Require(fixture.edits->Undo() && fixture.edits->Redo(), "budget refusal leaves undo and redo intact");
}

void UnsupportedUnitWireNeverEntersNativeCombination() {
    Fixture fixture; fixture.Water();
    auto units = *fixture.edits->Snapshot().Units(); units.units[0].rotation = 1;
    Check(fixture.edits->EditUnits(units));
    const auto expected = Take(units.Serialize()); const auto revision = fixture.edits->Revision();
    std::vector<Bytes> invalid;
    auto truncated = expected; truncated.pop_back(); invalid.push_back(std::move(truncated));
    auto trailing = expected; trailing.push_back(std::byte{0x91}); invalid.push_back(std::move(trailing));
    auto version = expected; version[4] = std::byte{9}; invalid.push_back(std::move(version));
    auto nonfinite = expected;
    // 首条记录X位于16字节头+rawcode4+variation4；写入positive infinity。
    nonfinite[24] = nonfinite[25] = std::byte{0}; nonfinite[26] = std::byte{0x80}; nonfinite[27] = std::byte{0x7f};
    invalid.push_back(std::move(nonfinite));
    auto single = units; single.units.resize(1); auto skin = Take(single.Serialize());
    const auto skinId = Text("H001"); skin.insert(skin.begin() + 52, skinId.begin(), skinId.end());
    invalid.push_back(std::move(skin));
    for (const auto& wire : invalid) {
        Require(!fixture.edits->SetSerialized(MapDataKind::Units, wire).Succeeded(),
            "unknown, truncated, trailing, nonfinite and skin Units cannot replace the canonical editable document");
        Require(fixture.edits->Revision() == revision && Take(fixture.edits->Snapshot().Units()->Serialize()) == expected,
            "invalid Units intake leaves the complete current wire and revision unchanged");
    }
    const auto plan = fixture.Plan(); Check(plan.SaveGate());
    Check(plan.VerifyForSave(*fixture.edits, fixture.source->SourceSha256(), fixture.Outputs(plan)));
}
}

int main() {
    try {
        NativeSaveSupportsUnitTransformsAndCounts();
        FreezeReadbackAndCommitKeepWholeUnitsIdentity();
        OldWpmAndOrdinaryDoodadGatesRemainStrict();
        BudgetFailureRetainsDraftAndOriginal();
        UnsupportedUnitWireNeverEntersNativeCombination();
        std::cout << "terrain native Units combination save passed\n";
        return 0;
    } catch (const std::exception& exception) { std::cerr << exception.what() << '\n'; return 1; }
}
