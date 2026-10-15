#include "War3MapCore/MapDocument.h"
#include "War3MapCore/MapInfo.h"
#include "War3MapCore/Hash.h"
#include "../src/Core/Mpq/MpqCrypt.h"
#include "MapDataFixtures.h"
#include "LniMapSettingsFixtures.h"

#include <Windows.h>
#include <algorithm>
#include <array>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace War3Map;
namespace {
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
std::vector<std::byte> Bytes(std::string_view text) {
    const auto* p = reinterpret_cast<const std::byte*>(text.data()); return {p, p + text.size()};
}
std::vector<MapArchiveMember> Members() {
    return {{"war3map.w3i", MapDataFixtures::FullMapSettingsV25().bytes},
        {"war3mapUnits.doo", MapDataFixtures::EmptyUnitsV8Sub11()},
        {"war3map.doo", MapDataFixtures::EmptyDoodadsV8Sub11()},
        {"war3map.wts", Bytes("STRING 42\n{\n地图说明\n}\n")}};
}
std::vector<std::byte> Region(std::string_view ambientSound, bool duplicate = false) {
    MapDataFixtures::Fixture fixture;
    fixture.Int(5); fixture.Int(duplicate ? 2 : 1);
    for (int i = 0; i < (duplicate ? 2 : 1); ++i) {
        for (const float value : {0.f, 0.f, 512.f, 512.f}) fixture.Float(value);
        fixture.String("Test region"); fixture.Int(12); fixture.Int(0); fixture.String(ambientSound); fixture.UInt(0);
    }
    return fixture.bytes;
}
std::vector<std::byte> Waygate() {
    MapDataFixtures::Fixture f; f.Raw("W3do"); f.Int(8); f.Int(11); f.Int(1);
    f.Raw("hway"); f.Int(0);
    for (int i = 0; i < 4; ++i) f.Float(0);
    for (int i = 0; i < 3; ++i) f.Float(1);
    f.Byte(2); f.Int(0); f.Byte(0); f.Byte(0); f.Int(-1); f.Int(-1);
    f.Int(-1); f.Int(0); f.Int(0); f.Float(-1); // 掉落表、掉落组、金币、索敌。
    for (int i = 0; i < 6; ++i) f.Int(0); // 英雄等级/三属性、库存、技能。
    f.Int(-1); f.Int(-1); f.Int(12); f.Int(7); // 随机类型、团队色、传送门区域、实例 ID。
    return f.bytes;
}
void TestSourceStates() {
    auto members = Members();
    auto document = MapDocument::FromMembers(members);
    Require(document.InitializationSourceStatus().Succeeded(), "complete required sources");
    Require(document.Settings()->players.size() == 2, "typed settings");
    Require(document.Member(MapDataKind::Units).recordCount == 0, "present zero count");
    Require(!document.Member(MapDataKind::Regions).recordCount, "missing count is unknown");
    Require(*document.ResolveString("TRIGSTR_0042").value == "地图说明", "WTS resolved without changing raw text");
    Require(document.Settings()->description == "TRIGSTR_0042", "reference preserved");
    members.push_back({"war3map.w3c", Bytes("broken")});
    document = MapDocument::FromMembers(members);
    Require(document.Member(MapDataKind::Cameras).state == MapDataState::Invalid, "corrupt camera isolated");
    Require(document.Settings().has_value() && document.Strings().has_value(), "other members remain readable");
    Require(document.Member(MapDataKind::Cameras).sources[0].data == Bytes("broken"), "corrupt original bytes retained");
    Require(!document.InitializationSourceStatus().Succeeded(), "corruption blocks initialization data readiness");
    members = Members(); members[0].name = "WAR3MAP.W3I";
    Require(MapDocument::FromMembers(members).Settings().has_value(), "archive case insensitive");
    members.push_back({"war3map.w3i", members[0].data});
    document = MapDocument::FromMembers(members);
    Require(document.Member(MapDataKind::Settings).state == MapDataState::Ambiguous && !document.Settings(), "duplicate source rejected");
    Require(document.Member(MapDataKind::Settings).sources.size() == 2, "both inputs retained");
}
void TestReferences() {
    auto members = Members(); members.pop_back();
    auto document = MapDocument::FromMembers(members);
    Require(!document.InitializationSourceStatus().Succeeded(), "explicit missing WTS reference blocks");
    Require(!document.ResolveString("TRIGSTR_42").Succeeded(), "unresolved value not defaulted");
    Require(*document.ResolveString("literal").value == "literal", "literal does not require WTS");
    members = Members(); members.push_back({"war3map.w3r", Region("MissingSound")});
    Require(!MapDocument::FromMembers(members).InitializationSourceStatus().Succeeded(), "explicit missing sound blocks");
    members.back().data = Region(""); members[1].data = Waygate();
    document = MapDocument::FromMembers(members);
    Require(document.Units() && document.Units()->units.size() == 1, "waygate fixture parsed");
    Require(document.InitializationSourceStatus().Succeeded(), "waygate resolved");
    members.pop_back();
    Require(!MapDocument::FromMembers(members).InitializationSourceStatus().Succeeded(), "missing waygate region blocks");
    members.push_back({"war3map.w3r", Region("", true)});
    Require(MapDocument::FromMembers(members).Member(MapDataKind::Regions).state == MapDataState::Partial, "duplicate region identity not ready");
}
void TestDirectoryAndArchive() {
    const auto root = std::filesystem::temp_directory_path() / ("Forge-map-document-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
    Require(std::filesystem::create_directories(root / "map"), "temporary directory created");
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code e; std::filesystem::remove_all(path, e); } } cleanup{root};
    const auto write = [&](const std::filesystem::path& relative, const std::vector<std::byte>& bytes) {
        std::ofstream file(root / relative, std::ios::binary); file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size()); Require(bool(file), "fixture write");
    };
    auto members = Members();
    for (const auto& member : members) write(std::filesystem::path("map") / member.name, member.data);
    auto workspace = DirectoryWorkspace::Open(root); Require(workspace.Succeeded(), "snapshot directory");
    auto document = MapDocument::FromDirectory(*workspace.value); Require(document.Succeeded(), "load directory data");
    Require(document.value->InitializationSourceStatus().Succeeded(), "directory full input");
    write("map/war3map.wts", Bytes("STRING 42\n{\nchanged\n}\n"));
    Require(!MapDocument::FromDirectory(*workspace.value).Succeeded(), "read changed source rejects snapshot mismatch");
    write("WAR3MAP.W3I", members[0].data);
    workspace = DirectoryWorkspace::Open(root); document = MapDocument::FromDirectory(*workspace.value);
    Require(document.Succeeded() && document.value->Member(MapDataKind::Settings).state == MapDataState::Ambiguous, "root and map conflicts remain reviewable");
    members.push_back({"opaque.bin", {std::byte{0xff}, std::byte{0}}});
    const auto archivePath = root / "source.mpq";
    Require(WriteVerifiedArchive(archivePath, members).Succeeded(), "synthetic archive");
    auto archive = MapArchiveSession::Open(archivePath); Require(archive.Succeeded(), "archive snapshot");
    const auto fromArchive = MapDocument::FromArchive(*archive.value);
    Require(fromArchive.InitializationSourceStatus().Succeeded(), "archive full input");
    Require(fromArchive.Member(MapDataKind::Settings).sources[0].data == members[0].data, "input bytes retained");
    Require(archive.value->VerifySourceUnchanged().Succeeded(), "inspection left archive untouched");
    Require(archive.value->ReadMember("opaque.bin").value == members.back().data, "unknown input not consumed or replaced");
}
void TestReadOnlyArchiveBinding() {
    const auto root = std::filesystem::temp_directory_path() / ("Audastra-readonly-archive-" +
        std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
    Require(std::filesystem::create_directories(root), "owned read-only archive directory");
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code e; std::filesystem::remove_all(path, e); } } cleanup{root};
    const auto path = root / "signed-shape.w3x";
    auto members = Members();
    members.push_back({"Imported/marker.bin", Bytes("original imported asset")});
    Require(WriteVerifiedArchive(path, members).Succeeded(), "write original read-only fixture");
    // 只合成外部签名的结构形态，不宣称该签名通过密码学验证。
    std::array<char, 260> trailer{};
    std::copy_n("NGIS", 4, trailer.begin());
    {
        std::ofstream file(path, std::ios::binary | std::ios::app);
        file.write(trailer.data(), static_cast<std::streamsize>(trailer.size()));
        Require(bool(file), "append synthetic NGIS and 256 signature bytes");
    }
    const auto digest = Sha256File(path); Require(digest.Succeeded(), "capture whole source including trailer");
    const auto strict = MapArchiveSession::Open(path);
    Require(!strict.Succeeded() && std::any_of(strict.diagnostics.begin(), strict.diagnostics.end(), [](const auto& note) {
        return note.code == "ARCHIVE_TRAILING_BYTES_REJECTED";
    }), "external trailer never grants strict save eligibility");

    auto opened = MpqWorkspace::OpenReadOnly(path); Require(opened.Succeeded(), "read-only binding accepts external trailer");
    auto& binding = *opened.value;
    Require(binding.SourceSha256() == *digest.value && std::filesystem::equivalent(binding.SourcePath(), path),
        "read-only path and whole-file SHA refer to original source");
    Require(binding.VerifyUnchanged().Succeeded(), "fresh read-only lease verifies");
    const auto asset = std::find_if(binding.KnownMembers().begin(), binding.KnownMembers().end(), [](const auto& member) {
        return member.name == "Imported/marker.bin";
    });
    Require(asset != binding.KnownMembers().end() && asset->fileSize == members.back().data.size(),
        "listed imported asset metadata survives read-only fallback");
    const auto budget = (std::max)(asset->fileSize, asset->compressedSize);
    const auto imported = binding.ReadMember("Imported/marker.bin", budget);
    Require(imported.Succeeded() && *imported.value == members.back().data, "exact storage and decoded budget reads complete imported bytes");
    const auto overBudget = binding.ReadMember("Imported/marker.bin", budget - 1);
    Require(!overBudget.Succeeded() && std::any_of(overBudget.diagnostics.begin(), overBudget.diagnostics.end(), [](const auto& note) {
        return note.code == "FILE_READ_BUDGET_EXCEEDED";
    }), "too-small member budget rejects instead of truncating");
    const auto missing = binding.ReadMember("absent.bin", budget);
    Require(!missing.Succeeded() && std::any_of(missing.diagnostics.begin(), missing.diagnostics.end(), [](const auto& note) {
        return note.code == "FILE_NOT_FOUND";
    }), "missing member remains distinct from read budget failure");
    const auto document = MapDocument::FromReadOnlyArchive(binding);
    Require(document.Succeeded() && document.value->InitializationSourceStatus().Succeeded(), "complete map data readable through bound archive");
    Require(document.value->SourcePath() == binding.SourcePath()
        && document.value->Member(MapDataKind::Settings).sources[0].data == members[0].data,
        "document provenance and original member bytes preserved");
    Require(MapDocument::OpenReadOnlyArchive(path).Succeeded(), "path convenience reader shares read-only behavior");
    const auto afterRead = Sha256File(path);
    Require(afterRead.Succeeded() && *afterRead.value == *digest.value, "read-only inspection preserves full source SHA");
    Require(!MapArchiveSession::Open(path).Succeeded(), "successful inspection cannot promote source to editable snapshot");

    // 保留旧句柄，再以完全相同的字节替换路径；仅比较SHA会错误放行旧绑定。
    const auto retired = root / "retired.w3x";
    std::filesystem::rename(path, retired);
    Require(std::filesystem::copy_file(retired, path), "replace path with distinct file containing identical bytes");
    const auto replacementDigest = Sha256File(path);
    Require(replacementDigest.Succeeded() && *replacementDigest.value == *digest.value, "replacement deliberately has identical SHA");
    const auto stale = binding.VerifyUnchanged();
    Require(!stale.Succeeded() && std::any_of(stale.diagnostics.begin(), stale.diagnostics.end(), [](const auto& note) {
        return note.code == "MPQ_SOURCE_CHANGED";
    }), "same-byte path replacement invalidates original file identity");
    Require(!binding.ReadMember("Imported/marker.bin", budget).Succeeded(), "stale binding cannot continue member reads");
    Require(!MapDocument::FromReadOnlyArchive(binding).Succeeded(), "stale binding cannot produce a new map document");
    auto fresh = MpqWorkspace::OpenReadOnly(path); Require(fresh.Succeeded(), "explicit reopen binds replacement identity");
    Require(fresh.value->SourceSha256() == binding.SourceSha256() && fresh.value->VerifyUnchanged().Succeeded(),
        "new identity can retain equal content hash");
    Require(MapDocument::FromReadOnlyArchive(*fresh.value).Succeeded(), "explicitly rebound source is readable");
}
void TestReadOnlyArchiveWithoutListfile() {
    const auto root = std::filesystem::temp_directory_path() / ("Audastra-unlisted-document-" +
        std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
    Require(std::filesystem::create_directories(root), "owned unlisted archive directory");
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code e; std::filesystem::remove_all(path, e); } } cleanup{root};
    // 原创单成员v0归档：完整W3I按名字可读，物理上不存在(listfile)，没有匿名填充块。
    const auto settings = MapDataFixtures::FullMapSettingsV25().bytes;
    const auto hashOffset = static_cast<std::uint32_t>(32 + settings.size());
    const auto blockOffset = hashOffset + 64;
    std::vector<std::byte> image(blockOffset + 16);
    const auto put = [&](std::size_t at, std::uint32_t value) {
        for (unsigned shift = 0; shift < 32; shift += 8) image[at + shift / 8] = static_cast<std::byte>((value >> shift) & 0xffu);
    };
    put(0, 0x1a51504d); put(4, 32); put(8, static_cast<std::uint32_t>(image.size())); put(12, 0x00030000);
    put(16, hashOffset); put(20, blockOffset); put(24, 4); put(28, 1);
    std::copy(settings.begin(), settings.end(), image.begin() + 32);
    std::fill(image.begin() + hashOffset, image.begin() + blockOffset, std::byte{0xff});
    const auto& crypt = Mpq::MpqCrypt::Instance();
    const auto entry = hashOffset + (crypt.HashString("war3map.w3i", Mpq::MpqCrypt::HashType::TableOffset) & 3u) * 16;
    put(entry, crypt.HashString("war3map.w3i", Mpq::MpqCrypt::HashType::NameA));
    put(entry + 4, crypt.HashString("war3map.w3i", Mpq::MpqCrypt::HashType::NameB));
    put(entry + 8, 0); put(entry + 12, 0);
    crypt.EncryptBlock(image.data() + hashOffset, 64, crypt.HashString("(hash table)", Mpq::MpqCrypt::HashType::FileKey));
    put(blockOffset, 32); put(blockOffset + 4, static_cast<std::uint32_t>(settings.size()));
    put(blockOffset + 8, static_cast<std::uint32_t>(settings.size())); put(blockOffset + 12, Mpq::kFileExists | Mpq::kFileSingleUnit);
    crypt.EncryptBlock(image.data() + blockOffset, 16, crypt.HashString("(block table)", Mpq::MpqCrypt::HashType::FileKey));
    const auto path = root / "unlisted.w3x";
    {
        std::ofstream file(path, std::ios::binary);
        file.write(reinterpret_cast<const char*>(image.data()), static_cast<std::streamsize>(image.size()));
        Require(bool(file), "write original no-listfile fixture");
    }
    const auto before = Sha256File(path); Require(before.Succeeded(), "capture unlisted fixture identity");
    auto opened = MpqWorkspace::OpenReadOnly(path); Require(opened.Succeeded(), "read-only unlisted archive opens");
    Require(!opened.value->Archive().HasFile("(listfile)"), "fixture has no listfile at all");
    const auto member = opened.value->ReadMember("war3map.w3i", settings.size());
    Require(member.Succeeded() && *member.value == settings, "unlisted standard member found by name hash");
    const auto document = MapDocument::FromReadOnlyArchive(*opened.value);
    Require(document.Succeeded() && document.value->Settings().has_value()
        && document.value->Member(MapDataKind::Settings).state == MapDataState::Ready
        && document.value->Member(MapDataKind::Settings).sources[0].data == settings,
        "MapDocument does not interpret an incomplete name inventory as absent known map data");
    Require(document.value->Member(MapDataKind::Units).state == MapDataState::Missing,
        "genuinely absent member remains missing");
    Require(!MapArchiveSession::Open(path).Succeeded(), "no-listfile read access cannot authorize full-map save");
    const auto after = Sha256File(path);
    Require(after.Succeeded() && *after.value == *before.value && opened.value->SourceSha256() == *before.value,
        "unlisted inspection retains full source identity and bytes");
}
void TestLegacyHeaderTruncation() {
    const auto valid = MapDataFixtures::FullMapSettingsV25().bytes;
    Require(MapInfo::Parse(valid).Succeeded(), "legacy header still readable");
    std::vector<std::byte> broken(valid.begin(), valid.begin() + 18);
    Require(!MapInfo::Parse(broken).Succeeded(), "unterminated header strings rejected");
}
void TestDirectoryCameraLayoutSourceAliases() {
    MapDataFixtures::Fixture camera;
    camera.Int(0); camera.Int(1);
    for (const float value : {0.f, 0.f, 0.f, 0.f, 0.f, 1600.f, 0.f, 70.f, 5000.f, 100.f}) camera.Float(value);
    camera.String("Original camera");
    const auto ambiguous = MapData::ReadCamerasVersioned(camera.bytes);
    Require(!ambiguous.Succeeded() && std::any_of(ambiguous.diagnostics.begin(), ambiguous.diagnostics.end(),
        [](const auto& row) { return row.code == "W3C_LAYOUT_AMBIGUOUS"; }),
        "long classic name deliberately needs trustworthy source-version evidence");
    Require(MapData::ParseCameras(camera.bytes).Succeeded(), "same independent W3C is a complete classic record");

    const auto tft = MapDataFixtures::FullMapSettingsV25();
    auto roc = tft;
    roc.bytes.resize(tft.offsets.at("camera") + 61);
    roc.bytes[0] = std::byte{18};
    for (const auto number : {17, 29}) {
        roc.Int(number);
        for (const auto text : {"Text", "Title", "Subtitle"}) roc.String(text);
    }
    roc.bytes.insert(roc.bytes.end(), tft.bytes.begin() + tft.offsets.at("players"),
        tft.bytes.begin() + tft.offsets.at("itemTables"));

    const auto root = std::filesystem::temp_directory_path() / ("Audastra-camera-source-" +
        std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
    Require(std::filesystem::create_directories(root), "owned camera-source directory");
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code e; std::filesystem::remove_all(path, e); } } cleanup{root};
    const auto load = [&](const std::string& caseName, std::vector<MapArchiveMember> settings) {
        settings.push_back({"map/war3map.w3c", camera.bytes});
        const auto directory = root / caseName;
        for (const auto& member : settings) {
            const auto filePath = directory / member.name;
            std::filesystem::create_directories(filePath.parent_path());
            std::ofstream file(filePath, std::ios::binary);
            file.write(reinterpret_cast<const char*>(member.data.data()), static_cast<std::streamsize>(member.data.size()));
            Require(bool(file), "write exact camera-source fixture");
        }
        const auto source = DirectoryWorkspace::Open(directory); Require(source.Succeeded(), "camera-source actual snapshot");
        auto document = MapDocument::FromDirectory(*source.value); Require(document.Succeeded(), "camera-source actual directory parse");
        return std::move(*document.value);
    };
    for (const auto version : {18u, 25u}) {
        const auto& settingsBytes = version == 18u ? roc.bytes : tft.bytes;
        for (const bool mapAlias : {false, true}) {
            const std::string name = mapAlias ? "map/war3map.w3i" : "war3map.w3i";
            const auto document = load(std::to_string(version) + (mapAlias ? "-map" : "-root"), {{name, settingsBytes}});
            Require(document.Settings() && document.Settings()->formatVersion == static_cast<std::int32_t>(version) && document.Settings()->FullyParsed(),
                "root and map aliases provide the same complete actual 18/25 W3I");
            Require(document.Member(MapDataKind::Settings).sources.front().name == name
                && document.Member(MapDataKind::Settings).sources.front().data == settingsBytes,
                "version evidence keeps the original source name and every W3I byte");
            Require(document.Member(MapDataKind::Cameras).state == MapDataState::Ready && document.Cameras()
                && document.Cameras()->layout == MapData::CameraLayout::Classic
                && document.Cameras()->entries.front().name == "Original camera"
                && document.Cameras()->entries.front().targetDistance == 1600.f
                && document.Cameras()->Serialize().value == camera.bytes,
                "root/map classic version evidence recovers the full long-name camera without rewriting it");
            if (version == 18u) Require(!document.Settings()->Serialize().Succeeded(), "ROC source recognition never grants W3I write permission");
        }
    }
    const auto mixedCase = load("mixed-case", {{"map/WAR3MAP.W3I", tft.bytes}});
    Require(mixedCase.Cameras() && mixedCase.Cameras()->layout == MapData::CameraLayout::Classic
        && mixedCase.Member(MapDataKind::Settings).sources.front().name == "map/WAR3MAP.W3I",
        "canonical alias comparison preserves original case");

    const auto reject = [&](const MapDocument& document) {
        const auto& member = document.Member(MapDataKind::Cameras);
        Require(!document.Cameras() && member.state == MapDataState::Invalid
            && member.sources.front().data == camera.bytes
            && std::any_of(member.diagnostics.begin(), member.diagnostics.end(),
                [](const auto& row) { return row.code == "W3C_LAYOUT_AMBIGUOUS"; }),
            "unproven or conflicting source never chooses a camera layout, and retains all original bytes");
    };
    reject(load("missing", {}));
    reject(load("unrecognized-prefix", {{"other/war3map.w3i", tft.bytes}}));
    auto future = tft.bytes; future[0] = std::byte{32};
    for (const auto name : {"war3map.w3i", "map/war3map.w3i"}) reject(load(std::string("future-") + (name[0] == 'm' ? "map" : "root"), {{name, future}}));

    const auto nativeConflict = load("native-conflict", {{"war3map.w3i", tft.bytes}, {"map/war3map.w3i", tft.bytes}});
    Require(nativeConflict.Member(MapDataKind::Settings).state == MapDataState::Ambiguous
        && nativeConflict.Member(MapDataKind::Settings).sources.size() == 2 && !nativeConflict.Settings(),
        "root/map duplicate W3I sources still refuse arbitrary source selection");
    reject(nativeConflict);
    const auto lni = LniMapSettingsFixtures::Bytes(LniMapSettingsFixtures::Complete());
    const auto lniOnly = load("lni-only", {{"table/w3i.ini", lni}});
    Require(lniOnly.LniSettings() && lniOnly.Cameras() && lniOnly.Cameras()->layout == MapData::CameraLayout::Classic,
        "existing parsed LNI classic evidence stays separate from the native alias rule");
    const auto lniConflict = load("lni-conflict", {{"map/war3map.w3i", tft.bytes}, {"table/w3i.ini", lni}});
    Require(lniConflict.Member(MapDataKind::Settings).state == MapDataState::Ambiguous
        && lniConflict.Member(MapDataKind::Settings).sources.size() == 2 && !lniConflict.Settings() && !lniConflict.LniSettings(),
        "native/LNI conflict grants neither format authority");
    reject(lniConflict);
}
void TestLniSettingsProvenanceAndConflicts() {
    auto members = Members();
    const auto original = LniMapSettingsFixtures::Bytes(LniMapSettingsFixtures::Complete());
    members[0] = {"table/w3i.ini", original};
    auto document = MapDocument::FromMembers(members);
    Require(document.Settings().has_value() && document.LniSettings().has_value(), "LNI typed data exposed");
    Require(document.Settings()->OriginalBytes().empty(), "LNI never claims original native bytes");
    Require(document.LniSettings()->originalBytes == original, "LNI original text retained");
    Require(document.Member(MapDataKind::Settings).memberName == "table/w3i.ini", "actual source path");
    Require(document.Member(MapDataKind::Settings).state == MapDataState::Ready, "known LNI ready");
    Require(document.InitializationSourceStatus().Succeeded(), "LNI can supply initialization input");
    members.push_back({"war3map.w3i", MapDataFixtures::FullMapSettingsV25().bytes});
    document = MapDocument::FromMembers(members);
    Require(document.Member(MapDataKind::Settings).state == MapDataState::Ambiguous, "native and LNI conflict");
    Require(!document.Settings() && !document.LniSettings(), "conflict cannot choose arbitrary format");
    members.pop_back();
    members[0].data = LniMapSettingsFixtures::Bytes(LniMapSettingsFixtures::Complete() + "[extension]\nvalue = 9\n");
    document = MapDocument::FromMembers(members);
    Require(document.Member(MapDataKind::Settings).state == MapDataState::Partial, "unknown LNI stays partial");
    Require(!document.InitializationSourceStatus().Succeeded(), "unknown semantics block complete initialization");

    const auto root = std::filesystem::temp_directory_path() / ("Forge-lni-document-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
    Require(std::filesystem::create_directories(root / "table"), "LNI fixture directory");
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code e; std::filesystem::remove_all(path, e); } } cleanup{root};
    const auto write = [&](const std::filesystem::path& path, const std::vector<std::byte>& bytes) {
        std::ofstream out(root / path, std::ios::binary); out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        Require(bool(out), "LNI fixture written");
    };
    write("table/w3i.ini", original);
    auto snapshot = DirectoryWorkspace::Open(root); Require(snapshot.Succeeded(), "LNI directory snapshot");
    auto parsed = MapDocument::FromDirectory(*snapshot.value);
    Require(parsed.Succeeded() && parsed.value->LniSettings().has_value(), "directory reads LNI settings");
    Require(parsed.value->Member(MapDataKind::Settings).sources[0].data == original, "directory text preserved");
    write("table/w3i.ini", Bytes("changed"));
    Require(!MapDocument::FromDirectory(*snapshot.value).Succeeded(), "changed LNI cannot reuse stale fingerprint");
    write("table/w3i.ini", original);
    write("war3map.w3i", MapDataFixtures::FullMapSettingsV25().bytes);
    snapshot = DirectoryWorkspace::Open(root);
    parsed = MapDocument::FromDirectory(*snapshot.value);
    Require(parsed.Succeeded() && parsed.value->Member(MapDataKind::Settings).state == MapDataState::Ambiguous,
        "directory native LNI conflict retained");
}

void TestV31WidgetLayoutIsolation() {
    auto members = Members();
    auto settings = MapData::MapSettings::Parse(members[0].data);
    Require(settings.Succeeded(), "v25 fixture must parse before building v31 routing fixture");
    settings.value->formatVersion = 31;
    settings.value->gameVersion = {1, 32, 10, 20456};
    const auto encoded = settings.value->Serialize();
    Require(encoded.Succeeded(), "v31 routing fixture must serialize");
    members[0].data = *encoded.value;

    auto unit = Waygate();
    const std::array<std::byte, 4> skin{std::byte{'H'}, std::byte{'0'}, std::byte{'0'}, std::byte{'2'}};
    unit.insert(unit.begin() + 52, skin.begin(), skin.end());
    members[1].data = unit;
    const auto untouchedEmptyDoodads = members[2].data;
    const auto document = MapDocument::FromMembers(members);
    Require(document.Settings() && document.Settings()->formatVersion == 31, "v31 settings select versioned widget reader");
    Require(document.Units() && document.Units()->recordLayout == MapData::PlacementRecordLayout::SkinId
        && document.Units()->units.size() == 1 && document.Units()->units[0].skinId == "H002",
        "v31 widget route exposes SkinId without rewriting its source");
    Require(document.Units()->SerializeOriginal() == unit && !document.Units()->Serialize().Succeeded(),
        "v31 skinned widget remains read-only and byte-identical");
    Require(document.Doodads() && document.Member(MapDataKind::Doodads).state == MapDataState::Ready
        && document.Doodads()->recordLayout == MapData::PlacementRecordLayout::Undetermined
        && document.Doodads()->SerializeOriginal() == untouchedEmptyDoodads
        && !document.Doodads()->Serialize().Succeeded(),
        "empty 8/11 doodad table remains readable but cannot be written without a proven layout");

    // v32 设置目前不能完整解析，但头部版本已足以禁止把 SkinId 单位误当经典可编辑记录。
    members[0].data[0] = std::byte{32};
    const auto future = MapDocument::FromMembers(members);
    Require(!future.Settings() && future.Member(MapDataKind::Settings).state == MapDataState::Invalid
        && future.Units() && future.Units()->recordLayout == MapData::PlacementRecordLayout::SkinId
        && future.Units()->SerializeOriginal() == unit,
        "unsupported W3I header version still selects read-only versioned widgets");
}
}
int main() {
    try { TestSourceStates(); TestReferences(); TestDirectoryAndArchive(); TestReadOnlyArchiveBinding();
        TestReadOnlyArchiveWithoutListfile(); TestLegacyHeaderTruncation();
        TestDirectoryCameraLayoutSourceAliases(); TestLniSettingsProvenanceAndConflicts(); TestV31WidgetLayoutIsolation(); }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
    std::cout << "Map document source, isolation, reference and snapshot checks passed\n";
}
