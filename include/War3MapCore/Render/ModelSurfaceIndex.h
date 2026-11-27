#pragma once

#include "War3MapCore/Types.h"
#include "War3MapCore/Render/MdxModel.h"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace War3Map::Render {

// 一个摆放的模型几何：模型取绑定姿态（站立可见的几何体），按实例变换（与 ModelInstanceTransform 相同的行主序 3×4）放到世界。
struct ModelSurfaceSource {
    std::uint32_t source = 0; // 调用方自己的编号（例如世界页请求序号），命中时原样返回
    std::shared_ptr<const MdxModel> model;
    std::array<float, 12> transform{};
};

struct ModelSurfaceLimits {
    // 三角形总数上限；超出的摆放整块跳过并在警告里计数，已收进的照常可用。
    std::uint64_t maxTriangles = 8'000'000;
};

struct ModelSurfaceHit {
    std::array<double, 3> position{};
    std::array<double, 3> normal{}; // 单位几何法线；朝向射线来处（竖直落点时总是朝上）
    std::uint32_t source = 0;
    double distance = 0;
};

// 模型表面落点索引：只在 CPU 上对静态摆放的三角形建 BVH，供散布笔刷等把东西放到模型表面上。
// 与渲染场景无关，界面、命令行和 MCP 共用；建好后只读，可跨线程查询。
class ModelSurfaceIndex {
public:
    [[nodiscard]] static Result<ModelSurfaceIndex> Build(std::span<const ModelSurfaceSource> sources,
        const ModelSurfaceLimits& limits = {});

    // 沿射线找最近命中（direction 不必归一化，distance 按 direction 长度计）。
    [[nodiscard]] std::optional<ModelSurfaceHit> Raycast(const std::array<double, 3>& origin,
        const std::array<double, 3>& direction, double maxDistance) const;
    // 在 (x, y) 处从上往下找最高的表面。
    [[nodiscard]] std::optional<ModelSurfaceHit> Drop(double x, double y) const;

    [[nodiscard]] std::size_t TriangleCount() const noexcept { return triangles_.size(); }
    [[nodiscard]] std::size_t SourceCount() const noexcept { return sourceCount_; }
    [[nodiscard]] bool Empty() const noexcept { return triangles_.empty(); }
    [[nodiscard]] const std::array<float, 6>& Bounds() const noexcept { return bounds_; } // minX minY minZ maxX maxY maxZ

private:
    struct Triangle { std::array<float, 9> points{}; std::uint32_t source = 0; };
    struct Node { std::array<float, 6> bounds{}; std::uint32_t first = 0, count = 0; }; // count==0：内部节点，first 为左子
    std::vector<Triangle> triangles_;
    std::vector<Node> nodes_;
    std::array<float, 6> bounds_{};
    std::size_t sourceCount_ = 0;
    void BuildNodes();
};

} // namespace War3Map::Render
