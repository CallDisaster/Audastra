#include "War3MapCore/Render/TerrainCliffPreview.h"

#include "War3MapCore/Hash.h"
#include "War3MapCore/Render/MdxModel.h"
#include "War3MapCore/Resources/ImportTable.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <new>
#include <set>
#include <stdexcept>
#include <utility>

namespace War3Map::Render {
namespace {

constexpr const char* kSubsystem = "Render.TerrainCliffPreview";

using PreviewResult = Result<TerrainCliffPreview>;

PreviewResult Failure(const char* code, std::string message) {
    return PreviewResult::Failure(kSubsystem, code, std::move(message));
}

Result<std::size_t> CountCandidates(const MapData::TerrainGridSnapshot& terrain,
    const TerrainCliffPreviewLimits& limits) {
    const TerrainCliffPreviewLimits hard;
    if (limits.maxTiles > hard.maxTiles || limits.maxVertices > hard.maxVertices
        || limits.maxIndices > hard.maxIndices || limits.maxMeshes > hard.maxMeshes
        || limits.maxModelPaths > hard.maxModelPaths || limits.maxModelBytes > hard.maxModelBytes
        || limits.maxTotalModelBytes > hard.maxTotalModelBytes)
        return Result<std::size_t>::Failure(kSubsystem, "TERRAIN_CLIFF_PREVIEW_LIMITS",
            "悬崖预览限额只能收紧，不能放宽硬上限。");

    const auto width = terrain.header.width, height = terrain.header.height;
    if (terrain.header.version != 11 || width < 2 || height < 2 || width > 257 || height > 257
        || terrain.header.cliffTilesets.size() > 256
        || static_cast<std::uint64_t>(width) * height != terrain.vertices.size()
        || static_cast<std::uint64_t>(width - 1) * (height - 1) != terrain.tiles.size())
        return Result<std::size_t>::Failure(kSubsystem, "TERRAIN_CLIFF_PREVIEW_GRID",
            "输入不是尺寸、顶点与原格数量一致的经典 W3E v11 地形快照。");
    if (terrain.tiles.size() > limits.maxTiles)
        return Result<std::size_t>::Failure(kSubsystem, "TERRAIN_CLIFF_PREVIEW_BUDGET",
            "地形原格数量超过悬崖预览预算。");
    for (const auto& vertex : terrain.vertices)
        if (!std::all_of(vertex.position.begin(), vertex.position.end(), [](float value) {
                return std::isfinite(value) && std::abs(value) <= 16777216.0f;
            }))
            return Result<std::size_t>::Failure(kSubsystem, "TERRAIN_CLIFF_PREVIEW_GRID",
                "地形快照含非有限或超出坐标预算的顶点。");

    std::size_t candidates = 0;
    for (std::uint32_t y = 0; y + 1 < height; ++y) for (std::uint32_t x = 0; x + 1 < width; ++x) {
        const auto index = static_cast<std::size_t>(y) * (width - 1) + x;
        const auto sw = y * width + x;
        if (terrain.tiles[index].vertices != std::array<std::uint32_t, 4>{sw, sw + 1, sw + width, sw + width + 1})
            return Result<std::size_t>::Failure(kSubsystem, "TERRAIN_CLIFF_PREVIEW_GRID",
                "tile 顶点不符合 W3E 的 SW/SE/NW/NE 原格对应关系。");
        const auto& corners = terrain.tiles[index].vertices;
        const auto first = terrain.vertices[corners[0]].CliffLevel();
        bool levelsDiffer = false, anyRamp = false, allRamp = true;
        for (const auto corner : corners) {
            const auto& vertex = terrain.vertices[corner];
            levelsDiffer |= vertex.CliffLevel() != first;
            anyRamp |= vertex.IsRamp();
            allRamp &= vertex.IsRamp();
        }
        // 全角坡道的层级过渡面由地面绘制；其余异层或部分坡道格可能放模型。
        candidates += (levelsDiffer && !allRamp) || (anyRamp && !allRamp);
    }
    return Result<std::size_t>::Success(candidates);
}

bool MissingOnly(const std::vector<Diagnostic>& diagnostics) {
    return diagnostics.size() == 1 && diagnostics.front().code == "RESOURCE_NOT_FOUND";
}

PreviewResult WithCatalog(const MapData::TerrainGridSnapshot& terrain,
    const TerrainMaterialCatalog& catalog, const Resources::ResourceResolver& resolver,
    const TerrainCliffPreviewLimits& limits, std::size_t candidates,
    std::optional<std::uint8_t> actualMaximumCliffLevel) {
    TerrainCliffPreview preview;
    std::vector<Diagnostic> notes;
    preview.candidateTiles = candidates;
    auto shaped = terrain;
    if (actualMaximumCliffLevel) {
        const auto adjusted = ApplyRampHeights(shaped, *actualMaximumCliffLevel);
        if (!adjusted.Succeeded()) return {{}, adjusted.diagnostics};
        preview.rampAdjustedVertices = *adjusted.value;
    } else {
        preview.rampAdjustedVertices = ApplyRampHeights(shaped);
        if (std::any_of(terrain.vertices.begin(), terrain.vertices.end(), [](const auto& vertex) { return vertex.IsRamp(); }))
            notes.push_back({DiagnosticSeverity::Warning, kSubsystem, "TERRAIN_HEIGHT_APPROXIMATE",
                "未提供实际Terrain.MaxHeight，坡道高度仅为旧直坡近似。", std::nullopt});
    }
    if (candidates == 0) {
        preview.geometry.coveredTiles.assign(terrain.tiles.size(), 0);
        return {std::move(preview), std::move(notes)};
    }
    if (catalog.tables[1].canonicalPath.empty())
        return Failure("TERRAIN_CLIFF_PREVIEW_CATALOG", "悬崖目录没有 CliffTypes.slk 的来源身份。");
    preview.cliffTableSource = catalog.tables[1];

    auto types = ResolveTerrainCliffTypes(catalog, terrain.header);
    if (!types.Succeeded()) return {std::nullopt, std::move(types.diagnostics)};

    std::size_t attemptedPaths = 0, totalModelBytes = 0;
    std::vector<Diagnostic> fatal;
    const auto reject = [&](const char* code, std::string message) -> Result<std::vector<std::byte>> {
        if (fatal.empty()) fatal.push_back({DiagnosticSeverity::Error, kSubsystem, code, std::move(message), std::nullopt});
        return {std::nullopt, fatal};
    };
    ModelFileReader reader = [&](std::string_view path) -> Result<std::vector<std::byte>> {
        if (!fatal.empty()) return {std::nullopt, fatal};
        if (++attemptedPaths > limits.maxModelPaths)
            return reject("TERRAIN_CLIFF_PREVIEW_BUDGET", "悬崖模型变体路径数量超过预算。");
        auto resolved = resolver.Read(path);
        if (!resolved.Succeeded()) {
            if (!MissingOnly(resolved.diagnostics)) fatal = resolved.diagnostics;
            return {std::nullopt, std::move(resolved.diagnostics)};
        }
        auto& bytes = resolved.value->bytes;
        if (bytes.size() > limits.maxModelBytes || bytes.size() > limits.maxTotalModelBytes - totalModelBytes)
            return reject("TERRAIN_CLIFF_PREVIEW_BUDGET", "悬崖 MDX 单文件或累计读取字节超出预算。");
        MdxLimits mdxLimits;
        mdxLimits.maxInputBytes = std::min(mdxLimits.maxInputBytes, limits.maxModelBytes);
        auto model = ParseMdx(bytes, mdxLimits);
        if (!model.Succeeded()) {
            fatal = std::move(model.diagnostics);
            return {std::nullopt, fatal};
        }
        std::size_t vertices = 0, indices = 0;
        for (const auto& geoset : model.value->geosets) {
            if (!geoset.visibleInStand || geoset.indices.empty()) continue;
            if (geoset.materialId >= model.value->materials.size())
                return reject("TERRAIN_CLIFF_PREVIEW_MODEL", "悬崖 MDX 的 geoset 材质索引无效。");
            const auto& material = model.value->materials[geoset.materialId];
            if (material.layers.empty()) continue;
            if (material.layers.front().textureId >= model.value->textures.size())
                return reject("TERRAIN_CLIFF_PREVIEW_MODEL", "悬崖 MDX 的图层贴图索引无效。");
            vertices += geoset.positions.size();
            indices += geoset.indices.size();
            if (material.layers.size() > 1 || geoset.skippedPrimitiveGroups != 0)
                notes.push_back({DiagnosticSeverity::Warning, kSubsystem, "TERRAIN_CLIFF_PREVIEW_MODEL_PARTIAL",
                    "悬崖 MDX 存在额外图层或非三角形图元；烘焙几何只使用首层三角形：" + std::string(path),
                    std::nullopt});
        }
        if (vertices == 0 || indices == 0)
            return reject("TERRAIN_CLIFF_PREVIEW_MODEL", "悬崖 MDX 没有可显示的 Stand 三角形，不能隐藏对应地面：" + std::string(path));
        // BuildTerrainCliffs 内部按格追加几何、无可注入的逐格预算。用所有候选格都使用本模型
        // 的保守上界，在首次烘焙之前拒绝可能超额的来源。
        if (vertices > limits.maxVertices / candidates || indices > limits.maxIndices / candidates)
            return reject("TERRAIN_CLIFF_PREVIEW_BUDGET", "悬崖模型与候选格的最坏情况几何超过预算。");
        auto sha = Sha256(bytes);
        if (!sha.Succeeded()) {
            fatal = std::move(sha.diagnostics);
            return {std::nullopt, fatal};
        }
        totalModelBytes += bytes.size();
        preview.modelSources.push_back({std::move(resolved.value->location), std::move(*sha.value), bytes.size()});
        return Result<std::vector<std::byte>>::Success(std::move(bytes));
    };
    auto geometry = BuildTerrainCliffs(shaped, *types.value, reader);
    if (!fatal.empty()) return {std::nullopt, std::move(fatal)};
    if (!geometry.Succeeded()) return {std::nullopt, std::move(geometry.diagnostics)};
    std::size_t vertices = 0, indices = 0;
    if (geometry.value->meshes.size() > limits.maxMeshes)
        return Failure("TERRAIN_CLIFF_PREVIEW_BUDGET", "悬崖烘焙 mesh 数量超过预算。");
    std::set<std::string> texturePaths;
    for (const auto& mesh : geometry.value->meshes) {
        if (mesh.vertices.size() > limits.maxVertices - vertices || mesh.indices.size() > limits.maxIndices - indices)
            return Failure("TERRAIN_CLIFF_PREVIEW_BUDGET", "悬崖烘焙顶点或索引数量超过预算。");
        if (!Resources::CanonicalImportPath(mesh.texturePath).Succeeded())
            return Failure("TERRAIN_CLIFF_PREVIEW_TEXTURE", "悬崖模型使用空路径或不安全的贴图路径，不能隐藏对应地面。");
        vertices += mesh.vertices.size();
        indices += mesh.indices.size();
        texturePaths.insert(mesh.texturePath);
    }
    preview.geometry = std::move(*geometry.value);
    preview.texturePaths.assign(texturePaths.begin(), texturePaths.end());
    notes.insert(notes.end(), std::make_move_iterator(preview.geometry.warnings.begin()),
        std::make_move_iterator(preview.geometry.warnings.end()));
    if (preview.geometry.unknownTypeTiles != 0 || preview.geometry.tooHighTiles != 0)
        notes.push_back({DiagnosticSeverity::Warning, kSubsystem, "TERRAIN_CLIFF_PREVIEW_UNCOVERED",
            "部分悬崖原格因未知类型或层差过大未放模型；调用方须保留高度色并报告覆盖数量。", std::nullopt});
    return {std::move(preview), std::move(notes)};
}

template <typename Operation>
PreviewResult Checked(Operation operation) {
    try {
        return operation();
    } catch (const std::bad_alloc&) {
        return Failure("TERRAIN_CLIFF_PREVIEW_ALLOCATION", "悬崖预览分配失败，未发布部分网格。");
    } catch (const std::length_error&) {
        return Failure("TERRAIN_CLIFF_PREVIEW_ALLOCATION", "悬崖预览超过容器容量，未发布部分网格。");
    }
}

} // namespace

Result<TerrainCliffPreview> BuildTerrainCliffPreview(const MapData::TerrainGridSnapshot& terrain,
    const Resources::ResourceResolver& resolver, const TerrainCliffPreviewLimits& limits,
    std::optional<std::uint8_t> actualMaximumCliffLevel) {
    return Checked([&] {
        auto counted = CountCandidates(terrain, limits);
        if (!counted.Succeeded()) return PreviewResult{std::nullopt, std::move(counted.diagnostics)};
        if (*counted.value == 0) return WithCatalog(terrain, {}, resolver, limits, 0, actualMaximumCliffLevel);
        auto catalog = LoadTerrainMaterialCatalog(resolver, limits.materials);
        if (!catalog.Succeeded()) return PreviewResult{std::nullopt, std::move(catalog.diagnostics)};
        auto result = WithCatalog(terrain, *catalog.value, resolver, limits, *counted.value, actualMaximumCliffLevel);
        result.diagnostics.insert(result.diagnostics.begin(),
            std::make_move_iterator(catalog.diagnostics.begin()), std::make_move_iterator(catalog.diagnostics.end()));
        return result;
    });
}

Result<TerrainCliffPreview> BuildTerrainCliffPreview(const MapData::TerrainGridSnapshot& terrain,
    const TerrainMaterialCatalog& catalog, const Resources::ResourceResolver& resolver,
    const TerrainCliffPreviewLimits& limits, std::optional<std::uint8_t> actualMaximumCliffLevel) {
    return Checked([&] {
        auto counted = CountCandidates(terrain, limits);
        if (!counted.Succeeded()) return PreviewResult{std::nullopt, std::move(counted.diagnostics)};
        return WithCatalog(terrain, catalog, resolver, limits, *counted.value, actualMaximumCliffLevel);
    });
}

} // namespace War3Map::Render
