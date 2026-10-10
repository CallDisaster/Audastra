#include "War3MapCore/Render/ViewRay.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>

#if defined(_WIN32)
#include <DirectXMath.h> // 仅测试侧对照；生产 Core 不依赖 DirectX。
#endif

namespace {

using namespace War3Map;
using namespace War3Map::Render;
using Matrix = std::array<float, 16>;

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void Near(double actual, double expected, const char* message, double tolerance = 1e-8) {
    if (!std::isfinite(actual) || std::abs(actual - expected) > tolerance) throw std::runtime_error(message);
}

void VectorNear(const std::array<double, 3>& actual, const std::array<double, 3>& expected,
    const char* message, double tolerance = 1e-8) {
    for (std::size_t axis = 0; axis < 3; ++axis) Near(actual[axis], expected[axis], message, tolerance);
}

Matrix Identity() { return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}; }

Matrix Perspective() {
    // 原创易手算的 LH 投影：xScale=1、yScale=2、nearZ=2、farZ=10。
    return {1, 0, 0, 0, 0, 2, 0, 0, 0, 0, 1.25f, 1, 0, 0, -2.5f, 0};
}

Matrix Product(const Matrix& a, const Matrix& b) {
    Matrix result{};
    for (std::size_t row = 0; row < 4; ++row) for (std::size_t column = 0; column < 4; ++column) {
        double value = 0;
        for (std::size_t i = 0; i < 4; ++i) value += static_cast<double>(a[row * 4 + i]) * b[i * 4 + column];
        result[row * 4 + column] = static_cast<float>(value);
    }
    return result;
}

ViewProjectionFrame Frame(Matrix matrix = Identity(), std::uint32_t width = 400, std::uint32_t height = 200) {
    return {{123, 456, 789, 1011}, matrix, width, height};
}

ViewRaySegment Success(const ViewProjectionFrame& frame, PhysicalViewPoint point) {
    const auto result = MakeViewRay(frame, point);
    if (!result.Succeeded()) throw std::runtime_error("射线意外失败：" + result.diagnostics.front().code);
    Require(result.value->identity == frame.identity, "CPU 帧身份必须原样传递");
    Near(result.value->physicalPoint.x, point.x, "物理点 X 不能被取整或加半像素");
    Near(result.value->physicalPoint.y, point.y, "物理点 Y 不能被取整或加半像素");
    Near(std::hypot(result.value->ray.direction[0], result.value->ray.direction[1], result.value->ray.direction[2]), 1,
        "射线方向必须归一化");
    return *result.value;
}

template<typename T>
void Rejected(const Result<T>& result, std::string_view code) {
    Require(!result.Succeeded() && !result.value, "错误必须无部分结果");
    Require(result.diagnostics.size() == 1 && result.diagnostics[0].severity == DiagnosticSeverity::Error,
        "错误必须有明确诊断");
    if (result.diagnostics[0].code != code) throw std::runtime_error("错误代码不符：" + result.diagnostics[0].code + "，预期 " + std::string(code));
}

void TestOrthographicCoordinatesAndIdentity() {
    auto frame = Frame();
    const auto center = Success(frame, {200, 100});
    VectorNear(center.ray.origin, {0, 0, 0}, "中心应映射 NDC 原点");
    VectorNear(center.farPoint, {0, 0, 1}, "D3D far 深度应为 1");
    Near(center.maxDistance, 1, "单位正交深度范围错误");
    const auto topLeft = Success(frame, {0, 0});
    VectorNear(topLeft.ray.origin, {-1, 1, 0}, "左上角坐标或 Y 方向错误");
    const auto bottomRight = Success(frame, {399.5, 199.5});
    VectorNear(bottomRight.ray.origin, {0.9975, -0.995, 0}, "右下像素中心换算错误");
    const auto fractional = Success(frame, {123.25, 67.75});
    VectorNear(fractional.ray.origin, {-0.38375, 0.3225, 0}, "连续物理坐标不能取整");
    frame.identity = {}; // 数学层不冒充应用层的完成帧/新鲜度验证器。
    Require(Success(frame, {200, 100}).identity == ViewFrameIdentity{}, "不透明帧身份不能由数学层重写");
    auto scaled = Identity();
    for (auto& value : scaled) value *= 1e-30f;
    const auto smallMatrix = Success(Frame(scaled), {200, 100});
    VectorNear(smallMatrix.farPoint, {0, 0, 1}, "整体小尺度齐次矩阵不等于奇异矩阵");
}

void TestPerspectiveAndOriginalFloatMatrix() {
    const auto frame = Frame(Perspective());
    const auto center = Success(frame, {200, 100});
    VectorNear(center.ray.origin, {0, 0, 2}, "near 不能从 eye 或深度 -1 开始");
    VectorNear(center.farPoint, {0, 0, 10}, "透视 far 点错误");
    Near(center.maxDistance, 8, "中心 near→far 距离错误");
    const auto corner = Success(frame, {0, 0});
    VectorNear(corner.ray.origin, {-2, 1, 2}, "非正方形透视视口 near 点错误");
    VectorNear(corner.farPoint, {-10, 5, 10}, "非正方形透视视口 far 点错误");
    Near(corner.maxDistance, 12, "斜射线长度不能直接用 farZ-nearZ");
    auto rounded = Identity();
    rounded[10] = std::nextafter(1.0f, 2.0f);
    rounded[11] = 1;
    rounded[14] = -1;
    rounded[15] = 0;
    const auto actual = Success(Frame(rounded), {200, 100});
    Near(actual.ray.origin[2], 1.0 / rounded[10], "near 必须来自传入 float 矩阵");
    Near(actual.farPoint[2], 8388608, "不得重算或猜测原相机的 nominal farZ", 0.1);
}

void TestViewComposition() {
    // eye=(10,20,30)，看向世界 +X；屏幕右方为 +Y，上方为 +Z。
    const Matrix view{0, 0, 1, 0, 1, 0, 0, 0, 0, 1, 0, 0, -20, -30, -10, 1};
    const auto frame = Frame(Product(view, Perspective()));
    const auto center = Success(frame, {200, 100});
    VectorNear(center.ray.origin, {12, 20, 30}, "组合 view/projection 近点错误");
    VectorNear(center.farPoint, {20, 20, 30}, "行向量 view*projection 顺序错误");
    VectorNear(center.ray.direction, {1, 0, 0}, "世界朝向错误");
    const auto upperRight = Success(frame, {300, 50});
    VectorNear(upperRight.ray.origin, {12, 21, 30.5}, "右上方应沿世界 +Y/+Z 偏移");
    const auto lowerLeft = Success(frame, {100, 150});
    VectorNear(lowerLeft.ray.origin, {12, 19, 29.5}, "左下方应沿世界 -Y/-Z 偏移");
}

void TestLogicalPixelsAndDpr() {
    for (const auto dpr : {1.0, 1.25, 1.5, 2.0}) {
        const ViewLogicalMetrics metrics{320, 160, dpr};
        const auto frame = Frame(Perspective(), static_cast<std::uint32_t>(320 * dpr), static_cast<std::uint32_t>(160 * dpr));
        const auto result = MakeViewRayFromLogical(frame, metrics, {160, 80});
        Require(result.Succeeded(), "各 DPI 中心点应可反投影");
        VectorNear(result.value->ray.origin, {0, 0, 2}, "DPR 不应重复应用或改变中心射线");
        Require(result.value->identity == frame.identity, "逻辑入口不能丢失帧身份");
    }
    const auto frame = Frame(Identity(), 1001, 754);
    const ViewLogicalMetrics odd{801, 603, 1.25};
    const auto point = LogicalToPhysicalViewPoint(frame, odd, {400.5 / 1.25, 300.5 / 1.25});
    Require(point.Succeeded(), "奇数逻辑尺寸应遵守 qRound");
    Near(point.value->x, 400.5, "DPR 换算 X 不能取整");
    Near(point.value->y, 300.5, "DPR 换算 Y 不能取整");
    const auto expected = Success(frame, *point.value);
    const auto actual = MakeViewRayFromLogical(frame, odd, {400.5 / 1.25, 300.5 / 1.25});
    Require(actual.Succeeded(), "逻辑组合入口应成功");
    VectorNear(actual.value->ray.origin, expected.ray.origin, "两种入口必须生成同一射线");
    Rejected(LogicalToPhysicalViewPoint(Frame(Identity(), 1002, 754), odd, {320, 240}), "VIEW_RAY_DPR_MISMATCH");
    Rejected(LogicalToPhysicalViewPoint(frame, odd, {800.99, 0}), "VIEW_RAY_POINT_OUTSIDE");
    Rejected(LogicalToPhysicalViewPoint(frame, odd, {801, 0}), "VIEW_RAY_POINT_OUTSIDE");
    Rejected(LogicalToPhysicalViewPoint(frame, odd, {0, 603}), "VIEW_RAY_POINT_OUTSIDE");
    Rejected(MakeViewRayFromLogical(frame, {801, 603, 0}, {0, 0}), "VIEW_RAY_DPR_INVALID");
    Rejected(MakeViewRayFromLogical(frame, {801, 603, -1}, {0, 0}), "VIEW_RAY_DPR_INVALID");
    Rejected(MakeViewRayFromLogical(frame, {801, 603, std::numeric_limits<double>::infinity()}, {0, 0}), "VIEW_RAY_DPR_INVALID");
    Rejected(MakeViewRayFromLogical(frame, {801, 603, 1e300}, {0, 0}), "VIEW_RAY_DPR_MISMATCH");
    Rejected(MakeViewRayFromLogical(frame, odd, {std::numeric_limits<double>::quiet_NaN(), 0}), "VIEW_RAY_POINT_INVALID");
}

void TestFailuresHaveNoPartialRay() {
    Rejected(MakeViewRay(Frame(Identity(), 0, 1), {0, 0}), "VIEW_RAY_VIEWPORT_INVALID");
    Rejected(MakeViewRay(Frame(Identity(), 1, 4097), {0, 0}), "VIEW_RAY_VIEWPORT_INVALID");
    Rejected(MakeViewRay(Frame(), {-0.1, 0}), "VIEW_RAY_POINT_OUTSIDE");
    Rejected(MakeViewRay(Frame(), {400, 0}), "VIEW_RAY_POINT_OUTSIDE");
    Rejected(MakeViewRay(Frame(), {0, 200}), "VIEW_RAY_POINT_OUTSIDE");
    Rejected(MakeViewRay(Frame(), {std::numeric_limits<double>::infinity(), 0}), "VIEW_RAY_POINT_INVALID");
    auto bad = Identity();
    bad[0] = std::numeric_limits<float>::quiet_NaN();
    Rejected(MakeViewRay(Frame(bad), {0, 0}), "VIEW_RAY_MATRIX_INVALID");
    Rejected(MakeViewRay(Frame({}), {0, 0}), "VIEW_RAY_MATRIX_SINGULAR");
    bad = Identity();
    bad[4] = 1; bad[5] = 0;
    Rejected(MakeViewRay(Frame(bad), {0, 0}), "VIEW_RAY_MATRIX_SINGULAR");
    bad[5] = 1e-20f;
    Rejected(MakeViewRay(Frame(bad), {0, 0}), "VIEW_RAY_MATRIX_UNSTABLE");
    const Matrix nearInfinity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 1, 0};
    Rejected(MakeViewRay(Frame(nearInfinity), {200, 100}), "VIEW_RAY_HOMOGENEOUS_INVALID");
    const Matrix farInfinity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 1, 0, 0, -1, 0};
    Rejected(MakeViewRay(Frame(farInfinity), {200, 100}), "VIEW_RAY_HOMOGENEOUS_INVALID");
    bad = Identity();
    for (auto& entry : bad) entry = -entry;
    Rejected(MakeViewRay(Frame(bad), {200, 100}), "VIEW_RAY_HOMOGENEOUS_INVALID");
    bad = Identity(); bad[10] = 1e-10f;
    Rejected(MakeViewRay(Frame(bad), {200, 100}), "VIEW_RAY_COORDINATE_LIMIT");
    bad = Identity(); bad[10] = 1e30f;
    Rejected(MakeViewRay(Frame(bad), {200, 100}), "VIEW_RAY_SEGMENT_DEGENERATE");
    const Matrix tooLong{1, 0, 0, 0, 0, 1, 0, 0, -1, -1, 1.0f / 600000000.0f, 0, 0, 0, 0, 1};
    Rejected(MakeViewRay(Frame(tooLong), {200, 100}), "VIEW_RAY_DISTANCE_LIMIT");
}

MapData::TerrainGridSnapshot FlatTerrain(float height) {
    MapData::TerrainGridSnapshot grid;
    grid.header.width = grid.header.height = 2;
    grid.header.offset = {-64, -64};
    for (const auto y : {-64.0f, 64.0f}) for (const auto x : {-64.0f, 64.0f}) {
        MapData::TerrainVertex vertex;
        vertex.groundHeightRaw = static_cast<std::uint16_t>(8192 + height * 4);
        vertex.waterHeightAndFlagsRaw = 8192; vertex.cliffAndLayerRaw = 2;
        vertex.position = {x, y, height};
        grid.vertices.push_back(vertex);
    }
    grid.tiles.push_back({{0, 1, 2, 3}});
    grid.minimum = {-64, -64, height}; grid.maximum = {64, 64, height};
    return grid;
}

void TestTerrainNearFarClipping() {
    const auto segment = Success(Frame(Perspective()), {200, 100});
    for (const auto height : {1.0f, 11.0f}) {
        const auto result = PickTerrainHeightPreview(FlatTerrain(height), segment.ray, {segment.maxDistance, 8192});
        Require(result.Succeeded() && !result.value->hit, "near 前/far 后的地形不能被选中");
    }
    const auto result = PickTerrainHeightPreview(FlatTerrain(6), segment.ray, {segment.maxDistance, 8192});
    Require(result.Succeeded() && result.value->hit, "视锥内部地形应命中");
    Near(result.value->hit->distance, 4, "地形命中距离应从 near 点起算");
    // 射线段自身包含数学裁剪端点；最终像素覆盖/DepthFunc 的边界由渲染集成层决定。
    for (const auto height : {2.0f, 10.0f}) {
        const auto endpoint = PickTerrainHeightPreview(FlatTerrain(height), segment.ray, {segment.maxDistance, 8192});
        Require(endpoint.Succeeded() && endpoint.value->hit, "有限射线应保留近远端点");
    }
}

#if defined(_WIN32)
void TestDirectXMathOracle() {
    using namespace DirectX;
    const auto identity = XMMatrixIdentity();
    for (const auto dimensions : {std::array<std::uint32_t, 2>{800, 400}, std::array<std::uint32_t, 2>{400, 800}}) {
        for (const auto yaw : {-2.4f, -0.3f, 1.2f, 2.8f}) {
            for (const auto pitch : {0.05f, 0.8f, 1.5f}) {
                const XMVECTOR target = XMVectorSet(7, -11, 13, 1);
                const XMVECTOR eye = XMVectorAdd(target, XMVectorSet(80 * std::cos(pitch) * std::cos(yaw),
                    80 * std::cos(pitch) * std::sin(yaw), 80 * std::sin(pitch), 0));
                const XMMATRIX view = XMMatrixLookAtLH(eye, target, XMVectorSet(0, 0, 1, 0));
                const XMMATRIX projection = XMMatrixPerspectiveFovLH(XM_PIDIV4,
                    static_cast<float>(dimensions[0]) / static_cast<float>(dimensions[1]), 1, 256);
                const XMMATRIX combined = view * projection;
                XMFLOAT4X4 stored;
                XMStoreFloat4x4(&stored, combined);
                Matrix matrix{};
                for (std::size_t row = 0; row < 4; ++row) for (std::size_t column = 0; column < 4; ++column) {
                    matrix[row * 4 + column] = stored.m[row][column];
                }
                const auto frame = Frame(matrix, dimensions[0], dimensions[1]);
                for (const auto normalized : {std::array<double, 2>{0.5, 0.5}, std::array<double, 2>{0.01, 0.01},
                    std::array<double, 2>{0.99, 0.01}, std::array<double, 2>{0.01, 0.99}, std::array<double, 2>{0.99, 0.99}}) {
                    const PhysicalViewPoint point{normalized[0] * dimensions[0], normalized[1] * dimensions[1]};
                    const auto ray = Success(frame, point);
                    for (const auto depth : {0.0f, 1.0f}) {
                        XMFLOAT3 oracle;
                        // 将已经相乘的同一 float MVP 作为 projection，避免 oracle 重算另一种舍入的乘积。
                        XMStoreFloat3(&oracle, XMVector3Unproject(XMVectorSet(static_cast<float>(point.x), static_cast<float>(point.y), depth, 1),
                            0, 0, static_cast<float>(dimensions[0]), static_cast<float>(dimensions[1]), 0, 1, combined, identity, identity));
                        const auto& actual = depth == 0 ? ray.ray.origin : ray.farPoint;
                        VectorNear(actual, {oracle.x, oracle.y, oracle.z}, "DirectXMath 反投影对照不一致", depth == 0 ? 0.002 : 0.03);
                    }
                }
            }
        }
    }
}

void TestActualRendererScale() {
    using namespace DirectX;
    for (const auto dimension : {129u, 193u}) {
        MapData::TerrainGridSnapshot terrain;
        terrain.header.width = terrain.header.height = dimension;
        const float offset = -static_cast<float>((dimension - 1) * 64) - 32;
        terrain.header.offset = {offset, offset};
        for (std::uint32_t y = 0; y < dimension; ++y) for (std::uint32_t x = 0; x < dimension; ++x) {
            MapData::TerrainVertex vertex;
            vertex.groundHeightRaw = 8192; vertex.waterHeightAndFlagsRaw = 8192; vertex.cliffAndLayerRaw = 2;
            vertex.position = {offset + static_cast<float>(x) * 128.0f, offset + static_cast<float>(y) * 128.0f, 0};
            terrain.vertices.push_back(vertex);
            if (x + 1 < dimension && y + 1 < dimension) {
                const auto sw = y * dimension + x;
                terrain.tiles.push_back({{sw, sw + 1, sw + dimension, sw + dimension + 1}});
            }
        }
        terrain.minimum = {offset, offset, 0};
        terrain.maximum = {offset + static_cast<float>(dimension - 1) * 128.0f, offset + static_cast<float>(dimension - 1) * 128.0f, 0};
        for (const auto targetCoordinate : {0.0f, -1000.0f}) for (const auto distance : {20000.0f, 60000.0f}) {
            constexpr float yaw = -0.7853982f, pitch = 0.8f;
            const XMVECTOR target = XMVectorSet(targetCoordinate, targetCoordinate, 0, 1);
            const XMVECTOR eye = XMVectorAdd(target, XMVectorSet(distance * std::cos(pitch) * std::cos(yaw),
                distance * std::cos(pitch) * std::sin(yaw), distance * std::sin(pitch), 0));
            // 与实际 renderer 一致的 float 路径和 near/far；不重算 double camera 或猜 nominal far 端点。
            XMFLOAT4X4 stored;
            XMStoreFloat4x4(&stored, XMMatrixLookAtLH(eye, target, XMVectorSet(0, 0, 1, 0)) *
                XMMatrixPerspectiveFovLH(XM_PIDIV4, 1920.0f / 1080.0f, 1.0f, 1000000.0f));
            Matrix matrix{};
            for (std::size_t row = 0; row < 4; ++row) for (std::size_t column = 0; column < 4; ++column) {
                matrix[row * 4 + column] = stored.m[row][column];
            }
            const auto frame = Frame(matrix, 1920, 1080);
            for (const auto point : {PhysicalViewPoint{960, 540}, PhysicalViewPoint{0.5, 0.5}, PhysicalViewPoint{1919.5, 0.5},
                PhysicalViewPoint{0.5, 1079.5}, PhysicalViewPoint{1919.5, 1079.5}}) {
                const auto ray = Success(frame, point);
                Require(std::isfinite(ray.maxDistance) && ray.maxDistance > 0 && ray.maxDistance <= 1e9,
                    "真实尺度中心/边角应产生有限裁剪射线");
                const auto pick = PickTerrainHeightPreview(terrain, ray.ray, {ray.maxDistance, 8192});
                Require(pick.Succeeded(), "真实尺度射线须得到明确命中/未命中，不能因矩阵条件差报错");
                const auto repeat = PickTerrainHeightPreview(terrain, ray.ray, {ray.maxDistance, 8192});
                Require(repeat.Succeeded() && repeat.value->hit.has_value() == pick.value->hit.has_value(),
                    "重复拾取必须稳定");
                if (pick.value->hit) {
                    Require(repeat.value->hit->tileIndex == pick.value->hit->tileIndex
                        && repeat.value->hit->triangle == pick.value->hit->triangle, "真实尺度命中格子不应漂移");
                    VectorNear(repeat.value->hit->position, pick.value->hit->position, "重复命中位置不应漂移");
                }
                if (point.x == 960 && point.y == 540) {
                    Require(pick.value->hit.has_value(), "真实相机中心应命中地形");
                    const auto tile = static_cast<std::uint32_t>(std::floor((targetCoordinate - offset) / 128.0f));
                    Require(pick.value->hit->tileX == tile && pick.value->hit->tileY == tile, "中心应命中 target 所在原始格子");
                }
            }
            for (const auto delta : {std::array<double, 2>{37, 53}, std::array<double, 2>{-701, 811}, std::array<double, 2>{977, -613}}) {
                const std::array<double, 4> world{targetCoordinate + delta[0], targetCoordinate + delta[1], 0, 1};
                std::array<double, 4> clip{};
                for (std::size_t column = 0; column < 4; ++column) for (std::size_t row = 0; row < 4; ++row) {
                    clip[column] += world[row] * matrix[row * 4 + column];
                }
                Require(clip[3] > 0, "原创样点必须位于实际帧前方");
                const PhysicalViewPoint pixel{(clip[0] / clip[3] + 1) * 960, (1 - clip[1] / clip[3]) * 540};
                const auto ray = Success(frame, pixel);
                const auto pick = PickTerrainHeightPreview(terrain, ray.ray, {ray.maxDistance, 8192});
                Require(pick.Succeeded() && pick.value->hit, "实际 float MVP 投影出的地形点应可拾回");
                VectorNear(pick.value->hit->position, {world[0], world[1], 0}, "真实尺度投影/拾取坐标误差超过 0.05 世界单位", 0.05);
                const auto tileX = static_cast<std::uint32_t>(std::floor((world[0] - offset) / 128));
                const auto tileY = static_cast<std::uint32_t>(std::floor((world[1] - offset) / 128));
                Require(pick.value->hit->tileX == tileX && pick.value->hit->tileY == tileY, "投影样点必须映射回正确原始格子");
            }
        }
    }
}
#endif

} // namespace

int main() {
    try {
        TestOrthographicCoordinatesAndIdentity();
        TestPerspectiveAndOriginalFloatMatrix();
        TestViewComposition();
        TestLogicalPixelsAndDpr();
        TestFailuresHaveNoPartialRay();
        TestTerrainNearFarClipping();
#if defined(_WIN32)
        TestDirectXMathOracle();
        TestActualRendererScale();
        std::cout << "ViewRayTests passed (DirectXMath oracle enabled)\n";
#else
        std::cout << "ViewRayTests passed (portable fixtures; DirectXMath oracle unavailable)\n";
#endif
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
