#include "War3MapCore/Render/ClassicMinimapObjectAssets.h"
#include "War3MapCore/Render/TerrainMinimapAssets.h"

#include "War3MapCore/Hash.h"
#include "War3MapCore/IO/FileBytes.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/MapData/TerrainEditing.h"
#include "War3MapCore/MapDocument.h"
#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/Mpq/MpqWriter.h"
#include "../src/Core/Mpq/MpqCrypt.h"
#include "MapDataFixtures.h"

#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <initializer_list>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <span>
#include <source_location>
#include <type_traits>
#include <vector>

using namespace War3Map;
using namespace War3Map::MapData;
using namespace War3Map::Render;
using Bytes = std::vector<std::byte>;

void Require(bool value, std::string_view message) { if (!value) throw std::runtime_error(std::string(message)); }

template<class T> T Take(Result<T> result) {
    if (!result.Succeeded() || !result.value) {
        std::string message = "fixture result failed";
        for (const auto& diagnostic : result.diagnostics) message += " | " + diagnostic.code + ":" + diagnostic.message;
        throw std::runtime_error(message);
    }
    return std::move(*result.value);
}

template<class T> void Rejected(const Result<T>& result, std::string_view code = {},
    const std::source_location location = std::source_location::current()) {
    const auto context = "capture negative at line " + std::to_string(location.line()) + ": ";
    Require(!result.Succeeded() && !result.value && !result.diagnostics.empty(), context + "失败必须无部分结果且带诊断");
    if (!code.empty()) {
        Require(std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
            [&](const auto& diagnostic) { return diagnostic.code == code; }), context + "失败诊断不是预期的fail-closed门");
    }
}

class TempDirectory {
public:
    TempDirectory() {
        static std::atomic<unsigned> sequence{0};
        path_ = std::filesystem::temp_directory_path() / ("Audastra-classic-object-capture-"
            + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(sequence.fetch_add(1)));
        Require(std::filesystem::create_directory(path_), "创建对象捕获MPQ临时目录");
    }
    ~TempDirectory() { std::error_code error; std::filesystem::remove_all(path_, error); }
    const std::filesystem::path& Path() const { return path_; }
private:
    std::filesystem::path path_;
};

Bytes Text(std::string_view text) {
    const auto* first = reinterpret_cast<const std::byte*>(text.data());
    return {first, first + text.size()};
}

void Put32(Bytes& bytes, std::size_t at, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) bytes.at(at + shift / 8) = static_cast<std::byte>((value >> shift) & 255);
}

Bytes Slk(std::initializer_list<std::string_view> rows) {
    std::string value = "ID;PWXL;N;E\n"; std::size_t y = 1;
    for (const auto& row : rows) {
        std::size_t x = 1, start = 0;
        while (start <= row.size()) {
            const auto end = row.find('|', start);
            const auto cell = row.substr(start, end == std::string_view::npos ? row.size() - start : end - start);
            value += "C;X" + std::to_string(x++) + ";Y" + std::to_string(y) + ";K\"" + std::string(cell) + "\"\n";
            if (end == std::string_view::npos) break;
            start = end + 1;
        }
        ++y;
    }
    return Text(value + "E\n");
}

Bytes Blp() {
    Bytes bytes(1180 + 341); std::copy_n(Text("BLP1").begin(), 4, bytes.begin());
    Put32(bytes, 4, 1); Put32(bytes, 12, 16); Put32(bytes, 16, 16); Put32(bytes, 20, 5);
    const std::array<std::array<unsigned, 3>, 5> colors{{{80, 120, 160}, {0, 200, 0}, {20, 40, 60}, {250, 250, 0}, {255, 0, 255}}};
    std::size_t offset = 1180;
    for (std::uint32_t level = 0, size = 16; level < 5; ++level, size /= 2) {
        Put32(bytes, 28 + level * 4, static_cast<std::uint32_t>(offset)); Put32(bytes, 92 + level * 4, size * size);
        const auto palette = 156 + (level + 1) * 4;
        for (std::size_t channel = 0; channel < 3; ++channel) bytes[palette + channel] = static_cast<std::byte>(colors[level][2 - channel]);
        std::fill_n(bytes.begin() + static_cast<std::ptrdiff_t>(offset), size * size, static_cast<std::byte>(level + 1));
        offset += static_cast<std::size_t>(size) * size;
    }
    return bytes;
}

std::vector<MapArchiveMember> Materials() {
    return {{"TerrainArt\\Terrain.slk", Slk({"tileID|dir|file", "Tg01|Fixture|Ground"})},
        {"TerrainArt\\CliffTypes.slk", Slk({"cliffID"})},
        {"TerrainArt\\Water.slk", Slk({"waterID"})}, {"Fixture\\Ground.blp", Blp()}};
}

class Fixture {
public:
    TempDirectory directory;
    Bytes w3e, wpm;
    std::optional<Bytes> sourceUnits, sourceDoodads;
    MapDataEditSession edits;

    Fixture(std::optional<Bytes> sourceUnitsArg = MapDataFixtures::EmptyUnitsV8Sub11(),
        std::optional<Bytes> sourceDoodadsArg = MapDataFixtures::EmptyDoodadsV8Sub11(),
        std::optional<Bytes> currentUnitsArg = MapDataFixtures::EmptyUnitsV8Sub11(),
        std::optional<Bytes> currentDoodadsArg = MapDataFixtures::EmptyDoodadsV8Sub11(),
        std::optional<Bytes> currentSettingsArg = {})
        : sourceUnits(std::move(sourceUnitsArg)), sourceDoodads(std::move(sourceDoodadsArg)),
          edits(CurrentDocument(std::move(currentUnitsArg), std::move(currentDoodadsArg), std::move(currentSettingsArg))) {
        TerrainGridSnapshot terrain;
        terrain.header.width = terrain.header.height = 9; terrain.header.mainTileset = 'L';
        terrain.header.groundTilesets = {"Tg01"}; terrain.header.cliffTilesets = {"CLdi"}; terrain.vertices.resize(81);
        for (auto& vertex : terrain.vertices) {
            vertex.groundHeightRaw = 8192; vertex.waterHeightAndFlagsRaw = 8192; vertex.cliffAndLayerRaw = 2;
        }
        RefreshTerrainDerived(terrain, 0, 0, 9, 9);
        w3e = Take(SerializeMapTerrain(terrain)); wpm = Text("MP3W"); wpm.resize(16 + 32 * 32);
        Put32(wpm, 8, 32); Put32(wpm, 12, 32);
        Require(edits.AttachTerrain(w3e).Succeeded(), "附加真实W3E");
        Require(edits.AttachPathing(wpm).Succeeded(), "附加真实WPM");
    }

    static MapDocument CurrentDocument(std::optional<Bytes> units, std::optional<Bytes> doodads, std::optional<Bytes> settings) {
        std::vector<MapArchiveMember> members;
        if (units) members.push_back({"war3mapUnits.doo", std::move(*units)});
        if (doodads) members.push_back({"war3map.doo", std::move(*doodads)});
        if (settings) members.push_back({"war3map.w3i", std::move(*settings)});
        return MapDocument::FromMembers(std::move(members));
    }

    MapArchiveSession Source(std::string_view name = "source.w3x", bool unlistedDoo = false, bool presentEmptyUnits = false) {
        auto members = Materials();
        members.push_back({"war3map.w3e", w3e}); members.push_back({"war3map.wpm", wpm});
        if (sourceUnits || presentEmptyUnits) members.push_back({"war3mapUnits.doo", sourceUnits.value_or(Bytes{})});
        if (sourceDoodads) members.push_back({"war3map.doo", *sourceDoodads});
        const auto path = directory.Path() / std::string(name);
        if (unlistedDoo) {
            Require(IO::WriteFileBytes(path, UnlistedArchive(std::move(members))).Succeeded(), "写清单外DOO MPQ");
        } else {
            Take(WriteVerifiedArchive(path, members));
        }
        return Take(MapArchiveSession::Open(path));
    }

private:
    static Bytes UnlistedArchive(std::vector<MapArchiveMember> members) {
        members.push_back({"(listfile)", Text("war3map.w3e\nwar3map.wpm\n")});
        const auto count = static_cast<std::uint32_t>(members.size());
        std::uint32_t hashCount = 8, payloadBytes = 0;
        while (hashCount < count * 2) hashCount *= 2;
        for (const auto& member : members) payloadBytes += static_cast<std::uint32_t>(member.data.size());
        const auto hashAt = 32 + payloadBytes, blockAt = hashAt + hashCount * 16;
        Bytes bytes(blockAt + count * 16); Put32(bytes, 0, 0x1a51504d); Put32(bytes, 4, 32);
        Put32(bytes, 8, static_cast<std::uint32_t>(bytes.size())); Put32(bytes, 12, 0x00030000);
        Put32(bytes, 16, hashAt); Put32(bytes, 20, blockAt); Put32(bytes, 24, hashCount); Put32(bytes, 28, count);
        std::fill(bytes.begin() + hashAt, bytes.begin() + blockAt, std::byte{255});
        const auto& crypt = Mpq::MpqCrypt::Instance(); std::uint32_t dataAt = 32;
        for (std::uint32_t index = 0; index < count; ++index) {
            const auto& member = members[index]; const auto size = static_cast<std::uint32_t>(member.data.size());
            std::copy(member.data.begin(), member.data.end(), bytes.begin() + dataAt);
            auto bucket = crypt.HashString(member.name, Mpq::MpqCrypt::HashType::TableOffset) & (hashCount - 1);
            while (bytes[hashAt + bucket * 16 + 12] != std::byte{255}) bucket = (bucket + 1) & (hashCount - 1);
            const auto at = hashAt + bucket * 16; Put32(bytes, at, crypt.HashString(member.name, Mpq::MpqCrypt::HashType::NameA));
            Put32(bytes, at + 4, crypt.HashString(member.name, Mpq::MpqCrypt::HashType::NameB)); Put32(bytes, at + 8, 0); Put32(bytes, at + 12, index);
            Put32(bytes, blockAt + index * 16, dataAt); Put32(bytes, blockAt + index * 16 + 4, size); Put32(bytes, blockAt + index * 16 + 8, size);
            Put32(bytes, blockAt + index * 16 + 12, Mpq::kFileExists | Mpq::kFileSingleUnit); dataAt += size;
        }
        crypt.EncryptBlock(bytes.data() + hashAt, hashCount * 16, crypt.HashString("(hash table)", Mpq::MpqCrypt::HashType::FileKey));
        crypt.EncryptBlock(bytes.data() + blockAt, count * 16, crypt.HashString("(block table)", Mpq::MpqCrypt::HashType::FileKey));
        return bytes;
    }
};

TerrainMinimapPostprocessSettings FlatPostprocess() {
    TerrainMinimapPostprocessSettings settings; settings.medianRadius = 0; settings.blurRadius = 0; settings.noisePercent = 0; return settings;
}

TerrainMinimapAssetSnapshot CaptureTerrain(Fixture& fixture, const MapArchiveSession& source) {
    return Take(CaptureTerrainMinimapAssets(source, fixture.edits, {}));
}

void TestRealParsedEmptyAndTypedPixels() {
    Fixture fixture; auto source = fixture.Source(); auto terrain = CaptureTerrain(fixture, source);
    auto assets = Take(CaptureClassicMinimapObjectAssets(std::move(terrain), source, fixture.edits));
    Require(assets.CapturedMembers()[0].state == ClassicMinimapCapturedObjectState::ParsedEmpty
        && assets.CapturedMembers()[1].state == ClassicMinimapCapturedObjectState::ParsedEmpty, "真实16/24字节DOO应证明ParsedEmpty");
    Require(assets.CapturedMembers()[0].sourceBytes == 16 && assets.CapturedMembers()[1].sourceBytes == 24
        && assets.CapturedMembers()[0].parserFullyConsumed && assets.CapturedMembers()[1].parserFullyConsumed, "真实空表事实必须保留原字节和EOF证明");
    auto candidate = Take(BuildClassicMinimapCandidate(assets, FlatPostprocess()));
    Require(candidate.raw.width == 8 && candidate.raw.height == 8 && std::to_integer<unsigned>(candidate.raw.rgba[0]) == 80
        && std::to_integer<unsigned>(candidate.raw.rgba[1]) == 120 && std::to_integer<unsigned>(candidate.raw.rgba[2]) == 160
        && std::to_integer<unsigned>(candidate.raw.rgba[3]) == 255, "typed候选必须从真实Terrain/atlas生成预期像素");
}

void TestMissingAndMismatch() {
    Fixture missing({}, {}, {}, {}); auto missingSource = missing.Source("missing.w3x");
    auto assets = Take(CaptureClassicMinimapObjectAssets(CaptureTerrain(missing, missingSource), missingSource, missing.edits));
    Require(assets.CapturedMembers()[0].state == ClassicMinimapCapturedObjectState::Missing
        && assets.CapturedMembers()[1].state == ClassicMinimapCapturedObjectState::Missing, "Missing/Missing必须保留缺失事实");
    Fixture missingUnits({}, MapDataFixtures::EmptyDoodadsV8Sub11(), {}, MapDataFixtures::EmptyDoodadsV8Sub11());
    auto missingUnitsSource = missingUnits.Source("missing-units.w3x");
    auto missingUnitsAssets = Take(CaptureClassicMinimapObjectAssets(CaptureTerrain(missingUnits, missingUnitsSource), missingUnitsSource, missingUnits.edits));
    Require(missingUnitsAssets.CapturedMembers()[0].state == ClassicMinimapCapturedObjectState::Missing
        && missingUnitsAssets.CapturedMembers()[1].state == ClassicMinimapCapturedObjectState::ParsedEmpty, "Missing/ParsedEmpty必须逐成员保留");
    Fixture missingDoodads(MapDataFixtures::EmptyUnitsV8Sub11(), {}, MapDataFixtures::EmptyUnitsV8Sub11(), {});
    auto missingDoodadsSource = missingDoodads.Source("missing-doodads.w3x");
    auto missingDoodadsAssets = Take(CaptureClassicMinimapObjectAssets(CaptureTerrain(missingDoodads, missingDoodadsSource), missingDoodadsSource, missingDoodads.edits));
    Require(missingDoodadsAssets.CapturedMembers()[0].state == ClassicMinimapCapturedObjectState::ParsedEmpty
        && missingDoodadsAssets.CapturedMembers()[1].state == ClassicMinimapCapturedObjectState::Missing, "ParsedEmpty/Missing必须逐成员保留");
    Fixture sourceOnly({}, MapDataFixtures::EmptyDoodadsV8Sub11(), {}, {});
    auto sourceOnlyArchive = sourceOnly.Source("source-only.w3x");
    auto sourceOnlyAssets = Take(CaptureClassicMinimapObjectAssets(CaptureTerrain(sourceOnly, sourceOnlyArchive), sourceOnlyArchive, sourceOnly.edits));
    Require(sourceOnlyAssets.CapturedMembers()[1].sourcePresent && !sourceOnlyAssets.CapturedMembers()[1].currentPresent,
        "显式删除必须保留源Present/当前Missing，而不是把源当成空");
    Require(!VerifyClassicMinimapObjectAssetsFrozenOutput(sourceOnlyAssets, sourceOnlyArchive, {}).Succeeded(),
        "当前Missing但最终没有删除DOO必须拒绝");
    const std::array<std::string, 1> deletedDoo{"war3map.doo"};
    Require(VerifyClassicMinimapObjectAssetsFrozenOutput(sourceOnlyAssets, sourceOnlyArchive, {}, deletedDoo).Succeeded(),
        "当前Missing与最终删除闭环后允许保存");
    Fixture currentOnly({}, {}, MapDataFixtures::EmptyUnitsV8Sub11(), MapDataFixtures::EmptyDoodadsV8Sub11());
    auto currentOnlyArchive = currentOnly.Source("current-only.w3x");
    auto currentOnlyAssets = Take(CaptureClassicMinimapObjectAssets(CaptureTerrain(currentOnly, currentOnlyArchive), currentOnlyArchive, currentOnly.edits));
    Require(!currentOnlyAssets.CapturedMembers()[0].sourcePresent && currentOnlyAssets.CapturedMembers()[0].currentPresent,
        "新增合法空单位表必须保留两侧不同存在性");
    Require(!VerifyClassicMinimapObjectAssetsFrozenOutput(currentOnlyAssets, currentOnlyArchive, {}).Succeeded(),
        "新增当前成员没有进入最终replacement时必须拒绝");
    const std::array<MemberReplacement, 2> createdDoo{{{"war3mapUnits.doo", MapDataFixtures::EmptyUnitsV8Sub11()},
        {"war3map.doo", MapDataFixtures::EmptyDoodadsV8Sub11()}}};
    Require(VerifyClassicMinimapObjectAssetsFrozenOutput(currentOnlyAssets, currentOnlyArchive, createdDoo).Succeeded(),
        "新增当前成员与实际最终replacement闭环允许保存");
    auto v7 = MapDataFixtures::EmptyUnitsV8Sub11(); Put32(v7, 4, 7); Put32(v7, 8, 9);
    Fixture bytes(MapDataFixtures::EmptyUnitsV8Sub11(), MapDataFixtures::EmptyDoodadsV8Sub11(), v7, MapDataFixtures::EmptyDoodadsV8Sub11());
    auto bytesSource = bytes.Source("bytes-mismatch.w3x");
    auto bytesAssets = Take(CaptureClassicMinimapObjectAssets(CaptureTerrain(bytes, bytesSource), bytesSource, bytes.edits));
    Require(bytesAssets.CapturedMembers()[0].sourceSha256 != bytesAssets.CapturedMembers()[0].currentSha256,
        "合法classic布局编辑必须独立保存源SHA与当前SHA");
    Require(!VerifyClassicMinimapObjectAssetsFrozenOutput(bytesAssets, bytesSource, {}).Succeeded(),
        "当前布局编辑未写到最终DOO必须拒绝");
    const std::array<MemberReplacement, 1> changedUnits{{{"war3mapUnits.doo", v7}}};
    Require(VerifyClassicMinimapObjectAssetsFrozenOutput(bytesAssets, bytesSource, changedUnits).Succeeded(),
        "合法当前布局与最终replacement相符后允许");
    Fixture unlisted; auto unlistedSource = unlisted.Source("unlisted.w3x", true);
    auto unlistedAssets = Take(CaptureClassicMinimapObjectAssets(CaptureTerrain(unlisted, unlistedSource), unlistedSource, unlisted.edits));
    Require(unlistedAssets.CapturedMembers()[0].state == ClassicMinimapCapturedObjectState::ParsedEmpty
        && unlistedAssets.CapturedMembers()[1].state == ClassicMinimapCapturedObjectState::ParsedEmpty
        && unlistedSource.HasOpaqueMembers() && !unlistedSource.HasMember("war3mapUnits.doo")
        && !unlistedSource.HasMember("war3map.doo"), "准确读取不能依赖listfile列名");
}

void TestZeroNonzeroStaleAndBudget() {
    Fixture zero(Bytes{}, MapDataFixtures::EmptyDoodadsV8Sub11(), {}, MapDataFixtures::EmptyDoodadsV8Sub11());
    auto zeroSource = zero.Source("zero-present.w3x", false, true);
    Rejected(CaptureClassicMinimapObjectAssets(CaptureTerrain(zero, zeroSource), zeroSource, zero.edits), "CLASSIC_OBJECT_CAPTURE_ZERO_BYTES");
    auto nonzero = MapDataFixtures::EmptyUnitsV8Sub11(); Put32(nonzero, 12, 1);
    Fixture count(nonzero, MapDataFixtures::EmptyDoodadsV8Sub11(), nonzero, MapDataFixtures::EmptyDoodadsV8Sub11()); auto countSource = count.Source("count.w3x");
    Rejected(CaptureClassicMinimapObjectAssets(CaptureTerrain(count, countSource), countSource, count.edits), "PLACEMENT_TRUNCATED");
    auto special = MapDataFixtures::EmptyDoodadsV8Sub11(); Put32(special, 20, 1);
    Fixture specialCount(MapDataFixtures::EmptyUnitsV8Sub11(), special, MapDataFixtures::EmptyUnitsV8Sub11(), special);
    auto specialSource = specialCount.Source("special-count.w3x");
    Rejected(CaptureClassicMinimapObjectAssets(CaptureTerrain(specialCount, specialSource), specialSource, specialCount.edits), "PLACEMENT_TRUNCATED");
    Fixture stale; auto staleSource = stale.Source("stale.w3x"); auto terrain = CaptureTerrain(stale, staleSource);
    TerrainBrush brush; brush.kind = TerrainBrushKind::Raise; brush.radius = 0; brush.strength = 1;
    const TerrainStrokePoint point{256, 256};
    Require(stale.edits.EditTerrain(brush, std::span(&point, 1), false).Succeeded(), "合法地形编辑建立stale revision");
    Rejected(CaptureClassicMinimapObjectAssets(std::move(terrain), staleSource, stale.edits), "CLASSIC_OBJECT_CAPTURE_STALE");
    Fixture budget; auto budgetSource = budget.Source("budget.w3x"); auto budgetTerrain = CaptureTerrain(budget, budgetSource);
    ClassicMinimapObjectAssetLimits limits; limits.maxWorkingBytes = 1;
    Rejected(CaptureClassicMinimapObjectAssets(std::move(budgetTerrain), budgetSource, budget.edits, limits), "CLASSIC_OBJECT_CAPTURE_BUDGET");
    Fixture snapshotBudget(MapDataFixtures::EmptyUnitsV8Sub11(), MapDataFixtures::EmptyDoodadsV8Sub11(),
        MapDataFixtures::EmptyUnitsV8Sub11(), MapDataFixtures::EmptyDoodadsV8Sub11(), MapDataFixtures::FullMapSettingsV25().bytes);
    auto snapshotSource = snapshotBudget.Source("snapshot-budget.w3x");
    auto snapshotTerrain = CaptureTerrain(snapshotBudget, snapshotSource);
    limits = {}; limits.maxCurrentSnapshotBytes = 16;
    Rejected(CaptureClassicMinimapObjectAssets(std::move(snapshotTerrain), snapshotSource, snapshotBudget.edits, limits), "CLASSIC_OBJECT_CAPTURE_BUDGET");
    Fixture memberBudget; auto memberSource = memberBudget.Source("member-budget.w3x");
    auto memberTerrain = CaptureTerrain(memberBudget, memberSource);
    limits = {}; limits.maxObjectMemberBytes = 15;
    Rejected(CaptureClassicMinimapObjectAssets(std::move(memberTerrain), memberSource, memberBudget.edits, limits));
}

void TestMovedFrom() {
    Fixture fixture; auto source = fixture.Source("moved.w3x");
    auto assets = Take(CaptureClassicMinimapObjectAssets(CaptureTerrain(fixture, source), source, fixture.edits));
    ClassicMinimapObjectAssets moved(std::move(assets));
    Rejected(BuildClassicMinimapCandidate(assets, FlatPostprocess()), "CLASSIC_MINIMAP_OBJECT_CAPTURE_STALE");
    const auto candidate = Take(BuildClassicMinimapCandidate(moved, FlatPostprocess()));
    Require(candidate.raw.width == 8 && candidate.raw.height == 8, "move目标仍可消费sealed捕获");
}

void TestMeasuredLargeNestedUnitsAndNewCurrentMember() {
    UnitPlacements placements;
    placements.units.resize(4096);
    for (std::size_t i = 0; i < placements.units.size(); ++i) {
        auto& unit = placements.units[i]; unit.rawcode = "hfoo"; unit.scale = {1, 1, 1};
        unit.creationId = static_cast<std::int32_t>(i);
        unit.itemSets.resize(5);
        for (auto& set : unit.itemSets) set.items.assign(8, PlacementItemChoice{"I000", 100});
        unit.inventory.assign(3, PlacementInventoryItem{0, "I000"});
        unit.abilities.assign(3, PlacementAbility{"A000", false, 1});
    }
    const auto bytes = Take(placements.Serialize());
    Require(bytes.size() > 64u * 1024u, "真实规模原创样例必须超过旧64KiB对象上限");
    Fixture fixture({}, {}, bytes, {});
    auto source = fixture.Source("large-new-units.w3x");
    Require(Take(fixture.edits.SnapshotByteSize(MapDataKind::Units)) == bytes.size(), "wire计量与唯一当前成员一致且无正文副本");
    const auto bounded = fixture.edits.Snapshot(MapDataKind::Units, bytes.size() - 1);
    Require(!bounded.Succeeded(), "Snapshot必须复用相同wire门而不是忽略收紧预算");
    auto assets = Take(CaptureClassicMinimapObjectAssets(CaptureTerrain(fixture, source), source, fixture.edits));
    const auto& fact = assets.CapturedMembers()[0];
    Require(!fact.sourcePresent && fact.currentPresent && fact.sourceSha256.empty() && fact.currentSha256.size() == 64
        && fact.state == ClassicMinimapCapturedObjectState::Parsed && assets.Units().units.size() == 4096,
        "源Missing和当前非空Parsed必须分别有身份，不能把当前domain写成空SHA");
    const auto candidate = Take(BuildClassicMinimapCandidate(assets, FlatPostprocess()));
    Require(candidate.raw.width == 8 && candidate.raw.height == 8, "单位不属于0x08层但其存在不得阻止地图生成");
    Require(!VerifyClassicMinimapObjectAssetsFrozenOutput(assets, source, {}).Succeeded(), "新增大单位层缺最终replacement必须拒绝");
    const std::array<MemberReplacement, 1> replacement{{{"war3mapUnits.doo", bytes}}};
    Require(VerifyClassicMinimapObjectAssetsFrozenOutput(assets, source, replacement).Succeeded(), "真实当前大单位原字进入最终列表后允许");
    ClassicMinimapObjectAssetLimits limit; limit.maxNestedRecords = 1;
    Rejected(CaptureClassicMinimapObjectAssets(CaptureTerrain(fixture, source), source, fixture.edits, limit), "PLACEMENT_RECORD_LIMIT");
}

int main() {
    try {
        TestRealParsedEmptyAndTypedPixels(); TestMissingAndMismatch(); TestZeroNonzeroStaleAndBudget(); TestMovedFrom();
        TestMeasuredLargeNestedUnitsAndNewCurrentMember();
        std::cout << "ClassicMinimapObjectCaptureTests: real capture scenarios declared\n"; return 0;
    } catch (const std::exception& error) {
        std::cerr << "ClassicMinimapObjectCaptureTests: " << error.what() << '\n'; return 1;
    }
}
