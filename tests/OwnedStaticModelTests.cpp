#include "War3MapCore/Render/WorldModelScene.h"
#include "War3MapCore/Render/ModelAnimation.h"
#include "War3MapCore/Render/ModelPicking.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <utility>

namespace {
using namespace War3Map;
using namespace War3Map::Render;
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class T> T Good(Result<T> result) {
    if (!result.Succeeded()) throw std::runtime_error(result.diagnostics.empty() ? "Missing diagnostic" : result.diagnostics.front().code);
    return std::move(*result.value);
}
ModelInstanceRequest Request(std::shared_ptr<const OwnedStaticModelAsset> asset, float x = 0) {
    ModelInstanceRequest r; r.modelPath = "Objects\\InvisibleBlocker.mdx"; r.ownedStaticAsset = std::move(asset); r.position = {x, 0, 0}; return r;
}
void Factory() {
    const auto asset = Good(CreateCheckerBlockerAsset(std::array<float, 2>{64, 96}));
    const auto same = Good(CreateCheckerBlockerAsset(std::array<float, 2>{64, 96}));
    Check(asset->IdentitySha256() == same->IdentitySha256(), "same owned content changed identity");
    auto ordinary = Request(nullptr); ordinary.modelPath = ModelAssetKey(Request(asset));
    ModelTextureRgba ordinaryTexture; ordinaryTexture.path = ModelTextureAssetKey(asset->Textures()[0]);
    Check(ModelAssetKey(ordinary) != ModelAssetKey(Request(asset)) &&
        ModelTextureAssetKey(ordinaryTexture) != ModelTextureAssetKey(asset->Textures()[0]) &&
        ModelAssetKey(Request(nullptr)) == "objects\\invisibleblocker.mdx",
        "ordinary reserved-control paths borrowed owned cache or changed legal legacy keys");
    std::vector<OwnedModelMesh> meshes(asset->Meshes().begin(), asset->Meshes().end());
    std::vector<ModelTextureRgba> textures(asset->Textures().begin(), asset->Textures().end());
    const auto frozen = Good(FreezeOwnedStaticModelAsset(meshes, textures));
    auto sourceChain=textures;
    sourceChain[0].additionalMips.push_back({4,4,std::vector<std::byte>(4*4*4)});
    Check(!FreezeOwnedStaticModelAsset(meshes,sourceChain).Succeeded(),"owned v1 silently ignored new lower mip bytes");
    sourceChain[0].additionalMips.clear();
    sourceChain[0].mipPolicyVersion=ModelTextureMipPolicyVersion;
    Check(!FreezeOwnedStaticModelAsset(meshes,sourceChain).Succeeded(),"owned v1 silently ignored new source/policy state");
    Check(Good(FreezeOwnedStaticModelAsset(meshes,textures))->IdentitySha256()==frozen->IdentitySha256(),
        "rejecting new chains changed existing owned v1 canonical identity");
    meshes[0].vertices[0].x = -123; textures[0].rgba[0] ^= 255; textures[0].ownedIdentity = "caller-sha";
    Check(frozen->Meshes()[0].vertices[0].x == -32 && frozen->Textures()[0].rgba[0] == 12,
        "owned asset borrowed caller mutable storage");
    Check(Good(FreezeOwnedStaticModelAsset(meshes, textures))->IdentitySha256() != frozen->IdentitySha256(),
        "geometry/texture changes or caller identity reused old cache");
    const auto originalFlags = meshes[0].flags;
    const auto beforeFlag = Good(FreezeOwnedStaticModelAsset(meshes, textures));
    meshes[0].flags = originalFlags | WAR3_RENDER_MODEL_UNFOGGED;
    const auto unfogged = Good(FreezeOwnedStaticModelAsset(meshes, textures));
    Check((unfogged->Meshes()[0].flags & WAR3_RENDER_MODEL_UNFOGGED) != 0
        && unfogged->IdentitySha256() != beforeFlag->IdentitySha256(),
        "owned Unfogged must survive freezing and have a distinct content identity");
    meshes[0].flags = 8;
    Check(!FreezeOwnedStaticModelAsset(meshes, textures).Succeeded(), "unknown owned layer flag accepted");
    meshes[0].flags = originalFlags;
    meshes[0].indices[0] = UINT16_MAX;
    Check(!FreezeOwnedStaticModelAsset(meshes, textures).Succeeded(), "bad owned index accepted");
    meshes.assign(asset->Meshes().begin(), asset->Meshes().end()); meshes[0].alpha = std::numeric_limits<float>::quiet_NaN();
    Check(!FreezeOwnedStaticModelAsset(meshes, textures).Succeeded(), "invalid layer alpha accepted");
    WorldModelLimits budget; budget.maxModelBytes = 16;
    Check(!CreateCheckerBlockerAsset(std::array<float, 2>{64, 96}, budget).Succeeded(), "owned byte budget ignored");
    const auto unknown = CreateCheckerBlockerAsset(std::nullopt);
    Check(unknown.Succeeded() && (*unknown.value)->Meshes()[0].vertices[0].x == -4 &&
        std::any_of(unknown.diagnostics.begin(), unknown.diagnostics.end(), [](const auto& d) { return d.code == "WORLD_HELPER_FOOTPRINT_UNKNOWN"; }),
        "unknown footprint became a claimed 64-unit range");
}
void StaticRoster() {
    auto asset = Good(CreateCheckerBlockerAsset(std::array<float, 2>{64, 96}));
    std::array<ModelInstanceRequest, 3> requests{Request(asset), Request(asset, 200), Request(nullptr, 400)};
    unsigned reads = 0;
    auto scene = Good(BuildWorldModelScene(requests, [&](std::string_view) {
        ++reads; return Result<std::vector<std::byte>>::Failure("Test", "MISSING", "actual model absent");
    }));
    Check(reads == 1 && scene.instances.size() == 2 && scene.models.size() == 1 && scene.textures.size() == 1 &&
        scene.instanceRequests == std::vector<std::uint32_t>{0, 1} && scene.skippedInstances == 1,
        "helper hit MDX reader, lost normal request identity or hid same-path model failure");
    Check(ModelAssetKey(requests[0]) != ModelAssetKey(requests[2]) && !scene.textures[0].ownedIdentity.empty(), "typed key shared resource namespace");
    const auto& immutable = std::as_const(scene);
    const auto* vertices = immutable.vertices.data(); const auto* pixels = immutable.textures[0].rgba.data();
    std::vector<ModelInstanceRequest> next(20, requests[0]);
    for (std::size_t i = 0; i < next.size(); ++i) next[i].position[0] = static_cast<float>(i) * 50;
    Check(ReplaceModelInstances(scene, requests, next) && reads == 1 && scene.instances.size() == 20 &&
        std::as_const(scene).vertices.data() == vertices && std::as_const(scene).textures[0].rgba.data() == pixels,
        "owned roster movement rebuilt resident assets");
    auto other = next; other[0].ownedStaticAsset = Good(CreateCheckerBlockerAsset(std::array<float, 2>{96, 96}));
    Check(!ReplaceModelInstances(scene, next, other) && scene.instances.size() == 20, "unknown owned variant mutated old roster");
    Check(ReplaceModelInstances(scene, next, {}) && scene.instances.empty() && scene.models.size() == 1, "empty helper roster discarded resident assets");
}
void AnimationAndPicking() {
    const auto asset = Good(CreateCheckerBlockerAsset(std::array<float, 2>{64, 96}));
    AnimationInstanceSpec spec; spec.identity = {17, 7}; spec.hostKey = "real-placement"; spec.model = Request(asset); spec.playing = true;
    unsigned reads = 0;
    const AnimationResourceReader read = [&](std::string_view, std::uint64_t) {
        ++reads; return Result<AnimationResource>::Failure("Test", "MISSING", "helpers must not read resources");
    };
    AnimationLimits limits; limits.enableParticles = true;
    auto service = Good(ModelAnimationService::Create({7, 21, "owned-source"}, std::span(&spec, 1), read, limits));
    auto frame = Good(service->Prepare(nullptr)); const auto scene = service->Scene();
    Check(!reads && scene->unavailableInstances.empty() && !scene->particleInstanceCount && !frame->poses[0].animated &&
        scene->geometry.instanceRequests == std::vector<std::uint32_t>{0} && scene->geometry.modelPaths[0] == "objects\\invisibleblocker.mdx",
        "explicit helper became unsupported MDX, baked mesh, particle or false resource path");
    ModelPickBinding binding{21, 7, scene->sceneVersion, frame->contentVersion, 31, 32, {33, 34, 35, 36}};
    const auto hit = Good(PickAnimatedModelInstances(*scene, *frame, {binding, {1}, {1}}, binding, {{0, 0, 100}, {0, 0, -1}}, 200));
    Check(hit.hits.size() == 1 && hit.hits[0].identity == spec.identity && hit.hits[0].requestIndex == 0 && hit.hits[0].grade == 8,
        "owned helper cannot be picked by actual triangle with original stable identity");
    const auto hidden = Good(PickAnimatedModelInstances(*scene, *frame, {binding, {0}, {1}}, binding, {{0, 0, 100}, {0, 0, -1}}, 200));
    Check(hidden.hits.empty(), "hidden helper remained selectable");
    auto wrong = *scene; wrong.geometry.modelAssetKeys[0] = "unbound-owned-key";
    Check(!PickAnimatedModelInstances(wrong, *frame, {binding, {1}, {1}}, binding, {{0, 0, 100}, {0, 0, -1}}, 200).Succeeded(),
        "typed pointer alone bypassed actual geometry identity binding");
    AnimationInstanceDelta delta; delta.session = {7, 22, "owned-source"}; delta.removals = {spec.identity};
    auto added = spec; added.identity = {18, 7}; added.hostKey = "new-placement";
    const auto other = Good(CreateCheckerBlockerAsset(std::array<float, 2>{128, 96}));
    Check(other->Textures()[0].ownedIdentity == asset->Textures()[0].ownedIdentity && other->IdentitySha256() != asset->IdentitySha256(),
        "identical checker texture cannot reuse resident texture across footprint variants");
    auto mesh = other->Meshes()[0]; auto image = other->Textures()[0]; image.rgba[0] = 33;
    added.model = Request(Good(FreezeOwnedStaticModelAsset(std::span(&mesh, 1), std::span(&image, 1))), 300); delta.additions = {added};
    const auto plan = Good(service->PrepareInstanceCatalog(delta, read));
    const auto appended = Good(service->PrepareInstanceDelta(*frame, *plan, {}));
    Check(plan->AssetsChanged() && !reads && appended.frame->states[0].identity == added.identity &&
        appended.service->Scene()->geometry.modelAssetKeys.size() == 2 && appended.service->Scene()->geometry.textures.size() == 2 &&
        scene->geometry.models.size() == 1 && scene->geometry.textures.size() == 1,
        "owned asset append lost typed identities, merged RGBA by empty path, or mutated old scene");
    AnimationAdvance offscreen;
    offscreen.worldView = AnimationView{{sizeof(War3RenderCameraV2), 0, 1000000, 0, 500, 0, 0.6f}, 640, 480, 64};
    const auto outside = Good(appended.service->Prepare(appended.frame.get(), offscreen));
    Check(outside->poses[0].outsideView && !outside->poses[0].animated && !reads,
        "finite owned helper bounds fell back to unknown full-scene visibility or pose evaluation");
}
} // namespace
int main() try {
    Factory(); StaticRoster(); AnimationAndPicking();
    std::cout << "PASS owned static content, mutation isolation, ordinary roster and precise picking\n"; return 0;
} catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
