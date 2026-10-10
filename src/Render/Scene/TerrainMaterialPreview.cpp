#include "War3MapCore/Render/TerrainMaterialPreview.h"

#include "War3MapCore/Render/TerrainCliffs.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <iterator>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace War3Map::Render {
namespace {

constexpr std::size_t kMaxTiles = 256 * 256;
constexpr std::size_t kMaxAtlases = 16;
constexpr std::size_t kMaxAtlasRgbaBytes = 64 * 1024 * 1024;

using PreviewResult = Result<TerrainMaterialPreview>;
using Atlas = std::shared_ptr<const TerrainAtlasSnapshot>;

PreviewResult Fail(std::string code, std::string message) {
    return PreviewResult::Failure("TerrainMaterialPreview", std::move(code), std::move(message));
}

bool LocalAtlasFailure(const std::vector<Diagnostic>& diagnostics) {
    bool local = false;
    const bool decodeFailure = std::any_of(diagnostics.begin(), diagnostics.end(), [](const Diagnostic& diagnostic) {
        return diagnostic.code == "TERRAIN_ATLAS_DECODE";
    });
    for (const auto& diagnostic : diagnostics) {
        if (diagnostic.severity != DiagnosticSeverity::Error) continue;
        const auto& code = diagnostic.code;
        if (code == "RESOURCE_NOT_FOUND" || code == "TERRAIN_ATLAS_ALPHA_UNSUPPORTED"
            || code == "TERRAIN_ATLAS_DECODE"
            || code == "TERRAIN_ATLAS_FORMAT" || code == "TERRAIN_ATLAS_JPEG"
            || code == "TERRAIN_ATLAS_LAYOUT" || code == "TERRAIN_ATLAS_MIP") {
            local = true;
            continue;
        }
        // Core BLP 的像素错误由 TERRAIN_ATLAS_DECODE 一同报告。
        if (decodeFailure && diagnostic.subsystem == "Image.Blp" && diagnostic.code != "input-budget") continue;
        return false;
    }
    return local;
}

void WarnUnavailable(std::vector<Diagnostic>& warnings, std::string_view id,
    std::string_view path, const std::vector<Diagnostic>& diagnostics) {
    for (auto diagnostic : diagnostics) {
        diagnostic.severity = DiagnosticSeverity::Warning;
        diagnostic.message = "地表材质 " + std::string(id) + "（" + std::string(path) + "）："
            + diagnostic.message + "；相关原格保留高度色。";
        warnings.push_back(std::move(diagnostic));
    }
}

} // namespace

Result<TerrainMaterialPreview> BuildTerrainMaterialPreview(
    const MapData::TerrainGridSnapshot& terrain, const Resources::ResourceResolver& resolver,
    const TerrainMaterialPreviewLimits& limits, const TerrainLighting& lighting,
    std::optional<std::uint32_t> waterFrameIndex, bool includeCliffs) {
    try {
        if (limits.maxTiles == 0 || limits.maxTiles > kMaxTiles
            || limits.maxGroundAtlases == 0 || limits.maxGroundAtlases > kMaxAtlases
            || limits.maxTotalAtlasRgbaBytes == 0 || limits.maxTotalAtlasRgbaBytes > kMaxAtlasRgbaBytes)
            return Fail("TERRAIN_MATERIAL_PREVIEW_LIMITS", "调用方不能放宽材质预览硬预算。");
        const auto& header = terrain.header;
        if (header.version != 11 || header.width < 2 || header.height < 2
            || header.groundTilesets.empty() || header.groundTilesets.size() > limits.maxGroundAtlases)
            return Fail("TERRAIN_MATERIAL_PREVIEW_GRID", "仅支持有 1–16 个地面 rawcode 的经典 W3E v11。");
        const auto tileCount = static_cast<std::uint64_t>(header.width - 1) * (header.height - 1);
        if (tileCount > limits.maxTiles || tileCount != terrain.tiles.size()
            || static_cast<std::uint64_t>(header.width) * header.height != terrain.vertices.size())
            return Fail("TERRAIN_MATERIAL_PREVIEW_GRID", "地形尺寸、顶点或原格数量无效或超出预算。");

        // 必需目录作为整体校验；损坏、缺表、冲突或预算失败不能伪装为局部贴图回退。
        auto catalog = LoadTerrainMaterialCatalog(resolver, limits.materials);
        if (!catalog.Succeeded()) return {std::nullopt, std::move(catalog.diagnostics)};
        auto types = ResolveTerrainCliffTypes(*catalog.value, header);
        if (!types.Succeeded()) return {std::nullopt, std::move(types.diagnostics)};
        auto selection = ResolveTerrainGroundSelection(terrain, *types.value);
        if (!selection.Succeeded()) return {std::nullopt, std::move(selection.diagnostics)};
        // 派生 paired ground 即使未被 raw 顶点引用也必须加载；是否画悬崖模型不改变此选择。
        std::array<bool, kMaxAtlases> referenced{};
        for (const auto index : selection.value->indices)
            if (index < header.groundTilesets.size()) referenced[index] = true;
        std::vector<Diagnostic> warnings = std::move(selection.diagnostics);
        std::optional<TerrainWaterPreview> water;
        if (waterFrameIndex) {
            TerrainWaterPreviewLimits waterLimits;
            waterLimits.maxTiles = limits.maxTiles;
            waterLimits.materials = limits.materials;
            auto selected = BuildTerrainWaterPreview(terrain, *catalog.value, resolver, *waterFrameIndex, waterLimits);
            if (!selected.Succeeded()) return {std::nullopt, std::move(selected.diagnostics)};
            warnings.insert(warnings.end(), std::make_move_iterator(selected.diagnostics.begin()),
                std::make_move_iterator(selected.diagnostics.end()));
            water = std::move(*selected.value);
        }

        std::optional<TerrainCliffPreview> cliffs;
        if (includeCliffs) {
            auto cliffLimits = limits.cliffs;
            cliffLimits.maxTiles = std::min(cliffLimits.maxTiles, limits.maxTiles);
            auto built = BuildTerrainCliffPreview(terrain, *catalog.value, resolver, cliffLimits);
            if (!built.Succeeded()) return {std::nullopt, std::move(built.diagnostics)};
            warnings.insert(warnings.end(), std::make_move_iterator(built.diagnostics.begin()),
                std::make_move_iterator(built.diagnostics.end()));
            cliffs = std::move(*built.value);
        }

        std::vector<Atlas> atlases(header.groundTilesets.size());
        std::map<std::string, Atlas, std::less<>> cache;
        std::size_t retainedBytes = 0, referencedSlots = 0, loadedSlots = 0, unavailableSlots = 0;
        for (std::size_t index = 0; index < atlases.size(); ++index) {
            if (!referenced[index]) continue;
            ++referencedSlots;
            const auto& id = header.groundTilesets[index];
            const auto material = catalog.value->ground.find(id);
            if (material == catalog.value->ground.end()) {
                ++unavailableSlots;
                warnings.push_back({DiagnosticSeverity::Warning, "TerrainMaterialPreview",
                    "TERRAIN_MATERIAL_UNKNOWN_GROUND",
                    "Terrain.slk 未定义地面 rawcode " + id + "；相关原格保留高度色。", std::nullopt});
                continue;
            }
            const auto& path = material->second.texturePath;
            if (const auto cached = cache.find(path); cached != cache.end()) {
                atlases[index] = cached->second;
            } else {
                auto loaded = LoadTerrainTextureAtlas(resolver, path, limits.atlas);
                if (!loaded.Succeeded()) {
                    if (!LocalAtlasFailure(loaded.diagnostics)) {
                        auto failure = Fail("TERRAIN_MATERIAL_PREVIEW_ASSET", "地表资源来源或预算出错，不能发布材质场景：" + path);
                        failure.diagnostics.insert(failure.diagnostics.end(),
                            std::make_move_iterator(loaded.diagnostics.begin()),
                            std::make_move_iterator(loaded.diagnostics.end()));
                        return failure;
                    }
                    WarnUnavailable(warnings, id, path, loaded.diagnostics);
                    cache.emplace(path, nullptr);
                } else {
                    const auto bytes = (*loaded.value)->RgbaByteCount();
                    if (bytes > limits.maxTotalAtlasRgbaBytes - retainedBytes)
                        return Fail("TERRAIN_MATERIAL_PREVIEW_BUDGET", "地表图集保留 RGBA 总量超过预算。");
                    retainedBytes += bytes;
                    atlases[index] = std::move(*loaded.value);
                    cache.emplace(path, atlases[index]);
                }
            }
            if (atlases[index]) ++loadedSlots;
            else ++unavailableSlots;
        }
        if (loadedSlots == 0) {
            auto failure = Fail("TERRAIN_MATERIAL_PREVIEW_NO_ATLAS", "当前地图没有可用的已引用地表图集，不能标记为材质预览。");
            failure.diagnostics.insert(failure.diagnostics.end(),
                std::make_move_iterator(warnings.begin()), std::make_move_iterator(warnings.end()));
            return failure;
        }

        auto shaped = terrain;
        const auto rampAdjusted = ApplyRampHeights(shaped);
        if (cliffs && cliffs->rampAdjustedVertices != rampAdjusted)
            return Fail("TERRAIN_MATERIAL_PREVIEW_CLIFF_SHAPE", "悬崖与地表的坡道高度修正不一致。");
        const auto* waterStyle = water && water->style ? &*water->style : nullptr;
        const auto covered = cliffs ? std::span<const std::uint8_t>(cliffs->geometry.coveredTiles)
                                    : std::span<const std::uint8_t>{};
        auto ground = BuildTerrainGroundMesh(shaped, atlases, limits.ground, lighting, waterStyle, covered,
            {}, selection.value->indices);
        if (!ground.Succeeded()) return {std::nullopt, std::move(ground.diagnostics)};
        TerrainMaterialPreview preview;
        preview.coverage.totalTiles = tileCount;
        preview.coverage.texturedTiles = ground.value->texturedTiles;
        preview.coverage.heightColorTiles = ground.value->fallback.totalTiles;
        preview.coverage.hiddenCliffTiles = ground.value->hiddenTileCount;
        preview.coverage.referencedAtlasSlots = referencedSlots;
        preview.coverage.loadedAtlasSlots = loadedSlots;
        preview.coverage.unavailableAtlasSlots = unavailableSlots;
        preview.coverage.waterFlaggedTiles = ground.value->waterFlaggedTiles;
        preview.coverage.waterRenderedTiles = ground.value->waterTiles.size();
        preview.coverage.waterOmitted = ground.value->waterOmitted;
        preview.coverage.blightApproximationTiles = ground.value->blightApproximationTiles;
        preview.coverage.fallback = ground.value->fallback;
        if (preview.coverage.texturedTiles + preview.coverage.heightColorTiles
            + preview.coverage.hiddenCliffTiles != tileCount)
            return Fail("TERRAIN_MATERIAL_PREVIEW_COVERAGE", "地表原格覆盖统计与输入数量不一致。");
        if (cliffs && static_cast<std::size_t>(std::count_if(cliffs->geometry.coveredTiles.begin(),
                cliffs->geometry.coveredTiles.end(), [](std::uint8_t value) { return value != 0; }))
                != preview.coverage.hiddenCliffTiles)
            return Fail("TERRAIN_MATERIAL_PREVIEW_CLIFF_COVERAGE", "悬崖模型覆盖与实际隐藏的地面原格不一致。");
        if (water && water->coverage.waterFlaggedTiles != preview.coverage.waterFlaggedTiles)
            return Fail("TERRAIN_MATERIAL_PREVIEW_WATER_COVERAGE", "水面标记覆盖与地表网格不一致。");
        if (water && water->style && !preview.coverage.waterOmitted
            && (preview.coverage.waterRenderedTiles != preview.coverage.waterFlaggedTiles
                || ground.value->waterFrameIndex != water->style->frameIndex
                || ground.value->waterFrameCount != water->style->frameCount
                || ground.value->waterFrameSourcePath != water->style->frameSourcePath
                || ground.value->waterFrameSourceSha256 != water->style->frameSourceSha256))
            return Fail("TERRAIN_MATERIAL_PREVIEW_WATER_COVERAGE", "所选水面帧身份或实际绘制原格数与网格不一致。");
        if (preview.coverage.heightColorTiles != 0)
            warnings.push_back({DiagnosticSeverity::Warning, "TerrainMaterialPreview", "TERRAIN_MATERIAL_PREVIEW_FALLBACK",
                std::to_string(preview.coverage.heightColorTiles) + " 个原格仅显示高度色；详见 coverage.fallback。", std::nullopt});
        if (cliffs && cliffs->candidateTiles != 0 && preview.coverage.hiddenCliffTiles == 0)
            warnings.push_back({DiagnosticSeverity::Warning, "TerrainMaterialPreview", "TERRAIN_MATERIAL_PREVIEW_CLIFF_EMPTY",
                "W3E 有悬崖/坡道候选格，但没有模型实际覆盖原格；场景仍只显示地面或高度色。", std::nullopt});
        if (preview.coverage.blightApproximationTiles != 0)
            warnings.push_back({DiagnosticSeverity::Warning, "TerrainMaterialPreview", "TERRAIN_MATERIAL_PREVIEW_BLIGHT",
                std::to_string(preview.coverage.blightApproximationTiles) + " 个荒芜原格仅以近似色调显示。", std::nullopt});
        if (preview.coverage.waterFlaggedTiles != 0 && !waterFrameIndex)
            warnings.push_back({DiagnosticSeverity::Warning, "TerrainMaterialPreview", "TERRAIN_MATERIAL_PREVIEW_WATER",
                std::to_string(preview.coverage.waterFlaggedTiles) + " 个原格带水标记，本入口未构造水面。", std::nullopt});
        else if (preview.coverage.waterFlaggedTiles != 0 && preview.coverage.waterOmitted)
            warnings.push_back({DiagnosticSeverity::Warning, "TerrainMaterialPreview", "TERRAIN_MATERIAL_PREVIEW_WATER_OMITTED",
                std::to_string(preview.coverage.waterFlaggedTiles) + " 个原格带水标记，所选帧已读取但网格纹理预算不足，未绘制水面。", std::nullopt});
        else if (preview.coverage.waterRenderedTiles != 0)
            warnings.push_back({DiagnosticSeverity::Warning, "TerrainMaterialPreview", "TERRAIN_MATERIAL_PREVIEW_WATER_APPROXIMATION",
                "水面按原格 quad 与所选 Water.slk 帧预览；岸线、UV 与动画不等同于原版验证。", std::nullopt});
        preview.ground = std::move(*ground.value);
        preview.rampAdjustedVertices = rampAdjusted;
        preview.water = std::move(water);
        preview.cliffs = std::move(cliffs);
        return {std::move(preview), std::move(warnings)};
    } catch (const std::exception&) {
        return Fail("TERRAIN_MATERIAL_PREVIEW_FAILED", "材质预览构建失败，未发布部分场景。");
    }
}

} // namespace War3Map::Render
