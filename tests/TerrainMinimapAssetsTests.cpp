#include "War3MapCore/Render/TerrainMinimapAssets.h"

#include "War3MapCore/Hash.h"
#include "War3MapCore/IO/FileBytes.h"
#include "War3MapCore/Mpq/MpqArchive.h"
#include "War3MapCore/MapData/TerrainEditing.h"
#include "War3MapCore/MapData/TerrainSavePlan.h"
#include "War3MapCore/MapDocument.h"
#include "../src/Core/Mpq/MpqCrypt.h"

#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <iostream>
#include <stdexcept>

namespace {
using namespace War3Map;
using namespace War3Map::Render;
using namespace War3Map::MapData;
using Bytes = std::vector<std::byte>;

void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class T> T Take(Result<T> result) {
    if (!result.Succeeded()) {
        std::string message = "fixture failed";
        for (const auto& d : result.diagnostics) message += " | " + d.code + ":" + d.message;
        throw std::runtime_error(message);
    }
    return std::move(*result.value);
}
template<class T> void Rejected(const Result<T>& result) {
    Require(!result.Succeeded() && !result.value && !result.diagnostics.empty(), "失败不能返回部分qualified资源快照");
}

void RequireRejected(const Status& status, std::string_view code = {}) {
    Require(!status.Succeeded() && !status.diagnostics.empty(), "最终依赖失败须有诊断");
    if (!code.empty()) Require(std::any_of(status.diagnostics.begin(), status.diagnostics.end(), [&](const auto& d) {
        return d.code == code;
    }), "最终依赖失败必须属于预期的具体拒绝门");
}

class TempDirectory {
public:
    TempDirectory() {
        static std::atomic<unsigned> next{0};
        path_ = std::filesystem::temp_directory_path() / ("Audastra-minimap-assets-" + std::to_string(GetCurrentProcessId())
            + "-" + std::to_string(GetTickCount64()) + "-" + std::to_string(next.fetch_add(1)));
        Require(std::filesystem::create_directory(path_), "创建独立临时fixture目录");
    }
    ~TempDirectory() { std::error_code error; std::filesystem::remove_all(path_, error); }
    const std::filesystem::path& Path() const { return path_; }
private:
    std::filesystem::path path_;
};

Bytes Text(std::string_view text) {
    Bytes bytes; for (const auto c : text) bytes.push_back(static_cast<std::byte>(c)); return bytes;
}
void Put32(Bytes& bytes, std::size_t at, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes[at + i] = static_cast<std::byte>((value >> (i * 8)) & 255);
}
Bytes W3i() {
    // 独立v25原字；8×8源tile扣L/R/B/T=[1,3,2,1]后为4×5。
    Bytes bytes(147);
    Put32(bytes, 0, 25); Put32(bytes, 8, 6061);
    Put32(bytes, 48, 1); Put32(bytes, 52, 3); Put32(bytes, 56, 2); Put32(bytes, 60, 1);
    Put32(bytes, 64, 4); Put32(bytes, 68, 5); bytes[76] = std::byte{'L'};
    return bytes;
}
MapDocument SettingsDocument(const std::optional<Bytes>& bytes) {
    if (!bytes) return MapDocument::FromMembers({});
    const std::vector<MapArchiveMember> members{{"war3map.w3i", *bytes}};
    return MapDocument::FromMembers(members);
}
Bytes Slk(const std::vector<std::string>& headers, const std::vector<std::vector<std::string>>& rows = {}) {
    std::string slk = "ID;PWXL;N;E\n";
    const auto add = [&](const auto& row, std::size_t y) {
        for (std::size_t x = 0; x < row.size(); ++x)
            slk += "C;X" + std::to_string(x + 1) + ";Y" + std::to_string(y) + ";K\"" + row[x] + "\"\n";
    };
    add(headers, 1); for (std::size_t i = 0; i < rows.size(); ++i) add(rows[i], i + 2);
    return Text(slk + "E\n");
}

Bytes Blp(std::array<unsigned, 3> top = {80, 120, 160}) {
    // 原创BLP1调色板字节，不调用生产encoder生成expected。16→8→4→2→1共341索引。
    Bytes bytes(1180 + 341);
    std::copy_n(Text("BLP1").begin(), 4, bytes.begin());
    Put32(bytes, 4, 1); Put32(bytes, 12, 16); Put32(bytes, 16, 16); Put32(bytes, 20, 5);
    const std::array<std::array<unsigned, 3>, 5> colors{top, std::array<unsigned, 3>{0, 200, 0},
        std::array<unsigned, 3>{20, 40, 60}, std::array<unsigned, 3>{250, 250, 0}, std::array<unsigned, 3>{255, 0, 255}};
    std::size_t at = 1180;
    for (std::uint32_t level = 0, size = 16; level < 5; ++level, size /= 2) {
        Put32(bytes, 28 + level * 4, static_cast<std::uint32_t>(at)); Put32(bytes, 92 + level * 4, size * size);
        const auto palette = 156 + (level + 1) * 4;
        for (std::size_t c = 0; c < 3; ++c) bytes[palette + c] = static_cast<std::byte>(colors[level][2 - c]);
        std::fill_n(bytes.begin() + static_cast<std::ptrdiff_t>(at), size * size, static_cast<std::byte>(level + 1));
        at += static_cast<std::size_t>(size) * size;
    }
    return bytes;
}

Bytes Rgba(std::uint32_t size, std::array<unsigned, 3> color) {
    Bytes bytes(static_cast<std::size_t>(size) * size * 4);
    for (std::size_t p = 0; p < bytes.size(); p += 4) {
        for (std::size_t c = 0; c < 3; ++c) bytes[p + c] = static_cast<std::byte>(color[c]);
        bytes[p + 3] = std::byte{255};
    }
    return bytes;
}

std::vector<MapArchiveMember> Materials(Bytes blp = Blp()) {
    return {{"TerrainArt\\Terrain.slk", Slk({"tileID", "dir", "file"}, {{"Tg01", "Fixture", "Ground"}, {"Tg02", "Fixture", "Ground"}})},
        {"TerrainArt\\CliffTypes.slk", Slk({"cliffID"})}, {"TerrainArt\\Water.slk", Slk({"waterID"})},
        {"Fixture\\Ground.blp", std::move(blp)}};
}

// 沿用既有TerrainSavePlan测试的独立未压缩MPQ布局思路；listfile只列W3E/WPM，
// 真正材质hash/块在清单之外。badTexture故意使用精确读取器明确拒绝的patch块标志。
Bytes UnlistedArchive(std::vector<MapArchiveMember> members, bool badTexture = false,
    std::string_view unreadableMember = "Fixture\\Ground.blp") {
    members.push_back({"(listfile)", Text("war3map.w3e\nwar3map.wpm\n")});
    const auto count = static_cast<std::uint32_t>(members.size());
    std::uint32_t hashCount = 8, payloadBytes = 0;
    while (hashCount < count * 2) hashCount *= 2;
    for (const auto& member : members) payloadBytes += static_cast<std::uint32_t>(member.data.size());
    const auto hashAt = 32 + payloadBytes, blockAt = hashAt + hashCount * 16;
    Bytes bytes(blockAt + count * 16);
    Put32(bytes, 0, 0x1a51504d); Put32(bytes, 4, 32); Put32(bytes, 8, static_cast<std::uint32_t>(bytes.size()));
    Put32(bytes, 12, 0x00030000); Put32(bytes, 16, hashAt); Put32(bytes, 20, blockAt);
    Put32(bytes, 24, hashCount); Put32(bytes, 28, count);
    std::fill(bytes.begin() + hashAt, bytes.begin() + blockAt, std::byte{255});
    const auto& crypt = Mpq::MpqCrypt::Instance(); std::uint32_t dataAt = 32;
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto& member = members[i]; const auto size = static_cast<std::uint32_t>(member.data.size());
        std::copy(member.data.begin(), member.data.end(), bytes.begin() + dataAt);
        auto bucket = crypt.HashString(member.name, Mpq::MpqCrypt::HashType::TableOffset) & (hashCount - 1);
        while (bytes[hashAt + bucket * 16 + 12] != std::byte{255}) bucket = (bucket + 1) & (hashCount - 1);
        const auto at = hashAt + bucket * 16;
        Put32(bytes, at, crypt.HashString(member.name, Mpq::MpqCrypt::HashType::NameA));
        Put32(bytes, at + 4, crypt.HashString(member.name, Mpq::MpqCrypt::HashType::NameB));
        Put32(bytes, at + 8, 0); Put32(bytes, at + 12, i);
        Put32(bytes, blockAt + i * 16, dataAt); Put32(bytes, blockAt + i * 16 + 4, size); Put32(bytes, blockAt + i * 16 + 8, size);
        Put32(bytes, blockAt + i * 16 + 12, Mpq::kFileExists | Mpq::kFileSingleUnit
            | (badTexture && member.name == unreadableMember ? Mpq::kFilePatch : 0u));
        dataAt += size;
    }
    crypt.EncryptBlock(bytes.data() + hashAt, hashCount * 16, crypt.HashString("(hash table)", Mpq::MpqCrypt::HashType::FileKey));
    crypt.EncryptBlock(bytes.data() + blockAt, count * 16, crypt.HashString("(block table)", Mpq::MpqCrypt::HashType::FileKey));
    return bytes;
}

struct Fixture {
    TempDirectory directory;
    Bytes w3e, wpm;
    std::optional<Bytes> w3i;
    MapDataEditSession edits;
    Fixture(bool duplicatePath = false, std::optional<Bytes> settings = {})
        : w3i(std::move(settings)), edits(SettingsDocument(w3i)) {
        TerrainGridSnapshot terrain;
        terrain.header.width = terrain.header.height = 9; terrain.header.mainTileset = 'L';
        terrain.header.groundTilesets = duplicatePath ? std::vector<std::string>{"Tg01", "Tg02"} : std::vector<std::string>{"Tg01"};
        terrain.header.cliffTilesets = {"CLdi"}; terrain.vertices.resize(81);
        for (auto& v : terrain.vertices) { v.groundHeightRaw = 8192; v.waterHeightAndFlagsRaw = 8192; v.cliffAndLayerRaw = 2; }
        RefreshTerrainDerived(terrain, 0, 0, 9, 9); w3e = Take(SerializeMapTerrain(terrain));
        wpm = Text("MP3W"); wpm.resize(16 + 32 * 32); Put32(wpm, 8, 32); Put32(wpm, 12, 32);
        Require(edits.AttachTerrain(w3e).Succeeded() && edits.AttachPathing(wpm).Succeeded(), "附上真实源W3E/WPM");
    }
    MapArchiveSession Source(std::string_view name = "source.w3x", std::vector<MapArchiveMember> resources = Materials()) {
        resources.push_back({"war3map.w3e", w3e}); resources.push_back({"war3map.wpm", wpm});
        if (w3i) resources.push_back({"war3map.w3i", *w3i});
        const auto path = directory.Path() / name;
        Take(WriteVerifiedArchive(path, resources)); return Take(MapArchiveSession::Open(path));
    }
    std::filesystem::path Game(std::vector<MapArchiveMember> resources = Materials()) {
        const auto root = directory.Path() / "game"; Require(std::filesystem::create_directory(root), "创建合成game目录");
        Take(WriteVerifiedArchive(root / "war3.mpq", resources)); return root;
    }
    void TextureVariation() {
        TerrainBrush brush; brush.kind = TerrainBrushKind::Texture; brush.radius = 0;
        brush.groundTexture = "Tg01"; brush.variation = 1;
        const TerrainStrokePoint point{256, 256};
        Require(edits.EditTerrain(brush, std::span(&point, 1), false).Succeeded(), "真实会话改变当前地面variation");
    }
};

void TestActualMapWinnerAndOriginalMips() {
    Fixture fixture;
    auto source = fixture.Source(); const auto game = fixture.Game(Materials(Blp({210, 30, 10})));
    auto captured = Take(CaptureTerrainMinimapAssets(source, fixture.edits, game));
    Require(captured.Dependencies().size() == 4 && captured.Tables().size() == 3 && captured.GroundAssets().size() == 1,
        "必须拥有实际三表→palette→唯一atlas依赖");
    Require(captured.Tables()[0].idColumn == "tileid" && captured.Tables()[0].rows == 2
        && captured.Dependencies()[captured.Tables()[0].dependencyIndex].encodedSha256 == Take(Sha256(Materials()[0].data))
        && captured.GroundAssets()[0].rawcode == "Tg01" && captured.GroundAssets()[0].tableRow == 2
        && captured.GroundAssets()[0].canonicalTexturePath == "FIXTURE\\GROUND.BLP",
        "真实Terrain.slk原bytes/SHA、严格row及canonical texture路径必须连成同一依赖");
    const auto& dependency = captured.Dependencies().at(captured.GroundAssets()[0].dependencyIndex);
    Require(dependency.mapMemberPresent && dependency.winner.layer == Resources::ResourceLayer::MapImport
        && dependency.containerIdentity == source.SourceSha256() && dependency.encodedSha256 == Take(Sha256(Blp())),
        "地图覆盖须读取真实encoded bytes，不能偷偷采用同名game BLP");
    Require(!captured.Facts().gameMounted && captured.Facts().exactMemberCaptureBatches == 3
        && captured.Facts().uniqueAtlasCount == 1, "完整地图覆盖不挂载无关game来源；map精确读取固定三轮");
    Require(dependency.decodedMips.size() == 5 && dependency.decodedMips[0].decodedRgbaSha256 == Take(Sha256(Rgba(16, {80, 120, 160})))
        && dependency.decodedMips[1].decodedRgbaSha256 == Take(Sha256(Rgba(8, {0, 200, 0})))
        && dependency.decodedMips[2].decodedRgbaSha256 == Take(Sha256(Rgba(4, {20, 40, 60}))),
        "独立手造BLP的各原mip颜色/字节必须经唯一decoder进入SHA事实，不能从mip0伪造");
    Require(captured.Binding().archiveSha256 == source.SourceSha256()
        && captured.Binding().originalTerrainSha256 == Take(Sha256(fixture.w3e))
        && captured.Binding().currentTerrainSha256 == Take(Sha256(captured.Input().currentW3e)), "source/current W3E身份应分别从真实字节得到");
    Require(!captured.formalSaveAuthorized && !captured.nativeEditorOrGameVerified && !captured.pendingImportReplaceRemoveCovered
        && !captured.w3iDooShdSemanticsCovered && !captured.Facts().includesMountAndMpqMemory
        && !captured.Facts().gameInternalInventoryIndependentlyVerified, "快照不能自授未覆盖来源/MPQ库存/原版/正式保存资格");
    Require(VerifyTerrainMinimapAssetsCurrent(captured, source, fixture.edits, game).Succeeded(), "真实未变化来源可重新核对");
    const auto candidate = Take(BuildTerrainMinimapCandidate(captured.Input()));
    Require(std::to_integer<unsigned>(candidate.image.rgba[0]) == 80 && !candidate.manifest.formalSaveAuthorized,
        "真实捕获输入可接既有候选，但仍无地图保存资格");
}

void TestGameFallbackAndWinnerChange() {
    Fixture fixture; auto source = fixture.Source("fallback.w3x", {}); const auto game = fixture.Game();
    const auto captured = Take(CaptureTerrainMinimapAssets(source, fixture.edits, game));
    Require(captured.Facts().gameMounted && captured.Facts().mapDependencyBytes == 0 && captured.Facts().gameDependencyBytes > 0,
        "地图准确missing后应读取真实game fallback并分开计字节");
    for (const auto& dependency : captured.Dependencies())
        Require(!dependency.mapMemberPresent && dependency.winner.layer == Resources::ResourceLayer::Game
            && dependency.containerIdentity == "war3.mpq", "必须记录实际命中的game MPQ，不能用笼统Game sourceId");
    Require(VerifyTerrainMinimapAssetsFrozenOutput(captured, source, {}).Succeeded(), "无覆盖的实际game来源可核最终依赖");
    for (const auto& material : Materials()) {
        const std::vector<MemberReplacement> added{material};
        RequireRejected(VerifyTerrainMinimapAssetsFrozenOutput(captured, source, added), "TERRAIN_ASSETS_FINAL_WINNER");
    } // 连相同编码字节也改变map/game winner，不能按SHA相同洗白。
    // 新增高层patch中相同字节的纹理；像素/SHA可相同，真实winner已改变，旧快照仍失效。
    Take(WriteVerifiedArchive(game / "war3patch.mpq", std::vector<MapArchiveMember>{{"Fixture\\Ground.blp", Blp()}}));
    Require(!VerifyTerrainMinimapAssetsCurrent(captured, source, fixture.edits, game).Succeeded(), "相同字节换高优先级winner必须使旧快照失效");
    RequireRejected(VerifyTerrainMinimapAssetsFrozenOutput(captured, source, {}), "TERRAIN_ASSETS_FINAL_DEPENDENCY");
    const auto changed = Take(CaptureTerrainMinimapAssets(source, fixture.edits, game));
    const auto& atlas = changed.Dependencies()[changed.GroundAssets()[0].dependencyIndex];
    Require(atlas.containerIdentity == "war3patch.mpq" && atlas.encodedSha256 == Take(Sha256(Blp()))
        && changed.IdentitySha256() != captured.IdentitySha256(), "winner身份必须进入snapshot摘要");
}

void TestSameGamePathChangedMember() {
    Fixture fixture; auto source = fixture.Source("game-replaced.w3x", {}); const auto game = fixture.Game();
    const auto captured = Take(CaptureTerrainMinimapAssets(source, fixture.edits, game));
    // 只移动本测试独占临时目录中的合成包；返回后的snapshot不暗中长期锁住作者来源。
    std::filesystem::rename(game / "war3.mpq", game / "previous-fixture.mpq");
    Take(WriteVerifiedArchive(game / "war3.mpq", Materials(Blp({220, 40, 20}))));
    Require(!VerifyTerrainMinimapAssetsCurrent(captured, source, fixture.edits, game).Succeeded(),
        "同路径游戏包的已读取member字节变化必须使旧资源快照失效");
    RequireRejected(VerifyTerrainMinimapAssetsFrozenOutput(captured, source, {}), "TERRAIN_ASSETS_FINAL_DEPENDENCY");
}

void TestMissingDoesNotHideUnreadable() {
    Fixture missing; auto source = missing.Source("missing.w3x", {});
    Rejected(CaptureTerrainMinimapAssets(source, missing.edits, {}));
    Fixture corrupt; auto badResources = Materials(Text("present-but-not-a-BLP"));
    auto badSource = corrupt.Source("corrupt.w3x", std::move(badResources)); const auto validGame = corrupt.Game();
    const auto failed = CaptureTerrainMinimapAssets(badSource, corrupt.edits, validGame);
    Rejected(failed);
    Require(std::any_of(failed.diagnostics.begin(), failed.diagnostics.end(), [](const auto& d) {
        return d.subsystem == "Render.TerrainAtlas" || d.subsystem == "Image.BlpMipChain";
    }), "存在但不可解码的高层BLP必须拒绝，不能冒充missing后读取有效低层game BLP");
    Fixture brokenGame; auto noMap = brokenGame.Source("nomap.w3x", {}); const auto game = brokenGame.Game();
    Take(WriteVerifiedArchive(game / "war3patch.mpq", std::vector<MapArchiveMember>{{"Fixture\\Ground.blp", Text("broken-top-layer")}}));
    Rejected(CaptureTerrainMinimapAssets(noMap, brokenGame.edits, game));
}

void TestUnlistedExactAndReadError() {
    Fixture fixture;
    auto files = Materials(); files.push_back({"war3map.w3e", fixture.w3e}); files.push_back({"war3map.wpm", fixture.wpm});
    const auto path = fixture.directory.Path() / "unlisted.w3x";
    Require(IO::WriteFileBytes(path, UnlistedArchive(files)).Succeeded(), "写独立unlisted MPQ fixture");
    auto source = Take(MapArchiveSession::Open(path));
    Require(source.HasOpaqueMembers() && !source.HasMember("Fixture\\Ground.blp"), "fixture应使材质不在命名快照内");
    const auto captured = Take(CaptureTerrainMinimapAssets(source, fixture.edits, {}));
    Require(captured.Dependencies()[captured.GroundAssets()[0].dependencyIndex].mapMemberPresent,
        "命名清单缺项不能当missing；精确成员捕获应找到实际地图材质");
    Require(VerifyTerrainMinimapAssetsFrozenOutput(captured, source, {}).Succeeded(), "最终依赖同样必须支持listfile外实际成员");
    const auto badPath = fixture.directory.Path() / "unreadable.w3x";
    Require(IO::WriteFileBytes(badPath, UnlistedArchive(files, true)).Succeeded(), "写独立不可读高层块fixture");
    auto unreadable = Take(MapArchiveSession::Open(badPath)); const auto game = fixture.Game();
    const auto failed = CaptureTerrainMinimapAssets(unreadable, fixture.edits, game);
    Rejected(failed);
    Require(std::any_of(failed.diagnostics.begin(), failed.diagnostics.end(), [](const auto& d) {
        return d.code == "EXACT_MEMBER_ENCODING_UNSUPPORTED";
    }), "实际地图成员不可读取必须保留诊断，禁止降成missing后回退game");
}

void TestBaselineAndCurrentRevision() {
    Fixture fixture; auto source = fixture.Source();
    auto original = Take(CaptureTerrainMinimapAssets(source, fixture.edits, {}));
    fixture.TextureVariation();
    Require(!fixture.edits.VerifyTerrainPathingForSave().Succeeded(), "variation草稿仍应受共同save gate拒绝");
    auto current = Take(CaptureTerrainMinimapAssets(source, fixture.edits, {}));
    Require(current.Binding().originalTerrainSha256 == original.Binding().originalTerrainSha256
        && current.Binding().currentTerrainSha256 != original.Binding().currentTerrainSha256
        && current.Input().currentW3e != fixture.w3e && current.Binding().revision == fixture.edits.Revision(),
        "工厂须捕获当前真实未保存W3E，Prepare成功不能混成SaveGate已放行");
    Require(!VerifyTerrainMinimapAssetsCurrent(original, source, fixture.edits, {}).Succeeded(), "旧修订不能继续签收");
    Require(fixture.edits.Undo(), "撤销真实variation修改");
    Require(!VerifyTerrainMinimapAssetsCurrent(original, source, fixture.edits, {}).Succeeded(), "undo回相同字节也不能复活旧修订");
    MapDataEditSession another{MapDocument::FromMembers({})};
    Require(another.AttachTerrain(fixture.w3e).Succeeded() && another.AttachPathing(fixture.wpm).Succeeded(), "创建另一真实会话");
    Require(!VerifyTerrainMinimapAssetsCurrent(original, source, another, {}).Succeeded(), "同字节跨会话应拒绝");
    const auto freshBeforeRebase = Take(CaptureTerrainMinimapAssets(source, fixture.edits, {}));
    const auto beforeRebase = fixture.edits.Revision(); fixture.edits.CommitSaved(MapDocument::FromMembers({}));
    Require(fixture.edits.Revision() == beforeRebase
        && fixture.edits.TerrainSourceRevision() != freshBeforeRebase.Binding().terrainSourceRevision
        && !VerifyTerrainMinimapAssetsCurrent(freshBeforeRebase, source, fixture.edits, {}).Succeeded(),
        "重新附基线的terrainSourceRevision应独立于普通revision并使旧快照失效");
    Fixture mismatch;
    mismatch.w3e[45] = std::byte{1}; // classic头45字节后的首顶点raw低字节，从8192变8193。
    auto foreign = mismatch.Source();
    Rejected(CaptureTerrainMinimapAssets(foreign, fixture.edits, {}));
}

void TestRealHeightEditsGenerateWithoutChangingSaveAuthority() {
    Fixture fixture; auto source = fixture.Source();
    const auto original = Take(CaptureTerrainMinimapAssets(source, fixture.edits, {}));
    const auto flat = Take(BuildTerrainMinimapCandidate(original.Input()));
    for (std::size_t at = 0; at < flat.image.rgba.size(); at += 4) {
        Require(flat.image.rgba[at] == std::byte{80} && flat.image.rgba[at + 1] == std::byte{120}
            && flat.image.rgba[at + 2] == std::byte{160} && flat.image.rgba[at + 3] == std::byte{255},
            "真实手写BLP顶级固定颜色应覆盖整个方形顶视图");
    }
    for (const unsigned delta : {156u, 157u}) {
        TerrainBrush brush; brush.kind = TerrainBrushKind::Raise; brush.radius = 0; brush.strength = delta / 4.0f;
        const TerrainStrokePoint point{512, 512};
        Require(fixture.edits.EditTerrain(brush, std::span(&point, 1), false).Succeeded(), "真实会话产生中心普通高程编辑");
        Require(fixture.edits.Terrain()->vertices[40].groundHeightRaw == 8192 + delta, "真实笔刷应给出手算raw净差");
        const auto plan = Take(TerrainSavePlan::Prepare(source, fixture.edits));
        Require(plan.SaveGate().Succeeded() == (delta == 156), "生成域扩大不能放宽正式raw156保存门");
        const auto captured = Take(CaptureTerrainMinimapAssets(source, fixture.edits, {}));
        const auto candidate = Take(BuildTerrainMinimapCandidate(captured.Input()));
        Require(captured.Binding().archiveSha256 == source.SourceSha256()
            && captured.Binding().sessionIdentity == fixture.edits.SessionIdentity()
            && captured.Binding().revision == fixture.edits.Revision()
            && captured.Binding().currentTerrainSha256 != original.Binding().currentTerrainSha256
            && captured.Binding().currentTerrainSha256 == candidate.manifest.currentW3eSha256,
            "实际source/edit/assets与当前W3E身份应连续绑定");
        Require(candidate.image.rgba == flat.image.rgba && candidate.encoded.bytes == flat.encoded.bytes
            && candidate.manifest.outputRgbaSha256 == flat.manifest.outputRgbaSha256
            && candidate.manifest.outputBlpSha256 == flat.manifest.outputBlpSha256
            && candidate.manifest.meshSha256 != flat.manifest.meshSha256
            && candidate.manifest.manifestSha256 != flat.manifest.manifestSha256,
            "真实非平坦编辑应保持固定RGBA/BLP但更新mesh与manifest身份");
        Require(!candidate.manifest.formalSaveAuthorized && !candidate.manifest.nativeEditorOrGameVerified
            && !TerrainMinimapAssetSnapshot::formalSaveAuthorized && !TerrainMinimapAssetSnapshot::nativeEditorOrGameVerified,
            "真实来源与生成成功仍不授予新派生成员保存或原版资格");
        Require(!VerifyTerrainMinimapAssetsCurrent(original, source, fixture.edits, {}).Succeeded(), "相同像素不能复活旧来源修订");
        if (delta == 156) {
            Require(plan.Members()[1].sourceSha256 == plan.Members()[1].outputSha256, "纯高度编辑应原字保留WPM");
            const auto saved = Take(source.SaveCandidate(fixture.directory.Path() / "height156.w3x", plan.Replacements()));
            const auto readback = Take(MapArchiveSession::Open(saved.path));
            Require(plan.VerifyReadback(readback).Succeeded(), "现有正式门内W3E输出仍按五成员精确回读");
            const auto savedTerrain = Take(readback.ReadMember("war3map.w3e"));
            const auto savedPathing = Take(readback.ReadMember("war3map.wpm"));
            Require(savedTerrain == captured.Input().currentW3e && savedPathing == fixture.wpm,
                "回读实际W3E对应本次候选输入，原始WPM保持逐字一致");
            std::vector<MemberReplacement> unapproved(plan.Replacements().begin(), plan.Replacements().end());
            unapproved.push_back({"war3mapMap.blp", candidate.encoded.bytes});
            Require(!plan.VerifyFrozenOutput(source.SourceSha256(), unapproved).Succeeded(),
                "小地图生成成功不能把未批准MapBLP夹入正式冻结成员计划");
        }
        Require(fixture.edits.Undo(), "撤销真实高度编辑");
        Require(fixture.edits.Terrain()->vertices[40].groundHeightRaw == 8192
            && !VerifyTerrainMinimapAssetsCurrent(captured, source, fixture.edits, {}).Succeeded(),
            "undo回到原图字节也不能复活旧高程快照修订");
    }
}

void TestDuplicatePathsAndBudgets() {
    Fixture fixture(true); auto source = fixture.Source();
    auto captured = Take(CaptureTerrainMinimapAssets(source, fixture.edits, {}));
    Require(captured.Facts().exactMemberCaptureBatches == 3 && captured.Facts().uniqueAtlasCount == 1
        && captured.GroundAssets().size() == 2 && captured.Input().groundAtlases[0] == captured.Input().groundAtlases[1],
        "palette两个rawcode指向同一路径应共用一次批量读取和真实atlas保活");
    Require(captured.Facts().cumulativeSourceBytes == captured.Facts().baselineSourceBytes
        + captured.Facts().mapDependencyBytes + captured.Facts().gameDependencyBytes
        && captured.Facts().retainedAtlasRgbaCapacityBytes >= (256 + 64 + 16 + 4 + 1) * 4, "读取账和完整原mip容量账应分开清楚");
    TerrainMinimapAssetCaptureLimits limits;
    limits.maxSourceBytes = captured.Facts().cumulativeSourceBytes - 1; Rejected(CaptureTerrainMinimapAssets(source, fixture.edits, {}, limits));
    limits = {}; limits.maxCurrentW3eBytes = fixture.w3e.size() - 1; Rejected(CaptureTerrainMinimapAssets(source, fixture.edits, {}, limits));
    limits = {}; limits.maxTiles = 1; Rejected(CaptureTerrainMinimapAssets(source, fixture.edits, {}, limits));
    limits = {}; limits.tables.maxTableBytes = 1; Rejected(CaptureTerrainMinimapAssets(source, fixture.edits, {}, limits));
    limits = {}; limits.maxRetainedAtlasCapacityBytes = 1023; Rejected(CaptureTerrainMinimapAssets(source, fixture.edits, {}, limits));
    limits = {}; limits.atlas.maxInputBytes = 100; Rejected(CaptureTerrainMinimapAssets(source, fixture.edits, {}, limits));
    limits = {}; limits.atlas.maxRgbaBytes = 1023; Rejected(CaptureTerrainMinimapAssets(source, fixture.edits, {}, limits));
    limits = {}; limits.atlas.maxTotalMipPixels = 1; Rejected(CaptureTerrainMinimapAssets(source, fixture.edits, {}, limits));
    limits = {}; ++limits.maxSourceBytes; Rejected(CaptureTerrainMinimapAssets(source, fixture.edits, {}, limits));
    limits = {}; ++limits.atlas.maxWorkingBytes; Rejected(CaptureTerrainMinimapAssets(source, fixture.edits, {}, limits));
}

void TestSourceMarginsCaptureAndCurrentGuards() {
    Fixture fixture(false, W3i()); auto source = fixture.Source();
    const auto full = Take(CaptureTerrainMinimapAssets(source, fixture.edits, {}));
    const auto captured = Take(CaptureTerrainMinimapAssets(source, fixture.edits, {}, {}, TerrainMinimapRangePolicy::DeclaredW3iMargins));
    Require(captured.Facts().exactMemberCaptureBatches == 3 && captured.Tables().size() == 3
        && captured.Dependencies().size() == 5 && !captured.Facts().gameMounted
        && captured.Input().sourceW3i == fixture.w3i && !full.Input().sourceW3i,
        "W3I必须复用三表批次、保持三张表且只在crop拥有源原字");
    const auto settings = std::find_if(captured.Dependencies().begin(), captured.Dependencies().end(),
        [](const auto& dependency) { return dependency.kind == TerrainMinimapDependencyKind::Settings; });
    Require(settings != captured.Dependencies().end() && settings->mapMemberPresent
        && settings->winner.layer == Resources::ResourceLayer::MapImport && settings->encodedSha256 == Take(Sha256(W3i()))
        && settings->containerIdentity == source.SourceSha256() && settings->decodedMips.empty(), "Settings不能错记Table或来自game");
    Require(captured.Facts().retainedW3iCapacityBytes >= W3i().size()
        && captured.Facts().estimatedSettingsWorkingBytes > captured.Facts().cumulativeSourceBytes
        && captured.IdentitySha256() != full.IdentitySha256(), "政策、真实W3I与临时容量都必须有独立身份/账");
    const auto candidate = Take(BuildTerrainMinimapCandidate(captured.Input()));
    Require(candidate.manifest.renderExtent == std::array<double, 4>{128, 256, 640, 896}
        && candidate.manifest.w3i.sha256 == settings->encodedSha256, "factory与pure生成器须绑定同一实际W3I");
    Require(VerifyTerrainMinimapAssetsCurrent(captured, source, fixture.edits, {}).Succeeded(), "复核必须重用显式crop政策而非默认full");
    auto changed = W3i(); Put32(changed, 4, 1); // 同长度、只改saves，仍须拒current不等于源。
    Require(fixture.edits.EditSerialized(MapDataKind::Settings, changed).Succeeded(), "建立真实settings修改");
    Rejected(CaptureTerrainMinimapAssets(source, fixture.edits, {}, {}, TerrainMinimapRangePolicy::DeclaredW3iMargins));
    Require(!VerifyTerrainMinimapAssetsCurrent(captured, source, fixture.edits, {}).Succeeded(), "W3I编辑须使旧crop快照stale");
    Require(fixture.edits.Undo(), "撤销settings修改");
    Require(!VerifyTerrainMinimapAssetsCurrent(captured, source, fixture.edits, {}).Succeeded(), "undo相同原字不复活旧revision");
    Require(CaptureTerrainMinimapAssets(source, fixture.edits, {}, {}, TerrainMinimapRangePolicy::DeclaredW3iMargins).Succeeded(),
        "undo后同源current可取得新修订快照");
    Require(!fixture.edits.Remove(MapDataKind::Settings).Succeeded(), "必需Settings成员删除应先被既有会话API拒绝");
    Require(CaptureTerrainMinimapAssets(source, fixture.edits, {}, {}, TerrainMinimapRangePolicy::DeclaredW3iMargins).Succeeded(),
        "删除命令被拒后不能污染当前crop资格");
    fixture.edits.CommitSaved(MapDocument::FromMembers({})); // rebase报告Missing却仍有original原字，须拒绝不一致来源。
    Rejected(CaptureTerrainMinimapAssets(source, fixture.edits, {}, {}, TerrainMinimapRangePolicy::DeclaredW3iMargins));
    Require(CaptureTerrainMinimapAssets(source, fixture.edits, {}).Succeeded(), "未消费W3I的默认full不应受Settings基线缺失影响");
    Fixture foreign(false, changed); foreign.w3i = W3i(); auto foreignSource = foreign.Source();
    Require(foreign.edits.EditSerialized(MapDataKind::Settings, W3i()).Succeeded(), "foreign原B后来改为source A");
    const auto rejected = CaptureTerrainMinimapAssets(foreignSource, foreign.edits, {}, {}, TerrainMinimapRangePolicy::DeclaredW3iMargins);
    Rejected(rejected);
    Require(std::any_of(rejected.diagnostics.begin(), rejected.diagnostics.end(), [](const auto& d) {
        return d.code == "TERRAIN_EDIT_INSPECTION_SOURCE_MISMATCH";
    }), "source A/original B/current A仍须由共用原baseline检查拒绝");
    Fixture budget(false, W3i()); auto budgetSource = budget.Source();
    const auto measured = Take(CaptureTerrainMinimapAssets(budgetSource, budget.edits, {}, {}, TerrainMinimapRangePolicy::DeclaredW3iMargins));
    TerrainMinimapAssetCaptureLimits limits;
    limits.maxSourceBytes = measured.Facts().estimatedSettingsWorkingBytes - 1;
    const auto earlyBudget = CaptureTerrainMinimapAssets(budgetSource, budget.edits, {}, limits, TerrainMinimapRangePolicy::DeclaredW3iMargins);
    Rejected(earlyBudget);
    Require(std::any_of(earlyBudget.diagnostics.begin(), earlyBudget.diagnostics.end(), [](const auto& d) { return d.code == "TERRAIN_ASSETS_BUDGET"; }),
        "同源Ready W3I必须在Snapshot分配前因解析预收失败，而非靠current/stale分支冒充预算门");
    auto larger = W3i(); larger.insert(larger.begin() + 12, 1024, std::byte{'N'});
    Fixture perMember(false, larger); auto largerSource = perMember.Source();
    limits = {}; limits.tables.maxTableBytes = 512;
    Require(CaptureTerrainMinimapAssets(largerSource, perMember.edits, {}, limits).Succeeded(), "三表本身在原512B cap内");
    Rejected(CaptureTerrainMinimapAssets(largerSource, perMember.edits, {}, limits, TerrainMinimapRangePolicy::DeclaredW3iMargins));
}

void TestW3iMissingInvalidAndDefaultNotRequested() {
    Fixture missing; auto source = missing.Source();
    auto gameResources = Materials(); gameResources.push_back({"war3map.w3i", W3i()}); const auto game = missing.Game(gameResources);
    const auto noW3i = CaptureTerrainMinimapAssets(source, missing.edits, game, {}, TerrainMinimapRangePolicy::DeclaredW3iMargins);
    Rejected(noW3i);
    Require(std::any_of(noW3i.diagnostics.begin(), noW3i.diagnostics.end(), [](const auto& d) { return d.code == "TERRAIN_ASSETS_W3I_MISSING"; }),
        "map-only missing不能读game内同名W3I");
    for (const auto& bytes : {Bytes{}, Text("broken-W3I"), W3i()}) {
        auto malformed = bytes;
        if (malformed == W3i()) malformed[0] = std::byte{99};
        Fixture fixture(false, malformed); auto badSource = fixture.Source();
        const auto full = Take(CaptureTerrainMinimapAssets(badSource, fixture.edits, {}));
        Require(!full.Input().sourceW3i && full.Facts().exactMemberCaptureBatches == 3
            && Take(BuildTerrainMinimapCandidate(full.Input())).manifest.w3i.state == "NotRequested",
            "缺失/空/损坏/未知W3I不应给默认full增加解析门");
        Rejected(CaptureTerrainMinimapAssets(badSource, fixture.edits, {}, {}, TerrainMinimapRangePolicy::DeclaredW3iMargins));
    }
    auto partial = W3i(); partial.push_back(std::byte{0});
    Fixture incomplete(false, partial); auto partialSource = incomplete.Source();
    Rejected(CaptureTerrainMinimapAssets(partialSource, incomplete.edits, {}, {}, TerrainMinimapRangePolicy::DeclaredW3iMargins));
    Fixture unlisted(false, W3i()); auto files = Materials();
    files.push_back({"war3map.w3e", unlisted.w3e}); files.push_back({"war3map.wpm", unlisted.wpm}); files.push_back({"war3map.w3i", W3i()});
    const auto path = unlisted.directory.Path() / "unlisted-w3i.w3x";
    Require(IO::WriteFileBytes(path, UnlistedArchive(files)).Succeeded(), "写清单外W3I fixture");
    auto exact = Take(MapArchiveSession::Open(path));
    Require(CaptureTerrainMinimapAssets(exact, unlisted.edits, {}, {}, TerrainMinimapRangePolicy::DeclaredW3iMargins).Succeeded(),
        "exact W3I不依赖listfile命名快照");
    const auto unreadablePath = unlisted.directory.Path() / "unreadable-w3i.w3x";
    Require(IO::WriteFileBytes(unreadablePath, UnlistedArchive(files, true, "war3map.w3i")).Succeeded(), "写不可读W3I fixture");
    auto unreadable = Take(MapArchiveSession::Open(unreadablePath));
    Require(CaptureTerrainMinimapAssets(unreadable, unlisted.edits, {}).Succeeded(), "full不请求清单外不可读W3I");
    Rejected(CaptureTerrainMinimapAssets(unreadable, unlisted.edits, {}, {}, TerrainMinimapRangePolicy::DeclaredW3iMargins));
    auto aliases = files; aliases.push_back({"WAR3MAP.W3I", W3i()});
    const auto aliasPath = unlisted.directory.Path() / "alias-w3i.w3x";
    Require(IO::WriteFileBytes(aliasPath, UnlistedArchive(aliases)).Succeeded(), "写同canonical名双hash fixture");
    auto aliasSource = MapArchiveSession::Open(aliasPath);
    if (aliasSource.Succeeded())
        Rejected(CaptureTerrainMinimapAssets(*aliasSource.value, unlisted.edits, {}, {}, TerrainMinimapRangePolicy::DeclaredW3iMargins));
    else Require(!aliasSource.diagnostics.empty(), "歧义源若在open拒绝也须保留诊断");
}

void TestFinalMapDependenciesAndPolicies() {
    Fixture fixture(false, W3i()); auto source = fixture.Source();
    const auto crop = Take(CaptureTerrainMinimapAssets(source, fixture.edits, {}, {}, TerrainMinimapRangePolicy::DeclaredW3iMargins));
    Require(crop.Binding().originalTerrainBytes == fixture.w3e.size() && crop.Binding().mainTileset == 'L',
        "源W3E大小和真实当前tileset须由工厂冻结，不能由最终调用者指定");
    Require(VerifyTerrainMinimapAssetsFrozenOutput(crop, source, {}).Succeeded(), "不覆盖的crop依赖可通过实际来源复核");
    auto members = Materials(); members.push_back({"war3map.w3i", *fixture.w3i}); members.push_back({"war3map.w3e", fixture.w3e});
    Require(VerifyTerrainMinimapAssetsFrozenOutput(crop, source, members).Succeeded(), "每个地图依赖的唯一同字替换可通过");
    for (const auto& member : members) {
        auto changed = member; changed.data.back() ^= std::byte{1};
        const std::vector<MemberReplacement> replacements{changed};
        RequireRejected(VerifyTerrainMinimapAssetsFrozenOutput(crop, source, replacements), "TERRAIN_ASSETS_FINAL_BYTES");
    }
    auto renamedSettings = *fixture.w3i;
    renamedSettings.insert(renamedSettings.begin() + 12, std::byte{'X'}); // 只给原空mapName加字符，其余字段整体后移。
    const std::vector<MemberReplacement> settings{{"war3map.w3i", renamedSettings}};
    RequireRejected(VerifyTerrainMinimapAssetsFrozenOutput(crop, source, settings), "TERRAIN_ASSETS_FINAL_BYTES");
    const auto full = Take(CaptureTerrainMinimapAssets(source, fixture.edits, {}));
    Require(!full.Input().sourceW3i && VerifyTerrainMinimapAssetsFrozenOutput(full, source, settings).Succeeded(),
        "FullW3e未请求W3I，不能在最终门暗加W3I依赖或声称native crop");
    const std::vector<std::string> removedSettings{"war3map.w3i"};
    Require(VerifyTerrainMinimapAssetsFrozenOutput(full, source, {}, removedSettings).Succeeded(),
        "FullW3e的无关W3I删除仍交给其余正式保存门处理");
    Require(!crop.formalSaveAuthorized && !crop.nativeEditorOrGameVerified && !crop.pendingImportReplaceRemoveCovered
        && !crop.Facts().gameInternalInventoryIndependentlyVerified, "单次最终门通过不能改写快照来源阶段或未知game库存事实");
}

void TestFinalCanonicalConflictsAndEditedW3e() {
    Fixture fixture(false, W3i()); auto source = fixture.Source();
    const auto original = Take(CaptureTerrainMinimapAssets(source, fixture.edits, {}, {}, TerrainMinimapRangePolicy::DeclaredW3iMargins));
    auto members = Materials(); members.push_back({"war3map.w3i", *fixture.w3i}); members.push_back({"war3map.w3e", fixture.w3e});
    for (const auto& member : members) {
        auto alias = member;
        for (auto& c : alias.name) {
            if (c == '\\') c = '/';
            else if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        }
        const std::vector<MemberReplacement> duplicate{member, alias}, single{member};
        const std::vector<std::string> removed{alias.name}, duplicateRemoved{member.name, alias.name};
        RequireRejected(VerifyTerrainMinimapAssetsFrozenOutput(original, source, duplicate), "TERRAIN_ASSETS_FINAL_DUPLICATE");
        RequireRejected(VerifyTerrainMinimapAssetsFrozenOutput(original, source, {}, removed), "TERRAIN_ASSETS_FINAL_REMOVAL");
        RequireRejected(VerifyTerrainMinimapAssetsFrozenOutput(original, source, single, removed), "TERRAIN_ASSETS_FINAL_CONFLICT");
        RequireRejected(VerifyTerrainMinimapAssetsFrozenOutput(original, source, {}, duplicateRemoved), "TERRAIN_ASSETS_FINAL_DUPLICATE");
    }
    TerrainBrush brush; brush.kind = TerrainBrushKind::Raise; brush.radius = 0; brush.strength = 39;
    const TerrainStrokePoint point{512, 512};
    Require(fixture.edits.EditTerrain(brush, std::span(&point, 1), false).Succeeded(), "产生原正式门内raw156的真实修改");
    const auto edited = Take(CaptureTerrainMinimapAssets(source, fixture.edits, {}, {}, TerrainMinimapRangePolicy::DeclaredW3iMargins));
    Require(!VerifyTerrainMinimapAssetsCurrent(original, source, fixture.edits, {}).Succeeded(), "准备时仍须拒旧修订");
    RequireRejected(VerifyTerrainMinimapAssetsFrozenOutput(edited, source, {}), "TERRAIN_ASSETS_FINAL_W3E_MISSING");
    const std::vector<MemberReplacement> oldOutput{{"war3map.w3e", fixture.w3e}}, currentOutput{{"war3map.w3e", edited.Input().currentW3e}};
    RequireRejected(VerifyTerrainMinimapAssetsFrozenOutput(edited, source, oldOutput), "TERRAIN_ASSETS_FINAL_BYTES");
    RequireRejected(VerifyTerrainMinimapAssetsFrozenOutput(original, source, currentOutput), "TERRAIN_ASSETS_FINAL_BYTES");
    Require(VerifyTerrainMinimapAssetsFrozenOutput(edited, source, currentOutput).Succeeded(),
        "最终必须逐字对应已编辑W3E，同时复核不同的原始W3E而非把两者混为一谈");
    const auto plan = Take(TerrainSavePlan::Prepare(source, fixture.edits));
    Require(plan.SaveGate().Succeeded(), "最终依赖功能保留原raw156地形门");
    auto unapproved = currentOutput; unapproved.push_back({"war3mapMap.blp", Blp()});
    Require(!plan.VerifyForSave(fixture.edits, source.SourceSha256(), unapproved).Succeeded(), "最终依赖通过仍不允许夹入新的MapBLP");
}

void TestFinalBudgetAndUnrelatedBodies() {
    Fixture fixture; auto source = fixture.Source();
    const auto snapshot = Take(CaptureTerrainMinimapAssets(source, fixture.edits, {}));
    // 独立原始fixture字节账：现W3E散列 + 原W3E重读 + 三表/唯一atlas重读。
    std::size_t required = 2 * fixture.w3e.size();
    for (const auto& member : Materials()) required += member.data.size();
    TerrainMinimapAssetCaptureLimits limits; limits.maxSourceBytes = required;
    const std::vector<MemberReplacement> unrelated{{"Other\\Large.blp", Bytes(2 * 1024 * 1024, std::byte{255})},
        {"模型\\纹理.blp", Text("unrelated-non-ASCII-name")}};
    Require(VerifyTerrainMinimapAssetsFrozenOutput(snapshot, source, unrelated, {}, limits).Succeeded(),
        "无关正文即使超过本次总预算也不能被散列/解码；无关非ASCII名字不被资源依赖门误拒");
    --limits.maxSourceBytes;
    RequireRejected(VerifyTerrainMinimapAssetsFrozenOutput(snapshot, source, {}, {}, limits), "TERRAIN_ASSETS_BUDGET");
    const std::vector<MemberReplacement> sameW3e{{"war3map.w3e", fixture.w3e}};
    limits.maxSourceBytes = required + fixture.w3e.size() - 1;
    RequireRejected(VerifyTerrainMinimapAssetsFrozenOutput(snapshot, source, sameW3e, {}, limits), "TERRAIN_ASSETS_BUDGET");
    ++limits.maxSourceBytes;
    Require(VerifyTerrainMinimapAssetsFrozenOutput(snapshot, source, sameW3e, {}, limits).Succeeded(),
        "相关replacement散列须额外计一次，精确边界才允许");
    limits.maxCurrentW3eBytes = fixture.w3e.size() - 1;
    RequireRejected(VerifyTerrainMinimapAssetsFrozenOutput(snapshot, source, {}, {}, limits), "TERRAIN_ASSETS_BUDGET");
    limits.maxCurrentW3eBytes = TerrainMinimapAssetCaptureLimits{}.maxCurrentW3eBytes;
    limits.tables.maxTableBytes = 1;
    RequireRejected(VerifyTerrainMinimapAssetsFrozenOutput(snapshot, source, {}, {}, limits), "TERRAIN_ASSETS_BUDGET");
    limits.tables = {}; limits.maxSourceBytes = required - 1;
    const auto savedPath = fixture.directory.Path() / "budget-source-away.w3x";
    std::filesystem::rename(source.SourcePath(), savedPath);
    RequireRejected(VerifyTerrainMinimapAssetsFrozenOutput(snapshot, source, {}, {}, limits), "TERRAIN_ASSETS_BUDGET");
    // 预算先于来源读/租约；预算足够后才得到真实源缺失，而非以快照状态伪造来源。
    ++limits.maxSourceBytes;
    RequireRejected(VerifyTerrainMinimapAssetsFrozenOutput(snapshot, source, {}, {}, limits));
    std::filesystem::rename(savedPath, source.SourcePath());
    Require(VerifyTerrainMinimapAssetsFrozenOutput(snapshot, source, {}, {}, limits).Succeeded(), "恢复本测试独占源路径后可重新核验");
    Fixture foreign; auto other = foreign.Source("foreign.w3x", Materials(Blp({7, 8, 9})));
    RequireRejected(VerifyTerrainMinimapAssetsFrozenOutput(snapshot, other, {}), "TERRAIN_ASSETS_FINAL_SOURCE");
}

MapArchiveSession Saved(Fixture& fixture, const MapArchiveSession& source, std::string_view name,
    std::span<const MemberReplacement> replacements = {}, std::span<const std::string> removals = {}) {
    const auto result = Take(source.SaveCandidate(fixture.directory.Path() / name, replacements, removals));
    return Take(MapArchiveSession::Open(result.path));
}

void TestReadbackCurrentTerrainAndSourcePreservation() {
    Fixture fixture(false, W3i()); auto source = fixture.Source();
    const auto originalSha = Take(Sha256File(source.SourcePath()));
    TerrainBrush brush; brush.kind = TerrainBrushKind::Raise; brush.radius = 0; brush.strength = 39;
    const TerrainStrokePoint point{512, 512};
    Require(fixture.edits.EditTerrain(brush, std::span(&point, 1), false).Succeeded(), "产生真实raw156编辑");
    const auto snapshot = Take(CaptureTerrainMinimapAssets(source, fixture.edits, {}, {}, TerrainMinimapRangePolicy::DeclaredW3iMargins));
    const auto revision = fixture.edits.Revision(); const auto dirty = fixture.edits.IsDirty();
    const std::vector<MemberReplacement> output{{"war3map.w3e", snapshot.Input().currentW3e}, {"saved-only.txt", Text("new-archive")}};
    auto saved = Saved(fixture, source, "readback-current.w3x", output);
    Require(saved.SourceSha256() != source.SourceSha256() && VerifyTerrainMinimapAssetsReadback(snapshot, saved).Succeeded(),
        "真实输出整档SHA不同，map容器应重绑定而非误判原sourceSHA；当前W3E与全部crop依赖仍核原字");
    auto stale = Saved(fixture, source, "readback-old-w3e.w3x");
    RequireRejected(VerifyTerrainMinimapAssetsReadback(snapshot, stale), "TERRAIN_ASSETS_READBACK_TERRAIN");
    auto members = Materials(); members.push_back({"war3map.w3i", *fixture.w3i});
    unsigned number = 0;
    for (const auto& member : members) {
        auto changed = member; changed.data.back() ^= std::byte{1};
        auto replacements = output; replacements.push_back(std::move(changed));
        auto mismatch = Saved(fixture, source, "readback-changed-" + std::to_string(number++) + ".w3x", replacements);
        RequireRejected(VerifyTerrainMinimapAssetsReadback(snapshot, mismatch), "TERRAIN_ASSETS_FINAL_DEPENDENCY");
    }
    auto renamed = *fixture.w3i; renamed.insert(renamed.begin() + 12, std::byte{'X'});
    auto nameOutput = output; nameOutput.push_back({"war3map.w3i", renamed});
    auto settings = Saved(fixture, source, "readback-settings-name.w3x", nameOutput);
    RequireRejected(VerifyTerrainMinimapAssetsReadback(snapshot, settings), "TERRAIN_ASSETS_FINAL_DEPENDENCY");
    const std::vector<MemberReplacement> emptyAtlas{{"Fixture\\Ground.blp", {}}};
    auto emptyOutput = output; emptyOutput.push_back(emptyAtlas.front());
    auto empty = Saved(fixture, source, "readback-empty-atlas.w3x", emptyOutput);
    RequireRejected(VerifyTerrainMinimapAssetsReadback(snapshot, empty), "TERRAIN_ASSETS_FINAL_DEPENDENCY");
    const std::vector<std::string> removedAtlas{"Fixture\\Ground.blp"};
    auto missing = Saved(fixture, source, "readback-missing-atlas.w3x", output, removedAtlas);
    RequireRejected(VerifyTerrainMinimapAssetsReadback(snapshot, missing), "TERRAIN_ASSETS_FINAL_WINNER");
    Require(Take(Sha256File(source.SourcePath())) == originalSha && source.VerifySourceUnchanged().Succeeded()
        && fixture.edits.Revision() == revision && fixture.edits.IsDirty() == dirty,
        "输出回读不得写原source或活跃编辑会话；此断言只涉及测试拥有的真实归档");
    Require(!snapshot.formalSaveAuthorized && !snapshot.nativeEditorOrGameVerified && !snapshot.pendingImportReplaceRemoveCovered,
        "输出回读成功不提升正式保存/native或源阶段snapshot资格");
}

void TestReadbackGameWinnersAndMapOverrides() {
    Fixture fixture(false, W3i()); auto source = fixture.Source("readback-game-source.w3x", {}); const auto game = fixture.Game();
    const auto snapshot = Take(CaptureTerrainMinimapAssets(source, fixture.edits, game, {}, TerrainMinimapRangePolicy::DeclaredW3iMargins));
    const std::vector<MemberReplacement> unrelated{{"saved-only.txt", Text("different-output")}};
    auto saved = Saved(fixture, source, "readback-game.w3x", unrelated);
    Require(VerifyTerrainMinimapAssetsReadback(snapshot, saved).Succeeded(), "实际game fallback重读winner，输出map仍须准确missing");
    unsigned number = 0;
    for (const auto& material : Materials()) {
        const std::vector<MemberReplacement> sameMap{material};
        auto overridden = Saved(fixture, source, "readback-game-map-" + std::to_string(number++) + ".w3x", sameMap);
        RequireRejected(VerifyTerrainMinimapAssetsReadback(snapshot, overridden), "TERRAIN_ASSETS_FINAL_WINNER");
    }
    Take(WriteVerifiedArchive(game / "war3patch.mpq", std::vector<MapArchiveMember>{{"Fixture\\Ground.blp", Blp()}}));
    RequireRejected(VerifyTerrainMinimapAssetsReadback(snapshot, saved), "TERRAIN_ASSETS_FINAL_DEPENDENCY");
}

void TestReadbackExactPoliciesBudgetsAndOutputIdentity() {
    Fixture fixture(false, W3i()); auto source = fixture.Source();
    const auto full = Take(CaptureTerrainMinimapAssets(source, fixture.edits, {}));
    const auto crop = Take(CaptureTerrainMinimapAssets(source, fixture.edits, {}, {}, TerrainMinimapRangePolicy::DeclaredW3iMargins));
    auto files = Materials(); files.push_back({"war3map.w3e", fixture.w3e}); files.push_back({"war3map.w3i", *fixture.w3i});
    const auto path = fixture.directory.Path() / "readback-unlisted.w3x";
    Require(IO::WriteFileBytes(path, UnlistedArchive(files)).Succeeded(), "写原创清单外output");
    auto unlisted = Take(MapArchiveSession::Open(path));
    Require(VerifyTerrainMinimapAssetsReadback(crop, unlisted).Succeeded(), "输出exact依赖不依赖listfile命名快照");
    const auto badPath = fixture.directory.Path() / "readback-unreadable-settings.w3x";
    Require(IO::WriteFileBytes(badPath, UnlistedArchive(files, true, "war3map.w3i")).Succeeded(), "写不可读W3I输出");
    auto bad = Take(MapArchiveSession::Open(badPath));
    Require(VerifyTerrainMinimapAssetsReadback(full, bad).Succeeded(), "FullW3e不读取未请求的不可读W3I");
    RequireRejected(VerifyTerrainMinimapAssetsReadback(crop, bad), "EXACT_MEMBER_ENCODING_UNSUPPORTED");
    for (const auto& alias : {std::string("WAR3MAP.W3I"), std::string("fixture/ground.blp")}) {
        auto duplicate = files; duplicate.push_back({alias, alias == "WAR3MAP.W3I" ? *fixture.w3i : Blp()});
        const auto duplicatePath = fixture.directory.Path() / (alias == "WAR3MAP.W3I" ? "readback-alias-w3i.w3x" : "readback-alias-atlas.w3x");
        Require(IO::WriteFileBytes(duplicatePath, UnlistedArchive(duplicate)).Succeeded(), "写同canonical双hash输出");
        auto opened = MapArchiveSession::Open(duplicatePath);
        if (opened.Succeeded()) RequireRejected(VerifyTerrainMinimapAssetsReadback(crop, *opened.value));
        else Require(!opened.diagnostics.empty(), "歧义output可在Open先拒，但不得静默通过");
    }
    std::size_t required = 2 * fixture.w3e.size() + fixture.w3i->size();
    for (const auto& material : Materials()) required += material.data.size();
    TerrainMinimapAssetCaptureLimits limits; limits.maxSourceBytes = required;
    Require(VerifyTerrainMinimapAssetsReadback(crop, unlisted, limits).Succeeded(), "独立原字账精确预算边界可通过");
    --limits.maxSourceBytes;
    RequireRejected(VerifyTerrainMinimapAssetsReadback(crop, unlisted, limits), "TERRAIN_ASSETS_BUDGET");
    const auto oldPath = fixture.directory.Path() / "readback-previous-file.w3x";
    const auto sameBytes = Take(IO::ReadFileBytes(path)); std::filesystem::rename(path, oldPath);
    Require(IO::WriteFileBytes(path, sameBytes).Succeeded(), "samebytes换新file-ID输出，保留旧句柄身份反例");
    RequireRejected(VerifyTerrainMinimapAssetsReadback(crop, unlisted, limits), "TERRAIN_ASSETS_BUDGET");
    ++limits.maxSourceBytes;
    RequireRejected(VerifyTerrainMinimapAssetsReadback(crop, unlisted, limits));
    auto reopened = Take(MapArchiveSession::Open(path));
    Require(VerifyTerrainMinimapAssetsReadback(crop, reopened, limits).Succeeded(), "只有重新取得真实新output身份才能签收");
    limits.maxCurrentW3eBytes = fixture.w3e.size() - 1;
    RequireRejected(VerifyTerrainMinimapAssetsReadback(crop, reopened, limits), "TERRAIN_ASSETS_BUDGET");
    limits.maxCurrentW3eBytes = TerrainMinimapAssetCaptureLimits{}.maxCurrentW3eBytes; limits.atlas.maxInputBytes = 1;
    RequireRejected(VerifyTerrainMinimapAssetsReadback(crop, reopened, limits), "TERRAIN_ASSETS_BUDGET");
}

} // namespace

int main() {
    try {
        TestActualMapWinnerAndOriginalMips(); TestGameFallbackAndWinnerChange(); TestSameGamePathChangedMember();
        TestMissingDoesNotHideUnreadable(); TestUnlistedExactAndReadError();
        TestBaselineAndCurrentRevision(); TestDuplicatePathsAndBudgets();
        TestRealHeightEditsGenerateWithoutChangingSaveAuthority();
        TestSourceMarginsCaptureAndCurrentGuards(); TestW3iMissingInvalidAndDefaultNotRequested();
        TestFinalMapDependenciesAndPolicies(); TestFinalCanonicalConflictsAndEditedW3e(); TestFinalBudgetAndUnrelatedBodies();
        TestReadbackCurrentTerrainAndSourcePreservation(); TestReadbackGameWinnersAndMapOverrides();
        TestReadbackExactPoliciesBudgetsAndOutputIdentity();
        std::cout << "TerrainMinimapAssetsTests: all tests passed\n"; return 0;
    } catch (const std::exception& e) {
        std::cerr << "TerrainMinimapAssetsTests: " << e.what() << '\n'; return 1;
    }
}
