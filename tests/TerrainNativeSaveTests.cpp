#include "War3MapCore/Hash.h"
#include "War3MapCore/IO/FileBytes.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/MapData/TerrainPatchCodec.h"
#include "War3MapCore/MapData/TerrainPathingResources.h"
#include "War3MapCore/MapData/TerrainSavePlan.h"

#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <iostream>
#include <stdexcept>

namespace {
using namespace War3Map;
using namespace War3Map::MapData;
using Bytes = std::vector<std::byte>;
void Require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
void Check(Status status) {
    if (status.Succeeded()) return;
    std::string message;
    for (const auto& diagnostic : status.diagnostics) message += diagnostic.code + ": " + diagnostic.message + "\n";
    throw std::runtime_error(message);
}
template<class T> T Take(Result<T> result) {
    if (!result.Succeeded() || !result.value) { Check({result.diagnostics}); throw std::runtime_error("missing result"); }
    return std::move(*result.value);
}
Bytes Text(std::string_view text) {
    return {reinterpret_cast<const std::byte*>(text.data()), reinterpret_cast<const std::byte*>(text.data() + text.size())};
}

struct Fixture {
    std::filesystem::path directory;
    std::vector<MapArchiveMember> members;
    std::unique_ptr<MapArchiveSession> archive;
    std::unique_ptr<MapDataEditSession> edits;
    const TerrainStrokePoint point{512, 512};
    explicit Fixture(bool wet = true) {
        static std::atomic<unsigned> sequence{0};
        directory = std::filesystem::temp_directory_path() / ("Audastra-native-terrain-save-"
            + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()) + "-" + std::to_string(sequence++));
        Require(std::filesystem::create_directory(directory), "create private native save fixture");
        TerrainGridSnapshot terrain;
        terrain.header.width = terrain.header.height = 9; terrain.header.mainTileset = 'L';
        terrain.header.groundTilesets = {"Tg01", "Tg02"}; terrain.header.cliffTilesets = {"Cl01"};
        terrain.vertices.resize(81);
        for (auto& vertex : terrain.vertices) {
            vertex.groundHeightRaw = 8192; vertex.waterHeightAndFlagsRaw = 8192 + 48 * 4;
            vertex.textureAndFlagsRaw = wet ? 0x40 : 0; vertex.cliffAndLayerRaw = 2;
        }
        RefreshTerrainDerived(terrain, 0, 0, 9, 9);
        Bytes pathing = Text("MP3W");
        for (auto value : {0u, 32u, 32u}) for (unsigned shift = 0; shift < 32; shift += 8)
            pathing.push_back(static_cast<std::byte>((value >> shift) & 255));
        // 独立的平地/浅水六mode预期，不调用待测生成器制造原始WPM。
        pathing.resize(16 + 32 * 32, wet ? std::byte{0x19} : std::byte{0x51});
        MapSettings settings;
        settings.formatVersion = 25; settings.custom.emplace();
        settings.tileset = 'L'; settings.playableWidth = settings.playableHeight = 8;
        settings.flags |= 0x400; // 由地图边距和实际CameraMargins生成范围。
        DoodadPlacements doodads;
        UnitPlacements units;
        UnitPlacement unit; unit.rawcode = "u001"; unit.creationId = 1;
        unit.position = {512, 512, 0}; unit.scale = {1, 1, 1}; unit.flags = 2; units.units.push_back(unit);
        members = {{"war3map.w3e", Take(SerializeMapTerrain(terrain))}, {"war3map.wpm", std::move(pathing)},
            {"war3map.w3i", Take(settings.Serialize())}, {"war3map.doo", Take(doodads.Serialize())},
            {"war3mapUnits.doo", Take(units.Serialize())}, {"war3mapMap.tga", Text("preserved author image")},
            {"TerrainArt/Terrain.slk", Text("ID;PWXL;N;E\nB;X4;Y3;D0\n"
                "C;X1;Y1;K\"tileID\"\nC;X2;K\"walkable\"\nC;X3;K\"flyable\"\nC;X4;K\"buildable\"\n"
                "C;X1;Y2;K\"Tg01\"\nC;X2;K1\nC;X3;K1\nC;X4;K1\n"
                "C;X1;Y3;K\"Tg02\"\nC;X2;K0\nC;X3;K1\nC;X4;K1\nE\n")},
            {"TerrainArt/Water.slk", Text("ID;PWXL;N;E\nB;X3;Y2;D0\nC;X1;Y1;K\"waterID\"\n"
                "C;X2;K\"height\"\nC;X3;K\"impassable\"\nC;X1;Y2;K\"LSha\"\nC;X2;K0\nC;X3;K0\nE\n")},
            {"UI/MiscData.txt", Text("[Water]\nDeepLevelPathing=64\n[CameraMargins]\nLeft=0\nRight=0\nBottom=0\nTop=0\n")},
            {"UI/WorldEditData.txt", Text("[Other]\nValue=1\n")}};
        Open();
    }
    ~Fixture() { std::error_code error; std::filesystem::remove_all(directory, error); }
    void Open(std::string name = "source.w3x") {
        archive = std::make_unique<MapArchiveSession>(Take(MapArchiveSession::Open(
            Take(WriteVerifiedArchive(directory / name, members)).path)));
        edits = std::make_unique<MapDataEditSession>(MapDocument::FromMembers(members));
        Check(edits->AttachTerrain(members[0].data)); Check(edits->AttachPathing(members[1].data));
    }
    TerrainBrush Water() const {
        TerrainBrush brush; brush.kind = TerrainBrushKind::WaterHeight; brush.radius = 0; brush.waterHeight = 80;
        return brush;
    }
    void Paint(const TerrainBrush& brush) { Take(edits->EditTerrain(brush, std::span(&point, 1), false)); }
    TerrainSavePlan Plan() const { return Take(TerrainSavePlan::PrepareForSave(*archive, *edits, {})); }
    void Save(const char* name, bool expectRebuild = true) {
        const auto plan = Plan(); Check(plan.SaveGate());
        Require(plan.RebuiltPathing() == expectRebuild, "correct native rebuild path");
        auto output = edits->Replacements(); Check(plan.InstallPathingOutput(*edits, output));
        Check(plan.VerifyForSave(*edits, archive->SourceSha256(), output));
        auto saved = Take(MapArchiveSession::Open(Take(archive->SaveCandidate(directory / name, output)).path));
        Check(plan.VerifyReadback(saved)); Check(plan.CommitSaved(*edits, MapDocument::FromArchive(saved)));
        Require(!edits->IsDirty(), "actual output becomes the new clean session baseline");
        Require(Take(saved.ReadMember("war3mapMap.tga")) == members[5].data, "custom minimap is preserved");
        archive = std::make_unique<MapArchiveSession>(std::move(saved));
    }
};

void WaterCrossingAndUndoUseActualSavedPathing() {
    Fixture f;
    f.Paint(f.Water());
    const auto plan = f.Plan(); Check(plan.SaveGate());
    Require(plan.RebuiltPathing(), "crossing deep water needs native generation");
    Require(!plan.VerifyForSave(*f.edits, f.archive->SourceSha256(), f.edits->Replacements()).Succeeded(),
        "draft WPM cannot substitute for generated output");
    f.Save("deep.w3x");
    Require(Take(f.edits->InspectTerrainPoint(f.point)).pathingFlags == 0x1b, "deep water gains walk restriction and preserves 0x11");
    Require(f.edits->Undo(), "undo saved water edit");
    f.Save("shallow-again.w3x");
    Require(Take(f.edits->InspectTerrainPoint(f.point)).pathingFlags == 0x19, "native walk restriction is removed after returning to shallow");
    Require(Take(f.archive->ReadMember("war3map.wpm")) == f.members[1].data, "undo and resave restore full original WPM");
    Require(f.edits->Redo(), "redo saved water edit"); f.Save("deep-again.w3x");
    Require(Take(f.edits->InspectTerrainPoint(f.point)).pathingFlags == 0x1b, "redo resaves the native output");
}

void NewHasWaterUsesIndependentG0G1AndPreservesSourceProtection() {
    for (const bool deep : {false, true}) {
        Fixture f(false);
        const auto original = Take(ParseMapTerrain(f.members[0].data));
        auto current = *original.Snapshot();
        for (auto& vertex : current.vertices) {
            vertex.textureAndFlagsRaw |= 0x40;
            vertex.waterHeightAndFlagsRaw = static_cast<std::uint16_t>(8192 + (deep ? 80 : 48) * 4);
        }
        RefreshTerrainDerived(current, 0, 0, 9, 9);
        Check(f.edits->SetTerrainAndPathing(Take(SerializeMapTerrain(current)), f.members[1].data));
        const auto owner = Take(TerrainPathingResources::CaptureNative(*f.archive, *original.Snapshot(), current, {}, 1024 * 1024));
        const auto g0 = Take(owner.GenerateNative(*original.Snapshot(), 1024 * 1024));
        const auto g1 = Take(owner.GenerateNative(current, 1024 * 1024));
        const auto expected = deep ? std::byte{0x0a} : std::byte{0x08};
        Require(std::all_of(g0.begin(), g0.end(), [](auto value) { return value == std::byte{0x40}; })
            && std::all_of(g1.begin(), g1.end(), [&](auto value) { return value == expected; }),
            "new HasWater changes independent dry G0=40 to shallow G1=08 or deep G1=0a");
        TerrainBrush restriction; restriction.kind = TerrainBrushKind::PathingRestriction;
        restriction.size = 1; restriction.pathingRestrictions = 0x04; f.Paint(restriction);
        const auto plan = f.Plan(); Check(plan.SaveGate());
        auto output = f.edits->Replacements(); Check(plan.InstallPathingOutput(*f.edits, output));
        const auto wpm = std::find_if(output.begin(), output.end(), [](const auto& member) { return member.name == "war3map.wpm"; });
        Require(wpm != output.end() && wpm->data[16] == (std::byte{0x11} | expected)
            && wpm->data[16 + 16 * 32 + 16] == (std::byte{0x15} | expected),
            "new HasWater output replaces native six bits and preserves source 11 plus explicit intent 04");
        f.Save(deep ? "new-deep.w3x" : "new-shallow.w3x");
        Require(f.edits->Undo() && f.edits->Undo(), "undo saved intent and new HasWater raw delta");
        f.Save("dry-restored.w3x");
        Require(Take(f.archive->ReadMember("war3map.wpm")) == f.members[1].data,
            "undoing HasWater regenerates dry G1 without retaining former water restrictions");
    }

    Fixture unexplained(false);
    unexplained.members[1].data[16] ^= std::byte{0x04}; unexplained.Open("unexplained-dry.w3x");
    const auto parsed = Take(ParseMapTerrain(unexplained.members[0].data));
    auto wet = *parsed.Snapshot();
    for (auto& vertex : wet.vertices) vertex.textureAndFlagsRaw |= 0x40;
    RefreshTerrainDerived(wet, 0, 0, 9, 9);
    Check(unexplained.edits->SetTerrainAndPathing(Take(SerializeMapTerrain(wet)), unexplained.members[1].data));
    const auto refused = unexplained.Plan();
    Require(!refused.SaveGate().Succeeded() && !refused.RebuiltPathing()
        && std::any_of(refused.SaveGate().diagnostics.begin(), refused.SaveGate().diagnostics.end(), [](const auto& diagnostic) {
            return diagnostic.code == "TERRAIN_PATHING_SOURCE_UNEXPLAINED" && diagnostic.message.find("(0,0)") != std::string::npos;
        }), "new HasWater cannot bypass old WPM matching against dry G0 and saved intent");
}

void OverlappingSavedIntentIsNotRecoveredBySubtractingBits() {
    // 行走限制与深水贡献重叠；飞行限制不重叠，撤销后会留下真实WPM净差。
    for (const auto bits : {0x02u, 0x04u}) {
        Fixture f;
        TerrainBrush restriction; restriction.kind = TerrainBrushKind::PathingRestriction;
        restriction.size = 1; restriction.pathingRestrictions = static_cast<std::uint8_t>(bits);
        f.Paint(restriction); f.Paint(f.Water()); f.Save("restricted-deep.w3x");
        const auto draft = Take(f.edits->SerializePathingRestrictionDraft());
        Require(draft.size() > 4 && draft[3] == std::byte{3}, "saved intent retains APR3 layers");
        const auto terrain = Take(f.archive->ReadMember("war3map.w3e"));
        const auto pathing = Take(f.archive->ReadMember("war3map.wpm"));
        MapDataEditSession restored(MapDocument::FromArchive(*f.archive));
        Check(restored.AttachTerrain(terrain)); Check(restored.AttachPathing(pathing));
        Check(restored.SetTerrainAndPathingDraft(terrain, pathing, draft));
        Require(Take(restored.SerializePathingRestrictionDraft()) == draft, "APR3 recovers the exact saved P/S/A layers");
        Require(f.edits->Undo() && f.edits->Undo(), "undo water and saved explicit intent");
        f.Save("restriction-undone.w3x");
        Require(Take(f.archive->ReadMember("war3map.wpm")) == f.members[1].data,
            "withdrawing S does not preserve it as guessed native provenance");
        Require(f.edits->Redo() && f.edits->Redo(), "redo saved intent and water against the latest saved source");
        f.Save("restriction-redone.w3x");
        Require(Take(f.archive->ReadMember("war3map.w3e")) == terrain
            && Take(f.archive->ReadMember("war3map.wpm")) == pathing,
            "redo and resave restore the full saved terrain and pathing, including explicit restrictions");
    }
}

void PathingOutputRecognizesNamesAndPreservesConflictChecks() {
    for (const bool changedDraft : {false, true}) {
        Fixture f;
        if (changedDraft) {
            TerrainBrush restriction; restriction.kind = TerrainBrushKind::PathingRestriction;
            restriction.size = 1; restriction.pathingRestrictions = 0x04;
            f.Paint(restriction);
        }
        f.Paint(f.Water());
        const auto plan = f.Plan(); Check(plan.SaveGate());
        Require(plan.RebuiltPathing(), "combined save uses the native pathing output");
        const auto current = f.edits->Replacements();
        const auto wpm = std::find_if(current.begin(), current.end(), [](const auto& member) {
            return member.name == "war3map.wpm";
        });
        Require((wpm != current.end()) == changedDraft, "session includes WPM exactly when its bytes changed");
        const auto draftBytes = changedDraft ? wpm->data : f.members[1].data;
        auto explicitWpm = current;
        if (!changedDraft) explicitWpm.push_back({"war3map.wpm", draftBytes});
        for (const auto name : {"war3map.wpm", "WAR3MAP.WPM", "War3Map.WpM"}) {
            auto output = explicitWpm;
            const auto member = std::find_if(output.begin(), output.end(), [](const auto& value) {
                return value.name == "war3map.wpm";
            });
            Require(member != output.end(), "explicit draft WPM is present");
            member->name = name;
            Check(plan.InstallPathingOutput(*f.edits, output));
            Require(output.size() == explicitWpm.size(), "native output replaces the existing WPM without appending a duplicate");
            Check(plan.VerifyForSave(*f.edits, f.archive->SourceSha256(), output));
        }
        const auto rejectUnchanged = [&](std::vector<MemberReplacement> output, const char* message) {
            const auto before = output;
            const auto status = plan.InstallPathingOutput(*f.edits, output);
            Require(!status.Succeeded() && std::any_of(status.diagnostics.begin(), status.diagnostics.end(), [](const auto& d) {
                return d.code == "TERRAIN_PATHING_REBUILD_CONFLICT";
            }), message);
            Require(std::equal(output.begin(), output.end(), before.begin(), before.end(), [](const auto& a, const auto& b) {
                return a.name == b.name && a.data == b.data;
            }), "rejected native installation preserves the entire caller replacement list");
        };
        auto missing = current;
        std::erase_if(missing, [](const auto& member) { return member.name == "war3map.wpm"; });
        if (changedDraft) rejectUnchanged(std::move(missing), "omitting a real WPM edit is still rejected");
        else {
            Check(plan.InstallPathingOutput(*f.edits, missing));
            Check(plan.VerifyForSave(*f.edits, f.archive->SourceSha256(), missing));
        }
        auto tampered = explicitWpm;
        const auto altered = std::find_if(tampered.begin(), tampered.end(), [](const auto& member) {
            return member.name == "war3map.wpm";
        });
        altered->name = "WAR3MAP.WPM"; altered->data[16] ^= std::byte{0x04};
        rejectUnchanged(std::move(tampered), "non-current WPM cannot be hidden by a rebuilt output");
        auto duplicate = explicitWpm;
        duplicate.push_back({"War3Map.WpM", draftBytes});
        rejectUnchanged(std::move(duplicate), "case aliases of WPM are still duplicate outputs");
    }
}

void MaterialsAndCliffsRebuildThePathing() {
    for (const bool cliff : {false, true}) {
        Fixture f(false);
        TerrainBrush brush; brush.radius = 0;
        if (cliff) { brush.kind = TerrainBrushKind::CliffLevel; brush.levelDelta = 1; }
        else { brush.kind = TerrainBrushKind::Texture; brush.groundTexture = "Tg02"; }
        f.Paint(brush); f.Save(cliff ? "cliff.w3x" : "texture.w3x");
        Require(Take(f.edits->InspectTerrainPoint(f.point)).pathingFlags == (cliff ? 0xdb : 0x53),
            "changed cliffs/material properties write their native restrictions");
        Require(f.edits->Undo(), "undo saved native terrain change"); f.Save("restored.w3x");
        Require(Take(f.archive->ReadMember("war3map.wpm")) == f.members[1].data, "derived source bits are not retained after undo");
    }
}

void UnexplainedSourceAndOutputTamperingRefuse() {
    Fixture f;
    f.members[1].data[16] ^= std::byte{0x04}; f.Open("external.w3x"); f.Paint(f.Water());
    const auto bad = f.Plan();
    Require(!bad.SaveGate().Succeeded() && !bad.RebuiltPathing(), "unexplained original bit refuses whole regeneration");
    Require(std::any_of(bad.SaveGate().diagnostics.begin(), bad.SaveGate().diagnostics.end(), [](const auto& d) {
        return d.code == "TERRAIN_PATHING_SOURCE_UNEXPLAINED" && d.message.find("(0,0)") != std::string::npos;
    }), "source mismatch reports exact cell");
    Fixture good; good.Paint(good.Water()); const auto plan = good.Plan(); Check(plan.SaveGate());
    auto output = good.edits->Replacements(); output.push_back({"war3map.wpm", good.members[1].data});
    output.back().data[16] ^= std::byte{0x04};
    const auto unchanged = output.back().data;
    Require(!plan.InstallPathingOutput(*good.edits, output).Succeeded() && output.back().data == unchanged,
        "tampered explicit WPM is not silently replaced by native generation");
    output = good.edits->Replacements(); Check(plan.InstallPathingOutput(*good.edits, output));
    output.push_back({"TerrainArt/Terrain.slk", Text("changed rules")});
    Require(!plan.VerifyFrozenOutput(good.archive->SourceSha256(), output).Succeeded(), "resource changes cannot be smuggled into generated output");
    Require(good.edits->Undo(), "change the active revision");
    const auto beforeRevision = good.edits->TerrainSourceRevision();
    const auto beforePathing = Take(good.edits->SerializePathingRestrictionDraft());
    Require(!plan.CommitSaved(*good.edits, MapDocument::FromArchive(*good.archive)).Succeeded(),
        "old result cannot rebind changed front-end content");
    Require(good.edits->TerrainSourceRevision() == beforeRevision
        && Take(good.edits->SerializePathingRestrictionDraft()) == beforePathing,
        "failed baseline commit leaves current pathing and source generation unchanged");
}

void DirectoryRecipeUsesGeneratedWpm() {
    Fixture f;
    const auto directory = f.directory / "directory"; std::filesystem::create_directory(directory);
    for (const auto& member : f.members) {
        const auto path = directory / member.name; std::filesystem::create_directories(path.parent_path());
        Check(IO::WriteFileBytes(path, member.data));
    }
    const auto source = Take(DirectoryWorkspace::Open(directory));
    f.Paint(f.Water());
    const TerrainStrokeIntent intent{f.Water(), {f.point}, false};
    const auto patch = Take(TerrainPatchCodec::Seal(MapDocument::FromMembers(f.members), f.members[0].data,
        f.members[1].data, source.Snapshot(), *f.edits, std::span(&intent, 1)));
    const auto wpm = std::find_if(patch.Files().begin(), patch.Files().end(), [](const auto& file) {
        return file.relativePath.filename() == "war3map.wpm";
    });
    Require(wpm != patch.Files().end() && wpm->replacement[16 + 16 * 32 + 16] == std::byte{0x1b},
        "directory save seals actual generated WPM, not the editor draft");
    Require(Take(IO::ReadFileBytes(directory / "war3map.wpm")) == f.members[1].data, "sealing leaves the source untouched");
}
}

int main() {
    try {
        WaterCrossingAndUndoUseActualSavedPathing();
        NewHasWaterUsesIndependentG0G1AndPreservesSourceProtection();
        OverlappingSavedIntentIsNotRecoveredBySubtractingBits();
        PathingOutputRecognizesNamesAndPreservesConflictChecks();
        MaterialsAndCliffsRebuildThePathing();
        UnexplainedSourceAndOutputTamperingRefuse();
        DirectoryRecipeUsesGeneratedWpm();
        std::cout << "Native terrain save tests passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
