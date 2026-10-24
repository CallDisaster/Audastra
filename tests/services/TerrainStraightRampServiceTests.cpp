#include "../TerrainStraightRampFixtures.h"

#include <iostream>
#include <limits>

namespace War3Map::TerrainStraightRampTests {
namespace {
using namespace TerrainStraightRampFixtures;

void CompleteLiteralGridsAndThresholds() {
    for (const auto surface : {SurfaceCase::BelowSurface, SurfaceCase::SurfaceEquality,
        SurfaceCase::BelowDeep, SurfaceCase::DeepEquality}) for (const bool clearBlight : {false, true}) {
        StraightFixture f(surface, clearBlight); const auto rules = f.Rules();
        const auto brush = StraightBrush(); const auto original = f.TerrainLiteral(false);
        Require(rules.WaterBrushRules() && rules.WaterBrushRules()->waterLayerOffset == 32
            && rules.WaterBrushRules()->deepWaterThreshold == 64, "straight uses actual nonzero Water.height and deep threshold");
        Require(!PlanTerrainRamp(f.grid, f.point).Succeeded(), "old dry Ramp cannot acquire the new wet domain");
        auto ordinary = f.grid;
        Require(!ApplyTerrainStroke(ordinary, brush, std::span(&f.point, 1)).Succeeded()
            && Take(SerializeMapTerrain(ordinary)) == original, "unowned generic stroke cannot bypass actual rule preparation");
        const auto created = Take(PrepareTerrainStraightRampStroke(f.grid, *rules.WaterBrushRules(), brush,
            std::span(&f.point, 1), f.doodads, rules.WaterSpecialShapes()));
        Require(Take(SerializeMapTerrain(created.terrain)) == f.TerrainLiteral(true)
            && Take(SerializeMapTerrain(f.grid)) == original && created.result.removedSpecialSourceRecords.empty(),
            "complete W3E only changes literal three R bits and strict-water blight, never water/B/L/palette/special");
        const std::array ground{TerrainNativeGroundRule{"Tg01", true, true, true}};
        for (const bool after : {false, true}) {
            TerrainNativePathingInput input; input.terrain = after ? &created.terrain : &f.grid;
            input.playableBounds = {0, 0, 4096, 4096}; input.groundRules = ground;
            input.water = TerrainNativeWaterRules{0.25f, 64, false}; input.maximumCliffLevel = 14;
            const auto actual = Take(GenerateTerrainNativePathing(input));
            const auto literal = StraightPathingLiteral(surface, clearBlight, after);
            Require(actual.size() + 16 == literal.size(), "independent WPM is complete, including all unchanged protected pixels");
            for (std::size_t i = 0; i < actual.size(); ++i)
                Require((std::to_integer<unsigned>(literal[i + 16]) & 0xeeu) == std::to_integer<unsigned>(actual[i]),
                    "every WPM pixel matches independent cliff/shoulder/depth/equality literal");
        }
        const auto remove = StraightBrush(false);
        const auto removed = Take(PrepareTerrainStraightRampStroke(created.terrain, *rules.WaterBrushRules(), remove,
            std::span(&f.point, 1), f.doodads, rules.WaterSpecialShapes()));
        auto expected = f.grid;
        if (clearBlight) expected.vertices[12 * 33 + 12].textureAndFlagsRaw &= std::uint8_t(~0x20);
        Require(Take(SerializeMapTerrain(removed.terrain)) == Take(SerializeMapTerrain(expected)),
            "straight removal retains exact water fields and does not resurrect already cleared blight");
    }
}

void FourDirectionsAndDryCompatibility() {
    StraightFixture f; auto grid = f.grid; const auto rules = f.Rules(); auto point = f.point;
    std::array<unsigned, 3> selected{16 * 33 + 16, 16 * 33 + 17, 16 * 33 + 18};
    for (unsigned direction = 0; direction < 4; ++direction) {
        for (const bool wet : {false, true}) {
            auto source = grid;
            if (!wet) for (auto& vertex : source.vertices) vertex.textureAndFlagsRaw &= std::uint8_t(~0x40);
            auto expected = source;
            for (const auto index : selected) expected.vertices[index].textureAndFlagsRaw |= 0x10;
            const auto candidate = Take(PrepareTerrainStraightRampStroke(source, *rules.WaterBrushRules(), StraightBrush(),
                std::span(&point, 1), f.doodads, rules.WaterSpecialShapes()));
            Require(Take(SerializeMapTerrain(candidate.terrain)) == Take(SerializeMapTerrain(expected)),
                "all four actual three-point orientations preserve complete original raw fields on wet and dry inputs");
        }
        auto rotated = grid;
        for (unsigned y = 0; y < 33; ++y) for (unsigned x = 0; x < 33; ++x)
            rotated.vertices[x * 33 + 32 - y] = grid.vertices[y * 33 + x];
        grid = std::move(rotated); RefreshTerrainDerived(grid, 0, 0, 33, 33);
        for (auto& index : selected) index = (index % 33) * 33 + 32 - index / 33;
        point = {4096 - point.y, point.x};
    }
}

void RejectedDomainsAndBudgetsAreAtomic() {
    StraightFixture f; const auto resources = f.Rules(); const auto rules = *resources.WaterBrushRules(); const auto brush = StraightBrush();
    const auto reject = [&](TerrainGridSnapshot source, TerrainStrokePoint point = {2080, 2080}) {
        const auto before = Take(SerializeMapTerrain(source));
        Require(!PrepareTerrainStraightRampStroke(source, rules, brush, std::span(&point, 1), f.doodads).Succeeded()
            && Take(SerializeMapTerrain(source)) == before, "unsupported straight input leaves the complete source unchanged");
    };
    auto changed = f.grid; changed.vertices[16 * 33 + 17].groundHeightRaw += 1; reject(changed);
    changed = f.grid; changed.vertices[19 * 33 + 19].textureAndFlagsRaw |= 0x10; reject(changed);
    changed = f.grid; changed.vertices[19 * 33 + 19].waterHeightAndFlagsRaw |= 0x8000; reject(changed);
    changed = f.grid; changed.vertices[16 * 33 + 17].cliffAndLayerRaw = 1; reject(changed);
    changed = f.grid; changed.vertices[16 * 33 + 17].textureAndFlagsRaw |= 0x80; reject(changed);
    changed = f.grid; changed.header.version = 12;
    Require(!PrepareTerrainStraightRampStroke(changed, rules, brush, std::span(&f.point, 1), f.doodads).Succeeded(),
        "unknown native terrain layout cannot acquire straight-ramp support");
    reject(f.grid, {2080.25, 2080}); reject(f.grid, {std::numeric_limits<double>::infinity(), 2080});
    auto limitedRules = rules; limitedRules.maximumCliffLevel = 2;
    Require(!PrepareTerrainStraightRampStroke(f.grid, limitedRules, brush, std::span(&f.point, 1), f.doodads).Succeeded(),
        "actual maximum level is consumed instead of assuming fourteen");
    const std::array twice{f.point, f.point};
    Require(!PrepareTerrainStraightRampStroke(f.grid, rules, brush, twice, f.doodads).Succeeded(), "duplicate points cannot bypass one-point contract");
    for (const unsigned field : {0u, 1u, 2u, 3u, 4u}) {
        auto unsupported = brush;
        if (field == 0) unsupported.radius = 0;
        if (field == 1) unsupported.size = 1;
        if (field == 2) unsupported.seed = 1;
        if (field == 3) unsupported.shape = TerrainBrushShape::Square;
        if (field == 4) unsupported.strength = 1;
        Require(!PrepareTerrainStraightRampStroke(f.grid, rules, unsupported, std::span(&f.point, 1), f.doodads).Succeeded(),
            "new typed mode cannot reinterpret old ignored ordinary brush parameters");
    }
    auto special = f.doodads; special.specialDoodads.push_back({"Doo0", 0, {16, 16}});
    const std::array shapes{TerrainWaterSpecialDoodadShape{0, "Doo0", 0, {16, 16}, 16, 16, 17, 17, {0, 0, 0, 0}}};
    Require(!PrepareTerrainStraightRampStroke(f.grid, rules, brush, std::span(&f.point, 1), special, shapes).Succeeded(),
        "straight never deletes intersecting special models");
    const auto prepared = Take(PrepareTerrainStraightRampStroke(f.grid, rules, brush, std::span(&f.point, 1), f.doodads));
    auto noOpDomain = prepared.terrain; noOpDomain.vertices[19 * 33 + 19].textureAndFlagsRaw |= 0x10;
    reject(noOpDomain);
    Require(Take(PrepareTerrainStraightRampStroke(prepared.terrain, rules, brush, std::span(&f.point, 1), f.doodads)).result.changed.Empty(),
        "same straight create is a true no-op only after complete domain verification");
    TerrainWaterBrushLimits limits; limits.maximumWork = prepared.result.work;
    Require(PrepareTerrainStraightRampStroke(f.grid, rules, brush, std::span(&f.point, 1), f.doodads, {}, limits).Succeeded(),
        "complete reported work includes preflight and owned postprocessing");
    --limits.maximumWork;
    Require(!PrepareTerrainStraightRampStroke(f.grid, rules, brush, std::span(&f.point, 1), f.doodads, {}, limits).Succeeded(),
        "one less work unit rejects atomically");
    limits = {}; limits.maximumCandidateBytes = 1;
    Require(!PrepareTerrainStraightRampStroke(f.grid, rules, brush, std::span(&f.point, 1), f.doodads, {}, limits).Succeeded(),
        "candidate memory cannot be borrowed from capture budget");
}

void SourceBoundHistoryAndColdRecovery() {
    StraightFixture f; const auto resources = f.Rules(); const auto originalSha = f.source->SourceSha256();
    const auto ownerOff = f.Rules(false);
    Require(!PrepareTerrainStraightRampEdit(*f.edits, f.Document(), f.Binding(), ownerOff, StraightBrush(),
        std::span(&f.point, 1), true).Succeeded(), "fixed-v1 history cannot lose enforceWaterLimits identity");
    const auto frozen = Take(f.edits->FreezeForSave(kBudget));
    const auto plan = Take(PrepareTerrainStraightRampEdit(*frozen, f.Document(), f.Binding(), resources, StraightBrush(),
        std::span(&f.point, 1), true));
    f.CheckState(*f.edits, false); Take(ApplyTerrainEdit(*f.edits, f.Document(), plan)); f.CheckState(*f.edits, true);
    Require(!ApplyTerrainEdit(*f.edits, f.Document(), plan).Succeeded(), "worker candidate remains revision-CAS protected");
    const auto journal = Take(TerrainPathingSave::CaptureJournal(*f.source, *f.edits));
    const auto wire = Take(EncodeTerrainStrokeJournal(journal));
    Require(wire[3] == std::byte{'B'} && journal.strokes.size() == 1
        && journal.strokes[0].intent.brush.kind == TerrainBrushKind::StraightRamp
        && journal.strokes[0].intent.cornerRampAlgorithm == TerrainCornerRampAlgorithm::DryOnly
        && journal.strokes[0].rulesSha256 == Take(resources.WaterBrushDependencyFingerprint()),
        "FTJB records independent kind15 with actual rules, keeping corner algorithm neutral");
    const auto decoded = Take(DecodeTerrainStrokeJournal(wire));
    Require(Take(EncodeTerrainStrokeJournal(decoded)) == wire, "complete FTJB canonical roundtrip");
    auto mixed = journal;
    auto corner = journal.strokes.front(); corner.intent.brush.kind = TerrainBrushKind::CornerRamp;
    corner.intent.cornerRampAlgorithm = TerrainCornerRampAlgorithm::WetTerrain;
    mixed.strokes.push_back(corner);
    auto legacy = journal.strokes.front(); legacy.intent.brush.kind = TerrainBrushKind::Ramp;
    legacy.rulesSha256.clear(); mixed.strokes.push_back(legacy);
    const auto mixedWire = Take(EncodeTerrainStrokeJournal(mixed));
    const auto mixedDecoded = Take(DecodeTerrainStrokeJournal(mixedWire));
    Require(mixedWire[3] == std::byte{'B'} && mixedDecoded.strokes.size() == 3
        && mixedDecoded.strokes[0].intent.brush.kind == TerrainBrushKind::StraightRamp
        && mixedDecoded.strokes[1].intent.cornerRampAlgorithm == TerrainCornerRampAlgorithm::WetTerrain
        && mixedDecoded.strokes[2].intent.brush.kind == TerrainBrushKind::Ramp
        && mixedDecoded.strokes[2].rulesSha256.empty() && Take(EncodeTerrainStrokeJournal(mixedDecoded)) == mixedWire,
        "FTJB mixed kinds retain the corner algorithm byte and legacy dry Ramp identity without upgrading either");
    for (const char version : {'1', '4', '8', '9', 'A', 'C', ':'}) {
        auto disguised = wire; disguised[3] = std::byte(version);
        Require(!DecodeTerrainStrokeJournal(disguised).Succeeded(), "old/unknown version cannot grant kind15 capability");
    }
    for (std::size_t size = 0; size < wire.size(); ++size)
        Require(!DecodeTerrainStrokeJournal(std::span(wire).first(size)).Succeeded(), "every FTJB truncation rejects");
    auto malformed = journal; malformed.strokes[0].intent.points.push_back(f.point);
    Require(!EncodeTerrainStrokeJournal(malformed).Succeeded(), "FTJB remains strictly one point");
    malformed = journal; malformed.strokes[0].intent.cornerRampAlgorithm = TerrainCornerRampAlgorithm::WetTerrain;
    Require(!EncodeTerrainStrokeJournal(malformed).Succeeded(), "new kind cannot borrow corner algorithm fields");
    malformed = journal; malformed.strokes[0].rulesSha256.clear();
    Require(!EncodeTerrainStrokeJournal(malformed).Succeeded(), "typed identity cannot be omitted");
    auto restored = Take(TerrainPathingSave::RestoreJournal(*f.source, MapDocument::FromMembers(f.members),
        f.members[0].data, f.members[1].data, decoded, *f.edits, f.game));
    Require(restored.receipts.size() == 1 && restored.waterOwners.size() == 1
        && restored.session->MatchesTerrainJournalDraft(*f.edits), "cold recovery retains full actual source owner and one receipt");
    f.CheckState(*restored.session, true);
    Require(restored.session->Replay(restored.receipts[0], false).Succeeded(), "restored straight Undo"); f.CheckState(*restored.session, false);
    Require(restored.session->Replay(restored.receipts[0], true).Succeeded(), "restored straight Redo"); f.CheckState(*restored.session, true);
    malformed = journal; malformed.strokes[0].rulesSha256.assign(64, 'f');
    Require(!TerrainPathingSave::RestoreJournal(*f.source, MapDocument::FromMembers(f.members), f.members[0].data,
        f.members[1].data, malformed, *f.edits, f.game).Succeeded(), "drifted actual rules cannot install any replay prefix");
    Require(Take(Sha256File(f.source->SourcePath())) == originalSha, "history never changes source map bytes");
}

void FormalSaveAsAndInPlaceRetainUndoRedo() {
    for (const bool inPlace : {false, true}) for (const bool follow : {false, true}) {
        StraightFixture f; const auto resources = f.Rules(); const auto originalPath = f.source->SourcePath();
        const auto originalSha = f.source->SourceSha256();
        Take(ApplyTerrainEdit(*f.edits, f.Document(), Take(PrepareTerrainStraightRampEdit(*f.edits, f.Document(), f.Binding(),
            resources, StraightBrush(), std::span(&f.point, 1), follow))));
        const auto save = [&](bool created, const char* name) {
            f.CheckState(*f.edits, created, follow);
            const auto plan = Take(TerrainPathingSave::PrepareForSave(*f.source, *f.edits, f.game));
            Require(plan.SaveGate().Succeeded(), "formal straight save requires complete actual G0/G1 comparison");
            MapSaveTask task; Require(task.FreezeArchive(*f.source).Succeeded(), "formal save freezes source identity");
            task.inPlace = inPlace; task.target = inPlace ? f.source->SourcePath() : f.root / name;
            task.inPlaceOptions.backupDirectory = f.root / "straight-backups"; task.inPlaceOptions.keepBackups = 0;
            task.replacements = f.edits->Replacements(); task.removals = f.edits->Removals();
            Require(plan.InstallPathingOutput(*f.edits, task.replacements).Succeeded(), "formal save installs complete derived WPM");
            task.beforePublish = [&] { return plan.VerifyForSave(*f.edits, task.sourceSha256, task.replacements, task.removals); };
            const auto previousSha = f.source->SourceSha256(); MapSaveService::Run(task);
            Require(task.executed && task.succeeded && task.nextSession, "formal save publishes then reopens actual archive");
            if (inPlace) Require(!task.backup.empty() && Take(Sha256File(task.backup)) == previousSha,
                "in-place backup exactly matches prior source archive");
            auto actual = Take(MapArchiveSession::Open(task.written));
            Require(plan.VerifyReadback(actual).Succeeded()
                && Take(actual.ReadMember("war3map.w3e")) == f.TerrainLiteral(created)
                && Take(actual.ReadMember("war3map.wpm")) == StraightPathingLiteral(f.surface, f.blightAbove, created)
                && Take(actual.ReadMember("war3mapUnits.doo")) == Take(f.UnitsLiteral(created && follow).Serialize())
                && Take(actual.ReadMember("war3map.doo")) == Take(f.DoodadsLiteral(created && follow).Serialize()),
                "formal readback matches complete independent W3E/WPM and both nonempty placement payloads");
            for (const auto& member : f.members) if (member.name != "war3map.w3e" && member.name != "war3map.wpm"
                && member.name != "war3mapUnits.doo" && member.name != "war3map.doo")
                Require(Take(actual.ReadMember(member.name)) == member.data, "every unrelated authored member survives formal save");
            Require(plan.CommitSaved(*f.edits, MapDocument::FromArchive(actual)).Succeeded() && !f.edits->IsDirty(),
                "formal save rebases source and retains command history");
            f.source = std::make_unique<MapArchiveSession>(std::move(actual));
        };
        save(true, "straight-created.w3x");
        Require(f.edits->Undo() && !f.edits->CanUndo(), "one straight Undo remains available after save"); save(false, "straight-undone.w3x");
        Require(f.edits->Redo() && !f.edits->CanRedo(), "straight Redo remains available after saved Undo"); save(true, "straight-redone.w3x");
        if (!inPlace) Require(Take(Sha256File(originalPath)) == originalSha, "SaveAs leaves original source archive unchanged");
    }
}
} // namespace

void CheckTerrainStraightRampContracts() {
    CompleteLiteralGridsAndThresholds(); FourDirectionsAndDryCompatibility(); RejectedDomainsAndBudgetsAreAtomic();
    SourceBoundHistoryAndColdRecovery(); FormalSaveAsAndInPlaceRetainUndoRedo();
}
} // namespace War3Map::TerrainStraightRampTests

int main() {
    try { War3Map::TerrainStraightRampTests::CheckTerrainStraightRampContracts(); return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
