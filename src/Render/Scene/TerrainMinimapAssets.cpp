#include "War3MapCore/Render/TerrainMinimapAssets.h"

#include "War3MapCore/Game/GameDataSource.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/MapData/TerrainEditing.h"
#include "War3MapCore/MapData/TerrainEditInspection.h"
#include "War3MapCore/MapData/TerrainSavePlan.h"
#include "War3MapCore/Render/AnimationSourceGuard.h"
#include "War3MapCore/Render/TerrainMaterials.h"
#include "War3MapCore/Render/TerrainMinimapRepresentativeColor.h"
#include "War3MapCore/Image/BlpMipChain.h"
#include "War3MapCore/Resources/ImportTable.h"
#include "War3MapCore/Text/TextUtil.h"

#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <map>
#include <new>
#include <set>
#include <stdexcept>

namespace War3Map::Render {
namespace {

constexpr TerrainMinimapAssetCaptureLimits kHard{};
constexpr const char* kSubsystem = "TerrainMinimapAssets";
struct Failure { std::vector<Diagnostic> diagnostics; };

[[noreturn]] void Fail(const char* code, std::string message) {
    throw Failure{{{DiagnosticSeverity::Error, kSubsystem, code, std::move(message), {}}}};
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
bool ResourceOverlayPath(std::string_view path, bool finalSettings) {
    // W3E始终来自编辑会话。W3I捕获先单独核源基线，再采用同会话最终正文；
    // worker复核时才把已封存的最终Settings加入普通依赖视图。
    return path != "WAR3MAP.W3E" && (path != "WAR3MAP.W3I" || finalSettings);
}

void Add(std::size_t& used, std::size_t bytes, std::size_t maximum) {
    if (used > maximum || bytes > maximum - used) Fail("TERRAIN_ASSETS_BUDGET", "实际读取或保留容量超过捕获预算。");
    used += bytes;
}

void ValidateLimits(const TerrainMinimapAssetCaptureLimits& limits) {
    Check(MapData::ValidateTerrainRowScanLimits(limits.classicScan));
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

bool SameCaptureLimits(const TerrainMinimapAssetCaptureLimits& requested,
    const TerrainMinimapAssetCaptureLimits& captured) {
    const auto& t = requested.tables; const auto& oldT = captured.tables;
    const auto& a = requested.atlas; const auto& oldA = captured.atlas;
    return requested.maxSourceBytes == captured.maxSourceBytes
        && requested.maxCurrentW3eBytes == captured.maxCurrentW3eBytes && requested.maxTiles == captured.maxTiles
        && requested.classicScan == captured.classicScan
        && requested.maxRetainedAtlasCapacityBytes == captured.maxRetainedAtlasCapacityBytes
        && t.maxTableBytes == oldT.maxTableBytes && t.maxRows == oldT.maxRows
        && t.maxCells == oldT.maxCells && t.maxCellBytes == oldT.maxCellBytes
        && a.maxInputBytes == oldA.maxInputBytes && a.maxDimension == oldA.maxDimension
        && a.maxRgbaBytes == oldA.maxRgbaBytes && a.maxRetainedRgbaBytes == oldA.maxRetainedRgbaBytes
        && a.maxTotalMipPixels == oldA.maxTotalMipPixels && a.maxWorkingBytes == oldA.maxWorkingBytes
        && a.maxJpegSegments == oldA.maxJpegSegments && a.maxJpegScans == oldA.maxJpegScans;
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
    std::map<std::string, const MemberReplacement*, std::less<>> replacements;
    std::set<std::string, std::less<>> removals;
    std::size_t memberLimit = 0;
    TerrainMinimapDependencyKind kind = TerrainMinimapDependencyKind::Table;
    Resources::ResourceResolver resolver;

    CaptureContext(const MapArchiveSession& archive, const TerrainMinimapAssetCaptureLimits& budget,
        TerrainMinimapAssetCaptureFacts& observed, std::vector<TerrainMinimapAssetDependency>& records,
        std::filesystem::path root, std::uint8_t tileset,
        std::span<const MemberReplacement> output = {}, std::span<const std::string> removed = {},
        bool finalSettings = false)
        : source(archive), limits(budget), facts(observed), dependencies(records), gameRoot(std::move(root)), mainTileset(tileset) {
        if (output.size() > 16384 || removed.size() > 16384)
            Fail("TERRAIN_ASSETS_BUDGET", "地形资源overlay超过有界成员数量。");
        std::set<std::string, std::less<>> all;
        std::size_t pathBytes = 0;
        for (const auto& replacement : output) {
            if (replacement.name.size() > ArchiveExactMemberLimits{}.pathBytes)
                Fail("TERRAIN_ASSETS_BUDGET", "地形资源overlay成员路径过长。");
            const auto canonical = Resources::CanonicalImportPath(replacement.name);
            // 保存任务还可能带有与地形无关的任意成员。资源捕获只消费
            // 可安全规范化的依赖路径，不能因为无关的非ASCII文件名替保存门
            // 读取或拒绝正文；通用归档校验仍由保存服务负责。
            if (!canonical.Succeeded() || !canonical.value) continue;
            const auto& path = *canonical.value;
            Add(pathBytes, path.size(), ArchiveExactMemberLimits{}.totalPathBytes);
            if (!all.emplace(path).second)
                Fail("TERRAIN_ASSETS_FINAL_DUPLICATE", "地形资源overlay存在规范路径重复：" + path);
            if (ResourceOverlayPath(path, finalSettings)) replacements.emplace(path, &replacement);
        }
        for (const auto& removal : removed) {
            if (removal.size() > ArchiveExactMemberLimits{}.pathBytes)
                Fail("TERRAIN_ASSETS_BUDGET", "地形资源overlay成员路径过长。");
            const auto canonical = Resources::CanonicalImportPath(removal);
            if (!canonical.Succeeded() || !canonical.value) continue;
            const auto& path = *canonical.value;
            Add(pathBytes, path.size(), ArchiveExactMemberLimits{}.totalPathBytes);
            if (!all.emplace(path).second)
                Fail("TERRAIN_ASSETS_FINAL_CONFLICT", "地形资源overlay同时重复删除或替换：" + path);
            if (ResourceOverlayPath(path, finalSettings)) removals.emplace(path);
        }
    }

    void EnsureGame() {
        if (game) return;
        Check(guard.WatchGameRoot(gameRoot));
        game = Take(Game::GameDataSource::MountStrict(gameRoot, mainTileset));
        Check(guard.Verify()); facts.gameMounted = true;
    }

    Resources::ResourceLocation Location(std::string_view path, bool map, bool overlay = false) const {
        return {std::string(path), overlay ? Resources::ResourceLayer::SessionOverride
            : map ? Resources::ResourceLayer::MapImport : Resources::ResourceLayer::Game,
            overlay ? Resources::ResourcePriority::SessionOverride
                : map ? Resources::ResourcePriority::MapArchive : Resources::ResourcePriority::GameBase,
            overlay ? "terrain-minimap-overlay" : map ? "terrain-minimap-map:" + source.SourceSha256() : "terrain-minimap-game",
            std::string(path)};
    }

    void Record(std::string_view path, std::span<const std::byte> bytes, bool map, std::string container,
        bool overlay = false) {
        if (dependencyIndices.contains(path)) Fail("TERRAIN_ASSETS_DEPENDENCY", "同一捕获阶段重复消费了资源。");
        TerrainMinimapAssetDependency dependency;
        dependency.kind = kind; dependency.winner = Location(path, map, overlay); dependency.containerIdentity = std::move(container);
        dependency.encodedBytes = bytes.size(); dependency.encodedSha256 = Take(Sha256(bytes)); dependency.mapMemberPresent = map;
        dependencyIndices.emplace(std::string(path), dependencies.size()); dependencies.push_back(std::move(dependency));
    }

    void Phase(const std::vector<std::string>& names, std::size_t perMember, TerrainMinimapDependencyKind purpose) {
        mapMembers.clear(); mapIndices.clear(); dependencyIndices.clear(); resolver.ClearSources();
        memberLimit = perMember; kind = purpose;
        ArchiveExactMemberLimits exact;
        exact.memberCount = 64; exact.memberBytes = perMember;
        // baseline一轮，三表一轮，全部唯一texture一轮；无逐材质CaptureExactMembers热路径。
        std::vector<std::string> sourceNames;
        sourceNames.reserve(names.size());
        for (const auto& name : names) {
            const auto path = Canonical(name);
            if (const auto replacement = replacements.find(path); replacement != replacements.end()) {
                if (replacement->second->data.size() > memberLimit)
                    Fail("TERRAIN_ASSETS_BUDGET", "地形资源overlay超过本阶段成员预算：" + path);
                Add(facts.cumulativeSourceBytes, replacement->second->data.size(), limits.maxSourceBytes);
            } else if (!removals.contains(path)) sourceNames.push_back(name);
        }
        exact.totalBytes = limits.maxSourceBytes - facts.cumulativeSourceBytes;
        auto sourceMembers = sourceNames.empty()
            ? std::vector<ArchiveExactMember>{}
            : Take(source.CaptureExactMembers(sourceNames, exact));
        std::map<std::string, ArchiveExactMember, std::less<>> sourceByPath;
        for (auto& member : sourceMembers) sourceByPath.emplace(Canonical(member.name), std::move(member));
        mapMembers.reserve(names.size());
        for (const auto& name : names) {
            const auto path = Canonical(name);
            if (const auto replacement = replacements.find(path); replacement != replacements.end()) {
                mapMembers.push_back({std::string(name), true, replacement->second->data});
                Add(facts.mapDependencyBytes, replacement->second->data.size(), limits.maxSourceBytes);
            } else if (removals.contains(path)) {
                mapMembers.push_back({std::string(name), false, {}});
            } else if (const auto source = sourceByPath.find(path); source != sourceByPath.end()) {
                mapMembers.push_back(std::move(source->second));
            } else {
                mapMembers.push_back({std::string(name), false, {}});
            }
        }
        ++facts.exactMemberCaptureBatches;
        std::size_t batchBytes = 0;
        for (std::size_t i = 0; i < mapMembers.size(); ++i) {
            if (!mapIndices.emplace(Canonical(mapMembers[i].name), i).second)
                Fail("TERRAIN_ASSETS_DEPENDENCY", "阶段请求有规范路径冲突。");
            if (!replacements.contains(Canonical(mapMembers[i].name))) {
                Add(facts.cumulativeSourceBytes, mapMembers[i].bytes.size(), limits.maxSourceBytes);
                Add(facts.mapDependencyBytes, mapMembers[i].bytes.size(), limits.maxSourceBytes);
            }
            Add(batchBytes, mapMembers[i].bytes.size(), limits.maxSourceBytes);
        }
        facts.maximumMapBatchRetainedBytes = std::max(facts.maximumMapBatchRetainedBytes, batchBytes);
        for (const bool overlay : {true, false}) {
            Resources::ResourceSource mapSource;
            const auto location = Location(std::string_view{}, true, overlay);
            mapSource.id = location.sourceId; mapSource.layer = location.layer; mapSource.priority = location.priority;
            mapSource.probe = [this, overlay](std::string_view path) {
                const auto found = mapIndices.find(path);
                if (found == mapIndices.end()) return Result<Resources::ResourceProbe>::Failure(kSubsystem,
                    "TERRAIN_ASSETS_UNPLANNED_RESOURCE", "请求不在本阶段准确捕获名单内。");
                return Result<Resources::ResourceProbe>::Success(mapMembers[found->second].present
                    && replacements.contains(path) == overlay
                    ? Resources::ResourceProbe{Resources::ResourceProbeState::Found, std::string(path)} : Resources::ResourceProbe{});
            };
            mapSource.read = [this, overlay](std::string_view path) {
                return Callback<std::vector<std::byte>>([&] {
                    auto& member = mapMembers.at(mapIndices.at(std::string(path)));
                    Record(path, member.bytes, true, source.SourceSha256(), overlay);
                    return std::move(member.bytes);
                });
            };
            Check(resolver.AddSource(std::move(mapSource)));
        }
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

Text::ProfileSections MinimapConfiguration(CaptureContext& context) {
    Text::ProfileSections merged;
    // 与原版通用配置的后两层顺序相同。需要的键必须在实际读取的配置中存在。
    for (const auto path : {"UI\\MiscData.txt", "UI\\WorldEditData.txt"}) {
        const auto read = Take(context.resolver.Read(path));
        auto text = std::string_view(reinterpret_cast<const char*>(read.bytes.data()), read.bytes.size());
        if (text.starts_with("\xef\xbb\xbf")) text.remove_prefix(3);
        for (auto& [section, fields] : Text::ParseProfile(text))
            for (auto& [key, value] : fields) merged[section][key] = std::move(value);
    }
    return merged;
}

double ConfigurationNumber(const Text::ProfileSections& profile, const char* section, const char* key) {
    const auto group = profile.find(section);
    if (group == profile.end() || !group->second.contains(key))
        Fail("CLASSIC_MINIMAP_CONFIG_MISSING", std::string("小地图配置缺少 ") + section + "." + key);
    const auto text = Text::TrimCWhitespace(group->second.at(key));
    double value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || !std::isfinite(value)
        || value < 0 || value > 65536)
        Fail("CLASSIC_MINIMAP_CONFIG_INVALID", std::string("小地图配置数值无效：") + section + "." + key);
    return value;
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
    void F64(double value) { U64(std::bit_cast<std::uint64_t>(value)); }
    void Text(std::string_view value) { U64(value.size()); Check(stream_.Update(std::as_bytes(std::span(value.data(), value.size())))); }
    std::string Finish() { return Take(stream_.Finish()); }
private:
    Sha256Stream stream_;
};

std::string SnapshotIdentity(const TerrainMinimapAssetSnapshot& snapshot) {
    Identity hash; hash.Text(snapshot.sourcePolicy); hash.Text(snapshot.Facts().gameLookupPolicy);
    hash.U64(static_cast<std::uint64_t>(snapshot.Input().rangePolicy));
    const auto& b = snapshot.Binding();
    hash.Text(b.archiveSha256); hash.Text(b.originalTerrainSha256); hash.Text(b.currentTerrainSha256);
    hash.U64(b.originalTerrainBytes); hash.U64(b.mainTileset);
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
    if (snapshot.Input().classicColors) {
        const auto& c = *snapshot.Input().classicColors;
        hash.U64(1); hash.U64(c.waterConfigured ? 1u : 0u); hash.F64(c.waterHeightOffset);
        hash.F64(c.deepLevelPathing); hash.U64(c.cameraMarginsConfigured ? 1u : 0u);
        for (const auto value : c.cameraMargins) hash.F64(value);
        hash.U64(c.waterColor[0]); hash.U64(c.waterColor[1]); hash.U64(c.waterColor[2]); hash.U64(c.waterColor[3]);
        hash.U64(c.cliffColors.size());
        for (const auto& color : c.cliffColors) for (const auto value : color) hash.U64(value);
    } else hash.U64(0);
    return hash.Finish();
}

struct FinalMember {
    const TerrainMinimapAssetDependency* dependency = nullptr; // null仅用于W3E。
    const MemberReplacement* replacement = nullptr;
    bool removed = false;
};

using FinalMembers = std::map<std::string, FinalMember, std::less<>>;

FinalMembers SelectFinalMembers(const TerrainMinimapAssetSnapshot& snapshot,
    std::span<const MemberReplacement> replacements, std::span<const std::string> removals) {
    FinalMembers selected;
    selected.emplace(Canonical("war3map.w3e"), FinalMember{});
    for (const auto& dependency : snapshot.Dependencies()) {
        if (!selected.emplace(Canonical(dependency.winner.canonicalPath), FinalMember{&dependency}).second)
            Fail("TERRAIN_ASSETS_FINAL_SNAPSHOT", "冻结依赖存在重复规范路径。");
    }
    const auto key = [](std::string_view name) -> std::optional<std::string> {
        // 只查所消费的ASCII安全路径；其它名称/全表合法性仍由通用writer负责。
        if (name.size() > ArchiveExactMemberLimits{}.pathBytes) return {};
        auto canonical = Resources::CanonicalImportPath(name);
        if (!canonical.Succeeded()) return {};
        return std::move(*canonical.value);
    };
    for (const auto& replacement : replacements) {
        const auto path = key(replacement.name);
        if (!path) continue;
        const auto found = selected.find(*path);
        if (found == selected.end()) continue;
        if (found->second.replacement)
            Fail("TERRAIN_ASSETS_FINAL_DUPLICATE", "最终列表重复覆盖小地图依赖：" + found->first);
        found->second.replacement = &replacement;
    }
    for (const auto& removal : removals) {
        const auto path = key(removal);
        if (!path) continue;
        const auto found = selected.find(*path);
        if (found == selected.end()) continue;
        if (found->second.removed)
            Fail("TERRAIN_ASSETS_FINAL_DUPLICATE", "最终列表重复删除小地图依赖：" + found->first);
        found->second.removed = true;
    }
    for (const auto& [path, member] : selected) {
        if (member.removed && member.replacement)
            Fail("TERRAIN_ASSETS_FINAL_CONFLICT", "最终列表同时删除和替换小地图依赖：" + path);
        // 资源删除只屏蔽地图层；若捕获时确实选中game，最终列表必须仍能
        // 重现该winner。W3E/Settings和地图winner依然不能被删除。
        if (member.removed && (!member.dependency || member.dependency->mapMemberPresent))
            Fail("TERRAIN_ASSETS_FINAL_REMOVAL", "最终列表删除了小地图依赖：" + path);
    }
    return selected;
}

std::size_t PrechargeInputs(const TerrainMinimapAssetSnapshot& snapshot, std::size_t terrainReadBytes,
    const TerrainMinimapAssetCaptureLimits& limits) {
    if (snapshot.Input().currentW3e.size() > limits.maxCurrentW3eBytes || !terrainReadBytes)
        Fail("TERRAIN_ASSETS_BUDGET", "冻结W3E缺少有界的源/现字节大小。");
    std::size_t total = 0;
    Add(total, terrainReadBytes, limits.maxSourceBytes);
    Add(total, snapshot.Input().currentW3e.size(), limits.maxSourceBytes);
    for (const auto& dependency : snapshot.Dependencies()) {
        const auto cap = dependency.kind == TerrainMinimapDependencyKind::Atlas
            ? limits.atlas.maxInputBytes : limits.tables.maxTableBytes;
        if (dependency.encodedBytes > cap)
            Fail("TERRAIN_ASSETS_BUDGET", "冻结依赖超过本次收紧的成员硬限额：" + dependency.winner.canonicalPath);
        Add(total, dependency.encodedBytes, limits.maxSourceBytes);
    }
    return total;
}

std::size_t PrechargeFinal(const TerrainMinimapAssetSnapshot& snapshot, const FinalMembers& selected,
    const TerrainMinimapAssetCaptureLimits& limits) {
    auto total = PrechargeInputs(snapshot, snapshot.Binding().originalTerrainBytes, limits);
    std::size_t hashed = snapshot.Input().currentW3e.size();
    for (const auto& entry : selected) if (entry.second.replacement) {
        Add(total, entry.second.replacement->data.size(), limits.maxSourceBytes);
        Add(hashed, entry.second.replacement->data.size(), limits.maxSourceBytes);
    }
    return hashed; // source/依赖的预收留给随后CaptureContext实际读取时累计。
}

void VerifyCurrentTerrainBytes(const TerrainMinimapAssetSnapshot& snapshot) {
    if (Take(Sha256(snapshot.Input().currentW3e)) != snapshot.Binding().currentTerrainSha256)
        Fail("TERRAIN_ASSETS_FINAL_SNAPSHOT", "冻结当前W3E正文与真实捕获身份不同。");
}

void VerifyFinalBytes(const TerrainMinimapAssetSnapshot& snapshot, const FinalMembers& selected,
    bool liveTerrain = false) {
    const auto& binding = snapshot.Binding();
    VerifyCurrentTerrainBytes(snapshot);
    for (const auto& [path, member] : selected) {
        if (!liveTerrain && !member.dependency && !member.replacement && binding.originalTerrainSha256 != binding.currentTerrainSha256)
            Fail("TERRAIN_ASSETS_FINAL_W3E_MISSING", "最终列表缺少冻结的已编辑W3E输出。");
        if (member.dependency && member.dependency->winner.layer == Resources::ResourceLayer::SessionOverride
            && !member.replacement)
            Fail("TERRAIN_ASSETS_FINAL_WINNER", "最终列表缺少捕获时实际消费的资源overlay：" + path);
        if (!member.replacement) continue;
        if (member.dependency && !member.dependency->mapMemberPresent)
            Fail("TERRAIN_ASSETS_FINAL_WINNER", "最终新增地图成员将覆盖冻结game winner：" + path);
        const auto expectedBytes = member.dependency ? member.dependency->encodedBytes : snapshot.Input().currentW3e.size();
        const auto& expectedSha = member.dependency ? member.dependency->encodedSha256 : binding.currentTerrainSha256;
        if (member.replacement->data.size() != expectedBytes || Take(Sha256(member.replacement->data)) != expectedSha)
            Fail("TERRAIN_ASSETS_FINAL_BYTES", "最终成员实际正文与小地图冻结依赖不同：" + path);
    }
}

bool SameWinner(const TerrainMinimapAssetDependency& a, const TerrainMinimapAssetDependency& b, std::string_view savedMapSha) {
    // 新归档map资源的容器/sourceId必然随整档SHA改变；其实际路径/层级/正文仍必须相同。
    const bool rebindMap = !savedMapSha.empty() && a.mapMemberPresent && b.mapMemberPresent;
    const bool reemittedMap = savedMapSha.empty() && a.mapMemberPresent && b.mapMemberPresent
        && a.winner.layer == Resources::ResourceLayer::SessionOverride && b.winner.layer == Resources::ResourceLayer::MapImport;
    const bool sameContainer = rebindMap
        ? a.containerIdentity == savedMapSha && a.winner.sourceId == "terrain-minimap-map:" + std::string(savedMapSha)
        : reemittedMap ? a.containerIdentity == b.containerIdentity
        : a.winner.sourceId == b.winner.sourceId && a.containerIdentity == b.containerIdentity;
    const bool sameLayer = rebindMap
        ? a.winner.layer == Resources::ResourceLayer::MapImport && a.winner.priority == Resources::ResourcePriority::MapArchive
            && (b.winner.layer == Resources::ResourceLayer::MapImport || b.winner.layer == Resources::ResourceLayer::SessionOverride)
        : reemittedMap || (a.winner.layer == b.winner.layer && a.winner.priority == b.winner.priority);
    return a.winner.canonicalPath == b.winner.canonicalPath && sameLayer
        && a.winner.sourcePath == b.winner.sourcePath && sameContainer
        && a.mapMemberPresent == b.mapMemberPresent && a.encodedBytes == b.encodedBytes && a.encodedSha256 == b.encodedSha256;
}

void VerifyDependencyPhase(CaptureContext& context, const TerrainMinimapAssetSnapshot& snapshot, bool atlas,
    bool savedMap = false, bool exactCurrentWinner = false) {
    std::vector<std::string> names;
    for (const auto& dependency : snapshot.Dependencies())
        if ((dependency.kind == TerrainMinimapDependencyKind::Atlas) == atlas) names.push_back(dependency.winner.canonicalPath);
    context.Phase(names, atlas ? context.limits.atlas.maxInputBytes : context.limits.tables.maxTableBytes,
        atlas ? TerrainMinimapDependencyKind::Atlas : TerrainMinimapDependencyKind::Table);
    for (const auto& expected : snapshot.Dependencies()) {
        if ((expected.kind == TerrainMinimapDependencyKind::Atlas) != atlas) continue;
        const auto& path = expected.winner.canonicalPath;
        if (context.mapMembers.at(context.mapIndices.at(path)).present != expected.mapMemberPresent)
            Fail("TERRAIN_ASSETS_FINAL_WINNER", "源地图存在性改变了小地图依赖winner：" + path);
        const auto resource = Take(context.resolver.Read(path));
        const auto& actual = context.dependencies.at(context.dependencyIndices.at(path));
        if (resource.bytes.size() != expected.encodedBytes
            || !SameWinner(actual, expected,
                savedMap ? std::string_view(context.source.SourceSha256()) : std::string_view{}))
            Fail("TERRAIN_ASSETS_FINAL_DEPENDENCY", "实际来源容器、winner或编码正文已改变：" + path);
        // Current仍保持捕获时的来源层；同字atlas新增overlay也不能冒充旧map winner。
        // W3I捕获只把实际改变的最终正文标为overlay，同字显式回写仍是原map语义。
        if (exactCurrentWinner && actual.winner.layer != expected.winner.layer
            && expected.kind != TerrainMinimapDependencyKind::Settings)
            Fail("TERRAIN_ASSETS_STALE", "当前资源来源层不同于捕获时的winner：" + path);
    }
    Check(context.guard.Verify());
}

void VerifyEncodedSources(const TerrainMinimapAssetSnapshot& snapshot, const MapArchiveSession& source,
    const TerrainMinimapAssetCaptureLimits& limits, std::span<const MemberReplacement> replacements,
    std::span<const std::string> removals, std::size_t hashedBytes, bool exactCurrentWinner = false) {
    TerrainMinimapAssetCaptureFacts facts;
    facts.cumulativeSourceBytes = hashedBytes;
    std::vector<TerrainMinimapAssetDependency> dependencies;
    const auto& binding = snapshot.Binding();
    CaptureContext context{source, limits, facts, dependencies, binding.gameRoot, binding.mainTileset,
        replacements, removals, true};
    Check(context.guard.Watch(source.SourcePath()));
    context.Phase({"war3map.w3e"}, binding.originalTerrainBytes, TerrainMinimapDependencyKind::Table);
    const auto& original = context.mapMembers.front();
    if (!original.present || original.bytes.size() != binding.originalTerrainBytes
        || Take(Sha256(original.bytes)) != binding.originalTerrainSha256)
        Fail("TERRAIN_ASSETS_FINAL_SOURCE", "实际源W3E与冻结原版身份不同。");
    Check(context.guard.Verify());
    VerifyDependencyPhase(context, snapshot, false, false, exactCurrentWinner);
    VerifyDependencyPhase(context, snapshot, true, false, exactCurrentWinner);
    Check(context.guard.Verify());
}

} // namespace

Result<TerrainMinimapAssetSnapshot> CaptureTerrainMinimapAssets(const MapArchiveSession& source,
    const MapData::MapDataEditSession& edits, const std::filesystem::path& gameRoot,
    const TerrainMinimapAssetCaptureLimits& limits, TerrainMinimapRangePolicy rangePolicy,
    std::span<const MemberReplacement> replacements, std::span<const std::string> removals) {
    try {
        ValidateLimits(limits);
        if (rangePolicy != TerrainMinimapRangePolicy::FullW3e && rangePolicy != TerrainMinimapRangePolicy::DeclaredW3iMargins
            && rangePolicy != TerrainMinimapRangePolicy::DeclaredW3iMarginsExteriorMapEdge
            && rangePolicy != TerrainMinimapRangePolicy::ClassicPlayable)
            Fail("TERRAIN_ASSETS_RANGE_POLICY", "未知的小地图范围政策。");
        TerrainMinimapAssetSnapshot result;
        result.captureLimits_ = limits;
        result.input_.rangePolicy = rangePolicy;
        auto& binding = result.binding_; auto& facts = result.facts_;
        binding.archiveSha256 = source.SourceSha256(); binding.sessionIdentity = edits.SessionIdentity();
        binding.revision = edits.Revision(); binding.terrainSourceRevision = edits.TerrainSourceRevision(); binding.gameRoot = GameRoot(gameRoot);
        auto terrain = edits.Terrain();
        if (!terrain || terrain->header.version != 11 || terrain->header.groundTilesets.empty() || terrain->header.groundTilesets.size() > 16)
            Fail("TERRAIN_ASSETS_TERRAIN", "捕获需要真实编辑会话的classic v11与1..16个地面材质槽。");
        if (rangePolicy != TerrainMinimapRangePolicy::ClassicPlayable
            && (terrain->tiles.size() > limits.maxTiles || terrain->vertices.size() > 2 * limits.maxTiles + 2))
            Fail("TERRAIN_ASSETS_BUDGET", "当前地形超过捕获网格预算。");
        if (rangePolicy == TerrainMinimapRangePolicy::ClassicPlayable) {
            const auto& scan = limits.classicScan;
            // 当前会话已经拥有完整网格；这里只借用它并冻结原字，不再解析第二份网格。
            // 全图水标志扫描与四角悬崖扫描预收同一累计上界，不按材质/行重新领预算。
            if (terrain->vertices.size() > scan.maxVertexDecodes || terrain->tiles.size() > scan.maxTileAnchors
                || terrain->vertices.size() > scan.maxWorkUnits
                || terrain->tiles.size() > (scan.maxWorkUnits - terrain->vertices.size()) / 4)
                Fail("TERRAIN_ASSETS_BUDGET", "Classic完整输入与累计地形查询超过行扫描预算。");
        }
        binding.mainTileset = terrain->header.mainTileset;
        {
            // 此处只取得真实基线关系。SaveGate可失败，不能因此授予/改变正式保存资格。
            if (rangePolicy == TerrainMinimapRangePolicy::FullW3e) {
                const std::array<std::string, 1> names{"war3map.w3e"};
                ArchiveExactMemberLimits captureLimits;
                captureLimits.memberCount = names.size(); captureLimits.memberBytes = limits.maxSourceBytes;
                captureLimits.totalBytes = limits.maxSourceBytes;
                const auto identities = Take(source.DescribeExactMemberIdentities(source.SourceSha256(), names, captureLimits));
                if (!identities.members[0].present || !identities.members[0].sha256 || !identities.members[0].byteSize)
                    Fail("TERRAIN_ASSETS_BASELINE", "源W3E缺少准确成员身份。");
                facts.exactMemberCaptureBatches = 1;
                facts.baselineSourceBytes = *identities.members[0].byteSize;
                facts.cumulativeSourceBytes = facts.baselineSourceBytes;
                binding.originalTerrainSha256 = *identities.members[0].sha256;
                binding.originalTerrainBytes = *identities.members[0].byteSize;
                const auto originalBytes = Take(source.ReadMember(names[0]));
                Check(edits.VerifyTerrainSourceBaseline(originalBytes));
            } else {
                const auto plan = Take(MapData::TerrainSavePlan::Prepare(source, edits, {limits.maxSourceBytes}));
                facts.exactMemberCaptureBatches = 1;
                for (const auto& member : plan.Members()) if (member.sourceBytes) Add(facts.baselineSourceBytes, *member.sourceBytes, limits.maxSourceBytes);
                facts.cumulativeSourceBytes = facts.baselineSourceBytes;
                if (!plan.Members()[0].sourceSha256) Fail("TERRAIN_ASSETS_BASELINE", "源W3E缺少准确成员身份。");
                binding.originalTerrainSha256 = *plan.Members()[0].sourceSha256;
                binding.originalTerrainBytes = *plan.Members()[0].sourceBytes;
            }
        }
        const auto serializedBytes = 37 + (terrain->header.groundTilesets.size() + terrain->header.cliffTilesets.size()) * 4
            + terrain->vertices.size() * 7;
        if (serializedBytes > limits.maxCurrentW3eBytes) Fail("TERRAIN_ASSETS_BUDGET", "当前W3E序列化前字节估算超过捕获预算。");
        if (rangePolicy == TerrainMinimapRangePolicy::ClassicPlayable && serializedBytes > limits.classicScan.maxInputBytes)
            Fail("TERRAIN_ASSETS_BUDGET", "Classic当前W3E超过独立行扫描输入字节预算。");
        result.input_.currentW3e = Take(MapData::SerializeMapTerrain(*terrain));
        if (result.input_.currentW3e.size() > limits.maxCurrentW3eBytes) Fail("TERRAIN_ASSETS_BUDGET", "当前W3E超过捕获字节预算。");
        binding.currentTerrainSha256 = Take(Sha256(result.input_.currentW3e));
        result.input_.binding = {"terrain-session:" + std::to_string(binding.sessionIdentity), binding.sessionIdentity,
            binding.revision, binding.archiveSha256, binding.currentTerrainSha256};
        CaptureContext context{source, limits, facts, result.dependencies_, binding.gameRoot, terrain->header.mainTileset,
            replacements, removals};
        Check(context.guard.Watch(source.SourcePath()));
        std::vector<Diagnostic> diagnostics;
        std::vector<std::string> texturePaths;
        std::vector<std::string> cliffPaths;
        {
            const std::vector<std::string> tablePaths{"TerrainArt\\Terrain.slk", "TerrainArt\\CliffTypes.slk", "TerrainArt\\Water.slk"};
            auto exactPaths = tablePaths;
            if (rangePolicy != TerrainMinimapRangePolicy::FullW3e) exactPaths.emplace_back("war3map.w3i");
            if (rangePolicy == TerrainMinimapRangePolicy::ClassicPlayable) {
                exactPaths.emplace_back("UI\\MiscData.txt");
                exactPaths.emplace_back("UI\\WorldEditData.txt");
            }
            // 四名共用原三表member cap；绝不为W3I抬高调用方给表设定的读取上限。
            context.Phase(exactPaths, limits.tables.maxTableBytes, TerrainMinimapDependencyKind::Table);
            if (rangePolicy != TerrainMinimapRangePolicy::FullW3e) {
                const auto path = Canonical("war3map.w3i");
                auto& member = context.mapMembers.at(context.mapIndices.at(path));
                if (!member.present) Fail("TERRAIN_ASSETS_W3I_MISSING", "地图准确捕获证明缺少W3I；禁止回退game。");
                if (member.bytes.empty()) Fail("TERRAIN_ASSETS_W3I_EMPTY", "源W3I是真实空成员，不能用于声明边距。");
                Check(MapData::ValidateTerrainSettingsSourceBaseline(edits, member.present, member.bytes));
                const MemberReplacement* finalSettings = nullptr;
                for (const auto& replacement : replacements) {
                    const auto key = Resources::CanonicalImportPath(replacement.name);
                    if (!key.Succeeded() || !key.value || *key.value != path) continue;
                    if (finalSettings) Fail("TERRAIN_ASSETS_W3I_CURRENT", "最终保存列表重复覆盖W3I。");
                    finalSettings = &replacement;
                }
                for (const auto& removal : removals) {
                    const auto key = Resources::CanonicalImportPath(removal);
                    if (key.Succeeded() && key.value && *key.value == path)
                        Fail("TERRAIN_ASSETS_FINAL_REMOVAL", "小地图不能消费本次保存已删除的W3I。");
                }
                const auto currentSettingsBytes = Take(edits.SnapshotByteSize(MapDataKind::Settings));
                if (currentSettingsBytes > limits.tables.maxTableBytes
                    || (finalSettings && finalSettings->data.size() > limits.tables.maxTableBytes))
                    Fail("TERRAIN_ASSETS_BUDGET", "当前W3I超过同次设置成员预算。");
                // Snapshot内部也解析W3I；先计读入原字、当前副本和有界解析峰值，
                // 在atlas解码前销毁MapDocument，Candidate不会同时保活第二次解析。
                facts.estimatedSettingsWorkingBytes = facts.cumulativeSourceBytes;
                Add(facts.estimatedSettingsWorkingBytes, result.input_.currentW3e.capacity(), limits.maxSourceBytes);
                Add(facts.estimatedSettingsWorkingBytes, member.bytes.capacity(), limits.maxSourceBytes);
                // 最终正文可以比源正文更长（例如改地图名），输出副本与当前解析暂时重叠。
                if (finalSettings && finalSettings->data != member.bytes)
                    Add(facts.estimatedSettingsWorkingBytes, finalSettings->data.size(), limits.maxSourceBytes);
                Add(facts.estimatedSettingsWorkingBytes, TerrainMinimapMemoryFacts::settingsScratchFixedBytes, limits.maxSourceBytes);
                const auto remaining = limits.maxSourceBytes - facts.estimatedSettingsWorkingBytes;
                if (currentSettingsBytes > remaining / TerrainMinimapMemoryFacts::settingsScratchBytesPerSourceByte)
                    Fail("TERRAIN_ASSETS_BUDGET", "W3I当前快照与解析预留超过现有累计硬预算。");
                Add(facts.estimatedSettingsWorkingBytes, currentSettingsBytes * TerrainMinimapMemoryFacts::settingsScratchBytesPerSourceByte,
                    limits.maxSourceBytes);
                {
                    const auto current = Take(edits.Snapshot(MapDataKind::Settings, currentSettingsBytes));
                    const auto& settingsMember = current.Member(MapDataKind::Settings);
                    const auto& currentBytes = finalSettings ? finalSettings->data : member.bytes;
                    if (settingsMember.state != MapDataState::Ready || settingsMember.memberName != "war3map.w3i"
                        || settingsMember.sources.size() != 1 || settingsMember.sources.front().data != currentBytes)
                        Fail("TERRAIN_ASSETS_W3I_CURRENT", "当前W3I必须Ready、唯一且逐字等于本次最终输出；缺失或不一致不能继续保存。");
                    if (!current.Settings() || current.Settings()->formatVersion != 25 || !current.Settings()->FullyParsed())
                        Fail("TERRAIN_ASSETS_W3I_LAYOUT", "小地图可玩范围仅消费完整classic v25最终W3I。");
                }
                const bool settingsChanged = finalSettings && finalSettings->data != member.bytes;
                if (settingsChanged) {
                    Add(facts.cumulativeSourceBytes, finalSettings->data.size(), limits.maxSourceBytes);
                    Add(facts.mapDependencyBytes, finalSettings->data.size(), limits.maxSourceBytes);
                    member.bytes = finalSettings->data;
                }
                // 直接消费map-only exact member；绝不向支持game fallback的resolver请求W3I。
                context.Record(path, member.bytes, true, source.SourceSha256(), settingsChanged);
                result.dependencies_[context.dependencyIndices.at(path)].kind = TerrainMinimapDependencyKind::Settings;
                facts.retainedW3iCapacityBytes = member.bytes.capacity();
                result.input_.sourceW3i = std::move(member.bytes);
            }
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
            if (rangePolicy == TerrainMinimapRangePolicy::ClassicPlayable) {
                auto& colors = result.input_.classicColors.emplace();
                const auto configuration = MinimapConfiguration(context);
                constexpr std::array<const char*, 4> margins{"left", "right", "bottom", "top"};
                for (std::size_t i = 0; i < margins.size(); ++i)
                    colors.cameraMargins[i] = ConfigurationNumber(configuration, "cameramargins", margins[i]);
                colors.cameraMarginsConfigured = true;
                if (std::any_of(terrain->vertices.begin(), terrain->vertices.end(), [](const auto& v) { return v.HasWater(); })) {
                    const auto id = std::string(1, static_cast<char>(terrain->header.mainTileset)) + "Sha";
                    const auto row = catalog.water.find(id);
                    if (row == catalog.water.end()) Fail("CLASSIC_MINIMAP_WATER_MISSING", "Water.slk缺少当前地形集的浅水行。");
                    colors.waterColor = row->second.minimapColor;
                    colors.waterHeightOffset = row->second.heightOffsetWorld;
                    colors.deepLevelPathing = ConfigurationNumber(configuration, "water", "deeplevelpathing");
                    colors.waterConfigured = true;
                }
                const bool hasCliffs = std::any_of(terrain->tiles.begin(), terrain->tiles.end(), [&](const auto& tile) {
                    const auto level = terrain->vertices.at(tile.vertices[0]).CliffLevel();
                    return std::any_of(tile.vertices.begin() + 1, tile.vertices.end(), [&](const auto index) {
                        return terrain->vertices.at(index).CliffLevel() != level;
                    });
                });
                for (const auto& id : terrain->header.cliffTilesets) {
                    if (!hasCliffs) { cliffPaths.emplace_back(); continue; }
                    const auto row = catalog.cliffs.find(id);
                    if (row == catalog.cliffs.end()) Fail("CLASSIC_MINIMAP_CLIFF_MISSING", "CliffTypes.slk缺少当前悬崖类型。");
                    const auto path = Canonical(row->second.declaredTexturePath);
                    cliffPaths.push_back(path);
                    if (unique.emplace(path, texturePaths.size()).second) texturePaths.push_back(path);
                }
            }
        } // 释放三表原字节与catalog后才捕获/解码atlas，避免长期保留重复表解析中间体。
        Check(context.guard.Verify());
        context.Phase(texturePaths, limits.atlas.maxInputBytes, TerrainMinimapDependencyKind::Atlas);
        std::map<std::string, std::shared_ptr<const TerrainAtlasSnapshot>, std::less<>> atlases;
        for (const auto& path : texturePaths) {
            auto resource = Take(context.resolver.Read(path));
            const bool groundAtlas = std::any_of(result.ground_.begin(), result.ground_.end(),
                [&](const auto& ground) { return ground.canonicalTexturePath == path; });
            if (!groundAtlas) {
                const auto& configured = limits.atlas;
                Image::BlpMipChainLimits decodeLimits{configured.maxInputBytes, configured.maxDimension,
                    configured.maxRgbaBytes, configured.maxRetainedRgbaBytes, configured.maxTotalMipPixels,
                    configured.maxWorkingBytes, configured.maxJpegSegments, configured.maxJpegScans};
                decodeLimits.maxRetainedRgbaBytes = std::min(decodeLimits.maxRetainedRgbaBytes,
                    limits.maxRetainedAtlasCapacityBytes - facts.retainedAtlasRgbaCapacityBytes);
                auto decoded = Take(Image::DecodeBlpMipChain(resource.bytes, decodeLimits));
                const auto color = Take(ComputeTerrainMinimapRepresentativeColor(decoded.image, TerrainMinimapTextureRole::Cliff)).rgba;
                const auto index = context.dependencyIndices.at(path);
                result.dependencies_[index].decodedMips.push_back({decoded.image.width, decoded.image.height,
                    decoded.image.rgba.size(), decoded.image.rgba.capacity(), Take(Sha256(decoded.image.rgba))});
                for (std::size_t i = 0; i < cliffPaths.size(); ++i) if (cliffPaths[i] == path) {
                    auto& colors = result.input_.classicColors->cliffColors;
                    if (colors.size() < cliffPaths.size()) colors.resize(cliffPaths.size());
                    colors[i] = color;
                }
                continue;
            }
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
        if (result.input_.classicColors) {
            auto& colors = result.input_.classicColors->cliffColors;
            colors.resize(cliffPaths.size());
            for (std::size_t i = 0; i < cliffPaths.size(); ++i) if (const auto atlas = atlases.find(cliffPaths[i]); atlas != atlases.end())
                colors[i] = Take(ComputeTerrainMinimapRepresentativeColor(atlas->second->image, TerrainMinimapTextureRole::Cliff)).rgba;
        }
        Check(context.guard.Verify());
        // 不额外VerifySourceUnchanged重复散列整图：最后一批精确捕获已核SHA/租约，
        // 本次guard仍持有地图路径拒写租约并在上面核身份/路径；再核编辑修订。
        if (!Current(source, edits, binding) || edits.Terrain() != terrain)
            Fail("TERRAIN_ASSETS_STALE", "捕获期间实际会话、修订或地形来源已变化。");
        result.identitySha256_ = SnapshotIdentity(result);
        result.captureDiagnostics_ = diagnostics;
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
    const MapData::MapDataEditSession& edits, const std::filesystem::path& gameRoot,
    const TerrainMinimapAssetCaptureLimits& limits, std::span<const MemberReplacement> replacements,
    std::span<const std::string> removals) {
    try {
        ValidateLimits(limits);
        if (!Current(source, edits, snapshot.Binding()) || GameRoot(gameRoot) != snapshot.Binding().gameRoot)
            return Status::Failure(kSubsystem, "TERRAIN_ASSETS_STALE", "资源快照不属于当前来源或编辑修订。");
        if (SnapshotIdentity(snapshot) != snapshot.IdentitySha256())
            Fail("TERRAIN_ASSETS_FINAL_SNAPSHOT", "当前复核需要原工厂冻结的真实快照身份。");
        const auto selected = SelectFinalMembers(snapshot, replacements, removals);
        const auto recapture = [&]() -> Status {
            auto current = CaptureTerrainMinimapAssets(source, edits, gameRoot, limits, snapshot.Input().rangePolicy,
                replacements, removals);
            if (!current.Succeeded()) return {std::move(current.diagnostics)};
            if (current.value->IdentitySha256() != snapshot.IdentitySha256())
                return Status::Failure(kSubsystem, "TERRAIN_ASSETS_STALE", "实际winner、编码字节或解码mip身份已变化。");
            // 原重捕获从live会话取W3E，不消费列表中的W3E；补核显式提供的输出。
            // 单独沿原current-W3E字节帽预收，避免在收紧预算失败前散列任意载荷。
            for (const auto& [path, member] : selected) if (!member.dependency && member.replacement) {
                const auto& bytes = member.replacement->data;
                if (bytes.size() > limits.maxCurrentW3eBytes)
                    Fail("TERRAIN_ASSETS_BUDGET", "当前W3E输出超过本次字节预算。");
                if (bytes.size() != snapshot.Input().currentW3e.size()
                    || Take(Sha256(bytes)) != snapshot.Binding().currentTerrainSha256)
                    Fail("TERRAIN_ASSETS_FINAL_BYTES", "最终W3E正文不属于当前地形修订。");
            }
            return {std::move(current.diagnostics)};
        };
        // 限额会决定可验证的原始mip前缀；任一变化均走原检查，不凭RGBA反推工作峰值。
        if (!SameCaptureLimits(limits, snapshot.captureLimits_)) return recapture();
        std::size_t hashedBytes = 0;
        try { hashedBytes = PrechargeFinal(snapshot, selected, limits); }
        catch (Failure& failure) {
            // 原捕获的计费不含额外冻结输出散列；精确预算边界继续用原有路径，不能误拒绝。
            if (failure.diagnostics.size() == 1 && failure.diagnostics.front().code == "TERRAIN_ASSETS_BUDGET")
                return recapture();
            throw;
        }
        // Current也可用于尚未形成写盘列表的预览；已提供的W3E仍须精确匹配当前修订。
        VerifyFinalBytes(snapshot, selected, true);
        VerifyEncodedSources(snapshot, source, limits, replacements, removals, hashedBytes, true);
        if (!Current(source, edits, snapshot.Binding()) || GameRoot(gameRoot) != snapshot.Binding().gameRoot)
            Fail("TERRAIN_ASSETS_STALE", "当前依赖复核期间编辑来源或修订已改变。");
        return {snapshot.captureDiagnostics_};
    } catch (Failure& failure) { return {std::move(failure.diagnostics)}; }
    catch (const std::bad_alloc&) {
        return Status::Failure(kSubsystem, "TERRAIN_ASSETS_ALLOCATION", "当前依赖核验分配失败，不返回部分通过。");
    } catch (const std::length_error&) {
        return Status::Failure(kSubsystem, "TERRAIN_ASSETS_ALLOCATION", "当前依赖核验超出容器容量，不返回部分通过。");
    }
}

Status VerifyTerrainMinimapAssetsFrozenOutput(const TerrainMinimapAssetSnapshot& snapshot, const MapArchiveSession& source,
    std::span<const MemberReplacement> replacements, std::span<const std::string> removals,
    const TerrainMinimapAssetCaptureLimits& limits) {
    try {
        ValidateLimits(limits);
        const auto& binding = snapshot.Binding();
        if (source.SourceSha256() != binding.archiveSha256 || SnapshotIdentity(snapshot) != snapshot.IdentitySha256())
            Fail("TERRAIN_ASSETS_FINAL_SOURCE", "最终依赖核验需要原工厂冻结的同一真实来源和快照身份。");
        const auto selected = SelectFinalMembers(snapshot, replacements, removals);
        const auto hashedBytes = PrechargeFinal(snapshot, selected, limits);
        VerifyFinalBytes(snapshot, selected);
        VerifyEncodedSources(snapshot, source, limits, replacements, removals, hashedBytes);
        return Status::Ok();
    } catch (Failure& failure) { return {std::move(failure.diagnostics)}; }
    catch (const std::bad_alloc&) {
        return Status::Failure(kSubsystem, "TERRAIN_ASSETS_ALLOCATION", "最终依赖核验分配失败，不返回部分通过。");
    } catch (const std::length_error&) {
        return Status::Failure(kSubsystem, "TERRAIN_ASSETS_ALLOCATION", "最终依赖核验超出容器容量，不返回部分通过。");
    }
}

Status VerifyTerrainMinimapAssetsReadback(const TerrainMinimapAssetSnapshot& snapshot, const MapArchiveSession& saved,
    const TerrainMinimapAssetCaptureLimits& limits) {
    try {
        ValidateLimits(limits);
        if (SnapshotIdentity(snapshot) != snapshot.IdentitySha256())
            Fail("TERRAIN_ASSETS_FINAL_SNAPSHOT", "回读需要原工厂冻结的真实快照身份。");
        const auto terrainBytes = snapshot.Input().currentW3e.size();
        static_cast<void>(PrechargeInputs(snapshot, terrainBytes, limits));
        VerifyCurrentTerrainBytes(snapshot);
        TerrainMinimapAssetCaptureFacts facts;
        facts.cumulativeSourceBytes = terrainBytes; // 上述owned current实际散列也计原工作字节账。
        std::vector<TerrainMinimapAssetDependency> dependencies;
        const auto& binding = snapshot.Binding();
        CaptureContext context{saved, limits, facts, dependencies, binding.gameRoot, binding.mainTileset};
        Check(context.guard.Watch(saved.SourcePath()));
        context.Phase({"war3map.w3e"}, terrainBytes, TerrainMinimapDependencyKind::Table);
        const auto& terrain = context.mapMembers.front();
        if (!terrain.present || terrain.bytes.size() != terrainBytes || Take(Sha256(terrain.bytes)) != binding.currentTerrainSha256)
            Fail("TERRAIN_ASSETS_READBACK_TERRAIN", "实际输出W3E与冻结当前修订不同。");
        Check(context.guard.Verify());
        VerifyDependencyPhase(context, snapshot, false, true);
        VerifyDependencyPhase(context, snapshot, true, true);
        Check(context.guard.Verify());
        return Status::Ok();
    } catch (Failure& failure) { return {std::move(failure.diagnostics)}; }
    catch (const std::bad_alloc&) {
        return Status::Failure(kSubsystem, "TERRAIN_ASSETS_ALLOCATION", "输出依赖回读分配失败，不返回部分通过。");
    } catch (const std::length_error&) {
        return Status::Failure(kSubsystem, "TERRAIN_ASSETS_ALLOCATION", "输出依赖回读超出容器容量，不返回部分通过。");
    }
}

} // namespace War3Map::Render
