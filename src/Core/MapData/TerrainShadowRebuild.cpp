#include "War3MapCore/MapData/TerrainShadowRebuild.h"
#include "War3MapCore/MapData/TerrainEditing.h"

#include <cmath>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

namespace War3Map::MapData {
namespace {

Result<TerrainShadowProjection> Reject(const char* code, const char* message) {
    return Result<TerrainShadowProjection>::Failure("MapTerrain.Shadow", code, message);
}

} // namespace

Result<std::array<float, 3>> PrepareTerrainShadowLightDirection(const std::array<float, 3>& editorDirection) {
    using Output = Result<std::array<float, 3>>;
    double squared = 0;
    for (const auto value : editorDirection) {
        if (!std::isfinite(value))
            return Output::Failure("MapTerrain.Shadow", "TERRAIN_SHADOW_LIGHT", "实际Light/Direction包含非有限分量。");
        squared += static_cast<double>(value) * value;
    }
    if (squared == 0)
        return Output::Failure("MapTerrain.Shadow", "TERRAIN_SHADOW_LIGHT", "实际Light/Direction为零，不能归一化。");
    const double inverseLength = 1.0 / std::sqrt(squared);
    std::array<float, 3> nativeDirection{};
    for (std::size_t axis = 0; axis < 3; ++axis)
        nativeDirection[axis] = -static_cast<float>(editorDirection[axis] * inverseLength);
    return Output::Success(nativeDirection);
}

Result<TerrainShadowReceiverGrid> SampleOrdinaryTerrainShadowHeights(const TerrainGridSnapshot& terrain,
    std::span<const SpecialDoodadPlacement> specialDoodads, const TerrainShadowRebuildLimits& limits,
    std::stop_token cancellation) {
    using Output = Result<TerrainShadowReceiverGrid>;
    const auto fail = [](const char* code, const char* message) { return Output::Failure("MapTerrain.Shadow", code, message); };
    const TerrainShadowRebuildLimits hard;
    if (limits.maximumDimension > hard.maximumDimension || limits.maximumPixels > hard.maximumPixels
        || limits.maximumSampleBytes > hard.maximumSampleBytes || limits.maximumWork > hard.maximumWork)
        return fail("TERRAIN_SHADOW_LIMITS", "阴影采样只能收紧硬预算。");
    const auto& header = terrain.header;
    if (header.version != 11 || header.width < 2 || header.height < 2
        || header.width > 2049 || header.height > 2049
        || terrain.vertices.size() != static_cast<std::size_t>(header.width) * header.height)
        return fail("TERRAIN_SHADOW_TERRAIN", "阴影采样需要完整经典 W3E v11 原始顶点网格。");
    const auto width = (header.width - 1) * 4, height = (header.height - 1) * 4;
    if (width > limits.maximumDimension || height > limits.maximumDimension || height > limits.maximumPixels / width)
        return fail("TERRAIN_SHADOW_BUDGET", "阴影采样网格超过像素预算。");
    const auto count = static_cast<std::size_t>(width) * height;
    if (count > limits.maximumSampleBytes / sizeof(float))
        return fail("TERRAIN_SHADOW_BUDGET", "阴影高度场超过工作内存预算。");
    if (!specialDoodads.empty())
        return fail("TERRAIN_SHADOW_SPECIAL_GEOMETRY", "特殊装饰物需要实际模型表面，不能以普通地形高度替代。");
    for (std::size_t axis = 0; axis < 2; ++axis) {
        const auto offset = static_cast<double>(header.offset[axis]);
        const auto last = offset + static_cast<double>((axis == 0 ? header.width : header.height) - 1) * 128.0;
        if (!std::isfinite(offset) || std::fmod(offset, 128.0) != 0 || offset < -32768 || last > 32640)
            return fail("TERRAIN_SHADOW_COORDINATE", "阴影采样坐标须128对齐并落在原版九位顶点坐标范围内。");
    }
    try {
        TerrainShadowReceiverGrid candidate;
        candidate.width = width; candidate.height = height;
        const auto consume = [&] {
            if (candidate.work == limits.maximumWork) return false;
            ++candidate.work;
            return true;
        };
        const auto level = terrain.vertices.front().CliffLevel();
        for (const auto& vertex : terrain.vertices) {
            if (cancellation.stop_requested())
                return fail("TERRAIN_SHADOW_CANCELLED", "阴影地形检查已取消，未返回部分采样。");
            if (!consume()) return fail("TERRAIN_SHADOW_BUDGET", "阴影地形检查超过工作预算。");
            if (vertex.extendedTextureLayout || vertex.textureFlagsExtensionRaw != 0
                || (vertex.groundHeightRaw & 0xC000) != 0 || vertex.UnknownWaterFlags() != 0 || vertex.CliffLevel() == 15)
                return fail("TERRAIN_SHADOW_TERRAIN_FLAGS", "阴影采样遇到未闭合的地形原始字段，不能套用普通表面。");
            if (vertex.IsRamp())
                return fail("TERRAIN_SHADOW_RAMP_GEOMETRY", "坡道阴影需要原版分类及实际表面，不能套用普通采样。");
            if (vertex.CliffLevel() != level)
                return fail("TERRAIN_SHADOW_CLIFF_GEOMETRY", "异层悬崖阴影需要实际模型表面，不能套用普通采样。");
        }
        candidate.receiverHeights.reserve(count);
        for (std::uint32_t y = 0; y < height; ++y) {
            for (std::uint32_t x = 0; x < width; ++x) {
                if (cancellation.stop_requested())
                    return fail("TERRAIN_SHADOW_CANCELLED", "阴影地形采样已取消，未返回部分采样。");
                if (!consume()) return fail("TERRAIN_SHADOW_BUDGET", "阴影地形采样超过工作预算。");
                const auto sampled = SampleGroundHeight(terrain, static_cast<double>(header.offset[0]) + x * 32.0 + 16.0,
                    static_cast<double>(header.offset[1]) + y * 32.0 + 16.0, TerrainHeightSource::RawWithRamps);
                if (!sampled || !std::isfinite(*sampled))
                    return fail("TERRAIN_SHADOW_HEIGHT", "实际地形像素中心不能完成采样，未返回部分高度场。");
                candidate.receiverHeights.push_back(static_cast<float>(static_cast<double>(*sampled) + 0.1));
            }
        }
        return Output::Success(std::move(candidate));
    } catch (const std::bad_alloc&) {
        return fail("TERRAIN_SHADOW_ALLOCATION", "阴影采样分配失败，未返回部分高度场。");
    } catch (const std::length_error&) {
        return fail("TERRAIN_SHADOW_ALLOCATION", "阴影采样容量不足，未返回部分高度场。");
    }
}

Result<TerrainShadowProjection> ProjectTerrainShadow(const TerrainShadowHeightSamples& samples,
    const TerrainShadowRebuildLimits& limits, std::stop_token cancellation) {
    const TerrainShadowRebuildLimits hard;
    if (limits.maximumDimension > hard.maximumDimension || limits.maximumPixels > hard.maximumPixels
        || limits.maximumSampleBytes > hard.maximumSampleBytes || limits.maximumWork > hard.maximumWork)
        return Reject("TERRAIN_SHADOW_LIMITS", "阴影投影只能收紧硬预算。");
    if (samples.width == 0 || samples.height == 0 || samples.width % 4 != 0 || samples.height % 4 != 0)
        return Reject("TERRAIN_SHADOW_DIMENSIONS", "阴影高度场宽高必须为正且为每地格四像素的完整尺寸。");
    if (samples.width > limits.maximumDimension || samples.height > limits.maximumDimension
        || samples.height > limits.maximumPixels / samples.width)
        return Reject("TERRAIN_SHADOW_BUDGET", "阴影高度场尺寸或像素数超过预算。");
    const auto count = static_cast<std::size_t>(samples.width) * samples.height;
    if (count > limits.maximumSampleBytes / sizeof(float))
        return Reject("TERRAIN_SHADOW_BUDGET", "阴影高度场超过本次样本字节预算。");
    if (samples.receiverHeights.size() != count)
        return Reject("TERRAIN_SHADOW_HEIGHT_COUNT", "阴影高度场缺失或超出完整网格，不能补零或截断。");
    for (const auto value : samples.lightDirection) {
        if (!std::isfinite(value))
            return Reject("TERRAIN_SHADOW_LIGHT", "阴影光照方向必须为有限数。");
    }

    // 原版选择负方向中绝对值较大的水平分量；恰好相等时沿 Y 主轴。
    // 单轴零值显式处理，避免依赖浮点除零得到的无穷值参与分支。
    const double dx = -static_cast<double>(samples.lightDirection[0]);
    const double dy = -static_cast<double>(samples.lightDirection[1]);
    const double dz = -static_cast<double>(samples.lightDirection[2]);
    if (dx == 0 && dy == 0)
        return Reject("TERRAIN_SHADOW_LIGHT_VERTICAL", "光照没有水平分量，原版主轴投影规则未定义。");
    const bool majorX = std::abs(dx) > std::abs(dy);
    const double major = majorX ? dx : dy;
    const double minor = majorX ? dy : dx;
    const std::int64_t step = major > 0 ? 1 : -1;
    // 原版把这两个步长以 binary32 传入逐像素投影函数，然后累计。
    const double projectedMinorStep = static_cast<double>(step) * minor / major;
    const double projectedHeightStep = dz / major * 32.0;
    if (!std::isfinite(projectedHeightStep)
        || std::abs(projectedHeightStep) > std::numeric_limits<float>::max())
        return Reject("TERRAIN_SHADOW_LIGHT_STEP", "阴影投影步长不能表示为有限 binary32 值。");
    const float minorStep = static_cast<float>(projectedMinorStep);
    const float heightStep = static_cast<float>(projectedHeightStep);
    if (cancellation.stop_requested())
        return Reject("TERRAIN_SHADOW_CANCELLED", "阴影投影已取消，未返回部分候选。");

    try {
        TerrainShadowProjection candidate;
        candidate.width = samples.width;
        candidate.height = samples.height;
        const auto consume = [&] {
            if (candidate.work == limits.maximumWork) return false;
            ++candidate.work;
            return true;
        };
        for (const auto value : samples.receiverHeights) {
            if (candidate.work % 1024 == 0 && cancellation.stop_requested())
                return Reject("TERRAIN_SHADOW_CANCELLED", "阴影高度检查已取消，未返回部分候选。");
            if (!consume()) return Reject("TERRAIN_SHADOW_BUDGET", "阴影高度检查超过工作预算。");
            if (!std::isfinite(value))
                return Reject("TERRAIN_SHADOW_HEIGHT", "阴影高度场包含非有限样本，不能返回部分候选。");
        }
        candidate.terrainShadowBits.assign(count, 0);
        for (std::uint32_t y = 0; y < samples.height; ++y) {
            for (std::uint32_t x = 0; x < samples.width; ++x) {
                if (cancellation.stop_requested())
                    return Reject("TERRAIN_SHADOW_CANCELLED", "阴影投影已取消，未返回部分候选。");
                if (!consume()) return Reject("TERRAIN_SHADOW_BUDGET", "阴影投影起点超过工作预算。");
                std::int64_t majorAt = static_cast<std::int64_t>(majorX ? x : y) + step;
                double minorAt = static_cast<double>(majorX ? y : x) + minorStep;
                double rayHeight = static_cast<double>(samples.receiverHeights[
                    static_cast<std::size_t>(y) * samples.width + x]) + heightStep;
                while (true) {
                    // minorStep 的绝对值不超过 1，主轴至多越过宽/高一次，
                    // 因而 minorAt 始终在 int64 可表示范围内。保留原版向零
                    // 取整：例如 -0.5 仍映射到第 0 行，再做整数边界检查。
                    const auto minorPixel = static_cast<std::int64_t>(minorAt);
                    const auto px = majorX ? majorAt : minorPixel;
                    const auto py = majorX ? minorPixel : majorAt;
                    if (px < 0 || py < 0 || px >= samples.width || py >= samples.height) break;
                    if (!consume()) return Reject("TERRAIN_SHADOW_BUDGET", "阴影投影步进超过工作预算。");
                    const auto index = static_cast<std::size_t>(py) * samples.width + static_cast<std::size_t>(px);
                    // 相等时停止，不能把接触点或其后的像素继续置阴影。
                    if (rayHeight <= samples.receiverHeights[index]) break;
                    candidate.terrainShadowBits[index] = 1;
                    majorAt += step;
                    minorAt += minorStep;
                    rayHeight += heightStep;
                }
            }
        }
        return Result<TerrainShadowProjection>::Success(std::move(candidate));
    } catch (const std::bad_alloc&) {
        return Reject("TERRAIN_SHADOW_ALLOCATION", "阴影候选分配失败，未返回部分候选。");
    } catch (const std::length_error&) {
        return Reject("TERRAIN_SHADOW_ALLOCATION", "阴影候选容量不足，未返回部分候选。");
    }
}

} // namespace War3Map::MapData
