#include "War3MapCore/Render/EditorOverlay.h"

#include "War3MapCore/Render/TerrainPicking.h"

#include <algorithm>
#include <cmath>

namespace War3Map::Render {
namespace {

constexpr float kMaximumCoordinate = 1.0e6f;
constexpr double kDegrees = 3.14159265358979323846 / 180.0;

bool Valid(float value) { return std::isfinite(value) && std::abs(value) <= kMaximumCoordinate; }

void Line(OverlayLines& lines, const std::array<float, 3>& from, const std::array<float, 3>& to, const OverlayColor& color) {
    if (!std::all_of(from.begin(), from.end(), Valid) || !std::all_of(to.begin(), to.end(), Valid)) return;
    lines.push_back({{from[0], from[1], from[2]}, {to[0], to[1], to[2]}, {color[0], color[1], color[2]}});
}

float Ground(const MapData::TerrainGridSnapshot* terrain, double x, double y) {
    if (!terrain) return 0;
    return SampleTerrainHeight(*terrain, x, y).value_or(0.0f);
}

} // namespace

void AppendBoxLines(OverlayLines& lines, const std::array<std::array<float, 3>, 8>& corners, const OverlayColor& color) {
    for (int i = 0; i < 4; ++i) {
        Line(lines, corners[i], corners[(i + 1) % 4], color);
        Line(lines, corners[4 + i], corners[4 + (i + 1) % 4], color);
        Line(lines, corners[i], corners[4 + i], color);
    }
}

void AppendGroundRectangle(OverlayLines& lines, const MapData::TerrainGridSnapshot* terrain,
    float left, float bottom, float right, float top, const OverlayColor& color, float lift) {
    if (!Valid(left) || !Valid(bottom) || !Valid(right) || !Valid(top) || !(left < right) || !(bottom < top)) return;
    const std::array<std::array<float, 2>, 5> corners{{{left, bottom}, {right, bottom}, {right, top}, {left, top}, {left, bottom}}};
    for (std::size_t edge = 0; edge < 4; ++edge) {
        const auto& a = corners[edge];
        const auto& b = corners[edge + 1];
        const double length = std::hypot(b[0] - a[0], b[1] - a[1]);
        const int segments = std::clamp(static_cast<int>(std::ceil(length / 64.0)), 1, 256);
        std::array<float, 3> previous{a[0], a[1], Ground(terrain, a[0], a[1]) + lift};
        for (int step = 1; step <= segments; ++step) {
            const double t = static_cast<double>(step) / segments;
            const double x = a[0] + (b[0] - a[0]) * t, y = a[1] + (b[1] - a[1]) * t;
            const std::array<float, 3> point{static_cast<float>(x), static_cast<float>(y), Ground(terrain, x, y) + lift};
            Line(lines, previous, point, color);
            previous = point;
        }
    }
}

void AppendGroundCircle(OverlayLines& lines, const MapData::TerrainGridSnapshot* terrain,
    double centerX, double centerY, double radius, const OverlayColor& color, float lift) {
    if (!std::isfinite(centerX) || !std::isfinite(centerY) || !std::isfinite(radius) || !(radius > 0)
        || std::abs(centerX) + radius > kMaximumCoordinate || std::abs(centerY) + radius > kMaximumCoordinate) return;
    const double circumference = 2.0 * 3.14159265358979323846 * radius;
    const int segments = std::clamp(static_cast<int>(std::ceil(circumference / 32.0)), 24, 512);
    const auto at = [&](int step) {
        const double angle = 2.0 * 3.14159265358979323846 * step / segments;
        const double x = centerX + std::cos(angle) * radius, y = centerY + std::sin(angle) * radius;
        return std::array<float, 3>{static_cast<float>(x), static_cast<float>(y), Ground(terrain, x, y) + lift};
    };
    auto previous = at(0);
    for (int step = 1; step <= segments; ++step) {
        const auto point = at(step);
        Line(lines, previous, point, color);
        previous = point;
    }
}

void AppendCameraMarker(OverlayLines& lines, const MapData::TerrainGridSnapshot* terrain,
    const MapData::CameraRecord& camera, const OverlayColor& color) {
    if (!Valid(camera.targetX) || !Valid(camera.targetY) || !Valid(camera.zOffset) || !std::isfinite(camera.rotation)
        || !std::isfinite(camera.angleOfAttack) || !Valid(camera.targetDistance) || !std::isfinite(camera.fieldOfView)) return;
    const std::array<float, 3> target{camera.targetX, camera.targetY,
        Ground(terrain, camera.targetX, camera.targetY) + camera.zOffset};
    constexpr float cross = 48.0f;
    Line(lines, {target[0] - cross, target[1], target[2]}, {target[0] + cross, target[1], target[2]}, color);
    Line(lines, {target[0], target[1] - cross, target[2]}, {target[0], target[1] + cross, target[2]}, color);
    // 视线方向：rotation 为水平朝向，angleOfAttack 为俯仰（304° ≈ −56°，向下看）。
    const double yaw = camera.rotation * kDegrees, pitch = camera.angleOfAttack * kDegrees;
    const std::array<double, 3> forward{std::cos(yaw) * std::cos(pitch), std::sin(yaw) * std::cos(pitch), std::sin(pitch)};
    const double distance = std::clamp(static_cast<double>(camera.targetDistance), 0.0, 100000.0);
    const std::array<float, 3> eye{static_cast<float>(target[0] - forward[0] * distance),
        static_cast<float>(target[1] - forward[1] * distance), static_cast<float>(target[2] - forward[2] * distance)};
    Line(lines, eye, target, color);
    // 视锥：right = forward × up，up' = right × forward。
    const std::array<double, 3> right{forward[1], -forward[0], 0};
    const double rightLength = std::hypot(right[0], right[1]);
    if (rightLength < 1e-6) return; // 垂直向下/向上时不画视锥
    const std::array<double, 3> r{right[0] / rightLength, right[1] / rightLength, 0};
    const std::array<double, 3> u{r[1] * forward[2] - r[2] * forward[1], r[2] * forward[0] - r[0] * forward[2],
        r[0] * forward[1] - r[1] * forward[0]};
    const double length = std::max(64.0, distance * 0.15);
    const double fov = std::clamp(static_cast<double>(camera.fieldOfView), 1.0, 170.0) * kDegrees;
    const double halfHeight = std::tan(fov * 0.5) * length, halfWidth = halfHeight * 4.0 / 3.0;
    std::array<std::array<float, 3>, 4> rim{};
    const double signs[4][2]{{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
    for (int i = 0; i < 4; ++i)
        for (int axis = 0; axis < 3; ++axis)
            rim[i][axis] = static_cast<float>(eye[axis] + forward[axis] * length + r[axis] * halfWidth * signs[i][0]
                + u[axis] * halfHeight * signs[i][1]);
    for (int i = 0; i < 4; ++i) {
        Line(lines, eye, rim[i], color);
        Line(lines, rim[i], rim[(i + 1) % 4], color);
    }
}

OverlayColor RegionOverlayColor(const MapData::RegionRecord& region) {
    return {region.colorBgra[2] / 255.0f, region.colorBgra[1] / 255.0f, region.colorBgra[0] / 255.0f};
}

} // namespace War3Map::Render
