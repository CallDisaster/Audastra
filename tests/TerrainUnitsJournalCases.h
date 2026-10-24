#pragma once

// 在TerrainStrokeJournalTests.cpp的fixture命名空间内包含。
UnitPlacements JournalClassicUnits() {
    UnitPlacements value;
    UnitPlacement unit; unit.rawcode = "Hfoo"; unit.position = {256, 384, 19};
    unit.rotation = 0.625f; unit.scale = {1, 1.25f, 0.75f}; unit.variation = 2;
    unit.flags = 2; unit.owner = 3; unit.unknownOwnerBytes = {0x41, 0x7f};
    unit.hitPoints = 83; unit.mana = 54; unit.droppedItemTable = -1;
    PlacementItemSet drops; drops.items = {{"gold", 70}, {"lmbr", 30}}; unit.itemSets.push_back(drops);
    unit.gold = 29; unit.targetAcquisition = 633.25f;
    unit.heroLevel = 7; unit.heroStrength = 23; unit.heroAgility = 17; unit.heroIntelligence = 31;
    unit.inventory = {{0, "gold"}, {5, "lmbr"}}; unit.abilities = {{"AHhb", true, 2}, {"AHds", false, 1}};
    unit.random = RandomAnyUnit{-1, 2}; unit.teamColor = 4; unit.waygateRegionId = 17; unit.creationId = 907;
    value.units.push_back(unit);
    unit.rawcode = "hfoo"; unit.creationId = 908; unit.random = RandomGlobalTable{3, 1}; value.units.push_back(unit);
    unit.rawcode = "hrif"; unit.creationId = 909; unit.random = RandomCustomTable{{{"hfoo", 65}, {"hrif", 35}}};
    value.units.push_back(unit);
    unit.rawcode = "hkni"; unit.creationId = 910; unit.random = std::monostate{}; value.units.push_back(unit);
    return value;
}

void UnitsJournalDeltaPreservesCompleteClassicPayload() {
    const auto original = JournalClassicUnits(); auto changed = original;
    changed.units[0].position[0] += 128; changed.units[0].hitPoints = 1;
    changed.units[1].abilities.push_back({"AHbz", false, 3});
    changed.units[2].inventory.erase(changed.units[2].inventory.begin());
    changed.units[3].random = RandomCustomTable{{{"hfoo", 100}}};
    const auto before = Take(original.Serialize()), after = Take(changed.Serialize());
    const auto edit = Take(BuildTerrainUnitsJournalEdit(before, after));
    Require(edit.beforeBytes == before.size() && edit.afterBytes == after.size()
        && edit.beforeSha256 != edit.afterSha256, "Units delta binds both complete classic members");
    Require(Take(ApplyTerrainUnitsJournalEdit(before, edit)).OriginalBytes() == after,
        "all nested inventory/drop/ability/random/unknown and transform fields survive exact replay");
    const auto empty = Take(UnitPlacements{}.Serialize());
    const auto appended = Take(BuildTerrainUnitsJournalEdit(empty, after));
    Require(Take(ApplyTerrainUnitsJournalEdit(empty, appended)).OriginalBytes() == after,
        "empty classic source supports full insertion without inferring rows");
    const auto deleted = Take(BuildTerrainUnitsJournalEdit(after, empty));
    Require(Take(ApplyTerrainUnitsJournalEdit(after, deleted)).OriginalBytes() == empty,
        "full classic delete retains complete header and empty result");
    Require(!BuildTerrainUnitsJournalEdit(before, before).Succeeded(), "no-change Units record cannot mint a receipt");
    Require(!BuildTerrainUnitsJournalEdit(before, after, 0).Succeeded(), "Units middle budget checked before copy");
    auto corrupt = before; corrupt.push_back(std::byte{0x7f});
    Require(!BuildTerrainUnitsJournalEdit(corrupt, after).Succeeded(), "unknown Units tails cannot enter typed recovery");
    auto invalid = edit; invalid.prefixBytes = std::numeric_limits<std::uint32_t>::max();
    Require(!ValidateTerrainUnitsJournalEdit(invalid).Succeeded(), "Units prefix checked before subtraction");
    invalid = edit; invalid.removedBytes = std::numeric_limits<std::uint32_t>::max();
    Require(!ApplyTerrainUnitsJournalEdit(before, invalid).Succeeded(), "Units remove length cannot overflow");
    invalid = edit; invalid.afterBytes++;
    Require(!ValidateTerrainUnitsJournalEdit(invalid).Succeeded(), "Units length equation cannot drift");
    invalid = edit; invalid.beforeSha256[0] = invalid.beforeSha256[0] == 'a' ? 'b' : 'a';
    Require(!ApplyTerrainUnitsJournalEdit(before, invalid).Succeeded(), "same-size wrong Units full source is rejected");
    invalid = edit; invalid.afterSha256[0] = invalid.afterSha256[0] == 'a' ? 'b' : 'a';
    Require(!ApplyTerrainUnitsJournalEdit(before, invalid).Succeeded(), "Units output full SHA is required");
    invalid = edit; invalid.inserted.front() ^= std::byte{1};
    Require(!ApplyTerrainUnitsJournalEdit(before, invalid).Succeeded(), "Units payload cannot bypass new full SHA");
    invalid = edit; Require(invalid.prefixBytes > 0, "fixture edit has a complete common prefix");
    --invalid.prefixBytes; ++invalid.removedBytes; invalid.inserted.insert(invalid.inserted.begin(), before[invalid.prefixBytes]);
    Require(ValidateTerrainUnitsJournalEdit(invalid).Succeeded() && !ApplyTerrainUnitsJournalEdit(before, invalid).Succeeded(),
        "same complete output SHA cannot authorize a nonminimal alternate Units middle");
    auto old = original; old.version = 7; old.subversion = 9;
    for (auto& unit : old.units) unit.heroStrength = unit.heroAgility = unit.heroIntelligence = 0;
    const auto oldWire = Take(old.Serialize());
    Require(!BuildTerrainUnitsJournalEdit(before, oldWire).Succeeded(), "valid alternate classic header is not an instance edit");
    const auto cost = Take(MeasureTerrainUnitsJournalEdit(before.size(), after.size(), 64u * 1024u * 1024u, 64u * 1024u * 1024u));
    Require(!MeasureTerrainUnitsJournalEdit(before.size(), after.size(), cost.workingBytes - 1, cost.work).Succeeded(),
        "complete Units codec peak is prepaid before parsing");
    Require(!MeasureTerrainUnitsJournalEdit(before.size(), after.size(), cost.workingBytes, cost.work - 1).Succeeded(),
        "Units full scan budget cannot be exceeded");
}

void UnitsJournalCodecKeepsOrderedClosedVariants() {
    const auto before = Take(JournalClassicUnits().Serialize()); auto changed = JournalClassicUnits(); changed.units[0].owner = 2;
    auto journal = Journal(); TerrainJournalStroke units;
    journal.members[4] = {"war3mapunits.doo", true, Take(Sha256(before)), before.size()};
    units.units = Take(BuildTerrainUnitsJournalEdit(before, Take(changed.Serialize())));
    journal.strokes.insert(journal.strokes.begin(), units);
    const auto wire = Take(EncodeTerrainStrokeJournal(journal)); const auto decoded = Take(DecodeTerrainStrokeJournal(wire));
    Require(wire[3] == std::byte{'9'} && decoded.strokes.size() == 2 && decoded.strokes[0].units
        && Take(EncodeTerrainStrokeJournal(decoded)) == wire, "FTJ9 stores exact Units/terrain order with canonical codec");
    for (std::size_t size = 0; size < wire.size(); ++size)
        Require(!DecodeTerrainStrokeJournal(std::span(wire).first(size)).Succeeded(), "all FTJ9 truncations reject partial output");
    for (char version = '1'; version <= '8'; ++version) {
        auto wrong = wire; wrong[3] = std::byte(version);
        Require(!DecodeTerrainStrokeJournal(wrong).Succeeded(), "old versions cannot acquire Units tag4");
    }
    auto invalid = journal; invalid.strokes[0].intent.points = {{1, 2}};
    Require(!EncodeTerrainStrokeJournal(invalid).Succeeded(), "Units does not carry public brush payload");
    invalid = journal; invalid.strokes[0].rulesSha256.assign(64, 'c');
    Require(!EncodeTerrainStrokeJournal(invalid).Succeeded(), "Units delta cannot claim a water owner");
    invalid = journal; invalid.strokes[0].ordinaryDoodads = TerrainDoodadsJournalEdit{};
    Require(!EncodeTerrainStrokeJournal(invalid).Succeeded(), "Units and DOO are exclusive variants");
    invalid = journal; invalid.members[4] = {"war3mapunits.doo", false, {}, 0};
    Require(!EncodeTerrainStrokeJournal(invalid).Succeeded(), "Units recovery requires real complete source member");
    auto noUnits = Take(EncodeTerrainStrokeJournal(Journal())); noUnits[3] = std::byte{'9'};
    Require(!DecodeTerrainStrokeJournal(noUnits).Succeeded(), "FTJ9 without Units is noncanonical");
    auto corner = journal.strokes.back(); corner.intent.brush = {}; corner.intent.brush.kind = TerrainBrushKind::CornerRamp;
    corner.intent.points = {{256, 256}}; corner.rulesSha256.assign(64, 'd');
    corner.intent.cornerRampAlgorithm = TerrainCornerRampAlgorithm::DryOnly;
    journal.strokes.back() = corner;
    auto restored = Take(DecodeTerrainStrokeJournal(Take(EncodeTerrainStrokeJournal(journal))));
    Require(restored.strokes[1].intent.cornerRampAlgorithm == TerrainCornerRampAlgorithm::DryOnly,
        "FTJ9 explicitly preserves older dry corner semantics");
    journal.strokes[1].intent.cornerRampAlgorithm = TerrainCornerRampAlgorithm::WetTerrain;
    restored = Take(DecodeTerrainStrokeJournal(Take(EncodeTerrainStrokeJournal(journal))));
    Require(restored.strokes[1].intent.cornerRampAlgorithm == TerrainCornerRampAlgorithm::WetTerrain,
        "FTJ9 composes the FTJ8 wet corner semantic byte");
    auto cornerOnly = journal; cornerOnly.strokes.erase(cornerOnly.strokes.begin());
    auto cornerWire = Take(EncodeTerrainStrokeJournal(cornerOnly));
    Require(cornerWire[3] == std::byte{'8'}, "without Units the same wet corner remains FTJ8");
    cornerWire[3] = std::byte{'9'};
    Require(!DecodeTerrainStrokeJournal(cornerWire).Succeeded(), "otherwise valid FTJ8 payload cannot become FTJ9 without Units");
    journal.strokes.assign(128, units);
    Require(EncodeTerrainStrokeJournal(journal).Succeeded(), "Units share the same 128 total-command ceiling");
    journal.strokes.push_back(units);
    Require(!EncodeTerrainStrokeJournal(journal).Succeeded(), "Units cannot increase the total-command ceiling");
}

void UnitsJournalFreezesRealCommandsAndRestoresReceipts() {
    SessionFixture grid;
    const auto original = JournalClassicUnits(); const auto before = Take(original.Serialize());
    const std::vector<MapArchiveMember> members{{"war3mapUnits.doo", before}};
    MapDataEditSession hot{MapDocument::FromMembers(members)};
    Require(hot.AttachTerrain(grid.terrainBytes).Succeeded() && hot.AttachPathing(grid.pathing).Succeeded(), "attach Units journal source");
    auto changed = original; changed.units[0].position[0] += 128;
    const auto after = Take(changed.Serialize());
    Require(hot.EditUnits(changed).Succeeded(), "real full Units mutation commits");
    auto frozen = Take(hot.ExportTerrainStrokeJournal());
    Require(frozen.strokes.size() == 1 && frozen.strokes[0].frozenUnits && !frozen.strokes[0].units,
        "UI captures immutable actual Units Command aliases without parsing or hashing");
    auto moved = frozen; const auto kept = std::move(*moved.strokes[0].frozenUnits);
    Require(!MaterializeTerrainJournalPlacements(moved).Succeeded(), "moved-from Units alias cannot be dereferenced");
    const auto scope = hot.TerrainJournalStateFingerprint();
    TerrainBrush brush; brush.kind = TerrainBrushKind::Raise; brush.radius = 0;
    const TerrainStrokePoint point{256, 256}; Take(hot.EditTerrain(brush, std::span(&point, 1), false));
    const auto receipt = hot.LastEdit();
    Require(hot.Replay(receipt, false).Succeeded() && hot.TerrainJournalStateFingerprint() == scope,
        "Undo restores exact active Units command scope");
    changed.units[0].position[0] += 128; Require(hot.EditUnits(changed).Succeeded(), "later Units command accepted");
    Require(std::equal(kept.After().begin(), kept.After().end(), after.begin(), after.end()),
        "new active mutations cannot replace old immutable alias");
    Require(!EncodeTerrainStrokeJournal(frozen).Succeeded(), "frozen Units must materialize in background");
    frozen.sourceSha256 = Journal().sourceSha256; frozen.members = Journal().members;
    frozen.members[4] = {"war3mapunits.doo", true, Take(Sha256(before)), before.size()};
    Require(MaterializeTerrainJournalPlacements(frozen).Succeeded() && frozen.strokes[0].units && !frozen.strokes[0].frozenUnits,
        "background typed Units materialization supplies complete SHA and minimal delta");
    const auto& edit = *frozen.strokes[0].units;
    const auto cost = Take(MeasureTerrainUnitsJournalEdit(edit.beforeBytes, edit.afterBytes, 64u * 1024u * 1024u, 64u * 1024u * 1024u));
    MapDataEditSession cold{MapDocument::FromMembers(members)};
    Require(!cold.ReplayTerrainJournalUnits(edit, 1, cost.work).Succeeded() && !cold.IsDirty(), "Units budget failure publishes nothing");
    Require(cold.ReplayTerrainJournalUnits(edit, cost.workingBytes, cost.work).Succeeded(), "Units replay uses normal prepared transaction");
    Require(cold.LastEdit().CommandIdentity() && Take(cold.Snapshot().Units()->Serialize()) == after,
        "cold Units transaction has genuine receipt and complete expected output");
    Require(cold.Undo() && Take(cold.Snapshot().Units()->Serialize()) == before, "Units undo returns complete source");
    Require(cold.Redo() && Take(cold.Snapshot().Units()->Serialize()) == after, "Units redo restores full payload");
    auto start = original; start.units[0].rawcode = "sloc";
    const auto startBefore = Take(start.Serialize()); start.units[0].position[0] += 128;
    const auto startEdit = Take(BuildTerrainUnitsJournalEdit(startBefore, Take(start.Serialize())));
    MapDataEditSession linked{MapDocument::FromMembers(std::vector<MapArchiveMember>{{"war3mapUnits.doo", startBefore}})};
    Require(!linked.ReplayTerrainJournalUnits(startEdit, 64u * 1024u * 1024u, 64u * 1024u * 1024u).Succeeded()
        && !linked.IsDirty(), "required start-location Settings linkage is never silently omitted");
    MapSettings settings; settings.formatVersion = 25; settings.custom.emplace();
    MapPlayerSettings player; player.number = 3; player.controller = 1; settings.players.push_back(player);
    const std::vector<MapArchiveMember> linkedMembers{{"war3mapUnits.doo", startBefore}, {"war3map.w3i", Take(settings.Serialize())}};
    MapDataEditSession linkedHot{MapDocument::FromMembers(linkedMembers)};
    Require(linkedHot.AttachTerrain(grid.terrainBytes).Succeeded() && linkedHot.AttachPathing(grid.pathing).Succeeded(),
        "attach full start-location source");
    Require(linkedHot.EditUnits(start).Succeeded() && linkedHot.Revision() == 1,
        "ordinary editor still commits start-location Units and W3I atomically");
    const auto linkedJournal = Take(linkedHot.ExportTerrainStrokeJournal());
    Require(linkedJournal.strokes.size() == 1 && linkedJournal.strokes.front().frozenStartLocations,
        "two-member start-location transaction freezes both complete members for FTJA, never skips its W3I effect");
    MapDataEditSession linkedCold{MapDocument::FromMembers(linkedMembers)};
    const auto rejected = linkedCold.ReplayTerrainJournalUnits(startEdit, 64u * 1024u * 1024u, 64u * 1024u * 1024u);
    Require(!rejected.Succeeded() && !linkedCold.IsDirty() && linkedCold.Revision() == 0,
        "even a valid matching W3I cannot let single-member Units replay publish partial linked changes");
    auto doodads = OrdinaryJournalDoodads();
    const std::vector<MapArchiveMember> mixedMembers{{"war3mapUnits.doo", before}, {"war3map.doo", Take(doodads.Serialize())}};
    MapDataEditSession mixed{MapDocument::FromMembers(mixedMembers)};
    Require(mixed.AttachTerrain(grid.terrainBytes).Succeeded() && mixed.AttachPathing(grid.pathing).Succeeded(), "attach mixed placements source");
    doodads.doodads[0].position[0] += 128;
    Require(mixed.EditPlacements(&changed, &doodads).Succeeded() && !mixed.ExportTerrainStrokeJournal().Succeeded(),
        "a two-member mixed placement Command cannot be silently reduced to either Units or DOO");
}
