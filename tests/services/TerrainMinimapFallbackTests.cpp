#include "War3MapCore/Services/TerrainMinimapSave.h"

#include "War3MapCore/MapData/MapPlacements.h"
#include "War3MapCore/MapData/MapSettings.h"
#include "War3MapCore/MapData/TerrainEditing.h"
#include "War3MapCore/IO/FileBytes.h"

#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <initializer_list>
#include <iostream>
#include <stdexcept>

namespace {
using namespace War3Map;
using namespace War3Map::MapData;
using namespace War3Map::Services;
using Bytes = std::vector<std::byte>;
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class T> T Take(Result<T> result) {
    if (!result.Succeeded() || !result.value) {
        std::string message = "原创小地图降级样例失败";
        for (const auto& diagnostic : result.diagnostics) message += " | " + diagnostic.code;
        throw std::runtime_error(message);
    }
    return std::move(*result.value);
}
bool HasCode(const std::vector<Diagnostic>& diagnostics, std::string_view code) {
    return std::any_of(diagnostics.begin(), diagnostics.end(),
        [&](const auto& diagnostic) { return diagnostic.code == code; });
}
Bytes Text(std::string_view text) {
    const auto* begin = reinterpret_cast<const std::byte*>(text.data()); return {begin, begin + text.size()};
}
void Put32(Bytes& bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned byte = 0; byte < 4; ++byte) bytes.at(offset + byte) = static_cast<std::byte>((value >> (byte * 8)) & 255);
}
Bytes Slk(std::initializer_list<std::string_view> rows) {
    std::string output = "ID;PWXL;N;E\n"; std::size_t y = 1;
    for (const auto row : rows) {
        std::size_t x = 1, begin = 0;
        while (begin <= row.size()) {
            const auto end = row.find('|', begin);
            const auto value = row.substr(begin, end == std::string_view::npos ? row.size() - begin : end - begin);
            output += "C;X" + std::to_string(x++) + ";Y" + std::to_string(y) + ";K\"" + std::string(value) + "\"\n";
            if (end == std::string_view::npos) break;
            begin = end + 1;
        }
        ++y;
    }
    return Text(output + "E\n");
}
Bytes Atlas() {
    Bytes bytes(1180 + 341); const auto magic = Text("BLP1"); std::copy(magic.begin(), magic.end(), bytes.begin());
    Put32(bytes, 4, 1); Put32(bytes, 12, 16); Put32(bytes, 16, 16); Put32(bytes, 20, 5);
    bytes[160] = std::byte{90}; bytes[161] = std::byte{120}; bytes[162] = std::byte{150};
    std::size_t offset = 1180;
    for (std::uint32_t level = 0, size = 16; level < 5; ++level, size /= 2) {
        Put32(bytes, 28 + level * 4, static_cast<std::uint32_t>(offset)); Put32(bytes, 92 + level * 4, size * size);
        std::fill_n(bytes.begin() + static_cast<std::ptrdiff_t>(offset), size * size, std::byte{1}); offset += size * size;
    }
    return bytes;
}
struct Fixture {
    std::filesystem::path directory;
    Bytes w3e, wpm, w3i, doo, originalMinimap = Text("author minimap bytes must stay exact");
    MapArchiveSession archive;
    std::unique_ptr<MapDataEditSession> edits;
    bool hadMinimap;
    explicit Fixture(bool special = false, bool minimap = true, bool tga = false) : hadMinimap(minimap) {
        static std::atomic<unsigned> sequence{0};
        directory = std::filesystem::temp_directory_path() / ("Audastra-minimap-fallback-"
            + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()) + "-" + std::to_string(sequence++));
        Require(std::filesystem::create_directory(directory), "创建原创降级样例目录");
        TerrainGridSnapshot terrain;
        terrain.header.width = terrain.header.height = 9; terrain.header.mainTileset = 'L';
        terrain.header.groundTilesets = {"Tg01"}; terrain.header.cliffTilesets = {"CLdi"}; terrain.vertices.resize(81);
        for (auto& vertex : terrain.vertices) {
            vertex.groundHeightRaw = 8192; vertex.waterHeightAndFlagsRaw = 7936; vertex.cliffAndLayerRaw = 2;
        }
        RefreshTerrainDerived(terrain, 0, 0, 9, 9); w3e = Take(SerializeMapTerrain(terrain));
        wpm = Text("MP3W"); wpm.resize(16 + 32 * 32); Put32(wpm, 8, 32); Put32(wpm, 12, 32);
        w3i.resize(147); Put32(w3i, 0, 25); Put32(w3i, 8, 6061);
        Put32(w3i, 48, 1); Put32(w3i, 52, 3); Put32(w3i, 56, 2); Put32(w3i, 60, 1);
        Put32(w3i, 64, 4); Put32(w3i, 68, 5); Put32(w3i, 72, 0x400); w3i[76] = std::byte{'L'};
        DoodadPlacements doodads;
        if (special) { SpecialDoodadPlacement placement; placement.rawcode = "SPC1"; doodads.specialDoodads.push_back(placement); }
        doo = Take(doodads.Serialize());
        std::vector<MapArchiveMember> members{{"war3map.w3e", w3e}, {"war3map.wpm", wpm}, {"war3map.w3i", w3i},
            {"war3map.doo", doo}, {"TerrainArt\\Terrain.slk", Slk({"tileID|dir|file", "Tg01|Fixture|Ground"})},
            {"TerrainArt\\CliffTypes.slk", Slk({"cliffID|texDir|texFile|cliffModelDir|rampModelDir|groundTile|cliffClass", "CLdi|Fixture|Cliff|Cliff|Ramp|Tg01|0"})},
            {"TerrainArt\\Water.slk", Slk({"waterID"})}, {"Fixture\\Ground.blp", Atlas()}, {"Fixture\\Cliff.blp", Atlas()},
            {"UI\\MiscData.txt", Text("[Water]\nDeepLevelPathing=64\n")},
            {"UI\\WorldEditData.txt", Text("[CameraMargins]\nLeft=0\nRight=0\nBottom=0\nTop=0\n")}};
        if (minimap) members.push_back({"war3mapMap.blp", originalMinimap});
        if (tga) members.push_back({"war3mapMap.tga", Text("author custom TGA bytes")});
        Take(WriteVerifiedArchive(directory / "source.w3x", members)); archive = Take(MapArchiveSession::Open(directory / "source.w3x"));
        edits = std::make_unique<MapDataEditSession>(MapDocument::FromMembers(archive.Members(), archive.SourcePath()));
        Require(edits->AttachTerrain(w3e).Succeeded() && edits->AttachPathing(wpm).Succeeded(), "附上原生地形通行来源");
    }
    ~Fixture() { std::error_code ignored; std::filesystem::remove_all(directory, ignored); }
    void Freeze(MapSaveTask& task, bool inPlace = false) {
        Require(task.FreezeArchive(archive).Succeeded(), "冻结真实来源"); task.inPlace = inPlace;
        task.target = inPlace ? archive.SourcePath() : directory / "saved.w3x";
        // 新增资源触发自动重建，内容不改变原地形/DOO；所有资源均为原创。
        task.replacements.push_back({"Fixture\\Unused.blp", Text("new fixture resource")});
    }
};
void PreserveThroughActualSave(bool present, bool inPlace) {
    Fixture fixture(true, present); MapSaveTask task; fixture.Freeze(task, inPlace);
    auto result = TerrainMinimapSave::PrepareAutomaticOrPreserve(task, *fixture.edits, {});
    Require(result.Succeeded() && result.value && !result.value->generated && result.value->preserved,
        "真实特殊装饰物不支持时取得保留资格");
    Require(HasCode(result.diagnostics, "CLASSIC_OBJECT_CAPTURE_SPECIAL_UNRESOLVED")
        && HasCode(result.diagnostics, "MINIMAP_GENERATION_SKIPPED"), "保留原始原因并明确跳过生成");
    Require(std::none_of(result.diagnostics.begin(), result.diagnostics.end(),
        [](const auto& diagnostic) { return diagnostic.severity == DiagnosticSeverity::Error; }), "降级后只有警告");
    const auto guard = result.value->preserved;
    Require(guard->VerifyCurrent(task, *fixture.edits).Succeeded(), "保留当前来源闭环");
    task.diagnostics = result.diagnostics;
    task.beforePublish = [&] { return guard->VerifyFrozenOutput(task); };
    MapSaveService::Run(task);
    Require(task.succeeded && task.nextSession, "生成器不支持不阻断已通过检查的其它保存");
    const auto saved = Take(MapArchiveSession::Open(task.target));
    Require(guard->VerifyReadback(task, saved).Succeeded(), "保留小地图独立回读");
    Require(saved.HasMember("war3mapMap.blp") == present && !saved.HasMember("war3mapMap.tga"), "原来缺失仍不写");
    if (present) Require(Take(saved.ReadMember("war3mapMap.blp")) == fixture.originalMinimap, "原小地图逐字不变");
    Require(Take(saved.ReadMember("war3map.doo")) == fixture.doo
        && Take(saved.ReadMember("Fixture\\Unused.blp")) == Text("new fixture resource"), "其它合法成员实际写出");
    if (present && !inPlace) {
        const std::array changed{MemberReplacement{"war3mapMap.blp", Text("unexpected map rewrite")}};
        const auto altered = Take(saved.SaveCandidate(fixture.directory / "altered.w3x", changed));
        Require(IO::WriteFileBytes(task.target, Take(IO::ReadFileBytes(altered.path))).Succeeded(), "只替换自有目标以验证回读字节门");
        const auto changedTarget = Take(MapArchiveSession::Open(task.target));
        Require(!guard->VerifyReadback(task, changedTarget).Succeeded(), "同一目标路径的小地图正文变化不能通过回读");
    }
}
void BudgetAndTga() {
    Fixture fixture; MapSaveTask task; fixture.Freeze(task);
    TerrainMinimapSaveLimits limits; limits.maxWorkingBytes = 1;
    const auto result = Take(TerrainMinimapSave::PrepareAutomaticOrPreserve(task, *fixture.edits, {}, limits));
    Require(!result.generated && result.preserved && result.preserved->VerifyFrozenOutput(task).Succeeded(),
        "真实生成工作预算不足可以保留原小地图");
    Fixture tga(false, true, true); MapSaveTask tgaTask; tga.Freeze(tgaTask);
    const auto protectedTga = Take(TerrainMinimapSave::PrepareAutomaticOrPreserve(tgaTask, *tga.edits, {}));
    Require(!protectedTga.generated && protectedTga.preserved, "已有自定义TGA也返回原字保护");
    tgaTask.beforePublish = [&] { return protectedTga.preserved->VerifyFrozenOutput(tgaTask); };
    MapSaveService::Run(tgaTask);
    Require(tgaTask.succeeded && protectedTga.preserved->VerifyReadback(tgaTask, *tgaTask.nextSession).Succeeded(), "已有TGA保存回读");
    Require(Take(tgaTask.nextSession->ReadMember("war3mapMap.tga")) == Text("author custom TGA bytes"), "自定义TGA原字保留");

    Fixture early; MapSaveTask earlyTask; early.Freeze(earlyTask);
    const auto preflightBudget = TerrainMinimapSave::PreserveUnsupportedAutomatic(earlyTask, *early.edits,
        Status::Failure("MapData.Read", "MAP_SNAPSHOT_BUDGET", "生成私有输入快照超预算").diagnostics);
    Require(preflightBudget.Succeeded() && preflightBudget.value && preflightBudget.value->preserved
        && preflightBudget.value->preserved->VerifyFrozenOutput(earlyTask).Succeeded(), "调用者输入预算失败复用同一降级门");
}
void SourceStaleAndFinalMutationStillReject() {
    Fixture fixture(true); MapSaveTask task; fixture.Freeze(task);
    const auto result = Take(TerrainMinimapSave::PrepareAutomaticOrPreserve(task, *fixture.edits, {}));
    const auto guard = result.preserved;
    task.replacements.push_back({"war3mapMap.blp", Text("wrong regenerated minimap")});
    Require(!guard->VerifyFrozenOutput(task).Succeeded(), "最终列表悄悄改写原BLP必须拒绝"); task.replacements.pop_back();
    task.removals.push_back("war3mapMap.blp"); Require(!guard->VerifyFrozenOutput(task).Succeeded(), "删除原BLP必须拒绝"); task.removals.clear();
    auto settings = Take(MapSettings::Parse(fixture.w3i)); ++settings.saves;
    Require(fixture.edits->EditSettings(settings).Succeeded(), "建立真实编辑修订变化");
    Require(!guard->VerifyCurrent(task, *fixture.edits).Succeeded(), "真实修订变化不可降级复用");
    task.sourceSha256[0] = task.sourceSha256[0] == 'a' ? 'b' : 'a';
    const auto wrongSource = TerrainMinimapSave::PrepareAutomaticOrPreserve(task, *fixture.edits, {});
    Require(!wrongSource.Succeeded() && !wrongSource.value, "source错误绝不能转warning");
    auto unsafe = Status::Failure("fixture", "CLASSIC_OBJECT_CAPTURE_STALE", "真实来源过期");
    const auto stale = TerrainMinimapSave::PreserveUnsupportedAutomatic(task, *fixture.edits, unsafe.diagnostics);
    Require(!stale.Succeeded() && !stale.value && HasCode(stale.diagnostics, "CLASSIC_OBJECT_CAPTURE_STALE"), "stale错误原样拒绝");
    for (const auto* code : {"MAP_SOURCE_UNSAFE", "CLASSIC_OBJECT_CAPTURE_SOURCE_STALE", "MINIMAP_SAVE_SOURCE",
            "TERRAIN_ASSETS_IDENTITY", "TERRAIN_ASSETS_W3I_CURRENT", "MINIMAP_SAVE_AUTOMATIC_EXCEPTION",
            "TERRAIN_ASSETS_W3I_LAYOUT", "CLASSIC_OBJECT_CAPTURE_CURRENT_LAYOUT", "FILE_READ_FAILED"}) {
        const auto failure = TerrainMinimapSave::PreserveUnsupportedAutomatic(task, *fixture.edits,
            Status::Failure("fixture", code, "不能降级的错误").diagnostics);
        Require(!failure.Succeeded() && !failure.value && HasCode(failure.diagnostics, code), "来源身份I/O未知异常不降级");
    }
}
} // namespace
int main() {
    try {
        PreserveThroughActualSave(true, false); PreserveThroughActualSave(false, false); PreserveThroughActualSave(true, true);
        BudgetAndTga(); SourceStaleAndFinalMutationStillReject(); return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
