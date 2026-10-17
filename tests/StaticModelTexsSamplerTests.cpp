#include "War3MapCore/Hash.h"
#include "War3MapCore/Render/AnimationRenderer.h"
#include "../src/Render/Scene/ModelSkinInput.h"
#include "../src/Render/Particles/ParticleInput.h"
#include "ModelAnimationMaterialTextures.h"

#include <map>

namespace {
using namespace War3Map;
using namespace War3Map::Render;
using AnimationParticleContinuityTest::Check;
using AnimationParticleContinuityTest::Good;

std::vector<std::byte> Model(std::uint32_t flags) {
    auto bytes = AnimationTest::Bytes();
    for (std::size_t at = 4; at + 8 <= bytes.size();) {
        std::uint32_t size = 0; std::memcpy(&size, bytes.data() + at + 4, 4);
        Check(size <= bytes.size() - at - 8, "source TEXS fixture chunk is truncated");
        if (!std::memcmp(bytes.data() + at, "TEXS", 4)) {
            Check(size == 536, "source TEXS fixture layout changed");
            for (std::size_t index = 0; index < 2; ++index) {
                auto* entry = bytes.data() + at + 8 + index * 268;
                std::fill_n(entry, 268, std::byte{});
                constexpr char path[] = "Textures\\Crate.blp";
                std::memcpy(entry + 4, path, sizeof(path));
                std::memcpy(entry + 264, &flags, 4);
            }
            return bytes;
        }
        at += 8 + size;
    }
    throw std::runtime_error("source TEXS fixture is missing");
}

struct Resources {
    std::map<std::string, std::vector<std::byte>> models;
    const std::vector<std::byte> image = AnimationParticleContinuityTest::Texture();
    bool resident = true;
    unsigned textureReads = 0;
    ModelFileReader StaticReader() {
        return [this](std::string_view path) -> Result<std::vector<std::byte>> {
            if (const auto found = models.find(std::string(path)); found != models.end())
                return Result<std::vector<std::byte>>::Success(found->second);
            ++textureReads;
            if (resident) return Result<std::vector<std::byte>>::Success(image);
            return Result<std::vector<std::byte>>::Failure("Test", "MISSING", "intentional white fallback");
        };
    }
    AnimationResourceReader AnimatedReader() {
        return [this](std::string_view path, std::uint64_t) -> Result<AnimationResource> {
            if (const auto found = models.find(std::string(path)); found != models.end())
                return Result<AnimationResource>::Success({found->second, "source-texs-fixture", Good(Sha256(found->second))});
            ++textureReads;
            if (resident) return Result<AnimationResource>::Success({image, "source-texs-fixture", Good(Sha256(image))});
            return Result<AnimationResource>::Failure("Test", "MISSING", "intentional white fallback");
        };
    }
};

ModelInstanceRequest Request(std::string path) {
    ModelInstanceRequest value; value.modelPath = std::move(path); return value;
}

std::vector<std::uint32_t> Effective(const WorldModelScene& scene) {
    Check(scene.layerSamplerFlags.empty() || scene.layerSamplerFlags.size() == scene.layers.size(), "partial sampler array");
    if (scene.layerSamplerFlags.empty()) return std::vector<std::uint32_t>(scene.layers.size(), 3);
    return {scene.layerSamplerFlags.cbegin(), scene.layerSamplerFlags.cend()};
}

void CheckModel(const WorldModelScene& scene, std::uint32_t model, std::uint32_t expected) {
    const auto flags = Effective(scene);
    const auto entry = scene.models.at(model);
    Check(entry.meshCount > 0, "source model has no emitted geometry");
    for (std::uint32_t i = 0; i < entry.meshCount; ++i) {
        const auto mesh = scene.meshes.at(entry.firstMesh + i);
        for (std::uint32_t layer = 0; layer < mesh.layerCount; ++layer)
            Check(flags.at(mesh.firstLayer + layer) == expected, "emitted layer lost source TEXS flags");
    }
}

void StaticSourceAndRefresh() {
    Resources resources;
    std::vector<ModelInstanceRequest> requests;
    // 先legacy、后首次显式值、再多个growth；所有源TEXS共享同一张真实图片。
    for (const auto flags : {3u, 0u, 1u, 2u, 3u, 1u}) {
        const auto path = "model" + std::to_string(requests.size()) + ".mdx";
        resources.models.emplace(path, Model(flags)); requests.push_back(Request(path));
    }
    const auto legacy = Good(BuildWorldModelScene(std::span(requests.data(), 1), resources.StaticReader()));
    Check(legacy.layerSamplerFlags.empty(), "all3 source acquired an unnecessary static sampler sidecar");
    auto scene = Good(BuildWorldModelScene(requests, resources.StaticReader()));
    const std::array expected{3u, 0u, 1u, 2u, 3u, 1u};
    Check(scene.models.size() == expected.size() && scene.textures.size() == 1, "source models duplicated shared RGBA or were skipped");
    for (std::uint32_t model = 0; model < expected.size(); ++model) CheckModel(scene, model, expected[model]);
    const auto snapshot = scene;
    BakedModelMesh baked; baked.texturePath = "Textures\\Crate.blp"; baked.samplerFlags = 2;
    baked.vertices = {{0,0,0,0,0,1,0,0}, {1,0,0,0,0,1,1,0}, {0,1,0,0,0,1,0,1}}; baked.indices = {0,1,2};
    Check(ReplaceBakedMeshes(scene, std::span(&baked, 1)), "source sampler refresh failed");
    CheckModel(scene, 0, 2);
    for (std::uint32_t model = 0; model < expected.size(); ++model) CheckModel(scene, model + 1, expected[model]);
    Check(Effective(snapshot) != Effective(scene) && snapshot.textures.SharesStorageWith(scene.textures), "refresh changed source snapshot or RGBA cache");
    Check(ReplaceBakedMeshes(scene, {}), "source sampler prefix removal failed");
    Check(Effective(scene) == Effective(snapshot), "removing baked prefix lost ordinary source flags");

    auto owned = Request("owned-helper"); owned.ownedStaticAsset = Good(CreateCheckerBlockerAsset(std::array<float,2>{16,16}));
    const std::array mixed{requests[1], owned, requests[0]};
    const auto mixedScene = Good(BuildWorldModelScene(mixed, resources.StaticReader(), {}, std::span(&baked, 1)));
    CheckModel(mixedScene, 0, 2); CheckModel(mixedScene, 1, 0); CheckModel(mixedScene, 2, 3); CheckModel(mixedScene, 3, 3);
    const auto descriptor = DescribeModelScene(snapshot);
    Check(descriptor.validStaticSamplers && descriptor.StaticScene()->structSize > sizeof(War3RenderModelSceneV2), "actual source flags did not reach static ABI");
    Check(!CheckStaticModelSamplerSupport(nullptr, nullptr, snapshot).Succeeded(), "nonlegacy static source bypassed capability gate");
}

void MaterialBudgetAndInvalidFlags() {
    Resources resources; resources.models.emplace("source.mdx", Model(0)); const auto request = Request("source.mdx");
    const auto baseline = Good(BuildWorldModelScene(std::span(&request, 1), resources.StaticReader()));
    const auto exact = std::uint64_t(baseline.textures.front().rgba.size()) + baseline.layerSamplerFlags.capacity() * sizeof(std::uint32_t)
        + sizeof(War3RenderStaticSamplerModelSceneV2) - sizeof(War3RenderModelSceneV2);
    WorldModelLimits limits; limits.maxTotalTextureBytes = exact;
    Check(BuildWorldModelScene(std::span(&request, 1), resources.StaticReader(), limits).Succeeded(), "exact source sampler resident budget rejected");
    limits.maxTotalTextureBytes = exact - 1;
    const auto rejected = BuildWorldModelScene(std::span(&request, 1), resources.StaticReader(), limits);
    Check(!rejected.Succeeded() && !rejected.value && !rejected.diagnostics.empty() && rejected.diagnostics.front().code == "WORLD_STATIC_SAMPLER_BUDGET",
        "source sampler overhead became an accepted white texture or partial geometry");
    auto parsed = std::make_shared<MdxModel>(Good(ParseMdx(resources.models.at("source.mdx")))); parsed->textures[0].flags = 4;
    const auto invalid = BuildAnimatedModelGeometry(std::span(&request, 1), resources.StaticReader(),
        [&](std::string_view) { return ModelGeometryAsset{parsed, false, {}}; });
    Check(!invalid.Succeeded() && !invalid.value, "unknown emitted TEXS flags became wrap3");
    Check(Effective(baseline).front() == 0 && baseline.textures.size() == 1, "failed build mutated an already published scene");

    // 没有图片常驻时隔离第二次reserve：预算必须覆盖仍存活的旧capacity和新capacity。
    resources.resident = false; resources.models.emplace("second.mdx", Model(1));
    const std::array growing{request, Request("second.mdx")};
    const auto one = Good(BuildWorldModelScene(std::span(&request,1), resources.StaticReader()));
    const auto two = Good(BuildWorldModelScene(growing, resources.StaticReader()));
    Check(two.layerSamplerFlags.capacity() > one.layerSamplerFlags.capacity(), "growth fixture did not reallocate sampler storage");
    const auto peak = std::uint64_t(one.layerSamplerFlags.capacity() + two.layerSamplerFlags.capacity()) * sizeof(std::uint32_t)
        + sizeof(War3RenderStaticSamplerModelSceneV2) - sizeof(War3RenderModelSceneV2);
    limits.maxTotalTextureBytes = peak;
    Check(BuildWorldModelScene(growing, resources.StaticReader(), limits).Succeeded(), "exact old/new sampler growth peak rejected");
    limits.maxTotalTextureBytes = peak - 1;
    const auto overflow = BuildWorldModelScene(growing, resources.StaticReader(), limits);
    Check(!overflow.Succeeded() && !overflow.value && overflow.diagnostics.front().code == "WORLD_STATIC_SAMPLER_BUDGET",
        "sampler realloc checked only the final resident allocation");
}

AnimationInstanceSpec Spec(std::uint64_t identity, std::string path) {
    AnimationInstanceSpec value; value.identity = {identity,7}; value.hostKey = "texs:" + std::to_string(identity);
    value.model = Request(std::move(path)); value.sequenceIndex = 0; value.elapsedMs = 200; value.playing = true; return value;
}

void SkinSourceAndCapabilities() {
    for (const bool resident : {true, false}) for (const auto flags : {0u, 1u, 2u, 3u}) {
        Resources resources; resources.resident = resident; resources.models.emplace("source.mdx", Model(flags));
        const auto spec = Spec(1, "source.mdx");
        const auto service = Good(ModelAnimationService::Create({7,1,"source-skin"}, std::span(&spec,1), resources.AnimatedReader()));
        const auto scene = service->Scene(); const bool explicitState = resident && flags != 3;
        Check(scene->animatedInstances == 1 && scene->dynamicMaterialStates == explicitState, "skin source did not select its actual resident sampler path");
        CheckModel(scene->geometry, 0, flags);
        const auto first = Good(service->Prepare(nullptr)); AnimationAdvance advance; advance.deltaMs = 33;
        const auto next = Good(service->Prepare(first.get(), advance));
        if (!explicitState) { Check(first->materialStates.empty() && next->materialStates.empty(), "legacy or white fallback acquired a sampling ABI"); continue; }
        Check(!first->materialStates.empty(), "skin source flags never reached first frame");
        for (const auto& state : first->materialStates) Check(state.samplerFlags == flags && state.textureIndex != WAR3_RENDER_MODEL_NO_TEXTURE, "skin first frame guessed wrap flags");
        for (const auto& state : next->materialStates) Check(state.samplerFlags == flags, "skin update lost source flags");
        AnimationMaterialTextureTest::CapabilityBackend backend; backend.materialAlpha = true;
        War3RenderPluginApiV2 api{}; api.structSize = sizeof(api); api.apiVersion = WAR3_RENDER_PLUGIN_API_V2;
        api.submitSkinnedModelScene = AnimationMaterialTextureTest::CapabilitySubmit;
        api.updateSkinMatrices = AnimationMaterialTextureTest::CapabilityUpdate;
        api.getSkinStatus = AnimationMaterialTextureTest::CapabilityStatus;
        api.captureSkinnedFrameRgba = [](War3RendererHandle, void*, std::uint64_t, War3RenderSkinnedCapturedFrameV2*) -> std::int32_t { return -1; };
        AnimationRenderer renderer(&api, &backend); const auto denied = renderer.Submit(scene, first);
        Check(!denied.Succeeded() && denied.diagnostics.front().code == "ANIMATION_MATERIAL_STATE_BACKEND_UNSUPPORTED" && backend.uploads == 0, "skin source bypassed material-state capability");
        backend.materialState = true;
        Check(renderer.Submit(scene, first).Succeeded() && renderer.Update(next).Succeeded() && backend.pose.materialStates.front().samplerFlags == flags,
            "source flags did not survive shared ABI submit/update validators");
    }
}

void SkinAppendTransaction() {
    Resources resources; resources.models.emplace("legacy.mdx", Model(3)); resources.models.emplace("clamp.mdx", Model(0));
    const auto original = Spec(1, "legacy.mdx"); const auto added = Spec(2, "clamp.mdx");
    const auto service = Good(ModelAnimationService::Create({7,1,"source-append"}, std::span(&original,1), resources.AnimatedReader()));
    const auto scene = service->Scene(); const auto frame = Good(service->Prepare(nullptr));
    const auto catalog = Good(service->CaptureInstanceCatalog()); const auto packet = Good(catalog->PrepareStaticAssets(added.model, resources.AnimatedReader()));
    Check(service->Scene() == scene && !scene->dynamicMaterialStates && scene->geometry.layerSamplerFlags.empty(), "source prefetch mutated resident scene");
    AnimationInstanceDelta delta; delta.session = {7,2,"source-append"}; delta.additions = {added};
    const auto plan = Good(catalog->PrepareInstanceCatalog(delta, resources.AnimatedReader(), {}, nullptr, packet));
    const auto result = Good(service->PrepareInstanceDelta(*frame, *plan));
    Check(result.service->Scene()->dynamicMaterialStates && service->Scene() == scene && !scene->dynamicMaterialStates,
        "clamped source append failed to promote or changed resident snapshot");
    CheckModel(result.service->Scene()->geometry, 0, 3); CheckModel(result.service->Scene()->geometry, 1, 0);
    Check(!result.frame->materialStates.empty(), "appended skin source has no actual material-state payload");
    for (const auto& slice : result.frame->materialStateSlices)
        Check(slice.count > 0, "appended source layer range was dropped");
    AnimationLimits tight; tight.geometry.maxModels = 1;
    const auto bounded = Good(ModelAnimationService::Create({7,1,"source-append"}, std::span(&original,1), resources.AnimatedReader(), tight));
    const auto boundedScene = bounded->Scene(); const auto boundedCatalog = Good(bounded->CaptureInstanceCatalog());
    const auto denied = boundedCatalog->PrepareStaticAssets(added.model, resources.AnimatedReader());
    Check(!denied.Succeeded() && bounded->Scene() == boundedScene && !boundedScene->dynamicMaterialStates,
        "overbudget source append became partial geometry or mutated resident state");
}
}

int main() try {
    StaticSourceAndRefresh(); MaterialBudgetAndInvalidFlags(); SkinSourceAndCapabilities(); SkinAppendTransaction();
    std::cout << "static/skin source TEXS sampler tests passed\n"; return 0;
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
