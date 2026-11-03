#include "War3MapCore/Services/TerrainMinimapSave.h"
#include "War3MapCore/Services/MinimapPreview.h"
#include "War3MapCore/Services/ArchiveExpand.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/IO/FileBytes.h"
#include "War3MapCore/MapData/TerrainEditing.h"
#include "../support/MinimapPreviewFixture.h"

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
using War3Map::Tests::MinimapPreviewFixtures::Text;
using War3Map::Tests::MinimapPreviewFixtures::Put32;
using War3Map::Tests::MinimapPreviewFixtures::Slk;
using War3Map::Tests::MinimapPreviewFixtures::Atlas;
using War3Map::Tests::MinimapPreviewFixtures::Materials;
using War3Map::Tests::MinimapPreviewFixtures::MapMembers;
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
    const auto preview = [&](Session& session,std::uint64_t revision) {
        CurrentMinimapPreviewInput input;input.archive=std::make_shared<MapArchiveSession>(Take(MapArchiveSession::Open(firstPath)));
        input.edits=Take(session.edits.FreezeForSave(128U*1024U*1024U));input.gameRoot=gameRoot;input.documentRevision=revision;
        return Take(GenerateCurrentMinimapPreview(input));
    };
    const auto viewBefore=preview(first,7);
    Require(viewBefore.current && viewBefore.documentRevision==7 && viewBefore.image.rgba==Take(Image::DecodeBlp(baseline.blp)).rgba,
        "只读当前视图与真实保存编码解码像素逐字节相同");
    Require(!first.edits.IsDirty(),"只读生成不产生编辑");
    const auto saved=directory.Path()/"preview-saved.w3x";
    auto savedMembers=members;savedMembers.push_back({"war3mapMap.blp",baseline.blp});Take(WriteVerifiedArchive(saved,savedMembers));
    const auto savedSha=Take(Sha256File(saved));
    const auto savedView=Take(OpenMinimapPreview(saved,savedSha));
    Require(savedView.image.rgba==viewBefore.image.rgba,"保存后当前与已保存像素一致");
    Session previewEdits(firstPath,members);
    TerrainBrush brush;brush.kind=TerrainBrushKind::Raise;brush.radius=1;brush.strength=64;
    const std::array<TerrainStrokePoint,1> points{{{384,384}}};
    const auto plan=Take(previewEdits.edits.PrepareTerrainEdit(brush,points,false));
    Require(previewEdits.edits.ApplyPreparedTerrainEdit(plan,[](const auto&){return Status::Ok();}).Succeeded(),"应用原创未保存高度笔");
    const auto editedView=preview(previewEdits,8);
    Require(editedView.rgbaSha256!=viewBefore.rgbaSha256,"未保存地形变化影响当前视图");
    Require(Take(OpenMinimapPreview(saved,savedSha)).rgbaSha256==savedView.rgbaSha256,"未保存修改不影响已保存视图");
    const auto edited=Generate(previewEdits,gameRoot,directory.Path()/"edited-preview.w3x");
    Require(editedView.image.rgba==Take(Image::DecodeBlp(edited.blp)).rgba,"含未保存修改的视图仍与保存服务像素相同");
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
