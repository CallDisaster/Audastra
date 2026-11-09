#include "../OwnLua/NativeTypeHierarchy.h"
#include "../OwnLua/GameNativeProvider.h"
#include "../OwnLua/GameNativeObserver.h"
#include "War3MapCore/Mpq/MpqWriter.h"

#include <windows.h>
#include <array>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
using namespace Audastra::Runtime32::OwnLua;
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
struct Temp {
    std::filesystem::path root = std::filesystem::temp_directory_path()
        / (L"audastra-lua-types-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    Temp() { Check(std::filesystem::create_directory(root), "fixture directory"); }
    ~Temp() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
};
void Archive(const std::filesystem::path& path, std::string_view source, bool withBlizzard = true) {
    const std::vector<std::byte> bytes{reinterpret_cast<const std::byte*>(source.data()),
        reinterpret_cast<const std::byte*>(source.data() + source.size())};
    std::vector<War3Map::Mpq::MpqBuildFile> members{{"scripts\\common.j", bytes}};
    if (withBlizzard) {
        constexpr std::string_view blizzard="globals\ninteger FixtureBlizzard=0\nendglobals\n";
        members.push_back({"scripts\\blizzard.j", {reinterpret_cast<const std::byte*>(blizzard.data()),
            reinterpret_cast<const std::byte*>(blizzard.data()+blizzard.size())}});
    }
    Check(War3Map::Mpq::WriteMpqArchive(path, members).Succeeded(), "write original common fixture");
}
void ParseAndSources() {
    const auto original = NativeTypeHierarchy::Parse(R"(
// 原创名称用于验证真实语法解析，不依赖安装中的游戏文件。
type base extends handle
type widget extends base
type unit extends widget
type timer extends base
globals
    constant integer FixtureCount = 17
    real FixtureScale = 1.5
    boolean FixtureEnabled = false
    string FixtureText = null
    unit FixtureUnit = null
    timer array FixtureTimers
    code FixtureCallback = null
endglobals
constant native Example takes widget w returns nothing
)");
    if (!original.types) throw std::runtime_error(original.error);
    Check(original.types->Size() == 5 && original.types->IsA("unit", "widget")
        && original.types->IsA("unit", "handle") && original.types->IsA("timer", "base"), "declared transitive parents");
    Check(!original.types->IsA("unit", "timer") && !original.types->IsA("widget", "unit")
        && !original.types->IsA("invented", "handle") && !original.types->IsA("unit", "invented"), "unknown and wrong direction refused");
    const auto count = original.types->Global("FixtureCount"), timers = original.types->Global("FixtureTimers");
    Check(count && count->constant && !count->array && count->type.kind == NativeKind::Integer,
        "constant global declaration is metadata, not a guessed runtime value");
    Check(timers && timers->array && timers->type.kind == NativeKind::Handle && timers->type.handleType == "timer",
        "array and concrete handle type preserved for later capability rejection");
    Check(original.types->Global("FixtureScale")->type.kind == NativeKind::Real
        && original.types->Global("FixtureEnabled")->type.kind == NativeKind::Boolean
        && original.types->Global("FixtureText")->type.kind == NativeKind::String
        && original.types->Global("FixtureCallback")->type.kind == NativeKind::Code
        && !original.types->Global("Absent"), "primitive declarations and unknown name");
    for (const auto source : {"", "native X takes nothing returns nothing", "type a extends missing",
        "type a extends a", "type a extends handle\ntype a extends handle", "type handle extends handle",
        "type a extends handle\ntype b extends", "type a extends b\ntype b extends a"})
        Check(!NativeTypeHierarchy::Parse(source).types, "incomplete, cyclic, duplicate declarations rejected");
    for (const auto declarations : {"unknown x", "integer x\nreal x", "nothing x"})
        Check(!NativeTypeHierarchy::Parse(std::string("type a extends handle\nglobals\n") + declarations + "\nendglobals").types,
            "unknown and duplicate global declarations rejected");
    Check(!NativeTypeHierarchy::Parse(std::string(4 * 1024 * 1024 + 1, ' ')).types, "source budget");
    Temp temp;
    Archive(temp.root / "war3.mpq", "type frombase extends handle");
    Archive(temp.root / "war3x.mpq", "type fromexpansion extends handle");
    Archive(temp.root / "war3xlocal.mpq", "type fromlocal extends handle");
    Archive(temp.root / "war3patch.mpq", "type frompatch extends handle");
    auto loaded = NativeTypeHierarchy::ReadGame(temp.root);
    Check(loaded.types && loaded.types->IsA("frompatch", "handle")
        && !loaded.types->IsA("frombase", "handle") && loaded.sourceArchive == "war3patch.mpq"
        && loaded.types->Global("FixtureBlizzard"),
        "actual highest-priority complete source, without merging incompatible type tables");
    std::filesystem::remove(temp.root / "war3patch.mpq");
    loaded = NativeTypeHierarchy::ReadGame(temp.root);
    Check(loaded.types && loaded.types->IsA("fromlocal", "handle") && loaded.sourceArchive == "war3xlocal.mpq", "local priority");
    Archive(temp.root / "war3patch.mpq", "type invalid extends unknown");
    loaded = NativeTypeHierarchy::ReadGame(temp.root);
    Check(!loaded.types && !loaded.error.empty(), "bad high-priority syntax never falls back");
    std::filesystem::remove(temp.root / "war3patch.mpq");
    std::ofstream bad(temp.root / "war3patch.mpq", std::ios::binary); bad << "corrupt fixture"; bad.close();
    Check(!NativeTypeHierarchy::ReadGame(temp.root).types, "corrupt high-priority archive never falls back");
    Temp missing;
    Archive(missing.root / "war3.mpq", "type onlycommon extends handle", false);
    const auto incomplete=NativeTypeHierarchy::ReadGame(missing.root);
    Check(!incomplete.types && incomplete.error.find("Blizzard.j")!=std::string::npos,
        "global lookup now requires Blizzard declarations; missing input is never silently ignored");
}
void ProductionIdentityGates() {
    Check(!GameVm127::OpenAtScriptEntry(static_cast<HMODULE>(nullptr), 1).vm, "null game refused without ABI access");
    Check(!GameVm127::OpenAtScriptEntry(GetModuleHandleW(nullptr), 0).vm, "zero generation refused");
    const auto wrongImage = GameVm127::OpenAtScriptEntry(GetModuleHandleW(nullptr), 1);
    Check(!wrongImage.vm && wrongImage.error.find("1.27a") != std::string::npos, "fixture PE cannot masquerade as known Game image");
    GameNativeProvider absent({}, std::make_shared<NativeRegistry>(), {});
    Check(!absent.Generation() && !absent.Lookup("Example") && !absent.Invoke("Example", {}).succeeded,
        "missing production VM never invokes arbitrary registered address");
    Check(!GameVm127::ReadStringObject(0) && !GameVm127::ReadStringObject(1), "invalid string objects safely rejected");
    Check(!GameImage127::Open(nullptr).image && !GameImage127::Open(GetModuleHandleW(nullptr)).image,
        "observation identity cannot use an unverified fixture image");
    std::string error;
    Check(!InstallGameNativeObserver({}, {}, error) && !error.empty(), "observer never patches without verified image and lifecycle handlers");
}
void WeatherSources() {
    Temp temp;
    const auto add = ParseNativeSignature("(Hrect;I)Hweathereffect;").value();
    const auto check = [&](const WeatherTemplates& table, std::int32_t id) {
        const std::array<NativeValue, 2> args{NativeHandle{1, 1, "rect", {}}, id};
        return table.CheckCall("AddWeatherEffect", add, args);
    };
    constexpr std::string_view header = "ID;P\nC;X1;Y1;K\"effectID\"\n";
    const auto valid = std::string(header) + "C;X1;Y2;K\"R053\"\nE\n";
    const auto override = std::string(header) + "C;X1;Y2;K\"O053\"\nE\n";
    constexpr std::int32_t gameId = 0x52303533, mapId = 0x4f303533;
    const auto absent = [](std::string_view, std::size_t) { return LuaModuleRead{}; };
    Archive(temp.root / "war3.mpq", "type weatherfixture extends handle");
    const auto missing = WeatherTemplates::Read(absent, temp.root);
    const auto missingError = check(missing, gameId);
    Check(missingError.find("缺失") != std::string::npos && missingError.find(std::to_string(gameId)) != std::string::npos,
        "missing game weather table fails closed with requested ID");
    const std::vector<War3Map::Mpq::MpqBuildFile> members{{"TerrainArt\\Weather.slk",
        {reinterpret_cast<const std::byte*>(valid.data()), reinterpret_cast<const std::byte*>(valid.data() + valid.size())}}};
    Check(War3Map::Mpq::WriteMpqArchive(temp.root / "war3patch.mpq", members).Succeeded(), "write original weather fixture");
    const auto game = WeatherTemplates::Read(absent, temp.root);
    Check(check(game, gameId).empty(), "accept ID in effective game SLK");
    Check(check(game, 0).find("天气编号 0 被拒绝") != std::string::npos, "unknown template zero is refused before invocation");
    Check(!game.CheckCall("UnknownWeatherProducer", add, {}).empty(), "other producers cannot create weather ordinals");
    const std::array<NativeValue, 2> nullRect{std::monostate{}, gameId};
    Check(game.CheckCall("AddWeatherEffect", add, nullRect).find("rect") != std::string::npos,
        "valid template cannot bypass null rect gate");
    const auto map = WeatherTemplates::Read([&](std::string_view name, std::size_t bytes) {
        Check(name == "TerrainArt\\Weather.slk" && bytes == 1024 * 1024, "only bounded weather member selected");
        return LuaModuleRead{true, override, {}};
    }, temp.root);
    Check(check(map, mapId).empty() && !check(map, gameId).empty(), "map override replaces rather than merges game table");
    for (const auto& corrupt : {std::string{}, valid + "C;X1;Y3;K\"O053\"\n",
        std::string(header) + "C;X1;Y2;K\"O053\"\nC;X1;Y3;K\"O053\"\nE\n",
        std::string(header) + "C;X1;Y2;K\"O053\"\nC;X2;Y3;K\"missing ID\"\nE\n"}) {
        const auto table = WeatherTemplates::Read([&](std::string_view, std::size_t) {
            return LuaModuleRead{true, corrupt, {}};
        }, temp.root);
        Check(check(table, mapId).find("无效") != std::string::npos && !check(table, gameId).empty(),
            "corrupt map table never permits partial IDs or falls back");
    }
    const auto unreadable = WeatherTemplates::Read([](std::string_view, std::size_t) {
        return LuaModuleRead{false, {}, "fixture read error"};
    }, temp.root);
    Check(check(unreadable, gameId).find("fixture read error") != std::string::npos, "failed map read blocks game fallback");
}
}

int main() {
    try { ParseAndSources(); ProductionIdentityGates(); WeatherSources(); std::cout << "own Lua type and weather source tests passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
