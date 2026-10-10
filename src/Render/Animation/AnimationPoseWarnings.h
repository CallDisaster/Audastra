#pragma once
#include "War3MapCore/Render/ModelAnimation.h"
#include <initializer_list>
#include <algorithm>

namespace War3Map::Render {
inline War3RenderCore::PoseDiagnostic FirstAnimationPoseError(
    std::span<const War3RenderCore::PoseDiagnostic> diagnostics, War3RenderCore::Code fallback) noexcept {
    for (const auto& diagnostic : diagnostics)
        if (diagnostic.severity == War3RenderCore::Severity::Error) return diagnostic;
    return {fallback};
}

// 只在同次Evaluate之后使用对应request/receipt；从成功数值的原诊断捕获，禁止猜owner。
inline Status CaptureAnimationPoseWarnings(AnimationPoseLink& link,
    const War3RenderCore::PoseReceipt& receipt, const War3RenderCore::PoseRequest& request,
    std::span<const War3RenderCore::PoseDiagnostic> diagnostics, bool cachedStand,
    std::span<const War3RenderCore::MaterialAlphaPreviewWarning> materialWarnings = {}) {
    using namespace War3RenderCore;
    link.warnings = {};
    if (receipt.error != Code::Ok || receipt.instance != request.instance || receipt.asset != request.asset ||
        receipt.instance != link.identity || receipt.asset != link.asset ||
        receipt.evaluationEpoch != link.evaluationEpoch || receipt.poseVersion != link.poseVersion ||
        !receipt.evaluationEpoch || !receipt.poseVersion || receipt.source != PoseSource::Computed ||
        request.source != PoseSource::Computed)
        return Status::Failure("Render.Animation", "ANIMATION_POSE_WARNING_IDENTITY",
            "成功姿势的警告来源、请求或求值版本不匹配，未捕获错属记录。");
    auto& warnings = link.warnings;
    warnings.requestIndex = link.requestIndex;
    warnings.identity = receipt.instance;
    warnings.asset = receipt.asset;
    warnings.evaluationEpoch = receipt.evaluationEpoch;
    warnings.poseVersion = receipt.poseVersion;
    warnings.sequenceIndex = request.sequenceIndex;
    warnings.elapsedMs = request.sequenceElapsedMs;
    warnings.globalMs = request.globalElapsedMs;
    warnings.cachedStand = cachedStand;
    // 主问题优先保留；其余warning仍有精确遗漏计数，不伪称完整保留。
    for (const bool sourceOrder : {true, false}) {
        for (std::size_t diagnosticIndex = 0; diagnosticIndex < diagnostics.size(); ++diagnosticIndex) {
            const auto& diagnostic = diagnostics[diagnosticIndex];
            if (diagnostic.severity != Severity::Warning ||
                (diagnostic.code == Code::NodeSourceOrderPreview) != sourceOrder) continue;
            if (warnings.count < warnings.records.size()) {
                const auto outputIndex = warnings.count++;
                warnings.records[outputIndex] = diagnostic;
                if (diagnostic.code == Code::MaterialAlphaIdenticalDuplicatesPreview) {
                    const auto found = std::find_if(materialWarnings.begin(), materialWarnings.end(),
                        [diagnosticIndex](const auto& warning) { return warning.diagnosticIndex == diagnosticIndex; });
                    if (found == materialWarnings.end() || found->materialIndex == NoIndex || found->layerIndex == NoIndex)
                        return Status::Failure("Render.Animation", "ANIMATION_MATERIAL_WARNING_SOURCE",
                            "KMTA 成功警告缺少同次求值的材质来源，未猜测节点或轨迹归属。");
                    warnings.materialRecords[outputIndex] = *found;
                    warnings.materialRecords[outputIndex].diagnosticIndex = outputIndex;
                }
            }
            else ++warnings.omitted;
        }
    }
    return Status::Ok();
}

inline void RebindCachedAnimationPoseWarnings(AnimationPoseLink& link) noexcept {
    if (!link.warnings.cachedStand || link.hidden || link.outsideView || !link.staticForBudget) {
        link.warnings = {};
        return;
    }
    // 仅复制已明确展示的同一缓存姿势；样本asset/epoch/version和时钟保持原值供读取验证。
    link.warnings.requestIndex = link.requestIndex;
    link.warnings.identity = link.identity;
}
}
