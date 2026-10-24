#include "War3MapCore/Services/TerrainShadowBake.h"

#include "War3MapCore/Hash.h"
#include "War3MapCore/IO/FileBytes.h"
#include "War3MapCore/MapDocument.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/MapData/MapPlacements.h"
#include "War3MapCore/MapData/MapSettings.h"
#include "War3MapCore/MapData/MapShadow.h"
#include "War3MapCore/MapData/TerrainEditing.h"
#include "War3MapCore/MapData/TerrainSavePlan.h"
#include "War3MapCore/Services/MapSaveService.h"
#include "War3MapCore/Services/TerrainShadowCasterResources.h"

#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <vector>

namespace {
using namespace War3Map;
using namespace War3Map::MapData;
using namespace War3Map::Services;
using Bytes = std::vector<std::byte>;
constexpr std::uint32_t kSide = 32;

void Require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
template<class T> T Take(Result<T> result) {
    if (!result.Succeeded() || !result.value) {
        std::string message = "原创阴影保存样例失败";
        for (const auto& diagnostic : result.diagnostics) message += " | " + diagnostic.code + ":" + diagnostic.message;
        throw std::runtime_error(message);
    }
    return std::move(*result.value);
}
void Check(Status status) {
    if (status.Succeeded()) return;
    std::string message;
    for (const auto& diagnostic : status.diagnostics) message += " | " + diagnostic.code + ":" + diagnostic.message;
    throw std::runtime_error(message);
}
void HasCode(std::span<const Diagnostic> diagnostics, std::string_view code) {
    Require(!diagnostics.empty(), "拒绝必须携带明确诊断");
    if (code.empty() || std::any_of(diagnostics.begin(), diagnostics.end(),
        [&](const auto& diagnostic) { return diagnostic.code == code; })) return;
    std::string message = "缺少预期错误：" + std::string(code);
    for (const auto& diagnostic : diagnostics) message += " | " + diagnostic.code + ":" + diagnostic.message;
    throw std::runtime_error(message);
}
template<class T> void Rejected(const Result<T>& result, std::string_view code = {}) {
    Require(!result.Succeeded() && !result.value, "失败或取消不得返回部分阴影候选"); HasCode(result.diagnostics, code);
}
void Rejected(const Status& status, std::string_view code = {}) {
    Require(!status.Succeeded(), "过期或冲突的阴影结果必须拒绝"); HasCode(status.diagnostics, code);
}
Bytes Text(std::string_view text) {
    Bytes result;
    for (const auto character : text) result.push_back(static_cast<std::byte>(character));
    return result;
}
void Put32(Bytes& bytes, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) bytes.push_back(static_cast<std::byte>((value >> shift) & 255));
}
Bytes Pathing() {
    auto bytes = Text("MP3W"); Put32(bytes, 0); Put32(bytes, kSide); Put32(bytes, kSide);
    bytes.resize(16 + kSide * kSide, std::byte{0x40}); return bytes;
}

// 独立字面预期，不调用采样器、投影器或SHD编码器制造expected。
// x=4的顶点脊线高512，其右侧第一个receiver为448；原版步长约-42又2/3，
// 因而x17..26落在阴影内，x27已降到平地以下。各y行的原始高度完全相同。
Bytes ExpectedShadow(bool ridge) {
    Bytes expected(kSide * kSide, std::byte{0});
    if (ridge) for (std::size_t y = 0; y < kSide; ++y)
        for (std::size_t x = 17; x <= 26; ++x) expected[y * kSide + x] = std::byte{0xff};
    return expected;
}

struct Fixture {
    std::filesystem::path root, game;
    std::vector<MapArchiveMember> members;
    TerrainGridSnapshot terrain;
    std::unique_ptr<MapArchiveSession> source;
    std::string sourceSha;
    explicit Fixture(bool ridge = false) {
        static std::atomic<unsigned> sequence{0};
        root = std::filesystem::temp_directory_path() / ("Audastra-shadow-bake-" + std::to_string(GetCurrentProcessId())
            + "-" + std::to_string(GetTickCount64()) + "-" + std::to_string(sequence++));
        Require(std::filesystem::create_directory(root), "创建原创专用地图目录");
        game = root / "game"; Require(std::filesystem::create_directory(game), "创建原创profile目录");
        Direction("[Light]\nDirection=3,0,-4\n");
        terrain.header.width = terrain.header.height = 9; terrain.header.mainTileset = 'L';
        terrain.header.groundTilesets = {"G001"}; terrain.header.cliffTilesets = {"C001"};
        terrain.vertices.resize(81);
        for (std::size_t y = 0; y < 9; ++y) for (std::size_t x = 0; x < 9; ++x) {
            auto& vertex = terrain.vertices[y * 9 + x];
            vertex.groundHeightRaw = static_cast<std::uint16_t>(8192 + (ridge && x == 4 ? 2048 : 0));
            vertex.cliffAndLayerRaw = 2;
        }
        RefreshTerrainDerived(terrain, 0, 0, 9, 9);
        MapSettings settings; settings.formatVersion = 25; settings.tileset = 'L'; settings.custom.emplace();
        settings.playableWidth = settings.playableHeight = 8;
        DoodadPlacements emptyDoodads;
        members = {{"war3map.w3e", Take(SerializeMapTerrain(terrain))}, {"war3map.wpm", Pathing()},
            {"war3map.w3i", Take(settings.Serialize())}, {"war3map.doo", Take(emptyDoodads.Serialize())},
            {"war3map.shd", Bytes(kSide * kSide, std::byte{0x7d})},
            {"war3mapMap.tga", Text("authored minimap sentinel")},
            {"war3map.j", Text("// original authored script remains byte-exact\n")},
            {"Fixture/Unrelated.bin", Text("original unrelated member")}};
        source = std::make_unique<MapArchiveSession>(Take(MapArchiveSession::Open(
            Take(WriteVerifiedArchive(root / "source.w3x", members)).path)));
        sourceSha = source->SourceSha256();
    }
    ~Fixture() { source.reset(); std::error_code error; std::filesystem::remove_all(root, error); }
    const Bytes& Member(std::string_view name) const {
        const auto found = std::find_if(members.begin(), members.end(), [&](const auto& member) { return member.name == name; });
        Require(found != members.end(), "原创源成员必须存在"); return found->data;
    }
    void Direction(std::string_view text) const {
        Check(IO::WriteFileBytes(game / "WorldEditData.txt", Text(text)));
    }
    void Freeze(MapSaveTask& task) const { Check(task.FreezeArchive(*source)); task.target = root / "candidate.w3x"; }
    void Prepare(MapSaveTask& task) const {
        task.prepareFinalDerived = [this](MapSaveTask& current) {
            Require(current.InFinalDerivedStage(), "阴影只能在最终脚本和模板完成后的同次派生阶段计算");
            std::shared_ptr<const TerrainSavePlan> noPlan;
            auto status = PrepareTerrainShadowBake(current, game, nullptr, noPlan);
            Require(!noPlan, "仅计算阴影不能臆造地形或通行保存计划"); return status;
        };
    }
    void VerifySource() const {
        Require(Take(Sha256File(source->SourcePath())) == sourceSha, "候选保存或拒绝不得改写原创源地图");
    }
    void VerifyDenied(const MapSaveTask& task) const {
        Require(task.executed && !task.succeeded && !task.nextSession && task.written.empty() && task.backup.empty(),
            "失败必须在发布归档前终止，不得留下成功基线或备份结果");
        Require(std::any_of(task.diagnostics.begin(), task.diagnostics.end(),
            [](const auto& diagnostic) { return diagnostic.severity == DiagnosticSeverity::Error; }), "失败必须给出error诊断");
        if (!task.inPlace) Require(!std::filesystem::exists(task.target), "拒绝不能先写候选再报失败");
        VerifySource();
    }
};

MemberReplacement& ShadowOutput(MapSaveTask& task) {
    const auto found = std::find_if(task.replacements.begin(), task.replacements.end(),
        [](const auto& member) { return member.name == "war3map.shd"; });
    Require(found != task.replacements.end() && !found->data.empty(), "篡改夹具必须命中真实生成的SHD"); return *found;
}

void ExplicitBakeWritesCompleteLiteralShadowAndPreservesOtherMembers() {
    for (const bool ridge : {false, true}) {
        Fixture fixture(ridge); MapSaveTask task; fixture.Freeze(task); fixture.Prepare(task);
        const auto unrelated = Text("same-save unrelated edit");
        task.replacements.push_back({"Fixture/Unrelated.bin", unrelated});
        MapSaveService::Run(task);
        if (!task.succeeded) Check({task.diagnostics});
        Require(task.succeeded && task.nextSession && task.terrainShadow && !task.scriptRebuilt,
            "正式共用保存服务必须实际写出、回读并持有本次阴影owner");
        Require(task.terrainShadow->Width() == kSide && task.terrainShadow->Height() == kSide
            && task.terrainShadow->CasterCount() == 0 && task.terrainShadow->Bytes() == ExpectedShadow(ridge),
            "实际32x32完整SHD必须匹配独立的平地或脊线00/FF字面图");
        const auto reopened = Take(MapArchiveSession::Open(task.written));
        Require(Take(reopened.ReadMember("war3map.shd")) == ExpectedShadow(ridge), "独立MPQ重开必须读回完整相同阴影");
        const auto parsed = Take(ParseMapShadow(Take(reopened.ReadMember("war3map.shd")), kSide, kSide));
        Require(std::all_of(parsed.intensities.begin(), parsed.intensities.end(),
            [](auto value) { return value == 0 || value == 0xff; }), "原版持久bit0必须写00/FF，不能写逻辑0/1或保留旧高位");
        for (const auto* name : {"war3map.w3e", "war3map.wpm", "war3map.w3i", "war3map.doo", "war3map.j", "war3mapMap.tga"})
            Require(Take(reopened.ReadMember(name)) == fixture.Member(name), "显式阴影计算逐字保留所有无关原成员");
        Require(Take(reopened.ReadMember("Fixture/Unrelated.bin")) == unrelated, "同次普通编辑不能被阴影计算丢弃");
        Check(task.terrainShadow->VerifyReadback(reopened)); fixture.VerifySource();
    }
}

void OrdinarySaveRetainsOriginalShadowAndDoesNotNeedGameProfiles() {
    Fixture fixture(true); MapSaveTask task; fixture.Freeze(task);
    Check(IO::WriteFileBytes(fixture.game / "WorldEditData.txt", Text("deliberately invalid unused profile")));
    const auto unrelated = Text("ordinary edit only"); task.replacements.push_back({"Fixture/Unrelated.bin", unrelated});
    MapSaveService::Run(task);
    if (!task.succeeded) Check({task.diagnostics});
    Require(task.succeeded && task.nextSession && !task.terrainShadow, "普通保存不调用阴影生成或要求方向来源");
    Require(Take(task.nextSession->ReadMember("war3map.shd")) == fixture.Member("war3map.shd")
        && Take(task.nextSession->ReadMember("Fixture/Unrelated.bin")) == unrelated,
        "普通保存保留全部旧SHD哨兵并保存普通编辑");
    fixture.VerifySource();
}

enum class Tamper { Shadow, Owner, Terrain, Direction };
void Alter(Fixture& fixture, MapSaveTask& task, Tamper tamper) {
    Require(bool(task.terrainShadow), "篡改发生前必须已成功计算真实阴影");
    switch (tamper) {
    case Tamper::Shadow: ShadowOutput(task).data.front() ^= std::byte{0xff}; break;
    case Tamper::Owner: task.terrainShadow.reset(); break;
    case Tamper::Terrain: {
        auto different = fixture.terrain; different.vertices.front().groundHeightRaw += 4;
        RefreshTerrainDerived(different, 0, 0, 9, 9);
        task.replacements.push_back({"war3map.w3e", Take(SerializeMapTerrain(different))}); break;
    }
    case Tamper::Direction: fixture.Direction("[Light]\nDirection=3,0,-5\n"); break;
    }
}
void FinalCallbackCannotChangeOutputOwnerTerrainOrActualDirection() {
    for (const auto tamper : {Tamper::Shadow, Tamper::Owner, Tamper::Terrain, Tamper::Direction}) {
        Fixture fixture; MapSaveTask task; fixture.Freeze(task); fixture.Prepare(task); bool reached = false;
        task.beforePublish = [&] { reached = true; Alter(fixture, task, tamper); return Status::Ok(); };
        MapSaveService::Run(task);
        Require(reached, "必须实际到达生成后的最后宿主回调，不能用早期失败冒充篡改保护");
        fixture.VerifyDenied(task);
    }
}
void PublicationProgressCannotTamperAfterEarlierChecks() {
    for (const auto stage : {"archive-candidate", "archive-in-place", "archive-preflight"}) {
        for (const auto tamper : {Tamper::Shadow, Tamper::Owner, Tamper::Terrain, Tamper::Direction}) {
            Fixture fixture; MapSaveTask task; fixture.Freeze(task); fixture.Prepare(task); bool reached = false;
            if (std::string_view(stage) == "archive-in-place") {
                task.inPlace = true; task.target = fixture.source->SourcePath();
                task.inPlaceOptions.backupDirectory = fixture.root / "backups";
            }
            const auto progress = [&](std::string_view current) {
                if (current == stage) { reached = true; Alter(fixture, task, tamper); }
            };
            if (std::string_view(stage) == "archive-preflight") Rejected(MapSaveService::Preflight(task, progress));
            else MapSaveService::Run(task, progress);
            Require(reached, "必须到达archive发布展示回调后再尝试篡改"); fixture.VerifyDenied(task);
        }
    }
}

void CaptureBudgetsCancellationAndManualConflictsPreserveSource() {
    Fixture fixture;
    const auto capture = [&](const TerrainShadowBakeLimits& limits, std::stop_token token = {}) {
        return TerrainShadowBake::Capture(*fixture.source, fixture.game, {}, {}, limits, token);
    };
    auto limits = TerrainShadowBakeLimits{}; ++limits.maximumWorkingBytes;
    Rejected(capture(limits), "TERRAIN_SHADOW_LIMITS");
    limits = {}; limits.maximumSourceBytes = 1; Rejected(capture(limits));
    limits = {}; limits.maximumWorkingBytes = 1; Rejected(capture(limits));
    limits = {}; limits.maximumWork = 1; Rejected(capture(limits));
    limits = {}; limits.terrain.maximumSampleBytes = kSide * kSide * sizeof(float) - 1; Rejected(capture(limits));
    limits = {}; limits.terrain.maximumWork = 0; Rejected(capture(limits));
    const auto caster = Take(TerrainShadowCasterResources::Capture(*fixture.source, fixture.game));
    limits = {}; limits.maximumWorkingBytes = caster->WorkingBytes() + 1;
    Rejected(capture(limits)); // 方向owner不得在caster之后借用已耗尽的工作空间。
    std::stop_source cancelled; cancelled.request_stop();
    Rejected(capture({}, cancelled.get_token()), "TERRAIN_SHADOW_CANCELLED");
    for (const bool remove : {false, true}) {
        MapSaveTask task; fixture.Freeze(task);
        if (remove) task.removals.push_back("war3map.shd");
        else task.replacements.push_back({"war3map.shd", fixture.Member("war3map.shd")});
        std::shared_ptr<const TerrainSavePlan> plan;
        Rejected(PrepareTerrainShadowBake(task, fixture.game, nullptr, plan), "TERRAIN_SHADOW_OUTPUT_CONFLICT");
        Require(!plan && !task.terrainShadow && !std::filesystem::exists(task.target), "冲突不得安装部分计划或发布地图");
    }
    fixture.VerifySource();
}

void AdmissionClonesPlanAndReadbackKeepsItsOtherMembers() {
    Fixture fixture; auto edits = std::make_unique<MapDataEditSession>(MapDocument::FromArchive(*fixture.source));
    Check(edits->AttachTerrain(fixture.Member("war3map.w3e"))); Check(edits->AttachPathing(fixture.Member("war3map.wpm")));
    auto baseline = std::make_shared<const TerrainSavePlan>(Take(TerrainSavePlan::Prepare(*fixture.source, *edits)));
    Check(baseline->SaveGate()); const auto originalShadowSha = baseline->Members()[2].outputSha256;
    auto plan = baseline; MapSaveTask task; fixture.Freeze(task);
    task.prepareFinalDerived = [&](MapSaveTask& current) { return PrepareTerrainShadowBake(current, fixture.game, edits.get(), plan); };
    task.beforePublish = [&] {
        Require(plan != baseline && baseline->Members()[2].outputSha256 == originalShadowSha,
            "新阴影计划不得提前改写调用方保留的旧计划");
        return plan->VerifyFrozenOutput(task.sourceSha256, task.replacements, task.removals);
    };
    MapSaveService::Run(task);
    if (!task.succeeded) Check({task.diagnostics});
    Require(task.succeeded && task.nextSession && plan->Members()[2].action == TerrainSaveMemberAction::WriteEdited
        && plan->Members()[2].reasonCode == "TERRAIN_SAVE_WRITE_REBUILT_SHADOW", "同一正式计划必须登记真实阴影输出");
    Check(plan->VerifyReadback(*task.nextSession));
    for (std::size_t i = 0; i < plan->Members().size(); ++i) if (i != 2)
        Require(plan->Members()[i].outputSha256 == baseline->Members()[i].outputSha256,
            "阴影admission不能丢失或替换原计划其它派生文件身份");
    fixture.VerifySource();
}
} // namespace

int main() {
    try {
        ExplicitBakeWritesCompleteLiteralShadowAndPreservesOtherMembers();
        OrdinarySaveRetainsOriginalShadowAndDoesNotNeedGameProfiles();
        FinalCallbackCannotChangeOutputOwnerTerrainOrActualDirection();
        PublicationProgressCannotTamperAfterEarlierChecks();
        CaptureBudgetsCancellationAndManualConflictsPreserveSource();
        AdmissionClonesPlanAndReadbackKeepsItsOtherMembers();
        std::cout << "TerrainShadowBakeTests passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
