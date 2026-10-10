#pragma once

#include "War3MapCore/RenderPluginEditorVisibility.h"
#include "War3MapCore/Render/ModelAnimation.h"
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace War3Map::Modules {

[[nodiscard]] inline std::uint64_t WorldInstanceNowNs() noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

// 正式摆放从地图事务到真正Draw/Present的紧凑回执。零时间表示该阶段未执行/未取证；
// completed只有COMPLETED roster票据可以填写，UI占位及CPU接受均不能填充它。
struct WorldPlacementCommandTiming {
    std::uint64_t commandIdentity = 0, documentRevision = 0, placements = 0, retainedPlacements = 0;
    std::uint64_t inputStartedNs = 0, transactionCompletedNs = 0, captureStartedNs = 0;
    std::uint64_t captureNs = 0, registryPrepareNs = 0;
};
struct WorldPathingHelperTimings {
    // 当前prepare的CPU区间，资源读取属于queryNs；不冒称GPU或完成画面延迟。
    std::uint64_t mappingNs = 0, cacheNs = 0, classifyNs = 0, queryNs = 0, assetNs = 0, verifyNs = 0, totalNs = 0;
    std::uint64_t queryValidateNs = 0, queryRowCopyNs = 0, queryCanonicalNs = 0, queryHashNs = 0;
    std::uint64_t queryResourceNs = 0, queryFootprintNs = 0;
    std::uint64_t candidateCount = 0, helperCount = 0, queryCount = 0;
    std::uint64_t resourceReads = 0, resourceCacheHits = 0, assetsCreated = 0, assetsReused = 0;
};
struct WorldPlacementRefreshTimings {
    std::uint64_t snapshotNs = 0, auxiliaryNs = 0, buildingSplatsNs = 0, outlinerNs = 0, stageSnapshotNs = 0;
    bool reusedSnapshot = false, splatsDeferredToRoster = false, outlinerAfterQueue = false;
};
struct WorldInstanceTiming {
    std::uint64_t sequence = 0, operation = 0, commandIdentity = 0;
    std::uint64_t generation = 0, editSessionIdentity = 0, documentRevision = 0;
    std::uint64_t inputStartedNs = 0, transactionCompletedNs = 0, captureStartedNs = 0;
    std::uint64_t captureNs = 0, registryPrepareNs = 0, sourceSnapshotNs = 0;
    std::uint64_t queuedNs = 0, catalogCaptureNs = 0, catalogWorkerStartedNs = 0;
    std::uint64_t sourceOpenNs = 0, sourceHashNs = 0, archiveOpenNs = 0, sourceVerifyNs = 0;
    std::uint64_t hashedBytes = 0, ownedAssetsNs = 0, catalogPrepareNs = 0;
    std::uint64_t catalogWorkerFinishedNs = 0, rebaseStartedNs = 0, rebaseNs = 0;
    std::uint64_t pickingNs = 0, rosterQueueNs = 0, submittedNs = 0, finishedNs = 0;
    std::uint64_t additions = 0, removals = 0, edits = 0, rosterTicket = 0;
    bool verifiedArchiveReused = false, assetsChanged = false, accepted = false;
    bool resourceReaderReused = false; // 完整来源配置相同且专用reader独占转移；仍逐项重读/前后Verify。
    std::string diagnosticCode;
    WorldPathingHelperTimings helpers;
    Render::AnimationInstanceDeltaTimings catalogDetail, rebaseDetail;
    Render::AnimationFrameTimings firstFrameDetail;
    std::uint64_t prefetchRequestedNs = 0, prefetchStartedNs = 0, prefetchFinishedNs = 0;
    std::string prefetchState, prefetchDiagnostic, prefetchModel;
    WorldPlacementRefreshTimings refresh;
    // 一张roster可合并多笔连续摆放，每笔保留自己输入时刻，不能只测最后一次点击。
    std::vector<WorldPlacementCommandTiming> commands;
    War3RenderEditorCompletedFrameV2 completed{};
};

} // namespace War3Map::Modules
