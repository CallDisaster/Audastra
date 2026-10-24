#include "support/TerrainBoundsFixture.h"

namespace {
using namespace AudastraTest::TerrainBounds;
void BoundsSaveHasLiteralFullByteOraclesAndSavedUndo() {
    Fixture f;
    auto expectedTerrain = f.members[0].data;
    for (unsigned y = 0; y < 8; ++y) for (unsigned x = 0; x < 10; ++x)
        if (x < 1 || x >= 9 || y < 1 || y >= 7) expectedTerrain[45 + (y * 11 + x) * 7 + 3] |= std::byte{0x40};
    auto expectedSettings = Take(MapSettings::Parse(f.members[2].data));
    expectedSettings.cameraBounds = {-384, -256, 256, 128, -384, 128, 256, -256};
    expectedSettings.cameraMargins = {1, 1, 1, 1}; expectedSettings.playableWidth = 8; expectedSettings.playableHeight = 6;
    const auto expectedW3i = Take(expectedSettings.Serialize());
    auto expectedPathing = f.members[1].data;
    for (unsigned y = 0; y < 32; ++y) for (unsigned x = 0; x < 40; ++x)
        if (x / 4 < 1 || x / 4 >= 9 || y / 4 < 1 || y / 4 >= 7) expectedPathing[16 + y * 40 + x] = std::byte{0xdf};
    const auto resources = f.Capture();
    const auto prepared = Take(f.edits->PrepareTerrainBoundsEdit(resources, {{1, 1, 9, 7}}));
    const auto revision = f.edits->Revision();
    Require(prepared.HasChanges() && prepared.ChangedMapEdges() == 32, "exact SW edge count for non-square inset");
    Check(f.edits->ApplyPreparedTerrainBoundsEdit(prepared));
    Require(f.edits->Revision() == revision + 1 && f.edits->HasTerrainBoundsChanges(), "one compound command advances one revision");
    Check(f.edits->VerifyTerrainSettingsForSave());
    Require(!f.edits->VerifyTerrainPathingForSave().Succeeded(), "preservation-only route cannot approve new bounds");
    f.Save("inset.w3x");
    Require(Take(f.archive->ReadMember("war3map.w3e")) == expectedTerrain, "complete W3E matches independent MapEdge byte oracle");
    Require(Take(f.archive->ReadMember("war3map.w3i")) == expectedW3i, "complete W3I matches independent camera/margin oracle");
    Require(Take(f.archive->ReadMember("war3map.wpm")) == expectedPathing, "complete WPM moves only native playable restrictions and preserves 0x11");
    Require(f.edits->Undo(), "undo bounds after native save"); Check(f.edits->VerifyTerrainSettingsForSave()); f.Save("undo.w3x");
    Require(Take(f.archive->ReadMember("war3map.w3e")) == f.members[0].data
        && Take(f.archive->ReadMember("war3map.w3i")) == f.members[2].data
        && Take(f.archive->ReadMember("war3map.wpm")) == f.members[1].data, "undo/save restores all three complete source members");
    Require(f.edits->Redo(), "redo bounds across second saved baseline"); f.Save("redo.w3x");
    Require(Take(f.archive->ReadMember("war3map.w3e")) == expectedTerrain
        && Take(f.archive->ReadMember("war3map.w3i")) == expectedW3i
        && Take(f.archive->ReadMember("war3map.wpm")) == expectedPathing, "redo/save recreates complete deterministic bytes");
}

void PreparationNoOpStaleAndPrepublishAreAtomic() {
    Fixture f;
    const auto owner = f.Capture();
    const auto digestBytes = owner.TerrainBoundsDependencyBudgetBytes();
    Require(digestBytes > 512 && owner.TerrainBoundsDependencyFingerprint(digestBytes).Succeeded()
        && !owner.TerrainBoundsDependencyFingerprint(digestBytes - 1).Succeeded(),
        "actual dependency wire is premeasured and enforces exact and one-byte-low budgets");
    const auto noOp = Take(f.edits->PrepareTerrainBoundsEdit(owner, {{0, 0, 10, 8}}));
    Require(!noOp.HasChanges(), "already canonical full bounds form a no-op");
    Check(f.edits->ApplyPreparedTerrainBoundsEdit(noOp)); Require(f.edits->Revision() == 0 && !f.edits->CanUndo(), "no-op consumes no revision or history");
    const auto prepared = Take(f.edits->PrepareTerrainBoundsEdit(owner, {{1, 1, 9, 7}}));
    const auto bytes = prepared.PreparationBytes(), work = prepared.PreparationWork();
    Require(f.edits->PrepareTerrainBoundsEdit(owner, {{1, 1, 9, 7}}, bytes, work).Succeeded(), "exact candidate/work budgets succeed");
    Require(!f.edits->PrepareTerrainBoundsEdit(owner, {{1, 1, 9, 7}}, bytes - 1, work).Succeeded()
        && !f.edits->PrepareTerrainBoundsEdit(owner, {{1, 1, 9, 7}}, bytes, work - 1).Succeeded(), "one-unit-low budgets reject before mutation");
    const auto rejected = f.edits->ApplyPreparedTerrainBoundsEdit(prepared, [](const auto&) {
        return Status::Failure("Test", "BOUNDS_PREPUBLISH_REJECTED", "synthetic refusal");
    });
    Require(HasCode(rejected, "BOUNDS_PREPUBLISH_REJECTED") && f.edits->Revision() == 0 && !f.edits->IsDirty(), "prepublish refusal is fully atomic");
    auto settings = *f.edits->Snapshot().Settings(); settings.mapName = "changed description field";
    Check(f.edits->EditSettings(settings));
    Require(HasCode(f.edits->ApplyPreparedTerrainBoundsEdit(prepared), "TERRAIN_BOUNDS_EDIT_STALE"), "any intervening revision rejects prepared bounds");
    const auto fresh = Take(f.edits->PrepareTerrainBoundsEdit(owner, {{1, 1, 9, 7}}));
    Check(f.edits->ApplyPreparedTerrainBoundsEdit(fresh)); Check(f.edits->VerifyTerrainSettingsForSave());
    auto tampered = *f.edits->Snapshot().Settings(); tampered.cameraBounds[0] += 128;
    Require(HasCode(f.edits->EditSettings(tampered), "TERRAIN_BOUNDS_TYPED_COMMAND_REQUIRED"), "raw settings cannot forge typed bounds proof");
    Check(f.edits->VerifyTerrainSettingsForSave());
    f.Save("description-and-bounds.w3x");
    Require(f.edits->Snapshot().Settings()->mapName == "changed description field", "four description fields combine with owned geometry");
    auto afterSave = *f.edits->Snapshot().Settings(); afterSave.cameraMargins[0] += 1;
    const auto revision = f.edits->Revision();
    Require(HasCode(f.edits->EditSettings(afterSave), "TERRAIN_BOUNDS_TYPED_COMMAND_REQUIRED")
        && HasCode(f.edits->SetSerialized(MapDataKind::Settings, Take(afterSave.Serialize())), "TERRAIN_BOUNDS_TYPED_COMMAND_REQUIRED"),
        "both raw write paths reject camera-margin tampering even after bounds were saved");
    Require(f.edits->Revision() == revision && !f.edits->IsDirty(), "saved bounds tampering preserves revision and clean bytes");
    const auto unchangedPlan = Take(TerrainSavePlan::PrepareForSave(*f.archive, *f.edits, {}));
    Check(unchangedPlan.SaveGate());
}

void BoundsColdRecoveryRetainsWholeDraftAndDescriptionFields() {
    Fixture f;
    auto settings = *f.edits->Snapshot().Settings(); settings.description = "description before bounds";
    Check(f.edits->EditSettings(settings));
    Check(f.edits->ApplyPreparedTerrainBoundsEdit(Take(f.edits->PrepareTerrainBoundsEdit(f.Capture(), {{1, 1, 9, 7}}))));
    TerrainBrush raise; raise.kind = TerrainBrushKind::Raise; raise.radius = 0; raise.strength = 8;
    const TerrainStrokePoint point{0, 0}; Take(f.edits->EditTerrain(raise, std::span(&point, 1), false));
    settings = *f.edits->Snapshot().Settings(); settings.author = "description after brush";
    Check(f.edits->EditSettings(settings));
    const auto original = MapDocument::FromMembers(f.members);
    const auto journal = Take(Services::TerrainPathingSave::CaptureJournal(*f.archive, *f.edits));
    Require(journal.strokes.size() == 2 && journal.strokes[0].intent.bounds
        && journal.strokes[1].intent.brush.kind == TerrainBrushKind::Raise,
        "description-only commands are proven and omitted from ordered typed terrain journal");
    const auto wire = Take(EncodeTerrainStrokeJournal(journal));
    Require(wire[3] == std::byte{'5'}, "owned bounds require FTJ5");
    const auto terrain = Take(SerializeMapTerrain(*f.edits->Terrain()));
    const auto replacements = f.edits->Replacements();
    const auto pathing = std::find_if(replacements.begin(), replacements.end(), [](const auto& item) { return item.name == "war3map.wpm"; });
    const auto& draftPathing = pathing == replacements.end() ? f.members[1].data : pathing->data;
    MapDataEditSession raw(original);
    Check(raw.AttachTerrain(f.members[0].data)); Check(raw.AttachPathing(f.members[1].data));
    Check(raw.SetTerrainAndPathing(terrain, draftPathing));
    Check(raw.SetSerialized(MapDataKind::Settings, Take(settings.Serialize())));
    Require(!raw.VerifyTerrainSettingsForSave().Succeeded(), "raw geometry does not acquire owned bounds save permission");
    auto recovered = Take(Services::TerrainPathingSave::RestoreJournal(*f.archive, original,
        f.members[0].data, f.members[1].data, Take(DecodeTerrainStrokeJournal(wire)), raw, {}));
    Require(recovered.session->MatchesTerrainJournalDraft(raw) && recovered.session->HasTerrainBoundsChanges(),
        "source-bound replay reconstructs geometry ownership and every raw draft byte");
    Check(recovered.session->VerifyTerrainSettingsForSave());
    Require(Take(Services::TerrainPathingSave::CaptureJournal(*f.archive, *recovered.session)).strokes.size() == 2,
        "recovered description bridge remains expressible for another cold recovery");
    Require(recovered.receipts.size() == 3, "description bridge is explicitly included in the recovered shared Undo receipts");
    for (auto i = recovered.receipts.rbegin(); i != recovered.receipts.rend(); ++i) Check(recovered.session->Replay(*i, false));
    Require(!recovered.session->IsDirty(), "Qt-equivalent reverse receipt replay restores complete source after cold recovery");
    for (const auto& receipt : recovered.receipts) Check(recovered.session->Replay(receipt, true));
    Require(recovered.session->MatchesTerrainJournalDraft(raw), "Qt-equivalent receipt redo recreates exact raw W3I/W3E/WPM");
    auto changedRules = journal; changedRules.strokes[0].rulesSha256[0] = changedRules.strokes[0].rulesSha256[0] == '0' ? '1' : '0';
    Require(!Services::TerrainPathingSave::RestoreJournal(*f.archive, original, f.members[0].data,
        f.members[1].data, changedRules, raw, {}).Succeeded(), "changed actual dependency digest cannot install a prefix");
    Services::TerrainReplayLimits tiny; tiny.maximumCandidateBytes = 1;
    Require(!Services::TerrainPathingSave::RestoreJournal(*f.archive, original, f.members[0].data,
        f.members[1].data, journal, raw, {}, tiny).Succeeded(), "whole recovery rejects before exceeding its candidate budget");
    auto tampered = settings; tampered.flags ^= 0x20;
    Check(raw.SetSerialized(MapDataKind::Settings, Take(tampered.Serialize())));
    Require(!Services::TerrainPathingSave::RestoreJournal(*f.archive, original, f.members[0].data,
        f.members[1].data, journal, raw, {}).Succeeded(), "description bridge cannot accept unrelated raw W3I flags");
    f.edits = std::move(recovered.session); f.Save("cold-recovered.w3x");
    Require(Take(f.archive->ReadMember("war3map.w3e")) == terrain
        && Take(f.archive->ReadMember("war3map.w3i")) == Take(settings.Serialize()),
        "cold-recovered owned geometry and descriptions reach ordinary native archive save");
}
}

int main() {
    try {
        BoundsSaveHasLiteralFullByteOraclesAndSavedUndo();
        PreparationNoOpStaleAndPrepublishAreAtomic();
        BoundsColdRecoveryRetainsWholeDraftAndDescriptionFields();
        std::cout << "terrain bounds session tests passed\n"; return 0;
    } catch (const std::exception& exception) { std::cerr << exception.what() << '\n'; return 1; }
}
