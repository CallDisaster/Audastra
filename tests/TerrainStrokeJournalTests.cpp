#include "War3MapCore/MapData/MapDataEditSession.h"

#include <algorithm>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using namespace War3Map;
using namespace War3Map::MapData;
using Bytes = std::vector<std::byte>;
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class T> T Take(Result<T> value) {
    if (!value.Succeeded() || !value.value) throw std::runtime_error("journal fixture operation failed");
    return std::move(*value.value);
}
TerrainStrokeJournal Journal() {
    TerrainStrokeJournal journal; journal.sourceSha256.assign(64, 'a');
    for (std::size_t i = 0; i < TerrainJournalMemberNames.size(); ++i)
        journal.members.push_back({std::string(TerrainJournalMemberNames[i]), i < 2, i < 2 ? std::string(64, 'b') : std::string{}, i < 2 ? 4u : 0u});
    TerrainJournalStroke stroke; stroke.intent.brush.kind = TerrainBrushKind::Raise;
    stroke.intent.brush.radius = 0; stroke.intent.points = {{256, 256}, {384, 256}};
    journal.strokes.push_back(std::move(stroke)); return journal;
}
void StrictCodecRoundtripAndMalformedInputs() {
    const auto wire = Take(EncodeTerrainStrokeJournal(Journal()));
    const auto restored = Take(DecodeTerrainStrokeJournal(wire));
    Require(Take(EncodeTerrainStrokeJournal(restored)) == wire, "canonical complete roundtrip");
    for (std::size_t size = 0; size < wire.size(); ++size)
        Require(!DecodeTerrainStrokeJournal(std::span(wire).first(size)).Succeeded(), "every truncated record fails closed");
    auto changed = wire; changed.push_back(std::byte{0});
    Require(!DecodeTerrainStrokeJournal(changed).Succeeded(), "unknown trailing extension rejected");
    changed = wire; changed[3] = std::byte{'9'};
    Require(!DecodeTerrainStrokeJournal(changed).Succeeded(), "unknown version rejected");
    changed = wire; changed[88] = std::byte{2};
    Require(!DecodeTerrainStrokeJournal(changed).Succeeded(), "nonboolean member presence rejected");
    auto invalid = Journal(); invalid.members[2].sha256.assign(64, 'c');
    Require(!EncodeTerrainStrokeJournal(invalid).Succeeded(), "missing member cannot carry a fabricated digest");
    invalid = Journal(); invalid.members[0].present = false;
    Require(!EncodeTerrainStrokeJournal(invalid).Succeeded(), "W3E is required");
    invalid = Journal(); invalid.sourceSha256[0] = 'A';
    Require(!EncodeTerrainStrokeJournal(invalid).Succeeded(), "only lowercase complete SHA accepted");
    invalid = Journal(); invalid.strokes[0].intent.brush.anchor = TerrainStrokeAnchor{};
    Require(!EncodeTerrainStrokeJournal(invalid).Succeeded(), "serialized anchors cannot grant source identity");
    invalid = Journal(); invalid.strokes[0].intent.points[0].x = std::numeric_limits<double>::infinity();
    Require(!EncodeTerrainStrokeJournal(invalid).Succeeded(), "nonfinite world point rejected");
    invalid = Journal(); invalid.strokes[0].intent.brush.kind = static_cast<TerrainBrushKind>(255);
    Require(!EncodeTerrainStrokeJournal(invalid).Succeeded(), "unknown brush kind rejected");
    invalid = Journal(); invalid.strokes[0].intent.brush.size = 9;
    Require(!EncodeTerrainStrokeJournal(invalid).Succeeded(), "classic size range checked");
    invalid = Journal(); invalid.strokes[0].intent.points.assign(4096, {256, 256});
    Require(EncodeTerrainStrokeJournal(invalid).Succeeded(), "4096 points remains in domain");
    invalid.strokes[0].intent.points.push_back({256, 256});
    Require(!EncodeTerrainStrokeJournal(invalid).Succeeded(), "4097 points cannot be trimmed or split");
    invalid = Journal(); invalid.strokes.assign(129, invalid.strokes.front());
    Require(!EncodeTerrainStrokeJournal(invalid).Succeeded(), "129 strokes rejected");
    invalid = Journal(); auto& water = invalid.strokes[0];
    water.intent.brush = {}; water.intent.waterBrush = TerrainWaterStrokeOptions{}; water.rulesSha256.assign(64, 'd');
    Require(EncodeTerrainStrokeJournal(invalid).Succeeded(), "water options have independent neutral ordinary brush");
    water.intent.brush.seed = 1;
    Require(!EncodeTerrainStrokeJournal(invalid).Succeeded(), "water ordinary conflicts rejected");
}

Bytes LegacyWaterGolden(bool nativeCliff) {
    // 独立固定wire夹具：这些标量/IEEE字节直接来自FTJ1/2合同，不调用产品编码器。
    Bytes bytes;
    const auto hex = [&](std::string_view value) {
        const auto digit = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
        for (std::size_t i = 0; i < value.size(); i += 2)
            bytes.push_back(std::byte(digit(value[i]) * 16 + digit(value[i + 1])));
    };
    const auto text = [&](std::string_view value) {
        bytes.push_back(std::byte(value.size())); hex("000000");
        for (const char c : value) bytes.push_back(std::byte(c));
    };
    hex(nativeCliff ? "46544a32" : "46544a31"); text(std::string(64, 'a')); hex("0a");
    for (std::size_t i = 0; i < TerrainJournalMemberNames.size(); ++i) {
        text(TerrainJournalMemberNames[i]); hex(i < 2 ? "01" : "00"); text(i < 2 ? std::string(64, 'b') : "");
        hex(i < 2 ? "0400000000000000" : "0000000000000000");
    }
    hex("00000000"); hex(nativeCliff ? "02000000" : "01000000");
    for (const bool cliff : {false, true}) {
        if (cliff && !nativeCliff) break;
        hex(cliff ? "0600" : "0000");
        hex("01000000" "00000000" "00" "00000000" "01" "00000042" "00000000"
            "000000" "00000000" "0200");
        hex(cliff ? "00" : "01");
        if (!cliff) hex("0000" "01000000" "00" "00000000" "01");
        if (nativeCliff) hex(cliff ? "0100" : "00");
        text(std::string(64, 'd')); hex("02000000");
        hex("0000000000007040" "0000000000007040" "0000000000007840" "0000000000007040");
    }
    return bytes;
}

void WaterAlgorithmVersionIsCanonicalAndLegacyBytesRemainExact() {
    auto journal = Journal(); auto& water = journal.strokes.front();
    water.intent.brush = {}; water.intent.waterBrush = TerrainWaterStrokeOptions{};
    water.intent.waterBrush->algorithm = TerrainWaterAlgorithm::LegacyFlat; water.rulesSha256.assign(64, 'd');
    const auto legacy1 = LegacyWaterGolden(false);
    Require(Take(EncodeTerrainStrokeJournal(journal)) == legacy1, "legacy FTJ1 water retains every original byte");
    const auto old1 = Take(DecodeTerrainStrokeJournal(legacy1));
    Require(old1.strokes.front().intent.waterBrush->algorithm == TerrainWaterAlgorithm::LegacyFlat
        && Take(EncodeTerrainStrokeJournal(old1)) == legacy1, "FTJ1 decoding cannot upgrade water by the new default");
    TerrainJournalStroke cliff; cliff.intent.brush.kind = TerrainBrushKind::CliffLevel;
    cliff.intent.points = water.intent.points; cliff.rulesSha256.assign(64, 'd'); cliff.cliffEnforceWaterLimits = false;
    journal.strokes.push_back(cliff);
    const auto legacy2 = LegacyWaterGolden(true);
    Require(Take(EncodeTerrainStrokeJournal(journal)) == legacy2, "mixed FTJ2 keeps legacy water and cliff bytes exact");
    const auto old2 = Take(DecodeTerrainStrokeJournal(legacy2));
    Require(old2.strokes.front().intent.waterBrush->algorithm == TerrainWaterAlgorithm::LegacyFlat
        && Take(EncodeTerrainStrokeJournal(old2)) == legacy2, "FTJ2 rule digest cannot opt its water into v2");
    auto native = journal.strokes.front(); native.intent.waterBrush->algorithm = TerrainWaterAlgorithm::NativeRamps;
    journal.strokes.insert(journal.strokes.begin() + 1, native);
    const auto wire = Take(EncodeTerrainStrokeJournal(journal));
    const auto decoded = Take(DecodeTerrainStrokeJournal(wire));
    Require(wire[3] == std::byte{'3'} && decoded.strokes[0].intent.waterBrush->algorithm == TerrainWaterAlgorithm::LegacyFlat
        && decoded.strokes[1].intent.waterBrush->algorithm == TerrainWaterAlgorithm::NativeRamps
        && decoded.strokes[2].cliffEnforceWaterLimits == false && Take(EncodeTerrainStrokeJournal(decoded)) == wire,
        "FTJ3 retains explicit mixed water algorithms and the existing cliff option");
    for (std::size_t size = 0; size < wire.size(); ++size)
        Require(!DecodeTerrainStrokeJournal(std::span(wire).first(size)).Succeeded(), "every FTJ3 truncation fails closed");
    journal.strokes = {native}; const auto single = Take(EncodeTerrainStrokeJournal(journal));
    // 最后104字节是digest长度/64字节文本及2个点；前面依次为水版本和cliff optional。
    const auto algorithmOffset = single.size() - 106;
    auto invalid = single; invalid[algorithmOffset] = std::byte{9};
    Require(!DecodeTerrainStrokeJournal(invalid).Succeeded(), "unknown persisted water algorithm is rejected");
    invalid = single; invalid[algorithmOffset] = std::byte{1};
    Require(!DecodeTerrainStrokeJournal(invalid).Succeeded(), "FTJ3 with no v2 water is noncanonical");
    invalid = legacy1; invalid[3] = std::byte{'3'};
    Require(!DecodeTerrainStrokeJournal(invalid).Succeeded(), "changing only the magic cannot upgrade an old record");
    journal.strokes.front().intent.waterBrush->algorithm = static_cast<TerrainWaterAlgorithm>(255);
    Require(!EncodeTerrainStrokeJournal(journal).Succeeded(), "unknown typed algorithm is rejected before encoding");
}

void NativeCliffVersionAndOptionsRemainExplicit() {
    auto journal = Journal(); auto& stroke = journal.strokes.front();
    stroke.intent.brush.kind = TerrainBrushKind::CliffLevel;
    const auto legacy = Take(EncodeTerrainStrokeJournal(journal));
    Require(legacy[3] == std::byte{'1'}, "legacy ordinary cliff retains FTJ1 encoding");
    Require(!Take(DecodeTerrainStrokeJournal(legacy)).strokes.front().cliffEnforceWaterLimits,
        "legacy cliff cannot acquire native rule semantics during decoding");
    stroke.rulesSha256.assign(64, 'd');
    Require(!EncodeTerrainStrokeJournal(journal).Succeeded(), "native cliff digest alone cannot omit its water-limit option");
    stroke.cliffEnforceWaterLimits = false;
    const auto wire = Take(EncodeTerrainStrokeJournal(journal));
    Require(wire[3] == std::byte{'2'}, "native cliff opts into FTJ2 explicitly");
    const auto decoded = Take(DecodeTerrainStrokeJournal(wire));
    Require(decoded.strokes.front().cliffEnforceWaterLimits == false
        && decoded.strokes.front().rulesSha256 == std::string(64, 'd')
        && !decoded.strokes.front().intent.waterBrush, "ordinary cliff and disabled water limits roundtrip independently of water brush");
    Require(Take(EncodeTerrainStrokeJournal(decoded)) == wire, "FTJ2 canonical bytes roundtrip");
    for (std::size_t size = 0; size < wire.size(); ++size)
        Require(!DecodeTerrainStrokeJournal(std::span(wire).first(size)).Succeeded(), "every FTJ2 truncation fails closed");
    stroke.intent.brush.kind = TerrainBrushKind::Raise;
    Require(!EncodeTerrainStrokeJournal(journal).Succeeded(), "non-cliff cannot impersonate native cliff rules");
}

void CornerVersionIsExplicitAndMixedAlgorithmsRemainOrdered() {
    static_assert(static_cast<unsigned>(TerrainBrushKind::CornerRamp) == 13);
    auto journal = Journal(); auto& corner = journal.strokes.front();
    corner.intent.brush = {}; corner.intent.brush.kind = TerrainBrushKind::CornerRamp;
    corner.intent.points = {{1056, 1056}}; corner.rulesSha256.assign(64, 'd');
    const auto single = Take(EncodeTerrainStrokeJournal(journal));
    Require(single[3] == std::byte{'4'} && Take(EncodeTerrainStrokeJournal(Take(DecodeTerrainStrokeJournal(single)))) == single,
        "numeric13 corner uses canonical FTJ4 bytes");
    for (std::size_t size = 0; size < single.size(); ++size)
        Require(!DecodeTerrainStrokeJournal(std::span(single).first(size)).Succeeded(), "every FTJ4 truncation fails closed");
    for (const char old : {'1', '2', '3'}) {
        auto changed = single; changed[3] = std::byte(old);
        Require(!DecodeTerrainStrokeJournal(changed).Succeeded(), "old wire version cannot acquire corner semantics");
    }
    // 单Corner尾部固定为SHA长度+64字节、点数+16字节；其前面为cliff optional、water optional。
    // 在FTJ4布局中仅改kind为普通Raise，仍不得保留不含Corner的新magic。
    auto noCorner = single;
    const auto strokeOffset = single.size() - 123;
    Require(std::to_integer<unsigned>(single[strokeOffset]) == 13, "independent fixed one-point wire offset identifies kind13");
    noCorner[strokeOffset] = std::byte(static_cast<unsigned>(TerrainBrushKind::Raise));
    // 普通Raise不带规则；重编码为FTJ1后只升级magic也不构成FTJ4。
    auto ordinary = Journal(); auto oldWire = Take(EncodeTerrainStrokeJournal(ordinary)); oldWire[3] = std::byte{'4'};
    Require(!DecodeTerrainStrokeJournal(oldWire).Succeeded() && !DecodeTerrainStrokeJournal(noCorner).Succeeded(), "FTJ4 without a corner is noncanonical");
    auto invalid = journal; invalid.strokes.front().rulesSha256.clear();
    Require(!EncodeTerrainStrokeJournal(invalid).Succeeded(), "corner requires complete actual rule digest");
    invalid = journal; invalid.strokes.front().cliffEnforceWaterLimits = true;
    Require(!EncodeTerrainStrokeJournal(invalid).Succeeded(), "corner cannot carry unrelated cliff option");
    invalid = journal; invalid.strokes.front().intent.points.push_back({1184, 1056});
    Require(!EncodeTerrainStrokeJournal(invalid).Succeeded(), "corner must remain one exact point");
    invalid = journal; invalid.strokes.front().intent.brush.radius = 0;
    Require(!EncodeTerrainStrokeJournal(invalid).Succeeded(), "corner cannot restore nondefault brush combinations");
    TerrainJournalStroke water; water.intent.waterBrush = TerrainWaterStrokeOptions{};
    water.intent.waterBrush->algorithm = TerrainWaterAlgorithm::LegacyFlat;
    water.intent.points = {{256, 256}}; water.rulesSha256.assign(64, 'e');
    journal.strokes.push_back(water);
    water.intent.waterBrush->algorithm = TerrainWaterAlgorithm::NativeRamps; journal.strokes.push_back(water);
    auto noCornerJournal = journal; noCornerJournal.strokes.erase(noCornerJournal.strokes.begin());
    auto noCornerWire = Take(EncodeTerrainStrokeJournal(noCornerJournal)); noCornerWire[3] = std::byte{'4'};
    Require(!DecodeTerrainStrokeJournal(noCornerWire).Succeeded(), "valid FTJ3 layout with explicit algorithms still cannot claim FTJ4 without a corner");
    TerrainJournalStroke cliff; cliff.intent.brush.kind = TerrainBrushKind::CliffLevel;
    cliff.intent.points = {{384, 384}}; cliff.cliffEnforceWaterLimits = false; cliff.rulesSha256.assign(64, 'f');
    journal.strokes.push_back(cliff); auto remove = journal.strokes.front(); remove.intent.brush.value = false;
    journal.strokes.push_back(remove);
    const auto wire = Take(EncodeTerrainStrokeJournal(journal)); const auto decoded = Take(DecodeTerrainStrokeJournal(wire));
    Require(wire[3] == std::byte{'4'} && decoded.strokes.size() == 5
        && decoded.strokes[0].intent.brush.kind == TerrainBrushKind::CornerRamp
        && decoded.strokes[1].intent.waterBrush->algorithm == TerrainWaterAlgorithm::LegacyFlat
        && decoded.strokes[2].intent.waterBrush->algorithm == TerrainWaterAlgorithm::NativeRamps
        && decoded.strokes[3].cliffEnforceWaterLimits == false && !decoded.strokes[4].intent.brush.value
        && Take(EncodeTerrainStrokeJournal(decoded)) == wire, "FTJ4 preserves ordered corner/create remove, both water algorithms and native cliff");
}

void BoundsVersionHasIndependentPayloadAndStrictVariants() {
    auto journal = Journal(); TerrainJournalStroke bounds;
    bounds.intent.bounds = TerrainBoundsEditIntent{{1, 2, 17, 14}}; bounds.rulesSha256.assign(64, 'c');
    journal.strokes = {bounds}; const auto single = Take(EncodeTerrainStrokeJournal(journal));
    Require(single[3] == std::byte{'5'}, "bounds are explicitly persisted as FTJ5");
    // FTJ5范围正文没有假笔刷/点：kind1、algorithm1、四个LE u32、SHA长度及正文。
    const Bytes prefix{std::byte{1}, std::byte{1}, std::byte{1}, std::byte{0}, std::byte{0}, std::byte{0},
        std::byte{2}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{17}, std::byte{0}, std::byte{0}, std::byte{0},
        std::byte{14}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{64}, std::byte{0}, std::byte{0}, std::byte{0}};
    const auto payload = single.size() - prefix.size() - 64;
    Require(std::equal(prefix.begin(), prefix.end(), single.begin() + payload)
        && std::all_of(single.begin() + payload + prefix.size(), single.end(), [](auto c) { return c == std::byte{'c'}; }),
        "complete bounds payload matches independent literal bytes");
    const auto decoded = Take(DecodeTerrainStrokeJournal(single));
    Require(decoded.strokes.front().intent.points.empty() && decoded.strokes.front().intent.bounds->playable.east == 17
        && Take(EncodeTerrainStrokeJournal(decoded)) == single, "FTJ5 bounds canonical roundtrip uses no synthetic point");
    for (std::size_t size = 0; size < single.size(); ++size)
        Require(!DecodeTerrainStrokeJournal(std::span(single).first(size)).Succeeded(), "every FTJ5 truncation fails closed");
    for (const auto offset : {payload, payload + 1, payload + 2, payload + 18, single.size() - 1}) {
        auto invalid = single; invalid[offset] = std::byte{255};
        Require(!DecodeTerrainStrokeJournal(invalid).Succeeded(), "unknown variant, algorithm, rectangle or SHA cannot be decoded");
    }
    for (const char old : {'1', '2', '3', '4'}) {
        auto invalid = single; invalid[3] = std::byte(old);
        Require(!DecodeTerrainStrokeJournal(invalid).Succeeded(), "legacy codecs cannot grant owned geometry semantics");
    }
    auto ordinary = Take(EncodeTerrainStrokeJournal(Journal())); ordinary[3] = std::byte{'5'};
    Require(!DecodeTerrainStrokeJournal(ordinary).Succeeded(), "FTJ5 cannot be selected merely by replacing old magic");
    auto invalid = journal; invalid.strokes.front().intent.points = {{0, 0}};
    Require(!EncodeTerrainStrokeJournal(invalid).Succeeded(), "bounds cannot hide sampling points");
    invalid = journal; invalid.strokes.front().intent.followDoodads = true;
    Require(!EncodeTerrainStrokeJournal(invalid).Succeeded(), "bounds cannot imply object translation");
    invalid = journal; invalid.strokes.front().intent.waterBrush.emplace();
    Require(!EncodeTerrainStrokeJournal(invalid).Succeeded(), "bounds cannot combine an unrelated water request");
    invalid = journal; invalid.strokes.front().intent.brush.seed = 1;
    Require(!EncodeTerrainStrokeJournal(invalid).Succeeded(), "bounds cannot discard nondefault brush fields");
    TerrainJournalStroke corner; corner.intent.brush.kind = TerrainBrushKind::CornerRamp;
    corner.intent.points = {{1056, 1056}}; corner.rulesSha256.assign(64, 'd');
    TerrainJournalStroke water; water.intent.waterBrush.emplace(); water.intent.points = {{256, 256}};
    water.intent.waterBrush->algorithm = TerrainWaterAlgorithm::LegacyFlat; water.rulesSha256.assign(64, 'e');
    journal.strokes.push_back(corner); journal.strokes.push_back(water);
    water.intent.waterBrush->algorithm = TerrainWaterAlgorithm::NativeRamps; journal.strokes.push_back(water);
    journal.strokes.push_back(bounds);
    const auto mixed = Take(EncodeTerrainStrokeJournal(journal)); const auto values = Take(DecodeTerrainStrokeJournal(mixed));
    Require(values.strokes.size() == 5 && values.strokes[0].intent.bounds && values.strokes[4].intent.bounds
        && values.strokes[1].intent.brush.kind == TerrainBrushKind::CornerRamp
        && values.strokes[2].intent.waterBrush->algorithm == TerrainWaterAlgorithm::LegacyFlat
        && values.strokes[3].intent.waterBrush->algorithm == TerrainWaterAlgorithm::NativeRamps
        && Take(EncodeTerrainStrokeJournal(values)) == mixed, "mixed FTJ5 retains exact bounds/corner/legacy-water/native-water order");
}

struct SessionFixture {
    TerrainGridSnapshot terrain;
    Bytes terrainBytes, pathing;
    MapDataEditSession session{MapDocument::FromMembers({})};
    TerrainStrokePoint point{512, 512};
    SessionFixture() {
        terrain.header.width = terrain.header.height = 9;
        terrain.header.mainTileset = 'L'; terrain.header.groundTilesets = {"Tg01"}; terrain.header.cliffTilesets = {"Cl01"};
        terrain.vertices.resize(81);
        for (auto& vertex : terrain.vertices) { vertex.groundHeightRaw = 8320; vertex.waterHeightAndFlagsRaw = 7936; vertex.cliffAndLayerRaw = 2; }
        RefreshTerrainDerived(terrain, 0, 0, 9, 9); terrainBytes = Take(SerializeMapTerrain(terrain));
        pathing = {std::byte{'M'}, std::byte{'P'}, std::byte{'3'}, std::byte{'W'}};
        for (const auto value : {0u, 32u, 32u}) for (unsigned shift = 0; shift < 32; shift += 8)
            pathing.push_back(std::byte((value >> shift) & 255));
        pathing.resize(16 + 32 * 32, std::byte{0x11});
        Require(session.AttachTerrain(terrainBytes).Succeeded() && session.AttachPathing(pathing).Succeeded(), "attach owned fixture");
    }
    void Raise(float amount = 16) {
        TerrainBrush brush; brush.kind = TerrainBrushKind::Raise; brush.radius = 0; brush.strength = amount;
        Take(session.EditTerrain(brush, std::span(&point, 1), false));
    }
};
void PaletteVersionHasStrictZeroPointPayload() {
    auto journal = Journal();
    TerrainJournalStroke palette;
    palette.intent.palette = TerrainPaletteEditIntent{TerrainTextureTable::Ground, 0, "Tg01", "Tg02", 1};
    palette.rulesSha256.assign(64, 'd'); journal.strokes = {palette};
    const auto wire = Take(EncodeTerrainStrokeJournal(journal));
    Require(wire[3] == std::byte{'6'} && Take(EncodeTerrainStrokeJournal(Take(DecodeTerrainStrokeJournal(wire)))) == wire,
        "FTJ6 independently records a zero-point existing palette slot and complete actual rules identity");
    for (std::size_t i = 0; i < wire.size(); ++i)
        Require(!DecodeTerrainStrokeJournal(std::span(wire).first(i)).Succeeded(), "every FTJ6 truncation fails closed");
    // 最后91字节是variant2、algorithm、table、slot、两个Text4及Text64，不使用产品reader寻址。
    const auto offset = wire.size() - 91;
    for (const auto [delta, value] : {std::pair{0u, 3u}, {1u, 2u}, {2u, 2u}, {3u, 16u}}) {
        auto changed = wire; changed[offset + delta] = std::byte(value);
        Require(!DecodeTerrainStrokeJournal(changed).Succeeded(), "unknown FTJ6 variant, algorithm, table and ground slot reject");
    }
    auto changed = wire; changed[3] = std::byte{'5'};
    Require(!DecodeTerrainStrokeJournal(changed).Succeeded(), "FTJ5 cannot acquire palette semantics by a magic change");
    auto invalid = journal; invalid.strokes[0].intent.palette->replacementRawcode = "Tg01";
    Require(!EncodeTerrainStrokeJournal(invalid).Succeeded(), "palette no-op cannot be serialized as a real command");
    invalid = journal; invalid.strokes[0].intent.points = {{1, 1}};
    Require(!EncodeTerrainStrokeJournal(invalid).Succeeded(), "palette cannot carry sample points");
    invalid = journal; invalid.strokes[0].intent.bounds = TerrainBoundsEditIntent{};
    Require(!EncodeTerrainStrokeJournal(invalid).Succeeded(), "palette and bounds variants cannot be combined");
    invalid = journal; invalid.strokes[0].intent.waterBrush = TerrainWaterStrokeOptions{};
    Require(!EncodeTerrainStrokeJournal(invalid).Succeeded(), "palette and water variants cannot be combined");
    invalid = journal; invalid.strokes[0].intent.brush.seed = 1;
    Require(!EncodeTerrainStrokeJournal(invalid).Succeeded(), "palette cannot smuggle ordinary brush parameters");
    invalid = journal; invalid.strokes[0].rulesSha256.clear();
    Require(!EncodeTerrainStrokeJournal(invalid).Succeeded(), "palette resource identity remains mandatory");
    invalid = journal; invalid.strokes[0].intent.palette->table = TerrainTextureTable::Cliff;
    invalid.strokes[0].intent.palette->slot = 15;
    Require(!EncodeTerrainStrokeJournal(invalid).Succeeded(), "sentinel cliff slot15 is outside typed palette edits");
    journal.strokes.assign(128, palette);
    Require(EncodeTerrainStrokeJournal(journal).Succeeded(), "128 palette commands need no fabricated sampling points");
    journal.strokes.front() = Journal().strokes.front();
    journal.strokes.front().intent.points.assign(4096, {256, 256});
    const auto mixed = Take(EncodeTerrainStrokeJournal(journal));
    Require(Take(EncodeTerrainStrokeJournal(Take(DecodeTerrainStrokeJournal(mixed)))) == mixed,
        "4096 real points plus127 zero-point palette commands preserve both independent budgets");
    journal.strokes.front().intent.points.push_back({256, 256});
    Require(!EncodeTerrainStrokeJournal(journal).Succeeded(), "palette does not raise the4096 sampling limit");
    journal.strokes.assign(129, palette);
    Require(!EncodeTerrainStrokeJournal(journal).Succeeded(), "palette still counts toward128 command limit");
    SessionFixture raw;
    Take(raw.session.EditTerrainTextureSlot(TerrainTextureTable::Ground, 0, "Tg02"));
    Require(!raw.session.ExportTerrainStrokeJournal().Succeeded(), "old raw API without actual owner cannot invent palette provenance");
}

void RemoveWaterUsesTheOrdinaryJournalOpcode() {
    static_assert(static_cast<unsigned>(TerrainBrushKind::RemoveWater) == 14);
    auto journal = Journal();
    journal.strokes.front().intent.brush.kind = TerrainBrushKind::RemoveWater;
    journal.strokes.front().intent.brush.shape = TerrainBrushShape::Square;
    const auto wire = Take(EncodeTerrainStrokeJournal(journal));
    Require(wire[3] == std::byte{'1'}, "ordinary remove-water keeps the FTJ1 layout");
    const auto decoded = Take(DecodeTerrainStrokeJournal(wire));
    Require(decoded.strokes.front().intent.brush.kind == TerrainBrushKind::RemoveWater
        && !decoded.strokes.front().intent.waterBrush && decoded.strokes.front().rulesSha256.empty()
        && Take(EncodeTerrainStrokeJournal(decoded)) == wire,
        "removal replays as an ordinary brush without inventing native-water ownership");
    auto unknown = decoded; unknown.strokes.front().intent.brush.kind = static_cast<TerrainBrushKind>(15);
    Require(!EncodeTerrainStrokeJournal(unknown).Succeeded(), "the next unknown ordinary opcode stays rejected");
    TerrainJournalStroke water; water.intent.waterBrush = TerrainWaterStrokeOptions{};
    water.intent.points = journal.strokes.front().intent.points; water.rulesSha256.assign(64, 'd');
    journal.strokes.insert(journal.strokes.begin(), water);
    const auto mixed = Take(EncodeTerrainStrokeJournal(journal));
    const auto restored = Take(DecodeTerrainStrokeJournal(mixed));
    Require(mixed[3] == std::byte{'3'} && restored.strokes.size() == 2
        && restored.strokes[0].intent.waterBrush && restored.strokes[1].intent.brush.kind == TerrainBrushKind::RemoveWater
        && Take(EncodeTerrainStrokeJournal(restored)) == mixed,
        "native shallow/deep creation followed by removal keeps ordered FTJ3 history");
}

void LogicalQtReplayAndSavedBoundary() {
    SessionFixture f; f.Raise(); const auto first = f.session.LastEdit();
    const auto firstState = f.session.TerrainJournalStateFingerprint();
    f.Raise(); const auto second = f.session.LastEdit();
    Require(Take(f.session.ExportTerrainStrokeJournal()).strokes.size() == 2, "two real commands exported");
    Require(f.session.Replay(second, false).Succeeded(), "Qt inverse appends a command");
    Require(Take(f.session.ExportTerrainStrokeJournal()).strokes.size() == 1
        && f.session.TerrainJournalStateFingerprint() == firstState, "inverse reduces the logical top");
    Require(f.session.Replay(second, true).Succeeded(), "Qt redo restores logical prefix");
    Require(Take(f.session.ExportTerrainStrokeJournal()).strokes.size() == 2, "redo has two forward strokes");
    Require(f.session.Undo() && Take(f.session.ExportTerrainStrokeJournal()).strokes.size() == 1, "Core cursor undo preserves logical prefix");
    Require(f.session.Redo(), "Core redo succeeds");
    f.session.CommitSaved(f.session.Snapshot());
    Require(Take(f.session.ExportTerrainStrokeJournal()).strokes.empty(), "real save moves journal baseline");
    Require(f.session.Replay(second, false).Succeeded(), "saved history remains undoable");
    Require(!f.session.ExportTerrainStrokeJournal().Succeeded(), "Qt inverse of saved command cannot become forward journal");
    Require(f.session.TerrainJournalStateFingerprint().find("SAVED_BOUNDARY") != std::string::npos, "CAS sees the degraded state");
    (void)first;
    SessionFixture branched; branched.Raise(); branched.session.CommitSaved(branched.session.Snapshot());
    Require(branched.session.Undo(), "cross saved cursor remains a valid raw undo");
    branched.Raise(32);
    Require(!branched.session.ExportTerrainStrokeJournal().Succeeded(), "branch replacing saved prefix cannot reuse numeric cursor");
}
void OversizedUiStrokeAndOpaqueDraftRemainEditable() {
    SessionFixture f;
    TerrainBrush brush; brush.kind = TerrainBrushKind::Raise; brush.radius = 0;
    std::vector<TerrainStrokePoint> points(4097, f.point);
    Take(f.session.EditTerrain(brush, points, false));
    Require(f.session.IsDirty() && !f.session.ExportTerrainStrokeJournal().Succeeded(), "UI edit survives journal point overflow");
    Require(f.session.Undo() && Take(f.session.ExportTerrainStrokeJournal()).strokes.empty(), "no truncation of original undo command");
    SessionFixture raw; auto altered = raw.terrain; altered.vertices[40].groundHeightRaw += 16;
    RefreshTerrainDerived(altered, 0, 0, 9, 9);
    Require(raw.session.SetTerrain(Take(SerializeMapTerrain(altered))).Succeeded(), "legacy raw draft still accepted");
    Require(!raw.session.ExportTerrainStrokeJournal().Succeeded(), "raw draft cannot invent typed provenance");
}
} // namespace

int main() {
    try {
        StrictCodecRoundtripAndMalformedInputs(); NativeCliffVersionAndOptionsRemainExplicit();
        WaterAlgorithmVersionIsCanonicalAndLegacyBytesRemainExact();
        CornerVersionIsExplicitAndMixedAlgorithmsRemainOrdered();
        BoundsVersionHasIndependentPayloadAndStrictVariants();
        PaletteVersionHasStrictZeroPointPayload();
        RemoveWaterUsesTheOrdinaryJournalOpcode();
        LogicalQtReplayAndSavedBoundary(); OversizedUiStrokeAndOpaqueDraftRemainEditable();
        std::cout << "TerrainStrokeJournalTests passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
