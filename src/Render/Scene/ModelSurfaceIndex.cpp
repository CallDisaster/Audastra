#include "War3MapCore/Render/ModelSurfaceIndex.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>

namespace War3Map::Render {
namespace {
constexpr std::uint32_t kLeafSize = 4;
using Point = std::array<double, 3>;

std::array<float, 6> EmptyBounds() {
    constexpr float inf = std::numeric_limits<float>::infinity();
    return {inf, inf, inf, -inf, -inf, -inf};
}
void Grow(std::array<float, 6>& bounds, const float* point) {
    for (int axis = 0; axis < 3; ++axis) {
        bounds[axis] = std::min(bounds[axis], point[axis]);
        bounds[axis + 3] = std::max(bounds[axis + 3], point[axis]);
    }
}
Point Cross(const Point& a, const Point& b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
double Dot(const Point& a, const Point& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// 射线与包围盒的进入/离开参数；不相交时返回 false。
bool HitBox(const std::array<float, 6>& box, const Point& origin, const Point& inverse, double limit, double& entry) {
    double enter = 0, leave = limit;
    for (int axis = 0; axis < 3; ++axis) {
        double t0 = (box[axis] - origin[axis]) * inverse[axis], t1 = (box[axis + 3] - origin[axis]) * inverse[axis];
        if (std::isnan(t0) || std::isnan(t1)) { // 射线平行于该轴且起点正在面上：按落在盒内处理
            if (origin[axis] < box[axis] || origin[axis] > box[axis + 3]) return false;
            continue;
        }
        if (t0 > t1) std::swap(t0, t1);
        enter = std::max(enter, t0); leave = std::min(leave, t1);
        if (enter > leave) return false;
    }
    entry = enter;
    return true;
}
} // namespace

Result<ModelSurfaceIndex> ModelSurfaceIndex::Build(std::span<const ModelSurfaceSource> sources, const ModelSurfaceLimits& limits) {
    ModelSurfaceIndex index;
    index.bounds_ = EmptyBounds();
    std::size_t skipped = 0;
    std::vector<Diagnostic> warnings;
    for (const auto& source : sources) {
        if (!source.model) continue;
        std::uint64_t triangles = 0;
        for (const auto& geoset : source.model->geosets)
            if (geoset.visibleInStand) triangles += geoset.indices.size() / 3;
        if (triangles == 0) continue;
        if (index.triangles_.size() + triangles > limits.maxTriangles) { ++skipped; continue; }
        const auto& m = source.transform;
        if (!std::all_of(m.begin(), m.end(), [](float value) { return std::isfinite(value); })) { ++skipped; continue; }
        for (const auto& geoset : source.model->geosets) {
            if (!geoset.visibleInStand) continue;
            const auto vertexCount = geoset.positions.size();
            for (std::size_t i = 0; i + 2 < geoset.indices.size(); i += 3) {
                Triangle triangle; triangle.source = source.source;
                bool valid = true;
                for (int corner = 0; corner < 3 && valid; ++corner) {
                    const auto vertex = geoset.indices[i + corner];
                    if (vertex >= vertexCount) { valid = false; break; }
                    const auto& p = geoset.positions[vertex];
                    for (int row = 0; row < 3; ++row) {
                        const float value = m[row * 4] * p[0] + m[row * 4 + 1] * p[1] + m[row * 4 + 2] * p[2] + m[row * 4 + 3];
                        if (!std::isfinite(value)) { valid = false; break; }
                        triangle.points[corner * 3 + row] = value;
                    }
                }
                if (!valid) continue;
                for (int corner = 0; corner < 3; ++corner) Grow(index.bounds_, &triangle.points[corner * 3]);
                index.triangles_.push_back(triangle);
            }
        }
        ++index.sourceCount_;
    }
    if (skipped)
        warnings.push_back({DiagnosticSeverity::Warning, "Render.Surface", "MODEL_SURFACE_SKIPPED",
            std::to_string(skipped) + " 个摆放超出表面三角形上限或变换无效，没有进入落点索引。", std::nullopt});
    if (!index.triangles_.empty()) index.BuildNodes();
    auto result = Result<ModelSurfaceIndex>::Success(std::move(index));
    result.diagnostics = std::move(warnings);
    return result;
}

void ModelSurfaceIndex::BuildNodes() {
    // 按质心在最长轴上取中位数二分；叶子最多 kLeafSize 个三角形。
    std::vector<std::array<float, 3>> centroids(triangles_.size());
    for (std::size_t i = 0; i < triangles_.size(); ++i) {
        const auto& p = triangles_[i].points;
        for (int axis = 0; axis < 3; ++axis) centroids[i][axis] = (p[axis] + p[3 + axis] + p[6 + axis]) / 3.0f;
    }
    std::vector<std::uint32_t> order(triangles_.size());
    for (std::uint32_t i = 0; i < order.size(); ++i) order[i] = i;
    nodes_.clear();
    nodes_.reserve(triangles_.size() / kLeafSize * 2 + 1);
    struct Pending { std::uint32_t node, begin, end; };
    std::vector<Pending> stack;
    nodes_.push_back({});
    stack.push_back({0, 0, static_cast<std::uint32_t>(order.size())});
    while (!stack.empty()) {
        const auto [node, begin, end] = stack.back();
        stack.pop_back();
        auto bounds = EmptyBounds(), centroidBounds = EmptyBounds();
        for (auto i = begin; i < end; ++i) {
            const auto& p = triangles_[order[i]].points;
            for (int corner = 0; corner < 3; ++corner) Grow(bounds, &p[corner * 3]);
            Grow(centroidBounds, centroids[order[i]].data());
        }
        nodes_[node].bounds = bounds;
        if (end - begin <= kLeafSize) { nodes_[node].first = begin; nodes_[node].count = end - begin; continue; }
        int axis = 0;
        for (int candidate = 1; candidate < 3; ++candidate)
            if (centroidBounds[candidate + 3] - centroidBounds[candidate] > centroidBounds[axis + 3] - centroidBounds[axis]) axis = candidate;
        const auto middle = begin + (end - begin) / 2;
        std::nth_element(order.begin() + begin, order.begin() + middle, order.begin() + end,
            [&](std::uint32_t a, std::uint32_t b) { return centroids[a][axis] < centroids[b][axis]; });
        const auto left = static_cast<std::uint32_t>(nodes_.size());
        nodes_.push_back({}); nodes_.push_back({});
        nodes_[node].first = left; nodes_[node].count = 0;
        stack.push_back({left + 1, middle, end});
        stack.push_back({left, begin, middle});
    }
    std::vector<Triangle> sorted(triangles_.size());
    for (std::size_t i = 0; i < order.size(); ++i) sorted[i] = triangles_[order[i]];
    triangles_ = std::move(sorted);
}

std::optional<ModelSurfaceHit> ModelSurfaceIndex::Raycast(const Point& origin, const Point& direction, double maxDistance) const {
    if (nodes_.empty() || !(maxDistance > 0) || Dot(direction, direction) <= 0) return std::nullopt;
    const Point inverse{1.0 / direction[0], 1.0 / direction[1], 1.0 / direction[2]};
    double best = maxDistance;
    std::optional<ModelSurfaceHit> hit;
    std::uint32_t stack[64];
    int top = 0;
    stack[top++] = 0;
    while (top) {
        const auto& node = nodes_[stack[--top]];
        double entry = 0;
        if (!HitBox(node.bounds, origin, inverse, best, entry)) continue;
        if (node.count == 0) {
            if (top + 2 > 64) return std::nullopt; // 中位数二分深度不超过 log2(n)+1，不会到这里
            stack[top++] = node.first + 1;
            stack[top++] = node.first;
            continue;
        }
        for (auto i = node.first; i < node.first + node.count; ++i) {
            const auto& p = triangles_[i].points;
            const Point a{p[0], p[1], p[2]}, e1{p[3] - a[0], p[4] - a[1], p[5] - a[2]}, e2{p[6] - a[0], p[7] - a[1], p[8] - a[2]};
            // Möller–Trumbore，双面。
            const auto h = Cross(direction, e2);
            const double det = Dot(e1, h);
            if (std::abs(det) < 1e-12) continue;
            const double f = 1.0 / det;
            const Point s{origin[0] - a[0], origin[1] - a[1], origin[2] - a[2]};
            const double u = f * Dot(s, h);
            if (u < 0 || u > 1) continue;
            const auto q = Cross(s, e1);
            const double v = f * Dot(direction, q);
            if (v < 0 || u + v > 1) continue;
            const double t = f * Dot(e2, q);
            if (!(t >= 0) || t >= best) continue;
            auto normal = Cross(e1, e2);
            const double length = std::sqrt(Dot(normal, normal));
            if (!(length > 0)) continue;
            for (auto& value : normal) value /= length;
            if (Dot(normal, direction) > 0) for (auto& value : normal) value = -value;
            best = t;
            hit = ModelSurfaceHit{{origin[0] + direction[0] * t, origin[1] + direction[1] * t, origin[2] + direction[2] * t},
                normal, triangles_[i].source, t};
        }
    }
    return hit;
}

std::optional<ModelSurfaceHit> ModelSurfaceIndex::Drop(double x, double y) const {
    if (nodes_.empty()) return std::nullopt;
    const double top = static_cast<double>(bounds_[5]) + 1.0;
    return Raycast({x, y, top}, {0, 0, -1}, top - static_cast<double>(bounds_[2]) + 2.0);
}

} // namespace War3Map::Render
