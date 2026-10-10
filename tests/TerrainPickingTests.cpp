#include "War3MapCore/Render/TerrainPicking.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace {

using namespace War3Map;
using namespace War3Map::MapData;
using namespace War3Map::Render;

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void Near(double actual, double expected, const char* message, double tolerance = 1e-6) {
    if (!std::isfinite(actual) || std::abs(actual - expected) > tolerance) throw std::runtime_error(message);
}

TerrainGridSnapshot MakeGrid(std::uint32_t width, std::uint32_t height,
    const std::function<float(std::uint32_t, std::uint32_t)>& elevation = {}, std::array<float, 2> offset = {}) {
    TerrainGridSnapshot grid;
    grid.header.width = width;
    grid.header.height = height;
    grid.header.offset = offset;
    grid.vertices.reserve(static_cast<std::size_t>(width) * height);
    grid.tiles.reserve(static_cast<std::size_t>(width - 1) * (height - 1));
    grid.minimum.fill(std::numeric_limits<float>::infinity());
    grid.maximum.fill(-std::numeric_limits<float>::infinity());
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            TerrainVertex vertex;
            vertex.position = {static_cast<float>(static_cast<double>(offset[0]) + x * 128.0),
                static_cast<float>(static_cast<double>(offset[1]) + y * 128.0), elevation ? elevation(x, y) : 0};
            vertex.groundHeightRaw = 8192;
            vertex.cliffAndLayerRaw = 2;
            for (std::size_t axis = 0; axis < 3; ++axis) {
                grid.minimum[axis] = std::min(grid.minimum[axis], vertex.position[axis]);
                grid.maximum[axis] = std::max(grid.maximum[axis], vertex.position[axis]);
            }
            grid.vertices.push_back(vertex);
            if (x + 1 < width && y + 1 < height) {
                const auto sw = y * width + x;
                grid.tiles.push_back({{sw, sw + 1, sw + width, sw + width + 1}});
            }
        }
    }
    return grid;
}

TerrainPickResult Success(const TerrainGridSnapshot& grid, const TerrainRay& ray, const TerrainPickLimits& limits = {}) {
    auto result = PickTerrainHeightPreview(grid, ray, limits);
    if (!result.Succeeded()) {
        throw std::runtime_error("拾取意外失败：" + (result.diagnostics.empty() ? std::string("无诊断") : result.diagnostics.front().code));
    }
    return std::move(*result.value);
}

void Rejected(const TerrainGridSnapshot& grid, const TerrainRay& ray, std::string_view code,
    const TerrainPickLimits& limits = {}) {
    const auto result = PickTerrainHeightPreview(grid, ray, limits);
    Require(!result.Succeeded() && !result.value, "错误不得返回部分拾取结果");
    Require(result.diagnostics.size() == 1 && result.diagnostics.front().severity == DiagnosticSeverity::Error,
        "失败需要一条明确错误");
    if (result.diagnostics.front().code != code) throw std::runtime_error("错误代码不符：" + result.diagnostics.front().code);
}

void TestSlopeBarycentricAndDirectionScale() {
    const auto grid = MakeGrid(2, 2, [](auto x, auto y) { return static_cast<float>(32 * x + 64 * y); });
    auto result = Success(grid, {{32, 48, 100}, {0, 0, -7}});
    Require(result.hit && result.visitedTiles == 1 && result.hit->triangle == 0, "西南三角形拾取错误");
    Near(result.hit->distance, 68, "distance 应为世界距离而非输入方向参数");
    Near(result.hit->position[2], 32, "坡面高度插值错误");
    Near(result.hit->barycentric[0], 0.375, "SW 重心坐标错误");
    Near(result.hit->barycentric[1], 0.25, "SE 重心坐标错误");
    Near(result.hit->barycentric[2], 0.375, "NW 重心坐标错误");
    result = Success(grid, {{96, 96, 100}, {0, 0, -1e308}});
    Require(result.hit && result.hit->triangle == 1, "东北三角形或大方向归一化错误");
    Near(result.hit->position[2], 72, "东北坡面高度错误");
    Near(result.hit->distance, 28, "有限大方向不应溢出");
    const std::array<double, 3> expected{0.25, 0.5, 0.25};
    for (std::size_t i = 0; i < 3; ++i) Near(result.hit->barycentric[i], expected[i], "东北三角形重心顺序错误");
    const auto tiny = Success(grid, {{96, 96, 100}, {0, 0, -2e-12}});
    Require(tiny.hit.has_value(), "合法短方向应可归一化");
    Near(tiny.hit->distance, result.hit->distance, "缩放方向不应改变拾取位置");
}

void TestNonPlanarDiagonal() {
    auto grid = MakeGrid(2, 2, [](auto x, auto y) { return x == 1 && y == 1 ? 128.0f : 0.0f; });
    grid.vertices[3].textureAndFlagsRaw = 0xff;
    grid.vertices[3].waterBaseHeight = 500;
    const auto originalPosition = grid.vertices[3].position;
    auto lower = Success(grid, {{32, 32, 200}, {0, 0, -1}});
    auto upper = Success(grid, {{96, 96, 200}, {0, 0, -1}});
    auto diagonal = Success(grid, {{64, 64, 200}, {0, 0, -1}});
    Require(lower.hit && upper.hit && diagonal.hit, "非共面四边形应命中");
    Near(lower.hit->position[2], 0, "不得切换为 SW-NE 对角线");
    Near(upper.hit->position[2], 64, "NW-SE 三角高度错误");
    Require(diagonal.hit->triangle == 0, "共享对角线必须选择较小 triangle");
    Near(diagonal.hit->position[2], 0, "共享对角线高度错误");
    Require(grid.vertices[3].textureAndFlagsRaw == 0xff && grid.vertices[3].waterBaseHeight == 500
        && grid.vertices[3].position == originalPosition, "只读拾取不应改地形或应用水面/坡道标志");
}

void TestMissDistanceAndSurfaceOrigin() {
    const auto grid = MakeGrid(3, 3);
    for (const TerrainRay ray : {TerrainRay{{32, 32, -10}, {0, 0, -1}}, TerrainRay{{32, 32, 10}, {0, 0, 1}},
        TerrainRay{{-1, 32, 10}, {0, 0, -1}}, TerrainRay{{32, 32, 10}, {1, 0, 0}}}) {
        const auto result = Success(grid, ray, {1000, 0});
        Require(!result.hit && result.visitedTiles == 0, "AABB 确定未命中不应消费 tile 预算");
    }
    const auto shortRay = Success(grid, {{32, 32, 100}, {0, 0, -1}}, {99, 8192});
    Require(!shortRay.hit && shortRay.visitedTiles == 0, "最大世界距离应裁剪命中");
    const auto exact = Success(grid, {{32, 32, 100}, {0, 0, -1}}, {100, 8192});
    Require(exact.hit.has_value(), "最大距离端点应包含");
    for (const auto direction : {-1.0, 1.0}) {
        const auto surface = Success(grid, {{32, 32, 0}, {0, 0, direction}}, {0, 8192});
        Require(surface.hit && surface.hit->distance == 0, "从表面出发应包含零距离交点");
    }
    // 方向法向分量很小，但交点在 100 世界单位处；不能按固定角度阈值判平行。
    const auto shallow = Success(grid, {{0, 16, 1e-11}, {1, 0, -1e-13}});
    Require(shallow.hit.has_value(), "近水平有限射线不应误判为平行");
    Near(shallow.hit->distance, 100, "近水平射线交点距离错误");
}

void TestBoundaryTieBreak() {
    const auto grid = MakeGrid(3, 3);
    const auto corner = Success(grid, {{128, 128, 100}, {0, 0, -1}});
    Require(corner.hit && corner.hit->tileIndex == 0 && corner.hit->triangle == 1 && corner.visitedTiles == 4,
        "四格共享顶点应检查相邻格子并选择较小原始索引");
    const auto edge = Success(grid, {{128, 32, 100}, {0, 0, -1}});
    Require(edge.hit && edge.hit->tileIndex == 0 && edge.visitedTiles == 2, "格线上应稳定选择西侧格子");
    for (const auto sign : {-1.0, 1.0}) {
        const auto crossing = Success(grid, {{128 - sign * 64, 128, 64}, {sign, 0, -1}});
        Require(crossing.hit && crossing.hit->tileIndex == 0 && crossing.hit->triangle == 1,
            "正反向射线的同点裁决必须一致");
        Near(crossing.hit->position[0], 128, "边界命中 X 错误");
        Near(crossing.hit->position[1], 128, "边界命中 Y 错误");
    }
    const auto outer = Success(grid, {{256, 256, 1}, {0, 0, -1}});
    Require(outer.hit && outer.hit->tileIndex == 3 && outer.visitedTiles == 1, "地图最后一行/列的外边界应包含");
    const auto offset = MakeGrid(4, 3, {}, {0.1f, -8192.3f});
    const auto east = offset.vertices.back().position;
    const auto rounded = Success(offset, {{east[0], east[1], 4}, {0, 0, -1}});
    Require(rounded.hit && rounded.hit->tileIndex == 5, "小数地图偏移必须使用解析器的 float 网格落点");
}

void TestInvalidAndCoplanar() {
    const auto grid = MakeGrid(2, 2);
    const TerrainRay vertical{{32, 32, 1}, {0, 0, -1}};
    Rejected(grid, {{32, 32, 1}, {0, 0, 0}}, "TERRAIN_PICK_DIRECTION_ZERO");
    Rejected(grid, {{32, 32, 1}, {1e-13, 1e-13, 0}}, "TERRAIN_PICK_DIRECTION_ZERO");
    Rejected(grid, {{32, 32, 1}, {0, 0, -1e-12}}, "TERRAIN_PICK_DIRECTION_ZERO");
    Rejected(grid, {{std::numeric_limits<double>::quiet_NaN(), 0, 1}, {0, 0, -1}}, "TERRAIN_PICK_RAY_INVALID");
    Rejected(grid, {{0, 0, 1}, {std::numeric_limits<double>::infinity(), 0, -1}}, "TERRAIN_PICK_RAY_INVALID");
    Rejected(grid, {{1e9 + 1, 0, 1}, {0, 0, -1}}, "TERRAIN_PICK_RAY_INVALID");
    Rejected(grid, vertical, "TERRAIN_PICK_LIMITS", {-1, 8192});
    Rejected(grid, vertical, "TERRAIN_PICK_LIMITS", {1e9 + 1, 8192});
    Rejected(grid, vertical, "TERRAIN_PICK_LIMITS", {1e9, 8193});
    Rejected(grid, vertical, "TERRAIN_PICK_LIMITS", {std::numeric_limits<double>::infinity(), 8192});
    Rejected(grid, {{-64, 32, 0}, {1, 0, 0}}, "TERRAIN_PICK_COPLANAR");
    Rejected(grid, {{32, 32, 0}, {1, 0, 0}}, "TERRAIN_PICK_COPLANAR");
    auto broken = grid;
    broken.tiles.clear();
    Rejected(broken, vertical, "TERRAIN_PICK_GRID_INVALID");
    broken = grid;
    broken.tiles[0].vertices[0] = 100000;
    Rejected(broken, vertical, "TERRAIN_PICK_GRID_INVALID");
    broken = grid;
    broken.vertices[1].position[2] = std::numeric_limits<float>::quiet_NaN();
    Rejected(broken, vertical, "TERRAIN_PICK_GRID_INVALID");
    broken = grid;
    broken.vertices[1].position = broken.vertices[0].position;
    Rejected(broken, vertical, "TERRAIN_PICK_DEGENERATE");
    broken = grid;
    broken.vertices[1].position[0] += 1;
    Rejected(broken, vertical, "TERRAIN_PICK_GRID_INVALID");
    broken = grid;
    broken.maximum[0] += 1;
    Rejected(broken, vertical, "TERRAIN_PICK_GRID_INVALID");
}

void TestBudgetAndLargeGridTraversal() {
    const auto cornerGrid = MakeGrid(3, 3);
    Rejected(cornerGrid, {{128, 128, 100}, {0, 0, -1}}, "TERRAIN_PICK_BUDGET", {1000, 1});
    Rejected(cornerGrid, {{32, 32, 100}, {0, 0, -1}}, "TERRAIN_PICK_BUDGET", {1000, 0});
    // 超过两百万三角形；远角峰值让 Z=64 的水平射线通过 AABB，但所经行全平，确定未命中。
    const auto large = MakeGrid(1025, 1025, [](auto x, auto y) { return x == 1024 && y == 1024 ? 128.0f : 0.0f; });
    const TerrainRay traverse{{-64, 64, 64}, {1, 0, 0}};
    Rejected(large, traverse, "TERRAIN_PICK_BUDGET", {1e9, 10});
    const auto miss = Success(large, traverse);
    Require(!miss.hit && miss.visitedTiles == 1024, "大图射线应只访问所经的一行，不扫描整图三角形");
    const auto edgeMiss = Success(large, {{-64, 128, 64}, {1, 0, 0}});
    Require(!edgeMiss.hit && edgeMiss.visitedTiles == 2048, "沿格线射线应只访问相邻两行");
    const auto vertical = Success(large, {{500 * 128.0 + 32, 500 * 128.0 + 32, 64}, {0, 0, -1}});
    Require(vertical.hit && vertical.visitedTiles == 1 && vertical.hit->tileIndex == 500 * 1024 + 500,
        "大图垂直拾取应保留原 tile 索引且只检查一格");
    const auto reverse = Success(large, {{1024 * 128.0 + 64, 64, 64}, {-1, 0, 0}});
    Require(!reverse.hit && reverse.visitedTiles == 1024, "负方向遍历不能丢边界格子或停滞");
}

void TestNearestAcrossTilesAndCornerTraversal() {
    const auto ridge = MakeGrid(5, 2, [](auto x, auto) { return x == 2 ? 128.0f : 0.0f; });
    const auto forward = Success(ridge, {{-64, 32, 64}, {1, 0, 0}}, {1000, 2});
    Require(forward.hit && forward.hit->tileIndex == 1 && forward.visitedTiles == 2,
        "最近坡面命中应在两格预算内停止，不继续访问后方坡面");
    Near(forward.hit->position[0], 192, "正向射线最近坡面错误");
    Near(forward.hit->distance, 256, "从 AABB 外进入后的距离应包含进入前路程");
    const auto reverse = Success(ridge, {{576, 32, 64}, {-1, 0, 0}}, {1000, 2});
    Require(reverse.hit && reverse.hit->tileIndex == 2 && reverse.visitedTiles == 2,
        "反向射线应优先命中另一面，不能依赖 tile 索引排序选择最近面");
    Near(reverse.hit->position[0], 320, "反向射线最近坡面错误");
    const auto diagonal = MakeGrid(5, 5, [](auto x, auto y) { return x == 0 && y == 4 ? 128.0f : 0.0f; });
    const auto miss = Success(diagonal, {{-64, -64, 64}, {1, 1, 0}});
    Require(!miss.hit && miss.visitedTiles == 10,
        "穿越内部顶点时应覆盖对角线两侧格子，但不能全图遍历");
}

} // namespace

// 高度采样与拾取同一三角划分（对角线 NW→SE），网格外为空。
void TestSampleHeight() {
    const auto grid = MakeGrid(2, 2, [](auto x, auto y) { return x == 1 && y == 1 ? 128.0f : 0.0f; }, {-64.0f, -64.0f});
    Near(*SampleTerrainHeight(grid, -64, -64), 0, "SW corner");
    Near(*SampleTerrainHeight(grid, 64, 64), 128, "NE corner");
    // 西南三角（u+v<=1）全为 0；东北三角在 NE 处抬升。
    Near(*SampleTerrainHeight(grid, -64 + 32, -64 + 32), 0, "SW triangle is flat");
    Near(*SampleTerrainHeight(grid, -64 + 96, -64 + 96), 64, "NE triangle interpolates toward NE");
    Require(!SampleTerrainHeight(grid, 65, 0) && !SampleTerrainHeight(grid, 0, -65), "outside grid is empty");
    Require(!SampleTerrainHeight(grid, std::nan(""), 0), "non-finite input is empty");
}

int main() {
    try {
        TestSampleHeight();
        TestSlopeBarycentricAndDirectionScale();
        TestNonPlanarDiagonal();
        TestMissDistanceAndSurfaceOrigin();
        TestBoundaryTieBreak();
        TestInvalidAndCoplanar();
        TestNearestAcrossTilesAndCornerTraversal();
        TestBudgetAndLargeGridTraversal();
        std::cout << "TerrainPickingTests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
