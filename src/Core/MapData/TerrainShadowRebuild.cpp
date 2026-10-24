#include "War3MapCore/MapData/TerrainShadowRebuild.h"

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

Result<TerrainShadowProjection> ProjectTerrainShadow(const TerrainShadowHeightSamples& samples,
    const TerrainShadowRebuildLimits& limits, std::stop_token cancellation) {
    const TerrainShadowRebuildLimits hard;
    if (limits.maximumDimension > hard.maximumDimension || limits.maximumPixels > hard.maximumPixels
        || limits.maximumWork > hard.maximumWork)
        return Reject("TERRAIN_SHADOW_LIMITS", "阴影投影只能收紧硬预算。");
    if (samples.width == 0 || samples.height == 0 || samples.width % 4 != 0 || samples.height % 4 != 0)
        return Reject("TERRAIN_SHADOW_DIMENSIONS", "阴影高度场宽高必须为正且为每地格四像素的完整尺寸。");
    if (samples.width > limits.maximumDimension || samples.height > limits.maximumDimension
        || samples.height > limits.maximumPixels / samples.width)
        return Reject("TERRAIN_SHADOW_BUDGET", "阴影高度场尺寸或像素数超过预算。");
    const auto count = static_cast<std::size_t>(samples.width) * samples.height;
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
