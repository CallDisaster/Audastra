#include "War3MapCore/Render/BuildingSplatPreview.h"

#include "War3MapCore/Render/TerrainPicking.h"
#include "War3MapCore/Resources/ImportTable.h"
#include "BuildingSplatProjection.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <exception>
#include <map>
#include <set>
#include <tuple>
#include <utility>

namespace War3Map::Render {
namespace {
constexpr std::size_t MaximumBytes = 64 * 1024 * 1024;
constexpr std::uint32_t MaximumVertices = 2097152, MaximumIndices = 6291456;
constexpr std::uint32_t MaximumGroups = 1048576, MaximumModels = 16384;
using OwnerKey = std::tuple<MapData::PlacementFile, std::uint32_t, std::string, std::int32_t>;

bool Text(std::string_view value) { return !value.empty() && value.size() <= 4096; }
bool LimitsValid(const BuildingSplatPreviewLimits& limits) {
    return limits.maxAssetBytes <= MaximumBytes && limits.maxFrameBytes <= MaximumBytes
        && limits.maxIssueBytes <= 64 * 1024 && limits.maxIssues <= 128
        && std::isfinite(limits.maximumCellSize) && limits.maximumCellSize >= 1 && limits.maximumCellSize <= 128
        && limits.previewHeightOffset == 0;
}
bool Charge(std::size_t& bytes, std::size_t needed, std::size_t maximum) {
    if (bytes > maximum || needed > maximum - bytes) return false;
    bytes += needed; return true;
}
std::size_t IssueBytes(const BuildingSplatPreviewIssue& issue) {
    return sizeof(issue) + issue.splatKey.size() + issue.code.size() + issue.reason.size()
        + (issue.owner ? issue.owner->rawcode.size() : 0);
}
template<class Output> void Issue(Output& output, BuildingSplatPreviewIssue issue,
    const BuildingSplatPreviewLimits& limits, std::size_t& bytes) {
    if (output.issues.size() >= limits.maxIssues || !Charge(bytes, IssueBytes(issue), limits.maxIssueBytes)) {
        ++output.omittedIssues; return;
    }
    output.issues.push_back(std::move(issue));
}
BuildingSplatPreviewIssue RowIssue(const BuildingSplatPreviewInstance& input,
    std::string code, std::string reason) {
    return {input.row.owner, input.stablePlacementId, input.row.splatKey, std::move(code), std::move(reason), true};
}
std::size_t SourceBytes(const BuildingSplatPreviewTexture& source) {
    return sizeof(source) + source.path.size() + source.sourceLayer.size() + source.digest.size();
}
bool ImageValid(const BuildingSplatPreviewTexture& texture, std::string_view canonical) {
    if (!texture.image || !texture.image->width || !texture.image->height
        || texture.image->width > 2048 || texture.image->height > 2048) return false;
    const auto path = Resources::CanonicalImportPath(texture.image->path);
    return path.Succeeded() && *path.value == canonical
        && texture.image->rgba.size() == static_cast<std::size_t>(texture.image->width) * texture.image->height * 4;
}
std::optional<std::size_t> OriginBytes(const Services::BuildingSplatRow& row) {
    if (row.origins.size() > 64 || row.splatKey.size() > 4096 || row.reason.size() > 4096) return {};
    std::size_t bytes = row.owner.rawcode.size() + row.splatKey.size();
    for (const auto& origin : row.origins) {
        if (origin.field.size() > 4096 || origin.objectRawcode.size() > 4096
            || origin.sourceId.size() > 4096 || origin.sourcePath.size() > 4096) return {};
        bytes += sizeof(origin) + origin.field.size() + origin.objectRawcode.size()
            + origin.sourceId.size() + origin.sourcePath.size();
    }
    return bytes;
}
bool OriginsComplete(const Services::BuildingSplatRow& row) {
    for (const auto field : {"uubs", "uclr", "uclg", "uclb"}) {
        if (!std::any_of(row.origins.begin(), row.origins.end(), [&](const auto& origin) {
            return origin.field == field && origin.objectRawcode.size() == 4
                && Text(origin.sourceId) && Text(origin.sourcePath);
        })) return false;
    }
    return true;
}
bool GridValid(const MapData::TerrainGridSnapshot& terrain) {
    const auto width = terrain.header.width, height = terrain.header.height;
    if (terrain.header.version != 11 || width < 2 || height < 2 || width > 2049 || height > 2049
        || terrain.vertices.size() != static_cast<std::size_t>(width) * height
        || terrain.tiles.size() != static_cast<std::size_t>(width - 1) * (height - 1)) return false;
    for (std::size_t axis = 0; axis < 2; ++axis) {
        const auto count = axis == 0 ? width : height;
        const auto end = static_cast<float>(static_cast<double>(terrain.header.offset[axis])
            + static_cast<double>(count - 1) * MapData::TerrainGridSnapshot::tileSize);
        if (!std::isfinite(terrain.header.offset[axis]) || std::abs(terrain.header.offset[axis]) > 16777216
            || terrain.minimum[axis] != terrain.header.offset[axis] || terrain.maximum[axis] != end) return false;
    }
    return true;
}
// 检查覆盖的全部tile，而不是仅网格采样点；不以四角无水证明内部也无水/悬崖。
std::optional<std::pair<std::string, std::string>> SurfaceIssue(
    const MapData::TerrainGridSnapshot& terrain, const Services::BuildingSplatFootprint& footprint) {
    for (std::size_t axis = 0; axis < 2; ++axis) {
        if (footprint.minimum[axis] < terrain.minimum[axis] || footprint.maximum[axis] > terrain.maximum[axis])
            return {{"BUILDING_SPLAT_OUTSIDE", "完整footprint超出显示地形；首版不裁切或回退Z0。"}};
    }
    const auto coordinate = [&](float value, std::size_t axis) {
        return (static_cast<double>(value) - terrain.header.offset[axis]) / MapData::TerrainGridSnapshot::tileSize;
    };
    const auto left = static_cast<std::uint32_t>(std::floor(coordinate(footprint.minimum[0], 0)));
    const auto bottom = static_cast<std::uint32_t>(std::floor(coordinate(footprint.minimum[1], 1)));
    const auto right = std::min(terrain.header.width - 2,
        static_cast<std::uint32_t>(std::floor(coordinate(footprint.maximum[0], 0))));
    const auto top = std::min(terrain.header.height - 2,
        static_cast<std::uint32_t>(std::floor(coordinate(footprint.maximum[1], 1))));
    for (auto y = bottom; y <= top; ++y) for (auto x = left; x <= right; ++x) {
        const auto& tile = terrain.tiles[static_cast<std::size_t>(y) * (terrain.header.width - 1) + x];
        const std::array<std::uint32_t, 4> expected{y * terrain.header.width + x,
            y * terrain.header.width + x + 1, (y + 1) * terrain.header.width + x, (y + 1) * terrain.header.width + x + 1};
        if (tile.vertices != expected)
            return {{"BUILDING_SPLAT_TERRAIN_INVALID", "访问tile的顶点索引不符合冻结显示高度网格。"}};
        auto low = static_cast<std::uint8_t>(15), high = static_cast<std::uint8_t>(0);
        bool ramp = false;
        for (const auto index : tile.vertices) {
            const auto& vertex = terrain.vertices[index];
            const auto expectedX = static_cast<float>(static_cast<double>(terrain.header.offset[0])
                + static_cast<double>(index % terrain.header.width) * MapData::TerrainGridSnapshot::tileSize);
            const auto expectedY = static_cast<float>(static_cast<double>(terrain.header.offset[1])
                + static_cast<double>(index / terrain.header.width) * MapData::TerrainGridSnapshot::tileSize);
            if (!std::all_of(vertex.position.begin(), vertex.position.end(), [](float value) {
                return std::isfinite(value) && std::abs(value) <= 16777216;
            }) || std::abs(vertex.position[2]) > 65536 || vertex.position[0] != expectedX || vertex.position[1] != expectedY
                || vertex.extendedTextureLayout || (vertex.groundHeightRaw & 0x8000) != 0
                || vertex.UnknownWaterFlags() || vertex.UnknownTextureFlags() || vertex.CliffLevel() == 15)
                return {{"BUILDING_SPLAT_TERRAIN_INVALID", "访问地形含无效高度或未知标志；未猜表面。"}};
            if (vertex.IsBoundary() || vertex.IsMapEdge())
                return {{"BUILDING_SPLAT_BOUNDARY_UNSUPPORTED", "footprint覆盖边界标记；首版未实现原版边界裁剪。"}};
            if (vertex.HasWater())
                return {{"BUILDING_SPLAT_WATER_UNSUPPORTED", "footprint覆盖水面标记；首版未选择或混合水面高度。"}};
            low = std::min(low, vertex.CliffLevel()); high = std::max(high, vertex.CliffLevel());
            ramp = ramp || vertex.IsRamp();
        }
        if (low != high && !ramp)
            return {{"BUILDING_SPLAT_CLIFF_UNSUPPORTED", "footprint覆盖非坡道层高断面；首版未实现悬崖投影。"}};
    }
    return {};
}
std::shared_ptr<AnimatedModelScene> NewScene(const AnimatedModelScene& source) {
    auto scene = std::make_shared<AnimatedModelScene>(source);
    scene->sceneVersion = NextAnimationSerial();
    scene->geometry.indices.clear(); scene->geometry.layers.clear(); scene->geometry.meshes.clear();
    scene->geometry.models.clear(); scene->geometry.modelBounds.clear(); scene->meshes.clear(); scene->models.clear();
    scene->vertices.clear(); scene->geometry.vertices.clear();
    return scene;
}
}

Result<BuildingSplatPreviewAssets> BuildBuildingSplatPreviewAssets(
    std::shared_ptr<const Services::BuildingSplatCatalog> catalog,
    std::span<const BuildingSplatPreviewTexture> textures, const AnimationSession& session,
    const BuildingSplatPreviewLimits& limits) try {
    using Output = Result<BuildingSplatPreviewAssets>;
    if (!LimitsValid(limits)) return Output::Failure("BuildingSplat.Preview", "BUILDING_SPLAT_PREVIEW_LIMIT", "贴画预览预算或显式高度偏移无效。");
    if (!catalog || !Text(catalog->identitySha256) || !Text(catalog->tableSha256)
        || catalog->definitions.size() > 4096 || !session.generation || !Text(session.sourceIdentity) || textures.size() > 4096)
        return Output::Failure("BuildingSplat.Preview", "BUILDING_SPLAT_PREVIEW_SOURCE", "贴画目录或纹理来源身份无效。");
    BuildingSplatPreviewAssets output; output.catalog = std::move(catalog); output.maximumCellSize = limits.maximumCellSize;
    auto scene = std::make_shared<AnimatedModelScene>();
    scene->sceneVersion = NextAnimationSerial(); scene->generation = session.generation;
    scene->documentRevision = session.documentRevision; scene->sourceIdentity = session.sourceIdentity;
    std::map<std::string, const BuildingSplatPreviewTexture*> supplied;
    for (const auto& texture : textures) {
        auto canonical = Resources::CanonicalImportPath(texture.path);
        if (!canonical.Succeeded() || !Text(texture.sourceLayer) || !Text(texture.digest)
            || !supplied.emplace(*canonical.value, &texture).second)
            return Output::Failure("BuildingSplat.Preview", "BUILDING_SPLAT_TEXTURE_SOURCE", "纹理来源缺失、路径不安全或同路径存在重复来源；未回落。");
    }
    std::map<std::string, std::uint32_t> installed;
    std::size_t issueBytes = 0;
    for (const auto& [key, definition] : output.catalog->definitions) {
        const auto suppliedTexture = supplied.find(definition.texturePath);
        if (suppliedTexture == supplied.end()) continue; // 未使用的目录项不加载、不虚构白贴图。
        const auto& texture = *suppliedTexture->second;
        if (!ImageValid(texture, definition.texturePath)) {
            Issue(output, {{}, 0, key, "BUILDING_SPLAT_TEXTURE_INVALID", "实际纹理RGBA尺寸、路径或载荷无效。", true}, limits, issueBytes); continue;
        }
        const double width = 2.0 * definition.scale;
        const double segments = std::ceil(width / limits.maximumCellSize);
        if (!std::isfinite(width) || width <= 0 || segments < 1 || segments > 255) {
            Issue(output, {{}, 0, key, "BUILDING_SPLAT_GRID_BUDGET", "实际表尺寸的固定网格超出单网格16位索引预算。", true}, limits, issueBytes); continue;
        }
        const auto count = static_cast<std::uint32_t>(segments);
        const auto vertexCount = (count + 1) * (count + 1), indexCount = count * count * 6;
        const auto existingTexture = installed.find(definition.texturePath);
        const std::size_t textureBytes = existingTexture == installed.end()
            ? 2 * texture.image->rgba.size() + SourceBytes(texture) + sizeof(ModelTextureRgba) + definition.texturePath.size() : 0;
        const std::size_t needed = static_cast<std::size_t>(vertexCount)
                * (sizeof(War3RenderSkinnedModelVertexV2) + sizeof(War3RenderModelVertexV2))
            + static_cast<std::size_t>(indexCount) * sizeof(std::uint16_t) + sizeof(BuildingSplatPreviewAssets::Grid) + key.size()
            + sizeof(War3RenderModelLayerV2) + sizeof(War3RenderModelMeshV2) + sizeof(War3RenderSkinnedModelMeshV2)
            + sizeof(War3RenderModelV2) + sizeof(War3RenderSkinnedModelV2) + 6 * sizeof(float) + textureBytes;
        if (vertexCount > MaximumVertices - scene->vertices.size() || indexCount > MaximumIndices - scene->geometry.indices.size()
            || !Charge(output.logicalBytes, needed, limits.maxAssetBytes)) {
            Issue(output, {{}, 0, key, "BUILDING_SPLAT_ASSET_BUDGET", "本次真实网格/纹理owned字节预算不足，省略此key。", true}, limits, issueBytes); continue;
        }
        std::uint32_t textureIndex = 0;
        if (existingTexture == installed.end()) {
            textureIndex = static_cast<std::uint32_t>(scene->geometry.textures.size());
            installed.emplace(definition.texturePath, textureIndex);
            auto image = *texture.image; image.path = definition.texturePath;
            scene->geometry.textures.push_back(std::move(image)); output.sources.push_back(texture);
        } else textureIndex = existingTexture->second;
        BuildingSplatPreviewAssets::Grid grid{key, static_cast<std::uint32_t>(scene->vertices.size()),
            static_cast<std::uint32_t>(scene->geometry.indices.size()), indexCount, vertexCount, count, count,
            textureIndex, static_cast<float>(width), static_cast<float>(width)};
        for (std::uint32_t y = 0; y <= count; ++y) for (std::uint32_t x = 0; x <= count; ++x) {
            const auto u = static_cast<float>(x) / static_cast<float>(count), v = static_cast<float>(y) / static_cast<float>(count);
            const War3RenderModelVertexV2 vertex{(u - .5f) * grid.fullWidth, (v - .5f) * grid.fullHeight, 0, 0, 0, 1, u, 1 - v};
            scene->vertices.push_back({vertex, y * (count + 1) + x}); scene->geometry.vertices.push_back(vertex);
        }
        for (std::uint32_t y = 0; y < count; ++y) for (std::uint32_t x = 0; x < count; ++x) {
            const auto sw = y * (count + 1) + x, se = sw + 1, nw = sw + count + 1, ne = nw + 1;
            for (const auto index : {sw, se, nw, se, ne, nw}) scene->geometry.indices.push_back(static_cast<std::uint16_t>(index));
        }
        const auto model = static_cast<std::uint32_t>(output.grids.size());
        scene->geometry.layers.push_back({textureIndex, 2, 5, 1}); // 源alpha混合/no-write；没有宣称原版GE4。
        const War3RenderModelMeshV2 mesh{grid.firstIndex, indexCount, grid.firstVertex, model, 1};
        scene->geometry.meshes.push_back(mesh); scene->geometry.models.push_back({model, 1});
        scene->meshes.push_back({mesh, 0, vertexCount}); scene->models.push_back({{model, 1}, vertexCount});
        scene->geometry.modelBounds.push_back({-grid.fullWidth / 2, -grid.fullHeight / 2, 0, grid.fullWidth / 2, grid.fullHeight / 2, 0});
        output.grids.push_back(std::move(grid));
    }
    // 输出来源alias指向已冻结scene里的RGBA，不重复保活调用方的整份解码副本。
    for (auto& source : output.sources) {
        const auto canonical = Resources::CanonicalImportPath(source.path);
        const auto index = installed.at(*canonical.value);
        source.image = std::shared_ptr<const ModelTextureRgba>(scene, &std::as_const(scene->geometry.textures)[index]);
    }
    output.scene = std::move(scene); return Output::Success(std::move(output));
} catch (const std::exception&) {
    return Result<BuildingSplatPreviewAssets>::Failure("BuildingSplat.Preview", "BUILDING_SPLAT_ASSETS_FAILED", "贴画资产构建失败，未返回部分候选。");
}

Result<BuildingSplatPreviewFrame> BuildBuildingSplatPreviewFrame(
    const BuildingSplatPreviewAssets& assets, std::span<const BuildingSplatPreviewInstance> instances,
    const BuildingSplatPreviewBinding& binding, std::shared_ptr<const MapData::TerrainGridSnapshot> displayedTerrain,
    std::uint64_t contentVersion, const BuildingSplatPreviewLimits& limits,
    const BuildingSplatPreviewFrame* previous) try {
    using Output = Result<BuildingSplatPreviewFrame>;
    if (!LimitsValid(limits)) return Output::Failure("BuildingSplat.Preview", "BUILDING_SPLAT_PREVIEW_LIMIT", "贴画预览预算无效。");
    if (!assets.catalog || !assets.scene || !assets.scene->generation || !contentVersion
        || assets.catalog->identitySha256 != binding.catalogIdentity || !Text(binding.terrainIdentity)
        || !binding.version.document.generation || !Text(binding.version.document.documentId) || !Text(binding.version.document.sourceSha256))
        return Output::Failure("BuildingSplat.Preview", "BUILDING_SPLAT_FRAME_STALE", "贴画目录、地形或地图帧身份无效，未提交部分候选。");
    if (assets.logicalBytes > limits.maxAssetBytes || assets.maximumCellSize > limits.maximumCellSize)
        return Output::Failure("BuildingSplat.Preview", "BUILDING_SPLAT_ASSET_BUDGET", "常驻资产不符合此次收紧的网格或owned字节预算。");
    BuildingSplatPreviewFrame output; output.binding = binding; output.terrain = std::move(displayedTerrain); output.requested = instances.size();
    std::size_t issueBytes = 0;
    Issue(output, {{}, 0, {}, "BUILDING_SPLAT_PREVIEW_APPROXIMATE",
        "贴画已按显示地形三角面裁切；源alpha混合仍为PreviewApproximate，原版水/悬崖投影、GE4、depth bias与跨pass排序尚未全部证明。", false}, limits, issueBytes);
    std::map<std::string, const BuildingSplatPreviewAssets::Grid*> grids;
    for (const auto& grid : assets.grids) {
        if (!grid.vertexCount || !grid.indexCount || grid.firstVertex > assets.scene->vertices.size()
            || grid.vertexCount > assets.scene->vertices.size() - grid.firstVertex
            || grid.firstIndex > assets.scene->geometry.indices.size() || grid.indexCount > assets.scene->geometry.indices.size() - grid.firstIndex
            || grid.texture >= assets.scene->geometry.textures.size() || !grids.emplace(grid.key, &grid).second)
            return Output::Failure("BuildingSplat.Preview", "BUILDING_SPLAT_ASSET_INVALID", "贴画常驻网格范围或key身份无效。");
    }
    auto frame = std::make_shared<AnimationFrame>(); frame->contentVersion = contentVersion;
    auto scene = NewScene(*assets.scene);
    std::set<OwnerKey> owners; std::set<std::uint64_t> stableIds;
    const bool terrainValid = output.terrain && GridValid(*output.terrain);
    for (const auto& input : instances) {
        const auto& row = input.row;
        if (row.version != binding.version || row.owner.file != MapData::PlacementFile::Units || row.owner.rawcode.size() != 4
            || !input.stablePlacementId || !OriginBytes(row))
            return Output::Failure("BuildingSplat.Preview", "BUILDING_SPLAT_OWNER_STALE", "贴画行、stablePlacementId或来源修订无效。");
        if (row.availability == Services::BuildingSplatAvailability::Absent) { ++output.absent; continue; }
        const auto grid = grids.find(row.splatKey);
        if (row.availability != Services::BuildingSplatAvailability::Available || !row.footprint || !OriginsComplete(row) || grid == grids.end()) {
            ++output.unavailable;
            Issue(output, RowIssue(input, "BUILDING_SPLAT_DATA_UNAVAILABLE", row.reason.empty() ? "此key缺合法footprint、字段来源或真实纹理/常驻网格。" : row.reason), limits, issueBytes); continue;
        }
        const auto& shape = *row.footprint; const auto& layout = *grid->second;
        const auto spanMatches = [&](std::size_t axis, float size) {
            const double span = static_cast<double>(shape.maximum[axis]) - shape.minimum[axis];
            // min/max来自float位置±表Scale；仅容纳浮点落点误差，不接受任意缩小后的coverage。
            const double tolerance = 1e-4 + 4 * 1.1920928955078125e-7
                * (std::abs(static_cast<double>(shape.minimum[axis])) + std::abs(static_cast<double>(shape.maximum[axis])) + size);
            return std::abs(span - size) <= tolerance;
        };
        if (shape.fullWidth != layout.fullWidth || shape.fullHeight != layout.fullHeight
            || !shape.tableSizeIndependentOfModel || !std::isfinite(shape.fullWidth) || shape.fullWidth <= 0
            || !std::all_of(shape.minimum.begin(), shape.minimum.end(), [](float v) { return std::isfinite(v) && std::abs(v) <= 16777216; })
            || !std::all_of(shape.maximum.begin(), shape.maximum.end(), [](float v) { return std::isfinite(v) && std::abs(v) <= 16777216; })
            || shape.maximum[0] <= shape.minimum[0] || shape.maximum[1] <= shape.minimum[1]
            || !spanMatches(0, layout.fullWidth) || !spanMatches(1, layout.fullHeight)) {
            ++output.unavailable; Issue(output, RowIssue(input, "BUILDING_SPLAT_FOOTPRINT_INVALID", "footprint不符合当前key独立表尺寸或有限坐标。"), limits, issueBytes); continue;
        }
        if (!terrainValid) {
            ++output.unavailable; Issue(output, RowIssue(input, "BUILDING_SPLAT_TERRAIN_UNAVAILABLE", "没有合法冻结显示地形；未回退dooZ或Z0。"), limits, issueBytes); continue;
        }
        const std::size_t baseBytes = sizeof(BuildingSplatPreviewOwner) + *OriginBytes(row)
            + sizeof(War3RenderSkinnedModelInstanceV2) + sizeof(float) + sizeof(War3RenderGroupMatrixV2)
            + sizeof(OwnerKey) + 96 + sizeof(War3RenderModelLayerV2) + sizeof(War3RenderModelMeshV2)
            + sizeof(War3RenderSkinnedModelMeshV2) + sizeof(War3RenderModelV2) + sizeof(War3RenderSkinnedModelV2) + 6*sizeof(float);
        if (frame->groups.size() == MaximumGroups || scene->models.size() == MaximumModels
            || output.logicalBytes > limits.maxFrameBytes || baseBytes > limits.maxFrameBytes-output.logicalBytes) {
            ++output.omittedBudget; Issue(output, RowIssue(input,"BUILDING_SPLAT_FRAME_BUDGET","投影owned帧预算不足，省略此实例。"),limits,issueBytes); continue;
        }
        if (const auto reason = SurfaceIssue(*output.terrain, shape)) {
            ++output.unavailable; Issue(output,RowIssue(input,reason->first,reason->second),limits,issueBytes); continue;
        }
        const auto remaining = limits.maxFrameBytes-output.logicalBytes-baseBytes;
        // 包含完整投影临时量；不先分配几何再核算其字节。
        constexpr auto vertexBytes=sizeof(War3RenderSkinnedModelVertexV2)+2*sizeof(War3RenderModelVertexV2);
        const auto projected=Detail::ProjectBuildingSplat(*output.terrain,shape,limits.maximumCellSize,
            std::min<std::size_t>(MaximumVertices-scene->vertices.size(),remaining/vertexBytes),
            std::min<std::size_t>(MaximumIndices-scene->geometry.indices.size(),remaining/(2*sizeof(std::uint16_t))));
        if (!projected.Succeeded() || !projected.value) {
            ++output.omittedBudget;
            const auto& diagnostic=projected.diagnostics.back();
            Issue(output,RowIssue(input,diagnostic.code,diagnostic.message),limits,issueBytes); continue;
        }
        const auto& geometry=*projected.value;
        const auto needed=baseBytes+geometry.vertices.size()*vertexBytes+geometry.indices.size()*2*sizeof(std::uint16_t);
        if (geometry.vertices.empty() || !Charge(output.logicalBytes,needed,limits.maxFrameBytes)) {
            ++output.omittedBudget; Issue(output,RowIssue(input,"BUILDING_SPLAT_FRAME_BUDGET","完整投影owned帧预算不足，省略此实例。"),limits,issueBytes); continue;
        }
        const auto firstGroup=static_cast<std::uint32_t>(frame->groups.size());
        // 地形世界坐标直接进入几何，不用 local+center 再舍入，也不按dooZ抬高。
        frame->groups.push_back({{1,0,0,0,1,0,0,0,1,0,0,0}});
        if (!owners.emplace(row.owner.file, row.owner.index, row.owner.rawcode, row.owner.creationId).second
            || !stableIds.emplace(input.stablePlacementId).second)
            return Output::Failure("BuildingSplat.Preview", "BUILDING_SPLAT_OWNER_DUPLICATE", "本帧有效贴画owner或registry身份重复。");
        const auto model=static_cast<std::uint32_t>(scene->models.size()), rendered=static_cast<std::uint32_t>(frame->instances.size());
        frame->instances.push_back({{model, {1,0,0,0,0,1,0,0,0,0,1,0},
            {static_cast<float>(shape.color[0]) / 255, static_cast<float>(shape.color[1]) / 255, static_cast<float>(shape.color[2]) / 255}},
            firstGroup, static_cast<std::uint32_t>(frame->alphas.size())});
        frame->alphas.push_back(static_cast<float>(shape.color[3]) / 255);
        output.owners.push_back({row.owner,input.stablePlacementId,row.inputIndex,rendered,firstGroup,1,row.splatKey,row.origins});
        const auto firstVertex=static_cast<std::uint32_t>(scene->vertices.size());
        const auto firstIndex=static_cast<std::uint32_t>(scene->geometry.indices.size());
        std::array<float,6> bounds{shape.minimum[0],shape.minimum[1],65536,shape.maximum[0],shape.maximum[1],-65536};
        for (const auto& vertex:geometry.vertices) {
            scene->vertices.push_back({vertex,0}); scene->geometry.vertices.push_back(vertex);
            bounds[2]=std::min(bounds[2],vertex.z); bounds[5]=std::max(bounds[5],vertex.z);
        }
        for (const auto index:geometry.indices) scene->geometry.indices.push_back(index);
        scene->geometry.layers.push_back({layout.texture,2,5,1});
        const War3RenderModelMeshV2 mesh{firstIndex,static_cast<std::uint32_t>(geometry.indices.size()),firstVertex,model,1};
        scene->geometry.meshes.push_back(mesh); scene->geometry.models.push_back({model,1});
        scene->meshes.push_back({mesh,0,1}); scene->models.push_back({{model,1},1});
        scene->geometry.modelBounds.push_back(bounds); ++output.displayed;
    }
    bool reuse = previous && previous->scene && previous->frame
        && previous->scene->generation == assets.scene->generation && previous->scene->sourceIdentity == assets.scene->sourceIdentity
        && previous->scene->geometry.textures.SharesStorageWith(assets.scene->geometry.textures)
        && previous->binding.catalogIdentity == binding.catalogIdentity && previous->owners.size() == output.owners.size()
        && previous->frame->instances.size() == previous->owners.size()
        && previous->scene->vertices.size()==scene->vertices.size()
        && previous->scene->geometry.indices==scene->geometry.indices;
    if (reuse) for (std::size_t i = 0; i < output.owners.size(); ++i) {
        if (previous->owners[i].splatKey != output.owners[i].splatKey
            || previous->owners[i].stablePlacementId != output.owners[i].stablePlacementId
            || previous->frame->instances[i].instance.model != frame->instances[i].instance.model) { reuse = false; break; }
    }
    if (reuse) for (std::size_t i=0;i<scene->vertices.size();++i)
        if (std::memcmp(&previous->scene->vertices[i],&scene->vertices[i],sizeof(War3RenderSkinnedModelVertexV2))!=0) { reuse=false; break; }
    if (reuse) output.scene = previous->scene;
    else {
        if (output.owners.empty()) { // canonical空roster不携带未使用的静态资产。
            scene->vertices.clear(); scene->geometry.vertices.clear(); scene->geometry.textures.clear();
        }
        output.scene = std::move(scene);
    }
    frame->sceneVersion = output.scene->sceneVersion; frame->documentRevision = output.scene->documentRevision;
    output.frame = std::move(frame); return Output::Success(std::move(output));
} catch (const std::exception&) {
    return Result<BuildingSplatPreviewFrame>::Failure("BuildingSplat.Preview", "BUILDING_SPLAT_FRAME_FAILED", "贴画帧构建失败，未返回部分候选。");
}
} // namespace War3Map::Render
