#include "BuildingSplatProjection.h"

#include <algorithm>
#include <cmath>

namespace War3Map::Render::Detail {
namespace {
using Point = std::array<double, 3>;
struct Polygon { std::array<Point, 8> points{}; std::size_t count = 0; };

Polygon Clip(const Polygon& input, std::size_t axis, double boundary, bool minimum) {
    Polygon output;
    if (!input.count) return output;
    auto last = input.points[input.count - 1];
    auto inside = [=](const Point& point) { return minimum ? point[axis] >= boundary : point[axis] <= boundary; };
    bool lastInside = inside(last);
    for (std::size_t i = 0; i < input.count; ++i) {
        const auto& current = input.points[i]; const bool currentInside = inside(current);
        if (lastInside != currentInside) {
            const auto t = (boundary - last[axis]) / (current[axis] - last[axis]);
            Point intersection;
            for (std::size_t a = 0; a < 3; ++a) intersection[a] = last[a] + t * (current[a] - last[a]);
            intersection[axis] = boundary;
            output.points[output.count++] = intersection;
        }
        if (currentInside) output.points[output.count++] = current;
        last = current; lastInside = currentInside;
    }
    return output;
}

Point Interpolate(const Point& a, const Point& b, const Point& c, double u, double v) {
    Point point;
    for (std::size_t axis = 0; axis < 3; ++axis)
        point[axis] = a[axis] + u * (b[axis] - a[axis]) + v * (c[axis] - a[axis]);
    return point;
}
Point Position(const MapData::TerrainVertex& vertex) {
    return {vertex.position[0], vertex.position[1], vertex.position[2]};
}
} // namespace

Result<SplatProjection> ProjectBuildingSplat(const MapData::TerrainGridSnapshot& terrain,
    const Services::BuildingSplatFootprint& footprint, float maximumCellSize,
    std::size_t maximumVertices, std::size_t maximumIndices) {
    using Output = Result<SplatProjection>;
    maximumVertices = std::min<std::size_t>(maximumVertices, 65536);
    SplatProjection output;
    const auto subdivisions = static_cast<std::uint32_t>(std::ceil(128.0 / maximumCellSize));
    const auto coordinate = [&](float value, std::size_t axis) {
        return (static_cast<double>(value) - terrain.header.offset[axis]) / 128.0;
    };
    const auto left = static_cast<std::uint32_t>(std::floor(coordinate(footprint.minimum[0], 0)));
    const auto bottom = static_cast<std::uint32_t>(std::floor(coordinate(footprint.minimum[1], 1)));
    const auto right = std::min(terrain.header.width - 2,
        static_cast<std::uint32_t>(std::ceil(coordinate(footprint.maximum[0], 0)) - 1));
    const auto top = std::min(terrain.header.height - 2,
        static_cast<std::uint32_t>(std::ceil(coordinate(footprint.maximum[1], 1)) - 1));
    // 大表尺寸或过紧的采样间距不能制造无界CPU扫描，哪怕最终被裁切掉。
    const auto tileCount = static_cast<std::uint64_t>(right - left + 1) * (top - bottom + 1);
    if (tileCount * subdivisions * subdivisions * 2 > 131072)
        return Output::Failure("BuildingSplat.Preview", "BUILDING_SPLAT_GRID_BUDGET", "地形投影的有界三角扫描预算不足，省略此实例。");
    const auto append = [&](const Point& a, const Point& b, const Point& c) {
        Polygon polygon{{a,b,c},3};
        for (std::size_t axis = 0; axis < 2; ++axis) {
            polygon = Clip(polygon, axis, footprint.minimum[axis], true);
            polygon = Clip(polygon, axis, footprint.maximum[axis], false);
        }
        if (polygon.count < 3) return true;
        // 删除裁切产生的相邻重复点，零面积边界不占几何预算。
        Polygon clean;
        for (std::size_t i = 0; i < polygon.count; ++i) {
            const auto& point = polygon.points[i];
            if (clean.count && point[0] == clean.points[clean.count-1][0] && point[1] == clean.points[clean.count-1][1]) continue;
            clean.points[clean.count++] = point;
        }
        if (clean.count > 1 && clean.points[0][0] == clean.points[clean.count-1][0]
            && clean.points[0][1] == clean.points[clean.count-1][1]) --clean.count;
        if (clean.count < 3) return true;
        double area = 0;
        for (std::size_t i = 1; i + 1 < clean.count; ++i)
            area += (clean.points[i][0] - clean.points[0][0]) * (clean.points[i+1][1] - clean.points[0][1])
                - (clean.points[i][1] - clean.points[0][1]) * (clean.points[i+1][0] - clean.points[0][0]);
        if (area <= 1e-12) return true;
        const auto indices = (clean.count - 2) * 3;
        if (clean.count > maximumVertices - output.vertices.size() || indices > maximumIndices - output.indices.size()) return false;
        const Point ab{b[0]-a[0],b[1]-a[1],b[2]-a[2]}, ac{c[0]-a[0],c[1]-a[1],c[2]-a[2]};
        Point normal{ab[1]*ac[2]-ab[2]*ac[1],ab[2]*ac[0]-ab[0]*ac[2],ab[0]*ac[1]-ab[1]*ac[0]};
        const auto length = std::hypot(normal[0],normal[1],normal[2]);
        for (auto& value : normal) value /= length;
        const auto first = static_cast<std::uint16_t>(output.vertices.size());
        for (std::size_t i = 0; i < clean.count; ++i) {
            const auto& p = clean.points[i];
            output.vertices.push_back({static_cast<float>(p[0]),static_cast<float>(p[1]),static_cast<float>(p[2]),
                static_cast<float>(normal[0]),static_cast<float>(normal[1]),static_cast<float>(normal[2]),
                static_cast<float>((p[0]-footprint.minimum[0])/footprint.fullWidth),
                static_cast<float>(1-(p[1]-footprint.minimum[1])/footprint.fullHeight)});
        }
        for (std::size_t i = 1; i + 1 < clean.count; ++i)
            for (const auto index : {std::size_t{0}, i, i+1}) output.indices.push_back(static_cast<std::uint16_t>(first+index));
        return true;
    };
    for (auto y = bottom; y <= top; ++y) for (auto x = left; x <= right; ++x) {
        const auto& corners = terrain.tiles[static_cast<std::size_t>(y) * (terrain.header.width-1) + x].vertices;
        for (const auto triangle : {std::array<std::size_t,3>{0,1,2},std::array<std::size_t,3>{1,3,2}}) {
            const auto a=Position(terrain.vertices[corners[triangle[0]]]);
            const auto b=Position(terrain.vertices[corners[triangle[1]]]);
            const auto c=Position(terrain.vertices[corners[triangle[2]]]);
            const double step = 1.0/subdivisions;
            for (std::uint32_t i=0;i<subdivisions;++i) for (std::uint32_t j=0;j<subdivisions-i;++j) {
                const auto p=Interpolate(a,b,c,i*step,j*step);
                const auto q=Interpolate(a,b,c,(i+1)*step,j*step);
                const auto r=Interpolate(a,b,c,i*step,(j+1)*step);
                if (!append(p,q,r) || (i+j+1<subdivisions && !append(q,
                    Interpolate(a,b,c,(i+1)*step,(j+1)*step),r)))
                    return Output::Failure("BuildingSplat.Preview", "BUILDING_SPLAT_FRAME_BUDGET", "实际投影几何超出16位索引或owned字节预算，省略此实例。");
            }
        }
    }
    return Output::Success(std::move(output));
}
} // namespace War3Map::Render::Detail
