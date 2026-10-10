#include "War3MapCore/Runtime/RuntimeLuaApiInventory.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string_view>

namespace {
using namespace War3Map::Runtime;
void Require(bool value, const char* why) { if (!value) throw std::runtime_error(why); }
struct Fixture {
    std::vector<std::byte> bytes = std::vector<std::byte>(0x3400);
    std::map<std::string, std::uint32_t> strings, imports;
    std::vector<std::uint32_t> relocations;
    std::uint32_t nextString = 0x2600;
    static constexpr std::uint32_t base = 0x400000;
    static std::size_t Raw(std::uint32_t rva) {
        if (rva >= 0x1000 && rva < 0x4000) return rva - 0x1000 + 0x400;
        return rva;
    }
    void Byte(std::size_t at, std::uint8_t value) { bytes.at(at) = static_cast<std::byte>(value); }
    void Word(std::size_t at, std::uint16_t value) { Byte(at, value & 255); Byte(at + 1, value >> 8); }
    void Dword(std::size_t at, std::uint32_t value) { Word(at, value & 65535); Word(at + 2, value >> 16); }
    std::uint32_t String(std::string_view value) {
        const auto key = std::string(value);
        if (strings.contains(key)) return strings.at(key);
        const auto rva = nextString; nextString += static_cast<std::uint32_t>(value.size() + 1);
        for (std::size_t i = 0; i < value.size(); ++i) Byte(Raw(rva) + i, static_cast<std::uint8_t>(value[i]));
        strings.emplace(key, rva); return rva;
    }
    struct Code {
        Fixture& fixture;
        std::uint32_t rva;
        void Emit(std::initializer_list<std::uint8_t> values) { for (const auto value : values) fixture.Byte(Raw(rva++), value); }
        void Pointer(std::uint32_t target) {
            fixture.Dword(Raw(rva), base + target); fixture.relocations.push_back(rva); rva += 4;
        }
        void Call(std::string_view name) { Emit({0xff, 0x15}); Pointer(fixture.imports.at(std::string(name))); }
        void Relative(std::uint32_t target) {
            Emit({0xe8}); fixture.Dword(Raw(rva), target - (rva + 4)); rva += 4;
        }
    };
    Fixture() {
        Word(0, 0x5a4d); Dword(0x3c, 0x80); Dword(0x80, 0x4550); Word(0x84, 0x14c);
        Word(0x86, 3); Word(0x94, 224); Word(0x98, 0x10b); Dword(0x98 + 28, base); Dword(0x98 + 92, 16);
        Dword(0x98 + 56, 0x4000);
        const auto section = [&](std::size_t at, std::uint32_t rva, std::uint32_t flags) {
            Dword(at + 8, 0x1000); Dword(at + 12, rva); Dword(at + 16, 0x1000);
            Dword(at + 20, static_cast<std::uint32_t>(Raw(rva))); Dword(at + 36, flags);
        };
        section(0x178, 0x1000, 0x60000020); section(0x1a0, 0x2000, 0x40000040); section(0x1c8, 0x3000, 0x40000040);
        Dword(0x98 + 96, 0x2000); Dword(0x98 + 100, 40);
        Dword(Raw(0x2000) + 20, 1); Dword(Raw(0x2000) + 24, 1);
        Dword(Raw(0x2000) + 28, 0x2050); Dword(Raw(0x2000) + 32, 0x2054); Dword(Raw(0x2000) + 36, 0x2058);
        Dword(Raw(0x2050), 0x1100); Dword(Raw(0x2054), String("Initialize")); Byte(Raw(0x1100), 0xc3);
        Dword(0x98 + 104, 0x2100); Dword(0x98 + 108, 60);
        const auto library = [&](unsigned descriptor, std::uint32_t thunk, std::uint32_t iat,
            std::string_view dll, std::initializer_list<std::string_view> names) {
            const auto at = Raw(0x2100 + descriptor * 20);
            Dword(at, thunk); Dword(at + 12, String(dll)); Dword(at + 16, iat);
            unsigned i = 0;
            for (const auto name : names) {
                const auto entry = nextString; nextString += 2;
                String(name); // hint(0) followed by actual name bytes.
                Dword(Raw(thunk + i * 4), entry); Dword(Raw(iat + i * 4), entry);
                imports.emplace(std::string(name), iat + i * 4); ++i;
            }
        };
        library(0, 0x2200, 0x2300, "lua53.dll", {"lua_getfield", "lua_pushcclosure", "lua_setfield",
            "lua_settop", "lua_pushstring", "lua_rawset", "luaL_setfuncs"});
        library(1, 0x2240, 0x2340, "ydwar3.dll", {"?jass_func@jass@fake", "?is_valid@func_value@jass@fake"});
        Code helper{*this, 0x1000};
        helper.Emit({0x55, 0x8b, 0xec, 0x56, 0x57, 0x68}); helper.Pointer(String("_PRELOAD"));
        helper.Emit({0x8b, 0xf9, 0x8b, 0xf2, 0x68, 0xd8, 0xb9, 0xf0, 0xff, 0x57}); helper.Call("lua_getfield");
        helper.Emit({0x6a, 0, 0xff, 0x75, 8, 0x57}); helper.Call("lua_pushcclosure");
        helper.Emit({0x56, 0x6a, 0xfe, 0x57}); helper.Call("lua_setfield");
        helper.Emit({0x6a, 0xfe, 0x57}); helper.Call("lua_settop");
        helper.Emit({0x83, 0xc4, 0x2c, 0x5f, 0x5e, 0x5d, 0xc3});
        Code registration{*this, 0x1180};
        registration.Emit({0xba}); registration.Pointer(String("jass.common"));
        registration.Emit({0x68}); registration.Pointer(0x1200); registration.Emit({0x89, 0x75, 0xd8}); registration.Relative(0x1000);
        registration.Emit({0xba}); registration.Pointer(String("jass.log")); registration.Emit({0xc7, 0x04, 0x24});
        registration.Pointer(0x1300); registration.Emit({0x8b, 0xce}); registration.Relative(0x1000);
        Code factory{*this, 0x1200};
        factory.Emit({0x55, 0x8b, 0xec, 0x8b, 0x7d, 8, 0x68}); factory.Pointer(String("__index"));
        factory.Emit({0x57}); factory.Call("lua_pushstring");
        factory.Emit({0x6a, 0, 0x68}); factory.Pointer(0x1500); factory.Emit({0x57}); factory.Call("lua_pushcclosure");
        factory.Emit({0x6a, 0xfd, 0x57}); factory.Call("lua_rawset"); factory.Emit({0xc3});
        Code lookup{*this, 0x1500}; lookup.Call("?jass_func@jass@fake"); lookup.Call("?is_valid@func_value@jass@fake");
        lookup.Call("lua_pushcclosure"); lookup.Emit({0xc3});
        Code logger{*this, 0x1300}; logger.Emit({0x6a, 0, 0x68}); logger.Pointer(0x2500);
        logger.Emit({0x57}); logger.Call("luaL_setfuncs"); logger.Emit({0xc3});
        Dword(Raw(0x2500), base + String("fixture_field")); Dword(Raw(0x2504), base + 0x1600);
        relocations.push_back(0x2500); relocations.push_back(0x2504); Byte(Raw(0x1600), 0xc3);
        RebuildRelocations();
    }
    void RebuildRelocations() {
        std::sort(relocations.begin(), relocations.end());
        std::uint32_t at = 0x3000;
        for (std::size_t i = 0; i < relocations.size();) {
            const auto page = relocations[i] & 0xfffff000u;
            std::size_t end = i;
            while (end < relocations.size() && (relocations[end] & 0xfffff000u) == page) ++end;
            const auto size = static_cast<std::uint32_t>(8 + (end - i) * 2);
            Dword(Raw(at), page); Dword(Raw(at) + 4, size);
            for (auto j = i; j < end; ++j) Word(Raw(at) + 8 + (j - i) * 2,
                static_cast<std::uint16_t>(0x3000u | (relocations[j] & 0xfffu)));
            at += size; i = end;
        }
        Dword(0x98 + 136, 0x3000); Dword(0x98 + 140, at - 0x3000);
    }
};
const RuntimeApiModule& Module(const RuntimeApiInventory& inventory, std::string_view name) {
    const auto it = std::find_if(inventory.modules.begin(), inventory.modules.end(), [&](const auto& module) { return module.name == name; });
    Require(it != inventory.modules.end(), "Actual registered module missing"); return *it;
}
void CheckProof() {
    Fixture fixture;
    const auto inventory = InspectRuntimeLuaEngine(fixture.bytes);
    Require(inventory.trusted && inventory.modules.size() == 2 && !inventory.modulesComplete, "Registered fixture proof failed");
    Require(Module(inventory, "jass.common").commonLookup, "Actual common native bridge was not proved");
    const auto& log = Module(inventory, "jass.log");
    Require(!log.fieldsComplete && log.fields.size() == 1 && log.fields[0].name == "fixture_field"
        && log.fields[0].registrationRva && log.fields[0].functionRva == 0x1600, "Actual luaL_Reg field missing");
}
void CheckZeroFilledDataExport() {
    Fixture fixture;
    Require(fixture.nextString < 0x2a00, "Fixture names must retain actual raw bytes");
    fixture.Dword(0x1a0 + 16, 0xa00); // virtual size remains 0x1000; tail [2a00,3000) is loader zero-fill.
    fixture.Dword(Fixture::Raw(0x2000) + 20, 2); fixture.Dword(Fixture::Raw(0x2000) + 24, 2);
    fixture.Dword(Fixture::Raw(0x2000) + 28, 0x2070); fixture.Dword(Fixture::Raw(0x2000) + 32, 0x2078);
    fixture.Dword(Fixture::Raw(0x2000) + 36, 0x2080);
    fixture.Dword(Fixture::Raw(0x2070), 0x1100); fixture.Dword(Fixture::Raw(0x2074), 0x2f00);
    fixture.Dword(Fixture::Raw(0x2078), fixture.strings.at("Initialize"));
    fixture.Dword(Fixture::Raw(0x207c), fixture.String("OwnedZeroData"));
    fixture.Word(Fixture::Raw(0x2080), 0); fixture.Word(Fixture::Raw(0x2082), 1);
    Require(InspectRuntimeLuaEngine(fixture.bytes).trusted, "Legal virtual zero DATA export was rejected");
    auto changed = fixture; changed.Dword(Fixture::Raw(0x2070), 0x2f00);
    Require(!InspectRuntimeLuaEngine(changed.bytes).trusted, "Virtual DATA became Initialize code");
    changed = fixture; changed.Dword(0x1a0 + 36, 0x60000020);
    Require(!InspectRuntimeLuaEngine(changed.bytes).trusted, "Virtual code export without raw bytes was accepted");
    changed = fixture; changed.Dword(Fixture::Raw(0x2074), 0x4001);
    Require(!InspectRuntimeLuaEngine(changed.bytes).trusted, "Out-of-image DATA export was accepted");
    changed = fixture; changed.Dword(0x1a0 + 8, 0x1400);
    Require(!InspectRuntimeLuaEngine(changed.bytes).trusted, "Overlapping virtual mapping was accepted");
}
void CheckNegativeProofs() {
    Fixture fixture;
    const auto fails = [&](Fixture changed, const char* label) {
        Require(!InspectRuntimeLuaEngine(changed.bytes).trusted, label);
    };
    auto changed = fixture; changed.Byte(Fixture::Raw(0x1000) + 16, 0); fails(changed, "Wrong registry constant accepted");
    changed = fixture; changed.Byte(Fixture::Raw(0x1000) + 43, 0); fails(changed, "Wrong Lua API instruction accepted");
    changed = fixture; changed.Dword(0x98 + 140, 0); fails(changed, "Missing relocations accepted");
    changed = fixture; changed.Dword(Fixture::Raw(0x1180) + 6, Fixture::base + 0x2000);
    changed.Dword(Fixture::Raw(0x1180) + 18 + 8, Fixture::base + 0x2000);
    fails(changed, "Data addresses became module code pointers");
    changed = fixture; changed.Dword(Fixture::Raw(0x2050), 0x2000); fails(changed, "Data export became Initialize code");
    changed = fixture; changed.Dword(0x1a0 + 12, 0x1000); fails(changed, "Overlapping PE mapping accepted");
    changed = fixture; changed.bytes.resize(512); fails(changed, "Truncated PE accepted");
    changed = fixture; changed.Dword(Fixture::Raw(0x1180) + 18 + 1, Fixture::base + fixture.strings.at("jass.common"));
    fails(changed, "Ambiguous repeated registration accepted");
    std::stop_source cancel; cancel.request_stop();
    Require(InspectRuntimeLuaEngine(fixture.bytes, cancel.get_token()).cancelled, "Cancellation ignored");
    Require(!InspectRuntimeLuaEngine(fixture.bytes, {}, std::chrono::steady_clock::now()).trusted, "Deadline ignored");
    const auto unconfigured = CaptureRuntimeLuaApiInventory({});
    Require(!unconfigured->trusted && unconfigured->reason.find("目前只检查语法") != std::string::npos,
        "Unconfigured inventory did not remain syntax-only");
}
}
int main() {
    try { CheckProof(); CheckZeroFilledDataExport(); CheckNegativeProofs(); std::cout << "Static Lua inventory: 3 grouped cases passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
