#include "War3MapCore/Services/TerrainMinimapMarkers.h"

#include "War3MapCore/IO/FileBytes.h"
#include "War3MapCore/MapData/MapPlacements.h"
#include "War3MapCore/MapData/TerrainEditing.h"
#include "War3MapCore/ObjectData/NativeObjectCodec.h"

#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <iostream>
#include <stdexcept>

namespace {
using namespace War3Map;
using namespace War3Map::MapData;
using namespace War3Map::ObjectData;
using namespace War3Map::Services;
using Bytes = std::vector<std::byte>;
void Require(bool condition, const char* reason) { if (!condition) throw std::runtime_error(reason); }
template<class T> T Take(Result<T> result) {
    if (!result.Succeeded() || !result.value) {
        std::string reason = "原创MMP实际捕获失败";
        for (const auto& diagnostic : result.diagnostics) reason += " | " + diagnostic.code + ":" + diagnostic.message;
        throw std::runtime_error(reason);
    }
    return std::move(*result.value);
}
void Check(Status status) {
    if (status.Succeeded()) return;
    std::string reason;
    for (const auto& diagnostic : status.diagnostics) reason += " | " + diagnostic.code + ":" + diagnostic.message;
    throw std::runtime_error(reason);
}
void Code(std::span<const Diagnostic> diagnostics, std::string_view code) {
    Require(!diagnostics.empty(), "拒绝必须有明确诊断");
    if (code.empty() || std::any_of(diagnostics.begin(), diagnostics.end(), [&](const auto& d) { return d.code == code; })) return;
    std::string reason = "未命中预期门：" + std::string(code);
    for (const auto& diagnostic : diagnostics) reason += " | " + diagnostic.code + ":" + diagnostic.message;
    throw std::runtime_error(reason);
}
template<class T> void Rejected(const Result<T>& result, std::string_view code = {}) {
    Require(!result.Succeeded() && !result.value, "失败不得返回部分记录owner"); Code(result.diagnostics, code);
}
void Rejected(const Status& result, std::string_view code = {}) {
    Require(!result.Succeeded(), "过期或错误输出必须拒绝"); Code(result.diagnostics, code);
}
Bytes Text(std::string_view text) {
    Bytes bytes; for (const auto ch : text) bytes.push_back(static_cast<std::byte>(ch)); return bytes;
}
Bytes Slk(const std::vector<std::string>& columns, const std::vector<std::vector<std::string>>& rows) {
    std::string text = "ID;PWXL;N;E\n";
    const auto append = [&](const auto& cells, std::size_t y) {
        for (std::size_t x = 0; x < cells.size(); ++x)
            text += "C;X" + std::to_string(x + 1) + ";Y" + std::to_string(y) + ";K\"" + cells[x] + "\"\n";
    };
    append(columns, 1); for (std::size_t i = 0; i < rows.size(); ++i) append(rows[i], i + 2);
    return Text(text + "E\n");
}
Bytes TeamTga(std::uint8_t redDelta = 0) {
    Bytes bytes(18); bytes[2] = std::byte{2}; bytes[12] = bytes[14] = std::byte{2};
    bytes[16] = std::byte{24}; bytes[17] = std::byte{0x20};
    // 独立四像素RGB整图均色=(25,35,45)，故BGRA=(45,35,25,255)。
    for (const auto red : {10, 20, 30, 41}) {
        bytes.push_back(static_cast<std::byte>(red + 20)); bytes.push_back(static_cast<std::byte>(red + 10));
        bytes.push_back(static_cast<std::byte>(red + redDelta));
    }
    return bytes;
}
UnitPlacement Unit(std::string code, std::int32_t owner, float x, float y, std::int32_t creation) {
    UnitPlacement unit; unit.rawcode = std::move(code); unit.owner = owner; unit.position = {x, y, 999};
    unit.scale = {1, 1, 1}; unit.creationId = creation; unit.flags = 0; unit.teamColor = 9;
    return unit;
}
struct Fixture {
    std::filesystem::path root, game;
    std::vector<MapArchiveMember> members;
    std::unique_ptr<MapArchiveSession> source;
    UnitPlacements units;
    TerrainGridSnapshot terrain;
    unsigned sequence = 0;
    Fixture() {
        static std::atomic<unsigned> next{0};
        root = std::filesystem::temp_directory_path() / ("Audastra-native-mmp-" + std::to_string(GetCurrentProcessId())
            + "-" + std::to_string(GetTickCount64()) + "-" + std::to_string(next++));
        Require(std::filesystem::create_directory(root), "创建原创专用目录");
        game = root / "game"; Require(std::filesystem::create_directory(game), "创建原创game包目录");
        terrain.header.width = 17; terrain.header.height = 9; terrain.header.mainTileset = 'L';
        terrain.header.offset = {-128, 256}; terrain.header.groundTilesets = {"G001"}; terrain.header.cliffTilesets = {"C001"};
        terrain.vertices.resize(17 * 9);
        for (auto& vertex : terrain.vertices) { vertex.groundHeightRaw = 8192; vertex.cliffAndLayerRaw = 2; }
        RefreshTerrainDerived(terrain, 0, 0, 17, 9);
        MapSettings settings; settings.formatVersion = 25; settings.tileset = 'L'; settings.custom.emplace();
        settings.flags = 0x400; settings.playableWidth = 16; settings.playableHeight = 8;
        units.units = {Unit("sloc", 3, 128, 512, 1), Unit("S001", 15, 896, 768, 2),
            Unit("M001", 0, -128, 256, 3), Unit("P001", 15, 0, 0, 4), Unit("S001", 0, 0, 0, 5),
            Unit("N001", 15, 0, 0, 6), Unit("R001", 15, 0, 0, 7), Unit("sloc", 3, 4096, -128, 8)};
        const auto empty = Take(NativeObjectDocument::CreateEmpty(ObjectType::Unit));
        members = {{"war3map.w3e", Take(SerializeMapTerrain(terrain))}, {"war3map.w3i", Take(settings.Serialize())},
            {"war3mapUnits.doo", Take(units.Serialize())},
            {"Units/UnitData.slk", Slk({"unitID", "race"}, {{"S001", "creeps"}, {"M001", "human"},
                {"P001", "other"}, {"N001", "naga"}, {"R001", "unrecognized"}})},
            {"Units/UnitBalance.slk", Slk({"unitBalanceID", "isbldg"}, {{"S001", "1"}, {"M001", "1"},
                {"P001", "0"}, {"N001", "1"}, {"R001", "1"}})},
            {"Units/UnitUI.slk", Slk({"unitUIID", "nbmmIcon"}, {{"S001", "1"}, {"M001", "0"},
                {"P001", "1"}, {"N001", "0"}, {"R001", "1"}})},
            {"Units/UnitAbilities.slk", Slk({"unitAbilID", "abilList"}, {{"S001", "_"}, {"M001", "_ Agld\t"},
                {"P001", "_"}, {"N001", "_"}, {"R001", "_"}})},
            {"Units/AbilityData.slk", Slk({"alias", "code"}, {{"Ab01", "Agld"}, {"Ab02", "Adef"}})},
            {"Units/ItemData.slk", Slk({"itemID", "code"}, {{"It01", "It01"}})},
            {"Units/UnitMetaData.slk", Slk({"ID", "field", "slk", "type", "repeat", "data", "index"},
                {{"p001", "isbldg", "UnitBalance", "bool", "0", "0", "-1"},
                 {"p002", "race", "UnitData", "unitRace", "0", "0", "-1"},
                 {"p003", "nbmmIcon", "UnitUI", "bool", "0", "0", "-1"},
                 {"p004", "abilList", "UnitAbilities", "abilityList", "0", "0", "-1"}})},
            {"war3map.w3u", Take(NativeObjectCodec(ObjectType::Unit).Serialize(empty))},
            {"Fixture/TeamColor03.tga", TeamTga()}, {"Fixture/TeamColor03.blp", Text("must not read after valid TGA")},
            {"UI/war3skins.txt", Text("[Default]\nTeamColor=DoNotUse/Team\n")},
            {"UI/UnitEditorData.txt", Text("[unitRace]\n0=human,Human\n1=creeps,Creeps\n")},
            {"war3map.shd", Text("unrelated authored shadow")}, {"war3mapMap.tga", Text("authored minimap remains")},
            {"war3map.j", Text("// untouched original fixture script\n")}};
        const std::vector<MapArchiveMember> startup{{"UI/war3skins.txt", Text("[Main]\nSkins=Human\n[Default]\nTeamColor=Fixture/TeamColor\n")}};
        Take(WriteVerifiedArchive(game / "war3.mpq", startup)); Open();
    }
    ~Fixture() { source.reset(); std::error_code error; std::filesystem::remove_all(root, error); }
    Bytes& Member(std::string_view name) {
        const auto found = std::find_if(members.begin(), members.end(), [&](const auto& m) { return m.name == name; });
        Require(found != members.end(), "原创成员存在"); return found->data;
    }
    void Open() {
        source = std::make_unique<MapArchiveSession>(Take(MapArchiveSession::Open(
            Take(WriteVerifiedArchive(root / ("source-" + std::to_string(sequence++) + ".w3x"), members)).path)));
    }
    void UpdateUnits() { Member("war3mapUnits.doo") = Take(units.Serialize()); Open(); }
    std::shared_ptr<const TerrainMinimapMarkers> Capture(std::span<const MemberReplacement> output = {}) {
        return Take(TerrainMinimapMarkers::Capture(*source, game, output));
    }
};
void ExactOrderCoordinatesAndReadback() {
    Fixture fixture;
    const auto owner = fixture.Capture();
    const auto markers = Take(ParseMapMinimapMarkers(owner->Bytes()));
    Require(owner->MarkerCount() == 4 && markers.records.size() == 4, "只含矿、商店、开始位置；无camp或重复去除");
    const std::array<std::int32_t, 4> types{1, 0, 2, 2};
    const std::array<std::array<std::int32_t, 2>, 4> coordinates{{{128, 128}, {0, 192}, {32, 160}, {528, 240}}};
    for (std::size_t i = 0; i < types.size(); ++i) {
        Require(markers.records[i].typeRaw == types[i], "先普通单位原顺序，再开始位置原顺序");
        Require(markers.records[i].positionRaw == coordinates[i], "非方图与offset采用真实可玩区，域外不夹边");
        const auto color = i < 2 ? std::array<std::uint8_t, 4>{255, 255, 255, 255}
            : std::array<std::uint8_t, 4>{45, 35, 25, 255};
        Require(markers.records[i].colorBgra == color, "矿商店白色，sloc使用实例owner的实际整图均色");
    }
    Require(std::any_of(owner->Dependencies().begin(), owner->Dependencies().end(), [](const auto& dependency) {
        return dependency.name == "startup:UI\\WAR3SKINS.TXT" && dependency.source == TerrainMinimapMarkerSource::Game
            && dependency.gameContainer == "war3.mpq" && !dependency.sha256.empty();
    }), "启动皮肤身份来自真实game包，不被地图同名文件冒充");
    std::vector<MemberReplacement> output{{"war3map.mmp", owner->Bytes()}};
    Check(owner->VerifyFrozenOutput(*fixture.source, output));
    const auto saved = Take(fixture.source->SaveCandidate(fixture.root / "candidate.w3x", output));
    const auto readback = Take(MapArchiveSession::Open(saved.path)); Check(owner->VerifyReadback(readback));
    Require(Take(readback.ReadMember("war3map.j")) == fixture.Member("war3map.j"), "脚本不参与或被改写");
    Require(Take(readback.ReadMember("war3mapMap.tga")) == fixture.Member("war3mapMap.tga"), "作者TGA小地图不受MMP重建影响");
    output.front().data.back() ^= std::byte{1};
    Rejected(owner->VerifyFrozenOutput(*fixture.source, output), "TERRAIN_MMP_OUTPUT_MISMATCH");
    Rejected(owner->VerifyFrozenOutput(*fixture.source, {}), "TERRAIN_MMP_OUTPUT_MISMATCH");
    const std::vector<std::string> removed{"war3map.mmp"};
    Rejected(owner->VerifyFrozenOutput(*fixture.source, output, removed), "TERRAIN_MMP_OUTPUT_REMOVED");
}
void FinalOverlayAndDrift() {
    Fixture fixture;
    const auto owner = fixture.Capture();
    auto changed = fixture.units; changed.units[2].position[0] += 128;
    std::vector<MemberReplacement> output{{"war3mapUnits.doo", Take(changed.Serialize())}};
    const auto moved = fixture.Capture(output);
    Require(moved->Bytes() != owner->Bytes(), "实际最终单位XY影响MMP，不能从源DOO代生成");
    output.push_back({"war3map.mmp", owner->Bytes()});
    Rejected(owner->VerifyFrozenOutput(*fixture.source, output), "TERRAIN_MMP_DEPENDENCY_STALE");
    output.back().data = moved->Bytes(); Check(moved->VerifyFrozenOutput(*fixture.source, output));
    const auto saved = Take(fixture.source->SaveCandidate(fixture.root / "moved.w3x", output));
    const auto readback = Take(MapArchiveSession::Open(saved.path)); Check(moved->VerifyReadback(readback));
    Rejected(owner->VerifyReadback(readback), "TERRAIN_MMP_READBACK_MISMATCH");
    output.back().data = owner->Bytes();
    const auto forged = Take(fixture.source->SaveCandidate(fixture.root / "forged.w3x", output));
    Rejected(owner->VerifyReadback(Take(MapArchiveSession::Open(forged.path))), "TERRAIN_MMP_READBACK_STALE");
    auto changedTerrain = fixture.terrain; changedTerrain.vertices[0].groundHeightRaw += 4;
    RefreshTerrainDerived(changedTerrain, 0, 0, 17, 9);
    std::vector<MemberReplacement> geometry{{"war3map.w3e", Take(SerializeMapTerrain(changedTerrain))},
        {"war3map.shd", Text("changed unrelated shadow")}};
    const auto same = fixture.Capture(geometry);
    Require(same->Bytes() == owner->Bytes(), "MMP坐标不读取地形Z或SHD，依赖身份仍绑定实际W3E");
    std::vector<MemberReplacement> recolor{{"Fixture/TeamColor03.tga", TeamTga(20)}};
    Require(fixture.Capture(recolor)->Bytes() != owner->Bytes(), "最终地图贴图覆盖确实影响开始位置颜色");
    recolor.push_back({"war3map.mmp", owner->Bytes()});
    Rejected(owner->VerifyFrozenOutput(*fixture.source, recolor), "TERRAIN_MMP_DEPENDENCY_STALE");
    fixture.Member("war3map.j").push_back(std::byte{'x'}); fixture.Open();
    Rejected(owner->VerifyFrozenOutput(*fixture.source, output), "TERRAIN_MMP_SOURCE_STALE");
}
Bytes UnitOverride(NativeValueType wire, Value value, bool custom = false) {
    auto document = Take(NativeObjectDocument::CreateEmpty(ObjectType::Unit));
    if (custom) Check(document.AddCustomObject("New1", "S001")); else Check(document.AddOriginalObject("S001"));
    Check(document.AddModification({ObjectType::Unit, custom ? "New1" : "S001"}, {"p001"}, wire, std::move(value)));
    return Take(NativeObjectCodec(ObjectType::Unit).Serialize(document));
}
void RealMetadataAndAbilityInheritance() {
    Fixture fixture;
    std::vector<MemberReplacement> output{{"war3map.w3u", UnitOverride(NativeValueType::Int32, std::int32_t{0})}};
    Require(fixture.Capture(output)->MarkerCount() == 3, "实际单位覆盖isbldg=0移除商店图标");
    auto race = Take(NativeObjectDocument::CreateEmpty(ObjectType::Unit));
    Check(race.AddOriginalObject("S001"));
    Check(race.AddModification({ObjectType::Unit, "S001"}, {"p002"}, NativeValueType::String, std::string{"human"}));
    const std::vector<MemberReplacement> raceOutput{{"war3map.w3u", Take(NativeObjectCodec(ObjectType::Unit).Serialize(race))}};
    Require(fixture.Capture(raceOutput)->MarkerCount() == 3, "unitRace实际枚举定义证明String存储，不能用caller值猜类型");
    output.front().data = UnitOverride(NativeValueType::Real, 1.0f);
    Rejected(TerrainMinimapMarkers::Capture(*fixture.source, fixture.game, output), "TERRAIN_MMP_OBJECT_TYPE");
    fixture.units.units[1].rawcode = "New1"; fixture.UpdateUnits();
    output.front().data = UnitOverride(NativeValueType::Int32, std::int32_t{1}, true);
    Require(fixture.Capture(output)->MarkerCount() == 4, "自定义单位从实际标准父读取race/nbmmIcon/能力");
    output.push_back({"Units/UnitMetaData.slk", Slk({"ID", "field", "slk", "type", "repeat", "data", "index"},
        {{"p001", "isbldg", "UnitBalance", "bool", "0", "0", "0"}})});
    Rejected(TerrainMinimapMarkers::Capture(*fixture.source, fixture.game, output), "TERRAIN_MMP_METADATA");
    fixture.units.units[1].rawcode = "S001"; fixture.UpdateUnits();
    auto abilities = Take(NativeObjectDocument::CreateEmpty(ObjectType::Ability));
    Check(abilities.AddCustomObject("As01", "Ab01"));
    output = {{"war3map.w3a", Take(NativeObjectCodec(ObjectType::Ability).Serialize(abilities))},
        {"Units/UnitAbilities.slk", Slk({"unitAbilID", "abilList"}, {{"S001", "_"}, {"M001", "Ab02-As01"},
            {"P001", "_"}, {"N001", "_"}, {"R001", "_"}})}};
    Require(fixture.Capture(output)->MarkerCount() == 4, "真实分隔字符集及自定义能力标准父code能识别矿能力");
    fixture.units.units.push_back(Unit("It01", 15, 0, 0, 9)); fixture.UpdateUnits();
    Require(fixture.Capture(output)->MarkerCount() == 4, "单位DOO中的标准物品不冒充建筑");
    fixture.units.units.back().rawcode = "Who?"; fixture.UpdateUnits();
    Rejected(TerrainMinimapMarkers::Capture(*fixture.source, fixture.game, output), "TERRAIN_MMP_OBJECT_UNKNOWN");
}
void MissingSourcesBudgetsAndGameWinner() {
    Fixture fixture;
    auto limits = TerrainMinimapMarkerLimits{}; limits.markers.maxBytes = 72;
    Require(Take(TerrainMinimapMarkers::Capture(*fixture.source, fixture.game, {}, {}, limits))->MarkerCount() == 4,
        "MMP精确wire预算通过");
    --limits.markers.maxBytes;
    Rejected(TerrainMinimapMarkers::Capture(*fixture.source, fixture.game, {}, {}, limits), "TERRAIN_MMP_BUDGET");
    limits = {}; limits.maximumUnits = 7;
    Rejected(TerrainMinimapMarkers::Capture(*fixture.source, fixture.game, {}, {}, limits));
    limits = {}; limits.maximumSourceBytes = 1;
    Rejected(TerrainMinimapMarkers::Capture(*fixture.source, fixture.game, {}, {}, limits));
    limits = {}; limits.maximumWorkingBytes = 1;
    Rejected(TerrainMinimapMarkers::Capture(*fixture.source, fixture.game, {}, {}, limits));
    const std::vector<std::string> removed{"war3map.w3i"};
    Rejected(TerrainMinimapMarkers::Capture(*fixture.source, fixture.game, {}, removed), "TERRAIN_MMP_SOURCE_MISSING");
    const std::vector<MemberReplacement> corrupt{{"Fixture/TeamColor03.tga", Text("not valid TGA")}};
    Rejected(TerrainMinimapMarkers::Capture(*fixture.source, fixture.game, corrupt));
    fixture.units.units[0].owner = 16; fixture.UpdateUnits();
    Rejected(TerrainMinimapMarkers::Capture(*fixture.source, fixture.game), "TERRAIN_MMP_START_OWNER");
    fixture.units.units[0].owner = 3; fixture.UpdateUnits();
    const auto owner = fixture.Capture();
    const std::vector<MapArchiveMember> patch{{"UI/war3skins.txt", Text("[Default]\nTeamColor=Fixture/TeamColor\n")}};
    Take(WriteVerifiedArchive(fixture.game / "war3patch.mpq", patch));
    const std::vector<MemberReplacement> output{{"war3map.mmp", owner->Bytes()}};
    Rejected(owner->VerifyFrozenOutput(*fixture.source, output), "TERRAIN_MMP_DEPENDENCY_STALE");
}
void EmptyUnitLayerDoesNotInventSources() {
    Fixture fixture;
    fixture.units.units.clear(); fixture.UpdateUnits();
    const auto empty = fixture.Capture();
    Require(empty->Bytes().size() == 8 && empty->MarkerCount() == 0, "空单位层仍正式写零记录MMP头");
    Require(empty->Dependencies().size() == 3, "补足边距空单位图只依赖实际W3E/W3I/DOO");
    const std::vector<std::string> removed{"war3mapUnits.doo"};
    const auto missing = Take(TerrainMinimapMarkers::Capture(*fixture.source, {}, {}, removed));
    Require(missing->Bytes() == empty->Bytes(), "准确Missing单位层不同于未知或坏DOO");
    const std::vector<MemberReplacement> bad{{"war3mapUnits.doo", {}}};
    Rejected(TerrainMinimapMarkers::Capture(*fixture.source, fixture.game, bad));
}
}

int main() {
    try {
        ExactOrderCoordinatesAndReadback(); FinalOverlayAndDrift(); RealMetadataAndAbilityInheritance();
        MissingSourcesBudgetsAndGameWinner(); EmptyUnitLayerDoesNotInventSources();
        std::cout << "Terrain MMP actual capture tests passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
