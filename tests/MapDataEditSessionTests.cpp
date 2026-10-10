#include "War3MapCore/MapData/MapDataEditSession.h"
#include "MapDataFixtures.h"

#include <algorithm>
#include <bit>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace {

using namespace War3Map;
using namespace War3Map::MapData;

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::vector<std::byte> RegionBytes() {
    std::vector<std::byte> bytes;
    const auto number = [&bytes](std::uint32_t value) {
        for (unsigned shift = 0; shift < 32; shift += 8)
            bytes.push_back(static_cast<std::byte>((value >> shift) & 0xffu));
    };
    const auto text = [&bytes](std::string_view value) {
        for (const auto c : value) bytes.push_back(static_cast<std::byte>(c));
        bytes.push_back(std::byte{0});
    };
    number(5); number(1);
    number(std::bit_cast<std::uint32_t>(-10.0f));
    number(std::bit_cast<std::uint32_t>(-20.0f));
    number(std::bit_cast<std::uint32_t>(30.0f));
    number(std::bit_cast<std::uint32_t>(40.0f));
    text("Original Region"); number(1);
    for (const auto c : std::string_view("RAhr")) bytes.push_back(static_cast<std::byte>(c));
    text("");
    for (unsigned i = 0; i < 4; ++i) bytes.push_back(std::byte{0});
    return bytes;
}

void EditsUndoRedoAndFailureDoNotChangeSource() {
    const auto settingsBytes = MapDataFixtures::FullMapSettingsV25().bytes;
    const auto regionsBytes = RegionBytes();
    const std::vector<MapArchiveMember> members{{"war3map.w3i", settingsBytes}, {"war3map.w3r", regionsBytes}};
    const auto source = MapDocument::FromMembers(members);
    Require(source.Settings().has_value() && source.Regions().has_value(), "source fixture must parse");
    MapDataEditSession edits(source);
    auto settings = *source.Settings();
    settings.mapName = "Changed map";
    Require(edits.EditSettings(settings).Succeeded(), "settings edit should be accepted");
    auto regions = *source.Regions();
    regions.entries[0].name = "Changed Region";
    Require(edits.EditRegions(regions).Succeeded(), "region edit should be accepted");
    auto replacements = edits.Replacements();
    Require(edits.IsDirty() && replacements.size() == 2, "two independent members must be replaced");
    Require(replacements[0].name == "war3map.w3i" && replacements[1].name == "war3map.w3r",
        "member names must remain stable");
    Require(MapSettings::Parse(replacements[0].data).value->mapName == "Changed map"
        && ParseRegions(replacements[1].data).value->entries[0].name == "Changed Region",
        "replacement bytes must contain actual typed edits");
    Require(members[0].data == settingsBytes && members[1].data == regionsBytes,
        "source member bytes must remain read-only");
    const auto revision = edits.Revision();
    regions.entries[0].left = std::numeric_limits<float>::quiet_NaN();
    Require(!edits.EditRegions(regions).Succeeded() && edits.Revision() == revision,
        "invalid edit must preserve the previous replacement and revision");
    Require(edits.Undo() && edits.Replacements().size() == 1, "undo should restore prior member state");
    Require(edits.Undo() && !edits.IsDirty(), "second undo should return to exact source");
    Require(edits.Redo() && edits.Redo() && edits.Replacements().size() == 2,
        "redo should restore both complete replacements");
    Require(edits.EditSettings(*source.Settings()).Succeeded(), "reverting one member to source should be accepted");
    Require(edits.Replacements().size() == 1 && edits.Replacements()[0].name == "war3map.w3r",
        "unchanged member must not be placed in replacement set");
    const auto snapshot = edits.Snapshot();
    Require(snapshot.Settings() && snapshot.Settings()->mapName == source.Settings()->mapName
        && snapshot.Regions() && snapshot.Regions()->entries[0].name == "Changed Region",
        "editable snapshot must reflect current pending members");
    edits.CommitSaved(snapshot);
    Require(!edits.IsDirty(), "verified save must update the baseline");
    Require(edits.Undo() && edits.IsDirty(), "undo after save must be a new change relative to saved bytes");
    Require(edits.Redo() && !edits.IsDirty(), "redo after save must return to saved bytes");
    Require(edits.Remove(MapDataKind::Regions).Succeeded() && edits.Removals().size() == 1,
        "removing an optional member produces an explicit removal");
    Require(edits.Undo() && edits.Removals().empty(), "removal undo restores the member");
    Require(!edits.Remove(MapDataKind::Settings).Succeeded(), "required settings cannot be removed");
}

void AmbiguousSourceIsNotEditable() {
    const auto settingsBytes = MapDataFixtures::FullMapSettingsV25().bytes;
    const std::vector<MapArchiveMember> members{{"war3map.w3i", settingsBytes}, {"WAR3MAP.W3I", settingsBytes}};
    const auto source = MapDocument::FromMembers(members);
    MapDataEditSession edits(source);
    auto settings = MapSettings::Parse(settingsBytes);
    settings.value->mapName = "Unsafe";
    Require(!edits.EditSettings(*settings.value).Succeeded(), "ambiguous source cannot be overwritten");
    Require(!edits.IsDirty(), "failed edit must not create an overlay");
}

void DuplicateEditedIdentityIsRejected() {
    const auto settingsBytes = MapDataFixtures::FullMapSettingsV25().bytes;
    const std::vector<MapArchiveMember> members{{"war3map.w3i", settingsBytes}};
    const auto source = MapDocument::FromMembers(members);
    MapDataEditSession edits(source);
    auto settings = *source.Settings();
    settings.players[1].number = settings.players[0].number;
    Require(!edits.EditSettings(settings).Succeeded() && !edits.IsDirty(),
        "duplicate edited player identity must not create a pending replacement");
}

// 来源里的未知控制者/种族原样保留；改动过的值和新增玩家只能写入已知取值，失败不产生覆盖。
void PlayerEnumsKeepUnknownSourceButRejectNewUnknown() {
    auto typed = MapSettings::Parse(MapDataFixtures::FullMapSettingsV25().bytes);
    Require(typed.Succeeded() && typed.value && typed.value->players.size() >= 2, "fixture needs two players");
    typed.value->players[0].controller = 7;
    const auto bytes = typed.value->Serialize();
    Require(bytes.Succeeded() && bytes.value, "unknown controller must serialize");
    const std::vector<MapArchiveMember> members{{"war3map.w3i", *bytes.value}};
    const auto source = MapDocument::FromMembers(members);
    Require(source.Settings() && source.Settings()->players[0].controller == 7, "unknown source value must parse");
    MapDataEditSession edits(source);
    auto settings = *source.Settings();
    settings.mapName = "Unknown kept";
    Require(edits.EditSettings(settings).Succeeded(), "unrelated edit must keep the unknown source controller");
    Require(MapSettings::Parse(edits.Replacements()[0].data).value->players[0].controller == 7,
        "unknown source controller must be written back unchanged");
    const auto revision = edits.Revision();
    const auto expectFailure = [&](MapSettings candidate, const char* code, const char* message) {
        const auto status = edits.EditSettings(candidate);
        Require(!status.Succeeded() && !status.diagnostics.empty() && status.diagnostics.front().code == code
            && edits.Revision() == revision
            && MapSettings::Parse(edits.Replacements()[0].data).value->mapName == "Unknown kept", message);
    };
    auto changed = settings;
    changed.players[0].controller = 99;
    expectFailure(changed, "MAP_PLAYER_CONTROLLER_INVALID", "changing to another unknown controller must fail");
    changed = settings;
    changed.players[1].race = -1;
    expectFailure(changed, "MAP_PLAYER_RACE_INVALID", "unknown race must not be written");
    changed = settings;
    changed.players.push_back(changed.players[1]);
    changed.players.back().number = 11;
    changed.players.back().controller = 0;
    expectFailure(changed, "MAP_PLAYER_CONTROLLER_INVALID", "a new player must use a known controller");
    changed = settings;
    changed.players[0].controller = 2;
    changed.players[1].race = 0;
    Require(edits.EditSettings(changed).Succeeded()
            && MapSettings::Parse(edits.Replacements()[0].data).value->players[0].controller == 2,
        "replacing an unknown controller with a known one must succeed");
}

void NewOptionalMemberCanBeUndoneWithoutRemoval() {
    const auto source = MapDocument::FromMembers({});
    MapDataEditSession edits(source);
    std::vector<std::byte> emptySounds;
    const auto append = [&emptySounds](std::uint32_t value) {
        for (unsigned shift = 0; shift < 32; shift += 8)
            emptySounds.push_back(static_cast<std::byte>((value >> shift) & 0xffu));
    };
    append(1); append(0);
    const auto sounds = ParseSounds(emptySounds);
    Require(sounds.Succeeded(), "empty W3S fixture must parse");
    Require(edits.EditSounds(*sounds.value).Succeeded() && edits.Replacements().size() == 1,
        "new optional sound member should be a replacement");
    Require(edits.Remove(MapDataKind::Sounds).Succeeded() && !edits.IsDirty()
        && edits.Removals().empty(), "discarding unsaved optional member restores absent source");
    Require(edits.Undo() && edits.IsDirty() && edits.Replacements().size() == 1,
        "undo should restore the new optional sound member");
}

std::vector<MapArchiveMember> StartLocationMembers(bool duplicateStart = false) {
    auto units = ParseUnitPlacements(MapDataFixtures::EmptyUnitsV8Sub11());
    Require(units.Succeeded() && units.value, "empty units fixture must parse");
    UnitPlacement start;
    start.rawcode = "sloc";
    start.creationId = 10;
    start.owner = 0;
    start.position = {100.0f, 200.0f, 64.0f};
    start.scale = {1, 1, 1};
    units.value->units.push_back(start);
    start.rawcode = duplicateStart ? "sloc" : "hfoo";
    start.creationId = 20;
    start.owner = duplicateStart ? 0 : 1;
    start.position = {300.0f, 400.0f, 0.0f};
    units.value->units.push_back(start);
    auto bytes = units.value->Serialize();
    Require(bytes.Succeeded() && bytes.value, "start units fixture must serialize");
    // W3I 起点刻意与 sloc 不同，撤销必须保留来源而非猜测应该以哪一份为准。
    return {{"war3map.w3i", MapDataFixtures::FullMapSettingsV25().bytes},
        {"war3mapUnits.doo", std::move(*bytes.value)}};
}

void StartLocationsCommitAndReplayAsOneTransaction() {
    const auto members = StartLocationMembers();
    const auto source = MapDocument::FromMembers(members);
    MapDataEditSession edits(source);
    auto units = *source.Units();
    units.units[0].position[0] = 512.0f;
    units.units[0].position[1] = 640.0f;
    Require(edits.EditUnits(units).Succeeded(), "moving a start location must succeed");
    const auto receipt = edits.LastEdit();
    const auto moved = edits.Snapshot();
    Require(edits.Replacements().size() == 2 && edits.Revision() == 1,
        "moving sloc must commit units and settings in one revision");
    Require(moved.Settings()->players[0].startX == 512.0f && moved.Settings()->players[0].startY == 640.0f,
        "runtime player start coordinates must follow sloc");
    Require(moved.Units()->units[0].position[2] == 64.0f
        && moved.Settings()->players[1].startX == source.Settings()->players[1].startX
        && moved.Settings()->flags == source.Settings()->flags,
        "linked edit must preserve height, other players and opaque flags");
    Require(edits.Undo() && !edits.IsDirty() && !edits.CanUndo(),
        "one undo must restore both original inconsistent members exactly");
    Require(edits.Redo() && edits.Replacements().size() == 2, "one redo must restore both linked members");
    edits.CommitSaved(edits.Snapshot());
    Require(edits.Replay(receipt, false).Succeeded(), "receipt undo must survive a saved baseline");
    const auto undone = edits.Snapshot();
    Require(undone.Settings()->Serialize().value == source.Settings()->Serialize().value
        && undone.Units()->Serialize().value == source.Units()->Serialize().value,
        "receipt undo must restore source bytes, not recompute inconsistent old coordinates");
    Require(edits.Replay(receipt, true).Succeeded() && !edits.IsDirty(),
        "receipt redo must return to the verified saved baseline");
    auto settings = *edits.Snapshot().Settings();
    settings.author = "Concurrent author edit";
    Require(edits.EditSettings(settings).Succeeded(), "unrelated settings edit must succeed");
    const auto revision = edits.Revision();
    Require(!edits.Replay(receipt, false).Succeeded() && edits.Revision() == revision
        && edits.Snapshot().Units()->units[0].position[0] == 512.0f,
        "stale linked receipt must reject the entire replay before changing units");
    MapDataEditSession other(source);
    Require(!other.Replay(receipt, false).Succeeded() && !other.IsDirty(),
        "a receipt cannot be applied to another map session");
}

void SettingsStartEditsLinkBackWithoutTouchingUnrelatedPlacements() {
    const auto source = MapDocument::FromMembers(StartLocationMembers());
    MapDataEditSession edits(source);
    auto settings = *source.Settings();
    settings.players[0].startX = 900.0f;
    settings.players[0].startY = 1000.0f;
    Require(edits.EditSettings(settings).Succeeded(), "editing player coordinates must succeed");
    auto current = edits.Snapshot();
    Require(current.Units()->units[0].position[0] == 900.0f
        && current.Units()->units[0].position[1] == 1000.0f
        && current.Units()->units[1].position == source.Units()->units[1].position,
        "settings coordinates must update only the matching sloc");
    Require(edits.Undo() && !edits.IsDirty(), "settings undo must restore both exact original members");
    auto units = *source.Units();
    units.units[1].position[0] = 600.0f;
    Require(edits.EditUnits(units).Succeeded() && edits.Replacements().size() == 1,
        "moving an ordinary unit must not normalize a preexisting inconsistent start");
    Require(edits.Snapshot().Settings()->players[0].startX == source.Settings()->players[0].startX,
        "unrelated unit edit must preserve original settings");
    const std::vector<MapArchiveMember> onlySettings{{"war3map.w3i", MapDataFixtures::FullMapSettingsV25().bytes}};
    MapDataEditSession noPlacements(MapDocument::FromMembers(onlySettings));
    Require(noPlacements.EditSettings(settings).Succeeded() && noPlacements.Replacements().size() == 1,
        "maps without a placement member may still edit player settings");
}

void UnsafeStartAssociationsFailWithoutPartialEdits() {
    const auto source = MapDocument::FromMembers(StartLocationMembers());
    MapDataEditSession edits(source);
    auto units = *source.Units();
    units.units[0].owner = 1;
    auto status = edits.EditUnits(units);
    Require(!status.Succeeded() && status.diagnostics.front().code == "MAP_START_OWNER_CHANGE_UNSUPPORTED"
        && !edits.IsDirty() && edits.Revision() == 0, "start owner changes must fail explicitly and atomically");
    units = *source.Units();
    units.units.erase(units.units.begin());
    Require(!edits.EditUnits(units).Succeeded() && !edits.IsDirty(), "start deletion must not silently orphan settings");
    auto settings = *source.Settings();
    settings.players[0].number = 4;
    Require(!edits.EditSettings(settings).Succeeded() && !edits.IsDirty(), "bound player number changes must fail");

    const auto duplicate = MapDocument::FromMembers(StartLocationMembers(true));
    MapDataEditSession ambiguous(duplicate);
    units = *duplicate.Units();
    units.units[0].position[0] = 777.0f;
    status = ambiguous.EditUnits(units);
    Require(!status.Succeeded() && status.diagnostics.front().code == "MAP_START_AMBIGUOUS"
        && !ambiguous.IsDirty(), "multiple starts for one player must reject movement");
    settings = *duplicate.Settings();
    settings.players[0].startX = 777.0f;
    Require(!ambiguous.EditSettings(settings).Succeeded() && !ambiguous.IsDirty(),
        "multiple starts must also reject player-coordinate edits");

    auto missing = StartLocationMembers();
    missing.erase(missing.begin());
    MapDataEditSession noSettings(MapDocument::FromMembers(missing));
    units = *source.Units();
    units.units[0].position[0] = 777.0f;
    Require(!noSettings.EditUnits(units).Succeeded() && !noSettings.IsDirty(),
        "missing settings must reject linked movement without storing unit bytes");
    units = *source.Units();
    units.units[1].position[0] = 777.0f;
    Require(noSettings.EditUnits(units).Succeeded(), "missing settings must not block ordinary unit edits");

    settings = *source.Settings();
    settings.players.clear();
    auto noPlayer = StartLocationMembers();
    noPlayer[0].data = *settings.Serialize().value;
    MapDataEditSession noMatchingPlayer(MapDocument::FromMembers(noPlayer));
    units = *source.Units();
    units.units[0].position[0] = 777.0f;
    status = noMatchingPlayer.EditUnits(units);
    Require(!status.Succeeded() && status.diagnostics.front().code == "MAP_START_PLAYER_UNAVAILABLE"
        && !noMatchingPlayer.IsDirty(), "missing matching player must reject the transaction");
}

void SerializedRecoveryPreservesBytesAndInteractiveEditsLink() {
    const auto source = MapDocument::FromMembers(StartLocationMembers());
    MapDataEditSession edits(source);
    auto units = *source.Units();
    units.units[0].position[0] = 333.0f;
    auto bytes = units.Serialize();
    Require(edits.SetSerialized(MapDataKind::Units, *bytes.value).Succeeded()
        && edits.Replacements().size() == 1, "project recovery must restore exact supplied members without derived writes");
    Require(edits.Snapshot().Settings()->players[0].startX == source.Settings()->players[0].startX,
        "recovery order must not rewrite unrelated serialized project settings");
    units.units[0].position[0] = 444.0f;
    bytes = units.Serialize();
    Require(edits.EditSerialized(MapDataKind::Units, *bytes.value).Succeeded()
        && edits.Snapshot().Settings()->players[0].startX == 444.0f,
        "interactive serialized edits must use the same linked transaction as typed edits");
}

std::vector<std::byte> SmallTerrain() {
    std::vector<std::byte> bytes;
    const auto u8 = [&](std::uint8_t value) { bytes.push_back(static_cast<std::byte>(value)); };
    const auto u16 = [&](std::uint16_t value) { u8(static_cast<std::uint8_t>(value)); u8(static_cast<std::uint8_t>(value >> 8)); };
    const auto u32 = [&](std::uint32_t value) { for (unsigned shift = 0; shift < 32; shift += 8) u8(static_cast<std::uint8_t>(value >> shift)); };
    for (char c : std::string_view("W3E!")) u8(static_cast<std::uint8_t>(c));
    u32(11); u8('L'); u32(0);
    u32(1); for (char c : std::string_view("Ngrs")) u8(static_cast<std::uint8_t>(c));
    u32(1); for (char c : std::string_view("CLdi")) u8(static_cast<std::uint8_t>(c));
    u32(5); u32(5); u32(0); u32(0);
    for (int i = 0; i < 25; ++i) { u16(8192); u16(8192); u8(0); u8(0); u8(2); }
    return bytes;
}

std::vector<std::byte> SmallPathing() {
    std::vector<std::byte> bytes;
    const auto u8 = [&](std::uint8_t value) { bytes.push_back(static_cast<std::byte>(value)); };
    const auto u32 = [&](std::uint32_t value) { for (unsigned shift = 0; shift < 32; shift += 8) u8(static_cast<std::uint8_t>(value >> shift)); };
    for (char c : std::string_view("MP3W")) u8(static_cast<std::uint8_t>(c));
    u32(0); u32(16); u32(16);
    for (std::uint32_t y = 0; y < 16; ++y)
        for (std::uint32_t x = 0; x < 16; ++x) u8(static_cast<std::uint8_t>((x * 17 + y * 31) & ~0x20u));
    return bytes;
}

const std::vector<std::byte>& Replacement(std::string_view name, const std::vector<MapArchiveMember>& replacements) {
    const auto found = std::find_if(replacements.begin(), replacements.end(), [&](const auto& member) { return member.name == name; });
    Require(found != replacements.end(), "expected replacement member is present");
    return found->data;
}

void BlightPathingUsesOneTransactionAndPreservesUnknownBits() {
    const auto terrain = SmallTerrain(), pathing = SmallPathing();
    MapDataEditSession missing(MapDocument::FromMembers({}));
    Require(missing.AttachTerrain(terrain).Succeeded(), "source terrain attaches");
    Require(missing.VerifyTerrainPathingForSave().Succeeded(), "unchanged terrain needs no WPM reconstruction");
    TerrainBrush brush;
    brush.kind = TerrainBrushKind::Blight; brush.radius = 0; brush.value = true;
    const TerrainStrokePoint center{256, 256}; // W3E 顶点 (2,2)，对应 WPM [6,10)×[6,10)。
    Require(!missing.EditTerrain(brush, std::span(&center, 1)).Succeeded() && !missing.IsDirty(),
        "a missing source WPM rejects blight without changing W3E");
    MapDataEditSession edits(MapDocument::FromMembers({}));
    Require(edits.AttachTerrain(terrain).Succeeded() && edits.AttachPathing(pathing).Succeeded(), "source terrain and WPM attach");
    Require(edits.EditTerrain(brush, std::span(&center, 1)).Succeeded(), "blight stroke succeeds with source WPM");
    Require(edits.VerifyTerrainPathingForSave().Succeeded(), "linked blight edit passes the shared save gate");
    const auto receipt = edits.LastEdit();
    const auto changed = edits.Replacements();
    Require(changed.size() == 2 && edits.Revision() == 1, "one revision includes W3E and WPM");
    const auto editedTerrain = Replacement("war3map.w3e", changed);
    const auto editedPathing = Replacement("war3map.wpm", changed);
    for (std::uint32_t y = 0; y < 16; ++y) {
        for (std::uint32_t x = 0; x < 16; ++x) {
            const auto i = 16 + y * 16 + x;
            const auto delta = std::to_integer<std::uint8_t>(editedPathing[i] ^ pathing[i]);
            Require(delta == ((x >= 6 && x < 10 && y >= 6 && y < 10) ? 0x20 : 0),
                "only the nearest-vertex 4x4 blight bit changes");
        }
    }
    Require(edits.Undo() && !edits.IsDirty() && edits.Replacements().empty(), "one undo restores W3E and WPM exactly");
    Require(edits.Redo() && edits.Replacements().size() == 2, "one redo restores both members");
    Require(edits.Replay(receipt, false).Succeeded() && !edits.IsDirty(), "receipt replay undoes both members");
    Require(edits.Replay(receipt, true).Succeeded() && edits.IsDirty(), "receipt replay redoes both members");
    edits.CommitSaved(edits.Snapshot());
    Require(!edits.IsDirty(), "verified save advances both baselines");

    MapDataEditSession restored(MapDocument::FromMembers({}));
    Require(restored.AttachTerrain(terrain).Succeeded() && restored.AttachPathing(pathing).Succeeded(), "project sources attach");
    auto invalidPair = editedPathing;
    invalidPair[16 + 7 * 16 + 7] ^= std::byte{0x01};
    Require(!restored.SetTerrainAndPathing(editedTerrain, invalidPair).Succeeded()
        && restored.Revision() == 0 && !restored.IsDirty() && restored.Replacements().empty(),
        "an invalid WPM rejects both project overlays without a partial terrain edit");
    auto invalidTerrain = editedTerrain;
    invalidTerrain.pop_back();
    Require(!restored.SetTerrainAndPathing(invalidTerrain, editedPathing).Succeeded()
        && restored.Revision() == 0 && !restored.IsDirty(),
        "an invalid W3E also leaves both project members untouched");
    Require(restored.SetTerrainAndPathing(editedTerrain, editedPathing).Succeeded() && restored.Revision() == 1,
        "project W3E and WPM overlays restore in one command");
    Require(restored.Undo() && !restored.IsDirty() && restored.Replacements().empty()
        && restored.Redo() && restored.Replacements().size() == 2,
        "one undo and redo restore the linked project overlays together");
    Require(restored.VerifyTerrainPathingForSave().Succeeded(), "restored linked overlays pass the save gate");
    Require(restored.Replacements().size() == 2, "restored project retains both byte overlays");
    auto unsupported = editedPathing;
    unsupported[16 + 7 * 16 + 7] ^= std::byte{0x01};
    const auto revision = restored.Revision();
    Require(!restored.SetPathing(unsupported).Succeeded() && restored.Revision() == revision,
        "project restore cannot rewrite an unknown WPM bit");
    unsupported = editedPathing;
    unsupported[16 + 7 * 16 + 7] ^= std::byte{0x20};
    Require(!restored.SetPathing(unsupported).Succeeded() && restored.Revision() == revision,
        "project restore cannot supply blight inconsistent with W3E");
}

void MismatchedPathingBaselineFailsClosed() {
    const auto terrain = SmallTerrain();
    auto pathing = SmallPathing();
    pathing[16 + 7 * 16 + 7] ^= std::byte{0x20};
    MapDataEditSession edits(MapDocument::FromMembers({}));
    Require(edits.AttachTerrain(terrain).Succeeded() && edits.AttachPathing(pathing).Succeeded(), "mismatched source still parses");
    TerrainBrush brush;
    brush.kind = TerrainBrushKind::Blight; brush.radius = 0; brush.value = true;
    const TerrainStrokePoint center{256, 256};
    const auto result = edits.EditTerrain(brush, std::span(&center, 1));
    Require(!result.Succeeded() && result.diagnostics.front().code == "MAP_PATHING_BLIGHT_MISMATCH"
        && edits.Revision() == 0 && !edits.IsDirty(), "mismatched source bit rejects both edits atomically");
    brush.kind = TerrainBrushKind::Raise; brush.strength = 16;
    Require(edits.EditTerrain(brush, std::span(&center, 1)).Succeeded(),
        "unrelated height edits remain available even when WPM blight bit is mismatched");
    Require(edits.VerifyTerrainPathingForSave().Succeeded(),
        "verified dry height leaves even a mismatched source WPM byte unchanged");
}

void SaveGateKeepsRemoteMismatchAndRejectsUnpairedMembers() {
    const auto terrain = SmallTerrain();
    auto pathing = SmallPathing();
    pathing[16] ^= std::byte{0x20}; // 远处既有来源不一致；此次笔刷不得改写该格。
    MapDataEditSession edits(MapDocument::FromMembers({}));
    Require(edits.AttachTerrain(terrain).Succeeded() && edits.AttachPathing(pathing).Succeeded(), "source members attach");
    TerrainBrush brush;
    brush.kind = TerrainBrushKind::Blight; brush.radius = 0; brush.value = true;
    const TerrainStrokePoint center{256, 256};
    Require(edits.EditTerrain(brush, std::span(&center, 1)).Succeeded()
        && edits.VerifyTerrainPathingForSave().Succeeded(), "remote source mismatch stays untouched and does not block local blight");
    const auto changed = edits.Replacements();
    const auto& editedTerrain = Replacement("war3map.w3e", changed);
    const auto& editedPathing = Replacement("war3map.wpm", changed);
    Require(editedPathing[16] == pathing[16], "remote WPM byte remains exact");

    MapDataEditSession restored(MapDocument::FromMembers({}));
    Require(restored.AttachTerrain(terrain).Succeeded() && restored.AttachPathing(pathing).Succeeded(), "project sources attach");
    Require(restored.SetTerrainAndPathing(editedTerrain, editedPathing).Succeeded()
        && restored.VerifyTerrainPathingForSave().Succeeded(), "project restore accepts the bounded WPM edit with remote mismatch");
    auto extra = editedPathing;
    extra[16] ^= std::byte{0x20};
    Require(!restored.SetPathing(extra).Succeeded(), "project restore rejects changes to an untouched WPM cell");
    Require(restored.SetTerrain(terrain).Succeeded(), "project can revert its W3E overlay independently");
    const auto blocked = restored.VerifyTerrainPathingForSave();
    Require(!blocked.Succeeded() && blocked.diagnostics.front().code == "MAP_TERRAIN_PATHING_UNVERIFIED",
        "a WPM-only delta cannot pass the formal save gate");
}

void DryHeightGatePreservesProjectRecovery() {
    const auto terrain = SmallTerrain(), pathing = SmallPathing();
    MapDataEditSession draft(MapDocument::FromMembers({}));
    Require(draft.AttachTerrain(terrain).Succeeded() && draft.AttachPathing(pathing).Succeeded(), "draft sources attach");
    TerrainBrush raise;
    raise.kind = TerrainBrushKind::Raise; raise.radius = 0; raise.strength = 32;
    const TerrainStrokePoint center{256, 256};
    Require(draft.EditTerrain(raise, std::span(&center, 1)).Succeeded(), "an unfinished height stroke remains editable");
    const auto overlays = draft.Replacements();
    Require(overlays.size() == 1 && overlays[0].name == "war3map.w3e", "height draft leaves source WPM unchanged");
    MapDataEditSession reopened(MapDocument::FromMembers({}));
    Require(reopened.AttachTerrain(terrain).Succeeded() && reopened.AttachPathing(pathing).Succeeded(), "reopened sources attach");
    Require(reopened.SetTerrainAndPathing(overlays[0].data, pathing).Succeeded()
        && reopened.IsDirty() && reopened.Revision() == 1,
        "joint project recovery keeps a structurally valid height draft and unchanged WPM");
    const auto gate = reopened.VerifyTerrainPathingForSave();
    Require(gate.Succeeded(), "the shared save gate accepts a recovered dry-height overlay by its raw effect");
    raise.strength = 16;
    Require(reopened.EditTerrain(raise, std::span(&center, 1)).Succeeded()
        && !reopened.VerifyTerrainPathingForSave().Succeeded(),
        "successive strokes cannot exceed raw delta 156 from the source baseline");
    Require(reopened.Undo() && reopened.VerifyTerrainPathingForSave().Succeeded(),
        "undo restores the bounded height overlay");
    Require(reopened.Undo() && !reopened.IsDirty() && reopened.Redo() && reopened.IsDirty(),
        "the recovered draft remains one undoable command");
}

void PureHeightSaveGateKeepsUnverifiedTerrainClosed() {
    const auto terrain = SmallTerrain(), pathing = SmallPathing();
    const TerrainStrokePoint center{256, 256};
    TerrainBrush brush;
    brush.kind = TerrainBrushKind::Raise; brush.radius = 0; brush.strength = 32;
    auto parsed = ParseMapTerrain(terrain);
    Require(parsed.Succeeded() && parsed.value, "terrain fixture parses");
    auto grid = *parsed.value->Snapshot();
    grid.vertices[2 * 5 + 3].textureAndFlagsRaw |= 0x40;
    auto wet = SerializeMapTerrain(grid);
    Require(wet.Succeeded() && wet.value, "nearby water fixture serializes");
    MapDataEditSession wetHeight(MapDocument::FromMembers({}));
    Require(wetHeight.AttachTerrain(*wet.value).Succeeded() && wetHeight.AttachPathing(pathing).Succeeded(),
        "wet-neighbor source attaches");
    Require(wetHeight.EditTerrain(brush, std::span(&center, 1)).Succeeded()
        && !wetHeight.VerifyTerrainPathingForSave().Succeeded(),
        "water one vertex outside the changed point rejects height save");
    auto doodads = ParseDoodadPlacements(MapDataFixtures::EmptyDoodadsV8Sub11());
    Require(doodads.Succeeded() && doodads.value, "doodad fixture parses");
    DoodadPlacement tree;
    tree.rawcode = "LTlt"; tree.creationId = 1; tree.flags = 2;
    tree.position = {256, 256, 10}; tree.scale = {1, 1, 1};
    doodads.value->doodads.push_back(tree);
    auto doodadBytes = doodads.value->Serialize();
    Require(doodadBytes.Succeeded() && doodadBytes.value, "doodad fixture serializes");
    const std::vector<MapArchiveMember> doodadMembers{{"war3map.doo", *doodadBytes.value}};
    MapDataEditSession linked(MapDocument::FromMembers(doodadMembers));
    Require(linked.AttachTerrain(terrain).Succeeded() && linked.AttachPathing(pathing).Succeeded(),
        "doodad-linked source attaches");
    Require(linked.EditTerrain(brush, std::span(&center, 1), true).Succeeded()
        && linked.Replacements().size() == 2 && !linked.VerifyTerrainPathingForSave().Succeeded(),
        "height and doodad relocation stay editable as a draft but cannot save yet");
}

} // namespace

int main() {
    try {
        EditsUndoRedoAndFailureDoNotChangeSource();
        AmbiguousSourceIsNotEditable();
        DuplicateEditedIdentityIsRejected();
        PlayerEnumsKeepUnknownSourceButRejectNewUnknown();
        NewOptionalMemberCanBeUndoneWithoutRemoval();
        StartLocationsCommitAndReplayAsOneTransaction();
        SettingsStartEditsLinkBackWithoutTouchingUnrelatedPlacements();
        UnsafeStartAssociationsFailWithoutPartialEdits();
        SerializedRecoveryPreservesBytesAndInteractiveEditsLink();
        BlightPathingUsesOneTransactionAndPreservesUnknownBits();
        MismatchedPathingBaselineFailsClosed();
        SaveGateKeepsRemoteMismatchAndRejectsUnpairedMembers();
        DryHeightGatePreservesProjectRecovery();
        PureHeightSaveGateKeepsUnverifiedTerrainClosed();
        std::cout << "MapDataEditSession: 14 scenario groups passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
