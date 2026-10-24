#pragma once

// 在TerrainGroundPaletteAppendCases.h后包含，共用原创SessionFixture与严格FTJ6检查。
void CliffPaletteAppendUsesDistinctFTJ6AlgorithmAndBounds() {
    auto journal = Journal(); TerrainJournalStroke append;
    append.intent.palette = TerrainPaletteEditIntent{TerrainTextureTable::Cliff, 1, "", "Cl02", 3};
    append.rulesSha256.assign(64, 'd'); journal.strokes = {append};
    const auto wire = Take(EncodeTerrainStrokeJournal(journal));
    // 独立字面正文：variant2、algorithm3、Cliff1、旧长度1、空旧rawcode、新值与digest长度。
    const Bytes prefix{std::byte{2}, std::byte{3}, std::byte{1},
        std::byte{1}, std::byte{0}, std::byte{0}, std::byte{0},
        std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0},
        std::byte{4}, std::byte{0}, std::byte{0}, std::byte{0},
        std::byte{'C'}, std::byte{'l'}, std::byte{'0'}, std::byte{'2'},
        std::byte{64}, std::byte{0}, std::byte{0}, std::byte{0}};
    Require(wire[3] == std::byte{'6'} && wire.size() > prefix.size() + 64
        && std::equal(prefix.begin(), prefix.end(), wire.end() - prefix.size() - 64),
        "cliff append preserves FTJ6 wire layout with its own explicit algorithm");
    const auto decoded = Take(DecodeTerrainStrokeJournal(wire));
    Require(decoded.strokes.size() == 1 && decoded.strokes[0].intent.palette
        && decoded.strokes[0].intent.palette->algorithm == 3
        && decoded.strokes[0].intent.palette->expectedRawcode.empty()
        && Take(EncodeTerrainStrokeJournal(decoded)) == wire, "cliff append has complete canonical recovery");
    for (std::size_t size = 0; size < wire.size(); ++size)
        Require(!DecodeTerrainStrokeJournal(std::span(wire).first(size)).Succeeded(), "every cliff append truncation is rejected");
    const auto offset = wire.size() - prefix.size() - 64;
    for (const auto [delta, value] : {std::pair{1u, 1u}, {1u, 2u}, {1u, 4u}, {2u, 0u}, {3u, 0u}, {3u, 15u}}) {
        auto invalid = wire; invalid[offset + delta] = std::byte(value);
        Require(!DecodeTerrainStrokeJournal(invalid).Succeeded(),
            "cliff append cannot acquire legacy replace, ground append, unknown algorithm or sentinel index semantics");
    }
    auto invalid = journal; invalid.strokes[0].intent.palette->expectedRawcode = "Cl01";
    Require(!EncodeTerrainStrokeJournal(invalid).Succeeded(), "cliff append cannot disguise a replacement");
    invalid = journal; invalid.strokes[0].intent.points = {{512, 512}};
    Require(!EncodeTerrainStrokeJournal(invalid).Succeeded(), "cliff append cannot hide brush points");
    invalid = journal; invalid.strokes[0].rulesSha256.clear();
    Require(!EncodeTerrainStrokeJournal(invalid).Succeeded(), "cliff append requires complete actual resource identity");
    journal.strokes[0].intent.palette->slot = 14;
    Require(EncodeTerrainStrokeJournal(journal).Succeeded(), "fourteen entries may append the fifteenth concrete cliff material");
    TerrainJournalStroke ground = append; ground.intent.palette = TerrainPaletteEditIntent{TerrainTextureTable::Ground, 1, "", "Tg02", 2};
    TerrainJournalStroke replace = append; replace.intent.palette = TerrainPaletteEditIntent{TerrainTextureTable::Cliff, 0, "Cl01", "Cl03", 1};
    journal.strokes = {ground, append, replace, Journal().strokes.front()};
    const auto mixed = Take(DecodeTerrainStrokeJournal(Take(EncodeTerrainStrokeJournal(journal))));
    Require(mixed.strokes.size() == 4 && mixed.strokes[0].intent.palette->algorithm == 2
        && mixed.strokes[1].intent.palette->algorithm == 3 && mixed.strokes[2].intent.palette->algorithm == 1
        && !mixed.strokes[3].intent.palette, "ground append, cliff append, legacy replace and brush keep distinct ordered payloads");
}

void RawCliffAppendUsesSharedUndoWithoutInventingResourceAuthority() {
    SessionFixture fixture;
    const auto original = Take(SerializeMapTerrain(*fixture.session.Terrain()));
    const auto appended = Take(fixture.session.EditTerrainTextureSlot(TerrainTextureTable::Cliff, 1, "Cl02"));
    Require(appended.headerChanged && fixture.session.Terrain()->header.cliffTilesets == std::vector<std::string>{"Cl01", "Cl02"},
        "raw core API appends the next concrete cliff material");
    const auto final = Take(SerializeMapTerrain(*fixture.session.Terrain()));
    Require(!fixture.session.ExportTerrainStrokeJournal().Succeeded(), "raw header append cannot invent source-bound provenance");
    const auto revision = fixture.session.Revision();
    Require(!fixture.session.EditTerrainTextureSlot(TerrainTextureTable::Cliff, 3, "Cl03").Succeeded()
        && !fixture.session.EditTerrainTextureSlot(TerrainTextureTable::Cliff, 2, "Cl01").Succeeded()
        && fixture.session.Revision() == revision, "sparse and duplicate requests leave the complete history unchanged");
    Require(!Take(fixture.session.EditTerrainTextureSlot(TerrainTextureTable::Cliff, 1, "Cl02")).headerChanged
        && fixture.session.Revision() == revision, "same-value replacement creates no phantom command");
    Require(fixture.session.Undo() && Take(SerializeMapTerrain(*fixture.session.Terrain())) == original,
        "cliff append Undo restores the exact original header and all vertices");
    Require(fixture.session.Redo() && Take(SerializeMapTerrain(*fixture.session.Terrain())) == final,
        "cliff append Redo restores the exact complete appended terrain");
}
