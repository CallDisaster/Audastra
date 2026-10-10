#include "War3MapCore/Script/MapScriptBuild.h"
#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/MapDocument.h"
#include "War3MapCore/MapData/MapPlacements.h"
#include "War3MapCore/MapData/PlacementEditing.h"
#include "War3MapCore/MapData/MapSettings.h"
#include "War3MapCore/Trigger/TriggerDocument.h"
#include "War3MapCore/Trigger/UiCatalog.h"
#include "MapDataFixtures.h"

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numbers>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace War3Map;

void Require(bool value, const char* reason) {
    if (!value) throw std::runtime_error(reason);
}

class Scratch {
public:
    Scratch() {
        path_ = std::filesystem::temp_directory_path() /
            ("forge-map-script-overlay-" + std::to_string(GetCurrentProcessId())
                + "-" + std::to_string(GetTickCount64()));
        Require(std::filesystem::create_directory(path_), "create private synthetic workspace");
        path_ = std::filesystem::canonical(path_);
    }
    ~Scratch() {
        std::error_code error;
        const auto current = std::filesystem::weakly_canonical(path_, error);
        if (!error && current == path_ && path_.filename().wstring().starts_with(L"forge-map-script-overlay-"))
            std::filesystem::remove_all(path_, error);
    }
    const auto& Path() const { return path_; }
private:
    std::filesystem::path path_;
};

void Write(const std::filesystem::path& file, std::string_view text) {
    std::filesystem::create_directories(file.parent_path());
    std::ofstream output(file, std::ios::binary | std::ios::trunc);
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    Require(static_cast<bool>(output), "write synthetic UI catalog");
}

bool HasCode(std::span<const Diagnostic> diagnostics, std::string_view code) {
    for (const auto& diagnostic : diagnostics) if (diagnostic.code == code) return true;
    return false;
}

std::vector<std::byte> FlatTerrain(float offsetX = -8192.0f, float offsetY = -8192.0f) {
    // 原创完整 W3E：129×129 顶点、单一草地/悬崖纹理、平坦且无水。
    MapDataFixtures::Fixture fixture;
    fixture.Raw("W3E!");
    fixture.Int(11);
    fixture.Byte('L');
    fixture.Int(0);
    fixture.Int(1);
    fixture.Raw("Lgrs");
    fixture.Int(1);
    fixture.Raw("CLdi");
    fixture.Int(129);
    fixture.Int(129);
    fixture.Float(offsetX);
    fixture.Float(offsetY);
    for (int vertex = 0; vertex < 129 * 129; ++vertex) {
        fixture.Byte(0); fixture.Byte(0x20); // 高度 8192。
        fixture.Byte(0); fixture.Byte(0x20); // 水位 8192，无水标志。
        fixture.Byte(0); fixture.Byte(0); fixture.Byte(0x02); // 地面纹理 0，悬崖纹理 0，层级 2。
    }
    return std::move(fixture.bytes);
}

void TestOverlayChangesGeneratedScript() {
    Scratch scratch;
    Write(scratch.Path() / "ui/config", "base\n");
    Write(scratch.Path() / "ui/base/define.txt", "[TriggerTypes]\ninteger=0,1,1,整数,integer\n");
    Write(scratch.Path() / "ui/base/event.txt", "[TimerEvent]\n");
    Write(scratch.Path() / "ui/base/condition.txt", "[AndMultiple]\nreturns=boolean\n");
    Write(scratch.Path() / "ui/base/action.txt", "[DoNothing]\n");
    Write(scratch.Path() / "ui/base/call.txt", "[GetLastCreated]\nreturns=unit\n");
    auto catalog = Trigger::UiCatalog::Load(scratch.Path());
    Require(catalog.Succeeded(), "synthetic UI catalog should load");
    const auto settingsBytes = MapDataFixtures::FullMapSettingsV25().bytes;
    auto baseUnits = MapData::ParseUnitPlacements(MapDataFixtures::EmptyUnitsV8Sub11());
    Require(baseUnits.Succeeded(), "empty source units should parse");
    MapData::UnitPlacement originalUnit;
    originalUnit.rawcode = "hfoo";
    originalUnit.owner = 0;
    originalUnit.position = {32.0f, 32.0f, 0.0f};
    originalUnit.scale = {1.0f, 1.0f, 1.0f};
    originalUnit.creationId = 40;
    baseUnits.value->units.push_back(originalUnit);
    const auto serializedBase = baseUnits.value->Serialize();
    Require(serializedBase.Succeeded(), "source unit should serialize");
    const auto unitBytes = *serializedBase.value;
    const std::vector<MapArchiveMember> members{{"war3map.w3i", settingsBytes},
        {"war3mapUnits.doo", unitBytes}, {"war3map.doo", MapDataFixtures::EmptyDoodadsV8Sub11()},
        {"war3map.w3e", FlatTerrain()}};
    const auto sourcePath = scratch.Path() / "source.mpq";
    Require(WriteVerifiedArchive(sourcePath, members).Succeeded(), "write synthetic map archive");
    auto archive = MapArchiveSession::Open(sourcePath);
    Require(archive.Succeeded(), "open synthetic map archive");
    Trigger::TriggerDocument triggers;
    Script::MapScriptBuildRequest request;
    request.archive = &*archive.value;
    request.triggers = &triggers;
    request.catalog = &*catalog.value;
    request.compile = false;
    const auto before = Script::BuildMapScript(request);
    Require(before.Succeeded(), "baseline script should generate");
    auto modern = MapData::MapSettings::Parse(settingsBytes);
    Require(modern.Succeeded(), "classic source should parse before synthetic v31 branch");
    modern.value->formatVersion = 31;
    modern.value->gameVersion = {1, 32, 0, 0};
    modern.value->scriptLanguage = 1;
    modern.value->graphicsModes = 3;
    modern.value->gameDataVersion = 1;
    const auto modernBytes = modern.value->Serialize();
    Require(modernBytes.Succeeded(), "synthetic v31 settings should serialize");
    request.editedMapMembers = {{"war3map.w3i", *modernBytes.value}};
    const auto modernScript = Script::BuildMapScript(request);
    Require(!modernScript.Succeeded() && HasCode(modernScript.diagnostics, "MAP_SCRIPT_FORMAT_UNSUPPORTED")
        && modernScript.FinalScript() == nullptr, "v31 must not fall through the classic JASS save path");
    request.editedMapMembers.clear();
    MapDataFixtures::Fixture extendedSound; extendedSound.Int(2); extendedSound.Int(0);
    request.editedMapMembers = {{"war3map.w3s", extendedSound.bytes}};
    const auto extendedScript = Script::BuildMapScript(request);
    Require(!extendedScript.Succeeded() && extendedScript.FinalScript() == nullptr
        && extendedScript.generated.text.empty()
        && HasCode(extendedScript.generated.diagnostics, "CLASSIC_SOUNDS_LAYOUT_UNSUPPORTED"),
        "readable extended sounds must not fall through script rebuilding");
    auto readableMembers = members; readableMembers.push_back(request.editedMapMembers.front());
    const auto readable = MapDocument::FromMembers(readableMembers);
    Require(readable.Member(MapDataKind::Sounds).state == MapDataState::Partial
        && readable.Sounds()->originalBytes == extendedSound.bytes
        && HasCode(readable.InitializationSourceStatus().diagnostics, "CLASSIC_SOUNDS_LAYOUT_UNSUPPORTED"),
        "read-only diagnostics and bytes remain available while the layout gate identifies the blocker");
    request.editedMapMembers.clear();
    Require(before.generated.text.find("AddWeatherEffect( Rect(-8192.0,-8192.0,8192.0,8192.0), 'RAhr' )")
            != std::string::npos,
        "archive terrain and weather settings must jointly determine the generated global weather");
    auto missingTerrainMembers = members;
    missingTerrainMembers.pop_back();
    const auto missingTerrainPath = scratch.Path() / "without-terrain.mpq";
    Require(WriteVerifiedArchive(missingTerrainPath, missingTerrainMembers).Succeeded(), "write missing-terrain fixture");
    auto missingTerrain = MapArchiveSession::Open(missingTerrainPath);
    Require(missingTerrain.Succeeded(), "open missing-terrain fixture");
    request.archive = &*missingTerrain.value;
    const auto noTerrainScript = Script::BuildMapScript(request);
    Require(!noTerrainScript.Succeeded()
            && HasCode(noTerrainScript.generated.diagnostics, "MAP_WEATHER_TERRAIN_INVALID")
            && noTerrainScript.FinalScript() == nullptr,
        "the build service must refuse a final script when configured global weather has no terrain extent");
    request.archive = &*archive.value;

    auto settings = MapData::MapSettings::Parse(settingsBytes);
    Require(settings.Succeeded(), "settings fixture should parse");
    settings.value->mapName = "Saved-Overlay-Map";
    const auto changedSettings = settings.value->Serialize();
    Require(changedSettings.Succeeded(), "edited settings should serialize");
    auto units = MapData::ParseUnitPlacements(unitBytes);
    Require(units.Succeeded(), "unit fixture should parse");
    MapData::UnitPlacement unit = originalUnit;
    unit.position = {256.0f, 128.0f, 0.0f};
    unit.creationId = 41;
    units.value->units.push_back(unit);
    const auto changedUnits = units.value->Serialize();
    Require(changedUnits.Succeeded(), "edited units should serialize");

    request.editedMapMembers = {{"WAR3MAP.W3I", *changedSettings.value},
        {"war3mapUnits.doo", *changedUnits.value}};
    const auto after = Script::BuildMapScript(request);
    Require(after.Succeeded(), "edited map should generate script");
    Require(after.generated.text != before.generated.text, "edited map must change generated script");
    Require(after.generated.text.find("Saved-Overlay-Map") != std::string::npos,
        "config must use edited map name");
    Require(after.generated.text.find("CreateUnit(") != std::string::npos,
        "main initialization must use edited preplaced unit");
    Require(archive.value->ReadMember("war3map.w3i").value == settingsBytes
        && archive.value->ReadMember("war3mapUnits.doo").value == unitBytes,
        "script generation must not mutate source archive");
    // 独立候选同时包含新地图数据和新脚本；这是离线打包往返，不是游戏可运行验收。
    auto replacements = request.editedMapMembers;
    const auto* scriptBytes = reinterpret_cast<const std::byte*>(after.generated.text.data());
    replacements.push_back({"war3map.j", {scriptBytes, scriptBytes + after.generated.text.size()}});
    const auto candidatePath = scratch.Path() / "candidate.mpq";
    const auto saved = archive.value->SaveCandidate(candidatePath, replacements);
    Require(saved.Succeeded(), "synthetic candidate should publish with all replacement members");
    const auto candidate = MapArchiveSession::Open(candidatePath);
    Require(candidate.Succeeded(), "saved candidate should reopen");
    const auto candidateSettings = candidate.value->ReadMember("war3map.w3i");
    const auto candidateUnits = candidate.value->ReadMember("war3mapUnits.doo");
    const auto candidateScript = candidate.value->ReadMember("war3map.j");
    Require(candidateSettings.Succeeded() && candidateUnits.Succeeded() && candidateScript.Succeeded(),
        "candidate should contain both typed data and generated script");
    Require(MapData::MapSettings::Parse(*candidateSettings.value).value->mapName == "Saved-Overlay-Map"
        && MapData::ParseUnitPlacements(*candidateUnits.value).value->units.size() == 2
        && *candidateScript.value == replacements.back().data,
        "candidate data and initialization script must correspond to the same edit set");
    Require(archive.value->VerifySourceUnchanged().Succeeded(), "candidate roundtrip must leave source archive unchanged");

    // 原生保存宿主提供整个已冻结 job；W3E 初始化也必须使用该集合，不能回读旧地形。
    auto frozenMembers = request.editedMapMembers;
    frozenMembers.push_back({"WAR3MAP.W3E", FlatTerrain(1024.0f, 2048.0f)});
    request.editedMapMembers.clear();
    request.finalSaveMembers = Script::MapScriptFinalMembers{frozenMembers, {}};
    const auto movedTerrain = Script::BuildMapScript(request);
    Require(movedTerrain.Succeeded() && movedTerrain.generated.text.find(
        "AddWeatherEffect( Rect(1024.0,2048.0,17408.0,18432.0), 'RAhr' )") != std::string::npos,
        "weather extent must use final W3E overlay through the frozen save member view");
    frozenMembers.push_back({"war3map.w3e", FlatTerrain()});
    request.finalSaveMembers = Script::MapScriptFinalMembers{frozenMembers, {}};
    Require(HasCode(Script::BuildMapScript(request).diagnostics, "MAP_EDIT_CONFLICT"), "duplicate terrain overlay must reject");
    frozenMembers.pop_back();
    frozenMembers.back().data = {std::byte{0}};
    request.finalSaveMembers = Script::MapScriptFinalMembers{frozenMembers, {}};
    Require(HasCode(Script::BuildMapScript(request).diagnostics, "MAP_TERRAIN_INVALID"), "malformed final terrain must reject");
    frozenMembers.pop_back();
    const std::vector<std::string> terrainRemoval{"war3map.w3e"};
    request.finalSaveMembers = Script::MapScriptFinalMembers{frozenMembers, terrainRemoval};
    Require(HasCode(Script::BuildMapScript(request).generated.diagnostics, "MAP_WEATHER_TERRAIN_INVALID"),
        "removed final terrain must not fall back to source extent");
    request.editedMapMembers = frozenMembers;
    Require(HasCode(Script::BuildMapScript(request).diagnostics, "MAP_EDIT_CONFLICT"), "two overlay sources must reject");
    request.finalSaveMembers.reset();

    request.editedMapMembers.push_back({"war3map.w3i", *changedSettings.value});
    const auto duplicate = Script::BuildMapScript(request);
    Require(!duplicate.Succeeded() && HasCode(duplicate.diagnostics, "MAP_EDIT_CONFLICT"),
        "conflicting edited member names must fail without choosing a winner");
    request.editedMapMembers.pop_back();
    request.removedMapMembers = {"WAR3MAP.W3I"};
    const auto removed = Script::BuildMapScript(request);
    Require(!removed.Succeeded() && HasCode(removed.diagnostics, "MAP_EDIT_CONFLICT"),
        "same member cannot be both replaced and removed");
    request.removedMapMembers = {"WAR3MAPUNITS.DOO"};
    request.editedMapMembers.pop_back();
    const auto removedUnits = Script::BuildMapScript(request);
    Require(removedUnits.Succeeded() && removedUnits.generated.text.find("CreateUnit(") == std::string::npos,
        "removed source placement member must not initialize the original unit");
}

void TestFinalDoodadsReachJassWithoutCreatingNativeDecorationsTwice() {
    Scratch scratch;
    Write(scratch.Path() / "ui/config", "base\n");
    Write(scratch.Path() / "ui/base/define.txt", "[TriggerTypes]\ninteger=0,1,1,整数,integer\n");
    Write(scratch.Path() / "ui/base/event.txt", "[TimerEvent]\n");
    Write(scratch.Path() / "ui/base/condition.txt", "[AndMultiple]\nreturns=boolean\n");
    Write(scratch.Path() / "ui/base/action.txt", "[DoNothing]\n");
    Write(scratch.Path() / "ui/base/call.txt", "[GetLastCreated]\nreturns=unit\n");
    auto catalog = Trigger::UiCatalog::Load(scratch.Path()); Require(catalog.Succeeded(), "原创 DOO 初始化目录可加载");
    MapData::DoodadPlacements doodads;
    MapData::DoodadPlacement record; record.rawcode = "A001"; record.creationId = 10;
    record.variation = 1; record.position = {64, 96, 4}; record.scale = {1, 1, 1}; record.flags = 0;
    doodads.doodads.push_back(record);
    record.rawcode = "A002"; record.creationId = 20; record.flags = 4; record.position = {128, 160, 24};
    doodads.doodads.push_back(record);
    record.rawcode = "A003"; record.creationId = 30; record.flags = 2; record.position = {200, 240, 10};
    record.scale = {2, 3, 4}; doodads.doodads.push_back(record);
    doodads.specialDoodads.push_back({"A004", 2, {7, -8}});
    const auto doo = doodads.Serialize(); Require(doo.Succeeded(), "原创非空 classic DOO 序列化");
    const std::vector<MapArchiveMember> members{{"war3map.w3i", MapDataFixtures::FullMapSettingsV25().bytes},
        {"war3mapUnits.doo", MapDataFixtures::EmptyUnitsV8Sub11()}, {"war3map.doo", *doo.value}, {"war3map.w3e", FlatTerrain()}};
    const auto source = scratch.Path() / "doodads.mpq";
    Require(WriteVerifiedArchive(source, members).Succeeded(), "真实写原创初始化归档");
    const auto archive = MapArchiveSession::Open(source); Require(archive.Succeeded(), "独立打开初始化来源");
    Trigger::TriggerDocument triggers;
    triggers.wct.customCode.text = "function OriginalReference takes nothing returns nothing\n"
        "    call SetDestructableInvulnerable(gg_dest_A001_0010, true)\nendfunction\n";
    Script::MapScriptBuildRequest request; request.archive = &*archive.value;
    request.triggers = &triggers; request.catalog = &*catalog.value; request.compile = false;
    const auto before = Script::BuildMapScript(request); Require(before.Succeeded(), "原创经典 DOO 基线 JASS 生成");
    const auto original = MapDocument::FromArchive(*archive.value);
    Require(original.Doodads() && original.Doodads()->doodads.size() == 3,
        "原创三条普通 DOO 必须从真实归档完整解析");
    MapData::MapDataEditSession edits(original);
    std::vector<MapData::PlacementMove> moves;
    for (std::uint32_t row = 0; row < 3; ++row) moves.push_back({
        MapData::MakePlacementId(MapData::PlacementFile::Doodads, row, original.Doodads()->doodads[row]),
        row == 0 ? std::array<float, 3>{512, 256, 12} : row == 1 ? std::array<float, 3>{-64, 80, 40}
            : std::array<float, 3>{333, 444, 55}, row == 0 ? std::optional<float>(std::numbers::pi_v<float> / 2) : std::nullopt});
    Require(MapData::MovePlacements(edits, moves).Succeeded(), "脚本组和原生普通地物沿同一移动事务");
    const auto moved = edits.Snapshot();
    const auto copiedId = MapData::MakePlacementId(MapData::PlacementFile::Doodads, 0, moved.Doodads()->doodads[0]);
    const auto copied = MapData::CopyPlacements(edits, std::span(&copiedId, 1), {32, -16, 4});
    Require(copied.Succeeded() && copied.value->size() == 1 && copied.value->front().creationId == 31,
        "真实复制生成最终 DOO 的 fresh ID");
    auto finalMembers = edits.Replacements();
    Require(finalMembers.size() == 1 && finalMembers.front().name == "war3map.doo", "同一事务只替换最终 DOO");
    request.finalSaveMembers = Script::MapScriptFinalMembers{finalMembers, {}};
    const auto after = Script::BuildMapScript(request); Require(after.Succeeded(), "JASS必须读取真实最终DOO overlay");
    const auto& text = after.generated.text;
    for (const auto* line : {"set gg_dest_A001_0010 = CreateDestructable( 'A001', 512.0, 256.0, 90.000, 1.000, 1 )",
            "CreateDestructable( 'A001', 544.0, 240.0, 90.000, 1.000, 1 )",
            "CreateDestructableZ( 'A002', -64.0, 80.0, 40.0, 0.000, 1.000, 1 )"})
        Require(text.find(line) != std::string::npos, "脚本创建必须使用最终位置/朝向/Z，原gg_dest引用保持绑定");
    const auto callCount = [&](std::string_view needle) {
        std::size_t count = 0, position = 0;
        while ((position = text.find(needle, position)) != std::string::npos) { ++count; position += needle.size(); }
        return count;
    };
    Require(callCount(" = CreateDestructable( ") == 2 && callCount(" = CreateDestructableZ( ") == 1,
        "仅原两个脚本组与其一个复制实例被创建，不能重复初始化");
    Require(text.find("CreateDestructable( 'A001', 64.0, 96.0") == std::string::npos
        && text.find("CreateDestructable( 'A003'") == std::string::npos
        && text.find("CreateDestructableZ( 'A003'") == std::string::npos
        && text.find("CreateDestructable( 'A004'") == std::string::npos
        && text.find("CreateDestructableZ( 'A004'") == std::string::npos,
        "不能回退旧DOO位置或重复脚本创建普通/特殊原生装饰物；不按rawcode前缀分類");
    const auto parsed = MapData::ParseDoodadPlacements(finalMembers.front().data);
    Require(parsed.Succeeded() && parsed.value->doodads.size() == 4 && parsed.value->specialDoodads.size() == 1
        && parsed.value->doodads[2].position == std::array<float, 3>{333, 444, 55}
        && parsed.value->specialDoodads[0].position == std::array<std::int32_t, 2>{7, -8},
        "普通装饰物的新位置与特殊尾由最终DOO表达");
    auto replacements = finalMembers;
    const auto* firstByte = reinterpret_cast<const std::byte*>(text.data());
    replacements.push_back({"war3map.j", {firstByte, firstByte + text.size()}});
    const auto candidatePath = scratch.Path() / "doodads-candidate.mpq";
    Require(archive.value->SaveCandidate(candidatePath, replacements).Succeeded(), "最终DOO与JASS同一候选实际封包");
    const auto independent = MapArchiveSession::Open(candidatePath); Require(independent.Succeeded(), "独立重开DOO/JASS候选");
    for (const auto& member : members) Require(independent.value->ReadMember(member.name).value
        == std::optional<std::vector<std::byte>>(member.name == "war3map.doo" ? finalMembers.front().data : member.data),
        "真实候选DOO与所有未编辑成员必须完整匹配");
    Require(independent.value->ReadMember("war3map.j").value == std::optional<std::vector<std::byte>>(replacements.back().data)
        && archive.value->VerifySourceUnchanged().Succeeded(), "实际新JASS绑定同一候选且来源不变；没有执行外部编译/game");
    auto invalid = *parsed.value; invalid.doodads[0].flags = 1;
    const auto invalidBytes = invalid.Serialize(); Require(invalidBytes.Succeeded(), "未知flags负例仍是可解析wire");
    finalMembers.front().data = *invalidBytes.value;
    request.finalSaveMembers = Script::MapScriptFinalMembers{finalMembers, {}};
    const auto rejected = Script::BuildMapScript(request);
    Require(!rejected.Succeeded() && HasCode(rejected.generated.diagnostics, "DESTRUCTABLE_FLAGS_UNKNOWN")
        && rejected.FinalScript() == nullptr, "未知创建规则不能生成可保存脚本");
}

void TestExtendedTypedInputs() {
    MapData::UnitPlacements units; units.units.emplace_back().skinId = "hfoo";
    MapData::DoodadPlacements doodads; doodads.doodads.emplace_back().skinId = "LTlt";
    MapData::CamerasDocument cameras{0, {}, {}, MapData::CameraLayout::Extended};
    cameras.entries.emplace_back().localRotation = std::array<float, 3>{1.f, 2.f, 3.f};
    MapData::SoundsDocument sounds{2, {}, {}}; sounds.entries.emplace_back().extended.emplace();
    Trigger::TriggerDocument triggers; Trigger::UiCatalog catalog;
    Script::MapScriptInputs inputs;
    inputs.triggers = &triggers; inputs.catalog = &catalog;
    inputs.units = &units; inputs.doodads = &doodads; inputs.cameras = &cameras; inputs.sounds = &sounds;
    MapData::MapSettings settings; settings.formatVersion = 33; settings.forceMinCameraZoom = -1;
    inputs.settings = &settings;
    for (int attempt = 0; attempt < 2; ++attempt) {
        const auto result = Script::GenerateMapScript(inputs);
        Require(result.text.empty() && !result.Succeeded(), "extensions cannot produce a partial success script");
        for (const auto* code : {"CLASSIC_UNITS_LAYOUT_UNSUPPORTED", "CLASSIC_DOODADS_LAYOUT_UNSUPPORTED",
            "CLASSIC_CAMERAS_LAYOUT_UNSUPPORTED", "CLASSIC_SOUNDS_LAYOUT_UNSUPPORTED", "CLASSIC_SETTINGS_LAYOUT_UNSUPPORTED"})
            Require(HasCode(result.diagnostics, code), "each unsupported member is identified");
        cameras.layout = MapData::CameraLayout::Classic; sounds.version = 1;
        settings.formatVersion = 25;
    }
}

} // namespace

int main() {
    try {
        TestOverlayChangesGeneratedScript();
        TestFinalDoodadsReachJassWithoutCreatingNativeDecorationsTwice();
        TestExtendedTypedInputs();
        std::cout << "MapScriptOverlay: edited typed data generated into config/main\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
