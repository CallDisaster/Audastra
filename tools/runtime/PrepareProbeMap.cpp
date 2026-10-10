#include "War3MapCore/Services/MapCreateService.h"
#include "War3MapCore/Services/CodeCheck.h"
#include "War3MapCore/Jass/JassDeclarationIndex.h"
#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/MapData/MapSettings.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/VerifiedFile.h"

#include <windows.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>

namespace {
using namespace War3Map;
using Json = nlohmann::ordered_json;
template<class T> T Take(Result<T> value, const char* operation) {
    if (value.Succeeded() && value.value) return std::move(*value.value);
    for (const auto& diagnostic : value.diagnostics) std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    throw std::runtime_error(operation);
}
std::string Utf8(const std::filesystem::path& path) {
    const auto text = path.generic_u8string(); return {reinterpret_cast<const char*>(text.data()), text.size()};
}
std::string Narrow(std::wstring_view value) {
    if (std::any_of(value.begin(), value.end(), [](wchar_t c) { return c < 32 || c > 126; })) throw std::runtime_error("Expected ASCII SHA/nonce");
    std::string result; result.reserve(value.size());
    for (const auto character : value) result.push_back(static_cast<char>(character));
    return result;
}
std::string ReadBound(const std::filesystem::path& path, const std::string& sha, std::size_t limit = 8 * 1024 * 1024) {
    const auto bytes = Take(ReadVerifiedFile(path, sha, limit), "Verified readonly source capture failed");
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}
std::vector<std::byte> Bytes(std::string_view value) {
    const auto* begin = reinterpret_cast<const std::byte*>(value.data()); return {begin, begin + value.size()};
}
void WriteNew(const std::filesystem::path& path, std::string_view text) {
    const auto file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("Exclusive artifact create failed");
    DWORD written = 0; const auto ok = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) && written == text.size() && FlushFileBuffers(file);
    CloseHandle(file); if (!ok) throw std::runtime_error("Artifact write failed; evidence retained");
}
void ReplaceOnce(std::string& text, std::string_view marker, const std::string& value) {
    const auto at = text.find(marker);
    if (at == std::string::npos || text.find(marker, at + marker.size()) != std::string::npos) throw std::runtime_error("Probe template marker is not unique");
    text.replace(at, marker.size(), value);
}
std::string LuaArray(const std::vector<std::string>& names) {
    std::string text = "{";
    for (const auto& name : names) { if (text.size() > 1) text += ','; text += Json(name).dump(); }
    return text + '}';
}
bool ModuleName(std::string_view text) {
    return text.starts_with("jass.") && text.size() > 5 && text.size() <= 128
        && std::all_of(text.begin() + 5, text.end(), [](unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.'; });
}
int Run(int argc, wchar_t** argv) {
    const std::set<std::wstring> allowed{L"--tool-root", L"--game-root", L"--output-root", L"--nonce",
        L"--common", L"--common-sha256", L"--blizzard", L"--blizzard-sha256", L"--lua-template",
        L"--lua-template-sha256", L"--runtime-engine", L"--runtime-engine-sha256"};
    if (argc != 25) throw std::runtime_error("Expected twelve explicit parameter/value pairs; no local path defaults");
    std::map<std::wstring, std::wstring> args;
    for (int at = 1; at < argc; at += 2) {
        const std::wstring key(argv[at]);
        if (!allowed.contains(key) || !args.emplace(key, argv[at + 1]).second) throw std::runtime_error("Unknown/duplicate argument");
    }
    for (const auto* key : {L"--tool-root", L"--game-root", L"--output-root", L"--common", L"--blizzard", L"--lua-template", L"--runtime-engine"})
        if (!std::filesystem::path(args.at(key)).is_absolute()) throw std::runtime_error("Every local source/output root must be explicitly absolute");
    const auto nonce = Narrow(args.at(L"--nonce"));
    if (nonce.size() != 32 || !std::all_of(nonce.begin(), nonce.end(), [](unsigned char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); })) throw std::runtime_error("Expected 32 lowercase hex nonce");
    const std::filesystem::path output(args.at(L"--output-root"));
    if (!output.is_absolute() || std::filesystem::exists(output)) throw std::runtime_error("Output root must be absolute and absent");
    auto common = ReadBound(args.at(L"--common"), Narrow(args.at(L"--common-sha256")));
    auto blizzard = ReadBound(args.at(L"--blizzard"), Narrow(args.at(L"--blizzard-sha256")));
    auto lua = ReadBound(args.at(L"--lua-template"), Narrow(args.at(L"--lua-template-sha256")), 64 * 1024);
    const auto engine = ReadBound(args.at(L"--runtime-engine"), Narrow(args.at(L"--runtime-engine-sha256")), 16 * 1024 * 1024);
    const std::vector<Jass::JassSource> declarations{{"frozen/common.j", common}};
    const auto index = Jass::IndexJassDeclarations(declarations);
    if (!index.completed) throw std::runtime_error("Actual common.j declaration index unavailable");
    Json signatures = Json::array(); std::vector<std::string> candidates;
    for (const auto& symbol : index.symbols) if (symbol.kind == Jass::DeclarationKind::Native || symbol.kind == Jass::DeclarationKind::Function) {
        candidates.push_back(symbol.name);
        Json parameters = Json::array(); for (const auto& parameter : symbol.parameters) parameters.push_back(parameter.type);
        signatures.push_back({{"name", symbol.name}, {"parameterCount", symbol.parameters.size()}, {"parameters", parameters}, {"returns", symbol.type}});
    }
    if (candidates.empty() || candidates.size() > 6000) throw std::runtime_error("Actual common.j function candidate budget");
    std::set<std::string> discovered;
    for (std::size_t at = 0; at < engine.size();) {
        if (static_cast<unsigned char>(engine[at]) < 32 || static_cast<unsigned char>(engine[at]) > 126) { ++at; continue; }
        const auto begin = at;
        while (at < engine.size() && static_cast<unsigned char>(engine[at]) >= 32 && static_cast<unsigned char>(engine[at]) <= 126) ++at;
        if (at < engine.size() && engine[at] == '\0' && ModuleName(std::string_view(engine).substr(begin, at - begin))) discovered.emplace(engine.substr(begin, at - begin));
    }
    if (discovered.empty() || discovered.size() > 64) throw std::runtime_error("No bounded module candidates from actual captured runtime binary");
    ReplaceOnce(lua, "__AUDASTRA_PROBE_NONCE__", nonce);
    ReplaceOnce(lua, "__AUDASTRA_COMMON_CANDIDATES__", LuaArray(candidates));
    const std::vector<std::string> moduleNames(discovered.begin(), discovered.end());
    ReplaceOnce(lua, "__AUDASTRA_MODULE_CANDIDATES__", LuaArray(moduleNames));
    if (lua.size() >= 64 * 1024) throw std::runtime_error("Owned probe source exceeds 64 KiB IPC budget");
    const auto syntax = Lua::CheckSyntax(lua, "AudastraRuntimeProbe.lua");
    if (!syntax.Succeeded()) throw std::runtime_error("Owned metadata probe syntax failed; no chunk executed");
    if (!std::filesystem::create_directory(output)) throw std::runtime_error("Exclusive output directory create failed");
    Services::MapCreateRequest request;
    request.options.name = "Audastra Lua Runtime Probe"; request.options.author = "Audastra owned probe";
    request.target = output / L"original-base.w3x"; request.workspace = output / L"seed-workspace";
    request.toolRoot = args.at(L"--tool-root"); request.gameRoot = args.at(L"--game-root");
    const auto created = Take(Services::CreateMapCandidate(request), "Production original map creation failed");
    auto base = Take(MapArchiveSession::Open(created.path), "Original map open failed");
    auto settings = Take(MapData::MapSettings::Parse(Take(base.ReadMember("war3map.w3i"), "Original settings missing")), "Original settings parse failed");
    if (!settings.HasClassicLayout() || settings.scriptLanguage != 0 || settings.players.size() != 1) throw std::runtime_error("Unexpected original classic-map profile");
    const std::string marker = "AUDASTRA_LUA_API_BEGIN_V1:" + nonce;
    const std::string script = "native EXExecuteScript takes string script returns string\n"
        "function AudastraCaptureLuaApi takes nothing returns nothing\n"
        // 本机运行库将参数包成 return(expression)，这里必须只给字串表达式。
        "call EXExecuteScript(\"\\\"" + marker + "\\\"\")\nendfunction\n"
        "function config takes nothing returns nothing\n"
        "call SetMapName(\"Audastra Lua Runtime Probe\")\ncall SetMapDescription(\"Owned metadata probe; no author script\")\n"
        "call SetPlayers(1)\ncall SetTeams(1)\ncall SetGamePlacement(MAP_PLACEMENT_USE_MAP_SETTINGS)\n"
        "call DefineStartLocation(0, 0.0, 0.0)\ncall SetPlayerStartLocation(Player(0), 0)\n"
        "call SetPlayerController(Player(0), MAP_CONTROL_USER)\ncall SetPlayerSlotAvailable(Player(0), MAP_CONTROL_USER)\n"
        "call SetPlayerRacePreference(Player(0), RACE_PREF_HUMAN)\ncall SetPlayerRaceSelectable(Player(0), true)\nendfunction\n"
        "function main takes nothing returns nothing\n"
        "call SetCameraBounds(-2816.0, -3328.0, 2816.0, 2816.0, -2816.0, 2816.0, 2816.0, -3328.0)\n"
        "call SetDayNightModels(\"Environment\\\\DNC\\\\DNCLordaeron\\\\DNCLordaeronTerrain\\\\DNCLordaeronTerrain.mdl\","
        "\"Environment\\\\DNC\\\\DNCLordaeron\\\\DNCLordaeronUnit\\\\DNCLordaeronUnit.mdl\")\n"
        "call InitBlizzard()\ncall TimerStart(CreateTimer(), 0.2, false, function AudastraCaptureLuaApi)\nendfunction\n";
    Services::CodeCheckRequest check;
    check.source = {"owned-probe/war3map.j", script}; check.context.complete = true;
    check.context.binding = "owned-probe-" + nonce;
    check.context.declarations = {{"frozen/common.j", common}, {"frozen/blizzard.j", blizzard}};
    const auto checked = Services::CheckCode(check);
    if (!checked.Succeeded() || !checked.semanticChecked || !checked.contextComplete) {
        for (const auto& diagnostic : checked.diagnostics) std::cerr << diagnostic.code << ':' << diagnostic.line << ": " << diagnostic.message << '\n';
        throw std::runtime_error("Owned classic probe failed the shared production JASS checker");
    }
    std::vector<MapArchiveMember> members;
    for (const auto& member : base.Members()) {
        if (member.name == "war3map.j" || member.name == "war3map.wtg" || member.name == "war3map.wct") continue;
        members.push_back(member);
    }
    members.push_back({"war3map.j", Bytes(script)});
    const auto map = Take(WriteVerifiedArchive(output / L"runtime-probe.w3x", members, base.Prefix()), "Probe archive verified write failed");
    WriteNew(output / L"runtime-probe.lua", lua); WriteNew(output / L"runtime-probe.j", script);
    Json receipt{{"schema", "audastra-lua-api-probe-preparation-v1"}, {"nonce", nonce}, {"mapPath", Utf8(map.path)}, {"mapSha256", map.sha256},
        {"mapBytes", map.fileSize}, {"luaPath", Utf8(output / L"runtime-probe.lua")}, {"luaSha256", Take(Sha256Text(lua), "Lua hash")},
        {"jassSha256", Take(Sha256Text(script), "JASS hash")}, {"generatedOriginalMapProfile", created.profile},
        {"originalBaseMapSha256", created.sha256}, {"commonSha256", Narrow(args.at(L"--common-sha256"))},
        {"blizzardSha256", Narrow(args.at(L"--blizzard-sha256"))}, {"runtimeEngineSha256", Narrow(args.at(L"--runtime-engine-sha256"))},
        {"commonSignatures", signatures}, {"moduleCandidates", discovered},
        {"moduleCandidatesAreExports", false}, {"authorScriptsIncluded", false}, {"syntaxCheckedOnly", true}, {"runtimeVerified", false}};
    WriteNew(output / L"preparation.json", receipt.dump(2) + '\n');
    std::cout << receipt.dump(2) << '\n'; return 0;
}
} // namespace
int wmain(int argc, wchar_t** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    try { return Run(argc, argv); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
