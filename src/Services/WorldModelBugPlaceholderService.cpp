#include "War3MapCore/Services/WorldModelBugPlaceholderService.h"
#include "War3MapCore/Hash.h"
#include <algorithm>
#include <cmath>
#include <set>
#include <unordered_set>

namespace War3Map::Services {
bool WorldModelBugLayerFrameQualified(const War3RenderModelLayersTargetV2& before,
    const War3RenderModelLayersTargetV2& after, const Render::ViewProjectionFrame& currentView,
    const War3RenderFrameMetricsV2& metric) noexcept {
    constexpr auto required = WAR3_RENDER_FRAME_METRICS_DRAW_COMPLETED | WAR3_RENDER_FRAME_METRICS_UPLOAD_VALID
        | WAR3_RENDER_FRAME_METRICS_WINDOW_ATTACHED;
    const auto& view = metric.completed.view;
    return before.structSize >= sizeof(before) && after.structSize >= sizeof(after)
        && before.layerCount && before.layerCount <= WAR3_RENDER_MAX_MODEL_SCENE_LAYERS && before.parentModelEpoch && before.layersRevision
        && before.layerCount == after.layerCount && before.parentModelEpoch == after.parentModelEpoch
        && before.layersRevision == after.layersRevision && metric.structSize >= sizeof(metric)
        && !metric.result && !metric.presentResult && (metric.flags & required) == required
        && !(metric.flags & WAR3_RENDER_FRAME_METRICS_PRESENT_OCCLUDED)
        && metric.completed.structSize >= sizeof(metric.completed) && view.structSize >= sizeof(view)
        && view.frameCount && view.surfaceEpoch && view.sceneEpoch && view.viewEpoch
        && view.surfaceEpoch == currentView.identity.surfaceEpoch && view.sceneEpoch == currentView.identity.sceneEpoch
        && view.viewEpoch == currentView.identity.viewEpoch && view.frameCount == currentView.identity.frameCount
        && view.width == currentView.physicalWidth && view.height == currentView.physicalHeight
        && std::equal(currentView.rowMajorViewProjection.begin(), currentView.rowMajorViewProjection.end(), view.viewProjection)
        && metric.completed.presentation.modelEpoch == before.parentModelEpoch;
}
namespace {
bool NonFailure(std::string_view code) {
    return code.empty() || code == "OK" || code == "RENDER_CORE_SOURCE_ORDER_KEY_LIMIT"
        || code.find("BUDGET") != code.npos || code.find("CAPACITY") != code.npos
        || code.find("ALLOC") != code.npos || code.find("MEMORY") != code.npos || code.find("PARTICLE") != code.npos || code.find("RIBBON") != code.npos
        || code.find("EMPTY") != code.npos || code.find("UNKNOWN") != code.npos;
}
bool HasPath(std::string_view path) {
    return !path.empty() && path != "_" && path.find_first_not_of(" \t\r\n") != path.npos;
}
bool BakedMapping(const Render::AnimatedModelScene& scene, std::size_t rendered, std::uint32_t model) {
    const auto& geometry = scene.geometry;
    // Builder::AddBaked / ReplaceBakedMeshes 只创建首个模型/实例；真实烘焙没有MDX或owned资产来源。
    if (rendered != 0 || model != 0 || !geometry.modelPaths[model].empty()
        || !geometry.modelAssetKeys[model].empty()) return false;
    const auto& source = scene.pickModels[model]; const auto& raw = geometry.models[model];
    if (source.source || source.collisionNodes || source.ownedStaticAsset
        || scene.models[model].groupCount != raw.meshCount || raw.firstMesh != 0
        || raw.firstMesh > geometry.meshGeosets.size() || raw.meshCount > geometry.meshGeosets.size() - raw.firstMesh)
        return false;
    for (std::uint32_t local = 0; local < raw.meshCount; ++local) {
        const auto mesh = raw.firstMesh + local;
        if (geometry.meshGeosets[mesh] != UINT32_MAX || scene.meshes[mesh].firstGroup != local
            || scene.meshes[mesh].groupCount != 1) return false;
    }
    return true;
}
}
bool WorldModelBugReadOnlySource::Matches(const std::shared_ptr<const Render::AnimatedModelScene>& scene) const noexcept {
    return scene && scene_.lock() == scene && sceneVersion_ == scene->sceneVersion
        && requestCount_ == scene->specs.size() && binding_.sourceGeneration == scene->generation
        && binding_.sourceIdentity == scene->sourceIdentity && binding_.mapRevision == scene->documentRevision;
}
Result<std::shared_ptr<const WorldModelBugReadOnlySource>> WorldModelBugReadOnlySource::Capture(
    const MapDocument& document, std::shared_ptr<const Render::AnimatedModelScene> scene,
    WorldInstanceBinding binding, std::span<const MapData::PlacementId> owners) try {
    using Output = Result<std::shared_ptr<const WorldModelBugReadOnlySource>>;
    const auto fail = [](const char* message) { return Output::Failure("World.BugPlaceholder",
        "WORLD_BUG_PLACEHOLDER_READONLY_SOURCE_UNPROVEN", message); };
    const auto budget = [] { return Output::Failure("World.BugPlaceholder",
        "WORLD_BUG_PLACEHOLDER_READONLY_SOURCE_BUDGET", "只读占位来源超出独立16 MiB快照预算。"); };
    if (!scene || !scene->generation || !scene->sceneVersion || binding.sourceIdentity.empty()
        || binding.sourceGeneration != scene->generation || binding.sourceIdentity != scene->sourceIdentity
        || binding.mapRevision != scene->documentRevision || owners.size() != scene->specs.size())
        return fail("只读占位来源与当前地图、请求代次或场景修订不一致。");
    if (owners.size() > kMaximumWorldModelBugSnapshotBytes / sizeof(WorldModelBugReadOnlyPlacement)
        || binding.sourceIdentity.size() > kMaximumWorldModelBugSnapshotBytes) return budget();
    auto output = std::shared_ptr<WorldModelBugReadOnlySource>(new WorldModelBugReadOnlySource);
    output->scene_ = scene; output->binding_ = std::move(binding); output->sceneVersion_ = scene->sceneVersion;
    output->requestCount_ = scene->specs.size(); output->placements_.reserve(owners.size());
    output->ownedBytes_ = sizeof(*output) + output->binding_.sourceIdentity.capacity() + 1
        + output->placements_.capacity() * sizeof(WorldModelBugReadOnlyPlacement);
    if (output->ownedBytes_ > kMaximumWorldModelBugSnapshotBytes) return budget();
    for (std::size_t file = 0; file < output->members_.size(); ++file) {
        auto& fact = output->members_[file]; fact.kind = file ? MapDataKind::Doodads : MapDataKind::Units;
        const auto& member = document.Member(fact.kind); fact.state = member.state;
        const auto typed = file ? bool(document.Doodads()) : bool(document.Units());
        if (member.state == MapDataState::Missing && member.sources.empty() && !typed) continue;
        if ((member.state != MapDataState::Ready && member.state != MapDataState::Partial)
            || member.sources.size() != 1 || !typed
            || (member.state == MapDataState::Partial && member.diagnostics.empty())
            || std::any_of(member.diagnostics.begin(), member.diagnostics.end(), [](const Diagnostic& item) {
                return item.severity == DiagnosticSeverity::Error || item.code != "MAP_IDENTITY_AMBIGUOUS";
            })) return fail("摆放来源未完整解析、来源歧义或含未支持的部分读取诊断；未将未知部分作为空表。");
        const auto& original = file ? document.Doodads()->OriginalBytes() : document.Units()->OriginalBytes();
        fact.parsedRecords = file ? document.Doodads()->doodads.size() + document.Doodads()->specialDoodads.size()
            : document.Units()->units.size();
        fact.initializationIdentityAmbiguous = member.state == MapDataState::Partial;
        if (original != member.sources.front().data || member.recordCount != fact.parsedRecords)
            return fail("摆放原始字节、完整解析记录数或唯一成员来源不一致。");
        auto sha = Sha256(original); if (!sha.Succeeded()) return {std::nullopt, std::move(sha.diagnostics)};
        fact.sourceSha256 = std::move(*sha.value); output->ownedBytes_ += fact.sourceSha256.capacity() + 1;
    }
    std::set<std::pair<MapData::PlacementFile, std::uint32_t>> rows;
    std::unordered_set<std::uint64_t> identities;
    std::unordered_set<std::string_view> hostKeys;
    for (std::size_t request = 0; request < owners.size(); ++request) {
        const auto& owner = owners[request]; const auto& spec = scene->specs[request];
        std::size_t file = 0; bool rowMatches = false;
        if (owner.file == MapData::PlacementFile::Units && document.Units()) {
            rowMatches = owner.index < document.Units()->units.size()
                && owner == MapData::MakePlacementId(owner.file, owner.index, document.Units()->units[owner.index]);
        } else if (owner.file == MapData::PlacementFile::Doodads && document.Doodads()) {
            file = 1; rowMatches = owner.index < document.Doodads()->doodads.size()
                && owner == MapData::MakePlacementId(owner.file, owner.index, document.Doodads()->doodads[owner.index]);
        }
        if (!rowMatches || !rows.emplace(owner.file, owner.index).second || !spec.identity.value
            || spec.identity.generation != scene->generation || !identities.insert(spec.identity.value).second
            || spec.hostKey.empty() || !hostKeys.insert(spec.hostKey).second)
            return fail("只读请求的原始记录索引、实例代次、hostKey或同序绑定重复/失配。");
        if (spec.hostKey.size() > kMaximumWorldModelBugSnapshotBytes
            || spec.model.modelPath.size() > kMaximumWorldModelBugSnapshotBytes) return budget();
        auto key = Render::ModelAssetKey(spec.model);
        if (key.size() > kMaximumWorldModelBugSnapshotBytes) return budget();
        output->placements_.push_back({owner, spec.identity, spec.hostKey, std::move(key)});
        const auto& added = output->placements_.back();
        output->ownedBytes_ += added.owner.rawcode.capacity() + added.hostKey.capacity() + added.assetKey.capacity() + 3;
        if (output->ownedBytes_ > kMaximumWorldModelBugSnapshotBytes) return budget();
        ++output->members_[file].boundRequests;
    }
    return Output::Success(std::move(output));
} catch (...) {
    return Result<std::shared_ptr<const WorldModelBugReadOnlySource>>::Failure("World.BugPlaceholder",
        "WORLD_BUG_PLACEHOLDER_READONLY_SOURCE_FAILED", "无法拥有完整只读占位来源，未生成不完整快照。");
}
Result<WorldModelBugPlaceholderFrameSnapshot> CaptureWorldModelBugPlaceholderFrame(
    const std::shared_ptr<const Render::AnimatedModelScene>& scene, const Render::AnimationFrame& frame) try {
    using Output = Result<WorldModelBugPlaceholderFrameSnapshot>;
    const auto fail = [](const char* message) { return Output::Failure("World.BugPlaceholder",
        "WORLD_BUG_PLACEHOLDER_SOURCE_STALE", message); };
    if (!scene || !scene->sceneVersion || !frame.contentVersion
        || frame.sceneVersion != scene->sceneVersion || frame.documentRevision != scene->documentRevision
        || frame.states.size() != scene->specs.size()
        || frame.poses.size() != frame.instances.size() || frame.instances.size() != scene->geometry.instances.size()
        || (!frame.requestVisibility.empty() && frame.requestVisibility.size() != scene->specs.size()))
        return fail("占位来源、完整状态或完成帧布局不匹配。");
    WorldModelBugPlaceholderFrameSnapshot snapshot;
    for (std::size_t i = 0; i < frame.states.size(); ++i)
        if (frame.states[i].identity != scene->specs[i].identity || !frame.states[i].identity.value
            || frame.states[i].identity.generation != scene->generation)
            return fail("占位完整状态的实例身份与当前请求代次不匹配。");
    snapshot.sourceScene = scene; snapshot.generation = scene->generation; snapshot.sourceIdentity = scene->sourceIdentity;
    snapshot.sceneVersion = frame.sceneVersion; snapshot.contentVersion = frame.contentVersion;
    snapshot.documentRevision = frame.documentRevision; snapshot.stateCount = frame.states.size(); snapshot.instanceCount = frame.instances.size();
    snapshot.ownedBytes = sizeof(snapshot) + snapshot.sourceIdentity.capacity() + 1;
    if (snapshot.ownedBytes > kMaximumWorldModelBugSnapshotBytes)
        return Output::Failure("World.BugPlaceholder", "WORLD_BUG_PLACEHOLDER_SNAPSHOT_BUDGET", "占位来源身份超出独立CPU快照预算。");
    std::size_t observationStringBytes = 0;
    const auto append = [&](std::uint32_t request, const War3RenderCore::InstanceIdentity& identity,
                            std::string_view code, std::string_view message) -> bool {
        const bool unknown = code.empty() || code.find("UNKNOWN") != code.npos;
        if (request >= scene->specs.size() || (!unknown && NonFailure(code)) || !HasPath(scene->specs[request].model.modelPath)) return true;
        if (code.size() > kMaximumWorldModelBugSnapshotBytes || message.size() > kMaximumWorldModelBugSnapshotBytes
            || snapshot.observations.size() >= kMaximumWorldModelBugSnapshotBytes / sizeof(WorldModelBugObservation)) return false;
        const auto& state = frame.states[request];
        snapshot.observations.push_back({identity, state.identity, request,
            {state.transform[3], state.transform[7], state.transform[11]}, std::string(code), std::string(message),
            frame.requestVisibility.empty() || frame.requestVisibility[request] != 0});
        const auto& added = snapshot.observations.back();
        observationStringBytes += added.reasonCode.capacity() + added.reason.capacity() + 2;
        snapshot.ownedBytes = sizeof(snapshot) + snapshot.sourceIdentity.capacity() + 1
            + snapshot.observations.capacity() * sizeof(WorldModelBugObservation) + observationStringBytes;
        return snapshot.ownedBytes <= kMaximumWorldModelBugSnapshotBytes;
    };
    for (const auto& missing : scene->unavailableInstances)
        if (!append(missing.requestIndex, missing.identity, missing.code, "原模型资产未被场景接收；保留原始拒绝代码。"))
            return Output::Failure("World.BugPlaceholder", "WORLD_BUG_PLACEHOLDER_SNAPSHOT_BUDGET", "占位来源观察超出独立CPU快照预算。");
    for (std::size_t index = 0; index < frame.poses.size(); ++index) {
        const auto& pose = frame.poses[index];
        if (!pose.hidden || pose.outsideView || pose.staticForBudget || !pose.rejectionCode.empty()
            || pose.rejection.code == War3RenderCore::Code::Ok) continue;
        const auto model = frame.instances[index].instance.model;
        if (model >= scene->models.size() || !scene->models[model].model.meshCount || !pose.alphaCount) continue;
        if (pose.renderedInstance != index || index >= scene->geometry.instanceRequests.size()
            || scene->geometry.instanceRequests[index] != pose.requestIndex)
            return fail("整模型姿势拒绝与渲染槽不一致。");
        if (!append(pose.requestIndex, pose.identity, War3RenderCore::CodeName(pose.rejection.code),
                    War3RenderCore::CodeMessage(pose.rejection.code)))
            return Output::Failure("World.BugPlaceholder", "WORLD_BUG_PLACEHOLDER_SNAPSHOT_BUDGET", "占位来源观察超出独立CPU快照预算。");
    }
    return Output::Success(std::move(snapshot));
} catch (...) {
    return Result<WorldModelBugPlaceholderFrameSnapshot>::Failure("World.BugPlaceholder", "WORLD_BUG_PLACEHOLDER_SNAPSHOT_FAILED",
        "无法拥有完整扫描后的占位来源观察，未使用不完整快照。");
}
Result<WorldModelBugPlaceholderPlan> PlanWorldModelBugPlaceholders(
    std::shared_ptr<const Render::AnimatedModelScene> scene, const Render::AnimationFrame& frame,
    std::shared_ptr<const WorldInstanceRegistry> registry, std::span<const MapData::PlacementId> owners,
    const WorldModelBugFlagsReader& flags) {
    auto snapshot = CaptureWorldModelBugPlaceholderFrame(scene, frame);
    if (!snapshot.Succeeded()) return {std::nullopt, std::move(snapshot.diagnostics)};
    return PlanWorldModelBugPlaceholders(std::move(scene), *snapshot.value, std::move(registry), owners, flags);
}
namespace {
Result<WorldModelBugPlaceholderPlan> PlanObservations(
    std::shared_ptr<const Render::AnimatedModelScene> scene, const WorldModelBugPlaceholderFrameSnapshot& frame,
    std::shared_ptr<const WorldInstanceRegistry> registry, std::span<const MapData::PlacementId> owners,
    const WorldModelBugFlagsReader& flags, std::shared_ptr<const WorldModelBugReadOnlySource> readOnly = {}) try {
    using Output = Result<WorldModelBugPlaceholderPlan>;
    const auto fail = [](const char* message) { return Output::Failure("World.BugPlaceholder",
        "WORLD_BUG_PLACEHOLDER_SOURCE_STALE", message); };
    const auto ownerCount = readOnly ? readOnly->Placements().size() : owners.size();
    if (!scene || (!registry && !readOnly) || (registry && readOnly) || !flags
        || frame.sourceScene.lock() != scene || !scene->sceneVersion || !frame.contentVersion
        || frame.sceneVersion != scene->sceneVersion || frame.documentRevision != scene->documentRevision
        || frame.generation != scene->generation || frame.sourceIdentity != scene->sourceIdentity
        || frame.stateCount != scene->specs.size() || frame.instanceCount != scene->geometry.instances.size()
        || ownerCount > scene->specs.size())
        return fail("占位来源、完整扫描快照或完成帧布局不匹配。");
    const auto& binding = registry ? registry->Binding() : readOnly->Binding();
    if ((registry && !binding.editSessionIdentity) || binding.sourceIdentity.empty() || binding.sourceIdentity != scene->sourceIdentity
        || binding.sourceGeneration != scene->generation || binding.mapRevision != scene->documentRevision
        || (registry && registry->CapturedSpecs().size() < ownerCount)
        || (readOnly && !readOnly->Matches(scene)))
        return fail("占位实例谱系与当前地图、会话或来源代次不一致。");
    WorldModelBugPlaceholderPlan plan{binding, registry, scene, frame.contentVersion, {}};
    plan.readOnlySource = readOnly;
    std::set<std::uint32_t> added;
    const auto append = [&](const WorldModelBugObservation& observation) -> bool {
        const auto request = observation.request; const auto& identity = observation.identity;
        const auto& code = observation.reasonCode; const auto& message = observation.reason;
        if (request >= ownerCount || request >= scene->specs.size()) return !readOnly;
        if (readOnly && (code.empty() || code.find("UNKNOWN") != code.npos)) return false;
        if (NonFailure(code)) return true;
        const auto& spec = scene->specs[request];
        const auto& owner = readOnly ? readOnly->Placements()[request].owner : owners[request];
        if (!HasPath(spec.model.modelPath)) return true;
        if (spec.identity != identity || observation.stateIdentity != identity) return false;
        if (registry) {
            const auto registered = registry->Find(owner); const auto& captured = registry->CapturedSpecs()[request];
            if (!registered || *registered != identity || captured.identity != identity || captured.hostKey != spec.hostKey) return false;
        } else {
            const auto& captured = readOnly->Placements()[request];
            if (captured.owner != owner || captured.identity != identity || captured.hostKey != spec.hostKey
                || captured.assetKey != Render::ModelAssetKey(spec.model)) return false;
        }
        if (!observation.inView) return true;
        const auto policy = flags(owner);
        if (!policy) return false;
        if (!policy->visible || added.contains(request)) return true;
        // ModelInstanceTransform及AnimationFrame实例都是row-major 3x4，位移列为3/7/11。
        const auto& position = observation.position;
        if (!std::all_of(position.begin(), position.end(), [](float value) { return std::isfinite(value); })) return false;
        added.insert(request);
        plan.markers.push_back({owner, identity, request, position, spec.model.modelPath, code, message,
            registry && !policy->locked});
        return true;
    };
    for (const auto& observation : frame.observations) {
        if (!append(observation))
            return fail("整模型姿势拒绝没有当前PlacementId或显隐来源。");
    }
    return Output::Success(std::move(plan));
} catch (...) {
    return Result<WorldModelBugPlaceholderPlan>::Failure("World.BugPlaceholder", "WORLD_BUG_PLACEHOLDER_POLICY_FAILED",
        "无法冻结真实占位来源与显隐/锁定，未使用猜测身份。");
}
} // namespace
Result<WorldModelBugPlaceholderPlan> PlanWorldModelBugPlaceholders(
    std::shared_ptr<const Render::AnimatedModelScene> scene, const WorldModelBugPlaceholderFrameSnapshot& frame,
    std::shared_ptr<const WorldInstanceRegistry> registry, std::span<const MapData::PlacementId> owners,
    const WorldModelBugFlagsReader& flags) {
    return PlanObservations(std::move(scene), frame, std::move(registry), owners, flags);
}
Result<WorldModelBugPlaceholderPlan> PlanWorldModelBugReadOnlyPlaceholders(
    std::shared_ptr<const Render::AnimatedModelScene> scene, const WorldModelBugPlaceholderFrameSnapshot& frame,
    std::shared_ptr<const WorldModelBugReadOnlySource> source, const WorldModelBugFlagsReader& flags) {
    if (!source) return Result<WorldModelBugPlaceholderPlan>::Failure("World.BugPlaceholder",
        "WORLD_BUG_PLACEHOLDER_READONLY_SOURCE_UNPROVEN", "只读占位缺少已验证原始摆放来源。");
    return PlanObservations(std::move(scene), frame, {}, {}, flags, std::move(source));
}

Result<WorldModelBugPlaceholderPlan> PlanWorldModelBugPlaceholderAbsence(
    std::shared_ptr<const Render::AnimatedModelScene> scene, const Render::AnimationFrame& frame) try {
    using Output = Result<WorldModelBugPlaceholderPlan>;
    const auto fail = [] { return Output::Failure("World.BugPlaceholder", "WORLD_BUG_PLACEHOLDER_ABSENCE_UNPROVEN",
        "只读场景的资产、完整姿势或来源无法证明无需占位；没有把缺少实例谱系当作空计划。"); };
    if (!scene || !scene->generation || scene->sourceIdentity.empty() || !scene->sceneVersion || !frame.contentVersion
        || frame.sceneVersion != scene->sceneVersion || frame.documentRevision != scene->documentRevision
        || frame.states.size() != scene->specs.size() || !scene->unavailableInstances.empty()
        || frame.instances.size() != scene->geometry.instances.size() || frame.poses.size() != frame.instances.size()
        || scene->geometry.instanceRequests.size() != frame.instances.size()
        || scene->requestGeometryRequired.size() != scene->specs.size()
        || scene->particleCapabilities.size() != scene->specs.size() || scene->sequences.size() != scene->specs.size()
        || scene->geometry.requestedInstances != scene->specs.size() || scene->geometry.skippedInstances
        || scene->models.size() != scene->geometry.models.size() || scene->pickModels.size() != scene->models.size()
        || scene->geometry.modelPaths.size() != scene->models.size() || scene->geometry.modelAssetKeys.size() != scene->models.size()
        || scene->meshes.size() != scene->geometry.meshes.size() || scene->geometry.meshGeosets.size() != scene->meshes.size()
        || (!frame.requestVisibility.empty() && frame.requestVisibility.size() != scene->specs.size())) return fail();
    for (std::size_t i = 0; i < frame.states.size(); ++i)
        if (frame.states[i].identity != scene->specs[i].identity || !frame.states[i].identity.value
            || frame.states[i].identity.generation != scene->generation || scene->requestGeometryRequired[i] > 1) return fail();
    std::vector<std::uint8_t> covered(scene->specs.size(), 0);
    for (std::size_t i = 0; i < frame.poses.size(); ++i) {
        const auto& pose = frame.poses[i]; const auto& instance = frame.instances[i];
        const auto model = instance.instance.model;
        if (pose.renderedInstance != i || pose.requestIndex != scene->geometry.instanceRequests[i]
            || pose.rejection.code != War3RenderCore::Code::Ok || !pose.rejectionCode.empty()
            || model >= scene->models.size() || model != scene->geometry.instances[i].model) return fail();
        const auto& descriptor = scene->models[model]; const auto& raw = scene->geometry.models[model];
        if (!raw.meshCount || descriptor.model.firstMesh != raw.firstMesh || descriptor.model.meshCount != raw.meshCount
            || raw.firstMesh > scene->meshes.size() || raw.meshCount > scene->meshes.size() - raw.firstMesh
            || pose.groupCount != descriptor.groupCount || pose.alphaCount != raw.meshCount
            || instance.firstGroup != pose.firstGroup || instance.firstAlpha != pose.firstAlpha
            || pose.firstGroup > frame.groups.size() || pose.groupCount > frame.groups.size() - pose.firstGroup
            || pose.firstAlpha > frame.alphas.size() || pose.alphaCount > frame.alphas.size() - pose.firstAlpha) return fail();
        if (pose.requestIndex == Render::kBakedInstance) {
            if (pose.identity != War3RenderCore::InstanceIdentity{} || !BakedMapping(*scene, i, model)) return fail();
        } else {
            const auto request = pose.requestIndex;
            if (request >= frame.states.size() || covered[request] || !scene->requestGeometryRequired[request]
                || pose.identity != frame.states[request].identity
                || scene->geometry.modelAssetKeys[model] != Render::ModelAssetKey(scene->specs[request].model)) return fail();
            covered[request] = 1;
        }
    }
    for (std::size_t request = 0; request < covered.size(); ++request) {
        if (covered[request]) continue;
        const auto& capability = scene->particleCapabilities[request];
        // 实际drawable=0才可省略实体网格；混合模型不能借粒子能力掩盖丢失的网格。
        // 纯粒子的本帧预算省略仍属正常预览，不要求必须产生粒子顶点或逐实例link。
        if (scene->requestGeometryRequired[request] || scene->specs[request].model.ownedStaticAsset
            || !scene->particlesEnabled || (!capability.particle2Count && !capability.ribbonCount)
            || (!capability.available && capability.rejectionCode != "ANIMATION_PARTICLE_INSTANCE_BUDGET")) return fail();
    }
    WorldModelBugPlaceholderPlan plan;
    plan.binding = {scene->generation, scene->documentRevision, 0, 0, scene->sourceIdentity};
    plan.sourceScene = std::move(scene); plan.completedContentVersion = frame.contentVersion;
    return Output::Success(std::move(plan));
} catch (...) {
    return Result<WorldModelBugPlaceholderPlan>::Failure("World.BugPlaceholder", "WORLD_BUG_PLACEHOLDER_ABSENCE_FAILED",
        "无法核对只读场景的完整占位需求，未生成空计划。");
}

Result<WorldModelBugPlaceholderCapturedFacts> QualifyWorldModelBugPlaceholderCapture(
    const WorldModelBugPlaceholderPlan& plan, const Render::CapturedFrameRgba& capture) try {
    using Output = Result<WorldModelBugPlaceholderCapturedFacts>;
    const auto fail = [](const char* text) { return Output::Failure("World.BugPlaceholder",
        "WORLD_BUG_PLACEHOLDER_CAPTURE_BINDING_INVALID", text); };
    const auto& scene = plan.sourceScene;
    const auto& main = capture.animation;
    const auto& view = capture.view;
    if (!scene || (plan.registry && (plan.binding != plan.registry->Binding() || !plan.binding.editSessionIdentity))
        || (plan.readOnlySource && (plan.registry || !plan.readOnlySource->Matches(scene)
            || plan.binding != plan.readOnlySource->Binding()))
        || plan.binding.sourceIdentity.empty()
        || plan.binding.sourceGeneration != scene->generation || plan.binding.sourceIdentity != scene->sourceIdentity
        || plan.binding.mapRevision != scene->documentRevision || !scene->sceneVersion || !plan.completedContentVersion
        || !main || main->sceneVersion != scene->sceneVersion || main->documentRevision != scene->documentRevision
        || main->contentVersion < plan.completedContentVersion || main->states.size() != scene->specs.size()
        || !view.identity.surfaceEpoch || !view.identity.sceneEpoch || !view.identity.viewEpoch || !view.identity.frameCount
        || !view.physicalWidth || !view.physicalHeight || view.physicalWidth > 4096 || view.physicalHeight > 4096
        || !std::all_of(view.rowMajorViewProjection.begin(), view.rowMajorViewProjection.end(), [](float v) { return std::isfinite(v); })
        || capture.rgba.size() != std::uint64_t(view.physicalWidth) * view.physicalHeight * 4
        || capture.rgba.size() > Render::kMaximumFrameCaptureBytes || !capture.skin || !capture.presentation
        || !capture.modelLayersRevision || capture.modelLayers.size() > WAR3_RENDER_MAX_MODEL_SCENE_LAYERS
        || capture.modelLayerFrames.size() != capture.modelLayers.size() || plan.markers.size() > 262144)
        return fail("完成截图、原地图会话、完整层表或准备计划版本不符。");
    if (!plan.registry && !plan.readOnlySource) {
        const auto absence = PlanWorldModelBugPlaceholderAbsence(scene, *main);
        if (!plan.markers.empty() || !absence.Succeeded() || absence.value->binding != plan.binding)
            return fail("缺少实例谱系时，须由同次完整 CPU 帧证明没有整模型占位需求；不能授权非空占位。");
    }
    const auto skinMatches = [&](const War3RenderSkinStatusV2& skin, const Render::AnimationFrame& frame) {
        return skin.structSize >= sizeof(skin) && !skin.result && skin.active == 1 && !skin.reserved
            && skin.frameCount == view.identity.frameCount && skin.sceneVersion == frame.sceneVersion
            && skin.contentVersion == frame.contentVersion && skin.staticUploadCount && skin.matrixUploadCount;
    };
    const auto& presentation = *capture.presentation;
    if (!skinMatches(*capture.skin, *main) || presentation.structSize < sizeof(presentation) || presentation.reserved
        || (presentation.mode != WAR3_EDITOR_PRESENTATION_SKINNED && presentation.mode != WAR3_EDITOR_PRESENTATION_PARTICLES)
        || !presentation.modelEpoch || !presentation.presentationRevision
        || presentation.sceneVersion != main->sceneVersion || presentation.contentVersion != main->contentVersion
        || presentation.instanceCount != main->instances.size() || presentation.particleDrawCount != main->particleDraws.size()
        || presentation.visibleInstanceCount > presentation.instanceCount
        || presentation.visibleParticleDrawCount > presentation.particleDrawCount)
        return fail("主动画上传、完成帧或呈现身份与同次截图不符。");
    std::set<std::uint64_t> layerIds;
    std::optional<std::size_t> markerLayer;
    for (std::size_t i = 0; i < capture.modelLayers.size(); ++i) {
        const auto& layer = capture.modelLayers[i]; const auto& frame = capture.modelLayerFrames[i];
        if (!layer.layerId || !layerIds.insert(layer.layerId).second || layer.structSize < sizeof(layer)
            || layer.result || !layer.revision || layer.parentModelEpoch != presentation.modelEpoch || !frame
            || !frame->sceneVersion || !frame->contentVersion || !skinMatches(layer.skin, *frame))
            return fail("附加层身份重复、上传未完成或层回执与同序拥有帧不一致。");
        if (layer.layerId == WorldModelBugPlaceholderLayerId) markerLayer = i;
    }
    WorldModelBugPlaceholderCapturedFacts facts;
    facts.frameCount = view.identity.frameCount; facts.parentModelEpoch = presentation.modelEpoch;
    facts.layersRevision = capture.modelLayersRevision; facts.preparedContentVersion = plan.completedContentVersion;
    facts.sourceContentVersion = main->contentVersion; facts.markerCount = static_cast<std::uint32_t>(plan.markers.size());
    if (plan.markers.empty()) {
        if (markerLayer) return fail("空占位计划仍捕获旧占位层，不能签收无占位。");
        return Output::Success(facts);
    }
    if (!markerLayer) return fail("有占位计划但同次截图缺少固定占位层。");
    const auto& layer = capture.modelLayers[*markerLayer]; const auto& frame = *capture.modelLayerFrames[*markerLayer];
    if (frame.documentRevision != plan.binding.mapRevision || frame.states.size() != plan.markers.size()
        || frame.instances.size() != plan.markers.size() || frame.poses.size() != plan.markers.size()
        || !frame.particleDraws.empty() || !frame.particleVertices.empty())
        return fail("占位层帧的地图修订、实例数或原创几何布局不同。");
    std::set<std::uint64_t> identities;
    for (std::size_t i = 0; i < plan.markers.size(); ++i) {
        const auto& marker = plan.markers[i]; const auto& pose = frame.poses[i]; const auto& state = frame.states[i];
        if (marker.request >= scene->specs.size()
            || (plan.registry && marker.request >= plan.registry->CapturedSpecs().size())
            || (plan.readOnlySource && marker.request >= plan.readOnlySource->Placements().size())
            || (!plan.registry && !plan.readOnlySource))
            return fail("占位原请求超出实际来源或实例谱系。");
        const auto& spec = scene->specs[marker.request];
        if (plan.registry) {
            const auto& registered = plan.registry->CapturedSpecs()[marker.request];
            const auto owner = plan.registry->Find(marker.owner);
            if (!owner || *owner != marker.identity || registered.identity != marker.identity || registered.hostKey != spec.hostKey)
                return fail("占位原始可编辑谱系与当前请求不一致。");
        } else {
            const auto& registered = plan.readOnlySource->Placements()[marker.request];
            if (marker.selectable || registered.owner != marker.owner || registered.identity != marker.identity
                || registered.hostKey != spec.hostKey || registered.assetKey != Render::ModelAssetKey(spec.model))
                return fail("只读占位原始记录、资产或请求身份失配，或被错误授予选择资格。");
        }
        const auto& sourceState = main->states[marker.request]; const auto& transform = frame.instances[i].instance.transform;
        if (!identities.insert(marker.identity.value).second || spec.identity != marker.identity
            || marker.modelPath != spec.model.modelPath || sourceState.identity != marker.identity
            || state.identity != marker.identity || pose.identity != marker.identity
            || pose.requestIndex != i || pose.renderedInstance != i || pose.hidden || pose.outsideView
            || pose.staticForBudget || pose.noVisibleGeometry || pose.rejection.code != War3RenderCore::Code::Ok
            || !pose.rejectionCode.empty() || !pose.groupCount || !pose.alphaCount
            || pose.firstGroup > frame.groups.size() || pose.groupCount > frame.groups.size() - pose.firstGroup
            || pose.firstAlpha > frame.alphas.size() || pose.alphaCount > frame.alphas.size() - pose.firstAlpha)
            return fail("占位同序姿势身份、原PlacementId、路径或实际可绘几何不同。");
        bool visibleAlpha = false;
        for (std::size_t a = pose.firstAlpha; a < std::size_t(pose.firstAlpha) + pose.alphaCount; ++a) {
            if (!std::isfinite(frame.alphas[a]) || frame.alphas[a] < 0 || frame.alphas[a] > 1)
                return fail("占位几何透明度不是有效完成内容。");
            visibleAlpha = visibleAlpha || frame.alphas[a] > 0;
        }
        if (!visibleAlpha) return fail("占位层本帧全透明，不能签收方块内容。");
        for (std::size_t axis = 0; axis < 3; ++axis) {
            const auto translation = axis * 4 + 3;
            if (!std::isfinite(marker.position[axis]) || marker.position[axis] != sourceState.transform[translation]
                || marker.position[axis] != state.transform[translation] || marker.position[axis] != transform[translation])
                return fail("占位原地图位置、CPU状态与实际上传实例位置不同。");
        }
    }
    facts.layerCaptured = true; facts.layerRevision = layer.revision; facts.layerSceneVersion = layer.skin.sceneVersion;
    facts.layerContentVersion = layer.skin.contentVersion; facts.staticUploadCount = layer.skin.staticUploadCount;
    facts.matrixUploadCount = layer.skin.matrixUploadCount;
    return Output::Success(facts);
} catch (...) {
    return Result<WorldModelBugPlaceholderCapturedFacts>::Failure("World.BugPlaceholder",
        "WORLD_BUG_PLACEHOLDER_CAPTURE_BINDING_FAILED", "无法拥有占位同帧绑定结果，未签收绘制证明。");
}
} // namespace War3Map::Services
