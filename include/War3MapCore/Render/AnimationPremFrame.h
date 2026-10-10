#pragma once
#include "War3MapCore/Render/PremPreview.h"
#include <memory>
#include <string>
#include <vector>

namespace War3Map::Render {
struct AnimatedModelScene;
struct AnimationFrame;

// 不可变的准备回执，实际支持只来自 emitter + 子模型真实 Stand0ms 的双重准入。
struct AnimationPremCapability {
    War3RenderCore::AssetIdentity parentAsset;
    War3RenderCore::AssetSource parentSource, childSource;
    std::uint32_t parentAssetIndex = UINT32_MAX, sourceIndex = UINT32_MAX;
    std::uint32_t nodeId = UINT32_MAX, otherNodeIndex = UINT32_MAX, compiledNodeIndex = UINT32_MAX;
    std::uint32_t childAssetIndex = UINT32_MAX;
    std::uint64_t sourceByteOffset = 0;
    std::string originalPath, resolvedPath, rejectionCode, rejectionMessage;
    std::shared_ptr<const Prem::Emitter> emitter;
    bool prepared = false;
};

struct AnimationPremRequestClock {
    War3RenderCore::InstanceIdentity identity;
    War3RenderCore::AssetIdentity asset;
    std::uint32_t requestIndex = UINT32_MAX, sequenceIndex = UINT32_MAX;
    std::uint64_t parentElapsedMs = 0, parentGlobalMs = 0; // 本帧真实父状态。
    std::uint64_t elapsedMs = 0, globalMs = 0; // PREM lifespan 继续老化的独立尾时钟。
    bool tailAging = false;
    bool tailPaused = false; // 显式暂停持续到显式播放；不因下一次空advance偷偷恢复。
    double tailFractionalMs = 0; // 非整数速度不丢小数尾量，属于本帧checkpoint。
};

struct AnimationPremSampleError {
    War3RenderCore::InstanceIdentity identity;
    War3RenderCore::AssetIdentity asset;
    std::uint32_t requestIndex = UINT32_MAX, sourceIndex = UINT32_MAX;
    std::uint64_t birthElapsedMs = 0, birthGlobalMs = 0;
    std::string code;
    War3RenderCore::PoseDiagnostic diagnostic;
};

struct AnimationPremSlotBinding {
    std::uint32_t childIndex = UINT32_MAX, childRequestIndex = UINT32_MAX;
};

// 保留实际所有者的预算分项；source/resource 为正式 State 的账，evaluator 为实际 StorageBytes。
// layout 与实际 Stand 容量取 max 收费一次，不把含 workspace 的 State.frameBytes 再整体相加。
struct AnimationPremBudgetBreakdown {
    std::uint64_t parentAssetBytesBefore = 0, parentAssetBytesAfter = 0, parentFixedFrameBytes = 0;
    std::uint64_t childSourceBytes = 0, childResourceBytes = 0, childSceneBytes = 0;
    std::uint64_t childStandFrameBytes = 0, childFrameLayoutBytes = 0, childFrameChargedBytes = 0;
    std::uint64_t childEvaluatorBytes = 0, birthEvaluatorBytes = 0;
    std::uint64_t preparationResourceBytes = 0; // 准备时临时冻结cache，结束后释放；不冒充持久帧资产。
};

// 只由同次已接受父帧产生，Qt/捕获须先比较全部 parent 字段，不能只按 layer 名称复用。
// childScene/childFrame 属于独立层；没有父地图摆放、点选或撤销身份。
struct AnimationPremFrame {
    std::uint64_t parentSceneVersion = 0, parentContentVersion = 0, parentDocumentRevision = 0;
    std::uint64_t parentGeneration = 0, parentEvaluationEpoch = 0;
    std::string parentSourceIdentity;
    std::shared_ptr<const AnimatedModelScene> childScene;
    std::shared_ptr<const AnimationFrame> childFrame;
    std::shared_ptr<const std::vector<AnimationPremCapability>> capabilities;
    Prem::Frame emission;
    std::vector<AnimationPremRequestClock> requestClocks;
    std::vector<AnimationPremSampleError> sampleErrors;
    std::vector<AnimationPremSlotBinding> childSlots; // childIndex 对应 emission.children 的真实行。
    std::uint64_t supportedEmitterInstances = 0, unsupportedEmitterInstances = 0;
    std::uint64_t requestBudgetOmissions = 0, slotSuppressedChildren = 0;
    std::uint64_t storageBytes = 0, workingBytes = 0, assetBytes = 0;
    bool childStandFrozenApproximation = true; // 真实 Stand0ms，不推进子自身动作。
    AnimationPremBudgetBreakdown budget;
};
} // namespace War3Map::Render
