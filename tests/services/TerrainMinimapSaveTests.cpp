#include "War3MapCore/Services/TerrainMinimapSave.h"

#include "War3MapCore/Hash.h"
#include "War3MapCore/IO/FileBytes.h"
#include "War3MapCore/Image/BlpImage.h"
#include "War3MapCore/MapData/TerrainEditing.h"

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
template<class T> void Rejected(const Result<T>& result, std::string_view code = {}) {
    Require(!result.Succeeded() && !result.value && !result.diagnostics.empty(), "拒绝不能返回部分小地图保存资格");
    if (!code.empty()) Require(Code(result.diagnostics, code), "必须命中预期的具体拒绝门");
}
void Rejected(const Status& status, std::string_view code = {}) {
    Require(!status.Succeeded() && !status.diagnostics.empty(), "复核必须明确失败");
    if (!code.empty()) Require(Code(status.diagnostics, code), "复核必须命中预期的具体拒绝门");
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
    MapDataEditSession edits{MapDocument::FromMembers({})};
    std::filesystem::path gameRoot;
    MapArchiveSession source;
    explicit Fixture(bool presentMinimap = false, bool gameWinner = false, bool colorful = false) {
        TerrainGridSnapshot terrain;
        terrain.header.width = terrain.header.height = 9; terrain.header.mainTileset = 'L';
        terrain.header.groundTilesets = {"Tg01"}; terrain.header.cliffTilesets = {"CLdi"}; terrain.vertices.resize(81);
        for (auto& vertex : terrain.vertices) {
            vertex.groundHeightRaw = 8192; vertex.waterHeightAndFlagsRaw = 7936; vertex.cliffAndLayerRaw = 2;
        }
        RefreshTerrainDerived(terrain, 0, 0, 9, 9); w3e = Take(SerializeMapTerrain(terrain));
        wpm = Text("MP3W"); wpm.resize(16 + 32 * 32, std::byte{0x82}); Put32(wpm, 4, 0); Put32(wpm, 8, 32); Put32(wpm, 12, 32);
        Require(edits.AttachTerrain(w3e).Succeeded() && edits.AttachPathing(wpm).Succeeded(), "附上真实原始W3E/WPM");
        auto members = gameWinner ? std::vector<MapArchiveMember>{} : Materials(colorful);
        members.push_back({"war3map.w3e", w3e}); members.push_back({"war3map.wpm", wpm});
        members.push_back({"war3map.j", Text("function config takes nothing returns nothing\nendfunction\nfunction main takes nothing returns nothing\nendfunction\n")});
        Bytes native(12); Put32(native, 0, 1); members.push_back({"war3map.w3u", native});
        members.push_back({"war3map.shd", {std::byte{17}, std::byte{25}}});
        members.push_back({"unrelated.bin", Text("original unrelated member")});
        if (presentMinimap) members.push_back({"war3mapMap.blp", {}});
        Take(WriteVerifiedArchive(directory.Path() / "source.w3x", members));
        source = Take(MapArchiveSession::Open(directory.Path() / "source.w3x"));
        gameRoot = directory.Path() / "game"; Require(std::filesystem::create_directory(gameRoot), "创建原创game来源目录");
        Take(WriteVerifiedArchive(gameRoot / "war3.mpq", Materials(colorful)));
    }
    void Stroke(TerrainBrushKind kind = TerrainBrushKind::Raise, float strength = 10.0f) {
        TerrainBrush brush; brush.kind = kind; brush.radius = 0; brush.strength = strength;
        const TerrainStrokePoint point{512, 512};
        Require(edits.EditTerrain(brush, std::span(&point, 1), false).Succeeded(), "应用真实height/blight笔刷事务");
    }
    void Freeze(MapSaveTask& task) {
        Require(task.FreezeArchive(source).Succeeded(), "FreezeArchive拥有实际来源");
        task.target = directory.Path() / "candidate.w3x"; task.replacements = edits.Replacements();
    }
    std::shared_ptr<const TerrainMinimapSave> Prepare(MapSaveTask& task) {
        return Take(TerrainMinimapSave::Prepare(task, edits, gameRoot, TerrainMinimapSaveChoice::GenerateMissingGroundOnly));
    }
};

void PreserveAndEarlyRejections() {
    MapSaveTask empty; MapDataEditSession noEdits{MapDocument::FromMembers({})};
    Require(!Take(TerrainMinimapSave::Prepare(empty, noEdits, {})), "默认Preserve成功null且不访问无效源/gameRoot");
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
    Fixture overHeight; overHeight.Stroke(TerrainBrushKind::Raise, 40); MapSaveTask heightTask; overHeight.Freeze(heightTask);
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
    Require(task.nextSession && task.written == task.target && !task.scriptRebuilt,
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

} // namespace

int main() {
    try {
        PreserveAndEarlyRejections(); FinalListRevisionAndWinnersAreReal(); PersistentArchiveSaveCopyAndIndependentOpen();
        std::cout << "Terrain missing-only minimap save tests passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
