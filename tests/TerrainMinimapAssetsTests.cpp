#include "War3MapCore/Render/TerrainMinimapAssets.h"

#include "War3MapCore/Hash.h"
#include "War3MapCore/IO/FileBytes.h"
#include "War3MapCore/Mpq/MpqArchive.h"
#include "War3MapCore/MapData/MapSettings.h"
#include "War3MapCore/MapData/TerrainEditing.h"
#include "War3MapCore/MapData/TerrainSavePlan.h"
#include "War3MapCore/MapDocument.h"
#include "War3MapCore/Render/ClassicMinimapObjectAssets.h"
#include "../src/Core/Mpq/MpqCrypt.h"
#include "fixtures/TinyJpegFixture.h"

#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <bit>
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

Bytes CreateW3i() {
    auto bytes = W3i();
    const std::array<float, 8> bounds{-2816, -3328, 2816, 2816, -2816, 2816, 2816, -3328};
    for (std::size_t i = 0; i < bounds.size(); ++i) Put32(bytes, 16 + i * 4, std::bit_cast<std::uint32_t>(bounds[i]));
    Put32(bytes, 48, 6); Put32(bytes, 52, 6); Put32(bytes, 56, 4); Put32(bytes, 60, 8);
    Put32(bytes, 64, 52); Put32(bytes, 68, 52); return bytes;
}

Bytes CreateW3e() {
    // 独立标准Create原字；包含六ground/two cliff和原SW-anchor MapEdge，绝不先清位。
    Bytes bytes(69 + 65 * 65 * 7);
    const auto magic = Text("W3E!"), ground = Text("LdrtLdroLdrgLrokLgrsLgrd"), cliff = Text("CLdiCLgr");
    std::copy(magic.begin(), magic.end(), bytes.begin()); Put32(bytes, 4, 11); bytes[8] = std::byte{'L'};
    Put32(bytes, 13, 6); std::copy(ground.begin(), ground.end(), bytes.begin() + 17);
    Put32(bytes, 41, 2); std::copy(cliff.begin(), cliff.end(), bytes.begin() + 45);
    Put32(bytes, 53, 65); Put32(bytes, 57, 65);
    Put32(bytes, 61, std::bit_cast<std::uint32_t>(-4096.0f)); Put32(bytes, 65, std::bit_cast<std::uint32_t>(-4096.0f));
    for (std::size_t y = 0; y < 65; ++y) for (std::size_t x = 0; x < 65; ++x) {
        const auto at = 69 + (y * 65 + x) * 7;
        const bool edge = x < 64 && y < 64 && (x < 6 || x >= 58 || y < 4 || y >= 56);
        bytes[at + 1] = std::byte{32}; bytes[at + 3] = edge ? std::byte{96} : std::byte{32}; bytes[at + 6] = std::byte{0xf2};
    }
    return bytes;
}
MapDocument SettingsDocument(const std::optional<Bytes>& bytes, const std::optional<Bytes>& doodads = {}) {
    std::vector<MapArchiveMember> members;
    if (bytes) members.push_back({"war3map.w3i", *bytes});
    if (doodads) members.push_back({"war3map.doo", *doodads});
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

std::vector<MapArchiveMember> CreateMaterials() {
    auto materials = Materials();
    materials.front().data = Slk({"tileID", "dir", "file"}, {{"Ldrt", "Fixture", "Ground"},
        {"Ldro", "Fixture", "Ground"}, {"Ldrg", "Fixture", "Ground"}, {"Lrok", "Fixture", "Ground"},
        {"Lgrs", "Fixture", "Ground"}, {"Lgrd", "Fixture", "Ground"}});
    return materials;
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
    std::optional<Bytes> w3i, doodads;
    MapDataEditSession edits;
    Fixture(bool duplicatePath = false, std::optional<Bytes> settings = {}, bool standardCreate = false,
        std::uint32_t dimension = 9, std::optional<Bytes> placements = {})
        : w3i(std::move(settings)), doodads(std::move(placements)), edits(SettingsDocument(w3i, doodads)) {
        TerrainGridSnapshot terrain;
        terrain.header.width = terrain.header.height = dimension; terrain.header.mainTileset = 'L';
        terrain.header.groundTilesets = duplicatePath ? std::vector<std::string>{"Tg01", "Tg02"} : std::vector<std::string>{"Tg01"};
        terrain.header.cliffTilesets = {"CLdi"}; terrain.vertices.resize(static_cast<std::size_t>(dimension) * dimension);
        for (auto& v : terrain.vertices) { v.groundHeightRaw = 8192; v.waterHeightAndFlagsRaw = 8192; v.cliffAndLayerRaw = 2; }
        RefreshTerrainDerived(terrain, 0, 0, dimension, dimension); w3e = standardCreate ? CreateW3e() : Take(SerializeMapTerrain(terrain));
        const auto pathingSize = standardCreate ? 256u : 4u * (dimension - 1);
        wpm = Text("MP3W"); wpm.resize(16 + pathingSize * pathingSize); Put32(wpm, 8, pathingSize); Put32(wpm, 12, pathingSize);
        if (standardCreate) for (unsigned y = 0; y < 256; ++y) for (unsigned x = 0; x < 256; ++x)
            wpm[16 + y * 256 + x] = x < 24 || x >= 232 || y < 16 || y >= 224 ? std::byte{0xce} : std::byte{0x40};
        Require(edits.AttachTerrain(w3e).Succeeded() && edits.AttachPathing(wpm).Succeeded(), "附上真实源W3E/WPM");
    }
    MapArchiveSession Source(std::string_view name = "source.w3x", std::vector<MapArchiveMember> resources = Materials()) {
        resources.push_back({"war3map.w3e", w3e}); resources.push_back({"war3map.wpm", wpm});
        if (w3i) resources.push_back({"war3map.w3i", *w3i});
        if (doodads) resources.push_back({"war3map.doo", *doodads});
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

void TestFinalTerrainResourceOverlayWinsAndIsBound() {
    Fixture fixture;
    auto source = fixture.Source("overlay-source.w3x");
    const auto replacementBytes = Blp({210, 30, 10});
    const auto replacementTable = Slk({"tileID", "dir", "file"}, {{"Tg01", "Fixture", "NewGround"}});
    std::vector<MemberReplacement> replacements{{"TerrainArt\\Terrain.slk", replacementTable},
        {"Fixture\\NewGround.blp", replacementBytes}};
    const auto captured = Take(CaptureTerrainMinimapAssets(source, fixture.edits, {}, {},
        TerrainMinimapRangePolicy::FullW3e, replacements, {}));
    const auto ground = captured.GroundAssets().front();
    const auto& dependency = captured.Dependencies().at(ground.dependencyIndex);
    const auto& tableDependency = captured.Dependencies().at(captured.Tables().front().dependencyIndex);
    Require(tableDependency.winner.layer == Resources::ResourceLayer::SessionOverride
        && tableDependency.encodedSha256 == Take(Sha256(replacementTable))
        && ground.canonicalTexturePath == "FIXTURE\\NEWGROUND.BLP"
        && dependency.winner.layer == Resources::ResourceLayer::SessionOverride
        && dependency.winner.sourceId == "terrain-minimap-overlay"
        && dependency.mapMemberPresent && dependency.encodedSha256 == Take(Sha256(replacementBytes)),
        "最终地形贴图overlay必须赢过原地图成员并绑定实际字节");
    Require(VerifyTerrainMinimapAssetsCurrent(captured, source, fixture.edits, {}, {}, replacements, {}).Succeeded(),
        "当前复核必须消费同一最终地形资源overlay");
    Require(VerifyTerrainMinimapAssetsFrozenOutput(captured, source, replacements).Succeeded(),
        "冻结输出必须接受同一overlay且不再读取旧地图正文");
    RequireRejected(VerifyTerrainMinimapAssetsFrozenOutput(captured, source, {}), "TERRAIN_ASSETS_FINAL_WINNER");
    replacements.front().data[0] = std::byte{'X'};
    Require(!VerifyTerrainMinimapAssetsFrozenOutput(captured, source, replacements).Succeeded(),
        "overlay字节改动后冻结输出必须拒绝");
    const std::array<std::string, 1> removedGround{"Fixture\\Ground.blp"};
    const auto noGame = CaptureTerrainMinimapAssets(source, fixture.edits, {}, {},
        TerrainMinimapRangePolicy::FullW3e, {}, removedGround);
    Rejected(noGame);
}

void TestFinalTerrainResourceRemovalUsesActualGameAndReadback() {
    Fixture fixture;
    auto source = fixture.Source("resource-removal-source.w3x");
    const auto game = fixture.Game(Materials(Blp({220, 40, 20})));
    const std::array<std::string, 1> removals{"Fixture\\Ground.blp"};
    const auto captured = Take(CaptureTerrainMinimapAssets(source, fixture.edits, game, {},
        TerrainMinimapRangePolicy::FullW3e, {}, removals));
    const auto& dependency = captured.Dependencies().at(captured.GroundAssets().front().dependencyIndex);
    Require(!dependency.mapMemberPresent && dependency.winner.layer == Resources::ResourceLayer::Game
        && dependency.encodedSha256 == Take(Sha256(Blp({220, 40, 20}))),
        "删除地图贴图后必须消费真实game winner，不能仍用被删map正文");
    Require(VerifyTerrainMinimapAssetsCurrent(captured, source, fixture.edits, game, {}, {}, removals).Succeeded()
        && VerifyTerrainMinimapAssetsFrozenOutput(captured, source, {}, removals).Succeeded(),
        "同一删除与真实game fallback必须通过Current/Frozen生命周期");
    RequireRejected(VerifyTerrainMinimapAssetsFrozenOutput(captured, source, {}), "TERRAIN_ASSETS_FINAL_WINNER");
    const auto target = fixture.directory.Path() / "resource-removal-saved.w3x";
    Take(source.SaveCandidate(target, {}, removals));
    const auto saved = Take(MapArchiveSession::Open(target));
    Require(VerifyTerrainMinimapAssetsReadback(captured, saved).Succeeded(),
        "独立Open必须核输出map准确missing及同一game来源");
    const auto oldWinner = Take(MapArchiveSession::Open(source.SourcePath()));
    RequireRejected(VerifyTerrainMinimapAssetsReadback(captured, oldWinner), "TERRAIN_ASSETS_FINAL_WINNER");
}

void TestFinalTerrainOverlayDoesNotReadBrokenOldResource() {
    Fixture fixture;
    auto members = Materials();
    members.push_back({"war3map.w3e", fixture.w3e}); members.push_back({"war3map.wpm", fixture.wpm});
    const auto path = fixture.directory.Path() / "broken-old-resource.w3x";
    Require(IO::WriteFileBytes(path, UnlistedArchive(members, true)).Succeeded(), "写入准确不可读旧贴图来源");
    const auto source = Take(MapArchiveSession::Open(path));
    const std::array replacement{MemberReplacement{"Fixture\\Ground.blp", Blp({110, 20, 90})}};
    const auto captured = Take(CaptureTerrainMinimapAssets(source, fixture.edits, {}, {},
        TerrainMinimapRangePolicy::FullW3e, replacement));
    Require(VerifyTerrainMinimapAssetsFrozenOutput(captured, source, replacement).Succeeded(),
        "被合法最终overlay覆盖的旧坏资源不应再读取正文");
    auto duplicate = std::vector<MemberReplacement>{replacement.front(), {"fixture/ground.BLP", Blp()}};
    Rejected(CaptureTerrainMinimapAssets(source, fixture.edits, {}, {}, TerrainMinimapRangePolicy::FullW3e, duplicate));
    auto limits = TerrainMinimapAssetCaptureLimits{};
    limits.atlas.maxInputBytes = replacement.front().data.size() - 1;
    Rejected(CaptureTerrainMinimapAssets(source, fixture.edits, {}, limits, TerrainMinimapRangePolicy::FullW3e, replacement));
    const std::array empty{MemberReplacement{"Fixture\\Ground.blp", {}}};
    Rejected(CaptureTerrainMinimapAssets(source, fixture.edits, {}, {}, TerrainMinimapRangePolicy::FullW3e, empty));
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
    Require(fixture.edits.VerifyTerrainPathingForSave().Succeeded(), "地面低位变体不改变通行，应通过共同保存检查");
    auto current = Take(CaptureTerrainMinimapAssets(source, fixture.edits, {}));
    Require(current.Binding().originalTerrainSha256 == original.Binding().originalTerrainSha256
        && current.Binding().currentTerrainSha256 != original.Binding().currentTerrainSha256
        && current.Input().currentW3e != fixture.w3e && current.Binding().revision == fixture.edits.Revision(),
        "工厂须捕获当前真实未保存W3E，Prepare成功不能混成SaveGate已放行");
    const auto plan = Take(TerrainSavePlan::Prepare(source, fixture.edits));
    Require(plan.SaveGate().Succeeded()
        && plan.VerifyForSave(fixture.edits, source.SourceSha256(), fixture.edits.Replacements()).Succeeded(),
        "当前合法变体应通过来源与修订检查");
    const auto saved = Take(source.SaveCandidate(fixture.directory.Path() / "variation.w3x", plan.Replacements()));
    const auto readback = Take(MapArchiveSession::Open(saved.path));
    Require(plan.VerifyReadback(readback).Succeeded()
        && Take(readback.ReadMember("war3map.w3e")) == current.Input().currentW3e
        && Take(readback.ReadMember("war3map.wpm")) == fixture.wpm,
        "变体修改必须实际写入地图并独立回读，WPM逐字保留");
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
        Require(plan.SaveGate().Succeeded(), "原版干地高度不变性允许两个14位范围内的纯干地高度差");
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
        {
            Require(plan.Members()[1].sourceSha256 == plan.Members()[1].outputSha256, "纯高度编辑应原字保留WPM");
            const auto saved = Take(source.SaveCandidate(fixture.directory.Path() / ("height" + std::to_string(delta) + ".w3x"), plan.Replacements()));
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

void TestDeclaredExteriorMapEdgeSourceHeightAndOutputClosure() {
    constexpr auto policy = TerrainMinimapRangePolicy::DeclaredW3iMarginsExteriorMapEdge;
    Fixture fixture(false, CreateW3i(), true); auto source = fixture.Source("create-edge-source.w3x", CreateMaterials());
    const auto originalArchive = Take(Sha256File(source.SourcePath())); const auto sourceBytes = fixture.w3e;
    const auto original = Take(CaptureTerrainMinimapAssets(source, fixture.edits, {}, {}, policy));
    const auto legacy = Take(CaptureTerrainMinimapAssets(source, fixture.edits, {}, {}, TerrainMinimapRangePolicy::DeclaredW3iMargins));
    Rejected(BuildTerrainMinimapCandidate(legacy.Input()));
    Require(original.IdentitySha256() != legacy.IdentitySha256() && original.Input().currentW3e == sourceBytes,
        "同W3E/W3I来源也必须把新显式policy纳入snapshot身份，标准MapEdge原字仍保留");
    const auto originalCandidate = Take(BuildTerrainMinimapCandidate(original.Input()));
    Require(originalCandidate.manifest.mapEdge.exteriorMapEdgeAnchors == 1392
        && originalCandidate.manifest.reserializedW3eSha256 == Take(Sha256(sourceBytes)), "标准真实归档原字须通过独立mask与重序列化核验");
    TerrainBrush brush; brush.kind = TerrainBrushKind::Raise; brush.radius = 0; brush.strength = 39;
    const TerrainStrokePoint center{0, 0};
    Require(fixture.edits.EditTerrain(brush, std::span(&center, 1), false).Succeeded(), "标准Create中心产生非空raw156高度修改");
    const auto revision = fixture.edits.Revision(); const auto dirty = fixture.edits.IsDirty();
    const auto snapshot = Take(CaptureTerrainMinimapAssets(source, fixture.edits, {}, {}, policy));
    const auto current = snapshot.Input().currentW3e;
    Require(current != sourceBytes && snapshot.Binding().originalTerrainSha256 == Take(Sha256(sourceBytes))
        && snapshot.Binding().currentTerrainSha256 == Take(Sha256(current)), "源/current身份不能因同像素而合并");
    for (std::size_t vertex = 0; vertex < 65 * 65; ++vertex) {
        const auto at = 69 + vertex * 7;
        Require(std::equal(current.begin() + at + 2, current.begin() + at + 7, sourceBytes.begin() + at + 2),
            "合法height编辑必须保留每vertex完整water/MapEdge/texture/variation/cliff原字");
    }
    const auto candidate = Take(BuildTerrainMinimapCandidate(snapshot.Input()));
    Require(candidate.image.rgba == originalCandidate.image.rgba
        && candidate.manifest.meshSha256 != originalCandidate.manifest.meshSha256
        && candidate.manifest.mapEdge.maskSha256 == originalCandidate.manifest.mapEdge.maskSha256,
        "非空height生成仍保留真实mesh身份和未修改mask，不能靠清bit通过");
    Require(snapshot.Facts().exactMemberCaptureBatches == 3 && snapshot.Facts().uniqueAtlasCount == 1
        && snapshot.Dependencies().size() == 5 && snapshot.Input().sourceW3i == fixture.w3i,
        "新policy须沿三批准确来源并复用六slot唯一atlas，W3I保持map原字");
    const auto settings = std::find_if(snapshot.Dependencies().begin(), snapshot.Dependencies().end(),
        [](const auto& dependency) { return dependency.kind == TerrainMinimapDependencyKind::Settings; });
    Require(settings != snapshot.Dependencies().end() && settings->mapMemberPresent
        && settings->winner.layer == Resources::ResourceLayer::MapImport
        && settings->containerIdentity == source.SourceSha256() && settings->encodedSha256 == Take(Sha256(*fixture.w3i)),
        "Settings依赖须认证实际map-only来源，而非caller标签或game同名项");
    Require(VerifyTerrainMinimapAssetsCurrent(snapshot, source, fixture.edits, {}).Succeeded(), "新policy current复核须保留原显式选择");
    Require(!VerifyTerrainMinimapAssetsCurrent(original, source, fixture.edits, {}).Succeeded(), "高度新修订不能复活旧snapshot");
    const auto plan = Take(TerrainSavePlan::Prepare(source, fixture.edits));
    Require(plan.SaveGate().Succeeded(), "原有SaveGate应接受标准图中心raw156，不经本策略放宽");
    const std::vector<MemberReplacement> output{{"war3map.w3e", current}};
    RequireRejected(VerifyTerrainMinimapAssetsFrozenOutput(snapshot, source, {}), "TERRAIN_ASSETS_FINAL_W3E_MISSING");
    Require(VerifyTerrainMinimapAssetsFrozenOutput(snapshot, source, output).Succeeded(), "最终输出须核当前W3E及原W3I/三表/atlas完整依赖");
    auto same = output; same.push_back({"war3map.w3i", *fixture.w3i});
    Require(VerifyTerrainMinimapAssetsFrozenOutput(snapshot, source, same).Succeeded(), "唯一同字W3I最终替换允许");
    auto renamed = *fixture.w3i; renamed.insert(renamed.begin() + 12, std::byte{'N'});
    auto changed = output; changed.push_back({"war3map.w3i", renamed});
    RequireRejected(VerifyTerrainMinimapAssetsFrozenOutput(snapshot, source, changed), "TERRAIN_ASSETS_FINAL_BYTES");
    const std::vector<std::string> removed{"war3map.w3i"};
    RequireRejected(VerifyTerrainMinimapAssetsFrozenOutput(snapshot, source, output, removed), "TERRAIN_ASSETS_FINAL_REMOVAL");
    auto saved = Saved(fixture, source, "create-edge-saved.w3x", output);
    Require(VerifyTerrainMinimapAssetsReadback(snapshot, saved).Succeeded(), "标准原edge加合法height输出应逐字回读完整依赖");
    auto stale = Saved(fixture, source, "create-edge-stale.w3x");
    RequireRejected(VerifyTerrainMinimapAssetsReadback(snapshot, stale), "TERRAIN_ASSETS_READBACK_TERRAIN");
    auto nameOutput = Saved(fixture, source, "create-edge-name.w3x", changed);
    RequireRejected(VerifyTerrainMinimapAssetsReadback(snapshot, nameOutput), "TERRAIN_ASSETS_FINAL_DEPENDENCY");
    auto noSettings = Saved(fixture, source, "create-edge-no-settings.w3x", output, removed);
    RequireRejected(VerifyTerrainMinimapAssetsReadback(snapshot, noSettings));
    Require(Take(Sha256File(source.SourcePath())) == originalArchive && source.VerifySourceUnchanged().Succeeded()
        && fixture.edits.Revision() == revision && fixture.edits.IsDirty() == dirty,
        "候选/frozen/readback均不得写原归档或活跃编辑会话");
    Require(!snapshot.formalSaveAuthorized && !snapshot.nativeEditorOrGameVerified && !candidate.manifest.formalSaveAuthorized,
        "来源闭环与SaveGate通过仍不自行授权MapBLP写入或native验收");
}

void TestDeclaredExteriorMapEdgeW3iSourceCurrentAndBudgets() {
    constexpr auto policy = TerrainMinimapRangePolicy::DeclaredW3iMarginsExteriorMapEdge;
    Fixture fixture(false, CreateW3i(), true); auto source = fixture.Source("create-current-source.w3x", CreateMaterials());
    const auto captured = Take(CaptureTerrainMinimapAssets(source, fixture.edits, {}, {}, policy));
    auto changed = *fixture.w3i; changed.insert(changed.begin() + 12, std::byte{'N'});
    Require(fixture.edits.EditSerialized(MapDataKind::Settings, changed).Succeeded(), "建立同选区同像素W3I名称修改");
    Rejected(CaptureTerrainMinimapAssets(source, fixture.edits, {}, {}, policy));
    Require(!VerifyTerrainMinimapAssetsCurrent(captured, source, fixture.edits, {}).Succeeded(), "同像素W3I变化也须使新policy快照stale");
    Require(fixture.edits.Undo(), "撤销真实W3I名称修改");
    Require(!VerifyTerrainMinimapAssetsCurrent(captured, source, fixture.edits, {}).Succeeded(), "Undo字节一致不能复活旧revision");
    const auto restored = Take(CaptureTerrainMinimapAssets(source, fixture.edits, {}, {}, policy));
    Require(restored.IdentitySha256() != captured.IdentitySha256(), "新revision须新快照身份");
    Fixture foreign(false, changed, true); foreign.w3i = CreateW3i(); auto foreignSource = foreign.Source("create-foreign.w3x", CreateMaterials());
    Require(foreign.edits.EditSerialized(MapDataKind::Settings, CreateW3i()).Succeeded(), "foreign originalB/currentA与sourceA夹具");
    const auto foreignResult = CaptureTerrainMinimapAssets(foreignSource, foreign.edits, {}, {}, policy); Rejected(foreignResult);
    Require(std::any_of(foreignResult.diagnostics.begin(), foreignResult.diagnostics.end(), [](const auto& d) {
        return d.code == "TERRAIN_EDIT_INSPECTION_SOURCE_MISMATCH";
    }), "新policy必须继续核真实原W3I基线，不能只比较currentA/sourceA");
    TerrainMinimapAssetCaptureLimits limits; limits.maxSourceBytes = restored.Facts().estimatedSettingsWorkingBytes - 1;
    const auto settingsBudget = CaptureTerrainMinimapAssets(source, fixture.edits, {}, limits, policy); Rejected(settingsBudget);
    Require(std::any_of(settingsBudget.diagnostics.begin(), settingsBudget.diagnostics.end(), [](const auto& d) {
        return d.code == "TERRAIN_ASSETS_BUDGET";
    }), "新policy Settings固定64KiB加64N预留应在Snapshot前执行");
    limits = {}; limits.maxCurrentW3eBytes = fixture.w3e.size() - 1;
    Rejected(CaptureTerrainMinimapAssets(source, fixture.edits, {}, limits, policy));
    limits = {}; limits.maxTiles = 2704; Rejected(CaptureTerrainMinimapAssets(source, fixture.edits, {}, limits, policy));
    auto longSettings = CreateW3i(); longSettings.insert(longSettings.begin() + 12, 1024, std::byte{'N'});
    Fixture bounded(false, longSettings, true); auto boundedSource = bounded.Source("create-table-cap.w3x", CreateMaterials());
    limits = {}; limits.tables.maxTableBytes = 768;
    Require(CaptureTerrainMinimapAssets(boundedSource, bounded.edits, {}, limits).Succeeded(), "三表原字可在原768B成员cap内单独捕获");
    Rejected(CaptureTerrainMinimapAssets(boundedSource, bounded.edits, {}, limits, policy));
    auto files = CreateMaterials(); files.push_back({"war3map.w3e", fixture.w3e}); files.push_back({"war3map.wpm", fixture.wpm});
    files.push_back({"war3map.w3i", *fixture.w3i});
    const auto exactPath = fixture.directory.Path() / "create-unlisted-w3i.w3x";
    Require(IO::WriteFileBytes(exactPath, UnlistedArchive(files)).Succeeded(), "写标准清单外W3I归档");
    auto exact = Take(MapArchiveSession::Open(exactPath));
    const auto unlisted = Take(CaptureTerrainMinimapAssets(exact, fixture.edits, {}, {}, policy));
    Require(VerifyTerrainMinimapAssetsReadback(unlisted, exact).Succeeded(), "新policy实际W3I读取不依赖listfile列名");
    std::size_t required = 2 * fixture.w3e.size() + fixture.w3i->size();
    for (const auto& material : CreateMaterials()) required += material.data.size();
    limits = {}; limits.maxSourceBytes = required;
    Require(VerifyTerrainMinimapAssetsReadback(unlisted, exact, limits).Succeeded(), "新policy回读按原字独立预算精确边界允许");
    --limits.maxSourceBytes; RequireRejected(VerifyTerrainMinimapAssetsReadback(unlisted, exact, limits), "TERRAIN_ASSETS_BUDGET");
}

void TestDeclaredExteriorMapEdgeW3iFailuresAndGameWinners() {
    constexpr auto policy = TerrainMinimapRangePolicy::DeclaredW3iMarginsExteriorMapEdge;
    Fixture missing(false, {}, true); auto missingSource = missing.Source("create-missing-w3i.w3x", CreateMaterials());
    auto resources = CreateMaterials(); resources.push_back({"war3map.w3i", CreateW3i()}); const auto missingGame = missing.Game(resources);
    const auto noSettings = CaptureTerrainMinimapAssets(missingSource, missing.edits, missingGame, {}, policy); Rejected(noSettings);
    Require(std::any_of(noSettings.diagnostics.begin(), noSettings.diagnostics.end(), [](const auto& d) {
        return d.code == "TERRAIN_ASSETS_W3I_MISSING";
    }), "新policy W3I准确missing不得回退game同名项");
    for (const auto& bytes : {Bytes{}, Text("broken-W3I"), [] { auto partial = CreateW3i(); partial.push_back(std::byte{0}); return partial; }()}) {
        Fixture malformed(false, bytes, true); auto badSource = malformed.Source("create-bad-w3i.w3x", CreateMaterials());
        Rejected(CaptureTerrainMinimapAssets(badSource, malformed.edits, {}, {}, policy));
    }
    Fixture fixture(false, CreateW3i(), true); auto source = fixture.Source("create-game-source.w3x", {});
    const auto game = fixture.Game(CreateMaterials());
    const auto snapshot = Take(CaptureTerrainMinimapAssets(source, fixture.edits, game, {}, policy));
    auto saved = Saved(fixture, source, "create-game-saved.w3x");
    Require(VerifyTerrainMinimapAssetsReadback(snapshot, saved).Succeeded(), "新policy回读仍须重读真实game winners并保留map W3I");
    const auto mapMaterial = CreateMaterials().back(); const std::vector<MemberReplacement> sameAtlas{mapMaterial};
    auto override = Saved(fixture, source, "create-game-map-override.w3x", sameAtlas);
    RequireRejected(VerifyTerrainMinimapAssetsReadback(snapshot, override), "TERRAIN_ASSETS_FINAL_WINNER");
    Take(WriteVerifiedArchive(game / "war3patch.mpq", std::vector<MapArchiveMember>{{"Fixture\\Ground.blp", Blp({7, 8, 9})}}));
    RequireRejected(VerifyTerrainMinimapAssetsFrozenOutput(snapshot, source, {}), "TERRAIN_ASSETS_FINAL_DEPENDENCY");
    RequireRejected(VerifyTerrainMinimapAssetsReadback(snapshot, saved), "TERRAIN_ASSETS_FINAL_DEPENDENCY");
}

void TestClassicPlayableCaptureAndDependencyReadback() {
    auto settings = W3i(); Put32(settings, 72, 0x400);
    Fixture fixture(false, settings);
    auto resources = Materials();
    resources.push_back({"UI\\MiscData.txt", Text("; original fixture\n")});
    resources.push_back({"UI\\WorldEditData.txt", Text("[CameraMargins]\nLeft=4\nRight=4\nBottom=2\nTop=2\n")});
    auto source = fixture.Source("classic-playable.w3x", resources);
    auto terrain = Take(CaptureTerrainMinimapAssets(source, fixture.edits, {}, {}, TerrainMinimapRangePolicy::ClassicPlayable));
    Require(terrain.Input().classicColors && terrain.Input().classicColors->cameraMarginsConfigured
        && terrain.Input().classicColors->cameraMargins == std::array<double, 4>{4, 4, 2, 2},
        "Classic必须从同次实际资源读取配置，不能用调用者verified标记");
    Require(terrain.Dependencies().size() == 7, "三表、W3I、两配置和atlas都必须计入依赖");
    Require(VerifyTerrainMinimapAssetsCurrent(terrain, source, fixture.edits, {}).Succeeded(), "Classic真实当前来源复核");
    Require(VerifyTerrainMinimapAssetsFrozenOutput(terrain, source, {}).Succeeded(), "Classic冻结来源复核");
    const std::vector<MemberReplacement> changed{{"UI\\WorldEditData.txt", Text("[CameraMargins]\nLeft=9\n")}};
    RequireRejected(VerifyTerrainMinimapAssetsFrozenOutput(terrain, source, changed), "TERRAIN_ASSETS_FINAL_BYTES");
    auto saved = Saved(fixture, source, "classic-readback.w3x");
    Require(VerifyTerrainMinimapAssetsReadback(terrain, saved).Succeeded(), "Classic同源依赖输出回读");
    auto objects = Take(CaptureClassicMinimapObjectAssets(std::move(terrain), source, fixture.edits));
    TerrainMinimapPostprocessSettings filters; filters.noisePercent = 0;
    const auto candidate = Take(BuildClassicMinimapCandidate(objects, filters));
    Require(candidate.manifest.sourceRectangle == std::array<std::uint32_t, 4>{1, 2, 5, 7}
        && candidate.raw.width == 4 && candidate.raw.height == 5, "真实捕获应通到Classic可玩域逐格图");
    Require(candidate.encoded.quality == 40 && candidate.finalImage.width == 256, "实际捕获应接到单mip JPEG输出");
}

void TestFinalSettingsCaptureUsesActualCurrentOutput() {
    constexpr auto policy = TerrainMinimapRangePolicy::ClassicPlayable;
    auto original = W3i(); Put32(original, 72, 0x400);
    Fixture fixture(false, original);
    auto resources = Materials();
    resources.push_back({"UI\\MiscData.txt", Text("; original fixture\n")});
    resources.push_back({"UI\\WorldEditData.txt", Text("[CameraMargins]\nLeft=0\nRight=0\nBottom=0\nTop=0\n")});
    auto source = fixture.Source("final-settings.w3x", resources);
    const auto sourceSha = Take(Sha256File(source.SourcePath()));
    const auto baseline = Take(CaptureTerrainMinimapAssets(source, fixture.edits, {}, {}, policy));
    auto settings = Take(MapSettings::Parse(original));
    settings.mapName = "Original final settings fixture";
    settings.cameraMargins = {2, 1, 1, 3}; settings.playableWidth = 5; settings.playableHeight = 4;
    const auto finalSettings = Take(settings.Serialize());
    Require(fixture.edits.EditSerialized(MapDataKind::Settings, finalSettings).Succeeded(), "通过同一会话修改实际W3I");
    auto output = fixture.edits.Replacements();
    output.push_back({"无关\\文件.txt", Text("unrelated final member")});
    auto captured = Take(CaptureTerrainMinimapAssets(source, fixture.edits, {}, {}, policy, output));
    const auto dependency = std::find_if(captured.Dependencies().begin(), captured.Dependencies().end(), [](const auto& item) {
        return item.kind == TerrainMinimapDependencyKind::Settings;
    });
    Require(captured.Input().sourceW3i == finalSettings && dependency != captured.Dependencies().end()
        && dependency->winner.layer == Resources::ResourceLayer::SessionOverride
        && dependency->containerIdentity == sourceSha && dependency->encodedSha256 == Take(Sha256(finalSettings))
        && captured.IdentitySha256() != baseline.IdentitySha256(),
        "最终W3I必须逐字来自同会话输出，源归档身份和最终正文身份分别保留");
    Require(VerifyTerrainMinimapAssetsCurrent(captured, source, fixture.edits, {}, {}, output).Succeeded()
        && VerifyTerrainMinimapAssetsFrozenOutput(captured, source, output).Succeeded(),
        "最终设置须能经过当前及后台发布前来源复核，无关非ASCII成员不影响捕获");
    Rejected(CaptureTerrainMinimapAssets(source, fixture.edits, {}, {}, policy));
    RequireRejected(VerifyTerrainMinimapAssetsFrozenOutput(captured, source, {}), "TERRAIN_ASSETS_FINAL_WINNER");
    const std::vector<MemberReplacement> stale{{"war3map.w3i", original}}, damaged{{"war3map.w3i", Text("damaged-final-settings")}};
    Rejected(CaptureTerrainMinimapAssets(source, fixture.edits, {}, {}, policy, stale));
    Rejected(CaptureTerrainMinimapAssets(source, fixture.edits, {}, {}, policy, damaged));
    RequireRejected(VerifyTerrainMinimapAssetsFrozenOutput(captured, source, stale), "TERRAIN_ASSETS_FINAL_BYTES");
    RequireRejected(VerifyTerrainMinimapAssetsFrozenOutput(captured, source, damaged), "TERRAIN_ASSETS_FINAL_BYTES");
    const std::array<std::string, 1> removal{"war3map.w3i"};
    Rejected(CaptureTerrainMinimapAssets(source, fixture.edits, {}, {}, policy, {}, removal));
    RequireRejected(VerifyTerrainMinimapAssetsFrozenOutput(captured, source, {}, removal), "TERRAIN_ASSETS_FINAL_REMOVAL");
    auto duplicate = output; duplicate.push_back({"WAR3MAP.W3I", finalSettings});
    Rejected(CaptureTerrainMinimapAssets(source, fixture.edits, {}, {}, policy, duplicate));
    RequireRejected(VerifyTerrainMinimapAssetsFrozenOutput(captured, source, duplicate), "TERRAIN_ASSETS_FINAL_DUPLICATE");
    TerrainMinimapAssetCaptureLimits limits; limits.tables.maxTableBytes = finalSettings.size() - 1;
    Rejected(CaptureTerrainMinimapAssets(source, fixture.edits, {}, limits, policy, output));
    limits = {}; limits.maxSourceBytes = captured.Facts().estimatedSettingsWorkingBytes;
    Require(CaptureTerrainMinimapAssets(source, fixture.edits, {}, limits, policy, output).Succeeded(),
        "当前正文比源正文更长时，真实设置解析峰值的精确预算可通过");
    --limits.maxSourceBytes;
    const auto budget = CaptureTerrainMinimapAssets(source, fixture.edits, {}, limits, policy, output);
    Rejected(budget);
    Require(std::any_of(budget.diagnostics.begin(), budget.diagnostics.end(), [](const auto& d) { return d.code == "TERRAIN_ASSETS_BUDGET"; }),
        "少一个字节须命中分配前设置峰值预算门");
    // 合成归档写入器只接受 ASCII 成员名；上面的非 ASCII 覆盖专门验证捕获时
    // 不扫描无关成员，回读夹具用同内容的 ASCII 名称，不改变真实 W3I 依赖。
    auto archiveOutput = output;
    archiveOutput.back().name = "Unrelated\\File.txt";
    auto saved = Saved(fixture, source, "final-settings-saved.w3x", archiveOutput);
    Require(VerifyTerrainMinimapAssetsReadback(captured, saved).Succeeded()
        && Take(saved.ReadMember("war3map.w3i")) == finalSettings
        && Take(Sha256File(source.SourcePath())) == sourceSha,
        "独立输出回读消费最终W3I且不修改源图");
    auto oldSettings = Saved(fixture, source, "old-settings-saved.w3x");
    RequireRejected(VerifyTerrainMinimapAssetsReadback(captured, oldSettings), "TERRAIN_ASSETS_FINAL_DEPENDENCY");
    auto objects = Take(CaptureClassicMinimapObjectAssets(std::move(captured), source, fixture.edits, {}, archiveOutput));
    TerrainMinimapPostprocessSettings filters; filters.noisePercent = 0;
    const auto candidate = Take(BuildClassicMinimapCandidate(objects, filters));
    Require(candidate.manifest.sourceRectangle == std::array<std::uint32_t, 4>{2, 1, 7, 5}
        && candidate.raw.width == 5 && candidate.raw.height == 4,
        "逐格小地图必须按最终设置的合法可玩矩形取色，不再使用源W3I");

    Fixture foreign(false, finalSettings); foreign.w3i = original;
    auto foreignSource = foreign.Source("wrong-settings-baseline.w3x", resources);
    const std::array<MemberReplacement, 1> foreignOutput{{{"war3map.w3i", finalSettings}}};
    const auto wrongBaseline = CaptureTerrainMinimapAssets(foreignSource, foreign.edits, {}, {}, policy, foreignOutput);
    Rejected(wrongBaseline);
    Require(std::any_of(wrongBaseline.diagnostics.begin(), wrongBaseline.diagnostics.end(), [](const auto& d) {
        return d.code == "TERRAIN_EDIT_INSPECTION_SOURCE_MISMATCH";
    }), "当前和最终相等不能掩盖会话原W3I与真实来源不同");
}

#include "TerrainMinimapAssetsCurrentCases.h"
#include "TerrainMinimapAssetsStreamingCases.h"

} // namespace

int main() {
    try {
        TestClassicPlayableCaptureAndDependencyReadback();
        TestLargeClassicCaptureAndTypedObjectOrder();
        TestFinalSettingsCaptureUsesActualCurrentOutput();
        TestCurrentEncodedClosureAndTighterBudgets();
        TestCurrentFinalTerrainAndEncodedMipChanges();
        TestCurrentWinnerLayersAndSettingsOutput();
        TestCurrentPreservesCapturedWarnings();
        TestActualMapWinnerAndOriginalMips(); TestGameFallbackAndWinnerChange(); TestSameGamePathChangedMember();
        TestFinalTerrainResourceOverlayWinsAndIsBound();
        TestFinalTerrainResourceRemovalUsesActualGameAndReadback();
        TestFinalTerrainOverlayDoesNotReadBrokenOldResource();
        TestMissingDoesNotHideUnreadable(); TestUnlistedExactAndReadError();
        TestBaselineAndCurrentRevision(); TestDuplicatePathsAndBudgets();
        TestRealHeightEditsGenerateWithoutChangingSaveAuthority();
        TestSourceMarginsCaptureAndCurrentGuards(); TestW3iMissingInvalidAndDefaultNotRequested();
        TestFinalMapDependenciesAndPolicies(); TestFinalCanonicalConflictsAndEditedW3e(); TestFinalBudgetAndUnrelatedBodies();
        TestReadbackCurrentTerrainAndSourcePreservation(); TestReadbackGameWinnersAndMapOverrides();
        TestReadbackExactPoliciesBudgetsAndOutputIdentity();
        TestDeclaredExteriorMapEdgeSourceHeightAndOutputClosure(); TestDeclaredExteriorMapEdgeW3iSourceCurrentAndBudgets();
        TestDeclaredExteriorMapEdgeW3iFailuresAndGameWinners();
        std::cout << "TerrainMinimapAssetsTests: all tests passed\n"; return 0;
    } catch (const std::exception& e) {
        std::cerr << "TerrainMinimapAssetsTests: " << e.what() << '\n'; return 1;
    }
}
