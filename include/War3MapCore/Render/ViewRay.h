#pragma once

#include "War3MapCore/Render/TerrainPicking.h"

namespace War3Map::Render {

struct ViewFrameIdentity {
    std::uint64_t surfaceEpoch = 0;
    std::uint64_t sceneEpoch = 0;
    std::uint64_t viewEpoch = 0;
    std::uint64_t frameCount = 0;
    [[nodiscard]] bool operator==(const ViewFrameIdentity&) const noexcept = default;
};

struct ViewProjectionFrame {
    // 必须由调用方提供已完成帧的同一份矩阵和物理尺寸，不从当前相机重新计算。
    ViewFrameIdentity identity;
    std::array<float, 16> rowMajorViewProjection{}; // 行向量 world * viewProjection；D3D 深度 0..1
    std::uint32_t physicalWidth = 0;
    std::uint32_t physicalHeight = 0;
};

struct PhysicalViewPoint { double x = 0, y = 0; }; // 连续物理像素坐标，不自动加半像素
struct LogicalViewPoint { double x = 0, y = 0; };
struct ViewLogicalMetrics {
    std::uint32_t width = 0, height = 0;
    double devicePixelRatio = 1;
};

struct ViewRaySegment {
    ViewFrameIdentity identity;
    PhysicalViewPoint physicalPoint;
    TerrainRay ray; // origin 在 near 裁剪面，direction 已归一化
    std::array<double, 3> farPoint{};
    double maxDistance = 0; // near→far 的世界斜距；不是相机 farZ 或从 eye 起算的距离
};

// 尺寸 1..4096。使用输入 float 矩阵的 double 求逆，不依赖 Qt/DirectX 或重算 camera。
// 越界、非有限、奇异/数值不稳定矩阵、无穷远端和不可表示射线均错误，无部分结果。
// 输出两端各分量绝对值 <= 1e9，长度在 (1e-12,1e9]；调用 TerrainPicking 时须显式
// 传入 {segment.maxDistance, tileBudget}，不能用默认距离越过本帧的 far 裁剪面。
// identity 原样传递；此纯数学 API 不证明帧已完成或仍新鲜，由调用方校验身份。
[[nodiscard]] Result<ViewRaySegment> MakeViewRay(
    const ViewProjectionFrame& frame, PhysicalViewPoint point);

// 独立 DPI 适配：按 max(1,qRound(logicalSize*DPR)) 核对帧尺寸；鼠标位置不 round。
// 逻辑/物理两侧均检查 [0,size)；末端因分数 DPR 越出栅格时拒绝，不能 clamp 成边缘命中。
// 只处理坐标；窗口可见性、DPR/viewEpoch 变化和 QObject 生命周期由 Qt 层负责。
[[nodiscard]] Result<PhysicalViewPoint> LogicalToPhysicalViewPoint(
    const ViewProjectionFrame& frame, const ViewLogicalMetrics& metrics, LogicalViewPoint point);

[[nodiscard]] Result<ViewRaySegment> MakeViewRayFromLogical(
    const ViewProjectionFrame& frame, const ViewLogicalMetrics& metrics, LogicalViewPoint point);

} // namespace War3Map::Render
