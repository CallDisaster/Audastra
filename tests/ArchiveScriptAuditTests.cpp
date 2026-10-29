#include "War3MapCore/MapArchiveSession.h"
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
        {"war3map.w3h", Bytes("original buff")}, {"war3map.w3q", Bytes("original upgrade")}};
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
    const auto after = Sha256File(source); Require(after.Succeeded() && *after.value == *before.value, "source unchanged");
    std::cout << "Script audit selective snapshot passed\n"; return 0;
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; } }
