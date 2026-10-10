#include "War3MapCore/Script/SlkMapSourceSnapshot.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/Mpq/MpqArchive.h"
#include "../src/Core/Mpq/MpqCrypt.h"

#include <Windows.h>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <utility>

using namespace War3Map;
using namespace War3Map::Script;
namespace {
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
bool HasCode(const auto& result, std::string_view code) {
    return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
        [code](const Diagnostic& diagnostic) { return diagnostic.code == code; });
}
struct Scratch {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("forge-slk-sources-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
    Scratch() {
        Require(std::filesystem::create_directory(path), "create private synthetic workspace");
        path = std::filesystem::canonical(path);
    }
    ~Scratch() {
        std::error_code error;
        const auto resolved = std::filesystem::weakly_canonical(path, error);
        if (!error && resolved == path && path.filename().wstring().starts_with(L"forge-slk-sources-"))
            std::filesystem::remove_all(path, error);
    }
};
std::vector<std::byte> Bytes(std::string_view value) {
    const auto* begin = reinterpret_cast<const std::byte*>(value.data());
    return {begin, begin + value.size()};
}
// 原创小归档：保留有效 listfile，但只列资源；其它固定来源依名字 hash 查证。
void WriteOpaqueSources(const std::filesystem::path& path, std::vector<MapArchiveMember> members, bool completeNames = false) {
    std::string listfile = "resource/unknown.bin\r\n";
    if (completeNames) {
        listfile.clear();
        for (const auto& member : members) listfile += member.name + "\r\n";
    }
    members.push_back({"(listfile)", Bytes(listfile)});
    constexpr std::uint32_t hashCount = 16;
    const auto blockCount = static_cast<std::uint32_t>(members.size());
    std::vector<std::byte> bytes(32);
    std::vector<std::uint32_t> positions;
    for (const auto& member : members) {
        positions.push_back(static_cast<std::uint32_t>(bytes.size()));
        bytes.insert(bytes.end(), member.data.begin(), member.data.end());
    }
    const auto hashPos = static_cast<std::uint32_t>(bytes.size()), blockPos = hashPos + hashCount * 16;
    bytes.resize(blockPos + blockCount * 16);
    const auto put = [&](std::size_t at, std::uint32_t value) { std::memcpy(bytes.data() + at, &value, 4); };
    put(0, 0x1A51504D); put(4, 32); put(8, static_cast<std::uint32_t>(bytes.size()));
    put(12, 0x00030000); put(16, hashPos); put(20, blockPos); put(24, hashCount); put(28, blockCount);
    std::fill(bytes.begin() + hashPos, bytes.begin() + blockPos, std::byte{0xFF});
    const auto& crypt = Mpq::MpqCrypt::Instance();
    for (std::uint32_t i = 0; i < blockCount; ++i) {
        const auto& member = members[i];
        auto bucket = crypt.HashString(member.name, Mpq::MpqCrypt::HashType::TableOffset) & (hashCount - 1);
        while (bytes[hashPos + bucket * 16 + 12] != std::byte{0xFF}) bucket = (bucket + 1) & (hashCount - 1);
        const auto hash = hashPos + bucket * 16, block = blockPos + i * 16;
        put(hash, crypt.HashString(member.name, Mpq::MpqCrypt::HashType::NameA));
        put(hash + 4, crypt.HashString(member.name, Mpq::MpqCrypt::HashType::NameB)); put(hash + 8, 0); put(hash + 12, i);
        put(block, positions[i]); put(block + 4, static_cast<std::uint32_t>(member.data.size()));
        put(block + 8, static_cast<std::uint32_t>(member.data.size())); put(block + 12, Mpq::kFileExists | Mpq::kFileSingleUnit);
    }
    crypt.EncryptBlock(bytes.data() + hashPos, hashCount * 16, crypt.HashString("(hash table)", Mpq::MpqCrypt::HashType::FileKey));
    crypt.EncryptBlock(bytes.data() + blockPos, blockCount * 16, crypt.HashString("(block table)", Mpq::MpqCrypt::HashType::FileKey));
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    Require(file.good(), "write opaque fixture");
}
void TestSnapshot() {
    Scratch scratch;
    const auto file = scratch.path / "source.mpq";
    const std::vector<MapArchiveMember> base{{"war3map.w3i", Bytes("info")},
        {"war3map.w3a", Bytes("old")}, {"war3map.wts", {}}, {"resource\\icon.blp", Bytes("icon")}};
    // single-unit storage=decoded，让13/14字节边界只测保留快照，而非扇区表读取预算。
    WriteOpaqueSources(file, base, true);
    auto opened = MapArchiveSession::Open(file);
    Require(opened.Succeeded(), "open synthetic source");
    const auto& archive = *opened.value;
    const auto before = SlkMapSourceSnapshot::Capture(archive, {}, {}, 0);
    Require(before.Succeeded() && before.value->NativeOnlySources(), "capture native-only source");
    Require(before.value->Members().size() == 9 && before.value->Revision() == 0, "exact nine owned source slots");
    Require(before.value->Members()[0].identity.presence == SlkSessionSourcePresence::Missing &&
        before.value->Members()[0].identity.sha256.empty(), "missing remains explicit");
    Require(before.value->Members()[7].identity.presence == SlkSessionSourcePresence::Present &&
        before.value->Members()[7].identity.sha256 == *Sha256({}).value, "empty present differs from missing");
    std::vector<MemberReplacement> edits{{"WAR3MAP.W3A", Bytes("edited")}, {"resource/new.txt", Bytes("asset")}};
    const std::vector<std::string> removals{"RESOURCE/icon.blp"};
    const auto after = SlkMapSourceSnapshot::Capture(archive, edits, removals, 7);
    Require(after.Succeeded() && after.value->Members()[1].bytes == Bytes("edited") &&
        after.value->Revision() == 7, "edited native bytes and revision are captured");
    Require(after.value->OriginalMembers().size() == 9 && after.value->OriginalMembers()[1].bytes == Bytes("old") &&
        after.value->FinalMemberNamesComplete(), "original native ancestor is independent from final overlay");
    Require(after.value->FinalMemberNames().size() == 4 && after.value->ManifestSha256() != before.value->ManifestSha256(),
        "final inventory and digest include edits and removals");
    const auto originalManifest = after.value->ManifestSha256();
    std::reverse(edits.begin(), edits.end());
    edits.back().name = "war3map.w3a";
    Require(SlkMapSourceSnapshot::Capture(archive, edits, removals, 7).value->ManifestSha256() == originalManifest,
        "equivalent path spelling and unordered overlay collection have same manifest");
    edits.front().data = Bytes("asset2");
    Require(SlkMapSourceSnapshot::Capture(archive, edits, removals, 7).value->ManifestSha256() != originalManifest,
        "unrelated import edits invalidate the save snapshot identity too");
    Require(SlkMapSourceSnapshot::Capture(archive, {}, {}, 1).value->ManifestSha256() != before.value->ManifestSha256(),
        "revision-only changes invalidate identity");
    edits.back().data = Bytes("newer");
    Require(after.value->Members()[1].bytes == Bytes("edited"), "snapshot bytes do not alias mutable edits");
    Require(archive.ReadMember("war3map.w3a").value == Bytes("old"), "capture never changes archive snapshot");

    edits = {{"war3map.w3a", Bytes("a")}, {"WAR3MAP.W3A", Bytes("b")}};
    Require(HasCode(SlkMapSourceSnapshot::Capture(archive, edits, {}, 0), "SLK_SOURCES_CONFLICT"), "duplicate edit alias rejected");
    edits.pop_back();
    const std::vector<std::string> sameRemoval{"war3map.w3a"};
    Require(HasCode(SlkMapSourceSnapshot::Capture(archive, edits, sameRemoval, 0), "SLK_SOURCES_CONFLICT"), "edit-remove conflict rejected");
    const std::vector<std::string> missingRemoval{"missing"};
    Require(HasCode(SlkMapSourceSnapshot::Capture(archive, {}, missingRemoval, 0), "SLK_SOURCES_REMOVAL_MISSING"), "missing deletion is not absence proof");
    const std::vector<std::string> twiceRemoval{"war3map.w3a", "WAR3MAP.W3A"};
    Require(HasCode(SlkMapSourceSnapshot::Capture(archive, {}, twiceRemoval, 0), "SLK_SOURCES_CONFLICT"), "duplicate removal rejected");
    edits = {{"../war3map.w3a", {}}};
    Require(!SlkMapSourceSnapshot::Capture(archive, edits, {}, 0).Succeeded(), "unsafe paths rejected");
    edits = {{"(listfile)", {}}};
    Require(HasCode(SlkMapSourceSnapshot::Capture(archive, edits, {}, 0), "SLK_SOURCES_RESERVED"), "archive metadata cannot be replaced");

    for (const std::string name : {"Units/AbilityData.slk", "Doodads/doodads.slk", "Units/CommonAbilityFunc.txt",
        "UI/MiscData.txt", "war3mapMisc.txt", "table/ability.ini", "war3map.w3i.ini", "war3map.txt.ini",
        "w3x2lni/locale/w3i.lng", "map/war3map.w3a", "war3mapSkin.txt"}) {
        edits = {{name, {}}};
        const auto unsupported = SlkMapSourceSnapshot::Capture(archive, edits, {}, 0);
        Require(unsupported.Succeeded() && !unsupported.value->NativeOnlySources() &&
            unsupported.value->UnsupportedSources().size() == 1 && HasCode(unsupported, "SLK_SOURCES_OVERLAY_UNSUPPORTED"),
            "empty and aliased frontend overlays remain explicitly unsupported");
    }
    SlkMapSourceLimits limits;
    limits.retainedBytes = 13; // 原始九槽位 + 最终 frontend 各 4 info + 3 old；空 WTS 不占字节
    Require(HasCode(SlkMapSourceSnapshot::Capture(archive, {}, {}, 0, limits), "SLK_SOURCES_LIMIT"), "retained byte budget checked before copy");
    limits.retainedBytes = 14;
    Require(SlkMapSourceSnapshot::Capture(archive, {}, {}, 0, limits).Succeeded(), "exact retained budget accepted");
    limits.memberCount = 3;
    Require(!SlkMapSourceSnapshot::Capture(archive, {}, {}, 0, limits).Succeeded(), "inventory budget enforced");
    limits = {}; limits.replacementBytes = 0; edits = {{"x", Bytes("a")}};
    Require(!SlkMapSourceSnapshot::Capture(archive, edits, {}, 0, limits).Succeeded(), "zero replacement budget enforced");
    limits = {}; ++limits.memberCount;
    Require(!SlkMapSourceSnapshot::Capture(archive, {}, {}, 0, limits).Succeeded(), "hard budgets cannot be enlarged");
    Require(archive.VerifySourceUnchanged().Succeeded(), "read-only checks leave source unchanged");
    { std::ofstream append(file, std::ios::binary | std::ios::app); append << 'x'; }
    Require(!SlkMapSourceSnapshot::Capture(archive, {}, {}, 0).Succeeded(), "external source changes reject stale snapshot");
}

void TestFrontendSources() {
    Scratch scratch;
    const auto file = scratch.path / "frontend.mpq";
    const std::vector<MapArchiveMember> base{
        {"war3map.w3a", Bytes("native")}, {"war3map.wts", Bytes("root-wts")},
        {"war3mapMisc.txt", {}}, {"Units/HumanUnitStrings.txt", Bytes("old txt")},
        {"Units/AbilityData.slk", Bytes("raw slk")}, {"units/miscgame.txt", Bytes("misc game")},
        {"table/ability.ini", {}}, {"war3map.w3a.ini", Bytes("legacy table")},
        {"war3map.txt.ini", Bytes("txt table")}, {"table/w3i.ini", Bytes("table info")},
        {"war3map.w3i", Bytes("native info")}, {"map/war3map.w3a", Bytes("lni native")},
        {"w3x2lni/locale/w3i.lng", Bytes("old locale")}, {"w3x2lni/locale/lml.lng", {}},
        {"resource/unknown.txt", Bytes("not a frontend source")}};
    Require(WriteVerifiedArchive(file, base).Succeeded(), "write original frontend fixture");
    const auto archive = MapArchiveSession::Open(file);
    Require(archive.Succeeded(), "open original frontend fixture");
    const auto captured = SlkMapSourceSnapshot::Capture(*archive.value, {}, {}, 11);
    Require(captured.Succeeded() && !captured.value->NativeOnlySources(), "capture does not widen native-only semantics");
    const auto& snapshot = *captured.value;
    Require(snapshot.Members().size() == 9 && snapshot.FrontendMembers().size() > 9,
        "legacy members and extended frontend inventory coexist");
    const auto native = snapshot.LookupMember("WAR3MAP.W3A");
    Require(native.Succeeded() && native.value->member == &snapshot.Members()[1] &&
        native.value->member == &snapshot.FrontendMembers()[1], "native source has one owned byte allocation");
    const auto empty = snapshot.LoadFrontendMember("map", "WAR3MAPMISC.TXT");
    Require(empty.Succeeded() && empty.value->state == SlkMapSourceLookupState::Captured &&
        empty.value->member->bytes.empty() && empty.value->member->identity.sha256 == *Sha256({}).value,
        "empty misc remains present and can trigger the actual frontend branch");
    const auto first = snapshot.LoadFrontendMember("table", "ability");
    Require(first.Succeeded() && first.value->state == SlkMapSourceLookupState::Captured &&
        first.value->memberName == "TABLE\\ABILITY.INI" && first.value->member->bytes.empty(),
        "empty first LNI alias shadows nonempty legacy alias");
    const auto info = snapshot.LoadFrontendMember("table", "w3i");
    Require(info.Succeeded() && info.value->member->bytes == Bytes("table info"), "table W3I remains distinct from native W3I");
    const auto txt = snapshot.LoadFrontendMember("table", "txt");
    Require(txt.Succeeded() && txt.value->memberName == "WAR3MAP.TXT.INI" &&
        txt.value->member->bytes == Bytes("txt table"), "known missing first alias permits second alias");
    const auto slk = snapshot.LoadFrontendMember("map", "Units/AbilityData.slk");
    Require(slk.Succeeded() && slk.value->member->bytes == Bytes("raw slk"), "SLK bytes are captured without interpreting them");
    const auto misc = snapshot.LoadFrontendMember("map", "units/miscgame.txt");
    Require(misc.Succeeded() && misc.value->member->bytes == Bytes("misc game"), "buildslk misc companions are captured");
    const std::vector<MemberReplacement> emptyTxt{{"units/humanunitstrings.txt", {}}};
    const auto emptyTxtSnapshot = SlkMapSourceSnapshot::Capture(*archive.value, emptyTxt, {}, 11);
    Require(emptyTxtSnapshot.Succeeded() && !emptyTxtSnapshot.value->NativeOnlySources(), "empty TXT remains an audited overlay");
    const auto emptyTxtQuery = emptyTxtSnapshot.value->LoadFrontendMember("map", "units/humanunitstrings.txt");
    Require(emptyTxtQuery.Succeeded() && emptyTxtQuery.value->state == SlkMapSourceLookupState::Captured &&
        emptyTxtQuery.value->member->bytes.empty(), "empty replacement TXT cannot fall back to original or installed bytes");
    const auto lniNative = snapshot.LoadFrontendMember("map", "war3map.w3a", SlkFrontendInputMode::Lni);
    Require(lniNative.Succeeded() && lniNative.value->member->bytes == Bytes("lni native") &&
        snapshot.LoadFrontendMember("map", "war3map.w3a").value->member->bytes == Bytes("native"),
        "explicit input mode selects one physical root without mixing");
    Require(snapshot.LoadFrontendMember("map", "war3map.wts", SlkFrontendInputMode::Lni).value->state ==
        SlkMapSourceLookupState::Missing, "missing map-prefixed member does not fall back to root bytes");

    const auto uncaptured = snapshot.LookupMember("RESOURCE/UNKNOWN.TXT");
    Require(uncaptured.Succeeded() && uncaptured.value->state == SlkMapSourceLookupState::PresentUncaptured &&
        uncaptured.value->member == nullptr, "existing uncaptured member is not absence evidence");
    Require(snapshot.LookupMember("resource/missing.txt").value->state == SlkMapSourceLookupState::Missing,
        "full final inventory proves an actual missing member");
    for (const auto& request : std::vector<std::pair<std::string, std::string>>{
        {"map", "resource/unknown.txt"}, {"map", "map/war3map.w3a"}, {"table", "../ability"},
        {"table", "ABILITY"}, {"table", "ability.ini"}, {"w3x2lni", "locale/unknown.lng"}, {"unknown", "war3map.w3a"}}) {
        const auto result = snapshot.LoadFrontendMember(request.first, request.second);
        Require(!result.Succeeded() && !result.value && HasCode(result, "SLK_SOURCES_QUERY_INVALID"),
            "unknown protocol requests cannot be converted to Missing and fallback");
    }
    Require(!snapshot.LoadFrontendMember("map", "war3map.w3a", static_cast<SlkFrontendInputMode>(9)).Succeeded(),
        "unknown input mode rejected");
    Require(!snapshot.LookupMember("../war3map.w3a").Succeeded() && !snapshot.LookupMember("(listfile)").Succeeded(),
        "invalid or reserved lookup cannot claim missing");

    std::vector<MemberReplacement> edits{{"units/HUMANUNITSTRINGS.TXT", Bytes("new whole txt")},
        {"W3X2LNI/LOCALE/W3I.LNG", Bytes("new locale")}};
    const std::vector<std::string> removals{"TABLE/ABILITY.INI", "WAR3MAPMISC.TXT"};
    const auto after = SlkMapSourceSnapshot::Capture(*archive.value, edits, removals, 12);
    Require(after.Succeeded() && after.value->ManifestSha256() != snapshot.ManifestSha256() &&
        after.value->SourceArchiveSha256() == snapshot.SourceArchiveSha256() && after.value->Revision() == 12,
        "frontend overlay binds the same source and changed revision manifest");
    Require(after.value->LoadFrontendMember("map", "units/humanunitstrings.txt").value->member->bytes == Bytes("new whole txt"),
        "TXT overlay replaces complete source bytes");
    const auto locale = after.value->LoadFrontendMember("w3x2lni", "locale/w3i.lng");
    Require(locale.Succeeded() && locale.value->member->bytes == Bytes("new locale") &&
        locale.value->member->identity.sha256 == *Sha256(Bytes("new locale")).value,
        "locale replacement carries exact source identity");
    Require(after.value->LoadFrontendMember("w3x2lni", "locale/lml.lng").value->state == SlkMapSourceLookupState::Captured,
        "empty locale remains captured");
    Require(after.value->LoadFrontendMember("map", "war3mapmisc.txt").value->state == SlkMapSourceLookupState::Missing &&
        after.value->LoadFrontendMember("table", "ability").value->member->bytes == Bytes("legacy table"),
        "valid removals affect presence and ordered alias selection");
    edits[1].data = Bytes("mutated later");
    Require(locale.value->member->bytes == Bytes("new locale"), "captured query view does not alias edits");
    edits.push_back({"w3x2lni\\locale\\w3i.lng", {}});
    const auto conflict = SlkMapSourceSnapshot::Capture(*archive.value, edits, {}, 12);
    Require(!conflict.value && HasCode(conflict, "SLK_SOURCES_CONFLICT"), "frontend case aliases fail without partial snapshot");

    SlkMapSourceLimits limits;
    limits.retainedBytes = 0;
    for (const auto& member : base) if (!member.name.starts_with("resource/")) limits.retainedBytes += member.data.size();
    for (const auto& member : snapshot.OriginalMembers()) limits.retainedBytes += member.bytes.size();
    Require(SlkMapSourceSnapshot::Capture(*archive.value, {}, {}, 11, limits).Succeeded(),
        "one shared budget counts retained originals and final frontend allocations");
    --limits.retainedBytes;
    const auto overrun = SlkMapSourceSnapshot::Capture(*archive.value, {}, {}, 11, limits);
    Require(!overrun.value && HasCode(overrun, "SLK_SOURCES_LIMIT"), "frontend overrun cannot become missing or partial bytes");
    limits = {}; limits.frontendMemberCount = snapshot.FrontendMembers().size() - 1;
    Require(!SlkMapSourceSnapshot::Capture(*archive.value, {}, {}, 11, limits).value, "fixed slots checked before allocation");
    limits = {}; ++limits.frontendMemberCount;
    Require(!SlkMapSourceSnapshot::Capture(*archive.value, {}, {}, 11, limits).value, "frontend hard slot limit cannot be enlarged");
    Require(archive.value->VerifySourceUnchanged().Succeeded(), "frontend capture is read-only");
}
void TestOpaqueFixedDomain() {
    Scratch scratch;
    const auto path = scratch.path / "opaque.mpq";
    WriteOpaqueSources(path, {{"resource/unknown.bin", Bytes("resource")}, {"war3map.w3a", Bytes("old native")},
        {"war3map.wts", {}}, {"units/abilitydata.slk", Bytes("unlisted slk")}});
    const auto archive = MapArchiveSession::Open(path);
    Require(archive.Succeeded() && archive.value->HasOpaqueMembers() && !archive.value->HasMember("war3map.w3a"),
        "fixed sources remain unlisted in opaque session");
    const auto captured = SlkMapSourceSnapshot::Capture(*archive.value, {}, {}, 3);
    Require(captured.Succeeded() && !captured.value->FinalMemberNamesComplete() && !captured.value->NativeOnlySources(),
        "opaque inventory permits exact capture but never proves native-only sources");
    const auto& sources = *captured.value;
    Require(sources.Members()[1].bytes == Bytes("old native") && sources.OriginalMembers()[1].bytes == Bytes("old native"),
        "unlisted native has a true original ancestor");
    Require(sources.LookupMember("units/abilitydata.slk").value->member->bytes == Bytes("unlisted slk"),
        "unlisted high-priority SLK does not fall back to installed data");
    Require(sources.Members()[7].identity.presence == SlkSessionSourcePresence::Present && sources.Members()[7].bytes.empty() &&
        sources.LookupMember("war3map.w3u").value->state == SlkMapSourceLookupState::Missing,
        "empty unlisted Present and proven fixed-domain Missing are distinct");
    Require(sources.LookupMember("outside/not-listed.bin").value->state == SlkMapSourceLookupState::Unknown &&
        sources.LookupMember("resource/unknown.bin").value->state == SlkMapSourceLookupState::PresentUncaptured,
        "incomplete inventory cannot prove domain-outside Missing");
    const std::vector<MemberReplacement> edits{{"war3map.w3a", Bytes("edited native")}};
    const std::vector<std::string> removals{"units/abilitydata.slk"};
    const auto overlaid = SlkMapSourceSnapshot::Capture(*archive.value, edits, removals, 4);
    Require(overlaid.Succeeded() && overlaid.value->OriginalMembers()[1].bytes == Bytes("old native") &&
        overlaid.value->Members()[1].bytes == Bytes("edited native") &&
        overlaid.value->LookupMember("units/abilitydata.slk").value->state == SlkMapSourceLookupState::Missing &&
        overlaid.value->ManifestSha256() != sources.ManifestSha256(), "opaque exact sources accept frozen overlay and true removals");
    Require(archive.value->VerifySourceUnchanged().Succeeded(), "fixed-domain capture leaves anonymous archive untouched");
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 3 && std::string_view(argv[1]) == "--probe") {
            const std::string_view path(argv[2]);
            auto archive = MapArchiveSession::Open(std::filesystem::path(std::u8string(path.begin(), path.end())));
            Require(archive.Succeeded(), "probe source could not open");
            const auto result = SlkMapSourceSnapshot::Capture(*archive.value, {}, {}, 0);
            for (const auto& diagnostic : result.diagnostics) std::cout << diagnostic.code << ": " << diagnostic.message << '\n';
            Require(result.Succeeded(), "probe did not produce a source snapshot");
            std::cout << "sourceSha=" << result.value->SourceArchiveSha256() << "\nmanifestSha=" << result.value->ManifestSha256()
                << "\nnativeOnly=" << result.value->NativeOnlySources() << " members=" << result.value->FinalMemberNames().size() << '\n';
            for (const auto& member : result.value->Members()) std::cout << member.name << " bytes=" << member.bytes.size()
                << " presence=" << static_cast<int>(member.identity.presence) << " sha=" << member.identity.sha256 << '\n';
            Require(archive.value->VerifySourceUnchanged().Succeeded(), "probe source changed");
            return 0;
        }
        Require(argc == 1, "usage: [--probe map]");
        TestSnapshot();
        TestFrontendSources();
        TestOpaqueFixedDomain();
        std::cout << "SlkMapSourceSnapshot: final source identities, overlays and bounded immutable bytes passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
