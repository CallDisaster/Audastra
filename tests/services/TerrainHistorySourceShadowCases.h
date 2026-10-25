#pragma once

#include "../support/TerrainResizeFixture.h"
#include "War3MapCore/Services/TerrainHistorySave.h"
#include "War3MapCore/Services/TerrainPathingSave.h"
#include "War3MapCore/Services/TerrainResizeEditService.h"

#include <limits>

namespace AudastraTest::TerrainHistorySourceShadowCases {
using namespace AudastraTest::TerrainResize;
using namespace War3Map::Services;

inline void Raise(Fixture& fixture) {
    TerrainBrush height; height.kind = TerrainBrushKind::Raise; height.radius = 0; height.strength = 16;
    const TerrainStrokePoint center{0, 0};
    Take(fixture.edits->EditTerrain(height, std::span(&center, 1), false));
}
inline Bytes CurrentPathing(const Fixture& fixture) {
    for (const auto& member : fixture.edits->Replacements())
        if (member.name == "war3map.wpm") return member.data;
    return Take(fixture.archive->ReadMember("war3map.wpm"));
}
inline std::filesystem::path Directory(Fixture& fixture) {
    const auto path = fixture.directory / "source-directory";
    Require(std::filesystem::create_directory(path), "create original source-shadow directory fixture");
    for (const auto& member : fixture.members) {
        const auto target = path / member.name; std::filesystem::create_directories(target.parent_path());
        Check(IO::WriteFileBytes(target, member.data));
    }
    return path;
}
inline void Save(Fixture& fixture, const TerrainHistorySave::Lease& lease, std::string_view name) {
    auto output = fixture.edits->Replacements(); const auto removals = fixture.edits->Removals();
    const auto plan = Take(TerrainPathingSave::PrepareForSave(*fixture.archive, *fixture.edits, fixture.gameRoot, {}, output));
    Check(plan.SaveGate()); Check(plan.InstallPathingOutput(*fixture.edits, output));
    Check(plan.VerifyForSave(*fixture.edits, fixture.archive->SourceSha256(), output, removals));
    Check(plan.VerifySourceInputs(*fixture.archive));
    auto saved = Take(MapArchiveSession::Open(Take(fixture.archive->SaveCandidate(fixture.directory / std::string(name), output, removals)).path));
    Check(plan.VerifyReadback(saved)); Check(plan.CommitSaved(*fixture.edits, MapDocument::FromArchive(saved)));
    Check(fixture.edits->CommitTerrainHistorySave(lease));
    fixture.archive = std::make_unique<MapArchiveSession>(std::move(saved));
    Require(!fixture.edits->IsDirty(), "actual ordinary save installs the physical clean station");
}

inline void ActualArchiveAndDirectoryBindingAreImmutableAndBudgeted() {
    for (const bool present : {false, true}) for (const bool directory : {false, true}) {
        Fixture fixture(false, present); Raise(fixture);
        const auto terrain = fixture.edits->Terrain(); const auto revision = fixture.edits->Revision();
        const auto lease = Take(fixture.edits->PrepareTerrainHistoryForSave());
        Require(TerrainHistorySave::NeedsSourceShadowBinding(lease), "first ordinary save needs an old physical SHD binding");
        const auto bind = [&](std::size_t maximum = TerrainJournalMaximumBytes) {
            if (!directory) return TerrainHistorySave::BindSourceShadow(*fixture.archive, lease, maximum);
            const auto path = fixture.directory / "source-directory";
            const auto source = Take(DirectoryWorkspace::Open(path));
            return TerrainHistorySave::BindSourceShadow(source.Snapshot(), lease, maximum);
        };
        if (directory) Directory(fixture);
        const auto bound = Take(bind());
        Require(!TerrainHistorySave::NeedsSourceShadowBinding(bound)
            && TerrainHistorySave::NeedsSourceShadowBinding(lease)
            && bound.RetainedBytes() > lease.RetainedBytes()
            && bound.PreparationBytes() > lease.PreparationBytes(),
            "new immutable lease charges the raw SHD or explicit absence without modifying the old lease");
        Require(!bind(1).Succeeded() && !bind(std::numeric_limits<std::size_t>::max()).Succeeded()
            && !bind(lease.PreparationBytes()).Succeeded(), "tightened, overflow and no-remaining budgets reject before capture");
        Require(!TerrainHistorySave::BindSourceShadow(*fixture.archive, {}, TerrainJournalMaximumBytes).Succeeded(),
            "an identity-free default lease is never treated as already bound");
        Require(fixture.edits->Terrain() == terrain && fixture.edits->Revision() == revision,
            "success and refusal leave the complete active terrain and revision unchanged");
        Save(fixture, bound, directory ? "directory-bound-station.w3x" : "archive-bound-station.w3x");
        Require(fixture.archive->HasMember("war3map.shd") == present
            && (!present || Take(fixture.archive->ReadMember("war3map.shd")) == ShadowLiteral()),
            "ordinary save preserves actual raw SHD bytes or actual absence");
    }
}

inline void SourceDriftCorruptionAndIoNeverBecomeMissing() {
    Fixture fixture; Raise(fixture);
    const auto lease = Take(fixture.edits->PrepareTerrainHistoryForSave());
    const auto source = fixture.archive->SourcePath();
    const auto held = fixture.directory / "temporarily-moved-source.w3x";
    std::filesystem::rename(source, held);
    const auto unreadable = TerrainHistorySave::BindSourceShadow(*fixture.archive, lease);
    std::filesystem::rename(held, source);
    Require(!unreadable.Succeeded() && !unreadable.value, "actual source I/O failure is not shadow absence");

    Fixture other; other.Member("war3map.w3e").back() ^= std::byte{1}; other.Seal();
    const auto wrongTerrain = TerrainHistorySave::BindSourceShadow(*other.archive, lease);
    Require(!wrongTerrain.Succeeded() && HasCode({wrongTerrain.diagnostics}, "TERRAIN_HISTORY_SOURCE_MISMATCH"),
        "same-shape different W3E bytes cannot lend SHD to the old epoch");
    other.PutMember("war3map.w3e", TerrainLiteral()); other.PutMember("war3map.shd", {std::byte{1}}); other.Seal();
    Require(!TerrainHistorySave::BindSourceShadow(*other.archive, lease).Succeeded(), "malformed present SHD length remains an error");
    Fixture missing(false, false); Raise(missing);
    const auto missingLease = Take(missing.edits->PrepareTerrainHistoryForSave());
    const auto directory = Directory(missing); const auto snapshot = Take(DirectoryWorkspace::Open(directory));
    Check(IO::WriteFileBytes(directory / "war3map.shd", ShadowLiteral()));
    Require(!TerrainHistorySave::BindSourceShadow(snapshot.Snapshot(), missingLease).Succeeded(),
        "directory absence changing to presence invalidates the frozen actual-source snapshot");
    Require(TerrainHistorySave::NeedsSourceShadowBinding(lease) && TerrainHistorySave::NeedsSourceShadowBinding(missingLease),
        "failed captures do not publish any partial source-shadow binding");
}

inline void RepeatedCleanSaveInheritsTheOldBindingWithoutReadingNewSource() {
    for (const bool bindFirst : {false, true}) {
        Fixture fixture; Raise(fixture);
        auto first = Take(fixture.edits->PrepareTerrainHistoryForSave());
        if (bindFirst) first = Take(TerrainHistorySave::BindSourceShadow(*fixture.archive, first));
        Save(fixture, first, "first-station.w3x");
        const auto next = Take(fixture.edits->PrepareTerrainHistoryForSave());
        const auto path = fixture.archive->SourcePath(), held = fixture.directory / "latest-physical-unavailable.w3x";
        std::filesystem::rename(path, held);
        auto inherited = TerrainHistorySave::BindSourceShadow(*fixture.archive, next);
        std::filesystem::rename(held, path);
        if (bindFirst) {
            Require(inherited.Succeeded() && inherited.value && inherited.value->RetainedBytes() == next.RetainedBytes()
                && inherited.value->PreparationBytes() == next.PreparationBytes(),
                "already bound oldest epoch inherits even when newer source cannot be read, with no duplicate charge");
            Save(fixture, *inherited.value, "repeated-clean-station.w3x");
        } else {
            Require(!inherited.Succeeded() && !inherited.value
                && HasCode({inherited.diagnostics}, "TERRAIN_HISTORY_SOURCE_SHADOW_UNAVAILABLE"),
                "legacy oldest epoch without SHD refuses later binding instead of guessing from latest physical source");
        }
    }
}

inline void OrdinarySaveThenFirstResizeRestoresAndRecapturesColdHistory() {
    for (const bool present : {false, true}) {
        Fixture fixture(false, present); const auto originalPath = fixture.archive->SourcePath();
        const auto originalSha = fixture.archive->SourceSha256(); Raise(fixture);
        const auto lease = Take(TerrainHistorySave::BindSourceShadow(*fixture.archive,
            Take(fixture.edits->PrepareTerrainHistoryForSave())));
        Save(fixture, lease, "ordinary-height-station.w3x");
        const auto physical = MapDocument::FromArchive(*fixture.archive);
        const auto physicalTerrain = Take(fixture.archive->ReadMember("war3map.w3e"));
        const auto physicalPathing = Take(fixture.archive->ReadMember("war3map.wpm"));
        const auto resources = Take(TerrainResizeEditService::Capture(*fixture.archive, *fixture.edits, fixture.gameRoot));
        Check(fixture.edits->ApplyPreparedTerrainResizeEdit(Take(fixture.edits->PrepareTerrainResizeEdit(
            resources, TerrainResizeIntent{1, {-32, 0, 32, 32}}))));
        const auto wire = Take(EncodeTerrainStrokeJournal(Take(TerrainPathingSave::CaptureJournal(*fixture.archive, *fixture.edits))));
        const auto journal = Take(DecodeTerrainStrokeJournal(wire));
        Require(wire[3] == std::byte{'H'} && journal.history && journal.history->baselineShadow
            && journal.history->baselineShadow->present == present
            && journal.history->baselineShadow->bytes == (present ? ShadowLiteral() : Bytes{})
            && journal.history->savedIndex == 1 && journal.history->currentIndex == 2,
            "ordinary first save then first resize exports the exact oldest raw shadow or explicit absence in FTJH");
        MapDataEditSession raw(physical); Check(raw.AttachTerrain(physicalTerrain)); Check(raw.AttachPathing(physicalPathing));
        Check(raw.SetTerrainResizeDraft(Take(SerializeMapTerrain(*fixture.edits->Terrain())), CurrentPathing(fixture),
            Take(fixture.edits->ExportTerrainResizeDraft())));
        Check(raw.SetSerialized(MapDataKind::Settings, fixture.edits->Snapshot().Member(MapDataKind::Settings).sources.front().data));
        Require(raw.MatchesTerrainJournalDraft(*fixture.edits), "cold raw endpoint is complete but has no typed authority");
        auto restored = Take(TerrainPathingSave::RestoreJournal(*fixture.archive, physical, physicalTerrain,
            physicalPathing, journal, raw, fixture.gameRoot));
        const auto recaptured = Take(EncodeTerrainStrokeJournal(Take(TerrainPathingSave::CaptureJournal(*fixture.archive, *restored.session))));
        Require(recaptured == wire, "cold restored ordinary save station retains old SHD when exported again");
        Check(restored.session->Replay(restored.receipts[1], false));
        const auto stationShadow = Take(restored.session->TerrainResizeShadowDraft());
        Require(!restored.session->IsDirty() && Take(SerializeMapTerrain(*restored.session->Terrain())) == physicalTerrain
            && Take(restored.session->SerializePathingRestrictionDraft()).empty()
            && stationShadow.present == present && stationShadow.bytes == (present ? ShadowLiteral() : Bytes{}),
            "cold Undo of first resize returns to real clean station bytes, P/S and original raw SHD");
        Check(restored.session->Replay(restored.receipts[0], false));
        Require(Take(SerializeMapTerrain(*restored.session->Terrain())) == TerrainLiteral(),
            "cold Undo crosses the ordinary save station back to independent original height bytes");
        for (const auto& receipt : restored.receipts) Check(restored.session->Replay(receipt, true));
        Require(restored.session->MatchesTerrainJournalDraft(raw) && Take(Sha256File(originalPath)) == originalSha,
            "cold full Redo reproduces the complete resize draft and leaves original physical source read-only");
    }
}

inline void RunTerrainHistorySourceShadowCases() {
    ActualArchiveAndDirectoryBindingAreImmutableAndBudgeted();
    SourceDriftCorruptionAndIoNeverBecomeMissing();
    RepeatedCleanSaveInheritsTheOldBindingWithoutReadingNewSource();
    OrdinarySaveThenFirstResizeRestoresAndRecapturesColdHistory();
}
} // namespace AudastraTest::TerrainHistorySourceShadowCases
