#include "D3D11Scene.h"
#include <algorithm>
#include <cstdio>
#include <limits>

namespace War3Map::Render {
bool CopyEditorVisibility(const ModelScene& models, const ParticleFrameData* particles,
    const War3RenderEditorPresentationTargetV2& target, const War3RenderEditorVisibilityUpdateV2& input,
    EditorVisibility& output, const SkinPose* currentPose) {
    constexpr std::uint64_t maxDrawCalls = 262144;
    constexpr std::uint64_t maxMaterialComparisons = 16ull * 1024 * 1024;
    if (input.instanceCount != models.instances.size()
        || input.particleDrawCount != (particles ? particles->draws.size() : 0)
        || (input.instanceCount && !input.instanceVisible)
        || (input.particleDrawCount && !input.particleDrawVisible)) return false;
    EditorVisibility candidate;
    auto& status = candidate.status;
    status.mode = target.mode; status.modelEpoch = target.modelEpoch;
    status.sceneVersion = target.sceneVersion; status.contentVersion = target.contentVersion;
    status.instanceCount = input.instanceCount; status.particleDrawCount = input.particleDrawCount;
    status.presentationRevision = input.presentationRevision;
    candidate.modelRuns.resize(models.models.size());
    for (std::uint32_t i = 0; i < input.instanceCount; ++i) {
        const auto visible = input.instanceVisible[i];
        if (visible > 1) return false;
        if (!visible) continue;
        ++status.visibleInstanceCount;
        auto& runs = candidate.modelRuns[models.instances[i].model];
        if (!runs.empty() && runs.back().first + runs.back().second == i) ++runs.back().second;
        else runs.emplace_back(i, 1);
    }
    std::uint64_t drawCalls = 0;
    const auto* pose = currentPose ? currentPose : models.initialSkin.get();
    if (models.dynamicMaterialStates && (!pose || !pose->dynamicMaterialStates
        || pose->sceneVersion != target.sceneVersion || pose->contentVersion != target.contentVersion
        || pose->materialStateSlices.size() != models.instances.size()
        || models.modelLayerCounts.size() != models.models.size())) return false;
    std::vector<std::uint64_t> layerPrefix(models.meshes.size() + 1, 0);
    for (std::size_t mesh = 0; mesh < models.meshes.size(); ++mesh)
        layerPrefix[mesh + 1] = layerPrefix[mesh] + models.meshes[mesh].layerCount;
    if (models.dynamicMaterialStates) {
        std::uint64_t comparisons = 0;
        const auto rejectWork = [&](bool overflow) {
            char message[192]{};
            std::snprintf(message, sizeof(message),
                "[Render.MaterialState] MATERIAL_STATE_COMPARISON_BUDGET required=%s%llu limit=%llu\n",
                overflow ? ">" : "", static_cast<unsigned long long>(comparisons),
                static_cast<unsigned long long>(maxMaterialComparisons));
            (void)std::fputs(message, stderr); OutputDebugStringA(message); return false;
        };
        // 显式限制本帧真正可见的状态比较工作；不限制地图总实例数，不省略实例。
        for (std::size_t model = 0; model < models.models.size(); ++model) {
            const auto& range = models.models[model];
            const auto layers = layerPrefix[range.firstMesh + range.meshCount] - layerPrefix[range.firstMesh];
            for (const auto& run : candidate.modelRuns[model]) {
                if (run.second && layers > (std::numeric_limits<std::uint64_t>::max)() / run.second) {
                    comparisons = (std::numeric_limits<std::uint64_t>::max)(); return rejectWork(true);
                }
                const auto work = layers * run.second;
                if (comparisons > (std::numeric_limits<std::uint64_t>::max)() - work) {
                    comparisons = (std::numeric_limits<std::uint64_t>::max)(); return rejectWork(true);
                }
                comparisons += work;
            }
        }
        if (comparisons > maxMaterialComparisons) return rejectWork(false);
    }
    std::uint64_t visitedMeshes = 0;
    for (std::size_t model = 0; model < models.models.size(); ++model) {
        if (candidate.modelRuns[model].empty()) continue;
        const auto& range = models.models[model];
        visitedMeshes += range.meshCount;
        if (visitedMeshes > maxDrawCalls) return false;
        const auto layers = layerPrefix[range.firstMesh + range.meshCount] - layerPrefix[range.firstMesh];
        if (!models.dynamicMaterialStates) drawCalls += layers * candidate.modelRuns[model].size();
        else for (std::uint64_t layer = 0; layer < layers; ++layer) {
            for (const auto& [first, count] : candidate.modelRuns[model]) {
                const War3RenderMaterialStateV2* previous = nullptr;
                for (auto index = first; index < first + count; ++index) {
                    const auto& slice = pose->materialStateSlices[index];
                    if (slice.count != layers || slice.first > pose->materialStates.size()
                        || slice.count > pose->materialStates.size() - slice.first) return false;
                    const auto& state = pose->materialStates[slice.first + static_cast<std::size_t>(layer)];
                    if (!previous || previous->textureIndex != state.textureIndex || previous->samplerFlags != state.samplerFlags) {
                        if (++drawCalls > maxDrawCalls) return false;
                    }
                    previous = &state;
                }
            }
        }
        if (drawCalls > maxDrawCalls) return false;
    }
    if (input.particleDrawCount) {
        candidate.particleDrawVisible.assign(input.particleDrawVisible, input.particleDrawVisible + input.particleDrawCount);
        for (const auto visible : candidate.particleDrawVisible) {
            if (visible > 1) return false;
            status.visibleParticleDrawCount += visible;
        }
    }
    drawCalls += status.visibleParticleDrawCount;
    if (drawCalls > maxDrawCalls) return false;
    status.drawCallCount = static_cast<std::uint32_t>(drawCalls);
    output = std::move(candidate);
    return true;
}
HRESULT D3D11Scene::SetEditorVisibility(std::shared_ptr<const EditorVisibility> visibility) noexcept {
    if (!visibility) { ClearEditorVisibility(); return S_OK; }
    const auto& status = visibility->status;
    if (status.instanceCount != modelInstanceCount_ || visibility->modelRuns.size() != modelRanges_.size()
        || status.sceneVersion != skinSceneVersion_ || status.contentVersion != skinContentVersion_
        || (status.mode != WAR3_EDITOR_PRESENTATION_STATIC) != skinned_
        || status.particleDrawCount != particleStatus_.drawCount
        || visibility->particleDrawVisible.size() != particleStatus_.drawCount
        || (status.mode == WAR3_EDITOR_PRESENTATION_PARTICLES) != (particleStatus_.sceneVersion != 0)) return E_INVALIDARG;
    editorVisibility_ = std::move(visibility);
    return S_OK;
}
void D3D11Scene::ClearEditorVisibility() noexcept { editorVisibility_.reset(); }
} // namespace War3Map::Render
