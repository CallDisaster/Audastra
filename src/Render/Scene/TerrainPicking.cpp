#include "War3MapCore/Render/TerrainPicking.h"
#include "War3MapCore/MapData/TerrainEditing.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <unordered_set>

namespace War3Map::Render {
namespace {

using Vector = std::array<double, 3>;
constexpr double DistanceTolerance = 1e-6;
constexpr double BarycentricTolerance = 1e-9;
constexpr double ProjectedClipTolerance = 1e-8;
constexpr double MaximumDistance = 1e9;
constexpr std::size_t MaximumVisitedTiles = 8192;

struct PickFailure {
    const char* code;
    const char* message;
};

[[noreturn]] void Fail(const char* code, const char* message) { throw PickFailure{code, message}; }

Vector Difference(const Vector& a, const Vector& b) {
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}

Vector Cross(const Vector& a, const Vector& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}

double Dot(const Vector& a, const Vector& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

double Coordinate(const MapData::TerrainGridSnapshot& grid, std::size_t axis, std::uint32_t index) {
    // 与 W3E 解析器相同的 float 落点；带小数偏移时不能假定相邻 float 恰好相差 128。
    return static_cast<float>(static_cast<double>(grid.header.offset[axis])
        + static_cast<double>(index) * MapData::TerrainGridSnapshot::tileSize);
}

void ValidateGrid(const MapData::TerrainGridSnapshot& grid) {
    const auto width = grid.header.width, height = grid.header.height;
    if (grid.header.version != 11 || width < 2 || height < 2 || width > 2049 || height > 2049
        || grid.vertices.size() != static_cast<std::size_t>(width) * height
        || grid.tiles.size() != static_cast<std::size_t>(width - 1) * (height - 1)) {
        Fail("TERRAIN_PICK_GRID_INVALID", "拾取需要完整的经典 W3E 高度网格。");
    }
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const double bound = axis == 2 ? 65536.0 : 16777216.0;
        if (!std::isfinite(grid.minimum[axis]) || !std::isfinite(grid.maximum[axis])
            || grid.minimum[axis] > grid.maximum[axis]
            || std::abs(grid.minimum[axis]) > bound || std::abs(grid.maximum[axis]) > bound) {
            Fail("TERRAIN_PICK_GRID_INVALID", "地形范围必须有限并符合经典网格坐标预算。");
        }
    }
    for (std::size_t axis = 0; axis < 2; ++axis) {
        const auto count = axis == 0 ? width : height;
        if (!std::isfinite(grid.header.offset[axis])
            || grid.minimum[axis] != Coordinate(grid, axis, 0)
            || grid.maximum[axis] != Coordinate(grid, axis, count - 1)
            || grid.minimum[axis] >= grid.maximum[axis]) {
            Fail("TERRAIN_PICK_GRID_INVALID", "地形 XY 范围与规则网格偏移不一致。");
        }
    }
}

Vector Normalize(const TerrainRay& ray) {
    double scale = 0;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        if (!std::isfinite(ray.origin[axis]) || std::abs(ray.origin[axis]) > MaximumDistance
            || !std::isfinite(ray.direction[axis])) {
            Fail("TERRAIN_PICK_RAY_INVALID", "拾取射线必须有限，起点各分量不能超过 1e9 世界单位。");
        }
        scale = std::max(scale, std::abs(ray.direction[axis]));
    }
    if (scale == 0) Fail("TERRAIN_PICK_DIRECTION_ZERO", "拾取射线方向长度必须大于 1e-12。");
    Vector direction{ray.direction[0] / scale, ray.direction[1] / scale, ray.direction[2] / scale};
    const double length = std::sqrt(Dot(direction, direction));
    if (scale <= 1e-12 / length) Fail("TERRAIN_PICK_DIRECTION_ZERO", "拾取射线方向长度必须大于 1e-12。");
    for (auto& component : direction) component /= length;
    return direction;
}

bool ClipBox(const MapData::TerrainGridSnapshot& grid, const TerrainRay& ray, const Vector& direction,
    double& begin, double& end) {
    for (std::size_t axis = 0; axis < 3; ++axis) {
        if (direction[axis] == 0) {
            if (ray.origin[axis] < grid.minimum[axis] || ray.origin[axis] > grid.maximum[axis]) return false;
            continue;
        }
        auto first = (grid.minimum[axis] - ray.origin[axis]) / direction[axis];
        auto last = (grid.maximum[axis] - ray.origin[axis]) / direction[axis];
        if (first > last) std::swap(first, last);
        begin = std::max(begin, first);
        end = std::min(end, last);
        if (begin > end) return false;
    }
    return true;
}

class Picker {
public:
    Picker(const MapData::TerrainGridSnapshot& grid, const TerrainRay& ray, Vector direction,
        const TerrainPickLimits& limits, double begin, double end)
        : grid_(grid), ray_(ray), direction_(direction), limits_(limits), begin_(begin), end_(end) {}

    TerrainPickResult Run() {
        auto time = begin_;
        const auto entry = Point(time);
        std::array<int, 2> step{Sign(direction_[0]), Sign(direction_[1])};
        std::array<int, 2> cell{Cell(0, entry[0], step[0]), Cell(1, entry[1], step[1])};
        VisitIncident(entry);
        while (true) {
            std::array<double, 2> crossing{NextCrossing(0, cell[0], step[0]), NextCrossing(1, cell[1], step[1])};
            double next = std::min({crossing[0], crossing[1], end_, nearestDistance_ + DistanceTolerance});
            if (next < time - DistanceTolerance) Fail("TERRAIN_PICK_GRID_INVALID", "地形射线格子遍历未能单调前进。");
            next = std::max(next, time);
            VisitIncident(Point(time + (next - time) * 0.5));
            VisitIncident(Point(next));
            // 端点的所有相邻格子已访问；格线/顶点上的同距离裁决不依赖遍历方向。
            if (nearestDistance_ <= next + DistanceTolerance) break;
            if (next >= end_) break;
            for (std::size_t axis = 0; axis < 2; ++axis) {
                if (crossing[axis] <= next + DistanceTolerance) cell[axis] += step[axis];
            }
            if (cell[0] < 0 || cell[1] < 0 || cell[0] >= static_cast<int>(grid_.header.width - 1)
                || cell[1] >= static_cast<int>(grid_.header.height - 1)) break;
            time = next;
        }
        // 以真实最小距离为锚点再裁决，避免连续 epsilon 比较产生依赖访问顺序的链式偏移。
        for (const auto& candidate : hits_) {
            if (candidate.distance > nearestDistance_ + DistanceTolerance) continue;
            if (!result_.hit || candidate.tileIndex < result_.hit->tileIndex
                || (candidate.tileIndex == result_.hit->tileIndex && candidate.triangle < result_.hit->triangle)) {
                result_.hit = candidate;
            }
        }
        return result_;
    }

private:
    static int Sign(double value) { return value > 0 ? 1 : value < 0 ? -1 : 0; }
    Vector Point(double distance) const {
        return {std::fma(direction_[0], distance, ray_.origin[0]), std::fma(direction_[1], distance, ray_.origin[1]),
            std::fma(direction_[2], distance, ray_.origin[2])};
    }

    int Cell(std::size_t axis, double coordinate, int step) const {
        const int count = static_cast<int>((axis == 0 ? grid_.header.width : grid_.header.height) - 1);
        const double estimate = std::floor((coordinate - grid_.header.offset[axis]) / MapData::TerrainGridSnapshot::tileSize);
        int cell = static_cast<int>(std::clamp(estimate, 0.0, static_cast<double>(count - 1)));
        while (cell > 0 && coordinate < Coordinate(grid_, axis, static_cast<std::uint32_t>(cell))) --cell;
        while (cell < count - 1 && coordinate >= Coordinate(grid_, axis, static_cast<std::uint32_t>(cell + 1))) ++cell;
        if (step < 0 && cell > 0
            && std::abs(coordinate - Coordinate(grid_, axis, static_cast<std::uint32_t>(cell))) <= DistanceTolerance) --cell;
        return cell;
    }

    double NextCrossing(std::size_t axis, int cell, int step) const {
        if (step == 0) return std::numeric_limits<double>::infinity();
        const auto boundary = static_cast<std::uint32_t>(cell + (step > 0 ? 1 : 0));
        return (Coordinate(grid_, axis, boundary) - ray_.origin[axis]) / direction_[axis];
    }

    void VisitIncident(const Vector& point) {
        std::array<std::array<int, 2>, 2> cells{};
        std::array<std::size_t, 2> counts{1, 1};
        for (std::size_t axis = 0; axis < 2; ++axis) {
            const int cell = Cell(axis, point[axis], 0);
            cells[axis][0] = cell;
            const int count = static_cast<int>((axis == 0 ? grid_.header.width : grid_.header.height) - 1);
            if (cell > 0 && std::abs(point[axis] - Coordinate(grid_, axis, static_cast<std::uint32_t>(cell))) <= DistanceTolerance) {
                cells[axis][counts[axis]++] = cell - 1;
            } else if (cell + 1 < count
                && std::abs(point[axis] - Coordinate(grid_, axis, static_cast<std::uint32_t>(cell + 1))) <= DistanceTolerance) {
                cells[axis][counts[axis]++] = cell + 1;
            }
        }
        for (std::size_t y = 0; y < counts[1]; ++y) {
            for (std::size_t x = 0; x < counts[0]; ++x) {
                Visit(static_cast<std::uint32_t>(cells[0][x]), static_cast<std::uint32_t>(cells[1][y]));
            }
        }
    }

    void Visit(std::uint32_t x, std::uint32_t y) {
        const auto index = static_cast<std::size_t>(y) * (grid_.header.width - 1) + x;
        if (visited_.contains(index)) return;
        if (visited_.size() >= limits_.maxVisitedTiles) Fail("TERRAIN_PICK_BUDGET", "地形射线拾取超过格子访问预算；不能确认最近命中或未命中。");
        visited_.insert(index);
        ++result_.visitedTiles;
        const auto sw = y * grid_.header.width + x;
        const std::array<std::uint32_t, 4> expected{sw, sw + 1, sw + grid_.header.width, sw + grid_.header.width + 1};
        if (grid_.tiles[index].vertices != expected) Fail("TERRAIN_PICK_GRID_INVALID", "访问到的地形格子不是原始 SW/SE/NW/NE 索引。");
        std::array<Vector, 4> points{};
        for (std::size_t i = 0; i < 4; ++i) {
            const auto& position = grid_.vertices[expected[i]].position;
            for (std::size_t axis = 0; axis < 3; ++axis) {
                if (!std::isfinite(position[axis])) Fail("TERRAIN_PICK_GRID_INVALID", "访问到非有限地形顶点。");
                points[i][axis] = position[axis];
            }
        }
        // 先辨识退化几何，再验证规则 XY，避免将退化面静默当作未命中。
        for (const auto triangle : {std::array<std::size_t, 3>{0, 1, 2}, std::array<std::size_t, 3>{1, 3, 2}}) {
            const auto normal = Cross(Difference(points[triangle[1]], points[triangle[0]]), Difference(points[triangle[2]], points[triangle[0]]));
            if (Dot(normal, normal) == 0) Fail("TERRAIN_PICK_DEGENERATE", "访问到退化地形三角形。");
        }
        for (std::size_t i = 0; i < 4; ++i) {
            if (points[i][0] != Coordinate(grid_, 0, x + static_cast<std::uint32_t>(i % 2))
                || points[i][1] != Coordinate(grid_, 1, y + static_cast<std::uint32_t>(i / 2))
                || points[i][2] < grid_.minimum[2] || points[i][2] > grid_.maximum[2]) {
                Fail("TERRAIN_PICK_GRID_INVALID", "访问到的地形顶点与规则网格或高度范围不一致。");
            }
        }
        Intersect(index, x, y, 0, points, {0, 1, 2});
        Intersect(index, x, y, 1, points, {1, 3, 2});
    }

    void Intersect(std::size_t index, std::uint32_t x, std::uint32_t y, std::uint32_t triangle,
        const std::array<Vector, 4>& points, const std::array<std::size_t, 3>& vertices) {
        const double width = points[1][0] - points[0][0], height = points[2][1] - points[0][1];
        const double u = (ray_.origin[0] - points[0][0]) / width, v = (ray_.origin[1] - points[0][1]) / height;
        const double du = direction_[0] / width, dv = direction_[1] / height;
        const Vector baryOrigin = triangle == 0 ? Vector{1 - u - v, u, v} : Vector{1 - v, u + v - 1, 1 - u};
        const Vector baryDirection = triangle == 0 ? Vector{-du - dv, du, dv} : Vector{-dv, du + dv, -du};
        double first = begin_, last = end_;
        for (std::size_t i = 0; i < 3; ++i) {
            if (baryDirection[i] == 0) {
                if (baryOrigin[i] < -ProjectedClipTolerance) return;
            } else {
                const double boundary = (-ProjectedClipTolerance - baryOrigin[i]) / baryDirection[i];
                if (baryDirection[i] > 0) first = std::max(first, boundary);
                else last = std::min(last, boundary);
            }
        }
        if (first > last) return;
        const auto normal = Cross(Difference(points[vertices[1]], points[vertices[0]]), Difference(points[vertices[2]], points[vertices[0]]));
        const double denominator = Dot(normal, direction_);
        const double numerator = Dot(normal, Difference(points[vertices[0]], ray_.origin));
        // 微小但非零的法向分量仍可在距离预算内相交，不能用固定 epsilon 将其当成平行。
        if (denominator == 0) {
            if (std::abs(numerator) <= std::sqrt(Dot(normal, normal)) * DistanceTolerance) {
                Fail("TERRAIN_PICK_COPLANAR", "拾取射线与地形面共面相交，无法给出唯一交点。");
            }
            return;
        }
        const double distance = numerator / denominator;
        if (!std::isfinite(distance) || distance < 0 || distance > limits_.maxDistance
            || distance < first - DistanceTolerance || distance > last + DistanceTolerance) return;
        const auto point = Point(distance);
        const double hitU = (point[0] - points[0][0]) / width, hitV = (point[1] - points[0][1]) / height;
        Vector bary = triangle == 0 ? Vector{1 - hitU - hitV, hitU, hitV} : Vector{1 - hitV, hitU + hitV - 1, 1 - hitU};
        double sum = 0;
        for (std::size_t i = 0; i < 3; ++i) {
            if (bary[i] < -BarycentricTolerance || bary[i] > 1 + BarycentricTolerance) return;
            bary[i] = std::clamp(bary[i], 0.0, 1.0);
            sum += bary[i];
        }
        for (auto& component : bary) component /= sum;
        nearestDistance_ = std::min(nearestDistance_, distance);
        hits_.push_back(TerrainPickHit{index, x, y, triangle, point, distance, bary});
    }

    const MapData::TerrainGridSnapshot& grid_;
    const TerrainRay& ray_;
    Vector direction_;
    const TerrainPickLimits& limits_;
    double begin_, end_;
    double nearestDistance_ = std::numeric_limits<double>::infinity();
    TerrainPickResult result_;
    std::vector<TerrainPickHit> hits_; // 至多为访问预算的两倍，不按全图分配。
    std::unordered_set<std::size_t> visited_;
};

} // namespace

Result<TerrainPickResult> PickTerrainHeightPreview(const MapData::TerrainGridSnapshot& terrain,
    const TerrainRay& ray, const TerrainPickLimits& limits) {
    try {
        if (!std::isfinite(limits.maxDistance) || limits.maxDistance < 0 || limits.maxDistance > MaximumDistance
            || limits.maxVisitedTiles > MaximumVisitedTiles) {
            Fail("TERRAIN_PICK_LIMITS", "地形拾取预算只能收紧默认上限，且距离必须有限、非负。");
        }
        ValidateGrid(terrain);
        const auto direction = Normalize(ray);
        double begin = 0, end = limits.maxDistance;
        if (!ClipBox(terrain, ray, direction, begin, end)) return Result<TerrainPickResult>::Success({});
        return Result<TerrainPickResult>::Success(Picker(terrain, ray, direction, limits, begin, end).Run());
    } catch (const PickFailure& failure) {
        return Result<TerrainPickResult>::Failure("TerrainPicking", failure.code, failure.message);
    } catch (const std::bad_alloc&) {
        return Result<TerrainPickResult>::Failure("TerrainPicking", "TERRAIN_PICK_ALLOCATION_FAILED", "无法分配有界地形拾取访问记录。");
    }
}

std::optional<float> SampleTerrainHeight(const MapData::TerrainGridSnapshot& terrain, double x, double y) {
    return MapData::SampleGroundHeight(terrain, x, y);
}

} // namespace War3Map::Render
