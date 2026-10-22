#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/MapData/TerrainSavePlan.h"

#include <Windows.h>

#include <algorithm>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <utility>

namespace {
using namespace War3Map;
using namespace War3Map::MapData;

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void Check(Status status) {
    if (status.Succeeded()) return;
    std::string text;
    for (const auto& d : status.diagnostics) text += d.code + ": " + d.message + "\n";
    throw std::runtime_error(text);
}
template<class T> T Take(Result<T> result) {
    if (!result.Succeeded() || !result.value) {
        Check({std::move(result.diagnostics)});
        throw std::runtime_error("missing result");
    }
    return std::move(*result.value);
}

struct Fixture {
    std::filesystem::path directory;
    std::vector<MapArchiveMember> members;
    std::unique_ptr<MapArchiveSession> source;
    std::unique_ptr<MapDataEditSession> edits;

    Fixture() {
        directory = std::filesystem::temp_directory_path() / ("Audastra-placement-elevation-"
            + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
        Require(std::filesystem::create_directory(directory), "create private fixture");
        TerrainGridSnapshot terrain;
        terrain.header.width = terrain.header.height = 9;
        terrain.header.mainTileset = 'L'; terrain.header.groundTilesets = {"Tg01"};
        terrain.header.cliffTilesets = {"Cl01"}; terrain.vertices.resize(81);
        for (auto& vertex : terrain.vertices) {
            vertex.groundHeightRaw = 8192; vertex.waterHeightAndFlagsRaw = 7936; vertex.cliffAndLayerRaw = 2;
        }
        RefreshTerrainDerived(terrain, 0, 0, 9, 9);
        std::vector<std::byte> wpm{std::byte{'M'}, std::byte{'P'}, std::byte{'3'}, std::byte{'W'}};
        for (auto value : {0u, 32u, 32u}) for (unsigned shift = 0; shift != 32; shift += 8)
            wpm.push_back(static_cast<std::byte>((value >> shift) & 0xff));
        wpm.resize(16 + 32 * 32, std::byte{0x82});
        UnitPlacements units;
        UnitPlacement unit;
        unit.rawcode = "u001"; unit.creationId = 1; unit.position = {512, 512, 10};
        unit.scale = {1, 1, 1}; unit.flags = 2;
        units.units.push_back(unit);
        unit.creationId = 2; unit.position = {0, 0, 20}; units.units.push_back(unit);
        DoodadPlacements doodads;
        DoodadPlacement tree;
        tree.rawcode = "d001"; tree.creationId = 3; tree.position = {512, 512, 5};
        tree.scale = {1, 1, 1}; tree.flags = 2; doodads.doodads.push_back(tree);
        // 该段不允许随普通 Z 改动被删除、归一化或当作未知尾数据丢弃。
        doodads.specialDoodads.push_back({"D001", 4, {0, 0}});
        members = {{"war3map.w3e", Take(SerializeMapTerrain(terrain))}, {"war3map.wpm", std::move(wpm)},
            {"war3mapUnits.doo", Take(units.Serialize())}, {"war3map.doo", Take(doodads.Serialize())},
            {"war3map.shd", {std::byte{0x41}, std::byte{0x42}}}};
        source = std::make_unique<MapArchiveSession>(Take(MapArchiveSession::Open(
            Take(WriteVerifiedArchive(directory / "source.w3x", members)).path)));
        edits = std::make_unique<MapDataEditSession>(MapDocument::FromMembers(members));
        Check(edits->AttachTerrain(members[0].data)); Check(edits->AttachPathing(members[1].data));
    }
    ~Fixture() { std::error_code error; std::filesystem::remove_all(directory, error); }

    void Raise() {
        TerrainBrush brush; brush.kind = TerrainBrushKind::Raise; brush.radius = 0; brush.strength = 32;
        const TerrainStrokePoint point{512, 512};
        const auto prepared = Take(edits->PrepareTerrainEdit(brush, std::span(&point, 1), true));
        Check(prepared.TerrainPathingGate());
        Take(edits->EditTerrain(brush, std::span(&point, 1), true));
        Check(edits->VerifyTerrainPathingForSave());
    }
};

void FollowAndSaveRoundtrip() {
    Fixture f; f.Raise();
    Require(f.edits->Snapshot().Units()->units[0].position[2] == 42.0f
        && f.edits->Snapshot().Units()->units[1].position[2] == 20.0f
        && f.edits->Snapshot().Doodads()->doodads[0].position[2] == 37.0f,
        "only objects under height stroke follow the ground");
    const auto plan = Take(TerrainSavePlan::Prepare(*f.source, *f.edits));
    Check(plan.SaveGate());
    const auto output = f.edits->Replacements();
    Check(plan.VerifyForSave(*f.edits, f.source->SourceSha256(), output));
    const auto saved = Take(MapArchiveSession::Open(Take(f.source->SaveCandidate(f.directory / "raised.w3x", output)).path));
    Check(plan.VerifyReadback(saved));
    Require(Take(saved.ReadMember("war3map.wpm")) == f.members[1].data, "source WPM retained byte-for-byte");
    Require(Take(saved.ReadMember("war3map.shd")) == f.members[5].data, "source shadow retained");
    const auto savedDoodads = Take(ParseDoodadPlacements(Take(saved.ReadMember("war3map.doo"))));
    Require(savedDoodads.specialDoodads.size() == 1 && savedDoodads.specialDoodads[0].variation == 4,
        "special tail retained");
    Require(!plan.VerifyReadback(*f.source).Succeeded(), "old source is not a valid edited readback");
    for (const auto& member : f.members)
        Require(Take(f.source->ReadMember(member.name)) == member.data, "authoritative source remains unchanged");

    for (const auto name : {"war3mapUnits.doo", "war3map.doo"}) {
        auto missing = output;
        std::erase_if(missing, [&](const auto& item) { return item.name == name; });
        Require(!plan.VerifyFrozenOutput(f.source->SourceSha256(), missing).Succeeded(),
            "final task cannot omit a promised Z update");
        auto tampered = output;
        auto item = std::find_if(tampered.begin(), tampered.end(), [&](const auto& replacement) { return replacement.name == name; });
        Require(item != tampered.end(), "followed member output exists");
        item->data[24] ^= std::byte{1}; // 第一个普通记录的 X；不能借冻结计划换成不同候选。
        Require(!plan.VerifyFrozenOutput(f.source->SourceSha256(), tampered).Succeeded(), "frozen output is exact");
        const auto wrong = Take(MapArchiveSession::Open(Take(f.source->SaveCandidate(
            f.directory / (std::string(name) + "-wrong.w3x"), tampered)).path));
        Require(!plan.VerifyReadback(wrong).Succeeded(), "readback rejects an unplanned placement value");
    }

    f.edits->CommitSaved(f.edits->Snapshot());
    Require(!f.edits->IsDirty() && f.edits->Undo(), "saved stroke remains undoable");
    const auto undoPlan = Take(TerrainSavePlan::Prepare(saved, *f.edits));
    Check(undoPlan.SaveGate());
    Check(undoPlan.VerifyForSave(*f.edits, saved.SourceSha256(), f.edits->Replacements()));
    const auto undone = Take(MapArchiveSession::Open(Take(saved.SaveCandidate(
        f.directory / "undo.w3x", f.edits->Replacements())).path));
    Check(undoPlan.VerifyReadback(undone));
    for (const auto& member : f.members)
        Require(Take(undone.ReadMember(member.name)) == member.data, "saved undo restores source bytes");
    Require(f.edits->Redo(), "redo remains available");
    Require(!undoPlan.VerifyForSave(*f.edits, saved.SourceSha256(), f.edits->Replacements()).Succeeded(),
        "late revision invalidates old plan");
}

void NonElevationEditsRemainRejected() {
    Fixture f; f.Raise();
    const auto units = *f.edits->Snapshot().Units();
    auto moved = units; moved.units[0].position[0] += 1.0f;
    Check(f.edits->EditUnits(moved));
    Require(!Take(TerrainSavePlan::Prepare(*f.source, *f.edits)).SaveGate().Succeeded(), "XY still requires pathing refresh");
    Require(f.edits->Undo(), "undo XY move");
    auto rotated = units; rotated.units[0].rotation = 0.75f;
    Check(f.edits->EditUnits(rotated));
    Require(!f.edits->VerifyTerrainPathingForSave().Succeeded(), "rotation not admitted as elevation");
    Require(f.edits->Undo(), "undo rotation");
    auto changedSpecial = *f.edits->Snapshot().Doodads(); changedSpecial.specialDoodads[0].position[0] = 1;
    Check(f.edits->EditDoodads(changedSpecial));
    Require(!f.edits->VerifyTerrainPathingForSave().Succeeded(), "special placement change still rejected");
}

void SavedRestrictionsAndFollowingShareUndoAndSave() {
    Fixture f;
    const TerrainStrokePoint point{512, 512}, secondPoint{544, 512};
    TerrainBrush restriction;
    restriction.kind = TerrainBrushKind::PathingRestriction;
    restriction.size = 1; restriction.pathingRestrictions = 0x06;
    Take(f.edits->EditTerrain(restriction, std::span(&point, 1)));
    const auto save = [&](const MapArchiveSession& source, const char* name) {
        const auto plan = Take(TerrainSavePlan::Prepare(source, *f.edits));
        Check(plan.SaveGate());
        const auto replacements = f.edits->Replacements();
        Check(plan.VerifyForSave(*f.edits, source.SourceSha256(), replacements));
        auto saved = Take(MapArchiveSession::Open(Take(source.SaveCandidate(f.directory / name, replacements)).path));
        Check(plan.VerifyReadback(saved));
        f.edits->CommitSaved(f.edits->Snapshot());
        Require(!f.edits->IsDirty(), "successful readback commits the exact baseline");
        return saved;
    };
    const auto first = save(*f.source, "restrictions.w3x");
    Require(Take(f.edits->SerializePathingRestrictionDraft())[3] == std::byte{3},
        "saved intent remains distinct from protected original restrictions");
    f.Raise();
    restriction.pathingRestrictions = 0x08;
    Take(f.edits->EditTerrain(restriction, std::span(&secondPoint, 1)));
    Require(f.edits->Replacements().size() == 4, "height, two placements and pathing coexist in one save");
    const auto raised = save(first, "restrictions-and-height.w3x");
    Require(Take(f.edits->InspectTerrainPoint(point)).pathingFlags == 0x86
        && Take(f.edits->InspectTerrainPoint(secondPoint)).pathingFlags == 0x8a,
        "height save retains both distinct authored restrictions");
    Require(f.edits->Undo() && f.edits->Undo(), "saved restriction and placement-following commands remain undoable");
    restriction.pathingRestrictions = 0x0e; restriction.value = false;
    Take(f.edits->EditTerrain(restriction, std::span(&point, 1)));
    Require(Take(f.edits->InspectTerrainPoint(point)).pathingFlags == 0x82,
        "erasing saved intent preserves overlapped original walk and unknown flags");
    const auto restored = save(raised, "restored.w3x");
    for (const auto& member : f.members)
        Require(Take(restored.ReadMember(member.name)) == member.data,
            "saving undo and erase restores terrain, both placements and protected source bytes");
    Require(Take(f.edits->SerializePathingRestrictionDraft()).empty(), "no saved intent remains after final erase save");
}

} // namespace

int main() {
    try {
        FollowAndSaveRoundtrip(); NonElevationEditsRemainRejected(); SavedRestrictionsAndFollowingShareUndoAndSave();
        std::cout << "terrain placement elevation save passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
