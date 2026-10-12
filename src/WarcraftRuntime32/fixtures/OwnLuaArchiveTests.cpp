#include "../OwnLua/LuaMapArchive.h"
#include "../RuntimeFiles.h"
#include "War3MapCore/Mpq/MpqWriter.h"

#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
using namespace Audastra::Runtime32;
using namespace Audastra::Runtime32::OwnLua;
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
struct Temp {
    std::filesystem::path root = std::filesystem::temp_directory_path()
        / (L"audastra-lua-archive-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    Temp() { Check(std::filesystem::create_directory(root), "create owned fixture directory"); }
    ~Temp() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
};
std::vector<std::byte> Bytes(std::string_view value) {
    return {reinterpret_cast<const std::byte*>(value.data()), reinterpret_cast<const std::byte*>(value.data() + value.size())};
}
std::string Hash(const std::filesystem::path& path) {
    Handle file(OpenLockedFile(path)); Check(file.value != INVALID_HANDLE_VALUE, "open fixture hash handle");
    const auto hash = Sha256(file.value); Check(hash.size() == 64, "fixture hash"); return hash;
}
void ArchiveIdentityAndHost() {
    Temp temp;
    const auto map = temp.root / "original.w3x";
    const std::vector<War3Map::Mpq::MpqBuildFile> members{
        {"main.lua", Bytes("return require('lib.value') + 1")},
        {"lib/value.lua", Bytes("return 41")},
        {"empty.lua", {}},
        {"large.lua", Bytes(std::string(5000, ' '))},
        {"binary.lua", Bytes(std::string("\x1bLua\0", 5))}
    };
    Check(War3Map::Mpq::WriteMpqArchive(map, members).Succeeded(), "write original MPQ fixture");
    const auto sha = Hash(map);
    Check(!LuaMapArchive::Open(map, std::string(64, '0')).archive, "changed expected identity rejected");
    Check(!LuaMapArchive::Open(map, "not-a-hash").archive, "malformed identity rejected");
    {
        auto opened = LuaMapArchive::Open(map, sha);
        if (!opened.archive) throw std::runtime_error("open: " + opened.error);
        auto& archive = *opened.archive;
        Check(archive.Read("MAIN.LUA", 4096).found, "MPQ lookup uses actual case-insensitive member hash");
        Check(archive.Read("lib\\value.lua", 4096).source == "return 41", "MPQ slash canonicalization");
        const auto empty = archive.Read("empty.lua", 4096);
        Check(empty.found && empty.error.empty() && empty.source.empty(), "empty module remains distinct from missing member");
        const auto missing = archive.Read("missing.lua", 4096);
        Check(!missing.found && missing.error.empty(), "missing module has no fabricated content");
        const auto large = archive.Read("large.lua", 4096);
        Check(large.found && !large.error.empty() && large.source.empty(), "oversize module rejected before returning partial source");
        for (const auto path : {"../outside.lua", "lib/../main.lua", "/main.lua", "C:main.lua", "lib//value.lua", "lib/./value.lua", "main.lua:stream"})
            Check(!archive.Read(path, 4096).error.empty(), "absolute/traversal/alternate stream path refused");
        Check(!archive.Read("main.lua", 0).error.empty() && !archive.Read("main.lua", 4 * 1024 * 1024 + 1).error.empty(),
            "module budget bounded");
        std::ofstream nearby(temp.root / "missing.lua"); nearby << "return 99"; nearby.close();
        LuaHost host([keep = opened.archive](std::string_view member, std::size_t budget) { return keep->Read(member, budget); });
        Check(host.Initialize().Succeeded(), "host initialize");
        const auto loaded = host.Execute("assert(require('main') == 42 and require('empty') == true)", "@entry.lua");
        if (!loaded.Succeeded()) throw std::runtime_error(loaded.message);
        Check(host.Execute("require('missing')", "@missing.lua").code == LuaReloadResult::RuntimeError,
            "require never searches sibling filesystem script");
        Check(host.Execute("require('binary')", "@binary-test.lua").code == LuaReloadResult::RuntimeError,
            "archive source still passes text-only host gate");
        const auto write = CreateFileW(map.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        const auto writeError = GetLastError();
        if (write != INVALID_HANDLE_VALUE) CloseHandle(write);
        Check(write == INVALID_HANDLE_VALUE && writeError == ERROR_SHARING_VIOLATION, "frozen read ownership excludes source writes");
        Check(!MoveFileW(map.c_str(), (temp.root / "replaced.w3x").c_str()), "frozen file cannot be swapped during module reads");
    }
    Check(Hash(map) == sha, "all module reads preserve original MPQ");
    const auto released = CreateFileW(map.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(released != INVALID_HANDLE_VALUE, "all archive and host leases close after destruction"); CloseHandle(released);
    const auto corrupt = temp.root / "corrupt.w3x";
    std::ofstream invalid(corrupt, std::ios::binary); invalid << "not MPQ"; invalid.close();
    const auto refused = LuaMapArchive::Open(corrupt, Hash(corrupt));
    Check(!refused.archive && !refused.error.empty(), "matching SHA does not make corrupt archive valid");
}
}

int main() {
    try {
        ArchiveIdentityAndHost();
        std::cout << "Owned Lua frozen MPQ reader passed; original archive fixture only; game_evidence=0\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
