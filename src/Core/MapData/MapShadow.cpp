#include "War3MapCore/MapData/MapShadow.h"

#include <new>
#include <stdexcept>
#include <utility>

// 根据公开的 headerless 原字节布局独立实现，不复制第三方解析器：
// https://github.com/Drake53/War3Net/blob/master/src/War3Net.Build.Core/Serialization/Binary/Environment/MapShadowMap.cs
namespace War3Map::MapData {
namespace {

Status CheckShape(std::uint32_t width, std::uint32_t height, const ShadowReadLimits& limits) {
    const ShadowReadLimits hard;
    if (limits.maxBytes > hard.maxBytes || limits.maxDimension > hard.maxDimension || limits.maxCells > hard.maxCells)
        return Status::Failure("MapShadow", "SHADOW_LIMITS", "调用方只能收紧 SHD 硬预算。");
    if (width == 0 || height == 0)
        return Status::Failure("MapShadow", "SHADOW_DIMENSIONS", "SHD 网格尺寸必须为正，不能从原字节猜测。");
    if (width > limits.maxDimension || height > limits.maxDimension
        || static_cast<std::size_t>(height) > limits.maxCells / width)
        return Status::Failure("MapShadow", "SHADOW_GRID_LIMIT", "SHD 网格尺寸或格数超过预算。");
    return Status::Ok();
}

Status Reject(const char* code, const char* message) {
    auto result = Status::Failure("MapShadow", code, message);
    result.diagnostics.front().origin = OriginSpan{"war3map.shd", 0, 0, {}};
    return result;
}

} // namespace

Result<ShadowGridSnapshot> ParseMapShadow(std::span<const std::byte> bytes,
    std::uint32_t width, std::uint32_t height, const ShadowReadLimits& limits) {
    const auto fail = [](const Status& status) -> Result<ShadowGridSnapshot> { return {{}, status.diagnostics}; };
    const auto shape = CheckShape(width, height, limits);
    if (!shape.Succeeded()) return fail(shape);
    if (bytes.size() > limits.maxBytes) return fail(Reject("SHADOW_BYTE_LIMIT", "SHD 超过读取字节预算。"));
    const auto count = static_cast<std::size_t>(width) * height; // CheckShape 已先用除法核过乘积。
    if (bytes.size() < count) return fail(Reject("SHADOW_TRUNCATED", "SHD 强度字节少于给定网格。"));
    if (bytes.size() > count) return fail(Reject("SHADOW_TRAILING_DATA", "SHD 超出给定网格，拒绝丢弃尾字节。"));
    try {
        ShadowGridSnapshot result;
        result.width = width; result.height = height;
        result.intensities.reserve(count);
        for (const auto value : bytes) result.intensities.push_back(std::to_integer<std::uint8_t>(value));
        return Result<ShadowGridSnapshot>::Success(std::move(result));
    } catch (const std::bad_alloc&) {
        return fail(Reject("SHADOW_ALLOCATION", "SHD 分配失败，未返回部分网格。"));
    } catch (const std::length_error&) {
        return fail(Reject("SHADOW_ALLOCATION", "SHD 容器容量不足，未返回部分网格。"));
    }
}

Result<std::vector<std::byte>> SerializeMapShadow(const ShadowGridSnapshot& shadow, const ShadowReadLimits& limits) {
    const auto fail = [](const Status& status) -> Result<std::vector<std::byte>> { return {{}, status.diagnostics}; };
    const auto shape = CheckShape(shadow.width, shadow.height, limits);
    if (!shape.Succeeded()) return fail(shape);
    const auto count = static_cast<std::size_t>(shadow.width) * shadow.height;
    if (shadow.intensities.size() != count)
        return fail(Reject("SHADOW_INCONSISTENT", "SHD 网格尺寸与强度字节数量不一致。"));
    if (count > limits.maxBytes) return fail(Reject("SHADOW_BYTE_LIMIT", "SHD 写出超过字节预算。"));
    try {
        std::vector<std::byte> result;
        result.reserve(count);
        for (const auto value : shadow.intensities) result.push_back(static_cast<std::byte>(value));
        return Result<std::vector<std::byte>>::Success(std::move(result));
    } catch (const std::bad_alloc&) {
        return fail(Reject("SHADOW_ALLOCATION", "SHD 写出分配失败，未返回部分输出。"));
    } catch (const std::length_error&) {
        return fail(Reject("SHADOW_ALLOCATION", "SHD 写出容量不足，未返回部分输出。"));
    }
}

} // namespace War3Map::MapData
