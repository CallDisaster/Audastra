#pragma once

#include "../support/TerrainResizeFixture.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/MapData/NativeEditorProfileInputs.h"
#include "War3MapCore/Services/TerrainPathingSave.h"
#include "War3MapCore/Services/TerrainResizeEditService.h"

// root在同一Services测试进程登记。全部使用原创归档和真实typed命令；
// 写盘回读只证明本产品保存链，不代替原版编辑器或游戏验收。
namespace War3Map::TerrainResizeReplayTests {
using namespace AudastraTest::TerrainResize;
using Services::TerrainPathingSave;
using Services::TerrainResizeEditService;

inline void Apply(Fixture& fixture, const TerrainResizeIntent& intent = {1, {-32, -32, 32, 32}}) {
    auto owner = Take(TerrainResizeEditService::Capture(*fixture.archive, *fixture.edits, fixture.gameRoot));
    auto prepared = Take(fixture.edits->PrepareTerrainResizeEdit(owner, intent));
    Check(owner.VerifySources(*fixture.archive, *fixture.edits));
    Check(fixture.edits->ApplyPreparedTerrainResizeEdit(prepared));
}

inline std::unique_ptr<MapDataEditSession> RawDraft(Fixture& fixture) {
    auto raw = fixture.FreshSession();
    const auto current = Take(SerializeMapTerrain(*fixture.edits->Terrain()));
    const auto pathing = fixture.edits->Replacements();
    const auto currentWpm = std::find_if(pathing.begin(), pathing.end(), [](const auto& value) { return value.name == "war3map.wpm"; });
    const auto& bytes = currentWpm == pathing.end() ? fixture.Member("war3map.wpm") : currentWpm->data;
    Check(raw->SetTerrainResizeDraft(current, bytes, Take(fixture.edits->ExportTerrainResizeDraft())));
    const auto document = fixture.edits->Snapshot();
    const auto original = MapDocument::FromMembers(fixture.members);
    for (const auto& member : document.Members()) {
        const auto& old = original.Member(member.kind);
        if (member.sources.size() == 1 && (old.sources.empty() || member.sources.front().data != old.sources.front().data))
            Check(raw->SetSerialized(member.kind, member.sources.front().data));
        else if (member.state == MapDataState::Missing && old.state != MapDataState::Missing) Check(raw->Remove(member.kind));
    }
    Require(raw->MatchesTerrainJournalDraft(*fixture.edits), "raw resize draft retains exact current geometry, W3I, P/S and SHD without typed authority");
    return raw;
}

inline void ResizeReplayRestoresTypedAuthorityAndRejectsTampering() {
    Fixture fixture(true); Apply(fixture);
    const auto originalSha = fixture.archive->SourceSha256();
    const auto encoded = Take(EncodeTerrainStrokeJournal(Take(TerrainPathingSave::CaptureJournal(*fixture.archive, *fixture.edits))));
    Require(encoded[3] == std::byte{'G'}, "forward resize uses canonical FTJG rather than ordinary brush placeholders");
    auto journal = Take(DecodeTerrainStrokeJournal(encoded));
    auto raw = RawDraft(fixture);
    const auto document = MapDocument::FromMembers(fixture.members);
    auto restored = Take(TerrainPathingSave::RestoreJournal(*fixture.archive, document, fixture.Member("war3map.w3e"),
        fixture.Member("war3map.wpm"), journal, *raw, fixture.gameRoot));
    Require(restored.receipts.size() == 1 && restored.resizeOwners.size() == 1 && restored.session->MatchesTerrainJournalDraft(*raw),
        "actual sources rebuild one owned resize command and match every raw endpoint byte");
    Require(Take(SerializeMapTerrain(*restored.session->Terrain())) == TerrainLiteral(-32, -32, 32, 32)
        && Take(restored.session->TerrainResizeShadowDraft()).bytes == ShadowLiteral(-32, -32, 32, 32, true),
        "signed southwest expansion preserves world data and independently expected SHD");
    Check(restored.session->Replay(restored.receipts.front(), false));
    Require(!restored.session->IsDirty() && Take(SerializeMapTerrain(*restored.session->Terrain())) == TerrainLiteral(),
        "cold typed receipt undoes to exact physical original");
    Check(restored.session->Replay(restored.receipts.front(), true));
    Require(restored.session->MatchesTerrainJournalDraft(*raw), "cold typed receipt redoes complete resize state");
    auto changed = journal; changed.strokes.front().rulesSha256[0] ^= 1;
    Require(!TerrainPathingSave::RestoreJournal(*fixture.archive, document, fixture.Member("war3map.w3e"),
        fixture.Member("war3map.wpm"), changed, *raw, fixture.gameRoot).Succeeded(), "changed actual resize dependency identity is rejected");
    auto forged = fixture.FreshSession(); auto capsule = Take(fixture.edits->ExportTerrainResizeDraft()); capsule.back() ^= std::byte{1};
    const auto replacements = fixture.edits->Replacements();
    const auto wpm = std::find_if(replacements.begin(), replacements.end(), [](const auto& value) { return value.name == "war3map.wpm"; });
    Require(wpm != replacements.end(), "expanded raw test has complete WPM");
    Check(forged->SetTerrainResizeDraft(Take(SerializeMapTerrain(*fixture.edits->Terrain())), wpm->data, capsule));
    Check(forged->SetSerialized(MapDataKind::Settings, Take(fixture.edits->Snapshot().Settings()->Serialize())));
    Require(!TerrainPathingSave::RestoreJournal(*fixture.archive, document, fixture.Member("war3map.w3e"),
        fixture.Member("war3map.wpm"), journal, *forged, fixture.gameRoot).Succeeded(), "arbitrary raw SHD never obtains typed resize authority");
    Require(Take(Sha256File(fixture.archive->SourcePath())) == originalSha, "replay and rejected raw changes keep original archive immutable");
}

inline void ResizeReplayUsesCurrentShapeAndAggregateBudgets() {
    Fixture fixture;
    TerrainStrokeIntent expand; expand.resize = TerrainResizeIntent{1, {-32, -32, 32, 32}};
    TerrainStrokeIntent crop; crop.resize = TerrainResizeIntent{1, {32, 32, 64, 64}};
    const std::array strokes{expand, crop};
    auto replayed = Take(TerrainPathingSave::ReplayStrokes(*fixture.archive, MapDocument::FromMembers(fixture.members),
        fixture.Member("war3map.w3e"), fixture.Member("war3map.wpm"), strokes, fixture.gameRoot));
    Require(replayed.receipts.size() == 2 && replayed.resizeOwners.size() == 2
        && Take(SerializeMapTerrain(*replayed.session->Terrain())) == TerrainLiteral(),
        "each resize rectangle is relative to its own before canvas rather than initial shape");
    Require(Take(replayed.session->TerrainResizeShadowDraft()).bytes == ShadowLiteral(0, 0, 32, 32, true),
        "expand then crop keeps independently normalized SHD rather than rereading old physical bytes");
    auto conflict = expand; conflict.points.push_back({0, 0});
    const std::array conflicting{conflict};
    Require(!TerrainPathingSave::ReplayStrokes(*fixture.archive, MapDocument::FromMembers(fixture.members),
        fixture.Member("war3map.w3e"), fixture.Member("war3map.wpm"), conflicting, fixture.gameRoot).Succeeded(),
        "typed resize refuses brush payload mixing");
    Services::TerrainReplayLimits limits; limits.maximumWork = 1;
    Require(!TerrainPathingSave::ReplayStrokes(*fixture.archive, MapDocument::FromMembers(fixture.members),
        fixture.Member("war3map.w3e"), fixture.Member("war3map.wpm"), strokes, fixture.gameRoot, limits).Succeeded(),
        "aggregate replay cannot exceed caller tightened work cap or return a successful prefix");
}

inline void ResizeHistoryKeepsOldShadowAcrossPhysicalSaveStation() {
    for (const bool shadow : {false, true}) for (const std::size_t current : {0u, 1u}) {
        Fixture fixture(false, shadow); Apply(fixture);
        const auto oldSource = fixture.archive->SourcePath();
        const auto oldSha = fixture.archive->SourceSha256();
        const auto lease = Take(fixture.edits->PrepareTerrainHistoryForSave());
        const auto plan = Take(TerrainPathingSave::PrepareForSave(*fixture.archive, *fixture.edits, fixture.gameRoot));
        Check(plan.SaveGate()); auto replacements = fixture.edits->Replacements();
        Check(plan.InstallPathingOutput(*fixture.edits, replacements));
        Check(plan.VerifyForSave(*fixture.edits, oldSha, replacements));
        auto members = fixture.members;
        for (const auto& replacement : replacements) {
            auto found = std::find_if(members.begin(), members.end(), [&](const auto& value) { return value.name == replacement.name; });
            if (found == members.end()) members.push_back({replacement.name, replacement.data}); else found->data = replacement.data;
        }
        const auto stationPath = fixture.directory / "resize-physical-station.w3x";
        const auto station = Take(WriteVerifiedArchive(stationPath, members));
        auto saved = std::make_unique<MapArchiveSession>(Take(MapArchiveSession::Open(station.path)));
        Check(plan.VerifyReadback(*saved)); Check(plan.CommitSaved(*fixture.edits, MapDocument::FromArchive(*saved)));
        Check(fixture.edits->CommitTerrainHistorySave(lease));
        fixture.archive = std::move(saved); fixture.members = std::move(members);
        if (!current) Require(fixture.edits->Undo(), "hot undo crosses resized physical station");
        auto raw = RawDraft(fixture);
        const auto wire = Take(EncodeTerrainStrokeJournal(Take(TerrainPathingSave::CaptureJournal(*fixture.archive, *fixture.edits))));
        const auto journal = Take(DecodeTerrainStrokeJournal(wire));
        Require(wire[3] == std::byte{'H'} && journal.history && journal.history->baselineShadow
            && journal.history->baselineShadow->present == shadow && journal.history->savedIndex == 1
            && journal.history->currentIndex == current, "FTJH preserves historical shadow presence separately from current physical shape");
        auto restored = Take(TerrainPathingSave::RestoreJournal(*fixture.archive, MapDocument::FromMembers(fixture.members),
            fixture.Member("war3map.w3e"), fixture.Member("war3map.wpm"), journal, *raw, fixture.gameRoot));
        Require(restored.currentIndex == current && restored.savedIndex == 1 && restored.receipts.size() == 1
            && restored.session->MatchesTerrainJournalDraft(*raw), "cold FTJH rebuilds complete old/current shadow across a real archive save station");
        if (!current) Check(restored.session->Replay(restored.receipts.front(), true));
        Require(!restored.session->IsDirty(), "cold redo reaches the exact resized physical clean station");
        Check(restored.session->Replay(restored.receipts.front(), false));
        const auto originalShadow = Take(restored.session->TerrainResizeShadowDraft());
        Require(originalShadow.present == shadow && originalShadow.bytes == (shadow ? ShadowLiteral() : Bytes{}),
            "cold undo restores historical raw SHD bytes or explicit absence rather than current disk data");
        auto tampered = journal;
        if (shadow) tampered.history->baselineShadow->bytes.back() ^= std::byte{1};
        else { tampered.history->baselineShadow->present = true; tampered.history->baselineShadow->bytes = ShadowLiteral(); }
        Require(!TerrainPathingSave::RestoreJournal(*fixture.archive, MapDocument::FromMembers(fixture.members),
            fixture.Member("war3map.w3e"), fixture.Member("war3map.wpm"), tampered, *raw, fixture.gameRoot).Succeeded(),
            "changed old-epoch SHD identity cannot borrow current physical save authority");
        Require(Take(Sha256File(oldSource)) == oldSha, "history restoration keeps pre-station original source immutable");
    }
}

inline void ResizeReplayComposesWithCurrentWaterCliffPaletteBoundsAndOrdinaryBrushes() {
    for (unsigned scenario = 0; scenario < 5; ++scenario) {
        Fixture fixture;
        // 本组走真实水规则捕获；只给这些原创场景补齐全部启动profile，
        // 1536派生经典14层上限，不改共享fixture或放宽产品的0..14校验。
        for (const auto& profile : kClassicProfiles) {
            if (std::none_of(fixture.members.begin(), fixture.members.end(), [&](const auto& member) { return member.name == profile.path; }))
                fixture.PutMember(profile.path, AudastraTest::TerrainBounds::Text("[Synthetic]\nPresent=1\n"));
        }
        fixture.PutMember("UI/MiscData.txt", AudastraTest::TerrainBounds::Text("[Terrain]\nMaxSlope=90\nMinHeight=-256\nMaxHeight=1536\n"
            "[Water]\nDeepLevelPathing=192\n[CameraMargins]\nLeft=1\nRight=2\nBottom=1\nTop=2\n"));
        fixture.PutMember("UI/WorldEditData.txt", AudastraTest::TerrainBounds::Text("[Synthetic]\nPresent=1\n"));
        fixture.Member("TerrainArt/Terrain.slk") = Slk({"tileID", "walkable", "flyable", "buildable"},
            {{"Tg01", "1", "1", "1"}, {"Tg02", "1", "1", "0"}});
        fixture.Seal();
        TerrainStrokeIntent expand; expand.resize = TerrainResizeIntent{1, {-32, -32, 32, 32}};
        TerrainStrokeIntent second;
        if (scenario == 0) {
            second.waterBrush = TerrainWaterStrokeOptions{}; second.waterBrush->size = 1;
            second.waterBrush->shape = TerrainBrushShape::Square; second.points = {{-1024, -1024}};
        } else if (scenario == 1) {
            second.brush.kind = TerrainBrushKind::CliffLevel; second.brush.size = 1;
            second.brush.shape = TerrainBrushShape::Square; second.brush.levelDelta = 1; second.points = {{-1024, -1024}};
        } else if (scenario == 2) {
            second.palette = TerrainPaletteEditIntent{TerrainTextureTable::Ground, 0, "Tg01", "Tg02", 1};
        } else if (scenario == 3) {
            second.bounds = TerrainBoundsEditIntent{{34, 34, 60, 60}, 1};
        } else {
            second.brush.kind = TerrainBrushKind::Raise; second.brush.size = 1;
            second.brush.shape = TerrainBrushShape::Square; second.brush.strength = 32; second.points = {{-1024, -1024}};
        }
        const std::array strokes{expand, second};
        auto replayed = Take(TerrainPathingSave::ReplayStrokes(*fixture.archive, MapDocument::FromMembers(fixture.members),
            fixture.Member("war3map.w3e"), fixture.Member("war3map.wpm"), strokes, fixture.gameRoot));
        Require(replayed.receipts.size() == 2 && replayed.session->Terrain()->header.width == 65,
            "resize combines with water, native cliff, palette, playable bounds and ordinary editing on current canvas");
        if (scenario != 4) Require(replayed.waterOwners.size() == 1 && replayed.sessionOwners.empty(),
            "post-resize resources finish physical/current verification and release borrowed bridge before public return");
        if (scenario == 2) {
            Apply(fixture);
            const auto changed = Take(TerrainPathingSave::EditTextureSlot(*fixture.archive, *fixture.edits,
                TerrainTextureTable::Ground, 0, "Tg02", fixture.gameRoot));
            Require(changed.headerChanged && replayed.session->MatchesTerrainJournalDraft(*fixture.edits),
                "live palette entry and replay share the same typed current bridge after resize");
        }
        fixture.edits = std::move(replayed.session);
        auto raw = RawDraft(fixture);
        const auto journal = Take(DecodeTerrainStrokeJournal(Take(EncodeTerrainStrokeJournal(
            Take(TerrainPathingSave::CaptureJournal(*fixture.archive, *fixture.edits))))));
        auto restored = Take(TerrainPathingSave::RestoreJournal(*fixture.archive, MapDocument::FromMembers(fixture.members),
            fixture.Member("war3map.w3e"), fixture.Member("war3map.wpm"), journal, *raw, fixture.gameRoot));
        Require(restored.receipts.size() == 2 && restored.session->MatchesTerrainJournalDraft(*raw),
            "cold mixed replay uses the same current shape and actual dependency identity as the live sequence");
        for (std::size_t i = restored.receipts.size(); i > 0; --i) Check(restored.session->Replay(restored.receipts[i - 1], false));
        Require(!restored.session->IsDirty() && Take(SerializeMapTerrain(*restored.session->Terrain())) == TerrainLiteral(),
            "mixed typed Undo reaches original canvas with no lost edit before the resize");
    }
}

} // namespace War3Map::TerrainResizeReplayTests
