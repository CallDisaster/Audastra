#pragma once

#include "War3MapCore/Render/TerrainPicking.h"
#include "War3MapCore/Render/WorldModelScene.h"
#include "War3MapCore/Render/ViewRay.h"
#include "War3RenderCore/PoseTypes.h"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace War3Map::Render {

struct ModelPickHit {
    std::uint32_t instance = 0; // WorldModelScene::instances 下标
    double distance = 0;        // 沿归一化射线的进入距离
};

// 射线对各摆放实例的定向包围盒（模型局部 AABB 经实例变换）。烘焙网格不参与；
// 世界尺寸小于 minimumHalfExtent 的轴按该半径放大，便于点中小物件。
// 结果按进入距离升序、至多 maxHits 个；同一位置重复单击可在其间轮换。
[[nodiscard]] std::vector<ModelPickHit> PickModelInstances(const WorldModelScene& scene, const TerrainRay& ray,
    double maxDistance, float minimumHalfExtent = 24.0f, std::size_t maxHits = 16);

// 实例定向包围盒的 8 个世界坐标角点：下层 (−x−y, +x−y, +x+y, −x+y) 后接上层同序。
[[nodiscard]] std::optional<std::array<std::array<float, 3>, 8>> ModelInstanceBox(const WorldModelScene& scene,
    std::uint32_t instance, float minimumHalfExtent = 24.0f);

struct AnimatedModelScene;
struct AnimationFrame;
struct ModelPickBinding {
    std::uint64_t documentRevision = 0, generation = 0, sceneVersion = 0, contentVersion = 0;
    std::uint64_t modelEpoch = 0, presentationRevision = 0;
    ViewFrameIdentity view;
    bool operator==(const ModelPickBinding&) const noexcept = default;
};
struct ModelPickVisibilitySnapshot {
    ModelPickBinding binding;
    std::vector<std::uint8_t> visible, selectable; // 原始 scene.specs 请求序，值必须 0/1。
};
struct ModelPickLimits {
    std::uint32_t maxInstances = 0, maxCandidates = 256, maxTriangles = 524288; // 0扫描既有场景；非0仅收紧本次工作。
    std::uint32_t maxShapes = 1024, maxHits = 16, maxIssues = 16; // 默认亦硬上限，只能收紧。
};
struct PreciseModelPickHit {
    War3RenderCore::InstanceIdentity identity;
    std::uint32_t requestIndex = 0, renderedInstance = 0, grade = 0; // Mesh=8，Collision=4。
    double distance = 0;
    std::optional<std::uint32_t> geoset, triangle, collisionShape;
};
struct ModelPickIssue {
    War3RenderCore::InstanceIdentity identity;
    std::uint32_t requestIndex = 0;
    std::optional<std::uint32_t> geoset, collisionShape;
    std::string code, message;
};
struct ModelPickResult {
    ModelPickBinding binding;
    std::vector<PreciseModelPickHit> hits;
    std::uint32_t testedInstances = 0, candidates = 0, triangles = 0, shapes = 0, issueTotal = 0;
    bool meshComplete = true, collisionComplete = true, budgetExceeded = false, hitsTruncated = false;
    std::vector<ModelPickIssue> issues;
};
// 宿主提供同一次已完成的 view/skin/presentation 回执，调用后还须复核其未变；纯数学层不证明 GPU 完成。
// AABB 仅作当前姿势保守粗筛；最终只返回真三角形/已证 box/sphere，不返回包围球或最小像素兜底。
// 预算不足返回明确 incomplete；unknown 碰撞仍允许独立 mesh 查询，不能当确定未命中。
[[nodiscard]] Result<ModelPickResult> PickAnimatedModelInstances(const AnimatedModelScene& scene,
    const AnimationFrame& frame, const ModelPickVisibilitySnapshot& visibility, const ModelPickBinding& expected,
    const TerrainRay& ray, double maxDistance, const ModelPickLimits& limits = {});

} // namespace War3Map::Render
