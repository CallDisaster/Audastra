#include "War3MapCore/MapData/TerrainEditInspection.h"

#include "War3MapCore/Hash.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/Mpq/MpqArchive.h"
#include "MapDataFixtures.h"
#include "../src/Core/Mpq/MpqCrypt.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {

using namespace War3Map;
using namespace War3Map::MapData;
using Bytes = std::vector<std::byte>;

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

template <typename T>
T Take(Result<T> result) {
    if (!result.Succeeded()) {
        std::string error = "fixture operation failed";
        for (const auto& diagnostic : result.diagnostics) error += " | " + diagnostic.code + ": " + diagnostic.message;
        throw std::runtime_error(error);
    }
    return std::move(*result.value);
}

template <typename T>
void Rejected(const Result<T>& result, std::string_view code = {}) {
    Require(!result.Succeeded() && !result.value && !result.diagnostics.empty(), "failure must have no partial snapshot");
    if (!code.empty()) Require(result.diagnostics.front().code == code,
        "wrong diagnostic: " + result.diagnostics.front().code + ", expected " + std::string(code));
}

class TempDirectory {
public:
    TempDirectory() {
        static std::atomic<unsigned> sequence{0};
        path_ = std::filesystem::temp_directory_path() / ("Forge-terrain-edit-inspection-"
            + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64())
            + "-" + std::to_string(sequence.fetch_add(1)));
        Require(std::filesystem::create_directory(path_), "create private fixture directory");
    }
    ~TempDirectory() { std::error_code ignored; std::filesystem::remove_all(path_, ignored); }
    const std::filesystem::path& Path() const { return path_; }
private:
    std::filesystem::path path_;
};

Bytes TerrainBytes(std::uint32_t version = 11, std::uint32_t width = 5, std::uint32_t height = 5) {
    MapDataFixtures::Fixture fixture;
    fixture.Raw("W3E!"); fixture.UInt(version); fixture.Byte('L'); fixture.UInt(0);
    fixture.UInt(2); fixture.Raw("Tg01"); fixture.Raw("tg02");
    fixture.UInt(1); fixture.Raw("Cl01"); fixture.UInt(width); fixture.UInt(height);
    fixture.Float(0.0f); fixture.Float(0.0f);
    for (std::uint32_t index = 0; index < width * height; ++index) {
        fixture.Byte(0); fixture.Byte(32); // ground raw 8192
        fixture.Byte(0); fixture.Byte(32); // water raw 8192; no water flag
        fixture.Byte(0);
        if (version == 12) fixture.Byte(0x80); // 原样保留只读扩展位。
        fixture.Byte(0); fixture.Byte(2);
    }
    return std::move(fixture.bytes);
}

Bytes PathingBytes() {
    MapDataFixtures::Fixture fixture;
    fixture.Raw("MP3W"); fixture.UInt(0); fixture.UInt(16); fixture.UInt(16);
    for (std::uint32_t index = 0; index < 256; ++index)
        fixture.Byte(static_cast<std::uint8_t>((index * 13) & 0xdfu));
    return std::move(fixture.bytes);
}

std::vector<MapArchiveMember> Members() {
    return {{"war3map.w3e", TerrainBytes()}, {"war3map.wpm", PathingBytes()},
        {"war3map.doo", MapDataFixtures::EmptyDoodadsV8Sub11()},
        {"war3mapUnits.doo", MapDataFixtures::EmptyUnitsV8Sub11()},
        {"war3map.w3i", MapDataFixtures::FullMapSettingsV25().bytes}};
}

MapArchiveSession WriteSource(const std::filesystem::path& path, const std::vector<MapArchiveMember>& members) {
    (void)Take(WriteVerifiedArchive(path, members));
    return Take(MapArchiveSession::Open(path));
}

void Attach(MapDataEditSession& edits, const std::vector<MapArchiveMember>& members) {
    Require(edits.AttachTerrain(members[0].data).Succeeded(), "attach original terrain");
    Require(edits.AttachPathing(members[1].data).Succeeded(), "attach original pathing");
}

void Stroke(MapDataEditSession& edits, TerrainBrushKind kind) {
    TerrainBrush brush;
    brush.kind = kind; brush.radius = 0; brush.strength = 4; brush.groundTexture = "tg02";
    const TerrainStrokePoint center{256, 256};
    (void)Take(edits.EditTerrain(brush, std::span(&center, 1), false));
}

void SameStatus(const Status& actual, const Status& expected) {
    Require(actual.Succeeded() == expected.Succeeded() && actual.diagnostics.size() == expected.diagnostics.size(),
        "gate result must match existing authority");
    for (std::size_t index = 0; index < actual.diagnostics.size(); ++index) {
        const auto& a = actual.diagnostics[index];
        const auto& b = expected.diagnostics[index];
        Require(a.severity == b.severity && a.subsystem == b.subsystem && a.code == b.code && a.message == b.message
            && a.origin.has_value() == b.origin.has_value(), "preserve complete gate diagnostic");
        if (a.origin) Require(a.origin->source == b.origin->source && a.origin->offset == b.origin->offset
            && a.origin->length == b.origin->length && a.origin->fingerprint == b.origin->fingerprint,
            "preserve gate diagnostic origin");
    }
}

void BaselineAndUndoCaptureAreReadOnly() {
    TempDirectory directory;
    const auto members = Members();
    auto source = WriteSource(directory.Path() / "baseline.w3x", members);
    MapDataEditSession edits(MapDocument::FromArchive(source));
    Attach(edits, members);
    const auto originalSha = Take(Sha256File(source.SourcePath()));
    const auto first = Take(CaptureTerrainEditInspection(source, edits));
    Require(first.SourceSha256() == originalSha && first.SessionIdentity() == edits.SessionIdentity()
        && first.Revision() == 0 && first.TerrainSourceRevision() == edits.TerrainSourceRevision()
        && !first.TerrainChanged() && first.TerrainPathingGate().Succeeded(),
        "baseline binds archive, session and exact revision");
    const auto preserved = Take(first.PathingPreservation());
    Require(preserved.Kind() == PathingPreservationKind::PreserveExact && preserved.ChangedCells() == 0,
        "the actual archive baseline supplies an independent exact-preservation proof");
    std::size_t byteCount = 0;
    for (std::size_t index = 0; index < members.size(); ++index) {
        const auto& captured = first.Members()[index];
        byteCount += members[index].data.size();
        Require(captured.name == members[index].name && captured.original && captured.current == captured.original
            && *captured.original == members[index].data && captured.originalSha256 == captured.currentSha256
            && captured.originalSha256 == std::optional(Take(Sha256(members[index].data))),
            "unchanged members must own exact bytes once and share immutable storage");
    }
    Require(first.OwnedBytes() == byteCount, "aggregate counts every unchanged member once");
    Require(Take(CaptureTerrainEditInspection(source, edits)).IdentitySha256() == first.IdentitySha256(),
        "same frozen state gives deterministic identity");
    MapDataEditSession other(MapDocument::FromArchive(source));
    Attach(other, members);
    Require(Take(CaptureTerrainEditInspection(source, other)).IdentitySha256() != first.IdentitySha256(),
        "another editor session cannot reuse an inspection identity");

    Stroke(edits, TerrainBrushKind::Raise);
    Require(edits.Undo(), "undo height stroke");
    const auto revision = edits.Revision();
    const auto receipt = edits.LastAppliedEdit();
    const auto undone = Take(CaptureTerrainEditInspection(source, edits));
    Require(!undone.TerrainChanged() && undone.Members()[0].original == undone.Members()[0].current
        && undone.IdentitySha256() != first.IdentitySha256(), "net original bytes still carry the newer revision");
    Require(edits.Revision() == revision && !edits.CanUndo() && edits.CanRedo() && !edits.IsDirty()
        && edits.LastAppliedEdit().CommandIdentity() == receipt.CommandIdentity(),
        "inspection must preserve revision, undo, redo and transaction receipt");
    Require(Take(Sha256File(source.SourcePath())) == originalSha, "inspection never writes the original archive");
}

void AcceptedAndRejectedTerrainRemainInspectable() {
    TempDirectory directory;
    const auto members = Members();
    auto source = WriteSource(directory.Path() / "terrain.w3x", members);
    for (const auto kind : {TerrainBrushKind::Blight, TerrainBrushKind::Raise, TerrainBrushKind::Texture}) {
        MapDataEditSession edits(MapDocument::FromArchive(source));
        Attach(edits, members);
        Stroke(edits, kind);
        const auto revision = edits.Revision();
        const auto gate = edits.VerifyTerrainPathingForSave();
        const auto snapshot = Take(CaptureTerrainEditInspection(source, edits));
        Require(snapshot.TerrainChanged() && snapshot.Members()[0].current != snapshot.Members()[0].original,
            "edited W3E must be explicitly serialized into owned final bytes");
        const auto finalTerrain = Take(ParseMapTerrain(*snapshot.Members()[0].current));
        const auto& center = finalTerrain.Snapshot()->vertices[12];
        if (kind == TerrainBrushKind::Blight) {
            Require(gate.Succeeded() && center.IsBlight(), "existing bounded blight remains allowed");
            Require(snapshot.Members()[1].current != snapshot.Members()[1].original, "blight captures changed WPM too");
            std::size_t changes = 0;
            for (std::size_t offset = 0; offset < members[1].data.size(); ++offset) {
                const auto before = std::to_integer<unsigned>(members[1].data[offset]);
                const auto after = std::to_integer<unsigned>((*snapshot.Members()[1].current)[offset]);
                if (before != after) { ++changes; Require((before ^ after) == 0x20, "blight retains all other original bits"); }
            }
            Require(changes == 16, "capture records the existing single-vertex blight result, without generating a new rule");
        } else if (kind == TerrainBrushKind::Raise) {
            Require(gate.Succeeded() && center.groundHeightRaw == 8208, "existing dry height remains allowed");
            Require(snapshot.Members()[1].current == snapshot.Members()[1].original, "height keeps source WPM exact");
        } else {
            Require(!gate.Succeeded() && center.GroundTextureIndex() == 1, "texture draft stays blocked by the existing gate");
            Require(snapshot.TerrainPathingGate().diagnostics.front().code == "MAP_TERRAIN_PATHING_UNVERIFIED",
                "rejected gate stays inside a successful read-only snapshot");
        }
        SameStatus(snapshot.TerrainPathingGate(), gate);
        Rejected(snapshot.PathingPreservation(), "PATHING_POLICY_TERRAIN_CHANGED");
        Require(edits.Revision() == revision && edits.CanUndo() && !edits.CanRedo(), "capture cannot add a command");
        Require(source.VerifySourceUnchanged().Succeeded(), "source unchanged for every terrain outcome");
    }
}

void RestrictionInspectionSharesNormalSavePolicyWithoutNativeAcceptance() {
    TempDirectory directory;
    const auto members = Members();
    auto source = WriteSource(directory.Path() / "restriction-inspection.w3x", members);
    const auto originalSha = Take(Sha256File(source.SourcePath()));
    MapDataEditSession edits(MapDocument::FromArchive(source));
    Attach(edits, members);
    TerrainBrush brush; brush.kind = TerrainBrushKind::PathingRestriction;
    brush.size = 1; brush.pathingRestrictions = 4;
    const TerrainStrokePoint point{16, 16}; // 原创WPM第0格原值为0，新增不可飞。
    (void)Take(edits.EditTerrain(brush, std::span(&point, 1), false));
    const auto revision = edits.Revision();
    const auto sourceRevision = edits.TerrainSourceRevision();
    const auto receipt = edits.LastAppliedEdit();
    const auto replacements = edits.Replacements();
    const auto snapshot = Take(CaptureTerrainEditInspection(source, edits));
    const auto proof = Take(snapshot.PathingPreservation());
    Require(proof.Kind() == PathingPreservationKind::ExplicitRestrictionCandidate && proof.ChangedCells() == 1
        && proof.RequiresNativeVerification(), "inspection proves the one declared cell without native acceptance");
    Require(snapshot.TerrainPathingGate().Succeeded(), "inspection reports the shared declared-restriction save policy");
    SameStatus(snapshot.TerrainPathingGate(), edits.VerifyTerrainPathingForSave());
    Require(snapshot.Members()[0].original == snapshot.Members()[0].current
        && snapshot.Members()[1].original != snapshot.Members()[1].current,
        "restriction-only inspection freezes exact W3E and the actual changed WPM");
    const auto bindingOf = [](const TerrainEditInspection& capture) {
        return PathingPreservationBinding{capture.SourceSha256(), *capture.Members()[0].originalSha256,
            *capture.Members()[1].originalSha256, capture.SessionIdentity(), capture.Revision(), capture.TerrainSourceRevision()};
    };
    const auto draft = Take(edits.SerializePathingRestrictionDraft());
    Require(proof.VerifyCurrent(bindingOf(snapshot), *snapshot.Members()[0].current, *snapshot.Members()[1].current, draft).Succeeded(),
        "proof uses the real archive SHA and same frozen session declaration");
    const auto after = edits.Replacements();
    Require(edits.Revision() == revision && edits.TerrainSourceRevision() == sourceRevision
        && edits.CanUndo() && !edits.CanRedo() && edits.LastAppliedEdit().CommandIdentity() == receipt.CommandIdentity()
        && std::equal(replacements.begin(), replacements.end(), after.begin(), after.end(), [](const auto& first, const auto& second) {
            return first.name == second.name && first.data == second.data;
        }), "inspection cannot change commands, generations, replacements or redo");
    Require(Take(CaptureTerrainEditInspection(source, edits)).IdentitySha256() == snapshot.IdentitySha256(),
        "same frozen restriction state has stable inspection identity");

    Require(edits.Undo(), "undo restriction before read-only capture");
    const auto undone = Take(CaptureTerrainEditInspection(source, edits));
    Require(Take(undone.PathingPreservation()).Kind() == PathingPreservationKind::PreserveExact
        && edits.CanRedo() && !edits.CanUndo(), "inspection preserves redo of a fully withdrawn declaration");
    Require(edits.Redo(), "redo after inspection still applies the same restriction");
    const auto redone = Take(CaptureTerrainEditInspection(source, edits));
    const auto stale = proof.VerifyCurrent(bindingOf(redone), *redone.Members()[0].current, *redone.Members()[1].current, draft);
    Require(!stale.Succeeded() && !stale.diagnostics.empty() && stale.diagnostics.front().code == "PATHING_POLICY_STALE_BINDING"
        && redone.IdentitySha256() != snapshot.IdentitySha256(), "matching bytes cannot revive a pre-undo proof");
    MapDataEditSession other(MapDocument::FromArchive(source));
    Attach(other, members);
    (void)Take(other.EditTerrain(brush, std::span(&point, 1), false));
    const auto another = Take(CaptureTerrainEditInspection(source, other));
    const auto otherStale = proof.VerifyCurrent(bindingOf(another), *another.Members()[0].current, *another.Members()[1].current, draft);
    Require(!otherStale.Succeeded() && !otherStale.diagnostics.empty()
        && otherStale.diagnostics.front().code == "PATHING_POLICY_STALE_BINDING", "another session cannot borrow a byte-identical proof");

    Require(edits.Undo(), "restore exact source before synthetic saved-generation rebase");
    const auto beforeRebase = Take(CaptureTerrainEditInspection(source, edits));
    const auto exact = Take(beforeRebase.PathingPreservation());
    edits.CommitSaved(MapDocument::FromArchive(source));
    const auto rebased = Take(CaptureTerrainEditInspection(source, edits));
    const auto rebaseStale = exact.VerifyCurrent(bindingOf(rebased), *rebased.Members()[0].current, *rebased.Members()[1].current, {});
    Require(rebased.Revision() == beforeRebase.Revision() && rebased.TerrainSourceRevision() != beforeRebase.TerrainSourceRevision()
        && rebased.IdentitySha256() != beforeRebase.IdentitySha256() && !rebaseStale.Succeeded()
        && !rebaseStale.diagnostics.empty() && rebaseStale.diagnostics.front().code == "PATHING_POLICY_STALE_BINDING",
        "source generation alone changes inspection identity and invalidates the old exact proof");
    Require(Take(Sha256File(source.SourcePath())) == originalSha && source.VerifySourceUnchanged().Succeeded(),
        "all read-only inspection and synthetic session rebases leave the source archive untouched");
}

void OverlappingIntentSnapshotsShareOwnershipAndBudget() {
    TempDirectory directory;
    const auto members = Members();
    auto source = WriteSource(directory.Path() / "intent-inspection.w3x", members);
    std::optional<TerrainEditInspection> retained;
    {
        MapDataEditSession edits(MapDocument::FromArchive(source));
        Attach(edits, members);
        const auto ordinary = Take(CaptureTerrainEditInspection(source, edits));
        Require(!ordinary.PathingRestrictionIntent(), "zero A needs no retained intent owner");
        const auto terrain = edits.Terrain();
        TerrainBrush brush; brush.kind = TerrainBrushKind::PathingRestriction; brush.size = 1; brush.pathingRestrictions = 2;
        const TerrainStrokePoint point{80, 16}; // cell2源0x1a已经包含不可走。
        (void)Take(edits.EditTerrain(brush, std::span(&point, 1), false));
        const auto first = Take(CaptureTerrainEditInspection(source, edits));
        const auto repeated = Take(CaptureTerrainEditInspection(source, edits));
        Require(first.PathingRestrictionIntent() && first.PathingRestrictionIntent() == repeated.PathingRestrictionIntent(),
            "unchanged captures share the same immutable A owner");
        Require(first.Members()[0].current == first.Members()[0].original
            && first.Members()[1].current == first.Members()[1].original && edits.Terrain() == terrain
            && edits.Replacements().empty(), "A-only does not duplicate terrain or native WPM output");
        const auto exact = ordinary.OwnedBytes() + members[1].data.size() - 16;
        Require(first.OwnedBytes() == exact && Take(CaptureTerrainEditInspection(source, edits, {exact})).OwnedBytes() == exact,
            "existing inspection budget charges the retained shared A once");
        Rejected(CaptureTerrainEditInspection(source, edits, {exact - 1}), "TERRAIN_EDIT_INSPECTION_BUDGET");
        Require(first.TerrainPathingGate().Succeeded() && Take(first.PathingPreservation()).Kind() == PathingPreservationKind::PreserveExact
            && Take(first.PathingPreservation()).ChangedCells() == 0, "intent overlap grants no new native permission");
        const TerrainStrokePoint secondPoint{112, 16};
        (void)Take(edits.EditTerrain(brush, std::span(&secondPoint, 1), false));
        const auto second = Take(CaptureTerrainEditInspection(source, edits));
        Require(second.PathingRestrictionIntent() != first.PathingRestrictionIntent()
            && (*first.PathingRestrictionIntent())[2] == std::byte{2} && (*first.PathingRestrictionIntent())[3] == std::byte{0}
            && (*second.PathingRestrictionIntent())[3] == std::byte{2}, "later edit cannot mutate an older shared snapshot");
        Require(edits.Undo() && Take(CaptureTerrainEditInspection(source, edits)).PathingRestrictionIntent() == first.PathingRestrictionIntent(),
            "undo restores the identical first A owner");
        Require(edits.Redo() && Take(CaptureTerrainEditInspection(source, edits)).PathingRestrictionIntent() == second.PathingRestrictionIntent(),
            "redo restores the identical second A owner");
        retained.emplace(first);
    }
    Require(retained->PathingRestrictionIntent() && (*retained->PathingRestrictionIntent())[2] == std::byte{2},
        "inspection retains immutable A after the editing session is destroyed");
}

void FullRawTerrainAndPathingBaselinesMustMatch() {
    TempDirectory directory;
    const auto members = Members();
    auto source = WriteSource(directory.Path() / "source.w3x", members);
    const auto parsed = Take(ParseMapTerrain(members[0].data));
    for (unsigned variation = 0; variation < 12; ++variation) {
        auto grid = *parsed.Snapshot();
        switch (variation) {
        case 0: grid.header.mainTileset = 'A'; break;
        case 1: grid.header.usesCustomTileset = true; break;
        case 2: grid.header.groundTilesets[0] = "Tg09"; break;
        case 3: grid.header.cliffTilesets[0] = "Cl09"; break;
        case 4: grid.header.offset[0] = std::bit_cast<float>(0x80000000u); break; // -0 与 +0 不能按float相等合并。
        case 5: grid.header.offset[1] = 32.0f; break;
        case 6: ++grid.vertices[12].groundHeightRaw; break;
        case 7: grid.vertices[12].waterHeightAndFlagsRaw ^= 0x8000; break;
        case 8: grid.vertices[12].textureAndFlagsRaw ^= 0x20; break;
        case 9: ++grid.vertices[12].variationRaw; break;
        case 10: grid.vertices[12].cliffAndLayerRaw = 3; break;
        case 11: grid = *Take(ParseMapTerrain(TerrainBytes(11, 6, 5))).Snapshot(); break;
        }
        MapDataEditSession edits(MapDocument::FromArchive(source));
        Require(edits.AttachTerrain(Take(SerializeMapTerrain(grid))).Succeeded(), "different but structurally valid terrain attaches");
        Rejected(CaptureTerrainEditInspection(source, edits), "TERRAIN_EDIT_INSPECTION_SOURCE_MISMATCH");
        Require(!edits.IsDirty() && edits.Revision() == 0, "mismatch must not mutate session");
    }
    MapDataEditSession wrongPathing(MapDocument::FromArchive(source));
    Require(wrongPathing.AttachTerrain(members[0].data).Succeeded(), "attach terrain for WPM mismatch");
    auto pathing = members[1].data; pathing.back() ^= std::byte{1};
    Require(wrongPathing.AttachPathing(pathing).Succeeded(), "valid different WPM attaches");
    Rejected(CaptureTerrainEditInspection(source, wrongPathing), "TERRAIN_EDIT_INSPECTION_SOURCE_MISMATCH");
    auto missingTerrainMembers = members; missingTerrainMembers.erase(missingTerrainMembers.begin());
    auto missingTerrain = WriteSource(directory.Path() / "missing-terrain.w3x", missingTerrainMembers);
    MapDataEditSession attachedToMissing(MapDocument::FromArchive(missingTerrain));
    Require(attachedToMissing.AttachTerrain(members[0].data).Succeeded(), "attach independent terrain to mismatched host");
    Rejected(CaptureTerrainEditInspection(missingTerrain, attachedToMissing), "TERRAIN_EDIT_INSPECTION_SOURCE_MISMATCH");
}

void NativeSourcesCannotBeInventedOrDiscarded() {
    TempDirectory directory;
    const auto members = Members();
    auto source = WriteSource(directory.Path() / "native.w3x", members);
    auto changed = members;
    auto settings = Take(MapSettings::Parse(changed[4].data));
    settings.mapName = "different original";
    changed[4].data = Take(settings.Serialize());
    MapDataEditSession wrong(MapDocument::FromMembers(changed));
    Rejected(CaptureTerrainEditInspection(source, wrong), "TERRAIN_EDIT_INSPECTION_SOURCE_MISMATCH");
    auto missing = members; missing.erase(missing.begin() + 2);
    MapDataEditSession missingDoo(MapDocument::FromMembers(missing));
    Rejected(CaptureTerrainEditInspection(source, missingDoo), "TERRAIN_EDIT_INSPECTION_SOURCE_MISMATCH");
    auto invalid = members; invalid[2].data.clear();
    MapDataEditSession invalidDoo(MapDocument::FromMembers(invalid));
    Rejected(CaptureTerrainEditInspection(source, invalidDoo), "TERRAIN_EDIT_INSPECTION_SOURCE_UNSAFE");
    auto duplicate = members; duplicate.push_back({"WAR3MAP.DOO", members[2].data});
    MapDataEditSession ambiguous(MapDocument::FromMembers(duplicate));
    Rejected(CaptureTerrainEditInspection(source, ambiguous), "TERRAIN_EDIT_INSPECTION_SOURCE_UNSAFE");

    MapDataEditSession edits(MapDocument::FromArchive(source));
    auto nextSettings = *edits.Snapshot().Settings();
    nextSettings.author = "inspection final value";
    Require(edits.EditSettings(nextSettings).Succeeded(), "typed native change");
    const auto snapshot = Take(CaptureTerrainEditInspection(source, edits));
    Require(!snapshot.TerrainChanged() && snapshot.Members()[4].original != snapshot.Members()[4].current
        && Take(MapSettings::Parse(*snapshot.Members()[4].current)).author == "inspection final value",
        "final native bytes must come from the current edit session");
    Require(*snapshot.Members()[4].original == members[4].data, "native original is still exact source");
}

void SettingsBaselineComparisonIsReadOnly() {
    const auto members = Members();
    MapDataEditSession edits(MapDocument::FromMembers(members));
    Require(ValidateTerrainSettingsSourceBaseline(edits, true, members[4].data).Succeeded(),
        "exact settings baseline compares without an archive capture");
    auto changed = members;
    auto settings = Take(MapSettings::Parse(changed[4].data));
    settings.mapName = "different settings baseline";
    changed[4].data = Take(settings.Serialize());
    Require(edits.SetSerialized(MapDataKind::Settings, changed[4].data).Succeeded(), "change current settings only");
    const auto revision = edits.Revision();
    Require(ValidateTerrainSettingsSourceBaseline(edits, true, members[4].data).Succeeded()
        && edits.Revision() == revision && edits.CanUndo() && !edits.CanRedo(),
        "baseline validation ignores current changes and preserves the undo stack");

    const auto rejected = [&](const Status& status, std::string_view code) {
        Require(!status.Succeeded() && !status.diagnostics.empty() && status.diagnostics.front().code == code,
            "settings baseline retains the shared inspection failure code");
    };
    rejected(ValidateTerrainSettingsSourceBaseline(edits, true, changed[4].data),
        "TERRAIN_EDIT_INSPECTION_SOURCE_MISMATCH");
    MapDataEditSession foreign(MapDocument::FromMembers(changed));
    Require(foreign.SetSerialized(MapDataKind::Settings, members[4].data).Succeeded(),
        "foreign session current now equals the supplied source A");
    const auto foreignCurrent = foreign.Snapshot(MapDataKind::Settings, members[4].data.size());
    Require(foreignCurrent.Succeeded() && foreignCurrent.value->Member(MapDataKind::Settings).sources.size() == 1
        && foreignCurrent.value->Member(MapDataKind::Settings).sources[0].data == members[4].data,
        "foreign regression really has source A / original B / current A");
    rejected(ValidateTerrainSettingsSourceBaseline(foreign, true, members[4].data),
        "TERRAIN_EDIT_INSPECTION_SOURCE_MISMATCH");
    Require(ValidateTerrainSettingsSourceBaseline(foreign, true, changed[4].data).Succeeded(),
        "only the actual foreign original matches, even after editing current");

    MapDataEditSession missing(MapDocument::FromMembers({}));
    Require(ValidateTerrainSettingsSourceBaseline(missing, false, {}).Succeeded(), "proven missing baseline matches missing source");
    rejected(ValidateTerrainSettingsSourceBaseline(missing, true, {}), "TERRAIN_EDIT_INSPECTION_SOURCE_MISMATCH");
    rejected(ValidateTerrainSettingsSourceBaseline(missing, false, members[4].data), "TERRAIN_EDIT_INSPECTION_SOURCE_UNSAFE");
    auto empty = members; empty[4].data.clear();
    MapDataEditSession invalid(MapDocument::FromMembers(empty));
    rejected(ValidateTerrainSettingsSourceBaseline(invalid, true, {}), "TERRAIN_EDIT_INSPECTION_SOURCE_UNSAFE");
    auto duplicate = members; duplicate.push_back({"WAR3MAP.W3I", members[4].data});
    MapDataEditSession ambiguous(MapDocument::FromMembers(duplicate));
    rejected(ValidateTerrainSettingsSourceBaseline(ambiguous, true, members[4].data), "TERRAIN_EDIT_INSPECTION_SOURCE_UNSAFE");
}

void MissingEmptyReadonlyAndDeletionStayDistinct() {
    TempDirectory directory;
    const std::vector<MapArchiveMember> emptyMembers{{"war3map.w3e", {}}, {"war3map.wpm", {}}};
    auto source = WriteSource(directory.Path() / "empty.w3x", emptyMembers);
    MapDataEditSession edits(MapDocument::FromArchive(source));
    const auto empty = Take(CaptureTerrainEditInspection(source, edits, {0}));
    Rejected(empty.PathingPreservation(), "PATHING_POLICY_CONTEXT_MISSING");
    Require(empty.OwnedBytes() == 0 && !empty.TerrainChanged(), "true empty unbound members fit a zero-byte budget");
    const auto emptyHash = Take(Sha256(std::span<const std::byte>{}));
    for (std::size_t index = 0; index < 2; ++index)
        Require(empty.Members()[index].original && empty.Members()[index].original->empty()
            && empty.Members()[index].original == empty.Members()[index].current
            && empty.Members()[index].originalSha256 == std::optional(emptyHash), "present empty is not missing");
    for (std::size_t index = 2; index < 5; ++index)
        Require(!empty.Members()[index].original && !empty.Members()[index].current
            && !empty.Members()[index].originalSha256 && !empty.Members()[index].currentSha256,
            "proven missing has no fabricated empty-content SHA");

    auto newMembers = Members(); newMembers[0].data = TerrainBytes(12);
    auto newSource = WriteSource(directory.Path() / "readonly-v12.w3x", newMembers);
    MapDataEditSession readonly(MapDocument::FromArchive(newSource));
    Require(!readonly.AttachTerrain(newMembers[0].data).Succeeded(), "v12 remains unavailable to classic editing");
    const auto v12 = Take(CaptureTerrainEditInspection(newSource, readonly));
    Rejected(v12.PathingPreservation(), "PATHING_POLICY_CONTEXT_MISSING");
    Require(v12.Members()[0].original == v12.Members()[0].current && *v12.Members()[0].current == newMembers[0].data
        && !v12.TerrainChanged(), "unattached readonly W3E must retain all original bytes");

    auto beforeMembers = Members(); beforeMembers.erase(beforeMembers.begin() + 2);
    auto beforeSource = WriteSource(directory.Path() / "before-add.w3x", beforeMembers);
    MapDataEditSession changing(MapDocument::FromArchive(beforeSource));
    auto emptyDoo = Take(ParseDoodadPlacements(MapDataFixtures::EmptyDoodadsV8Sub11()));
    Require(changing.EditDoodads(emptyDoo).Succeeded(), "add a formerly absent native member");
    const auto added = Take(CaptureTerrainEditInspection(beforeSource, changing));
    Require(!added.Members()[2].original && added.Members()[2].current, "capture newly created member");
    auto savedSource = WriteSource(directory.Path() / "saved-add.w3x", Members());
    changing.CommitSaved(MapDocument::FromArchive(savedSource));
    Require(changing.Undo(), "undo after saved addition creates a real member deletion");
    const auto deleted = Take(CaptureTerrainEditInspection(savedSource, changing));
    Require(deleted.Members()[2].original && !deleted.Members()[2].current
        && deleted.Members()[2].originalSha256 && !deleted.Members()[2].currentSha256,
        "a validated current deletion must differ from an empty output file");
    Require(changing.CanRedo() && changing.Redo(), "inspection preserves redo of deleted member");
    const auto restored = Take(CaptureTerrainEditInspection(savedSource, changing));
    Require(restored.Members()[2].current == restored.Members()[2].original, "redo restores shared exact baseline bytes");
}

void AggregateBudgetsAndIdentityUseFinalOwnedBytes() {
    TempDirectory directory;
    const auto members = Members();
    auto source = WriteSource(directory.Path() / "budgets.w3x", members);
    MapDataEditSession edits(MapDocument::FromArchive(source));
    Attach(edits, members);
    const auto ordinary = Take(CaptureTerrainEditInspection(source, edits));
    const auto exact = Take(CaptureTerrainEditInspection(source, edits, {ordinary.OwnedBytes()}));
    Require(exact.OwnedBytes() == ordinary.OwnedBytes() && exact.IdentitySha256() != ordinary.IdentitySha256(),
        "limits are part of identity even when owned bytes are equal");
    Rejected(CaptureTerrainEditInspection(source, edits, {ordinary.OwnedBytes() - 1}));
    Rejected(CaptureTerrainEditInspection(source, edits, {TerrainInspectionLimits{}.maximumOwnedBytes + 1}),
        "TERRAIN_EDIT_INSPECTION_LIMITS");
    Stroke(edits, TerrainBrushKind::Blight);
    const auto changed = Take(CaptureTerrainEditInspection(source, edits));
    const auto exactChanged = ordinary.OwnedBytes() + changed.Members()[0].current->size() + changed.Members()[1].current->size();
    Require(changed.OwnedBytes() == exactChanged, "aggregate counts both changed originals and both final buffers");
    Require(Take(CaptureTerrainEditInspection(source, edits, {exactChanged})).OwnedBytes() == exactChanged,
        "exact final aggregate boundary succeeds");
    const auto revision = edits.Revision();
    Rejected(CaptureTerrainEditInspection(source, edits, {exactChanged - 1}), "TERRAIN_EDIT_INSPECTION_BUDGET");
    Require(edits.Revision() == revision && edits.CanUndo() && !edits.CanRedo(), "budget failure cannot mutate the draft");
}

void ChangedOrReplacedSourceIsRejectedAndSnapshotOwnsData() {
    TempDirectory directory;
    const auto members = Members();
    const auto path = directory.Path() / "changed.w3x";
    std::optional<TerrainEditInspection> retained;
    {
        auto source = WriteSource(path, members);
        MapDataEditSession edits(MapDocument::FromArchive(source));
        Attach(edits, members);
        retained.emplace(Take(CaptureTerrainEditInspection(source, edits)));
        {
            std::ofstream file(path, std::ios::binary | std::ios::app);
            file.put('x'); Require(file.good(), "mutate only synthetic source after capture");
        }
        Rejected(CaptureTerrainEditInspection(source, edits), "SOURCE_CHANGED");
    }
    Require(retained->Members()[0].original && *retained->Members()[0].original == members[0].data,
        "snapshot survives archive/session destruction and actual source changes");
    auto identitySource = WriteSource(directory.Path() / "identity.w3x", members);
    MapDataEditSession identityEdits(MapDocument::FromArchive(identitySource));
    const auto preservedPath = directory.Path() / "original-file-object.w3x";
    std::filesystem::rename(identitySource.SourcePath(), preservedPath);
    std::filesystem::copy_file(preservedPath, identitySource.SourcePath());
    Require(Take(Sha256File(identitySource.SourcePath())) == identitySource.SourceSha256(), "replacement retains equal content");
    Rejected(CaptureTerrainEditInspection(identitySource, identityEdits));
}

void PutU32(Bytes& bytes, std::size_t offset, std::uint32_t value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

// 原创小归档：只有标记文件列进listfile，W3E以真实hash存在。复用生产MPQ加密helper。
std::filesystem::path HiddenTerrainSource(const TempDirectory& directory) {
    const auto& crypt = Mpq::MpqCrypt::Instance();
    const std::vector<MapArchiveMember> files{{"marker.bin", {std::byte{7}}}, {"war3map.w3e", TerrainBytes()},
        {"(listfile)", {std::byte{'m'}, std::byte{'a'}, std::byte{'r'}, std::byte{'k'}, std::byte{'e'},
            std::byte{'r'}, std::byte{'.'}, std::byte{'b'}, std::byte{'i'}, std::byte{'n'}, std::byte{'\n'}}}};
    constexpr std::uint32_t hashCount = 8, blockCount = 3;
    std::uint32_t dataBytes = 0;
    for (const auto& file : files) dataBytes += static_cast<std::uint32_t>(file.data.size());
    const auto hashOffset = 32u + dataBytes;
    const auto blockOffset = hashOffset + hashCount * 16;
    Bytes bytes(blockOffset + blockCount * 16);
    PutU32(bytes, 0, 0x1a51504d); PutU32(bytes, 4, 32); PutU32(bytes, 8, static_cast<std::uint32_t>(bytes.size()));
    PutU32(bytes, 12, 0x00030000); PutU32(bytes, 16, hashOffset); PutU32(bytes, 20, blockOffset);
    PutU32(bytes, 24, hashCount); PutU32(bytes, 28, blockCount);
    std::fill(bytes.begin() + hashOffset, bytes.begin() + blockOffset, std::byte{0xff});
    std::uint32_t position = 32;
    for (std::uint32_t index = 0; index < blockCount; ++index) {
        const auto& file = files[index];
        std::copy(file.data.begin(), file.data.end(), bytes.begin() + position);
        auto bucket = crypt.HashString(file.name, Mpq::MpqCrypt::HashType::TableOffset) & (hashCount - 1);
        for (;; bucket = (bucket + 1) & (hashCount - 1)) {
            const auto at = hashOffset + bucket * 16;
            std::uint32_t block = 0;
            std::memcpy(&block, bytes.data() + at + 12, sizeof(block));
            if (block != 0xffffffffu) continue;
            PutU32(bytes, at, crypt.HashString(file.name, Mpq::MpqCrypt::HashType::NameA));
            PutU32(bytes, at + 4, crypt.HashString(file.name, Mpq::MpqCrypt::HashType::NameB));
            PutU32(bytes, at + 8, 0); PutU32(bytes, at + 12, index); break;
        }
        const auto size = static_cast<std::uint32_t>(file.data.size());
        PutU32(bytes, blockOffset + index * 16, position); PutU32(bytes, blockOffset + index * 16 + 4, size);
        PutU32(bytes, blockOffset + index * 16 + 8, size);
        PutU32(bytes, blockOffset + index * 16 + 12, Mpq::kFileExists | Mpq::kFileSingleUnit);
        position += size;
    }
    crypt.EncryptBlock(bytes.data() + hashOffset, hashCount * 16, crypt.HashString("(hash table)", Mpq::MpqCrypt::HashType::FileKey));
    crypt.EncryptBlock(bytes.data() + blockOffset, blockCount * 16, crypt.HashString("(block table)", Mpq::MpqCrypt::HashType::FileKey));
    const auto path = directory.Path() / "hidden-terrain.w3x";
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    Require(file.good(), "write original hidden-terrain fixture");
    return path;
}

void ExactCaptureDoesNotDependOnListfile() {
    TempDirectory directory;
    auto source = Take(MapArchiveSession::Open(HiddenTerrainSource(directory)));
    Require(!source.HasMember("war3map.w3e"), "fixture must not expose terrain through the named snapshot");
    MapDataEditSession edits(MapDocument::FromArchive(source));
    Require(edits.AttachTerrain(TerrainBytes()).Succeeded(), "attach independently captured known W3E");
    const auto capture = Take(CaptureTerrainEditInspection(source, edits));
    Rejected(capture.PathingPreservation(), "PATHING_POLICY_CONTEXT_MISSING");
    Require(capture.Members()[0].original && *capture.Members()[0].original == TerrainBytes()
        && capture.Members()[0].current == capture.Members()[0].original,
        "capture reads the actual requested name despite incomplete listfile");
    Require(!capture.Members()[1].original && !capture.Members()[2].original, "other exact names are proven missing");
}

#include "TerrainSettingsCompatibilityCases.h"

} // namespace

int main() {
    try {
        BaselineAndUndoCaptureAreReadOnly();
        AcceptedAndRejectedTerrainRemainInspectable();
        RestrictionInspectionSharesNormalSavePolicyWithoutNativeAcceptance();
        OverlappingIntentSnapshotsShareOwnershipAndBudget();
        FullRawTerrainAndPathingBaselinesMustMatch();
        NativeSourcesCannotBeInventedOrDiscarded();
        SettingsBaselineComparisonIsReadOnly();
        MissingEmptyReadonlyAndDeletionStayDistinct();
        AggregateBudgetsAndIdentityUseFinalOwnedBytes();
        ChangedOrReplacedSourceIsRejectedAndSnapshotOwnsData();
        ExactCaptureDoesNotDependOnListfile();
        TerrainSettingsKnownFieldsComposeAndRestoreWithoutGeometry();
        TerrainSettingsRejectStructuralAndUnknownChangesAtomically();
        std::cout << "terrain-edit-inspection: 13 scenario groups passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "terrain-edit-inspection: " << error.what() << '\n';
        return 1;
    }
}
