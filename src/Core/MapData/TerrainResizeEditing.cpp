#include "War3MapCore/MapData/TerrainResizeEditing.h"
#include "War3MapCore/MapData/TerrainEditing.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <stdexcept>

namespace War3Map::MapData {
namespace {
template<class T> Result<T> ResizeFailure(const char* code, const char* message) {
    return Result<T>::Failure("Terrain.Resize", code, message);
}
constexpr std::size_t MaximumBytes = 256u * 1024u * 1024u;
constexpr std::size_t MaximumWork = 16u * 1024u * 1024u;
}

Status ValidateTerrainResizeIntent(const TerrainResizeIntent& intent) {
    const auto& r = intent.canvas;
    if (intent.algorithm != 1 || r.west >= r.east || r.south >= r.north)
        return Status::Failure("Terrain.Resize", "TERRAIN_RESIZE_INTENT", "外矩形需要算法1和非空整数地格范围。");
    const auto width = std::int64_t(r.east) - r.west, height = std::int64_t(r.north) - r.south;
    if (width > 2048 || height > 2048 || std::abs(std::int64_t(r.west)) > 2048
        || std::abs(std::int64_t(r.south)) > 2048 || std::abs(std::int64_t(r.east)) > 4096
        || std::abs(std::int64_t(r.north)) > 4096)
        return Status::Failure("Terrain.Resize", "TERRAIN_RESIZE_DIMENSION", "外矩形超出经典地形尺寸或坐标硬上限。");
    return Status::Ok();
}

Result<TerrainResizeGeometry> ProjectTerrainResizeGeometry(const TerrainGridSnapshot& source,
    const MapSettings& settings, const std::array<float, 4>& cameraMargins,
    const TerrainResizeDimensionRules& dimensions, const TerrainResizeIntent& intent,
    const TerrainResizeLimits& limits) {
    using Output = Result<TerrainResizeGeometry>;
    const auto valid = ValidateTerrainResizeIntent(intent);
    if (!valid.Succeeded()) return {{}, valid.diagnostics};
    if (!limits.maximumCandidateBytes || limits.maximumCandidateBytes > MaximumBytes
        || !limits.maximumWork || limits.maximumWork > MaximumWork)
        return ResizeFailure<TerrainResizeGeometry>("TERRAIN_RESIZE_LIMITS", "扩裁预算只能收紧既有硬上限，且不能为零。");
    const auto& old = source.header;
    const auto& r = intent.canvas;
    if (old.version != 11 || old.width < 2 || old.height < 2 || old.width > 2049 || old.height > 2049
        || source.vertices.size() != std::size_t(old.width) * old.height
        || source.tiles.size() != std::size_t(old.width - 1) * (old.height - 1)
        || !settings.FullyParsed() || settings.formatVersion != 25)
        return ResizeFailure<TerrainResizeGeometry>("TERRAIN_RESIZE_LAYOUT", "扩裁需要完整经典W3E和W3I，不接受部分网格。");
    if (r.east <= 0 || r.north <= 0 || r.west >= std::int64_t(old.width - 1) || r.south >= std::int64_t(old.height - 1))
        return ResizeFailure<TerrainResizeGeometry>("TERRAIN_RESIZE_OVERLAP", "新外矩形必须与原地图有地格重叠。");
    const auto width = static_cast<std::uint32_t>(std::int64_t(r.east) - r.west);
    const auto height = static_cast<std::uint32_t>(std::int64_t(r.north) - r.south);
    if (!dimensions.minimumSide || dimensions.minimumSide > dimensions.maximumSide || dimensions.maximumSide > 2048
        || dimensions.sideStep != 32 || !dimensions.maximumArea
        || width < dimensions.minimumSide || height < dimensions.minimumSide
        || width > dimensions.maximumSide || height > dimensions.maximumSide
        || width % dimensions.sideStep || height % dimensions.sideStep
        || std::uint64_t(width) * height > dimensions.maximumArea)
        return ResizeFailure<TerrainResizeGeometry>("TERRAIN_RESIZE_RULES", "新尺寸不符合已捕获的最小、最大、32格步长或面积限制。");
    const auto playable = ComputeTerrainNativePlayableBounds(old, settings, cameraMargins);
    if (!playable.Succeeded() || !playable.value) return {{}, playable.diagnostics};
    const double left = old.offset[0] + double(r.west) * 128, bottom = old.offset[1] + double(r.south) * 128;
    const double right = left + double(width) * 128, top = bottom + double(height) * 128;
    for (const auto value : {double(old.offset[0]), double(old.offset[1]), left, bottom, right, top})
        if (!std::isfinite(value) || std::abs(value) > 16777216 || std::fmod(value, 128.0) != 0)
            return ResizeFailure<TerrainResizeGeometry>("TERRAIN_RESIZE_COORDINATE", "扩裁需要128对齐且可精确表示的经典世界坐标。");
    const auto& p = *playable.value;
    if (p.left < left || p.bottom < bottom || p.right > right || p.top > top)
        return ResizeFailure<TerrainResizeGeometry>("TERRAIN_RESIZE_PLAYABLE_CLIPPED", "新外矩形裁及当前可玩范围；请先明确缩小可玩范围。");
    const std::array<double, 4> relative{(double(p.left) - left) / 128, (right - double(p.right)) / 128,
        (double(p.bottom) - bottom) / 128, (top - double(p.top)) / 128};
    for (const auto value : relative)
        if (!std::isfinite(value) || value < 0 || value > 2048 || std::trunc(value) != value)
            return ResizeFailure<TerrainResizeGeometry>("TERRAIN_RESIZE_PLAYABLE_ALIGNMENT", "当前实际可玩范围不能精确投影到新整数地格边距。");
    const std::size_t vertices = std::size_t(width + 1) * (height + 1), tiles = std::size_t(width) * height;
    // 先收齐完整候选及原网格保活材料，不在分配之后才检查预算。
    const std::size_t work = source.vertices.size() + vertices * 2 + tiles * 2;
    std::size_t bytes = 8192;
    const auto add = [&](std::size_t count, std::size_t size) {
        if (count > (limits.maximumCandidateBytes - bytes) / size) return false;
        bytes += count * size; return true;
    };
    if (bytes > limits.maximumCandidateBytes || work > limits.maximumWork
        || !add(source.vertices.capacity(), sizeof(TerrainVertex)) || !add(source.tiles.capacity(), sizeof(TerrainTile))
        || !add(vertices, sizeof(TerrainVertex)) || !add(tiles, sizeof(TerrainTile)))
        return ResizeFailure<TerrainResizeGeometry>("TERRAIN_RESIZE_BUDGET", "完整扩裁候选和可逆材料超过预算，未修改会话。");
    for (const auto& vertex : source.vertices) if (vertex.extendedTextureLayout)
        return ResizeFailure<TerrainResizeGeometry>("TERRAIN_RESIZE_LAYOUT", "经典扩裁不能混入扩展顶点布局。");
    try {
        const auto serializedSettings = settings.Serialize();
        if (!serializedSettings.Succeeded() || !serializedSettings.value) return {{}, serializedSettings.diagnostics};
        if (!add(serializedSettings.value->size(), 3))
            return ResizeFailure<TerrainResizeGeometry>("TERRAIN_RESIZE_BUDGET", "地图设置及其可逆副本超过扩裁预算。");
        TerrainResizeGeometry output;
        output.terrain.header = old;
        output.terrain.header.width = width + 1; output.terrain.header.height = height + 1;
        output.terrain.header.offset = {static_cast<float>(left), static_cast<float>(bottom)};
        output.terrain.vertices.resize(vertices); output.terrain.tiles.resize(tiles);
        for (std::uint32_t y = 0; y <= height; ++y) for (std::uint32_t x = 0; x <= width; ++x) {
            const auto sx = std::clamp(std::int64_t(x) + r.west, std::int64_t{0}, std::int64_t(old.width - 1));
            const auto sy = std::clamp(std::int64_t(y) + r.south, std::int64_t{0}, std::int64_t(old.height - 1));
            // 算法1确定性保留最近边缘的全部原始字段，包括variation；不猜实际权重重新抽签。
            output.terrain.vertices[std::size_t(y) * (width + 1) + x] = source.vertices[std::size_t(sy) * old.width + sx];
        }
        for (std::uint32_t y = 0; y < height; ++y) for (std::uint32_t x = 0; x < width; ++x) {
            const auto sw = y * (width + 1) + x;
            output.terrain.tiles[std::size_t(y) * width + x].vertices = {sw, sw + 1, sw + width + 1, sw + width + 2};
        }
        output.settings = settings;
        for (std::size_t i = 0; i < 4; ++i) output.settings.cameraMargins[i] = static_cast<std::int32_t>(relative[i]);
        output.settings.flags |= 0x400u;
        output.settings.playableWidth = static_cast<std::int32_t>((double(p.right) - p.left) / 128);
        output.settings.playableHeight = static_cast<std::int32_t>((double(p.top) - p.bottom) / 128);
        // cameraBounds八个F32逐字保持；未设0x400的来源也先按真实CameraMargins计算实际范围。
        output.playable = {static_cast<std::uint32_t>(relative[0]), static_cast<std::uint32_t>(relative[2]),
            width - static_cast<std::uint32_t>(relative[1]), height - static_cast<std::uint32_t>(relative[3])};
        auto edges = ApplyTerrainPlayableEdges(output.terrain, output.playable, std::min(limits.maximumWork, std::size_t{2048} * 2048));
        if (!edges.Succeeded()) return {{}, edges.diagnostics};
        RefreshTerrainDerived(output.terrain, 0, 0, width + 1, height + 1);
        const auto checked = SerializeMapTerrain(output.terrain);
        if (!checked.Succeeded()) return {{}, checked.diagnostics};
        output.preparationBytes = bytes; output.preparationWork = work;
        return Output::Success(std::move(output));
    } catch (const std::bad_alloc&) {
        return ResizeFailure<TerrainResizeGeometry>("TERRAIN_RESIZE_BUDGET", "扩裁内存不足，未修改会话。");
    } catch (const std::length_error&) {
        return ResizeFailure<TerrainResizeGeometry>("TERRAIN_RESIZE_BUDGET", "扩裁容量超限，未修改会话。");
    }
}
} // namespace War3Map::MapData
