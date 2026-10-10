#include "AnimationPrem.h"
#include "AnimationPoseWarnings.h"
#include "War3MapCore/Hash.h"
#include <algorithm>
#include <cmath>
#include <cctype>
#include <limits>
#include <map>
#include <new>
#include <utility>

namespace War3Map::Render {
namespace {
using namespace War3RenderCore;
constexpr std::uint32_t kChildAssets = 8, kSlotsPerAsset = 32, kCapabilities = 4096;
constexpr std::uint64_t kClockMaximum = UINT32_MAX;

template<class Vector> std::uint64_t Bytes(const Vector& values) {
    if constexpr (requires { values.capacity(); }) return std::uint64_t(values.capacity()) * sizeof(values[0]);
    else return std::uint64_t(values.size()) * sizeof(values[0]);
}
std::uint64_t SceneBytes(const AnimatedModelScene& scene) {
    const auto& g = scene.geometry;
    std::uint64_t bytes = sizeof(scene) + Bytes(g.vertices) + Bytes(g.indices) + Bytes(g.layers) + Bytes(g.meshes) +
        Bytes(g.models) + Bytes(g.instances) + Bytes(g.instanceRequests) + Bytes(g.modelBounds) + Bytes(g.vertexGroups) +
        Bytes(g.meshGeosets) + Bytes(g.modelPaths) + Bytes(g.modelAssetKeys) + Bytes(g.modelTextureBindings) +
        Bytes(scene.vertices) + Bytes(scene.meshes) + Bytes(scene.meshColors) + Bytes(scene.models) + Bytes(scene.specs) +
        Bytes(scene.sequences) + Bytes(scene.pickModels) + Bytes(scene.pickMeshGroups) + Bytes(scene.pickGroupBounds) +
        Bytes(scene.drawLayerBindings) + Bytes(scene.modelLayerRanges) + Bytes(scene.standPickBounds) + Bytes(scene.sources);
    bytes += Bytes(g.textures);
    for (const auto& texture : g.textures) bytes += Bytes(texture.rgba) + texture.path.size();
    for (const auto& bindings : g.modelTextureBindings) if (bindings) bytes += Bytes(*bindings);
    for (const auto& sequences : scene.sequences) {
        bytes += Bytes(sequences);
        for (const auto& sequence : sequences) bytes += sequence.name.size();
    }
    for (const auto& spec : scene.specs) bytes += spec.hostKey.size() + spec.model.modelPath.size() + spec.action.size();
    return bytes;
}
std::uint64_t FrameBytes(const AnimationFrame& frame) {
    return sizeof(frame) + Bytes(frame.states) + Bytes(frame.instances) + Bytes(frame.groups) + Bytes(frame.alphas) +
        Bytes(frame.colors) + Bytes(frame.layerAlphas) + Bytes(frame.layerSlices) + Bytes(frame.materialStates) +
        Bytes(frame.materialStateSlices) + Bytes(frame.poses) + Bytes(frame.geosetIssues) + Bytes(frame.materialIssues) +
        Bytes(frame.collisionMatrices) + Bytes(frame.pickBounds) + Bytes(frame.diagnostics) + Bytes(frame.requestVisibility);
}
std::uint64_t CapabilityBytes(const AnimationPremCapability& item) {
    std::uint64_t bytes = sizeof(item) + item.originalPath.size() + item.resolvedPath.size() + item.rejectionCode.size() + item.rejectionMessage.size() +
        item.parentSource.virtualPath.size() + item.parentSource.sourceLayer.size() + item.parentSource.contentDigest.size() +
        item.childSource.virtualPath.size() + item.childSource.sourceLayer.size() + item.childSource.contentDigest.size();
    if (item.emitter) {
        const auto& source = item.emitter->Source();
        bytes += sizeof(Prem::Emitter) + source.path.size() + source.issueCode.size();
        for (const auto& track : source.tracks) if (track) bytes += Bytes(track->keys);
        const auto& binding = item.emitter->Binding();
        bytes += binding.parentSource.virtualPath.size() + binding.parentSource.sourceLayer.size() + binding.parentSource.contentDigest.size() +
            binding.childSource.virtualPath.size() + binding.childSource.sourceLayer.size() + binding.childSource.contentDigest.size();
    }
    return bytes;
}
bool SameSource(const AssetSource& a, const AssetSource& b) {
    return a.virtualPath == b.virtualPath && a.sourceLayer == b.sourceLayer && a.contentDigest == b.contentDigest;
}
bool MissingOnly(std::span<const Diagnostic> diagnostics) {
    bool error = false;
    for (const auto& diagnostic : diagnostics) if (diagnostic.severity == DiagnosticSeverity::Error) {
        error = true;
        if (diagnostic.code != "RESOURCE_NOT_FOUND" && diagnostic.code != "file-not-found" &&
            diagnostic.code != "WORLD_ASSET_MISSING") return false;
    }
    return error;
}
std::string FirstCode(std::span<const Diagnostic> diagnostics, const char* fallback) {
    for (const auto& diagnostic : diagnostics) if (diagnostic.severity == DiagnosticSeverity::Error) return diagnostic.code;
    return fallback;
}
std::string Lower(std::string value) {
    for (auto& character : value) character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    return value;
}
Status VerifyResource(const AnimationResource& resource, std::uint64_t maximum) {
    if (resource.bytes.size() > maximum || resource.sourceLayer.empty() || resource.sourceLayer.size() > 256 ||
        resource.digest.size() != 64 || !std::all_of(resource.digest.begin(), resource.digest.end(),
            [](unsigned char value) { return std::isxdigit(value) != 0; }))
        return AnimationFailure("PREM_CHILD_SOURCE_INVALID", "子资源缺少实际来源、SHA 或超过读取预算。");
    auto sha = Sha256(resource.bytes);
    if (!sha.Succeeded()) return {std::move(sha.diagnostics)};
    if (Lower(*sha.value) != Lower(resource.digest))
        return AnimationFailure("PREM_CHILD_SOURCE_SHA_MISMATCH", "子资源 SHA 与本次读取的原字节不符，未消费旧缓存。");
    return Status::Ok();
}
struct ChildCandidate {
    AnimationPremChildAsset child;
};
void Reject(AnimationPremCapability& capability, std::string code, std::string message) {
    capability.prepared = false; capability.emitter.reset();
    capability.rejectionCode = std::move(code); capability.rejectionMessage = std::move(message);
}
void ClearChildBudget(AnimationPremBudgetBreakdown& budget) {
    budget.childSourceBytes = budget.childResourceBytes = budget.childSceneBytes = 0;
    budget.childStandFrameBytes = budget.childFrameLayoutBytes = budget.childFrameChargedBytes = 0;
    budget.childEvaluatorBytes = budget.birthEvaluatorBytes = 0;
}
bool Recursive(const MdxModel& model) {
    if (!model.premEmitters.empty()) return true;
    bool recursive = false;
    ForEachMdxNode(model, [&](const MdxNode& node) {
        recursive |= node.kind == MdxNodeKind::ParticleEmitter || node.kind == MdxNodeKind::ParticleEmitter2 || node.kind == MdxNodeKind::Ribbon;
    });
    return recursive;
}
bool ResetClock(const AnimationAdvance& advance, InstanceIdentity identity) {
    return std::any_of(advance.edits.begin(), advance.edits.end(), [&](const auto& edit) {
        return edit.identity == identity && (edit.sequenceIndex || edit.elapsedMs || edit.globalMs);
    });
}
bool TailPaused(const AnimationAdvance& advance, InstanceIdentity identity, bool previous) {
    if (advance.playAll) return !*advance.playAll;
    for (const auto& edit : advance.edits) if (edit.identity == identity && edit.playing) return !*edit.playing;
    return previous;
}
std::uint64_t AddClock(std::uint64_t time, std::uint64_t delta) { return std::min(kClockMaximum, time + std::min(delta, kClockMaximum)); }

AnimationPremRequestClock ClockFor(const AnimationPremHost& host, const AnimatedModelScene& scene,
    const AnimationFrame& frame, std::uint32_t requestIndex, AssetIdentity asset,
    const AnimationFrame* previous, const AnimationAdvance& advance) {
    const auto& state = frame.states[requestIndex];
    AnimationPremRequestClock clock{state.identity, asset, requestIndex, state.sequenceIndex,
        state.elapsedMs, state.globalMs, state.elapsedMs, state.globalMs, false};
    if (!previous || !previous->prem || ResetClock(advance, state.identity) || state.sequenceIndex >= scene.sequences[requestIndex].size()) return clock;
    const auto& checkpoint = *previous->prem;
    if (checkpoint.parentSceneVersion != scene.sceneVersion || checkpoint.parentDocumentRevision != scene.documentRevision ||
        checkpoint.parentGeneration != scene.generation || checkpoint.parentSourceIdentity != scene.sourceIdentity ||
        checkpoint.parentEvaluationEpoch != host.epoch || checkpoint.parentContentVersion != previous->contentVersion) return clock;
    const auto old = std::lower_bound(checkpoint.requestClocks.begin(), checkpoint.requestClocks.end(), requestIndex,
        [](const auto& candidate, auto index) { return candidate.requestIndex < index; });
    if (old == checkpoint.requestClocks.end() || old->requestIndex != requestIndex || old->identity != state.identity ||
        old->asset != asset || old->sequenceIndex != state.sequenceIndex || requestIndex >= previous->states.size() ||
        previous->states[requestIndex].identity != state.identity || old->parentElapsedMs != previous->states[requestIndex].elapsedMs ||
        old->parentGlobalMs != previous->states[requestIndex].globalMs || old->elapsedMs > kClockMaximum || old->globalMs > kClockMaximum ||
        !std::isfinite(old->tailFractionalMs) || old->tailFractionalMs < 0 || old->tailFractionalMs >= 1) return clock;
    const auto& sequence = scene.sequences[requestIndex][state.sequenceIndex];
    if (sequence.flags != 1 || sequence.end < sequence.start) return clock;
    const auto duration = std::uint64_t(sequence.end) - sequence.start;
    // 播放命令delta=0会先把playing改回true但尚未再次clamp；可信已有tail不可退回动作终点。
    if (state.elapsedMs != duration || (state.playing && !old->tailAging) ||
        (!old->tailAging && !previous->states[requestIndex].playing)) return clock;
    clock.tailAging = true;
    clock.tailPaused = TailPaused(advance, state.identity, old->tailPaused);
    const auto fractional = old->tailAging ? old->tailFractionalMs : previous->states[requestIndex].fractionalMs;
    const auto scaled = static_cast<double>(advance.deltaMs) * state.speed + fractional;
    const auto delta = (!clock.tailPaused || advance.stepPaused) && std::isfinite(scaled) && scaled >= 0
        ? static_cast<std::uint64_t>(std::min(scaled, static_cast<double>(kClockMaximum))) : 0;
    clock.tailFractionalMs = (!clock.tailPaused || advance.stepPaused) && std::isfinite(scaled) && scaled >= 0
        ? scaled - static_cast<double>(delta) : fractional;
    clock.elapsedMs = std::max(duration, AddClock(old->elapsedMs, delta));
    clock.globalMs = AddClock(old->globalMs, delta);
    return clock;
}
}

Status CreateAnimationPrem(ModelAnimationService::State& state, const AnimationResourceReader& read) {
    state.prem.reset();
    if (!state.scene) return AnimationFailure("PREM_HOST_IDENTITY", "PREM准备没有当前场景。");
    state.scene->premGeometry.reset();
    if (!state.limits.enablePremPreview) return Status::Ok();
    if (!read || !state.scene || !state.epoch || state.requestAssets.size() != state.scene->specs.size())
        return AnimationFailure("PREM_HOST_IDENTITY", "PREM 准备需要本次场景、冻结reader与非零求值代次。");
    try {
        auto host = std::make_shared<AnimationPremHost>(); host->epoch = state.epoch;
        host->fixedFrameBytes = state.frameBytes;
        host->budget.parentFixedFrameBytes = state.frameBytes;
        host->assetCapabilities.resize(state.assets.size());
        std::vector<bool> referenced(state.assets.size(), false);
        for (const auto index : state.requestAssets) {
            if (index >= state.assets.size()) return AnimationFailure("PREM_PARENT_ASSET_STALE", "PREM父请求不属于当前实际资产表。");
            referenced[index] = true;
        }
        auto capabilities = std::make_shared<std::vector<AnimationPremCapability>>();
        std::vector<ChildCandidate> candidates;
        std::map<std::string, std::shared_ptr<const AnimationResource>> resources;
        std::uint64_t rawBytes = 0;
        std::uint64_t emitterBytes = 0;
        const auto resident = state.resourceBytes + state.sourceBytes;
        host->budget.parentAssetBytesBefore = resident;
        const auto remainingAssets = resident < state.limits.maxAssetBytes ? state.limits.maxAssetBytes - resident : 0;
        const auto remainingFrames = state.frameBytes < state.limits.maxFrameBytes ? state.limits.maxFrameBytes - state.frameBytes : 0;
        const auto childLimits = [&] {
            auto limits = state.limits;
            limits.enableParticles = false; limits.enablePremPreview = false; limits.retainUnsupportedStatic = false;
            limits.allowSourceOrderNodePreview = false; limits.allowIdenticalMaterialAlphaKeysPreview = false;
            limits.allowRigidStaticPreview = false;
            limits.playbackPolicy = AnimationPlaybackPolicy::StrictReplay;
            limits.maxAssetBytes = remainingAssets / 2; limits.maxFrameBytes = remainingFrames / 3;
            limits.maxActivePoseBytes = std::min(limits.maxActivePoseBytes, limits.maxFrameBytes);
            return limits;
        };
        // 一次独立reader调用也验证缓存的当前实际层/SHA；准备结束后不持有reader/归档句柄。
        const auto verifiedRead = [&](std::string_view path, std::uint64_t maximum) -> Result<AnimationResource> {
            // 外层FrozenAssetReader即使不记录accesses仍会增加父缓存；每次按最新父账收紧额度。
            const auto currentResident = state.resourceBytes + state.sourceBytes;
            const auto available = currentResident < state.limits.maxAssetBytes ? state.limits.maxAssetBytes - currentResident : 0;
            const auto readLimit = std::min(maximum, available);
            auto resource = read(path, readLimit);
            if (!resource.Succeeded()) return resource;
            if (auto status = VerifyResource(*resource.value, readLimit); !status.Succeeded())
                return {std::nullopt, std::move(status.diagnostics)};
            const auto key = AnimationPathKey(path);
            const auto found = resources.find(key);
            if (found != resources.end()) {
                if (found->second->sourceLayer != resource.value->sourceLayer ||
                    Lower(found->second->digest) != Lower(resource.value->digest) || found->second->bytes != resource.value->bytes)
                    return Result<AnimationResource>::Failure("Render.Animation", "PREM_CHILD_SOURCE_CHANGED", "同次准备中子资源来源或字节改变。");
            } else {
                if (resources.size() == 256 || rawBytes > remainingAssets / 2 ||
                    resource.value->bytes.size() > remainingAssets / 2 - rawBytes)
                    return Result<AnimationResource>::Failure("Render.Animation", "PREM_CHILD_ASSET_BUDGET", "子模型和贴图冻结/工作副本超过原资产预算。");
                rawBytes += resource.value->bytes.size();
                resources.emplace(key, std::make_shared<const AnimationResource>(*resource.value));
            }
            return resource;
        };
        for (std::uint32_t assetIndex = 0; assetIndex < state.assets.size(); ++assetIndex) {
            if (!referenced[assetIndex]) continue; // retired catalog保留原编号，但不读child或占八资产名额。
            const auto& asset = state.assets[assetIndex];
            if (!asset.parsed) continue;
            if (asset.parsed->premEmitters.size() > kCapabilities - capabilities->size())
                return AnimationFailure("PREM_CAPABILITY_LIMIT", "PREM typed 来源回执超过有界4096条，未截断为假完整列表。");
            for (const auto& source : asset.parsed->premEmitters) {
                AnimationPremCapability capability;
                capability.parentAssetIndex = assetIndex; capability.parentAsset = asset.compiled.Identity();
                capability.parentSource = asset.source; capability.sourceIndex = source.sourceIndex;
                capability.nodeId = source.nodeId; capability.otherNodeIndex = source.otherNodeIndex;
                capability.sourceByteOffset = source.recordByteOffset; capability.originalPath = source.path;
                const auto finish = [&] {
                    host->assetCapabilities[assetIndex].push_back(static_cast<std::uint32_t>(capabilities->size()));
                    capabilities->push_back(std::move(capability));
                };
                if (!asset.compiled.Valid() || source.support != MdxPremSupport::Supported) {
                    Reject(capability, source.issueCode.empty() ? "PREM_PARENT_UNSUPPORTED" : source.issueCode,
                        "父发射器缺少可信compiled节点或 typed 结构不受支持。"); finish(); continue;
                }
                for (std::uint32_t node = 0; node < asset.compiled.NodeCount(); ++node)
                    if (asset.compiled.Node(node).objectId == source.nodeId) { capability.compiledNodeIndex = node; break; }
                if (capability.compiledNodeIndex == UINT32_MAX) {
                    Reject(capability, "PREM_NODE_MAPPING_INVALID", "真实compiled父节点不存在。"); finish(); continue;
                }
                std::uint64_t ownerBytes = sizeof(Prem::Emitter) + source.path.size() + source.issueCode.size() + 4096;
                for (const auto& track : source.tracks) if (track) ownerBytes += track->keys.size() * sizeof(MdxPremScalarKey);
                if (emitterBytes > remainingFrames / 3 || 2 * ownerBytes > remainingFrames / 3 - emitterBytes) {
                    Reject(capability, "PREM_BINDING_BUDGET", "发射器typed轨迹所有者及准备副本超过原帧工作预算。"); finish(); continue;
                }
                // 先用同一合同核 typed 参数，未合资格的源不占用有限 child 资产名额。
                auto qualified = Prem::CreateEmitter(asset.parsed, source.sourceIndex, capability.compiledNodeIndex, 0,
                    {asset.compiled.Identity(), asset.source, asset.source});
                if (!qualified.Succeeded()) {
                    Reject(capability, FirstCode(qualified.diagnostics, "PREM_SOURCE_UNSUPPORTED"), "typed发射参数或公共节点不符合基础预览合同。");
                    finish(); continue;
                }
                capability.resolvedPath = source.path;
                auto resource = verifiedRead(capability.resolvedPath, state.limits.geometry.maxModelBytes);
                const auto key = AnimationPathKey(capability.resolvedPath);
                if (!resource.Succeeded() && MissingOnly(resource.diagnostics) && key.ends_with(".mdl")) {
                    capability.resolvedPath.replace(capability.resolvedPath.size() - 4, 4, ".mdx");
                    resource = verifiedRead(capability.resolvedPath, state.limits.geometry.maxModelBytes);
                }
                if (!resource.Succeeded()) {
                    Reject(capability, FirstCode(resource.diagnostics, "PREM_CHILD_READ_FAILED"), "原子路径/许可别名读取失败，未换更低来源或猜资产。"); finish(); continue;
                }
                capability.childSource = {AnimationPathKey(capability.resolvedPath), state.scene->sourceIdentity + "|" + resource.value->sourceLayer,
                    Lower(resource.value->digest)};
                const auto existing = std::find_if(candidates.begin(), candidates.end(), [&](const auto& child) {
                    return SameSource(child.child.source, capability.childSource);
                });
                std::uint32_t childIndex = static_cast<std::uint32_t>(existing - candidates.begin());
                if (existing == candidates.end()) {
                    if (candidates.size() == kChildAssets) {
                        Reject(capability, "PREM_CHILD_ASSET_LIMIT", "首版最多8个来源绑定子资产，每资产32槽，未扩大主预算。"); finish(); continue;
                    }
                    auto parsed = ParseMdx(resource.value->bytes);
                    if (!parsed.Succeeded()) {
                        Reject(capability, FirstCode(parsed.diagnostics, "PREM_CHILD_PARSE_FAILED"), "命中的子模型格式无效；不对坏上层路径应用mdx别名。"); finish(); continue;
                    }
                    if (Recursive(*parsed.value)) {
                        Reject(capability, "PREM_CHILD_RECURSIVE_UNSUPPORTED", "子模型有PREM/PRE2/RIBB，首版拒绝递归发射。"); finish(); continue;
                    }
                    const auto stand = FindAnimationSequence(parsed.value->sequences, "Stand");
                    if (!stand) {
                        Reject(capability, "PREM_CHILD_STAND_UNSUPPORTED", "子模型没有真实Stand动作，不用bind pose代替。"); finish(); continue;
                    }
                    // 每资产先独立准入；一个坏子模型不会使其它已准入子模型的pool失败。
                    AnimationInstanceSpec probe;
                    probe.identity = {1, state.scene->generation}; probe.hostKey = "prem-child-admission";
                    probe.model.modelPath = capability.resolvedPath; probe.sequenceIndex = *stand;
                    probe.elapsedMs = 0; probe.globalMs = 0; probe.playing = false;
                    auto service = ModelAnimationService::Create({state.scene->generation, state.scene->documentRevision, state.scene->sourceIdentity},
                        std::span(&probe, 1), verifiedRead, childLimits());
                    if (!service.Succeeded()) {
                        Reject(capability, FirstCode(service.diagnostics, "PREM_CHILD_SERVICE_FAILED"), "该子资产独立Stand服务未通过；其它子资产不受影响。");
                        finish(); continue;
                    }
                    auto sampled = (*service.value)->Prepare(nullptr);
                    if (!sampled.Succeeded()) {
                        Reject(capability, FirstCode(sampled.diagnostics, "PREM_CHILD_STAND_FAILED"), "该子资产真实Stand0ms未通过。");
                        finish(); continue;
                    }
                    const auto& sample = **sampled.value;
                    if (sample.sharedPoseRanges || sample.poses.size() != 1 || !sample.poses[0].animated || sample.poses[0].hidden ||
                        sample.poses[0].outsideView || sample.poses[0].staticForBudget || sample.poses[0].noVisibleGeometry ||
                        !sample.poses[0].poseVersion || (*service.value)->Scene()->geometry.untexturedLayers) {
                        Reject(capability, "PREM_CHILD_STAND_NOT_VISIBLE", "该子资产真实Stand0ms缺少完整几何/贴图/独立姿势。");
                        finish(); continue;
                    }
                    candidates.push_back({{capability.resolvedPath, capability.childSource, *stand, childIndex * kSlotsPerAsset}});
                }
                capability.childAssetIndex = childIndex;
                auto emitter = Prem::CreateEmitter(asset.parsed, source.sourceIndex, capability.compiledNodeIndex, childIndex,
                    {asset.compiled.Identity(), asset.source, capability.childSource});
                if (!emitter.Succeeded()) {
                    Reject(capability, FirstCode(emitter.diagnostics, "PREM_SOURCE_UNSUPPORTED"), "typed发射参数或公共节点不符合基础预览合同。");
                } else { capability.emitter = std::move(*emitter.value); emitterBytes += ownerBytes; }
                finish();
            }
        }
        if (capabilities->empty()) return Status::Ok(); // 无PREM不安装额外帧/资产工作区。
        host->capabilities = capabilities;
        if (!candidates.empty()) {
            std::vector<AnimationInstanceSpec> poolSpecs;
            for (const auto& candidate : candidates) {
                host->children.push_back(candidate.child);
                for (std::uint32_t slot = 0; slot < kSlotsPerAsset; ++slot) {
                    AnimationInstanceSpec spec; spec.identity = {poolSpecs.size() + 1, state.scene->generation};
                    spec.hostKey = "prem-pool:" + std::to_string(poolSpecs.size()); spec.model.modelPath = candidate.child.path;
                    spec.sequenceIndex = candidate.child.sequenceIndex; spec.elapsedMs = 0; spec.globalMs = 0; spec.playing = false;
                    poolSpecs.push_back(std::move(spec));
                }
            }
            auto limits = childLimits();
            auto child = ModelAnimationService::Create({state.scene->generation, state.scene->documentRevision, state.scene->sourceIdentity},
                poolSpecs, verifiedRead, limits);
            if (child.Succeeded()) {
                host->childService = std::move(*child.value);
                auto stand = host->childService->Prepare(nullptr);
                if (stand.Succeeded() && !(*stand.value)->sharedPoseRanges && (*stand.value)->poses.size() == poolSpecs.size() &&
                    !host->childService->Scene()->geometry.untexturedLayers) host->childStand = std::move(*stand.value);
                else for (auto& capability : *capabilities) if (capability.emitter)
                    Reject(capability, stand.Succeeded() ? "PREM_CHILD_STAND_NOT_VISIBLE" : FirstCode(stand.diagnostics, "PREM_CHILD_STAND_FAILED"),
                        "子模型真实Stand0ms或独立槽位准备失败，未静态填值/白纹理代替。");
            } else for (auto& capability : *capabilities) if (capability.emitter)
                Reject(capability, FirstCode(child.diagnostics, "PREM_CHILD_SERVICE_FAILED"), "子资产独立动画服务准备失败，父几何保留。");
            if (host->childStand) {
                for (auto& capability : *capabilities) if (capability.emitter) {
                    const auto firstSlot = host->children[capability.childAssetIndex].firstSlot;
                    const auto pose = std::find_if(host->childStand->poses.begin(), host->childStand->poses.end(),
                        [&](const auto& item) { return item.requestIndex == firstSlot; });
                    if (pose == host->childStand->poses.end() || !pose->animated || pose->hidden || pose->outsideView ||
                        pose->staticForBudget || pose->noVisibleGeometry || !pose->poseVersion)
                        Reject(capability, "PREM_CHILD_STAND_NOT_VISIBLE", "子模型真实Stand0ms没有可显示完整姿势。");
                    else capability.prepared = true;
                }
                const auto& childState = *host->childService->state_;
                auto& budget = host->budget;
                budget.childSourceBytes = childState.sourceBytes;
                budget.childResourceBytes = childState.resourceBytes;
                budget.childSceneBytes = SceneBytes(*host->childService->Scene());
                budget.childStandFrameBytes = FrameBytes(*host->childStand);
                budget.childEvaluatorBytes = childState.evaluator ? childState.evaluator->StorageBytes() : 0;
                const bool chargedWorkspace = childState.collisionNodesRequired || childState.sharedPoseRanges ||
                    host->childService->Scene()->dynamicMaterialAlphas;
                const auto includedWorkspace = chargedWorkspace ? budget.childEvaluatorBytes : 0;
                if (includedWorkspace > childState.frameBytes)
                    return AnimationFailure("PREM_CHILD_BUDGET_LAYOUT", "子服务实际workspace与正式frame账不一致，未重复或漏算。");
                budget.childFrameLayoutBytes = childState.frameBytes - includedWorkspace;
                budget.childFrameChargedBytes = std::max(budget.childStandFrameBytes, budget.childFrameLayoutBytes);
                host->extraAssetBytes = budget.childSourceBytes + budget.childResourceBytes + budget.childSceneBytes;
                host->storageBytes += budget.childSceneBytes + budget.childFrameChargedBytes + budget.childEvaluatorBytes;
                host->workingBytes += 3 * budget.childFrameChargedBytes + Bytes(poolSpecs) + poolSpecs.size() * sizeof(AnimationEdit);
                state.scene->premGeometry = host->childService->Scene();
            }
            else { host->childService.reset(); host->children.clear(); }
        }
        for (const auto& capability : *capabilities) host->storageBytes += CapabilityBytes(capability);
        host->storageBytes += sizeof(AnimationPremHost) + Bytes(host->assetCapabilities) + Bytes(host->children);
        for (const auto& indices : host->assetCapabilities) host->storageBytes += Bytes(indices);
        const bool prepared = std::any_of(capabilities->begin(), capabilities->end(), [](const auto& capability) { return capability.prepared; });
        if (prepared) {
            std::vector<CompiledModel> compiled;
            host->birthAssetIndices.assign(state.assets.size(), UINT32_MAX);
            host->storageBytes += Bytes(host->birthAssetIndices);
            BatchLimits limits; limits.maxInstances = 1; limits.maxNodes = limits.maxGroups = limits.maxGeosets = 1;
            // 外层预算可能大于核心单采样器硬帽；只收紧上限，不让合法的大场景被参数门误拒绝。
            limits.maxBytes = std::min(BatchLimits{}.maxBytes, remainingFrames / 3);
            limits.outputMode = PoseOutputMode::FullNodes;
            for (std::uint32_t index = 0; index < state.assets.size(); ++index) {
                const bool used = std::any_of(host->assetCapabilities[index].begin(), host->assetCapabilities[index].end(),
                    [&](const auto capability) { return (*capabilities)[capability].prepared; });
                const auto& asset = state.assets[index];
                if (!referenced[index] || !used || !asset.compiled.Valid()) continue;
                host->birthAssetIndices[index] = static_cast<std::uint32_t>(compiled.size());
                compiled.push_back(asset.compiled);
                limits.maxNodes = std::max(limits.maxNodes, asset.compiled.NodeCount());
                limits.maxGroups = std::max(limits.maxGroups, asset.compiled.GroupCount());
                limits.maxGeosets = std::max(limits.maxGeosets, asset.compiled.GeosetCount());
            }
            const auto geosets = state.limits.retainUnsupportedStatic ? GeosetFailurePolicy::PreviewHideInvalidGeosets : GeosetFailurePolicy::Strict;
            auto evaluator = PoseBatchEvaluator::Create(compiled, host->epoch, limits, geosets, {}, {}, {}, AnimationNodeKeyPolicy(state.limits));
            if (!evaluator.Succeeded()) {
                for (auto& capability : *capabilities) if (capability.prepared)
                    Reject(capability, FirstCode(evaluator.diagnostics, "PREM_ANCHOR_PREWARM_FAILED"), "父节点独立出生采样器未通过原工作预算。");
            } else {
                host->evaluator = std::make_unique<PoseBatchEvaluator>(std::move(*evaluator.value));
                host->budget.birthEvaluatorBytes = host->evaluator->StorageBytes();
                host->storageBytes += host->budget.birthEvaluatorBytes;
            }
        }
        const bool active = host->evaluator && std::any_of(capabilities->begin(), capabilities->end(),
            [](const auto& capability) { return capability.prepared; });
        if (!active) {
            host->childService.reset(); host->childStand.reset(); host->children.clear();
            host->evaluator.reset(); std::vector<std::uint32_t>{}.swap(host->birthAssetIndices);
            state.scene->premGeometry.reset(); host->extraAssetBytes = 0;
            host->storageBytes = sizeof(AnimationPremHost) + Bytes(host->assetCapabilities) + Bytes(host->children);
            for (const auto& indices : host->assetCapabilities) host->storageBytes += Bytes(indices);
            for (const auto& capability : *capabilities) host->storageBytes += CapabilityBytes(capability);
            host->workingBytes = sizeof(AnimationPremFrame);
            ClearChildBudget(host->budget);
        }
        host->budget.parentAssetBytesAfter = state.resourceBytes + state.sourceBytes;
        host->budget.preparationResourceBytes = rawBytes;
        const Prem::Limits premLimits;
        // Prem::Build 的固定硬帽暂存、候选帧/前帧、child工作帧均有界，不提升父 budget。
        if (active) host->workingBytes += 2 * premLimits.maxFrameBytes + sizeof(AnimationPremFrame) +
            2ull * state.scene->specs.size() * sizeof(AnimationPremRequestClock) +
            premLimits.maxRequests * sizeof(Prem::Request) + premLimits.maxBirths * (sizeof(AnimationPremSampleError) + 128) +
            premLimits.maxLiveChildren * sizeof(AnimationPremSlotBinding);
        const auto total = host->storageBytes + host->workingBytes;
        const auto finalResident = host->budget.parentAssetBytesAfter;
        if (finalResident > state.limits.maxAssetBytes || host->extraAssetBytes > state.limits.maxAssetBytes - finalResident || total > remainingFrames) {
            for (auto& capability : *capabilities) if (capability.emitter || capability.prepared)
                Reject(capability, "PREM_HOST_BUDGET", "实际子几何、Stand/工作帧、出生节点workspace超过原资产/帧预算。");
            host->childService.reset(); host->childStand.reset(); host->evaluator.reset(); state.scene->premGeometry.reset();
            host->children.clear();
            std::vector<std::uint32_t>{}.swap(host->birthAssetIndices);
            host->extraAssetBytes = 0;
            ClearChildBudget(host->budget);
            host->storageBytes = sizeof(AnimationPremHost) + Bytes(host->assetCapabilities) + Bytes(host->children);
            for (const auto& indices : host->assetCapabilities) host->storageBytes += Bytes(indices);
            for (const auto& child : host->children) host->storageBytes += child.path.size() + child.source.virtualPath.size() +
                child.source.sourceLayer.size() + child.source.contentDigest.size();
            for (const auto& capability : *capabilities) host->storageBytes += CapabilityBytes(capability);
            host->workingBytes = sizeof(AnimationPremFrame);
            if (host->storageBytes + host->workingBytes > remainingFrames)
                return AnimationFailure("PREM_HOST_BUDGET", "PREM来源拒绝回执本身超出原帧预算，未发布无界列表。");
        }
        state.frameBytes += host->storageBytes + host->workingBytes;
        // 父Frozen reader新增缓存已进入最新parent账；child独立State账/scene另收费，不回写父计数。
        // workspace按实际StorageBytes单列，含workspace的childState.frameBytes不再重复整体收费。
        state.prem = std::move(host);
        return Status::Ok();
    } catch (const std::bad_alloc&) {
        state.scene->premGeometry.reset();
        return AnimationFailure("PREM_HOST_ALLOCATION", "PREM准备内存分配失败，未安装部分宿主。");
    }
}

Status PrepareAnimationPrem(ModelAnimationService::State& state, AnimationFrame& frame,
    const AnimationFrame* previous, const AnimationAdvance& advance) {
    frame.prem.reset();
    if (!state.prem || !state.limits.enablePremPreview) return Status::Ok();
    const auto& host = *state.prem; const auto& scene = *state.scene;
    if (frame.sceneVersion != scene.sceneVersion || frame.documentRevision != scene.documentRevision || !frame.contentVersion ||
        frame.states.size() != scene.specs.size() || host.epoch != state.epoch)
        return AnimationFailure("PREM_FRAME_STALE", "PREM候选帧不属于当前父场景、修订或求值代次。");
    try {
        auto output = std::make_shared<AnimationPremFrame>();
        output->parentSceneVersion = scene.sceneVersion; output->parentContentVersion = frame.contentVersion;
        output->parentDocumentRevision = scene.documentRevision; output->parentGeneration = scene.generation;
        output->parentEvaluationEpoch = host.epoch; output->parentSourceIdentity = scene.sourceIdentity;
        output->capabilities = host.capabilities; output->storageBytes = host.storageBytes;
        output->workingBytes = host.workingBytes; output->assetBytes = host.extraAssetBytes;
        output->budget = host.budget;
        for (std::uint32_t r = 0; r < state.requestAssets.size(); ++r) {
            const auto a = state.requestAssets[r];
            if (a >= host.assetCapabilities.size()) continue;
            for (const auto index : host.assetCapabilities[a]) {
                if ((*host.capabilities)[index].prepared) ++output->supportedEmitterInstances;
                else ++output->unsupportedEmitterInstances;
            }
        }
        if (!host.evaluator || !host.childStand || !host.childService) { frame.prem = std::move(output); return Status::Ok(); }
        // lifespan checkpoint 属于真实父请求，不属于当帧可见/动态槽集合。
        // 离屏、临时隐藏或预算Stand仍连续保留尾时钟和暂停，重入不能把Death重置到终点复活碎块。
        for (std::uint32_t r = 0; r < state.requestAssets.size(); ++r) {
            const auto a = state.requestAssets[r];
            if (a >= state.assets.size() || a >= host.assetCapabilities.size() || r >= scene.sequences.size() ||
                frame.states[r].identity != scene.specs[r].identity || !state.assets[a].compiled.Valid() ||
                frame.states[r].sequenceIndex >= scene.sequences[r].size()) continue;
            bool relevant = false;
            for (const auto index : host.assetCapabilities[a]) {
                const auto& capability = (*host.capabilities)[index];
                relevant |= capability.prepared && capability.parentAsset == state.assets[a].compiled.Identity() &&
                    SameSource(capability.parentSource, state.assets[a].source);
            }
            if (relevant) output->requestClocks.push_back(ClockFor(host, scene, frame, r,
                state.assets[a].compiled.Identity(), previous, advance));
        }
        Prem::Limits limits;
        std::vector<Prem::Request> requests; requests.reserve(limits.maxRequests);
        for (const auto& pose : frame.poses) {
            const auto r = pose.requestIndex;
            if (r >= frame.states.size() || r >= state.requestAssets.size() || !pose.animated || pose.hidden || pose.outsideView ||
                pose.staticForBudget || !pose.poseVersion || pose.evaluationEpoch != host.epoch) continue;
            const auto a = state.requestAssets[r];
            if (a >= host.assetCapabilities.size() || a >= state.assets.size() || pose.identity != frame.states[r].identity ||
                pose.asset != state.assets[a].compiled.Identity()) continue;
            const auto clockRow = std::lower_bound(output->requestClocks.begin(), output->requestClocks.end(), r,
                [](const auto& clock, auto requestIndex) { return clock.requestIndex < requestIndex; });
            if (clockRow == output->requestClocks.end() || clockRow->requestIndex != r || clockRow->asset != pose.asset) continue;
            const auto& clock = *clockRow;
            for (const auto index : host.assetCapabilities[a]) {
                const auto& capability = (*host.capabilities)[index]; if (!capability.prepared) continue;
                if (requests.size() == limits.maxRequests) { ++output->requestBudgetOmissions; continue; }
                requests.push_back({capability.emitter, pose.identity, r, clock.sequenceIndex,
                    clock.elapsedMs, clock.globalMs, frame.states[r].transform});
            }
        }
        std::sort(output->requestClocks.begin(), output->requestClocks.end(), [](const auto& a, const auto& b) { return a.requestIndex < b.requestIndex; });
        const auto sampler = [&](const Prem::Request& request, std::uint64_t born, std::uint64_t global) -> Result<NodePose> {
            const auto reject = [&](const char* code, PoseDiagnostic diagnostic = {}) -> Result<NodePose> {
                if (output->sampleErrors.size() < limits.maxBirths) output->sampleErrors.push_back({request.identity,
                    request.emitter->Binding().parentAsset, request.requestIndex, request.emitter->Source().sourceIndex,
                    born, global, code, diagnostic});
                return Result<NodePose>::Failure("Render.Animation", code, "PREM出生节点的实际核心求值未通过，不猜轴心/原点。");
            };
            if (request.requestIndex >= state.requestAssets.size() || request.requestIndex >= frame.states.size() ||
                request.identity != frame.states[request.requestIndex].identity) return reject("PREM_BIRTH_OWNER_STALE");
            const auto a = state.requestAssets[request.requestIndex];
            if (a >= state.assets.size()) return reject("PREM_BIRTH_ASSET_STALE");
            if (a >= host.birthAssetIndices.size() || host.birthAssetIndices[a] == UINT32_MAX)
                return reject("PREM_BIRTH_ASSET_MAPPING_STALE");
            const auto& asset = state.assets[a]; const auto& binding = request.emitter->Binding();
            if (!asset.compiled.Valid() || binding.parentAsset != asset.compiled.Identity() || !SameSource(binding.parentSource, asset.source) ||
                request.emitter->CompiledNodeIndex() >= asset.compiled.NodeCount() ||
                asset.compiled.Node(request.emitter->CompiledNodeIndex()).objectId != request.emitter->Source().nodeId)
                return reject("PREM_BIRTH_SOURCE_STALE");
            PoseRequest pose; pose.assetIndex = host.birthAssetIndices[a]; pose.asset = binding.parentAsset; pose.instance = request.identity;
            pose.sequenceIndex = request.sequenceIndex; pose.sequenceElapsedMs = born; pose.globalElapsedMs = global;
            auto worldCamera = advance.worldCamera;
            if (!worldCamera && advance.worldView) worldCamera = AnimationWorldCamera(advance.worldView->camera);
            if (worldCamera) {
                auto local = ToAnimationLocalCamera(*worldCamera, request.parentTransform);
                if (!local.Succeeded()) return reject("PREM_BIRTH_CAMERA_INVALID");
                pose.camera = *local.value;
            }
            const auto outcome = host.evaluator->Evaluate(std::span(&pose, 1));
            const auto receipts = host.evaluator->Receipts();
            if (outcome.error != Code::Ok || receipts.size() != 1) return reject("PREM_BIRTH_BATCH_REJECTED", {outcome.error});
            const auto& receipt = receipts[0];
            if (receipt.error != Code::Ok) return reject(CodeName(receipt.error), FirstAnimationPoseError(host.evaluator->Diagnostics(0), receipt.error));
            const auto nodes = host.evaluator->Nodes(0);
            if (receipt.asset != pose.asset || receipt.instance != pose.instance || receipt.evaluationEpoch != host.epoch || !receipt.poseVersion ||
                receipt.source != PoseSource::Computed || request.emitter->CompiledNodeIndex() >= nodes.size())
                return reject("PREM_BIRTH_RECEIPT_STALE");
            const auto node = nodes[request.emitter->CompiledNodeIndex()];
            if (node.objectId != request.emitter->Source().nodeId) return reject("PREM_BIRTH_NODE_STALE");
            return Result<NodePose>::Success(node); // 借用工作区的值在下一次Evaluate之前立即复制。
        };
        auto emission = Prem::Build(requests, sampler, limits);
        if (!emission.Succeeded()) return {std::move(emission.diagnostics)};
        output->emission = std::move(*emission.value);
        output->childScene = host.childService->Scene();
        if (output->childScene != scene.premGeometry || output->childScene->generation != scene.generation ||
            output->childScene->documentRevision != scene.documentRevision || output->childScene->sourceIdentity != scene.sourceIdentity)
            return AnimationFailure("PREM_CHILD_SCENE_STALE", "子层来源身份不属于同一父场景。");
        AnimationAdvance edits;
        edits.edits.resize(output->childScene->specs.size());
        std::vector<bool> filled(edits.edits.size(), false);
        for (std::uint32_t r = 0; r < edits.edits.size(); ++r) {
            auto& edit = edits.edits[r]; edit.identity = output->childScene->specs[r].identity;
            edit.transform = std::array<float, 12>{1,0,0,0,0,1,0,0,0,0,1,0}; edit.tint = std::array<float, 3>{0,0,0};
            edit.playing = false;
        }
        std::array<std::uint32_t, kChildAssets> counts{};
        for (std::uint32_t i = 0; i < output->emission.children.size(); ++i) {
            const auto& child = output->emission.children[i]; const auto a = child.emitter->ChildModelIndex();
            if (a >= host.children.size() || a >= counts.size() || counts[a] == kSlotsPerAsset) { ++output->slotSuppressedChildren; continue; }
            const auto slot = host.children[a].firstSlot + counts[a]++;
            if (slot >= edits.edits.size() || child.parentRequestIndex >= frame.states.size() ||
                child.parentIdentity != frame.states[child.parentRequestIndex].identity)
                return AnimationFailure("PREM_CHILD_SLOT_STALE", "子槽与真实父owner不匹配。");
            edits.edits[slot].transform = child.transform; edits.edits[slot].tint = frame.states[child.parentRequestIndex].tint;
            filled[slot] = true; output->childSlots.push_back({i, slot});
        }
        // 无可显示槽时Qt移除层，不为合法Stand无发射反复采样整套child pool。
        if (output->childSlots.empty()) { frame.prem = std::move(output); return Status::Ok(); }
        auto child = host.childService->Prepare(host.childStand.get(), edits);
        if (!child.Succeeded()) return {std::move(child.diagnostics)};
        auto childFrame = std::make_shared<AnimationFrame>(**child.value);
        if (childFrame->sharedPoseRanges) return AnimationFailure("PREM_CHILD_SHARED_ALPHA", "子pool必须有独立pose片段，未清共享alpha影响其它槽。");
        for (auto& pose : childFrame->poses) {
            if (pose.requestIndex >= filled.size()) return AnimationFailure("PREM_CHILD_SLOT_STALE", "子姿势owner超出固定pool。");
            if (filled[pose.requestIndex]) {
                if (!pose.animated || pose.hidden || pose.outsideView || pose.staticForBudget || pose.noVisibleGeometry || !pose.poseVersion)
                    return AnimationFailure("PREM_CHILD_POSE_FAILED", "已填子槽没有本次实际可见Stand姿势。");
                continue;
            }
            if (pose.firstAlpha > childFrame->alphas.size() || pose.alphaCount > childFrame->alphas.size() - pose.firstAlpha ||
                pose.materialAlphas.offset > childFrame->layerAlphas.size() || pose.materialAlphas.count > childFrame->layerAlphas.size() - pose.materialAlphas.offset)
                return AnimationFailure("PREM_CHILD_ALPHA_RANGE", "未填子槽的owned alpha片段越界。");
            std::fill_n(childFrame->alphas.begin() + pose.firstAlpha, pose.alphaCount, 0.0f);
            std::fill_n(childFrame->layerAlphas.begin() + pose.materialAlphas.offset, pose.materialAlphas.count, 0.0f);
            pose.alphaHash = AnimationBitsHash(childFrame->alphas.data() + pose.firstAlpha, pose.alphaCount * sizeof(float));
            if (pose.materialAlphas.count) pose.materialAlphaHash = AnimationBitsHash(childFrame->layerAlphas.data() + pose.materialAlphas.offset, pose.materialAlphas.count * sizeof(float));
        }
        output->childFrame = std::move(childFrame);
        frame.prem = std::move(output);
        return Status::Ok();
    } catch (const std::bad_alloc&) {
        return AnimationFailure("PREM_FRAME_ALLOCATION", "PREM候选帧分配失败，未发布部分子层。");
    }
}
} // namespace War3Map::Render
