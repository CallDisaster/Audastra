#include "WorldTerrainBuild.h"

#include <QObject>

#include <array>
#include <algorithm>
#include <bit>
#include <exception>

namespace War3Map::Modules {
namespace {

QStringList Messages(const std::vector<Diagnostic>& diagnostics) {
    QStringList messages;
    for (const auto& diagnostic : diagnostics) messages << QString::fromUtf8(diagnostic.message);
    return messages;
}

// 悬崖烘焙：仅成功读取按路径缓存并保留真实来源；失败原样交给builder，不新增预解析/自动重试。
Result<Render::TerrainCliffs> BuildCliffs(const MapData::TerrainGridSnapshot& shown, WorldTerrainCache& cache,
    std::optional<WorldCliffFailureFacts>& failure) {
    failure.reset();
    std::optional<WorldCliffFailureFacts> lastRequest;
    auto& resolver = cache.reader->Resolver();
    auto built = Render::BuildTerrainCliffs(shown, cache.cliffTypes, [&](std::string_view path) -> Result<std::vector<std::byte>> {
        lastRequest = WorldCliffFailureFacts{std::string(path), std::nullopt};
        auto found = cache.modelFiles.find(path);
        if (found == cache.modelFiles.end()) {
            auto read = resolver.Read(path);
            if (!read.Succeeded()) return {std::nullopt, std::move(read.diagnostics)};
            found = cache.modelFiles.emplace(std::string(path), std::move(read)).first;
        }
        lastRequest->location = found->second.value->location;
        return {found->second.value->bytes, found->second.diagnostics};
    });
    // 只接受builder在即时read/Parse失败点追加的明确回执；无此回执时不借最后callback归因。
    if (!built.Succeeded() && !built.diagnostics.empty()) {
        const auto& context = built.diagnostics.back();
        if (context.severity == DiagnosticSeverity::Info && context.subsystem == "Render.TerrainCliffs"
            && context.code == Render::kTerrainCliffModelFailureContextCode) failure = std::move(lastRequest);
    }
    return built;
}

// 显示副本（坡道抬高）与高度网格；超出逐格贴图预算时只给高度预览。
bool HasRamps(const MapData::TerrainGridSnapshot& raw) {
    return std::any_of(raw.vertices.begin(), raw.vertices.end(), [](const auto& vertex) { return vertex.IsRamp(); });
}

Status PrepareHeightRules(const MapData::TerrainGridSnapshot& raw, WorldTerrainCache& cache) {
    if (!cache.reader) return Status::Ok();
    if (cache.heightRules && cache.heightRulesTileset == raw.header.mainTileset)
        return cache.reader->VerifyHeightRules(*cache.heightRules);
    cache.heightRules.reset(); cache.heightRulesTileset.reset();
    if (!HasRamps(raw)) return Status::Ok();
    auto captured = cache.reader->CaptureHeightRules(raw.header.mainTileset);
    if (!captured.Succeeded() || !captured.value) return {captured.diagnostics};
    cache.heightRules = std::move(*captured.value);
    cache.heightRulesTileset = raw.header.mainTileset;
    return Status::Ok();
}

Result<WorldTerrainLoaded> Shape(const MapData::TerrainGridSnapshot& raw,
    const std::shared_ptr<const WorldTerrainHeightRules>& heightRules = {}) {
    auto shown = std::make_shared<MapData::TerrainGridSnapshot>(raw);
    if (heightRules) {
        const auto maximum = heightRules->HeightMaximumCliffLevel();
        if (!maximum) return Result<WorldTerrainLoaded>::Failure("World", "WORLD_HEIGHT_RULE_UNAVAILABLE", "坡道高度缺少实际规则owner。");
        const auto applied = Render::ApplyRampHeights(*shown, *maximum);
        if (!applied.Succeeded()) return {{}, applied.diagnostics};
    } else Render::ApplyRampHeights(*shown);
    auto mesh = BuildWorldTerrainMesh(*shown);
    if (!mesh.Succeeded()) return {std::nullopt, std::move(mesh.diagnostics)};
    WorldTerrainLoaded loaded;
    loaded.mesh = std::move(*mesh.value);
    loaded.terrain = std::move(shown);
    loaded.heightRules = heightRules;
    auto result = Result<WorldTerrainLoaded>::Success(std::move(loaded));
    if (!heightRules && HasRamps(raw)) {
        result.value->materialNote = QObject::tr("缺少实际Terrain.MaxHeight来源，坡道显示与拾取仅为旧直坡近似。");
        result.diagnostics.push_back({DiagnosticSeverity::Warning, "World", "TERRAIN_HEIGHT_APPROXIMATE",
            "缺少同地图实际Terrain.MaxHeight来源，坡道显示与拾取仅为旧直坡近似。", std::nullopt});
    }
    return result;
}

bool Textured(const WorldTerrainLoaded& loaded) {
    return loaded.mesh.stride == 1 && loaded.terrain->tiles.size() * 4 <= Render::TerrainGroundMeshLimits{}.maxVertices;
}

WorldShadowObservation ObserveShadow(const WorldShadowInput& input, const Result<WorldTerrainSources>& sources) {
    WorldShadowObservation observation;
    observation.context = input.context;
    if (sources.Succeeded()) observation.sourceArchiveSha = sources.value->archiveSha;
    if (input.read) {
        observation.readDiagnostics = input.read->diagnostics;
        observation.readState = input.read->Succeeded() ? WorldShadowReadState::Present : WorldShadowReadState::ReadFailed;
        if (input.read->Succeeded()) observation.sourceBytes = input.read->value->size();
    }
    return observation;
}

// 地表网格与悬崖（缓存已就绪时）。
void Surface(WorldTerrainLoaded& loaded, WorldTerrainCache& cache, std::shared_ptr<const Render::TerrainCliffs> keepCliffs,
    const Render::TerrainWaterStyle* selectedWater, Render::TerrainGroundBlightPolicy blightPolicy,
    Render::TerrainGroundRepresentation representation) {
    const auto& header = loaded.terrain->header;
    const bool groundChanged = header.groundTilesets != cache.header.groundTilesets || header.mainTileset != cache.header.mainTileset;
    const bool cliffsChanged = header.cliffTilesets != cache.header.cliffTilesets;
    if (cliffsChanged) {
        cache.cliffTypes.clear();
        if (auto types = LoadWorldCliffTypes(cache.reader->Resolver(), header); types.Succeeded())
            cache.cliffTypes = std::move(*types.value);
        else cache.notes << WorldDiagnosticText(types.diagnostics) + QObject::tr("（悬崖保留高度色）");
        keepCliffs.reset();
    }
    if (groundChanged) {
        cache.atlases.clear(); cache.blightAtlas.reset(); cache.atlasNotes.clear();
        auto atlases = LoadWorldTerrainAtlases(cache.reader->Resolver(), header, &cache.water, &cache.blightAtlas,
            &cache.waterDefinition);
        if (!atlases.Succeeded()) cache.atlasNotes << WorldDiagnosticText(atlases.diagnostics);
        else { cache.atlases = std::move(*atlases.value); cache.atlasNotes << Messages(atlases.diagnostics); }
    }
    if (groundChanged || cliffsChanged) {
        cache.names = {};
        if (auto names = LoadWorldTerrainNames(cache.reader->Resolver(), header); names.Succeeded())
            cache.names = std::move(*names.value);
        cache.header = header;
    }
    QStringList notes = cache.notes;
    if (!loaded.materialNote.isEmpty()) notes.prepend(loaded.materialNote);
    notes << cache.atlasNotes;
    loaded.environment = cache.environment;
    if (cache.atlases.empty()) { loaded.materialNote = notes.join(QLatin1Char('\n')); return; }
    std::shared_ptr<const Render::TerrainCliffs> cliffs = std::move(keepCliffs);
    if (!cliffs) {
        auto built = BuildCliffs(*loaded.terrain, cache, loaded.cliffFailure);
        if (!built.Succeeded()) {
            loaded.cliffDiagnostics = std::move(built.diagnostics);
            if (loaded.cliffFailure) {
                if (const auto& location = loaded.cliffFailure->location)
                    notes << QObject::tr("实际模型来源：%1 · %2").arg(QString::fromStdString(location->sourceId),
                        QString::fromStdString(location->sourcePath));
            }
            for (const auto& diagnostic : loaded.cliffDiagnostics)
                notes << QString::fromStdString(diagnostic.code) + QStringLiteral(": ") + QString::fromUtf8(diagnostic.message);
            notes << QObject::tr("悬崖模型未显示，保留高度预览。");
        } else {
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
        cache.blightAtlas, selectedGround, Render::TerrainGroundBudgetPolicy::PreserveBasePreview, blightPolicy, representation);
    if (!ground.Succeeded()) notes.prepend(WorldDiagnosticText(ground.diagnostics));
    else {
        if (ground.value->budget.reduced)
            notes << QObject::tr("已启用基础贴图预算预览：保留全部原格基础层；可选层按全图水面、普通过渡、精确荒芜顺序整类保留，省略数量见地表详情；不代表完整原版保真。");
        if (ground.value->blightApproximationTiles != 0)
            notes << QObject::tr("荒芜 %1 格因原版材质不可用或网格/纹理预算不足，仅以保留原地表贴图的近似色调预览。")
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

QString WorldShadowSummary(const WorldShadowObservation& shadow, const Render::SceneEnvironment* environment) {
    if (shadow.readState == WorldShadowReadState::NotObserved) return QObject::tr("静态阴影未观测");
    if (shadow.readState == WorldShadowReadState::ReadFailed) return QObject::tr("静态阴影未加载（存在性未确认）");
    QString text = QObject::tr("源图静态阴影未解析");
    if (environment) switch (environment->shadowInput.state) {
        case Render::SceneShadowInputState::NotProvided: break;
        case Render::SceneShadowInputState::Parsed: text = QObject::tr("源图静态阴影"); break;
        case Render::SceneShadowInputState::PresentEmpty: text = QObject::tr("源图静态阴影为空（未显示）"); break;
        case Render::SceneShadowInputState::Rejected: text = QObject::tr("源图静态阴影不可用（未显示）"); break;
    }
    if (shadow.context.terrainRelation == WorldShadowTerrainRelation::TerrainOrPathingEdited)
        text += QObject::tr(" · 地形或通行性已变化，阴影未重建");
    else if (shadow.context.terrainRelation == WorldShadowTerrainRelation::Unknown)
        text += QObject::tr(" · 地形关系未观测");
    return text;
}

QString WorldShadowDetails(const WorldShadowObservation& shadow, const Render::SceneEnvironment* environment) {
    QStringList lines{WorldShadowSummary(shadow, environment)};
    lines << QObject::tr("仅观察源图基线SHD；不从游戏同名资源补入，不证明原版烘焙或强度语义。");
    if (!shadow.sourceArchiveSha.empty()) lines << QObject::tr("源图 SHA：%1").arg(QString::fromStdString(shadow.sourceArchiveSha));
    if (shadow.readState == WorldShadowReadState::Present)
        lines << QObject::tr("已读原字：%1 字节").arg(static_cast<qulonglong>(shadow.sourceBytes));
    else lines << QObject::tr("没有准确缺失证明，成员存在性仍未确认。");
    if (shadow.context.terrainRelation == WorldShadowTerrainRelation::TerrainAndPathingUnchanged)
        lines << QObject::tr("当前会话地形与通行性未改；这不证明源阴影已经按当前场景重新烘焙。");
    if (shadow.context.sessionIdentity) lines << QObject::tr("会话 %1 · 修订 %2 · 地形来源代际 %3")
        .arg(static_cast<qulonglong>(shadow.context.sessionIdentity)).arg(static_cast<qulonglong>(shadow.context.revision))
        .arg(static_cast<qulonglong>(shadow.context.terrainSourceRevision));
    if (shadow.context.pendingSourceChanges)
        lines << QObject::tr("存在静态阴影资源草稿，当前资源草稿未用于此源图阴影预览。");
    const auto append = [&lines](const std::vector<Diagnostic>& diagnostics) {
        for (const auto& diagnostic : diagnostics) {
            auto text = QString::fromStdString(diagnostic.subsystem) + QStringLiteral(" · ")
                + QString::fromStdString(diagnostic.code) + QStringLiteral(" · ") + QString::fromUtf8(diagnostic.message);
            if (diagnostic.origin) text += QStringLiteral(" · ") + QString::fromStdWString(diagnostic.origin->source.wstring());
            lines << text;
        }
    };
    append(shadow.readDiagnostics);
    if (environment) append(environment->shadowInput.diagnostics);
    return lines.join(QLatin1Char('\n'));
}

Result<WorldTerrainLoaded> LoadWorldTerrain(std::span<const std::byte> bytes, const Result<WorldTerrainSources>& sources,
    const std::optional<WorldEnvironmentInputs>& environmentInputs, const WorldShadowInput& shadow,
    std::shared_ptr<WorldTerrainCache>* cache, Render::TerrainGroundBlightPolicy blightPolicy,
    Render::TerrainGroundRepresentation representation) {
    const auto* settings = environmentInputs ? &environmentInputs->settings : nullptr;
    if (cache) cache->reset();
    try {
        const auto terrain = MapData::ParseMapTerrain(bytes);
        if (!terrain.Succeeded()) return {std::nullopt, terrain.diagnostics};
        auto raw = terrain.value->Snapshot();
        std::shared_ptr<WorldTerrainCache> created;
        if (sources.Succeeded()) {
            auto reader = WorldResourceReader::Open(*sources.value);
            if (!reader.Succeeded()) return {std::nullopt, std::move(reader.diagnostics)};
            created = std::make_shared<WorldTerrainCache>();
            created->reader = std::move(*reader.value);
            const auto ready = PrepareHeightRules(*raw, *created);
            if (!ready.Succeeded()) return {{}, ready.diagnostics};
        }
        auto shaped = Shape(*raw, created ? created->heightRules : nullptr);
        if (!shaped.Succeeded()) return shaped;
        auto& loaded = *shaped.value;
        loaded.rawTerrain = std::move(raw);
        loaded.shadowObservation = ObserveShadow(shadow, sources);
        if (!sources.Succeeded()) {
            // 目录资源尚未接入时保留高度预览；身份失效、读取失败和预算拒绝不能伪装成成功。
            if (sources.diagnostics.size() != 1 || sources.diagnostics.front().code != "WORLD_ASSET_DIRECTORY_UNSUPPORTED")
                return {std::nullopt, sources.diagnostics};
            if (!loaded.materialNote.isEmpty()) loaded.materialNote += QLatin1Char('\n');
            loaded.materialNote += WorldDiagnosticText(sources.diagnostics);
            return shaped;
        }
        created->shadowObservation = loaded.shadowObservation;
        if (!Textured(loaded)) {
            if (!loaded.materialNote.isEmpty()) loaded.materialNote += QLatin1Char('\n');
            loaded.materialNote += QObject::tr("地表网格超过逐格贴图预算，当前显示高度预览");
            // 高度预览也持有来源租约，UI 提交前仍须复核，不因贴图预算跳过身份检查。
            const auto current = created->reader->VerifyCurrentSource();
            if (!current.Succeeded()) return {{}, current.diagnostics};
            if (cache) *cache = std::move(created);
            return shaped;
        }
        auto& resolver = created->reader->Resolver();
        // 原版昼夜光照（地形光烘焙进地表顶点色，单位光交给模型着色器）、地图雾与静态阴影。
        const auto lighting = LoadWorldLighting(resolver, static_cast<char>(loaded.terrain->header.mainTileset),
            settings ? settings->lightEnvironment : std::uint8_t{0});
        auto environment = Render::BuildSceneEnvironment(lighting.unit ? &*lighting.unit : nullptr,
            settings, environmentInputs && environmentInputs->terrainFog, *loaded.terrain,
            shadow.read && shadow.read->Succeeded()
                ? std::optional<std::span<const std::byte>>(*shadow.read->value) : std::nullopt);
        created->notes << Messages(lighting.notes) << Messages(environment.notes);
        created->environment = std::make_shared<const Render::SceneEnvironment>(std::move(environment));
        created->lighting = Render::TerrainLightingFrom(lighting.terrain ? &*lighting.terrain : nullptr);
        if (auto types = LoadWorldCliffTypes(resolver, loaded.terrain->header); types.Succeeded()) created->cliffTypes = std::move(*types.value);
        else created->notes << WorldDiagnosticText(types.diagnostics) + QObject::tr("（悬崖保留高度色）");
        // 名称只用于笔刷面板显示；读取失败时面板回退到 rawcode。
        if (auto names = LoadWorldTerrainNames(resolver, loaded.terrain->header); names.Succeeded())
            created->names = std::move(*names.value);
        Surface(loaded, *created, nullptr, nullptr, blightPolicy, representation);
        if (const auto current = created->reader->VerifyCurrentSource(); !current.Succeeded())
            return {std::nullopt, current.diagnostics};
        if (created->heightRules) {
            const auto current = created->reader->VerifyHeightRules(*created->heightRules);
            if (!current.Succeeded()) return {{}, current.diagnostics};
        }
        if (cache) *cache = std::move(created);
        return shaped;
    } catch (const std::exception&) {
        return Result<WorldTerrainLoaded>::Failure("World", "WORLD_PREVIEW_FAILED", "地形预览解码失败。");
    }
}

Result<WorldTerrainLoaded> RebuildWorldTerrain(const MapData::TerrainGridSnapshot& raw, WorldTerrainCache& cache,
    std::shared_ptr<const Render::TerrainCliffs> keepCliffs, std::optional<std::uint32_t> selectedWaterFrame,
    Render::TerrainGroundBlightPolicy blightPolicy, Render::TerrainGroundRepresentation representation,
    const std::optional<WorldShadowContext>& shadowContext) {
    try {
        if (cache.reader) {
            const auto current = cache.reader->VerifyCurrentSource();
            if (!current.Succeeded()) return {std::nullopt, current.diagnostics};
        }
        const auto ready = PrepareHeightRules(raw, cache);
        if (!ready.Succeeded()) return {{}, ready.diagnostics};
        auto shaped = Shape(raw, cache.heightRules);
        if (!shaped.Succeeded()) return shaped;
        shaped.value->shadowObservation = cache.shadowObservation;
        if (shadowContext) shaped.value->shadowObservation.context = *shadowContext;
        if (selectedWaterFrame && (!cache.reader || !cache.water || !cache.waterDefinition
            || raw.header.mainTileset != cache.header.mainTileset
            || raw.header.groundTilesets != cache.header.groundTilesets))
            return Result<WorldTerrainLoaded>::Failure("World", "WORLD_WATER_FRAME_UNAVAILABLE",
                "当前地形没有可选择的 Water.slk 水面帧或地表材质缓存已变化。");
        if (!Textured(*shaped.value) || !cache.reader) {
            if (selectedWaterFrame)
                return Result<WorldTerrainLoaded>::Failure("World", "WORLD_WATER_FRAME_UNAVAILABLE",
                    "当前地形仅可显示高度预览，不能确认水面帧已绘制。");
            if (!shaped.value->materialNote.isEmpty()) shaped.value->materialNote += QLatin1Char('\n');
            shaped.value->materialNote += QObject::tr("地表网格超过逐格贴图预算，当前显示高度预览");
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
        Surface(*shaped.value, cache, std::move(keepCliffs), selected ? &*selected : nullptr, blightPolicy, representation);
        if (selected) {
            if (!shaped.value->ground || shaped.value->ground->waterOmitted || shaped.value->ground->waterTiles.empty())
                return Result<WorldTerrainLoaded>::Failure("World", "WORLD_WATER_FRAME_RENDER",
                    "所选水面帧已读取，但地形网格未能绘制水面；保留上一帧。");
            shaped.value->selectedWater = std::move(selected);
        }
        if (cache.heightRules && cache.reader) {
            const auto current = cache.reader->VerifyHeightRules(*cache.heightRules);
            if (!current.Succeeded()) return {{}, current.diagnostics};
        }
        return shaped;
    } catch (const std::exception&) {
        return Result<WorldTerrainLoaded>::Failure("World", "WORLD_PREVIEW_FAILED", "地形重建失败。");
    }
}

} // namespace War3Map::Modules
