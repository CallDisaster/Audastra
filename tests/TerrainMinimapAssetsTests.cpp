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
Bytes UnlistedArchive(std::vector<MapArchiveMember> members, bool badTexture = false) {
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
            | (badTexture && member.name == "Fixture\\Ground.blp" ? Mpq::kFilePatch : 0u));
        dataAt += size;
    }
    crypt.EncryptBlock(bytes.data() + hashAt, hashCount * 16, crypt.HashString("(hash table)", Mpq::MpqCrypt::HashType::FileKey));
    crypt.EncryptBlock(bytes.data() + blockAt, count * 16, crypt.HashString("(block table)", Mpq::MpqCrypt::HashType::FileKey));
    return bytes;
}

struct Fixture {
    TempDirectory directory;
    Bytes w3e, wpm;
    MapDataEditSession edits{MapDocument::FromMembers({})};
    Fixture(bool duplicatePath = false) {
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
    // 新增高层patch中相同字节的纹理；像素/SHA可相同，真实winner已改变，旧快照仍失效。
    Take(WriteVerifiedArchive(game / "war3patch.mpq", std::vector<MapArchiveMember>{{"Fixture\\Ground.blp", Blp()}}));
    Require(!VerifyTerrainMinimapAssetsCurrent(captured, source, fixture.edits, game).Succeeded(), "相同字节换高优先级winner必须使旧快照失效");
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

} // namespace

int main() {
    try {
        TestActualMapWinnerAndOriginalMips(); TestGameFallbackAndWinnerChange(); TestSameGamePathChangedMember();
        TestMissingDoesNotHideUnreadable(); TestUnlistedExactAndReadError();
        TestBaselineAndCurrentRevision(); TestDuplicatePathsAndBudgets();
        std::cout << "TerrainMinimapAssetsTests: all tests passed\n"; return 0;
    } catch (const std::exception& e) {
        std::cerr << "TerrainMinimapAssetsTests: " << e.what() << '\n'; return 1;
    }
}
