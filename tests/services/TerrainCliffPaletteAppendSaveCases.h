#pragma once

// 在TerrainGroundPaletteAppendSaveCases.h后包含；只用原创资源与正式保存/回读链。
std::string CliffAppendRawcode(unsigned index) {
    return "Cl" + std::string(index < 10 ? "0" : "") + std::to_string(index);
}
Bytes CliffAppendResourceTable(bool changedUnusedRow = false) {
    std::string table = "ID;PWXL;N;E\nC;X1;Y1;K\"cliffID\"\nC;X2;Y1;K\"name\"\n";
    for (unsigned index = 1; index <= 15; ++index) {
        const auto row = std::to_string(index + 1);
        table += "C;X1;Y" + row + ";K\"" + CliffAppendRawcode(index) + "\"\nC;X2;Y" + row
            + ";K\"" + (changedUnusedRow && index == 2 ? "changed" : "synthetic") + "\"\n";
    }
    return Text(table + "E\n");
}
std::vector<MapArchiveMember> CliffAppendGameResources(bool changedUnusedRow = false) {
    return {{"TerrainArt/Terrain.slk", GroundAppendTable()},
        {"TerrainArt/CliffTypes.slk", CliffAppendResourceTable(changedUnusedRow)}};
}
void CliffAppendSpecialFixture(Fixture& f, unsigned existing = 1) {
    OrdinarySpecialWaterFixture(f);
    Require(existing >= 1 && existing <= 15, "owned cliff source retains a nonempty concrete table");
    f.original.header.cliffTilesets.clear();
    for (unsigned index = 1; index <= existing; ++index) f.original.header.cliffTilesets.push_back(CliffAppendRawcode(index));
    f.Member("war3map.w3e") = Take(SerializeMapTerrain(f.original));
    std::erase_if(f.members, [](const auto& entry) {
        return entry.name == "TerrainArt/Terrain.slk" || entry.name == "TerrainArt/CliffTypes.slk";
    });
    f.gameRoot = f.root / "cliff-append-owned-game";
    Require(std::filesystem::create_directory(f.gameRoot), "private cliff palette resources");
    Take(WriteVerifiedArchive(f.gameRoot / "war3.mpq", CliffAppendGameResources())); f.Open();
}

void CliffAppendJournalCombinesGroundWaterAndSavedUndo() {
    for (const bool appendFirst : {false, true}) {
        Fixture f; CliffAppendSpecialFixture(f);
        std::vector<Bytes> terrains{f.Member("war3map.w3e")}, doodads{f.Member("war3map.doo")};
        const auto remember = [&] {
            terrains.push_back(Take(SerializeMapTerrain(*f.edits->Terrain())));
            doodads.push_back(Take(OrdinaryCurrent(f).Serialize()));
        };
        const auto append = [&] {
            Require(Take(TerrainPathingSave::EditTextureSlot(*f.archive, *f.edits,
                TerrainTextureTable::Cliff, 1, "Cl02", f.gameRoot)).headerChanged,
                "captured cliff append publishes one real header command"); remember();
        };
        if (appendFirst) append();
        PaintBridgeSpecialWater(f); remember();
        if (!appendFirst) append();
        Take(TerrainPathingSave::EditTextureSlot(*f.archive, *f.edits, TerrainTextureTable::Ground, 1, "Tg02", f.gameRoot)); remember();
        const auto journal = Take(DecodeTerrainStrokeJournal(Take(EncodeTerrainStrokeJournal(
            Take(TerrainPathingSave::CaptureJournal(*f.archive, *f.edits))))));
        const auto& cliff = journal.strokes[appendFirst ? 0 : 1].intent.palette;
        Require(journal.strokes.size() == 3 && cliff && cliff->algorithm == 3
            && cliff->table == TerrainTextureTable::Cliff && cliff->slot == 1 && cliff->expectedRawcode.empty()
            && journal.strokes[2].intent.palette->algorithm == 2,
            "mixed recovery retains distinct cliff3 and ground2 payloads around the real water command");
        auto raw = OrdinaryJournalRawDraft(f);
        const auto rawPlan = Take(TerrainPathingSave::PrepareForSave(*f.archive, *raw, f.gameRoot));
        Require(!rawPlan.SaveGate().Succeeded() && HasCode(rawPlan.SaveGate(), "TERRAIN_SPECIAL_PROVENANCE_REQUIRED"),
            "raw appended headers cannot borrow special-removal provenance from the live session");
        auto recovered = Take(TerrainPathingSave::RestoreJournal(*f.archive, MapDocument::FromMembers(f.members),
            f.Member("war3map.w3e"), f.Member("war3map.wpm"), journal, *raw, f.gameRoot));
        Require(recovered.receipts.size() == 3 && recovered.waterOwners.size() == 3
            && recovered.session->MatchesTerrainJournalDraft(*raw), "cold source-bound replay recreates all owners and exact draft bytes");
        const auto scope = recovered.session->TerrainJournalStateFingerprint();
        for (std::size_t step = 3; step > 0; --step) {
            Check(recovered.session->Replay(recovered.receipts[step - 1], false));
            Require(Take(SerializeMapTerrain(*recovered.session->Terrain())) == terrains[step - 1]
                && Take(recovered.session->Snapshot().Doodads()->Serialize()) == doodads[step - 1],
                "each reverse receipt restores its complete preceding ground/cliff table and special sequence");
        }
        for (const auto& receipt : recovered.receipts) Check(recovered.session->Replay(receipt, true));
        Require(recovered.session->TerrainJournalStateFingerprint() == scope
            && recovered.session->MatchesTerrainJournalDraft(*raw), "mixed redo restores exact complete resource scope");
        f.edits = std::move(recovered.session);
        f.SaveExpected("cliff-ground-water-final.w3x", terrains[3], OrdinarySpecialExpectedPathing(f, true, false), &doodads[3], true);
        // 第三步只追加未被顶点引用的地面槽；保存后的撤销/重做可凭实际表保留WPM。
        // 悬崖槽和水刷仍必须各自通过完整原版G0/G1重建。
        for (unsigned step = 3; step > 0; --step) {
            Require(f.edits->Undo(), "each header/water command remains undoable after native save");
            const bool water = step - 1 >= (appendFirst ? 2u : 1u);
            const auto name = "cliff-ground-water-undo-" + std::to_string(step) + ".w3x";
            f.SaveExpected(name.c_str(), terrains[step - 1], OrdinarySpecialExpectedPathing(f, water, false), &doodads[step - 1], true, step != 3);
        }
        for (unsigned step = 1; step <= 3; ++step) {
            Require(f.edits->Redo(), "saved tail retraction keeps exact redo rather than arbitrary table deletion");
            const bool water = step >= (appendFirst ? 2u : 1u);
            const auto name = "cliff-ground-water-redo-" + std::to_string(step) + ".w3x";
            f.SaveExpected(name.c_str(), terrains[step], OrdinarySpecialExpectedPathing(f, water, false), &doodads[step], true, step != 3);
        }
    }
}

void CliffAppendCapturesUnusedResourcesAndFullBeforeTable() {
    Fixture f; CliffAppendSpecialFixture(f);
    const auto original = Take(SerializeMapTerrain(*f.edits->Terrain()));
    for (const auto [slot, rawcode] : {std::pair{2u, "Cl02"}, {1u, "Cl01"}, {1u, "ZZZZ"}})
        Require(!TerrainPathingSave::EditTextureSlot(*f.archive, *f.edits, TerrainTextureTable::Cliff, slot, rawcode, f.gameRoot).Succeeded()
            && f.edits->Revision() == 0 && Take(SerializeMapTerrain(*f.edits->Terrain())) == original,
            "sparse, duplicate and missing actual cliff rows fail before publishing a command");
    Require(!TerrainPathingSave::PrepareTextureSlot(*f.archive, *f.edits,
        TerrainTextureTable::Cliff, 1, "Cl02", f.gameRoot, 1).Succeeded() && f.edits->Revision() == 0,
        "tiny resource budget cannot publish partial cliff state");
    const auto prepared = Take(TerrainPathingSave::PrepareTextureSlot(*f.archive, *f.edits,
        TerrainTextureTable::Cliff, 1, "Cl02", f.gameRoot));
    Take(TerrainPathingSave::EditTextureSlot(*f.archive, *f.edits, TerrainTextureTable::Ground, 1, "Tg03", f.gameRoot));
    const auto differentTable = Take(SerializeMapTerrain(*f.edits->Terrain()));
    Require(!f.edits->ApplyPreparedTerrainEdit(prepared).Succeeded()
        && Take(SerializeMapTerrain(*f.edits->Terrain())) == differentTable, "changing either full table invalidates the captured before revision");
    Require(f.edits->Undo(), "restore original table before binding unused cliff row");
    Take(TerrainPathingSave::EditTextureSlot(*f.archive, *f.edits, TerrainTextureTable::Cliff, 1, "Cl02", f.gameRoot));
    const auto journal = Take(TerrainPathingSave::CaptureJournal(*f.archive, *f.edits));
    auto raw = OrdinaryJournalRawDraft(f);
    const auto restore = [&](const TerrainStrokeJournal& value, const std::filesystem::path& game) {
        return TerrainPathingSave::RestoreJournal(*f.archive, MapDocument::FromMembers(f.members),
            f.Member("war3map.w3e"), f.Member("war3map.wpm"), value, *raw, game);
    };
    auto changed = journal; changed.strokes[0].intent.palette->slot = 2;
    const auto length = restore(changed, f.gameRoot);
    Require(!length.Succeeded() && !length.value && HasCode(length, "TERRAIN_JOURNAL_PALETTE_DRIFT"),
        "correct source SHA cannot replace the exact full before-table length");
    changed = journal; changed.strokes[0].intent.palette->replacementRawcode = "Cl03";
    const auto prospective = restore(changed, f.gameRoot);
    Require(!prospective.Succeeded() && !prospective.value && HasCode(prospective, "TERRAIN_JOURNAL_RESOURCE_DRIFT"),
        "full prospective W3E identity binds the actual requested appended rawcode");
    const auto game = f.root / "cliff-append-resource-drift";
    Require(std::filesystem::create_directory(game), "private changed cliff resource root");
    Take(WriteVerifiedArchive(game / "war3.mpq", CliffAppendGameResources(true)));
    const auto drift = restore(journal, game);
    Require(!drift.Succeeded() && !drift.value && HasCode(drift, "TERRAIN_JOURNAL_RESOURCE_DRIFT"),
        "unused appended cliff row remains bound to the complete actual winning table bytes");
    Services::TerrainReplayLimits limits; limits.maximumWork = 1;
    const auto limited = TerrainPathingSave::RestoreJournal(*f.archive, MapDocument::FromMembers(f.members),
        f.Member("war3map.w3e"), f.Member("war3map.wpm"), journal, *raw, f.gameRoot, limits);
    Require(!limited.Succeeded() && !limited.value, "bounded recovery returns no partial appended session");
}

void CliffAppendFifteenthEntryCanBePaintedAndFormallySaved() {
    Fixture f; CliffAppendSpecialFixture(f, 14);
    // 来源中的平坦中心尚未选择悬崖材质；追加后要真正使用新槽，
    // 正式计划会从完整旧14项/新15项表验证这次15哨兵过渡。
    f.original.vertices[24].cliffAndLayerRaw = 0xf2;
    f.Member("war3map.w3e") = Take(SerializeMapTerrain(f.original)); f.Open();
    Take(TerrainPathingSave::EditTextureSlot(*f.archive, *f.edits, TerrainTextureTable::Cliff, 14, "Cl15", f.gameRoot));
    const auto appended = Take(SerializeMapTerrain(*f.edits->Terrain()));
    const auto revision = f.edits->Revision();
    const auto overflow = TerrainPathingSave::EditTextureSlot(*f.archive, *f.edits, TerrainTextureTable::Cliff, 15, "Cl16", f.gameRoot);
    Require(!overflow.Succeeded() && HasCode(overflow, "TERRAIN_PALETTE_JOURNAL_DOMAIN")
        && f.edits->Revision() == revision && f.edits->Terrain()->header.cliffTilesets.size() == 15,
        "reserved no-cliff index fifteen cannot become a sixteenth concrete material");
    TerrainBrush paint; paint.kind = TerrainBrushKind::CliffTexture; paint.radius = 0; paint.cliffTexture = 14; paint.variation = 0;
    const TerrainStrokePoint point{768, 256}; Take(f.edits->EditTerrain(paint, std::span(&point, 1), false));
    auto expected = f.original; expected.header.cliffTilesets.push_back("Cl15"); expected.header.usesCustomTileset = true;
    expected.vertices[24].cliffAndLayerRaw = static_cast<std::uint8_t>((expected.vertices[24].cliffAndLayerRaw & 15) | 0xe0);
    expected.vertices[24].variationRaw &= 0x1f;
    const auto terrain = Take(SerializeMapTerrain(expected));
    Require(Take(SerializeMapTerrain(*f.edits->Terrain())) == terrain,
        "last concrete cliff paint changes only the independent selected vertex high nibble");
    const auto journal = Take(DecodeTerrainStrokeJournal(Take(EncodeTerrainStrokeJournal(
        Take(TerrainPathingSave::CaptureJournal(*f.archive, *f.edits))))));
    auto raw = OrdinaryJournalRawDraft(f);
    auto recovered = Take(TerrainPathingSave::RestoreJournal(*f.archive, MapDocument::FromMembers(f.members),
        f.Member("war3map.w3e"), f.Member("war3map.wpm"), journal, *raw, f.gameRoot));
    Require(recovered.receipts.size() == 2 && recovered.session->MatchesTerrainJournalDraft(*raw),
        "last-slot append and actual painting survive source-bound cold replay");
    f.edits = std::move(recovered.session);
    f.SaveExpected("cliff-fifteenth-painted.w3x", terrain, f.Member("war3map.wpm"), nullptr, true);
    Require(f.edits->Undo(), "saved cliff paint remains undoable");
    f.SaveExpected("cliff-fifteenth-unpainted.w3x", appended, f.Member("war3map.wpm"), nullptr, true);
    Require(f.edits->Undo(), "unused final slot can be retracted through its real Undo receipt");
    f.SaveExpected("cliff-fifteenth-retracted.w3x", f.Member("war3map.w3e"), f.Member("war3map.wpm"), nullptr, true);
}
