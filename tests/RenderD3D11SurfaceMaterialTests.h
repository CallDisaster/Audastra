#pragma once
#include "War3MapCore/RenderPluginSurfaceMaterial.h"
#include "RenderSkinFixture.h"

// 地图表面材质（WarVK\materials.ini）由 GPU 实画：平法线等于旧光照；本类型法线、细节法线向光倾斜按公式变亮，
// 环境光随法线偏离朝天方向减弱；ORM 遮蔽压暗环境光；细节颜色按 Rec.709 亮度相对均值缩放、亮侧压缩；无光照层不受影响；
// 坏尾部整份拒绝且画面不变。复用 RenderD3D11Tests 的 Require 等。
void TestSurfaceMaterials(const War3RenderPluginApiV2* api, const War3RenderCreateInfoV2& info) {
    RendererOwner renderer{api, api->createRenderer(&info)};
    Require(renderer.handle != nullptr, "surface renderer creation failed");
    War3RenderSkinSurfaceCapabilitiesV2 capability{};
    auto* status = &capability.base.base.base.base.base.base.base;
    status->structSize = sizeof(capability);
    Require(api->getSkinStatus(renderer.handle, status) == 0 && capability.surfaceFormat == WAR3_RENDER_MODEL_SURFACE_V1
        && capability.reserved == 0, "surface capability missing");
    const War3RenderCameraV2 camera{sizeof(camera), 0, 0, 0, 3500, -1.5707963f, 1.5f};
    Require(api->setCamera(renderer.handle, &camera) == 0, "surface camera rejected");
    const War3RenderMeshV2 empty{sizeof(empty), 0, 0, 920, nullptr, nullptr};
    Require(api->submitMesh(renderer.handle, &empty) == 0, "surface revision rejected");
    // 光从 +x 上方照来：平面 N·L = 0.8，颜色 = 0.3 + 0.6 × 0.8 = 0.78。
    War3RenderEnvironmentV2 environment{sizeof(environment), {0.6f, 0, 0.8f}, {0.6f, 0.6f, 0.6f}, {0.3f, 0.3f, 0.3f},
        WAR3_RENDER_FOG_NONE, 100, 200, 0.01f, {0, 0, 0}, 0, 0, 0, 0, 0, 0, nullptr};
    Require(api->setEnvironment(renderer.handle, &environment) == 0, "surface environment rejected");

    RenderSkinFixture fixture;
    const auto pose = fixture.Update();
    fixture.layer = {0, 0, 0, 1}; // 贴图 0、不透明、有光照
    const auto solid = [](std::uint8_t r, std::uint8_t g, std::uint8_t b) {
        std::array<std::uint8_t, 16> p{};
        for (std::size_t i = 0; i < 4; ++i) { p[i * 4] = r; p[i * 4 + 1] = g; p[i * 4 + 2] = b; p[i * 4 + 3] = 255; }
        return p;
    };
    // 0 白色漫反射；1 向 +u 倾斜的法线（x≈0.70）；2 平法线；3 ORM（遮蔽 0、粗糙度 1、金属 0）；
    // 4 灰色（亮度 128/255）；5 纯红（亮度 0.2126）。
    const std::array<std::array<std::uint8_t, 16>, 6> images{solid(255, 255, 255), solid(217, 128, 255),
        solid(128, 128, 255), solid(0, 255, 0), solid(128, 128, 128), solid(255, 0, 0)};
    std::array<War3RenderMipRgbaV2, 6> mips{};
    std::array<War3RenderTextureMipChainV2, 6> textures{};
    for (std::size_t i = 0; i < images.size(); ++i) {
        mips[i] = {sizeof(War3RenderMipRgbaV2), 2, 2, images[i].size(), images[i].data()};
        textures[i] = {sizeof(War3RenderTextureMipChainV2), 1, 0, 1, 1, &mips[i]};
    }
    const std::array<War3RenderMeshColorV2, 1> white{{{{1, 1, 1}}}};
    War3RenderViewInfoV2 view{};
    const auto capture = [&] {
        std::vector<std::uint8_t> pixels(static_cast<std::size_t>(info.width) * info.height * 4);
        War3RenderSkinnedCapturedFrameV2 receipt{}; receipt.structSize = sizeof(receipt);
        Require(api->captureSkinnedFrameRgba(renderer.handle, pixels.data(), pixels.size(), &receipt) == 0,
            "surface capture failed");
        view = receipt.frame.view;
        return pixels;
    };
    const auto submit = [&](const War3RenderModelSurfaceV2* surface, std::uint32_t count = 1) {
        auto scene = fixture.Scene(pose);
        scene.textureCount = static_cast<std::uint32_t>(textures.size()); scene.textures = textures.data();
        War3RenderSurfaceSkinnedSceneV2 tail{{scene, WAR3_RENDER_STATIC_MESH_RGB_V1, 1, white.data()},
            WAR3_RENDER_MODEL_SURFACE_V1, count, surface};
        tail.base.base.structSize = sizeof(tail);
        return api->submitSkinnedModelScene(renderer.handle, surface ? &tail.base.base : &scene);
    };
    int step = 0;
    const auto render = [&](const War3RenderModelSurfaceV2* surface) {
        ++step;
        if (const auto code = submit(surface); code != 0) {
            std::cerr << "surface step " << step << " rejected, code " << code << '\n';
            Require(false, "surface scene rejected");
        }
        return capture();
    };
    const auto expect = [&](const std::vector<std::uint8_t>& pixels, int level, const char* what) {
        try {
            RequireWorldColor(pixels, view, -500, 0, {level, 0, 0}, 3);
            RequireWorldColor(pixels, view, 500, 0, {0, level, 0}, 3);
        } catch (...) { std::cerr << "surface case: " << what << '\n'; throw; }
    };
    constexpr auto none = WAR3_RENDER_MODEL_NO_TEXTURE;
    const War3RenderModelSurfaceV2 neutral{none, none, none, none, none, 1, -1, 1, 1, 1, 0, 256, 1, {1, 1}, 1};

    const auto baseline = render(nullptr);
    expect(baseline, 199, "baseline vertex lighting");
    auto surface = neutral; surface.normalTexture = 2;
    expect(render(&surface), 199, "flat normal equals vertex lighting");
    // 倾斜法线 n ≈ (0.702, 0, 0.712)：N·L ≈ 0.991；环境光 × (1 + 0.5 × (0.712 − 1)) ≈ 0.856，
    // 颜色 ≈ 0.3 × 0.856 + 0.6 × 0.991 = 0.851。
    surface.normalTexture = 1;
    const auto tilted = render(&surface);
    expect(tilted, 217, "normal tilted toward the light");
    surface.normalScale = 0;
    expect(render(&surface), 199, "zero normal scale");
    // 细节法线：平面朝上时取 Z 面，同样的倾斜；强度 0 时回到平面。
    surface = neutral; surface.detailNormalTexture = 1;
    expect(render(&surface), 217, "detail normal tilted toward the light");
    surface.detailStrength = 0;
    expect(render(&surface), 199, "zero detail strength");
    // ORM 遮蔽 0：只剩直射 0.6 × 0.8 = 0.48；occlusion=0 时不压暗。
    surface = neutral; surface.ormTexture = 3;
    expect(render(&surface), 122, "occlusion darkens ambient");
    surface.occlusion = 0;
    expect(render(&surface), 199, "zero occlusion strength");
    // 细节颜色：亮度 0.502、均值 1 → 基础色 × 0.502；强度 0.5 → × 0.751；纯红亮度 0.2126 与均值相同 → 不变。
    surface = neutral; surface.detailAlbedoTexture = 4;
    expect(render(&surface), 100, "detail albedo brightness scales base colour");
    surface.detailStrength = 0.5f;
    expect(render(&surface), 149, "detail albedo strength");
    surface = neutral; surface.detailAlbedoTexture = 5; surface.detailAlbedoMean = 0.2126f;
    expect(render(&surface), 199, "detail albedo uses Rec.709 brightness");
    // 亮侧压缩：灰色是均值的 1.25 倍 → 1 + 0.25/1.25 = 1.2，颜色 0.78 × 1.2 = 0.936（不压缩会是 0.975）。
    surface = neutral; surface.detailAlbedoTexture = 4; surface.detailAlbedoMean = (128.0f / 255.0f) / 1.25f;
    expect(render(&surface), 239, "detail albedo bright side rolls off");
    // 无光照层不受材质影响。
    fixture.layer.flags = 1;
    surface = neutral; surface.normalTexture = 1;
    expect(render(&surface), 255, "unshaded layer ignores material");
    fixture.layer.flags = 0;
    const auto accepted = render(&surface);
    Require(accepted == tilted, "same material produced different pixels");
    // 数量不符、纹理越界、保留位非零整份拒绝，画面不变。
    Require(submit(&surface, 2) != 0, "surface count mismatch accepted");
    auto bad = surface; bad.ormTexture = 6;
    Require(submit(&bad) != 0, "surface texture out of range accepted");
    bad = surface; bad.detailAlbedoMean = 0;
    Require(submit(&bad) != 0, "zero detail albedo mean accepted");
    Require(capture() == accepted, "rejected surface changed pixels");
    std::cout << "surface material WARP: flat/tilted/detail normals, hemisphere ambient, ORM occlusion, detail albedo with roll-off, unshaded and rejection passed\n";
}
