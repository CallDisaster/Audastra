#pragma once
#include "War3MapCore/Render/ModelSceneLayerRenderer.h"
#include "RenderSkinFixture.h"

// 天空附加层（地图天空模型预览）：最先画、以镜头为原点（镜头平移时天空在屏幕上不动）、
// 不受雾影响、画完清深度所以不挡场景；能力位、未知角色、带粒子与更换角色一律拒绝。复用 RenderD3D11Tests 的 Require 等。
void TestSkyLayer(const War3RenderPluginApiV2* api, const War3RenderCreateInfoV2& info) {
    using War3Map::Render::ModelSceneLayerRenderer;
    RendererOwner renderer{api, api->createRenderer(&info)};
    Require(renderer.handle != nullptr, "sky renderer unavailable");
    std::uint32_t capabilities = 0;
    Require(api->getModelLayerCapabilities(renderer.handle, &capabilities) == 0
        && (capabilities & WAR3_RENDER_MODEL_LAYER_CAP_SKY)
        && ModelSceneLayerRenderer::RoleSupported(api, renderer.handle, WAR3_RENDER_MODEL_LAYER_SKY), "sky capability missing");
    auto old = *api; old.structSize = static_cast<std::uint32_t>(offsetof(War3RenderPluginApiV2, getModelLayerCapabilities));
    Require(!ModelSceneLayerRenderer::RoleSupported(&old, renderer.handle, WAR3_RENDER_MODEL_LAYER_SKY),
        "provider without capabilities claims sky");
    War3RenderCameraV2 camera{sizeof(camera), 0, 0, 0, 3500, -1.5707963f, 1.5f};
    Require(api->setCamera(renderer.handle, &camera) == 0, "sky camera rejected");
    // 镜头外的小三角关掉默认网格线，背景只剩清屏色。
    const War3RenderVertexV2 backgroundVertices[]{{10000, 10000, 0, .045f, .060f, .075f},
        {10128, 10000, 0, .045f, .060f, .075f}, {10000, 10128, 0, .045f, .060f, .075f}};
    const std::uint32_t backgroundIndices[]{0, 1, 2};
    const War3RenderMeshV2 backgroundMesh{sizeof(backgroundMesh), 3, 3, 1901, backgroundVertices, backgroundIndices};
    Require(api->submitMesh(renderer.handle, &backgroundMesh) == 0, "sky background rejected");
    // 场景：x=±500 处红、绿两块（z=20）。
    RenderSkinFixture world; auto worldPose = world.Update(); auto worldScene = world.Scene(worldPose);
    Require(api->submitSkinnedModelScene(renderer.handle, &worldScene) == 0, "sky world rejected");
    War3RenderModelLayersTargetV2 target{sizeof(target)};
    War3RenderLayeredCapturedFrameV2 receipt{sizeof(receipt)};
    const auto capture = [&] {
        Require(api->getModelLayersTarget(renderer.handle, &target) == 0, "sky target query failed");
        std::vector<std::uint8_t> pixels(static_cast<std::size_t>(info.width) * info.height * 4);
        Require(api->captureLayeredFrameRgba(renderer.handle, target.layersRevision, pixels.data(), pixels.size(), &receipt) == 0,
            "sky capture failed");
        return pixels;
    };
    const std::array<int, 3> background{11, 15, 19}, red{255, 0, 0}, blue{0, 0, 255}, fog{128, 128, 128};
    const auto original = capture();
    RequireWorldColor(original, receipt.world.frame.view, 100, -300, background, 2);
    RequireWorldColor(original, receipt.world.frame.view, -500, 0, red, 1);

    // 天空：以镜头为原点、正下方 1000 处 400×400 的蓝色方块。镜头在 (0, -247.6, 3491.2)，
    // 方块盖住的地面范围约为 x ±698、y -946..450（相似三角形 ×3.49）。
    RenderSkinFixture sky;
    for (auto& instance : sky.instances) {
        instance.instance.transform[11] = -1020;
        instance.instance.color[0] = 0; instance.instance.color[1] = 0; instance.instance.color[2] = 1;
    }
    for (auto& group : sky.groups) group.values[9] = 0;
    auto skyPose = sky.Update(); skyPose.sceneVersion = 2; auto skyScene = sky.Scene(skyPose);
    War3RenderModelSceneLayerV2 layer{sizeof(layer), 0, 701, target.parentModelEpoch, 1, 0, &skyScene, nullptr,
        WAR3_RENDER_MODEL_LAYER_SKY, 0};
    auto unknown = layer; unknown.role = 3;
    Require(api->submitModelSceneLayer(renderer.handle, &unknown) != 0, "unknown layer role accepted");
    const War3RenderParticleSceneV2 particles{};
    auto withParticles = layer; withParticles.models = nullptr; withParticles.particles = &particles;
    Require(api->submitModelSceneLayer(renderer.handle, &withParticles) != 0, "sky layer with particles accepted");
    Require(capture() == original, "rejected sky layers changed pixels");
    Require(api->submitModelSceneLayer(renderer.handle, &layer) == 0, "sky layer rejected");
    const auto withSky = capture();
    Require(receipt.layerCount == 1 && receipt.layers[0].revision == 1, "sky layer receipt missing");
    RequireWorldColor(withSky, receipt.world.frame.view, 100, -300, blue, 2);
    // 天空比场景离镜头近（1000 对 3491），但画完清了深度，场景照样盖在上面。
    RequireWorldColor(withSky, receipt.world.frame.view, -500, 0, red, 1);
    // 镜头平移 800：天空跟着走（地面范围变为 y -146..1250），旧位置露出背景。
    camera.targetY = 800;
    Require(api->setCamera(renderer.handle, &camera) == 0, "sky moved camera rejected");
    const auto moved = capture();
    RequireWorldColor(moved, receipt.world.frame.view, 100, 500, blue, 2);
    RequireWorldColor(moved, receipt.world.frame.view, 100, -400, background, 2);
    camera.targetY = 0;
    Require(api->setCamera(renderer.handle, &camera) == 0, "sky camera restore rejected");
    Require(capture() == withSky, "sky frame not reproducible after camera restore");
    // 线性雾 100..200：场景整块变成雾色，天空不受雾影响。
    War3RenderEnvironmentV2 environment{sizeof(environment), {0, 0, 1}, {0.6f, 0.6f, 0.6f}, {0.3f, 0.3f, 0.3f},
        WAR3_RENDER_FOG_LINEAR, 100, 200, 0.01f, {0.5f, 0.5f, 0.5f}, 0, 0, 0, 0, 0, 0, nullptr};
    Require(api->setEnvironment(renderer.handle, &environment) == 0, "sky fog rejected");
    const auto fogged = capture();
    RequireWorldColor(fogged, receipt.world.frame.view, -500, 0, fog, 2);
    RequireWorldColor(fogged, receipt.world.frame.view, 100, -300, blue, 2);
    environment.fogMode = WAR3_RENDER_FOG_NONE;
    Require(api->setEnvironment(renderer.handle, &environment) == 0, "sky fog clear rejected");
    // 同一层号不能改成普通角色；移除后恢复原画面。
    auto ordinary = layer; ordinary.role = WAR3_RENDER_MODEL_LAYER_MODEL; ordinary.revision = 2; ordinary.expectedRevision = 1;
    Require(api->submitModelSceneLayer(renderer.handle, &ordinary) != 0, "sky layer role change accepted");
    Require(api->removeModelSceneLayer(renderer.handle, 701, layer.parentModelEpoch, 1) == 0, "sky layer removal rejected");
    const auto removed = capture();
    Require(receipt.layerCount == 0, "sky layer still present");
    RequireWorldColor(removed, receipt.world.frame.view, 100, -300, background, 2);
    std::cout << "sky layer WARP: camera-origin first pass, depth cleared, follows camera, unfogged, role/particle rejection passed\n";
}
