#pragma once
#include "War3MapCore/RenderPluginStaticSamplers.h"

// 原创2x2四色贴图；四层共享同一texture，像素oracle由UV落点和U/V寻址直接给出。
void TestStaticLayerSamplers(const War3RenderPluginApiV2* api, const War3RenderCreateInfoV2& info) {
    RendererOwner renderer{api, api->createRenderer(&info)};
    Require(renderer.handle != nullptr && api->getStaticModelSamplerCapabilities, "static sampler capability missing");
    struct CapabilityWithTail { War3RenderStaticSamplerCapabilitiesV2 value; std::uint32_t tail; } capability{{sizeof(capability)}, 0x91bacc42};
    Require(api->getStaticModelSamplerCapabilities(renderer.handle, &capability.value) == 0
        && capability.value.structSize == sizeof(capability.value)
        && capability.value.samplerFormat == WAR3_RENDER_STATIC_LAYER_SAMPLERS_V1
        && capability.value.supportedSamplerFlags == 3 && !capability.value.reserved
        && capability.tail == 0x91bacc42, "static sampler capability prefix/tail contract");
    auto tooSmall = capability.value; tooSmall.structSize = sizeof(tooSmall)-1;
    const auto unchangedSmall = tooSmall;
    Require(api->getStaticModelSamplerCapabilities(renderer.handle, &tooSmall) != 0
        && std::memcmp(&tooSmall, &unchangedSmall, sizeof(tooSmall)) == 0, "undersized capability modified output");
    const War3RenderCameraV2 camera{sizeof(camera), 0, 0, 0, 3500, -1.5707963f, 1.5f};
    const War3RenderMeshV2 ground{sizeof(ground), 0, 0, 968, nullptr, nullptr};
    Require(api->setCamera(renderer.handle, &camera) == 0 && api->submitMesh(renderer.handle, &ground) == 0,
        "static sampler camera/ground rejected");
    const std::array<std::uint8_t, 16> pixels{255,0,0,255, 0,255,0,255, 0,0,255,255, 255,255,0,255};
    const War3RenderMipRgbaV2 mip{sizeof(mip), 2, 2, pixels.size(), pixels.data()};
    const War3RenderTextureMipChainV2 texture{sizeof(texture), 1, 0, 1, 1, &mip};
    const std::array<std::array<float, 2>, 4> centers{{{-550,-550},{550,-550},{-550,550},{550,550}}};
    std::array<War3RenderModelVertexV2, 16> vertices{};
    std::array<std::uint16_t, 24> indices{};
    std::array<War3RenderModelMeshV2, 4> meshes{};
    std::array<War3RenderModelLayerV2, 4> layers{};
    for (std::uint32_t i=0;i<4;++i) {
        for (std::uint32_t corner=0;corner<4;++corner)
            vertices[i*4+corner]={centers[i][0]+(corner&1 ? 260.0f : -260.0f),
                centers[i][1]+(corner&2 ? 260.0f : -260.0f), 20, 0,0,1, 1.25f,1.25f};
        constexpr std::array<std::uint16_t, 6> local{0,1,2,1,3,2};
        std::copy(local.begin(),local.end(),indices.begin()+i*6);
        meshes[i]={i*6,6,i*4,i,1}; layers[i]={0,0,1,1}; // Unshaded隔离光照；不改sampler过滤。
    }
    const War3RenderModelV2 model{0,4};
    const War3RenderModelInstanceV2 instance{0,{1,0,0,0, 0,1,0,0, 0,0,1,0},{1,1,1}};
    War3RenderModelSceneV2 legacy{sizeof(legacy),16,24,4,4,1,1,1,vertices.data(),indices.data(),layers.data(),meshes.data(),&model,&instance,&texture};
    std::array<std::uint32_t,4> flags{0,1,2,3};
    War3RenderStaticSamplerModelSceneV2 scene{legacy,WAR3_RENDER_STATIC_LAYER_SAMPLERS_V1,4,flags.data()};
    scene.base.structSize=sizeof(scene);
    const std::array<std::array<int,3>,4> positive{{{255,255,0},{0,0,255},{0,255,0},{255,0,0}}};
    const std::array<std::array<int,3>,4> negative{{{255,0,0},{0,255,0},{0,0,255},{255,255,0}}};
    const auto check = [&](const auto& expected, float offsetX=0) {
        const auto frame=Capture(renderer,info.width,info.height,968); const auto view=ReadView(renderer);
        for (std::size_t i=0;i<4;++i) RequireWorldColor(frame,view,centers[i][0]+offsetX,centers[i][1],expected[i],1);
        return frame;
    };
    for (const float uv : {1.25f,-.25f}) {
        for (auto& vertex:vertices) vertex.u=vertex.v=uv;
        flags={0,1,2,3};
        Require(api->submitModelScene(renderer.handle,&scene.base)==0,"explicit static sampler scene rejected");
        flags.fill(3); // provider返回后调用方覆盖来源，四层仍必须保有独立深拷贝。
        const auto accepted=uv>0 ? check(positive) : check(negative);
        const auto epoch=ReadView(renderer).frameCount;
        const auto reject = [&](const char* message) {
            Require(api->submitModelScene(renderer.handle,&scene.base)!=0,message);
            Require(Capture(renderer,info.width,info.height,968)==accepted,"invalid sampler replaced accepted pixels");
        };
        flags={0,1,2,4}; reject("unknown static sampler flag accepted"); flags={0,1,2,3};
        scene.samplerFormat=0; reject("unknown sampler format accepted"); scene.samplerFormat=1;
        scene.samplerFlagCount=3; reject("partial sampler array accepted"); scene.samplerFlagCount=4;
        scene.samplerFlags=nullptr; reject("null sampler array accepted"); scene.samplerFlags=flags.data();
        scene.base.structSize=sizeof(scene)-1; reject("truncated sampler descriptor accepted"); scene.base.structSize=sizeof(scene);
        Require(ReadView(renderer).frameCount>=epoch,"invalid sampler capture regressed frame identity");
        Require(api->submitModelScene(renderer.handle,&legacy)==0,"explicit-to-legacy reset rejected");
        std::array<std::array<int,3>,4> allLegacy{}; allLegacy.fill(uv>0 ? positive[3] : negative[3]); check(allLegacy);
    }
    // 在端点处wrap线性滤波混合两个texel；clamp保留端点颜色，容差仅容纳UNORM舍入。
    for (const float uv:{0.0f,1.0f}) {
        for (auto& vertex:vertices) vertex.u=vertex.v=uv;
        flags={0,1,2,3};
        Require(api->submitModelScene(renderer.handle,&scene.base)==0,"sampler endpoint scene rejected");
        const std::array<std::array<int,3>,4> expected=uv==0 ?
            std::array<std::array<int,3>,4>{{{255,0,0},{128,128,0},{128,0,128},{128,128,64}}} :
            std::array<std::array<int,3>,4>{{{255,255,0},{128,128,128},{128,255,0},{128,128,64}}};
        check(expected);
    }
    for (auto& vertex:vertices) vertex.u=vertex.v=1.25f;
    flags={3,0,2,1};
    const std::array<std::array<int,3>,4> shuffled{positive[3],positive[0],positive[2],positive[1]};
    Require(api->submitModelScene(renderer.handle,&scene.base)==0,"shuffled static sampler scene rejected"); check(shuffled);
    auto moved=instance; moved.transform[3]=100;
    Require(api->updateModelInstances(renderer.handle,&moved,1)==0,"static sampler instance move rejected"); check(shuffled,100);
    layers[2].textureIndex=WAR3_RENDER_MODEL_NO_TEXTURE;
    auto untextured=shuffled; untextured[2]={255,255,255};
    Require(api->submitModelScene(renderer.handle,&scene.base)==0,"untextured explicit layer rejected"); check(untextured);
    layers[2].textureIndex=0;
    War3RenderEditorPresentationTargetV2 target{sizeof(target)};
    Require(api->getEditorPresentationTarget(renderer.handle,&target)==0,"static sampler target unavailable");
    const std::uint8_t visible=1;
    const War3RenderEditorVisibilityUpdateV2 visibility{sizeof(visibility),1,0,0,target,1,&visible,nullptr,nullptr,nullptr,nullptr};
    const War3RenderEditorModelSceneV2 combined{sizeof(combined),0,&scene.base,nullptr,nullptr,&visibility};
    Require(api->submitEditorModelScene(renderer.handle,&combined)==0,"combined visibility dropped static sampler extension");
    const auto combinedPixels=check(shuffled);
    War3RenderEditorPresentationTargetV2 acceptedTarget{sizeof(acceptedTarget)};
    Require(api->getEditorPresentationTarget(renderer.handle,&acceptedTarget)==0 && acceptedTarget.modelEpoch>target.modelEpoch,
        "combined static sampler publication did not advance model epoch");
    flags[0]=4;
    Require(api->submitEditorModelScene(renderer.handle,&combined)!=0,"invalid combined sampler input accepted");
    War3RenderEditorPresentationTargetV2 rejectedTarget{sizeof(rejectedTarget)};
    Require(api->getEditorPresentationTarget(renderer.handle,&rejectedTarget)==0
        && rejectedTarget.modelEpoch==acceptedTarget.modelEpoch
        && Capture(renderer,info.width,info.height,968)==combinedPixels,
        "rejected combined sampler changed accepted epoch or pixels");
    std::cout << "static TEXS samplers: four flags, signed UV, endpoints, deep copy, legacy reset and invalid preservation\n";
}
