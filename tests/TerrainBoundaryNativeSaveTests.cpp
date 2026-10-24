#include "support/TerrainBoundsFixture.h"

namespace {
using namespace AudastraTest::TerrainBounds;
constexpr TerrainStrokePoint boundaryPoint{-384, -256}; // 地格(2,2)的SW顶点。
constexpr std::size_t boundaryTextureByte = 45 + (2 * 11 + 2) * 7 + 4;

void PaintBoundary(Fixture& fixture, bool value) {
    TerrainBrush brush; brush.kind = TerrainBrushKind::Boundary; brush.radius = 0; brush.value = value;
    const auto changed = Take(fixture.edits->EditTerrain(brush, std::span(&boundaryPoint, 1), false));
    Require(!changed.Empty(), "boundary command changes the selected wire vertex");
}

Bytes BoundaryWpm(const Fixture& fixture, std::byte mask) {
    auto expected = fixture.members[1].data;
    // 独立字面预期：只有Q/SW为(2,2)的一个地格对应4x4子格。
    // 东、北与东北角同样引用该顶点的另外三个地格不得被扩大阻断。
    for (unsigned y = 8; y < 12; ++y) for (unsigned x = 8; x < 12; ++x)
        expected[16 + y * 40 + x] = mask;
    return expected;
}

void RequireOtherMembersPreserved(const Fixture& fixture) {
    for (const auto& member : fixture.members) {
        if (member.name == "war3map.w3e" || member.name == "war3map.wpm") continue;
        Require(Take(fixture.archive->ReadMember(member.name)) == member.data,
            "boundary save preserves every unrelated source member byte");
    }
}

void BoundaryAddsRemovesAndRebasesSavedUndoRedo() {
    Fixture fixture;
    const auto originalTerrain = fixture.members[0].data, originalWpm = fixture.members[1].data;
    auto expectedTerrain = originalTerrain; expectedTerrain[boundaryTextureByte] |= std::byte{0x80};
    const auto expectedWpm = BoundaryWpm(fixture, std::byte{0xdf});
    PaintBoundary(fixture, true);
    Require(!fixture.edits->VerifyTerrainPathingForSave().Succeeded(),
        "boundary edits never pass the preservation-only pathing gate");
    Require(Take(SerializeMapTerrain(*fixture.edits->Terrain())) == expectedTerrain,
        "one boundary brush changes only its literal W3E texture high bit");
    fixture.Save("boundary-added.w3x");
    Require(Take(fixture.archive->ReadMember("war3map.w3e")) == expectedTerrain
        && Take(fixture.archive->ReadMember("war3map.wpm")) == expectedWpm,
        "ordinary native save writes exact complete boundary W3E and six-mode WPM");
    RequireOtherMembersPreserved(fixture);

    Require(fixture.edits->Undo(), "undo saved boundary command"); fixture.Save("boundary-undo.w3x");
    Require(Take(fixture.archive->ReadMember("war3map.w3e")) == originalTerrain
        && Take(fixture.archive->ReadMember("war3map.wpm")) == originalWpm,
        "saved Undo restores every original W3E and WPM byte");
    Require(fixture.edits->Redo(), "redo saved boundary command"); fixture.Save("boundary-redo.w3x");
    Require(Take(fixture.archive->ReadMember("war3map.w3e")) == expectedTerrain
        && Take(fixture.archive->ReadMember("war3map.wpm")) == expectedWpm,
        "saved Redo restores the same complete deterministic outputs");

    PaintBoundary(fixture, false); fixture.Save("boundary-removed.w3x");
    Require(Take(fixture.archive->ReadMember("war3map.w3e")) == originalTerrain
        && Take(fixture.archive->ReadMember("war3map.wpm")) == originalWpm,
        "explicit boundary removal restores native ground permissions against the saved boundary baseline");
    RequireOtherMembersPreserved(fixture);
}

void BoundaryRemovalRetainsBlightAndOwnedRestrictions() {
    Fixture fixture;
    TerrainBrush blight; blight.kind = TerrainBrushKind::Blight; blight.radius = 0;
    Take(fixture.edits->EditTerrain(blight, std::span(&boundaryPoint, 1), false));
    PaintBoundary(fixture, true);
    TerrainBrush restrict; restrict.kind = TerrainBrushKind::PathingRestriction; restrict.size = 1;
    restrict.pathingRestrictions = 0x04;
    constexpr TerrainStrokePoint pathingPoint{-368, -240}; // 32单位子格(8,8)的中心。
    Take(fixture.edits->EditTerrain(restrict, std::span(&pathingPoint, 1), false));
    auto blocked = BoundaryWpm(fixture, std::byte{0xdf});
    // 荒芜按最近顶点取色，覆盖[6,10)x[6,10)，独立于Boundary的所属SW地格。
    for (unsigned y = 6; y < 10; ++y) for (unsigned x = 6; x < 10; ++x)
        blocked[16 + y * 40 + x] |= std::byte{0x20};
    fixture.Save("boundary-blight-restriction.w3x");
    Require(Take(fixture.archive->ReadMember("war3map.wpm")) == blocked,
        "native boundary preserves blight and an overlapping explicit restriction");
    PaintBoundary(fixture, false);
    auto expected = fixture.members[1].data;
    for (unsigned y = 6; y < 10; ++y) for (unsigned x = 6; x < 10; ++x)
        expected[16 + y * 40 + x] |= std::byte{0x20};
    expected[16 + 8 * 40 + 8] = std::byte{0x75};
    fixture.Save("boundary-cleared-restriction-kept.w3x");
    auto terrain = fixture.members[0].data; terrain[boundaryTextureByte] |= std::byte{0x20};
    Require(Take(fixture.archive->ReadMember("war3map.w3e")) == terrain
        && Take(fixture.archive->ReadMember("war3map.wpm")) == expected,
        "boundary removal clears only its native contribution, retaining exact blight and independently owned restriction");
}

void BoundaryColdRecoveryRetainsOrdinarySave() {
    Fixture fixture;
    PaintBoundary(fixture, true);
    const auto terrain = Take(SerializeMapTerrain(*fixture.edits->Terrain()));
    const auto journal = Take(Services::TerrainPathingSave::CaptureJournal(*fixture.archive, *fixture.edits));
    const auto wire = Take(EncodeTerrainStrokeJournal(journal));
    Require(journal.strokes.size() == 1 && journal.strokes.front().intent.brush.kind == TerrainBrushKind::Boundary,
        "boundary command remains expressible in the shared project recovery journal");
    const auto original = MapDocument::FromMembers(fixture.members);
    MapDataEditSession raw(original);
    Check(raw.AttachTerrain(fixture.members[0].data)); Check(raw.AttachPathing(fixture.members[1].data));
    Check(raw.SetTerrainAndPathing(terrain, fixture.members[1].data));
    auto recovered = Take(Services::TerrainPathingSave::RestoreJournal(*fixture.archive, original,
        fixture.members[0].data, fixture.members[1].data, Take(DecodeTerrainStrokeJournal(wire)), raw, {}));
    Require(recovered.session->MatchesTerrainJournalDraft(raw) && recovered.receipts.size() == 1,
        "cold recovery reconstructs exact boundary bytes and a real Undo command");
    fixture.edits = std::move(recovered.session); fixture.Save("boundary-cold-recovered.w3x");
    Require(Take(fixture.archive->ReadMember("war3map.w3e")) == terrain
        && Take(fixture.archive->ReadMember("war3map.wpm")) == BoundaryWpm(fixture, std::byte{0xdf}),
        "cold-recovered boundary draft reaches ordinary native archive save");
}

void BoundaryCannotHideUnknownSourcesOrRawFlags() {
    Fixture unexplained;
    unexplained.members[1].data[16] ^= std::byte{0x04};
    unexplained.archive = std::make_unique<MapArchiveSession>(Take(MapArchiveSession::Open(
        Take(WriteVerifiedArchive(unexplained.directory / "unexplained.w3x", unexplained.members)).path)));
    unexplained.edits = std::make_unique<MapDataEditSession>(MapDocument::FromMembers(unexplained.members));
    Check(unexplained.edits->AttachTerrain(unexplained.members[0].data));
    Check(unexplained.edits->AttachPathing(unexplained.members[1].data));
    PaintBoundary(unexplained, true);
    const auto revision = unexplained.edits->Revision();
    const auto draft = Take(SerializeMapTerrain(*unexplained.edits->Terrain()));
    const auto plan = Take(TerrainSavePlan::PrepareForSave(*unexplained.archive, *unexplained.edits, {}));
    Require(HasCode(plan.SaveGate(), "TERRAIN_PATHING_SOURCE_UNEXPLAINED"),
        "boundary support cannot erase an unexplained original WPM bit outside the edited cell");
    Require(unexplained.edits->Revision() == revision && unexplained.edits->IsDirty()
        && Take(SerializeMapTerrain(*unexplained.edits->Terrain())) == draft
        && Take(unexplained.archive->ReadMember("war3map.wpm")) == unexplained.members[1].data,
        "source refusal preserves complete draft, revision and original archive bytes");
    Require(unexplained.edits->Undo() && unexplained.edits->Redo(), "source refusal retains the real Undo and Redo commands");

    Fixture unknown;
    PaintBoundary(unknown, true);
    auto raw = Take(SerializeMapTerrain(*unknown.edits->Terrain()));
    raw[45 + (2 * 11 + 2) * 7 + 3] |= std::byte{0x80};
    Check(unknown.edits->SetTerrain(raw));
    const auto refused = Take(TerrainSavePlan::PrepareForSave(*unknown.archive, *unknown.edits, {}));
    Require(HasCode(refused.SaveGate(), "MAP_TERRAIN_PATHING_UNVERIFIED"),
        "known Boundary bit support does not authorize unknown water high bits");
}
} // namespace

int main() {
    try {
        BoundaryAddsRemovesAndRebasesSavedUndoRedo();
        BoundaryRemovalRetainsBlightAndOwnedRestrictions();
        BoundaryColdRecoveryRetainsOrdinarySave();
        BoundaryCannotHideUnknownSourcesOrRawFlags();
        std::cout << "terrain boundary native save tests passed\n"; return 0;
    } catch (const std::exception& exception) { std::cerr << exception.what() << '\n'; return 1; }
}
