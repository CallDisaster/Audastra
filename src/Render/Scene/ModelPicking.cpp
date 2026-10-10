#include "War3MapCore/Render/ModelPicking.h"

#include <algorithm>
#include <cmath>

namespace War3Map::Render {
namespace {

struct LocalBox {
    std::array<double, 3> minimum{}, maximum{};
};

// 局部盒按各轴世界缩放放大到最小半径；缩放为零（退化）时返回空。
std::optional<LocalBox> InstanceLocalBox(const WorldModelScene& scene, std::uint32_t instance, float minimumHalfExtent) {
    if (instance >= scene.instances.size() || instance >= scene.instanceRequests.size()
        || scene.instanceRequests[instance] == kBakedInstance) return std::nullopt;
    const auto& entry = scene.instances[instance];
    if (entry.model >= scene.modelBounds.size()) return std::nullopt;
    const auto& bounds = scene.modelBounds[entry.model];
    const auto* t = entry.transform;
    LocalBox box;
    for (int axis = 0; axis < 3; ++axis) {
        // 第 axis 列的长度即该局部轴的世界缩放。
        const double scale = std::sqrt(double(t[axis]) * t[axis] + double(t[4 + axis]) * t[4 + axis] + double(t[8 + axis]) * t[8 + axis]);
        if (!(scale > 1e-6)) return std::nullopt;
        double low = bounds[axis], high = bounds[axis + 3];
        const double minimumHalf = minimumHalfExtent / scale;
        if ((high - low) * 0.5 < minimumHalf) {
            const double center = (low + high) * 0.5;
            low = center - minimumHalf; high = center + minimumHalf;
        }
        box.minimum[axis] = low; box.maximum[axis] = high;
    }
    return box;
}

// 3×3 线性部分求逆（行主序）；奇异时返回空。
std::optional<std::array<double, 9>> Inverse(const float* t) {
    const double a = t[0], b = t[1], c = t[2], d = t[4], e = t[5], f = t[6], g = t[8], h = t[9], i = t[10];
    const double det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (!(std::abs(det) > 1e-12) || !std::isfinite(det)) return std::nullopt;
    const double r = 1.0 / det;
    return std::array<double, 9>{(e * i - f * h) * r, (c * h - b * i) * r, (b * f - c * e) * r,
        (f * g - d * i) * r, (a * i - c * g) * r, (c * d - a * f) * r,
        (d * h - e * g) * r, (b * g - a * h) * r, (a * e - b * d) * r};
}

} // namespace

std::vector<ModelPickHit> PickModelInstances(const WorldModelScene& scene, const TerrainRay& ray,
    double maxDistance, float minimumHalfExtent, std::size_t maxHits) {
    std::vector<ModelPickHit> hits;
    const double length = std::sqrt(ray.direction[0] * ray.direction[0] + ray.direction[1] * ray.direction[1]
        + ray.direction[2] * ray.direction[2]);
    if (!(length > 1e-12) || !std::isfinite(length) || !(maxDistance > 0) || maxHits == 0) return hits;
    const std::array<double, 3> direction{ray.direction[0] / length, ray.direction[1] / length, ray.direction[2] / length};
    for (std::uint32_t index = 0; index < scene.instances.size(); ++index) {
        const auto box = InstanceLocalBox(scene, index, minimumHalfExtent);
        if (!box) continue;
        const auto* t = scene.instances[index].transform;
        const auto inverse = Inverse(t);
        if (!inverse) continue;
        // 仿射变换保持射线参数：局部射线 = M⁻¹(o − p) + s·M⁻¹d，s 仍是世界距离。
        const double offset[3]{ray.origin[0] - t[3], ray.origin[1] - t[7], ray.origin[2] - t[11]};
        double origin[3], step[3];
        for (int row = 0; row < 3; ++row) {
            const auto* m = &(*inverse)[row * 3];
            origin[row] = m[0] * offset[0] + m[1] * offset[1] + m[2] * offset[2];
            step[row] = m[0] * direction[0] + m[1] * direction[1] + m[2] * direction[2];
        }
        double enter = 0, exit = maxDistance;
        bool missed = false;
        for (int axis = 0; axis < 3 && !missed; ++axis) {
            if (std::abs(step[axis]) < 1e-15) {
                missed = origin[axis] < box->minimum[axis] || origin[axis] > box->maximum[axis];
                continue;
            }
            double lo = (box->minimum[axis] - origin[axis]) / step[axis];
            double hi = (box->maximum[axis] - origin[axis]) / step[axis];
            if (lo > hi) std::swap(lo, hi);
            enter = std::max(enter, lo); exit = std::min(exit, hi);
            missed = enter > exit;
        }
        if (!missed) hits.push_back({index, enter});
    }
    std::stable_sort(hits.begin(), hits.end(), [](const auto& left, const auto& right) { return left.distance < right.distance; });
    if (hits.size() > maxHits) hits.resize(maxHits);
    return hits;
}

std::optional<std::array<std::array<float, 3>, 8>> ModelInstanceBox(const WorldModelScene& scene,
    std::uint32_t instance, float minimumHalfExtent) {
    const auto box = InstanceLocalBox(scene, instance, minimumHalfExtent);
    if (!box) return std::nullopt;
    const auto* t = scene.instances[instance].transform;
    std::array<std::array<float, 3>, 8> corners{};
    for (int corner = 0; corner < 8; ++corner) {
        const int xi = (corner & 3) == 1 || (corner & 3) == 2, yi = (corner & 3) >= 2, zi = corner >= 4;
        const double local[3]{xi ? box->maximum[0] : box->minimum[0], yi ? box->maximum[1] : box->minimum[1],
            zi ? box->maximum[2] : box->minimum[2]};
        for (int row = 0; row < 3; ++row)
            corners[corner][row] = static_cast<float>(t[row * 4] * local[0] + t[row * 4 + 1] * local[1]
                + t[row * 4 + 2] * local[2] + t[row * 4 + 3]);
    }
    return corners;
}

} // namespace War3Map::Render
