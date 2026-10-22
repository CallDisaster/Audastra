#include "War3MapCore/Hash.h"
#include "War3MapCore/MapData/MapPlacements.h"
#include "War3MapCore/Mpq/MpqWriter.h"
#include "War3MapCore/Services/MapObjectReferenceCapture.h"
#include "War3MapCore/Services/MapSaveService.h"
#include "../MapDataFixtures.h"

#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
using namespace War3Map;
using namespace War3Map::Services;
using Snapshot = Trigger::MapObjectReferenceSnapshot;
using Scope = Trigger::MapObjectReferenceSourceScope;
using Bytes = std::vector<std::byte>;

void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
std::string Codes(const auto& result) {
    std::string text;
    for (const auto& diagnostic : result.diagnostics) text += diagnostic.code + ": " + diagnostic.message + '\n';
    return text;
}
bool HasCode(const auto& result, std::string_view code) {
    return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
        [&](const auto& diagnostic) { return diagnostic.code == code; });
}
Bytes Text(std::string_view value) {
    return {reinterpret_cast<const std::byte*>(value.data()),
        reinterpret_cast<const std::byte*>(value.data()) + value.size()};
}
void Write(const std::filesystem::path& path, std::span<const std::byte> bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    Require(output.good(), "write owned original fixture");
}
class TempDirectory final {
public:
    TempDirectory() : parent_(std::filesystem::absolute(std::filesystem::temp_directory_path()).lexically_normal()) {
        static std::atomic<unsigned> serial{0};
        path_ = parent_ / ("Audastra-EffectiveReferences-" + std::to_string(GetCurrentProcessId()) + '-'
            + std::to_string(GetTickCount64()) + '-' + std::to_string(serial.fetch_add(1)));
        Require(std::filesystem::create_directory(path_), "create exclusive fixture directory");
    }
    ~TempDirectory() noexcept {
        const auto target = path_.lexically_normal();
        if (target.parent_path() != parent_ || !target.filename().string().starts_with("Audastra-EffectiveReferences-")) return;
        std::error_code error;
        std::filesystem::remove_all(target, error);
    }
    const auto& Path() const noexcept { return path_; }
private:
    std::filesystem::path parent_, path_;
};
Bytes Slk(std::string_view id, std::string_view label = "Original fixture") {
    return Text("ID;PWXL\nC;X1;Y1;K\"unitID\"\nC;X2;K\"Name\"\nC;X1;Y2;K\""
        + std::string(id) + "\"\nC;X2;K\"" + std::string(label) + "\"\nE\n");
}
Bytes EmptyNative() {
    MapDataFixtures::Fixture writer;
    writer.Int(2); writer.Int(0); writer.Int(0);
    return std::move(writer.bytes);
}
Bytes Placements() {
    MapData::UnitPlacements records;
    std::int32_t creation = 1;
    for (const auto id : {"U001", "I001", "hfoo"}) {
        MapData::UnitPlacement unit;
        unit.rawcode = id; unit.creationId = creation++; unit.flags = 2;
        unit.scale = {1, 1, 1};
        records.units.push_back(std::move(unit));
    }
    auto encoded = records.Serialize();
    Require(encoded.Succeeded(), "serialize original placement records");
    return std::move(*encoded.value);
}
std::shared_ptr<const Snapshot> Capture(Result<std::shared_ptr<const Snapshot>> result) {
    if (!result.Succeeded()) throw std::runtime_error(Codes(result));
    Require(result.value && *result.value, "capture owns a real qualification snapshot");
    return std::move(*result.value);
}
// 一个具名区域：脚本生成器为它声明 gg_rct_Arena，归档与目录都须授予引用资格。
std::vector<std::byte> Regions() {
    MapData::RegionsDocument regions;
    MapData::RegionRecord arena{};
    arena.left = -64; arena.bottom = -64; arena.right = 64; arena.top = 64;
    arena.name = "Arena"; arena.creationNumber = 1;
    regions.entries.push_back(std::move(arena));
    auto bytes = regions.Serialize();
    Require(bytes.Succeeded() && bytes.value.has_value(), "serialize original region fixture");
    return std::move(*bytes.value);
}
struct Fixture {
    TempDirectory temporary;
    std::filesystem::path directory = temporary.Path() / "expanded";
    std::filesystem::path game = temporary.Path() / "game";
    std::filesystem::path source = temporary.Path() / "source.w3x";
    std::vector<MapArchiveMember> members;
    std::unique_ptr<MapArchiveSession> archive;
    explicit Fixture(bool mapTables = true) {
        members = {{"war3map.w3i", MapDataFixtures::FullMapSettingsV25().bytes},
            {"war3mapUnits.doo", Placements()}, {"war3map.w3u", EmptyNative()}, {"war3map.w3t", EmptyNative()},
            {"war3map.j", Text("function main takes nothing returns nothing\nendfunction\n")},
            {"payload.bin", Text("original payload")}, {"war3map.w3r", Regions()}};
        if (mapTables) {
            members.push_back({"Units/UnitData.slk", Slk("U001")});
            members.push_back({"Units/ItemData.slk", Slk("I001")});
        }
        for (const auto& member : members) Write(directory / "map" / std::filesystem::path(member.name), member.data);
        std::filesystem::create_directories(game);
        const std::vector<Mpq::MpqBuildFile> gameFiles{
            {"Units/UnitData.slk", Slk("hfoo"), false}, {"Units/ItemData.slk", Slk("ratf"), false}};
        Require(Mpq::WriteMpqArchive(game / "war3.mpq", gameFiles).Succeeded(), "write synthetic game source");
        Require(WriteVerifiedArchive(source, members).Succeeded(), "write original test map");
        auto opened = MapArchiveSession::Open(source);
        Require(opened.Succeeded(), "open owned test archive");
        archive = std::make_unique<MapArchiveSession>(std::move(*opened.value));
    }
    void Prepare(MapSaveTask& task, std::shared_ptr<const Snapshot> snapshot, std::string_view name) const {
        Require(task.FreezeArchive(*archive).Succeeded(), "freeze original archive");
        task.target = temporary.Path() / std::filesystem::path(name);
        task.mapObjectReferences = std::move(snapshot);
        task.mapObjectReferenceGameRoot = game;
        task.replacements = {{"payload.bin", Text("changed owned payload")}};
    }
    void SourceUnchanged() const {
        auto hash = Sha256File(source);
        Require(hash.Succeeded() && *hash.value == archive->SourceSha256(), "author-like source remains byte identical");
    }
};

void MapTablesQualifyAndShadowWholeGameFiles() {
    Fixture fixture;
    const auto snapshot = Capture(CaptureDirectoryMapObjectReferences(fixture.directory, fixture.game));
    Require(snapshot->Find("gg_unit_U001_0001") && snapshot->Find("gg_item_I001_0002"),
        "map-only SLK rows supply positive unit and item classifications without native objects");
    Require(!snapshot->Find("gg_unit_hfoo_0003"), "whole map UnitData shadows absent game rows");
    Require(snapshot->Inputs()[3].scope == Scope::Map && snapshot->Inputs()[4].scope == Scope::Map,
        "source scope records map tables rather than game standards");
    Require(VerifyDirectoryMapObjectReferenceClassificationSources(*snapshot, fixture.directory, fixture.game).Succeeded(),
        "unchanged current effective sources verify");
    Require(!VerifyMapObjectReferenceStandardSources(*snapshot, fixture.game).Succeeded(),
        "game-only verifier cannot grant authority to map SLK sources");
    fixture.SourceUnchanged();
}

void MapTableMigrationAndFinalSaveStayBound() {
    Fixture fixture;
    const auto directory = Capture(CaptureDirectoryMapObjectReferences(fixture.directory, fixture.game));
    const auto archive = Capture(CaptureArchiveMapObjectReferences(*fixture.archive, fixture.game));
    Require(directory->Identity() != archive->Identity(), "strict identities retain actual containers");
    Require(directory->Find("gg_rct_Arena") && archive->Find("gg_rct_Arena"),
        "archive capture qualifies named regions like the expanded directory");
    Require(VerifyMapObjectReferenceContents(*directory, *archive).Succeeded(), "explicit save migration accepts equal map table contents");
    MapSaveTask task;
    fixture.Prepare(task, directory, "same-map-tables.w3x");
    task.replacements.push_back({"units\\unitdata.slk", Slk("U001")});
    MapSaveService::Run(task);
    if (!task.succeeded) throw std::runtime_error(Codes(task));
    Require(task.nextSession && std::filesystem::exists(task.target), "shared save service publishes and reopens valid output");
    const auto after = Capture(CaptureArchiveMapObjectReferences(*task.nextSession, fixture.game));
    Require(VerifyMapObjectReferenceContents(*directory, *after).Succeeded(), "reopened final map retains the same qualified placements");
    for (const auto change : {"same-id-different-data", "different-row"}) {
        MapSaveTask rejected;
        fixture.Prepare(rejected, directory, std::string(change) + ".w3x");
        rejected.replacements.push_back({"Units/UnitData.slk", change == std::string_view("different-row")
            ? Slk("U002") : Slk("U001", "Changed table body")});
        MapSaveService::Run(rejected);
        Require(!rejected.succeeded && HasCode(rejected, "MAP_OBJECT_REFERENCE_SOURCE_CHANGED")
            && !std::filesystem::exists(rejected.target), "final SLK mutations reject before publication");
    }
    fixture.SourceUnchanged();
}

void SameBytesChangingLayersAreRejected() {
    Fixture fixture(false);
    const auto old = Capture(CaptureDirectoryMapObjectReferences(fixture.directory, fixture.game));
    Require(old->Inputs()[3].scope == Scope::External && old->Find("gg_unit_hfoo_0003"), "confirmed map absence permits game table");
    Write(fixture.directory / "map/Units/UnitData.slk", Slk("hfoo"));
    const auto current = Capture(CaptureDirectoryMapObjectReferences(fixture.directory, fixture.game));
    Require(current->Inputs()[3].bytes == old->Inputs()[3].bytes && current->Inputs()[3].scope == Scope::Map,
        "fixture changes only selected source layer, not table bytes");
    Require(!VerifyMapObjectReferenceContents(*old, *current).Succeeded(), "save comparison rejects same-byte game-to-map layer switch");
    Require(!VerifyDirectoryMapObjectReferenceClassificationSources(*old, fixture.directory, fixture.game).Succeeded(),
        "live verification sees a newly introduced map shadow");
    Write(fixture.directory / "map/Units/UnitData.slk", Slk("U001"));
    Require(!VerifyDirectoryMapObjectReferenceClassificationSources(*current, fixture.directory, fixture.game).Succeeded(),
        "live verification binds actual map table bytes");
}

void InvalidHighSourceAndDirectoryAliasesNeverFallBack() {
    Fixture fixture;
    Write(fixture.directory / "map/Units/UnitData.slk", Text("not an SLK table"));
    const auto invalid = CaptureDirectoryMapObjectReferences(fixture.directory, fixture.game);
    Require(!invalid.Succeeded() && HasCode(invalid, "MAP_OBJECT_REFERENCE_STANDARD_UNQUALIFIED"),
        "present malformed map table cannot recover game classification");
    Write(fixture.directory / "map/Units/UnitData.slk", Slk("U001"));
    Write(fixture.directory / "Units/UnitData.slk", Slk("U001"));
    const auto conflicting = CaptureDirectoryMapObjectReferences(fixture.directory, fixture.game);
    Require(!conflicting.Succeeded() && HasCode(conflicting, "OBJECT_CATALOG_DIRECTORY_CONFLICT"),
        "root and map aliases must not pick one copy even when bytes match");
}

void MapSwitchDoesNotReusePreviousClassification() {
    Fixture first, second;
    Write(second.directory / "map/Units/UnitData.slk", Slk("U002", "Second map"));
    const auto before = Capture(CaptureDirectoryMapObjectReferences(first.directory, first.game));
    const auto after = Capture(CaptureDirectoryMapObjectReferences(second.directory, second.game));
    Require(before->Find("gg_unit_U001_0001") && !after->Find("gg_unit_U001_0001"),
        "same placement id is reclassified against the current map table");
    Require(!VerifyMapObjectReferenceContents(*before, *after).Succeeded(), "different effective data cannot cross map bindings");
}
} // namespace

int main() {
    try {
        MapTablesQualifyAndShadowWholeGameFiles();
        MapTableMigrationAndFinalSaveStayBound();
        SameBytesChangingLayersAreRejected();
        InvalidHighSourceAndDirectoryAliasesNeverFallBack();
        MapSwitchDoesNotReusePreviousClassification();
        std::cout << "effective map object references passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
