#pragma once

#include "TerrainHistorySourceShadowCases.h"
#include "War3MapCore/Hash.h"

namespace AudastraTest::TerrainOrdinaryShadowHistoryCases {
using namespace AudastraTest::TerrainResize;
using namespace War3Map::Services;
namespace SourceShadow = AudastraTest::TerrainHistorySourceShadowCases;

inline void Number(Bytes& output, std::uint64_t value, unsigned count = 4) {
    for (unsigned i = 0; i < count; ++i) output.push_back(std::byte((value >> (i * 8)) & 255));
}
inline void Payload(Bytes& output, std::span<const std::byte> value) {
    Number(output, value.size()); output.insert(output.end(), value.begin(), value.end());
}
inline void String(Bytes& output, std::string_view value) { Payload(output, AudastraTest::TerrainBounds::Text(value)); }

inline TerrainStrokeJournal CodecFixture(char version) {
    TerrainStrokeJournal journal; journal.sourceSha256.assign(64, 'a');
    TerrainJournalHistory history; history.savedIndex = history.currentIndex = 1;
    for (std::size_t i = 0; i < TerrainJournalMemberNames.size(); ++i) {
        journal.members.push_back({std::string(TerrainJournalMemberNames[i]), i < 2,
            i < 2 ? std::string(64, 'b') : std::string{}, i < 2 ? 4u : 0u});
        history.baselineMembers.push_back({std::string(TerrainJournalMemberNames[i]), i < 2,
            i < 2 ? Bytes(4, std::byte(i + 1)) : Bytes{}});
    }
    if (version != 'E') history.baselineShadow = ArchiveExactMember{"war3map.shd", true,
        {std::byte{0}, std::byte{1}, std::byte{0x80}, std::byte{0xff}}};
    TerrainJournalStroke stroke;
    if (version == 'H') {
        stroke.intent.resize = TerrainResizeIntent{1, {-32, -64, 32, 32}};
        stroke.rulesSha256.assign(64, 'd');
    } else {
        stroke.intent.brush.kind = TerrainBrushKind::Raise; stroke.intent.brush.radius = 0;
        stroke.intent.points = {{256, 256}, {384, 256}};
    }
    journal.strokes.push_back(std::move(stroke)); journal.history = std::move(history); return journal;
}

// 独立写历史封套及其旧结构identity。typed正文复用未修改的笔刷codec，
// 不调用历史BaselineJournal/encoder来构造E/H旧字节的expected。
inline Bytes ExpectedEnvelope(const TerrainStrokeJournal& journal, char version) {
    const auto& history = *journal.history; TerrainStrokeJournal typed;
    typed.strokes = journal.strokes; typed.baselinePathingLayers = history.baselinePathingLayers;
    Bytes identity; String(identity, std::string("FTJ") + version + "-historical-structure-v1");
    for (const auto& member : history.baselineMembers) {
        const auto hash = member.present ? Take(Sha256(member.bytes)) : std::string{};
        typed.members.push_back({member.name, member.present, hash, member.bytes.size()});
        String(identity, member.name); Number(identity, member.present, 1); String(identity, hash); Number(identity, member.bytes.size(), 8);
    }
    if (history.baselineShadow) {
        const auto& shadow = *history.baselineShadow;
        String(identity, shadow.name); Number(identity, shadow.present, 1); Number(identity, shadow.bytes.size(), 8);
        String(identity, shadow.present ? Take(Sha256(shadow.bytes)) : std::string{});
    }
    typed.sourceSha256 = Take(Sha256(identity));
    const auto child = Take(EncodeTerrainStrokeJournal(typed));
    Bytes output = AudastraTest::TerrainBounds::Text(std::string("FTJ") + version); String(output, journal.sourceSha256);
    Number(output, journal.members.size(), 1);
    for (const auto& member : journal.members) {
        String(output, member.name); Number(output, member.present, 1); String(output, member.sha256); Number(output, member.bytes, 8);
    }
    Payload(output, journal.baselinePathingLayers); Number(output, history.savedIndex); Number(output, history.currentIndex);
    Number(output, history.baselineMembers.size(), 1);
    for (const auto& member : history.baselineMembers) {
        String(output, member.name); Number(output, member.present, 1); Payload(output, member.bytes);
    }
    if (history.baselineShadow) {
        const auto& shadow = *history.baselineShadow;
        String(output, shadow.name); Number(output, shadow.present, 1); Payload(output, shadow.bytes);
    }
    Payload(output, child); return output;
}

inline void LegacyEnvelopesAndNewVersionDomainsAreStrict() {
    for (const char version : {'E', 'H', 'I'}) {
        const auto journal = CodecFixture(version); const auto expected = ExpectedEnvelope(journal, version);
        Require(Take(EncodeTerrainStrokeJournal(journal)) == expected
            && Take(EncodeTerrainStrokeJournal(Take(DecodeTerrainStrokeJournal(expected)))) == expected,
            "independent E/H legacy and new I envelopes retain exact canonical bytes and digest domains");
        for (const char other : {'E', 'H', 'I'}) if (other != version) {
            auto mislabeled = expected; mislabeled[3] = std::byte(other);
            Require(!DecodeTerrainStrokeJournal(mislabeled).Succeeded(), "changing a history magic cannot borrow another version's shape or digest domain");
        }
    }
    auto ordinary = CodecFixture('I'); const auto wire = Take(EncodeTerrainStrokeJournal(ordinary));
    for (std::size_t length = 0; length < wire.size(); ++length)
        Require(!DecodeTerrainStrokeJournal(std::span(wire).first(length)).Succeeded(), "every truncated small I envelope rejects without partial history");
    auto trailing = wire; trailing.push_back(std::byte{0});
    Require(!DecodeTerrainStrokeJournal(trailing).Succeeded(), "I cannot hide unknown trailing payload");
    const auto child = Take(EncodeTerrainStrokeJournal(Take(TerrainHistoryBaselineJournal(ordinary))));
    for (const char nested : {'E', 'H', 'I'}) {
        auto recursive = wire; recursive[wire.size() - child.size() + 3] = std::byte(nested);
        Require(!DecodeTerrainStrokeJournal(recursive).Succeeded(), "all nested history markers reject before recursive decoding");
    }
    ordinary.history->baselineShadow->present = false;
    Require(!EncodeTerrainStrokeJournal(ordinary).Succeeded(), "I explicit absence cannot carry raw shadow bytes");
    ordinary.history->baselineShadow->bytes.clear();
    const auto absent = Take(DecodeTerrainStrokeJournal(Take(EncodeTerrainStrokeJournal(ordinary))));
    Require(absent.history->baselineShadow && !absent.history->baselineShadow->present,
        "I explicit absence remains distinct from a legacy E record without shadow material");
    ordinary.history->baselineShadow->present = true;
    ordinary.history->baselineShadow->bytes.resize(TerrainJournalMaximumBytes);
    Require(!EncodeTerrainStrokeJournal(ordinary).Succeeded(), "I raw shadow and full member budget share the unchanged total history cap");
}

inline std::filesystem::path PhysicalDirectory(Fixture& fixture) {
    const auto path = fixture.directory / "saved-physical-directory";
    Require(std::filesystem::create_directory(path), "create original physical saved directory");
    for (const auto& member : fixture.archive->Members()) {
        const auto target = path / member.name; std::filesystem::create_directories(target.parent_path());
        Check(IO::WriteFileBytes(target, member.data));
    }
    return path;
}

inline void OrdinaryProjectColdOpenBeforeFirstResizeRetainsTheOldShadow() {
    for (const bool present : {false, true}) for (const bool directory : {false, true}) {
        Fixture fixture(false, present); const auto sourcePath = fixture.archive->SourcePath();
        const auto sourceSha = fixture.archive->SourceSha256(); SourceShadow::Raise(fixture);
        const auto lease = Take(TerrainHistorySave::BindSourceShadow(*fixture.archive,
            Take(fixture.edits->PrepareTerrainHistoryForSave())));
        SourceShadow::Save(fixture, lease, "pre-resize-ordinary-station.w3x");
        const auto physical = MapDocument::FromArchive(*fixture.archive);
        const auto terrain = Take(fixture.archive->ReadMember("war3map.w3e"));
        const auto pathing = Take(fixture.archive->ReadMember("war3map.wpm"));
        std::optional<DirectoryWorkspace> workspace;
        if (directory) workspace.emplace(Take(DirectoryWorkspace::Open(PhysicalDirectory(fixture))));
        const auto capture = [&](const MapDataEditSession& edits) {
            return directory ? TerrainPathingSave::CaptureJournal(workspace->Snapshot(), edits)
                : TerrainPathingSave::CaptureJournal(*fixture.archive, edits);
        };
        const auto restore = [&](const TerrainStrokeJournal& journal, const MapDataEditSession& raw) {
            return directory ? TerrainPathingSave::RestoreJournal(workspace->Snapshot(), physical, terrain, pathing, journal, raw, fixture.gameRoot)
                : TerrainPathingSave::RestoreJournal(*fixture.archive, physical, terrain, pathing, journal, raw, fixture.gameRoot);
        };
        const auto initialWire = Take(EncodeTerrainStrokeJournal(Take(capture(*fixture.edits))));
        const auto initial = Take(DecodeTerrainStrokeJournal(initialWire));
        Require(initialWire[3] == std::byte{'I'} && initial.history && initial.history->baselineShadow
            && initial.history->baselineShadow->present == present
            && initial.history->baselineShadow->bytes == (present ? ShadowLiteral() : Bytes{}),
            "ordinary pre-resize project persists exact old raw SHD or explicit absence as I");
        MapDataEditSession clean(physical); Check(clean.AttachTerrain(terrain)); Check(clean.AttachPathing(pathing));
        auto corrupted = initial;
        if (present) corrupted.history->baselineShadow->bytes[1] ^= std::byte{1};
        else {
            corrupted.history->baselineShadow->present = true;
            corrupted.history->baselineShadow->bytes.assign(128u * 128u, std::byte{0});
        }
        const auto wrongShadow = restore(corrupted, clean);
        Require(!wrongShadow.Succeeded() && !wrongShadow.value,
            "I raw shadow or absence is replay input and must still reproduce the actual physical station exactly");
        corrupted = initial; corrupted.history->baselineMembers[0].bytes.back() ^= std::byte{1};
        const auto wrongTerrain = restore(corrupted, clean);
        Require(!wrongTerrain.Succeeded() && !wrongTerrain.value,
            "I cannot acquire private commands from a same-dimension but changed historical W3E");
        auto opened = Take(restore(initial, clean));
        Require(!opened.session->IsDirty() && !opened.session->HasTerrainTypedResizeState()
            && Take(EncodeTerrainStrokeJournal(Take(capture(*opened.session)))) == initialWire,
            "I cold open proves the ordinary station and can be saved again without adding typed resize authority");
        fixture.edits = std::move(opened.session);
        const auto resources = Take(TerrainResizeEditService::Capture(*fixture.archive, *fixture.edits, fixture.gameRoot));
        Check(fixture.edits->ApplyPreparedTerrainResizeEdit(Take(fixture.edits->PrepareTerrainResizeEdit(
            resources, TerrainResizeIntent{1, {-32, 0, 32, 32}}))));
        const auto secondWire = Take(EncodeTerrainStrokeJournal(Take(capture(*fixture.edits))));
        const auto resized = Take(DecodeTerrainStrokeJournal(secondWire));
        Require(secondWire[3] == std::byte{'H'} && resized.history->savedIndex == 1
            && resized.history->currentIndex == 2 && resized.history->baselineShadow->bytes == initial.history->baselineShadow->bytes
            && resized.history->baselineShadow->present == present,
            "first resize after ordinary cold open upgrades to H with the same oldest shadow, never newer physical substitution");
        MapDataEditSession raw(physical); Check(raw.AttachTerrain(terrain)); Check(raw.AttachPathing(pathing));
        Check(raw.SetTerrainResizeDraft(Take(SerializeMapTerrain(*fixture.edits->Terrain())), SourceShadow::CurrentPathing(fixture),
            Take(fixture.edits->ExportTerrainResizeDraft())));
        Check(raw.SetSerialized(MapDataKind::Settings, fixture.edits->Snapshot().Member(MapDataKind::Settings).sources.front().data));
        auto reopened = Take(restore(resized, raw));
        Require(Take(EncodeTerrainStrokeJournal(Take(capture(*reopened.session)))) == secondWire,
            "second cold H open retains the complete I-origin history and exact canonical bytes");
        Check(reopened.session->Replay(reopened.receipts[1], false));
        const auto shadow = Take(reopened.session->TerrainResizeShadowDraft());
        Require(!reopened.session->IsDirty() && Take(SerializeMapTerrain(*reopened.session->Terrain())) == terrain
            && Take(reopened.session->SerializePathingRestrictionDraft()).empty()
            && shadow.present == present && shadow.bytes == (present ? ShadowLiteral() : Bytes{}),
            "Undo after two cold opens restores real saved height, clean P/S and raw SHD");
        Check(reopened.session->Replay(reopened.receipts[0], false));
        Require(Take(SerializeMapTerrain(*reopened.session->Terrain())) == TerrainLiteral(),
            "Undo crosses the original ordinary save station to independent original bytes");
        for (const auto& receipt : reopened.receipts) Check(reopened.session->Replay(receipt, true));
        Require(reopened.session->MatchesTerrainJournalDraft(raw) && Take(Sha256File(sourcePath)) == sourceSha,
            "full Redo matches all second cold raw output while original source remains read-only");
    }
}

inline void LegacyOrdinaryHistoryDoesNotAcquireMissingShadowMaterial() {
    Fixture fixture; SourceShadow::Raise(fixture);
    SourceShadow::Save(fixture, Take(fixture.edits->PrepareTerrainHistoryForSave()), "legacy-no-shadow-station.w3x");
    const auto physical = MapDocument::FromArchive(*fixture.archive);
    const auto terrain = Take(fixture.archive->ReadMember("war3map.w3e")), pathing = Take(fixture.archive->ReadMember("war3map.wpm"));
    const auto wire = Take(EncodeTerrainStrokeJournal(Take(TerrainPathingSave::CaptureJournal(*fixture.archive, *fixture.edits))));
    const auto journal = Take(DecodeTerrainStrokeJournal(wire));
    Require(wire[3] == std::byte{'E'} && !journal.history->baselineShadow, "unbound legacy ordinary history retains E and no fabricated shadow");
    MapDataEditSession raw(physical); Check(raw.AttachTerrain(terrain)); Check(raw.AttachPathing(pathing));
    auto restored = Take(TerrainPathingSave::RestoreJournal(*fixture.archive, physical, terrain, pathing, journal, raw, fixture.gameRoot));
    const auto lease = Take(restored.session->PrepareTerrainHistoryForSave());
    const auto refused = TerrainHistorySave::BindSourceShadow(*fixture.archive, lease);
    Require(!refused.Succeeded() && HasCode({refused.diagnostics}, "TERRAIN_HISTORY_SOURCE_SHADOW_UNAVAILABLE"),
        "old E cannot backfill oldest SHD from the latest physical save even when dimensions match");
}

inline void RunTerrainOrdinaryShadowHistoryCases() {
    LegacyEnvelopesAndNewVersionDomainsAreStrict();
    OrdinaryProjectColdOpenBeforeFirstResizeRetainsTheOldShadow();
    LegacyOrdinaryHistoryDoesNotAcquireMissingShadowMaterial();
}
} // namespace AudastraTest::TerrainOrdinaryShadowHistoryCases
