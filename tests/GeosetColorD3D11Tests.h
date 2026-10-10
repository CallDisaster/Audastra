#pragma once
#include "War3MapCore/RenderPluginModelColor.h"
#include "RenderSkinFixture.h"

// 动态颜色必须由真实GPU绘制；同一个静态场景连续更新RGB，不能退回贴图/顶点烘焙。
void TestDynamicGeosetColors(const War3RenderPluginApiV2* api,const War3RenderCreateInfoV2& info) {
    RendererOwner renderer{api,api->createRenderer(&info)};
    Require(renderer.handle != nullptr,"dynamic GEOA renderer missing");
    War3RenderSkinDynamicColorCapabilitiesV2 capability{};capability.base.base.structSize=sizeof(capability);
    Require(api->getSkinStatus(renderer.handle,&capability.base.base)==0 &&
        capability.dynamicColorFormat==WAR3_RENDER_DYNAMIC_MESH_RGB_V1,"dynamic GEOA capability missing");
    const War3RenderCameraV2 camera{sizeof(camera),0,0,0,3500,-1.5707963f,1.5f};
    Require(api->setCamera(renderer.handle,&camera)==0,"dynamic GEOA camera failed");
    const War3RenderMeshV2 empty{sizeof(empty),0,0,919,nullptr,nullptr};
    Require(api->submitMesh(renderer.handle,&empty)==0,"dynamic GEOA revision failed");
    RenderSkinFixture fixture;
    for(auto& i:fixture.instances) { i.instance.color[0]=0.5f;i.instance.color[1]=i.instance.color[2]=1; }
    std::array<War3RenderMeshColorV2,2> rgb{{{{0.5f,0,0}},{{0,0.5f,0.75f}}}};
    War3RenderDynamicColoredSkinUpdateV2 pose{fixture.Update(),WAR3_RENDER_DYNAMIC_MESH_RGB_V1,2,rgb.data()};
    pose.base.structSize=sizeof(pose);
    const std::array<War3RenderMeshColorV2,1> staticRgb{{{{0.125f,0.25f,0.25f}}}};
    War3RenderColoredSkinnedSceneV2 scene{fixture.Scene(pose.base),WAR3_RENDER_STATIC_MESH_RGB_V1,1,staticRgb.data()};
    scene.base.structSize=sizeof(scene);
    Require(api->submitSkinnedModelScene(renderer.handle,&scene.base)==0,"dynamic GEOA scene failed");
    War3RenderSkinnedCapturedFrameV2 receipt{};receipt.structSize=sizeof(receipt);
    const auto capture=[&](std::uint64_t version) {
        std::vector<std::uint8_t> pixels(static_cast<std::size_t>(info.width)*info.height*4);
        Require(api->captureSkinnedFrameRgba(renderer.handle,pixels.data(),pixels.size(),&receipt)==0 &&
            receipt.skin.contentVersion==version && receipt.skin.staticUploadCount==1 && receipt.skin.matrixUploadCount==version,
            "dynamic GEOA frame/version/static upload mismatch");return pixels;
    };
    const auto first=capture(1);
    RequireWorldColor(first,receipt.frame.view,-500,0,{64,0,0},1);
    RequireWorldColor(first,receipt.frame.view,500,0,{0,128,191},1);
    rgb[1]={{0,0.25f,1}};pose.base.contentVersion=2;
    Require(api->updateSkinMatrices(renderer.handle,&pose.base)==0,"RGB-only update rejected");
    const auto second=capture(2);
    RequireWorldColor(second,receipt.frame.view,-500,0,{64,0,0},1);
    RequireWorldColor(second,receipt.frame.view,500,0,{0,64,255},1);
    Require(first!=second,"RGB changed but actual pixels did not");
    pose.base.contentVersion=3;Require(api->updateSkinMatrices(renderer.handle,&pose.base)==0,"repeat RGB failed");
    Require(capture(3)==second,"same RGB/matrices produced different pixels");
    pose.base.contentVersion=4;pose.colorCount=1;
    Require(api->updateSkinMatrices(renderer.handle,&pose.base)!=0,"short dynamic RGB accepted");
    Require(capture(3)==second,"rejected dynamic RGB changed actual pixels");
    std::cout<<"GEOA WARP: dynamic RGB per instance, no double static multiply, repeat and rejected preservation passed\n";
}

// 嵌入既有完整 WARP 入口；仅查 GPU 实画，不用 CPU 着色图替代。
void TestGeosetColors(const War3RenderPluginApiV2* api, const War3RenderCreateInfoV2& info) {
    RendererOwner renderer{api, api->createRenderer(&info)};
    Require(renderer.handle != nullptr, "GEOA renderer creation failed");
    War3RenderSkinColorCapabilitiesV2 capability{}; capability.base.structSize = sizeof(capability);
    Require(api->getSkinStatus(renderer.handle, &capability.base) == 0 &&
        capability.colorFormat == WAR3_RENDER_STATIC_MESH_RGB_V1, "GEOA color capability missing");
    const War3RenderCameraV2 camera{sizeof(camera),0,0,0,3500,-1.5707963f,1.5f};
    Require(api->setCamera(renderer.handle, &camera) == 0, "GEOA camera rejected");
    const War3RenderMeshV2 empty{sizeof(empty),0,0,918,nullptr,nullptr};
    Require(api->submitMesh(renderer.handle, &empty) == 0, "GEOA revision rejected");
    RenderSkinFixture fixture;
    for (auto& instance : fixture.instances) {
        instance.instance.color[0] = 0.5f;
        instance.instance.color[1] = instance.instance.color[2] = 1.0f;
    }
    auto pose = fixture.Update(); auto plain = fixture.Scene(pose);
    const std::array<War3RenderMeshColorV2,1> colors{{{{0.5f,0.25f,0.75f}}}};
    War3RenderColoredSkinnedSceneV2 colored{plain, WAR3_RENDER_STATIC_MESH_RGB_V1,1,colors.data()};
    colored.base.structSize = sizeof(colored);
    Require(api->submitSkinnedModelScene(renderer.handle, &colored.base) == 0, "GEOA colored scene rejected");
    War3RenderSkinnedCapturedFrameV2 receipt{}; receipt.structSize = sizeof(receipt);
    const auto capture = [&](std::uint64_t content) {
        std::vector<std::uint8_t> pixels(static_cast<std::size_t>(info.width)*info.height*4);
        Require(api->captureSkinnedFrameRgba(renderer.handle,pixels.data(),pixels.size(),&receipt) == 0,
            "GEOA actual GPU frame missing");
        Require(receipt.skin.sceneVersion == 1 && receipt.skin.contentVersion == content &&
            receipt.skin.result == 0 && receipt.skin.staticUploadCount == 1, "GEOA wrong version or static reupload");
        return pixels;
    };
    const auto initial = capture(1);
    // unshaded白色材质 × 实例(0.5,1,1) × geoset(0.5,0.25,0.75)，不是BGR也不是instance替换。
    RequireWorldColor(initial, receipt.frame.view, -500, 0, {64,64,191}, 1);
    RequireWorldColor(initial, receipt.frame.view, 500, 0, {64,64,191}, 1);
    fixture.groups[1].values[9] = 800; pose = fixture.Update(2);
    Require(api->updateSkinMatrices(renderer.handle, &pose) == 0, "GEOA matrix-only update rejected");
    const auto moved = capture(2);
    RequireWorldColor(moved, receipt.frame.view, 800, 0, {64,64,191}, 1);
    Require(receipt.skin.matrixUploadCount == 2 && initial != moved, "GEOA pose did not update");
    auto bad = colored; pose.sceneVersion = 2; pose.contentVersion = 1; bad.colorCount = 0;
    Require(api->submitSkinnedModelScene(renderer.handle,&bad.base) != 0, "GEOA short color table accepted");
    Require(capture(2) == moved, "GEOA invalid scene changed pixels");
    const War3RenderModelSceneV2 oldEmpty{sizeof(oldEmpty)};
    Require(api->submitModelScene(renderer.handle,&oldEmpty) == 0, "GEOA legacy slot unavailable");
    Capture(renderer,info.width,info.height,918);
    std::cout << "GEOA WARP: RGB*instance, matrix-only update and rejected-state preservation passed\n";
    TestDynamicGeosetColors(api,info);
}
