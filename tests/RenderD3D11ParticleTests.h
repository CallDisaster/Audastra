#pragma once
#include "War3MapCore/RenderPluginParticleMaterial.h"
// 复用既有 WARP 测试的相机、颜色断言和蒙皮夹具；只验证组合提交的新增边界。
struct ParticleGpuFixture {
    RenderSkinFixture skin;
    std::array<War3RenderMeshColorV2, 2> colors{{{{1,0,0}},{{0,1,0}}}};
    War3RenderMaterialStateSkinUpdateV2 materialState{};
    War3RenderMaterialSkinUpdateV2& material = materialState.base;
    War3RenderDynamicColoredSkinUpdateV2& pose = material.base;
    std::array<float,2> layerAlphas{1,1};
    std::array<War3RenderMaterialLayerSliceV2,2> layerSlices{{{0,1},{1,1}}};
    bool materialMode = false;
    bool materialStateMode = false;
    std::array<War3RenderMaterialStateV2,2> materialStates{{
        {WAR3_RENDER_MODEL_NO_TEXTURE,0,{1,0,0,0,1,0}}, {WAR3_RENDER_MODEL_NO_TEXTURE,0,{1,0,0,0,1,0}}}};
    std::array<War3RenderMaterialLayerSliceV2,2> stateSlices{{{0,1},{1,1}}};
    // 模型有两张驻留贴图，粒子只有一张；材质状态的索引1不能被粒子池误拒。
    std::array<std::array<std::uint8_t,4>,2> modelRgba{{{{255,255,255,255}},{{0,0,0,255}}}};
    std::array<War3RenderMipRgbaV2,2> modelMips{{
        {sizeof(War3RenderMipRgbaV2),1,1,4,modelRgba[0].data()}, {sizeof(War3RenderMipRgbaV2),1,1,4,modelRgba[1].data()}}};
    std::array<War3RenderTextureMipChainV2,2> modelTextures{{
        {sizeof(War3RenderTextureMipChainV2),1,0,1,1,&modelMips[0]},
        {sizeof(War3RenderTextureMipChainV2),1,0,1,1,&modelMips[1]}}};
    War3RenderSkinnedModelSceneV2 models{};
    std::array<std::uint8_t, 4> white{255,255,255,255};
    War3RenderMipRgbaV2 mip{sizeof(mip),1,1,white.size(),white.data()};
    War3RenderTextureMipChainV2 texture{sizeof(texture),1,0,1,1,&mip};
    std::array<War3RenderParticleVertexV2, 6> vertices{{
        {{-650,-150,80},{0,1},{.25f,.5f,.75f,.5f}}, {{-350,-150,80},{1,1},{.25f,.5f,.75f,.5f}},
        {{-650,150,80},{0,0},{.25f,.5f,.75f,.5f}}, {{-350,-150,80},{1,1},{.25f,.5f,.75f,.5f}},
        {{-350,150,80},{1,0},{.25f,.5f,.75f,.5f}}, {{-650,150,80},{0,0},{.25f,.5f,.75f,.5f}}}};
    War3RenderParticleDrawV2 draw{0,6,0,WAR3_PARTICLE_OPAQUE};
    War3RenderParticleUpdateV2 frame{};
    War3RenderParticleSceneV2 scene{};
    void Bind(std::uint64_t sceneVersion, std::uint64_t content, bool empty = false, bool pure = false) {
        for (auto& instance : skin.instances) std::fill_n(instance.instance.color, 3, 1.0f);
        pose = {skin.Update(content),WAR3_RENDER_DYNAMIC_MESH_RGB_V1,2,colors.data()};
        pose.base.structSize = sizeof(pose); pose.base.sceneVersion = sceneVersion;
        models = skin.Scene(pose.base);
        if (pure) {
            pose.base.instanceCount = pose.base.groupCount = pose.base.alphaCount = pose.colorCount = 0;
            pose.base.instances = nullptr; pose.base.groups = nullptr; pose.base.alphas = nullptr; pose.colors = nullptr;
            models = {}; models.structSize = sizeof(models); models.initialPose = &pose.base;
        }
        if (materialMode || materialStateMode) {
            pose.base.structSize = sizeof(material); material.layerAlphaFormat = WAR3_RENDER_DYNAMIC_LAYER_ALPHA_V1;
            material.layerAlphaCount = material.instanceSliceCount = pure ? 0u : 2u;
            material.layerAlphas = pure ? nullptr : layerAlphas.data(); material.instanceSlices = pure ? nullptr : layerSlices.data();
        }
        if (materialStateMode) {
            pose.base.structSize = sizeof(materialState); materialState.materialStateFormat = WAR3_RENDER_DYNAMIC_MATERIAL_STATE_V1;
            materialState.materialStateCount = materialState.materialInstanceSliceCount = pure ? 0u : 2u;
            materialState.materialStates = pure ? nullptr : materialStates.data();
            materialState.materialInstanceSlices = pure ? nullptr : stateSlices.data();
            if (!pure) { models.textureCount = 2; models.textures = modelTextures.data(); }
        }
        frame = {sizeof(frame),empty ? 0u : 6u,empty ? 0u : 1u,0,sceneVersion,content,
            vertices.data(),&draw,&pose.base};
        scene = {sizeof(scene),1,&texture,&models,&frame};
    }
};
struct ParticleGpuCapture {
    std::vector<std::uint8_t> pixels;
    War3RenderParticleCapturedFrameV2 receipt{sizeof(War3RenderParticleCapturedFrameV2)};
};
ParticleGpuCapture CaptureParticles(const RendererOwner& renderer, std::uint64_t scene, std::uint64_t content) {
    ParticleGpuCapture captured; captured.pixels.resize(320 * 240 * 4);
    Require(renderer.api->captureParticleFrameRgba(renderer.handle,captured.pixels.data(),captured.pixels.size(),&captured.receipt) == 0,
        "particle combined capture failed");
    const auto& receipt = captured.receipt;
    Require(receipt.models.skin.active && receipt.models.skin.result == 0 && receipt.models.skin.sceneVersion == scene
        && receipt.models.skin.contentVersion == content && receipt.particles.sceneVersion == scene && receipt.particles.contentVersion == content
        && receipt.models.skin.frameCount == receipt.models.frame.capture.frameCount
        && receipt.models.frame.view.frameCount == receipt.models.frame.capture.frameCount, "particle capture mixed versions/frames");
    return captured;
}
void RejectedParticleCapture(const RendererOwner& renderer) {
    std::vector<std::uint8_t> pixels(320 * 240 * 4,0x71);
    War3RenderParticleCapturedFrameV2 receipt; std::memset(&receipt,0x53,sizeof(receipt)); receipt.structSize = sizeof(receipt);
    const auto before = receipt;
    Require(renderer.api->captureParticleFrameRgba(renderer.handle,pixels.data(),pixels.size(),&receipt) != 0
        && std::memcmp(&receipt,&before,sizeof(receipt)) == 0
        && std::all_of(pixels.begin(),pixels.end(),[](auto value) { return value == 0x71; }), "failed particle capture modified output");
}
void TestParticleModelsMode(const War3RenderPluginApiV2* api, const War3RenderCreateInfoV2& info,
    bool materialMode, bool materialStateMode = false) {
    Require(api->submitParticleModelScene && api->updateParticleModelFrame && api->captureParticleFrameRgba, "particle tail missing");
    RendererOwner renderer{api,api->createRenderer(&info)};
    Require(renderer.handle != nullptr,"particle renderer unavailable");
    if (materialStateMode) {
        War3RenderSkinMaterialStateCapabilitiesV2 capabilities{};
        capabilities.base.base.base.base.base.structSize = sizeof(capabilities);
        Require(api->getSkinStatus(renderer.handle,&capabilities.base.base.base.base.base) == 0
            && capabilities.materialStateFormat == WAR3_RENDER_DYNAMIC_MATERIAL_STATE_V1,
            "particle combined material-state capability absent");
    }
    const War3RenderCameraV2 camera{sizeof(camera),0,0,0,3500,-1.5707963f,1.5f};
    Require(api->setCamera(renderer.handle,&camera) == 0,"particle camera rejected");
    ParticleGpuFixture fixture; fixture.materialMode = materialMode; fixture.materialStateMode = materialStateMode; fixture.Bind(1,1,true);
    Require(api->submitParticleModelScene(renderer.handle,&fixture.scene) == 0,"empty initial particle frame rejected");
    fixture.white.fill(0); const auto initial = CaptureParticles(renderer,1,1); fixture.white.fill(255);
    Require(initial.receipt.particles.textureUploadCount == 1 && initial.receipt.particles.dynamicUploadCount == 1
        && initial.receipt.particles.vertexCount == 0,"initial empty particle counters wrong");
    fixture.colors[1] = {{0,0,1}};
    // 粒子枚举七种过滤，落在红色模型上；不把粒子枚举直接当 MDX 过滤编号。
    const std::array<std::array<int,3>,9> expected{{{159,64,96},{255,64,96},{64,0,0},{128,0,0},{255,0,0},{255,64,96},{64,128,191},{255,0,0},{111,96,144}}};
    for (std::uint32_t filter = 0; filter < expected.size(); ++filter) {
        fixture.Bind(1,filter + 2); fixture.draw.filter = filter < 7 ? filter : WAR3_PARTICLE_ALPHAKEY;
        for (auto& vertex : fixture.vertices) vertex.color[3] = filter < 7 ? .5f : filter == 7 ? .75f : 192.f/255.f;
        Require(api->updateParticleModelFrame(renderer.handle,&fixture.frame) == 0,"particle filter update rejected");
        const auto vertices = fixture.vertices; const auto colors = fixture.colors;
        fixture.vertices.fill({}); fixture.colors.fill({{1,0,1}}); // 返回后不能继续借用宿主动态数组。
        const auto colored = CaptureParticles(renderer,1,filter + 2);
        fixture.vertices = vertices; fixture.colors = colors;
        RequireWorldColor(colored.pixels,colored.receipt.models.frame.view,-500,0,expected[filter],2);
        RequireWorldColor(colored.pixels,colored.receipt.models.frame.view,500,0,{0,0,255},1);
        Require(colored.receipt.particles.textureUploadCount == 1 && colored.receipt.particles.dynamicUploadCount == filter + 2
            && colored.receipt.models.skin.staticUploadCount == 1,"dynamic particle frame reuploaded static resources");
    }
    fixture.Bind(1,11,true);
    Require(api->updateParticleModelFrame(renderer.handle,&fixture.frame) == 0,"particle empty update rejected");
    const auto cleared = CaptureParticles(renderer,1,11);
    RequireWorldColor(cleared.pixels,cleared.receipt.models.frame.view,-500,0,{255,0,0},1);
    Require(cleared.receipt.particles.vertexCount == 0 && cleared.receipt.particles.drawCount == 0
        && cleared.receipt.particles.dynamicUploadCount == 11,"empty update kept old particles");
    if (materialMode) {
        fixture.Bind(1,12,true); fixture.layerAlphas[1] = 0;
        Require(api->updateParticleModelFrame(renderer.handle,&fixture.frame) == 0,"combined KMTA update rejected");
        const auto hidden = CaptureParticles(renderer,1,12);
        Require(hidden.pixels != cleared.pixels && hidden.receipt.models.skin.staticUploadCount == 1,
            "combined KMTA did not affect pixels or reuploaded geometry");
        fixture.layerAlphas[1] = 1; fixture.Bind(1,13,true);
        Require(api->updateParticleModelFrame(renderer.handle,&fixture.frame) == 0 && CaptureParticles(renderer,1,13).pixels == cleared.pixels,
            "combined KMTA restore changed accepted pixels");
        fixture.Bind(1,14); fixture.material.reserved = 1;
        Require(api->updateParticleModelFrame(renderer.handle,&fixture.frame) != 0 && CaptureParticles(renderer,1,13).pixels == cleared.pixels,
            "bad material tail changed accepted particle/model pair"); fixture.material.reserved = 0;
    }
    if (materialStateMode) {
        fixture.Bind(1,14); fixture.draw.filter = WAR3_PARTICLE_OPAQUE;
        fixture.materialStates[1].textureIndex = 1;
        Require(api->updateParticleModelFrame(renderer.handle,&fixture.frame) == 0,
            "combined state used particle texture count or rejected full tail");
        fixture.materialStates[1].textureIndex = WAR3_RENDER_MODEL_NO_TEXTURE;
        const auto textured = CaptureParticles(renderer,1,14);
        RequireWorldColor(textured.pixels,textured.receipt.models.frame.view,500,0,{0,0,0},1);
        RequireWorldColor(textured.pixels,textured.receipt.models.frame.view,-500,0,{64,128,191},2);
        Require(textured.receipt.models.skin.staticUploadCount == 1 && textured.receipt.particles.textureUploadCount == 1
            && textured.receipt.particles.dynamicUploadCount == 14,
            "combined state update reuploaded assets or borrowed the changed host tail");
        fixture.Bind(1,15,true);
        Require(api->updateParticleModelFrame(renderer.handle,&fixture.frame) == 0,"combined state restore rejected");
        const auto restored = CaptureParticles(renderer,1,15);
        Require(restored.pixels == cleared.pixels,"combined state restore lost model RGB/KMTA or particle clear");
        fixture.Bind(1,16);
        const auto rejectState = [&](const char* message) {
            Require(api->updateParticleModelFrame(renderer.handle,&fixture.frame) != 0,message);
            const auto unchanged = CaptureParticles(renderer,1,15);
            Require(unchanged.pixels == restored.pixels
                && unchanged.receipt.models.skin.staticUploadCount == restored.receipt.models.skin.staticUploadCount
                && unchanged.receipt.models.skin.matrixUploadCount == restored.receipt.models.skin.matrixUploadCount
                && unchanged.receipt.particles.textureUploadCount == restored.receipt.particles.textureUploadCount
                && unchanged.receipt.particles.dynamicUploadCount == restored.receipt.particles.dynamicUploadCount,
                "rejected state tail changed one half of the completed model/particle pair");
        };
        fixture.materialState.reserved = 1; rejectState("combined state reserved bits accepted"); fixture.materialState.reserved = 0;
        fixture.materialState.materialStateFormat = 2; rejectState("combined unknown state format accepted");
        fixture.materialState.materialStateFormat = WAR3_RENDER_DYNAMIC_MATERIAL_STATE_V1;
        fixture.materialState.materialStates = nullptr; rejectState("combined missing state pool accepted");
        fixture.materialState.materialStates = fixture.materialStates.data();
        fixture.stateSlices[1].first = UINT32_MAX; rejectState("combined overflowing state slice accepted"); fixture.stateSlices[1].first = 1;
        fixture.materialStates[1].samplerFlags = 4; rejectState("combined unknown sampler flags accepted"); fixture.materialStates[1].samplerFlags = 0;
        fixture.materialStates[1].uvTransform[4] = std::numeric_limits<float>::quiet_NaN();
        rejectState("combined state nonfinite UV accepted"); fixture.materialStates[1].uvTransform[4] = 1;
        fixture.materialStates[1].textureIndex = 2;
        rejectState("combined state nonresident model texture escaped CopySkinUpdate");
        fixture.materialStates[1].textureIndex = WAR3_RENDER_MODEL_NO_TEXTURE;
        fixture.pose.base.structSize = sizeof(fixture.material);
        rejectState("combined state silently truncated to old alpha mode");
        fixture.pose.base.structSize = sizeof(fixture.materialState) - 1;
        rejectState("combined incomplete state tail accepted"); fixture.pose.base.structSize = sizeof(fixture.materialState);
    }
    const auto next = materialStateMode ? 16u : materialMode ? 14u : 12u;
    const auto accepted = materialStateMode ? 15u : materialMode ? 13u : 11u;
    fixture.Bind(1,next); fixture.pose.colorCount = 1;
    Require(api->updateParticleModelFrame(renderer.handle,&fixture.frame) != 0,"particle path truncated invalid dynamic GEOA");
    Require(CaptureParticles(renderer,1,accepted).pixels == cleared.pixels,"invalid combo changed accepted frame");
    fixture.Bind(1,next); fixture.frame.contentVersion = next + 1;
    Require(api->updateParticleModelFrame(renderer.handle,&fixture.frame) != 0,"particle/model version mismatch accepted");
    fixture.Bind(1,next);
    Require(api->updateSkinMatrices(renderer.handle,&fixture.pose.base) == 0,"old pose path rejected");
    RejectedParticleCapture(renderer); // 旧模型写入口退出粒子，查询/相机不会退出。
    fixture.draw.filter = WAR3_PARTICLE_OPAQUE;
    for (bool noTextures : {false,true}) {
        RendererOwner pure{api,api->createRenderer(&info)}; Require(pure.handle != nullptr,"pure particle renderer missing");
        Require(api->setCamera(pure.handle,&camera) == 0,"pure particle camera rejected");
        fixture.Bind(1,1,true,true); if (noTextures) fixture.scene.textureCount = 0;
        Require(api->submitParticleModelScene(pure.handle,&fixture.scene) == 0,"pure particle empty model prefix rejected");
        const auto empty = CaptureParticles(pure,1,1);
        Require(empty.receipt.models.skin.staticUploadCount == 0 && empty.receipt.models.skin.matrixUploadCount == 0
            && empty.receipt.particles.textureUploadCount == (noTextures ? 0u : 1u),"pure particle counters fake model upload");
        if (noTextures) continue;
        fixture.Bind(1,2,false,true);
        Require(api->updateParticleModelFrame(pure.handle,&fixture.frame) == 0,"pure particle nonempty update rejected");
        const auto filled = CaptureParticles(pure,1,2);
        RequireWorldColor(filled.pixels,filled.receipt.models.frame.view,-500,0,{64,128,191},2);
        Require(api->setCamera(pure.handle,&camera) == 0,"particle camera refresh rejected");
        Require(CaptureParticles(pure,1,2).pixels == filled.pixels,"view query erased pure particles");
        fixture.Bind(1,3,true,true); Require(api->updateParticleModelFrame(pure.handle,&fixture.frame) == 0,"pure particle clear failed");
        Require(CaptureParticles(pure,1,3).pixels == empty.pixels,"pure particle zero/nonzero/zero did not clear");
        const War3RenderModelSceneV2 legacy{sizeof(legacy)};
        Require(api->submitModelScene(pure.handle,&legacy) == 0,"legacy model clear rejected"); RejectedParticleCapture(pure);
    }
    std::cout << "particles WARP: combined GEOA/filter/empty/pure frames, AlphaKey GE192 blend, immutable uploads and legacy exits passed"
        << " material_mode=" << materialMode << " material_state_mode=" << materialStateMode << '\n';
}
void TestParticleModels(const War3RenderPluginApiV2* api, const War3RenderCreateInfoV2& info) {
    for (const bool material : {false,true}) TestParticleModelsMode(api,info,material);
    TestParticleModelsMode(api,info,true,true);
}
void TestParticleFaultsMode(const std::filesystem::path& directory, HMODULE production,
    const War3RenderCreateInfoV2& info, bool materialMode) {
    Require(GetProcAddress(production,"War3RenderTest_SetFault") == nullptr,"test hook leaked into production DLL");
    const auto module = LoadLibraryExW((directory / L"war3-render-d3d11-test.dll").c_str(),nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    Require(module != nullptr,"particle fault test DLL missing");
    struct ModuleGuard { HMODULE value; ~ModuleGuard() { FreeLibrary(value); } } moduleGuard{module};
    const auto getApi = reinterpret_cast<War3GetRenderPluginApiV2>(GetProcAddress(module,"War3MapEditor_GetRenderPluginApiV2"));
    using SetFault = std::int32_t (WAR3_RENDER_CALL*)(War3RendererHandle,std::uint32_t);
    const auto fault = reinterpret_cast<SetFault>(GetProcAddress(module,"War3RenderTest_SetFault"));
    Require(getApi && fault,"private fault controls unavailable");
    const auto* api = getApi(WAR3_RENDER_PLUGIN_API_V2);
    RendererOwner renderer{api,api->createRenderer(&info)}; Require(renderer.handle != nullptr,"fault renderer missing");
    ParticleGpuFixture fixture;
    std::uint32_t materialFlag = WAR3_PARTICLE_UNFOGGED;
    War3RenderParticleMaterialUpdateV2 materialFrame{};
    const auto bind = [&](std::uint64_t scene, std::uint64_t content) {
        fixture.Bind(scene,content);
        materialFrame = {fixture.frame,WAR3_RENDER_PARTICLE_MATERIAL_FLAGS_V1,1,&materialFlag};
        materialFrame.base.structSize = sizeof(materialFrame);
        if (materialMode) fixture.scene.initialFrame = &materialFrame.base;
    };
    bind(1,1);
    const War3RenderEnvironmentV2 environment{sizeof(environment),{0,0,1},{1,1,1},{1,1,1},
        WAR3_RENDER_FOG_LINEAR,100,200,.01f,{.1f,.2f,.4f},0,0,0,0,0,0,nullptr};
    if (materialMode) Require(api->setEnvironment(renderer.handle,&environment) == 0,"fault fog fixture rejected");
    Require(api->submitParticleModelScene(renderer.handle,&fixture.scene) == 0,"fault fixture rejected");
    auto accepted = CaptureParticles(renderer,1,1);
    std::uint64_t scene = 1, content = 1;
    for (const auto stage : {1u,2u}) for (const bool newScene : {false,true}) {
        if (newScene) { ++scene; content = 0; }
        materialFlag = materialFlag ? 0u : static_cast<std::uint32_t>(WAR3_PARTICLE_UNFOGGED);
        bind(scene,++content);
        fixture.colors[0] = {{0,1,0}}; fixture.vertices[0].position[0] -= 50;
        Require(fault(renderer.handle,stage) == 0,"fault arm failed");
        Require((newScene ? api->submitParticleModelScene(renderer.handle,&fixture.scene)
            : api->updateParticleModelFrame(renderer.handle,fixture.scene.initialFrame)) == 0,"fault candidate not queued");
        RejectedParticleCapture(renderer);
        War3RenderSkinStatusV2 actual{sizeof(actual)};
        Require(api->getSkinStatus(renderer.handle,&actual) == 0 && actual.result < 0
            && actual.sceneVersion == accepted.receipt.models.skin.sceneVersion
            && actual.contentVersion == accepted.receipt.models.skin.contentVersion
            && actual.staticUploadCount == accepted.receipt.models.skin.staticUploadCount
            && actual.matrixUploadCount == accepted.receipt.models.skin.matrixUploadCount,"failed combo published model half");
        Require(Capture(renderer,320,240,0) == accepted.pixels,"failed combo replaced old GPU pair");
        bind(scene,++content);
        Require(api->updateParticleModelFrame(renderer.handle,fixture.scene.initialFrame) == 0,"higher recovery version rejected");
        const auto recovered = CaptureParticles(renderer,scene,content);
        Require(recovered.receipt.particles.dynamicUploadCount == accepted.receipt.particles.dynamicUploadCount + 1
            && recovered.receipt.particles.textureUploadCount == accepted.receipt.particles.textureUploadCount + (newScene ? 1 : 0),
            "failed attempt advanced particle counters or recovery lost textures");
        accepted = recovered;
    }
    Require(fault(renderer.handle,3) == 0,"readback fault arm failed");
    RejectedParticleCapture(renderer); // Draw/readback 故障沿既有设备失效合同锁存；不伪报同设备恢复。
    std::cout << "particles fault WARP: old pair retained, no partial output, higher-version recovery, readback failure passed\n";
}
void TestParticleFaults(const std::filesystem::path& directory, HMODULE production, const War3RenderCreateInfoV2& info) {
    TestParticleFaultsMode(directory,production,info,false);
    TestParticleFaultsMode(directory,production,info,true);
}
