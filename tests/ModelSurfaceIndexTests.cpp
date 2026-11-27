#include "War3MapCore/Render/ModelSurfaceIndex.h"
#include "War3MapCore/Render/WorldModelScene.h"

#include <cmath>
#include <iostream>
#include <memory>
#include <numbers>
#include <stdexcept>
#include <vector>

namespace {
using namespace War3Map;
using namespace War3Map::Render;

void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

// 原创合成几何：z=0 平面上 [-h, h]² 的正方形，两个三角形。
MdxGeoset Square(float h, float z = 0) {
    MdxGeoset geoset;
    geoset.positions = {{-h, -h, z}, {h, -h, z}, {h, h, z}, {-h, h, z}};
    geoset.indices = {0, 1, 2, 0, 2, 3};
    return geoset;
}

std::shared_ptr<const MdxModel> Model(std::vector<MdxGeoset> geosets) {
    auto model = std::make_shared<MdxModel>();
    model->geosets = std::move(geosets);
    return model;
}

ModelSurfaceSource Place(std::uint32_t id, std::shared_ptr<const MdxModel> model, std::array<float, 3> position,
    float scale = 1, float angle = 0) {
    ModelInstanceRequest request;
    request.position = position; request.scale = {scale, scale, scale}; request.angle = angle;
    return {id, std::move(model), ModelInstanceTransform(request)};
}

void DropFindsTopmostSurfaceAndSource() {
    auto hidden = Square(100, 1000); hidden.visibleInStand = false;
    const auto square = Model({Square(100), hidden});
    const std::vector<ModelSurfaceSource> sources{Place(7, square, {0, 0, 50}), Place(9, square, {0, 0, 200}, 0.5f, std::numbers::pi_v<float> / 2)};
    auto built = ModelSurfaceIndex::Build(sources);
    Require(built.Succeeded() && built.value && built.value->TriangleCount() == 4 && built.value->SourceCount() == 2,
        "两个摆放各两个三角形，站立不可见的几何体不进索引");
    const auto& index = *built.value;
    const auto top = index.Drop(10, 10);
    Require(top && std::abs(top->position[2] - 200) < 1e-3 && top->source == 9 && top->normal[2] > 0.999, "重叠处取最高的表面");
    const auto lower = index.Drop(80, 80);
    Require(lower && std::abs(lower->position[2] - 50) < 1e-3 && lower->source == 7, "缩小的上层不覆盖处落到下层");
    Require(!index.Drop(500, 500), "表面以外没有落点");
    const auto ray = index.Raycast({0, 0, 1000}, {0, 0, -2}, 10000);
    Require(ray && std::abs(ray->distance - 400) < 1e-3 && ray->source == 9, "距离按方向向量长度计");
    Require(!index.Raycast({0, 0, 1000}, {0, 0, -1}, 700), "超出最大距离不命中");
    const auto below = index.Raycast({0, 0, 0}, {0, 0, 1}, 10000);
    Require(below && below->source == 7 && below->normal[2] < -0.999, "从下往上命中时法线朝向射线来处");
}

void SlopeNormalAndTransform() {
    // 沿 X 方向 45° 的斜面：z = x。
    MdxGeoset slope;
    slope.positions = {{0, -50, 0}, {100, -50, 100}, {100, 50, 100}, {0, 50, 0}};
    slope.indices = {0, 1, 2, 0, 2, 3};
    const std::vector<ModelSurfaceSource> sources{Place(1, Model({slope}), {1000, 2000, 10})};
    const auto index = *ModelSurfaceIndex::Build(sources).value;
    const auto hit = index.Drop(1050, 2000);
    Require(hit && std::abs(hit->position[2] - 60) < 1e-3, "斜面高度随位置变化并带实例平移");
    Require(std::abs(hit->normal[2] - std::cos(std::numbers::pi / 4)) < 1e-6 && hit->normal[0] < 0, "45° 斜面的法线");
}

void ManyTrianglesMatchBruteForce() {
    // 64×64 格、每格两个三角形的起伏面 z = 3·(i mod 5) + 2·(j mod 3)（按格的整体高度，便于逐点核对）。
    MdxGeoset grid;
    constexpr int n = 64;
    for (int j = 0; j < n; ++j) for (int i = 0; i < n; ++i) {
        const float z = 3.0f * (i % 5) + 2.0f * (j % 3);
        const auto base = static_cast<std::uint16_t>(grid.positions.size());
        grid.positions.push_back({i * 10.0f, j * 10.0f, z});
        grid.positions.push_back({i * 10.0f + 10, j * 10.0f, z});
        grid.positions.push_back({i * 10.0f + 10, j * 10.0f + 10, z});
        grid.positions.push_back({i * 10.0f, j * 10.0f + 10, z});
        for (const auto offset : {0, 1, 2, 0, 2, 3}) grid.indices.push_back(static_cast<std::uint16_t>(base + offset));
    }
    const std::vector<ModelSurfaceSource> sources{Place(3, Model({grid}), {0, 0, 0})};
    const auto index = *ModelSurfaceIndex::Build(sources).value;
    Require(index.TriangleCount() == n * n * 2, "全部三角形进入索引");
    for (int j = 0; j < n; ++j) for (int i = 0; i < n; ++i) {
        const auto hit = index.Drop(i * 10.0 + 3.7, j * 10.0 + 6.1);
        if (!hit || std::abs(hit->position[2] - (3.0 * (i % 5) + 2.0 * (j % 3))) > 1e-3) throw std::runtime_error("逐格落点与几何不符");
    }
}

void TriangleBudgetSkipsWholePlacements() {
    const auto square = Model({Square(10)});
    const std::vector<ModelSurfaceSource> sources{Place(1, square, {0, 0, 0}), Place(2, square, {0, 0, 50}), Place(3, nullptr, {0, 0, 0})};
    ModelSurfaceLimits limits; limits.maxTriangles = 3;
    auto built = ModelSurfaceIndex::Build(sources, limits);
    Require(built.Succeeded() && built.value->TriangleCount() == 2 && built.value->SourceCount() == 1, "超出上限的摆放整块跳过");
    Require(built.diagnostics.size() == 1 && built.diagnostics[0].code == "MODEL_SURFACE_SKIPPED"
        && built.diagnostics[0].severity == DiagnosticSeverity::Warning, "跳过的摆放给出警告而不是失败");
    const auto hit = built.value->Drop(0, 0);
    Require(hit && hit->source == 1, "收进的摆放照常可用");
    Require(ModelSurfaceIndex::Build({}).value->Empty() && !ModelSurfaceIndex::Build({}).value->Drop(0, 0), "空索引没有落点");
}
} // namespace

int main() {
    try {
        DropFindsTopmostSurfaceAndSource();
        SlopeNormalAndTransform();
        ManyTrianglesMatchBruteForce();
        TriangleBudgetSkipsWholePlacements();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
