// Included in TerrainStrokeJournalTests.cpp's fixture namespace.
DoodadPlacements OrdinaryJournalDoodads() {
    DoodadPlacements value;
    DoodadPlacement ordinary; ordinary.rawcode = "Ord0"; ordinary.creationId = 1;
    ordinary.position = {256, 384, 0}; value.doodads.push_back(ordinary);
    SpecialDoodadPlacement special; special.rawcode = "Spc0"; special.position = {3, 5};
    value.specialDoodads.push_back(special);
    return value;
}

void OrdinaryDoodadsDeltaIsBoundedAndPreservesFullClassicIdentity() {
    const auto original = OrdinaryJournalDoodads(); auto moved = original; moved.doodads.front().position[0] = 512;
    const auto before = Take(original.Serialize()), after = Take(moved.Serialize());
    const auto edit = Take(BuildTerrainDoodadsJournalEdit(before, after));
    Require(edit.beforeBytes == before.size() && edit.afterBytes == after.size()
        && edit.beforeSha256 != edit.afterSha256 && !edit.inserted.empty(), "ordinary delta binds both complete members");
    Require(Take(ApplyTerrainDoodadsJournalEdit(before, edit)).OriginalBytes() == after,
        "ordinary delta reproduces exact full classic wire");
    Require(!BuildTerrainDoodadsJournalEdit(before, before).Succeeded(), "no-change ordinary command cannot create a record");
    Require(!BuildTerrainDoodadsJournalEdit(before, after, 0).Succeeded(), "inserted wire budget is checked before copying the middle");
    auto inserted = moved; inserted.doodads.push_back(moved.doodads.front()); inserted.doodads.back().creationId = 2;
    const auto grown = Take(inserted.Serialize());
    const auto add = Take(BuildTerrainDoodadsJournalEdit(after, grown));
    Require(add.afterBytes > add.beforeBytes && Take(ApplyTerrainDoodadsJournalEdit(after, add)).OriginalBytes() == grown,
        "insertion changes only ordinary records and retains exact special tail");
    const auto remove = Take(BuildTerrainDoodadsJournalEdit(grown, before));
    Require(remove.afterBytes < remove.beforeBytes && Take(ApplyTerrainDoodadsJournalEdit(grown, remove)).OriginalBytes() == before,
        "deletion safely applies a shrinking single range");

    auto stale = before; stale[20] ^= std::byte{1};
    Require(!ApplyTerrainDoodadsJournalEdit(stale, edit).Succeeded(), "matching length cannot bypass full before CAS");
    auto bad = edit; bad.beforeSha256[0] = bad.beforeSha256[0] == '0' ? '1' : '0';
    Require(!ApplyTerrainDoodadsJournalEdit(before, bad).Succeeded(), "wrong complete before SHA rejected");
    bad = edit; bad.afterSha256[0] = bad.afterSha256[0] == '0' ? '1' : '0';
    Require(!ApplyTerrainDoodadsJournalEdit(before, bad).Succeeded(), "wrong complete after SHA rejected");
    bad = edit; bad.prefixBytes = std::numeric_limits<std::uint32_t>::max();
    Require(!ValidateTerrainDoodadsJournalEdit(bad).Succeeded(), "prefix range checked before subtraction");
    bad = edit; bad.removedBytes = std::numeric_limits<std::uint32_t>::max();
    Require(!ValidateTerrainDoodadsJournalEdit(bad).Succeeded(), "removed range cannot wrap current size");
    bad = edit; ++bad.afterBytes;
    Require(!ValidateTerrainDoodadsJournalEdit(bad).Succeeded(), "result length equals exact single-range equation");
    bad = edit; bad.inserted.front() ^= std::byte{1};
    Require(!ApplyTerrainDoodadsJournalEdit(before, bad).Succeeded(), "insert payload cannot bypass full output SHA");
    bad = edit; bad.beforeSha256[0] = 'A';
    Require(!ValidateTerrainDoodadsJournalEdit(bad).Succeeded(), "noncanonical SHA rejected");

    auto special = moved; special.specialDoodads.front().variation = 1;
    Require(!BuildTerrainDoodadsJournalEdit(before, Take(special.Serialize())).Succeeded(), "ordinary record cannot change special variation");
    // 独立字面位置：一个special尾为24字节，variation首字节位于EOF-12。
    // 即使伪造者给出正确完整SHA与最小中段，普通重放仍不能取得特殊删除/修改资格。
    special = original; special.specialDoodads.front().variation = 1;
    const auto forgedWire = Take(special.Serialize());
    TerrainDoodadsJournalEdit forged;
    forged.beforeSha256 = Take(Sha256(before)); forged.afterSha256 = Take(Sha256(forgedWire));
    forged.beforeBytes = static_cast<std::uint32_t>(before.size()); forged.afterBytes = forged.beforeBytes;
    forged.prefixBytes = forged.beforeBytes - 12; forged.removedBytes = 1; forged.inserted = {std::byte{1}};
    const auto rejectedSpecial = ApplyTerrainDoodadsJournalEdit(before, forged);
    Require(!rejectedSpecial.Succeeded() && !rejectedSpecial.diagnostics.empty()
        && rejectedSpecial.diagnostics.front().code == "TERRAIN_JOURNAL_DOODADS_SPECIAL",
        "correct full SHA and canonical patch cannot authorize a special-tail change");
    special = moved; special.specialDoodads.clear();
    Require(!BuildTerrainDoodadsJournalEdit(before, Take(special.Serialize())).Succeeded(), "ordinary record cannot delete special row");
    special = moved; special.specialDoodads.push_back(special.specialDoodads.front());
    Require(!BuildTerrainDoodadsJournalEdit(before, Take(special.Serialize())).Succeeded(), "ordinary record cannot insert special row");
    auto oldHeader = moved; oldHeader.version = 7; oldHeader.subversion = 9;
    Require(!BuildTerrainDoodadsJournalEdit(before, Take(oldHeader.Serialize())).Succeeded(), "valid alternate classic header is not an ordinary edit");
    auto corrupt = after; corrupt.push_back(std::byte{0});
    Require(!BuildTerrainDoodadsJournalEdit(before, corrupt).Succeeded(), "unknown wire tail never receives a delta");
    Require(!TerrainClassicDoodadsWorkingBytes(before.size(), after.size(), 1).Succeeded(),
        "pair budget reserves codec before any tree is allocated");
    Require(!TerrainClassicDoodadsWorkingBytes(std::numeric_limits<std::size_t>::max(), after.size(), 64u * 1024u * 1024u).Succeeded(),
        "pair budget cannot overflow wire sum");
}

void OrdinaryDoodadsJournalVersionAndStrictVariants() {
    const auto original = OrdinaryJournalDoodads(); auto moved = original; moved.doodads.front().position[0] = 512;
    auto journal = Journal(); TerrainJournalStroke ordinary;
    const auto originalWire = Take(original.Serialize());
    journal.members[5] = {"war3map.doo", true, Take(Sha256(originalWire)), originalWire.size()};
    ordinary.ordinaryDoodads = Take(BuildTerrainDoodadsJournalEdit(originalWire, Take(moved.Serialize())));
    journal.strokes.insert(journal.strokes.begin(), ordinary);
    const auto wire = Take(EncodeTerrainStrokeJournal(journal)); const auto decoded = Take(DecodeTerrainStrokeJournal(wire));
    Require(wire[3] == std::byte{'7'} && decoded.strokes.front().ordinaryDoodads
        && decoded.strokes.size() == 2 && Take(EncodeTerrainStrokeJournal(decoded)) == wire,
        "FTJ7 stores zero-point ordinary and typed terrain in exact order");
    for (std::size_t count = 0; count < wire.size(); ++count)
        Require(!DecodeTerrainStrokeJournal(std::span(wire).first(count)).Succeeded(), "every FTJ7 truncation fails closed");
    for (const char old : {'1', '2', '3', '4', '5', '6'}) {
        auto changed = wire; changed[3] = std::byte(old);
        Require(!DecodeTerrainStrokeJournal(changed).Succeeded(), "old wire magic cannot acquire ordinary records");
    }
    auto invalid = journal; invalid.strokes[0].intent.points.push_back({0, 0});
    Require(!EncodeTerrainStrokeJournal(invalid).Succeeded(), "ordinary cannot smuggle sampling points");
    invalid = journal; invalid.strokes[0].intent.brush.seed = 1;
    Require(!EncodeTerrainStrokeJournal(invalid).Succeeded(), "ordinary placeholder must be fully canonical");
    invalid = journal; invalid.strokes[0].intent.bounds = TerrainBoundsEditIntent{};
    Require(!EncodeTerrainStrokeJournal(invalid).Succeeded(), "ordinary and bounds are exclusive variants");
    invalid = journal; invalid.strokes[0].rulesSha256.assign(64, 'a');
    Require(!EncodeTerrainStrokeJournal(invalid).Succeeded(), "ordinary cannot borrow a resource-owner digest");
    invalid = journal; invalid.strokes[0].cliffEnforceWaterLimits = false;
    Require(!EncodeTerrainStrokeJournal(invalid).Succeeded(), "ordinary cannot carry a cliff-validation option");
    invalid = journal; invalid.strokes[0].ordinaryDoodads->prefixBytes = std::numeric_limits<std::uint32_t>::max();
    Require(!EncodeTerrainStrokeJournal(invalid).Succeeded(), "ordinary invalid bounds are rejected before encoding");
    invalid = journal; invalid.members[5] = {"war3map.doo", false, {}, 0};
    Require(!EncodeTerrainStrokeJournal(invalid).Succeeded(), "ordinary records require an actual complete source DOO");
    auto changed = Take(EncodeTerrainStrokeJournal(Journal())); changed[3] = std::byte{'7'};
    Require(!DecodeTerrainStrokeJournal(changed).Succeeded(), "FTJ7 without an ordinary record is noncanonical");
    journal.strokes.assign(128, ordinary);
    Require(EncodeTerrainStrokeJournal(journal).Succeeded(), "ordinary commands retain the same128 total command budget");
    journal.strokes.push_back(ordinary);
    Require(!EncodeTerrainStrokeJournal(journal).Succeeded(), "ordinary commands cannot raise the128 total command budget");
}

void OrdinaryDoodadsFreezeKeepsRealCommandScopeAndUndo() {
    SessionFixture grid;
    const auto original = OrdinaryJournalDoodads(); const auto before = Take(original.Serialize());
    const std::vector<MapArchiveMember> sourceMembers{{"war3map.doo", before}};
    MapDataEditSession session{MapDocument::FromMembers(sourceMembers)};
    Require(session.AttachTerrain(grid.terrainBytes).Succeeded() && session.AttachPathing(grid.pathing).Succeeded(), "attach journal ordinary source");
    auto moved = original; moved.doodads.front().position[0] = 512;
    Require(session.EditDoodads(moved).Succeeded(), "real ordinary transaction accepted");
    auto frozen = Take(session.ExportTerrainStrokeJournal());
    Require(frozen.strokes.size() == 1 && frozen.strokes.front().frozenDoodads && !frozen.strokes.front().ordinaryDoodads,
        "UI export freezes actual command aliases instead of constructing wire or authorization");
    Require(frozen.strokes.front().frozenDoodads->RetainedBytes() > before.size() * 2,
        "the alias budget includes its complete immutable command rather than only copied wire");
    auto movedFrom = frozen;
    const auto keptSnapshot = std::move(*movedFrom.strokes.front().frozenDoodads);
    Require(!MaterializeTerrainJournalDoodads(movedFrom).Succeeded(),
        "a moved-from optional frozen payload fails without dereferencing released aliases");
    const auto state = session.TerrainJournalStateFingerprint();
    Require(state.find("mixed:") != std::string::npos, "ordinary CAS is scoped to actual immutable command identities");
    TerrainBrush brush; brush.kind = TerrainBrushKind::Raise; brush.radius = 0;
    Take(session.EditTerrain(brush, std::span(&grid.point, 1), false)); const auto terrainReceipt = session.LastEdit();
    Require(session.TerrainJournalStateFingerprint() != state, "ordered terrain variant contributes to mixed CAS");
    Require(session.Replay(terrainReceipt, false).Succeeded() && session.TerrainJournalStateFingerprint() == state,
        "logical inverse restores the original ordinary command scope");
    moved.doodads.front().position[0] = 640; Require(session.EditDoodads(moved).Succeeded(), "later ordinary command accepted");
    const auto retained = frozen.strokes.front().frozenDoodads->After();
    Require(std::equal(retained.begin(), retained.end(), before.begin(), before.end()) == false,
        "frozen after holds the actual changed wire");
    auto firstMoved = original; firstMoved.doodads.front().position[0] = 512;
    const auto firstWire = Take(firstMoved.Serialize());
    Require(std::equal(retained.begin(), retained.end(), firstWire.begin(), firstWire.end()),
        "later active edits cannot replace an immutable frozen command snapshot");
    Require(!EncodeTerrainStrokeJournal(frozen).Succeeded(), "unmaterialized snapshots are never wire records");
    frozen.sourceSha256 = Journal().sourceSha256; frozen.members = Journal().members;
    frozen.members[5] = {"war3map.doo", true, Take(Sha256(before)), before.size()};
    Require(MaterializeTerrainJournalDoodads(frozen).Succeeded() && !frozen.strokes.front().frozenDoodads
        && frozen.strokes.front().ordinaryDoodads, "background materialization replaces alias with actual SHA and delta");
    const auto wire = Take(EncodeTerrainStrokeJournal(frozen)); Require(wire[3] == std::byte{'7'}, "materialized ordinary command selects FTJ7");
    MapDataEditSession restored{MapDocument::FromMembers(sourceMembers)};
    const auto& edit = *frozen.strokes.front().ordinaryDoodads;
    const auto cost = Take(MeasureTerrainDoodadsJournalEdit(edit.beforeBytes, edit.afterBytes, 64u * 1024u * 1024u, 64u * 1024u * 1024u));
    Require(!restored.ReplayTerrainJournalDoodads(edit, 1, cost.work).Succeeded() && !restored.IsDirty(),
        "candidate refusal occurs before ordinary replay publishes a command");
    Require(restored.ReplayTerrainJournalDoodads(edit, cost.workingBytes, cost.work).Succeeded(), "ordinary restore uses the normal core transaction");
    Require(restored.LastEdit().CommandIdentity() != 0 && Take(restored.Snapshot().Doodads()->Serialize()) == firstWire,
        "restored ordinary transaction supplies a real Undo receipt");
    Require(restored.Undo() && Take(restored.Snapshot().Doodads()->Serialize()) == before,
        "restored ordinary undo returns the exact source wire");
}
