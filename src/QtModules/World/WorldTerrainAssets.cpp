#include "WorldTerrainAssets.h"
#include "EditorProject.h"
#include "War3MapCore/Game/GameDataSource.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/Render/AnimationSourceGuard.h"
#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/Render/TerrainMaterials.h"
#include "War3MapCore/Resources/ImportTable.h"
#include "War3MapCore/Text/TextUtil.h"
#include "War3MapCore/VerifiedFile.h"

#include <Windows.h>
#include <algorithm>
#include <chrono>
#include <map>

namespace War3Map::Modules {
namespace {
constexpr std::size_t kResourceLimit = 16 * 1024 * 1024;
struct ReadLease {
    HANDLE handle = INVALID_HANDLE_VALUE;
    BY_HANDLE_FILE_INFORMATION identity{};
    explicit ReadLease(const std::filesystem::path& path)
        // 世界视图会缓存资源读取器；允许经过校验的保存流程 rename 替换来源路径，
        // 现有句柄仍锚定旧文件，MapDocumentChanged 后整张地形重新装载。
        : handle(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)) {
        if (handle != INVALID_HANDLE_VALUE && (!GetFileInformationByHandle(handle, &identity)
            || (identity.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))) {
            CloseHandle(handle); handle = INVALID_HANDLE_VALUE;
        }
    }
    ~ReadLease() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
    ReadLease(const ReadLease&) = delete;
    ReadLease& operator=(const ReadLease&) = delete;
};
bool SameFile(const ReadLease& a, const ReadLease& b) noexcept {
    if (a.handle == INVALID_HANDLE_VALUE || b.handle == INVALID_HANDLE_VALUE) return false;
    const auto& first = a.identity; const auto& second = b.identity;
    return first.dwVolumeSerialNumber == second.dwVolumeSerialNumber
        && first.nFileIndexHigh == second.nFileIndexHigh && first.nFileIndexLow == second.nFileIndexLow
        && first.nFileSizeHigh == second.nFileSizeHigh && first.nFileSizeLow == second.nFileSizeLow
        && CompareFileTime(&first.ftLastWriteTime, &second.ftLastWriteTime) == 0;
}
std::uint64_t ResourceStampNs() noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
Result<std::vector<std::byte>> ReadDisk(const std::filesystem::path& path) {
    ReadLease lease(path);
    LARGE_INTEGER size{};
    if (lease.handle == INVALID_HANDLE_VALUE || !GetFileSizeEx(lease.handle, &size) ||
        size.QuadPart < 0 || size.QuadPart > kResourceLimit)
        return Result<std::vector<std::byte>>::Failure("World", "WORLD_ASSET_READ", "导入地形资源无法锁定或超过 16 MiB。");
    const auto hash = Sha256Handle(lease.handle);
    if (!hash.Succeeded()) return {std::nullopt, hash.diagnostics};
    return ReadVerifiedFile(path, *hash.value, kResourceLimit);
}

Result<std::string> BlightTexturePath(Resources::ResourceResolver& resolver, std::uint8_t mainTileset) {
    auto table = resolver.Read("UI\\WorldEditData.txt");
    if (!table.Succeeded()) return {std::nullopt, std::move(table.diagnostics)};
    if (table.value->bytes.size() > 2 * 1024 * 1024)
        return Result<std::string>::Failure("World", "WORLD_BLIGHT_TABLE_BUDGET", "WorldEditData 超过 2 MiB。");
    std::string_view text(reinterpret_cast<const char*>(table.value->bytes.data()), table.value->bytes.size());
    if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
    const auto sections = Text::ParseProfile(text);
    const auto section = sections.find("tilesets");
    const auto key = Text::LowerAscii(std::string(1, static_cast<char>(mainTileset)));
    const auto entry = section == sections.end() ? nullptr : [&]() -> const std::string* {
        const auto found = section->second.find(key);
        return found == section->second.end() ? nullptr : &found->second;
    }();
    if (!entry)
        return Result<std::string>::Failure("World", "WORLD_BLIGHT_TILESET_UNDEFINED", "WorldEditData 未定义当前主地形的荒芜材质。");
    const auto fields = Text::SplitPlain(*entry, ',');
    if (fields.size() != 2 || Text::TrimCWhitespace(fields[0]).empty() || Text::TrimCWhitespace(fields[1]).empty())
        return Result<std::string>::Failure("World", "WORLD_BLIGHT_TILESET_INVALID", "WorldEditData 的荒芜材质路径格式无效。");
    std::string path(Text::TrimCWhitespace(fields[1]));
    if (!Text::LowerAscii(path).ends_with(".blp")) path += ".blp";
    auto canonical = Resources::CanonicalImportPath(path);
    if (!canonical.Succeeded()) return {std::nullopt, std::move(canonical.diagnostics)};
    return Result<std::string>::Success(std::move(*canonical.value));
}

bool HasBlightMaskAlpha(const Render::TerrainAtlasSnapshot& atlas) {
    if (atlas.columns != 8 || !atlas.image.Valid()) return false;
    // 仅 1–14 是局部遮罩；每格必须同时有透明与不透明像素。否则 alpha 混合会
    // 将底下的原地表整块盖住，不能仅凭 8×4 尺寸将任意 BLP 当作荒芜图集。
    for (std::uint32_t mask = 1; mask < 15; ++mask) {
        const auto* cell = atlas.Cell(mask % 4, mask / 4);
        if (!cell || cell->width == 0 || cell->height == 0) return false;
        bool transparent = false, opaque = false;
        for (std::uint32_t y = cell->y; y < cell->y + cell->height; ++y) {
            for (std::uint32_t x = cell->x; x < cell->x + cell->width; ++x) {
                const auto alpha = std::to_integer<std::uint8_t>(
                    atlas.image.rgba[(static_cast<std::size_t>(y) * atlas.image.width + x) * 4 + 3]);
                transparent |= alpha < 128;
                opaque |= alpha >= 128;
            }
        }
        if (!transparent || !opaque) return false;
    }
    return true;
}
} // namespace

bool SameWorldTerrainSources(const WorldTerrainSources& a, const WorldTerrainSources& b) {
    return a.archivePath == b.archivePath && a.archiveSha == b.archiveSha && a.gameRoot == b.gameRoot
        && a.mainTileset == b.mainTileset && a.entries.size() == b.entries.size()
        && std::equal(a.entries.begin(), a.entries.end(), b.entries.begin(), [](const auto& x, const auto& y) {
            return x.path == y.path && x.archivePath == y.archivePath && x.diskPath == y.diskPath;
        });
}

Result<WorldTerrainSources> CaptureWorldTerrainSources(const QtProject::EditorProject& project, std::size_t maxEntries) {
    if (!project.archiveSession_) return Result<WorldTerrainSources>::Failure("World", "WORLD_ASSET_SOURCE",
        "目录工程地形资源来源尚未接入；当前显示高度预览。");
    WorldTerrainSources result;
    result.archivePath = project.archiveSession_->SourcePath();
    result.archiveSha = project.archiveSession_->SourceSha256();
    result.gameRoot = project.war3GameRoot_;
    if (project.mapEdits_) {
        if (const auto terrain = project.mapEdits_->Terrain()) result.mainTileset = terrain->header.mainTileset;
    }
    const auto entries = project.AssetEntries(maxEntries == std::numeric_limits<std::size_t>::max() ? maxEntries : maxEntries + 1);
    if (entries.size() > maxEntries)
        return Result<WorldTerrainSources>::Failure("World", "WORLD_ASSET_METADATA_BUDGET",
            "资源覆盖清单超过查询预算；未截断后回退到游戏资源。");
    for (const auto& entry : entries)
        result.entries.push_back({entry.path.toStdString(), entry.originalPath.toStdString(), entry.diskPath.toStdWString()});
    return Result<WorldTerrainSources>::Success(std::move(result));
}

// 后台线程独占：地图只读租约 + 归档 + 导入快照 + 游戏包，按“会话导入 → 地图 → 游戏”解析。
struct WorldResourceReader::State {
    WorldTerrainSources sources;
    std::optional<ReadLease> lease;
    std::optional<Mpq::MpqArchive> archive;
    std::map<std::string, WorldAssetEntry, std::less<>> entries;
    std::optional<Game::GameDataSource> game;
    Resources::ResourceResolver resolver;
    Render::AnimationSourceGuard animationGuard;
    bool animationReads = false;
};

WorldResourceReader::WorldResourceReader() : state_(std::make_unique<State>()) {}
WorldResourceReader::~WorldResourceReader() = default;
Resources::ResourceResolver& WorldResourceReader::Resolver() noexcept { return state_->resolver; }
bool WorldResourceReader::MatchesSources(const WorldTerrainSources& sources) const {
    return SameWorldTerrainSources(state_->sources, sources);
}

Status WorldResourceReader::VerifyCurrentSource() const {
    ReadLease current(state_->sources.archivePath);
    if (current.handle == INVALID_HANDLE_VALUE || !state_->lease)
        return Status::Failure("World", "WORLD_ASSET_SOURCE_CHANGED", "地图资源来源已不可验证，请重新加载地形。");
    return SameFile(*state_->lease, current) ? state_->animationGuard.Verify() : Status::Failure("World", "WORLD_ASSET_SOURCE_CHANGED",
        "地图资源来源已被替换，请重新加载地形。");
}

Result<Render::AnimationResource> WorldResourceReader::ReadAnimationResource(std::string_view path, std::uint64_t maxBytes) {
    using Output = Result<Render::AnimationResource>;
    if (maxBytes > kResourceLimit) maxBytes = kResourceLimit;
    state_->animationReads = true;
    const auto location = state_->resolver.Locate(path);
    if (!location.Succeeded()) return {std::nullopt, location.diagnostics};
    std::vector<std::byte> bytes;
    std::string layer;
    if (location.value->layer == Resources::ResourceLayer::Game) {
        if (!state_->game) return Output::Failure("World", "WORLD_GAME_SOURCE_MISSING", "游戏资源未挂载。");
        auto read = state_->game->ReadFileBoundedWithSource(location.value->sourcePath, static_cast<std::size_t>(maxBytes));
        if (!read.Succeeded()) return {std::nullopt, std::move(read.diagnostics)};
        bytes = std::move(read.value->bytes);
        const auto root = state_->sources.gameRoot.generic_u8string();
        layer = "game:" + std::string(reinterpret_cast<const char*>(root.data()), root.size()) + "|" + read.value->archiveName;
    } else {
        const auto diskEntry = state_->entries.find(location.value->canonicalPath);
        if (diskEntry != state_->entries.end() && !diskEntry->second.diskPath.empty()) {
            auto guard = state_->animationGuard.Watch(diskEntry->second.diskPath);
            if (!guard.Succeeded()) return {std::nullopt, std::move(guard.diagnostics)};
        }
        auto read = state_->resolver.Read(path);
        if (!read.Succeeded()) return {std::nullopt, std::move(read.diagnostics)};
        if (read.value->bytes.size() > maxBytes)
            return Output::Failure("World", "WORLD_ANIMATION_RESOURCE_BUDGET", "动画资源超过读取预算。");
        bytes = std::move(read.value->bytes);
        layer = read.value->location.sourceId;
        const auto entry = state_->entries.find(location.value->canonicalPath);
        if (entry != state_->entries.end() && !entry->second.diskPath.empty()) {
            const auto disk = entry->second.diskPath.generic_u8string();
            layer += "|session:" + std::string(reinterpret_cast<const char*>(disk.data()), disk.size());
        } else layer += "|map-archive";
    }
    auto current = state_->animationGuard.Verify();
    if (!current.Succeeded()) return {std::nullopt, std::move(current.diagnostics)};
    auto digest = Sha256(bytes);
    if (!digest.Succeeded()) return {std::nullopt, std::move(digest.diagnostics)};
    return Output::Success({std::move(bytes), std::move(layer), std::move(*digest.value)});
}

Result<std::unique_ptr<WorldResourceReader>> WorldResourceReader::Open(const WorldTerrainSources& sources,
    const WorldResourceReader* verifiedSource, OpenTimings* timings) {
    using Reader = std::unique_ptr<WorldResourceReader>;
    using namespace Resources;
    if (timings) *timings = {};
    const auto openStarted = ResourceStampNs();
    struct FinishTiming {
        OpenTimings* output;
        std::uint64_t start;
        ~FinishTiming() { if (output) output->totalNs = ResourceStampNs() - start; }
    } finish{timings, openStarted};
    const auto fail = [](std::string code, std::string message) { return Result<Reader>::Failure("World", std::move(code), std::move(message)); };
    Reader reader(new WorldResourceReader);
    auto& state = *reader->state_;
    state.sources = sources;
    state.lease.emplace(sources.archivePath);
    if (state.lease->handle == INVALID_HANDLE_VALUE) return fail("WORLD_ASSET_SOURCE", "地图来源无法取得只读资源租约。");
    if (verifiedSource) {
        // 两份GENERIC_READ租约都不共享写访问：原文件从完整SHA验证到新租约取得始终
        // 禁止内容写入。比较冻结身份而不是路径存在，rename替换即便字节相同也拒绝。
        const auto& verified = *verifiedSource->state_;
        if (verified.sources.archiveSha != sources.archiveSha || !verified.lease
            || !SameFile(*verified.lease, *state.lease))
            return fail("WORLD_ASSET_SOURCE_CHANGED", "地图来源与已验证的只读租约不一致，请重新加载地形。");
        if (timings) timings->verifiedArchiveReused = true;
    } else {
        const auto hashStarted = ResourceStampNs();
        const auto hash = Sha256Handle(state.lease->handle);
        if (timings) {
            timings->hashNs = ResourceStampNs() - hashStarted;
            timings->hashedBytes = (std::uint64_t(state.lease->identity.nFileSizeHigh) << 32)
                | state.lease->identity.nFileSizeLow;
        }
        if (!hash.Succeeded()) return {std::nullopt, hash.diagnostics};
        if (*hash.value != sources.archiveSha) return fail("WORLD_ASSET_SOURCE_CHANGED", "地图来源已变化，请重新加载地形。");
    }
    const auto archiveStarted = ResourceStampNs();
    auto archive = Mpq::MpqArchive::OpenReadOnlyHandle(state.lease->handle, sources.archivePath);
    if (timings) timings->archiveNs = ResourceStampNs() - archiveStarted;
    if (!archive.Succeeded()) return {std::nullopt, archive.diagnostics};
    state.archive.emplace(std::move(*archive.value));
    for (const auto& entry : sources.entries) {
        const auto key = CanonicalImportPath(entry.path);
        if (!key.Succeeded()) continue; // 不相关的不可规范化名字不可能命中严格资源虚拟路径。
        const auto previous = state.entries.find(*key.value);
        if (previous == state.entries.end()) state.entries.emplace(*key.value, entry);
        else if (previous->second.diskPath.empty() != entry.diskPath.empty()) {
            // 与生产保存一致：会话待导入覆盖原归档（包括改名后的目标）。同层冲突仍拒绝。
            if (!entry.diskPath.empty()) previous->second = entry;
        } else return fail("WORLD_ASSET_CONFLICT", "地图资源同层存在大小写或路径冲突，地图资源未启用。");
    }
    State* shared = &state;
    ResourceSource mapSource;
    mapSource.id = "world-map-" + sources.archiveSha;
    mapSource.layer = ResourceLayer::MapImport;
    mapSource.priority = ResourcePriority::MapArchive;
    mapSource.probe = [shared](std::string_view path) {
        return Result<ResourceProbe>::Success(shared->entries.contains(path)
            ? ResourceProbe{ResourceProbeState::Found, std::string(path)} : ResourceProbe{});
    };
    mapSource.read = [shared](std::string_view path) {
        const auto found = shared->entries.find(path);
        if (found == shared->entries.end()) return Result<std::vector<std::byte>>::Failure("World", "WORLD_ASSET_MISSING", "资源快照内没有所请求的地图成员。");
        if (!found->second.diskPath.empty()) return ReadDisk(found->second.diskPath);
        return shared->archive->ReadFileBounded(found->second.archivePath, kResourceLimit);
    };
    if (const auto added = state.resolver.AddSource(std::move(mapSource)); !added.Succeeded()) return {std::nullopt, added.diagnostics};
    if (!sources.gameRoot.empty()) {
        ResourceSource gameSource;
        gameSource.id = "world-game-patch-local-expansion-base";
        if (sources.mainTileset) gameSource.id += "|tileset:" + std::string(1, static_cast<char>(*sources.mainTileset));
        gameSource.layer = ResourceLayer::Game;
        gameSource.priority = ResourcePriority::GameBase;
        gameSource.probe = [shared](std::string_view path) {
            // 只有高层未命中才访问游戏包；完整地图覆盖不受无关游戏目录故障影响。
            if (!shared->game) {
                // 嵌套包是父归档内的 owned 快照；静态贴图和动画都绑定父归档身份。
                auto guard = shared->animationGuard.WatchGameRoot(shared->sources.gameRoot);
                if (!guard.Succeeded()) return Result<ResourceProbe>{std::nullopt, std::move(guard.diagnostics)};
                auto mounted = shared->sources.mainTileset
                    ? Game::GameDataSource::MountStrict(shared->sources.gameRoot, *shared->sources.mainTileset)
                    : Game::GameDataSource::MountStrict(shared->sources.gameRoot);
                if (!mounted.Succeeded()) return Result<ResourceProbe>{std::nullopt, mounted.diagnostics};
                guard = shared->animationGuard.Verify();
                if (!guard.Succeeded()) return Result<ResourceProbe>{std::nullopt, std::move(guard.diagnostics)};
                shared->game = std::move(*mounted.value);
            }
            return Result<ResourceProbe>::Success(shared->game->HasFile(std::string(path))
                ? ResourceProbe{ResourceProbeState::Found, std::string(path)} : ResourceProbe{});
        };
        gameSource.read = [shared](std::string_view path) { return shared->game->ReadFileBounded(std::string(path), kResourceLimit); };
        if (const auto added = state.resolver.AddSource(std::move(gameSource)); !added.Succeeded()) return {std::nullopt, added.diagnostics};
    }
    return Result<Reader>::Success(std::move(reader));
}

Result<std::vector<std::shared_ptr<const Render::TerrainAtlasSnapshot>>> LoadWorldTerrainAtlases(
    const WorldTerrainSources& sources, const MapData::TerrainHeader& terrain,
    std::optional<Render::TerrainWaterStyle>* water,
    std::shared_ptr<const Render::TerrainAtlasSnapshot>* blight,
    std::optional<Render::WaterMaterialDefinition>* waterDefinition) {
    if (water) water->reset();
    if (blight) blight->reset();
    if (waterDefinition) waterDefinition->reset();
    auto opened = WorldResourceReader::Open(sources);
    if (!opened.Succeeded()) return {std::nullopt, opened.diagnostics};
    return LoadWorldTerrainAtlases((*opened.value)->Resolver(), terrain, water, blight, waterDefinition);
}

Result<std::vector<std::optional<Render::CliffTypeSource>>> LoadWorldCliffTypes(
    Resources::ResourceResolver& resolver, const MapData::TerrainHeader& terrain) {
    const auto catalog = Render::LoadTerrainMaterialCatalog(resolver);
    if (!catalog.Succeeded()) return {std::nullopt, catalog.diagnostics};
    return Render::ResolveTerrainCliffTypes(*catalog.value, terrain);
}

Result<WorldTerrainNames> LoadWorldTerrainNames(Resources::ResourceResolver& resolver, const MapData::TerrainHeader& terrain) {
    const auto catalog = Render::LoadTerrainMaterialCatalog(resolver);
    if (!catalog.Succeeded()) return {std::nullopt, catalog.diagnostics};
    const auto nameOf = [](const Render::TerrainMaterialRow& row) {
        for (const auto* column : {"name", "comment"})
            if (const auto found = row.fields.find(column); found != row.fields.end() && !found->second.empty()) return found->second;
        return std::string{};
    };
    WorldTerrainNames names;
    for (const auto& id : terrain.groundTilesets) {
        const auto row = catalog.value->ground.find(id);
        names.ground.push_back(row == catalog.value->ground.end() ? std::string{} : nameOf(row->second.source));
    }
    for (const auto& id : terrain.cliffTilesets) {
        const auto row = catalog.value->cliffs.find(id);
        names.cliffs.push_back(row == catalog.value->cliffs.end() ? std::string{} : nameOf(row->second.source));
    }
    return Result<WorldTerrainNames>::Success(std::move(names));
}

Result<std::vector<std::shared_ptr<const Render::TerrainAtlasSnapshot>>> LoadWorldTerrainAtlases(
    Resources::ResourceResolver& resolver, const MapData::TerrainHeader& terrain,
    std::optional<Render::TerrainWaterStyle>* water,
    std::shared_ptr<const Render::TerrainAtlasSnapshot>* blight,
    std::optional<Render::WaterMaterialDefinition>* waterDefinition) {
    if (water) water->reset();
    if (blight) blight->reset();
    if (waterDefinition) waterDefinition->reset();
    using Atlases = std::vector<std::shared_ptr<const Render::TerrainAtlasSnapshot>>;
    const auto fail = [](std::string code, std::string message) { return Result<Atlases>::Failure("World", std::move(code), std::move(message)); };
    if (terrain.groundTilesets.empty() || terrain.groundTilesets.size() > 16)
        return fail("WORLD_ATLAS_COUNT", "地表纹理数量不在 1–16 范围内。");
    const auto catalog = Render::LoadTerrainMaterialCatalog(resolver);
    if (!catalog.Succeeded()) return {std::nullopt, catalog.diagnostics};
    Atlases atlases;
    std::vector<Diagnostic> unavailable;
    const auto recordUnavailable = [&](std::string_view path, const std::vector<Diagnostic>& diagnostics,
        std::string_view effect = "（相关格保留高度色）") {
        for (auto diagnostic : diagnostics) {
            diagnostic.severity = DiagnosticSeverity::Warning;
            diagnostic.message = std::string(path) + "：" + diagnostic.message + std::string(effect);
            unavailable.push_back(std::move(diagnostic));
        }
    };
    std::size_t bytes = 0;
    std::map<std::string, std::shared_ptr<const Render::TerrainAtlasSnapshot>, std::less<>> cache;
    for (const auto& id : terrain.groundTilesets) {
        const auto material = catalog.value->ground.find(id);
        if (material == catalog.value->ground.end()) {
            recordUnavailable(id, {{DiagnosticSeverity::Warning, "World", "WORLD_ATLAS_UNKNOWN",
                "地图地表纹理在 Terrain.slk 中没有定义。", std::nullopt}});
            atlases.push_back(nullptr);
            continue;
        }
        const auto& path = material->second.texturePath;
        if (const auto old = cache.find(path); old != cache.end()) { atlases.push_back(old->second); continue; }
        auto atlas = Render::LoadTerrainTextureAtlas(resolver, path);
        if (!atlas.Succeeded()) {
            recordUnavailable(path, atlas.diagnostics);
            cache.emplace(path, nullptr);
            atlases.push_back(nullptr);
            continue;
        }
        const auto rgbaSize = (*atlas.value)->RgbaByteCount();
        if (rgbaSize > 64 * 1024 * 1024 - bytes) return fail("WORLD_ATLAS_BUDGET", "地表 RGBA 总量超过 64 MiB。");
        bytes += rgbaSize;
        cache.emplace(path, *atlas.value);
        atlases.push_back(std::move(*atlas.value));
    }
    if (water || waterDefinition) {
        const auto row = catalog.value->water.find(std::string(1, static_cast<char>(terrain.mainTileset)) + "Sha");
        if (row != catalog.value->water.end()) {
            const auto& definition = row->second;
            if (water) {
                Render::TerrainWaterStyle style{static_cast<float>(definition.heightOffsetWorld),
                    definition.shallowMinimum, definition.shallowMaximum, definition.deepMinimum, definition.deepMaximum, {}};
                style.frameCount = definition.frameCount;
                // 初始帧固定为 0；失败仍保留历史白色回退，但必须显示资源诊断。
                auto frame = Render::LoadTerrainWaterFrame(definition, 0, resolver);
                if (!frame.Succeeded()) recordUnavailable(definition.texturePrefix + "00.blp", frame.diagnostics,
                    "（水面按白色显示）");
                else {
                    style.texture = std::make_shared<const Image::DecodedImage>(std::move(frame.value->image));
                    style.frameSourcePath = frame.value->resource.canonicalPath;
                    style.frameSourceSha256 = std::move(frame.value->sourceSha256);
                }
                *water = std::move(style);
            }
            if (waterDefinition) *waterDefinition = definition;
        }
    }
    if (blight) {
        auto path = BlightTexturePath(resolver, terrain.mainTileset);
        if (!path.Succeeded()) recordUnavailable("UI\\WorldEditData.txt", path.diagnostics,
            "（荒芜保留原地表贴图近似色调）");
        else {
            auto atlas = Render::LoadTerrainTextureAtlas(resolver, *path.value);
            if (!atlas.Succeeded()) recordUnavailable(*path.value, atlas.diagnostics,
                "（荒芜保留原地表贴图近似色调）");
            else if ((*atlas.value)->columns != 8)
                recordUnavailable(*path.value, {{DiagnosticSeverity::Warning, "World", "WORLD_BLIGHT_ATLAS_LAYOUT",
                    "荒芜 atlas 不是已验证的 8×4 遮罩及基础变体布局。", std::nullopt}},
                    "（荒芜保留原地表贴图近似色调）");
            else if (!HasBlightMaskAlpha(**atlas.value))
                recordUnavailable(*path.value, {{DiagnosticSeverity::Warning, "World", "WORLD_BLIGHT_MASK_ALPHA",
                    "荒芜图集的局部遮罩没有可验证的透明与不透明像素。", std::nullopt}},
                    "（荒芜保留原地表贴图近似色调）");
            else *blight = std::move(*atlas.value);
        }
    }
    return {std::move(atlases), std::move(unavailable)};
}

WorldLighting LoadWorldLighting(Resources::ResourceResolver& resolver, char mainTileset,
    std::uint8_t lightEnvironment, float hours) {
    WorldLighting output;
    const auto note = [&output](std::string message) {
        output.notes.push_back({DiagnosticSeverity::Warning, "World", "WORLD_LIGHTING", std::move(message), std::nullopt});
    };
    Text::ProfileSections worldEditData;
    if (auto data = resolver.Read("UI\\WorldEditData.txt"); data.Succeeded()) {
        std::string_view text(reinterpret_cast<const char*>(data.value->bytes.data()), data.value->bytes.size());
        if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
        worldEditData = Text::ParseProfile(text);
    } else {
        note("UI\\WorldEditData.txt 不可用，昼夜光照按内置 Lordaeron 模型选择。");
    }
    for (const bool unit : {false, true}) {
        const auto path = Render::DayNightModelPath(worldEditData, mainTileset, lightEnvironment, unit);
        auto& target = unit ? output.unit : output.terrain;
        auto bytes = resolver.Read(path);
        if (!bytes.Succeeded()) { note(path + " 不可用，使用默认光照。"); continue; }
        auto model = Render::ParseMdx(bytes.value->bytes);
        if (!model.Succeeded()) { note(path + " 解析失败，使用默认光照。"); continue; }
        target = Render::SampleDayNight(*model.value, hours);
        if (!target) note(path + " 没有平行光或 Stand 动作，使用默认光照。");
    }
    return output;
}
} // namespace War3Map::Modules
