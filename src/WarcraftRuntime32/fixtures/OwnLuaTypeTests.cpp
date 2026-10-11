#include "../OwnLua/NativeTypeHierarchy.h"
#include "../OwnLua/GameNativeProvider.h"
#include "../OwnLua/GameNativeObserver.h"
#include "War3MapCore/Mpq/MpqWriter.h"

#include <windows.h>
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
void Archive(const std::filesystem::path& path, std::string_view source) {
    const std::vector<std::byte> bytes{reinterpret_cast<const std::byte*>(source.data()),
        reinterpret_cast<const std::byte*>(source.data() + source.size())};
    const std::vector<War3Map::Mpq::MpqBuildFile> members{{"scripts\\common.j", bytes}};
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
        && !loaded.types->IsA("frombase", "handle") && loaded.sourceArchive == "war3patch.mpq",
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
}

int main() {
    try { ParseAndSources(); ProductionIdentityGates(); std::cout << "own Lua type source tests passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
