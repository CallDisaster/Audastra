#include "War3MapCore/MapData/MapPlacements.h"

#include <bit>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace {

using namespace War3Map::MapData;

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Fixture {
    std::vector<std::byte> bytes;
    std::size_t randomOffset = 0;
    std::size_t booleanOffset = 0;
    std::size_t specialOffset = 0;

    void U32(std::uint32_t value) {
        for (unsigned int shift = 0; shift < 32; shift += 8) {
            bytes.push_back(static_cast<std::byte>((value >> shift) & 0xffu));
        }
    }
    void I32(std::int32_t value) { U32(std::bit_cast<std::uint32_t>(value)); }
    void F32(float value) { U32(std::bit_cast<std::uint32_t>(value)); }
    void U8(std::uint8_t value) { bytes.push_back(static_cast<std::byte>(value)); }
    void Raw(std::string_view value) {
        Require(value.size() == 4, "fixture rawcode size");
        for (const auto character : value) U8(static_cast<std::uint8_t>(character));
    }
    void Patch(std::size_t offset, std::uint32_t value) {
        Require(offset + 4 <= bytes.size(), "fixture patch bounds");
        for (unsigned int index = 0; index < 4; ++index) {
            bytes[offset + index] = static_cast<std::byte>((value >> (index * 8)) & 0xffu);
        }
    }
    void Header(std::uint32_t count) { Raw("W3do"); U32(8); U32(11); U32(count); }
    void Transform(std::string_view rawcode) {
        Raw(rawcode); I32(2);
        F32(-128.25f); F32(512.5f); F32(64.0f);
        F32(1.25f);
        F32(0.75f); F32(1.0f); F32(1.5f);
    }
    void ItemSets() {
        U32(2); // 两组独立掉落，各自拥有不同的候选列表。
        U32(2); Raw("I001"); I32(30); Raw("I002"); I32(70);
        U32(1); Raw("I003"); I32(100);
    }
    void Unit(std::int32_t randomMode, std::int32_t creationId, bool withSkin = false) {
        Transform(randomMode == -1 ? "H001" : "uDNR");
        if (withSkin) Raw("H002");
        U8(2); I32(7); U8(0x12); U8(0x34);
        I32(80); I32(-1); I32(4); ItemSets();
        I32(12500); F32(-2.0f);
        I32(6); I32(31); I32(17); I32(22);
        U32(2); I32(0); Raw("I004"); I32(5); Raw("I005");
        U32(2);
        Raw("A001"); booleanOffset = bytes.size(); I32(1); I32(3);
        Raw("A002"); I32(0); I32(1);
        randomOffset = bytes.size();
        I32(randomMode);
        if (randomMode == 0) {
            U8(0xff); U8(0xff); U8(0xff); U8(3);
        } else if (randomMode == 1) {
            I32(9); I32(2);
        } else if (randomMode == 2) {
            U32(2); Raw("hfoo"); I32(25); Raw("hrif"); I32(75);
        }
        I32(-1); I32(12); I32(creationId);
    }
};

Fixture Units() {
    Fixture result;
    result.Header(4);
    for (std::int32_t mode = -1; mode <= 2; ++mode) result.Unit(mode, 101 + mode);
    return result;
}

Fixture Doodads(bool withSkin = false) {
    Fixture result;
    result.Header(1);
    result.Transform("LTlt");
    if (withSkin) result.Raw("LT02");
    result.U8(2); result.U8(75); result.I32(-1);
    result.ItemSets(); result.I32(901);
    result.specialOffset = result.bytes.size();
    result.I32(0); result.U32(1);
    result.Raw("YTab"); result.I32(4); result.I32(16); result.I32(32);
    return result;
}

template <typename T>
void RequireFailure(const War3Map::Result<T>& result, std::string_view code) {
    Require(!result.Succeeded() && !result.value.has_value(), "failure must not expose partial typed data");
    Require(result.diagnostics.size() == 1 && result.diagnostics[0].code == code, "incorrect diagnostic code");
    Require(result.diagnostics[0].origin.has_value(), "parser error must locate source member and byte offset");
}

template <typename T>
void RequireWriteFailure(const War3Map::Result<T>& result, std::string_view code) {
    Require(!result.Succeeded() && !result.value.has_value(), "writer failure must not expose output bytes");
    Require(result.diagnostics.size() == 1 && result.diagnostics[0].code == code, "incorrect writer diagnostic code");
}

#include "TerrainWalkableDoodadWireCases.h"

void CheckUnits() {
    const auto fixture = Units();
    auto parsed = ParseUnitPlacements(fixture.bytes);
    Require(parsed.Succeeded(), "classic units should parse");
    const auto& doc = *parsed.value;
    Require(doc.version == 8 && doc.subversion == 11 && doc.units.size() == 4, "unit header/count");
    const auto& unit = doc.units[0];
    Require(unit.rawcode == "H001" && unit.variation == 2 && unit.creationId == 100, "unit identity");
    Require(unit.position == std::array<float, 3>{-128.25f, 512.5f, 64.0f} && unit.rotation == 1.25f,
        "unit position and rotation");
    Require(unit.scale == std::array<float, 3>{0.75f, 1.0f, 1.5f}, "unit scale");
    Require(unit.flags == 2 && unit.owner == 7 && unit.hitPoints == 80 && unit.mana == -1, "unit ownership/life");
    Require(unit.unknownOwnerBytes == std::array<std::uint8_t, 2>{0x12, 0x34}, "opaque owner bytes must remain");
    Require(unit.droppedItemTable == 4 && unit.itemSets.size() == 2 && unit.itemSets[0].items.size() == 2,
        "drop table structure");
    Require(unit.itemSets[0].items[1].rawcode == "I002" && unit.itemSets[0].items[1].chance == 70 &&
        unit.itemSets[1].items[0].rawcode == "I003", "drop table values");
    Require(unit.gold == 12500 && unit.targetAcquisition == -2.0f, "gold/acquisition sentinel");
    Require(unit.heroLevel == 6 && unit.heroStrength == 31 && unit.heroAgility == 17 && unit.heroIntelligence == 22,
        "hero attributes");
    Require(unit.inventory.size() == 2 && unit.inventory[1].slot == 5 && unit.inventory[1].rawcode == "I005",
        "inventory order and slots");
    Require(unit.abilities.size() == 2 && unit.abilities[0].autocast && !unit.abilities[1].autocast &&
        unit.abilities[0].rawcode == "A001" && unit.abilities[0].level == 3, "ability initialization");
    Require(unit.teamColor == -1 && unit.waygateRegionId == 12, "unit trailing initialization fields");
    Require(std::holds_alternative<std::monostate>(unit.random), "normal unit random mode -1");
    const auto& any = std::get<RandomAnyUnit>(doc.units[1].random);
    Require(any.level == -1 && any.itemClass == 3, "signed 24-bit random level");
    const auto& global = std::get<RandomGlobalTable>(doc.units[2].random);
    Require(global.tableId == 9 && global.column == 2, "global random table");
    const auto& custom = std::get<RandomCustomTable>(doc.units[3].random);
    Require(custom.choices.size() == 2 && custom.choices[1].rawcode == "hrif" && custom.choices[1].chance == 75,
        "custom random table");
    Require(doc.SerializeOriginal() == fixture.bytes && doc.OriginalBytes() == fixture.bytes, "original byte roundtrip");
    // 读取字段不是写入接口，不能将修改后的视图假报为已序列化编辑数据。
    parsed.value->units[0].heroLevel = 100;
    Require(parsed.value->SerializeOriginal() == fixture.bytes, "SerializeOriginal is explicitly original bytes only");
}

void CheckDoodads() {
    const auto fixture = Doodads();
    const auto parsed = ParseDoodadPlacements(fixture.bytes);
    Require(parsed.Succeeded(), "classic doodads should parse");
    const auto& doc = *parsed.value;
    Require(doc.doodads.size() == 1 && doc.specialVersion == 0 && doc.specialDoodads.size() == 1, "doodad groups");
    const auto& doodad = doc.doodads[0];
    Require(doodad.rawcode == "LTlt" && doodad.creationId == 901 && doodad.variation == 2 &&
        doodad.position[0] == -128.25f && doodad.rotation == 1.25f, "doodad transform and identity");
    Require(doodad.flags == 2 && doodad.lifePercent == 75 && doodad.droppedItemTable == -1 &&
        doodad.itemSets[1].items[0].rawcode == "I003", "doodad drop and life settings");
    Require(doc.specialDoodads[0].rawcode == "YTab" && doc.specialDoodads[0].variation == 4 &&
        doc.specialDoodads[0].position == std::array<std::int32_t, 2>{16, 32}, "special doodad grid position");
    Require(doc.SerializeOriginal() == fixture.bytes, "doodad byte roundtrip");
}

void CheckCorruption() {
    const auto units = Units();
    const auto doodads = Doodads();
    for (std::size_t end = 0; end < units.bytes.size(); ++end) {
        Require(!ParseUnitPlacements(std::span(units.bytes).first(end)).Succeeded(), "truncated units accepted");
    }
    for (std::size_t end = 0; end < doodads.bytes.size(); ++end) {
        Require(!ParseDoodadPlacements(std::span(doodads.bytes).first(end)).Succeeded(), "truncated doodads accepted");
    }
    auto broken = units;
    broken.Patch(0, 0);
    RequireFailure(ParseUnitPlacements(broken.bytes), "PLACEMENT_MAGIC");
    broken = units; broken.Patch(4, 7);
    RequireFailure(ParseUnitPlacements(broken.bytes), "PLACEMENT_VERSION_UNSUPPORTED");
    broken = units; broken.Patch(8, 10);
    RequireFailure(ParseUnitPlacements(broken.bytes), "PLACEMENT_VERSION_UNSUPPORTED");
    broken = units; broken.Patch(12, 0xffffffffu);
    RequireFailure(ParseUnitPlacements(broken.bytes), "PLACEMENT_NEGATIVE_COUNT");
    broken = units; broken.Patch(12, 10000);
    RequireFailure(ParseUnitPlacements(broken.bytes), "PLACEMENT_TRUNCATED");
    broken = units; broken.Patch(broken.randomOffset, 3);
    RequireFailure(ParseUnitPlacements(broken.bytes), "PLACEMENT_RANDOM_MODE");
    broken = units; broken.Patch(broken.booleanOffset, 2);
    RequireFailure(ParseUnitPlacements(broken.bytes), "PLACEMENT_INVALID_BOOLEAN");
    broken = units; broken.bytes[52] = std::byte{'H'};
    RequireFailure(ParseUnitPlacements(broken.bytes), "PLACEMENT_LAYOUT_UNSUPPORTED");
    broken = units; broken.U8(0x42);
    RequireFailure(ParseUnitPlacements(broken.bytes), "PLACEMENT_TRAILING_DATA");
    broken = doodads; broken.Patch(broken.specialOffset, 1);
    RequireFailure(ParseDoodadPlacements(broken.bytes), "PLACEMENT_SPECIAL_VERSION");
    broken = doodads; broken.U8(0);
    RequireFailure(ParseDoodadPlacements(broken.bytes), "PLACEMENT_TRAILING_DATA");
    // 每个变换浮点字段分别覆盖 NaN/Infinity，而非只检查第一个位置分量。
    for (std::size_t offset = 24; offset <= 48; offset += 4) {
        broken = units; broken.Patch(offset, 0x7fc00000u);
        RequireFailure(ParseUnitPlacements(broken.bytes), "PLACEMENT_NONFINITE");
        broken = doodads; broken.Patch(offset, 0x7f800000u);
        RequireFailure(ParseDoodadPlacements(broken.bytes), "PLACEMENT_NONFINITE");
    }
}

void CheckBudgetsAndEmpty() {
    const auto fixture = Units();
    PlacementReadLimits limits;
    limits.maxBytes = fixture.bytes.size() - 1;
    RequireFailure(ParseUnitPlacements(fixture.bytes, limits), "PLACEMENT_BYTE_LIMIT");
    limits = {}; limits.maxPlacements = 3;
    RequireFailure(ParseUnitPlacements(fixture.bytes, limits), "PLACEMENT_RECORD_LIMIT");
    limits = {}; limits.maxNestedRecords = 3;
    RequireFailure(ParseUnitPlacements(fixture.bytes, limits), "PLACEMENT_RECORD_LIMIT");
    limits = {}; limits.maxPlacements = 1;
    RequireFailure(ParseDoodadPlacements(Doodads().bytes, limits), "PLACEMENT_RECORD_LIMIT");
    Fixture empty; empty.Header(0);
    Require(ParseUnitPlacements(empty.bytes).Succeeded(), "empty units must remain a valid document");
    const auto versionedUnits = ReadUnitPlacementsVersioned(empty.bytes);
    Require(versionedUnits.Succeeded() && versionedUnits.value->recordLayout == PlacementRecordLayout::Undetermined
        && versionedUnits.value->SerializeOriginal() == empty.bytes,
        "empty versioned units retain bytes with unknown layout");
    RequireWriteFailure(versionedUnits.value->Serialize(), "PLACEMENT_SKIN_WRITE_UNSUPPORTED");
    empty.U32(0); empty.U32(0);
    Require(ParseDoodadPlacements(empty.bytes).Succeeded(), "empty doodads must retain special version/count");
    const auto versionedDoodads = ReadDoodadPlacementsVersioned(empty.bytes);
    Require(versionedDoodads.Succeeded() && versionedDoodads.value->recordLayout == PlacementRecordLayout::Undetermined
        && versionedDoodads.value->SerializeOriginal() == empty.bytes,
        "empty versioned doodads retain bytes with unknown layout");
    RequireWriteFailure(versionedDoodads.value->Serialize(), "PLACEMENT_SKIN_WRITE_UNSUPPORTED");
}

void CheckVersionedSkinReadOnly() {
    Fixture skinnedUnits;
    skinnedUnits.Header(1);
    skinnedUnits.Unit(-1, 121, true);
    RequireFailure(ParseUnitPlacements(skinnedUnits.bytes), "PLACEMENT_LAYOUT_UNSUPPORTED");
    const auto units = ReadUnitPlacementsVersioned(skinnedUnits.bytes);
    Require(units.Succeeded() && units.value->recordLayout == PlacementRecordLayout::SkinId
        && units.value->units.size() == 1 && units.value->units[0].rawcode == "H001"
        && units.value->units[0].skinId == "H002" && units.value->units[0].owner == 7,
        "8/11 unit SkinId must be read between scale and flags");
    Require(units.value->SerializeOriginal() == skinnedUnits.bytes, "skinned units must retain exact source bytes");
    RequireWriteFailure(units.value->Serialize(), "PLACEMENT_SKIN_WRITE_UNSUPPORTED");

    const auto skinnedDoodads = Doodads(true);
    RequireFailure(ParseDoodadPlacements(skinnedDoodads.bytes), "PLACEMENT_LAYOUT_UNSUPPORTED");
    const auto doodads = ReadDoodadPlacementsVersioned(skinnedDoodads.bytes);
    Require(doodads.Succeeded() && doodads.value->recordLayout == PlacementRecordLayout::SkinId
        && doodads.value->doodads.size() == 1 && doodads.value->doodads[0].skinId == "LT02"
        && doodads.value->doodads[0].flags == 2 && doodads.value->specialDoodads.size() == 1
        && doodads.value->specialDoodads[0].rawcode == "YTab",
        "8/11 doodad SkinId must not shift the separate special-doodad segment");
    Require(doodads.value->SerializeOriginal() == skinnedDoodads.bytes,
        "skinned doodads must retain exact source bytes");
    RequireWriteFailure(doodads.value->Serialize(), "PLACEMENT_SKIN_WRITE_UNSUPPORTED");

    const auto classicUnits = ReadUnitPlacementsVersioned(Units().bytes);
    const auto classicDoodads = ReadDoodadPlacementsVersioned(Doodads().bytes);
    Require(classicUnits.Succeeded() && classicUnits.value->recordLayout == PlacementRecordLayout::Classic
        && !classicUnits.value->units[0].skinId, "classic units must keep absent skin distinct from TypeId");
    Require(classicDoodads.Succeeded() && classicDoodads.value->recordLayout == PlacementRecordLayout::Classic
        && !classicDoodads.value->doodads[0].skinId, "classic doodads must keep absent skin distinct from TypeId");
}

void CheckWritablePlacements() {
    auto units = ParseUnitPlacements(Units().bytes);
    auto doodads = ParseDoodadPlacements(Doodads().bytes);
    Require(units.Succeeded() && doodads.Succeeded(), "placement writer fixtures must parse");
    Require(units.value->Serialize().Succeeded() && *units.value->Serialize().value == units.value->OriginalBytes(),
        "unchanged units must remain byte-identical");
    Require(doodads.value->Serialize().Succeeded() && *doodads.value->Serialize().value == doodads.value->OriginalBytes(),
        "unchanged doodads must remain byte-identical");

    units.value->units[0].owner = 4;
    units.value->units[0].position[0] = -64.5f;
    units.value->units[0].abilities[0].level = 7;
    units.value->units[0].itemSets[0].items[1].chance = 55;
    units.value->units.push_back(units.value->units[0]);
    units.value->units.back().creationId = 808;
    units.value->units.erase(units.value->units.begin() + 1);
    const auto changedUnits = units.value->Serialize();
    Require(changedUnits.Succeeded(), "changed units must serialize");
    const auto reparsedUnits = ParseUnitPlacements(*changedUnits.value);
    Require(reparsedUnits.Succeeded() && reparsedUnits.value->units.size() == 4
        && reparsedUnits.value->units[0].owner == 4
        && reparsedUnits.value->units[0].position[0] == -64.5f
        && reparsedUnits.value->units[0].abilities[0].level == 7
        && reparsedUnits.value->units[0].itemSets[0].items[1].chance == 55
        && reparsedUnits.value->units.back().creationId == 808,
        "unit owner, transform, nested items/abilities, add/delete must roundtrip");
    Require(std::holds_alternative<RandomGlobalTable>(reparsedUnits.value->units[1].random)
        && std::holds_alternative<RandomCustomTable>(reparsedUnits.value->units[2].random),
        "random-unit branch identities must survive removal and write");

    doodads.value->doodads[0].lifePercent = 63;
    doodads.value->doodads[0].itemSets[1].items[0].rawcode = "I777";
    doodads.value->specialDoodads[0].position[0] = 24;
    const auto changedDoodads = doodads.value->Serialize();
    Require(changedDoodads.Succeeded(), "changed doodads must serialize");
    const auto reparsedDoodads = ParseDoodadPlacements(*changedDoodads.value);
    Require(reparsedDoodads.Succeeded() && reparsedDoodads.value->doodads[0].lifePercent == 63
        && reparsedDoodads.value->doodads[0].itemSets[1].items[0].rawcode == "I777"
        && reparsedDoodads.value->specialDoodads[0].position[0] == 24,
        "doodad and special-doodad changes must roundtrip");

    units.value->units[0].flags = 0x20;
    Require(!units.value->Serialize().Succeeded(), "unsupported classic flags must reject writing");
    units.value->units[0].flags = 2;
    auto overflowUnits = ParseUnitPlacements(Units().bytes);
    std::get<RandomAnyUnit>(overflowUnits.value->units[1].random).level = -8388609;
    Require(!overflowUnits.value->Serialize().Succeeded(), "random 24-bit overflow must reject writing");
    doodads.value->specialVersion = 1;
    Require(!doodads.value->Serialize().Succeeded(), "unsupported special layout must reject writing");
}

} // namespace

int main() {
    try {
        CheckUnits();
        CheckDoodads();
        CheckCorruption();
        CheckBudgetsAndEmpty();
        CheckVersionedSkinReadOnly();
        CheckWritablePlacements();
        CheckClassicDoodadRecordWireMeasurement();
        std::cout << "Map placements parser tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
