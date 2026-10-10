#include "War3MapCore/Render/ViewRay.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace War3Map::Render {
namespace {

using Matrix = std::array<std::array<double, 4>, 4>;
using Vector4 = std::array<double, 4>;
using Vector3 = std::array<double, 3>;
constexpr double MaximumCoordinate = 1e9;
constexpr double MaximumDistance = 1e9;
constexpr double MinimumDistance = 1e-12;
constexpr double PivotTolerance = 64 * std::numeric_limits<double>::epsilon();
constexpr double ProjectionTolerance = 1e-6;

struct ViewRayFailure { const char* code; const char* message; };
[[noreturn]] void Fail(const char* code, const char* message) { throw ViewRayFailure{code, message}; }

void ValidateViewport(const ViewProjectionFrame& frame) {
    if (frame.physicalWidth == 0 || frame.physicalHeight == 0 || frame.physicalWidth > 4096 || frame.physicalHeight > 4096) {
        Fail("VIEW_RAY_VIEWPORT_INVALID", "拾取帧物理尺寸必须在 1..4096 范围内。");
    }
}

void ValidatePoint(const ViewProjectionFrame& frame, PhysicalViewPoint point) {
    if (!std::isfinite(point.x) || !std::isfinite(point.y)) Fail("VIEW_RAY_POINT_INVALID", "拾取坐标必须有限。");
    if (point.x < 0 || point.y < 0 || point.x >= frame.physicalWidth || point.y >= frame.physicalHeight) {
        Fail("VIEW_RAY_POINT_OUTSIDE", "拾取点不在当前帧物理视口内。");
    }
}

Matrix ReadMatrix(const ViewProjectionFrame& frame) {
    Matrix matrix{};
    for (std::size_t row = 0; row < 4; ++row) for (std::size_t column = 0; column < 4; ++column) {
        const auto value = frame.rowMajorViewProjection[row * 4 + column];
        if (!std::isfinite(value)) Fail("VIEW_RAY_MATRIX_INVALID", "已完成帧的投影矩阵包含非有限值。");
        matrix[row][column] = value;
    }
    return matrix;
}

Matrix Inverse(const Matrix& matrix) {
    std::array<std::array<double, 8>, 4> augmented{};
    std::array<double, 4> scale{};
    for (std::size_t row = 0; row < 4; ++row) {
        for (std::size_t column = 0; column < 4; ++column) {
            augmented[row][column] = matrix[row][column];
            scale[row] = std::max(scale[row], std::abs(matrix[row][column]));
        }
        if (scale[row] == 0) Fail("VIEW_RAY_MATRIX_SINGULAR", "已完成帧的投影矩阵不可逆。");
        augmented[row][row + 4] = 1;
    }
    // 按行尺度选主元，避免把整体很小的合法齐次矩阵与奇异矩阵混为一谈。
    for (std::size_t column = 0; column < 4; ++column) {
        std::size_t selected = column;
        double bestRatio = 0;
        for (std::size_t row = column; row < 4; ++row) {
            const double ratio = std::abs(augmented[row][column]) / scale[row];
            if (ratio > bestRatio) { bestRatio = ratio; selected = row; }
        }
        if (bestRatio == 0) Fail("VIEW_RAY_MATRIX_SINGULAR", "已完成帧的投影矩阵不可逆。");
        if (!std::isfinite(bestRatio) || bestRatio <= PivotTolerance) {
            Fail("VIEW_RAY_MATRIX_UNSTABLE", "投影矩阵接近奇异，不能可靠反投影。");
        }
        if (selected != column) {
            std::swap(augmented[selected], augmented[column]);
            std::swap(scale[selected], scale[column]);
        }
        const double pivot = augmented[column][column];
        for (auto& value : augmented[column]) value /= pivot;
        for (std::size_t row = 0; row < 4; ++row) {
            if (row == column) continue;
            const double factor = augmented[row][column];
            for (std::size_t entry = 0; entry < 8; ++entry) {
                augmented[row][entry] = std::fma(-factor, augmented[column][entry], augmented[row][entry]);
            }
            augmented[row][column] = 0;
        }
    }
    Matrix inverse{};
    for (std::size_t row = 0; row < 4; ++row) for (std::size_t column = 0; column < 4; ++column) {
        inverse[row][column] = augmented[row][column + 4];
        if (!std::isfinite(inverse[row][column])) Fail("VIEW_RAY_MATRIX_UNSTABLE", "投影矩阵求逆产生非有限值。");
    }
    // 验证逆矩阵的后向误差；后续还在所选像素执行 near/far 回投影检查。
    for (std::size_t row = 0; row < 4; ++row) for (std::size_t column = 0; column < 4; ++column) {
        double value = 0, magnitude = 0;
        for (std::size_t i = 0; i < 4; ++i) {
            const double term = matrix[row][i] * inverse[i][column];
            value += term; magnitude += std::abs(term);
        }
        const double expected = row == column ? 1 : 0;
        if (!std::isfinite(value) || std::abs(value - expected) > PivotTolerance * 4 * std::max(1.0, magnitude)) {
            Fail("VIEW_RAY_MATRIX_UNSTABLE", "投影矩阵求逆误差超出可用范围。");
        }
    }
    return inverse;
}

Vector4 Transform(const Vector4& vector, const Matrix& matrix) {
    Vector4 result{};
    for (std::size_t column = 0; column < 4; ++column) {
        for (std::size_t row = 0; row < 4; ++row) result[column] = std::fma(vector[row], matrix[row][column], result[column]);
    }
    return result;
}

Vector3 Unproject(const Vector4& clip, const Matrix& inverse, const Matrix& matrix) {
    const auto homogeneous = Transform(clip, inverse);
    // w=0 对应无穷远端；负 w 不属于当前 D3D 的可见齐次半空间。
    if (!std::all_of(homogeneous.begin(), homogeneous.end(), [](double value) { return std::isfinite(value); })
        || homogeneous[3] <= 0) Fail("VIEW_RAY_HOMOGENEOUS_INVALID", "反投影端点位于无穷远或无效齐次半空间。");
    Vector3 point{};
    for (std::size_t axis = 0; axis < 3; ++axis) {
        point[axis] = homogeneous[axis] / homogeneous[3];
        if (!std::isfinite(point[axis]) || std::abs(point[axis]) > MaximumCoordinate) {
            Fail("VIEW_RAY_COORDINATE_LIMIT", "反投影端点超出有限世界坐标预算。");
        }
    }
    const auto projected = Transform({point[0], point[1], point[2], 1}, matrix);
    if (!std::isfinite(projected[3]) || projected[3] <= 0) Fail("VIEW_RAY_MATRIX_UNSTABLE", "反投影端点无法回到可见齐次半空间。");
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const double ndc = projected[axis] / projected[3];
        if (!std::isfinite(ndc) || std::abs(ndc - clip[axis]) > ProjectionTolerance) {
            Fail("VIEW_RAY_MATRIX_UNSTABLE", "反投影端点的回投影误差超出可用范围。");
        }
    }
    return point;
}

std::uint32_t RoundedPhysicalSize(std::uint32_t logical, double ratio) {
    const double scaled = static_cast<double>(logical) * ratio;
    if (!std::isfinite(scaled) || scaled >= 4096.5) Fail("VIEW_RAY_DPR_MISMATCH", "逻辑尺寸与 DPR 对应的物理视口超过帧尺寸预算。");
    // logical 和 ratio 均为正；等价于现有 Qt max(1,qRound(...))，不依赖 Qt。
    return static_cast<std::uint32_t>(std::max(1.0, std::floor(scaled + 0.5)));
}

} // namespace

Result<ViewRaySegment> MakeViewRay(const ViewProjectionFrame& frame, PhysicalViewPoint point) {
    try {
        ValidateViewport(frame);
        ValidatePoint(frame, point);
        const auto matrix = ReadMatrix(frame);
        const auto inverse = Inverse(matrix);
        const double x = 2 * point.x / frame.physicalWidth - 1, y = 1 - 2 * point.y / frame.physicalHeight;
        const auto nearPoint = Unproject({x, y, 0, 1}, inverse, matrix);
        const auto farPoint = Unproject({x, y, 1, 1}, inverse, matrix);
        Vector3 direction{farPoint[0] - nearPoint[0], farPoint[1] - nearPoint[1], farPoint[2] - nearPoint[2]};
        const double distance = std::hypot(direction[0], direction[1], direction[2]);
        if (!std::isfinite(distance) || distance > MaximumDistance) Fail("VIEW_RAY_DISTANCE_LIMIT", "反投影射线长度超过拾取距离预算。");
        if (distance <= MinimumDistance) Fail("VIEW_RAY_SEGMENT_DEGENERATE", "反投影近远端点过近，无法确定拾取方向。");
        for (auto& component : direction) component /= distance;
        return Result<ViewRaySegment>::Success({frame.identity, point, {nearPoint, direction}, farPoint, distance});
    } catch (const ViewRayFailure& failure) {
        return Result<ViewRaySegment>::Failure("ViewRay", failure.code, failure.message);
    }
}

Result<PhysicalViewPoint> LogicalToPhysicalViewPoint(const ViewProjectionFrame& frame,
    const ViewLogicalMetrics& metrics, LogicalViewPoint point) {
    try {
        ValidateViewport(frame);
        if (metrics.width == 0 || metrics.height == 0 || !std::isfinite(metrics.devicePixelRatio) || metrics.devicePixelRatio <= 0) {
            Fail("VIEW_RAY_DPR_INVALID", "逻辑视口尺寸及 DPR 必须有限且为正。");
        }
        if (RoundedPhysicalSize(metrics.width, metrics.devicePixelRatio) != frame.physicalWidth
            || RoundedPhysicalSize(metrics.height, metrics.devicePixelRatio) != frame.physicalHeight) {
            Fail("VIEW_RAY_DPR_MISMATCH", "逻辑视口或 DPR 与已完成帧物理尺寸不一致。");
        }
        if (!std::isfinite(point.x) || !std::isfinite(point.y)) Fail("VIEW_RAY_POINT_INVALID", "拾取逻辑坐标必须有限。");
        if (point.x < 0 || point.y < 0 || point.x >= metrics.width || point.y >= metrics.height) {
            Fail("VIEW_RAY_POINT_OUTSIDE", "拾取点不在逻辑视口内。");
        }
        const PhysicalViewPoint physical{point.x * metrics.devicePixelRatio, point.y * metrics.devicePixelRatio};
        ValidatePoint(frame, physical);
        return Result<PhysicalViewPoint>::Success(physical);
    } catch (const ViewRayFailure& failure) {
        return Result<PhysicalViewPoint>::Failure("ViewRay", failure.code, failure.message);
    }
}

Result<ViewRaySegment> MakeViewRayFromLogical(const ViewProjectionFrame& frame,
    const ViewLogicalMetrics& metrics, LogicalViewPoint point) {
    const auto physical = LogicalToPhysicalViewPoint(frame, metrics, point);
    if (!physical.Succeeded()) {
        Result<ViewRaySegment> failure;
        failure.diagnostics = physical.diagnostics;
        return failure;
    }
    return MakeViewRay(frame, *physical.value);
}

} // namespace War3Map::Render
