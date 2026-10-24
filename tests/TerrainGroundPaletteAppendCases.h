#pragma once

// 在TerrainStrokeJournalTests的SessionFixture之后包含。
void GroundPaletteAppendUsesAnExplicitFTJ6Algorithm() {
    auto journal = Journal(); TerrainJournalStroke append;
    append.intent.palette = TerrainPaletteEditIntent{TerrainTextureTable::Ground, 1, "", "Tg02", 2};
    append.rulesSha256.assign(64, 'd'); journal.strokes = {append};
    const auto wire = Take(EncodeTerrainStrokeJournal(journal));
    Require(wire[3] == std::byte{'6'}, "ground append uses the existing explicit FTJ6 palette layout");
    // 独立字面正文：variant2、algorithm2、Ground0、旧长度1、空旧rawcode、新rawcode及digest长度。
    const Bytes prefix{std::byte{2}, std::byte{2}, std::byte{0},
        std::byte{1}, std::byte{0}, std::byte{0}, std::byte{0},
        std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0},
        std::byte{4}, std::byte{0}, std::byte{0}, std::byte{0},
        std::byte{'T'}, std::byte{'g'}, std::byte{'0'}, std::byte{'2'},
        std::byte{64}, std::byte{0}, std::byte{0}, std::byte{0}};
    Require(wire.size() > prefix.size() + 64 && std::equal(prefix.begin(), prefix.end(), wire.end() - prefix.size() - 64),
        "the complete independent append payload retains an explicit zero-length expected rawcode");
    const auto restored = Take(DecodeTerrainStrokeJournal(wire));
    Require(restored.strokes.size() == 1 && restored.strokes.front().intent.palette
        && restored.strokes.front().intent.palette->algorithm == 2
        && restored.strokes.front().intent.palette->expectedRawcode.empty()
        && Take(EncodeTerrainStrokeJournal(restored)) == wire, "explicit append survives complete canonical roundtrip");
    for (std::size_t size = 0; size < wire.size(); ++size)
        Require(!DecodeTerrainStrokeJournal(std::span(wire).first(size)).Succeeded(), "every append record truncation fails closed");
    const auto offset = wire.size() - prefix.size() - 64;
    for (const auto [delta, value] : {std::pair{1u, 1u}, {1u, 3u}, {2u, 1u}, {3u, 0u}, {3u, 16u}}) {
        auto changed = wire; changed[offset + delta] = std::byte(value);
        Require(!DecodeTerrainStrokeJournal(changed).Succeeded(),
            "wire cannot reinterpret empty expected value as legacy replace, unknown algorithm, cliff or invalid append length");
    }
    auto invalid = journal; invalid.strokes.front().intent.palette->algorithm = 1;
    Require(!EncodeTerrainStrokeJournal(invalid).Succeeded(), "old replace semantics do not accept an empty before slot");
    invalid = journal; invalid.strokes.front().intent.palette->algorithm = 3;
    Require(!EncodeTerrainStrokeJournal(invalid).Succeeded(), "unknown append algorithm stays rejected");
    invalid = journal; invalid.strokes.front().intent.palette->expectedRawcode = "Tg01";
    Require(!EncodeTerrainStrokeJournal(invalid).Succeeded(), "append cannot hide a replacement before-value");
    invalid = journal; invalid.strokes.front().intent.palette->table = TerrainTextureTable::Cliff;
    Require(!EncodeTerrainStrokeJournal(invalid).Succeeded(), "new recovery semantics do not add a cliff slot");
    invalid = journal; invalid.strokes.front().intent.palette->slot = 0;
    Require(!EncodeTerrainStrokeJournal(invalid).Succeeded(), "append cannot manufacture an originally empty ground table");
    invalid = journal; invalid.strokes.front().intent.palette->slot = 16;
    Require(!EncodeTerrainStrokeJournal(invalid).Succeeded(), "append cannot exceed four-bit ground indexes");
    invalid = journal; invalid.strokes.front().intent.points = {{512, 512}};
    Require(!EncodeTerrainStrokeJournal(invalid).Succeeded(), "append remains a separate zero-point command");
    invalid = journal; invalid.strokes.front().rulesSha256.clear();
    Require(!EncodeTerrainStrokeJournal(invalid).Succeeded(), "append cannot discard actual full prospective resource identity");
    journal.strokes.front().intent.palette->slot = 15;
    Require(EncodeTerrainStrokeJournal(journal).Succeeded(), "fifteenth existing slot may receive the sixteenth entry");
    TerrainJournalStroke replace;
    replace.intent.palette = TerrainPaletteEditIntent{TerrainTextureTable::Ground, 0, "Tg01", "Tg03", 1};
    replace.rulesSha256.assign(64, 'e'); journal.strokes = {append, replace, Journal().strokes.front()};
    const auto mixed = Take(DecodeTerrainStrokeJournal(Take(EncodeTerrainStrokeJournal(journal))));
    Require(mixed.strokes.size() == 3 && mixed.strokes[0].intent.palette && mixed.strokes[1].intent.palette
        && mixed.strokes[0].intent.palette->algorithm == 2 && mixed.strokes[1].intent.palette->algorithm == 1 && !mixed.strokes[2].intent.palette,
        "ordered append, legacy replacement and brush retain independent variants");
}

void RawGroundAppendRetainsUndoWithoutInventingJournalAuthority() {
    SessionFixture fixture;
    const auto source = Take(SerializeMapTerrain(*fixture.session.Terrain()));
    const auto changed = Take(fixture.session.EditTerrainTextureSlot(TerrainTextureTable::Ground, 1, "Tg02"));
    Require(changed.headerChanged && fixture.session.Terrain()->header.groundTilesets == std::vector<std::string>{"Tg01", "Tg02"},
        "the ordinary core API still appends at the next ground slot");
    Require(!fixture.session.ExportTerrainStrokeJournal().Succeeded(), "raw appended header cannot claim captured resources or special provenance");
    const auto appended = Take(SerializeMapTerrain(*fixture.session.Terrain()));
    Require(fixture.session.Undo() && Take(SerializeMapTerrain(*fixture.session.Terrain())) == source,
        "ordinary append Undo returns the exact old complete terrain header");
    Require(fixture.session.Redo() && Take(SerializeMapTerrain(*fixture.session.Terrain())) == appended,
        "ordinary append Redo keeps complete new header and unchanged vertices");
    const auto revision = fixture.session.Revision();
    Require(!fixture.session.EditTerrainTextureSlot(TerrainTextureTable::Ground, 3, "Tg03").Succeeded()
        && fixture.session.Revision() == revision, "sparse ground append cannot alter the current command history");
}
