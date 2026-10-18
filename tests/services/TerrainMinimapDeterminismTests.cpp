#include "War3MapCore/Services/TerrainMinimapSave.h"
#include "War3MapCore/Services/ArchiveExpand.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/IO/FileBytes.h"
#include "War3MapCore/MapData/TerrainEditing.h"

#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace War3Map;
using namespace War3Map::MapData;
using namespace War3Map::Services;
using Bytes = std::vector<std::byte>;

void Require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
template<class T> T Take(Result<T> result) {
    if (!result.Succeeded() || !result.value) {
        std::string message = "固定噪点原创样例失败";
        for (const auto& diagnostic : result.diagnostics) message += " | " + diagnostic.code + ":" + diagnostic.message;
        throw std::runtime_error(message);
    }
    return std::move(*result.value);
}
class Directory {
public:
    Directory() {
        static std::atomic<unsigned> next{0};
        path_ = std::filesystem::temp_directory_path() / ("Audastra-minimap-determinism-"
            + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()) + "-" + std::to_string(next++));
        Require(std::filesystem::create_directory(path_), "创建独立固定噪点样例目录");
    }
    ~Directory() { std::error_code error; std::filesystem::remove_all(path_, error); }
    const std::filesystem::path& Path() const { return path_; }
private:
    std::filesystem::path path_;
};
Bytes Text(std::string_view text) {
    Bytes bytes;
    for (const auto value : text) bytes.push_back(static_cast<std::byte>(value));
    return bytes;
}
void Put32(Bytes& bytes, std::size_t offset, std::uint32_t value) {
    Require(offset <= bytes.size() && bytes.size() - offset >= 4, "原创样例字段有界");
    for (unsigned index = 0; index < 4; ++index) bytes[offset + index] = static_cast<std::byte>((value >> (index * 8)) & 255);
}
Bytes Slk(const std::vector<std::string>& headers, const std::vector<std::vector<std::string>>& rows = {}) {
    std::string text = "ID;PWXL;N;E\n";
    const auto row = [&](const std::vector<std::string>& values, std::size_t y) {
        for (std::size_t x = 0; x < values.size(); ++x)
            text += "C;X" + std::to_string(x + 1) + ";Y" + std::to_string(y) + ";K\"" + values[x] + "\"\n";
    };
    row(headers, 1);
    for (std::size_t index = 0; index < rows.size(); ++index) row(rows[index], index + 2);
    return Text(text + "E\n");
}
Bytes Atlas(std::uint8_t red = 80) {
    // 手写原创 BLP1 palette+mips；输入不由被测JPEG编码器生成。
    Bytes bytes(1180 + 341);
    const auto magic = Text("BLP1"); std::copy(magic.begin(), magic.end(), bytes.begin());
    Put32(bytes, 4, 1); Put32(bytes, 12, 16); Put32(bytes, 16, 16); Put32(bytes, 20, 5); Put32(bytes, 24, 1);
    bytes[156] = std::byte{180}; bytes[157] = std::byte{120}; bytes[158] = static_cast<std::byte>(red);
    std::size_t offset = 1180;
    for (std::uint32_t level = 0, side = 16;; ++level, side /= 2) {
        Put32(bytes, 28 + level * 4, static_cast<std::uint32_t>(offset)); Put32(bytes, 92 + level * 4, side * side);
        offset += static_cast<std::size_t>(side) * side;
        if (side == 1) break;
    }
    return bytes;
}
std::vector<MapArchiveMember> Materials() {
    return {{"TerrainArt\\Terrain.slk", Slk({"tileID", "dir", "file"}, {{"Tg01", "Fixture", "Ground"}})},
        {"TerrainArt\\CliffTypes.slk", Slk({"cliffID", "texDir", "texFile", "cliffModelDir", "rampModelDir", "groundTile", "cliffClass"},
            {{"CLdi", "Fixture", "Cliff", "Cliff", "Ramp", "Tg01", "0"}})},
        {"TerrainArt\\Water.slk", Slk({"waterID"})}, {"Fixture\\Ground.blp", Atlas()},
        {"Fixture\\Cliff.blp", Atlas()}, {"UI\\MiscData.txt", Text("[Water]\nDeepLevelPathing=64\n")},
        {"UI\\WorldEditData.txt", Text("[CameraMargins]\nLeft=0\nRight=0\nBottom=0\nTop=0\n")}};
}
std::vector<MapArchiveMember> MapMembers() {
    TerrainGridSnapshot grid;
    grid.header.width = grid.header.height = 9; grid.header.mainTileset = 'L';
    grid.header.groundTilesets = {"Tg01"}; grid.header.cliffTilesets = {"CLdi"}; grid.vertices.resize(81);
    for (auto& vertex : grid.vertices) {
        vertex.groundHeightRaw = 8192; vertex.waterHeightAndFlagsRaw = 7936; vertex.cliffAndLayerRaw = 2;
    }
    RefreshTerrainDerived(grid, 0, 0, 9, 9);
    Bytes settings(147); Put32(settings, 0, 25); Put32(settings, 8, 6061);
    Put32(settings, 48, 1); Put32(settings, 52, 3); Put32(settings, 56, 2); Put32(settings, 60, 1);
    Put32(settings, 64, 4); Put32(settings, 68, 5); Put32(settings, 72, 0x400); settings[76] = std::byte{'L'};
    auto pathing = Text("MP3W"); pathing.resize(16 + 32 * 32, std::byte{0x82});
    Put32(pathing, 4, 0); Put32(pathing, 8, 32); Put32(pathing, 12, 32);
    auto members = Materials();
    members.push_back({"war3map.w3e", Take(SerializeMapTerrain(grid))});
    members.push_back({"war3map.wpm", std::move(pathing)});
    members.push_back({"war3map.w3i", std::move(settings)});
    Bytes native(12); Put32(native, 0, 1); members.push_back({"war3map.w3u", std::move(native)});
    members.push_back({"unrelated.bin", Text("unrelated first value")});
    return members;
}
Bytes& Member(std::vector<MapArchiveMember>& members, std::string_view name) {
    const auto found = std::find_if(members.begin(), members.end(), [&](const auto& member) { return member.name == name; });
    Require(found != members.end(), "原创样例成员必须存在");
    return found->data;
}
const Bytes& Member(const std::vector<MapArchiveMember>& members, std::string_view name) {
    const auto found = std::find_if(members.begin(), members.end(), [&](const auto& member) { return member.name == name; });
    Require(found != members.end(), "原创只读样例成员必须存在");
    return found->data;
}
MapDocument SettingsDocument(const std::vector<MapArchiveMember>& members) {
    const std::vector<MapArchiveMember> settings{{"war3map.w3i", Member(members, "war3map.w3i")}};
    return MapDocument::FromMembers(settings);
}
struct Session {
    MapArchiveSession source;
    MapDataEditSession edits;
    explicit Session(const std::filesystem::path& path, const std::vector<MapArchiveMember>& members)
        : source(Take(MapArchiveSession::Open(path))), edits(SettingsDocument(members)) {
        Require(edits.AttachTerrain(Member(members, "war3map.w3e")).Succeeded()
            && edits.AttachPathing(Member(members, "war3map.wpm")).Succeeded(), "附上原创地形和通行基线");
    }
};
struct Generated {
    Bytes blp;
    std::uint32_t seed = 0;
    std::string sourceSha256, preNoiseSha256, finalSha256;
};
Generated Generate(Session& session, const std::filesystem::path& gameRoot, const std::filesystem::path& target) {
    MapSaveTask task;
    Require(task.FreezeArchive(session.source).Succeeded(), "真实FreezeArchive拥有来源");
    task.target = target; task.replacements = session.edits.Replacements();
    const auto prepared = Take(TerrainMinimapSave::Prepare(task, session.edits, gameRoot, TerrainMinimapSaveChoice::Automatic));
    Require(prepared && prepared->ClassicManifest(), "服务实际生成Classic小地图");
    const auto& manifest = *prepared->ClassicManifest();
    Require(manifest.postprocessFacts.noiseSeed.has_value()
        && manifest.postprocessFacts.noiseSource == Render::TerrainMinimapNoiseSource::Lcg32V1
        && manifest.postprocessFacts.noisePercent == 10, "manifest记录实际固定种子和10%噪点");
    Require(manifest.blpSha256 == Take(Sha256(prepared->MinimapReplacement().data)), "实际BLP与清单摘要相符");
    return {prepared->MinimapReplacement().data, *manifest.postprocessFacts.noiseSeed,
        manifest.sourceSha256, manifest.preNoiseRgbaSha256, manifest.finalRgbaSha256};
}
void Same(const Generated& expected, const Generated& actual, const char* message) {
    Require(expected.seed == actual.seed && expected.preNoiseSha256 == actual.preNoiseSha256
        && expected.finalSha256 == actual.finalSha256 && expected.blp == actual.blp, message);
}
void DeterministicGeneratedBytes() {
    Directory directory; auto members = MapMembers();
    const auto gameRoot = directory.Path() / "game";
    Require(std::filesystem::create_directory(gameRoot), "创建原创游戏来源目录");
    Take(WriteVerifiedArchive(gameRoot / "war3.mpq", Materials()));
    const auto firstPath = directory.Path() / "first.w3x";
    Take(WriteVerifiedArchive(firstPath, members));
    Session first(firstPath, members);
    const auto baseline = Generate(first, gameRoot, directory.Path() / "first-output.w3x");
    Same(baseline, Generate(first, gameRoot, directory.Path() / "other-output.w3x"), "同会话连续生成和不同另存目标必须逐字节一致");
    Session independent(firstPath, members);
    Same(baseline, Generate(independent, gameRoot, directory.Path() / "independent-output.w3x"), "独立编辑会话不能改变固定噪点");

    const auto movedPath = directory.Path() / "different-directory";
    Require(std::filesystem::create_directory(movedPath), "创建第二份原创地图目录");
    Take(WriteVerifiedArchive(movedPath / "renamed.w3x", members));
    Session moved(movedPath / "renamed.w3x", members);
    Same(baseline, Generate(moved, gameRoot, directory.Path() / "moved-output.w3x"), "相同成员内容在不同地图路径必须生成相同BLP");

    auto unrelated = members;
    Member(unrelated, "unrelated.bin") = Text("a different unrelated payload");
    Put32(Member(unrelated, "war3map.w3i"), 4, 17); // W3I保存计数不参与小地图取色或裁剪。
    Take(WriteVerifiedArchive(directory.Path() / "unrelated.w3x", unrelated));
    Session unrelatedSession(directory.Path() / "unrelated.w3x", unrelated);
    const auto unrelatedResult = Generate(unrelatedSession, gameRoot, directory.Path() / "unrelated-output.w3x");
    Require(baseline.sourceSha256 != unrelatedResult.sourceSha256, "负控实际改变整档来源身份");
    Same(baseline, unrelatedResult, "无关成员和W3I保存次数不能让相同小地图抖动");

    const auto expandedPath = directory.Path() / "expanded";
    const auto expanded = Take(ExpandArchive(firstPath, first.source.SourceSha256(), expandedPath));
    Require(expanded.writtenMembers >= members.size(), "实际展开全部原创具名成员");
    auto fromExpanded = members;
    for (auto& member : fromExpanded) {
        member.data = Take(IO::ReadFileBytes(expandedPath / std::filesystem::path(member.name)));
        Require(member.data == Member(members, member.name), "展开后的每份内容与源归档相同");
    }
    Take(WriteVerifiedArchive(directory.Path() / "from-expanded.w3x", fromExpanded));
    Session expandedSession(directory.Path() / "from-expanded.w3x", fromExpanded);
    Same(baseline, Generate(expandedSession, gameRoot, directory.Path() / "expanded-output.w3x"),
        "实际展开再封装的同内容输入不能改变小地图字节");

    auto changed = members; Member(changed, "Fixture\\Ground.blp") = Atlas(160);
    Take(WriteVerifiedArchive(directory.Path() / "changed-ground.w3x", changed));
    Session changedSession(directory.Path() / "changed-ground.w3x", changed);
    const auto changedResult = Generate(changedSession, gameRoot, directory.Path() / "changed-output.w3x");
    Require(baseline.seed != changedResult.seed && baseline.preNoiseSha256 != changedResult.preNoiseSha256
        && baseline.blp != changedResult.blp, "相关地面资源内容变化必须影响种子和实际小地图");

    auto crop = members; Put32(Member(crop, "war3map.w3i"), 48, 2);
    Take(WriteVerifiedArchive(directory.Path() / "changed-crop.w3x", crop));
    Session cropSession(directory.Path() / "changed-crop.w3x", crop);
    const auto cropResult = Generate(cropSession, gameRoot, directory.Path() / "crop-output.w3x");
    Require(baseline.seed != cropResult.seed && baseline.blp != cropResult.blp, "实际可玩区变化必须影响种子和小地图");
}
} // namespace

int main() {
    try {
        DeterministicGeneratedBytes();
        std::cout << "TerrainMinimapDeterminismTests: passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "TerrainMinimapDeterminismTests: " << error.what() << '\n';
        return 1;
    }
}
