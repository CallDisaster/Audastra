#include "War3MapCore/Services/TerrainShadowReceiverResources.h"
#include "War3MapCore/Services/TerrainShadowBake.h"
#include "War3MapCore/Services/MapSaveService.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/IO/FileBytes.h"
#include "War3MapCore/MapData/TerrainEditing.h"
#include "War3MapCore/MapData/MapShadow.h"

#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <bit>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {
using namespace War3Map;
using namespace War3Map::MapData;
using namespace War3Map::Services;
using Bytes = std::vector<std::byte>;
void Require(bool value, const char* why) { if (!value) throw std::runtime_error(why); }
void RequireF32Bits(float actual, std::uint32_t expected, const char* why) {
    std::ostringstream detail;
    detail << why << ": actual=" << std::setprecision(17) << actual << " (0x" << std::hex
        << std::bit_cast<std::uint32_t>(actual) << "), expected=0x" << expected;
    std::cout << detail.str() << '\n';
    if (std::bit_cast<std::uint32_t>(actual) != expected) throw std::runtime_error(detail.str());
}
template<class T> T Take(Result<T> result) {
    if (!result.Succeeded() || !result.value) {
        std::string message;
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
void HasCode(std::span<const Diagnostic> diagnostics, std::string_view expected) {
    Require(!diagnostics.empty(), "拒绝必须给出诊断");
    if (expected.empty() || std::any_of(diagnostics.begin(), diagnostics.end(),
        [&](const auto& diagnostic) { return diagnostic.code == expected; })) return;
    std::string message = "缺少预期错误：" + std::string(expected);
    for (const auto& diagnostic : diagnostics) message += " | " + diagnostic.code + ":" + diagnostic.message;
    throw std::runtime_error(message);
}
template<class T> void Rejected(const Result<T>& result, std::string_view expected = {}) {
    Require(!result.Succeeded() && !result.value, "失败不能返回部分接收面"); HasCode(result.diagnostics, expected);
}
void Rejected(const Status& status, std::string_view expected = {}) {
    Require(!status.Succeeded(), "过期输入必须拒绝"); HasCode(status.diagnostics, expected);
}
Bytes Text(std::string_view value) {
    Bytes result; for (const auto character : value) result.push_back(static_cast<std::byte>(character)); return result;
}
struct Writer {
    Bytes bytes;
    void Raw(const void* data, std::size_t count) {
        const auto* begin = static_cast<const std::byte*>(data); bytes.insert(bytes.end(), begin, begin + count);
    }
    void Tag(const char* value) { Raw(value, 4); }
    void U32(std::uint32_t value) { Raw(&value, 4); }
    void U16(std::uint16_t value) { Raw(&value, 2); }
    void F32(float value) { Raw(&value, 4); }
    void Fixed(const char* value, std::size_t length) {
        std::vector<char> data(length, 0); std::memcpy(data.data(), value, std::min(length, std::strlen(value))); Raw(data.data(), length);
    }
    std::size_t Begin() { const auto offset = bytes.size(); U32(0); return offset; }
    void End(std::size_t offset, bool inclusive = false) {
        const auto count = static_cast<std::uint32_t>(bytes.size() - offset - (inclusive ? 0 : 4));
        std::memcpy(bytes.data() + offset, &count, 4);
    }
    void Extent(float height) { F32(4096); F32(-128); F32(0); F32(height); F32(0); F32(256); F32(height); }
};

// 原创合法MDX，保存与射线的expected只用下方独立字面值；不调用正式采样器造expected。
// 原局部面x=-128..0、y=0..width，原版-90度旋转后覆盖x=0..width、y=0..128。
Bytes Model(float height, bool lastMiss = false, float width = 128, bool dynamic = false, bool strip = false,
    bool emptyLastGroup = false, unsigned textureCount = 1) {
    Writer w; w.Tag("MDLX"); w.Tag("VERS"); w.U32(4); w.U32(800);
    w.Tag("MODL"); { const auto chunk = w.Begin(); w.Fixed("AuthoredReceiver", 80); w.Fixed("", 260); w.Extent(height); w.U32(0); w.End(chunk); }
    w.Tag("SEQS"); { const auto chunk = w.Begin(); w.Fixed("Stand", 80); w.U32(0); w.U32(1000);
        w.F32(0); w.U32(0); w.F32(0); w.U32(0); w.Extent(height); w.End(chunk); }
    w.Tag("TEXS"); w.U32(268 * textureCount);
    for (unsigned i = 0; i < textureCount; ++i) { w.U32(0); w.Fixed("Fixture/Unused.blp", 260); w.U32(0); }
    w.Tag("MTLS"); { const auto chunk = w.Begin(), material = w.Begin(); w.U32(0); w.U32(0); w.Tag("LAYS"); w.U32(1);
        const auto layer = w.Begin(); w.U32(0); w.U32(0); w.U32(0); w.U32(UINT32_MAX); w.U32(0); w.F32(0);
        w.End(layer, true); w.End(material, true); w.End(chunk); }
    w.Tag("GEOS"); { const auto chunk = w.Begin(), geoset = w.Begin();
        w.Tag("VRTX"); w.U32(4);
        for (const auto& point : std::array<std::array<float, 3>, 4>{{{-128,0,height}, {-128,width,height}, {0,0,height}, {0,width,height}}})
            for (const auto value : point) w.F32(value);
        w.Tag("NRMS"); w.U32(4); for (unsigned i = 0; i < 4; ++i) { w.F32(0); w.F32(0); w.F32(1); }
        w.Tag("PTYP"); w.U32(lastMiss || emptyLastGroup ? 2 : 1); w.U32(strip ? 5 : 4);
        if (lastMiss || emptyLastGroup) w.U32(emptyLastGroup ? 4 : 0x9999);
        w.Tag("PCNT"); w.U32(lastMiss || emptyLastGroup ? 2 : 1); w.U32(strip ? 4 : 6);
        if (lastMiss || emptyLastGroup) w.U32(emptyLastGroup ? 0 : 3);
        w.Tag("PVTX"); w.U32((strip ? 4 : 6) + (lastMiss ? 3 : 0));
        if (strip) for (const auto i : {0u, 1u, 2u, 3u}) w.U16(static_cast<std::uint16_t>(i));
        else for (const auto i : {0u, 1u, 2u, 2u, 1u, 3u}) w.U16(static_cast<std::uint16_t>(i));
        if (lastMiss) { w.U16(0); w.U16(1); w.U16(2); }
        w.Tag("GNDX"); w.U32(4); w.U32(0);
        w.Tag("MTGC"); w.U32(1); w.U32(1); w.Tag("MATS"); w.U32(1); w.U32(0);
        w.U32(0); w.U32(0); w.U32(0); w.Extent(height); w.U32(0);
        w.Tag("UVAS"); w.U32(1); w.Tag("UVBS"); w.U32(4);
        for (unsigned i = 0; i < 4; ++i) { w.F32(0); w.F32(0); }
        w.End(geoset, true); w.End(chunk); }
    // 显示隐藏不能删除接收几何：静态材质alpha=0且GEOA alpha=0。
    w.Tag("GEOA"); { const auto chunk = w.Begin(), entry = w.Begin(); w.F32(0); w.U32(0); w.F32(1); w.F32(1); w.F32(1); w.U32(0);
        w.End(entry, true); w.End(chunk); }
    w.Tag("BONE"); { const auto chunk = w.Begin(), node = w.Begin(); w.Fixed("Receiver", 80); w.U32(0); w.U32(UINT32_MAX); w.U32(0);
        if (dynamic) { w.Tag("KGTR"); w.U32(1); w.U32(0); w.U32(UINT32_MAX); w.U32(0); w.F32(0); w.F32(0); w.F32(0); }
        w.End(node, true); w.U32(UINT32_MAX); w.U32(UINT32_MAX); w.End(chunk); }
    w.Tag("PIVT"); w.U32(12); w.F32(0); w.F32(0); w.F32(0);
    return std::move(w.bytes);
}
Bytes CliffTable(std::string_view prefix = "Fixture/Cliff") {
    return Text("ID;PWXL;N;E\nC;X1;Y1;K\"cliffID\"\nC;X2;Y1;K\"cliffModelDir\"\n"
        "C;X1;Y2;K\"C001\"\nC;X2;Y2;K\"" + std::string(prefix) + "\"\nE\n");
}
constexpr std::string_view kZero = "Doodads/Terrain/Fixture/CliffABBA0.mdx";
constexpr std::string_view kOne = "Doodads/Terrain/Fixture/CliffABBA1.mdx";
constexpr std::string_view kTwo = "Doodads/Terrain/Fixture/CliffABBA2.mdx";

struct Fixture {
    std::filesystem::path root, game;
    TerrainGridSnapshot terrain;
    std::vector<MapArchiveMember> members;
    std::unique_ptr<MapArchiveSession> source;
    unsigned sequence = 0;
    explicit Fixture(bool ordinary = false) {
        static std::atomic<unsigned> next{0};
        root = std::filesystem::temp_directory_path() / ("Audastra-shadow-receivers-" + std::to_string(GetCurrentProcessId())
            + "-" + std::to_string(GetTickCount64()) + "-" + std::to_string(next++));
        Require(std::filesystem::create_directory(root), "创建原创receiver临时目录");
        game = root / "game"; Require(std::filesystem::create_directory(game), "创建原创游戏来源目录");
        Check(IO::WriteFileBytes(game / "WorldEditData.txt", Text("[Light]\nDirection=3,0,-4\n[Terrain]\nMaxHeight=1536\nMinHeight=-256\n")));
        Take(WriteVerifiedArchive(game / "war3.mpq", std::vector<MapArchiveMember>{{"Fixture/Unused.bin", Text("authored")}}));
        terrain.header.width = 3; terrain.header.height = 2; terrain.header.mainTileset = 'L';
        terrain.header.groundTilesets = {"G001"}; terrain.header.cliffTilesets = {"C001"}; terrain.vertices.resize(6);
        for (std::size_t i = 0; i < 6; ++i) {
            terrain.vertices[i].groundHeightRaw = 8192;
            terrain.vertices[i].cliffAndLayerRaw = ordinary || i % 3 == 0 ? 2 : 3;
            terrain.vertices[i].variationRaw = 5u << 5;
        }
        RefreshTerrainDerived(terrain, 0, 0, 3, 2);
        members = {{"war3map.w3e", Take(SerializeMapTerrain(terrain))},
            {"war3map.shd", Bytes(32, std::byte{0x7d})}, {"war3map.j", Text("// source script unchanged\n")},
            {"TerrainArt/CliffTypes.slk", CliffTable()}, {std::string(kZero), Model(64)}, {std::string(kOne), Model(96)}};
        Open();
    }
    ~Fixture() { source.reset(); std::error_code error; std::filesystem::remove_all(root, error); }
    void Open() {
        source = std::make_unique<MapArchiveSession>(Take(MapArchiveSession::Open(
            Take(WriteVerifiedArchive(root / ("source-" + std::to_string(sequence++) + ".w3x"), members)).path)));
    }
    void Set(std::string name, Bytes bytes) {
        const auto found = std::find_if(members.begin(), members.end(), [&](const auto& item) { return item.name == name; });
        if (found == members.end()) members.push_back({std::move(name), std::move(bytes)}); else found->data = std::move(bytes);
    }
    void Erase(std::string_view name) { std::erase_if(members, [&](const auto& item) { return item.name == name; }); }
    void ReplaceBaseGameArchive(std::span<const MapArchiveMember> replacementMembers) {
        // 只替换本夹具自建的固定base文件；生产writer仍须拒绝任何已存在目标。
        const auto target = game / "war3.mpq";
        Require(std::filesystem::canonical(game).parent_path() == std::filesystem::canonical(root)
            && std::filesystem::is_regular_file(std::filesystem::symlink_status(target)),
            "仅允许替换原创receiver夹具内自有的普通war3.mpq");
        const auto replacement = Take(WriteVerifiedArchive(
            root / ("replacement-game-" + std::to_string(sequence++) + ".mpq"), replacementMembers));
        if (!ReplaceFileW(target.c_str(), replacement.path.c_str(), nullptr, REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr))
            throw std::runtime_error("替换原创receiver游戏归档失败：" + std::to_string(GetLastError()));
        Require(Take(Sha256File(target)) == replacement.sha256, "替换后原创游戏归档必须与已验证候选一致");
    }
    auto Capture(std::span<const MemberReplacement> output = {}, const TerrainShadowReceiverLimits& limits = {}) const {
        auto casters = Take(TerrainShadowCasterResources::Capture(*source, game, output));
        auto light = Take(TerrainShadowResources::Capture(game, Game::NativeEditorStartupRules::Classic127aExpansion, 16u * 1024u * 1024u));
        return TerrainShadowReceiverResources::Capture(*source, game, std::move(casters), light, output, {}, limits);
    }
};

void OrderedAndSaved() {
    Fixture fixture;
    const auto owner = Take(fixture.Capture());
    Require(owner->RecordCount() == 1, "异层格生成单个原版cliff record");
    const auto grid = Take(owner->Sample());
    Require(grid.width == 8 && grid.height == 4 && grid.receiverHeights.size() == 32, "完整SHD像素中心网格");
    for (std::size_t y = 0; y < 4; ++y) for (std::size_t x = 0; x < 8; ++x)
        Require(grid.receiverHeights[y * 8 + x] == (x < 4 ? 96.1f : 128.1f), "variation5 modulo2取1；隐藏GEOS仍接收；普通格保原地高");
    Check(owner->VerifySources(*fixture.source));
    auto bake = Take(TerrainShadowBake::Capture(*fixture.source, fixture.game));
    const std::array<MemberReplacement, 1> output{{{"war3map.shd", bake->Bytes()}}};
    Check(bake->VerifyFrozenOutput(*fixture.source, output));
    const auto saved = Take(fixture.source->SaveCandidate(fixture.root / "candidate.w3x", output));
    auto reopened = Take(MapArchiveSession::Open(saved.path));
    Check(bake->VerifyReadback(reopened)); Check(owner->VerifyReadback(reopened));
    Require(Take(reopened.ReadMember("war3map.j")) == Text("// source script unchanged\n"), "计算SHD不改脚本");
    Require(Take(fixture.source->ReadMember("war3map.shd")) == Bytes(32, std::byte{0x7d}), "原创源归档只读");

    fixture.Set(std::string(kOne), Model(96, true)); fixture.Open();
    const auto lastMiss = Take(Take(fixture.Capture())->Sample());
    for (std::size_t y = 0; y < 4; ++y) for (std::size_t x = 0; x < 4; ++x)
        Require(lastMiss.receiverHeights[y * 8 + x] == (x < 2 ? 0.1f : 128.1f), "最后未知组false必须最近顶点回退，不是any-hit");
    fixture.Set(std::string(kOne), Model(96, false, 128, false, false, true)); fixture.Open();
    const auto emptyLast = Take(Take(fixture.Capture())->Sample());
    Require(emptyLast.receiverHeights == lastMiss.receiverHeights, "最后零长度list组仍返回false，不能丢空组沿用旧命中");
    fixture.Set(std::string(kOne), Model(3000, false, 128, false, true)); fixture.Open();
    const auto negative = Take(Take(fixture.Capture())->Sample());
    // 独立IEEE F32算式：原角度常量得到cos=-1.6292068494294654e-7，首三角
    // det=-16383.999283832974，inverse=0xb8800000，交距=-439.9999694824219
    // (0xc3dbffff)。第二三角v=-3.725290298461914e-8，须拒绝。
    // 最终F32(2560-t+0.1)=3000.099853515625，不能用理想平面3000.1f造expected。
    RequireF32Bits(negative.receiverHeights.front(), 0x453b8199u, "原strip保留；signed交距为负也参与最小值");
    fixture.Set(std::string(kOne), Model(64, false, 256)); fixture.Open();
    const auto overhang = Take(Take(fixture.Capture())->Sample());
    Require(overhang.receiverHeights[4] == 64.1f, "普通格向上扫描同block模型，不能只采当前格ground");

    for (auto& vertex : fixture.terrain.vertices) vertex.groundHeightRaw += 128;
    const std::array<MemberReplacement, 1> relief{{{"war3map.w3e", Take(SerializeMapTerrain(fixture.terrain))}}};
    Require(Take(Take(fixture.Capture(relief))->Sample()).receiverHeights.front() == 96.1f,
        "实际最终W3E的统一raw起伏抬升模型32，而不混入layer高度");

    Fixture bounded;
    Check(IO::WriteFileBytes(bounded.game / "WorldEditData.txt", Text("[Light]\nDirection=3,0,-4\n[Terrain]\nMaxHeight=0\nMinHeight=-256\n")));
    const auto skipped = Take(bounded.Capture());
    Require(skipped->RecordCount() == 0 && Take(skipped->Sample()).receiverHeights.front() == 0.1f,
        "原版MaxHeight拒建record后模型格返回0，再加0.1，不能偷偷夹回地高");
}

void CompleteCliffBakeUsesProductionSaveAndGuards() {
    Fixture fixture;
    // 左侧模型格保留作者的高模型面3000；右侧普通层地高128。实际方向
    // 从左向右每像素下降约42又2/3，故右侧4列全部被遮，左侧4列全亮。
    // expected是独立32字节字面格；不调用任何正式采样或投影函数生成。
    fixture.Set(std::string(kOne), Model(3000)); fixture.Open();
    const auto receiver = Take(fixture.Capture());
    Require(receiver->RecordCount() == 1, "生产保存夹具必须先确认确有普通cliff接收记录");
    Bytes expected(32, std::byte{0});
    for (std::size_t y = 0; y < 4; ++y) for (std::size_t x = 4; x < 8; ++x) expected[y * 8 + x] = std::byte{0xff};
    const auto prepare = [&](MapSaveTask& task) {
        Check(task.FreezeArchive(*fixture.source));
        task.prepareFinalDerived = [&](MapSaveTask& current) {
            Require(current.InFinalDerivedStage(), "cliff阴影必须经过正式最终派生阶段");
            std::shared_ptr<const TerrainSavePlan> plan;
            const auto status = PrepareTerrainShadowBake(current, fixture.game, nullptr, plan);
            Require(!plan, "仅计算cliff阴影不能伪造W3E/WPM地形编辑计划");
            return status;
        };
    };
    MapSaveTask task; prepare(task); task.target = fixture.root / "production-cliff.w3x";
    task.replacements.push_back({"Fixture/Unrelated.bin", Text("same-save edit sentinel")});
    MapSaveService::Run(task);
    if (!task.succeeded) Check({task.diagnostics});
    Require(task.executed && task.succeeded && task.nextSession && task.terrainShadow
        && task.terrainShadow->Width() == 8 && task.terrainShadow->Height() == 4
        && task.terrainShadow->Bytes() == expected && task.terrainShadow->CasterCount() == 0,
        "正式MapSaveService必须完成带cliff的Bake并写出完整16个FF字面SHD");
    const auto saved = Take(MapArchiveSession::Open(task.written));
    Require(Take(saved.ReadMember("war3map.shd")) == expected
        && Take(saved.ReadMember("Fixture/Unrelated.bin")) == Text("same-save edit sentinel"),
        "真实归档回读须同时保有完整cliff阴影和本次普通编辑");
    const auto decoded = Take(ParseMapShadow(Take(saved.ReadMember("war3map.shd")), 8, 4));
    Require(std::count(decoded.intensities.begin(), decoded.intensities.end(), 255) == 16,
        "持久SHD是00/FF强度，不能把逻辑bit或旧高位当输出");
    for (const auto& member : fixture.members) if (member.name != "war3map.shd")
        Require(Take(saved.ReadMember(member.name)) == member.data, "cliff阴影保存不得改动脚本/地形/表/模型等源成员");
    Check(task.terrainShadow->VerifyFrozenOutput(*fixture.source, task.replacements));
    Check(task.terrainShadow->VerifyReadback(saved));

    // 普通保存仍保原SHD；不因增加cliff重算能力而隐式进入生成器。
    MapSaveTask ordinary; Check(ordinary.FreezeArchive(*fixture.source));
    ordinary.target = fixture.root / "ordinary-cliff.w3x";
    ordinary.replacements.push_back({"Fixture/Unrelated.bin", Text("ordinary edit sentinel")});
    MapSaveService::Run(ordinary);
    if (!ordinary.succeeded) Check({ordinary.diagnostics});
    Require(ordinary.succeeded && ordinary.nextSession && !ordinary.terrainShadow
        && Take(ordinary.nextSession->ReadMember("war3map.shd")) == Bytes(32, std::byte{0x7d}),
        "含cliff的普通保存保留原SHD，不强制重算");

    for (const bool late : {false, true}) {
        MapSaveTask denied; prepare(denied);
        denied.target = fixture.root / (late ? "late-drift.w3x" : "final-drift.w3x");
        bool reached = false;
        const auto tamper = [&] {
            reached = true; Require(bool(denied.terrainShadow), "来源漂移必须发生在真实cliff阴影生成后");
            denied.replacements.push_back({std::string(kOne), Model(3001)});
        };
        if (!late) denied.beforePublish = [&] { tamper(); return Status::Ok(); };
        MapSaveService::Run(denied, [&](std::string_view stage) { if (late && stage == "archive-candidate") tamper(); });
        Require(reached && denied.executed && !denied.succeeded && !denied.nextSession
            && denied.written.empty() && !std::filesystem::exists(denied.target),
            "final与发布进度回调均不能绕过cliff模型来源绑定，也不能先发布再失败");
        HasCode(denied.diagnostics, "TERRAIN_SHADOW_RECEIVER_DEPENDENCY_STALE");
    }
    Require(Take(Sha256File(fixture.source->SourcePath())) == fixture.source->SourceSha256(),
        "成功、普通保存及两个晚期拒绝都不修改原创源图");
}

void FrozenSources() {
    Fixture fixture; const auto owner = Take(fixture.Capture());
    const std::array<MemberReplacement, 1> added{{{std::string(kTwo), Model(200)}}};
    Rejected(owner->VerifySources(*fixture.source, added), "TERRAIN_SHADOW_RECEIVER_DEPENDENCY_STALE");
    const auto next = Take(fixture.Capture(added));
    Require(Take(next->Sample()).receiverHeights.front() == 200.1f, "新增首缺变体改变count和modulo，旧absence不可沿用");
    auto saved = Take(fixture.source->SaveCandidate(fixture.root / "added-variant.w3x", added));
    Check(next->VerifyReadback(Take(MapArchiveSession::Open(saved.path))));
    Rejected(next->VerifyReadback(*fixture.source), "TERRAIN_SHADOW_RECEIVER_READBACK_STALE");
    const std::array<MemberReplacement, 1> mdl{{{"Doodads/Terrain/Fixture/CliffABBA0.mdl", Text("authored mdl sentinel")}}};
    Rejected(fixture.Capture(mdl), "TERRAIN_SHADOW_RECEIVER_MDL_UNSUPPORTED");
    Rejected(owner->VerifySources(*fixture.source, mdl), "TERRAIN_SHADOW_RECEIVER_DEPENDENCY_STALE");
    const std::array<MemberReplacement, 1> model{{{std::string(kOne), Model(97)}}};
    Rejected(owner->VerifySources(*fixture.source, model), "TERRAIN_SHADOW_RECEIVER_DEPENDENCY_STALE");
    const std::array<MemberReplacement, 1> table{{{"TerrainArt/CliffTypes.slk", CliffTable("Changed/Cliff")}}};
    Rejected(owner->VerifySources(*fixture.source, table), "TERRAIN_SHADOW_RECEIVER_DEPENDENCY_STALE");

    Fixture otherProfile;
    Check(IO::WriteFileBytes(otherProfile.game / "WorldEditData.txt", Text("[Light]\nDirection=3,0,-4\n[Terrain]\nMaxHeight=1408\nMinHeight=-256\n")));
    const auto foreignLight = Take(TerrainShadowResources::Capture(otherProfile.game,
        Game::NativeEditorStartupRules::Classic127aExpansion, 16u * 1024u * 1024u));
    const auto casters = Take(TerrainShadowCasterResources::Capture(*fixture.source, fixture.game));
    Rejected(TerrainShadowReceiverResources::Capture(*fixture.source, fixture.game, casters, foreignLight),
        "TERRAIN_SHADOW_RECEIVER_PROFILE_STALE");

    fixture.Erase(kZero); fixture.Erase(kOne); fixture.Open();
    fixture.ReplaceBaseGameArchive(std::vector<MapArchiveMember>{{std::string(kZero), Model(64)}});
    const auto gameOwner = Take(fixture.Capture());
    const auto gameModel = std::find_if(gameOwner->Dependencies().begin(), gameOwner->Dependencies().end(),
        [](const auto& fact) { return fact.source == TerrainShadowCasterSource::Game; });
    Require(gameModel != gameOwner->Dependencies().end() && !gameModel->gameContainer.empty(), "实际game模型winner具备容器身份");
    const std::array<MemberReplacement, 1> sameMap{{{std::string(kZero), Model(64)}}};
    Rejected(gameOwner->VerifySources(*fixture.source, sameMap), "TERRAIN_SHADOW_RECEIVER_DEPENDENCY_STALE");
    Take(WriteVerifiedArchive(fixture.game / "war3patch.mpq", std::vector<MapArchiveMember>{{std::string(kZero), Model(64)}}));
    Rejected(gameOwner->VerifySources(*fixture.source));
}

void ActualGameRootCannotBeMixedWithAnotherCasterOwner() {
    Fixture fixture;
    fixture.Erase(kZero); fixture.Erase(kOne); fixture.Open();
    const auto otherGame = fixture.root / "second-game";
    Require(std::filesystem::create_directory(otherGame), "创建第二个原创游戏来源目录");
    const auto profile = Text("[Light]\nDirection=3,0,-4\n[Terrain]\nMaxHeight=1536\nMinHeight=-256\n");
    Check(IO::WriteFileBytes(otherGame / "WorldEditData.txt", profile));
    Require(Take(IO::ReadFileBytes(fixture.game / "WorldEditData.txt", 4096)) == profile,
        "错root回归的两个真实目录必须具有相同profile字节，不能仅靠光照数值差异拒绝");
    fixture.ReplaceBaseGameArchive(std::vector<MapArchiveMember>{{std::string(kZero), Model(64)}});
    Take(WriteVerifiedArchive(otherGame / "war3.mpq", std::vector<MapArchiveMember>{{std::string(kZero), Model(200)}}));
    const auto casters = Take(TerrainShadowCasterResources::Capture(*fixture.source, fixture.game));
    const auto otherCasters = Take(TerrainShadowCasterResources::Capture(*fixture.source, otherGame));
    const auto light = Take(TerrainShadowResources::Capture(fixture.game,
        Game::NativeEditorStartupRules::Classic127aExpansion, 16u * 1024u * 1024u));
    const auto otherLight = Take(TerrainShadowResources::Capture(otherGame,
        Game::NativeEditorStartupRules::Classic127aExpansion, 16u * 1024u * 1024u));
    std::error_code pathError;
    Require(casters->GameRoot().is_absolute()
        && std::filesystem::equivalent(casters->GameRoot(), fixture.game, pathError) && !pathError,
        "caster只读身份须固定于捕获时实际存在的规范目录");
    const auto own = Take(TerrainShadowReceiverResources::Capture(*fixture.source, fixture.game, casters, light));
    const auto other = Take(TerrainShadowReceiverResources::Capture(*fixture.source, otherGame, otherCasters, otherLight));
    Require(Take(own->Sample()).receiverHeights.front() == 64.1f
        && Take(other->Sample()).receiverHeights.front() == 200.1f,
        "两个原创游戏目录各自正确绑定实际不同的模型winner");
    // B的light/profile与receiver根均一致，唯一错配是借了A的caster owner。
    // 不能等到profile漂移检查再偶然拒绝，更不能把A的DOO筛选与B的模型混成成功。
    Rejected(TerrainShadowReceiverResources::Capture(*fixture.source, otherGame, casters, otherLight),
        "TERRAIN_SHADOW_RECEIVER_GAME_ROOT");
    Rejected(TerrainShadowReceiverResources::Capture(*fixture.source, fixture.game, otherCasters, light),
        "TERRAIN_SHADOW_RECEIVER_GAME_ROOT");
    Rejected(TerrainShadowReceiverResources::Capture(*fixture.source, {}, casters, light),
        "TERRAIN_SHADOW_RECEIVER_GAME_ROOT");
    const auto mapOnlyCasters = Take(TerrainShadowCasterResources::Capture(*fixture.source, {}));
    Require(mapOnlyCasters->GameRoot().empty(), "没有配置游戏来源的caster保持空身份");
    Rejected(TerrainShadowReceiverResources::Capture(*fixture.source, fixture.game, mapOnlyCasters, light),
        "TERRAIN_SHADOW_RECEIVER_GAME_ROOT");
    const auto alias = fixture.game / ".." / fixture.game.filename() / ".";
    const auto equivalent = Take(TerrainShadowReceiverResources::Capture(*fixture.source, alias, casters, light));
    const auto aliasCasters = Take(TerrainShadowCasterResources::Capture(*fixture.source, alias));
    const auto equivalentOwner = Take(TerrainShadowReceiverResources::Capture(*fixture.source, fixture.game, aliasCasters, light));
    Require(Take(equivalent->Sample()).receiverHeights == Take(own->Sample()).receiverHeights
        && Take(equivalentOwner->Sample()).receiverHeights == Take(own->Sample()).receiverHeights,
        "同一实际目录的等价路径在receiver/caster任一侧都正确可用");
    Check(equivalent->VerifySources(*fixture.source));
}

void GuardsAndOrdinary() {
    Fixture ordinary(true); const auto owner = Take(ordinary.Capture());
    Require(owner->RecordCount() == 0 && owner->Dependencies().empty() && owner->SourceBytes() == 0,
        "普通面不读取悬崖模型或额外表");
    const auto direct = Take(SampleOrdinaryTerrainShadowHeights(ordinary.terrain, {}));
    const auto actual = Take(owner->Sample());
    Require(direct.receiverHeights == actual.receiverHeights && direct.work == actual.work, "普通采样输出与旧路径完全一致");
    Fixture fixture;
    auto limits = TerrainShadowReceiverLimits{}; limits.maximumSourceBytes = 1;
    Rejected(fixture.Capture({}, limits));
    limits = {}; limits.maximumWorkingBytes = 1; Rejected(fixture.Capture({}, limits));
    limits = {}; limits.maximumWork = 1; Rejected(fixture.Capture({}, limits), "TERRAIN_SHADOW_RECEIVER_BUDGET");
    limits = {}; ++limits.maximumModels; Rejected(fixture.Capture({}, limits), "TERRAIN_SHADOW_RECEIVER_LIMITS");
    const auto captured = Take(fixture.Capture());
    TerrainShadowRebuildLimits sampleLimits; sampleLimits.maximumWork = 1;
    Rejected(captured->Sample(sampleLimits), "TERRAIN_SHADOW_RECEIVER_BUDGET");
    sampleLimits = {}; sampleLimits.maximumSampleBytes = 1; Rejected(captured->Sample(sampleLimits));
    std::stop_source stop; stop.request_stop(); Rejected(captured->Sample({}, stop.get_token()), "TERRAIN_SHADOW_RECEIVER_CANCELLED");
    fixture.Set(std::string(kOne), Model(96, false, 128, true)); fixture.Open();
    Rejected(fixture.Capture(), "TERRAIN_SHADOW_RECEIVER_CONTROLLER");
    fixture.Set(std::string(kOne), Model(96)); fixture.terrain.vertices[0].textureAndFlagsRaw |= 0x10;
    fixture.Set("war3map.w3e", Take(SerializeMapTerrain(fixture.terrain))); fixture.Open();
    Rejected(fixture.Capture(), "TERRAIN_SHADOW_RAMP_GEOMETRY");
    fixture.terrain.vertices[0].textureAndFlagsRaw &= ~0x10;
    fixture.terrain.vertices[0].waterHeightAndFlagsRaw |= 0x8000;
    fixture.Set("war3map.w3e", Take(SerializeMapTerrain(fixture.terrain))); fixture.Open();
    Rejected(fixture.Capture(), "TERRAIN_SHADOW_TERRAIN_FLAGS");
    Fixture special;
    DoodadPlacements placements; placements.specialDoodads.push_back({"S001", 0, {0, 0}});
    special.Set("war3map.doo", Take(placements.Serialize())); special.Open();
    Rejected(special.Capture(), "TERRAIN_SHADOW_SPECIAL_GEOMETRY");
}
void BoundedSourceAndReadbackVerification() {
    Fixture fixture;
    // 每个有效MDX约69KiB，两个变体及缺失项都须复核。256KiB容得下一项
    // stored/decoded缓冲，却容不下旧复核所保留的两份模型缓存与解析工作。
    fixture.Set(std::string(kZero), Model(64, false, 128, false, false, false, 256));
    fixture.Set(std::string(kOne), Model(96, false, 128, false, false, false, 256)); fixture.Open();
    const auto owner = Take(fixture.Capture());
    TerrainShadowCasterVerificationLimits scratch; scratch.maximumWorkingBytes = 256u * 1024u;
    Require(owner->WorkingBytes() > scratch.maximumWorkingBytes, "窄复核额度小于完整接收面捕获工作量");
    TerrainShadowReceiverLimits recapture; recapture.maximumWorkingBytes = scratch.maximumWorkingBytes;
    Rejected(fixture.Capture({}, recapture));
    Check(owner->VerifySources(*fixture.source, {}, {}, scratch));
    const auto candidate = Take(fixture.source->SaveCandidate(fixture.root / "bounded-receiver-readback.w3x", {}));
    const auto saved = Take(MapArchiveSession::Open(candidate.path)); Check(owner->VerifyReadback(saved, scratch));
    auto tiny = scratch; tiny.maximumWorkingBytes = 1;
    Rejected(owner->VerifySources(*fixture.source, {}, {}, tiny)); Rejected(owner->VerifyReadback(saved, tiny));
    tiny = scratch; tiny.maximumFileBytes = 1; Rejected(owner->VerifyReadback(saved, tiny));
    const std::vector<MemberReplacement> late{{std::string(kTwo), Model(128)}};
    Rejected(owner->VerifySources(*fixture.source, late, {}, scratch), "TERRAIN_SHADOW_RECEIVER_DEPENDENCY_STALE");
}
} // namespace

int main(int argc, char** argv) {
    try {
        const std::string_view group = argc > 1 ? argv[1] : "all";
        Require(group == "ordered" || group == "sources" || group == "guards" || group == "all", "未知测试分组");
        if (group == "ordered" || group == "all") { OrderedAndSaved(); CompleteCliffBakeUsesProductionSaveAndGuards(); }
        if (group == "sources" || group == "all") { FrozenSources(); ActualGameRootCannotBeMixedWithAnotherCasterOwner(); }
        if (group == "guards" || group == "all") { GuardsAndOrdinary(); BoundedSourceAndReadbackVerification(); }
        std::cout << "terrain shadow receiver " << group << " passed\n"; return 0;
    } catch (const std::exception& failure) { std::cerr << failure.what() << '\n'; return 1; }
}
