#include "../OwnLua/NativeTypeHierarchy.h"
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
constant native Example takes widget w returns nothing
)");
    if (!original.types) throw std::runtime_error(original.error);
    Check(original.types->Size() == 5 && original.types->IsA("unit", "widget")
        && original.types->IsA("unit", "handle") && original.types->IsA("timer", "base"), "declared transitive parents");
    Check(!original.types->IsA("unit", "timer") && !original.types->IsA("widget", "unit")
        && !original.types->IsA("invented", "handle") && !original.types->IsA("unit", "invented"), "unknown and wrong direction refused");
    for (const auto source : {"", "native X takes nothing returns nothing", "type a extends missing",
        "type a extends a", "type a extends handle\ntype a extends handle", "type handle extends handle",
        "type a extends handle\ntype b extends", "type a extends b\ntype b extends a"})
        Check(!NativeTypeHierarchy::Parse(source).types, "incomplete, cyclic, duplicate declarations rejected");
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
}

int main() {
    try { ParseAndSources(); std::cout << "own Lua type source tests passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
