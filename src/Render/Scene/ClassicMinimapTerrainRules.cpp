#include "War3MapCore/Render/ClassicMinimapTerrainRules.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>

namespace War3Map::Render {
namespace {

using Grid = MapData::TerrainGridSnapshot;
using Vertex = MapData::TerrainVertex;
constexpr std::string_view kSubsystem = "Render.ClassicMinimapTerrainRules";
constexpr std::array<std::array<int, 2>, 4> kCorners{{{0, 0}, {1, 0}, {0, 1}, {1, 1}}};

enum class RampClass { Ordinary, High, Low, Straight, Saddle, Unknown };

bool ValidGrid(const Grid& terrain) {
    return terrain.header.version == 11 && terrain.header.width >= 2 && terrain.header.height >= 2
        && terrain.header.width <= 2049 && terrain.header.height <= 2049
        && static_cast<std::uint64_t>(terrain.header.width) * terrain.header.height == terrain.vertices.size();
}

const Vertex* At(const Grid& terrain, int x, int y) {
    if (x < 0 || y < 0 || static_cast<std::uint32_t>(x) >= terrain.header.width
        || static_cast<std::uint32_t>(y) >= terrain.header.height) return nullptr;
    return &terrain.vertices[static_cast<std::size_t>(y) * terrain.header.width + static_cast<std::size_t>(x)];
}

bool QuadInside(const Grid& terrain, int x, int y) {
    return x >= 0 && y >= 0 && static_cast<std::uint32_t>(x) + 1 < terrain.header.width
        && static_cast<std::uint32_t>(y) + 1 < terrain.header.height;
}

bool MixedLevels(const Grid& terrain, int x, int y) {
    const auto level = At(terrain, x, y)->CliffLevel();
    return std::any_of(kCorners.begin() + 1, kCorners.end(), [&](const auto& delta) {
        return At(terrain, x + delta[0], y + delta[1])->CliffLevel() != level;
    });
}

// 1.27a 的高端分类看所有相邻完整地格中的 ramp 顶点范围，不看普通顶点。
bool HighRampVertex(const Grid& terrain, int x, int y) {
    int low = 16, high = -1;
    for (const auto& delta : kCorners) {
        const auto cx = x - delta[0], cy = y - delta[1];
        if (!QuadInside(terrain, cx, cy)) continue;
        for (const auto& corner : kCorners) {
            const auto* vertex = At(terrain, cx + corner[0], cy + corner[1]);
            if (!vertex->IsRamp()) continue;
            low = std::min(low, static_cast<int>(vertex->CliffLevel()));
            high = std::max(high, static_cast<int>(vertex->CliffLevel()));
        }
    }
    return high >= 0 && low != high && At(terrain, x, y)->CliffLevel() == high;
}

using Neighborhood = std::array<const Vertex*, 9>; // x-major：索引=(dx+1)*3+dy+1。

Neighborhood Around(const Grid& terrain, int x, int y) {
    Neighborhood result{};
    for (int dx = -1; dx <= 1; ++dx)
        for (int dy = -1; dy <= 1; ++dy)
            result[static_cast<std::size_t>((dx + 1) * 3 + dy + 1)] = At(terrain, x + dx, y + dy);
    return result;
}

bool OppositeRampLevelsDiffer(const Vertex* first, const Vertex* second) {
    return first && second && first->IsRamp() && second->IsRamp()
        && first->CliffLevel() != second->CliffLevel();
}

// 原版 A/B/H/L/R/S 的几何分类。独立表达邻域约束，不使用渲染预览的近似抬升。
RampClass Classify(const Grid& terrain, int x, int y) {
    const auto* center = At(terrain, x, y);
    if (!center) return RampClass::Unknown;
    if (!center->IsRamp()) return RampClass::Ordinary;
    if (HighRampVertex(terrain, x, y)) return RampClass::High;

    const auto neighbors = Around(terrain, x, y);
    int low = center->CliffLevel(), high = low, rampLow = low, rampHigh = low;
    bool allRamp = true;
    for (const auto* vertex : neighbors) {
        if (!vertex) { allRamp = false; continue; }
        const auto level = static_cast<int>(vertex->CliffLevel());
        low = std::min(low, level); high = std::max(high, level);
        if (!vertex->IsRamp()) { allRamp = false; continue; }
        rampLow = std::min(rampLow, level); rampHigh = std::max(rampHigh, level);
    }
    const int span = high - low;
    if ((allRamp && span == 0) || rampHigh - rampLow > 1) return RampClass::Unknown;
    const bool acrossX = OppositeRampLevelsDiffer(neighbors[1], neighbors[7]);
    const bool acrossY = OppositeRampLevelsDiffer(neighbors[3], neighbors[5]);
    if (!acrossX && !acrossY) return allRamp ? RampClass::Saddle : RampClass::Low;
    if (span > 1) return RampClass::Unknown;
    return acrossX && acrossY ? RampClass::Saddle : RampClass::Straight;
}

// S 形坡道由 3x3 四角 L/H 及四条边中点 R 判定：一低三高=3/4，三低一高=1/4，
// 对角两低两高=1/2。其它形态原版返回 -1，不生成猜测的水深地高。
std::optional<double> SaddleOffset(const Grid& terrain, int x, int y) {
    std::array<RampClass, 9> classes{};
    for (int dx = -1; dx <= 1; ++dx) for (int dy = -1; dy <= 1; ++dy) {
        if (!At(terrain, x + dx, y + dy)) return std::nullopt;
        classes[static_cast<std::size_t>((dx + 1) * 3 + dy + 1)] = Classify(terrain, x + dx, y + dy);
    }
    constexpr std::array<std::array<int, 2>, 4> corners{{{0, 0}, {0, 2}, {2, 0}, {2, 2}}};
    int lowCount = 0, highCount = 0, lastLow = -1, lastHigh = -1;
    for (int i = 0; i < 4; ++i) {
        const auto& corner = corners[static_cast<std::size_t>(i)];
        const auto value = classes[static_cast<std::size_t>(corner[0] * 3 + corner[1])];
        if (value == RampClass::Low) { ++lowCount; lastLow = i; }
        if (value == RampClass::High) { ++highCount; lastHigh = i; }
    }
    const auto aroundCorner = [&](int cornerIndex, RampClass opposite) {
        const auto& c = corners[static_cast<std::size_t>(cornerIndex)];
        return classes[static_cast<std::size_t>(1 + 3 * c[0])] == RampClass::Straight
            && classes[static_cast<std::size_t>(3 + c[1])] == RampClass::Straight
            && classes[static_cast<std::size_t>(7 - 3 * c[0])] == opposite
            && classes[static_cast<std::size_t>(5 - c[1])] == opposite;
    };
    if (lowCount == 1 && highCount == 3 && aroundCorner(lastLow, RampClass::High)) return 96.0;
    if (lowCount == 3 && highCount == 1 && aroundCorner(lastHigh, RampClass::Low)) return 32.0;
    if (lowCount == 2 && highCount == 2) {
        const auto& c = corners[static_cast<std::size_t>(lastLow)];
        const auto opposite = static_cast<std::size_t>(8 - 3 * c[0] - c[1]);
        if (classes[opposite] == RampClass::Low
            && classes[1] == RampClass::Straight && classes[7] == RampClass::Straight
            && classes[3] == RampClass::Straight && classes[5] == RampClass::Straight) return 64.0;
    }
    return std::nullopt;
}

std::uint8_t CliffPalette(const Grid& terrain, int x, int y) {
    for (const auto& delta : kCorners) {
        const auto index = At(terrain, x + delta[0], y + delta[1])->CliffTextureIndex();
        if (index != 15) return index;
    }
    for (int nx = x - 2; nx <= x + 2; ++nx) for (int ny = y - 2; ny <= y + 2; ++ny) {
        const auto* vertex = At(terrain, nx, ny);
        if (vertex && vertex->CliffTextureIndex() != 15) return vertex->CliffTextureIndex();
    }
    return 0;
}

template<class T> Result<T> Failure(const char* code, const char* message) {
    return Result<T>::Failure(std::string(kSubsystem), code, message);
}

} // namespace

Result<std::optional<std::uint8_t>> ClassicMinimapCliffTextureAt(
    const Grid& terrain, std::uint32_t x, std::uint32_t y) {
    using Output = Result<std::optional<std::uint8_t>>;
    if (!ValidGrid(terrain)) return Failure<std::optional<std::uint8_t>>("CLASSIC_MINIMAP_TERRAIN_GRID", "悬崖取色需要完整经典 W3E v11 顶点网格。");
    if (x >= terrain.header.width || y >= terrain.header.height)
        return Failure<std::optional<std::uint8_t>>("CLASSIC_MINIMAP_TERRAIN_COORDINATE", "悬崖取色坐标超出地形网格。");
    for (const auto& delta : kCorners) {
        const auto cx = static_cast<int>(x) - delta[0], cy = static_cast<int>(y) - delta[1];
        if (!QuadInside(terrain, cx, cy) || !MixedLevels(terrain, cx, cy)) continue;
        bool rampSurface = false;
        for (const auto& corner : kCorners) {
            const auto shape = Classify(terrain, cx + corner[0], cy + corner[1]);
            if (shape == RampClass::Unknown)
                return Failure<std::optional<std::uint8_t>>("CLASSIC_MINIMAP_RAMP_UNRESOLVED", "悬崖邻格含原版未识别的坡道形态。");
            rampSurface |= shape == RampClass::Straight || shape == RampClass::Saddle;
        }
        if (rampSurface) return Output::Success(std::nullopt);
        return Output::Success(CliffPalette(terrain, cx, cy));
    }
    return Output::Success(std::nullopt);
}

Result<double> ClassicMinimapGroundHeightAt(const Grid& terrain, std::uint32_t x, std::uint32_t y) {
    if (!ValidGrid(terrain)) return Failure<double>("CLASSIC_MINIMAP_TERRAIN_GRID", "地高查询需要完整经典 W3E v11 顶点网格。");
    if (x >= terrain.header.width || y >= terrain.header.height)
        return Failure<double>("CLASSIC_MINIMAP_TERRAIN_COORDINATE", "地高查询坐标超出地形网格。");
    const auto& vertex = *At(terrain, static_cast<int>(x), static_cast<int>(y));
    if (!std::isfinite(vertex.position[2])) return Failure<double>("CLASSIC_MINIMAP_HEIGHT_NONFINITE", "地形顶点地高不是有限数。");
    double offset = 0;
    const auto shape = Classify(terrain, static_cast<int>(x), static_cast<int>(y));
    if (shape == RampClass::Straight) offset = 64.0;
    else if (shape == RampClass::Saddle) {
        const auto saddle = SaddleOffset(terrain, static_cast<int>(x), static_cast<int>(y));
        if (!saddle) return Failure<double>("CLASSIC_MINIMAP_RAMP_UNRESOLVED", "S 形坡道未满足原版 L/H/R 邻域规则，不能猜测水深地高。");
        offset = *saddle;
    } else if (shape == RampClass::Unknown)
        return Failure<double>("CLASSIC_MINIMAP_RAMP_UNRESOLVED", "坡道形态没有原版可确认的地高规则。");
    return Result<double>::Success(static_cast<double>(vertex.position[2]) + offset);
}

} // namespace War3Map::Render
