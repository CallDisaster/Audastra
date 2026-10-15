#include "../WorldModelBugPlaceholderFixture.h"
#include "../AnimationParticleContinuityTests.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/Render/ModelPicking.h"
#include <cstring>
#include <iostream>
#include <limits>

using namespace WorldModelBugPlaceholderTest;
namespace {
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void Classification() {
    Fixture placed({5952, 1984, 96}, 0.73f); placed.PoseFailure(); const auto placedPlan = placed.Plan();
    const auto expectedTransform = Render::ModelInstanceTransform(placed.scene->specs[0].model);
    Check(placed.frame->states[0].transform == expectedTransform && expectedTransform[1] != 0 && expectedTransform[4] != 0
        && expectedTransform[3] == 5952 && expectedTransform[7] == 1984 && expectedTransform[11] == 96,
        "real service fixture did not retain nonzero row-major translation and rotation");
    Check(placedPlan.markers.size() == 1 && placedPlan.markers[0].position == std::array<float, 3>{5952, 1984, 96},
        "marker lost source XYZ by reading linear coefficients as translation");
    Fixture f;
    Check(f.Plan().markers.empty(), "valid owned models became errors");
    f.PoseFailure(); auto plan = f.Plan();
    Check(plan.markers.size() == 1 && plan.markers[0].owner == f.owners[0]
        && plan.markers[0].identity == f.scene->specs[0].identity && plan.markers[0].reasonCode == "MDX_POSE_KEYS_INVALID"
        && plan.completedContentVersion == f.frame->contentVersion, "whole pose rejection lost typed owner/reason");
    f.policy[0].locked = true; Check(!f.Plan().markers.front().selectable, "locked failure became selectable");
    f.policy[0].visible = false; Check(f.Plan().markers.empty(), "user hidden failure still emitted placeholder");
    f.policy[0] = {};
    const auto saved = f.frame->poses[0];
    f.frame->poses[0].outsideView = true; Check(f.Plan().markers.empty(), "outside view became model failure");
    f.frame->poses[0] = saved; f.frame->poses[0].staticForBudget = true;
    Check(f.Plan().markers.empty(), "budget retention became model failure");
    f.frame->poses[0] = saved; f.frame->poses[0].hidden = false; f.frame->poses[0].partial = true;
    f.frame->poses[0].noVisibleGeometry = true; Check(f.Plan().markers.empty(), "natural transparency/local GEOA became whole failure");
    f.frame->poses[0] = saved; f.frame->poses[0].rejectionCode = "ANIMATION_PARTICLE_FAILED";
    Check(f.Plan().markers.empty(), "particle component failure became whole model placeholder");
    f.frame->poses[0] = saved; f.frame->poses[0].alphaCount = 0;
    Check(f.Plan().markers.empty(), "pure particle model became geometry failure");
    f.frame->poses[0] = saved;
    const auto path = f.scene->specs[0].model.modelPath;
    f.scene->specs[0].model.modelPath = "_"; Check(f.Plan().markers.empty(), "intentional underscore path became error");
    f.scene->specs[0].model.modelPath.clear(); Check(f.Plan().markers.empty(), "empty umdl became error");
    f.scene->specs[0].model.modelPath = path;
    auto owner = f.owners[0]; ++f.owners[0].creationId;
    Check(!PlanWorldModelBugPlaceholders(f.scene, *f.frame, f.registry, f.owners, f.Flags()).Succeeded(), "reused row accepted wrong typed owner");
    f.owners[0] = owner;
    f.frame->states[0].identity.value += 1;
    Check(!PlanWorldModelBugPlaceholders(f.scene, *f.frame, f.registry, f.owners, f.Flags()).Succeeded(), "wrong state identity accepted");
    f.frame->states[0].identity = f.scene->specs[0].identity;
    f.scene->sourceIdentity = "changed-source";
    Check(!PlanWorldModelBugPlaceholders(f.scene, *f.frame, f.registry, f.owners, f.Flags()).Succeeded(), "source hash mismatch accepted");
    Fixture missing; const auto id = missing.scene->specs[0].identity;
    missing.scene->unavailableInstances.push_back({id, {}, 0, "ANIMATION_ASSET_BUDGET"});
    Check(missing.Plan().markers.empty(), "asset budget emitted marker");
    missing.scene->unavailableInstances.back().code = "RENDER_CORE_SOURCE_ORDER_KEY_LIMIT";
    Check(missing.Plan().markers.empty(), "source-order work cap emitted model defect marker");
    missing.scene->unavailableInstances.back().code = "MDX_READ_FAILED";
    Check(missing.Plan().markers.size() == 1, "actual whole asset unavailable entry omitted");
    Fixture compact; compact.PoseFailure();
    const auto snapshot = Good(CaptureWorldModelBugPlaceholderFrame(compact.scene, *compact.frame));
    const auto frozenPlan = Good(PlanWorldModelBugPlaceholders(compact.scene, snapshot, compact.registry, compact.owners, compact.Flags()));
    Check(frozenPlan.markers == compact.Plan().markers && snapshot.observations.size() == 1
        && snapshot.ownedBytes < 4096, "compact snapshot lost classification or copied full geometry");
    compact.frame->states[0].transform[3] += 123;
    Check(Good(PlanWorldModelBugPlaceholders(compact.scene, snapshot, compact.registry, compact.owners, compact.Flags())).markers == frozenPlan.markers,
        "borrowed compact observation changed with a later CPU frame");
    auto stale = snapshot; ++stale.generation;
    Check(!PlanWorldModelBugPlaceholders(compact.scene, stale, compact.registry, compact.owners, compact.Flags()).Succeeded(),
        "compact source generation mismatch accepted");
    stale = snapshot; stale.sourceIdentity = "old-source";
    Check(!PlanWorldModelBugPlaceholders(compact.scene, stale, compact.registry, compact.owners, compact.Flags()).Succeeded(),
        "compact source identity mismatch accepted");
    stale = snapshot; --stale.instanceCount;
    Check(!PlanWorldModelBugPlaceholders(compact.scene, stale, compact.registry, compact.owners, compact.Flags()).Succeeded(),
        "compact incomplete instance coverage accepted");
    compact.policy[0].visible = false;
    Check(Good(PlanWorldModelBugPlaceholders(compact.scene, snapshot, compact.registry, compact.owners, compact.Flags())).markers.empty(),
        "compact snapshot bypassed current owner visibility");
    std::cout << "classification: real registry identity; synthetic whole-pose/asset failures and negative causes\n";
}
// 拥有真实原创模型服务产生的实例/姿势；完成回执及RGBA是合成合同输入，不是GPU证明。
Render::CapturedFrameRgba SyntheticCapture(Fixture& fixture, const WorldModelBugPlaceholderPlan& plan) {
    std::vector<Render::AnimationInstanceSpec> specs;
    for (const auto& marker : plan.markers) {
        Render::AnimationInstanceSpec spec; spec.identity = marker.identity;
        spec.hostKey = "bug-placeholder:" + std::to_string(marker.identity.value);
        spec.model.modelPath = marker.modelPath; spec.model.ownedStaticAsset = fixture.cube;
        spec.model.position = marker.position; spec.model.scale = {8, 8, 8}; specs.push_back(std::move(spec));
    }
    Render::CapturedFrameRgba capture; capture.animation = fixture.frame;
    capture.documentRevision = 99; // 视口地形展示修订，与动画地图修订0是两个空间。
    capture.view.identity = {1, 2, 3, 4}; capture.view.physicalWidth = capture.view.physicalHeight = 64;
    auto& matrix = capture.view.rowMajorViewProjection; matrix[0] = matrix[5] = matrix[10] = matrix[15] = 1;
    capture.rgba.resize(64 * 64 * 4); capture.modelLayersRevision = 20;
    capture.skin = War3RenderSkinStatusV2{sizeof(War3RenderSkinStatusV2), 0, 1, 0, 4,
        fixture.frame->sceneVersion, fixture.frame->contentVersion, 1, 1};
    capture.presentation = War3RenderEditorPresentationStatusV2{sizeof(War3RenderEditorPresentationStatusV2),
        WAR3_EDITOR_PRESENTATION_SKINNED, static_cast<std::uint32_t>(fixture.frame->instances.size()), 0,
        0, 0, 0, 0, 6, fixture.frame->sceneVersion, fixture.frame->contentVersion, 7};
    if (!specs.empty()) {
        const auto service = Good(Render::ModelAnimationService::Create({fixture.scene->generation,
            fixture.scene->documentRevision, fixture.scene->sourceIdentity}, specs,
            [](std::string_view, std::uint64_t) { return Result<Render::AnimationResource>::Failure("Fixture", "UNEXPECTED_READ", "no MDX read"); }));
        const auto frame = Good(service->Prepare(nullptr, {}));
        War3RenderModelLayerStatusV2 layer{}; layer.structSize = sizeof(layer);
        layer.layerId = WorldModelBugPlaceholderLayerId; layer.parentModelEpoch = 6; layer.revision = 8;
        layer.skin = {sizeof(War3RenderSkinStatusV2), 0, 1, 0, 4, frame->sceneVersion, frame->contentVersion, 1, 1};
        capture.modelLayers.push_back(layer); capture.modelLayerFrames.push_back(frame);
    }
    return capture;
}
void CaptureBindingContract() {
    Fixture fixture({5952, 1984, 96}, 0.73f); fixture.PoseFailure(0); fixture.PoseFailure(1);
    const auto plan = fixture.Plan(); const auto capture = SyntheticCapture(fixture, plan);
    const auto facts = Good(QualifyWorldModelBugPlaceholderCapture(plan, capture));
    Check(facts.layerCaptured && facts.markerCount == 2 && facts.frameCount == 4 && facts.parentModelEpoch == 6
        && facts.layersRevision == 20 && facts.layerRevision == 8 && facts.staticUploadCount == 1 && facts.matrixUploadCount == 1,
        "owned marker frame and synthetic capture binding rejected");
    const auto reject = [&](auto mutate, const char* message) {
        auto candidate = capture; mutate(candidate);
        Check(!QualifyWorldModelBugPlaceholderCapture(plan, candidate).Succeeded(), message);
    };
    reject([](auto& c) { c.modelLayers.clear(); c.modelLayerFrames.clear(); }, "missing marker layer accepted");
    reject([](auto& c) { c.modelLayers.push_back(c.modelLayers[0]); c.modelLayerFrames.push_back(c.modelLayerFrames[0]); }, "duplicate marker layer accepted");
    reject([](auto& c) { c.modelLayerFrames.clear(); }, "missing parallel owned frame accepted");
    reject([](auto& c) { c.modelLayers[0].layerId++; }, "different fixed layer ID accepted");
    reject([](auto& c) { c.modelLayers[0].parentModelEpoch++; }, "old parent model epoch accepted");
    reject([](auto& c) { c.modelLayers[0].revision = 0; }, "zero layer revision accepted");
    reject([](auto& c) { c.modelLayers[0].result = -1; }, "failed aggregate layer accepted");
    reject([](auto& c) { c.modelLayers[0].skin.result = 2; }, "pending marker upload accepted");
    reject([](auto& c) { c.modelLayers[0].skin.active = 0; }, "inactive marker layer accepted");
    reject([](auto& c) { c.modelLayers[0].skin.frameCount++; }, "different draw frame accepted");
    reject([](auto& c) { c.modelLayers[0].skin.sceneVersion++; }, "different owned scene accepted");
    reject([](auto& c) { c.modelLayers[0].skin.contentVersion++; }, "different owned content accepted");
    reject([](auto& c) { c.modelLayers[0].skin.matrixUploadCount = 0; }, "no successful matrix upload accepted");
    reject([](auto& c) { c.modelLayers[0].skin.staticUploadCount = 0; }, "no successful static upload accepted");
    reject([](auto& c) { c.skin->frameCount++; }, "old main completion accepted");
    reject([](auto& c) { c.presentation->modelEpoch++; }, "wrong main epoch accepted");
    reject([](auto& c) { c.presentation->contentVersion++; }, "wrong presentation content accepted");
    reject([](auto& c) { c.rgba.pop_back(); }, "short RGBA accepted");
    reject([](auto& c) { c.view.identity.viewEpoch = 0; }, "unbound view epoch accepted");
    reject([](auto& c) { c.view.rowMajorViewProjection[0] = std::numeric_limits<float>::infinity(); }, "nonfinite view accepted");
    const auto rejectFrame = [&](auto mutate, const char* message) {
        reject([&](auto& c) {
            auto frame = std::make_shared<Render::AnimationFrame>(*c.modelLayerFrames[0]); mutate(*frame); c.modelLayerFrames[0] = frame;
        }, message);
    };
    rejectFrame([](auto& f) { f.documentRevision++; }, "different marker map revision accepted");
    rejectFrame([](auto& f) { f.states[0].identity.value++; }, "wrong state identity accepted");
    rejectFrame([](auto& f) { std::swap(f.poses[0], f.poses[1]); }, "reordered identities accepted");
    rejectFrame([](auto& f) { f.poses[0].requestIndex++; }, "original request confused with local slot accepted");
    rejectFrame([](auto& f) { f.states[0].transform[3]++; }, "different state X accepted");
    rejectFrame([](auto& f) { f.instances[0].instance.transform[7]++; }, "different actual instance Y accepted");
    rejectFrame([](auto& f) { f.instances[0].instance.transform[11]++; }, "different actual instance Z accepted");
    rejectFrame([](auto& f) { f.poses[0].hidden = true; }, "hidden owned geometry accepted");
    rejectFrame([](auto& f) { std::fill(f.alphas.begin(), f.alphas.end(), 0); }, "transparent marker accepted");
    auto shiftedPlan = plan; shiftedPlan.markers[0].position[0]++;
    Check(!QualifyWorldModelBugPlaceholderCapture(shiftedPlan, capture).Succeeded(), "plan position unrelated to source accepted");
    auto later = capture; auto main = std::make_shared<Render::AnimationFrame>(*capture.animation); main->contentVersion++;
    later.animation = main; later.skin->contentVersion = main->contentVersion; later.presentation->contentVersion = main->contentVersion;
    const auto laterFacts = Good(QualifyWorldModelBugPlaceholderCapture(plan, later));
    Check(laterFacts.preparedContentVersion + 1 == laterFacts.sourceContentVersion, "stable marker plan cannot span later accepted content");
    main->states[0].transform[3]++;
    Check(!QualifyWorldModelBugPlaceholderCapture(plan, later).Succeeded(), "changed actual original X accepted against stale markers");
    Fixture healthy; const auto emptyPlan = healthy.Plan(); const auto emptyCapture = SyntheticCapture(healthy, emptyPlan);
    Check(!Good(QualifyWorldModelBugPlaceholderCapture(emptyPlan, emptyCapture)).layerCaptured, "zero marker plan invented captured layer");
    auto staleLayer = emptyCapture; staleLayer.modelLayers = capture.modelLayers; staleLayer.modelLayerFrames = capture.modelLayerFrames;
    Check(!QualifyWorldModelBugPlaceholderCapture(emptyPlan, staleLayer).Succeeded(), "zero marker plan ignored old fixed layer");
    const auto readOnlyPlan = Good(PlanWorldModelBugPlaceholderAbsence(healthy.scene, *healthy.frame));
    Check(!readOnlyPlan.registry && readOnlyPlan.markers.empty() && !readOnlyPlan.binding.editSessionIdentity,
        "read-only absence plan invented editable lineage");
    Check(!Good(QualifyWorldModelBugPlaceholderCapture(readOnlyPlan, emptyCapture)).layerCaptured,
        "healthy complete read-only frame cannot prove no placeholder layer");
    Check(!QualifyWorldModelBugPlaceholderCapture(readOnlyPlan, staleLayer).Succeeded(),
        "read-only absence ignored old fixed placeholder layer");
    auto nonemptyReadOnlyPlan = readOnlyPlan; nonemptyReadOnlyPlan.markers = plan.markers;
    Check(!QualifyWorldModelBugPlaceholderCapture(nonemptyReadOnlyPlan, emptyCapture).Succeeded(),
        "missing registry authorized nonempty placeholder markers");
    auto wrongReadOnlySource = readOnlyPlan; wrongReadOnlySource.binding.sourceIdentity += "-other";
    Check(!QualifyWorldModelBugPlaceholderCapture(wrongReadOnlySource, emptyCapture).Succeeded(),
        "read-only absence accepted different map source");
    auto partialFrame = *healthy.frame; partialFrame.poses.clear(); partialFrame.instances.clear();
    Check(!PlanWorldModelBugPlaceholderAbsence(healthy.scene, partialFrame).Succeeded(),
        "partial read-only frame was treated as empty fault list");
    auto wrongIdentity = *healthy.frame; wrongIdentity.states[0].identity.value++;
    Check(!PlanWorldModelBugPlaceholderAbsence(healthy.scene, wrongIdentity).Succeeded(),
        "read-only absence accepted unrelated request state");
    Render::BakedModelMesh triangle;
    triangle.vertices = {{-10, 0, 0, 0, 0, 1, 0, 0}, {10, 0, 0, 0, 0, 1, 1, 0}, {0, 10, 0, 0, 0, 1, 0, 1}};
    triangle.indices = {0, 1, 2};
    const auto bakedService = Good(Render::ModelAnimationService::Create({7, 0, "original-baked-source"}, {},
        [](std::string_view, std::uint64_t) {
            return Result<Render::AnimationResource>::Failure("Fixture", "UNEXPECTED_READ", "untextured baked fixture cannot read assets");
        }, {}, std::span(&triangle, 1)));
    const auto bakedScene = bakedService->Scene();
    auto bakedFrame = std::make_shared<Render::AnimationFrame>(*Good(bakedService->Prepare(nullptr)));
    Check(bakedScene->specs.empty() && bakedFrame->states.empty() && bakedFrame->instances.size() == 1
        && bakedScene->geometry.instanceRequests[0] == Render::kBakedInstance,
        "real baked service fixture lost its non-placement geometry");
    const auto bakedPlan = Good(PlanWorldModelBugPlaceholderAbsence(bakedScene, *bakedFrame));
    auto bakedCapture = emptyCapture; bakedCapture.animation = bakedFrame;
    bakedCapture.skin->sceneVersion = bakedFrame->sceneVersion; bakedCapture.skin->contentVersion = bakedFrame->contentVersion;
    bakedCapture.presentation->sceneVersion = bakedFrame->sceneVersion;
    bakedCapture.presentation->contentVersion = bakedFrame->contentVersion;
    bakedCapture.presentation->instanceCount = static_cast<std::uint32_t>(bakedFrame->instances.size());
    Check(!Good(QualifyWorldModelBugPlaceholderCapture(bakedPlan, bakedCapture)).layerCaptured,
        "host baked cliff geometry was mistaken for an unresolved map placement");
    bakedFrame->poses[0].identity = healthy.frame->states[0].identity;
    Check(!PlanWorldModelBugPlaceholderAbsence(bakedScene, *bakedFrame).Succeeded(),
        "baked sentinel authorized a selectable placement identity");
    bakedFrame->poses[0].identity = {};
    auto wrongBakedMapping = std::make_shared<Render::AnimatedModelScene>(*bakedScene);
    wrongBakedMapping->geometry.instanceRequests[0] = 0;
    Check(!PlanWorldModelBugPlaceholderAbsence(wrongBakedMapping, *bakedFrame).Succeeded(),
        "baked pose bypassed a mismatching geometry request mapping");
    healthy.PoseFailure();
    Check(!PlanWorldModelBugPlaceholderAbsence(healthy.scene, *healthy.frame).Succeeded()
        && !QualifyWorldModelBugPlaceholderCapture(readOnlyPlan, emptyCapture).Succeeded(),
        "actual read-only pose defect bypassed required editable lineage");
    Check(!PlanWorldModelBugPlaceholderAbsence({}, *healthy.frame).Succeeded(),
        "missing CPU scene was treated as no placeholder demand");
    std::cout << "capture-binding: real owned service frames; synthetic completion identity/XYZ/ordered-layer negative contracts, no GPU proof\n";
}
void ReadOnlyGeometryCoverage() {
    Fixture healthy;
    Check(healthy.scene->requestGeometryRequired.size() == healthy.scene->specs.size()
        && std::all_of(healthy.scene->requestGeometryRequired.cbegin(), healthy.scene->requestGeometryRequired.cend(),
            [](std::uint8_t required) { return required == 1; }), "owned model requests lost actual drawable provenance");
    auto badModel = *healthy.frame; badModel.instances[0].instance.model = UINT32_MAX;
    Check(!PlanWorldModelBugPlaceholderAbsence(healthy.scene, badModel).Succeeded(),
        "read-only absence accepted an actual uploaded model index outside the source geometry");
    auto badCapture = SyntheticCapture(healthy, healthy.Plan());
    badCapture.animation = std::make_shared<const Render::AnimationFrame>(badModel);
    Check(!QualifyWorldModelBugPlaceholderCapture(Good(PlanWorldModelBugPlaceholderAbsence(healthy.scene, *healthy.frame)), badCapture).Succeeded(),
        "capture qualification bypassed invalid actual uploaded model mapping");
    auto badRange = *healthy.frame; badRange.poses[0].firstGroup = static_cast<std::uint32_t>(badRange.groups.size() + 1);
    badRange.instances[0].firstGroup = badRange.poses[0].firstGroup;
    Check(!PlanWorldModelBugPlaceholderAbsence(healthy.scene, badRange).Succeeded(),
        "matching pose/instance offsets bypassed an invalid owned matrix range");
    Render::BakedModelMesh triangle;
    triangle.vertices = {{-10, 0, 0, 0, 0, 1, 0, 0}, {10, 0, 0, 0, 0, 1, 1, 0}, {0, 10, 0, 0, 0, 1, 0, 1}};
    triangle.indices = {0, 1, 2};
    const auto combined = Good(Render::ModelAnimationService::Create({7, 0, "original-combined-source"},
        std::span(healthy.scene->specs.data(), healthy.scene->specs.size()),
        [](std::string_view, std::uint64_t) {
            return Result<Render::AnimationResource>::Failure("Fixture", "UNEXPECTED_READ", "owned/baked fixture cannot read assets");
        }, {}, std::span(&triangle, 1)));
    const auto combinedFrame = Good(combined->Prepare(nullptr));
    Check(combined->Scene()->models.size() == 2 && Good(PlanWorldModelBugPlaceholderAbsence(combined->Scene(), *combinedFrame)).markers.empty(),
        "real host baked geometry could not coexist with ordinary drawable requests");
    auto swappedModel = *combinedFrame; swappedModel.instances[0].instance.model = 1;
    Check(!PlanWorldModelBugPlaceholderAbsence(combined->Scene(), swappedModel).Succeeded(),
        "valid model index from a different source slot bypassed actual geometry/model agreement");
    auto lostScene = std::make_shared<Render::AnimatedModelScene>(*healthy.scene);
    auto lostFrame = *healthy.frame;
    lostScene->geometry.instances.clear(); lostScene->geometry.instanceRequests.clear();
    lostFrame.instances.clear(); lostFrame.poses.clear();
    Check(!PlanWorldModelBugPlaceholderAbsence(lostScene, lostFrame).Succeeded(),
        "dropping every drawable request's geometry and pose manufactured an empty fault plan");
    auto legacyScene = std::make_shared<Render::AnimatedModelScene>(*healthy.scene);
    legacyScene->requestGeometryRequired.clear();
    Check(!PlanWorldModelBugPlaceholderAbsence(legacyScene, *healthy.frame).Succeeded(),
        "missing frozen drawable metadata silently opted a legacy scene out of coverage");
    auto forgedBakedScene = std::make_shared<Render::AnimatedModelScene>(*healthy.scene);
    auto forgedBakedFrame = *healthy.frame;
    forgedBakedScene->geometry.instanceRequests[0] = Render::kBakedInstance;
    forgedBakedFrame.poses[0].requestIndex = Render::kBakedInstance; forgedBakedFrame.poses[0].identity = {};
    Check(!PlanWorldModelBugPlaceholderAbsence(forgedBakedScene, forgedBakedFrame).Succeeded(),
        "ordinary owned model was relabelled as a host baked mesh with a zero pose identity");
    auto duplicateScene = std::make_shared<Render::AnimatedModelScene>(*healthy.scene);
    auto duplicateFrame = *healthy.frame;
    duplicateScene->geometry.instanceRequests[1] = 0;
    duplicateFrame.poses[1].requestIndex = 0; duplicateFrame.poses[1].identity = duplicateFrame.states[0].identity;
    Check(!PlanWorldModelBugPlaceholderAbsence(duplicateScene, duplicateFrame).Succeeded(),
        "duplicate coverage of one request hid a missing second request");

    const auto mixed = AnimationParticleContinuityTest::Model();
    auto pure = mixed;
    // 原创模型只去掉实体 GEOS/GEOA，保留真实 PRE2、节点、动作、材质和贴图。
    for (std::size_t at = 4; at + 8 <= pure.size();) {
        std::uint32_t size = 0; std::memcpy(&size, pure.data() + at + 4, 4);
        if (size > pure.size() - at - 8) throw std::runtime_error("original pure particle fixture is truncated");
        if (!std::memcmp(pure.data() + at, "GEOS", 4) || !std::memcmp(pure.data() + at, "GEOA", 4))
            pure.erase(pure.begin() + static_cast<std::ptrdiff_t>(at), pure.begin() + static_cast<std::ptrdiff_t>(at + 8 + size));
        else at += 8 + size;
    }
    const auto texture = AnimationParticleContinuityTest::Texture();
    std::size_t reads = 0;
    const auto read = [&](std::string_view path, std::uint64_t maximum) {
        ++reads;
        const auto& bytes = path == "original-mixed.mdx" ? mixed : path == "original-pure.mdx" ? pure : texture;
        if (bytes.size() > maximum) return Result<Render::AnimationResource>::Failure("Fixture", "RESOURCE_BUDGET", "original fixture exceeds read budget");
        return Result<Render::AnimationResource>::Success({bytes, "original-absence-layer", Good(Sha256(bytes))});
    };
    std::array<Render::AnimationInstanceSpec, 2> specs;
    for (std::size_t i = 0; i < specs.size(); ++i) {
        specs[i].identity = {i + 1, 7}; specs[i].hostKey = "absence:" + std::to_string(i);
        specs[i].model.modelPath = i ? "original-pure.mdx" : "original-mixed.mdx";
        specs[i].sequenceIndex = 0; specs[i].elapsedMs = 0; specs[i].globalMs = 0; specs[i].playing = true;
    }
    auto limits = Render::InteractiveAnimationPreviewLimits(); limits.enableParticles = true; limits.retainUnsupportedStatic = true;
    auto service = Good(Render::ModelAnimationService::Create({7, 0, "original-absence-source"}, specs, read, limits));
    Render::AnimationAdvance advance; advance.worldCamera = War3RenderCore::LocalCamera{{0, -100, 100}, {0, 0, 1}};
    auto frame = Good(service->Prepare(nullptr, advance)); const auto scene = service->Scene();
    Check(scene->requestGeometryRequired.size() == 2 && scene->requestGeometryRequired[0] == 1
        && scene->requestGeometryRequired[1] == 0 && frame->poses.size() == 1 && frame->particleLinks.size() == 2,
        "real mixed/pure service did not retain distinct drawable requirements");
    Check(Good(PlanWorldModelBugPlaceholderAbsence(scene, *frame)).markers.empty(),
        "healthy pure particles without entity geometry were rejected");
    const auto pureService = Good(Render::ModelAnimationService::Create({7, 0, "original-pure-only-source"},
        std::span(specs.data() + 1, 1), read, limits));
    const auto pureFrame = Good(pureService->Prepare(nullptr, advance));
    Check(pureFrame->instances.empty() && pureFrame->poses.empty()
        && Good(PlanWorldModelBugPlaceholderAbsence(pureService->Scene(), *pureFrame)).markers.empty(),
        "legitimate pure-particle-only scene was mistaken for dropped entity geometry");
    auto omittedMixedScene = std::make_shared<Render::AnimatedModelScene>(*scene);
    auto omittedMixedFrame = *frame;
    omittedMixedScene->geometry.instances.clear(); omittedMixedScene->geometry.instanceRequests.clear();
    omittedMixedFrame.instances.clear(); omittedMixedFrame.poses.clear();
    Check(!PlanWorldModelBugPlaceholderAbsence(omittedMixedScene, omittedMixedFrame).Succeeded(),
        "mixed model's particle capability disguised its missing entity geometry as pure particles");
    auto budgetScene = std::make_shared<Render::AnimatedModelScene>(*scene);
    // 以下只注入分类合同状态，不把它描述为实际触发了运行时预算或贴图故障。
    budgetScene->particleCapabilities[1].available = false;
    budgetScene->particleCapabilities[1].rejectionCode = "ANIMATION_PARTICLE_INSTANCE_BUDGET";
    Check(Good(PlanWorldModelBugPlaceholderAbsence(budgetScene, *frame)).markers.empty(),
        "pure particle preview budget omission became a missing entity model");
    budgetScene->particleCapabilities[1].rejectionCode = "PARTICLE_TEXTURE_MISSING";
    Check(!PlanWorldModelBugPlaceholderAbsence(budgetScene, *frame).Succeeded(),
        "missing pure particle source texture was treated as a normal budget omission");
    const auto initialReads = reads;
    Render::AnimationInstanceDelta delta; delta.session = {7, 1, "original-absence-source"}; delta.removals = {specs[0].identity};
    auto added = specs[0]; added.identity = {3, 7}; added.hostKey = "absence:added"; delta.additions.push_back(added);
    const auto catalog = Good(service->PrepareInstanceCatalog(delta, read));
    const auto applied = Good(service->PrepareInstanceDelta(*frame, *catalog, advance));
    Check(applied.service->Scene()->requestGeometryRequired.size() == 2
        && applied.service->Scene()->requestGeometryRequired[0] == 0
        && applied.service->Scene()->requestGeometryRequired[1] == 1 && reads == initialReads,
        "catalog delta did not rebuild survivor/addition drawable coverage from resident assets");
    Check(Good(PlanWorldModelBugPlaceholderAbsence(applied.service->Scene(), *applied.frame)).markers.empty(),
        "ordinary catalog delta invalidated healthy pure-particle coverage");
    std::cout << "read-only-coverage: real baked/mixed/pure CPU producers; synthetic malformed mappings rejected, no GPU proof\n";
}
void OwnedPicking() {
    Fixture f; f.PoseFailure(); auto plan = f.Plan();
    Render::AnimationInstanceSpec marker; marker.identity = plan.markers[0].identity; marker.hostKey = "marker";
    marker.model.modelPath = plan.markers[0].modelPath; marker.model.ownedStaticAsset = f.cube;
    marker.model.position = plan.markers[0].position; marker.model.scale = {8, 8, 8};
    auto service = Good(Render::ModelAnimationService::Create({7, 0, "synthetic-source-SHA"}, std::span(&marker, 1),
        [](std::string_view, std::uint64_t) { return Result<Render::AnimationResource>::Failure("Fixture", "UNEXPECTED_READ", "no MDX reads"); }));
    const auto frame = Good(service->Prepare(nullptr, {})); const auto scene = service->Scene();
    Render::ModelPickVisibilitySnapshot flags;
    flags.binding = {0, 7, scene->sceneVersion, frame->contentVersion, 20, 1, {1, 2, 3, 4}};
    flags.visible = {1}; flags.selectable = {1};
    const Render::TerrainRay ray{{100, 0, 40}, {-1, 0, 0}};
    auto hit = Good(Render::PickAnimatedModelInstances(*scene, *frame, flags, flags.binding, ray, 200));
    Check(hit.hits.size() == 1 && hit.hits[0].grade == 8 && hit.hits[0].identity == plan.markers[0].identity
        && hit.hits[0].requestIndex == 0 && std::abs(hit.hits[0].distance - 68.0) < 0.001,
        "owned cube did not produce actual mesh hit with independent typed mapping");
    flags.selectable = {0};
    Check(Good(Render::PickAnimatedModelInstances(*scene, *frame, flags, flags.binding, ray, 200)).hits.empty(), "locked marker could be picked");
    flags.selectable = {1}; flags.visible = {0};
    Check(Good(Render::PickAnimatedModelInstances(*scene, *frame, flags, flags.binding, ray, 200)).hits.empty(), "hidden marker could be picked");
    std::cout << "owned-picking: real frozen checker geometry triangles; no resource read, lock/visibility gates\n";
}
void CompletedLayerContract() {
    War3RenderModelLayersTargetV2 target{sizeof(target), 1, 20, 30};
    Render::ViewProjectionFrame view; view.identity = {1, 2, 3, 4}; view.physicalWidth = 64; view.physicalHeight = 64;
    view.rowMajorViewProjection[0] = view.rowMajorViewProjection[5] = view.rowMajorViewProjection[10] = view.rowMajorViewProjection[15] = 1;
    War3RenderFrameMetricsV2 metric{}; metric.structSize = sizeof(metric);
    metric.flags = WAR3_RENDER_FRAME_METRICS_DRAW_COMPLETED | WAR3_RENDER_FRAME_METRICS_UPLOAD_VALID | WAR3_RENDER_FRAME_METRICS_WINDOW_ATTACHED;
    metric.completed.structSize = sizeof(metric.completed); auto& receipt = metric.completed.view; receipt.structSize = sizeof(receipt);
    receipt.width = receipt.height = 64; receipt.surfaceEpoch = 1; receipt.sceneEpoch = 2; receipt.viewEpoch = 3; receipt.frameCount = 4;
    std::copy(view.rowMajorViewProjection.begin(), view.rowMajorViewProjection.end(), receipt.viewProjection);
    metric.completed.presentation.modelEpoch = 20;
    Check(WorldModelBugLayerFrameQualified(target, target, view, metric), "valid synthetic completion contract rejected");
    auto changed = target; ++changed.layersRevision;
    Check(!WorldModelBugLayerFrameQualified(target, changed, view, metric), "layer replacement race accepted");
    changed = target; ++changed.parentModelEpoch;
    Check(!WorldModelBugLayerFrameQualified(target, changed, view, metric), "main parent replacement accepted");
    const auto saved = metric;
    for (const auto flag : {WAR3_RENDER_FRAME_METRICS_DRAW_COMPLETED, WAR3_RENDER_FRAME_METRICS_UPLOAD_VALID, WAR3_RENDER_FRAME_METRICS_WINDOW_ATTACHED}) {
        metric = saved; metric.flags &= ~flag; Check(!WorldModelBugLayerFrameQualified(target, target, view, metric), "missing completion flag accepted");
    }
    metric = saved; metric.flags |= WAR3_RENDER_FRAME_METRICS_PRESENT_OCCLUDED;
    Check(!WorldModelBugLayerFrameQualified(target, target, view, metric), "occluded marker accepted");
    metric = saved; metric.result = -1;
    Check(!WorldModelBugLayerFrameQualified(target, target, view, metric), "failed layer aggregate upload accepted");
    metric = saved; ++metric.completed.view.sceneEpoch;
    Check(!WorldModelBugLayerFrameQualified(target, target, view, metric), "old global scene epoch accepted");
    CaptureBindingContract();
    ReadOnlyGeometryCoverage();
    std::cout << "completed-layer-contract: synthetic metadata predicates; does not claim backend draw\n";
}
}
int main(int argc, char** argv) try {
    const std::string_view group = argc > 1 ? argv[1] : "all";
    if (group == "all" || group == "classification") Classification();
    if (group == "all" || group == "owned-picking") OwnedPicking();
    if (group == "all" || group == "completed-layer-contract") CompletedLayerContract();
    if (group != "all" && group != "classification" && group != "owned-picking" && group != "completed-layer-contract") throw std::runtime_error("unknown placeholder group");
    return 0;
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
