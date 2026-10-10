#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// 原创确定性格式夹具；不含 Blizzard 地图或二进制资源。
namespace MapDataFixtures {

struct Fixture {
    std::vector<std::byte> bytes;
    std::map<std::string, std::size_t> offsets;

    void Mark(std::string key) { offsets.emplace(std::move(key), bytes.size()); }
    void UInt(std::uint32_t value) {
        for (unsigned shift = 0; shift < 32; shift += 8) {
            bytes.push_back(static_cast<std::byte>((value >> shift) & 0xff));
        }
    }
    void Int(std::int32_t value) { UInt(std::bit_cast<std::uint32_t>(value)); }
    void Float(float value) { UInt(std::bit_cast<std::uint32_t>(value)); }
    void Byte(std::uint8_t value) { bytes.push_back(static_cast<std::byte>(value)); }
    void Raw(std::string_view value) {
        if (value.size() != 4) throw std::invalid_argument("fixture rawcode must contain four bytes");
        for (const auto character : value) bytes.push_back(static_cast<std::byte>(character));
    }
    void String(std::string_view value) {
        for (const auto character : value) bytes.push_back(static_cast<std::byte>(character));
        Byte(0);
    }
    void Patch(std::string_view key, std::uint32_t value) {
        const auto offset = offsets.at(std::string(key));
        for (unsigned index = 0; index < 4; ++index) {
            bytes.at(offset + index) = static_cast<std::byte>((value >> (index * 8)) & 0xff);
        }
    }
};

inline Fixture FullMapSettingsV25() {
    Fixture fixture;
    fixture.Int(25);
    fixture.Int(7);
    fixture.Int(6061);
    fixture.String(std::string("Map-") + static_cast<char>(0xe9));
    fixture.String("Creator");
    fixture.String("TRIGSTR_0042");
    fixture.String("2-4");
    fixture.Mark("camera");
    for (int index = 0; index < 8; ++index) fixture.Float(-1000.5f + index * 256.0f);
    for (const auto margin : {6, 7, 8, 9}) fixture.Int(margin);
    fixture.Int(116);
    fixture.Int(112);
    fixture.UInt(0x81234567);
    fixture.Byte('L');
    fixture.Int(-1);
    fixture.String("Loading\\Scene.mdx");
    fixture.String("Loading text");
    fixture.String("Loading title");
    fixture.String("Loading subtitle");
    fixture.Int(1);
    fixture.String("Prologue\\Scene.mdx");
    fixture.String("Prologue text");
    fixture.String("Prologue title");
    fixture.String("Prologue subtitle");
    fixture.Int(2);
    fixture.Mark("fog");
    fixture.Float(1000.0f);
    fixture.Float(5000.0f);
    fixture.Float(0.75f);
    for (const auto channel : {10, 20, 30, 40}) fixture.Byte(static_cast<std::uint8_t>(channel));
    fixture.Raw("RAhr");
    fixture.String("Default");
    fixture.Byte('F');
    for (const auto channel : {50, 60, 70, 80}) fixture.Byte(static_cast<std::uint8_t>(channel));
    fixture.Mark("players");
    fixture.Int(2);
    for (int index = 0; index < 2; ++index) {
        fixture.Int(index);
        fixture.Int(index + 1);
        fixture.Int(index + 1);
        fixture.UInt(1);
        fixture.String(index == 0 ? "Human slot" : "Computer slot");
        if (index == 0) fixture.Mark("start");
        fixture.Float(index == 0 ? -512.5f : 256.0f);
        fixture.Float(index == 0 ? 320.0f : -256.0f);
        fixture.UInt(2);
        fixture.UInt(1);
    }
    fixture.Mark("forces");
    fixture.Int(1);
    fixture.UInt(0x80000033);
    fixture.UInt(3);
    fixture.String("Alliance");
    fixture.Mark("upgrades");
    fixture.Int(1);
    fixture.UInt(3);
    fixture.Raw("Rhme");
    fixture.Int(2);
    fixture.Int(2);
    fixture.Mark("tech");
    fixture.Int(2);
    fixture.UInt(1);
    fixture.Raw("hfoo");
    fixture.UInt(2);
    fixture.Raw("AHbz");
    fixture.Mark("unitTables");
    fixture.Int(1);
    fixture.Int(4);
    fixture.String("Patrol");
    fixture.Mark("columns");
    fixture.Int(3);
    fixture.Int(0);
    fixture.Int(1);
    fixture.Int(2);
    fixture.Mark("rows");
    fixture.Int(2);
    fixture.Int(75);
    fixture.Raw("hfoo");
    fixture.Raw("hbar");
    fixture.Raw("phea");
    fixture.Int(25);
    fixture.Raw("hrif");
    fixture.Raw(std::string_view("\0\0\0\0", 4));
    fixture.Raw("YjI/");
    fixture.Mark("itemTables");
    fixture.Int(1);
    fixture.Int(8);
    fixture.String("Loot");
    fixture.Mark("sets");
    fixture.Int(2);
    fixture.Mark("items");
    fixture.Int(2);
    fixture.Int(60);
    fixture.Raw("phea");
    fixture.Int(40);
    fixture.Raw("pman");
    fixture.Int(1);
    fixture.Int(100);
    fixture.Raw("YjI/");
    return fixture;
}

// war3mapUnits.doo：W3do，v8/sub11，无预放置单位或物品。
inline std::vector<std::byte> EmptyUnitsV8Sub11() {
    Fixture fixture;
    fixture.Raw("W3do");
    fixture.Int(8);
    fixture.Int(11);
    fixture.Int(0);
    return std::move(fixture.bytes);
}

// war3map.doo：v8/sub11，无普通装饰物；special v0，无特殊装饰物。
inline std::vector<std::byte> EmptyDoodadsV8Sub11() {
    Fixture fixture;
    fixture.Raw("W3do");
    fixture.Int(8);
    fixture.Int(11);
    fixture.Int(0);
    fixture.Int(0);
    fixture.Int(0);
    return std::move(fixture.bytes);
}

} // namespace MapDataFixtures

