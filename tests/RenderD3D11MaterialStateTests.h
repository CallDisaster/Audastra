#pragma once
#include "War3MapCore/RenderPluginMaterialState.h"
// 原创双实例/双层平面；沿现有headless主入口和完成帧，不增加工具或CPU烘焙路径。
void TestMaterialStates(const War3RenderPluginApiV2* api, const War3RenderCreateInfoV2& info,
    InstanceRosterSetFault fault = nullptr, InstanceRosterGateRead gate = nullptr, InstanceRosterAssetRead assets = nullptr) {
    RendererOwner renderer{api, api->createRenderer(&info)}; Require(renderer.handle != nullptr, "material state renderer unavailable");
    War3RenderSkinMaterialStateCapabilitiesV2 caps{}; caps.base.base.base.base.base.structSize = sizeof(caps);
    Require(api->getSkinStatus(renderer.handle, &caps.base.base.base.base.base) == 0
        && caps.materialStateFormat == WAR3_RENDER_DYNAMIC_MATERIAL_STATE_V1, "material state capability absent");
    caps.materialStateFormat = 0x5a5a5a5a; caps.base.base.base.base.base.structSize = sizeof(caps.base);
    Require(api->getSkinStatus(renderer.handle, &caps.base.base.base.base.base) == 0 && caps.materialStateFormat == 0x5a5a5a5a, "old alpha prefix overwrote state tail");
    const War3RenderCameraV2 camera{sizeof(camera), 0,0,0,3500,-1.5707963f,1.5f};
    const War3RenderMeshV2 empty{sizeof(empty),0,0,929,nullptr,nullptr};
    Require(api->setCamera(renderer.handle, &camera) == 0 && api->submitMesh(renderer.handle, &empty) == 0, "material state camera failed");
    const auto background = Capture(renderer, info.width, info.height, 929);
    RenderSkinFixture fixture; fixture.mesh.mesh.layerCount = 2;
    for (auto& instance : fixture.instances) for (auto& color : instance.instance.color) color = 1;
    std::array<std::array<std::uint8_t,16>,2> rgba{{{{255,0,0,255,255,0,0,255,255,0,0,255,255,0,0,255}},
        {{255,0,0,255,0,255,0,255,0,0,255,255,255,255,255,255}}}};
    const std::array<War3RenderMipRgbaV2,2> mips{{{sizeof(War3RenderMipRgbaV2),2,2,16,rgba[0].data()}, {sizeof(War3RenderMipRgbaV2),2,2,16,rgba[1].data()}}};
    const std::array<War3RenderTextureMipChainV2,2> textures{{{sizeof(War3RenderTextureMipChainV2),1,0,1,1,&mips[0]}, {sizeof(War3RenderTextureMipChainV2),1,0,1,1,&mips[1]}}};
    const std::array<War3RenderModelLayerV2,2> layers{{{0,0,1,.7f},{1,2,1,.2f}}};
    std::array<float,4> layerAlphas{.7f,.5f,.7f,0}; const std::array<War3RenderMaterialLayerSliceV2,2> slices{{{0,2},{2,2}}};
    std::array<War3RenderMaterialStateV2,4> states{{{0,0,{1,0,0,0,1,0}}, {1,0,{0,.25f,.125f,.25f,0,.625f}},
        {1,3,{0,0,1.25f,0,0,1.25f}}, {0,0,{1,0,0,0,1,0}}}};
    War3RenderMaterialStateSkinUpdateV2 pose{}; pose.base.base.base = fixture.Update(); pose.base.base.base.structSize = sizeof(pose);
    pose.base.layerAlphaFormat = WAR3_RENDER_DYNAMIC_LAYER_ALPHA_V1; pose.base.layerAlphaCount = 4; pose.base.instanceSliceCount = 2;
    pose.base.layerAlphas = layerAlphas.data(); pose.base.instanceSlices = slices.data();
    pose.materialStateFormat = WAR3_RENDER_DYNAMIC_MATERIAL_STATE_V1; pose.materialStateCount = 4; pose.materialInstanceSliceCount = 2;
    pose.materialStates = states.data(); pose.materialInstanceSlices = slices.data();
    auto scene = fixture.Scene(pose.base.base.base); scene.layerCount = 2; scene.layers = layers.data(); scene.textureCount = 2; scene.textures = textures.data();
    {
        RendererOwner first{api,api->createRenderer(&info)}; Require(first.handle != nullptr,"first state renderer absent");
        Require(api->setCamera(first.handle,&camera) == 0 && api->submitMesh(first.handle,&empty) == 0,"first state view rejected");
        auto emptyPose = pose; auto& plain = emptyPose.base.base.base;
        plain.instanceCount = plain.groupCount = plain.alphaCount = 0; plain.instances = nullptr; plain.groups = nullptr; plain.alphas = nullptr;
        emptyPose.base.layerAlphaCount = emptyPose.base.instanceSliceCount = 0; emptyPose.base.layerAlphas = nullptr; emptyPose.base.instanceSlices = nullptr;
        emptyPose.materialStateCount = emptyPose.materialInstanceSliceCount = 0; emptyPose.materialStates = nullptr; emptyPose.materialInstanceSlices = nullptr;
        War3RenderSkinnedModelSceneV2 emptyModels{}; emptyModels.structSize = sizeof(emptyModels); emptyModels.initialPose = &plain;
        Require(api->submitSkinnedModelScene(first.handle,&emptyModels) == 0,"empty skinned state rejected");
        War3RenderSkinnedCapturedFrameV2 emptyCompleted{sizeof(emptyCompleted)};
        std::vector<std::uint8_t> blank(static_cast<std::size_t>(info.width)*info.height*4);
        Require(api->captureSkinnedFrameRgba(first.handle,blank.data(),blank.size(),&emptyCompleted) == 0
            && emptyCompleted.skin.sceneVersion == 1 && emptyCompleted.skin.contentVersion == 1
            && emptyCompleted.skin.staticUploadCount == 0 && emptyCompleted.skin.matrixUploadCount == 0
            && emptyCompleted.frame.capture.documentRevision == 929,"empty state uploaded static assets or completed wrong frame");
        War3RenderInstanceRosterResultV2 base{sizeof(base)}; Require(api->getEditorInstanceRosterResult(first.handle,0,&base) == 0,"first state target missing");
        auto initialPose = pose; initialPose.base.base.base.sceneVersion = 2;
        War3RenderAssetAppendV2 suffix{}; suffix.structSize = sizeof(suffix);
        suffix.vertexCount = 4; suffix.indexCount = 6; suffix.layerCount = 2; suffix.meshCount = suffix.modelCount = 1; suffix.modelTextureCount = 2;
        suffix.vertices = fixture.vertices.data(); suffix.indices = fixture.indices.data(); suffix.layers = layers.data();
        suffix.meshes = &fixture.mesh; suffix.models = &fixture.model; suffix.modelTextures = textures.data();
        const std::array<std::uint8_t,2> visible{1,1};
        const War3RenderEditorVisibilityUpdateV2 mask{sizeof(mask),2,0,0,base.target,1,visible.data(),nullptr,nullptr,nullptr,nullptr};
        War3RenderEditorAssetRosterV2 append{}; auto& request = append.base;
        request.structSize = sizeof(append); request.flags = WAR3_RENDER_ROSTER_APPEND_ASSETS; request.expected = base.target;
        request.expectedAssetIdentity = base.assetIdentity; request.expectedLayersRevision = base.layersRevision;
        request.expectedPresentationRevision = base.presentationRevision; request.candidatePose = &initialPose.base.base.base; request.visibility = &mask; append.assets = &suffix;
        std::uint64_t ticket = 0;
        if (fault) Require(fault(first.handle,8) == 0 && api->submitEditorInstanceRoster(first.handle,&request,&ticket) == 0
            && WaitInstanceRoster(first,ticket).state == WAR3_RENDER_ROSTER_REJECTED
            && Capture(first,info.width,info.height,929) == blank,"failed first state append changed empty completed frame");
        Require(api->submitEditorInstanceRoster(first.handle,&request,&ticket) == 0,"first state append rejected");
        const auto completed = WaitInstanceRoster(first,ticket);
        Require(completed.state == WAR3_RENDER_ROSTER_COMPLETED && completed.completed.skin.staticUploadCount == 1
            && completed.completed.skin.matrixUploadCount == 1,"first state append uploaded old or missed new assets");
        War3RenderSkinnedCapturedFrameV2 captured{sizeof(captured)}; std::vector<std::uint8_t> pixels(blank.size());
        Require(api->captureSkinnedFrameRgba(first.handle,pixels.data(),pixels.size(),&captured) == 0
            && captured.skin.sceneVersion == 2 && captured.skin.contentVersion == 1,"first state completed identity wrong");
        RequireWorldColor(pixels,captured.frame.view,-500,0,{128,0,128},2);
        RequireWorldColor(pixels,captured.frame.view,500,0,{255,0,0},2);
    }
    Require(api->submitSkinnedModelScene(renderer.handle, &scene) == 0, "material state initial scene rejected");
    War3RenderSkinnedCapturedFrameV2 receipt{sizeof(receipt)};
    const auto capture = [&](std::uint64_t sceneVersion, std::uint64_t contentVersion) {
        std::vector<std::uint8_t> pixels(static_cast<std::size_t>(info.width)*info.height*4);
        Require(api->captureSkinnedFrameRgba(renderer.handle,pixels.data(),pixels.size(),&receipt) == 0
            && receipt.skin.sceneVersion == sceneVersion && receipt.skin.contentVersion == contentVersion
            && receipt.skin.frameCount == receipt.frame.view.frameCount && receipt.skin.staticUploadCount == 1
            && receipt.frame.capture.documentRevision == 929, "material state completion identity or static reupload wrong"); return pixels;
    };
    const auto initial = capture(1,1); RequireWorldColor(initial,receipt.frame.view,-500,0,{128,0,128},2);
    RequireWorldColor(initial,receipt.frame.view,500,0,{255,0,0},2); // 第二实例wrap两个轴；不与第一实例串状态。
    const std::array<std::array<int,3>,3> colors{{{{255,255,255}},{{0,0,255}},{{0,255,0}}}};
    std::uint64_t content = 1;
    for (std::uint32_t flags = 0; flags != 3; ++flags) {
        states[2].samplerFlags = flags; pose.base.base.base.contentVersion = ++content;
        Require(api->updateSkinMatrices(renderer.handle,&pose.base.base.base) == 0, "sampler state update failed");
        const auto pixels = capture(1,content); RequireWorldColor(pixels,receipt.frame.view,500,0,colors[flags],2);
        RequireWorldColor(pixels,receipt.frame.view,-500,0,{128,0,128},2);
    }
    fixture.alphas[0] = .5f; pose.base.base.base.contentVersion = ++content;
    Require(api->updateSkinMatrices(renderer.handle,&pose.base.base.base) == 0, "material state GEOA update failed");
    RequireWorldColor(capture(1,content),receipt.frame.view,-500,0,{191,0,64},2); // KMTA/GEOA各一次。
    fixture.groups[0].values[10] = 400; pose.base.base.base.contentVersion = ++content;
    Require(api->updateSkinMatrices(renderer.handle,&pose.base.base.base) == 0, "material state move failed");
    const auto moved = capture(1,content); RequireWorldColor(moved,receipt.frame.view,-500,400,{191,0,64},2);
    const auto uploads = receipt.skin.matrixUploadCount;
    pose.base.base.base.contentVersion = content+1; states[0].textureIndex = 2;
    Require(api->updateSkinMatrices(renderer.handle,&pose.base.base.base) != 0 && capture(1,content) == moved, "nonresident texture changed pixels"); states[0].textureIndex = 0;
    states[1].uvTransform[0] = std::numeric_limits<float>::quiet_NaN();
    Require(api->updateSkinMatrices(renderer.handle,&pose.base.base.base) != 0 && capture(1,content) == moved, "nonfinite UV published"); states[1].uvTransform[0] = 0;
    auto shortPose = pose.base; shortPose.base.base.structSize = sizeof(shortPose);
    Require(api->updateSkinMatrices(renderer.handle,&shortPose.base.base) != 0 && capture(1,content) == moved, "missing state tail silently fell back");
    War3RenderInstanceRosterResultV2 base{sizeof(base)}; Require(api->getEditorInstanceRosterResult(renderer.handle,0,&base) == 0, "material state roster target missing");
    pose.base.base.base.sceneVersion = 2; pose.base.base.base.contentVersion = 1;
    const std::array<std::uint8_t,2> visible{1,0};
    War3RenderEditorVisibilityUpdateV2 mask{sizeof(mask),2,0,0,base.target,1,visible.data(),nullptr,nullptr,nullptr,nullptr};
    War3RenderEditorInstanceRosterV2 request{}; request.structSize = sizeof(request); request.expected = base.target;
    request.expectedAssetIdentity = base.assetIdentity; request.expectedLayersRevision = base.layersRevision;
    request.expectedPresentationRevision = base.presentationRevision; request.candidatePose = &pose.base.base.base; request.visibility = &mask;
    std::uint64_t ticket = 0;
    if (fault) {
        Require(gate && assets, "material state fault controls absent"); std::array<std::uint64_t,7> before{}, after{};
        Require(assets(renderer.handle,before.data(),7) == 0 && fault(renderer.handle,8) == 0, "state allocation fault arm failed");
        Require(api->submitEditorInstanceRoster(renderer.handle,&request,&ticket) == 0 && WaitInstanceRoster(renderer,ticket).state == WAR3_RENDER_ROSTER_REJECTED
            && capture(1,content) == moved && receipt.skin.matrixUploadCount == uploads, "state allocation failure published");
        struct ReleaseGate { InstanceRosterSetFault fault; War3RendererHandle handle; ~ReleaseGate() { (void)fault(handle,0); } } release{fault,renderer.handle};
        Require(fault(renderer.handle,6) == 0 && api->submitEditorInstanceRoster(renderer.handle,&request,&ticket) == 0, "state cancel queue failed");
        const auto deadline = GetTickCount64()+10000; while (gate(renderer.handle)!=1 && GetTickCount64()<deadline) Sleep(1);
        Require(gate(renderer.handle)==1, "state prePublish gate absent"); War3RenderInstanceRosterResultV2 cancelled{sizeof(cancelled)};
        Require(api->cancelEditorInstanceRoster(renderer.handle,ticket,&cancelled) == 0 && cancelled.state == WAR3_RENDER_ROSTER_SUPERSEDED
            && !cancelled.completed.view.frameCount && fault(renderer.handle,0) == 0, "state cancel failed");
        Require(WaitInstanceRoster(renderer,ticket).state == WAR3_RENDER_ROSTER_SUPERSEDED && capture(1,content) == moved
            && receipt.skin.matrixUploadCount == uploads && assets(renderer.handle,after.data(),7) == 0 && before == after, "state cancel changed old assets/frame");
    }
    Require(api->submitEditorInstanceRoster(renderer.handle,&request,&ticket) == 0, "state hide roster rejected");
    states[1].textureIndex = 0; const auto accepted = WaitInstanceRoster(renderer,ticket);
    Require(accepted.state == WAR3_RENDER_ROSTER_COMPLETED && accepted.completed.presentation.visibleInstanceCount == 1
        && accepted.completed.presentation.drawCallCount == 2 && accepted.assetIdentity == base.assetIdentity, "state hide completion/runs wrong");
    const auto hidden = capture(2,1);
    RequireWorldColor(hidden,receipt.frame.view,-500,400,{191,0,64},2); // queued前owned状态不借后来改写。
    RequireWorldColor(hidden,receipt.frame.view,500,0,{11,15,19},1); // 隐藏实例确实不绘制，而非仅改统计。
    Require(hidden != background, "state visibility hid both instances");
    std::cout << "material states WARP: two instances, affine UV, four resident samplers, alpha pair, move/hide/cancel/fault; no static reupload\n";
}
