#include "AnimationParticles.h"
#include "AnimationWorkers.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <functional>
#include <optional>
#include <utility>

namespace War3Map::Render {
using namespace War3RenderCore;
namespace {
struct CpuPhase {
    std::uint64_t& target;
    std::chrono::steady_clock::time_point begin=std::chrono::steady_clock::now();
    bool running=true;
    void Stop() noexcept {
        if (!running) return;
        target+=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now()-begin).count());
        running=false;
    }
    ~CpuPhase() { Stop(); }
};
struct Hash {
    std::uint64_t value = 14695981039346656037ull;
    template<class T> void Add(const T& v) {
        const auto* bytes = reinterpret_cast<const unsigned char*>(&v);
        for (std::size_t i = 0; i < sizeof(v); ++i) { value ^= bytes[i]; value *= 1099511628211ull; }
    }
};
std::uint64_t StateHash(const Particles::State& s) {
    Hash h; h.Add(s.tickMs); h.Add(s.timeMs); h.Add(s.instance); h.Add(s.generation); h.Add(s.sequenceIndex);
    h.Add(s.globalStartMs.value_or(0));
    if (s.boundedWorldPreview) h.Add(s.boundedWorldPreview); // 严格默认保留旧hash字节布局。
    for (const auto& e : s.emitters) {
        h.Add(e.random); h.Add(e.spawned); h.Add(e.dropped); h.Add(e.quota); h.Add(e.accumulator); h.Add(e.lastBurst);
        h.Add(static_cast<std::uint32_t>(e.particles.size())); h.Add(static_cast<std::uint32_t>(e.segments.size()));
        for (const auto& p : e.particles) { h.Add(p.position); h.Add(p.velocity); h.Add(p.age); h.Add(p.life); h.Add(p.gravity); }
        for (const auto& p : e.segments) { h.Add(p.above); h.Add(p.below); h.Add(p.age); h.Add(p.color); h.Add(p.cell); }
    }
    return h.value;
}
// 一个实例本帧粒子模拟的中间结果。交互预览分步执行：前置检查按顺序；准备、推进与几何只读共享输入、
// 只写自己的这一份，按实例并行；单独求骨架（共用求值器与整帧计数）与合并仍按实例顺序串行。
// 严格回放逐实例依次走同样的步骤，结果与并行数无关。
struct ParticleWork {
    std::size_t binding = 0;
    bool invalidSequence = false, anchorsReused = false;
    bool built = false, geometryOk = false, mappingFailed = false;
    AnimationParticleLink link;
    Particles::Timeline timeline{};
    std::optional<Result<LocalCamera>> local;
    AnimationParticleState candidate;
    std::vector<Particles::Anchor> currentAnchors;
    Status fatal, status; // fatal 让整帧失败；status 是交给实例拒绝处理的失败
    std::exception_ptr exception;
    std::uint64_t sampledElapsed = 0, sampledGlobal = 0, simulationNs = 0, geometryNs = 0;
    Particles::Counts counts;
    std::uint64_t stateHash = 0, geometryHash = 0;
    std::uint32_t budget = 0, vertexCount = 0;
    std::vector<Diagnostic> geometryDiagnostics;
    std::vector<Particles::Draw> draws;
    std::vector<War3RenderParticleVertexV2> vertices;
};
bool SameCamera(const std::optional<LocalCamera>& a, const std::optional<LocalCamera>& b) {
    return a.has_value() == b.has_value() && (!a || (a->eye == b->eye && a->up == b->up));
}
std::uint64_t StateBytes(const AnimationParticleState& state) {
    std::uint64_t bytes=sizeof(state)+state.state.emitters.capacity()*sizeof(Particles::EmitterState)
        +state.anchors.capacity()*sizeof(Particles::Anchor);
    for (const auto& emitter:state.state.emitters)
        bytes+=emitter.particles.capacity()*sizeof(Particles::Particle)+emitter.segments.capacity()*sizeof(Particles::Segment);
    return bytes;
}
std::uint32_t ExpectedQuota(const Particles::Emitter& emitter, std::uint32_t maximum) {
    double rate=std::max(0.0f,emitter.rate);
    const auto& track=emitter.tracks[static_cast<std::size_t>(Particles::Parameter::Rate)];
    for (const auto& key:track.keys) rate=std::max(rate,static_cast<double>(key.value[0]));
    // 三次轨迹可超出键值，保留完整硬配额；线性/常量才按寿命估计稳态容量。
    if (track.interpolation>1) return maximum;
    const auto demand=emitter.squirt ? rate : rate*std::max(0.0f,emitter.life)+1.0;
    if (!std::isfinite(demand) || demand>=maximum) return maximum;
    return std::min(maximum,static_cast<std::uint32_t>(std::ceil(std::max(1.0,demand))));
}
void HideModel(AnimationFrame& frame, std::uint32_t rendered, const std::string& code, const std::string& message) {
    if (rendered<frame.poses.size()) {
        auto& pose=frame.poses[rendered]; if (pose.hidden) return;
        if (pose.animated) --frame.animatedInstances; else --frame.retainedStaticInstances;
        pose.hidden = true; pose.animated = false; pose.poseVersion = 0;
        pose.matrixHash = pose.alphaHash = pose.colorHash = pose.materialAlphaHash = pose.materialStateHash = 0;
        // 原始粒子诊断保存在同身份 particleLinks；模型回执只表达该姿势未显示。
        pose.rejection = {};
        pose.rejectionCode = code; pose.rejectionMessage = message;
        std::fill_n(frame.alphas.begin() + pose.firstAlpha, pose.alphaCount, 0.0f);
        if (frame.dynamicMaterialAlphas)
            std::fill_n(frame.layerAlphas.begin()+pose.materialAlphas.offset,pose.materialAlphas.count,0.0f);
        ++frame.hiddenInstances;
    }
}
void WriteAnchor(Particles::Anchor& anchor, const War3RenderCore::NodePose& node) noexcept {
    anchor.pivot = node.pivot;
    for (std::size_t row = 0; row < 3; ++row) for (std::size_t column = 0; column < 4; ++column)
        anchor.model[row * 4 + column] = node.worldMatrix[column * 4 + row];
}
}

void BeginAnimationParticleAnchors(AnimationParticleHost& host, std::size_t requestCount,
    std::span<const AnimationAsset> assets) {
    host.requestBindings.assign(requestCount, UINT32_MAX);
    host.anchorOffsets.resize(host.bindings.size());
    host.anchorSerials.resize(host.bindings.size(), 0);
    std::size_t total = 0;
    for (std::uint32_t b = 0; b < host.bindings.size(); ++b) {
        const auto& binding = host.bindings[b];
        if (binding.requestIndex < requestCount) host.requestBindings[binding.requestIndex] = b;
        host.anchorOffsets[b] = static_cast<std::uint32_t>(total);
        if (binding.assetIndex < assets.size()) total += assets[binding.assetIndex].emitterNodes.size();
    }
    host.frameAnchors.resize(total);
    ++host.anchorSerial; // 上一帧写下的序号自然失效，不需要逐绑定清零。
}

void CaptureAnimationParticleAnchors(AnimationParticleHost& host, std::uint32_t request, std::uint32_t assetIndex,
    const AnimationAsset& asset, std::span<const War3RenderCore::NodePose> nodes) {
    if (request >= host.requestBindings.size()) return;
    const auto b = host.requestBindings[request];
    if (b == UINT32_MAX || b >= host.bindings.size() || host.bindings[b].assetIndex != assetIndex) return;
    const auto offset = static_cast<std::size_t>(host.anchorOffsets[b]);
    const auto count = asset.emitterNodes.size();
    if (count > host.frameAnchors.size() - std::min(offset, host.frameAnchors.size())) return;
    // 任一节点越界都不写：粒子回到单独求值路径，由它报告映射错误。
    if (std::any_of(asset.emitterNodes.begin(), asset.emitterNodes.end(), [&](auto node) { return node >= nodes.size(); })) return;
    for (std::size_t i = 0; i < count; ++i) WriteAnchor(host.frameAnchors[offset + i], nodes[asset.emitterNodes[i]]);
    host.anchorSerials[b] = host.anchorSerial;
}

Status PrepareAnimationParticles(AnimationParticleHost& host, const AnimatedModelScene& scene,
    std::span<const AnimationAsset> assets, std::span<const std::uint32_t> requestAssets,
    AnimationFrame& frame, const AnimationFrame* previous) {
    if (previous && (!previous->particleCheckpoint || previous->particleCheckpoint->sceneVersion != scene.sceneVersion ||
        previous->particleCheckpoint->contentVersion != previous->contentVersion ||
        previous->particleCheckpoint->instances.size() != host.bindings.size() ||
        previous->particleCheckpoint->frozen.size() != host.bindings.size()))
        return AnimationFailure("ANIMATION_PARTICLE_CHECKPOINT_STALE", "粒子检查点不属于已接受帧。");
    const bool world=host.playbackPolicy==AnimationPlaybackPolicy::InteractivePreview;
    const auto* preview=world && frame.previewParticleBudget ? &*frame.previewParticleBudget : nullptr;
    if (frame.previewParticleBudget && (!world || !frame.previewParticleBudget->maxAnchorSamples ||
        frame.previewParticleBudget->maxAnchorSamples>kMaximumPreviewAnchorSamples))
        return AnimationFailure("ANIMATION_PREVIEW_BUDGET_INVALID", "特效单帧软预算超出交互预览的有效范围。");
    // 新预算是显式交互合同；旧/严格调用保持原6000锚样。其余帧内存、节点/几何硬限不变。
    const auto maximumAnchors=preview ? preview->maxAnchorSamples : host.limits.maxAnchorSamples;
    const auto outside=[&](std::uint32_t r) { return !frame.requestVisibility.empty() && frame.requestVisibility[r]==0; };
    // 本帧开始时登记、主姿势求值写下的节点；序号不等即不属于本帧。
    const auto anchorsReused=[&](std::size_t b) {
        return world && host.anchorSerial && b<host.anchorSerials.size() && host.anchorSerials[b]==host.anchorSerial
            && b<host.anchorOffsets.size();
    };
    struct CloseAnchors { AnimationParticleHost& host; ~CloseAnchors() { ++host.anchorSerial; } } closeAnchors{host};
    for (std::uint32_t r=0;r<scene.particleCapabilities.size();++r) {
        const auto& capability=scene.particleCapabilities[r];
        if (outside(r) && (capability.particle2Count || capability.ribbonCount || capability.premCount))
            ++frame.offscreenParticleInstances;
    }
    const bool needsCamera=std::any_of(host.bindings.begin(),host.bindings.end(),
        [&](const auto& binding) { return !outside(binding.requestIndex); });
    if (needsCamera && !frame.worldCamera)
        return AnimationFailure("ANIMATION_PARTICLE_CAMERA_REQUIRED", "粒子需要显式世界相机；没有用虚构几何自动取景。");
    auto checkpoint = std::make_shared<AnimationParticleCheckpoint>();
    checkpoint->sceneVersion = scene.sceneVersion; checkpoint->contentVersion = frame.contentVersion;
    if (previous) {
        checkpoint->instances=previous->particleCheckpoint->instances;
        checkpoint->stateBytes=previous->particleCheckpoint->stateBytes;
    } else checkpoint->instances.resize(host.bindings.size());
    checkpoint->frozen.resize(host.bindings.size(),1);
    if (!world) {
        frame.particleVertices.reserve(host.maxDynamicVertices); frame.particleDraws.reserve(host.maxDynamicDraws);
        frame.particleMaterialFlags.reserve(host.maxDynamicDraws);
    }
    frame.particleLinks.reserve(host.bindings.size());
    std::vector<std::uint32_t> rendered;
    if (!host.bindings.empty()) {
        rendered.assign(scene.specs.size(),UINT32_MAX);
        for (std::uint32_t i=0;i<frame.poses.size();++i)
            if (frame.poses[i].requestIndex!=kBakedInstance) rendered[frame.poses[i].requestIndex]=i;
    }
    std::uint64_t anchorSamples = 0, nodeVisits = 0;
    for (std::uint32_t r = 0; r < scene.particleCapabilities.size(); ++r) {
        const auto& c = scene.particleCapabilities[r];
        if (c.available || (!c.particle2Count && !c.ribbonCount && c.premCount == c.premPreparedCount)) continue;
        if (outside(r)) {
            if (!host.omitRejectedLinks) {
                AnimationParticleLink link;link.requestIndex=r;link.identity=frame.states[r].identity;
                link.asset={static_cast<std::uint64_t>(requestAssets[r])+1,scene.generation};link.outsideView=true;
                frame.particleLinks.push_back(std::move(link));
            }
            continue;
        }
        if (c.rejectionCode=="ANIMATION_PARTICLE_INSTANCE_BUDGET") {
            ++frame.budgetParticleInstances;
            const auto& asset=assets[requestAssets[r]];
            if (asset.particles) frame.budgetParticleEmitters+=asset.particles->emitters.size();
            continue;
        }
        if (host.omitRejectedLinks) continue; // 原始拒绝仍由当前 scene capability 与共用 Query 提供。
        AnimationParticleLink link; link.requestIndex = r; link.identity = frame.states[r].identity;
        link.asset = {static_cast<std::uint64_t>(requestAssets[r]) + 1, scene.generation};
        link.rejectionCode = c.rejectionCode;
        link.unsupportedPremCount = c.premCount - std::min(c.premCount, c.premPreparedCount);
        link.partial = link.unsupportedPremCount != 0 || (c.skippedEmitters && !c.skippedEmitters->empty());
        if (c.skippedEmitters && !c.skippedEmitters->empty()) link.rejectionMessage = c.skippedEmitters->front().message;
        link.hidden = std::any_of(scene.unavailableInstances.begin(), scene.unavailableInstances.end(),
            [&](const auto& item) { return item.requestIndex == r; });
        link.rejectedElapsedMs = frame.states[r].elapsedMs; link.rejectedGlobalMs = frame.states[r].globalMs;
        frame.particleLinks.push_back(std::move(link));
    }
    // 稳定距离桶内按实例身份排序，微小镜头移动不改变同桶预算胜者。
    std::vector<std::size_t> order,outsideOrder;
    order.reserve(host.bindings.size());
    for (std::size_t b=0;b<host.bindings.size();++b) {
        if (outside(host.bindings[b].requestIndex)) outsideOrder.push_back(b);else order.push_back(b);
    }
    if (host.retainUnsupportedStatic && frame.worldCamera) {
        const auto eye = frame.worldCamera->eye;
        std::vector<double> distance(host.bindings.size());
        for (const auto b:order) {
            const auto& t = frame.states[host.bindings[b].requestIndex].transform;
            const double dx = double(t[3]) - eye[0], dy = double(t[7]) - eye[1], dz = double(t[11]) - eye[2];
            distance[b] = std::isfinite(dx * dx + dy * dy + dz * dz) ? dx * dx + dy * dy + dz * dz : INFINITY;
            if (world) distance[b]=std::floor(std::sqrt(distance[b])/512.0);
        }
        std::stable_sort(order.begin(), order.end(), [&](auto a, auto c) {
            if (distance[a]!=distance[c]) return distance[a]<distance[c];
            if (!world) return false;
            const auto& x=frame.states[host.bindings[a].requestIndex].identity;
            const auto& y=frame.states[host.bindings[c].requestIndex].identity;
            if (x.value!=y.value) return x.value<y.value;
            return x.generation<y.generation;
        });
    }
    std::vector<AnimationParticleAdmission> admissions;
    if (world) {
        admissions.resize(host.bindings.size());
        std::uint64_t reservedAnchors=0,reservedNodes=0,reservedVertices=0,reservedDraws=0,reservedStates=0;
        std::uint64_t reservedParticles=0,reservedSegments=0;
        std::uint64_t admittedInstances=0;
        const auto oldGeometry=previous ? AnimationParticleOutputBytes(*previous) : 0;
        for (const auto b:order) {
            const auto& binding=host.bindings[b];const auto r=binding.requestIndex;
            if (outside(r)) continue;
            const auto* model=rendered[r]==UINT32_MAX ? nullptr : &frame.poses[rendered[r]];
            if (model && (model->hidden || model->staticForBudget)) continue;
            const auto& asset=assets[binding.assetIndex];auto& plan=admissions[b];
            if (preview && admittedInstances>=preview->maxInstances) {
                plan.code="ANIMATION_PARTICLE_PREVIEW_TIME_BUDGET";continue;
            }
            // 固定时间成本与显示帧间隔无关；慢帧也不改变同一可见集合的准入胜者。
            // 固定步在前后两帧节点之间插值；本帧主姿势已给出节点时不再单独求骨架，否则只求当前一次。
            plan.anchors=anchorsReused(b) ? 0 : 1;
            // 节点安全上限按每个模拟实例的整副骨骼计，节点取自主姿势时也计入，
            // 一帧模拟的骨骼规模始终受同一上限约束。
            plan.nodes=asset.compiled.NodeCount();
            if (plan.anchors>maximumAnchors-reservedAnchors ||
                plan.nodes>host.limits.maxAnchorNodeVisits-reservedNodes) {
                plan.code="ANIMATION_PARTICLE_ANCHOR_BUDGET";continue;
            }
            const auto* old=checkpoint->instances[b].get();
            std::uint64_t particles=0,segments=0;
            plan.stateBytes=sizeof(AnimationParticleState)+asset.particles->emitters.size()*sizeof(Particles::EmitterState)
                +asset.emitterNodes.size()*sizeof(Particles::Anchor);
            for (std::size_t e=0;e<asset.particles->emitters.size();++e) {
                const auto& emitter=asset.particles->emitters[e];
                const auto quota=old ? old->state.emitters[e].quota : ExpectedQuota(emitter,host.limits.maxPerEmitter);
                plan.quotas.push_back(quota);
                const bool particle=emitter.kind==Particles::Kind::Particle2;
                if (particle) particles+=quota;else segments+=quota;
                plan.stateBytes+=std::uint64_t(quota)*(particle ? sizeof(Particles::Particle) : sizeof(Particles::Segment));
                plan.vertices+=std::uint64_t(quota)*(particle && emitter.headOrTail==2 ? 12u : 6u);
            }
            plan.draws=asset.particles->emitters.size();
            if (particles>host.limits.maxParticles-reservedParticles || segments>host.limits.maxRibbonSegments-reservedSegments ||
                plan.vertices>host.limits.maxVertices-reservedVertices || plan.draws>host.limits.maxDraws-reservedDraws) {
                plan.code="ANIMATION_PARTICLE_INSTANCE_BUDGET";continue;
            }
            // 共享旧检查点只收费一次；本帧获准副本、输出及临时几何都收费，不把模拟配额称为RSS。
            // 同时保留同规模前帧输出的容量，避免旧/新容量交替挤出预算边缘实例。
            const auto outputCapacity=AnimationParticleOutputBytes(reservedVertices+plan.vertices,
                reservedDraws+plan.draws,reservedDraws+plan.draws);
            const auto bytes=host.fixedFrameBytes+checkpoint->stateBytes+std::max(oldGeometry,outputCapacity)+reservedStates+plan.stateBytes+
                3*(reservedVertices+plan.vertices)*sizeof(Particles::Vertex)+
                2*(reservedDraws+plan.draws)*sizeof(War3RenderParticleDrawV2)+
                (reservedDraws+plan.draws)*sizeof(std::uint32_t); // 当前输出拥有flags，临时geometry没有该数组。
            if (bytes>host.maxFrameBytes) { plan.code="ANIMATION_FRAME_BUDGET";continue; }
            plan.admitted=true;reservedAnchors+=plan.anchors;reservedNodes+=plan.nodes;
            ++admittedInstances;
            reservedParticles+=particles;reservedSegments+=segments;
            reservedVertices+=plan.vertices;reservedDraws+=plan.draws;reservedStates+=plan.stateBytes;
        }
        frame.particleVertices.reserve(static_cast<std::size_t>(reservedVertices));
        frame.particleDraws.reserve(static_cast<std::size_t>(reservedDraws));
        frame.particleMaterialFlags.reserve(static_cast<std::size_t>(reservedDraws));
    }
    order.insert(order.end(),outsideOrder.begin(),outsideOrder.end());
    const auto freeze=[&](AnimationParticleLink& link, std::size_t b) {
        link.skippedTimelineMs=0;
        const auto* old=checkpoint->instances[b].get();
        if (!old) return;
        link.frozen=true;link.simulationTimeMs=old->state.timeMs;link.stateHash=old->stateHash;
        link.particleCount=old->counts.particles;link.ribbonSegmentCount=old->counts.ribbonSegments;
        link.spawned=old->counts.spawned;link.suppressed=old->counts.suppressed;
    };
    // 前置检查：视野外、预算静态、隐藏、未获准的实例在这里写完回执并返回 false，其余进入模拟。
    // 动作序号越界只做标记，到该实例的原位置再由 at 抛出。每个请求至多一条回执，最后按请求排序，
    // 所以未模拟实例的回执可以先放入帧。
    const auto early=[&](AnimationParticleLink& link, std::size_t b, bool& invalidSequence) -> bool {
        const auto& binding = host.bindings[b]; const auto r = binding.requestIndex;
        const auto& asset = assets[binding.assetIndex]; const auto& current = frame.states[r];
        link.requestIndex = r; link.identity = current.identity; link.asset = asset.compiled.Identity();
        if (outside(r)) { link.outsideView=true;freeze(link,b);return false; }
        if (current.sequenceIndex>=scene.sequences[r].size()) { invalidSequence=true;return false; }
        link.unsupportedPremCount = asset.particleCapability.premCount -
            std::min(asset.particleCapability.premCount, asset.particleCapability.premPreparedCount);
        const auto& skipped = asset.particleCapability.skippedEmitters;
        link.partial = link.unsupportedPremCount != 0 || asset.particleUnfoggedPreview || (skipped && !skipped->empty());
        if (link.partial) {
            link.rejectionCode = asset.particleCapability.rejectionCode;
            if (link.unsupportedPremCount) link.rejectionMessage = asset.particleCapability.premPreparedCount
                ? "部分 PREM 未通过基础子模型准入；本列表仅表示 PRE2/RIBB，详见 PREM 回执。"
                : "PREM 未实现，本帧仅显示受支持的 PRE2/RIBB。";
            if (skipped && !skipped->empty()) link.rejectionMessage += " 已跳过无效发射器：" + skipped->front().message;
            if (asset.particleUnfoggedPreview) link.rejectionMessage += " 当前粒子接口统一应用环境雾，Unfogged 为预览近似；无雾时无差别。";
        }
        link.simulationTimeMs = current.elapsedMs; link.budgetLimited = !world && binding.quotaLimited;
        const auto* model=rendered[r]==UINT32_MAX ? nullptr : &frame.poses[rendered[r]];
        if (model && model->staticForBudget) {
            link.budgetLimited=true; link.rejectionCode="ANIMATION_PARTICLE_INSTANCE_STATIC_FOR_BUDGET";
            link.rejectionMessage="模型保留实际 Stand；本帧动态预算未分配此实例的粒子模拟。";
            freeze(link,b);++frame.budgetParticleInstances;frame.budgetParticleEmitters+=asset.particles->emitters.size();
            return false;
        }
        if (model && model->hidden) {
            link.hidden = true; link.rejectionCode = CodeName(model->rejection.code);
            link.rejectionMessage = CodeMessage(model->rejection.code);
            link.rejectedElapsedMs = current.elapsedMs; link.rejectedGlobalMs = current.globalMs;
            freeze(link,b);return false;
        }
        if (world && !admissions[b].admitted) {
            link.budgetLimited=true;link.rejectionCode=admissions[b].code;
            link.rejectionMessage="本帧完整粒子成本未获准；保留上一不可变检查点，省略几何。";
            freeze(link,b);++frame.budgetParticleInstances;frame.budgetParticleEmitters+=asset.particles->emitters.size();
            return false;
        }
        return true;
    };
    const auto throwInvalidSequence=[&](std::size_t b) {
        const auto r=host.bindings[b].requestIndex;
        (void)scene.sequences[r].at(frame.states[r].sequenceIndex);
    };
    const auto setup=[&](ParticleWork& w) {
        CpuPhase phase{w.simulationNs};
        const auto b=w.binding;const auto& binding=host.bindings[b];const auto r=binding.requestIndex;
        const auto& asset=assets[binding.assetIndex];const auto& current=frame.states[r];
        const auto* old=checkpoint->instances[b].get();
        const auto& sequence=scene.sequences[r][current.sequenceIndex];
        auto& link=w.link;auto& timeline=w.timeline;auto& candidate=w.candidate;
        timeline=Particles::Timeline{sequence.start, sequence.end, current.sequenceIndex, sequence.flags == 0,
            current.elapsedMs, current.globalMs, current.transform};
        timeline.boundedWorldPreview=world;
        const auto globalStart = Particles::GlobalPhase(timeline);
        // 宿主时间组合错误不能降级成内容隐藏。
        if (!globalStart.Succeeded()) { w.fatal=Status{globalStart.diagnostics};return; }
        if (!old) link.resetReason = world ? "initial" : previous ? "recover" : "initial";
        else if (old->state.sequenceIndex != current.sequenceIndex) link.resetReason = "sequence";
        else if (current.elapsedMs < old->state.timeMs ||
            (world && previous && current.elapsedMs<previous->states[r].elapsedMs)) link.resetReason = "rewind";
        else if (old->state.globalStartMs != globalStart.value || old->state.boundedWorldPreview!=world) link.resetReason = "global-phase";
        else if (old->timeline.world != current.transform) link.resetReason = "transform";
        // 镜头改变只更新本帧锚点与billboard几何，不删除已发出的世界粒子或重置随机流。
        // 严格历史预览仍显式回放camera依赖；世界模式为连续当前锚点的预览近似。
        else if (!world && asset.cameraDependent && !SameCamera(old->camera, frame.worldCamera)) link.resetReason = "camera";
        candidate.timeline = timeline; candidate.camera = frame.worldCamera;
        if (link.resetReason.empty()) {
            candidate.state = old->state;
            for (std::size_t i = 0; i < candidate.state.emitters.size(); ++i) {
                auto& e = candidate.state.emitters[i];
                if (asset.particles->emitters[i].kind == Particles::Kind::Particle2) e.particles.reserve(e.quota);
                else e.segments.reserve(e.quota);
            }
        } else {
            auto created = Particles::Create(*asset.particles, current.identity.value, current.identity.generation,
                current.sequenceIndex, world ? std::span<const std::uint32_t>(admissions[b].quotas) :
                    std::span<const std::uint32_t>(binding.quotas), host.simulation);
            if (!created.Succeeded()) { w.fatal=Status{std::move(created.diagnostics)};return; }
            candidate.state = std::move(*created.value);
        }
        if (world) {
            const auto stop=current.elapsedMs-current.elapsedMs%Particles::FixedStepMs;
            const auto maximum=std::uint64_t(host.limits.maxWorldFrameSteps)*Particles::FixedStepMs;
            std::optional<std::uint64_t> forward;
            if (!link.resetReason.empty() || (previous && previous->particleCheckpoint->frozen[b])) {
                // 首次进入视野与冻结恢复直接当前；没有recover或历史预热。
                forward=current.elapsedMs;
            } else if (stop-candidate.state.tickMs>maximum) {
                // 交互世界帧只模拟有界尾段；旧区间只老化，不补发，不改变随机流。
                forward=stop-maximum;
            }
            if (forward && current.elapsedMs>current.globalMs)
                *forward=std::max(*forward,current.elapsedMs-current.globalMs);
            if (forward) {
                link.skippedTimelineMs=*forward-candidate.state.timeMs;
                const auto forwarded=Particles::FastForwardFrozen(*asset.particles,candidate.state,timeline,*forward);
                if (!forwarded.Succeeded()) { w.fatal=forwarded;return; }
            }
        }
        w.sampledElapsed = current.elapsedMs; w.sampledGlobal = current.globalMs;
        w.local.emplace(ToAnimationLocalCamera(*frame.worldCamera, current.transform, host.retainUnsupportedStatic));
        if (!w.local->Succeeded() && !host.retainUnsupportedStatic) { w.fatal=Status{std::move(w.local->diagnostics)};return; }
        // 即使 t=0 不发射，也验证实际当前节点/GEOA；不能让纯粒子绕过核心门禁。
        // 交互预览：同一请求、同一相机的主姿势已通过核心求值，直接用它的节点。
        w.currentAnchors.resize(asset.emitterNodes.size());
        w.anchorsReused=anchorsReused(b);
        if (w.anchorsReused) {
            const auto first = host.frameAnchors.begin() + host.anchorOffsets[b];
            std::copy(first, first + static_cast<std::ptrdiff_t>(w.currentAnchors.size()), w.currentAnchors.begin());
        }
    };
    // 单独求骨架：共用核心求值器与整帧锚样预算，只能按实例顺序串行。
    const auto sample=[&](ParticleWork& w, std::uint64_t elapsed, std::span<Particles::Anchor> anchors) -> Status {
        // 交互预览：局部相机建不出来时只拒绝这个实例的粒子（由 reject 处理），不拒绝整帧。
        if (!w.local->Succeeded()) return Status{w.local->diagnostics};
        const auto& binding=host.bindings[w.binding];const auto& asset=assets[binding.assetIndex];
        const auto& current=frame.states[binding.requestIndex];
        w.sampledElapsed = elapsed;
        const auto global = Particles::GlobalTimeAt(w.timeline, elapsed);
        if (!global.Succeeded()) return {global.diagnostics};
        w.sampledGlobal = *global.value;
        if (anchorSamples >= maximumAnchors || asset.compiled.NodeCount() > host.limits.maxAnchorNodeVisits - nodeVisits)
            return AnimationFailure("ANIMATION_PARTICLE_ANCHOR_BUDGET", "整次帧请求的历史骨架调用或节点访问超过预算。");
        ++anchorSamples; nodeVisits += asset.compiled.NodeCount();
        ++frame.timings.particleAnchorRequests;
        frame.timings.particleAnchorNodeVisits+=asset.compiled.NodeCount();
        PoseRequest request; request.instance = current.identity; request.asset = asset.compiled.Identity();
        request.assetIndex = asset.evaluatorIndex; request.sequenceIndex = current.sequenceIndex;
        request.sequenceElapsedMs = elapsed; request.globalElapsedMs = *global.value;
        request.hasCamera = 1; request.camera = *w.local->value;
        const auto outcome = host.evaluator->Evaluate(std::span(&request, 1));
        if (outcome.error != Code::Ok) return AnimationFailure(CodeName(outcome.error), CodeMessage(outcome.error));
        const auto& receipt = host.evaluator->Receipts()[0];
        if (receipt.error != Code::Ok) return AnimationFailure(CodeName(receipt.error), CodeMessage(receipt.error));
        const auto nodes = host.evaluator->Nodes(0);
        if (anchors.size() != asset.emitterNodes.size()) return AnimationFailure("ANIMATION_PARTICLE_MAPPING", "发射器锚点数量不一致。");
        for (std::size_t i = 0; i < anchors.size(); ++i) {
            if (asset.emitterNodes[i] >= nodes.size()) return AnimationFailure("ANIMATION_PARTICLE_MAPPING", "发射器节点序号越界。");
            const auto& node = nodes[asset.emitterNodes[i]];
            anchors[i].pivot = node.pivot;
            for (std::size_t row = 0; row < 3; ++row) for (std::size_t column = 0; column < 4; ++column)
                anchors[i].model[row * 4 + column] = node.worldMatrix[column * 4 + row];
        }
        return {};
    };
    const auto sampleCurrent=[&](ParticleWork& w) {
        CpuPhase phase{w.simulationNs};
        w.status=sample(w, frame.states[host.bindings[w.binding].requestIndex].elapsedMs, w.currentAnchors);
    };
    const auto advance=[&](ParticleWork& w) {
        if (!w.status.Succeeded()) return;
        CpuPhase phase{w.simulationNs};
        const auto& binding=host.bindings[w.binding];const auto& asset=assets[binding.assetIndex];
        const auto& current=frame.states[binding.requestIndex];
        const auto* old=checkpoint->instances[w.binding].get();
        const auto& currentAnchors=w.currentAnchors;
        // 交互预览的固定步在上一帧与本帧节点之间线性插值（预览近似），不逐步重求骨架；
        // 没有连续的上一帧节点（初次、重置、动作切换）时保持当前节点。严格回放仍逐步求值。
        const std::vector<Particles::Anchor>* previousAnchors = nullptr;
        std::uint64_t previousElapsed = 0;
        if (world && w.link.resetReason.empty() && old && old->anchors.size() == currentAnchors.size()
            && old->anchorElapsedMs < current.elapsedMs) {
            previousAnchors = &old->anchors; previousElapsed = old->anchorElapsedMs;
        }
        const auto interpolated = [&](std::uint64_t elapsed, std::span<Particles::Anchor> anchors) -> Status {
            w.sampledElapsed = elapsed;
            const auto global = Particles::GlobalTimeAt(w.timeline, elapsed);
            if (!global.Succeeded()) return {global.diagnostics};
            w.sampledGlobal = *global.value;
            if (anchors.size() != currentAnchors.size()) return AnimationFailure("ANIMATION_PARTICLE_MAPPING", "发射器锚点数量不一致。");
            if (!previousAnchors || elapsed >= current.elapsedMs) {
                std::copy(currentAnchors.begin(), currentAnchors.end(), anchors.begin());
                return {};
            }
            if (elapsed <= previousElapsed) {
                std::copy(previousAnchors->begin(), previousAnchors->end(), anchors.begin());
                return {};
            }
            const auto t = static_cast<float>(double(elapsed - previousElapsed) / double(current.elapsedMs - previousElapsed));
            for (std::size_t i = 0; i < anchors.size(); ++i) {
                const auto& from = (*previousAnchors)[i]; const auto& to = currentAnchors[i];
                for (std::size_t j = 0; j < to.model.size(); ++j) anchors[i].model[j] = from.model[j] + (to.model[j] - from.model[j]) * t;
                anchors[i].pivot = to.pivot;
            }
            return {};
        };
        const auto sampler=[&](std::uint64_t elapsed, std::span<Particles::Anchor> anchors) { return sample(w, elapsed, anchors); };
        w.status = world
            ? Particles::Advance(*asset.particles, w.candidate.state, w.timeline, current.elapsedMs, interpolated, host.simulation)
            : Particles::Advance(*asset.particles, w.candidate.state, w.timeline, current.elapsedMs, sampler, host.simulation);
        if (w.status.Succeeded()) { w.counts=Particles::Count(w.candidate.state);w.stateHash=StateHash(w.candidate.state); }
    };
    // 生成几何、换成输出顶点并算几何哈希。并行时先按整帧上限生成；合并时若失败或超出剩余额度，再按剩余额度重做。
    // 每次加顶点前的额度检查在两种上限下同样通过，所以未重做的结果与按剩余额度生成相同。
    const auto build=[&](ParticleWork& w, std::uint32_t budget) {
        CpuPhase phase{w.geometryNs};
        const auto& binding=host.bindings[w.binding];const auto& asset=assets[binding.assetIndex];
        const auto& current=frame.states[binding.requestIndex];
        w.built=true;w.budget=budget;w.mappingFailed=false;
        w.vertices.clear();w.draws.clear();w.geometryDiagnostics.clear();
        auto geometry = Particles::Build(*asset.particles, w.candidate.state, w.timeline, frame.worldCamera->eye,
            frame.worldCamera->up, budget, w.currentAnchors);
        w.geometryOk=geometry.Succeeded();
        if (!w.geometryOk) { w.geometryDiagnostics=std::move(geometry.diagnostics);return; }
        w.vertexCount = static_cast<std::uint32_t>(geometry.value->vertices.size());
        w.draws=std::move(geometry.value->draws);
        w.vertices.reserve(geometry.value->vertices.size());
        Hash geometryHash;
        for (const auto& draw : w.draws) {
            if (draw.emitter >= binding.textures.size() || draw.emitter >= binding.layerAlpha.size()
                || !binding.materialFlags || draw.emitter >= binding.materialFlags->size()) { w.mappingFailed=true;return; }
            const auto materialFlags=(*binding.materialFlags)[draw.emitter];
            geometryHash.Add(draw.first); geometryHash.Add(draw.count); geometryHash.Add(draw.emitter); geometryHash.Add(draw.filter);
            geometryHash.Add(materialFlags);
            for (std::uint32_t i = 0; i < draw.count; ++i) {
                auto vertex = geometry.value->vertices[draw.first + i];
                for (std::size_t c = 0; c < 3; ++c) vertex.color[c] *= current.tint[c];
                vertex.color[3] *= binding.layerAlpha[draw.emitter];
                geometryHash.Add(vertex.position); geometryHash.Add(vertex.uv); geometryHash.Add(vertex.color);
                War3RenderParticleVertexV2 output{};
                std::copy(vertex.position.begin(), vertex.position.end(), output.position);
                std::copy(vertex.uv.begin(), vertex.uv.end(), output.uv);
                std::copy(vertex.color.begin(), vertex.color.end(), output.color); w.vertices.push_back(output);
            }
        }
        w.geometryHash=geometryHash.value;
    };
    // 按实例顺序合并：失败处理、整帧绘制额度、追加几何与提交检查点都在这里串行完成。
    const auto finish=[&](ParticleWork& w) -> Status {
        if (w.exception) std::rethrow_exception(w.exception);
        if (!w.fatal.Succeeded()) return w.fatal;
        const auto b=w.binding;const auto& binding=host.bindings[b];const auto r=binding.requestIndex;
        const auto& asset=assets[binding.assetIndex];const auto& current=frame.states[r];
        const auto* old=checkpoint->instances[b].get();
        auto& link=w.link;
        ++frame.timings.particleSimulationRequests;
        if (w.anchorsReused) ++frame.timings.particleAnchorReuses;
        frame.timings.particleSimulationNs+=w.simulationNs;
        const auto reject = [&](const std::vector<Diagnostic>& diagnostics) -> Status {
            const auto error = std::find_if(diagnostics.begin(), diagnostics.end(),
                [](const auto& d) { return d.severity == DiagnosticSeverity::Error; });
            if (error == diagnostics.end()) return AnimationFailure("ANIMATION_PARTICLE_DIAGNOSTIC", "粒子失败缺少原始诊断。");
            if (host.retainUnsupportedStatic && (error->code=="ANIMATION_PARTICLE_ANCHOR_BUDGET" || error->code=="PARTICLE_VERTEX_BUDGET")) {
                link.budgetLimited=true; link.rejectionCode=error->code; link.rejectionMessage=error->message;
                freeze(link,b);link.resetReason.clear();
                ++frame.budgetParticleInstances;frame.budgetParticleEmitters+=asset.particles->emitters.size();return {};
            }
            if (!host.retainUnsupportedStatic || !std::all_of(diagnostics.begin(), diagnostics.end(), [](const auto& d) {
                return d.severity != DiagnosticSeverity::Error || AnimationLocalDataFailure(d.code) ||
                    AnimationInstanceCameraFailure(d.code); })) return {diagnostics};
            link.hidden = !world; link.partial |= world;
            link.rejectionCode = error->code; link.rejectionMessage = error->message;
            link.rejectedElapsedMs = w.sampledElapsed; link.rejectedGlobalMs = w.sampledGlobal;
            freeze(link,b);
            // 交互预览仅省略失败粒子；模型已独立完成姿势求值，不能因此消失。
            // 严格历史回放仍按既有整实例拒绝语义，不能冒称数值一致。
            if (!world) HideModel(frame, rendered[r], error->code, error->message);
            return {};
        };
        if (!w.status.Succeeded()) {
            if (auto result = reject(w.status.diagnostics); !result.Succeeded()) return result;
            frame.particleLinks.push_back(std::move(link)); return {};
        }
        w.sampledElapsed = current.elapsedMs; w.sampledGlobal = current.globalMs;
        frame.timings.particleGeometryNs+=std::exchange(w.geometryNs,0);
        CpuPhase geometryPhase{frame.timings.particleGeometryNs};
        const auto remaining=host.limits.maxVertices - static_cast<std::uint32_t>(frame.particleVertices.size());
        if (!w.built || (w.budget!=remaining && (!w.geometryOk || w.vertexCount>remaining))) build(w, remaining);
        if (!w.geometryOk) {
            if (auto result = reject(w.geometryDiagnostics); !result.Succeeded()) return result;
            frame.particleLinks.push_back(std::move(link)); return {};
        }
        if (w.draws.size() > host.limits.maxDraws - frame.particleDraws.size()) {
            if (!host.retainUnsupportedStatic) return AnimationFailure("ANIMATION_PARTICLE_DRAW_BUDGET", "粒子绘制批次数超过整场景预算。");
            link.budgetLimited=true; link.rejectionCode="ANIMATION_PARTICLE_DRAW_BUDGET";
            link.rejectionMessage="本帧粒子绘制批次数不足，模型保留；没有提交部分发射器几何。";
            freeze(link,b);link.resetReason.clear();
            ++frame.budgetParticleInstances;frame.budgetParticleEmitters+=asset.particles->emitters.size();
            frame.particleLinks.push_back(std::move(link)); return {};
        }
        if (w.mappingFailed) return AnimationFailure("ANIMATION_PARTICLE_MAPPING", "粒子绘制发射器索引越界。");
        const auto& counts = w.counts;
        link.simulated = true; link.particleCount = counts.particles; link.ribbonSegmentCount = counts.ribbonSegments;
        link.spawned = counts.spawned; link.suppressed = counts.suppressed; link.budgetLimited |= counts.budgetLimited;
        link.stateHash = w.stateHash; link.firstVertex = static_cast<std::uint32_t>(frame.particleVertices.size());
        link.firstDraw = static_cast<std::uint32_t>(frame.particleDraws.size());
        link.vertexCount = w.vertexCount;
        link.drawCount = static_cast<std::uint32_t>(w.draws.size());
        frame.particleVertices.insert(frame.particleVertices.end(), w.vertices.begin(), w.vertices.end());
        for (const auto& draw : w.draws) {
            frame.particleDraws.push_back({link.firstVertex + draw.first, draw.count, binding.textures[draw.emitter], static_cast<std::uint32_t>(draw.filter)});
            frame.particleMaterialFlags.push_back((*binding.materialFlags)[draw.emitter]);
        }
        link.geometryHash = w.geometryHash;
        std::vector<War3RenderParticleVertexV2>().swap(w.vertices);
        auto& candidate=w.candidate;
        if (world) { candidate.anchors = std::move(w.currentAnchors); candidate.anchorElapsedMs = current.elapsedMs; }
        candidate.counts=counts;candidate.stateHash=link.stateHash;candidate.storageBytes=StateBytes(candidate);
        checkpoint->stateBytes-=old ? old->storageBytes : 0;checkpoint->stateBytes+=candidate.storageBytes;
        checkpoint->instances[b]=std::make_shared<const AnimationParticleState>(std::move(candidate));
        frame.timings.particleSkippedTimelineMs+=link.skippedTimelineMs;
        checkpoint->frozen[b]=0;frame.particleLinks.push_back(std::move(link));
        return {};
    };
    if (world) {
        // 只为进入模拟的实例建中间结果；动作序号越界的实例放在末尾占位，合并到它时抛出。
        std::vector<ParticleWork> works;
        bool invalid=false;
        for (const auto b:order) {
            AnimationParticleLink link;
            if (early(link,b,invalid)) { auto& w=works.emplace_back();w.binding=b;w.link=std::move(link); }
            else if (invalid) { auto& w=works.emplace_back();w.binding=b;w.invalidSequence=true;break; }
            else frame.particleLinks.push_back(std::move(link));
        }
        const auto tasks=[&](std::size_t count, const std::function<void(ParticleWork&)>& step) {
            const auto run=[&](std::uint32_t i) {
                auto& w=works[i];
                try { step(w); } catch (...) { w.exception=std::current_exception(); }
            };
            if (host.workers==1) { for (std::uint32_t i=0;i<count;++i) run(i); }
            else RunAnimationTasks(static_cast<std::uint32_t>(count), run);
        };
        const auto simulated=works.size()-(invalid ? 1 : 0);
        tasks(simulated, setup);
        std::size_t stop=simulated;
        for (std::size_t i=0;i<simulated;++i) {
            auto& w=works[i];
            if (w.exception || !w.fatal.Succeeded()) { stop=i;break; }
            if (!w.anchorsReused) sampleCurrent(w);
        }
        tasks(stop, [&](ParticleWork& w) { advance(w); if (w.status.Succeeded()) build(w, host.limits.maxVertices); });
        for (auto& w:works) {
            if (w.invalidSequence) throwInvalidSequence(w.binding);
            if (auto result=finish(w); !result.Succeeded()) return result;
        }
    } else for (const auto b:order) {
        ParticleWork w;w.binding=b;
        bool invalid=false;
        if (!early(w.link,b,invalid)) {
            if (invalid) throwInvalidSequence(b);
            frame.particleLinks.push_back(std::move(w.link));continue;
        }
        setup(w);
        if (w.fatal.Succeeded() && !w.anchorsReused) sampleCurrent(w);
        if (w.fatal.Succeeded()) advance(w);
        if (auto result=finish(w); !result.Succeeded()) return result;
    }
    std::sort(frame.particleLinks.begin(), frame.particleLinks.end(), [](const auto& a, const auto& b) { return a.requestIndex < b.requestIndex; });
    frame.particleCheckpoint = std::move(checkpoint);
    return {};
}
} // namespace War3Map::Render
