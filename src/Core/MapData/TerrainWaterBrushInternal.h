#pragma once

#include <cstdint>
#include <optional>

// 数值核与事务共享；测试可独立给出中心/距离/编码边界，避免借另一次笔刷生成期望值。
namespace War3Map::MapData::TerrainWaterBrushDetail {

struct HeightFalloffCenter {
    float delta, radiusSquared;
    double radius;
    std::int32_t extent;
};

[[nodiscard]] std::optional<HeightFalloffCenter> MakeHeightFalloffCenter(
    double ground, float minimum, float maximum);
[[nodiscard]] std::optional<float> HeightFalloffWeight(const HeightFalloffCenter& center, std::int32_t distanceSquared);
[[nodiscard]] bool HeightFalloffWrites(double ground, float minimum, float maximum, float weight);
[[nodiscard]] std::optional<std::uint16_t> EncodeBaseHeight(double base);

} // namespace War3Map::MapData::TerrainWaterBrushDetail
