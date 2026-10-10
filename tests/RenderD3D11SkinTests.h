#pragma once
#include "RenderSkinFixture.h"
#include "GeosetColorD3D11Tests.h"
// 复用 RenderD3D11Tests 的 Require / RequireWorldColor / RendererOwner，不另建测试运行器。
void TestSkinnedModels(const War3RenderPluginApiV2* api, const War3RenderCreateInfoV2& info) {
    const auto available = [](const War3RenderPluginApiV2* p) {
        return p && p->structSize >= offsetof(War3RenderPluginApiV2,captureSkinnedFrameRgba) + sizeof(p->captureSkinnedFrameRgba)
            && p->submitSkinnedModelScene && p->updateSkinMatrices && p->getSkinStatus && p->captureSkinnedFrameRgba;
    };
    Require(available(api), "optional skin tail missing");
    auto shortProvider = *api; shortProvider.structSize = static_cast<std::uint32_t>(offsetof(War3RenderPluginApiV2,submitSkinnedModelScene));
    Require(!available(&shortProvider) && !available(nullptr), "old V2 provider advertised skin tail");
    RendererOwner renderer{api,api->createRenderer(&info)};
    Require(renderer.handle != nullptr, "skin renderer creation failed");
    const War3RenderCameraV2 camera{sizeof(camera),0,0,0,3500,-1.5707963f,1.5f};
    Require(api->setCamera(renderer.handle,&camera) == 0, "skin camera rejected");
    const War3RenderMeshV2 empty{sizeof(empty),0,0,917,nullptr,nullptr};
    Require(api->submitMesh(renderer.handle,&empty) == 0, "skin revision rejected");
    RenderSkinFixture fixture; auto pose = fixture.Update(); auto scene = fixture.Scene(pose);
    std::array<std::uint8_t, 16> white{}; white.fill(255);
    const War3RenderMipRgbaV2 mip{sizeof(mip),2,2,white.size(),white.data()};
    const War3RenderTextureMipChainV2 texture{sizeof(texture),1,0,1,1,&mip};
    fixture.layer.textureIndex = 0; scene.textureCount = 1; scene.textures = &texture;
    Require(api->submitSkinnedModelScene(renderer.handle,&scene) == 0, "skin scene rejected");
    const auto vertices = fixture.vertices; fixture.vertices.fill({}); white.fill(0); // 必须深拷贝。
    War3RenderSkinnedCapturedFrameV2 receipt{}; receipt.structSize = sizeof(receipt);
    const auto capture = [&](std::uint64_t expected) {
        std::vector<std::uint8_t> pixels(static_cast<std::size_t>(info.width) * info.height * 4);
        Require(api->captureSkinnedFrameRgba(renderer.handle,pixels.data(),pixels.size(),&receipt) == 0, "skin completed capture failed");
        Require(receipt.skin.active == 1 && receipt.skin.result == 0 && receipt.skin.sceneVersion == 1
            && receipt.skin.contentVersion == expected && receipt.skin.frameCount == receipt.frame.capture.frameCount
            && receipt.frame.view.frameCount == receipt.skin.frameCount && receipt.frame.capture.documentRevision == 917
            && std::memcmp(&receipt.frame.camera,&camera,sizeof(camera)) == 0, "skin receipt bound to wrong content/frame");
        RequireExactMatrix(receipt.frame.view,camera); return pixels;
    };
    const auto first = capture(1);
    RequireWorldColor(first,receipt.frame.view,-500,0,{255,0,0},1);
    RequireWorldColor(first,receipt.frame.view,500,0,{0,255,0},1);
    Require(receipt.skin.staticUploadCount == 1 && receipt.skin.matrixUploadCount == 1, "initial upload counters incorrect");
    fixture.vertices = vertices;
    for (std::uint64_t version = 2; version <= 5; ++version) {
        fixture.groups[1].values[9] = 500 + static_cast<float>(version) * 50;
        pose = fixture.Update(version);
        Require(api->updateSkinMatrices(renderer.handle,&pose) == 0, "palette update rejected");
        const auto changed = capture(version);
        Require(changed != first && receipt.skin.staticUploadCount == 1 && receipt.skin.matrixUploadCount == version,
            "pose update reuploaded geometry or failed to change pixels");
        RequireWorldColor(changed,receipt.frame.view,-500,0,{255,0,0},1);
        RequireWorldColor(changed,receipt.frame.view,fixture.groups[1].values[9],0,{0,255,0},1);
    }
    const auto beforeAlpha = capture(5);
    pose.contentVersion = 6; fixture.alphas[1] = 0;
    Require(api->updateSkinMatrices(renderer.handle,&pose) == 0, "alpha-only update rejected");
    const auto hidden = capture(6);
    Require(hidden != beforeAlpha, "geoset alpha did not change pixels");
    RequireWorldColor(hidden,receipt.frame.view,-500,0,{255,0,0},1);
    auto invalid = pose; invalid.contentVersion = 7; invalid.groupCount = 1;
    Require(api->updateSkinMatrices(renderer.handle,&invalid) != 0, "bad palette accepted");
    Require(capture(6) == hidden, "bad update replaced committed version/pixels");
    invalid = pose; invalid.contentVersion = 5;
    Require(api->updateSkinMatrices(renderer.handle,&invalid) != 0, "stale content accepted");
    invalid = pose; invalid.contentVersion = 7; invalid.matrixStrideBytes = 64;
    Require(api->updateSkinMatrices(renderer.handle,&invalid) != 0, "bad layout accepted");
    fixture.vertices[0].group = 1; pose.sceneVersion = 2; pose.contentVersion = 1;
    Require(api->submitSkinnedModelScene(renderer.handle,&scene) != 0, "invalid static skin accepted");
    Require(capture(6) == hidden, "bad static request replaced last valid state");
    War3RenderSkinStatusV2 status{}; status.structSize = sizeof(status);
    Require(api->getSkinStatus(renderer.handle,&status) == 0 && status.result == 0 && status.contentVersion == 6
        && status.staticUploadCount == 1 && status.matrixUploadCount == 6, "skin status not actual drawn state");
    const War3RenderModelSceneV2 oldEmpty{sizeof(oldEmpty)};
    Require(api->submitModelScene(renderer.handle,&oldEmpty) == 0, "return to legacy slot failed");
    Capture(renderer,info.width,info.height,917);
    std::vector<std::uint8_t> sentinels(static_cast<std::size_t>(info.width) * info.height * 4,0x7b);
    receipt.skin.contentVersion = 123456;
    Require(api->captureSkinnedFrameRgba(renderer.handle,sentinels.data(),sentinels.size(),&receipt) != 0
        && receipt.skin.contentVersion == 123456 && sentinels.front() == 0x7b, "inactive capture overwrote caller output");
    TestGeosetColors(api, info);
    {
        RendererOwner sharedRenderer{api,api->createRenderer(&info)};
        Require(sharedRenderer.handle != nullptr, "shared-pose renderer creation failed");
        War3RenderSkinSharedRangeCapabilitiesV2 capability{};
        capability.base.base.base.structSize = sizeof(capability);
        Require(api->getSkinStatus(sharedRenderer.handle, &capability.base.base.base) == 0
            && capability.sharedRangeMatrixFormat == WAR3_RENDER_GROUP_LOCAL_COLUMN_3X4_SHARED_RANGES,
            "shared-pose format not advertised");
        Require(api->setCamera(sharedRenderer.handle, &camera) == 0, "shared-pose camera rejected");
        RenderSkinFixture sharedFixture;
        sharedFixture.groups[0].values[9] = 0; sharedFixture.groups[1].values[9] = 250;
        sharedFixture.instances[0].instance.transform[3] = -500;
        sharedFixture.instances[1].instance.transform[3] = 500;
        sharedFixture.instances[1].firstGroup = sharedFixture.instances[1].firstAlpha = 0;
        auto sharedPose = sharedFixture.Update();
        sharedPose.matrixFormat = WAR3_RENDER_GROUP_LOCAL_COLUMN_3X4_SHARED_RANGES;
        auto sharedScene = sharedFixture.Scene(sharedPose);
        Require(api->submitSkinnedModelScene(sharedRenderer.handle, &sharedScene) == 0, "shared Stand scene rejected");
        std::vector<std::uint8_t> pixels(static_cast<std::size_t>(info.width)*info.height*4);
        War3RenderSkinnedCapturedFrameV2 sharedFrame{sizeof(sharedFrame)};
        Require(api->captureSkinnedFrameRgba(sharedRenderer.handle,pixels.data(),pixels.size(),&sharedFrame) == 0,
            "shared Stand capture failed");
        RequireWorldColor(pixels,sharedFrame.frame.view,-500,0,{255,0,0},1);
        RequireWorldColor(pixels,sharedFrame.frame.view,500,0,{0,255,0},1);
        sharedFixture.instances[1].firstGroup = sharedFixture.instances[1].firstAlpha = 1;
        ++sharedPose.contentVersion;
        Require(api->updateSkinMatrices(sharedRenderer.handle,&sharedPose) == 0, "dynamic slot switch rejected");
        Require(api->captureSkinnedFrameRgba(sharedRenderer.handle,pixels.data(),pixels.size(),&sharedFrame) == 0
            && sharedFrame.skin.staticUploadCount == 1 && sharedFrame.skin.contentVersion == 2,
            "slot switch reuploaded static geometry or captured wrong frame");
        RequireWorldColor(pixels,sharedFrame.frame.view,-500,0,{255,0,0},1);
        RequireWorldColor(pixels,sharedFrame.frame.view,750,0,{0,255,0},1);
    }
    std::cout << "skin WARP: shared geometry / independent groups, upload counters, exact-version capture and rejected preservation passed\n";
}
