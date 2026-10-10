#pragma once
#include "War3MapCore/Render/ModelAnimation.h"

namespace War3Map::Render {
// 按实际摆放实例累计，不按共享资产去重；正常时刻没有存活粒子不算缺失。
// 只覆盖成功读出声明的模型，完全不能解析的资产不会被猜测为零发射器。
// 统计的是传入模拟帧的排除原因；GPU 是否完成及实际像素须另查完成帧截图回执。
struct AnimationParticleCoverage {
    std::uint64_t declared = 0, notShown = 0;
    std::uint64_t unsupportedPrem = 0, isolated = 0, disabled = 0;
    std::uint64_t assetRejected = 0, runtimeRejected = 0, budgetRejected = 0;
    std::uint64_t outsideView = 0, frozen = 0; // 发射器数；视野外不属于预算/运行失败，冻结与预算原因可重叠。
    std::uint64_t preparedPrem = 0; // 子模型来源/Stand已准入，不能据此宣称本帧GPU完成。
};
[[nodiscard]] Result<AnimationParticleCoverage> QueryAnimationParticleCoverage(
    const AnimatedModelScene& scene, const AnimationFrame& frame);
}
