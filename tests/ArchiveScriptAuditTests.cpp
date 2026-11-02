#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/MapData/NativeEditorProfileInputs.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/Script/SlkMapMemberProtocol.h"
#include <Windows.h>
#include <array>
#include <filesystem>
#include <iostream>
#include <stdexcept>
using namespace War3Map;
namespace {
void Require(bool pass, const char* message) { if (!pass) throw std::runtime_error(message); }
std::vector<std::byte> Bytes(std::string_view text) {
    const auto* p = reinterpret_cast<const std::byte*>(text.data()); return {p, p + text.size()};
}
struct Fixture {
    std::filesystem::path root = std::filesystem::temp_directory_path() / ("Audastra-ScriptAudit-" + std::to_string(GetCurrentProcessId()) + '-' + std::to_string(GetTickCount64()));
    Fixture() { Require(std::filesystem::create_directory(root), "fresh original fixture"); }
    ~Fixture() { std::error_code error; std::filesystem::remove_all(root, error); }
};
}
int main() { try {
    Fixture fixture; const auto source = fixture.root / "source.w3x", target = fixture.root / "not-written.w3x";
    const std::vector<MapArchiveMember> members{{"war3map.wtg", Bytes("original-wtg")}, {"war3map.wct", Bytes("original-wct")},
        {"unknown/opaque.bin", Bytes("original opaque fixture")}, {"war3map.j", Bytes("generated")},
        {"war3map.w3b", Bytes("original destructable")}, {"war3map.w3d", Bytes("original doodad")},
        {"war3map.w3h", Bytes("original buff")}, {"war3map.w3q", Bytes("original upgrade")},
        {"war3map.w3e", Bytes("original terrain")}, {"Fixture/Ground.blp", Bytes("atlas bytes")},
        {"UI/WorldEditData.txt", Bytes("original settings")}, {"Units/MiscData.txt", Bytes("original unit parameters")},
        {"Units/MiscGame.txt", Bytes("original unit game parameters")},
        {"forbidden.AI", Bytes("authored opaque fixture, not executable input")}, {"runtime.dll", Bytes("opaque binary")}};
    const auto written = WriteVerifiedArchive(source, members); Require(written.Succeeded(), "original fixture archive");
    const auto before = Sha256File(source); Require(before.Succeeded(), "source fingerprint");
    const auto audit = MapArchiveSession::OpenScriptAudit(source); Require(audit.Succeeded(), "selective audit opens");
    Require(audit.value->HasMember("war3map.wtg") && audit.value->HasMember("war3map.wct"), "paired sources loaded");
    Require(!audit.value->HasMember("unknown/opaque.bin") && !audit.value->HasMember("war3map.j"), "opaque/generated members absent");
    const std::array<std::string, 1> forbidden{"unknown/opaque.bin"}, pair{"war3map.wtg"};
    Require(!audit.value->CaptureExactMembers(forbidden).Succeeded() && !audit.value->ReadMember(forbidden.front()).Succeeded(), "cannot widen query reads");
    Require(audit.value->CaptureExactMembers(pair).Succeeded(), "exact paired read remains available");
    const auto slk = audit.value->CaptureExactMembers(Script::SlkMapMemberProtocol::FixedNames());
    Require(slk.Succeeded() && slk.value->size() == Script::SlkMapMemberProtocol::FixedNames().size(), "all fixed slk present/missing slots may be queried");
    for (const auto* member : {"war3map.w3b", "war3map.w3d", "war3map.w3h", "war3map.w3q"})
        Require(audit.value->HasMember(member) && audit.value->ReadMember(member).Succeeded(), "remaining native object sources captured");
    const std::array<std::string, 1> unknownTable{"units/unknown-opaque.slk"};
    Require(!audit.value->CaptureExactMembers(unknownTable).Succeeded(), "slk protocol does not authorize arbitrary member names");
    Require(!audit.value->SaveCandidate(target, {}).Succeeded() && !std::filesystem::exists(target), "cannot save partial archive");
    Require(!audit.value->SaveInPlace({}).Succeeded(), "cannot overwrite source");
    Require(!audit.value->PreflightSave({}).Succeeded(), "cannot grant save preflight");
    const auto minimap = MapArchiveSession::OpenMinimapAudit(source);
    Require(minimap.Succeeded() && minimap.value->HasMember("war3map.w3e"), "minimap snapshot loads fixed terrain");
    Require(!minimap.value->HasMember("war3map.wct") && !minimap.value->HasMember("Fixture/Ground.blp"),
        "minimap omits scripts and does not eagerly read textures");
    const std::array<std::string, 5> resources{"Fixture/Ground.blp", "UI/WorldEditData.txt", "units/miscdata.TXT", "Units/MiscGame.txt", "missing/model.mdx"};
    const auto captured = minimap.value->CaptureExactMembers(resources);
    Require(captured.Succeeded() && captured.value->at(0).present && captured.value->at(1).present
        && captured.value->at(2).present && captured.value->at(2).bytes == Bytes("original unit parameters")
        && captured.value->at(3).present && captured.value->at(3).bytes == Bytes("original unit game parameters")
        && !captured.value->at(4).present, "minimap exact resources preserve present/missing distinction and fixed unit profiles");
    std::vector<std::string> profileNames;
    for (const auto& profile : War3Map::MapData::kClassicProfiles) profileNames.emplace_back(profile.path);
    Require(minimap.value->CaptureExactMembers(profileNames).Succeeded(), "complete fixed classic profile chain is queryable");
    const std::array<std::string, 5> planInputs{"war3map.wpm", "war3map.shd", "war3map.mmp", "war3map.w3r", "war3map.w3c"};
    Require(minimap.value->CaptureExactMembers(planInputs).Succeeded(), "shared terrain plan baseline members are queryable without eager reads");
    for (const auto* name : {"war3map.j", "war3map.wct", "runtime.dll", "unknown/opaque.bin", "forbidden.AI", "Units/Unknown.txt"}) {
        const std::array<std::string, 1> names{name};
        Require(!minimap.value->CaptureExactMembers(names).Succeeded()
            && !minimap.value->ReadMember(name).Succeeded(), "minimap cannot read outside resource policy");
        const auto rejected = minimap.value->ReadMember(name);
        Require(!rejected.diagnostics.empty() && rejected.diagnostics.front().message.find(name) != std::string::npos,
            "resource policy diagnostic identifies the rejected member without reading it");
    }
    Require(!minimap.value->SaveCandidate(target, {}).Succeeded() && !std::filesystem::exists(target), "minimap cannot save partial snapshot");
    Require(!minimap.value->SaveInPlace({}).Succeeded() && !minimap.value->PreflightSave({}).Succeeded(), "minimap cannot grant writes");
    const auto after = Sha256File(source); Require(after.Succeeded() && *after.value == *before.value, "source unchanged");
    std::cout << "Script audit selective snapshot passed\n"; return 0;
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; } }
