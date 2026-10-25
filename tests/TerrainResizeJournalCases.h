#pragma once

#include "support/TerrainResizeFixture.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/MapData/PathingRestrictionEditing.h"
#include "War3MapCore/Services/TerrainResizeEditService.h"
#include "War3MapCore/Services/TerrainShadowBake.h"

namespace AudastraTest::TerrainResizeJournalCases {
using namespace TerrainResize;
using War3Map::Services::TerrainResizeEditService;
using War3Map::Services::TerrainPathingSave;

inline void WireNumber(Bytes& output, std::uint64_t value, unsigned count = 4) {
    for (unsigned i = 0; i < count; ++i) output.push_back(std::byte((value >> (i * 8)) & 255));
}
inline void WireText(Bytes& output, std::string_view text) {
    WireNumber(output, text.size()); const auto data = Text(text); output.insert(output.end(), data.begin(), data.end());
}
inline TerrainStrokeJournal Journal() {
    TerrainStrokeJournal journal; journal.sourceSha256.assign(64, 'a');
    for (std::size_t i = 0; i < TerrainJournalMemberNames.size(); ++i)
        journal.members.push_back({std::string(TerrainJournalMemberNames[i]), i < 2,
            i < 2 ? std::string(64, 'b') : std::string{}, i < 2 ? 4u : 0u});
    TerrainJournalStroke stroke; stroke.intent.resize = TerrainResizeIntent{1, {-32, -64, 32, 32}};
    stroke.rulesSha256.assign(64, 'd'); journal.strokes.push_back(std::move(stroke)); return journal;
}
inline Bytes LiteralResizeJournal() {
    // FTJG独立wire，不调用正式编码器制造expected。四边使用signed I32的补码。
    Bytes output = Text("FTJG"); WireText(output, std::string(64, 'a'));
    WireNumber(output, TerrainJournalMemberNames.size(), 1);
    for (std::size_t i = 0; i < TerrainJournalMemberNames.size(); ++i) {
        WireText(output, TerrainJournalMemberNames[i]); WireNumber(output, i < 2, 1);
        WireText(output, i < 2 ? std::string(64, 'b') : std::string{}); WireNumber(output, i < 2 ? 4 : 0, 8);
    }
    WireNumber(output, 0); WireNumber(output, 1); WireNumber(output, 7, 1); WireNumber(output, 1, 1);
    for (const std::int32_t edge : {-32, -64, 32, 32}) WireNumber(output, std::bit_cast<std::uint32_t>(edge));
    WireText(output, std::string(64, 'd')); return output;
}
inline void ResizeCodecHasIndependentSignedWireAndDomain() {
    const auto expected = LiteralResizeJournal(); const auto encoded = Take(EncodeTerrainStrokeJournal(Journal()));
    Require(encoded == expected, "FTJG full independent wire includes signed west/south and exact rule digest");
    const auto decoded = Take(DecodeTerrainStrokeJournal(expected));
    Require(decoded.strokes.front().intent.resize == Journal().strokes.front().intent.resize
        && Take(EncodeTerrainStrokeJournal(decoded)) == expected, "FTJG canonical signed roundtrip");
    for (std::size_t size = 0; size < expected.size(); ++size)
        Require(!DecodeTerrainStrokeJournal(std::span(expected).first(size)).Succeeded(), "every truncated resize record rejects");
    for (const char oldVersion : {'D', 'F'}) {
        auto changed = expected; changed[3] = std::byte(oldVersion);
        Require(!DecodeTerrainStrokeJournal(changed).Succeeded(), "old block/corner versions cannot consume resize variant7");
    }
    auto changed = expected; changed.push_back(std::byte{0});
    Require(!DecodeTerrainStrokeJournal(changed).Succeeded(), "resize wire cannot hide unknown extension");
    auto invalid = Journal(); invalid.strokes[0].intent.points.push_back({0, 0});
    Require(!EncodeTerrainStrokeJournal(invalid).Succeeded(), "resize variant cannot carry ordinary points");
    invalid = Journal(); invalid.strokes[0].intent.brush.seed = 1;
    Require(!EncodeTerrainStrokeJournal(invalid).Succeeded(), "resize variant requires neutral ordinary brush");
    invalid = Journal(); invalid.strokes[0].intent.followDoodads = true;
    Require(!EncodeTerrainStrokeJournal(invalid).Succeeded(), "resize never implies object movement");
    invalid = Journal(); invalid.strokes[0].rulesSha256.clear();
    Require(!EncodeTerrainStrokeJournal(invalid).Succeeded(), "typed resize must bind actual rule digest");
    invalid = Journal(); invalid.strokes[0].intent.resize->algorithm = 2;
    Require(!EncodeTerrainStrokeJournal(invalid).Succeeded(), "unknown resize algorithm rejected");
}

inline void HistoryShadowIsSeparateAndAuthenticated() {
    auto journal = Journal(); TerrainJournalHistory history;
    for (std::size_t i = 0; i < TerrainJournalMemberNames.size(); ++i)
        history.baselineMembers.push_back({std::string(TerrainJournalMemberNames[i]), i < 2,
            i < 2 ? Bytes{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}} : Bytes{}});
    history.savedIndex = history.currentIndex = 1;
    history.baselineShadow = ArchiveExactMember{"war3map.shd", true, {std::byte{0}, std::byte{2}, std::byte{0x80}, std::byte{0xff}}};
    journal.history = history;
    const auto wire = Take(EncodeTerrainStrokeJournal(journal));
    Require(wire[3] == std::byte{'H'}, "resize save-station uses separate FTJH envelope");
    const auto restored = Take(DecodeTerrainStrokeJournal(wire));
    Require(restored.history && restored.history->baselineShadow && restored.history->baselineShadow->bytes == history.baselineShadow->bytes
        && Take(EncodeTerrainStrokeJournal(restored)) == wire, "FTJH retains old physical shadow byte-exactly");
    const auto baseline = Take(TerrainHistoryBaselineJournal(journal));
    auto changed = journal; changed.history->baselineShadow->bytes[1] = std::byte{1};
    Require(Take(TerrainHistoryBaselineJournal(changed)).sourceSha256 != baseline.sourceSha256,
        "old SHD participates in structural replay identity");
    auto oldMagic = wire; oldMagic[3] = std::byte{'E'};
    Require(!DecodeTerrainStrokeJournal(oldMagic).Succeeded(), "old FTJE never acquires new shadow layout");
    for (const auto size : {std::size_t{0}, std::size_t{3}, wire.size() / 2, wire.size() - 1})
        Require(!DecodeTerrainStrokeJournal(std::span(wire).first(size)).Succeeded(), "truncated history leaves no partial restore");
    auto missing = journal; missing.history->baselineShadow.reset();
    Require(!EncodeTerrainStrokeJournal(missing).Succeeded(), "resize history must record old shadow presence or absence");
    missing = journal; missing.history->baselineShadow = ArchiveExactMember{"war3map.shd", false, {}};
    const auto absent = Take(DecodeTerrainStrokeJournal(Take(EncodeTerrainStrokeJournal(missing))));
    Require(absent.history->baselineShadow && !absent.history->baselineShadow->present && absent.history->baselineShadow->bytes.empty(),
        "missing SHD is an explicit authenticated history input");
    missing.history->baselineShadow->bytes.push_back(std::byte{1});
    Require(!EncodeTerrainStrokeJournal(missing).Succeeded(), "absence cannot carry hidden shadow payload");
}

inline TerrainResizeIntent Intent(int west, int south, int east, int north) { return {1, {west, south, east, north}}; }
inline void Resize(Fixture& fixture, const TerrainResizeIntent& intent) {
    auto resources = Take(TerrainResizeEditService::Capture(*fixture.archive, *fixture.edits, fixture.gameRoot));
    Check(fixture.edits->ApplyPreparedTerrainResizeEdit(Take(fixture.edits->PrepareTerrainResizeEdit(resources, intent))));
}
inline Bytes Wpm(const Fixture& fixture) {
    for (const auto& item : fixture.edits->Replacements()) if (item.name == "war3map.wpm") return item.data;
    return Take(fixture.archive->ReadMember("war3map.wpm"));
}
inline void Restriction(Fixture& fixture, TerrainStrokePoint point, bool value = true) {
    TerrainBrush brush; brush.kind = TerrainBrushKind::PathingRestriction; brush.radius = 0;
    brush.pathingRestrictions = 2; brush.value = value;
    Take(fixture.edits->EditTerrain(brush, std::span(&point, 1), false));
}
inline void CoreSnapshotBaseline(Fixture& fixture) {
    // 仅测Core的保存基线/热历史契约；不是原版WPM生成或原版重开验收。
    const auto current = fixture.edits->Snapshot(); fixture.edits->CommitSaved(current);
}
inline void CroppedPathingCannotResurrectAtSameShape() {
    Fixture fixture;
    // 原西侧第八列记录来源P=不可走；另在相同列声明本工程S，并建立保存站。
    auto& source = fixture.Member("war3map.wpm");
    const std::size_t cell = 8 + 64 * 128;
    source[16 + cell] |= std::byte{2}; fixture.Seal();
    const TerrainStrokePoint westPoint{-1776, 16};
    Restriction(fixture, westPoint);
    CoreSnapshotBaseline(fixture);
    const auto savedWpm = Wpm(fixture);
    // 缩playable后把canvas向东平移四格；旧西侧四格被裁去，东侧补四格。
    const auto before = Take(ParseMapTerrain(fixture.Member("war3map.w3e")));
    const auto boundsOwner = Take(TerrainPathingResources::CaptureNative(*fixture.archive, *before.Snapshot(), *fixture.edits->Terrain(),
        fixture.gameRoot, 64u * 1024u * 1024u));
    TerrainBoundsEditIntent bounds; bounds.playable = {4, 4, 28, 28};
    Check(fixture.edits->ApplyPreparedTerrainBoundsEdit(Take(fixture.edits->PrepareTerrainBoundsEdit(boundsOwner, bounds))));
    Resize(fixture, Intent(4, 0, 36, 32));
    Resize(fixture, Intent(-4, 0, 28, 32));
    Require(fixture.edits->Terrain()->header.width == 33 && fixture.edits->Terrain()->header.height == 33
        && fixture.edits->Terrain()->header.offset == std::array<float, 2>{-2048, -2048}, "test returns to exactly the original shape");
    Require((Wpm(fixture)[16 + cell] & std::byte{2}) == std::byte{0}, "newly expanded area cannot resurrect cropped source P or saved S");
    const auto restrictions = Take(fixture.edits->QueryPathingRestrictions());
    Require(restrictions.totalCells == 0, "cropped current restriction intent also stays absent");
    // 一次正常限制笔划会再次解析baseline/savedS；这里同头旧savedS也不得复活。
    Restriction(fixture, westPoint, false);
    Require((Wpm(fixture)[16 + cell] & std::byte{2}) == std::byte{0}, "pathing edit resolves mapped state identity instead of same-shape physical baseline");
    Require(fixture.edits->Undo(), "undo second resize");
    Require(fixture.edits->Undo(), "undo first resize");
    Require(fixture.edits->Undo(), "undo bounds shrink");
    Require(Wpm(fixture) == savedWpm, "actual Undo restores cropped P and S exactly through original state identity");
    Require(fixture.edits->Redo() && fixture.edits->Redo() && fixture.edits->Redo(), "mapped crop/expand sequence supports full redo");
    Require((Wpm(fixture)[16 + cell] & std::byte{2}) == std::byte{0}, "redo repeats crop semantics without resurrection");
}

inline void RawDraftHasNoSaveAuthorityAndShadowIsCompared() {
    Fixture fixture; Resize(fixture, Intent(-32, 0, 32, 32));
    const auto grid = Take(SerializeMapTerrain(*fixture.edits->Terrain())); const auto wpm = Wpm(fixture);
    const auto draft = Take(fixture.edits->ExportTerrainResizeDraft());
    auto raw = fixture.FreshSession();
    Check(raw->SetSerialized(MapDataKind::Settings, fixture.edits->Snapshot().Member(MapDataKind::Settings).sources.front().data));
    Check(raw->SetTerrainResizeDraft(grid, wpm, draft));
    Require(fixture.edits->MatchesTerrainJournalDraft(*raw), "complete raw output can be compared without installing typed authority");
    Require(!raw->VerifyTerrainPathingForSave().Succeeded(), "TRD1 is raw inspection and never grants native-save authority");
    Require(!raw->VerifyTerrainCurrentStructureInput().Succeeded(), "raw TRD1 cannot create a trusted current resource view");
    Require(raw->Undo() && !raw->HasTerrainResizeChanges() && Take(raw->ExportTerrainResizeDraft()).empty(),
        "raw inspection undo detaches the draft instead of inventing unknown source shadow");
    Require(!raw->TerrainResizeShadowDraft().Succeeded(), "detached raw baseline reports no shadow draft");
    Require(raw->Redo() && fixture.edits->MatchesTerrainJournalDraft(*raw), "raw inspection redo restores exact current output");
    auto changed = draft; changed.back() ^= std::byte{0xff};
    auto differentShadow = fixture.FreshSession();
    Check(differentShadow->SetSerialized(MapDataKind::Settings, fixture.edits->Snapshot().Member(MapDataKind::Settings).sources.front().data));
    Check(differentShadow->SetTerrainResizeDraft(grid, wpm, changed));
    Require(!fixture.edits->MatchesTerrainJournalDraft(*differentShadow), "final typed/raw comparison includes every SHD byte");
    changed = draft; changed[8] ^= std::byte{1};
    auto changedIdentity = fixture.FreshSession();
    const auto revision = changedIdentity->Revision();
    Require(!changedIdentity->SetTerrainResizeDraft(grid, wpm, changed).Succeeded() && changedIdentity->Revision() == revision,
        "mismatched original W3E identity cannot install partial raw state");
    Require(!changedIdentity->SetTerrainResizeDraft(grid, wpm, std::span(draft).first(draft.size() - 1)).Succeeded(),
        "truncated TRD1 rejected atomically");
}

inline void SavedBakeIsUsedByFollowingResizeAndUndo() {
    Fixture fixture;
    Check(IO::WriteFileBytes(fixture.gameRoot / "WorldEditData.txt", Text(
        "[WorldEditMisc]\nMinimumMapSize=32\nMaximumMapSize=480\n[CameraMargins]\nLeft=1\nRight=2\nBottom=1\nTop=2\n"
        "[Light]\nDirection=3,0,-4\n")));
    Resize(fixture, Intent(-32, 0, 32, 32));
    auto output = fixture.edits->Replacements();
    auto plan = Take(TerrainPathingSave::PrepareForSave(*fixture.archive, *fixture.edits, fixture.gameRoot, {}, output));
    Check(plan.SaveGate()); Check(plan.InstallPathingOutput(*fixture.edits, output));
    const auto bake = Take(War3Map::Services::TerrainShadowBake::Capture(*fixture.archive, fixture.gameRoot, output));
    Require(std::all_of(bake->Bytes().begin(), bake->Bytes().end(), [](auto value) { return value == std::byte{0}; }),
        "flat original map bakes to independent all-zero shadow expected");
    Check(bake->AdmitToPlan(plan, *fixture.edits));
    std::erase_if(output, [](const auto& item) { return item.name == "war3map.shd"; });
    output.push_back({"war3map.shd", bake->Bytes()});
    Check(plan.VerifyForSave(*fixture.edits, fixture.archive->SourceSha256(), output));
    auto saved = Take(MapArchiveSession::Open(Take(fixture.archive->SaveCandidate(fixture.directory / "resized-baked.w3x", output)).path));
    Check(plan.VerifyReadback(saved)); Check(bake->VerifyReadback(saved));
    Check(plan.CommitSaved(*fixture.edits, MapDocument::FromArchive(saved)));
    fixture.archive = std::make_unique<MapArchiveSession>(std::move(saved));
    const auto written = Take(fixture.archive->ReadMember("war3map.shd"));
    Require(Take(fixture.edits->TerrainResizeShadowDraft()).bytes == written && !fixture.edits->IsDirty(),
        "actual saved bake becomes the effective clean shadow baseline");
    Resize(fixture, Intent(0, -32, 64, 32));
    const auto nextShadow = Take(fixture.edits->TerrainResizeShadowDraft()).bytes;
    Require(nextShadow.size() == 256u * 256u && std::all_of(nextShadow.begin(), nextShadow.end(), [](auto value) { return value == std::byte{0}; }),
        "following resize uses actual saved bake instead of stale migration SHD");
    Require(fixture.edits->Undo() && Take(fixture.edits->TerrainResizeShadowDraft()).bytes == written,
        "undo following resize restores actual saved bake");
    Require(fixture.edits->Undo() && Take(fixture.edits->TerrainResizeShadowDraft()).bytes == ShadowLiteral(),
        "undo across saved shape restores exact original disk SHD");
}

inline void TestTerrainResizeJournalCases() {
    ResizeCodecHasIndependentSignedWireAndDomain();
    HistoryShadowIsSeparateAndAuthenticated();
    CroppedPathingCannotResurrectAtSameShape();
    RawDraftHasNoSaveAuthorityAndShadowIsCompared();
    SavedBakeIsUsedByFollowingResizeAndUndo();
}
} // namespace AudastraTest::TerrainResizeJournalCases
