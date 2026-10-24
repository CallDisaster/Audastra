#include "War3MapCore/MapData/MapDataEditSession.h"

#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using namespace War3Map;
using namespace War3Map::MapData;
using Bytes = std::vector<std::byte>;
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class T> T Take(Result<T> value) {
    if (!value.Succeeded() || !value.value) throw std::runtime_error("journal fixture operation failed");
    return std::move(*value.value);
}
TerrainStrokeJournal Journal() {
    TerrainStrokeJournal journal; journal.sourceSha256.assign(64, 'a');
    for (std::size_t i = 0; i < TerrainJournalMemberNames.size(); ++i)
        journal.members.push_back({std::string(TerrainJournalMemberNames[i]), i < 2, i < 2 ? std::string(64, 'b') : std::string{}, i < 2 ? 4u : 0u});
    TerrainJournalStroke stroke; stroke.intent.brush.kind = TerrainBrushKind::Raise;
    stroke.intent.brush.radius = 0; stroke.intent.points = {{256, 256}, {384, 256}};
    journal.strokes.push_back(std::move(stroke)); return journal;
}
void StrictCodecRoundtripAndMalformedInputs() {
    const auto wire = Take(EncodeTerrainStrokeJournal(Journal()));
    const auto restored = Take(DecodeTerrainStrokeJournal(wire));
    Require(Take(EncodeTerrainStrokeJournal(restored)) == wire, "canonical complete roundtrip");
    for (std::size_t size = 0; size < wire.size(); ++size)
        Require(!DecodeTerrainStrokeJournal(std::span(wire).first(size)).Succeeded(), "every truncated record fails closed");
    auto changed = wire; changed.push_back(std::byte{0});
    Require(!DecodeTerrainStrokeJournal(changed).Succeeded(), "unknown trailing extension rejected");
    changed = wire; changed[3] = std::byte{'3'};
    Require(!DecodeTerrainStrokeJournal(changed).Succeeded(), "unknown version rejected");
    changed = wire; changed[88] = std::byte{2};
    Require(!DecodeTerrainStrokeJournal(changed).Succeeded(), "nonboolean member presence rejected");
    auto invalid = Journal(); invalid.members[2].sha256.assign(64, 'c');
    Require(!EncodeTerrainStrokeJournal(invalid).Succeeded(), "missing member cannot carry a fabricated digest");
    invalid = Journal(); invalid.members[0].present = false;
    Require(!EncodeTerrainStrokeJournal(invalid).Succeeded(), "W3E is required");
    invalid = Journal(); invalid.sourceSha256[0] = 'A';
    Require(!EncodeTerrainStrokeJournal(invalid).Succeeded(), "only lowercase complete SHA accepted");
    invalid = Journal(); invalid.strokes[0].intent.brush.anchor = TerrainStrokeAnchor{};
    Require(!EncodeTerrainStrokeJournal(invalid).Succeeded(), "serialized anchors cannot grant source identity");
    invalid = Journal(); invalid.strokes[0].intent.points[0].x = std::numeric_limits<double>::infinity();
    Require(!EncodeTerrainStrokeJournal(invalid).Succeeded(), "nonfinite world point rejected");
    invalid = Journal(); invalid.strokes[0].intent.brush.kind = static_cast<TerrainBrushKind>(255);
    Require(!EncodeTerrainStrokeJournal(invalid).Succeeded(), "unknown brush kind rejected");
    invalid = Journal(); invalid.strokes[0].intent.brush.size = 9;
    Require(!EncodeTerrainStrokeJournal(invalid).Succeeded(), "classic size range checked");
    invalid = Journal(); invalid.strokes[0].intent.points.assign(4096, {256, 256});
    Require(EncodeTerrainStrokeJournal(invalid).Succeeded(), "4096 points remains in domain");
    invalid.strokes[0].intent.points.push_back({256, 256});
    Require(!EncodeTerrainStrokeJournal(invalid).Succeeded(), "4097 points cannot be trimmed or split");
    invalid = Journal(); invalid.strokes.assign(129, invalid.strokes.front());
    Require(!EncodeTerrainStrokeJournal(invalid).Succeeded(), "129 strokes rejected");
    invalid = Journal(); auto& water = invalid.strokes[0];
    water.intent.brush = {}; water.intent.waterBrush = TerrainWaterStrokeOptions{}; water.rulesSha256.assign(64, 'd');
    Require(EncodeTerrainStrokeJournal(invalid).Succeeded(), "water options have independent neutral ordinary brush");
    water.intent.brush.seed = 1;
    Require(!EncodeTerrainStrokeJournal(invalid).Succeeded(), "water ordinary conflicts rejected");
}

void NativeCliffVersionAndOptionsRemainExplicit() {
    auto journal = Journal(); auto& stroke = journal.strokes.front();
    stroke.intent.brush.kind = TerrainBrushKind::CliffLevel;
    const auto legacy = Take(EncodeTerrainStrokeJournal(journal));
    Require(legacy[3] == std::byte{'1'}, "legacy ordinary cliff retains FTJ1 encoding");
    Require(!Take(DecodeTerrainStrokeJournal(legacy)).strokes.front().cliffEnforceWaterLimits,
        "legacy cliff cannot acquire native rule semantics during decoding");
    stroke.rulesSha256.assign(64, 'd');
    Require(!EncodeTerrainStrokeJournal(journal).Succeeded(), "native cliff digest alone cannot omit its water-limit option");
    stroke.cliffEnforceWaterLimits = false;
    const auto wire = Take(EncodeTerrainStrokeJournal(journal));
    Require(wire[3] == std::byte{'2'}, "native cliff opts into FTJ2 explicitly");
    const auto decoded = Take(DecodeTerrainStrokeJournal(wire));
    Require(decoded.strokes.front().cliffEnforceWaterLimits == false
        && decoded.strokes.front().rulesSha256 == std::string(64, 'd')
        && !decoded.strokes.front().intent.waterBrush, "ordinary cliff and disabled water limits roundtrip independently of water brush");
    Require(Take(EncodeTerrainStrokeJournal(decoded)) == wire, "FTJ2 canonical bytes roundtrip");
    for (std::size_t size = 0; size < wire.size(); ++size)
        Require(!DecodeTerrainStrokeJournal(std::span(wire).first(size)).Succeeded(), "every FTJ2 truncation fails closed");
    stroke.intent.brush.kind = TerrainBrushKind::Raise;
    Require(!EncodeTerrainStrokeJournal(journal).Succeeded(), "non-cliff cannot impersonate native cliff rules");
}

struct SessionFixture {
    TerrainGridSnapshot terrain;
    Bytes terrainBytes, pathing;
    MapDataEditSession session{MapDocument::FromMembers({})};
    TerrainStrokePoint point{512, 512};
    SessionFixture() {
        terrain.header.width = terrain.header.height = 9;
        terrain.header.mainTileset = 'L'; terrain.header.groundTilesets = {"Tg01"}; terrain.header.cliffTilesets = {"Cl01"};
        terrain.vertices.resize(81);
        for (auto& vertex : terrain.vertices) { vertex.groundHeightRaw = 8320; vertex.waterHeightAndFlagsRaw = 7936; vertex.cliffAndLayerRaw = 2; }
        RefreshTerrainDerived(terrain, 0, 0, 9, 9); terrainBytes = Take(SerializeMapTerrain(terrain));
        pathing = {std::byte{'M'}, std::byte{'P'}, std::byte{'3'}, std::byte{'W'}};
        for (const auto value : {0u, 32u, 32u}) for (unsigned shift = 0; shift < 32; shift += 8)
            pathing.push_back(std::byte((value >> shift) & 255));
        pathing.resize(16 + 32 * 32, std::byte{0x11});
        Require(session.AttachTerrain(terrainBytes).Succeeded() && session.AttachPathing(pathing).Succeeded(), "attach owned fixture");
    }
    void Raise(float amount = 16) {
        TerrainBrush brush; brush.kind = TerrainBrushKind::Raise; brush.radius = 0; brush.strength = amount;
        Take(session.EditTerrain(brush, std::span(&point, 1), false));
    }
};
void LogicalQtReplayAndSavedBoundary() {
    SessionFixture f; f.Raise(); const auto first = f.session.LastEdit();
    const auto firstState = f.session.TerrainJournalStateFingerprint();
    f.Raise(); const auto second = f.session.LastEdit();
    Require(Take(f.session.ExportTerrainStrokeJournal()).strokes.size() == 2, "two real commands exported");
    Require(f.session.Replay(second, false).Succeeded(), "Qt inverse appends a command");
    Require(Take(f.session.ExportTerrainStrokeJournal()).strokes.size() == 1
        && f.session.TerrainJournalStateFingerprint() == firstState, "inverse reduces the logical top");
    Require(f.session.Replay(second, true).Succeeded(), "Qt redo restores logical prefix");
    Require(Take(f.session.ExportTerrainStrokeJournal()).strokes.size() == 2, "redo has two forward strokes");
    Require(f.session.Undo() && Take(f.session.ExportTerrainStrokeJournal()).strokes.size() == 1, "Core cursor undo preserves logical prefix");
    Require(f.session.Redo(), "Core redo succeeds");
    f.session.CommitSaved(f.session.Snapshot());
    Require(Take(f.session.ExportTerrainStrokeJournal()).strokes.empty(), "real save moves journal baseline");
    Require(f.session.Replay(second, false).Succeeded(), "saved history remains undoable");
    Require(!f.session.ExportTerrainStrokeJournal().Succeeded(), "Qt inverse of saved command cannot become forward journal");
    Require(f.session.TerrainJournalStateFingerprint().find("SAVED_BOUNDARY") != std::string::npos, "CAS sees the degraded state");
    (void)first;
    SessionFixture branched; branched.Raise(); branched.session.CommitSaved(branched.session.Snapshot());
    Require(branched.session.Undo(), "cross saved cursor remains a valid raw undo");
    branched.Raise(32);
    Require(!branched.session.ExportTerrainStrokeJournal().Succeeded(), "branch replacing saved prefix cannot reuse numeric cursor");
}
void OversizedUiStrokeAndOpaqueDraftRemainEditable() {
    SessionFixture f;
    TerrainBrush brush; brush.kind = TerrainBrushKind::Raise; brush.radius = 0;
    std::vector<TerrainStrokePoint> points(4097, f.point);
    Take(f.session.EditTerrain(brush, points, false));
    Require(f.session.IsDirty() && !f.session.ExportTerrainStrokeJournal().Succeeded(), "UI edit survives journal point overflow");
    Require(f.session.Undo() && Take(f.session.ExportTerrainStrokeJournal()).strokes.empty(), "no truncation of original undo command");
    SessionFixture raw; auto altered = raw.terrain; altered.vertices[40].groundHeightRaw += 16;
    RefreshTerrainDerived(altered, 0, 0, 9, 9);
    Require(raw.session.SetTerrain(Take(SerializeMapTerrain(altered))).Succeeded(), "legacy raw draft still accepted");
    Require(!raw.session.ExportTerrainStrokeJournal().Succeeded(), "raw draft cannot invent typed provenance");
}
} // namespace

int main() {
    try {
        StrictCodecRoundtripAndMalformedInputs(); NativeCliffVersionAndOptionsRemainExplicit();
        LogicalQtReplayAndSavedBoundary(); OversizedUiStrokeAndOpaqueDraftRemainEditable();
        std::cout << "TerrainStrokeJournalTests passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
