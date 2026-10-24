#include "War3MapCore/Services/TerrainShadowCasterResources.h"

#include "War3MapCore/IO/FileBytes.h"
#include "War3MapCore/MapData/MapTerrain.h"
#include "War3MapCore/MapData/TerrainEditing.h"
#include "War3MapCore/Mpq/MpqArchive.h"
#include "War3MapCore/ObjectData/NativeObjectCodec.h"

#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <iostream>
#include <limits>
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
        std::string reason;
        for (const auto& diagnostic : result.diagnostics) reason += " | " + diagnostic.code + ":" + diagnostic.message;
        throw std::runtime_error(reason);
    }
    return std::move(*result.value);
}
void Check(Status result) {
    if (result.Succeeded()) return;
    std::string reason;
    for (const auto& diagnostic : result.diagnostics) reason += " | " + diagnostic.code + ":" + diagnostic.message;
    throw std::runtime_error(reason);
}
void Code(std::span<const Diagnostic> diagnostics, std::string_view expected) {
    Require(!diagnostics.empty(), "错误必须带诊断");
    if (expected.empty() || std::any_of(diagnostics.begin(), diagnostics.end(),
        [&](const auto& diagnostic) { return diagnostic.code == expected; })) return;
    std::string reason = "缺少预期错误：" + std::string(expected);
    for (const auto& diagnostic : diagnostics) reason += " | " + diagnostic.code + ":" + diagnostic.message;
    throw std::runtime_error(reason);
}
template<class T> void Rejected(const Result<T>& result, std::string_view code = {}) {
    Require(!result.Succeeded() && !result.value, "错误不得返回部分caster集合"); Code(result.diagnostics, code);
}
void Rejected(const Status& result, std::string_view code = {}) {
    Require(!result.Succeeded(), "来源漂移必须拒绝"); Code(result.diagnostics, code);
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
DoodadPlacement Placement(std::string id, std::int32_t variation, float x, std::int32_t creationId) {
    DoodadPlacement placement; placement.rawcode = std::move(id); placement.variation = variation;
    placement.position = {x, 256, 0}; placement.scale = {2, 3, 4}; placement.rotation = 0.75f;
    placement.creationId = creationId; placement.flags = 2; placement.lifePercent = 100;
    return placement;
}
struct Fixture {
    std::filesystem::path root, game;
    std::vector<MapArchiveMember> members;
    DoodadPlacements placements;
    std::unique_ptr<MapArchiveSession> source;
    unsigned sequence = 0;
    Fixture() {
        static std::atomic<unsigned> next{0};
        root = std::filesystem::temp_directory_path() / ("Audastra-shadow-casters-" + std::to_string(GetCurrentProcessId())
            + "-" + std::to_string(GetTickCount64()) + "-" + std::to_string(next++));
        Require(std::filesystem::create_directory(root), "创建原创临时目录");
        game = root / "game"; Require(std::filesystem::create_directory(game), "创建原创游戏包目录");
        TerrainGridSnapshot grid; grid.header.width = grid.header.height = 9; grid.header.mainTileset = 'L';
        grid.header.groundTilesets = {"G001"}; grid.header.cliffTilesets = {"C001"}; grid.vertices.resize(81);
        for (auto& vertex : grid.vertices) { vertex.groundHeightRaw = 8192; vertex.cliffAndLayerRaw = 2; }
        RefreshTerrainDerived(grid, 0, 0, 9, 9);
        placements.doodads = {Placement("D001", 0, 128, 1), Placement("N001", 0, 256, 2),
            Placement("C001", 1, 384, 3), Placement("C001", 1, 512, 4)};
        placements.doodads.back().rotation = 0;
        const auto emptyB = Take(NativeObjectDocument::CreateEmpty(ObjectType::Destructable));
        const auto emptyD = Take(NativeObjectDocument::CreateEmpty(ObjectType::Doodad));
        members = {{"war3map.w3e", Take(SerializeMapTerrain(grid))}, {"war3map.doo", Take(placements.Serialize())},
            {"war3map.w3b", Take(NativeObjectCodec(ObjectType::Destructable).Serialize(emptyB))},
            {"war3map.w3d", Take(NativeObjectCodec(ObjectType::Doodad).Serialize(emptyD))},
            {"Units/DestructableData.slk", Slk({"DestructableID"}, {{"D001"}})},
            {"Doodads/Doodads.slk", Slk({"doodID", "shadow", "file", "numVar", "defScale", "maxPitch", "maxRoll", "fixedRot", "floats"},
                {{"D001", "1", "MustNotRead/Destructable", "1", "1", "0", "0", "-1", "0"},
                 {"N001", "0", "_", "0", "1", "0", "0", "-1", "0"},
                 {"C001", "1", "Fixture/Caster", "2", "1.25", "0", "0", "-1", "0"}})},
            // 本测试只验证来源adapter，不把这些原创占位原字节冒充有效MDX或姿势资格。
            {"Fixture/Caster1.mdx", Text("authored fixture model bytes")}, {"war3map.j", Text("// untouched\n")}};
        const std::vector<MapArchiveMember> base{{"Fixture/Unused.mdx", Text("unused model")}};
        Take(WriteVerifiedArchive(game / "war3.mpq", base)); Open();
    }
    ~Fixture() { source.reset(); std::error_code error; std::filesystem::remove_all(root, error); }
    Bytes& Member(std::string_view name) {
        const auto found = std::find_if(members.begin(), members.end(), [&](const auto& member) { return member.name == name; });
        Require(found != members.end(), "原创成员存在"); return found->data;
    }
    void Erase(std::string_view name) {
        std::erase_if(members, [&](const auto& member) { return member.name == name; });
    }
    void Open() {
        source = std::make_unique<MapArchiveSession>(Take(MapArchiveSession::Open(
            Take(WriteVerifiedArchive(root / ("source-" + std::to_string(sequence++) + ".w3x"), members)).path)));
    }
    std::shared_ptr<const TerrainShadowCasterResources> Capture(std::span<const MemberReplacement> output = {},
        std::span<const std::string> removed = {}, const TerrainShadowCasterLimits& limits = {}) const {
        return Take(TerrainShadowCasterResources::Capture(*source, game, output, removed, limits));
    }
    const TerrainShadowCasterDependency& Dependency(const TerrainShadowCasterResources& owner, std::string_view name) const {
        const auto found = std::find_if(owner.Dependencies().begin(), owner.Dependencies().end(),
            [&](const auto& dependency) { return dependency.name == name; });
        Require(found != owner.Dependencies().end(), "实际依赖必须存在"); return *found;
    }
};
void ActualClassificationAndExactInstances() {
    Fixture f; const auto owner = f.Capture();
    Require(owner->DestructableCount() == 1 && owner->NonShadowDoodadCount() == 1 && owner->Casters().size() == 2,
        "DestructableData存在优先排除，只筛真实Doodads.shadow");
    Require(owner->Models().size() == 1 && owner->Models().front().path == "Fixture/Caster1.mdx"
        && owner->Models().front().bytes == f.Member("Fixture/Caster1.mdx"), "真实变体模型原字按唯一路径共享");
    for (std::size_t index = 0; index < 2; ++index) {
        const auto& caster = owner->Casters()[index]; const auto& actual = f.placements.doodads[index + 2];
        Require(caster.sourceRow == index + 2 && caster.modelIndex == 0 && caster.placement.rawcode == actual.rawcode
            && caster.placement.creationId == actual.creationId && caster.placement.position == actual.position
            && caster.placement.scale == actual.scale && caster.placement.rotation == actual.rotation
            && caster.placement.flags == actual.flags && caster.placement.variation == actual.variation
            && caster.defaultScale == 1.25f && caster.maximumPitch == 0 && caster.maximumRoll == 0 && caster.fixedRotation == -1
            && !caster.floating,
            "同rawcode实例仍按完整DOO源行和实际变换分别捕获");
    }
    Require(f.Dependency(*owner, "FIXTURE\\CASTER1.MDX").source == TerrainShadowCasterSource::Map,
        "来源身份是实际地图winner");
    const std::array<float, 12> matrix{2.5f, 0, 0, 512, 0, 3.75f, 0, 256, 0, 0, 5, 0};
    Require(owner->Casters()[1].nativeMatrix == matrix && owner->Terrain().header.width == 9,
        "独立轴向矩阵预期包含实际defScale与DOO缩放/平移，复用同份实际地形");
    Check(owner->VerifySources(*f.source));
    const auto candidate = Take(f.source->SaveCandidate(f.root / "same-inputs.w3x", {}));
    Check(owner->VerifyReadback(Take(MapArchiveSession::Open(candidate.path))));
}
void FinalViewAndPublicationIdentity() {
    Fixture f; const auto original = f.Capture();
    auto changed = f.placements; changed.doodads[2].position[0] = 400;
    std::vector<MemberReplacement> output{{"war3map.doo", Take(changed.Serialize())},
        {"Fixture/Caster1.mdx", Text("actual final model bytes")}, {"war3map.j", Text("// unrelated output\n")}};
    const auto owner = f.Capture(output);
    Require(owner->Casters()[0].placement.position[0] == 400 && owner->Models()[0].bytes == output[1].data,
        "实际最终DOO和模型覆盖都被消费");
    Rejected(original->VerifySources(*f.source, output), "TERRAIN_SHADOW_CASTER_DEPENDENCY_STALE");
    Check(owner->VerifySources(*f.source, output));
    const auto candidate = Take(f.source->SaveCandidate(f.root / "final-view.w3x", output));
    const auto saved = Take(MapArchiveSession::Open(candidate.path)); Check(owner->VerifyReadback(saved));
    Rejected(original->VerifyReadback(saved), "TERRAIN_SHADOW_CASTER_READBACK_STALE");
    output.push_back({"WAR3MAP.DOO", output[0].data});
    Rejected(TerrainShadowCasterResources::Capture(*f.source, f.game, output), "TERRAIN_SHADOW_CASTER_FINAL_CONFLICT");
    output.pop_back(); const std::vector<std::string> removed{"fixture/caster1.mdx"};
    Rejected(TerrainShadowCasterResources::Capture(*f.source, f.game, output, removed), "TERRAIN_SHADOW_CASTER_FINAL_CONFLICT");
    output.erase(output.begin() + 1);
    Rejected(TerrainShadowCasterResources::Capture(*f.source, f.game, output, removed), "TERRAIN_SHADOW_CASTER_SOURCE_MISSING");
}
void GameWinnerAndKnownAbsence() {
    Fixture f; f.Erase("Fixture/Caster1.mdx"); f.Erase("war3map.w3b"); f.Erase("war3map.w3d"); f.Open();
    const std::vector<MapArchiveMember> gameMembers{{"Fixture/Caster1.mdx", Text("patch model bytes")},
        {"war3map.w3b", Text("game native member must not fill map absence")},
        {"war3map.w3d", Text("game native member must not fill map absence")}};
    Take(WriteVerifiedArchive(f.game / "War3Patch.mpq", gameMembers));
    const auto owner = f.Capture();
    Require(f.Dependency(*owner, "WAR3MAP.W3B").source == TerrainShadowCasterSource::Missing
        && f.Dependency(*owner, "WAR3MAP.W3D").source == TerrainShadowCasterSource::Missing,
        "原生对象表明确缺失不能冒充已解析空表");
    const auto& dependency = f.Dependency(*owner, "FIXTURE\\CASTER1.MDX");
    Require(dependency.source == TerrainShadowCasterSource::Game && dependency.gameContainer == "war3patch.mpq"
        && !dependency.sha256.empty(), "最高层实际游戏模型来源/SHA被绑定");
    Check(owner->VerifySources(*f.source)); // W3B/W3D缺失只按Capture封存的map-only规则复核。
    std::vector<MemberReplacement> output{{"Fixture/Caster1.mdx", gameMembers[0].data}};
    Rejected(owner->VerifySources(*f.source, output), "TERRAIN_SHADOW_CASTER_DEPENDENCY_STALE");
    // 相同字节从游戏变成地图winner仍是来源变化，不把SHA相同当同一来源。
    Require(f.Capture(output)->Models()[0].bytes == owner->Models()[0].bytes, "等字节资源不会伪造相同winner身份");
    Check(IO::WriteFileBytes(f.game / "War3Patch.mpq", Text("corrupt highest archive")));
    Rejected(owner->VerifySources(*f.source));
}
void ActualTilesetAndFinalTableOverrides() {
    Fixture f; f.Erase("Fixture/Caster1.mdx"); f.Open();
    const std::vector<MapArchiveMember> lower{{"Fixture/Caster1.mdx", Text("actual L model")}};
    const std::vector<MapArchiveMember> upper{{"Fixture/Caster1.mdx", Text("actual A model")}};
    const auto layerL = Take(WriteVerifiedArchive(f.root / "layer-l.mpq", lower));
    const auto layerA = Take(WriteVerifiedArchive(f.root / "layer-a.mpq", upper));
    const std::vector<MapArchiveMember> local{{"L.mpq", Take(IO::ReadFileBytes(layerL.path))},
        {"A.mpq", Take(IO::ReadFileBytes(layerA.path))}};
    Take(WriteVerifiedArchive(f.game / "War3Local.mpq", local));
    const auto original = f.Capture();
    Require(original->Models()[0].bytes == lower[0].data
        && f.Dependency(*original, "FIXTURE\\CASTER1.MDX").gameContainer.find("|L.mpq|") != std::string::npos,
        "模型使用实际最终W3E地形集的嵌套包winner");
    auto grid = *Take(ParseMapTerrain(f.Member("war3map.w3e"))).Snapshot(); grid.header.mainTileset = 'A';
    std::vector<MemberReplacement> output{{"war3map.w3e", Take(SerializeMapTerrain(grid))}};
    const auto changed = f.Capture(output);
    Require(changed->Models()[0].bytes == upper[0].data
        && f.Dependency(*changed, "FIXTURE\\CASTER1.MDX").gameContainer.find("|A.mpq|") != std::string::npos,
        "同源最终W3E覆盖确实重选A而非固定原L资源");
    Rejected(original->VerifySources(*f.source, output), "TERRAIN_SHADOW_CASTER_DEPENDENCY_STALE");
    output.push_back({"Doodads/Doodads.slk", Slk({"doodID", "shadow"}, {{"N001", "0"}, {"C001", "0"}})});
    const auto disabled = f.Capture(output);
    Require(disabled->Casters().empty() && disabled->Models().empty() && disabled->NonShadowDoodadCount() == 3,
        "实际最终表的shadow0使模型不被读取，不拿原表或游戏表冒充session覆盖");
}
void UnsupportedAndCorruptAreNotEmpty() {
    Fixture f;
    auto native = Take(NativeObjectDocument::CreateEmpty(ObjectType::Doodad)); Check(native.AddOriginalObject("C001"));
    std::vector<MemberReplacement> output{{"war3map.w3d", Take(NativeObjectCodec(ObjectType::Doodad).Serialize(native))}};
    Rejected(TerrainShadowCasterResources::Capture(*f.source, f.game, output), "TERRAIN_SHADOW_CASTER_NATIVE_UNSUPPORTED");
    output[0] = {"war3map.w3d", {}}; Rejected(TerrainShadowCasterResources::Capture(*f.source, f.game, output));
    auto unknown = f.placements; unknown.doodads.back().rawcode = "Z999";
    output[0] = {"war3map.doo", Take(unknown.Serialize())};
    Rejected(TerrainShadowCasterResources::Capture(*f.source, f.game, output), "TERRAIN_SHADOW_CASTER_OBJECT_UNKNOWN");
    output[0] = {"Fixture/Caster1.mdx", {}};
    Rejected(TerrainShadowCasterResources::Capture(*f.source, f.game, output), "TERRAIN_SHADOW_CASTER_SOURCE_MISSING");
    auto duplicate = Slk({"doodID", "shadow", "file", "numVar"},
        {{"N001", "0", "_", "0"}, {"C001", "1", "Fixture/Caster", "2"}, {"C001", "0", "_", "0"}});
    output[0] = {"Doodads/Doodads.slk", duplicate};
    Rejected(TerrainShadowCasterResources::Capture(*f.source, f.game, output), "TERRAIN_SHADOW_CASTER_TABLE_ID");
    output[0] = {"Doodads/Doodads.slk", Slk({"doodID", "shadow", "file", "numVar"},
        {{"N001", "0", "_", "0"}, {"C001", "1tail", "Fixture/Caster", "2"}})};
    Rejected(TerrainShadowCasterResources::Capture(*f.source, f.game, output), "TERRAIN_SHADOW_CASTER_FIELD");
}
void EmptyAndSpecialListsRemainExplicit() {
    Fixture f; DoodadPlacements empty;
    std::vector<MemberReplacement> output{{"war3map.doo", Take(empty.Serialize())}};
    auto owner = f.Capture(output);
    Require(owner->Casters().empty() && owner->Models().empty() && owner->Dependencies().size() == 2,
        "实际空DOO不虚读对象或模型；仍绑定W3E和DOO来源");
    empty.specialDoodads.push_back({"S001", 2, {3, 4}}); output[0].data = Take(empty.Serialize());
    owner = f.Capture(output);
    Require(owner->Casters().empty() && owner->SpecialDoodads().size() == 1
        && owner->SpecialDoodads()[0].position == std::array<std::int32_t, 2>{3, 4},
        "特殊DOO完整交接给表面owner，不暗中当成空或授予完整重建资格");
    const std::vector<std::string> removed{"war3map.doo"};
    owner = f.Capture({}, removed);
    Require(owner->Casters().empty() && f.Dependency(*owner, "WAR3MAP.DOO").source == TerrainShadowCasterSource::Missing,
        "实际缺失DOO明确记录缺失身份");
}
void StrictVariationAndUniqueModelLimit() {
    Fixture f;
    auto changed = f.placements; changed.doodads[2].variation = 2;
    std::vector<MemberReplacement> output{{"war3map.doo", Take(changed.Serialize())}};
    Rejected(TerrainShadowCasterResources::Capture(*f.source, f.game, output), "TERRAIN_SHADOW_CASTER_VARIATION");
    changed.doodads[2].variation = -1; output[0].data = Take(changed.Serialize());
    Rejected(TerrainShadowCasterResources::Capture(*f.source, f.game, output), "TERRAIN_SHADOW_CASTER_VARIATION");
    output[0] = {"Doodads/Doodads.slk", Slk({"doodID", "shadow", "file", "numVar", "defScale", "maxPitch", "maxRoll", "fixedRot", "floats"},
        {{"N001", "0", "_", "0", "1", "0", "0", "-1", "0"}, {"C001", "1", "Fixture/Caster.mdl", "2", "1", "0", "0", "-1", "0"}})};
    Rejected(TerrainShadowCasterResources::Capture(*f.source, f.game, output), "TERRAIN_SHADOW_CASTER_MODEL_PATH_UNSUPPORTED");
    changed = f.placements; changed.doodads[3].rawcode = "C002"; changed.doodads[3].variation = 0;
    output[0] = {"war3map.doo", Take(changed.Serialize())};
    output.push_back({"Doodads/Doodads.slk", Slk({"doodID", "shadow", "file", "numVar", "defScale", "maxPitch", "maxRoll", "fixedRot", "floats"},
        {{"N001", "0", "_", "0", "1", "0", "0", "-1", "0"}, {"C001", "1", "Fixture/Caster", "2", "1", "0", "0", "-1", "0"},
         {"C002", "1", "Fixture/Second", "1", "2", "0", "0", "-1", "0"}})});
    output.push_back({"Fixture/Second.mdx", Text("second actual model")});
    Require(f.Capture(output)->Models().size() == 2, "第二个实际文件保持独立模型身份");
    TerrainShadowCasterLimits limits; limits.maximumModels = 1;
    Rejected(TerrainShadowCasterResources::Capture(*f.source, f.game, output, {}, limits), "TERRAIN_SHADOW_CASTER_BUDGET");
}
void NativeTransformAndSurfaceRefusals() {
    Fixture f;
    for (const auto& values : std::vector<std::array<std::string, 5>>{
        {"0.001", "0", "0", "-1", "0"}, {"1", "1", "0", "-1", "0"},
        {"1", "0", "1", "-1", "0"}, {"1", "0", "0", "0", "0"}, {"1", "0", "0", "-1", "1"}}) {
        const std::vector<MemberReplacement> output{{"Doodads/Doodads.slk",
            Slk({"doodID", "shadow", "file", "numVar", "defScale", "maxPitch", "maxRoll", "fixedRot", "floats"},
                {{"N001", "0", "_", "0", "1", "0", "0", "-1", "0"},
                 {"C001", "1", "Fixture/Caster", "2", values[0], values[1], values[2], values[3], values[4]}})}};
        Rejected(TerrainShadowCasterResources::Capture(*f.source, f.game, output), "TERRAIN_SHADOW_CASTER_TRANSFORM_UNSUPPORTED");
    }
    auto placements = f.placements; placements.doodads[2].position[2] = 1;
    std::vector<MemberReplacement> output{{"war3map.doo", Take(placements.Serialize())}};
    Rejected(TerrainShadowCasterResources::Capture(*f.source, f.game, output), "TERRAIN_SHADOW_CASTER_HEIGHT_UNSUPPORTED");
    auto grid = *Take(ParseMapTerrain(f.Member("war3map.w3e"))).Snapshot();
    grid.vertices[1].textureAndFlagsRaw |= 0x40; output[0] = {"war3map.w3e", Take(SerializeMapTerrain(grid))};
    Rejected(TerrainShadowCasterResources::Capture(*f.source, f.game, output), "TERRAIN_SHADOW_CASTER_WATER_UNSUPPORTED");
    grid.vertices[1].textureAndFlagsRaw &= static_cast<std::uint8_t>(~0x40);
    grid.vertices[1].textureAndFlagsRaw |= 0x10; output[0].data = Take(SerializeMapTerrain(grid));
    Rejected(TerrainShadowCasterResources::Capture(*f.source, f.game, output));
    placements = f.placements; placements.specialDoodads.push_back({"S001", 0, {1, 1}});
    output[0] = {"war3map.doo", Take(placements.Serialize())};
    Rejected(TerrainShadowCasterResources::Capture(*f.source, f.game, output));
}
void LimitsCancellationAndRealSourceDrift() {
    Fixture f;
    // 最终内存覆盖只消费正文，不需要MPQ存储缓冲；在此单独核正文累计预算。
    // 归档读取的stored/decoded双门与精确准入额由下个用例独立覆盖。
    const std::vector<MemberReplacement> finalModel{{"Fixture/Caster1.mdx", f.Member("Fixture/Caster1.mdx")}};
    const auto owner = f.Capture(finalModel);
    TerrainShadowCasterLimits limits; limits.maximumSourceBytes = owner->SourceBytes();
    Require(f.Capture(finalModel, {}, limits)->SourceBytes() == limits.maximumSourceBytes, "精确正文累计预算可重复");
    --limits.maximumSourceBytes;
    Rejected(TerrainShadowCasterResources::Capture(*f.source, f.game, finalModel, {}, limits), "TERRAIN_SHADOW_CASTER_BUDGET");
    limits = {}; limits.maximumWorkingBytes = owner->WorkingBytes();
    Require(f.Capture(finalModel, {}, limits)->WorkingBytes() == limits.maximumWorkingBytes, "精确工作预留预算可重复");
    --limits.maximumWorkingBytes;
    Rejected(TerrainShadowCasterResources::Capture(*f.source, f.game, finalModel, {}, limits), "TERRAIN_SHADOW_CASTER_BUDGET");
    limits = {}; limits.maximumPlacements = 3;
    Rejected(TerrainShadowCasterResources::Capture(*f.source, f.game, {}, {}, limits));
    limits = {}; limits.maximumModels = 0;
    Rejected(TerrainShadowCasterResources::Capture(*f.source, f.game, {}, {}, limits), "TERRAIN_SHADOW_CASTER_LIMITS");
    std::stop_source stop; stop.request_stop();
    Rejected(TerrainShadowCasterResources::Capture(*f.source, f.game, {}, {}, {}, stop.get_token()),
        "TERRAIN_SHADOW_CASTER_CANCELLED");
    auto bytes = Take(IO::ReadFileBytes(f.source->SourcePath())); bytes.push_back(std::byte{0x42});
    Check(IO::WriteFileBytes(f.source->SourcePath(), bytes)); Rejected(owner->VerifySources(*f.source));
}

void StoredAndDecodedSourceBudgets() {
    for (const bool fromGame : {false, true}) {
        Fixture f;
        // 固定夹具的前六份来源先消费；模型最后读。独立按夹具正文求前缀，
        // 不用捕获器返回的SourceBytes反推能通过的预算。
        std::size_t prefixBytes = 0;
        for (const auto* name : {"war3map.w3e", "war3map.doo", "war3map.w3b", "war3map.w3d",
            "Units/DestructableData.slk", "Doodads/Doodads.slk"}) prefixBytes += f.Member(name).size();
        const auto model = f.Member("Fixture/Caster1.mdx");
        auto archivePath = f.source->SourcePath();
        if (fromGame) {
            f.Erase("Fixture/Caster1.mdx"); f.Open();
            archivePath = Take(WriteVerifiedArchive(f.game / "War3Patch.mpq",
                std::vector<MapArchiveMember>{{"Fixture/Caster1.mdx", model}})).path;
        }
        const auto archive = Take(Mpq::MpqArchive::OpenReadOnly(archivePath));
        const auto info = archive.GetFileInfo("Fixture/Caster1.mdx");
        Require(info && info->fileSize == model.size() && info->compressedSize > info->fileSize,
            "原创小模型必须覆盖存储块大于解压正文的真实MPQ边界");
        const auto admission = prefixBytes + (std::max)(std::size_t(info->compressedSize), std::size_t(info->fileSize));
        TerrainShadowCasterLimits limits; limits.maximumSourceBytes = admission;
        const auto owner = f.Capture({}, {}, limits);
        const auto& dependency = f.Dependency(*owner, "FIXTURE\\CASTER1.MDX");
        Require(owner->SourceBytes() == prefixBytes + model.size() && owner->Models()[0].bytes == model,
            "精确stored/decoded准入额仍返回完整正文并保持实际正文计数");
        Require(dependency.source == (fromGame ? TerrainShadowCasterSource::Game : TerrainShadowCasterSource::Map)
            && dependency.gameContainer == (fromGame ? "war3patch.mpq" : ""),
            "边界用例分别命中实际地图与最高优先级游戏包，不替换来源");
        Check(owner->VerifySources(*f.source));
        --limits.maximumSourceBytes;
        Rejected(TerrainShadowCasterResources::Capture(*f.source, f.game, {}, {}, limits), "FILE_READ_BUDGET_EXCEEDED");
    }
}
void BoundedDependencyVerification() {
    Fixture f;
    // 两个实际模型各64KiB。复核额度容得下一份stored/decoded正文，容不下
    // 两份缓存，更不能重新展开地形、DOO、表格与完整caster owner。
    f.placements.doodads[2].variation = 0;
    f.Member("war3map.doo") = Take(f.placements.Serialize());
    f.Member("Fixture/Caster1.mdx").resize(64u * 1024u, std::byte{0x31});
    f.members.push_back({"Fixture/Caster0.mdx", Bytes(64u * 1024u, std::byte{0x30})}); f.Open();
    const auto owner = f.Capture();
    TerrainShadowCasterVerificationLimits scratch; scratch.maximumWorkingBytes = 160u * 1024u;
    Require(owner->WorkingBytes() > scratch.maximumWorkingBytes, "窄复核额度必须小于完整owner捕获工作量");
    TerrainShadowCasterLimits full; full.maximumWorkingBytes = scratch.maximumWorkingBytes;
    Rejected(TerrainShadowCasterResources::Capture(*f.source, f.game, {}, {}, full), "TERRAIN_SHADOW_CASTER_BUDGET");
    Check(owner->VerifySources(*f.source, {}, {}, {}, scratch));
    const auto candidate = Take(f.source->SaveCandidate(f.root / "bounded-readback.w3x", {}));
    const auto saved = Take(MapArchiveSession::Open(candidate.path));
    Check(owner->VerifyReadback(saved, {}, scratch));
    std::stop_source cancelled; cancelled.request_stop();
    Rejected(owner->VerifySources(*f.source, {}, {}, cancelled.get_token(), scratch), "TERRAIN_SHADOW_CASTER_CANCELLED");
    auto tiny = scratch; tiny.maximumWorkingBytes = 1;
    Rejected(owner->VerifySources(*f.source, {}, {}, {}, tiny), "TERRAIN_SHADOW_CASTER_BUDGET");
    Rejected(owner->VerifyReadback(saved, {}, tiny), "TERRAIN_SHADOW_CASTER_BUDGET");
    tiny = scratch; tiny.maximumSourceBytes = 1; Rejected(owner->VerifySources(*f.source, {}, {}, {}, tiny));
    tiny = scratch; tiny.maximumFileBytes = 1; Rejected(owner->VerifyReadback(saved, {}, tiny));
    std::vector<MemberReplacement> changed{{"Fixture/Caster0.mdx", f.Member("Fixture/Caster0.mdx")}};
    changed.front().data.back() = std::byte{0x32};
    Rejected(owner->VerifySources(*f.source, changed, {}, {}, scratch), "TERRAIN_SHADOW_CASTER_DEPENDENCY_STALE");
    // 默认复核也必须扣除驻留owner，不能忽略调用方已用尽的捕获工作额度。
    full = {}; full.maximumWorkingBytes = owner->WorkingBytes();
    const auto noSpare = f.Capture({}, {}, full);
    Rejected(noSpare->VerifySources(*f.source), "TERRAIN_SHADOW_CASTER_BUDGET");
    Rejected(noSpare->VerifyReadback(saved), "TERRAIN_SHADOW_CASTER_BUDGET");
}
void BatchedDependencyVerificationPreservesEveryFact() {
    Fixture f;
    constexpr std::size_t modelCount = 300, modelBytes = 4096;
    std::vector<std::vector<std::string>> rows;
    std::vector<MapArchiveMember> gameMembers;
    f.placements.doodads.clear(); f.Erase("Fixture/Caster1.mdx");
    f.Erase("war3map.w3b"); f.Erase("war3map.w3d");
    const auto suffix = [](std::size_t index) {
        const auto value = std::to_string(index);
        return std::string(3 - value.size(), '0') + value;
    };
    for (std::size_t index = 0; index < modelCount; ++index) {
        const auto number = suffix(index), id = "M" + number, path = "Fixture/Batch/Model" + number;
        rows.push_back({id, "1", path, "1", "1", "0", "0", "-1", "0"});
        f.placements.doodads.push_back(Placement(id, 0, 128 + 128 * float(index % 6), std::int32_t(index + 1)));
        MapArchiveMember member{path + ".mdx", Bytes(modelBytes, std::byte{0x51})};
        const auto marker = Text("authored source-only model " + number);
        std::copy(marker.begin(), marker.end(), member.data.begin());
        if (index % 2) gameMembers.push_back(std::move(member));
        else f.members.push_back(std::move(member));
    }
    f.Member("war3map.doo") = Take(f.placements.Serialize());
    f.Member("Doodads/Doodads.slk") = Slk({"doodID", "shadow", "file", "numVar", "defScale",
        "maxPitch", "maxRoll", "fixedRot", "floats"}, rows);
    // 这些游戏层同名成员不能填补map-only原生表的缺失事实。
    gameMembers.push_back({"war3map.w3b", Text("must not fill map-only absence")});
    gameMembers.push_back({"war3map.w3d", Text("must not fill map-only absence")});
    Take(WriteVerifiedArchive(f.game / "War3Patch.mpq", gameMembers)); f.Open();
    const auto owner = f.Capture();
    Require(owner->Models().size() == modelCount && owner->Dependencies().size() > 256,
        "原创独立模型与缺失表必须跨过exact读取256项硬界");
    Require(f.Dependency(*owner, "WAR3MAP.W3B").source == TerrainShadowCasterSource::Missing
        && f.Dependency(*owner, "WAR3MAP.W3D").source == TerrainShadowCasterSource::Missing,
        "跨批复核保留原生表map-only缺失策略");
    for (std::size_t index = 0; index < modelCount; ++index) {
        const auto& dependency = f.Dependency(*owner, "FIXTURE\\BATCH\\MODEL" + suffix(index) + ".MDX");
        Require(dependency.bytes == modelBytes && dependency.sha256.size() == 64
            && dependency.source == (index % 2 ? TerrainShadowCasterSource::Game : TerrainShadowCasterSource::Map)
            && dependency.gameContainer == (index % 2 ? "war3patch.mpq" : ""),
            "每个跨批依赖分别保留全文SHA和实际map/game winner");
    }
    Check(owner->VerifySources(*f.source));
    const auto candidate = Take(f.source->SaveCandidate(f.root / "many-dependencies.w3x", {}));
    const auto saved = Take(MapArchiveSession::Open(candidate.path));
    Check(owner->VerifyReadback(saved));
    TerrainShadowCasterVerificationLimits scratch; scratch.maximumWorkingBytes = 256u * 1024u;
    Require(owner->WorkingBytes() > scratch.maximumWorkingBytes,
        "批复核窄预算不足以重新建立完整owner");
    Check(owner->VerifySources(*f.source, {}, {}, {}, scratch));
    Check(owner->VerifyReadback(saved, {}, scratch));
    // 首项和末项都要完整比较；末项来自游戏，把相同正文提到地图层也须拒绝。
    std::vector<MemberReplacement> changed{{"Fixture/Batch/Model000.mdx", f.Member("Fixture/Batch/Model000.mdx")}};
    changed[0].data.back() = std::byte{0x52};
    Rejected(owner->VerifySources(*f.source, changed, {}, {}, scratch), "TERRAIN_SHADOW_CASTER_DEPENDENCY_STALE");
    changed[0] = {"Fixture/Batch/Model299.mdx", gameMembers[modelCount / 2 - 1].data};
    Rejected(owner->VerifySources(*f.source, changed, {}, {}, scratch), "TERRAIN_SHADOW_CASTER_DEPENDENCY_STALE");
    const auto moved = Take(f.source->SaveCandidate(f.root / "changed-final-winner.w3x", changed));
    Rejected(owner->VerifyReadback(Take(MapArchiveSession::Open(moved.path)), {}, scratch),
        "TERRAIN_SHADOW_CASTER_READBACK_STALE");
    const auto empty = Take(NativeObjectDocument::CreateEmpty(ObjectType::Destructable));
    changed[0] = {"war3map.w3b", Take(NativeObjectCodec(ObjectType::Destructable).Serialize(empty))};
    Rejected(owner->VerifySources(*f.source, changed, {}, {}, scratch), "TERRAIN_SHADOW_CASTER_DEPENDENCY_STALE");
    changed[0] = {"Doodads/Doodads.slk", Bytes(2u * 1024u * 1024u + 1, std::byte{0x53})};
    Rejected(owner->VerifySources(*f.source, changed), "TERRAIN_SHADOW_CASTER_BUDGET");
    auto exhausted = scratch; exhausted.maximumSourceBytes = 256 * modelBytes;
    Rejected(owner->VerifySources(*f.source, {}, {}, {}, exhausted));
    Rejected(owner->VerifyReadback(saved, {}, exhausted));
    // 容得下单项而非全批时应成功；连一项metadata也容不下则准确拒绝。
    auto tiny = scratch; tiny.maximumWorkingBytes = 4096;
    Rejected(owner->VerifySources(*f.source, {}, {}, {}, tiny), "TERRAIN_SHADOW_CASTER_BUDGET");
    tiny = scratch; tiny.maximumFileBytes = modelBytes - 1;
    Rejected(owner->VerifyReadback(saved, {}, tiny), "TERRAIN_SHADOW_CASTER_BUDGET");
    std::stop_source cancelled; cancelled.request_stop();
    Rejected(owner->VerifyReadback(saved, cancelled.get_token(), scratch), "TERRAIN_SHADOW_CASTER_CANCELLED");
    // 真实游戏归档的末项正文改变、winner保持相同，仍须命中全文SHA变化。
    gameMembers[modelCount / 2 - 1].data.back() = std::byte{0x54};
    const auto changedPatch = Take(WriteVerifiedArchive(f.root / "changed-patch.mpq", gameMembers));
    Check(IO::WriteFileBytes(f.game / "War3Patch.mpq", Take(IO::ReadFileBytes(changedPatch.path))));
    Rejected(owner->VerifySources(*f.source, {}, {}, {}, scratch), "TERRAIN_SHADOW_CASTER_DEPENDENCY_STALE");
    Rejected(owner->VerifyReadback(saved, {}, scratch), "TERRAIN_SHADOW_CASTER_READBACK_STALE");
}
} // namespace

int main() {
    try {
        ActualClassificationAndExactInstances(); FinalViewAndPublicationIdentity(); GameWinnerAndKnownAbsence();
        ActualTilesetAndFinalTableOverrides(); UnsupportedAndCorruptAreNotEmpty(); EmptyAndSpecialListsRemainExplicit();
        StrictVariationAndUniqueModelLimit(); NativeTransformAndSurfaceRefusals(); LimitsCancellationAndRealSourceDrift();
        StoredAndDecodedSourceBudgets(); BoundedDependencyVerification(); BatchedDependencyVerificationPreservesEveryFact();
        std::cout << "TerrainShadowCasterResourcesTests passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
