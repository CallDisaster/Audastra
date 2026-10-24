#include "support/TerrainBoundsFixture.h"

#include <fstream>

namespace {
using namespace AudastraTest::TerrainBounds;
constexpr std::size_t maximumBytes = 1024u * 1024u;

Bytes CliffRows(std::span<const std::string_view> ids) {
    std::string text = "ID;PWXL;N;E\nB;X2;Y" + std::to_string(ids.size() + 1)
        + ";D0\nC;X1;Y1;K\"cliffID\"\nC;X2;K\"name\"\n";
    for (std::size_t i = 0; i < ids.size(); ++i)
        text += "C;X1;Y" + std::to_string(i + 2) + ";K\"" + std::string(ids[i])
            + "\"\nC;X2;K\"synthetic\"\n";
    return Text(text + "E\n");
}

Bytes CompleteRows() {
    constexpr std::string_view ids[]{"Cl01", "Cl02", "Cl03", "Cl04"};
    return CliffRows(ids);
}

void Reopen(Fixture& fixture, const char* name = "palette-source.w3x") {
    fixture.archive = std::make_unique<MapArchiveSession>(Take(MapArchiveSession::Open(
        Take(WriteVerifiedArchive(fixture.directory / name, fixture.members)).path)));
    fixture.edits = std::make_unique<MapDataEditSession>(MapDocument::FromMembers(fixture.members));
    Check(fixture.edits->AttachTerrain(fixture.members[0].data));
    Check(fixture.edits->AttachPathing(fixture.members[1].data));
}

void Prepare(Fixture& fixture, bool unusedSlot = false, bool unusualVariation = false) {
    auto grid = *Take(ParseMapTerrain(fixture.members[0].data)).Snapshot();
    if (unusedSlot) grid.header.cliffTilesets.push_back("Cl03");
    // 所有地格保持平地；原有地面/悬崖变体原字不因换表重写或洗白。
    for (std::size_t i = 0; i < grid.vertices.size(); ++i)
        grid.vertices[i].variationRaw = static_cast<std::uint8_t>((i % 18)
            | (((unusualVariation ? i % 8 : i % 3)) << 5));
    fixture.members[0].data = Take(SerializeMapTerrain(grid));
    fixture.members.push_back({"TerrainArt/CliffTypes.slk", CompleteRows()});
    Reopen(fixture);
}

void Replace(Fixture& fixture, std::uint32_t slot, const std::string& rawcode) {
    const auto result = Take(fixture.edits->EditTerrainTextureSlot(TerrainTextureTable::Cliff, slot, rawcode));
    Require(result.headerChanged, "existing cliff palette command changes its real header");
}

Bytes ExpectedHeaderChange(const Bytes& source, std::uint32_t slot, std::string_view rawcode) {
    auto expected = source;
    expected[9] = std::byte{1}; // usesCustomTileset little-endian u32。
    // 本原创样例只有一项地面表：cliffCount位于21，首cliff rawcode位于25。
    for (std::size_t i = 0; i < 4; ++i) expected[25 + slot * 4 + i] = std::byte(rawcode[i]);
    return expected;
}

void RequireOtherMembersPreserved(const Fixture& fixture) {
    for (const auto& member : fixture.members) {
        if (member.name == "war3map.w3e") continue;
        Require(Take(fixture.archive->ReadMember(member.name)) == member.data,
            "cliff palette save preserves complete WPM and every unrelated member");
    }
}

void PaletteSavePreservesAllVertexBytesAndUndoRedo() {
    for (const bool unusualVariation : {false, true}) {
        Fixture fixture; Prepare(fixture, false, unusualVariation);
        const auto original = fixture.members[0].data;
        const auto expected = ExpectedHeaderChange(original, 0, "Cl02");
        Replace(fixture, 0, "Cl02");
        const auto revision = fixture.edits->Revision();
        Require(!Take(fixture.edits->EditTerrainTextureSlot(TerrainTextureTable::Cliff, 0, "Cl02")).headerChanged
            && fixture.edits->Revision() == revision,
            "replacing an existing slot by its current rawcode does not create a phantom edit");
        Require(!fixture.edits->VerifyTerrainPathingForSave().Succeeded(),
            "palette changes cannot pass a preservation-only gate");
        Require(Take(SerializeMapTerrain(*fixture.edits->Terrain())) == expected,
            "cliff table replacement changes only its literal rawcode and custom-table flag");
        Require(std::equal(original.begin() + 45, original.end(), expected.begin() + 45),
            "every seven-byte vertex, cliff slot and variation remains byte-identical");
        auto owner = fixture.Capture();
        const auto before = *Take(ParseMapTerrain(original)).Snapshot();
        const Bytes expectedNative(40 * 32, std::byte{0x40});
        Require(Take(owner.GenerateNative(before, TerrainNativeStage::Before, maximumBytes)) == expectedNative
            && Take(owner.GenerateNative(*fixture.edits->Terrain(), TerrainNativeStage::After, maximumBytes)) == expectedNative,
            "full independent G0 and G1 masks are unchanged by cliff material selection");
        fixture.Save("palette-saved.w3x");
        Require(Take(fixture.archive->ReadMember("war3map.w3e")) == expected,
            "ordinary native save writes exact palette header and complete original vertices");
        RequireOtherMembersPreserved(fixture);
        Require(fixture.edits->Undo(), "undo saved palette command"); fixture.Save("palette-undo.w3x");
        Require(Take(fixture.archive->ReadMember("war3map.w3e")) == original,
            "Undo after actual save restores complete original header and raw payload");
        RequireOtherMembersPreserved(fixture);
        Require(fixture.edits->Redo(), "redo saved palette command"); fixture.Save("palette-redo.w3x");
        Require(Take(fixture.archive->ReadMember("war3map.w3e")) == expected,
            "Redo after saved Undo restores deterministic exact output");
        RequireOtherMembersPreserved(fixture);
    }
}

void RawProjectRecoveryRetainsPaletteAndOrdinarySave() {
    Fixture fixture; Prepare(fixture, true);
    Replace(fixture, 0, "Cl02"); Replace(fixture, 1, "Cl04");
    auto expected = ExpectedHeaderChange(fixture.members[0].data, 0, "Cl02");
    expected = ExpectedHeaderChange(expected, 1, "Cl04");
    const auto raw = Take(SerializeMapTerrain(*fixture.edits->Terrain()));
    Require(raw == expected, "both used and unused slot changes remain in the raw project overlay");
    const auto journal = fixture.edits->ExportTerrainStrokeJournal();
    Require(!journal.Succeeded() && HasCode({journal.diagnostics}, "TERRAIN_JOURNAL_HISTORY_UNEXPRESSIBLE"),
        "header-only edits are not forged into brush strokes or trusted water provenance");
    auto cold = std::make_unique<MapDataEditSession>(MapDocument::FromMembers(fixture.members));
    Check(cold->AttachTerrain(fixture.members[0].data)); Check(cold->AttachPathing(fixture.members[1].data));
    Check(cold->SetTerrainAndPathing(raw, fixture.members[1].data));
    Require(Take(SerializeMapTerrain(*cold->Terrain())) == expected,
        "project raw recovery restores the actual W3E header and all vertices");
    Require(cold->Undo() && Take(SerializeMapTerrain(*cold->Terrain())) == fixture.members[0].data,
        "raw recovery produces a real reversible header transaction");
    Require(cold->Redo() && Take(SerializeMapTerrain(*cold->Terrain())) == expected,
        "raw recovered header transaction supports Redo");
    fixture.edits = std::move(cold); fixture.Save("palette-cold.w3x");
    Require(Take(fixture.archive->ReadMember("war3map.w3e")) == expected,
        "cold-recovered palette reaches ordinary native save without a manufactured journal");
    RequireOtherMembersPreserved(fixture);
}

void LastConcreteSlotIsWritableWithoutUsingSentinel() {
    Fixture fixture;
    auto grid = *fixture.edits->Terrain();
    for (unsigned i = 1; i < 15; ++i) grid.header.cliffTilesets.push_back("C" + std::to_string(100 + i));
    fixture.members[0].data = Take(SerializeMapTerrain(grid));
    std::vector<std::string_view> ids;
    for (const auto& id : grid.header.cliffTilesets) ids.push_back(id);
    ids.push_back("Cl02");
    fixture.members.push_back({"TerrainArt/CliffTypes.slk", CliffRows(ids)}); Reopen(fixture);
    const auto expected = ExpectedHeaderChange(fixture.members[0].data, 14, "Cl02");
    Replace(fixture, 14, "Cl02"); fixture.Save("last-concrete-slot.w3x");
    Require(Take(fixture.archive->ReadMember("war3map.w3e")) == expected
        && fixture.edits->Terrain()->header.cliffTilesets.size() == 15,
        "fifteen-entry palette permits its last concrete slot without allocating reserved slot fifteen");
    RequireOtherMembersPreserved(fixture);
}

void EveryChangedSlotNeedsBothActualRows() {
    for (const bool unusedSlot : {false, true}) {
        for (const std::string_view missing : {unusedSlot ? "Cl03" : "Cl01", unusedSlot ? "Cl04" : "Cl02"}) {
            Fixture fixture; Prepare(fixture, unusedSlot);
            constexpr std::string_view all[]{"Cl01", "Cl02", "Cl03", "Cl04"};
            std::vector<std::string_view> ids;
            for (const auto id : all) if (id != missing) ids.push_back(id);
            fixture.members.back().data = CliffRows(ids); Reopen(fixture, "missing-row.w3x");
            Replace(fixture, unusedSlot ? 1u : 0u, unusedSlot ? "Cl04" : "Cl02");
            const auto source = *Take(ParseMapTerrain(fixture.members[0].data)).Snapshot();
            const auto owner = TerrainPathingResources::CaptureNative(*fixture.archive, source,
                *fixture.edits->Terrain(), {}, maximumBytes);
            Require(!owner.Succeeded() && HasCode({owner.diagnostics}, "TERRAIN_PATHING_PROPERTY_MISSING"),
                "each changed used or unused slot binds its exact old and new rawcode rows");
            const auto plan = Take(TerrainSavePlan::PrepareForSave(*fixture.archive, *fixture.edits, {}));
            Require(!plan.SaveGate().Succeeded(), "missing cliff row refuses normal save without discarding the draft");
            Require(fixture.edits->IsDirty() && fixture.edits->Undo() && fixture.edits->Redo(),
                "missing-row refusal preserves real Undo and Redo history");
        }
    }
    Fixture exact; Prepare(exact);
    constexpr std::string_view aliasIds[]{"Cl01", "cl02"};
    exact.members.back().data = CliffRows(aliasIds); Reopen(exact, "case-row.w3x"); Replace(exact, 0, "Cl02");
    const auto before = *Take(ParseMapTerrain(exact.members[0].data)).Snapshot();
    Require(!TerrainPathingResources::CaptureNative(*exact.archive, before, *exact.edits->Terrain(), {}, maximumBytes).Succeeded(),
        "rawcode case aliases cannot substitute for the actual selected cliff row");

    for (const std::string_view duplicate : {"Cl01", "Cl02"}) {
        Fixture ambiguous; Prepare(ambiguous);
        const std::string_view duplicateIds[]{"Cl01", "Cl02", duplicate};
        ambiguous.members.back().data = CliffRows(duplicateIds); Reopen(ambiguous, "duplicate-row.w3x");
        Replace(ambiguous, 0, "Cl02");
        const auto source = *Take(ParseMapTerrain(ambiguous.members[0].data)).Snapshot();
        const auto refused = TerrainPathingResources::CaptureNative(*ambiguous.archive, source,
            *ambiguous.edits->Terrain(), {}, maximumBytes);
        Require(!refused.Succeeded() && HasCode({refused.diagnostics}, "TERRAIN_TABLE_DUPLICATE"),
            "duplicate old or new cliff rawcode rows are explicitly ambiguous, not mere presence");
    }
}

void ResourceIdentityBudgetAndFrozenOutputsStayStrict() {
    Fixture fixture; Prepare(fixture); Replace(fixture, 0, "Cl02");
    const auto before = *Take(ParseMapTerrain(fixture.members[0].data)).Snapshot();
    const auto owner = fixture.Capture();
    Check(owner.VerifySourceInputs(*fixture.archive, maximumBytes));
    Require(!TerrainPathingResources::CaptureNative(*fixture.archive, before, *fixture.edits->Terrain(), {}, 64).Succeeded()
        && !owner.VerifySourceInputs(*fixture.archive, 64).Succeeded()
        && !owner.GenerateNative(*fixture.edits->Terrain(), 64).Succeeded(),
        "capture, source verification and native generation retain their independent byte budgets");
    auto changed = fixture.members.back().data; changed.push_back(std::byte{'\n'});
    const auto output = Take(SerializeMapTerrain(*fixture.edits->Terrain()));
    const std::vector<MemberReplacement> replacement{{"war3map.w3e", output}, {"TerrainArt/CliffTypes.slk", changed}};
    Require(!owner.VerifyFrozenOutput(replacement, {}).Succeeded(), "frozen output cannot change an actual cliff table");
    const std::vector<MemberReplacement> terrainOnly{{"war3map.w3e", output}};
    Check(owner.VerifyFrozenOutput(terrainOnly, {}));
    const std::vector<std::string> removal{"TerrainArt\\CliffTypes.slk"};
    Require(!owner.VerifyFrozenOutput(terrainOnly, removal).Succeeded(), "canonical table deletion cannot drop a bound dependency");
    const std::vector<MemberReplacement> duplicateOverrides{{"war3map.w3e", output},
        {"TerrainArt/CliffTypes.slk", CompleteRows()}, {"terrainart\\clifftypes.slk", CompleteRows()}};
    Require(!owner.VerifyFrozenOutput(duplicateOverrides, {}).Succeeded(),
        "canonical output aliases cannot override the same cliff table twice");
    const auto drift = Take(MapArchiveSession::Open(Take(fixture.archive->SaveCandidate(
        fixture.directory / "palette-drift.w3x", replacement)).path));
    Require(!owner.VerifyReadback(drift, maximumBytes).Succeeded(), "independent readback catches table-byte drift");

    // 同字节转到游戏回退仍是不同来源，不能用内容相等冒充同一winner。
    auto noTable = fixture.members; noTable.pop_back();
    const auto fallbackMap = Take(MapArchiveSession::Open(Take(WriteVerifiedArchive(
        fixture.directory / "fallback-map.w3x", noTable)).path));
    const auto game = fixture.directory / "game";
    Require(std::filesystem::create_directory(game), "private game data directory");
    const std::vector<MapArchiveMember> table{{"TerrainArt/CliffTypes.slk", CompleteRows()}};
    Take(WriteVerifiedArchive(game / "war3.mpq", table));
    const auto gameOwner = Take(TerrainPathingResources::CaptureNative(fallbackMap, before,
        *fixture.edits->Terrain(), game, maximumBytes));
    Take(WriteVerifiedArchive(game / "war3patch.mpq", table));
    Require(!gameOwner.VerifySourceInputs(fallbackMap, maximumBytes).Succeeded(),
        "equal table bytes in a newly winning game archive invalidate the captured source identity");

    const auto plan = Take(TerrainSavePlan::PrepareForSave(*fixture.archive, *fixture.edits, {}));
    Check(plan.SaveGate()); auto frozen = fixture.edits->Replacements();
    Check(plan.InstallPathingOutput(*fixture.edits, frozen));
    Check(plan.VerifyForSave(*fixture.edits, fixture.archive->SourceSha256(), frozen));
    Replace(fixture, 0, "Cl03");
    Require(!plan.VerifyForSave(*fixture.edits, fixture.archive->SourceSha256(), frozen).Succeeded(),
        "a late palette edit invalidates the already prepared save revision");
}

void DirectoryAliasesAndChangedSourceAreRejected() {
    Fixture fixture; Prepare(fixture); Replace(fixture, 0, "Cl02");
    const auto before = *Take(ParseMapTerrain(fixture.members[0].data)).Snapshot();
    const auto root = fixture.directory / "workspace";
    for (const auto& member : fixture.members) {
        const auto path = root / std::filesystem::path(member.name);
        std::filesystem::create_directories(path.parent_path());
        std::ofstream stream(path, std::ios::binary);
        stream.write(reinterpret_cast<const char*>(member.data.data()), static_cast<std::streamsize>(member.data.size()));
        Require(static_cast<bool>(stream), "write private synthetic directory member");
    }
    const auto directory = Take(DirectoryWorkspace::Open(root));
    const auto owner = Take(TerrainPathingResources::CaptureNative(directory.Snapshot(), before,
        *fixture.edits->Terrain(), {}, maximumBytes));
    Check(owner.VerifySourceInputs(directory.Snapshot(), maximumBytes));
    const auto alias = root / "map/TerrainArt/CliffTypes.slk";
    std::filesystem::create_directories(alias.parent_path());
    const auto table = CompleteRows();
    {
        std::ofstream stream(alias, std::ios::binary);
        stream.write(reinterpret_cast<const char*>(table.data()), static_cast<std::streamsize>(table.size()));
        Require(static_cast<bool>(stream), "write private canonical table alias");
    }
    const auto aliased = Take(DirectoryWorkspace::Open(root));
    const auto refused = TerrainPathingResources::CaptureNative(aliased.Snapshot(), before,
        *fixture.edits->Terrain(), {}, maximumBytes);
    Require(!refused.Succeeded() && HasCode({refused.diagnostics}, "TERRAIN_PATHING_RESOURCE_AMBIGUOUS"),
        "two map and root directory aliases remain ambiguous even when table bytes are equal");
    Require(!owner.VerifySourceInputs(aliased.Snapshot(), maximumBytes).Succeeded(),
        "an existing owner cannot accept a newly ambiguous source directory");
}

void PaletteCountAndOriginalWpmRemainProtected() {
    Fixture fixture; Prepare(fixture);
    const auto before = *fixture.edits->Terrain();
    for (const auto count : {0u, 16u}) {
        auto after = before; after.header.cliffTilesets.resize(count, "Cl02");
        const auto owner = TerrainPathingResources::CaptureNative(*fixture.archive, before, after, {}, maximumBytes);
        Require(!owner.Succeeded() && HasCode({owner.diagnostics}, "TERRAIN_NATIVE_SOURCE_SHAPE"),
            "empty cliff tables and sentinel-slot overflow remain outside this save domain");
    }
    {
        Fixture tail; Prepare(tail);
        const auto original = tail.members[0].data;
        auto expected = original; expected[9] = std::byte{1}; expected[21] = std::byte{2};
        const Bytes appended{std::byte{'C'}, std::byte{'l'}, std::byte{'0'}, std::byte{'2'}};
        expected.insert(expected.begin() + 29, appended.begin(), appended.end());
        Replace(tail, 1, "Cl02");
        Require(Take(SerializeMapTerrain(*tail.edits->Terrain())) == expected,
            "tail append updates literal count/custom fields and inserts one rawcode without touching vertices");
        tail.Save("palette-tail-appended.w3x");
        Require(Take(tail.archive->ReadMember("war3map.w3e")) == expected, "tail append reaches the ordinary save output");
        RequireOtherMembersPreserved(tail);
        Require(tail.edits->Undo(), "saved cliff tail append remains undoable"); tail.Save("palette-tail-retracted.w3x");
        Require(Take(tail.archive->ReadMember("war3map.w3e")) == original,
            "saved Undo retracts only the real appended tail and restores complete original bytes");
        Require(tail.edits->Redo(), "tail withdrawal preserves Redo"); tail.Save("palette-tail-restored.w3x");
        Require(Take(tail.archive->ReadMember("war3map.w3e")) == expected, "saved Redo restores exact appended bytes");
    }
    {
        Fixture middle; Prepare(middle);
        auto original = *middle.edits->Terrain(); original.header.cliffTilesets = {"Cl01", "Cl02", "Cl03"};
        middle.members[0].data = Take(SerializeMapTerrain(original)); Reopen(middle, "three-cliff-source.w3x");
        auto after = original; after.header.cliffTilesets = {"Cl01", "Cl03"};
        const auto removed = TerrainPathingResources::CaptureNative(*middle.archive, original, after, {}, maximumBytes);
        Require(!removed.Succeeded() && HasCode({removed.diagnostics}, "TERRAIN_NATIVE_SOURCE_SHAPE"),
            "middle deletion cannot masquerade as an Undo tail withdrawal");
        after.header.cliffTilesets = {"Cl02", "Cl03", "Cl04", "Cl01"};
        const auto reordered = TerrainPathingResources::CaptureNative(*middle.archive, original, after, {}, maximumBytes);
        Require(!reordered.Succeeded() && HasCode({reordered.diagnostics}, "TERRAIN_NATIVE_SOURCE_SHAPE"),
            "count-changing reordering cannot masquerade as a tail append");
    }
    {
        auto flat = before;
        for (auto& vertex : flat.vertices) { vertex.cliffAndLayerRaw = 0xf2; vertex.variationRaw = 0; }
        auto selected = flat; selected.header.cliffTilesets.push_back("Cl02");
        constexpr std::size_t index = 4 * 11 + 5;
        selected.vertices[index].cliffAndLayerRaw = 0x12;
        Require(IsNativeTerrainCliffSentinelTransition(flat, selected, index)
            && IsNativeTerrainCliffSentinelTransition(selected, flat, index),
            "flat sentinel selection accepts a newly appended concrete material in either saved direction");
        auto dangling = flat; dangling.vertices[index + 1].cliffAndLayerRaw = 0x12;
        Require(!IsNativeTerrainCliffSentinelTransition(selected, dangling, index),
            "tail withdrawal rejects a neighbor still referencing a removed slot using its own table length");
        auto diagonal = flat; diagonal.vertices[index + 12].cliffAndLayerRaw = 0xf3;
        Require(!IsNativeTerrainCliffSentinelTransition(diagonal, selected, index),
            "appended resources do not weaken the full nine-point flat-sentinel condition");
    }
    fixture.members[1].data[16] ^= std::byte{0x04}; Reopen(fixture, "unexplained-wpm.w3x");
    Replace(fixture, 0, "Cl02");
    const auto draft = Take(SerializeMapTerrain(*fixture.edits->Terrain()));
    const auto revision = fixture.edits->Revision();
    const auto plan = Take(TerrainSavePlan::PrepareForSave(*fixture.archive, *fixture.edits, {}));
    Require(HasCode(plan.SaveGate(), "TERRAIN_PATHING_SOURCE_UNEXPLAINED"),
        "palette replacement cannot erase an unexplained original WPM bit");
    Require(fixture.edits->Revision() == revision && fixture.edits->IsDirty()
        && Take(SerializeMapTerrain(*fixture.edits->Terrain())) == draft,
        "original-WPM refusal preserves revision and the full palette draft");

    Fixture changedVariation; Prepare(changedVariation); Replace(changedVariation, 0, "Cl02");
    auto raw = Take(SerializeMapTerrain(*changedVariation.edits->Terrain()));
    raw[45 + 5] |= std::byte{0xe0}; Check(changedVariation.edits->SetTerrain(raw));
    const auto unsupported = Take(TerrainSavePlan::PrepareForSave(*changedVariation.archive, *changedVariation.edits, {}));
    Require(!unsupported.SaveGate().Succeeded(),
        "palette support cannot authorize a new unsupported cliff variation hidden in the same draft");
}

void ChangedPaletteRejectsDuplicateIdsWithoutCleaningHistoricalTables() {
    Fixture fixture; Prepare(fixture, true);
    const auto before = *fixture.edits->Terrain();
    auto duplicate = before; duplicate.header.cliffTilesets[1] = "Cl01";
    Check(fixture.edits->SetTerrain(Take(SerializeMapTerrain(duplicate))));
    const auto owner = TerrainPathingResources::CaptureNative(*fixture.archive, before,
        *fixture.edits->Terrain(), {}, maximumBytes);
    Require(!owner.Succeeded() && HasCode({owner.diagnostics}, "TERRAIN_NATIVE_CLIFF_PALETTE_AMBIGUOUS"),
        "raw project input cannot bypass the UI duplicate-palette refusal");
    const auto plan = Take(TerrainSavePlan::PrepareForSave(*fixture.archive, *fixture.edits, {}));
    Require(HasCode(plan.SaveGate(), "TERRAIN_NATIVE_CLIFF_PALETTE_AMBIGUOUS"),
        "normal save explicitly rejects a newly duplicated cliff palette");

    fixture.members[0].data = Take(SerializeMapTerrain(duplicate)); Reopen(fixture, "historical-duplicate.w3x");
    const auto historical = *fixture.edits->Terrain();
    const auto unchanged = Take(TerrainPathingResources::CaptureNative(*fixture.archive, historical,
        historical, {}, maximumBytes));
    const Bytes expectedNative(40 * 32, std::byte{0x40});
    Require(Take(unchanged.GenerateNative(historical, maximumBytes)) == expectedNative
        && Take(SerializeMapTerrain(*fixture.edits->Terrain())) == fixture.members[0].data,
        "unchanged historical duplicate slots stay byte-identical and retain the previous native domain");
}

void ExistingCustomPaletteFreezesPlacementAndObjectOutputs() {
    UnitPlacements units;
    UnitPlacement unit; unit.rawcode = "u001"; unit.creationId = 1; unit.scale = {1, 1, 1};
    units.units.push_back(unit);
    const auto sourceUnits = Take(units.Serialize());
    units.units[0].rotation = 0.75f;
    const auto changedUnits = Take(units.Serialize());
    Bytes sourceAbility(12, std::byte{0}); sourceAbility[0] = std::byte{2};
    // 完整经典v2能力物编：原表一条A001、零修改，自定义表为空。
    auto changedAbility = sourceAbility; changedAbility[4] = std::byte{1};
    auto abilityRecord = Text("A001"); abilityRecord.resize(12, std::byte{0});
    changedAbility.insert(changedAbility.begin() + 8, abilityRecord.begin(), abilityRecord.end());
    for (const bool sourceMembers : {false, true}) {
        Fixture fixture; Prepare(fixture);
        auto grid = *fixture.edits->Terrain(); grid.header.usesCustomTileset = true;
        fixture.members[0].data = Take(SerializeMapTerrain(grid));
        if (sourceMembers) {
            fixture.members[4].data = sourceUnits;
            fixture.members.push_back({"war3map.w3a", sourceAbility});
        } else {
            // 保持共享夹具的TGA固定索引，但实际来源确实没有Units成员。
            fixture.members[4] = {"without-units.bin", Text("preserved missing-units fixture member")};
        }
        Reopen(fixture, "already-custom-palette-source.w3x");
        Replace(fixture, 0, "Cl02");
        Require(fixture.edits->Terrain()->header.usesCustomTileset
            && Take(SerializeMapTerrain(*fixture.edits->Terrain())) == ExpectedHeaderChange(fixture.members[0].data, 0, "Cl02"),
            "the source custom-table flag stays set while only a cliff rawcode changes");
        const auto plan = Take(TerrainSavePlan::PrepareForSave(*fixture.archive, *fixture.edits, {}));
        Check(plan.SaveGate()); Require(plan.RebuiltPathing(), "existing custom palettes still take complete native G0/G1");
        auto output = fixture.edits->Replacements(); Check(plan.InstallPathingOutput(*fixture.edits, output));
        Check(plan.VerifyForSave(*fixture.edits, fixture.archive->SourceSha256(), output));
        for (const auto& late : {MemberReplacement{"war3mapUnits.doo", changedUnits},
            MemberReplacement{"war3map.w3a", changedAbility}}) {
            auto tampered = output; tampered.push_back(late);
            Require(HasCode(plan.VerifyForSave(*fixture.edits, fixture.archive->SourceSha256(), tampered),
                "TERRAIN_SAVE_PLAN_MEMBER_MISMATCH"),
                "a prepared custom-palette save rejects late added or changed Units and unrelated ability data");
            const auto path = fixture.directory / (late.name == "war3mapUnits.doo" ? "late-units.w3x" : "late-ability.w3x");
            const auto readback = Take(MapArchiveSession::Open(Take(fixture.archive->SaveCandidate(path, tampered)).path));
            Require(!plan.VerifyReadback(readback).Succeeded(),
                "independent readback rejects the same added or changed placement and object outputs");
        }
        fixture.Save("already-custom-palette.w3x");
        RequireOtherMembersPreserved(fixture);
        if (sourceMembers) {
            // 正式编辑先于计划冻结，仍由原来的私有Units证明核对完整新旧成员。
            Check(fixture.edits->EditUnits(units)); Replace(fixture, 0, "Cl03");
            fixture.Save("already-custom-palette-with-units.w3x");
            Require(Take(fixture.archive->ReadMember("war3mapUnits.doo")) == changedUnits,
                "a legitimate pre-prepare Units edit still saves through the existing typed proof");
        }
    }
}
} // namespace

int main() {
    try {
        PaletteSavePreservesAllVertexBytesAndUndoRedo();
        RawProjectRecoveryRetainsPaletteAndOrdinarySave();
        LastConcreteSlotIsWritableWithoutUsingSentinel();
        EveryChangedSlotNeedsBothActualRows();
        ResourceIdentityBudgetAndFrozenOutputsStayStrict();
        DirectoryAliasesAndChangedSourceAreRejected();
        PaletteCountAndOriginalWpmRemainProtected();
        ChangedPaletteRejectsDuplicateIdsWithoutCleaningHistoricalTables();
        ExistingCustomPaletteFreezesPlacementAndObjectOutputs();
        std::cout << "terrain cliff palette native save tests passed\n"; return 0;
    } catch (const std::exception& exception) { std::cerr << exception.what() << '\n'; return 1; }
}
