#include "War3MapCore/Render/TerrainGroundMesh.h"

#include <algorithm>
#include <cmath>
#include <new>
#include <stdexcept>

namespace War3Map::Render {
namespace {

using MapData::TerrainGridSnapshot;
constexpr TerrainGroundMeshLimits kHardLimits{};
constexpr const char* kSubsystem = "TerrainGroundMesh";
constexpr std::size_t kMaximumGroundEntries = 256;
using AtlasFlags = std::array<bool, kMaximumGroundEntries>;
using TextureSlots = std::array<std::uint32_t, kMaximumGroundEntries>;
using BatchCounters = std::array<std::uint32_t, kHardLimits.maxTextures + 1>;
using OverlayCounters = std::array<std::uint32_t, kMaximumGroundEntries>;
// 固定材质和分组 scratch 与原格数量无关，也必须在分配前计入紧缩预算。
constexpr std::size_t kFixedScratchBytes = 2 * sizeof(AtlasFlags) + sizeof(TextureSlots) + 3 * sizeof(BatchCounters)
    + 3 * sizeof(OverlayCounters);
static_assert(sizeof(TerrainGroundVertex) == sizeof(float) * 8);
constexpr std::uint32_t Reason(TerrainGroundOmissionReason reason) { return static_cast<std::uint32_t>(reason); }

bool ValidLimits(const TerrainGroundMeshLimits& limits) {
    return limits.maxVertices <= kHardLimits.maxVertices && limits.maxIndices <= kHardLimits.maxIndices
        && limits.maxTextures <= kHardLimits.maxTextures && limits.maxTextureDimension <= kHardLimits.maxTextureDimension
        && limits.maxTextureBytes <= kHardLimits.maxTextureBytes && limits.maxRgbaBytes <= kHardLimits.maxRgbaBytes
        && limits.maxTextureChainBytes <= kHardLimits.maxTextureChainBytes
        && limits.maxBatches <= kHardLimits.maxBatches && limits.maxWorkingBytes <= kHardLimits.maxWorkingBytes;
}

bool ValidAtlas(const TerrainAtlasSnapshot& atlas) {
    const auto width = atlas.image.width, height = atlas.image.height;
    if ((atlas.columns != 4 && atlas.columns != 8) || width == 0 || height == 0
        || width > kHardLimits.maxTextureDimension || height > kHardLimits.maxTextureDimension
        || atlas.sourceWidth != width || atlas.sourceHeight != height || height % 4 != 0
        || width != height / 4 * atlas.columns || atlas.cells.size() != atlas.columns * 4
        || !atlas.image.Valid() || atlas.image.rgba.size() > kHardLimits.maxTextureBytes) return false;
    auto mipWidth = width, mipHeight = height;
    std::size_t retained = atlas.image.rgba.size();
    if (atlas.additionalMips.size() > 11) return false;
    for (const auto& mip : atlas.additionalMips) {
        if (mipWidth == 1 && mipHeight == 1) return false;
        mipWidth = std::max(1u, mipWidth / 2); mipHeight = std::max(1u, mipHeight / 2);
        if (mip.width != mipWidth || mip.height != mipHeight || !mip.Valid()
            || mip.rgba.size() > kHardLimits.maxTextureChainBytes - retained) return false;
        retained += mip.rgba.size();
    }
    const auto cellSize = height / 4;
    for (std::uint32_t y = 0; y < 4; ++y) for (std::uint32_t x = 0; x < atlas.columns; ++x) {
        const auto& cell = atlas.cells[static_cast<std::size_t>(y) * atlas.columns + x];
        if (cell.column != x || cell.row != y || cell.x != x * cellSize || cell.y != y * cellSize
            || cell.width != cellSize || cell.height != cellSize) return false;
    }
    return true;
}

std::array<std::uint32_t, 2> VariationCell(std::uint32_t columns, std::uint8_t variation) {
    // 独立实现的经典基础变体映射；来源和不覆盖的 blend mask 见设计文档。
    if (columns == 4) return variation == 0 ? std::array<std::uint32_t, 2>{0, 0}
        : std::array<std::uint32_t, 2>{3, 3};
    if (variation < 16) return {4u + variation % 4u, variation / 4u};
    return variation == 16 ? std::array<std::uint32_t, 2>{3, 3}
        : std::array<std::uint32_t, 2>{0, 0};
}

std::array<float, 3> HeightColor(float z, float low, float high) {
    const auto ratio = high > low ? std::clamp((z - low) / (high - low), 0.0f, 1.0f) : 0.5f;
    return {0.18f + ratio * 0.38f, 0.30f + ratio * 0.38f, 0.23f + ratio * 0.31f};
}

// 原版荒芜 atlas 缺失或超出纹理预算时的显式近似，不用平坦高度色替代贴图。
constexpr std::array<float, 3> kBlightApproximationTint{0.87f, 0.78f, 0.91f};

// 过渡层：tile、材质索引与遮罩格号。
struct Overlay {
    std::uint32_t tile;
    std::uint8_t ground;
    std::uint8_t mask;
};

// 遮罩格号按原版 atlas 实测：东南 1、西南 2、东北 4、西北 8（图像下方为南）。
std::uint8_t GroundIndex(const TerrainGridSnapshot& terrain, std::size_t index,
    std::span<const std::uint8_t> effective) {
    return effective.empty() ? terrain.vertices[index].GroundTextureIndex() : effective[index];
}

std::uint8_t CornerMask(const TerrainGridSnapshot& terrain, const std::array<std::uint32_t, 4>& corners,
    std::uint8_t ground, std::span<const std::uint8_t> effective) {
    const auto at = [&](std::size_t corner) { return GroundIndex(terrain, corners[corner], effective) == ground; };
    return static_cast<std::uint8_t>((at(1) ? 1 : 0) | (at(0) ? 2 : 0) | (at(3) ? 4 : 0) | (at(2) ? 8 : 0));
}

std::uint8_t BlightMask(const TerrainGridSnapshot& terrain, const std::array<std::uint32_t, 4>& corners) {
    const auto at = [&](std::size_t corner) { return terrain.vertices[corners[corner]].IsBlight(); };
    return static_cast<std::uint8_t>((at(1) ? 1 : 0) | (at(0) ? 2 : 0) | (at(3) ? 4 : 0) | (at(2) ? 8 : 0));
}

struct CellRect {
    float left = 0, right = 0, top = 0, bottom = 0;
};

// cell 的 UV，以实际像素尺寸半 texel 内缩。
CellRect CellUv(const TerrainAtlasSnapshot& atlas, std::uint32_t column, std::uint32_t row) {
    const auto& cell = *atlas.Cell(column, row);
    const auto imageWidth = static_cast<float>(atlas.image.width), imageHeight = static_cast<float>(atlas.image.height);
    return {(static_cast<float>(cell.x) + 0.5f) / imageWidth, (static_cast<float>(cell.x + cell.width) - 0.5f) / imageWidth,
        (static_cast<float>(cell.y) + 0.5f) / imageHeight, (static_cast<float>(cell.y + cell.height) - 0.5f) / imageHeight};
}

// 按 SW/SE/NW/NE 追加一格四个顶点：坐标取原格，仅在荒芜 atlas 未使用时近似调色。
void EmitQuad(TerrainGroundMesh& output, const TerrainGridSnapshot& terrain, const std::array<std::uint32_t, 4>& corners,
    bool colored, bool approximateBlight, const CellRect& rect, float low, float high, const TerrainLighting& lighting) {
    const auto width = terrain.header.width;
    for (std::size_t corner = 0; corner < 4; ++corner) {
        const auto& position = terrain.vertices[corners[corner]].position;
        auto color = colored ? HeightColor(position[2], low, high) : std::array<float, 3>{1, 1, 1};
        if (!colored && approximateBlight && terrain.vertices[corners[corner]].IsBlight())
            for (std::size_t channel = 0; channel < 3; ++channel) color[channel] *= kBlightApproximationTint[channel];
        const auto shade = TerrainVertexShade(terrain, corners[corner] % width, corners[corner] / width, lighting);
        for (std::size_t channel = 0; channel < 3; ++channel) color[channel] *= shade[channel];
        output.vertices.push_back({position[0], position[1], position[2], color[0], color[1], color[2],
            corner % 2 == 0 ? rect.left : rect.right, corner < 2 ? rect.bottom : rect.top});
    }
}

void WriteFace(TerrainGroundMesh& output, std::uint32_t sw, std::size_t at) {
    const std::array<std::uint32_t, 6> face{sw, sw + 1, sw + 2, sw + 1, sw + 3, sw + 2};
    std::copy(face.begin(), face.end(), output.indices.begin() + static_cast<std::ptrdiff_t>(at));
}

bool ValidLighting(const TerrainLighting& lighting) {
    if (!lighting.enabled) return true;
    const auto& light = lighting.towardLight;
    const auto color = [](const std::array<float, 3>& value) {
        return std::all_of(value.begin(), value.end(), [](float c) { return std::isfinite(c) && c >= 0.0f && c <= 16.0f; });
    };
    return std::all_of(light.begin(), light.end(), [](float value) { return std::isfinite(value); })
        && light[2] > 0.0f && color(lighting.ambient) && color(lighting.diffuse);
}

Result<TerrainGroundMesh> Failure(const char* code, const char* message) {
    return Result<TerrainGroundMesh>::Failure(kSubsystem, code, message);
}

// 合成的水面 atlas：4×4 格、每格 32×32 像素，第 k 格 = 水面贴图（最近邻缩放；无贴图为白）× alpha k/15。
// 渲染 ABI 顶点没有 alpha，水面每个 quad 通过选格得到透明度。
constexpr std::uint32_t kWaterAlphaLevels = 16;
constexpr std::uint32_t kWaterCellSize = 32;

std::array<std::uint32_t, 4> WaterSourceTexel(const Image::DecodedImage* texture, std::uint32_t x, std::uint32_t y) {
    if (!texture) return {255, 255, 255, 255};
    const auto sx = x * texture->width / kWaterCellSize, sy = y * texture->height / kWaterCellSize;
    const auto* texel = texture->rgba.data() + (static_cast<std::size_t>(sy) * texture->width + sx) * 4;
    std::array<std::uint32_t, 4> source{};
    for (std::size_t channel = 0; channel < 4; ++channel) source[channel] = std::to_integer<std::uint32_t>(texel[channel]);
    return source;
}

std::uint32_t WaterTexelAlpha(std::uint32_t level, std::uint32_t sourceAlpha) {
    return (level * sourceAlpha + 7) / 15;
}

std::uint32_t WaterMaximumSourceAlpha(const Image::DecodedImage* texture) {
    std::uint32_t maximum = 0;
    // 必须与 WaterAtlas 共用 32×32 最近邻取样，不能用源图任意像素或角点替代。
    for (std::uint32_t y = 0; y < kWaterCellSize; ++y) for (std::uint32_t x = 0; x < kWaterCellSize; ++x)
        maximum = std::max(maximum, WaterSourceTexel(texture, x, y)[3]);
    return maximum;
}

std::shared_ptr<const TerrainAtlasSnapshot> WaterAtlas(const Image::DecodedImage* texture) {
    auto result = std::make_shared<TerrainAtlasSnapshot>();
    constexpr std::uint32_t cell = kWaterCellSize, size = cell * 4;
    result->source.canonicalPath = "（合成）水面贴图 × 透明度";
    result->columns = 4;
    result->sourceWidth = result->sourceHeight = size;
    result->validatedMipCount = 1;
    result->image = {size, size, std::vector<std::byte>(static_cast<std::size_t>(size) * size * 4)};
    for (std::uint32_t y = 0; y < size; ++y) for (std::uint32_t x = 0; x < size; ++x) {
        const auto level = (y / cell) * 4 + x / cell;
        auto* pixel = result->image.rgba.data() + (static_cast<std::size_t>(y) * size + x) * 4;
        const auto source = WaterSourceTexel(texture, x % cell, y % cell);
        for (std::size_t channel = 0; channel < 3; ++channel) pixel[channel] = static_cast<std::byte>(source[channel]);
        pixel[3] = static_cast<std::byte>(WaterTexelAlpha(level, source[3]));
    }
    for (std::uint32_t row = 0; row < 4; ++row) for (std::uint32_t column = 0; column < 4; ++column)
        result->cells.push_back({column, row, column * cell, row * cell, cell, cell});
    return result;
}

float WaterHeight(const TerrainGridSnapshot& terrain, std::uint32_t vertex, const TerrainWaterStyle& style) {
    return terrain.vertices[vertex].waterBaseHeight + style.heightOffset;
}

std::uint32_t WaterAlphaLevel(const TerrainGridSnapshot& terrain, const std::array<std::uint32_t, 4>& corners,
    const TerrainWaterStyle& style, std::array<std::array<float, 4>, 4>* colors = nullptr) {
    float alpha = 0;
    for (std::size_t corner = 0; corner < corners.size(); ++corner) {
        const auto index = corners[corner];
        const auto color = TerrainWaterColor(style, WaterHeight(terrain, index, style) - terrain.vertices[index].position[2]);
        if (colors) (*colors)[corner] = color;
        alpha += color[3] / 4.0f;
    }
    return std::min(kWaterAlphaLevels - 1,
        static_cast<std::uint32_t>(std::lround(std::clamp(alpha, 0.0f, 1.0f) * static_cast<float>(kWaterAlphaLevels - 1))));
}

} // namespace

std::array<float, 4> TerrainWaterColor(const TerrainWaterStyle& style, float depth) {
    // 深度区间常量来自社区对原版水面着色的逆向结果（浅水起始 10、深水起点 64、最深 72）。
    constexpr float shallowStart = 10.0f, deepStart = 64.0f, deepEnd = 72.0f;
    const auto mix = [](const std::array<std::uint8_t, 4>& low, const std::array<std::uint8_t, 4>& high, float t) {
        std::array<float, 4> color{};
        for (std::size_t channel = 0; channel < 4; ++channel)
            // 保持端点及区间边界，避免双乘加把满强度通道舍入成 1+1ULP。
            color[channel] = std::lerp(static_cast<float>(low[channel]), static_cast<float>(high[channel]), t) / 255.0f;
        return color;
    };
    if (!std::isfinite(depth)) depth = 0;
    if (depth <= deepStart)
        return mix(style.shallowMinimum, style.shallowMaximum,
            std::max(depth - shallowStart, 0.0f) / (deepStart - shallowStart));
    return mix(style.deepMinimum, style.deepMaximum, std::min(depth - deepStart, deepEnd - deepStart) / (deepEnd - deepStart));
}

std::array<float, 3> TerrainVertexShade(const TerrainGridSnapshot& terrain, std::uint32_t x, std::uint32_t y,
    const TerrainLighting& lighting) {
    const auto width = terrain.header.width, height = terrain.header.height;
    if (!lighting.enabled || width < 2 || height < 2 || x >= width || y >= height) return {1.0f, 1.0f, 1.0f};
    const auto z = [&](std::uint32_t column, std::uint32_t row) {
        return terrain.vertices[static_cast<std::size_t>(row) * width + column].position[2];
    };
    // 中心差分；边缘退化为单侧差分。原格间距固定 128。
    const auto west = x > 0 ? x - 1 : x, east = x + 1 < width ? x + 1 : x;
    const auto south = y > 0 ? y - 1 : y, north = y + 1 < height ? y + 1 : y;
    const auto slopeX = (z(east, y) - z(west, y)) / (static_cast<float>(east - west) * 128.0f);
    const auto slopeY = (z(x, north) - z(x, south)) / (static_cast<float>(north - south) * 128.0f);
    const auto normalLength = std::sqrt(slopeX * slopeX + slopeY * slopeY + 1.0f);
    const auto& light = lighting.towardLight;
    const auto lightLength = std::sqrt(light[0] * light[0] + light[1] * light[1] + light[2] * light[2]);
    const auto facing = std::max((-slopeX * light[0] - slopeY * light[1] + light[2]) / (normalLength * lightLength), 0.0f);
    std::array<float, 3> shade{};
    for (std::size_t channel = 0; channel < 3; ++channel)
        shade[channel] = std::clamp(lighting.ambient[channel] + lighting.diffuse[channel] * facing, 0.0f, 1.0f);
    return shade;
}

Result<TerrainGroundMesh> BuildTerrainGroundMesh(const TerrainGridSnapshot& terrain,
    std::span<const std::shared_ptr<const TerrainAtlasSnapshot>> groundAtlases,
    const TerrainGroundMeshLimits& limits, const TerrainLighting& lighting, const TerrainWaterStyle* waterStyle,
    std::span<const std::uint8_t> coveredTiles, std::shared_ptr<const TerrainAtlasSnapshot> blightAtlas,
    std::span<const std::uint8_t> effectiveGroundIndices, TerrainGroundBudgetPolicy budgetPolicy) {
    try {
        if (!ValidLimits(limits)) return Failure("TERRAIN_GROUND_LIMITS", "调用方不能放宽地表网格硬预算。");
        if (budgetPolicy != TerrainGroundBudgetPolicy::Strict && budgetPolicy != TerrainGroundBudgetPolicy::PreserveBasePreview)
            return Failure("TERRAIN_GROUND_POLICY", "地表网格预算策略无效。");
        if (!ValidLighting(lighting)) return Failure("TERRAIN_GROUND_LIGHTING", "光照方向或亮度参数无效。");
        if (waterStyle && (waterStyle->frameCount == 0 || waterStyle->frameCount > 256
            || waterStyle->frameIndex >= waterStyle->frameCount
            || (waterStyle->texture && !waterStyle->texture->Valid())))
            return Failure("TERRAIN_GROUND_WATER_FRAME", "水面帧索引或解码纹理无效。");
        if (waterStyle && !std::isfinite(waterStyle->heightOffset))
            return Failure("TERRAIN_GROUND_WATER_HEIGHT", "水面高度偏移不是有限数值。");
        const auto width = terrain.header.width, height = terrain.header.height;
        if (width < 2 || height < 2 || width > 2049 || height > 2049
            || terrain.header.groundTilesets.size() > kMaximumGroundEntries
            || groundAtlases.size() != terrain.header.groundTilesets.size()
            || static_cast<std::uint64_t>(width) * height != terrain.vertices.size()
            || static_cast<std::uint64_t>(width - 1) * (height - 1) != terrain.tiles.size())
            return Failure("TERRAIN_GROUND_GRID", "网格尺寸、顶点/tile 数量或地表材质对应关系无效。");
        if (!coveredTiles.empty() && coveredTiles.size() != terrain.tiles.size())
            return Failure("TERRAIN_GROUND_GRID", "悬崖覆盖标记数量与 tile 数量不一致。");
        if (!effectiveGroundIndices.empty()) {
            if (effectiveGroundIndices.size() != terrain.vertices.size())
                return Failure("TERRAIN_GROUND_SELECTION", "派生地面索引数量与原顶点数量不一致。");
            for (std::size_t i = 0; i < effectiveGroundIndices.size(); ++i)
                if (effectiveGroundIndices[i] >= groundAtlases.size()
                    && effectiveGroundIndices[i] != terrain.vertices[i].GroundTextureIndex())
                    return Failure("TERRAIN_GROUND_SELECTION", "派生地面不得新增 palette 外索引。");
            // 未覆盖顶点可原样携带未知 raw：继续下方逐 tile 的 invalidIndex 高度色回退。
        }
        const auto tileCount = terrain.tiles.size();
        const auto hiddenTileCount = static_cast<std::size_t>(std::count_if(coveredTiles.begin(), coveredTiles.end(),
            [](std::uint8_t covered) { return covered != 0; }));
        const auto baseQuadCount = tileCount - hiddenTileCount;
        const auto vertexCount = baseQuadCount * 4, indexCount = baseQuadCount * 6;
        const auto blockColumns = (width - 2) / TerrainGroundMesh::blockTileSize + 1;
        const auto blockRows = (height - 2) / TerrainGroundMesh::blockTileSize + 1;
        const auto blockCount = static_cast<std::size_t>(blockColumns) * blockRows;
        const auto fixedBytes = vertexCount * sizeof(TerrainGroundVertex) + indexCount * sizeof(std::uint32_t)
            + blockCount * sizeof(TerrainGroundBlock) + tileCount * (sizeof(std::uint32_t) + sizeof(std::uint8_t))
            + (hiddenTileCount ? tileCount * sizeof(std::uint8_t) + baseQuadCount * sizeof(std::uint32_t) : 0)
            + kFixedScratchBytes;
        if (vertexCount > limits.maxVertices || indexCount > limits.maxIndices || fixedBytes > limits.maxWorkingBytes)
            return Failure("TERRAIN_GROUND_BUDGET", "原格地表网格超出预算，请由宿主回退完整地图的高度预览。");

        std::array<float, 3> minimum{}, maximum{};
        minimum.fill(std::numeric_limits<float>::infinity());
        maximum.fill(-std::numeric_limits<float>::infinity());
        for (std::uint32_t y = 0; y < height; ++y) for (std::uint32_t x = 0; x < width; ++x) {
            const auto& position = terrain.vertices[static_cast<std::size_t>(y) * width + x].position;
            if (std::any_of(position.begin(), position.end(), [](float value) {
                    return !std::isfinite(value) || std::abs(value) > 10000000.0f;
                }) || position[0] != static_cast<float>(static_cast<double>(terrain.header.offset[0]) + x * 128.0)
                || position[1] != static_cast<float>(static_cast<double>(terrain.header.offset[1]) + y * 128.0))
                return Failure("TERRAIN_GROUND_GRID", "原格坐标不是有限规则地形，拒绝猜测或重建高度。");
            for (std::size_t axis = 0; axis < 3; ++axis) {
                minimum[axis] = std::min(minimum[axis], position[axis]);
                maximum[axis] = std::max(maximum[axis], position[axis]);
            }
        }
        if (minimum != terrain.minimum || maximum != terrain.maximum)
            return Failure("TERRAIN_GROUND_GRID", "地形边界与实际顶点不一致。");
        std::vector<std::uint8_t> cliffTiles(tileCount);
        for (std::uint32_t y = 0; y + 1 < height; ++y) for (std::uint32_t x = 0; x + 1 < width; ++x) {
            const auto index = static_cast<std::size_t>(y) * (width - 1) + x;
            const auto sw = y * width + x;
            const std::array<std::uint32_t, 4> expected{sw, sw + 1, sw + width, sw + width + 1};
            if (terrain.tiles[index].vertices != expected)
                return Failure("TERRAIN_GROUND_GRID", "tile 顶点不符合 SW/SE/NW/NE 原格对应关系。");
            // 四角全是坡道的层级过渡格是坡道面（按地表贴图绘制），不算悬崖格。
            const auto level = terrain.vertices[sw].CliffLevel();
            cliffTiles[index] = static_cast<std::uint8_t>(std::any_of(expected.begin(), expected.end(),
                [&](std::uint32_t corner) { return terrain.vertices[corner].CliffLevel() != level; })
                && !std::all_of(expected.begin(), expected.end(), [&](std::uint32_t corner) { return terrain.vertices[corner].IsRamp(); }));
        }
        AtlasFlags atlasValid{}, used{};
        for (std::size_t i = 0; i < groundAtlases.size(); ++i) atlasValid[i] = groundAtlases[i] && ValidAtlas(*groundAtlases[i]);
        std::vector<std::uint32_t> tileTextures(tileCount, TerrainGroundHeightColor);
        std::vector<Overlay> overlays;
        std::vector<Overlay> blightOverlays;
        OverlayCounters overlayCounts{};
        std::vector<std::uint32_t> waterTiles;
        TerrainGroundMesh output;
        output.sourceWidth = width; output.sourceHeight = height;
        output.budget.policy = budgetPolicy;
        output.budget.base = {baseQuadCount, baseQuadCount, 0};
        output.budget.textureSlotLimit = limits.maxTextures;
        output.budget.waterStyleAvailable = waterStyle != nullptr;
        output.budget.waterSourceTextureAvailable = waterStyle && waterStyle->texture;
        output.budget.blightAtlasAvailable = blightAtlas != nullptr;
        const auto covered = [&](std::size_t tile) { return !coveredTiles.empty() && coveredTiles[tile] != 0; };
        if (hiddenTileCount) {
            output.hiddenTiles.assign(tileCount, 0);
            output.baseTiles.reserve(baseQuadCount);
        }
        output.lit = lighting.enabled;
        const auto maximumWaterAlpha = waterStyle ? WaterMaximumSourceAlpha(waterStyle->texture.get()) : 0;
        for (std::uint32_t y = 0; y + 1 < height; ++y) for (std::uint32_t x = 0; x + 1 < width; ++x) {
            const auto tileIndex = static_cast<std::size_t>(y) * (width - 1) + x;
            const auto& corners = terrain.tiles[tileIndex].vertices;
            bool blight = false, water = false, invalidIndex = false, neighboringCliff = false;
            std::uint8_t lowest = std::numeric_limits<std::uint8_t>::max(), highest = 0;
            for (const auto index : corners) {
                const auto& vertex = terrain.vertices[index];
                blight |= vertex.IsBlight(); water |= vertex.HasWater();
                const auto ground = GroundIndex(terrain, index, effectiveGroundIndices);
                lowest = std::min(lowest, ground);
                highest = std::max(highest, ground);
                invalidIndex |= static_cast<std::size_t>(ground) >= groundAtlases.size();
            }
            // 即使预览稍后整类省略水面，也不能掩盖本应由渲染宿主拒绝的非法水面坐标。
            if (water && waterStyle) for (const auto index : corners) {
                const auto surface = WaterHeight(terrain, index, *waterStyle);
                if (!std::isfinite(surface) || std::abs(surface) > 10000000.0f)
                    return Failure("TERRAIN_GROUND_WATER_HEIGHT", "请求的水面坐标不是有限有效高度。");
            }
            output.waterFlaggedTiles += water;
            if (water && waterStyle) {
                // 仅合成单 mip 的全部 32×32 texel 为零才剔除；先保留上面的逐角高度校验。
                // 单调整数 alpha 与 atlas 同源。深度只读、目标 alpha 保持的水面混合因此无贡献。
                // 没有基础层时保持原水 draw：空地形协议会显示默认网格，不能把局部零贡献扩大为整场景变化。
                if (baseQuadCount != 0 && WaterTexelAlpha(WaterAlphaLevel(terrain, corners, *waterStyle), maximumWaterAlpha) == 0)
                    ++output.budget.water.noContribution;
                else waterTiles.push_back(static_cast<std::uint32_t>(tileIndex));
            }
            const auto firstX = x > 0 ? x - 1 : 0, firstY = y > 0 ? y - 1 : 0;
            for (auto nearY = firstY; nearY <= std::min(y + 1, height - 2); ++nearY)
                for (auto nearX = firstX; nearX <= std::min(x + 1, width - 2); ++nearX)
                    if ((nearX != x || nearY != y) && cliffTiles[static_cast<std::size_t>(nearY) * (width - 1) + nearX]
                        && !covered(static_cast<std::size_t>(nearY) * (width - 1) + nearX))
                        neighboringCliff = true; // 已由悬崖模型覆盖的邻格不再影响本格贴图
            if (covered(tileIndex)) {
                // 悬崖模型覆盖整格（含上下平台），地面不画；水面照常。
                output.hiddenTiles[tileIndex] = 1; ++output.hiddenTileCount;
                continue;
            }
            const bool mixedCliff = cliffTiles[tileIndex] != 0;
            // 四角用到的每个材质都要可用：过渡层缺图时整格保留高度色，不只画基础层冒充完整。
            bool missing = false, invalidAtlas = false;
            if (!invalidIndex) for (const auto index : corners) {
                const auto ground = GroundIndex(terrain, index, effectiveGroundIndices);
                missing |= !groundAtlases[ground];
                invalidAtlas |= groundAtlases[ground] && !atlasValid[ground];
            }
            invalidAtlas &= !missing;
            output.fallback.blightTiles += blight;
            output.fallback.mixedCliffLayerTiles += mixedCliff;
            output.fallback.neighboringCliffTiles += neighboringCliff;
            output.fallback.invalidGroundIndexTiles += invalidIndex;
            output.fallback.missingAtlasTiles += missing;
            output.fallback.invalidAtlasTiles += invalidAtlas;
            output.mixedGroundTextureTiles += lowest != highest;
            if (mixedCliff || neighboringCliff || invalidIndex || missing || invalidAtlas) {
                ++output.fallback.totalTiles;
            } else {
                tileTextures[tileIndex] = lowest;
                used[lowest] = true;
                ++output.texturedTiles;
                if (blight) blightOverlays.push_back({static_cast<std::uint32_t>(tileIndex), 0,
                    BlightMask(terrain, corners)});
                // 过渡层：比基础层索引大的每个材质按其所在角取遮罩格，按材质索引从小到大叠加。
                std::uint8_t previous = lowest;
                while (true) {
                    std::uint8_t next = 255;
                    for (const auto index : corners) {
                        const auto ground = GroundIndex(terrain, index, effectiveGroundIndices);
                        if (ground > previous) next = std::min(next, ground);
                    }
                    if (next == 255) break;
                    overlays.push_back({static_cast<std::uint32_t>(tileIndex), next,
                        CornerMask(terrain, corners, next, effectiveGroundIndices)});
                    used[next] = true;
                    ++overlayCounts[next];
                    previous = next;
                }
            }
        }

        output.budget.transitions.requested = overlays.size();
        output.budget.blight.requested = blightOverlays.size();
        output.budget.water.requested = output.waterFlaggedTiles;
        TextureSlots textureSlots{};
        textureSlots.fill(TerrainGroundHeightColor);
        std::size_t rgbaBytes = 0;
        for (std::size_t i = 0; i < groundAtlases.size(); ++i) if (used[i]) {
            const auto& atlas = groundAtlases[i];
            const auto existing = std::find(output.textures.begin(), output.textures.end(), atlas);
            if (existing != output.textures.end()) {
                textureSlots[i] = static_cast<std::uint32_t>(existing - output.textures.begin());
                continue;
            }
            if (output.textures.size() >= limits.maxTextures || atlas->image.width > limits.maxTextureDimension
                || atlas->image.height > limits.maxTextureDimension || atlas->image.rgba.size() > limits.maxTextureBytes
                || atlas->RgbaByteCount() > limits.maxTextureChainBytes)
                return Failure("TERRAIN_GROUND_BUDGET", "使用的地表纹理数量或尺寸超过预算。");
            rgbaBytes += atlas->RgbaByteCount();
            if (rgbaBytes > limits.maxRgbaBytes) return Failure("TERRAIN_GROUND_BUDGET", "使用的地表 RGBA 总量超过预算。");
            textureSlots[i] = static_cast<std::uint32_t>(output.textures.size());
            output.textures.push_back(atlas);
        }
        const auto groundTextureCount = output.textures.size();
        output.budget.groundTextureSlots = groundTextureCount;
        // 水面用合成透明度 atlas 占一个纹理槽；槽满时不画水面并如实标记。
        output.waterOmitted = output.waterFlaggedTiles != 0 && !waterStyle;
        if (output.waterFlaggedTiles != 0) {
            output.budget.water.resourceAdmissionChecked = true;
            output.budget.water.textureSlotsBeforeAdmission = output.textures.size();
            if (!waterStyle) output.budget.water.omissionReasons = Reason(TerrainGroundOmissionReason::MissingWaterStyle);
        }
        auto waterSlot = TerrainGroundHeightColor;
        if (!waterTiles.empty()) {
            if (output.textures.size() < limits.maxTextures) {
                auto atlas = WaterAtlas(waterStyle->texture.get());
                if (atlas->RgbaByteCount() <= limits.maxRgbaBytes - rgbaBytes) {
                    waterSlot = static_cast<std::uint32_t>(output.textures.size());
                    rgbaBytes += atlas->RgbaByteCount();
                    output.textures.push_back(std::move(atlas));
                    output.waterTextureIndex = waterSlot;
                    output.waterFrameIndex = waterStyle->frameIndex;
                    output.waterFrameCount = waterStyle->frameCount;
                    output.waterFrameSourcePath = waterStyle->frameSourcePath;
                    output.waterFrameSourceSha256 = waterStyle->frameSourceSha256;
                } else {
                    waterTiles.clear();
                    output.waterOmitted = true;
                    output.budget.water.omissionReasons = Reason(TerrainGroundOmissionReason::RgbaBytes);
                }
            } else {
                waterTiles.clear();
                output.waterOmitted = true;
                output.budget.water.omissionReasons = Reason(TerrainGroundOmissionReason::TextureSlots);
            }
        }
        auto overlayGroups = static_cast<std::size_t>(std::count_if(overlayCounts.begin(), overlayCounts.end(),
            [](std::uint32_t count) { return count != 0; }));
        constexpr auto kOverlayBytes = 4 * sizeof(TerrainGroundVertex) + 6 * sizeof(std::uint32_t)
            + sizeof(Overlay) + sizeof(std::uint32_t);
        auto baseBatchCount = groundTextureCount + static_cast<std::size_t>(output.fallback.totalTiles != 0);
        // 只允许显式世界预览减少可选几何。所有请求的地表资源已在上方完整校验，不能借省略绕过。
        // 保留顺序：完整基础层 > 全图水面 > 全图普通过渡 > 精确荒芜遮罩（下方）。
        // 每类全留或全省略；水域轮廓优先于地表混合细节，不按 tile/块前缀截断。
        if (budgetPolicy == TerrainGroundBudgetPolicy::PreserveBasePreview) {
            // 过渡专用 atlas 不必产生基础层批次；预览取实际可见基础层分组数，不能虚耗批次预算。
            BatchCounters baseGroups{};
            for (std::size_t tile = 0; tile < tileCount; ++tile) if (!covered(tile))
                ++baseGroups[tileTextures[tile] == TerrainGroundHeightColor ? groundTextureCount : textureSlots[tileTextures[tile]]];
            baseBatchCount = static_cast<std::size_t>(std::count_if(baseGroups.begin(), baseGroups.end(),
                [](std::uint32_t count) { return count != 0; }));
            const auto fitReasons = [&](std::size_t transitionQuads, std::size_t waterQuads, std::size_t bytes) {
                const auto extra = transitionQuads + waterQuads;
                const auto quads = baseQuadCount + extra;
                const auto batches = baseBatchCount + (transitionQuads != 0 ? overlayGroups : 0)
                    + static_cast<std::size_t>(waterQuads != 0);
                std::uint32_t reasons = 0;
                if (quads * 4 > limits.maxVertices) reasons |= Reason(TerrainGroundOmissionReason::Vertices);
                if (quads * 6 > limits.maxIndices) reasons |= Reason(TerrainGroundOmissionReason::Indices);
                if (batches > limits.maxBatches) reasons |= Reason(TerrainGroundOmissionReason::Batches);
                if (fixedBytes + extra * kOverlayBytes + bytes + batches * sizeof(TerrainGroundBatch)
                    + output.textures.capacity() * sizeof(std::shared_ptr<const TerrainAtlasSnapshot>) > limits.maxWorkingBytes)
                    reasons |= Reason(TerrainGroundOmissionReason::WorkingBytes);
                return reasons;
            };
            const auto fits = [&](std::size_t transitionQuads, std::size_t waterQuads, std::size_t bytes) {
                return fitReasons(transitionQuads, waterQuads, bytes) == 0;
            };
            if (!fits(overlays.size(), waterTiles.size(), rgbaBytes)) {
                const auto waterBytes = waterSlot != TerrainGroundHeightColor
                    ? output.textures[waterSlot]->RgbaByteCount() : 0;
                const auto groundBytes = rgbaBytes - waterBytes;
                if (!fits(0, 0, groundBytes))
                    return Failure("TERRAIN_GROUND_BUDGET", "完整基础层与所需地表材质超过预算，不能发布基础贴图预览。");
                const bool keepWater = fits(0, waterTiles.size(), rgbaBytes);
                if (!keepWater) {
                    output.budget.water.omissionReasons |= fitReasons(0, waterTiles.size(), rgbaBytes);
                    // 水面 atlas 在地表 atlas 之后且尚未追加荒芜；省略时清掉帧身份，不能声称已显示该帧。
                    output.textures.pop_back();
                    rgbaBytes = groundBytes;
                    waterSlot = TerrainGroundHeightColor;
                    output.waterTextureIndex.reset();
                    output.waterFrameIndex = output.waterFrameCount = 0;
                    output.waterFrameSourcePath.clear(); output.waterFrameSourceSha256.clear();
                    std::vector<std::uint32_t>().swap(waterTiles);
                    output.waterOmitted = true;
                    output.budget.reduced = true;
                }
                if (!fits(overlays.size(), waterTiles.size(), rgbaBytes)) {
                    output.budget.transitions.omissionReasons = fitReasons(overlays.size(), waterTiles.size(), rgbaBytes);
                    std::vector<Overlay>().swap(overlays);
                    overlayCounts.fill(0);
                    overlayGroups = 0;
                    output.budget.reduced = true;
                }
            }
        }
        auto blightSlot = TerrainGroundHeightColor;
        if (!blightOverlays.empty()) {
            output.budget.blight.resourceAdmissionChecked = true;
            output.budget.blight.textureSlotsBeforeAdmission = output.textures.size();
            output.budget.blightAtlasValidationPerformed = blightAtlas != nullptr;
            output.budget.blightAtlasValid = blightAtlas && blightAtlas->columns == 8 && ValidAtlas(*blightAtlas);
            if (!blightAtlas) output.budget.blight.omissionReasons = Reason(TerrainGroundOmissionReason::MissingBlightAtlas);
            else if (!output.budget.blightAtlasValid)
                output.budget.blight.omissionReasons = Reason(TerrainGroundOmissionReason::InvalidBlightAtlas);
        }
        if (!blightOverlays.empty() && output.budget.blightAtlasValid) {
            const auto existing = std::find(output.textures.begin(), output.textures.end(), blightAtlas);
            const bool newTexture = existing == output.textures.end();
            const auto addedRgba = newTexture ? blightAtlas->RgbaByteCount() : 0;
            const auto quads = baseQuadCount + overlays.size() + blightOverlays.size() + waterTiles.size();
            const auto batches = baseBatchCount + overlayGroups + 1 + static_cast<std::size_t>(!waterTiles.empty());
            std::uint32_t resourceReasons = 0;
            if (newTexture && output.textures.size() >= limits.maxTextures) resourceReasons |= Reason(TerrainGroundOmissionReason::TextureSlots);
            if (newTexture && (blightAtlas->image.width > limits.maxTextureDimension
                || blightAtlas->image.height > limits.maxTextureDimension)) resourceReasons |= Reason(TerrainGroundOmissionReason::TextureDimension);
            if (newTexture && blightAtlas->image.rgba.size() > limits.maxTextureBytes) resourceReasons |= Reason(TerrainGroundOmissionReason::TextureBytes);
            if (newTexture && addedRgba > limits.maxTextureChainBytes) resourceReasons |= Reason(TerrainGroundOmissionReason::TextureChainBytes);
            if (addedRgba > limits.maxRgbaBytes - rgbaBytes) resourceReasons |= Reason(TerrainGroundOmissionReason::RgbaBytes);
            const bool resourceFits = resourceReasons == 0;
            const auto textureCapacity = budgetPolicy == TerrainGroundBudgetPolicy::PreserveBasePreview
                ? std::max(limits.maxTextures, output.textures.capacity()) : limits.maxTextures;
            std::uint32_t meshReasons = 0;
            if (quads * 4 > limits.maxVertices) meshReasons |= Reason(TerrainGroundOmissionReason::Vertices);
            if (quads * 6 > limits.maxIndices) meshReasons |= Reason(TerrainGroundOmissionReason::Indices);
            if (batches > limits.maxBatches) meshReasons |= Reason(TerrainGroundOmissionReason::Batches);
            if (fixedBytes + (overlays.size() + blightOverlays.size() + waterTiles.size()) * kOverlayBytes
                + rgbaBytes + addedRgba + batches * sizeof(TerrainGroundBatch)
                + textureCapacity * sizeof(std::shared_ptr<const TerrainAtlasSnapshot>) > limits.maxWorkingBytes)
                meshReasons |= Reason(TerrainGroundOmissionReason::WorkingBytes);
            const bool meshFits = meshReasons == 0;
            if (resourceFits && meshFits) {
                blightSlot = newTexture ? static_cast<std::uint32_t>(output.textures.size())
                    : static_cast<std::uint32_t>(existing - output.textures.begin());
                if (newTexture) {
                    // 预览已按槽预算计费，先预留这些槽，避免 push_back 的几何扩容越过预检字节数。
                    if (budgetPolicy == TerrainGroundBudgetPolicy::PreserveBasePreview)
                        output.textures.reserve(limits.maxTextures);
                    rgbaBytes += addedRgba;
                    output.textures.push_back(blightAtlas);
                }
            } else {
                // 两项判定均已在上方实际计算；资源未准入也保留同时存在的几何阻断。
                output.budget.blight.omissionReasons = resourceReasons | meshReasons;
                if (resourceFits && budgetPolicy == TerrainGroundBudgetPolicy::PreserveBasePreview) output.budget.reduced = true;
            }
        }
        if (blightSlot == TerrainGroundHeightColor) {
            output.blightApproximationTiles = blightOverlays.size();
            std::vector<Overlay>().swap(blightOverlays);
        } else output.blightTexturedTiles = blightOverlays.size();
        const auto quadCount = baseQuadCount + overlays.size() + blightOverlays.size() + waterTiles.size();
        const auto overlayBytes = (overlays.size() + blightOverlays.size() + waterTiles.size()) * kOverlayBytes;
        if (quadCount * 4 > limits.maxVertices || quadCount * 6 > limits.maxIndices
            || fixedBytes + overlayBytes > limits.maxWorkingBytes)
            return Failure("TERRAIN_GROUND_BUDGET", "地表过渡层或水面超出网格预算，请由宿主回退完整地图的高度预览。");
        // 基础层每个地表纹理最多一批，随后是地表过渡、荒芜和水面批次。
        const auto batchCount = baseBatchCount + overlayGroups + static_cast<std::size_t>(blightSlot != TerrainGroundHeightColor)
            + static_cast<std::size_t>(!waterTiles.empty());
        if (batchCount > limits.maxBatches || fixedBytes + overlayBytes + rgbaBytes + batchCount * sizeof(TerrainGroundBatch)
            + output.textures.capacity() * sizeof(std::shared_ptr<const TerrainAtlasSnapshot>) > limits.maxWorkingBytes)
            return Failure("TERRAIN_GROUND_BUDGET", "地表批次或整体载荷超过预算。");
        const auto counts = [](TerrainGroundLayerCounts& layer, std::size_t displayed) {
            layer.displayed = displayed;
            layer.omitted = layer.requested - layer.noContribution - displayed;
        };
        counts(output.budget.transitions, overlays.size());
        counts(output.budget.blight, blightOverlays.size());
        counts(output.budget.water, waterTiles.size());
        output.budget.finalTextureSlots = output.textures.size();
        output.vertices.reserve(quadCount * 4);
        output.indices.resize(quadCount * 6);
        output.overlayTiles.reserve(overlays.size() + blightOverlays.size());
        output.waterTiles.reserve(waterTiles.size());
        output.batches.reserve(batchCount);
        output.blocks.reserve(blockCount);
        BatchCounters groupCounts{}, groupOffsets{}, groupWritten{};
        // 高度色组号紧跟地表纹理槽；水面与荒芜 atlas 均不参与基础层分组。
        const auto heightGroup = static_cast<std::uint32_t>(groundTextureCount);
        for (std::size_t tile = 0; tile < tileCount; ++tile) if (!covered(tile))
            ++groupCounts[tileTextures[tile] == TerrainGroundHeightColor ? heightGroup : textureSlots[tileTextures[tile]]];
        std::uint32_t firstIndex = 0;
        for (std::uint32_t group = 0; group <= heightGroup; ++group) if (groupCounts[group]) {
            groupOffsets[group] = firstIndex;
            const auto count = groupCounts[group] * 6;
            output.batches.push_back({firstIndex, count, group == heightGroup ? TerrainGroundHeightColor : group});
            firstIndex += count;
        }
        // 过渡层批次在全部基础层之后，按材质索引升序，保证同一格内低索引层先画。
        OverlayCounters overlayOffsets{}, overlayWritten{};
        for (std::size_t ground = 0; ground < overlayCounts.size(); ++ground) if (overlayCounts[ground]) {
            overlayOffsets[ground] = firstIndex;
            const auto count = overlayCounts[ground] * 6;
            output.batches.push_back({firstIndex, count, textureSlots[ground], true});
            firstIndex += count;
        }
        const auto blightFirst = firstIndex;
        if (!blightOverlays.empty()) {
            output.batches.push_back({firstIndex, static_cast<std::uint32_t>(blightOverlays.size() * 6), blightSlot, true});
            firstIndex += static_cast<std::uint32_t>(blightOverlays.size() * 6);
        }
        const auto waterFirst = firstIndex;
        if (!waterTiles.empty())
            output.batches.push_back({firstIndex, static_cast<std::uint32_t>(waterTiles.size() * 6), waterSlot, true, true});
        for (std::size_t tileIndex = 0; tileIndex < tileCount; ++tileIndex) {
            if (covered(tileIndex)) continue; // 已准备的悬崖模型负责此格，地表不再分配不可见顶点。
            const auto texture = tileTextures[tileIndex];
            const bool colored = texture == TerrainGroundHeightColor;
            const auto& corners = terrain.tiles[tileIndex].vertices;
            CellRect rect;
            if (!colored) {
                const auto& atlas = *groundAtlases[texture];
                const auto coordinate = VariationCell(atlas.columns, terrain.vertices[corners[0]].GroundVariation());
                rect = CellUv(atlas, coordinate[0], coordinate[1]);
            }
            const auto first = static_cast<std::uint32_t>(output.vertices.size());
            EmitQuad(output, terrain, corners, colored, blightSlot == TerrainGroundHeightColor,
                rect, minimum[2], maximum[2], lighting);
            if (hiddenTileCount) output.baseTiles.push_back(static_cast<std::uint32_t>(tileIndex));
            const auto group = colored ? heightGroup : textureSlots[texture];
            const auto at = groupOffsets[group] + groupWritten[group] * 6;
            WriteFace(output, first, at);
            ++groupWritten[group];
        }
        for (const auto& overlay : overlays) {
            const auto& corners = terrain.tiles[overlay.tile].vertices;
            // 遮罩格位于 atlas 左侧 4×4：列 = mask % 4，行 = mask / 4（第 0/15 格为满格，不会出现在过渡层）。
            const auto rect = CellUv(*groundAtlases[overlay.ground], overlay.mask % 4u, overlay.mask / 4u);
            const auto first = static_cast<std::uint32_t>(output.vertices.size());
            EmitQuad(output, terrain, corners, false, blightSlot == TerrainGroundHeightColor,
                rect, minimum[2], maximum[2], lighting);
            WriteFace(output, first, overlayOffsets[overlay.ground] + overlayWritten[overlay.ground] * 6);
            ++overlayWritten[overlay.ground];
            output.overlayTiles.push_back(overlay.tile);
        }
        for (std::size_t index = 0; index < blightOverlays.size(); ++index) {
            const auto& overlay = blightOverlays[index];
            const auto& corners = terrain.tiles[overlay.tile].vertices;
            const auto coordinate = overlay.mask == 15
                ? VariationCell(blightAtlas->columns, terrain.vertices[corners[0]].GroundVariation())
                : std::array<std::uint32_t, 2>{overlay.mask % 4u, overlay.mask / 4u};
            const auto rect = CellUv(*blightAtlas, coordinate[0], coordinate[1]);
            const auto first = static_cast<std::uint32_t>(output.vertices.size());
            EmitQuad(output, terrain, corners, false, false, rect, minimum[2], maximum[2], lighting);
            WriteFace(output, first, blightFirst + index * 6);
            output.overlayTiles.push_back(overlay.tile);
        }
        const auto* waterAtlas = waterTiles.empty() ? nullptr : output.textures[waterSlot].get();
        for (std::size_t index = 0; index < waterTiles.size(); ++index) {
            const auto& corners = terrain.tiles[waterTiles[index]].vertices;
            std::array<std::array<float, 4>, 4> colors{};
            const auto level = WaterAlphaLevel(terrain, corners, *waterStyle, &colors);
            const auto first = static_cast<std::uint32_t>(output.vertices.size());
            for (std::size_t corner = 0; corner < 4; ++corner) {
                const auto& position = terrain.vertices[corners[corner]].position;
                const auto surface = WaterHeight(terrain, corners[corner], *waterStyle);
                const auto& color = colors[corner];
                output.vertices.push_back({position[0], position[1], surface, color[0], color[1], color[2], 0, 0});
            }
            // 四角平均透明度量化到合成 atlas 的 16 级，UV 指向该格。
            const auto rect = CellUv(*waterAtlas, level % 4, level / 4);
            for (std::size_t corner = 0; corner < 4; ++corner) {
                auto& vertex = output.vertices[first + corner];
                vertex.u = corner % 2 == 0 ? rect.left : rect.right;
                vertex.v = corner < 2 ? rect.bottom : rect.top;
            }
            WriteFace(output, first, waterFirst + index * 6);
            output.waterTiles.push_back(waterTiles[index]);
        }
        for (std::uint32_t by = 0; by < blockRows; ++by) for (std::uint32_t bx = 0; bx < blockColumns; ++bx) {
            TerrainGroundBlock block;
            block.tileX = bx * TerrainGroundMesh::blockTileSize; block.tileY = by * TerrainGroundMesh::blockTileSize;
            block.tileWidth = std::min(TerrainGroundMesh::blockTileSize, width - 1 - block.tileX);
            block.tileHeight = std::min(TerrainGroundMesh::blockTileSize, height - 1 - block.tileY);
            block.minimum.fill(std::numeric_limits<float>::infinity());
            block.maximum.fill(-std::numeric_limits<float>::infinity());
            for (auto y = block.tileY; y <= block.tileY + block.tileHeight; ++y)
                for (auto x = block.tileX; x <= block.tileX + block.tileWidth; ++x) {
                    const auto& position = terrain.vertices[static_cast<std::size_t>(y) * width + x].position;
                    for (std::size_t axis = 0; axis < 3; ++axis) {
                        block.minimum[axis] = std::min(block.minimum[axis], position[axis]);
                        block.maximum[axis] = std::max(block.maximum[axis], position[axis]);
                    }
                }
            output.blocks.push_back(block);
        }
        return Result<TerrainGroundMesh>::Success(std::move(output));
    } catch (const std::bad_alloc&) {
        return Failure("TERRAIN_GROUND_ALLOCATION", "地表网格分配失败，未发布部分结果。");
    } catch (const std::length_error&) {
        return Failure("TERRAIN_GROUND_ALLOCATION", "地表网格超出容器容量，未发布部分结果。");
    }
}

} // namespace War3Map::Render
