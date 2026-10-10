#include "War3MapCore/Render/TerrainWaterPreview.h"

#include <cmath>
#include <iterator>
#include <new>
#include <stdexcept>
#include <utility>

namespace War3Map::Render {
namespace {

constexpr const char* kSubsystem = "Render.TerrainWaterPreview";

Result<TerrainWaterPreview> Failure(const char* code, const char* message) {
    return Result<TerrainWaterPreview>::Failure(kSubsystem, code, message);
}

Result<TerrainWaterPreviewCoverage> MeasureCoverage(const MapData::TerrainGridSnapshot& terrain,
    const TerrainWaterPreviewLimits& limits) {
    const TerrainWaterPreviewLimits hard;
    if (limits.maxTiles > hard.maxTiles || limits.maxVertices > hard.maxVertices)
        return Result<TerrainWaterPreviewCoverage>::Failure(kSubsystem, "TERRAIN_WATER_PREVIEW_LIMITS",
            "水面预览网格限额只能收紧，不能放宽硬上限。");

    const auto width = terrain.header.width, height = terrain.header.height;
    if (terrain.header.version != 11 || width < 2 || height < 2 || width > 2049 || height > 2049
        || static_cast<std::uint64_t>(width) * height != terrain.vertices.size()
        || static_cast<std::uint64_t>(width - 1) * (height - 1) != terrain.tiles.size())
        return Result<TerrainWaterPreviewCoverage>::Failure(kSubsystem, "TERRAIN_WATER_PREVIEW_GRID",
            "输入不是尺寸与顶点/tile 数量一致的经典 v11 地形快照。");
    if (terrain.vertices.size() > limits.maxVertices || terrain.tiles.size() > limits.maxTiles)
        return Result<TerrainWaterPreviewCoverage>::Failure(kSubsystem, "TERRAIN_WATER_PREVIEW_BUDGET",
            "地形顶点或原格数量超过水面预览预算。");

    TerrainWaterPreviewCoverage coverage;
    coverage.totalTiles = terrain.tiles.size();
    for (const auto& vertex : terrain.vertices) {
        if (!std::isfinite(vertex.waterBaseHeight))
            return Result<TerrainWaterPreviewCoverage>::Failure(kSubsystem, "TERRAIN_WATER_PREVIEW_GRID",
                "地形快照含非有限水面基准高度。");
        coverage.waterFlaggedVertices += vertex.HasWater();
    }
    for (std::uint32_t y = 0; y + 1 < height; ++y) for (std::uint32_t x = 0; x + 1 < width; ++x) {
        const auto index = static_cast<std::size_t>(y) * (width - 1) + x;
        const auto sw = y * width + x;
        if (terrain.tiles[index].vertices != std::array<std::uint32_t, 4>{sw, sw + 1, sw + width, sw + width + 1})
            return Result<TerrainWaterPreviewCoverage>::Failure(kSubsystem, "TERRAIN_WATER_PREVIEW_GRID",
                "tile 顶点不符合 W3E 的 SW/SE/NW/NE 原格对应关系。");
        unsigned flaggedCorners = 0;
        for (const auto vertex : terrain.tiles[index].vertices)
            flaggedCorners += terrain.vertices[vertex].HasWater();
        coverage.waterFlaggedTiles += flaggedCorners != 0;
        coverage.mixedWaterFlagTiles += flaggedCorners != 0 && flaggedCorners != 4;
    }
    return Result<TerrainWaterPreviewCoverage>::Success(coverage);
}

Result<TerrainWaterPreview> EmptyPreview(TerrainWaterPreviewCoverage coverage) {
    TerrainWaterPreview preview;
    preview.coverage = coverage;
    auto result = Result<TerrainWaterPreview>::Success(std::move(preview));
    result.diagnostics.push_back({DiagnosticSeverity::Warning, kSubsystem, "TERRAIN_WATER_PREVIEW_EMPTY",
        "W3E 没有带水标记的原格，未读取或选择 Water.slk 水面帧。", std::nullopt});
    return result;
}

Result<TerrainWaterPreview> WithCatalog(TerrainWaterPreviewCoverage coverage,
    const MapData::TerrainGridSnapshot& terrain, const TerrainMaterialCatalog& catalog,
    const Resources::ResourceResolver& resolver, std::uint32_t explicitFrameIndex,
    const TerrainWaterPreviewLimits& limits) {
    if (coverage.waterFlaggedTiles == 0) return EmptyPreview(coverage);
    const auto tileset = terrain.header.mainTileset;
    if (tileset < 'A' || tileset > 'Z')
        return Failure("TERRAIN_WATER_PREVIEW_ID", "W3E 主 tileset 不是可用于 Water.slk waterID 的大写字母。");
    const std::string id = std::string(1, static_cast<char>(tileset)) + "Sha";
    const auto found = catalog.water.find(id);
    if (found == catalog.water.end())
        return Result<TerrainWaterPreview>::Failure(kSubsystem, "TERRAIN_WATER_PREVIEW_UNKNOWN_ID",
            "Water.slk 中没有 W3E 主 tileset 对应的水材质：" + id);

    const auto& definition = found->second;
    if (definition.source.id != id || definition.frameCount == 0
        || !std::isfinite(definition.heightOffsetTiles) || std::abs(definition.heightOffsetTiles) > 512
        || !std::isfinite(definition.heightOffsetWorld)
        || definition.heightOffsetWorld != definition.heightOffsetTiles * MapData::TerrainGridSnapshot::tileSize
        || definition.heightOffsetWorld < -65536 || definition.heightOffsetWorld > 65536)
        return Failure("TERRAIN_WATER_PREVIEW_DEFINITION", "Water.slk 水材质标识或高度偏移无效。");

    auto frame = LoadTerrainWaterFrame(definition, explicitFrameIndex, resolver, limits.materials);
    if (!frame.Succeeded()) return {std::nullopt, std::move(frame.diagnostics)};

    TerrainWaterStyle style;
    style.heightOffset = static_cast<float>(definition.heightOffsetWorld);
    style.shallowMinimum = definition.shallowMinimum;
    style.shallowMaximum = definition.shallowMaximum;
    style.deepMinimum = definition.deepMinimum;
    style.deepMaximum = definition.deepMaximum;
    style.texture = std::make_shared<const Image::DecodedImage>(std::move(frame.value->image));
    style.frameIndex = frame.value->index;
    style.frameCount = definition.frameCount;
    style.frameSourcePath = frame.value->resource.canonicalPath;
    style.frameSourceSha256 = std::move(frame.value->sourceSha256);

    TerrainWaterPreview preview;
    preview.coverage = coverage;
    preview.style = std::move(style);
    preview.waterId = id;
    preview.waterTableSource = definition.source.table;
    preview.waterTableRow = definition.source.row;
    preview.frameSource = std::move(frame.value->resource);
    auto result = Result<TerrainWaterPreview>::Success(std::move(preview));
    result.diagnostics = std::move(frame.diagnostics);
    if (coverage.mixedWaterFlagTiles != 0)
        result.diagnostics.push_back({DiagnosticSeverity::Warning, kSubsystem, "TERRAIN_WATER_PREVIEW_MIXED_FLAGS",
            "部分原格只有一至三个角带水；当前地表网格按整格水面 quad 预览，岸线形状未验证。", std::nullopt});
    return result;
}

template <typename Operation>
Result<TerrainWaterPreview> Checked(Operation operation) {
    try {
        return operation();
    } catch (const std::bad_alloc&) {
        return Failure("TERRAIN_WATER_PREVIEW_ALLOCATION", "水面预览分配失败，未发布部分结果。");
    } catch (const std::length_error&) {
        return Failure("TERRAIN_WATER_PREVIEW_ALLOCATION", "水面预览超出容器容量，未发布部分结果。");
    }
}

} // namespace

Result<TerrainWaterPreview> BuildTerrainWaterPreview(const MapData::TerrainGridSnapshot& terrain,
    const Resources::ResourceResolver& resolver, std::uint32_t explicitFrameIndex,
    const TerrainWaterPreviewLimits& limits) {
    return Checked([&] {
        auto measured = MeasureCoverage(terrain, limits);
        if (!measured.Succeeded()) return Result<TerrainWaterPreview>{std::nullopt, std::move(measured.diagnostics)};
        if (measured.value->waterFlaggedTiles == 0) return EmptyPreview(*measured.value);
        auto catalog = LoadTerrainMaterialCatalog(resolver, limits.materials);
        if (!catalog.Succeeded()) return Result<TerrainWaterPreview>{std::nullopt, std::move(catalog.diagnostics)};
        auto result = WithCatalog(*measured.value, terrain, *catalog.value, resolver, explicitFrameIndex, limits);
        result.diagnostics.insert(result.diagnostics.begin(),
            std::make_move_iterator(catalog.diagnostics.begin()), std::make_move_iterator(catalog.diagnostics.end()));
        return result;
    });
}

Result<TerrainWaterPreview> BuildTerrainWaterPreview(const MapData::TerrainGridSnapshot& terrain,
    const TerrainMaterialCatalog& catalog, const Resources::ResourceResolver& resolver,
    std::uint32_t explicitFrameIndex, const TerrainWaterPreviewLimits& limits) {
    return Checked([&] {
        auto measured = MeasureCoverage(terrain, limits);
        if (!measured.Succeeded()) return Result<TerrainWaterPreview>{std::nullopt, std::move(measured.diagnostics)};
        return WithCatalog(*measured.value, terrain, catalog, resolver, explicitFrameIndex, limits);
    });
}

} // namespace War3Map::Render
