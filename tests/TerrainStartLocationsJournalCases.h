#pragma once

// 在TerrainStrokeJournalTests.cpp的fixture命名空间内包含。
UnitPlacements StartJournalUnits() {
    UnitPlacements value;
    UnitPlacement start; start.rawcode = "sloc"; start.creationId = 73; start.owner = 3;
    start.position = {256, 384, 19}; start.scale = {1, 1, 1}; start.rotation = 0.625f;
    value.units.push_back(start);
    UnitPlacement unit; unit.rawcode = "Hfoo"; unit.creationId = 74; unit.owner = 3;
    unit.position = {640, 768, 27}; unit.scale = {1, 1.25f, 0.75f};
    unit.hitPoints = 83; unit.mana = 54; unit.unknownOwnerBytes = {0x41, 0x7f};
    unit.inventory = {{0, "gold"}, {5, "lmbr"}}; unit.abilities = {{"AHhb", true, 2}};
    unit.random = RandomCustomTable{{{"hfoo", 65}, {"hrif", 35}}};
    value.units.push_back(unit);
    start.creationId = 75; start.owner = 0; start.position = {-128, 640, 0};
    value.units.push_back(start);
    return value;
}

MapSettings StartJournalSettings() {
    MapSettings value; value.formatVersion = 25; value.custom.emplace();
    value.mapName = "Original map"; value.author = "Original author";
    value.description = "Original description"; value.recommendedPlayers = "Original recommendation";
    value.playableWidth = 32; value.playableHeight = 32; value.tileset = 'L';
    MapPlayerSettings player; player.number = 3; player.controller = 1; player.race = 2;
    player.name = "Player three"; player.flags = 7; player.startX = 256; player.startY = 384;
    value.players.push_back(player);
    player.number = 0; player.name = "Player zero"; player.startX = -128; player.startY = 640;
    value.players.push_back(player);
    return value;
}

// 构造攻击者提供的完整合法CAS中段；后续仍须由产品重新核验起点语义。
TerrainPlacementJournalDelta StartJournalForgedSettingsDelta(const Bytes& before, const Bytes& after) {
    TerrainPlacementJournalDelta result;
    result.beforeBytes = static_cast<std::uint32_t>(before.size());
    result.afterBytes = static_cast<std::uint32_t>(after.size());
    result.beforeSha256 = Take(Sha256(before)); result.afterSha256 = Take(Sha256(after));
    std::size_t prefix = 0, suffix = 0;
    while (prefix < std::min(before.size(), after.size()) && before[prefix] == after[prefix]) ++prefix;
    while (suffix < before.size() - prefix && suffix < after.size() - prefix
        && before[before.size() - suffix - 1] == after[after.size() - suffix - 1]) ++suffix;
    result.prefixBytes = static_cast<std::uint32_t>(prefix);
    result.removedBytes = static_cast<std::uint32_t>(before.size() - prefix - suffix);
    result.inserted.assign(after.begin() + prefix, after.end() - suffix);
    return result;
}

void StartLocationsJournalDeltaPreservesCompletePayload() {
    auto oldUnits = StartJournalUnits(), newUnits = oldUnits;
    auto oldSettings = StartJournalSettings(), newSettings = oldSettings;
    newUnits.units[0].position[0] = 512; newUnits.units[0].position[1] = 640;
    newUnits.units[1].hitPoints = 1; newUnits.units[1].abilities.push_back({"AHds", false, 3});
    newSettings.players[0].startX = 512; newSettings.players[0].startY = 640;
    const auto before = Take(oldUnits.Serialize()), after = Take(newUnits.Serialize());
    const auto settingsBefore = Take(oldSettings.Serialize()), settingsAfter = Take(newSettings.Serialize());
    const auto canonicalBefore = Take(CanonicalTerrainStartSettings(settingsBefore));
    const auto canonicalAfter = Take(CanonicalTerrainStartSettings(settingsAfter));
    const auto edit = Take(BuildTerrainStartLocationsJournalEdit(before, after, settingsBefore, settingsAfter));
    Require(edit.units.beforeBytes == before.size() && edit.units.afterBytes == after.size()
        && edit.settings.beforeBytes == canonicalBefore.size() && edit.settings.afterBytes == canonicalAfter.size(),
        "start record binds complete Units and canonical complete W3I lengths");
    Require(edit.settings.beforeSha256 == Take(Sha256(canonicalBefore))
        && edit.settings.afterSha256 == Take(Sha256(canonicalAfter)),
        "only the four descriptions are omitted from W3I CAS identity");
    const auto replayed = Take(ApplyTerrainStartLocationsJournalEdit(before, settingsBefore, edit));
    Require(replayed.units.OriginalBytes() == after && replayed.settings == canonicalAfter,
        "compound replay retains nested ordinary Units fields and exact linked canonical W3I");
    auto addedUnits = oldUnits; auto addedSettings = oldSettings;
    MapPlayerSettings addedPlayer; addedPlayer.number = 1; addedPlayer.controller = 1;
    addedSettings.players.push_back(addedPlayer);
    const auto addSettingsBefore = Take(addedSettings.Serialize());
    auto addedStart = oldUnits.units.front(); addedStart.creationId = 76; addedStart.owner = 1;
    addedStart.position = {896, 1024, 0}; addedUnits.units.push_back(addedStart);
    addedSettings.players.back().startX = 896; addedSettings.players.back().startY = 1024;
    const auto addedWire = Take(addedUnits.Serialize()), addSettingsAfter = Take(addedSettings.Serialize());
    const auto add = Take(BuildTerrainStartLocationsJournalEdit(before, addedWire, addSettingsBefore, addSettingsAfter));
    const auto addedReplay = Take(ApplyTerrainStartLocationsJournalEdit(before, addSettingsBefore, add));
    Require(addedReplay.units.OriginalBytes() == addedWire
        && addedReplay.settings == Take(CanonicalTerrainStartSettings(addSettingsAfter)),
        "a unique new start updates exactly its existing W3I player");
    const auto cost = Take(MeasureTerrainStartLocationsJournalEdit(before.size(), after.size(),
        settingsBefore.size(), settingsAfter.size(), TerrainJournalMaximumBytes, TerrainJournalMaximumBytes));
    Require(!MeasureTerrainStartLocationsJournalEdit(before.size(), after.size(), settingsBefore.size(), settingsAfter.size(),
        cost.workingBytes - 1, cost.work).Succeeded(), "two-member codec peak must fit the cumulative memory budget");
    Require(!MeasureTerrainStartLocationsJournalEdit(before.size(), after.size(), settingsBefore.size(), settingsAfter.size(),
        cost.workingBytes, cost.work - 1).Succeeded(), "two-member full scan must fit the cumulative work budget");
    Require(!MeasureTerrainStartLocationsJournalEdit(std::numeric_limits<std::size_t>::max(), after.size(),
        settingsBefore.size(), settingsAfter.size(), TerrainJournalMaximumBytes, TerrainJournalMaximumBytes).Succeeded(),
        "Units source length cannot overflow the paired budget");
    Require(!MeasureTerrainStartLocationsJournalEdit(before.size(), after.size(), 1024u * 1024u + 1, settingsAfter.size(),
        TerrainJournalMaximumBytes, TerrainJournalMaximumBytes).Succeeded(), "complete W3I must remain within its own ceiling");
    const auto inserted = edit.units.inserted.size() + edit.settings.inserted.size();
    Require(inserted > 0 && !BuildTerrainStartLocationsJournalEdit(before, after, settingsBefore, settingsAfter, inserted - 1).Succeeded(),
        "both inserted middles share one remaining insertion budget");

    auto bad = edit; bad.units.beforeSha256[0] = bad.units.beforeSha256[0] == 'a' ? 'b' : 'a';
    Require(!ApplyTerrainStartLocationsJournalEdit(before, settingsBefore, bad).Succeeded(), "complete Units source SHA is required");
    bad = edit; bad.settings.beforeSha256[0] = bad.settings.beforeSha256[0] == 'a' ? 'b' : 'a';
    Require(!ApplyTerrainStartLocationsJournalEdit(before, settingsBefore, bad).Succeeded(), "complete canonical W3I source SHA is required");
    bad = edit; bad.settings.afterSha256[0] = bad.settings.afterSha256[0] == 'a' ? 'b' : 'a';
    Require(!ApplyTerrainStartLocationsJournalEdit(before, settingsBefore, bad).Succeeded(), "complete canonical W3I output SHA is required");
    bad = edit; Require(!bad.settings.inserted.empty(), "moved start changes the W3I middle");
    bad.settings.inserted.front() ^= std::byte{1};
    Require(!ApplyTerrainStartLocationsJournalEdit(before, settingsBefore, bad).Succeeded(), "W3I payload must reproduce its full output SHA");
    bad = edit; Require(bad.settings.prefixBytes > 0, "W3I fixture has an unchanged complete prefix");
    --bad.settings.prefixBytes; ++bad.settings.removedBytes;
    bad.settings.inserted.insert(bad.settings.inserted.begin(), canonicalBefore[bad.settings.prefixBytes]);
    Require(!ApplyTerrainStartLocationsJournalEdit(before, settingsBefore, bad).Succeeded(),
        "equal output bytes cannot authorize a nonminimal alternate W3I middle");
}

void StartLocationsJournalRejectsUnlinkedAndMalformedMembers() {
    const auto original = StartJournalUnits(); auto changed = original;
    const auto settings = StartJournalSettings(); auto linked = settings;
    changed.units[0].position[0] = 512; linked.players[0].startX = 512;
    const auto before = Take(original.Serialize()), after = Take(changed.Serialize());
    const auto settingsBefore = Take(settings.Serialize()), settingsAfter = Take(linked.Serialize());
    const auto edit = Take(BuildTerrainStartLocationsJournalEdit(before, after, settingsBefore, settingsAfter));
    auto otherPlayerField = linked; otherPlayerField.players[0].flags ^= 1;
    Require(!BuildTerrainStartLocationsJournalEdit(before, after, settingsBefore, Take(otherPlayerField.Serialize())).Succeeded(),
        "start linkage cannot edit another field of the corresponding player");
    auto unrelated = linked; unrelated.players[1].startX += 128;
    Require(!BuildTerrainStartLocationsJournalEdit(before, after, settingsBefore, Take(unrelated.Serialize())).Succeeded(),
        "an unmoved player's start cannot piggyback on a real moved start");
    auto forged = edit;
    forged.settings = StartJournalForgedSettingsDelta(Take(CanonicalTerrainStartSettings(settingsBefore)),
        Take(CanonicalTerrainStartSettings(Take(otherPlayerField.Serialize()))));
    Require(ValidateTerrainStartLocationsJournalEdit(forged).Succeeded()
        && !ApplyTerrainStartLocationsJournalEdit(before, settingsBefore, forged).Succeeded(),
        "matching complete before and after SHA do not replace the linked-field semantic check");
    auto duplicate = changed; duplicate.units[1].creationId = duplicate.units[0].creationId;
    Require(!BuildTerrainStartLocationsJournalEdit(before, Take(duplicate.Serialize()), settingsBefore, settingsAfter).Succeeded(),
        "an ordinary unit cannot reuse the moved start's creation identity");
    duplicate = changed; duplicate.units[2].owner = duplicate.units[0].owner;
    Require(!BuildTerrainStartLocationsJournalEdit(before, Take(duplicate.Serialize()), settingsBefore, settingsAfter).Succeeded(),
        "two start locations cannot share one player");
    auto reassigned = changed; reassigned.units[0].owner = 1;
    Require(!BuildTerrainStartLocationsJournalEdit(before, Take(reassigned.Serialize()), settingsBefore, settingsAfter).Succeeded(),
        "an existing start cannot change its owner");
    auto nonfinite = changed; nonfinite.units[0].position[0] = std::numeric_limits<float>::infinity();
    Require(!VerifyTerrainStartLocationsChange(original, nonfinite, settings, linked).Succeeded(),
        "typed start linkage rejects nonfinite coordinates before writer allocation");
    auto excessive = original;
    for (std::int32_t i = 0; i < 17; ++i) {
        auto start = original.units[0]; start.creationId = 1000 + i; excessive.units.push_back(std::move(start));
    }
    Require(!VerifyTerrainStartLocationsChange(excessive, changed, settings, linked).Succeeded(),
        "excessive source starts are rejected before repeated full identity scans");
    auto invalidSource = original; invalidSource.units[2].owner = -1;
    Require(!VerifyTerrainStartLocationsChange(invalidSource, changed, settings, linked).Succeeded(),
        "unchanged invalid source owner cannot obtain a start linkage record");
    auto duplicatePlayer = settings; duplicatePlayer.players[1].number = duplicatePlayer.players[0].number;
    Require(!VerifyTerrainStartLocationsChange(original, changed, duplicatePlayer, linked).Succeeded(),
        "source settings need unambiguous players independently of final hashes");
    auto removed = changed; removed.units.erase(removed.units.begin());
    Require(!BuildTerrainStartLocationsJournalEdit(before, Take(removed.Serialize()), settingsBefore, settingsAfter).Succeeded(),
        "deleting a start cannot be disguised as a start-coordinate update");
    auto ordinaryOnly = original; ordinaryOnly.units[1].hitPoints = 1;
    Require(!BuildTerrainStartLocationsJournalEdit(before, Take(ordinaryOnly.Serialize()), settingsBefore, settingsBefore).Succeeded(),
        "ordinary Units edits use their own variant and cannot mint a start receipt");
    Require(!BuildTerrainStartLocationsJournalEdit(before, before, settingsBefore, settingsAfter).Succeeded(),
        "W3I alone cannot create a start-location record");
    auto tail = settingsBefore; tail.push_back(std::byte{0x7f});
    Require(!CanonicalTerrainStartSettings(tail).Succeeded()
        && !BuildTerrainStartLocationsJournalEdit(before, after, tail, settingsAfter).Succeeded(),
        "unknown W3I tail is preserved by refusing the affected start operation");
    auto wrongVersion = settings; wrongVersion.formatVersion = 31;
    Require(!CanonicalTerrainStartSettings(Take(wrongVersion.Serialize())).Succeeded(), "start recovery does not infer nonclassic W3I layouts");
    auto truncated = settingsBefore; truncated.pop_back();
    Require(!CanonicalTerrainStartSettings(truncated).Succeeded(), "truncated W3I does not become a canonical settings source");
}

void StartLocationsJournalIdentitySettingsRemainStrict() {
    auto original = StartJournalUnits(), changed = original;
    auto settings = StartJournalSettings(); settings.players[0].startX = 512; settings.players[0].startY = 640;
    changed.units[0].position[0] = 512; changed.units[0].position[1] = 640;
    const auto before = Take(original.Serialize()), after = Take(changed.Serialize());
    const auto settingsWire = Take(settings.Serialize()), canonical = Take(CanonicalTerrainStartSettings(settingsWire));
    const auto edit = Take(BuildTerrainStartLocationsJournalEdit(before, after, settingsWire, settingsWire));
    Require(edit.settings.beforeSha256 == edit.settings.afterSha256 && edit.settings.beforeBytes == canonical.size()
        && edit.settings.afterBytes == canonical.size() && edit.settings.prefixBytes == canonical.size()
        && edit.settings.removedBytes == 0 && edit.settings.inserted.empty(),
        "moving an old inconsistent sloc to its existing W3I coordinate uses the one strict identity delta");
    const auto replayed = Take(ApplyTerrainStartLocationsJournalEdit(before, settingsWire, edit));
    Require(replayed.units.OriginalBytes() == after && replayed.settings == canonical,
        "a genuine Units movement may have zero net W3I bytes without losing its compound meaning");
    auto bad = edit; --bad.settings.prefixBytes;
    Require(!ValidateTerrainStartLocationsJournalEdit(bad).Succeeded(), "identity requires the full-member prefix");
    bad = edit; --bad.settings.prefixBytes; bad.settings.removedBytes = 1;
    bad.settings.inserted.push_back(canonical.back());
    Require(!ValidateTerrainStartLocationsJournalEdit(bad).Succeeded(), "identity cannot carry a redundant removed and inserted byte");
    bad = edit; ++bad.settings.afterBytes;
    Require(!ValidateTerrainStartLocationsJournalEdit(bad).Succeeded(), "identity must retain the exact full-member length");
    bad = edit; bad.settings.beforeSha256[0] = 'A'; bad.settings.afterSha256 = bad.settings.beforeSha256;
    Require(!ValidateTerrainStartLocationsJournalEdit(bad).Succeeded(), "identity must still use lowercase complete SHA");
    bad = edit; bad.units.afterSha256 = bad.units.beforeSha256; bad.units.afterBytes = bad.units.beforeBytes;
    bad.units.prefixBytes = bad.units.beforeBytes; bad.units.removedBytes = 0; bad.units.inserted.clear();
    Require(!ValidateTerrainStartLocationsJournalEdit(bad).Succeeded(), "only W3I may be identity; Units must actually change");
    auto stale = settings; stale.players[1].startY += 1;
    Require(!ApplyTerrainStartLocationsJournalEdit(before, Take(stale.Serialize()), edit).Succeeded(),
        "identity settings still compare their entire source rather than skipping CAS");
}

void StartLocationsJournalCanonicalDescriptionsCompose() {
    auto original = StartJournalUnits(), changed = original; changed.units[0].position[0] = 512;
    auto settings = StartJournalSettings(), changedSettings = settings; changedSettings.players[0].startX = 512;
    changedSettings.mapName = "A longer edited map name"; changedSettings.author.clear();
    changedSettings.description = "An independently edited description"; changedSettings.recommendedPlayers = "1-8";
    const auto before = Take(original.Serialize()), after = Take(changed.Serialize());
    const auto source = Take(settings.Serialize()), output = Take(changedSettings.Serialize());
    const auto edit = Take(BuildTerrainStartLocationsJournalEdit(before, after, source, output));
    auto current = settings;
    current.mapName = "Current name with another length"; current.author = "Current author";
    current.description.clear(); current.recommendedPlayers = "Different recommendation";
    const auto replayed = Take(ApplyTerrainStartLocationsJournalEdit(before, Take(current.Serialize()), edit));
    Require(replayed.units.OriginalBytes() == after && replayed.settings == Take(CanonicalTerrainStartSettings(output)),
        "prior or later independent description edits do not make a genuine start record stale");
    const auto canonical = Take(MapSettings::Parse(replayed.settings));
    Require(canonical.mapName.empty() && canonical.author.empty() && canonical.description.empty()
        && canonical.recommendedPlayers.empty() && canonical.players[0].startX == 512
        && canonical.players[0].flags == settings.players[0].flags && canonical.players[1].startY == settings.players[1].startY,
        "canonical output clears exactly the four descriptions and retains unrelated full W3I data");
    current.players[0].race = 4;
    Require(!ApplyTerrainStartLocationsJournalEdit(before, Take(current.Serialize()), edit).Succeeded(),
        "description compatibility does not ignore other player settings");
}

void StartLocationsJournalCodecKeepsClosedFTJAVariant() {
    auto original = StartJournalUnits(), changed = original; changed.units[0].position[0] = 512;
    auto settings = StartJournalSettings(), changedSettings = settings; changedSettings.players[0].startX = 512;
    const auto unitsBefore = Take(original.Serialize()), settingsBefore = Take(settings.Serialize());
    TerrainJournalStroke starts;
    starts.startLocations = Take(BuildTerrainStartLocationsJournalEdit(unitsBefore, Take(changed.Serialize()),
        settingsBefore, Take(changedSettings.Serialize())));
    auto journal = Journal(); journal.strokes.insert(journal.strokes.begin(), starts);
    journal.members[2] = {"war3map.w3i", true, Take(Sha256(settingsBefore)), settingsBefore.size()};
    journal.members[4] = {"war3mapunits.doo", true, Take(Sha256(unitsBefore)), unitsBefore.size()};
    const auto wire = Take(EncodeTerrainStrokeJournal(journal));
    const auto decoded = Take(DecodeTerrainStrokeJournal(wire));
    Require(wire[3] == std::byte{'A'} && decoded.strokes.size() == 2 && decoded.strokes[0].startLocations
        && Take(EncodeTerrainStrokeJournal(decoded)) == wire, "FTJA retains one real compound start followed by terrain in exact order");
    auto identity = journal;
    const auto identitySettings = Take(changedSettings.Serialize());
    identity.members[2] = {"war3map.w3i", true, Take(Sha256(identitySettings)), identitySettings.size()};
    identity.strokes[0].startLocations = Take(BuildTerrainStartLocationsJournalEdit(unitsBefore, Take(changed.Serialize()),
        identitySettings, identitySettings));
    const auto identityWire = Take(EncodeTerrainStrokeJournal(identity));
    const auto identityDecoded = Take(DecodeTerrainStrokeJournal(identityWire));
    Require(identityWire[3] == std::byte{'A'} && identityDecoded.strokes[0].startLocations
        && Take(EncodeTerrainStrokeJournal(identityDecoded)) == identityWire,
        "FTJA roundtrips the strict identity W3I form without weakening the Units variant");
    for (std::size_t size = 0; size < wire.size(); ++size)
        Require(!DecodeTerrainStrokeJournal(std::span(wire).first(size)).Succeeded(), "every FTJA truncation fails closed");
    for (char version = '1'; version <= '9'; ++version) {
        auto wrong = wire; wrong[3] = std::byte(version);
        Require(!DecodeTerrainStrokeJournal(wrong).Succeeded(), "earlier versions cannot acquire the compound start variant");
    }
    auto wrong = wire; wrong[3] = std::byte{':'};
    Require(!DecodeTerrainStrokeJournal(wrong).Succeeded(), "the old unknown colon magic remains invalid");
    auto noStarts = Take(EncodeTerrainStrokeJournal(Journal())); noStarts[3] = std::byte{'A'};
    Require(!DecodeTerrainStrokeJournal(noStarts).Succeeded(), "FTJA must actually contain a start-location record");
    auto invalid = journal; invalid.strokes[0].intent.points = {{256, 256}};
    Require(!EncodeTerrainStrokeJournal(invalid).Succeeded(), "start record cannot carry brush sampling points");
    invalid = journal; invalid.strokes[0].units = invalid.strokes[0].startLocations->units;
    Require(!EncodeTerrainStrokeJournal(invalid).Succeeded(), "start and ordinary Units remain exclusive variants");
    invalid = journal; invalid.strokes[0].rulesSha256.assign(64, 'c');
    Require(!EncodeTerrainStrokeJournal(invalid).Succeeded(), "a start command cannot borrow a terrain resource owner");
    invalid = journal; invalid.members[2] = {"war3map.w3i", false, {}, 0};
    Require(!EncodeTerrainStrokeJournal(invalid).Succeeded(), "compound recovery needs the actual W3I source member");
    invalid = journal; invalid.members[4] = {"war3mapunits.doo", false, {}, 0};
    Require(!EncodeTerrainStrokeJournal(invalid).Succeeded(), "compound recovery needs the actual Units source member");
    wrong = wire; wrong.push_back(std::byte{0});
    Require(!DecodeTerrainStrokeJournal(wrong).Succeeded(), "FTJA cannot ignore an unknown trailing extension");
}
