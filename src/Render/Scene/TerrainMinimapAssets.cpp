#include "War3MapCore/Render/TerrainMinimapAssets.h"

#include "War3MapCore/Game/GameDataSource.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/MapData/TerrainEditing.h"
#include "War3MapCore/MapData/TerrainSavePlan.h"
#include "War3MapCore/Render/AnimationSourceGuard.h"
#include "War3MapCore/Render/TerrainMaterials.h"
#include "War3MapCore/Resources/ImportTable.h"

#include <algorithm>
#include <bit>
#include <map>
#include <new>
#include <stdexcept>

namespace War3Map::Render {
namespace {

constexpr TerrainMinimapAssetCaptureLimits kHard{};
constexpr const char* kSubsystem = "TerrainMinimapAssets";
struct Failure { std::vector<Diagnostic> diagnostics; };

[[noreturn]] void Fail(const char* code, const char* message) {
    throw Failure{{{DiagnosticSeverity::Error, kSubsystem, code, message, {}}}};
}
template<class T> T Take(Result<T> result) {
    if (!result.Succeeded()) throw Failure{std::move(result.diagnostics)};
    return std::move(*result.value);
}
void Check(Status status) { if (!status.Succeeded()) throw Failure{std::move(status.diagnostics)}; }
template<class T, class Function> Result<T> Callback(Function&& function) {
    try { return Result<T>::Success(function()); }
    catch (Failure& failure) { return {std::nullopt, std::move(failure.diagnostics)}; }
}
std::string Canonical(std::string_view path) { return Take(Resources::CanonicalImportPath(path)); }

void Add(std::size_t& used, std::size_t bytes, std::size_t maximum) {
    if (used > maximum || bytes > maximum - used) Fail("TERRAIN_ASSETS_BUDGET", "实际读取或保留容量超过捕获预算。");
    used += bytes;
}

void ValidateLimits(const TerrainMinimapAssetCaptureLimits& limits) {
    const auto& t = limits.tables; const auto& a = limits.atlas;
    if (!limits.maxSourceBytes || limits.maxSourceBytes > kHard.maxSourceBytes || !limits.maxCurrentW3eBytes
        || limits.maxCurrentW3eBytes > kHard.maxCurrentW3eBytes || !limits.maxTiles || limits.maxTiles > kHard.maxTiles
        || !limits.maxRetainedAtlasCapacityBytes || limits.maxRetainedAtlasCapacityBytes > kHard.maxRetainedAtlasCapacityBytes
        || !t.maxTableBytes || t.maxTableBytes > kHard.tables.maxTableBytes || t.maxRows > kHard.tables.maxRows
        || t.maxCells > kHard.tables.maxCells || t.maxCellBytes > kHard.tables.maxCellBytes
        || !a.maxInputBytes || a.maxInputBytes > kHard.atlas.maxInputBytes || a.maxDimension > kHard.atlas.maxDimension
        || a.maxRgbaBytes > kHard.atlas.maxRgbaBytes || a.maxRetainedRgbaBytes > kHard.atlas.maxRetainedRgbaBytes
        || a.maxTotalMipPixels > kHard.atlas.maxTotalMipPixels || a.maxWorkingBytes > kHard.atlas.maxWorkingBytes
        || a.maxJpegSegments > kHard.atlas.maxJpegSegments || a.maxJpegScans > kHard.atlas.maxJpegScans)
        Fail("TERRAIN_ASSETS_LIMITS", "捕获预算必须有界且只能收紧既有硬上限。");
}

std::filesystem::path GameRoot(const std::filesystem::path& root) {
    if (root.empty()) return {};
    if (root.native().size() > 32767) Fail("TERRAIN_ASSETS_GAME_ROOT", "游戏来源路径超过Windows有界路径长度。");
    std::error_code error;
    auto absolute = std::filesystem::absolute(root, error);
    if (error) Fail("TERRAIN_ASSETS_GAME_ROOT", "游戏来源路径无法固定为绝对路径。");
    return absolute.lexically_normal();
}

bool Current(const MapArchiveSession& source, const MapData::MapDataEditSession& edits,
    const TerrainMinimapAssetBinding& binding) {
    return source.SourceSha256() == binding.archiveSha256 && edits.SessionIdentity() == binding.sessionIdentity
        && edits.Revision() == binding.revision && edits.TerrainSourceRevision() == binding.terrainSourceRevision;
}

// 一次调用独占此上下文。地图精确捕获固定三轮，游戏只在明确missing后挂载。
struct CaptureContext {
    const MapArchiveSession& source;
    const TerrainMinimapAssetCaptureLimits& limits;
    TerrainMinimapAssetCaptureFacts& facts;
    std::vector<TerrainMinimapAssetDependency>& dependencies;
    std::filesystem::path gameRoot;
    std::uint8_t mainTileset = 0;
    AnimationSourceGuard guard;
    std::optional<Game::GameDataSource> game;
    std::vector<ArchiveExactMember> mapMembers;
    std::map<std::string, std::size_t, std::less<>> mapIndices;
    std::map<std::string, std::size_t, std::less<>> dependencyIndices;
    std::size_t memberLimit = 0;
    TerrainMinimapDependencyKind kind = TerrainMinimapDependencyKind::Table;
    Resources::ResourceResolver resolver;

    CaptureContext(const MapArchiveSession& archive, const TerrainMinimapAssetCaptureLimits& budget,
        TerrainMinimapAssetCaptureFacts& observed, std::vector<TerrainMinimapAssetDependency>& records,
        std::filesystem::path root, std::uint8_t tileset)
        : source(archive), limits(budget), facts(observed), dependencies(records), gameRoot(std::move(root)), mainTileset(tileset) {}

    void EnsureGame() {
        if (game) return;
        Check(guard.WatchGameRoot(gameRoot));
        game = Take(Game::GameDataSource::MountStrict(gameRoot, mainTileset));
        Check(guard.Verify()); facts.gameMounted = true;
    }

    Resources::ResourceLocation Location(std::string_view path, bool map) const {
        return {std::string(path), map ? Resources::ResourceLayer::MapImport : Resources::ResourceLayer::Game,
            map ? Resources::ResourcePriority::MapArchive : Resources::ResourcePriority::GameBase,
            map ? "terrain-minimap-map:" + source.SourceSha256() : "terrain-minimap-game", std::string(path)};
    }

    void Record(std::string_view path, std::span<const std::byte> bytes, bool map, std::string container) {
        if (dependencyIndices.contains(path)) Fail("TERRAIN_ASSETS_DEPENDENCY", "同一捕获阶段重复消费了资源。");
        TerrainMinimapAssetDependency dependency;
        dependency.kind = kind; dependency.winner = Location(path, map); dependency.containerIdentity = std::move(container);
        dependency.encodedBytes = bytes.size(); dependency.encodedSha256 = Take(Sha256(bytes)); dependency.mapMemberPresent = map;
        dependencyIndices.emplace(std::string(path), dependencies.size()); dependencies.push_back(std::move(dependency));
    }

    void Phase(const std::vector<std::string>& names, std::size_t perMember, TerrainMinimapDependencyKind purpose) {
        mapMembers.clear(); mapIndices.clear(); dependencyIndices.clear(); resolver.ClearSources();
        memberLimit = perMember; kind = purpose;
        ArchiveExactMemberLimits exact;
        exact.memberCount = 16; exact.memberBytes = perMember;
        exact.totalBytes = limits.maxSourceBytes - facts.cumulativeSourceBytes;
        // baseline一轮，三表一轮，全部唯一texture一轮；无逐材质CaptureExactMembers热路径。
        mapMembers = Take(source.CaptureExactMembers(names, exact)); ++facts.exactMemberCaptureBatches;
        std::size_t batchBytes = 0;
        for (std::size_t i = 0; i < mapMembers.size(); ++i) {
            if (!mapIndices.emplace(Canonical(mapMembers[i].name), i).second)
                Fail("TERRAIN_ASSETS_DEPENDENCY", "阶段请求有规范路径冲突。");
            Add(facts.cumulativeSourceBytes, mapMembers[i].bytes.size(), limits.maxSourceBytes);
            Add(facts.mapDependencyBytes, mapMembers[i].bytes.size(), limits.maxSourceBytes);
            Add(batchBytes, mapMembers[i].bytes.size(), limits.maxSourceBytes);
        }
        facts.maximumMapBatchRetainedBytes = std::max(facts.maximumMapBatchRetainedBytes, batchBytes);
        Resources::ResourceSource mapSource;
        mapSource.id = "terrain-minimap-map:" + source.SourceSha256(); mapSource.layer = Resources::ResourceLayer::MapImport;
        mapSource.priority = Resources::ResourcePriority::MapArchive;
        mapSource.probe = [this](std::string_view path) {
            const auto found = mapIndices.find(path);
            if (found == mapIndices.end()) return Result<Resources::ResourceProbe>::Failure(kSubsystem,
                "TERRAIN_ASSETS_UNPLANNED_RESOURCE", "请求不在本阶段准确捕获名单内。");
            return Result<Resources::ResourceProbe>::Success(mapMembers[found->second].present
                ? Resources::ResourceProbe{Resources::ResourceProbeState::Found, std::string(path)} : Resources::ResourceProbe{});
        };
        mapSource.read = [this](std::string_view path) {
            return Callback<std::vector<std::byte>>([&] {
                auto& member = mapMembers.at(mapIndices.at(std::string(path)));
                Record(path, member.bytes, true, source.SourceSha256());
                return std::move(member.bytes);
            });
        };
        Check(resolver.AddSource(std::move(mapSource)));
        Resources::ResourceSource gameSource;
        gameSource.id = "terrain-minimap-game"; gameSource.layer = Resources::ResourceLayer::Game;
        gameSource.priority = Resources::ResourcePriority::GameBase;
        gameSource.probe = [this](std::string_view path) {
            return Callback<Resources::ResourceProbe>([&] {
                if (gameRoot.empty()) return Resources::ResourceProbe{};
                EnsureGame();
                return game->HasFile(std::string(path))
                    ? Resources::ResourceProbe{Resources::ResourceProbeState::Found, std::string(path)} : Resources::ResourceProbe{};
            });
        };
        gameSource.read = [this](std::string_view path) {
            return Callback<std::vector<std::byte>>([&] {
                const auto maximum = std::min(memberLimit, limits.maxSourceBytes - facts.cumulativeSourceBytes);
                auto read = Take(game->ReadFileBoundedWithSource(std::string(path), maximum));
                Add(facts.cumulativeSourceBytes, read.bytes.size(), limits.maxSourceBytes);
                Add(facts.gameDependencyBytes, read.bytes.size(), limits.maxSourceBytes);
                Record(path, read.bytes, false, std::move(read.archiveName));
                return std::move(read.bytes);
            });
        };
        Check(resolver.AddSource(std::move(gameSource)));
    }
};

Resources::ResourceResolver ReplayTables(const std::vector<Resources::TerrainTableSnapshot>& tables) {
    Resources::ResourceResolver replay;
    std::map<std::string, Resources::ResourceLocation, std::less<>> sources;
    for (const auto& table : tables) sources.emplace(table.Location().sourceId, table.Location());
    for (const auto& [id, location] : sources) {
        Resources::ResourceSource source;
        source.id = id; source.layer = location.layer; source.priority = location.priority;
        source.probe = [&tables, id](std::string_view path) {
            for (const auto& table : tables) if (table.Location().sourceId == id && table.Location().canonicalPath == path)
                return Result<Resources::ResourceProbe>::Success({Resources::ResourceProbeState::Found, table.Location().sourcePath});
            return Result<Resources::ResourceProbe>::Success({});
        };
        source.read = [&tables, id](std::string_view path) {
            for (const auto& table : tables) if (table.Location().sourceId == id && table.Location().sourcePath == path)
                return Result<std::vector<std::byte>>::Success(std::vector<std::byte>(table.Bytes().begin(), table.Bytes().end()));
            return Result<std::vector<std::byte>>::Failure(kSubsystem, "TERRAIN_ASSETS_TABLE_REPLAY", "表回放请求未捕获的来源。");
        };
        Check(replay.AddSource(std::move(source)));
    }
    return replay;
}

std::vector<TerrainMinimapMipFacts> MipFacts(const TerrainAtlasSnapshot& atlas) {
    std::vector<TerrainMinimapMipFacts> facts; facts.reserve(atlas.additionalMips.size() + 1);
    const auto append = [&](const Image::DecodedImage& mip) {
        facts.push_back({mip.width, mip.height, mip.rgba.size(), mip.rgba.capacity(), Take(Sha256(mip.rgba))});
    };
    append(atlas.image); for (const auto& mip : atlas.additionalMips) append(mip);
    return facts;
}

// 小字段按长度前缀输入流，保持有界；不复制完整编码文件或RGBA为摘要序列化缓冲。
class Identity {
public:
    Identity() : stream_(4 * 1024 * 1024) {}
    void U64(std::uint64_t value) {
        std::array<std::byte, 8> bytes{};
        for (unsigned i = 0; i < 8; ++i) bytes[i] = static_cast<std::byte>((value >> (i * 8)) & 255);
        Check(stream_.Update(bytes));
    }
    void Text(std::string_view value) { U64(value.size()); Check(stream_.Update(std::as_bytes(std::span(value.data(), value.size())))); }
    std::string Finish() { return Take(stream_.Finish()); }
private:
    Sha256Stream stream_;
};

std::string SnapshotIdentity(const TerrainMinimapAssetSnapshot& snapshot) {
    Identity hash; hash.Text(snapshot.sourcePolicy); hash.Text(snapshot.Facts().gameLookupPolicy);
    const auto& b = snapshot.Binding();
    hash.Text(b.archiveSha256); hash.Text(b.originalTerrainSha256); hash.Text(b.currentTerrainSha256);
    const auto root = b.gameRoot.generic_u8string(); hash.Text(std::string_view(reinterpret_cast<const char*>(root.data()), root.size()));
    hash.U64(b.sessionIdentity); hash.U64(b.revision); hash.U64(b.terrainSourceRevision);
    hash.U64(snapshot.Dependencies().size());
    for (const auto& d : snapshot.Dependencies()) {
        hash.U64(static_cast<std::uint64_t>(d.kind)); hash.Text(d.winner.canonicalPath);
        hash.U64(static_cast<std::uint64_t>(d.winner.layer)); hash.U64(static_cast<std::uint64_t>(d.winner.priority));
        hash.Text(d.winner.sourceId); hash.Text(d.winner.sourcePath); hash.Text(d.containerIdentity);
        hash.Text(d.encodedSha256); hash.U64(d.encodedBytes); hash.U64(d.mapMemberPresent ? 1u : 0u);
        hash.U64(d.decodedMips.size());
        for (const auto& mip : d.decodedMips) {
            hash.U64(mip.width); hash.U64(mip.height); hash.U64(mip.rgbaBytes); hash.Text(mip.decodedRgbaSha256);
        }
    }
    hash.U64(snapshot.Tables().size());
    for (const auto& t : snapshot.Tables()) { hash.U64(t.dependencyIndex); hash.U64(t.rows); hash.Text(t.idColumn); hash.Text(t.identitySha256); }
    hash.U64(snapshot.GroundAssets().size());
    for (const auto& g : snapshot.GroundAssets()) {
        hash.Text(g.rawcode); hash.Text(g.canonicalTexturePath); hash.U64(static_cast<std::uint64_t>(g.tableRow)); hash.U64(g.dependencyIndex);
    }
    return hash.Finish();
}

} // namespace

Result<TerrainMinimapAssetSnapshot> CaptureTerrainMinimapAssets(const MapArchiveSession& source,
    const MapData::MapDataEditSession& edits, const std::filesystem::path& gameRoot,
    const TerrainMinimapAssetCaptureLimits& limits) {
    try {
        ValidateLimits(limits);
        TerrainMinimapAssetSnapshot result;
        auto& binding = result.binding_; auto& facts = result.facts_;
        binding.archiveSha256 = source.SourceSha256(); binding.sessionIdentity = edits.SessionIdentity();
        binding.revision = edits.Revision(); binding.terrainSourceRevision = edits.TerrainSourceRevision(); binding.gameRoot = GameRoot(gameRoot);
        auto terrain = edits.Terrain();
        if (!terrain || terrain->header.version != 11 || terrain->header.groundTilesets.empty() || terrain->header.groundTilesets.size() > 16)
            Fail("TERRAIN_ASSETS_TERRAIN", "捕获需要真实编辑会话的classic v11与1..16个地面材质槽。");
        if (terrain->tiles.size() > limits.maxTiles || terrain->vertices.size() > 2 * limits.maxTiles + 2)
            Fail("TERRAIN_ASSETS_BUDGET", "当前地形超过捕获网格预算。");
        {
            // 此处只取得真实基线关系。SaveGate可失败，不能因此授予/改变正式保存资格。
            const auto plan = Take(MapData::TerrainSavePlan::Prepare(source, edits, {limits.maxSourceBytes}));
            facts.exactMemberCaptureBatches = 1;
            for (const auto& member : plan.Members()) if (member.sourceBytes) Add(facts.baselineSourceBytes, *member.sourceBytes, limits.maxSourceBytes);
            facts.cumulativeSourceBytes = facts.baselineSourceBytes;
            if (!plan.Members()[0].sourceSha256) Fail("TERRAIN_ASSETS_BASELINE", "源W3E缺少准确成员身份。");
            binding.originalTerrainSha256 = *plan.Members()[0].sourceSha256;
        }
        const auto serializedBytes = 37 + (terrain->header.groundTilesets.size() + terrain->header.cliffTilesets.size()) * 4
            + terrain->vertices.size() * 7;
        if (serializedBytes > limits.maxCurrentW3eBytes) Fail("TERRAIN_ASSETS_BUDGET", "当前W3E序列化前字节估算超过捕获预算。");
        result.input_.currentW3e = Take(MapData::SerializeMapTerrain(*terrain));
        if (result.input_.currentW3e.size() > limits.maxCurrentW3eBytes) Fail("TERRAIN_ASSETS_BUDGET", "当前W3E超过捕获字节预算。");
        binding.currentTerrainSha256 = Take(Sha256(result.input_.currentW3e));
        result.input_.binding = {"terrain-session:" + std::to_string(binding.sessionIdentity), binding.sessionIdentity,
            binding.revision, binding.archiveSha256, binding.currentTerrainSha256};
        CaptureContext context{source, limits, facts, result.dependencies_, binding.gameRoot, terrain->header.mainTileset};
        Check(context.guard.Watch(source.SourcePath()));
        std::vector<Diagnostic> diagnostics;
        std::vector<std::string> texturePaths;
        {
            const std::vector<std::string> tablePaths{"TerrainArt\\Terrain.slk", "TerrainArt\\CliffTypes.slk", "TerrainArt\\Water.slk"};
            context.Phase(tablePaths, limits.tables.maxTableBytes, TerrainMinimapDependencyKind::Table);
            std::vector<Resources::TerrainTableSnapshot> tables;
            const std::array<std::string_view, 3> ids{"tileid", "cliffid", "waterid"};
            tables.reserve(3);
            for (std::size_t i = 0; i < 3; ++i) {
                tables.push_back(Take(Resources::CaptureTerrainTable(context.resolver, tablePaths[i], ids[i], limits.tables)));
                const auto index = context.dependencyIndices.at(tables.back().Location().canonicalPath);
                result.tables_.push_back({index, tables.back().Rows().size(), tables.back().IdColumn(), tables.back().IdentitySha256()});
                if (result.dependencies_[index].encodedSha256 != tables.back().SourceSha256())
                    Fail("TERRAIN_ASSETS_IDENTITY", "表捕获字节与实际来源摘要不同。");
            }
            auto replay = ReplayTables(tables);
            TerrainMaterialLimits materialLimits;
            materialLimits.maxTableBytes = limits.tables.maxTableBytes; materialLimits.maxRowsPerTable = limits.tables.maxRows;
            materialLimits.maxCellsPerTable = limits.tables.maxCells; materialLimits.maxCellBytes = limits.tables.maxCellBytes;
            const auto catalog = Take(LoadTerrainMaterialCatalog(replay, materialLimits));
            std::map<std::string, std::size_t, std::less<>> unique;
            for (const auto& rawcode : terrain->header.groundTilesets) {
                const auto found = catalog.ground.find(rawcode);
                if (found == catalog.ground.end()) Fail("TERRAIN_ASSETS_UNKNOWN_GROUND", "当前地面rawcode未在真实Terrain.slk中定义。");
                const auto path = Canonical(found->second.texturePath);
                if (unique.emplace(path, texturePaths.size()).second) texturePaths.push_back(path);
                result.ground_.push_back({rawcode, path, found->second.source.row, 0});
            }
        } // 释放三表原字节与catalog后才捕获/解码atlas，避免长期保留重复表解析中间体。
        Check(context.guard.Verify());
        context.Phase(texturePaths, limits.atlas.maxInputBytes, TerrainMinimapDependencyKind::Atlas);
        std::map<std::string, std::shared_ptr<const TerrainAtlasSnapshot>, std::less<>> atlases;
        for (const auto& path : texturePaths) {
            auto resource = Take(context.resolver.Read(path));
            auto atlasLimits = limits.atlas;
            atlasLimits.maxRetainedRgbaBytes = std::min(atlasLimits.maxRetainedRgbaBytes,
                limits.maxRetainedAtlasCapacityBytes - facts.retainedAtlasRgbaCapacityBytes);
            auto decoded = DecodeTerrainTextureAtlas(resource, atlasLimits);
            diagnostics.insert(diagnostics.end(), decoded.diagnostics.begin(), decoded.diagnostics.end());
            const auto atlas = Take(std::move(decoded));
            const auto index = context.dependencyIndices.at(path);
            if (atlas->sourceSha256 != result.dependencies_[index].encodedSha256)
                Fail("TERRAIN_ASSETS_IDENTITY", "atlas原编码字节与准确winner来源不同。");
            auto& dependency = result.dependencies_[index]; dependency.decodedMips = MipFacts(*atlas);
            for (const auto& mip : dependency.decodedMips)
                Add(facts.retainedAtlasRgbaCapacityBytes, mip.rgbaCapacityBytes, limits.maxRetainedAtlasCapacityBytes);
            atlases.emplace(path, atlas); ++facts.uniqueAtlasCount;
        }
        result.input_.groundAtlases.reserve(result.ground_.size());
        for (auto& ground : result.ground_) {
            ground.dependencyIndex = context.dependencyIndices.at(ground.canonicalTexturePath);
            result.input_.groundAtlases.push_back(atlases.at(ground.canonicalTexturePath));
        }
        Check(context.guard.Verify());
        // 不额外VerifySourceUnchanged重复散列整图：最后一批精确捕获已核SHA/租约，
        // 本次guard仍持有地图路径拒写租约并在上面核身份/路径；再核编辑修订。
        if (!Current(source, edits, binding) || edits.Terrain() != terrain)
            Fail("TERRAIN_ASSETS_STALE", "捕获期间实际会话、修订或地形来源已变化。");
        result.identitySha256_ = SnapshotIdentity(result);
        auto success = Result<TerrainMinimapAssetSnapshot>::Success(std::move(result)); success.diagnostics = std::move(diagnostics); return success;
    } catch (Failure& failure) {
        return {std::nullopt, std::move(failure.diagnostics)};
    } catch (const std::bad_alloc&) {
        return Result<TerrainMinimapAssetSnapshot>::Failure(kSubsystem, "TERRAIN_ASSETS_ALLOCATION", "资源捕获分配失败，不返回部分快照。");
    } catch (const std::length_error&) {
        return Result<TerrainMinimapAssetSnapshot>::Failure(kSubsystem, "TERRAIN_ASSETS_ALLOCATION", "资源捕获容量异常，不返回部分快照。");
    }
}

Status VerifyTerrainMinimapAssetsCurrent(const TerrainMinimapAssetSnapshot& snapshot, const MapArchiveSession& source,
    const MapData::MapDataEditSession& edits, const std::filesystem::path& gameRoot, const TerrainMinimapAssetCaptureLimits& limits) {
    try {
        if (!Current(source, edits, snapshot.Binding()) || GameRoot(gameRoot) != snapshot.Binding().gameRoot)
            return Status::Failure(kSubsystem, "TERRAIN_ASSETS_STALE", "资源快照不属于当前来源或编辑修订。");
        auto current = CaptureTerrainMinimapAssets(source, edits, gameRoot, limits);
        if (!current.Succeeded()) return {std::move(current.diagnostics)};
        if (current.value->IdentitySha256() != snapshot.IdentitySha256())
            return Status::Failure(kSubsystem, "TERRAIN_ASSETS_STALE", "实际winner、编码字节或解码mip身份已变化。");
        return {std::move(current.diagnostics)};
    } catch (Failure& failure) { return {std::move(failure.diagnostics)}; }
}

} // namespace War3Map::Render
