#include "../RuntimeFiles.h"
#include "War3MapCore/Mpq/MpqArchive.h"
#include "War3MapCore/Runtime/LuaTestMailboxValidation.h"
#include <fstream>
#include <cstring>
#include <iostream>
#include <set>
#include <stdexcept>

using namespace Audastra::Runtime32;
namespace {
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
std::vector<std::byte> Bytes(std::string_view text) {
    return {reinterpret_cast<const std::byte*>(text.data()), reinterpret_cast<const std::byte*>(text.data() + text.size())};
}
}

// 用 map-create 生成的原创平图作种子，追加本仓库原创 Lua；输出必须是新文件。
// 不执行游戏、不改源地图、不提供普遍的经典 Lua 保存资格。
int wmain(int argc, wchar_t** argv) {
    try {
        const bool jassProbe=argc==6 && std::wstring_view(argv[5])==L"--jass-probe";
        Check(argc == 5 || jassProbe, "usage: own-lua-game-map <generated-seed.w3x> <seed-sha256> <fixture.lua> <new-map.w3x> [--jass-probe]");
        const std::filesystem::path seed(argv[1]), source(argv[3]), output(argv[4]);
        const std::wstring expectedWide(argv[2]);
        std::string expected;
        for (const auto character : expectedWide) {
            Check((character >= L'0' && character <= L'9') || (character >= L'a' && character <= L'f'), "ASCII seed SHA required");
            expected.push_back(static_cast<char>(character));
        }
        Check(LuaMailboxHex(expected, 64) && PlainPath(seed) && PlainPath(source) && PlainPath(output)
            && !std::filesystem::exists(output), "explicit seed identity and new output required");
        Check(source.extension()==(jassProbe?L".j":L".lua"),"fixture has explicit script role");
        std::ifstream input(source, std::ios::binary);
        const std::string fixture(std::istreambuf_iterator<char>(input), {});
        Check(input.is_open() && !fixture.empty() && fixture.size() <= 64 * 1024 && LuaMailboxUtf8(fixture),"bounded original fixture");
        Handle file(OpenLockedFile(seed));
        Check(file.value != INVALID_HANDLE_VALUE && Sha256(file.value, 4 * 1024 * 1024) == expected, "generated seed identity mismatch");
        auto opened = War3Map::Mpq::MpqArchive::OpenReadOnlyHandle(file.value, seed);
        Check(opened.Succeeded(), "read generated seed MPQ");
        const std::set<std::string> allowed{"war3map.w3i", "war3map.w3e", "war3map.wpm", "war3mapUnits.doo",
            "war3map.doo", "war3map.wtg", "war3map.wct", "war3map.w3r", "war3map.w3c", "war3map.wts", "war3map.shd", "war3map.j", "war3mapMap.blp"};
        auto names = opened.value->ListFilesStrict();
        Check(names.Succeeded() && names.value->size() <= allowed.size() + 1, "fixture seed member scope");
        std::vector<War3Map::Mpq::MpqBuildFile> members;
        for (const auto& entry : *names.value) {
            if (entry.name == "(listfile)") continue;
            Check(allowed.contains(entry.name), "seed has content outside original map-create fixture");
            auto contents = opened.value->ReadFileBounded(entry.name, 1024 * 1024);
            Check(contents.Succeeded(), "read bounded original member");
            if (entry.name == "war3map.w3i") {
                const std::string text(reinterpret_cast<const char*>(contents.value->data()), contents.value->size());
                Check(text.find("Audastra Lua Runtime Fixture") != std::string::npos, "seed must be explicitly named runtime fixture");
            }
            if (entry.name == "war3map.j") {
                std::string text(reinterpret_cast<const char*>(contents.value->data()), contents.value->size());
                const auto main = text.find("function main takes nothing returns nothing");
                const auto end = main == std::string::npos ? std::string::npos : text.find("endfunction", main);
                Check(end != std::string::npos && text.find("exec-lua:") == std::string::npos, "original generated main entry");
                if(jassProbe) {
                    constexpr std::string_view split="// AUD_NATIVE_PROBE_FUNCTIONS";
                    const auto marker=fixture.find(split);
                    const auto globalsEnd=text.find("endglobals");
                    Check(marker!=std::string::npos && globalsEnd<main && fixture.find("function AudastraNativeHandleProbe takes nothing returns nothing")!=std::string::npos,
                        "explicit original native probe sections");
                    text.insert(end,"    call TimerStart(CreateTimer(), 1.0, false, function AudastraNativeHandleProbe)\n");
                    text.insert(main,fixture.substr(marker+split.size())+"\n");
                    text.insert(globalsEnd,fixture.substr(0,marker)+"\n");
                } else text.insert(end, "    call Cheat(\"exec-lua: \\\"runtime_fixture\\\"\")\n");
                *contents.value = Bytes(text);
            }
            members.push_back({entry.name, std::move(*contents.value)});
        }
        if(!jassProbe)members.push_back({"runtime_fixture.lua", Bytes(fixture)});
        std::array<std::byte, 512> prefix{};
        LARGE_INTEGER zero{}; DWORD bytes = 0;
        Check(SetFilePointerEx(file.value, zero, nullptr, FILE_BEGIN)
            && ReadFile(file.value, prefix.data(), static_cast<DWORD>(prefix.size()), &bytes, nullptr)
            && bytes == prefix.size() && std::memcmp(prefix.data(), "HM3W", 4) == 0, "original generated map prefix");
        Check(War3Map::Mpq::WriteMpqArchive(output, members, prefix).Succeeded(), "write new synthetic runtime map");
        auto verify = War3Map::Mpq::MpqArchive::OpenReadOnly(output);
        Check(verify.Succeeded(), "read back synthetic map");
        for (const auto& member : members) {
            const auto actual = verify.value->ReadFile(member.name);
            Check(actual.Succeeded() && *actual.value == member.data, "exact synthetic map member roundtrip");
        }
        Handle built(OpenLockedFile(output));
        std::cout << "synthetic_map_sha256=" << Sha256(built.value) << "\nroundtrip=true\ngame_evidence=0\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
