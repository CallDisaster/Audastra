#include "War3MapCore/MapData/MapSettings.h"
#include "War3MapCore/MapData/MapFields.h"
#include "MapDataFixtures.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using War3Map::MapData::MapSettings;
using War3Map::MapData::SettingsRawcode;

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

using MapDataFixtures::Fixture;
using MapDataFixtures::FullMapSettingsV25;

SettingsRawcode Rawcode(std::string_view text) {
    Require(text.size() == 4, "expected rawcode must contain four bytes");
    return {static_cast<std::byte>(text[0]), static_cast<std::byte>(text[1]),
        static_cast<std::byte>(text[2]), static_cast<std::byte>(text[3])};
}

bool HasCode(const War3Map::Result<MapSettings>& result, std::string_view code) {
    return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
        [&](const auto& diagnostic) { return diagnostic.code == code; });
}

void TestCompleteClassicData() {
    const auto fixture = FullMapSettingsV25();
    const auto parsed = MapSettings::Parse(fixture.bytes);
    Require(parsed.Succeeded() && parsed.value->FullyParsed(), "complete TFT data must fully parse");
    Require(parsed.diagnostics.empty(), "valid fixture must not produce guessed warnings");
    const auto& map = *parsed.value;
    Require(map.formatVersion == 25 && map.saves == 7 && map.editorVersion == 6061,
        "TFT version header must parse");
    Require(map.mapName == std::string("Map-") + static_cast<char>(0xe9)
        && map.description == "TRIGSTR_0042", "raw string bytes and WTS references must be retained");
    Require(map.cameraBounds[0] == -1000.5f && map.cameraBounds[7] == 791.5f,
        "all eight camera bounds must parse in file order");
    Require(map.cameraMargins == std::array<std::int32_t, 4>{6, 7, 8, 9}
        && map.playableWidth == 116 && map.playableHeight == 112, "map dimensions must remain distinct");
    Require(map.flags == 0x81234567 && map.tileset == 'L', "unknown flag bits must remain unchanged");
    Require(map.loadingBackground == -1 && map.loadingScreen.path == "Loading\\Scene.mdx"
        && map.loadingScreen.subtitle == "Loading subtitle" && map.gameDataSet == 1
        && map.prologueScreen.title == "Prologue title", "screen and dataset fields must align");
    Require(map.environment.fogStyle == 2 && map.environment.fogStart == 1000.0f
        && map.environment.fogEnd == 5000.0f && map.environment.fogDensity == 0.75f,
        "fog settings must parse");
    Require(map.environment.fogColor.blue == 10 && map.environment.fogColor.red == 30
        && map.environment.waterColor.green == 60 && map.environment.waterColor.alpha == 80,
        "BGRA channel order must be explicit");
    Require(map.environment.weather == Rawcode("RAhr") && map.environment.soundEnvironment == "Default"
        && map.environment.lightEnvironment == 'F', "weather rawcode must not be endian-swapped");
    Require(map.players.size() == 2 && map.players[0].startX == -512.5f
        && map.players[1].controller == 2 && map.players[1].allyHighPriorityMask == 1,
        "player slots and initial positions must parse");
    Require(map.forces.size() == 1 && map.forces[0].playerMask == 3
        && map.forces[0].flags == 0x80000033 && map.forces[0].name == "Alliance", "force masks must parse");
    const auto& custom = *map.custom;
    Require(custom.upgrades.size() == 1 && custom.upgrades[0].id == Rawcode("Rhme")
        && custom.upgrades[0].level == 2 && custom.upgrades[0].availability == 2,
        "upgrade availability must keep the native zero-based level");
    Require(custom.techRestrictions.size() == 2 && custom.techRestrictions[1].id == Rawcode("AHbz"),
        "unit and ability restrictions must both parse");
    Require(custom.randomUnits.size() == 1 && custom.randomUnits[0].number == 4
        && custom.randomUnits[0].columnTypes == std::vector<std::int32_t>{0, 1, 2}
        && custom.randomUnits[0].rows[1].chance == 25
        && custom.randomUnits[0].rows[1].ids[1] == SettingsRawcode{}, "random unit matrix must parse");
    Require(custom.randomItems.size() == 1 && custom.randomItems[0].number == 8
        && custom.randomItems[0].sets.size() == 2 && custom.randomItems[0].sets[0][1].chance == 40
        && custom.randomItems[0].sets[1][0].id == Rawcode("YjI/"), "nested item drop sets must parse");
    Require(map.OriginalBytes() == fixture.bytes, "complete data must preserve the exact source bytes");
    const auto reopened = MapSettings::Parse(map.OriginalBytes());
    Require(reopened.Succeeded() && reopened.value->OriginalBytes() == fixture.bytes
        && reopened.value->custom->randomItems[0].sets[0][1].id == Rawcode("pman"),
        "no-edit reparse must retain fields and raw bytes");
}

void TestEveryTruncationBoundary() {
    const auto fixture = FullMapSettingsV25();
    for (std::size_t size = 0; size < fixture.bytes.size(); ++size) {
        const auto parsed = MapSettings::Parse(std::span(fixture.bytes).first(size));
        Require(!parsed.Succeeded() && !parsed.value, "every incomplete prefix must fail without a partial default model");
        Require(!parsed.diagnostics.empty() && parsed.diagnostics.front().origin.has_value(),
            "truncation diagnostics must identify a source offset");
    }
}

void TestVersionsAndCounts() {
    for (const auto version : {0u, 19u, 24u, 26u, 27u, 29u, 30u, 34u, 0xffffffffu}) {
        auto fixture = FullMapSettingsV25();
        fixture.bytes[0] = static_cast<std::byte>(version & 0xff);
        const auto parsed = MapSettings::Parse(fixture.bytes);
        Require(!parsed.Succeeded() && HasCode(parsed, "W3I_UNSUPPORTED_VERSION"),
            "other versions must not be parsed using the TFT layout");
    }
    for (const auto version : {18u, 28u, 32u, 33u}) {
        auto fixture = FullMapSettingsV25(); fixture.bytes[0] = static_cast<std::byte>(version);
        Require(!MapSettings::Parse(fixture.bytes).Succeeded(), "new label cannot reinterpret a classic payload");
    }
    for (const auto name : {"players", "forces", "upgrades", "tech", "unitTables", "columns", "rows",
            "itemTables", "sets", "items"}) {
        for (const auto count : {0xffffffffu, 0x7fffffffu, 65535u}) {
            auto fixture = FullMapSettingsV25();
            fixture.Patch(name, count);
            const auto parsed = MapSettings::Parse(fixture.bytes);
            Require(!parsed.Succeeded() && !parsed.value, "negative, excessive and truncated counts must fail before allocation");
        }
    }
}

void TestRocLayout() {
    const auto source = FullMapSettingsV25();
    auto fixture = source;
    fixture.bytes.resize(source.offsets.at("camera") + 61);
    fixture.bytes[0] = std::byte{18};
    for (const auto number : {17, 29}) {
        fixture.Int(number);
        for (const auto text : {"Text", "Title", "Subtitle"}) fixture.String(text);
    }
    fixture.bytes.insert(fixture.bytes.end(), source.bytes.begin() + source.offsets.at("players"),
        source.bytes.begin() + source.offsets.at("itemTables"));
    auto parsed = MapSettings::Parse(fixture.bytes);
    Require(parsed.Succeeded() && parsed.value->FullyParsed() && parsed.value->OriginalBytes() == fixture.bytes,
        "independent ROC layout must retain every byte");
    Require(parsed.value->rocCampaignBackground == 17 && parsed.value->rocLoadingScreenNumber == 29
        && parsed.value->players.size() == 2 && parsed.value->custom->randomUnits.size() == 1
        && parsed.value->custom->randomItems.empty(), "ROC integers and custom tables must align");
    Require(!parsed.value->Serialize().Succeeded(), "ROC writing is not authorized");
}

void TestVersionedLayout(int version) {
    // 从原创 v25 字节夹具逐段插入版本字段，避免用待测 writer 制造预期输入。
    auto fixture = FullMapSettingsV25();
    const auto insert = [&fixture](std::size_t offset, std::initializer_list<std::uint32_t> values) {
        std::vector<std::byte> encoded;
        for (const auto value : values) {
            for (unsigned shift = 0; shift < 32; shift += 8)
                encoded.push_back(static_cast<std::byte>((value >> shift) & 0xffu));
        }
        fixture.bytes.insert(fixture.bytes.begin() + static_cast<std::ptrdiff_t>(offset),
            encoded.begin(), encoded.end());
    };
    const auto players = fixture.offsets.at("players");
    const auto first = players + 4;
    const auto second = first + 16 + std::string_view("Human slot").size() + 1 + 8 + 8;
    const auto end = second + 16 + std::string_view("Computer slot").size() + 1 + 8 + 8;
    if (version >= 31) {
        insert(end, {0x80000000u, 0x01020304u});
        insert(second, {0x40000000u, 0x05060708u});
    }
    if (version == 33) insert(players, {0x80000000u});
    if (version >= 32) insert(players, {0xffffffffu, 0x7fffffffu});
    if (version >= 31) insert(players, {0x87654321u, 1u});
    insert(players, {1u}); // Lua；v28 后面直接是玩家计数。
    insert(12, {1u, 32u, 10u, 20456u});
    fixture.bytes[0] = static_cast<std::byte>(version);

    const auto parsed = MapSettings::Parse(fixture.bytes);
    Require(parsed.Succeeded() && parsed.value->FullyParsed() && parsed.diagnostics.empty(),
        "independently assembled v31 must parse without a guessed tail");
    Require(parsed.value->gameVersion == std::array<std::int32_t, 4>{1, 32, 10, 20456}
        && parsed.value->scriptLanguage == 1 && parsed.value->graphicsModes == (version >= 31 ? 0x87654321u : 0u)
        && parsed.value->gameDataVersion == (version >= 31 ? 1 : 0), "versioned header retains raw values");
    const auto& map = *parsed.value;
    Require(map.SourceFormatVersion() == version && map.OriginalBytes() == fixture.bytes,
        "private source version and bytes are preserved");
    Require(map.forceDefaultCameraZoom == (version >= 32 ? std::optional<std::int32_t>{-1} : std::nullopt)
        && map.forceMaxCameraZoom == (version >= 32 ? std::optional<std::int32_t>{0x7fffffff} : std::nullopt)
        && map.forceMinCameraZoom == (version == 33 ? std::optional<std::int32_t>{std::numeric_limits<std::int32_t>::min()} : std::nullopt),
        "zoom limits are exact I32 with explicit field presence");
    using namespace War3Map;
    const std::vector<MapArchiveMember> members{{"war3map.w3i", fixture.bytes}};
    const auto document = MapDocument::FromMembers(members, {});
    const auto& field = *MapData::FindMapField(MapDataKind::Settings, "map_name");
    Require(!MapData::MapFieldLayoutAllowsWrite(document, field), "modern settings fields remain read-only");
    for (const auto& [name, exists] : {std::pair{"script_language", true}, {"graphics_modes", version >= 31},
        {"force_default_camera_zoom", version >= 32}, {"force_min_camera_zoom", version == 33}}) {
        const auto value = MapData::ReadMapField(document, MapDataKind::Settings, 0,
            *MapData::FindMapField(MapDataKind::Settings, name));
        Require(value.Succeeded() == exists, "field catalog must distinguish absent from zero");
    }
    if (version >= 31) {
    Require(parsed.value->players.size() == 2
        && parsed.value->players[0].enemyLowPriorityMask == 0x40000000u
        && parsed.value->players[0].enemyHighPriorityMask == 0x05060708u
        && parsed.value->players[1].enemyLowPriorityMask == 0x80000000u
        && parsed.value->players[1].enemyHighPriorityMask == 0x01020304u,
        "v31 enemy masks must not be read as the next player or force");
    }
    const auto unchanged = parsed.value->Serialize();
    if (version != 31) {
        Require(!unchanged.Succeeded() && !map.HasClassicLayout(), "new profiles cannot serialize or initialize");
        auto spoofed = map;
        spoofed.formatVersion = 31; spoofed.forceDefaultCameraZoom.reset();
        spoofed.forceMaxCameraZoom.reset(); spoofed.forceMinCameraZoom.reset();
        Require(!spoofed.Serialize().Succeeded(), "clearing extensions cannot hide private source version");
        spoofed.formatVersion = 25;
        Require(!spoofed.Serialize().Succeeded() && !spoofed.HasClassicLayout(), "downgrade label stays rejected");
        for (const auto size : {std::size_t{15}, players + 20, fixture.bytes.size() - 1})
            Require(!MapSettings::Parse(std::span(fixture.bytes).first(size)).Succeeded(), "truncated profile fails");
        return;
    }
    Require(unchanged.Succeeded() && *unchanged.value == fixture.bytes,
        "v31 no-edit writer must be byte-identical to independent input");
    auto edited = *parsed.value;
    edited.players[0].enemyHighPriorityMask = 0xaabbccddu;
    edited.mapName = "New v31 name";
    const auto written = edited.Serialize();
    Require(written.Succeeded(), "v31 known fields should serialize");
    const auto reopened = MapSettings::Parse(*written.value);
    Require(reopened.Succeeded() && reopened.value->FullyParsed()
        && reopened.value->mapName == "New v31 name"
        && reopened.value->players[0].enemyHighPriorityMask == 0xaabbccddu
        && reopened.value->graphicsModes == parsed.value->graphicsModes,
        "v31 edit must preserve unrelated modern fields");
    for (const auto size : {std::size_t{15}, players + 20, fixture.bytes.size() - 1}) {
        Require(!MapSettings::Parse(std::span(fixture.bytes).first(size)).Succeeded(),
            "truncated v31 must not produce a partial settings object");
    }
    auto mislabeled = FullMapSettingsV25();
    mislabeled.bytes[0] = std::byte{31};
    Require(!MapSettings::Parse(mislabeled.bytes).Succeeded(),
        "classic payload with v31 label must not be accepted as a modern map");
}

void TestNonfiniteAndStringBudgets() {
    for (const auto name : {"camera", "fog", "start"}) {
        for (const auto bitPattern : {0x7fc00000u, 0x7f800000u, 0xff800000u}) {
            auto fixture = FullMapSettingsV25();
            fixture.Patch(name, bitPattern);
            const auto parsed = MapSettings::Parse(fixture.bytes);
            Require(!parsed.Succeeded() && HasCode(parsed, "W3I_NONFINITE"),
                "NaN and infinity must fail with a specific diagnostic");
        }
    }
    Fixture fixture;
    fixture.Int(25);
    fixture.Int(0);
    fixture.Int(6061);
    fixture.bytes.insert(fixture.bytes.end(), 1024 * 1024 + 1, std::byte{'A'});
    const auto parsed = MapSettings::Parse(fixture.bytes);
    Require(!parsed.Succeeded() && HasCode(parsed, "W3I_STRING_BUDGET"), "unterminated oversized strings need a bounded scan");
    fixture.bytes.assign(64 * 1024 * 1024 + 1, std::byte{0});
    const auto oversized = MapSettings::Parse(fixture.bytes);
    Require(!oversized.Succeeded() && HasCode(oversized, "W3I_FILE_BUDGET"), "oversized members must fail before copying bytes");
}

void TestUnknownTailAndOmittedTables() {
    auto fixture = FullMapSettingsV25();
    const std::vector<std::byte> tail{std::byte{0xa5}, std::byte{0}, std::byte{0xff}};
    fixture.bytes.insert(fixture.bytes.end(), tail.begin(), tail.end());
    const auto parsed = MapSettings::Parse(fixture.bytes);
    Require(parsed.Succeeded() && !parsed.value->FullyParsed() && HasCode(parsed, "W3I_UNKNOWN_TAIL"),
        "known-prefix decoding must not imply full understanding of an unknown tail");
    Require(parsed.value->UnknownTail() == tail && parsed.value->OriginalBytes() == fixture.bytes
        && parsed.value->custom.has_value(), "unknown tail and parsed known tables must both remain available");

    fixture = FullMapSettingsV25();
    fixture.bytes.resize(fixture.offsets.at("upgrades"));
    fixture.Byte(0xff);
    const auto omitted = MapSettings::Parse(fixture.bytes);
    Require(omitted.Succeeded() && !omitted.value->FullyParsed() && !omitted.value->custom.has_value()
        && HasCode(omitted, "W3I_CUSTOM_TABLES_OMITTED"), "omitted custom tables must not turn into empty tables");
    Require(omitted.value->OriginalBytes() == fixture.bytes, "omitted tables source must remain byte-identical");
}

void TestEmptyTablesAndCount255() {
    auto fixture = FullMapSettingsV25();
    fixture.bytes.resize(fixture.offsets.at("upgrades"));
    fixture.Int(0);
    fixture.Int(0);
    fixture.Int(0);
    fixture.Int(0);
    const auto empty = MapSettings::Parse(fixture.bytes);
    Require(empty.Succeeded() && empty.value->FullyParsed() && empty.value->custom
        && empty.value->custom->upgrades.empty(), "four explicit empty tables must be distinguished from missing data");

    fixture.bytes.resize(fixture.offsets.at("upgrades"));
    fixture.Int(255);
    for (int index = 0; index < 255; ++index) {
        fixture.UInt(1);
        fixture.Raw("Rhme");
        fixture.Int(index);
        fixture.Int(1);
    }
    fixture.Int(0);
    fixture.Int(0);
    fixture.Int(0);
    const auto many = MapSettings::Parse(fixture.bytes);
    Require(many.Succeeded() && many.value->FullyParsed() && many.value->custom->upgrades.size() == 255,
        "count low byte 0xff must not be mistaken for the one-byte omission marker");
}

void TestWritableSettingsRoundtrip() {
    const auto fixture = FullMapSettingsV25();
    auto parsed = MapSettings::Parse(fixture.bytes);
    Require(parsed.Succeeded(), "writer fixture must parse");
    const auto same = parsed.value->Serialize();
    Require(same.Succeeded() && *same.value == fixture.bytes, "unchanged settings must remain byte-identical");
    parsed.value->mapName = "Edited map";
    parsed.value->players[0].name = "Edited player";
    parsed.value->custom->randomUnits[0].rows[0].chance = 70;
    const auto changed = parsed.value->Serialize();
    Require(changed.Succeeded(), "settings edits must serialize");
    const auto reopened = MapSettings::Parse(*changed.value);
    Require(reopened.Succeeded() && reopened.value->mapName == "Edited map"
        && reopened.value->players[0].name == "Edited player"
        && reopened.value->custom->randomUnits[0].rows[0].chance == 70
        && reopened.value->custom->randomItems[0].sets.size() == 2,
        "known settings must survive write and parse");
    parsed.value->cameraBounds[0] = std::numeric_limits<float>::quiet_NaN();
    Require(!parsed.value->Serialize().Succeeded(), "nonfinite settings must reject writing");

    auto tailFixture = FullMapSettingsV25();
    tailFixture.bytes.push_back(std::byte{0xa5});
    auto withTail = MapSettings::Parse(tailFixture.bytes);
    Require(withTail.Succeeded(), "bounded unknown tail fixture must parse");
    withTail.value->author = "New author";
    const auto withTailBytes = withTail.value->Serialize();
    Require(withTailBytes.Succeeded() && withTailBytes.value->back() == std::byte{0xa5},
        "edited known settings must preserve bounded unknown tail");
    auto omitted = FullMapSettingsV25();
    omitted.bytes.resize(omitted.offsets.at("upgrades"));
    omitted.Byte(0xff);
    auto omittedDoc = MapSettings::Parse(omitted.bytes);
    Require(omittedDoc.Succeeded(), "omitted tables must parse");
    omittedDoc.value->description = "changed";
    Require(omittedDoc.value->Serialize().Succeeded(), "omitted table marker must survive known-field change");
    auto inconsistent = MapSettings::Parse(fixture.bytes);
    inconsistent.value->custom->randomUnits[0].rows[0].ids.pop_back();
    Require(!inconsistent.value->Serialize().Succeeded(), "mismatched random-unit columns must reject writing");
    inconsistent = MapSettings::Parse(fixture.bytes);
    inconsistent.value->formatVersion = 99;
    Require(!inconsistent.value->Serialize().Succeeded(), "unknown settings version must reject writing");
}

} // namespace

int main() {
    try {
        TestCompleteClassicData();
        TestEveryTruncationBoundary();
        TestVersionsAndCounts();
        TestRocLayout();
        for (const auto version : {28, 31, 32, 33}) TestVersionedLayout(version);
        TestNonfiniteAndStringBudgets();
        TestUnknownTailAndOmittedTables();
        TestEmptyTablesAndCount255();
        TestWritableSettingsRoundtrip();
        std::cout << "MapSettings: 7 scenario groups passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "MapSettings: " << exception.what() << '\n';
        return 1;
    }
}
