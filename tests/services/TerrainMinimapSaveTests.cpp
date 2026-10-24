#include "War3MapCore/Services/TerrainMinimapSave.h"

#include "War3MapCore/Hash.h"
#include "War3MapCore/IO/FileBytes.h"
#include "War3MapCore/Image/BlpImage.h"
#include "War3MapCore/MapData/MapPlacements.h"
#include "War3MapCore/MapData/MapSettings.h"
#include "War3MapCore/MapData/TerrainEditing.h"
#include "War3MapCore/Services/TerrainMinimapMarkers.h"

#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <iostream>
#include <stdexcept>

namespace {
using namespace War3Map;
using namespace War3Map::MapData;
using namespace War3Map::Services;
using Bytes = std::vector<std::byte>;

void Require(bool condition, const char* reason) { if (!condition) throw std::runtime_error(reason); }
template<class T> T Take(Result<T> result) {
    if (!result.Succeeded() || !result.value) {
        std::string message = "原创fixture失败";
        for (const auto& diagnostic : result.diagnostics) message += " | " + diagnostic.code + ":" + diagnostic.message;
        throw std::runtime_error(message);
    }
    return std::move(*result.value);
}
bool Code(const std::vector<Diagnostic>& diagnostics, std::string_view code) {
    return std::any_of(diagnostics.begin(), diagnostics.end(), [&](const auto& diagnostic) { return diagnostic.code == code; });
}
void RequireCode(const std::vector<Diagnostic>& diagnostics, std::string_view code) {
    if (code.empty() || Code(diagnostics, code)) return;
    std::string message = "必须命中预期的具体拒绝门：" + std::string(code);
    for (const auto& diagnostic : diagnostics) message += " | " + diagnostic.code + ":" + diagnostic.message;
    throw std::runtime_error(message);
}
template<class T> void Rejected(const Result<T>& result, std::string_view code = {}) {
    Require(!result.Succeeded() && !result.value && !result.diagnostics.empty(), "拒绝不能返回部分小地图保存资格");
    RequireCode(result.diagnostics, code);
}
void Rejected(const Status& status, std::string_view code = {}) {
    Require(!status.Succeeded() && !status.diagnostics.empty(), "复核必须明确失败");
    RequireCode(status.diagnostics, code);
}
class Directory {
public:
    Directory() {
        static std::atomic<unsigned> next{0};
        path_ = std::filesystem::temp_directory_path() / ("Audastra-missing-minimap-save-"
            + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()) + "-" + std::to_string(next++));
        Require(std::filesystem::create_directory(path_), "创建独立原创fixture目录");
    }
    ~Directory() { if (!keep_) { std::error_code error; std::filesystem::remove_all(path_, error); } }
    const std::filesystem::path& Path() const { return path_; }
    void Keep() { keep_ = true; }
private:
    std::filesystem::path path_;
    bool keep_ = false;
};
Bytes Text(std::string_view text) {
    Bytes bytes; for (const auto character : text) bytes.push_back(static_cast<std::byte>(character)); return bytes;
}
void Put32(Bytes& bytes, std::size_t at, std::uint32_t value) {
    for (unsigned shift = 0; shift < 4; ++shift) bytes[at + shift] = static_cast<std::byte>((value >> (shift * 8)) & 255);
}
Bytes W3i() {
    // 独立classic v25原字：8×8 tiles扣L/R/B/T=[1,3,2,1]后为4×5。
    Bytes bytes(147); Put32(bytes, 0, 25); Put32(bytes, 8, 6061);
    Put32(bytes, 48, 1); Put32(bytes, 52, 3); Put32(bytes, 56, 2); Put32(bytes, 60, 1);
    Put32(bytes, 64, 4); Put32(bytes, 68, 5); bytes[76] = std::byte{'L'};
    return bytes;
}
MapDocument SettingsDocument(const std::optional<Bytes>& settings, const std::optional<Bytes>& units = {}) {
    std::vector<MapArchiveMember> members;
    if (settings) members.push_back({"war3map.w3i", *settings});
    if (units) members.push_back({"war3mapUnits.doo", *units});
    return MapDocument::FromMembers(members);
}
Bytes StartLocation() {
    UnitPlacements units;
    UnitPlacement start; start.rawcode = "sloc"; start.position = {512, 384, 0};
    start.scale = {1, 1, 1}; start.owner = 0; start.creationId = 1;
    units.units.push_back(std::move(start)); return Take(units.Serialize());
}
Bytes TeamColor() {
    // 原创单像素24位TGA，实际BGRA=(3,2,1,255)。
    Bytes bytes(21); bytes[2] = std::byte{2}; bytes[12] = bytes[14] = std::byte{1};
    bytes[16] = std::byte{24}; bytes[17] = std::byte{0x20};
    bytes[18] = std::byte{3}; bytes[19] = std::byte{2}; bytes[20] = std::byte{1}; return bytes;
}
Bytes ImportedMinimap() {
    Bytes bytes(9); Put32(bytes, 0, 1); Put32(bytes, 4, 1); bytes[8] = std::byte{13};
    const auto path = Text("war3mapMap.blp");
    bytes.insert(bytes.end(), path.begin(), path.end()); bytes.push_back(std::byte{0}); return bytes;
}
Bytes Slk(const std::vector<std::string>& headers, const std::vector<std::vector<std::string>>& rows = {}) {
    std::string text = "ID;PWXL;N;E\n";
    const auto row = [&](const auto& values, std::size_t y) {
        for (std::size_t x = 0; x < values.size(); ++x)
            text += "C;X" + std::to_string(x + 1) + ";Y" + std::to_string(y) + ";K\"" + values[x] + "\"\n";
    };
    row(headers, 1); for (std::size_t index = 0; index < rows.size(); ++index) row(rows[index], index + 2);
    return Text(text + "E\n");
}
Bytes Atlas(bool colorful = false) {
    // 原创手造BLP1 oracle，不用产品encoder生成输入。彩色版的cell内双线性插值
    // 会产生超过256种颜色，用来确保实际encoded解码SHA不误用渲染前RGBA SHA。
    const std::uint32_t dimension = colorful ? 64u : 16u;
    std::size_t pixels = 0;
    for (auto size = dimension;; size /= 2) { pixels += static_cast<std::size_t>(size) * size; if (size == 1) break; }
    Bytes bytes(1180 + pixels); const auto magic = Text("BLP1"); std::copy(magic.begin(), magic.end(), bytes.begin());
    Put32(bytes, 4, 1); Put32(bytes, 12, dimension); Put32(bytes, 16, dimension); Put32(bytes, 20, 5); Put32(bytes, 24, 1);
    for (std::uint32_t index = 0; index < 256; ++index) {
        bytes[156 + index * 4] = static_cast<std::byte>((index * 151) & 255);
        bytes[157 + index * 4] = static_cast<std::byte>((index * 73) & 255);
        bytes[158 + index * 4] = static_cast<std::byte>(index);
    }
    std::size_t at = 1180;
    for (std::uint32_t level = 0, size = dimension;; ++level, size /= 2) {
        Put32(bytes, 28 + level * 4, static_cast<std::uint32_t>(at)); Put32(bytes, 92 + level * 4, size * size);
        for (std::uint32_t y = 0; y < size; ++y) for (std::uint32_t x = 0; x < size; ++x)
            bytes[at + static_cast<std::size_t>(y) * size + x] = static_cast<std::byte>(colorful ? (x * 19 + y * 7 + level * 31) & 255 : 30 + level);
        at += static_cast<std::size_t>(size) * size; if (size == 1) break;
    }
    return bytes;
}
std::vector<MapArchiveMember> Materials(bool colorful = false) {
    return {{"TerrainArt\\Terrain.slk", Slk({"tileID", "dir", "file"}, {{"Tg01", "Fixture", "Ground"}})},
        {"TerrainArt\\CliffTypes.slk", Slk({"cliffID"})}, {"TerrainArt\\Water.slk", Slk({"waterID"})},
        {"Fixture\\Ground.blp", Atlas(colorful)}};
}
struct Fixture {
    Directory directory;
    Bytes w3e, wpm;
    std::optional<Bytes> w3i, units;
    MapDataEditSession edits;
    std::filesystem::path gameRoot;
    MapArchiveSession source;
    explicit Fixture(bool presentMinimap = false, bool gameWinner = false, bool colorful = false,
        std::optional<Bytes> settings = {}, bool standardEdges = false, std::optional<Bytes> minimapTga = {},
        bool classicMaterials = false, bool startLocation = false, bool importedMinimap = false)
        : w3i(std::move(settings)), units(startLocation ? std::optional<Bytes>{StartLocation()} : std::nullopt),
          edits(SettingsDocument(w3i, units)) {
        TerrainGridSnapshot terrain;
        terrain.header.width = terrain.header.height = 9; terrain.header.mainTileset = 'L';
        terrain.header.groundTilesets = {"Tg01"}; terrain.header.cliffTilesets = {"CLdi"}; terrain.vertices.resize(81);
        for (auto& vertex : terrain.vertices) {
            vertex.groundHeightRaw = 8192; vertex.waterHeightAndFlagsRaw = 7936; vertex.cliffAndLayerRaw = 2;
        }
        if (standardEdges) {
            for (std::size_t y = 0; y < 8; ++y) for (std::size_t x = 0; x < 8; ++x)
                if (x < 1 || x >= 5 || y < 2 || y >= 7)
                    terrain.vertices[y * 9 + x].waterHeightAndFlagsRaw |= 0x4000;
        }
        RefreshTerrainDerived(terrain, 0, 0, 9, 9); w3e = Take(SerializeMapTerrain(terrain));
        wpm = Text("MP3W"); wpm.resize(16 + 32 * 32, std::byte{0x82}); Put32(wpm, 4, 0); Put32(wpm, 8, 32); Put32(wpm, 12, 32);
        Require(edits.AttachTerrain(w3e).Succeeded() && edits.AttachPathing(wpm).Succeeded(), "附上真实原始W3E/WPM");
        auto members = gameWinner ? std::vector<MapArchiveMember>{} : Materials(colorful);
        if (classicMaterials) {
            for (auto& member : members) if (member.name == "TerrainArt\\CliffTypes.slk")
                member.data = Slk({"cliffID", "texDir", "texFile", "cliffModelDir", "rampModelDir", "groundTile", "cliffClass"},
                    {{"CLdi", "Fixture", "Cliff", "Cliff", "Ramp", "Tg01", "0"}});
            members.push_back({"Fixture\\Cliff.blp", Atlas(false)});
            members.push_back({"UI\\MiscData.txt", Text("[Water]\nDeepLevelPathing=64\n")});
            members.push_back({"UI\\WorldEditData.txt", Text("[CameraMargins]\nLeft=0\nRight=0\nBottom=0\nTop=0\n")});
        }
        members.push_back({"war3map.w3e", w3e}); members.push_back({"war3map.wpm", wpm});
        if (w3i) members.push_back({"war3map.w3i", *w3i});
        if (units) {
            members.push_back({"war3mapUnits.doo", *units});
            members.push_back({"Fixture\\TeamColor00.tga", TeamColor()});
        }
        members.push_back({"war3map.j", Text("function config takes nothing returns nothing\nendfunction\nfunction main takes nothing returns nothing\nendfunction\n")});
        Bytes native(12); Put32(native, 0, 1); members.push_back({"war3map.w3u", native});
        members.push_back({"war3map.shd", {std::byte{17}, std::byte{25}}});
        members.push_back({"unrelated.bin", Text("original unrelated member")});
        if (presentMinimap) members.push_back({"war3mapMap.blp", importedMinimap ? Atlas(true) : Bytes{}});
        if (importedMinimap) members.push_back({"war3map.imp", ImportedMinimap()});
        if (minimapTga) members.push_back({"war3mapMap.TGA", *minimapTga});
        Take(WriteVerifiedArchive(directory.Path() / "source.w3x", members));
        source = Take(MapArchiveSession::Open(directory.Path() / "source.w3x"));
        gameRoot = directory.Path() / "game"; Require(std::filesystem::create_directory(gameRoot), "创建原创game来源目录");
        auto gameMaterials = Materials(colorful);
        if (units) gameMaterials.push_back({"UI\\war3skins.txt", Text("[Default]\nTeamColor=Fixture/TeamColor\n")});
        Take(WriteVerifiedArchive(gameRoot / "war3.mpq", gameMaterials));
    }
    void Stroke(TerrainBrushKind kind = TerrainBrushKind::Raise, float strength = 10.0f,
        TerrainStrokePoint point = {512, 512}) {
        TerrainBrush brush; brush.kind = kind; brush.radius = 0; brush.strength = strength;
        Require(edits.EditTerrain(brush, std::span(&point, 1), false).Succeeded(), "应用真实height/blight笔刷事务");
    }
    void Freeze(MapSaveTask& task) {
        Require(task.FreezeArchive(source).Succeeded(), "FreezeArchive拥有实际来源");
        task.target = directory.Path() / "candidate.w3x"; task.replacements = edits.Replacements();
    }
    std::shared_ptr<const TerrainMinimapSave> Prepare(MapSaveTask& task,
        TerrainMinimapSaveChoice choice = TerrainMinimapSaveChoice::GenerateMissingGroundOnly) {
        return Take(TerrainMinimapSave::Prepare(task, edits, gameRoot, choice));
    }
};

void DeclaredChoiceRequiresActualW3iAndRetainsStandardEdges() {
    constexpr auto declared = TerrainMinimapSaveChoice::GenerateMissingDeclaredGroundOnly;
    Fixture fixture(false, false, true, W3i(), true); fixture.Stroke(TerrainBrushKind::Raise, 10, {384, 512});
    MapSaveTask task; fixture.Freeze(task);
    Rejected(TerrainMinimapSave::Prepare(task, fixture.edits, fixture.gameRoot,
        TerrainMinimapSaveChoice::GenerateMissingGroundOnly), "TERRAIN_MINIMAP_MAP_EDGE");
    const auto prepared = fixture.Prepare(task, declared);
    Require(prepared->Manifest().rangePolicy == Render::TerrainMinimapRangePolicy::DeclaredW3iMarginsExteriorMapEdge
        && prepared->Manifest().w3i.sha256 == Take(Sha256(*fixture.w3i))
        && prepared->Manifest().renderExtent == std::array<double, 4>{128, 256, 640, 896},
        "显式新choice绑定真实W3I声明范围与实际manifest政策");
    task.replacements.push_back(prepared->MinimapReplacement());
    Require(prepared->VerifyCurrent(task, fixture.edits, fixture.gameRoot).Succeeded()
        && prepared->VerifyFrozenOutput(task).Succeeded(), "标准源/当前MapEdge不必清位即可进入独立生成政策");
    task.replacements.push_back({"war3map.w3i", *fixture.w3i});
    Rejected(prepared->VerifyFrozenOutput(task), "MINIMAP_SAVE_DOMAIN"); task.replacements.pop_back();
    task.beforePublish = [&] { return prepared->VerifyFrozenOutput(task); };
    MapSaveService::Run(task);
    Require(task.succeeded && task.nextSession, "声明范围候选走真实Archive SaveCopy");
    auto opened = Take(MapArchiveSession::Open(task.target));
    Require(prepared->VerifyReadback(task, opened).Succeeded()
        && Take(opened.ReadMember("war3map.w3i")) == *fixture.w3i,
        "独立Open回读绑定实际完整MapBLP与未改动W3I原字");
    const auto saved = Take(ParseMapTerrain(Take(opened.ReadMember("war3map.w3e"))));
    const auto original = Take(ParseMapTerrain(fixture.w3e));
    for (std::size_t i = 0; i < original.Snapshot()->vertices.size(); ++i)
        Require(saved.Snapshot()->vertices[i].waterHeightAndFlagsRaw == original.Snapshot()->vertices[i].waterHeightAndFlagsRaw,
            "生成与保存不清除源/当前任何water或MapEdge位");
    auto changedW3i = *fixture.w3i; Put32(changedW3i, 4, 1);
    const std::array changed{MemberReplacement{"war3map.w3i", changedW3i}};
    Take(opened.SaveCandidate(fixture.directory.Path() / "changed-readback.w3x", changed));
    auto changedArchive = Take(MapArchiveSession::Open(fixture.directory.Path() / "changed-readback.w3x"));
    Rejected(prepared->VerifyReadback(task, changedArchive), "MINIMAP_SAVE_TARGET");
    Require(IO::WriteFileBytes(task.target, Take(IO::ReadFileBytes(changedArchive.SourcePath()))).Succeeded(),
        "只改自有SaveCopy目标的W3I原字以测试实际回读依赖");
    auto changedSameTarget = Take(MapArchiveSession::Open(task.target));
    Rejected(prepared->VerifyReadback(task, changedSameTarget), "TERRAIN_SAVE_PLAN_MEMBER_MISMATCH");

    Fixture current(false, false, false, W3i(), true); current.Stroke(TerrainBrushKind::Raise, 10, {384, 512});
    MapSaveTask currentTask; current.Freeze(currentTask); const auto currentPrepared = current.Prepare(currentTask, declared);
    currentTask.replacements.push_back(currentPrepared->MinimapReplacement());
    auto currentW3i = *current.w3i; Put32(currentW3i, 4, 1);
    Require(current.edits.EditSerialized(MapDataKind::Settings, currentW3i).Succeeded(), "建立真实当前Settings修改");
    Rejected(currentPrepared->VerifyCurrent(currentTask, current.edits, current.gameRoot), "TERRAIN_SAVE_PLAN_STALE");

    Fixture missing; missing.Stroke(); MapSaveTask missingTask; missing.Freeze(missingTask);
    Rejected(TerrainMinimapSave::Prepare(missingTask, missing.edits, missing.gameRoot, declared), "TERRAIN_ASSETS_W3I_MISSING");
    Fixture mask(false, false, false, W3i()); mask.Stroke(TerrainBrushKind::Raise, 10, {384, 512});
    MapSaveTask maskTask; mask.Freeze(maskTask);
    Rejected(TerrainMinimapSave::Prepare(maskTask, mask.edits, mask.gameRoot, declared), "TERRAIN_MINIMAP_MAP_EDGE_MASK");
    Fixture bounded(false, false, false, W3i(), true); bounded.Stroke(TerrainBrushKind::Raise, 10, {384, 512});
    MapSaveTask boundedTask; bounded.Freeze(boundedTask);
    TerrainMinimapSaveLimits limits; limits.assets.maxSourceBytes = 32 * 1024;
    // 只测W3I Settings固定64KiB预收。把不参与本例的表/atlas上限也收紧，
    // 避免共享envelope为默认大限额整体缩减，先在基线Plan处耗尽32KiB。
    limits.assets.maxCurrentW3eBytes = 4 * 1024;
    limits.assets.tables.maxTableBytes = 4 * 1024;
    limits.assets.tables.maxRows = 16; limits.assets.tables.maxCells = 64;
    limits.assets.maxRetainedAtlasCapacityBytes = 1024 * 1024;
    limits.assets.atlas.maxWorkingBytes = 1024 * 1024;
    Rejected(TerrainMinimapSave::Prepare(boundedTask, bounded.edits, bounded.gameRoot, declared, limits), "TERRAIN_ASSETS_BUDGET");
    limits = {}; limits.candidate.maxWorkingBytes = 64 * 1024;
    Rejected(TerrainMinimapSave::Prepare(boundedTask, bounded.edits, bounded.gameRoot, declared, limits), "TERRAIN_MINIMAP_BUDGET");
    Fixture flat(false, false, false, W3i(), true); flat.Stroke(TerrainBrushKind::Smooth, 1, {384, 512});
    MapSaveTask flatTask; flat.Freeze(flatTask);
    Rejected(TerrainMinimapSave::Prepare(flatTask, flat.edits, flat.gameRoot, declared), "MINIMAP_SAVE_DOMAIN");
    Fixture present(true, false, false, W3i(), true); present.Stroke(TerrainBrushKind::Raise, 10, {384, 512});
    MapSaveTask presentTask; present.Freeze(presentTask);
    Rejected(TerrainMinimapSave::Prepare(presentTask, present.edits, present.gameRoot, declared), "MINIMAP_SAVE_NOT_MISSING");
}

void PreserveAndEarlyRejections() {
    MapSaveTask empty; MapDataEditSession noEdits{MapDocument::FromMembers({})};
    Require(!Take(TerrainMinimapSave::Prepare(empty, noEdits, {}, TerrainMinimapSaveChoice::Preserve)),
        "显式Preserve成功null且不访问无效源/gameRoot");
    Rejected(TerrainMinimapSave::Prepare(empty, noEdits, {}, static_cast<TerrainMinimapSaveChoice>(99)), "MINIMAP_SAVE_CHOICE");
    Fixture noChange; MapSaveTask noChangeTask; noChange.Freeze(noChangeTask);
    Rejected(TerrainMinimapSave::Prepare(noChangeTask, noChange.edits, noChange.gameRoot,
        TerrainMinimapSaveChoice::GenerateMissingGroundOnly), "MINIMAP_SAVE_DOMAIN");
    Fixture existing(true); existing.Stroke(); MapSaveTask existingTask; existing.Freeze(existingTask);
    Rejected(TerrainMinimapSave::Prepare(existingTask, existing.edits, existing.gameRoot,
        TerrainMinimapSaveChoice::GenerateMissingGroundOnly), "MINIMAP_SAVE_NOT_MISSING");
    const std::array customMinimap{MemberReplacement{"war3mapMap.blp", {std::byte{9}, std::byte{23}}}};
    Take(existing.source.SaveCandidate(existing.directory.Path() / "custom-minimap.w3x", customMinimap));
    auto customSource = Take(MapArchiveSession::Open(existing.directory.Path() / "custom-minimap.w3x"));
    MapSaveTask customTask; Require(customTask.FreezeArchive(customSource).Succeeded(), "冻结真实已有自定义MapBLP");
    customTask.target = existing.directory.Path() / "custom-candidate.w3x"; customTask.replacements = existing.edits.Replacements();
    Rejected(TerrainMinimapSave::Prepare(customTask, existing.edits, existing.gameRoot,
        TerrainMinimapSaveChoice::GenerateMissingGroundOnly), "MINIMAP_SAVE_NOT_MISSING");
    Fixture blight; blight.Stroke(TerrainBrushKind::Blight); MapSaveTask blightTask; blight.Freeze(blightTask);
    Rejected(TerrainMinimapSave::Prepare(blightTask, blight.edits, blight.gameRoot,
        TerrainMinimapSaveChoice::GenerateMissingGroundOnly), "MINIMAP_SAVE_DOMAIN");
    Fixture overHeight; overHeight.Stroke(TerrainBrushKind::Raise, 40);
    auto wetHeight = *overHeight.edits.Terrain(); wetHeight.vertices[4 * 9 + 4].textureAndFlagsRaw |= 0x40;
    Require(overHeight.edits.SetTerrain(Take(SerializeMapTerrain(wetHeight))).Succeeded(), "混合涉水修改保持草稿但不能正式保存");
    MapSaveTask heightTask; overHeight.Freeze(heightTask);
    Rejected(TerrainMinimapSave::Prepare(heightTask, overHeight.edits, overHeight.gameRoot,
        TerrainMinimapSaveChoice::GenerateMissingGroundOnly), "MAP_TERRAIN_PATHING_UNVERIFIED");
    Fixture bounded; bounded.Stroke(); MapSaveTask task; bounded.Freeze(task);
    auto limits = TerrainMinimapSaveLimits{}; limits.maxWorkingBytes = 1024;
    Rejected(TerrainMinimapSave::Prepare(task, bounded.edits, bounded.gameRoot,
        TerrainMinimapSaveChoice::GenerateMissingGroundOnly, limits), "MINIMAP_SAVE_BUDGET");
    limits.maxWorkingBytes = TerrainMinimapSaveLimits{}.maxWorkingBytes + 1;
    Rejected(TerrainMinimapSave::Prepare(task, bounded.edits, bounded.gameRoot,
        TerrainMinimapSaveChoice::GenerateMissingGroundOnly, limits), "MINIMAP_SAVE_BUDGET");
    task.inPlace = true;
    Rejected(TerrainMinimapSave::Prepare(task, bounded.edits, bounded.gameRoot,
        TerrainMinimapSaveChoice::GenerateMissingGroundOnly), "MINIMAP_SAVE_TARGET");
    task.inPlace = false; task.sourceSha256[0] = task.sourceSha256[0] == 'a' ? 'b' : 'a';
    Rejected(TerrainMinimapSave::Prepare(task, bounded.edits, bounded.gameRoot,
        TerrainMinimapSaveChoice::GenerateMissingGroundOnly), "MINIMAP_SAVE_SOURCE");
    bounded.Freeze(task); task.target = bounded.source.SourcePath();
    Rejected(TerrainMinimapSave::Prepare(task, bounded.edits, bounded.gameRoot,
        TerrainMinimapSaveChoice::GenerateMissingGroundOnly), "MINIMAP_SAVE_TARGET");
    bounded.Freeze(task);
    Require(IO::WriteFileBytes(task.target, Text("existing user-owned target")).Succeeded(), "原创既有目标fixture");
    Rejected(TerrainMinimapSave::Prepare(task, bounded.edits, bounded.gameRoot,
        TerrainMinimapSaveChoice::GenerateMissingGroundOnly), "MINIMAP_SAVE_TARGET");
    Require(Take(IO::ReadFileBytes(task.target)) == Text("existing user-owned target"), "目标拒绝保留已有原字");
    task.target = bounded.directory.Path() / "still-missing.w3x";
    Require(!std::filesystem::exists(task.target), "所有准备失败均不发布候选");
}

void FinalListRevisionAndWinnersAreReal() {
    Fixture fixture; fixture.Stroke(); MapSaveTask task; fixture.Freeze(task);
    task.replacements.push_back({"unrelated.bin", Text("even identical unrelated replacement is forbidden")});
    Rejected(TerrainMinimapSave::Prepare(task, fixture.edits, fixture.gameRoot,
        TerrainMinimapSaveChoice::GenerateMissingGroundOnly), "MINIMAP_SAVE_DOMAIN");
    task.replacements.pop_back(); const auto prepared = fixture.Prepare(task);
    task.replacements.push_back(prepared->MinimapReplacement());
    Require(prepared->VerifyCurrent(task, fixture.edits, fixture.gameRoot).Succeeded(), "append唯一MapBLP后核真实会话");
    Require(prepared->Plan()->Members()[4].action == TerrainSaveMemberAction::WriteEdited
        && prepared->Plan()->Members()[4].sourceState == TerrainSaveSourceState::Missing,
        "同一扩展Plan准确声明Missing→WriteEdited");
    Require(Code(prepared->Plan()->Diagnostics(), "TERRAIN_SAVE_SHADOW_NOT_REBUILT")
        && Code(prepared->Plan()->Diagnostics(), "TERRAIN_SAVE_MARKERS_NOT_REBUILT")
        && !Code(prepared->Plan()->Diagnostics(), "TERRAIN_SAVE_MINIMAP_NOT_REBUILT"), "只移除MapBLP未重建告警");
    for (const auto* name : {"war3map.j", "war3map.w3u"})
        task.replacements.push_back({name, Take(task.archive->ReadMember(name))});
    Require(prepared->VerifyFrozenOutput(task).Succeeded(), "真实原脚本/native模板原字重现允许通过");
    task.replacements.back().data[0] ^= std::byte{1}; Rejected(prepared->VerifyFrozenOutput(task), "MINIMAP_SAVE_DOMAIN");
    task.replacements.back().data[0] ^= std::byte{1};
    task.replacements.push_back({"war3map.lua", Text("new member")}); Rejected(prepared->VerifyFrozenOutput(task), "MINIMAP_SAVE_DOMAIN");
    task.replacements.pop_back(); task.replacements.push_back({"notes.txt", Text("new import")});
    Rejected(prepared->VerifyFrozenOutput(task), "MINIMAP_SAVE_DOMAIN"); task.replacements.pop_back();
    task.removals.push_back("unrelated.bin"); Rejected(prepared->VerifyFrozenOutput(task), "MINIMAP_SAVE_DOMAIN"); task.removals.clear();
    task.replacements.push_back(prepared->MinimapReplacement()); Rejected(prepared->VerifyFrozenOutput(task), "MINIMAP_SAVE_DOMAIN");
    task.replacements.pop_back(); task.replacements[1].data.back() ^= std::byte{1};
    Rejected(prepared->VerifyFrozenOutput(task), "TERRAIN_SAVE_PLAN_MEMBER_MISMATCH"); task.replacements[1].data.back() ^= std::byte{1};
    const auto target = task.target; task.target = fixture.directory.Path() / "other.w3x";
    Rejected(prepared->VerifyFrozenOutput(task), "MINIMAP_SAVE_TARGET"); task.target = target;
    Require(fixture.edits.Undo() && fixture.edits.Redo(), "相同字节Undo/Redo改变实际revision");
    Rejected(prepared->VerifyCurrent(task, fixture.edits, fixture.gameRoot), "TERRAIN_SAVE_PLAN_STALE");

    Fixture game(false, true); game.Stroke(); MapSaveTask gameTask; game.Freeze(gameTask);
    const auto gamePrepared = game.Prepare(gameTask); gameTask.replacements.push_back(gamePrepared->MinimapReplacement());
    Require(gamePrepared->VerifyFrozenOutput(gameTask).Succeeded(), "实际game winner复核通过");
    auto changedResources = Materials(); changedResources.back().data[156 + 30 * 4] ^= std::byte{1};
    Take(WriteVerifiedArchive(game.directory.Path() / "changed-game.mpq", changedResources));
    const auto bytes = Take(IO::ReadFileBytes(game.directory.Path() / "changed-game.mpq"));
    Require(IO::WriteFileBytes(game.gameRoot / "war3.mpq", bytes).Succeeded(), "只改变自有game fixture实际winner编码原字");
    Rejected(gamePrepared->VerifyFrozenOutput(gameTask));
}

void ExistingTgaCannotBeShadowedByGeneratedBlp() {
    for (const auto& tga : {Bytes{}, Text("original custom TGA payload")}) {
        Fixture fixture(false, false, false, {}, false, tga);
        fixture.Stroke(); MapSaveTask task; fixture.Freeze(task);
        const auto sourceSha = Take(Sha256File(fixture.source.SourcePath()));
        const auto replacements = task.replacements;
        const auto revision = fixture.edits.Revision();
        for (const auto choice : {TerrainMinimapSaveChoice::GenerateMissingGroundOnly,
            TerrainMinimapSaveChoice::GenerateMissingDeclaredGroundOnly})
            Rejected(TerrainMinimapSave::Prepare(task, fixture.edits, fixture.gameRoot, choice), "MINIMAP_SAVE_TGA_PRESENT");
        const auto preserve = TerrainMinimapSave::Prepare(task, fixture.edits, fixture.gameRoot, TerrainMinimapSaveChoice::Preserve);
        Require(preserve.Succeeded() && !*preserve.value, "ordinary preserve does not regenerate or decode author TGA");
        Require(task.replacements.size() == replacements.size() && !task.executed && !std::filesystem::exists(task.target)
            && fixture.edits.Revision() == revision && fixture.edits.IsDirty(), "TGA refusal leaves task, target and pending edits unchanged");
        Require(Take(fixture.source.ReadMember("war3mapMap.TGA")) == tga
            && Take(Sha256File(fixture.source.SourcePath())) == sourceSha, "existing and empty TGA bytes/source remain unchanged");
    }
}

void PersistentArchiveSaveCopyAndIndependentOpen() {
    Fixture fixture(false, false, true); fixture.Stroke(); MapSaveTask task; fixture.Freeze(task);
    const auto sourceSha = Take(Sha256File(fixture.source.SourcePath()));
    const auto prepared = fixture.Prepare(task); task.replacements.push_back(prepared->MinimapReplacement());
    Require(prepared->Manifest().encodedMipCount == 9 && prepared->Manifest().encodedTotalMipPixels == 87381
        && prepared->Manifest().outputBlpSha256 == Take(Sha256(prepared->MinimapReplacement().data)), "实际encoded完整九级及整字节摘要");
    const auto decoded = Take(Image::DecodeBlp(prepared->MinimapReplacement().data));
    Require(prepared->DecodedMip0().width == 256 && prepared->DecodedMip0().height == 256
        && prepared->DecodedMip0().decodedRgbaSha256 == Take(Sha256(decoded.rgba)), "decodedMip0来源只能是实际encoded解码");
    Require(prepared->Manifest().paletteQuantized
        && prepared->DecodedMip0().decodedRgbaSha256 != prepared->Manifest().outputRgbaSha256,
        "原创彩色atlas必须覆盖有损量化且不能拿渲染前RGBA当回读基线");
    Require(!prepared->Manifest().formalSaveAuthorized && !prepared->Manifest().nativeEditorOrGameVerified,
        "通用candidate事实仍不宣称formal/native授权");
    Require(prepared->VerifyCurrent(task, fixture.edits, fixture.gameRoot).Succeeded(), "真实revision/current源复核");
    task.beforePublish = [&] { return prepared->VerifyFrozenOutput(task); };
    MapSaveService::Run(task);
    if (!task.succeeded) {
        std::string message = "原创SaveCopy失败";
        for (const auto& diagnostic : task.diagnostics) message += " | " + diagnostic.code + ":" + diagnostic.message;
        throw std::runtime_error(message);
    }
    std::error_code pathError;
    const auto equivalent = std::filesystem::equivalent(task.written, task.target, pathError);
    if (!task.nextSession || !equivalent || pathError || task.scriptRebuilt) {
        const auto written = task.written.u8string(), target = task.target.u8string();
        std::cerr << "SaveCopy断言现场：succeeded=" << task.succeeded
            << " nextSession=" << static_cast<bool>(task.nextSession)
            << " scriptRebuilt=" << task.scriptRebuilt
            << " lexicalPathEqual=" << (task.written == task.target)
            << " equivalent=" << equivalent << " pathError=" << pathError.value()
            << " written=" << std::string(written.begin(), written.end())
            << " target=" << std::string(target.begin(), target.end()) << '\n';
        for (const auto& diagnostic : task.diagnostics)
            std::cerr << diagnostic.subsystem << ':' << diagnostic.code << ':' << diagnostic.message << '\n';
    }
    Require(task.nextSession && equivalent && !pathError && !task.scriptRebuilt,
        "本层验证真实archive SaveCopy/AfterSave，不伪称运行JASS工具链");
    auto independentlyOpened = Take(MapArchiveSession::Open(task.target));
    Require(prepared->VerifyReadback(task, independentlyOpened).Succeeded(), "独立新Open执行五成员/依赖/encoded/实际decode回读");
    Rejected(prepared->Plan()->VerifyReadback(independentlyOpened, {32}));
    MapSaveTask unrelatedTask; fixture.Freeze(unrelatedTask);
    Rejected(prepared->VerifyReadback(unrelatedTask, independentlyOpened), "MINIMAP_SAVE_SOURCE");
    Require(Take(independentlyOpened.ReadMember("war3mapMap.blp")) == prepared->MinimapReplacement().data
        && Take(Sha256File(fixture.source.SourcePath())) == sourceSha, "候选持有实际完整BLP且原图SHA不变");
    for (const auto* name : {"war3map.wpm", "war3map.shd", "war3map.j", "war3map.w3u", "unrelated.bin", "Fixture\\Ground.blp"})
        Require(Take(independentlyOpened.ReadMember(name)) == Take(fixture.source.ReadMember(name)), "旧成员原字保留");
    fixture.directory.Keep();
    std::cout << "archive SaveCopy/independent Open evidence (no JASS toolchain/native/game): "
        << task.target.string() << " source=" << sourceSha << " encoded=" << prepared->Manifest().outputBlpSha256 << '\n';
}

void AutomaticDecisionUsesExactSourceMembers() {
    Fixture existing(true, false, false, W3i());
    MapSaveTask untouched; existing.Freeze(untouched);
    Require(!Take(TerrainMinimapSave::RequiresAutomaticGeneration(untouched)),
        "已有BLP且地形与对象未变时必须沿用原字，空BLP仍是已存在成员");
    untouched.replacements.push_back({"WAR3MAP.W3E", existing.w3e});
    untouched.replacements.push_back({"war3map.w3i", *existing.w3i});
    untouched.replacements.push_back({"war3map.w3u", Take(existing.source.ReadMember("war3map.w3u"))});
    Require(!Take(TerrainMinimapSave::RequiresAutomaticGeneration(untouched)),
        "模板重现原W3E/W3I/native原字不算净变化，不能额外读取贴图和重建噪点");
    Require(!Take(TerrainMinimapSave::Prepare(untouched, existing.edits, existing.gameRoot,
        TerrainMinimapSaveChoice::Automatic)), "最终W3I原字未变时仍沿用既有BLP，不无故启动生成器");
    untouched.replacements.push_back({"unrelated.bin", Text("changed unrelated member")});
    untouched.removals.push_back("not-present.blp");
    Require(!Take(TerrainMinimapSave::RequiresAutomaticGeneration(untouched)),
        "无关成员变化和不存在的依赖删除不应重建小地图");
    untouched.removals.push_back("Fixture/Ground.blp");
    Require(Take(TerrainMinimapSave::RequiresAutomaticGeneration(untouched)), "真正删除贴图依赖会触发重建");
    existing.Stroke(); MapSaveTask changed; existing.Freeze(changed);
    Require(Take(TerrainMinimapSave::RequiresAutomaticGeneration(changed)),
        "地形变化后普通保存必须自动重建已有BLP，而不是仅在缺失时补建");
    Fixture missing(false, false, false, W3i()); MapSaveTask newMap; missing.Freeze(newMap);
    Require(Take(TerrainMinimapSave::RequiresAutomaticGeneration(newMap)), "完整新图缺少小地图时自动生成");
    for (const auto& tga : {Bytes{}, Text("original tga bytes")}) {
        Fixture original(false, false, false, W3i(), false, tga); original.Stroke();
        MapSaveTask task; original.Freeze(task);
        Require(!Take(TerrainMinimapSave::RequiresAutomaticGeneration(task)),
            "源图已有TGA（包括present空成员）时不生成BLP");
        Require(!Take(TerrainMinimapSave::Prepare(task, original.edits, original.gameRoot, TerrainMinimapSaveChoice::Automatic)),
            "TGA保留不要求加载地形贴图或返回生成资格");
        task.removals.push_back("war3mapMap.tga");
        const auto removed = TerrainMinimapSave::RequiresAutomaticGeneration(task);
        Rejected(removed, "MINIMAP_SAVE_TGA_REMOVED");
    }
    {
        Fixture complete(true, false, false, W3i());
        const auto blp = complete.source.ReadMember("war3mapMap.blp");
        Require(blp.Succeeded() && blp.value, "read partial-archive BLP fixture");
        const auto partialPath = complete.directory.Path() / "partial-with-blp.w3x";
        Require(WriteVerifiedArchive(partialPath, std::vector<MapArchiveMember>{{"war3mapMap.blp", *blp.value}}).Succeeded(),
            "write partial archive without W3E");
        auto partial = MapArchiveSession::Open(partialPath);
        Require(partial.Succeeded() && partial.value, "open partial archive without W3E");
        MapSaveTask task; Require(task.FreezeArchive(*partial.value).Succeeded(), "freeze partial-archive source");
        task.target = complete.directory.Path() / "partial-saved.w3x";
        Require(!Take(TerrainMinimapSave::RequiresAutomaticGeneration(task)),
            "accurately missing W3E partial archives do not trigger automatic minimap generation");
    }
    MapSaveTask unowned;
    Rejected(TerrainMinimapSave::RequiresAutomaticGeneration(unowned), "MINIMAP_SAVE_SOURCE");
}

void AutomaticGeneratedBlpIsVerifiedThroughSaveCopyAndInPlace() {
    for (const auto [existing, inPlace] : {std::pair{false, false}, std::pair{true, false}, std::pair{true, true}}) {
        auto settings = W3i(); Put32(settings, 72, 0x400); // 原版自动补足相机边距模式
        Fixture fixture(existing, false, false, settings, false, {}, true);
        if (existing) fixture.Stroke();
        MapSaveTask task; fixture.Freeze(task);
        task.inPlace = inPlace;
        if (inPlace) {
            task.target = fixture.source.SourcePath();
            task.inPlaceOptions.backupDirectory = fixture.directory.Path() / "backups";
        }
        const auto prepared = Take(TerrainMinimapSave::Prepare(task, fixture.edits, fixture.gameRoot,
            TerrainMinimapSaveChoice::Automatic));
        Require(prepared && prepared->ClassicManifest(), "正常保存必须使用原版逐格算法清单");
        Require(prepared->Manifest().encodedMipCount == 1 && prepared->Manifest().encodedTotalMipPixels == 65536,
            "正常保存生成JPEGq40单mip，不沿用旧调色板九mip政策");
        task.replacements.push_back(prepared->MinimapReplacement());
        Require(prepared->VerifyCurrent(task, fixture.edits, fixture.gameRoot).Succeeded(), "自动小地图当前会话与依赖复核");
        task.beforePublish = [&] { return prepared->VerifyFrozenOutput(task); };
        MapSaveService::Run(task);
        if (!task.succeeded || !task.nextSession) {
            std::string message = "自动小地图正式保存失败";
            for (const auto& diagnostic : task.diagnostics) message += " | " + diagnostic.code + ":" + diagnostic.message;
            throw std::runtime_error(message);
        }
        auto saved = Take(MapArchiveSession::Open(task.target));
        Require(prepared->VerifyReadback(task, saved).Succeeded(), "自动小地图独立Open回读真实输出");
        Require(Take(saved.ReadMember("war3mapMap.blp")) == prepared->MinimapReplacement().data,
            "自动小地图保存字节与实际生成完全一致");
        Require(Take(saved.ReadMember("war3map.shd")) == Text(std::string_view("\x11\x19", 2)),
            "自动小地图准入不改变SHD保存资格或原字");
        if (inPlace) Require(!task.backup.empty() && std::filesystem::exists(task.backup), "原地保存保留真实备份");
    }
}

void AutomaticResourceOverlaySavesAndReadsBack() {
    auto settings = W3i(); Put32(settings, 72, 0x400);
    Fixture fixture(false, false, false, settings, false, {}, true);
    MapSaveTask task;
    fixture.Freeze(task);
    const auto overlay = Atlas(true);
    task.replacements.push_back({"Fixture\\Ground.blp", overlay});
    const auto prepared = Take(TerrainMinimapSave::Prepare(task, fixture.edits, fixture.gameRoot,
        TerrainMinimapSaveChoice::Automatic));
    Require(prepared && prepared->ClassicManifest(), "Automatic资源overlay必须生成完整Classic候选");
    task.replacements.push_back(prepared->MinimapReplacement());
    Require(prepared->VerifyCurrent(task, fixture.edits, fixture.gameRoot).Succeeded(),
        "Automatic资源overlay当前来源复核");
    task.beforePublish = [&] { return prepared->VerifyFrozenOutput(task); };
    MapSaveService::Run(task);
    Require(task.succeeded && task.nextSession, "Automatic资源overlay必须真实写出候选");
    const auto saved = Take(MapArchiveSession::Open(task.target));
    Require(prepared->VerifyReadback(task, saved).Succeeded(),
        "Automatic资源overlay必须独立回读验证");
    Require(Take(saved.ReadMember("Fixture\\Ground.blp")) == overlay,
        "保存归档必须包含最终资源overlay正文");
}

void AutomaticReusesVerifiedPlanAndPreservesOnBudgetFailure() {
    auto settings = W3i(); Put32(settings, 72, 0x400);
    Fixture fixture(true, false, false, settings, false, {}, true);
    fixture.Stroke();
    MapSaveTask task; fixture.Freeze(task);
    const auto plan = std::make_shared<const TerrainSavePlan>(Take(
        TerrainSavePlan::PrepareForSave(fixture.source, fixture.edits, fixture.gameRoot)));
    Require(plan->SaveGate().Succeeded(), "shared terrain plan passes before minimap generation");
    TerrainMinimapSaveLimits bounded; bounded.maxWorkingBytes = 1;
    const auto preserved = Take(TerrainMinimapSave::PrepareAutomaticOrPreserve(
        task, fixture.edits, fixture.gameRoot, bounded, plan));
    Require(preserved.preserved && !preserved.generated, "budget exhaustion keeps the source image");
    Require(preserved.preserved->VerifyCurrent(task, fixture.edits).Succeeded(), "preservation binds the same actual task");
    Require(plan->VerifyForSave(fixture.edits, task.sourceSha256, task.replacements).Succeeded(),
        "failed minimap generation does not mutate the shared terrain plan");
    const auto generated = Take(TerrainMinimapSave::PrepareAutomaticOrPreserve(
        task, fixture.edits, fixture.gameRoot, {}, plan));
    Require(generated.generated && !generated.preserved, "same shared terrain plan can generate with sufficient budget");
    Require(generated.generated->Plan()->Members()[1].outputSha256 == plan->Members()[1].outputSha256,
        "automatic image generation keeps the existing pathing output identity");
    task.replacements.push_back(generated.generated->MinimapReplacement());
    Require(generated.generated->VerifyCurrent(task, fixture.edits, fixture.gameRoot).Succeeded(),
        "cloned plan binds the real task and inputs");
}

void AutomaticFinalSettingsAndMarkersShareTheSavedView() {
    for (const auto mode : {0, 1, 2}) {
        auto original = W3i(); Put32(original, 72, 0x400);
        Fixture fixture(false, false, false, original, false, {}, true, true);
        const auto sourceSha = Take(Sha256File(fixture.source.SourcePath()));
        Bytes originalBlp; std::optional<std::uint32_t> originalNoise;
        {
            MapSaveTask before; fixture.Freeze(before);
            const auto baseline = fixture.Prepare(before, TerrainMinimapSaveChoice::Automatic);
            originalBlp = baseline->MinimapReplacement().data;
            originalNoise = baseline->ClassicManifest()->postprocessFacts.noiseSeed;
        }
        auto settings = Take(MapSettings::Parse(original));
        settings.mapName = "Final settings save fixture"; settings.author = "Synthetic fixture"; settings.saves = 9;
        std::array<std::uint32_t, 4> rectangle{1, 2, 5, 7};
        std::array<std::int32_t, 2> icon{179, 205};
        if (mode == 1) {
            settings.cameraMargins = {2, 1, 1, 3}; settings.playableWidth = 5; settings.playableHeight = 4;
            rectangle = {2, 1, 7, 5}; icon = {102, 128};
        } else if (mode == 2) {
            settings.flags &= ~0x400u;
            settings.cameraBounds = {128, 384, 768, 896, 128, 896, 768, 384};
            rectangle = {1, 3, 6, 7}; icon = {153, 230};
        }
        const auto finalSettings = Take(settings.Serialize());
        Require(fixture.edits.EditSerialized(MapDataKind::Settings, finalSettings).Succeeded(), "真实设置事务改变最终W3I");
        MapSaveTask task; fixture.Freeze(task);
        if (mode == 2) {
            task.inPlace = true; task.target = fixture.source.SourcePath();
            task.inPlaceOptions.backupDirectory = fixture.directory.Path() / "backups";
        }
        const auto markers = Take(TerrainMinimapMarkers::Capture(*task.archive, fixture.gameRoot,
            task.replacements, task.removals));
        const auto settingsDependency = std::find_if(markers->Dependencies().begin(), markers->Dependencies().end(), [](const auto& input) {
            return input.name == "WAR3MAP.W3I";
        });
        Require(settingsDependency != markers->Dependencies().end()
            && settingsDependency->sha256 == Take(Sha256(finalSettings)), "MMP也必须消费同次最终W3I正文");
        auto plan = std::make_shared<TerrainSavePlan>(Take(TerrainSavePlan::Prepare(fixture.source, fixture.edits)));
        Require(markers->AdmitToPlan(*plan, fixture.edits).Succeeded(), "真实MMP owner进入同次保存计划");
        task.replacements.push_back({"war3map.mmp", markers->Bytes()});
        const auto preparation = Take(TerrainMinimapSave::PrepareAutomaticOrPreserve(task,
            fixture.edits, fixture.gameRoot, {}, plan));
        Require(preparation.generated && !preparation.preserved,
            "只改名称或合法可玩范围必须生成MapBLP，不能退回保留旧图的能力警告");
        const auto& prepared = preparation.generated;
        Require(prepared->ClassicManifest()->sourceRectangle == rectangle,
            "MapBLP必须按最终设置使用与MMP相同的原版可玩区");
        if (mode == 0) Require(prepared->MinimapReplacement().data == originalBlp
            && prepared->ClassicManifest()->postprocessFacts.noiseSeed == originalNoise,
            "只改名称、作者、保存次数不能改变同内容的小地图像素或固定噪点");
        task.replacements.push_back(prepared->MinimapReplacement());
        Require(prepared->VerifyCurrent(task, fixture.edits, fixture.gameRoot).Succeeded(), "最终设置当前代际通过同一复核");
        task.beforePublish = [&] {
            auto checked = prepared->VerifyFrozenOutput(task);
            if (checked.Succeeded()) checked = markers->VerifyFrozenOutput(*task.archive, task.replacements, task.removals);
            return checked;
        };
        MapSaveService::Run(task);
        Require(task.succeeded && task.nextSession, "最终设置、MapBLP与MMP必须经过正式worker保存成功");
        const auto saved = Take(MapArchiveSession::Open(task.target));
        Require(prepared->VerifyReadback(task, saved).Succeeded() && markers->VerifyReadback(saved).Succeeded()
            && Take(saved.ReadMember("war3map.w3i")) == finalSettings,
            "独立Open必须同时核对最终W3I、MapBLP、MMP及其真实来源");
        const auto mmp = Take(ParseMapMinimapMarkers(Take(saved.ReadMember("war3map.mmp"))));
        Require(mmp.records.size() == 1 && mmp.records[0].typeRaw == 2 && mmp.records[0].positionRaw == icon
            && mmp.records[0].colorBgra == std::array<std::uint8_t, 4>{3, 2, 1, 255},
            "原创开始位置在最终矩形中须得到独立手算的图标坐标及实际贴图颜色");
        if (task.inPlace) Require(!task.backup.empty() && Take(Sha256File(task.backup)) == sourceSha,
            "原地保存的真实备份必须等于修改前来源");
        else Require(Take(Sha256File(fixture.source.SourcePath())) == sourceSha, "另存不修改源图");
    }
}

void AutomaticFinalSettingsDriftIsRejectedBeforePublish() {
    for (const auto mode : {0, 1, 2, 3}) {
        auto original = W3i(); Put32(original, 72, 0x400);
        Fixture fixture(false, false, false, original, false, {}, true);
        const auto sourceSha = Take(Sha256File(fixture.source.SourcePath()));
        auto settings = Take(MapSettings::Parse(original)); settings.mapName = "Final settings before publish";
        const auto finalSettings = Take(settings.Serialize());
        Require(fixture.edits.EditSerialized(MapDataKind::Settings, finalSettings).Succeeded(), "建立最终设置会话");
        MapSaveTask task; fixture.Freeze(task);
        const auto prepared = fixture.Prepare(task, TerrainMinimapSaveChoice::Automatic);
        task.replacements.push_back(prepared->MinimapReplacement());
        task.beforePublish = [&] {
            const auto current = std::find_if(task.replacements.begin(), task.replacements.end(), [](const auto& member) {
                return member.name == "war3map.w3i";
            });
            Require(current != task.replacements.end(), "发布前必须仍有最终W3I");
            if (mode == 0) { auto changed = settings; changed.saves += 1; current->data = Take(changed.Serialize()); }
            else if (mode == 1) task.replacements.erase(current);
            else if (mode == 2) { task.replacements.erase(current); task.removals.push_back("war3map.w3i"); }
            else current->data = Text("damaged-final-w3i");
            return prepared->VerifyFrozenOutput(task);
        };
        MapSaveService::Run(task);
        Require(!task.succeeded && !task.nextSession && !std::filesystem::exists(task.target),
            "发布前W3I漂移、遗漏、删除或损坏不得发布部分地图");
        RequireCode(task.diagnostics, mode == 1 ? "TERRAIN_ASSETS_FINAL_WINNER"
            : mode == 2 ? "TERRAIN_ASSETS_FINAL_REMOVAL" : "TERRAIN_ASSETS_FINAL_BYTES");
        Require(Take(Sha256File(fixture.source.SourcePath())) == sourceSha && fixture.edits.IsDirty(),
            "发布失败必须保持源图和前台未保存编辑");
    }
}

void AutomaticFinalSettingsStillProtectsAuthorImages() {
    for (const auto tga : {false, true}) {
        auto original = W3i(); Put32(original, 72, 0x400);
        Fixture fixture(true, false, false, original, false,
            tga ? std::optional<Bytes>{Text("author TGA bytes")} : std::nullopt, true, false, !tga);
        const auto imageName = tga ? "war3mapMap.tga" : "war3mapMap.blp";
        const auto image = Take(fixture.source.ReadMember(imageName));
        auto settings = Take(MapSettings::Parse(original)); settings.mapName = "Authored image settings edit";
        const auto finalSettings = Take(settings.Serialize());
        Require(fixture.edits.EditSerialized(MapDataKind::Settings, finalSettings).Succeeded(), "作者图像保护下正常修改设置");
        MapSaveTask task; fixture.Freeze(task);
        Require(!Take(TerrainMinimapSave::RequiresAutomaticGeneration(task)), "设置变化不能覆盖已有TGA或导入清单声明的BLP");
        const auto prepared = Take(TerrainMinimapSave::PrepareAutomaticOrPreserve(task, fixture.edits, fixture.gameRoot));
        Require(!prepared.generated, "受保护图像不启动自动生成器");
        MapSaveService::Run(task);
        Require(task.succeeded && task.nextSession, "受保护图像不阻止普通设置保存");
        const auto saved = Take(MapArchiveSession::Open(task.target));
        Require(Take(saved.ReadMember(imageName)) == image && Take(saved.ReadMember("war3map.w3i")) == finalSettings,
            "独立回读确认作者图像原字保留且最终设置真实写出");
        if (!tga) Require(Take(saved.ReadMember("war3map.imp")) == ImportedMinimap(), "作者BLP的导入声明保持原字");
    }
}

void AutomaticInvalidFinalRectangleRemainsARealFailure() {
    auto original = W3i(); Put32(original, 72, 0x400);
    Fixture fixture(true, false, false, original, false, {}, true);
    auto settings = Take(MapSettings::Parse(original)); settings.flags &= ~0x400u;
    const auto finalSettings = Take(settings.Serialize());
    Require(fixture.edits.EditSerialized(MapDataKind::Settings, finalSettings).Succeeded(), "建立可序列化但空相机矩形的设置");
    MapSaveTask task; fixture.Freeze(task);
    Rejected(TerrainMinimapSave::PrepareAutomaticOrPreserve(task, fixture.edits, fixture.gameRoot),
        "CLASSIC_MINIMAP_PLAYABLE_EMPTY");
    Require(!task.executed && !std::filesystem::exists(task.target), "无效最终矩形不能以保留原图警告掩盖");
}

} // namespace

int main() {
    try {
        PreserveAndEarlyRejections(); FinalListRevisionAndWinnersAreReal(); PersistentArchiveSaveCopyAndIndependentOpen();
        DeclaredChoiceRequiresActualW3iAndRetainsStandardEdges();
        ExistingTgaCannotBeShadowedByGeneratedBlp();
        AutomaticDecisionUsesExactSourceMembers();
        AutomaticGeneratedBlpIsVerifiedThroughSaveCopyAndInPlace();
        AutomaticResourceOverlaySavesAndReadsBack();
        AutomaticReusesVerifiedPlanAndPreservesOnBudgetFailure();
        AutomaticFinalSettingsAndMarkersShareTheSavedView();
        AutomaticFinalSettingsDriftIsRejectedBeforePublish();
        AutomaticFinalSettingsStillProtectsAuthorImages();
        AutomaticInvalidFinalRectangleRemainsARealFailure();
        std::cout << "Terrain missing-only minimap save tests passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
