#include "War3MapCore/Services/TerrainMinimapMarkers.h"
#include "War3MapCore/Services/TerrainMinimapMarkersSave.h"

#include "War3MapCore/IO/FileBytes.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/MapData/MapPlacements.h"
#include "War3MapCore/MapData/TerrainSavePlan.h"
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
void Put32(Bytes& bytes, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) bytes.push_back(static_cast<std::byte>((value >> shift) & 255));
}
Bytes Pathing(std::uint32_t width, std::uint32_t height) {
    auto bytes = Text("MP3W"); Put32(bytes, 0); Put32(bytes, width); Put32(bytes, height);
    bytes.resize(16 + static_cast<std::size_t>(width) * height, std::byte{0x82}); return bytes;
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
    void Freeze(MapSaveTask& task, std::string_view target = "adapter-candidate.w3x") const {
        Check(task.FreezeArchive(*source)); task.target = root / target;
    }
    void PrepareOrdinary(MapSaveTask& task) const {
        task.prepareFinalDerived = [this](MapSaveTask& current) {
            std::shared_ptr<const TerrainSavePlan> noPlan;
            const auto checked = PrepareTerrainMinimapMarkers(current, game, nullptr, noPlan);
            Require(!noPlan, "普通MMP保存不能臆造地形计划"); return checked;
        };
    }
    std::unique_ptr<MapDataEditSession> TerrainSession() {
        members.push_back({"war3map.wpm", Pathing(64, 32)}); Open();
        auto edits = std::make_unique<MapDataEditSession>(MapDocument::FromArchive(*source));
        Check(edits->AttachTerrain(Member("war3map.w3e"))); Check(edits->AttachPathing(Member("war3map.wpm")));
        return edits;
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
void HeaderlessCellsKeepNamedFieldSemanticsAndSourceBinding() {
    Fixture fixture;
    const auto expected = fixture.Capture()->Bytes();
    auto sparse = Slk({"unitBalanceID", "unnamed", "isbldg"},
        {{"S001", "FALSE", "1"}, {"M001", "FALSE", "1"}, {"P001", "FALSE", "0"},
         {"N001", "FALSE", "1"}, {"R001", "FALSE", "1"}});
    const auto erase = [&](std::string_view line) {
        const auto bytes = Text(line);
        const auto found = std::search(sparse.begin(), sparse.end(), bytes.begin(), bytes.end());
        Require(found != sparse.end(), "原创稀疏表有待移除的明确表头");
        sparse.erase(found, found + bytes.size());
    };
    erase("C;X2;Y1;K\"unnamed\"\n");
    fixture.Member("Units/UnitBalance.slk") = sparse; fixture.Open();
    const auto owner = fixture.Capture();
    Require(owner->Bytes() == expected, "无标题列不偏移真实isbldg列或改变MMP语义");
    const std::vector<MemberReplacement> output{{"war3map.mmp", owner->Bytes()}};
    Check(owner->VerifyFrozenOutput(*fixture.source, output));
    const auto saved = Take(fixture.source->SaveCandidate(fixture.root / "sparse-table.w3x", output));
    const auto readback = Take(MapArchiveSession::Open(saved.path));
    Check(owner->VerifyReadback(readback));
    Require(Take(readback.ReadMember("Units/UnitBalance.slk")) == sparse,
        "保存不改写或丢弃无标题单元格的原表字节");

    auto changed = sparse;
    const auto value = Text("FALSE");
    const auto found = std::search(changed.begin(), changed.end(), value.begin(), value.end());
    Require(found != changed.end(), "原创无标题列含真实正文");
    *found = std::byte{'T'};
    const std::vector<MemberReplacement> drift{{"Units/UnitBalance.slk", changed}, {"war3map.mmp", owner->Bytes()}};
    Rejected(owner->VerifyFrozenOutput(*fixture.source, drift), "TERRAIN_MMP_DEPENDENCY_STALE");

    erase("C;X3;Y1;K\"isbldg\"\n");
    const std::vector<MemberReplacement> missing{{"Units/UnitBalance.slk", sparse}};
    Rejected(TerrainMinimapMarkers::Capture(*fixture.source, fixture.game, missing), "TERRAIN_MMP_TABLE_COLUMN");

    auto duplicate = Text("ID;PWXL;N;E\nC;X1;Y1;K\"unitBalanceID\"\nC;X3;Y1;K\"isbldg\"\n"
        "C;X1;Y2;K\"S001\"\nC;X2;Y2;KFALSE\nC;X2;Y2;KTRUE\nC;X3;Y2;K1\nE\n");
    const std::vector<MemberReplacement> repeated{{"Units/UnitBalance.slk", std::move(duplicate)}};
    Rejected(TerrainMinimapMarkers::Capture(*fixture.source, fixture.game, repeated));
    auto large = Text("ID;PWXL;N;E\nC;X1;Y1;K\"unitBalanceID\"\nC;X3;Y1;K\"isbldg\"\n"
        "C;X1;Y2;K\"S001\"\nC;X2;Y2;K\"" + std::string(4097, 'x') + "\"\nC;X3;Y2;K1\nE\n");
    const std::vector<MemberReplacement> oversized{{"Units/UnitBalance.slk", std::move(large)}};
    Rejected(TerrainMinimapMarkers::Capture(*fixture.source, fixture.game, oversized), "TERRAIN_MMP_TABLE_COLUMN");
}

void SparseRowsKeepNamedIdsAndSourceBinding() {
    Fixture fixture;
    const auto expected = fixture.Capture()->Bytes();
    const auto sparse = Text("ID;PWXL;N;E\nC;X1;Y1;K\"alias\"\nC;X2;Y1;K\"code\"\n"
        "C;X1;Y2;K\"Ab01\"\nC;X2;Y2;K\"Agld\"\n"
        "C;X29;Y3;K\"-\"\nC;X35;Y3;K\"_\"\nC;X49;Y3;K\"-\"\n"
        "C;X1;Y4;K\"Ab02\"\nC;X2;Y4;K\"Adef\"\nE\n");
    fixture.Member("Units/AbilityData.slk") = sparse; fixture.Open();
    const auto owner = fixture.Capture();
    Require(owner->Bytes() == expected, "无alias占位行不影响具名能力语义");
    const std::vector<MemberReplacement> output{{"war3map.mmp", owner->Bytes()}};
    const auto saved = Take(fixture.source->SaveCandidate(fixture.root / "sparse-rows.w3x", output));
    const auto readback = Take(MapArchiveSession::Open(saved.path));
    Check(owner->VerifyReadback(readback));
    Require(Take(readback.ReadMember("Units/AbilityData.slk")) == sparse, "保存逐字保留稀疏占位行");

    auto changed = sparse;
    const auto needle = Text("C;X35;Y3;K\"_\"");
    const auto found = std::search(changed.begin(), changed.end(), needle.begin(), needle.end());
    Require(found != changed.end(), "原创占位行有可变正文");
    *(found + needle.size() - 2) = std::byte{'-'};
    const std::vector<MemberReplacement> drift{{"Units/AbilityData.slk", changed}, {"war3map.mmp", owner->Bytes()}};
    Rejected(owner->VerifyFrozenOutput(*fixture.source, drift), "TERRAIN_MMP_DEPENDENCY_STALE");

    const auto reject = [&](std::string_view extra, std::string_view code) {
        auto bytes = sparse; bytes.resize(bytes.size() - 2);
        const auto tail = Text(std::string(extra) + "E\n"); bytes.insert(bytes.end(), tail.begin(), tail.end());
        const std::vector<MemberReplacement> overlay{{"Units/AbilityData.slk", std::move(bytes)}};
        Rejected(TerrainMinimapMarkers::Capture(*fixture.source, fixture.game, overlay), code);
    };
    reject("C;X1;Y5;K\"\"\n", "TERRAIN_MMP_TABLE_ID");
    reject("C;X1;Y5;K\"Ab0\"\n", "TERRAIN_MMP_TABLE_ID");
    reject("C;X1;Y5;K\"Ab01\"\n", "TERRAIN_MMP_TABLE_ID");
    reject("C;X2;Y5;K\"" + std::string(4097, 'x') + "\"\n", "TERRAIN_MMP_TABLE_COLUMN");
    reject("C;X2;Y5;K\"-\"\nC;X2;Y5;K\"_\"\n", {});
    fixture.Member("Units/UnitAbilities.slk") = Slk({"unitAbilID", "abilList"}, {{"S001", "Ab03"}});
    fixture.Open();
    Rejected(TerrainMinimapMarkers::Capture(*fixture.source, fixture.game), "TERRAIN_MMP_ABILITY_UNKNOWN");
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
void OrdinarySaveRebuildsMarkersWithoutMapBlpOrTerrainPlan() {
    Fixture fixture;
    MapSaveTask task; fixture.Freeze(task);
    // 普通物编修改没有W3E笔刷，也没有MapBLP生成资格；MMP仍消费最终物编。
    task.replacements.push_back({"war3map.w3u", UnitOverride(NativeValueType::Int32, std::int32_t{0})});
    const auto expected = fixture.Capture(task.replacements)->Bytes();
    fixture.PrepareOrdinary(task); MapSaveService::Run(task);
    if (!task.succeeded) Check({task.diagnostics});
    Require(task.succeeded && task.nextSession && task.minimapMarkers, "普通保存以独立owner写出并回读MMP");
    const auto readback = Take(MapArchiveSession::Open(task.target));
    Require(Take(readback.ReadMember("war3map.mmp")) == expected, "正式通用保存使用最终单位物编生成的完整MMP原字");
    Require(Take(readback.ReadMember("war3map.w3u")) == task.replacements.front().data, "同次普通物编修改正常保存");
    for (const auto* name : {"war3map.w3e", "war3map.shd", "war3map.j", "war3mapMap.tga"})
        Require(Take(readback.ReadMember(name)) == fixture.Member(name), "普通图标重建保留其它作者成员");
    const std::array<std::string, 1> blpName{"war3mapMap.blp"};
    const auto captured = Take(readback.CaptureExactMembers(blpName));
    Require(!captured.front().present, "MMP不能冒充小地图图像生成或新增MapBLP");
    Check(task.minimapMarkers->VerifyReadback(readback));
}
void AutomaticUnsupportedKeepsOriginalMarkersAndSavesOtherEdits() {
    Fixture fixture;
    const auto original = fixture.Capture()->Bytes();
    fixture.members.push_back({"war3map.mmp", original});
    fixture.units.units.push_back(Unit("Who?", 15, 0, 0, 9)); fixture.UpdateUnits();
    MapSaveTask task; fixture.Freeze(task);
    const auto native = UnitOverride(NativeValueType::Int32, std::int32_t{0});
    task.replacements.push_back({"war3map.w3u", native}); fixture.PrepareOrdinary(task);
    MapSaveService::Run(task);
    if (!task.succeeded) Check({task.diagnostics});
    Require(task.succeeded && task.nextSession && !task.minimapMarkers, "未支持对象不应阻断普通物编保存");
    Code(task.diagnostics, "TERRAIN_MMP_PRESERVED_UNSUPPORTED");
    Require(std::none_of(task.diagnostics.begin(), task.diagnostics.end(), [](const auto& diagnostic) {
        return diagnostic.severity == DiagnosticSeverity::Error;
    }), "能力不足只留明确warning，不能偷偷把错误带入成功任务");
    const auto readback = Take(MapArchiveSession::Open(task.target));
    Require(Take(readback.ReadMember("war3map.mmp")) == original, "能力不足必须保留旧MMP完整字节");
    Require(Take(readback.ReadMember("war3map.w3u")) == native, "保留图标不能丢掉本次真实物编修改");
    Require(Take(readback.ReadMember("war3mapMap.tga")) == fixture.Member("war3mapMap.tga"), "能力不足也保留作者图像");
}
void PlanAdmissionUsesCanonicalTerrainAndKeepsRevisionGate() {
    Fixture fixture; auto edits = fixture.TerrainSession();
    const auto baseline = Take(TerrainSavePlan::Prepare(*fixture.source, *edits)); Check(baseline.SaveGate());
    MapSaveTask task; fixture.Freeze(task);
    std::shared_ptr<const TerrainSavePlan> plan = std::make_shared<const TerrainSavePlan>(baseline);
    Check(PrepareTerrainMinimapMarkers(task, fixture.game, edits.get(), plan));
    Require(task.minimapMarkers && plan && plan->Members()[3].action == TerrainSaveMemberAction::WriteEdited,
        "真实capture的canonical W3E允许把MMP安装进同一地形计划");
    Require(plan->Members()[3].outputBytes == task.minimapMarkers->Bytes().size(), "计划记录完整MMP输出大小");
    Check(plan->VerifyForSave(*edits, task.sourceSha256, task.replacements));
    task.beforePublish = [&] { return plan->VerifyFrozenOutput(task.sourceSha256, task.replacements, task.removals); };
    MapSaveService::Run(task);
    if (!task.succeeded) Check({task.diagnostics});
    Require(task.succeeded && task.nextSession, "同一计划与通用保存的双门通过");
    Check(plan->VerifyReadback(*task.nextSession));

    auto stalePlan = baseline;
    auto moved = fixture.units; moved.units[0].position[2] += 1;
    Check(edits->SetSerialized(MapDataKind::Units, Take(moved.Serialize())));
    Rejected(task.minimapMarkers->AdmitToPlan(stalePlan, *edits), "TERRAIN_SAVE_PLAN_STALE");

    Fixture different; auto differentEdits = different.TerrainSession();
    different.Member("war3map.j").push_back(std::byte{'x'}); different.Open();
    auto foreignPlan = Take(TerrainSavePlan::Prepare(*different.source, *differentEdits));
    Rejected(task.minimapMarkers->AdmitToPlan(foreignPlan, *differentEdits), "TERRAIN_MMP_SOURCE_STALE");

    // 只改变W3E高度也仍须匹配plan的最终W3E，不能因图标正文恰好不变而放行。
    auto changedTerrain = fixture.terrain; changedTerrain.vertices[0].groundHeightRaw += 4;
    RefreshTerrainDerived(changedTerrain, 0, 0, 17, 9);
    const std::array finalTerrain{MemberReplacement{"war3map.w3e", Take(SerializeMapTerrain(changedTerrain))}};
    const auto mismatched = fixture.Capture(finalTerrain);
    auto originalPlan = baseline;
    Rejected(mismatched->AdmitToPlan(originalPlan, *edits), "TERRAIN_MMP_DEPENDENCY_STALE");
}
void PlanBudgetPreservesOriginalOutputWithoutPartialInstall() {
    Fixture fixture; auto edits = fixture.TerrainSession();
    // 查出真实Prepare的精确容量界；不把实现私有计数或固定fixture字节数复制到测试。
    std::size_t lower = 0, upper = TerrainSavePlanLimits{}.maximumBytes;
    while (lower < upper) {
        const auto middle = lower + (upper - lower) / 2;
        const auto prepared = TerrainSavePlan::Prepare(*fixture.source, *edits, {middle});
        if (prepared.Succeeded() && prepared.value) upper = middle; else lower = middle + 1;
    }
    auto exact = Take(TerrainSavePlan::Prepare(*fixture.source, *edits, {lower})); Check(exact.SaveGate());
    const auto owner = fixture.Capture();
    Rejected(owner->AdmitToPlan(exact, *edits), "TERRAIN_MMP_BUDGET");
    Require(exact.Members()[3].action == TerrainSaveMemberAction::Preserve && exact.Replacements().empty(),
        "plan预算不足不能留下半份MMP安装");
    MapSaveTask task; fixture.Freeze(task);
    std::shared_ptr<const TerrainSavePlan> plan = std::make_shared<const TerrainSavePlan>(exact);
    const auto originalPlan = plan;
    Check(PrepareTerrainMinimapMarkers(task, fixture.game, edits.get(), plan));
    Require(plan == originalPlan && !task.minimapMarkers && task.replacements.empty(),
        "自动plan容量不足保留原计划、原MMP与最终列表");
    Code(task.diagnostics, "TERRAIN_MMP_PRESERVED_UNSUPPORTED");
    MapSaveService::Run(task);
    if (!task.succeeded) Check({task.diagnostics});
    Require(task.succeeded && task.nextSession, "图标容量不足不阻断其它正常保存");
    Check(plan->VerifyReadback(*task.nextSession));
}
void AdapterConflictsAndFrozenSourceRemainHardFailures() {
    Fixture fixture;
    const auto runConflict = [&](bool removal) {
        MapSaveTask task; fixture.Freeze(task);
        if (removal) task.removals.push_back("WAR3MAP.MMP");
        else task.replacements.push_back({"WaR3MaP.MmP", fixture.Capture()->Bytes()});
        std::shared_ptr<const TerrainSavePlan> noPlan;
        Rejected(PrepareTerrainMinimapMarkers(task, fixture.game, nullptr, noPlan), "TERRAIN_MMP_FINAL_CONFLICT");
        Require(!task.minimapMarkers && !std::filesystem::exists(task.target), "冲突准备不能安装owner或写目标");
    };
    runConflict(false); runConflict(true);
    MapSaveTask twice; fixture.Freeze(twice);
    std::shared_ptr<const TerrainSavePlan> noPlan;
    Check(PrepareTerrainMinimapMarkers(twice, fixture.game, nullptr, noPlan));
    const auto bytes = twice.minimapMarkers->Bytes();
    Rejected(PrepareTerrainMinimapMarkers(twice, fixture.game, nullptr, noPlan), "TERRAIN_MMP_FINAL_CONFLICT");
    Require(twice.replacements.size() == 1 && twice.replacements.front().data == bytes, "重复准备不能覆盖第一次完整输出");
    MapSaveTask stale; fixture.Freeze(stale); stale.sourceSha256[0] ^= 1;
    Rejected(PrepareTerrainMinimapMarkers(stale, fixture.game, nullptr, noPlan), "TERRAIN_MMP_SOURCE_STALE");
    Require(!stale.minimapMarkers && stale.replacements.empty(), "错误来源身份绝不能降级为保留警告");

    MapSaveTask unreadable; fixture.Freeze(unreadable);
    const auto sourceBytes = Take(IO::ReadFileBytes(fixture.source->SourcePath()));
    Check(IO::WriteFileBytes(fixture.source->SourcePath(), Text("changed private source archive")));
    Rejected(PrepareTerrainMinimapMarkers(unreadable, fixture.game, nullptr, noPlan), "SOURCE_CHANGED");
    Require(!unreadable.minimapMarkers && unreadable.replacements.empty()
        && !std::filesystem::exists(unreadable.target), "真实来源读取失败不能当作能力不足写出地图");
    Check(IO::WriteFileBytes(fixture.source->SourcePath(), sourceBytes));
}
void BeforePublishCannotChangeMarkersOrConsumedDependencies() {
    for (unsigned mode = 0; mode < 4; ++mode) {
        Fixture fixture; MapSaveTask task; fixture.Freeze(task); fixture.PrepareOrdinary(task);
        task.beforePublish = [&]() -> Status {
            Require(task.minimapMarkers && task.InFinalDerivedStage() == false,
                "回调必须确实位于派生完成后、归档写出前");
            if (mode == 0) {
                const auto marker = std::find_if(task.replacements.begin(), task.replacements.end(), [](const auto& member) {
                    return member.name == "war3map.mmp";
                });
                Require(marker != task.replacements.end(), "回调修改的必须是已安装的实际MMP");
                marker->data.back() ^= std::byte{1};
            } else if (mode == 1) {
                auto moved = fixture.units; moved.units[1].position[0] += 128;
                task.replacements.push_back({"war3mapUnits.doo", Take(moved.Serialize())});
            } else if (mode == 2) {
                const std::vector<MapArchiveMember> patch{{"UI/war3skins.txt", Text("[Default]\nTeamColor=Fixture/TeamColor\n")}};
                Take(WriteVerifiedArchive(fixture.game / "war3patch.mpq", patch));
            } else {
                Check(IO::WriteFileBytes(fixture.source->SourcePath(), Text("source changed after final capture")));
            }
            return Status::Ok();
        };
        MapSaveService::Run(task);
        Require(task.executed && !task.succeeded && !task.nextSession && task.written.empty()
            && !std::filesystem::exists(task.target), "发布前MMP/依赖/来源被改动必须在写目标之前拒绝");
        Code(task.diagnostics, mode == 0 ? "TERRAIN_MMP_OUTPUT_MISMATCH"
            : mode == 3 ? "SOURCE_CHANGED" : "TERRAIN_MMP_DEPENDENCY_STALE");
        Require(std::none_of(task.diagnostics.begin(), task.diagnostics.end(), [](const auto& diagnostic) {
            return diagnostic.code == "TERRAIN_MMP_PRESERVED_UNSUPPORTED";
        }), "最后来源与输出门不得降级成自动能力警告");
    }
}
}

int main() {
    try {
        ExactOrderCoordinatesAndReadback(); HeaderlessCellsKeepNamedFieldSemanticsAndSourceBinding();
        SparseRowsKeepNamedIdsAndSourceBinding();
        FinalOverlayAndDrift(); RealMetadataAndAbilityInheritance();
        MissingSourcesBudgetsAndGameWinner(); EmptyUnitLayerDoesNotInventSources();
        OrdinarySaveRebuildsMarkersWithoutMapBlpOrTerrainPlan(); AutomaticUnsupportedKeepsOriginalMarkersAndSavesOtherEdits();
        PlanAdmissionUsesCanonicalTerrainAndKeepsRevisionGate(); PlanBudgetPreservesOriginalOutputWithoutPartialInstall();
        AdapterConflictsAndFrozenSourceRemainHardFailures(); BeforePublishCannotChangeMarkersOrConsumedDependencies();
        std::cout << "Terrain MMP actual capture tests passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
