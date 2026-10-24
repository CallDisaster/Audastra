#pragma once

// 在TerrainOrdinaryJournalSaveCases.h后包含，复用原创特殊水刷及真实普通占地。
std::string GroundAppendRawcode(unsigned index) {
    return "Tg" + std::string(index < 10 ? "0" : "") + std::to_string(index);
}
Bytes GroundAppendTable(bool secondWalkable = false) {
    std::string table = "ID;PWXL;N;E\nC;X1;Y1;K\"tileID\"\nC;X2;Y1;K\"walkable\"\n"
        "C;X3;Y1;K\"flyable\"\nC;X4;Y1;K\"buildable\"\n";
    for (unsigned index = 1; index <= 16; ++index) {
        const auto row = std::to_string(index + 1);
        const auto walk = (index == 2 && !secondWalkable) || index == 16 ? "0" : "1";
        table += "C;X1;Y" + row + ";K\"" + GroundAppendRawcode(index) + "\"\nC;X2;Y" + row + ";K" + walk
            + "\nC;X3;Y" + row + ";K1\nC;X4;Y" + row + ";K1\n";
    }
    return Text(table + "E\n");
}
void GroundAppendSpecialFixture(Fixture& f, unsigned existing = 1) {
    OrdinarySpecialWaterFixture(f);
    Require(existing >= 1 && existing <= 16, "owned ground source has a classic nonempty table");
    f.original.header.groundTilesets.clear();
    for (unsigned index = 1; index <= existing; ++index) f.original.header.groundTilesets.push_back(GroundAppendRawcode(index));
    f.Member("war3map.w3e") = Take(SerializeMapTerrain(f.original));
    std::erase_if(f.members, [](const auto& entry) { return entry.name == "TerrainArt/Terrain.slk"; });
    // 地面表只放原创game归档，明确检测真实winner漂移而非拿地图SHA变化冒充资源检查。
    f.gameRoot = f.root / "append-owned-game";
    Require(std::filesystem::create_directory(f.gameRoot), "private append game resource directory");
    const std::vector<MapArchiveMember> resources{{"TerrainArt/Terrain.slk", GroundAppendTable()}};
    Take(WriteVerifiedArchive(f.gameRoot / "war3.mpq", resources)); f.Open();
}

void GroundAppendJournalRestoresBothWaterOrdersAndFormalSave() {
    for (const bool appendFirst : {false, true}) {
        Fixture f; GroundAppendSpecialFixture(f);
        const auto append = [&] {
            const auto changed = Take(TerrainPathingSave::EditTextureSlot(*f.archive, *f.edits,
                TerrainTextureTable::Ground, 1, "Tg02", f.gameRoot));
            Require(changed.headerChanged, "captured ground append forms a real header command");
        };
        if (appendFirst) append();
        PaintBridgeSpecialWater(f);
        if (!appendFirst) append();
        const auto terrain = Take(SerializeMapTerrain(*f.edits->Terrain()));
        const auto doodads = Take(OrdinaryCurrent(f).Serialize());
        auto raw = OrdinaryJournalRawDraft(f);
        const auto rawPlan = Take(TerrainPathingSave::PrepareForSave(*f.archive, *raw, f.gameRoot));
        Require(!rawPlan.SaveGate().Succeeded() && HasCode(rawPlan.SaveGate(), "TERRAIN_SPECIAL_PROVENANCE_REQUIRED"),
            "raw appended header plus deleted special tail cannot claim the hot command's provenance");
        const auto journal = Take(TerrainPathingSave::CaptureJournal(*f.archive, *f.edits));
        const auto wire = Take(EncodeTerrainStrokeJournal(journal)); const auto decoded = Take(DecodeTerrainStrokeJournal(wire));
        Require(decoded.strokes.size() == 2 && decoded.strokes[appendFirst ? 0 : 1].intent.palette,
            "both real operation orders recover one explicit palette command");
        const auto& palette = *decoded.strokes[appendFirst ? 0 : 1].intent.palette;
        Require(wire[3] == std::byte{'6'} && palette.algorithm == 2
            && palette.table == TerrainTextureTable::Ground && palette.slot == 1 && palette.expectedRawcode.empty(),
            "both real operation orders persist ground append as explicit FTJ6 algorithm2");
        auto restored = Take(TerrainPathingSave::RestoreJournal(*f.archive, MapDocument::FromMembers(f.members),
            f.Member("war3map.w3e"), f.Member("war3map.wpm"), decoded, *raw, f.gameRoot));
        Require(restored.receipts.size() == 2 && restored.waterOwners.size() == 2
            && restored.session->MatchesTerrainJournalDraft(*raw), "source-bound restore recreates both owners and every expected byte");
        const auto scope = restored.session->TerrainJournalStateFingerprint();
        for (auto i = restored.receipts.rbegin(); i != restored.receipts.rend(); ++i) Check(restored.session->Replay(*i, false));
        Require(!restored.session->IsDirty() && restored.session->Terrain()->header.groundTilesets.size() == 1,
            "both cold-restored commands undo to the complete original ground table and special set");
        for (const auto& receipt : restored.receipts) Check(restored.session->Replay(receipt, true));
        Require(restored.session->MatchesTerrainJournalDraft(*raw) && restored.session->TerrainJournalStateFingerprint() == scope,
            "cold redo restores the exact table, source scope and private water lineage");
        f.edits = std::move(restored.session);
        f.SaveExpected("ground-append-water.w3x", terrain, OrdinarySpecialExpectedPathing(f, true, false), &doodads, true);
        Require(Take(f.edits->ExportTerrainStrokeJournal()).strokes.empty(), "verified formal save rebases the recovered append journal");
    }
}

void GroundAppendCapturesTheFullResourceAndBeforeTable() {
    Fixture f; GroundAppendSpecialFixture(f);
    const auto before = Take(SerializeMapTerrain(*f.edits->Terrain()));
    const auto revision = f.edits->Revision();
    Require(!TerrainPathingSave::EditTextureSlot(*f.archive, *f.edits, TerrainTextureTable::Ground, 2, "Tg02", f.gameRoot).Succeeded()
        && !TerrainPathingSave::EditTextureSlot(*f.archive, *f.edits, TerrainTextureTable::Ground, 1, "Tg01", f.gameRoot).Succeeded()
        && !TerrainPathingSave::EditTextureSlot(*f.archive, *f.edits, TerrainTextureTable::Cliff, 1, "Cl02", f.gameRoot).Succeeded()
        && f.edits->Revision() == revision && Take(SerializeMapTerrain(*f.edits->Terrain())) == before,
        "sparse, duplicate and cliff append requests preserve the exact active source and history");
    const auto pending = Take(TerrainPathingSave::PrepareTextureSlot(*f.archive, *f.edits,
        TerrainTextureTable::Ground, 1, "Tg02", f.gameRoot));
    Require(f.edits->Revision() == revision, "preparation freezes real after-table resources without publishing the append");
    Take(TerrainPathingSave::EditTextureSlot(*f.archive, *f.edits, TerrainTextureTable::Ground, 1, "Tg03", f.gameRoot));
    const auto alternate = Take(SerializeMapTerrain(*f.edits->Terrain()));
    Require(!f.edits->ApplyPreparedTerrainEdit(pending).Succeeded()
        && Take(SerializeMapTerrain(*f.edits->Terrain())) == alternate, "a prepared append never overwrites a changed full before-table");
    Require(f.edits->Undo(), "return to source before testing actual-resource recovery");
    Take(TerrainPathingSave::EditTextureSlot(*f.archive, *f.edits, TerrainTextureTable::Ground, 1, "Tg02", f.gameRoot));
    PaintBridgeSpecialWater(f); auto raw = OrdinaryJournalRawDraft(f);
    const auto journal = Take(TerrainPathingSave::CaptureJournal(*f.archive, *f.edits));
    Require(journal.strokes.size() == 2 && journal.strokes.front().intent.palette,
        "resource drift checks target the real initial appended palette record");
    const auto restore = [&](const TerrainStrokeJournal& value, const std::filesystem::path& game) {
        return TerrainPathingSave::RestoreJournal(*f.archive, MapDocument::FromMembers(f.members),
            f.Member("war3map.w3e"), f.Member("war3map.wpm"), value, *raw, game);
    };
    auto invalid = journal; invalid.strokes.front().intent.palette->slot = 2;
    const auto driftedBefore = restore(invalid, f.gameRoot);
    Require(!driftedBefore.Succeeded() && !driftedBefore.value && HasCode(driftedBefore, "TERRAIN_JOURNAL_PALETTE_DRIFT"),
        "correct source SHA cannot bypass the exact full before-table length");
    invalid = journal; invalid.strokes.front().intent.palette->replacementRawcode = "Tg03";
    const auto changedAfter = restore(invalid, f.gameRoot);
    Require(!changedAfter.Succeeded() && !changedAfter.value && HasCode(changedAfter, "TERRAIN_JOURNAL_RESOURCE_DRIFT"),
        "different after table is rejected by the full prospective W3E dependency SHA before publication");
    const auto gameDrift = f.root / "append-game-drift";
    Require(std::filesystem::create_directory(gameDrift), "owned resource drift directory");
    const std::vector<MapArchiveMember> changedResources{{"TerrainArt/Terrain.slk", GroundAppendTable(true)}};
    Take(WriteVerifiedArchive(gameDrift / "war3.mpq", changedResources));
    const auto drift = restore(journal, gameDrift);
    Require(!drift.Succeeded() && !drift.value && HasCode(drift, "TERRAIN_JOURNAL_RESOURCE_DRIFT"),
        "changing the actual game table winner is detected even for an unused appended ground material");
    Require(f.edits->Terrain()->header.groundTilesets == std::vector<std::string>{"Tg01", "Tg02"}
        && OrdinaryCurrent(f).specialDoodads.size() == 2, "all declined candidates preserve the complete live source-bound session");
}

void GroundAppendsRetainOrderedIntermediateTablesAndWaterLineage() {
    for (const bool waterBetween : {false, true}) {
        Fixture f; GroundAppendSpecialFixture(f);
        std::vector<Bytes> terrains{f.Member("war3map.w3e")}, placements{f.Member("war3map.doo")};
        const auto remember = [&] {
            terrains.push_back(Take(SerializeMapTerrain(*f.edits->Terrain())));
            placements.push_back(Take(OrdinaryCurrent(f).Serialize()));
        };
        const auto append = [&](std::uint32_t slot, const char* rawcode) {
            const auto changed = Take(TerrainPathingSave::EditTextureSlot(*f.archive, *f.edits,
                TerrainTextureTable::Ground, slot, rawcode, f.gameRoot));
            Require(changed.headerChanged, "each successive append freezes its own real intermediate table"); remember();
        };
        append(1, "Tg02");
        if (waterBetween) { PaintBridgeSpecialWater(f); remember(); }
        append(2, "Tg03");
        if (!waterBetween) { PaintBridgeSpecialWater(f); remember(); }
        auto raw = OrdinaryJournalRawDraft(f);
        const auto journal = Take(DecodeTerrainStrokeJournal(Take(EncodeTerrainStrokeJournal(
            Take(TerrainPathingSave::CaptureJournal(*f.archive, *f.edits))))));
        const auto next = waterBetween ? 2u : 1u;
        Require(journal.strokes.size() == 3 && journal.strokes[0].intent.palette
            && journal.strokes[next].intent.palette && journal.strokes[waterBetween ? 1 : 2].intent.waterBrush
            && journal.strokes[0].intent.palette->slot == 1 && journal.strokes[next].intent.palette->slot == 2
            && journal.strokes[0].rulesSha256 != journal.strokes[next].rulesSha256,
            "one-to-two-to-three tables retain independent full before/after and current-DOO identities");
        auto reordered = journal; std::swap(reordered.strokes[0], reordered.strokes[next]);
        const auto refused = TerrainPathingSave::RestoreJournal(*f.archive, MapDocument::FromMembers(f.members),
            f.Member("war3map.w3e"), f.Member("war3map.wpm"), reordered, *raw, f.gameRoot);
        Require(!refused.Succeeded() && !refused.value && HasCode(refused, "TERRAIN_JOURNAL_PALETTE_DRIFT"),
            "successive appends cannot be reordered even when all final rawcodes are present");
        auto recovered = Take(TerrainPathingSave::RestoreJournal(*f.archive, MapDocument::FromMembers(f.members),
            f.Member("war3map.w3e"), f.Member("war3map.wpm"), journal, *raw, f.gameRoot));
        Require(recovered.receipts.size() == 3 && recovered.waterOwners.size() == 3
            && recovered.session->MatchesTerrainJournalDraft(*raw), "three real receipts recreate the complete ordered cold draft");
        const auto scope = recovered.session->TerrainJournalStateFingerprint();
        for (std::size_t i = 3; i > 0; --i) {
            Check(recovered.session->Replay(recovered.receipts[i - 1], false));
            const auto document = recovered.session->Snapshot();
            Require(Take(SerializeMapTerrain(*recovered.session->Terrain())) == terrains[i - 1]
                && document.Doodads() && Take(document.Doodads()->Serialize()) == placements[i - 1],
                "each reverse step retains the exact preceding table and complete special ordering");
        }
        Require(!recovered.session->IsDirty(), "all successive append receipts reverse to untouched source");
        for (std::size_t i = 0; i < 3; ++i) {
            Check(recovered.session->Replay(recovered.receipts[i], true));
            const auto document = recovered.session->Snapshot();
            Require(Take(SerializeMapTerrain(*recovered.session->Terrain())) == terrains[i + 1]
                && document.Doodads() && Take(document.Doodads()->Serialize()) == placements[i + 1],
                "each forward step restores its own captured full table and special lineage");
        }
        Require(recovered.session->TerrainJournalStateFingerprint() == scope
            && recovered.session->MatchesTerrainJournalDraft(*raw), "successive redo preserves the exact complete source scope");
        f.edits = std::move(recovered.session);
        f.SaveExpected("successive-ground-appends-water.w3x", terrains.back(),
            OrdinarySpecialExpectedPathing(f, true, false), &placements.back(), true);
    }
}

void GroundAppendSixteenthEntryAndResourceFailuresStayBounded() {
    Fixture last; GroundAppendSpecialFixture(last, 15);
    const auto appended = Take(TerrainPathingSave::EditTextureSlot(*last.archive, *last.edits,
        TerrainTextureTable::Ground, 15, "Tg16", last.gameRoot));
    Require(appended.headerChanged && last.edits->Terrain()->header.groundTilesets.size() == 16,
        "actual captured properties permit the last encodable ground slot");
    const auto revision = last.edits->Revision();
    const auto overflow = TerrainPathingSave::EditTextureSlot(*last.archive, *last.edits,
        TerrainTextureTable::Ground, 16, "Tg17", last.gameRoot);
    Require(!overflow.Succeeded() && HasCode(overflow, "TERRAIN_PALETTE_JOURNAL_DOMAIN")
        && last.edits->Revision() == revision,
        "the seventeenth slot is rejected by the slot limit before trying to resolve its resources");
    const auto fullJournal = Take(TerrainPathingSave::CaptureJournal(*last.archive, *last.edits));
    auto fullRaw = OrdinaryJournalRawDraft(last);
    auto full = Take(TerrainPathingSave::RestoreJournal(*last.archive, MapDocument::FromMembers(last.members),
        last.Member("war3map.w3e"), last.Member("war3map.wpm"),
        Take(DecodeTerrainStrokeJournal(Take(EncodeTerrainStrokeJournal(fullJournal)))), *fullRaw, last.gameRoot));
    Require(full.session->Terrain()->header.groundTilesets.size() == 16 && full.receipts.size() == 1,
        "sixteenth-slot capture survives real-source journal recovery");
    TerrainBrush paint; paint.kind = TerrainBrushKind::Texture; paint.radius = 0;
    paint.groundTexture = "Tg16"; paint.variation = 0;
    const TerrainStrokePoint point{768, 256}; Take(last.edits->EditTerrain(paint, std::span(&point, 1), false));
    auto expected = last.original; expected.header.groundTilesets.push_back("Tg16"); expected.header.usesCustomTileset = true;
    expected.vertices[24].textureAndFlagsRaw = static_cast<std::uint8_t>((expected.vertices[24].textureAndFlagsRaw & 0xf0) | 15);
    expected.vertices[24].variationRaw &= 0xe0;
    const auto expectedTerrain = Take(SerializeMapTerrain(expected));
    Require(Take(SerializeMapTerrain(*last.edits->Terrain())) == expectedTerrain,
        "painting the appended sixteenth entry preserves high flags and uses the full four-bit ground index");
    const auto paintedJournal = Take(DecodeTerrainStrokeJournal(Take(EncodeTerrainStrokeJournal(
        Take(TerrainPathingSave::CaptureJournal(*last.archive, *last.edits))))));
    auto paintedRaw = OrdinaryJournalRawDraft(last);
    auto painted = Take(TerrainPathingSave::RestoreJournal(*last.archive, MapDocument::FromMembers(last.members),
        last.Member("war3map.w3e"), last.Member("war3map.wpm"), paintedJournal, *paintedRaw, last.gameRoot));
    Require(painted.receipts.size() == 2 && painted.waterOwners.size() == 1
        && painted.session->MatchesTerrainJournalDraft(*paintedRaw), "cold restoration keeps append and actual last-slot painting as distinct commands");
    auto expectedPathing = last.Member("war3map.wpm");
    // 单个(6,2)顶点的最近顶点区域为x22..25/y6..9，远离所有ordinary与special占地。
    for (unsigned y = 6; y < 10; ++y) for (unsigned x = 22; x < 26; ++x)
        expectedPathing[16 + y * 32 + x] = std::byte{0x53};
    last.edits = std::move(painted.session);
    last.SaveExpected("ground-sixteenth-painted.w3x", expectedTerrain, expectedPathing, nullptr, true);
    Fixture missing; GroundAppendSpecialFixture(missing);
    const auto unchanged = Take(SerializeMapTerrain(*missing.edits->Terrain()));
    const auto noProperties = TerrainPathingSave::EditTextureSlot(*missing.archive, *missing.edits,
        TerrainTextureTable::Ground, 1, "ZZZZ", missing.gameRoot);
    Require(!noProperties.Succeeded() && missing.edits->Revision() == 0
        && Take(SerializeMapTerrain(*missing.edits->Terrain())) == unchanged,
        "an unused appended slot still requires actual valid material properties before commit");
    const auto limited = TerrainPathingSave::PrepareTextureSlot(*missing.archive, *missing.edits,
        TerrainTextureTable::Ground, 1, "Tg02", missing.gameRoot, 1);
    Require(!limited.Succeeded() && missing.edits->Revision() == 0,
        "tight actual-source budget rejects before any header edit or partial recovery is published");
}
