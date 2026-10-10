#include "WorldTerrainBuild.h"

#include <QObject>

#include <array>
#include <bit>
#include <exception>

namespace War3Map::Modules {
namespace {

QStringList Messages(const std::vector<Diagnostic>& diagnostics) {
    QStringList messages;
    for (const auto& diagnostic : diagnostics) messages << QString::fromUtf8(diagnostic.message);
    return messages;
}

// 悬崖烘焙：模型文件按路径缓存（含读取失败），编辑重建不重复读取。
Result<Render::TerrainCliffs> BuildCliffs(const MapData::TerrainGridSnapshot& shown, WorldTerrainCache& cache) {
    auto& resolver = cache.reader->Resolver();
    return Render::BuildTerrainCliffs(shown, cache.cliffTypes, [&](std::string_view path) {
        auto found = cache.modelFiles.find(path);
        if (found == cache.modelFiles.end()) {
            auto read = resolver.Read(path);
            found = cache.modelFiles.emplace(std::string(path),
                read.Succeeded() ? std::optional(std::move(read.value->bytes)) : std::nullopt).first;
        }
        if (!found->second) return Result<std::vector<std::byte>>::Failure("World", "WORLD_CLIFF_MODEL", "悬崖模型不可用。");
        return Result<std::vector<std::byte>>::Success(*found->second);
    });
}

// 显示副本（坡道抬高）与高度网格；超出逐格贴图预算时只给高度预览。
Result<WorldTerrainLoaded> Shape(const MapData::TerrainGridSnapshot& raw) {
    auto shown = std::make_shared<MapData::TerrainGridSnapshot>(raw);
    Render::ApplyRampHeights(*shown);
    auto mesh = BuildWorldTerrainMesh(*shown);
    if (!mesh.Succeeded()) return {std::nullopt, std::move(mesh.diagnostics)};
    WorldTerrainLoaded loaded;
    loaded.mesh = std::move(*mesh.value);
    loaded.terrain = std::move(shown);
    return Result<WorldTerrainLoaded>::Success(std::move(loaded));
}

bool Textured(const WorldTerrainLoaded& loaded) {
    return loaded.mesh.stride == 1 && loaded.terrain->tiles.size() * 4 <= Render::TerrainGroundMeshLimits{}.maxVertices;
}

// 地表网格与悬崖（缓存已就绪时）。
void Surface(WorldTerrainLoaded& loaded, WorldTerrainCache& cache, std::shared_ptr<const Render::TerrainCliffs> keepCliffs,
    const Render::TerrainWaterStyle* selectedWater = nullptr) {
    const auto& header = loaded.terrain->header;
    if (header.groundTilesets != cache.header.groundTilesets || header.mainTileset != cache.header.mainTileset) {
        cache.atlases.clear(); cache.blightAtlas.reset(); cache.atlasNotes.clear();
        auto atlases = LoadWorldTerrainAtlases(cache.reader->Resolver(), header, &cache.water, &cache.blightAtlas,
            &cache.waterDefinition);
        if (!atlases.Succeeded()) cache.atlasNotes << WorldDiagnosticText(atlases.diagnostics);
        else { cache.atlases = std::move(*atlases.value); cache.atlasNotes << Messages(atlases.diagnostics); }
        cache.header = header;
    }
    QStringList notes = cache.notes;
    notes << cache.atlasNotes;
    loaded.environment = cache.environment;
    if (cache.atlases.empty()) { loaded.materialNote = notes.join(QLatin1Char('\n')); return; }
    std::shared_ptr<const Render::TerrainCliffs> cliffs = std::move(keepCliffs);
    if (!cliffs) {
        auto built = BuildCliffs(*loaded.terrain, cache);
        if (!built.Succeeded()) notes << Messages(built.diagnostics).join(QLatin1Char('\n')) + QObject::tr("（悬崖保留高度色）");
        else {
            notes << Messages(built.value->warnings);
            cliffs = std::make_shared<const Render::TerrainCliffs>(std::move(*built.value));
        }
    }
    auto selection = Render::ResolveTerrainGroundSelection(*loaded.terrain, cache.cliffTypes);
    notes << Messages(selection.diagnostics);
    const std::span<const std::uint8_t> selectedGround = selection.Succeeded()
        ? std::span<const std::uint8_t>(selection.value->indices) : std::span<const std::uint8_t>{};
    auto ground = Render::BuildTerrainGroundMesh(*loaded.terrain, cache.atlases, {}, cache.lighting,
        selectedWater ? selectedWater : cache.water ? &*cache.water : nullptr,
        cliffs ? std::span<const std::uint8_t>(cliffs->coveredTiles) : std::span<const std::uint8_t>{},
        cache.blightAtlas, selectedGround);
    if (!ground.Succeeded()) notes.prepend(WorldDiagnosticText(ground.diagnostics));
    else {
        if (ground.value->blightApproximationTiles != 0)
            notes << QObject::tr("荒芜 %1 格因原版材质不可用或纹理预算不足，仅以保留原地表贴图的近似色调预览。")
                .arg(static_cast<qulonglong>(ground.value->blightApproximationTiles));
        loaded.ground = std::make_shared<const Render::TerrainGroundMesh>(std::move(*ground.value));
        loaded.cliffs = std::move(cliffs);
    }
    loaded.materialNote = notes.join(QLatin1Char('\n'));
}

} // namespace

std::optional<WorldEnvironmentInputs> CaptureWorldEnvironment(const MapData::MapSettings* settings) {
    if (!settings) return std::nullopt;
    return WorldEnvironmentInputs{settings->environment, (settings->flags & MapData::kMapFlagTerrainFog) != 0};
}

bool SameWorldLighting(const WorldEnvironmentInputs& left, const WorldEnvironmentInputs& right) {
    return left.settings.lightEnvironment == right.settings.lightEnvironment;
}

bool SameWorldFog(const WorldEnvironmentInputs& left, const WorldEnvironmentInputs& right) {
    const auto bits = [](float value) { return std::bit_cast<std::uint32_t>(value); };
    const auto& a = left.settings;
    const auto& b = right.settings;
    return left.terrainFog == right.terrainFog && a.fogStyle == b.fogStyle && bits(a.fogStart) == bits(b.fogStart)
        && bits(a.fogEnd) == bits(b.fogEnd) && bits(a.fogDensity) == bits(b.fogDensity)
        && a.fogColor.red == b.fogColor.red && a.fogColor.green == b.fogColor.green && a.fogColor.blue == b.fogColor.blue
        && a.fogColor.alpha == b.fogColor.alpha;
}

Result<WorldTerrainLoaded> LoadWorldTerrain(std::span<const std::byte> bytes, const Result<WorldTerrainSources>& sources,
    const std::optional<WorldEnvironmentInputs>& environmentInputs, const std::optional<std::vector<std::byte>>& shadow,
    std::shared_ptr<WorldTerrainCache>* cache) {
    const auto* settings = environmentInputs ? &environmentInputs->settings : nullptr;
    if (cache) cache->reset();
    try {
        const auto terrain = MapData::ParseMapTerrain(bytes);
        if (!terrain.Succeeded()) return {std::nullopt, terrain.diagnostics};
        auto raw = terrain.value->Snapshot();
        auto shaped = Shape(*raw);
        if (!shaped.Succeeded()) return shaped;
        auto& loaded = *shaped.value;
        loaded.rawTerrain = std::move(raw);
        if (!Textured(loaded)) { loaded.materialNote = QObject::tr("地表网格超过逐格贴图预算，当前显示高度预览"); return shaped; }
        if (!sources.Succeeded()) { loaded.materialNote = WorldDiagnosticText(sources.diagnostics); return shaped; }
        auto reader = WorldResourceReader::Open(*sources.value);
        if (!reader.Succeeded()) { loaded.materialNote = WorldDiagnosticText(reader.diagnostics); return shaped; }
        auto created = std::make_shared<WorldTerrainCache>();
        created->reader = std::move(*reader.value);
        auto& resolver = created->reader->Resolver();
        // 原版昼夜光照（地形光烘焙进地表顶点色，单位光交给模型着色器）、地图雾与静态阴影。
        const auto lighting = LoadWorldLighting(resolver, static_cast<char>(loaded.terrain->header.mainTileset),
            settings ? settings->lightEnvironment : std::uint8_t{0});
        auto environment = Render::BuildSceneEnvironment(lighting.unit ? &*lighting.unit : nullptr,
            settings, environmentInputs && environmentInputs->terrainFog, *loaded.terrain,
            shadow ? std::optional<std::span<const std::byte>>(*shadow) : std::nullopt);
        created->notes << Messages(lighting.notes) << Messages(environment.notes);
        created->environment = std::make_shared<const Render::SceneEnvironment>(std::move(environment));
        created->lighting = Render::TerrainLightingFrom(lighting.terrain ? &*lighting.terrain : nullptr);
        if (auto types = LoadWorldCliffTypes(resolver, loaded.terrain->header); types.Succeeded()) created->cliffTypes = std::move(*types.value);
        else created->notes << WorldDiagnosticText(types.diagnostics) + QObject::tr("（悬崖保留高度色）");
        // 名称只用于笔刷面板显示；读取失败时面板回退到 rawcode。
        if (auto names = LoadWorldTerrainNames(resolver, loaded.terrain->header); names.Succeeded())
            created->names = std::move(*names.value);
        Surface(loaded, *created, nullptr);
        if (const auto current = created->reader->VerifyCurrentSource(); !current.Succeeded())
            return {std::nullopt, current.diagnostics};
        if (cache) *cache = std::move(created);
        return shaped;
    } catch (const std::exception&) {
        return Result<WorldTerrainLoaded>::Failure("World", "WORLD_PREVIEW_FAILED", "地形预览解码失败。");
    }
}

Result<WorldTerrainLoaded> RebuildWorldTerrain(const MapData::TerrainGridSnapshot& raw, WorldTerrainCache& cache,
    std::shared_ptr<const Render::TerrainCliffs> keepCliffs, std::optional<std::uint32_t> selectedWaterFrame) {
    try {
        if (cache.reader) {
            const auto current = cache.reader->VerifyCurrentSource();
            if (!current.Succeeded()) return {std::nullopt, current.diagnostics};
        }
        auto shaped = Shape(raw);
        if (!shaped.Succeeded()) return shaped;
        if (selectedWaterFrame && (!cache.reader || !cache.water || !cache.waterDefinition
            || raw.header.mainTileset != cache.header.mainTileset
            || raw.header.groundTilesets != cache.header.groundTilesets))
            return Result<WorldTerrainLoaded>::Failure("World", "WORLD_WATER_FRAME_UNAVAILABLE",
                "当前地形没有可选择的 Water.slk 水面帧或地表材质缓存已变化。");
        if (!Textured(*shaped.value) || !cache.reader) {
            if (selectedWaterFrame)
                return Result<WorldTerrainLoaded>::Failure("World", "WORLD_WATER_FRAME_UNAVAILABLE",
                    "当前地形仅可显示高度预览，不能确认水面帧已绘制。");
            shaped.value->materialNote = QObject::tr("地表网格超过逐格贴图预算，当前显示高度预览");
            return shaped;
        }
        std::optional<Render::TerrainWaterStyle> selected;
        if (selectedWaterFrame) {
            auto frame = Render::LoadTerrainWaterFrame(*cache.waterDefinition, *selectedWaterFrame,
                cache.reader->Resolver());
            if (!frame.Succeeded()) return {std::nullopt, std::move(frame.diagnostics)};
            selected = *cache.water;
            selected->texture = std::make_shared<const Image::DecodedImage>(std::move(frame.value->image));
            selected->frameIndex = *selectedWaterFrame;
            selected->frameSourcePath = frame.value->resource.canonicalPath;
            selected->frameSourceSha256 = std::move(frame.value->sourceSha256);
        }
        Surface(*shaped.value, cache, std::move(keepCliffs), selected ? &*selected : nullptr);
        if (selected) {
            if (!shaped.value->ground || shaped.value->ground->waterOmitted || shaped.value->ground->waterTiles.empty())
                return Result<WorldTerrainLoaded>::Failure("World", "WORLD_WATER_FRAME_RENDER",
                    "所选水面帧已读取，但地形网格未能绘制水面；保留上一帧。");
            shaped.value->selectedWater = std::move(selected);
        }
        return shaped;
    } catch (const std::exception&) {
        return Result<WorldTerrainLoaded>::Failure("World", "WORLD_PREVIEW_FAILED", "地形重建失败。");
    }
}

} // namespace War3Map::Modules
