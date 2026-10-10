#include "War3MapCore/Render/AnimationParticleCoverage.h"
#include <algorithm>

namespace War3Map::Render {
Result<AnimationParticleCoverage> QueryAnimationParticleCoverage(const AnimatedModelScene& scene, const AnimationFrame& frame) {
    using Output = Result<AnimationParticleCoverage>;
    if (scene.sceneVersion != frame.sceneVersion || scene.documentRevision != frame.documentRevision
        || scene.particleCapabilities.size() != scene.specs.size()
        || (!frame.requestVisibility.empty() && frame.requestVisibility.size() != scene.specs.size()))
        return Output::Failure("Render.Animation", "ANIMATION_PARTICLE_COVERAGE_STALE", "粒子覆盖统计的场景和模拟帧身份不符。");
    AnimationParticleCoverage result;
    std::size_t linkIndex = 0;
    for (std::size_t request = 0; request < scene.particleCapabilities.size(); ++request) {
        const auto& capability = scene.particleCapabilities[request];
        const auto total = static_cast<std::uint64_t>(capability.particle2Count) + capability.ribbonCount + capability.premCount;
        result.declared += total;
        if (!total) continue;
        if (capability.premPreparedCount > capability.premCount)
            return Output::Failure("Render.Animation", "ANIMATION_PREM_CAPABILITY_RANGE", "PREM 实际准入数量超过来源声明。");
        if (scene.premEnabled) {
            result.unsupportedPrem += capability.premCount - capability.premPreparedCount;
            result.preparedPrem += capability.premPreparedCount;
        } else if (scene.particlesEnabled) {
            result.unsupportedPrem += capability.premCount; // 保留旧 PRE2 开启而 PREM 不支持的合同。
        } else result.disabled += capability.premCount;
        const auto supportedKinds = total - capability.premCount;
        if (!scene.particlesEnabled) { result.disabled += supportedKinds; continue; }
        const auto isolated = std::min<std::uint64_t>(supportedKinds, capability.skippedEmitters ? capability.skippedEmitters->size() : 0);
        result.isolated += isolated;
        const auto remaining = supportedKinds - isolated;
        if (!remaining) continue;
        if (!frame.requestVisibility.empty() && !frame.requestVisibility[request]) {
            result.outsideView += remaining;
            continue;
        }
        if (!capability.available) {
            if (capability.rejectionCode == "ANIMATION_PARTICLE_INSTANCE_BUDGET") result.budgetRejected += remaining;
            else result.assetRejected += remaining;
            continue;
        }
        // 服务产生的 particleLinks 按原始 requestIndex 升序；一次线性走访，避免大图 N²。
        while (linkIndex < frame.particleLinks.size() && frame.particleLinks[linkIndex].requestIndex < request) ++linkIndex;
        if (linkIndex == frame.particleLinks.size() || frame.particleLinks[linkIndex].requestIndex != request) {
            result.runtimeRejected += remaining; continue;
        }
        const auto& link = frame.particleLinks[linkIndex];
        if (link.identity != scene.specs[request].identity)
            return Output::Failure("Render.Animation", "ANIMATION_PARTICLE_COVERAGE_STALE", "粒子实例身份与统计场景不符。");
        if (link.frozen) result.frozen += remaining;
        if (!link.simulated || link.hidden) {
            if (link.budgetLimited) result.budgetRejected += remaining;
            else result.runtimeRejected += remaining;
        }
        // simulated=true 且 particleCount=0 是合法的发射间歇；不据此声称发射器不支持。
    }
    result.notShown = result.unsupportedPrem + result.isolated + result.disabled
        + result.assetRejected + result.runtimeRejected + result.budgetRejected + result.outsideView;
    return Output::Success(result);
}
}
