#pragma once
#include "AnimationParticleContinuityTests.h"
#include "AnimationMaterialPrefetchTests.h"
#include "War3MapCore/Hash.h"
#include "../src/Render/Animation/AnimationInternal.h"
#include <algorithm>
#include <iostream>
#include <map>

namespace AnimationInstanceDeltaTest {
using namespace War3Map;
using namespace War3Map::Render;
using AnimationParticleContinuityTest::Check;
using AnimationParticleContinuityTest::Good;
inline void CheckDeltaTimings(const AnimationInstanceDeltaTimings& timing) {
    const auto phases=timing.cloneNs+timing.identityNs+timing.assetNs+timing.orderingNs+timing.storageNs+
        timing.stateRemapNs+timing.particleRemapNs+timing.firstFrameNs+timing.renderedRemapNs;
    const auto storage=timing.storageLayoutNs+timing.storageEvaluatorNs+timing.storageStandNs+timing.storageParticlesNs;
    Check(timing.totalNs>0 && timing.storageNs>0 && timing.totalNs>=phases && timing.storageNs>=storage,
        "delta phase measurement overlapped top-level work or omitted actual storage initialization");
}
inline void NegativeCatalogEntriesSurvive() {
    auto pure=AnimationParticleContinuityTest::Model(),empty=AnimationTest::Bytes();
    const auto removeGeometry=[](std::vector<std::byte>& bytes) {
        for (std::size_t at=4;at+8<=bytes.size();) {
            std::uint32_t size=0;std::memcpy(&size,bytes.data()+at+4,4);
            if (std::memcmp(bytes.data()+at,"GEOS",4)==0 || std::memcmp(bytes.data()+at,"GEOA",4)==0)
                bytes.erase(bytes.begin()+static_cast<std::ptrdiff_t>(at),bytes.begin()+static_cast<std::ptrdiff_t>(at+8+size));
            else at+=8+size;
        }
    };
    removeGeometry(pure);removeGeometry(empty);
    const auto model=AnimationParticleContinuityTest::Model(),texture=AnimationParticleContinuityTest::Texture();
    unsigned reads=0;
    const auto read=[&](std::string_view path,std::uint64_t) {
        ++reads;
        return Result<AnimationResource>::Success({path=="pure.mdx" ? pure : path=="empty.mdx" ? empty :
            path=="model.mdx" ? model : texture,"delta-negative-fixture",std::string(64,'e')});
    };
    std::array<AnimationInstanceSpec,3> specs;
    for (std::size_t i=0;i<specs.size();++i) {
        specs[i].identity={i+1,7};specs[i].hostKey="negative:"+std::to_string(i);
        specs[i].model.modelPath=i==0 ? "model.mdx" : i==1 ? "pure.mdx" : "empty.mdx";
    }
    AnimationLimits limits;limits.enableParticles=true;limits.retainUnsupportedStatic=true;
    limits.playbackPolicy=AnimationPlaybackPolicy::InteractivePreview;
    auto service=Good(ModelAnimationService::Create({7,1,"negative-source"},specs,read,limits));
    const auto advance=AnimationParticleContinuityTest::View(0);
    auto frame=Good(service->Prepare(nullptr,advance));const auto initialReads=reads;
    AnimationInstanceDelta delta;delta.session={7,2,"negative-source"};delta.removals={specs[2].identity};
    delta.rows.push_back({specs[1].identity,"negative:retained-particle"});
    for (const auto i:{1u,2u}) {
        auto added=specs[i];added.identity={i+3,7};added.hostKey="added:"+std::to_string(i);
        delta.additions.push_back(added);
    }
    const auto plan=Good(service->PrepareInstanceCatalog(delta,read));
    auto result=Good(service->PrepareInstanceDelta(*frame,*plan,advance));const auto scene=result.service->Scene();
    Check(!plan->AssetsChanged() && reads==initialReads && scene->particleInstanceCount==3 &&
        scene->geometry.skippedInstances==1 && scene->unavailableInstances.size()==1 &&
        scene->unavailableInstances[0].identity==delta.additions[1].identity &&
        scene->unavailableInstances[0].requestIndex==3 && scene->sequences.size()==4 &&
        scene->particleCapabilities.size()==4 && scene->specs[1].hostKey=="negative:retained-particle" &&
        scene->geometry.instances.size()==1 && result.frame->particleLinks.size()==3,
        "same-asset delta reread particle-only/negative assets or lost their remapped classification");
    delta.session.documentRevision=3;delta.removals.clear();delta.rows.clear();
    delta.additions.resize(1);delta.additions[0].identity={9,7};delta.additions[0].hostKey="invalid:sequence";
    delta.additions[0].sequenceIndex=UINT32_MAX;
    const auto invalid=result.service->PrepareInstanceCatalog(delta,read);
    Check(!invalid.Succeeded() && invalid.diagnostics.front().code=="ANIMATION_SEQUENCE_INVALID",
        "resident-asset optimization skipped validation of a new instance's sequence");
}
inline void PreviewAssetSnapshot() {
    const auto model = AnimationTest::Bytes();
    auto seedModel = model;
    constexpr std::string_view texturePath = "Textures\\Crate.blp";
    const auto at = std::search(seedModel.begin(), seedModel.end(),
        reinterpret_cast<const std::byte*>(texturePath.data()),
        reinterpret_cast<const std::byte*>(texturePath.data() + texturePath.size()));
    Check(at != seedModel.end(), "preview reuse fixture has no texture field");
    std::memcpy(&*at, "Textures\\Other.blp", texturePath.size());
    auto texture = AnimationParticleContinuityTest::Texture();
    std::string layer = "actual-model-layer";
    const auto read = [&](std::string_view path, std::uint64_t) {
        const auto& bytes = path == "seed.mdx" ? seedModel : path.ends_with(".mdx") ? model : texture;
        return Result<AnimationResource>::Success({bytes, layer, Good(Sha256(bytes))});
    };
    AnimationInstanceSpec ghost; ghost.identity = {1, 98}; ghost.hostKey = "preview";
    ghost.model.modelPath = "new.mdx"; ghost.model.previewGhost = true;
    auto preview = Good(ModelAnimationService::Create({98, 1, "source"}, std::span(&ghost, 1), read));
    auto snapshot = Good(preview->CaptureAssetSnapshot());
    Good(preview->Prepare(nullptr));
    const auto parsed = preview->Scene()->pickModels.front().source;
    Check(!preview->Scene()->geometry.layers.empty() && preview->Scene()->geometry.layers.front().alpha == 0.45f,
        "snapshot fixture is not actual preview material");
    preview.reset(); // snapshot不得依赖预览服务或worker寿命。
    AnimationInstanceSpec seed; seed.identity = {1, 7}; seed.hostKey = "seed"; seed.model.modelPath = "seed.mdx";
    const auto resident = Good(ModelAnimationService::Create({7, 1, "source"}, std::span(&seed, 1), read));
    auto previous = Good(resident->Prepare(nullptr));
    AnimationInstanceDelta delta; delta.session = {7, 2, "source"};
    auto added = seed; added.identity = {2, 7}; added.hostKey = "formal"; added.model.modelPath = "new.mdx";
    delta.additions.push_back(added);
    for (unsigned change = 0; change < 3; ++change) {
        if (change == 1) layer = "actual-overlay-layer";
        if (change == 2) { layer = "actual-model-layer"; texture[158] = std::byte{0}; texture[156] = std::byte{255}; }
        const auto cachedPlan = Good(resident->PrepareInstanceCatalog(delta, read, snapshot));
        const auto plainPlan = Good(resident->PrepareInstanceCatalog(delta, read));
        const auto cached = Good(resident->PrepareInstanceDelta(*previous, *cachedPlan));
        const auto plain = Good(resident->PrepareInstanceDelta(*previous, *plainPlan));
        const auto cachedScene = cached.service->Scene(), plainScene = plain.service->Scene();
        Check((cachedScene->pickModels.back().source == parsed) == (change != 1),
            "parsed preview asset was not reused or ignored changed source layer");
        Check(cachedScene->geometry.textures.size() == plainScene->geometry.textures.size(), "snapshot changed texture catalog");
        for (std::size_t i = 0; i < cachedScene->geometry.textures.size(); ++i)
            Check(cachedScene->geometry.textures[i].rgba == plainScene->geometry.textures[i].rgba,
                "decoded snapshot ignored actual changed texture SHA");
        const auto firstFormalLayer = cachedScene->geometry.meshes.at(cachedScene->geometry.models.back().firstMesh).firstLayer;
        Check(cachedScene->geometry.layers.size() == plainScene->geometry.layers.size(), "snapshot changed static material layout");
        for (std::size_t i = 0; i < cachedScene->geometry.layers.size(); ++i)
            Check(cachedScene->geometry.layers[i].alpha == plainScene->geometry.layers[i].alpha,
                "snapshot changed an actual formal material alpha");
        Check(cached.frame->states.back().identity == added.identity && cachedScene->geometry.layers.at(firstFormalLayer).alpha == 1.0f
            && !cachedScene->specs.back().model.previewGhost,
            "formal instance borrowed preview generation, clock or translucent material");
    }
    const auto failedRead = [](std::string_view, std::uint64_t) {
        return Result<AnimationResource>::Failure("Test", "RESOURCE_ACCESS_DENIED", "actual source refused");
    };
    Check(!resident->PrepareInstanceCatalog(delta, failedRead, snapshot).Succeeded(),
        "preloaded asset bypassed actual-source refusal");
}
inline void PreparedFormalAssets() {
    const auto model = AnimationParticleContinuityTest::Model(), plainModel = AnimationTest::Bytes();
    auto seedModel = model, texture = AnimationParticleContinuityTest::Texture();
    constexpr std::string_view texturePath = "Textures\\Crate.blp";
    const auto at = std::search(seedModel.begin(), seedModel.end(),
        reinterpret_cast<const std::byte*>(texturePath.data()), reinterpret_cast<const std::byte*>(texturePath.data() + texturePath.size()));
    Check(at != seedModel.end(), "prepared fixture has no texture field");
    std::memcpy(&*at, "Textures\\Other.blp", texturePath.size());
    std::map<std::string, unsigned> reads;
    std::string layer = "formal-new-layer";
    bool denyNew = false, missingTexture = false;
    const auto read = [&](std::string_view path, std::uint64_t) {
        ++reads[std::string(path)];
        const bool old = path == "seed.mdx" || path.find("Other.blp") != std::string_view::npos ||
            path.find("other.blp") != std::string_view::npos;
        if (!old && denyNew) return Result<AnimationResource>::Failure("Test", "RESOURCE_ACCESS_DENIED", "actual source refused");
        if (!old && missingTexture && !path.ends_with(".mdx"))
            return Result<AnimationResource>::Failure("Test", "RESOURCE_NOT_FOUND", "texture missing");
        const auto& bytes = path == "seed.mdx" ? seedModel : path == "missing.mdx" ? plainModel :
            path.ends_with(".mdx") ? model : texture;
        return Result<AnimationResource>::Success({bytes, old ? "formal-base-layer" : layer, Good(Sha256(bytes))});
    };
    AnimationLimits limits; limits.enableParticles = true; limits.retainUnsupportedStatic = true;
    limits.playbackPolicy = AnimationPlaybackPolicy::InteractivePreview;
    AnimationInstanceSpec ghost; ghost.identity = {44, 98}; ghost.hostKey = "ghost"; ghost.model.modelPath = "new.mdx";
    ghost.model.previewGhost = true;
    auto preview = Good(ModelAnimationService::Create({98, 1, "formal-source"}, std::span(&ghost, 1), read, limits));
    auto parsed = preview->Scene()->pickModels.front().source;
    auto snapshot = Good(preview->CaptureAssetSnapshot()); preview.reset();
    AnimationInstanceSpec seed; seed.identity = {1, 7}; seed.hostKey = "seed"; seed.model.modelPath = "seed.mdx";
    seed.sequenceIndex = 0; seed.elapsedMs = 300; seed.globalMs = 300; seed.playing = true;
    auto resident = Good(ModelAnimationService::Create({7, 1, "formal-source"}, std::span(&seed, 1), read, limits));
    const auto original = resident->Scene(); auto view = AnimationParticleContinuityTest::View(0);
    const auto originalVertices = original->geometry.vertices.data();
    const auto originalTextures = original->geometry.textures.data();
    auto previous = Good(resident->Prepare(nullptr, view));
    const auto catalog = Good(resident->CaptureInstanceCatalog());
    auto added = seed; added.identity = {2, 7}; added.hostKey = "formal"; added.model.modelPath = "new.mdx";
    added.elapsedMs = 700; added.globalMs = 700; added.model.position[0] = 200;
    const auto prepared = Good(catalog->PrepareStaticAssets(added.model, read, snapshot));
    Check(catalog->Scene() == original && original->specs.size() == 1 && original->geometry.models.size() == 1 &&
        original->geometry.vertices.data() == originalVertices && original->geometry.textures.data() == originalTextures,
        "formal prefetch changed the resident scene or added a provisional instance");
    const auto before = reads;
    AnimationInstanceDelta delta; delta.session = {7, 2, "formal-source"}; delta.additions.push_back(added);
    AnimationInstanceDeltaTimings timing;
    const auto plan = Good(catalog->PrepareInstanceCatalog(delta, read, {}, &timing, prepared));
    Check(timing.preparedAssetHit && timing.preparedAssetMissReason.empty() && plan->AssetsChanged() &&
        reads["new.mdx"] == before.at("new.mdx") + 1 && reads["seed.mdx"] == before.at("seed.mdx"),
        "formal static packet missed or did not verify the new source without rereading old models");
    auto first = Good(resident->PrepareInstanceDelta(*previous, *plan, view));
    auto second = Good(resident->PrepareInstanceDelta(*previous, *plan, view));
    const auto scene = first.service->Scene();
    const auto layerIndex = scene->geometry.meshes.at(scene->geometry.models.back().firstMesh).firstLayer;
    Check(scene->pickModels.back().source == parsed && scene->geometry.layers.at(layerIndex).alpha == 1.0f &&
        !scene->specs.back().model.previewGhost && first.frame->states.back().identity == added.identity &&
        first.frame->states.back().transform == ModelInstanceTransform(added.model) &&
        first.frame->poses.back().asset == War3RenderCore::AssetIdentity{2, 7} &&
        first.service != second.service && first.service->Scene() != second.service->Scene() &&
        first.frame->particleCheckpoint->instances.back() != second.frame->particleCheckpoint->instances.back(),
        "formal packet borrowed ghost materials/compiled generation or a mutable instance workspace");
    view.deltaMs = 33; const auto flowing = Good(first.service->Prepare(first.frame.get(), view));
    view.deltaMs = 0; const auto unchanged = Good(second.service->Prepare(second.frame.get(), view));
    Check(flowing->states.back().elapsedMs == 733 && unchanged->states.back().elapsedMs == 700 &&
        flowing->particleLinks.back().spawned > 0 && second.frame->states.front().elapsedMs == 300 &&
        Good(resident->Prepare(previous.get(), view))->states.front().elapsedMs == 300,
        "one prepared delta advanced a sibling or the original instance's clock/particles");
    // 同一静态包可被独立候选消费，但每次的实例表、位置和时钟只能来自那次编辑事务。
    // 检查底层共享可直接发现正式消费又对整个旧世界做一次append/COW的性能回退。
    auto alternate = delta; alternate.session.documentRevision = 3;
    alternate.rows = {{seed.identity, "seed-renamed"}};
    alternate.additions[0].identity = {20, 7}; alternate.additions[0].hostKey = "other-formal";
    alternate.additions[0].model.position[0] = 450;
    alternate.additions[0].elapsedMs = 90; alternate.additions[0].globalMs = 90;
    const auto alternatePlan = Good(catalog->PrepareInstanceCatalog(alternate, read, {}, &timing, prepared));
    const auto alternateResult = Good(resident->PrepareInstanceDelta(*previous, *alternatePlan, view));
    const auto alternateScene = alternateResult.service->Scene();
    Check(timing.preparedAssetHit && alternateScene->geometry.vertices.SharesStorageWith(scene->geometry.vertices) &&
        alternateScene->geometry.indices.SharesStorageWith(scene->geometry.indices) &&
        alternateScene->geometry.layers.SharesStorageWith(scene->geometry.layers) &&
        alternateScene->geometry.meshes.SharesStorageWith(scene->geometry.meshes) &&
        alternateScene->geometry.models.SharesStorageWith(scene->geometry.models) &&
        alternateScene->geometry.textures.SharesStorageWith(scene->geometry.textures) &&
        alternateScene->vertices.SharesStorageWith(scene->vertices) &&
        alternateScene->models.SharesStorageWith(scene->models),
        "consuming the same prepared packet recopied merged static buffers at formal placement time");
    Check(alternateScene->documentRevision == 3 && alternateScene->specs[0].hostKey == "seed-renamed" &&
        alternateScene->specs.back().identity == alternate.additions[0].identity &&
        alternateResult.frame->states.back().transform == ModelInstanceTransform(alternate.additions[0].model) &&
        alternateResult.frame->states.back().elapsedMs == 90 &&
        alternateResult.frame->particleCheckpoint->instances.back() != first.frame->particleCheckpoint->instances.back() &&
        scene->specs[0].hostKey == "seed" && scene->specs.back().model.position[0] == 200 &&
        first.frame->states.back().elapsedMs == 700 && original->specs[0].hostKey == "seed",
        "applying prepared static geometry overwrote an edited row, independent transform, clock or particle workspace");
    auto mixed = delta; auto extra = added; extra.identity = {3, 7}; extra.hostKey = "additional-cold-asset";
    extra.model.modelPath = "another.mdx"; mixed.additions.push_back(extra);
    const auto mixedPlan = Good(catalog->PrepareInstanceCatalog(mixed, read, {}, &timing, prepared));
    const auto mixedResult = Good(resident->PrepareInstanceDelta(*previous, *mixedPlan, view));
    Check(timing.preparedAssetHit && mixedResult.service->Scene()->models.size() == 3 &&
        mixedResult.frame->poses[1].asset == War3RenderCore::AssetIdentity{2, 7} &&
        mixedResult.frame->poses[2].asset == War3RenderCore::AssetIdentity{3, 7} &&
        !mixedResult.service->Scene()->geometry.vertices.SharesStorageWith(scene->geometry.vertices) &&
        scene->models.size() == 2 && alternateScene->models.size() == 2 && original->models.size() == 1 &&
        original->geometry.vertices.data() == originalVertices && original->geometry.textures.data() == originalTextures,
        "prepared packet collided with a second cold asset or rebound its compiled identity");
    const auto siblingPacket = Good(catalog->PrepareStaticAssets(extra.model, read));
    AnimationInstanceDelta appendSibling; appendSibling.session = {7, 3, "formal-source"}; appendSibling.additions = {extra};
    const auto siblingPlan = Good(first.service->PrepareInstanceCatalog(appendSibling, read, {}, &timing, siblingPacket));
    const auto siblingResult = Good(first.service->PrepareInstanceDelta(*first.frame, *siblingPlan, view));
    Check(!timing.preparedAssetHit && timing.preparedAssetMissReason == "ANIMATION_PREPARED_CATALOG_STALE" &&
        siblingResult.service->Scene()->geometry.modelPaths == std::vector<std::string>{"seed.mdx", "new.mdx", "another.mdx"} &&
        siblingResult.frame->poses[1].asset == War3RenderCore::AssetIdentity{2, 7} &&
        siblingResult.frame->poses[2].asset == War3RenderCore::AssetIdentity{3, 7} && scene->models.size() == 2,
        "a second packet from the same old base overwrote a committed sibling model or reused its compiled identity");
    auto later = delta; later.session.documentRevision = 3; later.additions[0].identity = {3, 7}; later.additions[0].hostKey = "later";
    const auto laterPlan = Good(first.service->PrepareInstanceCatalog(later, read, {}, &timing, prepared));
    Check(!timing.preparedAssetHit && timing.preparedAssetMissReason == "ANIMATION_PREPARED_CATALOG_STALE" &&
        !laterPlan->AssetsChanged(), "packet survived a changed catalog or rebuilt an already resident model");
    auto different = added.model; different.teamColor = 3;
    auto changedRequest = delta; changedRequest.additions[0].model = different;
    Good(catalog->PrepareInstanceCatalog(changedRequest, read, {}, &timing, prepared));
    Check(!timing.preparedAssetHit && timing.preparedAssetMissReason == "ANIMATION_PREPARED_REQUEST_MISMATCH",
        "formal packet ignored a changed material variant request");
    auto changedLimits = limits; changedLimits.geometry.maxTextureDimension = 256;
    auto changedCatalog = Good(ModelAnimationService::Create({7, 1, "formal-source"}, std::span(&seed, 1), read, changedLimits));
    Good(changedCatalog->PrepareInstanceCatalog(delta, read, {}, &timing, prepared));
    Check(!timing.preparedAssetHit && timing.preparedAssetMissReason == "ANIMATION_PREPARED_CATALOG_STALE",
        "packet from different formal options was accepted as the same catalog");
    for (const bool changeBytes : {false, true}) {
        if (changeBytes) texture[158] = std::byte{127}; else layer = "shadowing-layer";
        const auto rejected = catalog->PrepareInstanceCatalog(delta, read, {}, &timing, prepared);
        Check(!rejected.Succeeded() && rejected.diagnostics.front().code == "ANIMATION_PREPARED_SOURCE_CHANGED" &&
            !timing.preparedAssetHit && resident->Scene() == original,
            "changed source layer/bytes were hidden by the prepared static packet");
        layer = "formal-new-layer"; texture[158] = std::byte{255};
    }
    denyNew = true;
    const auto denied = catalog->PrepareInstanceCatalog(delta, read, {}, &timing, prepared); denyNew = false;
    Check(!denied.Succeeded() && denied.diagnostics.front().code == "RESOURCE_ACCESS_DENIED" && !timing.preparedAssetHit,
        "formal packet bypassed actual source refusal");
    missingTexture = true;
    auto missingRequest = added.model; missingRequest.modelPath = "missing.mdx";
    const auto negative = Good(catalog->PrepareStaticAssets(missingRequest, read));
    auto missingDelta = delta; missingDelta.additions[0].model = missingRequest;
    Good(catalog->PrepareInstanceCatalog(missingDelta, read, {}, &timing, negative));
    Check(timing.preparedAssetHit, "unchanged actual negative source was not verified for the packet");
    missingTexture = false;
    const auto appeared = catalog->PrepareInstanceCatalog(missingDelta, read, {}, &timing, negative);
    Check(!appeared.Succeeded() && appeared.diagnostics.front().code == "ANIMATION_PREPARED_SOURCE_CHANGED",
        "newly available texture was hidden by a prepared negative entry");
    const auto badGhost = catalog->PrepareStaticAssets(ghost.model, read);
    Check(!badGhost.Succeeded() && badGhost.diagnostics.front().code == "ANIMATION_PREPARED_GHOST_REQUEST",
        "ghost request was silently converted into a formal prepared asset");
    auto tightLimits = limits; tightLimits.geometry.maxModels = 1;
    const auto tight = Good(ModelAnimationService::Create({7, 1, "formal-source"}, std::span(&seed, 1), read, tightLimits));
    const auto overBudget = tight->PrepareStaticAssets(added.model, read);
    Check(!overBudget.Succeeded() && overBudget.diagnostics.front().code == "ANIMATION_DELTA_ASSET_BUDGET",
        "prefetch ignored the combined resident and prepared geometry budget");
    tightLimits.geometry.maxModels = 2;
    const auto twoModels = Good(ModelAnimationService::Create({7, 1, "formal-source"}, std::span(&seed, 1), read, tightLimits));
    const auto twoPacket = Good(twoModels->PrepareStaticAssets(added.model, read));
    const auto tooMany = twoModels->PrepareInstanceCatalog(mixed, read, {}, &timing, twoPacket);
    Check(!tooMany.Succeeded() && tooMany.diagnostics.front().code == "ANIMATION_DELTA_ASSET_BUDGET" &&
        twoModels->Scene()->models.size() == 1 && twoModels->Scene()->specs.size() == 1,
        "prepared geometry application let a second cold asset bypass the live combined budget");
}
inline void StableModelOrderAndBoundedMapping() {
    const auto model=AnimationTest::Bytes(),texture=AnimationParticleContinuityTest::Texture();
    const auto read=[&](std::string_view path,std::uint64_t) {
        return Result<AnimationResource>::Success({path.ends_with(".mdx")?model:texture,
            "stable-model-delta-fixture",std::string(64,'f')});
    };
    for (const bool unsorted:{false,true}) {
        std::array<AnimationInstanceSpec,5> specs;
        const std::array paths{"a.mdx","b.mdx","a.mdx","c.mdx","b.mdx"};
        for (std::size_t i=0;i<specs.size();++i) {
            specs[i].identity={i+1,17};specs[i].hostKey="order:"+std::to_string(i);specs[i].model.modelPath=paths[i];
        }
        auto service=Good(ModelAnimationService::Create({17,1,"ordering-source"},specs,read));
        Check(service->Scene()->geometry.instanceRequests==std::vector<std::uint32_t>{0,2,1,4,3},
            "ordering fixture did not establish interleaved request/model identities");
        if (unsorted) {
            // 仅在独占合成fixture、首次Prepare及捕获前注入早期未排序目录；生产快照仍只读。
            auto injected=std::const_pointer_cast<AnimatedModelScene>(service->Scene());
            std::swap(injected->geometry.instances.front(),injected->geometry.instances.back());
            std::swap(injected->geometry.instanceRequests.front(),injected->geometry.instanceRequests.back());
        }
        const auto original=service->Scene();
        auto frame=Good(service->Prepare(nullptr));
        auto catalog=Good(service->CaptureInstanceCatalog());
        Check(catalog->Scene()==original,"read-only capture copied the already complete scene directory");
        AnimationInstanceDelta delta;delta.session={17,2,"ordering-source"};delta.removals={specs[4].identity};
        for (const auto source:{1u,0u,3u,2u}) {
            auto added=specs[source];added.identity={10+delta.additions.size(),17};
            added.hostKey="order-added:"+std::to_string(delta.additions.size());delta.additions.push_back(std::move(added));
        }
        AnimationInstanceDeltaTimings catalogTiming;catalogTiming.cloneNs=UINT64_MAX;
        const auto plan=Good(catalog->PrepareInstanceCatalog(delta,read,{},&catalogTiming));
        CheckDeltaTimings(catalogTiming);
        Check(catalogTiming.cloneNs!=UINT64_MAX && !plan->AssetsChanged(),"catalog timings were not reset or old model variants reloaded");
        AnimationInstanceDeltaTimings rebaseTiming;
        auto result=Good(service->PrepareInstanceDelta(*frame,*plan,{},&rebaseTiming));CheckDeltaTimings(rebaseTiming);
        const std::vector<std::uint32_t> expected=unsorted ? std::vector<std::uint32_t>{2,0,5,7,1,4,3,6}
            : std::vector<std::uint32_t>{0,2,5,7,1,4,3,6};
        const auto scene=result.service->Scene();
        Check(scene!=original && scene->geometry.instanceRequests==expected && scene->specs.size()==8 &&
            catalog->Scene()==original && original->specs.size()==5 && scene->geometry.vertices.data()==original->geometry.vertices.data(),
            "stable model merge changed same-model survivor order, put new before old, or mutated captured source");
        for (std::size_t i=0;i<frame->poses.size();++i) {
            const auto oldRequest=frame->poses[i].requestIndex;
            if (oldRequest==4) Check(result.oldToNewRendered[i]==UINT32_MAX,"removed request retained a rendered target");
            else Check(result.oldToNewRendered[i]<result.frame->poses.size() &&
                result.frame->poses[result.oldToNewRendered[i]].identity==frame->poses[i].identity,
                "dense render map confused original request order and sorted geometry order");
        }
        AnimationFrame duplicate=*frame;duplicate.poses[1].requestIndex=duplicate.poses[0].requestIndex;
        const auto rejectedDuplicate=service->PrepareInstanceDelta(duplicate,*plan);
        AnimationFrame invalid=*frame;invalid.poses[0].requestIndex=static_cast<std::uint32_t>(specs.size());
        const auto rejectedRange=service->PrepareInstanceDelta(invalid,*plan);
        Check(!rejectedDuplicate.Succeeded() && rejectedDuplicate.diagnostics.front().code=="ANIMATION_DELTA_RENDER_MAPPING_INVALID" &&
            !rejectedRange.Succeeded() && rejectedRange.diagnostics.front().code=="ANIMATION_DELTA_RENDER_MAPPING_INVALID",
            "dense request map accepted duplicate or out-of-range old rendered input");
        AnimationFrame truncated=*frame;truncated.poses.pop_back();
        Check(!service->PrepareInstanceDelta(truncated,*plan).Succeeded(),"partial latest rendered roster was accepted");
        auto unchanged=Good(service->Prepare(frame.get()));
        Check(unchanged->states.size()==5 && unchanged->poses.size()==5 && catalog->Scene()==original,
            "failed dense mapping changed resident playback or captured catalog");
    }
    AnimationInstanceSpec one;one.identity={1,27};one.hostKey="budget:original";one.model.modelPath="a.mdx";
    AnimationLimits limits;limits.maxFrameBytes=1024*1024;
    auto bounded=Good(ModelAnimationService::Create({27,1,"budget-source"},std::span(&one,1),read,limits));
    auto frame=Good(bounded->Prepare(nullptr));
    AnimationInstanceDelta over;over.session={27,2,"budget-source"};
    over.additions.resize(static_cast<std::size_t>(limits.maxFrameBytes/sizeof(AnimationInstanceSpec)));
    const auto refused=bounded->PrepareInstanceCatalog(std::move(over),read);
    Check(!refused.Succeeded() && refused.diagnostics.front().code=="ANIMATION_DELTA_BUDGET" &&
        Good(bounded->Prepare(frame.get()))->states.size()==1,"catalog allocation budget truncated input or harmed resident service");
}
inline void IdentityIndexKeepsMutationGates() {
    const auto model=AnimationTest::Bytes(),texture=AnimationParticleContinuityTest::Texture();
    const auto read=[&](std::string_view path,std::uint64_t) {
        return Result<AnimationResource>::Success({path.ends_with(".mdx")?model:texture,
            "identity-delta-fixture",std::string(64,'c')});
    };
    std::array<AnimationInstanceSpec,3> specs;
    const std::array<std::uint64_t,3> identities{(1ull<<40)+1,7,1};
    for (std::size_t i=0;i<specs.size();++i) {
        specs[i].identity={identities[i],37};specs[i].hostKey="identity:"+std::to_string(i);
        specs[i].model.modelPath="identity.mdx";
    }
    auto service=Good(ModelAnimationService::Create({37,1,"identity-source"},specs,read));
    const auto original=service->Scene();const auto frame=Good(service->Prepare(nullptr));
    const auto catalog=Good(service->CaptureInstanceCatalog());
    AnimationInstanceDelta delta;delta.session={37,2,"identity-source"};delta.removals={specs[1].identity};
    auto added=specs[1];added.identity={3,37};added.hostKey="identity:added";delta.additions={added};
    delta.rows={{specs[0].identity,"identity:high"},{specs[2].identity,"identity:low"},{added.identity,"identity:new"}};
    AnimationEdit oldEdit;oldEdit.identity=specs[0].identity;oldEdit.elapsedMs=81;oldEdit.globalMs=81;
    AnimationEdit newEdit;newEdit.identity=added.identity;newEdit.elapsedMs=93;newEdit.globalMs=93;
    delta.edits={oldEdit,newEdit};
    const auto plan=Good(catalog->PrepareInstanceCatalog(delta,read));
    const auto result=Good(service->PrepareInstanceDelta(*frame,*plan));const auto scene=result.service->Scene();
    Check(result.oldToNewRequests==std::vector<std::uint32_t>{0,UINT32_MAX,1} &&
        scene->specs[0].identity==specs[0].identity && scene->specs[1].identity==specs[2].identity &&
        scene->specs[0].hostKey=="identity:high" && scene->specs[1].hostKey=="identity:low" &&
        scene->specs[2].hostKey=="identity:new" && result.frame->states[0].elapsedMs==81 &&
        result.frame->states[2].elapsedMs==93,"identity index changed request order, truncated high identity bits or lost row/state edits");
    const auto rejects=[&](const AnimationInstanceDelta& bad,std::string_view code) {
        const auto refused=catalog->PrepareInstanceCatalog(bad,read);
        Check(!refused.Succeeded() && !refused.diagnostics.empty() && refused.diagnostics.front().code==code,
            "identity index weakened an unknown, duplicate, removed or reused-identity gate");
    };
    auto bad=delta;bad.removals={{99,37}};rejects(bad,"ANIMATION_DELTA_INSTANCE_STALE");
    bad=delta;bad.removals[0].generation=38;rejects(bad,"ANIMATION_DELTA_INSTANCE_STALE");
    bad=delta;bad.removals.push_back(specs[1].identity);rejects(bad,"ANIMATION_DELTA_INSTANCE_STALE");
    bad=delta;bad.additions[0].identity=specs[1].identity;rejects(bad,"ANIMATION_DELTA_INSTANCE_IDENTITY");
    bad=delta;bad.additions[0].identity.generation=38;rejects(bad,"ANIMATION_DELTA_INSTANCE_IDENTITY");
    bad=delta;bad.additions.push_back(added);rejects(bad,"ANIMATION_DELTA_INSTANCE_IDENTITY");
    bad=delta;bad.rows[0].identity={99,37};rejects(bad,"ANIMATION_DELTA_ROW_STALE");
    bad=delta;bad.rows[0].identity.generation=38;rejects(bad,"ANIMATION_DELTA_ROW_STALE");
    bad=delta;bad.rows[0].identity=specs[1].identity;rejects(bad,"ANIMATION_DELTA_ROW_STALE");
    bad=delta;bad.rows.push_back(bad.rows[0]);rejects(bad,"ANIMATION_DELTA_ROW_STALE");
    bad=delta;bad.edits[0].identity={99,37};rejects(bad,"ANIMATION_DELTA_EDIT_STALE");
    bad=delta;bad.edits[0].identity.generation=38;rejects(bad,"ANIMATION_DELTA_EDIT_STALE");
    bad=delta;bad.edits[0].identity=specs[1].identity;rejects(bad,"ANIMATION_DELTA_EDIT_STALE");
    bad=delta;bad.edits.push_back(bad.edits[0]);rejects(bad,"ANIMATION_DELTA_EDIT_STALE");
    bad=delta;bad.rows[2].hostKey=bad.rows[0].hostKey;rejects(bad,"ANIMATION_DELTA_ROW_DUPLICATE");
    Check(service->Scene()==original && catalog->Scene()==original && original->specs[0].hostKey=="identity:0" &&
        Good(service->Prepare(frame.get()))->states.size()==3,"rejected identity delta mutated the original playback or frozen catalog");
    AnimationInstanceDelta swapped;swapped.session={37,2,"identity-source"};
    swapped.rows={{specs[0].identity,specs[2].hostKey},{specs[2].identity,specs[0].hostKey}};
    const auto swapPlan=Good(catalog->PrepareInstanceCatalog(swapped,read));
    const auto swappedResult=Good(service->PrepareInstanceDelta(*frame,*swapPlan));
    Check(swappedResult.service->Scene()->specs[0].hostKey==specs[2].hostKey &&
        swappedResult.service->Scene()->specs[2].hostKey==specs[0].hostKey,
        "incremental host index rejected a valid simultaneous row-key swap");
    swapped.rows.pop_back();rejects(swapped,"ANIMATION_DELTA_ROW_DUPLICATE");
    AnimationInstanceDelta recycled;recycled.session={37,2,"identity-source"};recycled.removals={specs[1].identity};
    recycled.additions={added};recycled.additions[0].hostKey=specs[1].hostKey;
    const auto recyclePlan=Good(catalog->PrepareInstanceCatalog(recycled,read));
    const auto recycledResult=Good(service->PrepareInstanceDelta(*frame,*recyclePlan));
    Check(recycledResult.service->Scene()->specs.back().hostKey==specs[1].hostKey,
        "host index retained the row key of a removed instance");
    auto other=Good(ModelAnimationService::Create({37,1,"identity-source"},specs,read));
    const auto otherFrame=Good(other->Prepare(nullptr));
    const auto wrongScene=other->PrepareInstanceDelta(*otherFrame,*plan);
    Check(!wrongScene.Succeeded() && wrongScene.diagnostics.front().code=="ANIMATION_DELTA_FRAME_STALE",
        "identity index allowed a plan from a different scene with the same revision");
    // 独占合成场景模拟COW后的早期/外部目录；缓存须察觉同版本号下真实表已变。
    auto injected=std::const_pointer_cast<AnimatedModelScene>(other->Scene());
    const auto frozenSpecs=injected->specs;
    injected->specs[0].hostKey=specs[1].hostKey;
    AnimationInstanceDelta empty;empty.session={37,2,"identity-source"};
    const auto duplicate=other->PrepareInstanceCatalog(empty,read);
    Check(!duplicate.Succeeded() && duplicate.diagnostics.front().code=="ANIMATION_DELTA_ROW_DUPLICATE" &&
        frozenSpecs[0].hostKey==specs[0].hostKey,
        "same-version COW mutation reused stale host-key validation or changed the frozen table");
    empty.rows={{specs[0].identity,"identity:repaired"}};
    Good(other->PrepareInstanceCatalog(empty,read));
}
inline void OwnedIdentityIndexChecksCompleteBinding() {
    AnimatedModelScene scene;scene.sceneVersion=90;scene.documentRevision=8;scene.generation=47;scene.sourceIdentity="index-source";
    AnimationInstanceSpec spec;spec.identity={1,47};spec.hostKey="index:one";scene.specs={spec};
    constexpr std::uint64_t budget=1024;
    const auto index=Good(EnsureAnimationCatalogIdentityIndex(scene,{},budget));
    const auto copied=scene;
    Check(Good(EnsureAnimationCatalogIdentityIndex(copied,index,budget))==index,
        "identical owned catalog binding rebuilt its identity index");
    for (unsigned changed=0;changed<5;++changed) {
        auto different=scene;
        if (changed==0) ++different.sceneVersion;
        else if (changed==1) ++different.documentRevision;
        else if (changed==2) ++different.generation;
        else if (changed==3) different.sourceIdentity="shadowing-source";
        else different.specs[0].hostKey="index:changed";
        Check(Good(EnsureAnimationCatalogIdentityIndex(different,index,budget))!=index,
            "identity cache ignored scene/revision/generation/source or a COW request table change");
    }
    const auto small=EnsureAnimationCatalogIdentityIndex(scene,index,2*sizeof(std::uint32_t)-1);
    Check(!small.Succeeded() && small.diagnostics.front().code=="ANIMATION_DELTA_BUDGET",
        "identity cache reuse bypassed the current memory budget");
    auto duplicate=scene;duplicate.specs.push_back(spec);
    const auto refused=EnsureAnimationCatalogIdentityIndex(duplicate,index,budget);
    Check(!refused.Succeeded() && refused.diagnostics.front().code=="ANIMATION_DELTA_INSTANCE_STALE" &&
        scene.specs.size()==1 && scene.specs[0].hostKey=="index:one",
        "identity cache hid duplicated COW requests or modified the source table");
}
inline void StaticRosterSnapshotsAreCopyOnWrite() {
    const auto model=AnimationTest::Bytes(),texture=AnimationParticleContinuityTest::Texture();
    const auto read=[&](std::string_view path,std::uint64_t) {
        return Result<AnimationResource>::Success({path.ends_with(".mdx")?model:texture,
            "cow-roster-fixture",std::string(64,'b')});
    };
    std::array<AnimationInstanceSpec,3> specs;
    for (std::size_t i=0;i<specs.size();++i) {
        specs[i].identity={i+1,47}; specs[i].hostKey="cow:"+std::to_string(i);
        specs[i].model.modelPath="same.mdx"; specs[i].sequenceIndex=0;
        specs[i].elapsedMs=100+i*100; specs[i].globalMs=*specs[i].elapsedMs; specs[i].playing=true;
    }
    auto service=Good(ModelAnimationService::Create({47,1,"cow-source"},specs,read));
    const auto original=service->Scene(); const auto frame=Good(service->Prepare(nullptr));
    AnimationInstanceDelta empty; empty.session={47,2,"cow-source"};
    const auto plan=Good(service->PrepareInstanceCatalog(empty,read));
    auto first=Good(service->PrepareInstanceDelta(*frame,*plan));
    auto second=Good(service->PrepareInstanceDelta(*frame,*plan));
    const auto firstScene=first.service->Scene(),secondScene=second.service->Scene();
    Check(firstScene!=original && secondScene!=firstScene &&
        firstScene->specs.SharesStorageWith(original->specs) && firstScene->sequences.SharesStorageWith(original->sequences) &&
        secondScene->specs.SharesStorageWith(firstScene->specs) && secondScene->sequences.SharesStorageWith(firstScene->sequences),
        "unchanged catalog/rebase copied static request or sequence tables instead of sharing immutable storage");
    AnimationInstanceDelta edit; edit.session={47,3,"cow-source"};
    edit.rows={{specs[0].identity,"cow:renamed"}};
    AnimationEdit clock; clock.identity=specs[0].identity; clock.elapsedMs=450; clock.globalMs=450; edit.edits={clock};
    const auto editPlan=Good(first.service->PrepareInstanceCatalog(edit,read));
    auto edited=Good(first.service->PrepareInstanceDelta(*first.frame,*editPlan));
    const auto editedScene=edited.service->Scene();
    Check(!editedScene->specs.SharesStorageWith(firstScene->specs) &&
        editedScene->sequences.SharesStorageWith(firstScene->sequences) &&
        editedScene->specs[0].hostKey=="cow:renamed" && original->specs[0].hostKey=="cow:0" &&
        firstScene->specs[0].hostKey=="cow:0" && secondScene->specs[0].hostKey=="cow:0" &&
        edited.frame->states[0].elapsedMs==450 && first.frame->states[0].elapsedMs==100 && second.frame->states[0].elapsedMs==100,
        "row/clock edit mutated a captured or sibling catalog, or unnecessarily copied unchanged sequences");
    AnimationInstanceDelta replace; replace.session={47,4,"cow-source"}; replace.removals={specs[1].identity};
    auto added=specs[1]; added.identity={4,47}; added.hostKey="cow:new"; added.model.position={500,0,0};
    replace.additions={added};
    const auto replacePlan=Good(edited.service->PrepareInstanceCatalog(replace,read));
    auto replaced=Good(edited.service->PrepareInstanceDelta(*edited.frame,*replacePlan));
    const auto replacedScene=replaced.service->Scene();
    Check(replaced.oldToNewRequests==std::vector<std::uint32_t>{0,UINT32_MAX,1} && replacedScene->specs.size()==3 &&
        replacedScene->specs[0].hostKey=="cow:renamed" && replacedScene->specs[1].identity==specs[2].identity &&
        replacedScene->specs[2].identity==added.identity && replacedScene->specs[2].model.position[0]==500 &&
        !replacedScene->specs.SharesStorageWith(editedScene->specs) &&
        !replacedScene->sequences.SharesStorageWith(editedScene->sequences) && editedScene->specs[1].identity==specs[1].identity &&
        editedScene->specs[2].model.position[0]==0 && secondScene->specs[1].identity==specs[1].identity,
        "stable compaction/append changed an earlier catalog or confused survivor/new request descriptors");
    AnimationInstanceDelta append; append.session={47,3,"cow-source"};
    added.identity={5,47}; added.hostKey="cow:sibling-new"; added.model.position={700,0,0}; append.additions={added};
    const auto appendPlan=Good(second.service->PrepareInstanceCatalog(append,read));
    auto appended=Good(second.service->PrepareInstanceDelta(*second.frame,*appendPlan));
    const auto appendedScene=appended.service->Scene();
    Check(appended.oldToNewRequests==std::vector<std::uint32_t>{0,1,2} && appendedScene->specs.size()==4 &&
        appendedScene->sequences.size()==4 && appendedScene->specs.back().identity==added.identity &&
        appendedScene->specs[0].hostKey=="cow:0" && original->specs.size()==3 && original->sequences.size()==3 &&
        secondScene->specs.size()==3 && secondScene->sequences.size()==3 && replacedScene->specs.size()==3 &&
        replacedScene->specs.back().identity.value==4 && replacedScene->specs.back().model.position[0]==500,
        "append-only branch moved or changed another branch's request/sequence rows");
    AnimationAdvance advance; advance.deltaMs=33;
    const auto flowing=Good(replaced.service->Prepare(replaced.frame.get(),advance));
    const auto sibling=Good(appended.service->Prepare(appended.frame.get()));
    const auto continuing=Good(service->Prepare(frame.get()));
    Check(flowing->states[0].elapsedMs==483 && sibling->states[0].elapsedMs==100 && continuing->states[0].elapsedMs==100,
        "static table sharing also shared a mutable clock or evaluator workspace");
}
inline void ReusablePlansKeepFrozenParticleWorkspacesIndependent() {
    const auto model=AnimationParticleContinuityTest::Model(),texture=AnimationParticleContinuityTest::Texture();
    const auto read=[&](std::string_view path,std::uint64_t) {
        return Result<AnimationResource>::Success({path.ends_with(".mdx")?model:texture,
            "frozen-delta-fixture",std::string(64,'a')});
    };
    std::array<AnimationInstanceSpec,2> specs;
    for (std::size_t i=0;i<specs.size();++i) {
        // 默认镜头中两实例同属一个512距离桶：19获准、20冻结；新增3替代19后仍优先于20。
        // 换回近处镜头后，20由更近的距离桶获准，才能独立检查冻结恢复而非误把合法准入当重置。
        specs[i].identity={20-i,7};specs[i].hostKey="frozen:"+std::to_string(i);specs[i].model.modelPath="particles.mdx";
        specs[i].model.position={static_cast<float>(i)*2048,0,0};specs[i].sequenceIndex=0;
        specs[i].elapsedMs=60000;specs[i].globalMs=60000;specs[i].playing=true;
    }
    AnimationLimits limits;limits.enableParticles=true;limits.retainUnsupportedStatic=true;
    limits.playbackPolicy=AnimationPlaybackPolicy::InteractivePreview;limits.particles.maxAnchorSamples=9;
    auto service=Good(ModelAnimationService::Create({7,1,"frozen-source"},specs,read,limits));
    auto view=AnimationParticleContinuityTest::View(-2048);auto frame=Good(service->Prepare(nullptr,view));
    const auto frozen=frame->particleCheckpoint->instances[0];
    view=AnimationParticleContinuityTest::View();view.deltaMs=33;frame=Good(service->Prepare(frame.get(),view));
    Check(frozen && frame->particleCheckpoint->instances[0]==frozen && frame->particleCheckpoint->frozen[0],
        "delta fixture did not establish an actual frozen checkpoint");
    const auto catalog=Good(service->CaptureInstanceCatalog());
    AnimationInstanceDelta delta;delta.session={7,2,"frozen-source"};delta.removals={specs[1].identity};
    auto added=specs[1];added.identity={3,7};added.hostKey="frozen:new";added.model.position[0]=1024;
    delta.additions.push_back(added);
    const auto plan=Good(catalog->PrepareInstanceCatalog(delta,read));
    frame=Good(service->Prepare(frame.get(),view)); // plan建立后原服务继续播放，重基必须使用这份最新时间。
    view.deltaMs=0;
    auto first=Good(service->PrepareInstanceDelta(*frame,*plan,view));
    auto second=Good(service->PrepareInstanceDelta(*frame,*plan,view));
    Check(first.service->Scene()->specs.SharesStorageWith(second.service->Scene()->specs) &&
        first.service->Scene()->sequences.SharesStorageWith(second.service->Scene()->sequences) &&
        !first.service->Scene()->specs.SharesStorageWith(service->Scene()->specs),
        "particle workspace rebase copied immutable request/sequence tables or aliased the changed original roster");
    std::cerr << "frozen-plan distinct-service=" << (first.service!=second.service)
        << " distinct-scene=" << (first.service->Scene()!=second.service->Scene())
        << " captured-source=" << (catalog->Scene()==service->Scene()) << '\n';
    const auto diagnose=[&](std::string_view label,const auto& value) {
        for (std::size_t i=0;i<std::min<std::size_t>(2,value.states.size());++i) {
            const auto& state=value.states[i];const auto& link=value.particleLinks[i];
            const auto& checkpoint=value.particleCheckpoint->instances[i];
            std::cerr << "frozen-plan " << label << " request=" << i << " identity=" << state.identity.value
                << " elapsed=" << state.elapsedMs << " global=" << state.globalMs
                << " checkpoint-original=" << (checkpoint==frozen)
                << " checkpoint-frozen=" << unsigned(value.particleCheckpoint->frozen[i])
                << " simulated=" << link.simulated << " frozen=" << link.frozen
                << " outside=" << link.outsideView << " reset=" << link.resetReason
                << " budget=" << link.rejectionCode << " time=" << link.simulationTimeMs
                << " hash=" << link.stateHash << '\n';
        }
    };
    diagnose("source",*frame);diagnose("first",*first.frame);diagnose("second",*second.frame);
    Check(first.service!=second.service && first.service->Scene()!=second.service->Scene() &&
        catalog->Scene()==service->Scene() && first.frame->states[0].elapsedMs==frame->states[0].elapsedMs &&
        second.frame->states[0].elapsedMs==frame->states[0].elapsedMs &&
        first.frame->particleCheckpoint->instances[0]==frozen && second.frame->particleCheckpoint->instances[0]==frozen &&
        first.frame->particleCheckpoint->frozen[0] && second.frame->particleCheckpoint->frozen[0] &&
        first.frame->particleLinks[0].resetReason.empty() && second.frame->particleLinks[0].resetReason.empty(),
        "repeated plan consumed shared mutable workspaces, reset frozen RNG or rebased to an old clock");
    auto resume=AnimationParticleContinuityTest::View(-2048);resume.deltaMs=33;
    const auto flowing=Good(first.service->Prepare(first.frame.get(),resume));
    view.deltaMs=33;const auto stillFrozen=Good(second.service->Prepare(second.frame.get(),view));
    const auto oldStillPlaying=Good(service->Prepare(frame.get(),view));
    Check(flowing->particleLinks[0].simulated && flowing->particleLinks[0].resetReason.empty() &&
        flowing->particleCheckpoint->instances[0]->state.emitters[0].random==frozen->state.emitters[0].random &&
        stillFrozen->particleCheckpoint->instances[0]==frozen && stillFrozen->particleCheckpoint->frozen[0] &&
        oldStillPlaying->particleCheckpoint->instances[0]==frozen && oldStillPlaying->states[0].elapsedMs==frame->states[0].elapsedMs+33,
        "one candidate's particle resume mutated another candidate or the continuing resident service");
}
inline void Run() {
    AnimationMaterialPrefetchTest::Run();
    PreviewAssetSnapshot();
    PreparedFormalAssets();
    NegativeCatalogEntriesSurvive();
    StableModelOrderAndBoundedMapping();
    IdentityIndexKeepsMutationGates();
    OwnedIdentityIndexChecksCompleteBinding();
    StaticRosterSnapshotsAreCopyOnWrite();
    ReusablePlansKeepFrozenParticleWorkspacesIndependent();
    const auto model=AnimationParticleContinuityTest::Model(),texture=AnimationParticleContinuityTest::Texture();
    std::map<std::string,unsigned> reads;
    const auto read=[&](std::string_view path,std::uint64_t) {
        ++reads[std::string(path)];
        return Result<AnimationResource>::Success({path.ends_with(".mdx")?model:texture,
            "original-delta-fixture",std::string(64,'d')});
    };
    std::array<AnimationInstanceSpec,2> specs;
    for (std::size_t i=0;i<specs.size();++i) {
        auto& spec=specs[i];spec.identity={i+1,7};spec.hostKey="row:"+std::to_string(i);
        spec.model.modelPath="same.mdx";spec.model.position={static_cast<float>(i)*200,0,0};
        spec.sequenceIndex=0;spec.elapsedMs=100+i*300;spec.globalMs=*spec.elapsedMs;spec.playing=true;
    }
    AnimationLimits limits;limits.retainUnsupportedStatic=true;limits.enableParticles=true;
    limits.playbackPolicy=AnimationPlaybackPolicy::InteractivePreview;
    auto service=Good(ModelAnimationService::Create({7,11,"delta-source"},specs,read,limits));
    const auto original=service->Scene();const auto initialReads=reads;
    auto advance=AnimationParticleContinuityTest::View(0);
    auto accepted=Good(service->Prepare(nullptr,advance));advance.deltaMs=50;
    accepted=Good(service->Prepare(accepted.get(),advance));
    Check(accepted->states[0].elapsedMs!=accepted->states[1].elapsedMs&&
        accepted->particleCheckpoint->instances[0]->state.emitters[0].random!=
            accepted->particleCheckpoint->instances[1]->state.emitters[0].random,
        "fixture did not establish independently clocked/RNG particle instances");
    AnimationInstanceDelta delta;delta.session={7,12,"delta-source"};delta.removals={specs[0].identity};
    delta.rows.push_back({specs[1].identity,"row:survivor"});
    auto added=specs[0];added.identity={3,7};added.hostKey="row:new";added.elapsedMs=700;added.globalMs=700;
    delta.additions.push_back(added);
    const auto immutableCatalog=Good(service->CaptureInstanceCatalog());
    const auto plan=Good(immutableCatalog->PrepareInstanceCatalog(delta,read));
    Check(!plan->AssetsChanged()&&reads==initialReads,"same-asset catalog read or rebuilt static asset");
    // Catalog工作期间播放仍接受新帧；必须用新的时钟/RNG重基，而不是构建plan时的旧帧。
    advance.deltaMs=33;accepted=Good(service->Prepare(accepted.get(),advance));
    const auto survivor=accepted->particleCheckpoint->instances[1];advance.deltaMs=0;
    auto rebound=Good(service->PrepareInstanceDelta(*accepted,*plan,advance));
    const auto scene=rebound.service->Scene();
    Check(!rebound.assetsChanged&&rebound.oldToNewRequests==std::vector<std::uint32_t>{UINT32_MAX,0}&&
        rebound.oldToNewRendered==std::vector<std::uint32_t>{UINT32_MAX,0}&&
        rebound.oldToNewParticleBindings==std::vector<std::uint32_t>{UINT32_MAX,0},
        "delta request/render/particle row maps reused a removed identity");
    Check(scene->specs.size()==2&&scene->specs[0].identity==specs[1].identity&&
        scene->specs[0].hostKey=="row:survivor"&&scene->specs[1].identity==added.identity&&
        rebound.frame->states[0].elapsedMs==accepted->states[1].elapsedMs&&
        rebound.frame->states[0].globalMs==accepted->states[1].globalMs&&
        rebound.frame->particleCheckpoint->instances[0]->stateHash==survivor->stateHash&&
        rebound.frame->particleCheckpoint->instances[0]->state.emitters[0].random==survivor->state.emitters[0].random&&
        rebound.frame->states[1].elapsedMs==700&&rebound.frame->particleLinks[1].spawned==0,
        "rebase reset survivor time/RNG or assigned deleted state to the new instance");
    Check(scene->vertices.data()==original->vertices.data()&&scene->models.data()==original->models.data()&&
        scene->meshes.data()==original->meshes.data()&&scene->pickGroupBounds.data()==original->pickGroupBounds.data()&&
        scene->geometry.vertices.data()==original->geometry.vertices.data()&&
        scene->geometry.indices.data()==original->geometry.indices.data()&&
        scene->geometry.textures.data()==original->geometry.textures.data()&&
        scene->particleTextures.data()==original->particleTextures.data(),
        "same-asset roster copied static model, picking or texture storage");
    const auto stale=rebound.service->Prepare(accepted.get(),advance);
    const auto wrongOwner=service->Prepare(rebound.frame.get(),advance);
    Check(!stale.Succeeded()&&stale.diagnostics.front().code=="ANIMATION_FRAME_STALE"&&
        !wrongOwner.Succeeded()&&wrongOwner.diagnostics.front().code=="ANIMATION_FRAME_STALE"&&
        original->specs[1].hostKey=="row:1","ordinary Prepare accepted foreign scene or mutated original rows");
    advance.deltaMs=33;auto flowing=Good(rebound.service->Prepare(rebound.frame.get(),advance));
    Check(flowing->states[0].elapsedMs==accepted->states[1].elapsedMs+33&&
        flowing->states[1].elapsedMs==733&&flowing->particleLinks[0].resetReason.empty()&&
        flowing->particleLinks[1].resetReason.empty()&&flowing->particleLinks[1].spawned>0,
        "survivor/new instance failed independent playback after delta");
    AnimationInstanceDelta foreign;foreign.session={7,13,"delta-source"};
    added.identity={4,7};added.hostKey="row:foreign";added.model.modelPath="new.mdx";foreign.additions={added};
    const auto appendPlan=Good(rebound.service->PrepareInstanceCatalog(foreign,read));
    auto residentReads=reads;residentReads.erase("new.mdx");
    Check(appendPlan->AssetsChanged()&&reads["new.mdx"]==1&&residentReads==initialReads,
        "new asset reread old model/texture or loaded its path more than once");
    advance.deltaMs=0;auto appended=Good(rebound.service->PrepareInstanceDelta(*flowing,*appendPlan,advance));
    const auto extended=appended.service->Scene();
    Check(appended.assetsChanged&&extended->models.size()==scene->models.size()+1&&
        extended->vertices.size()==scene->vertices.size()*2&&scene->vertices.data()==original->vertices.data()&&
        appended.frame->states[0].elapsedMs==flowing->states[0].elapsedMs&&
        appended.frame->particleCheckpoint->instances[0]->stateHash==flowing->particleCheckpoint->instances[0]->stateHash,
        "asset append duplicated fragment, mutated resident snapshot or reset survivor");
    const auto appendedReads=reads;foreign.session.documentRevision=14;
    foreign.additions[0].identity={5,7};foreign.additions[0].hostKey="row:second-foreign";
    const auto againPlan=Good(appended.service->PrepareInstanceCatalog(foreign,read));
    auto again=Good(appended.service->PrepareInstanceDelta(*appended.frame,*againPlan,advance));
    Check(!again.assetsChanged&&reads==appendedReads&&again.service->Scene()->models.size()==extended->models.size()&&
        again.service->Scene()->vertices.data()==extended->vertices.data()&&again.frame->states.back().identity.value==5&&
        again.frame->particleLinks.back().spawned==0,"resident new path was reloaded or newborn inherited prior particle state");
    std::cout<<"instance_delta_latest_frame_rebase=1 row_remap_identity=1 survivor_clock_rng=1 "
        "same_asset_static_addresses=1 old_prepare_strict=1 new_path_read_once=1 fragment_once=1\n";
}
}
