#pragma once
#include "War3MapCore/RenderPluginInstanceRoster.h"
// 复用已有原创粒子/蒙皮夹具与WARP入口，不复制另一套资产或运行器。
War3RenderInstanceRosterResultV2 WaitInstanceRoster(const RendererOwner& renderer, std::uint64_t ticket) {
    const auto deadline = GetTickCount64() + 10000;
    War3RenderInstanceRosterResultV2 result{};
    do {
        result = {}; result.structSize = sizeof(result);
        Require(renderer.api->getEditorInstanceRosterResult(renderer.handle, ticket, &result) == 0
            && result.ticket == ticket, "roster ticket lookup failed");
        if (result.state != WAR3_RENDER_ROSTER_QUEUED) return result;
        Sleep(1);
    } while (GetTickCount64() < deadline);
    throw std::runtime_error("roster ticket completion timed out");
}
using InstanceRosterSetFault = std::int32_t (WAR3_RENDER_CALL*)(War3RendererHandle, std::uint32_t);
using InstanceRosterGateRead = std::int32_t (WAR3_RENDER_CALL*)(War3RendererHandle);
using InstanceRosterAssetRead = std::int32_t (WAR3_RENDER_CALL*)(War3RendererHandle, std::uint64_t*, std::uint32_t);
void TestInstanceRoster(const War3RenderPluginApiV2* api, const War3RenderCreateInfoV2& info,
    InstanceRosterSetFault fault = nullptr, InstanceRosterGateRead gate = nullptr, InstanceRosterAssetRead assets = nullptr) {
    Require(api->structSize >= offsetof(War3RenderPluginApiV2, getEditorInstanceRosterResult)
        + sizeof(api->getEditorInstanceRosterResult) && api->submitEditorInstanceRoster && api->getEditorInstanceRosterResult,
        "resident roster tail missing");
    Require(api->structSize >= offsetof(War3RenderPluginApiV2, cancelEditorInstanceRoster)
        + sizeof(api->cancelEditorInstanceRoster) && api->cancelEditorInstanceRoster, "roster cancellation tail missing");
    RendererOwner renderer{api, api->createRenderer(&info)}; Require(renderer.handle != nullptr, "roster renderer unavailable");
    const War3RenderCameraV2 camera{sizeof(camera),0,0,0,3500,-1.5707963f,1.5f};
    Require(api->setCamera(renderer.handle, &camera) == 0, "roster camera rejected");
    ParticleGpuFixture fixture; fixture.Bind(1,1);
    fixture.pose.base.instanceCount = fixture.pose.base.groupCount = fixture.pose.base.alphaCount = fixture.pose.colorCount = 1;
    War3RenderEditorPresentationTargetV2 target{sizeof(target)};
    Require(api->getEditorPresentationTarget(renderer.handle, &target) == 0, "roster initial target missing");
    std::uint8_t visible = 1;
    War3RenderEditorVisibilityUpdateV2 mask{sizeof(mask),1,1,0,target,1,&visible,&visible,nullptr,nullptr,nullptr};
    const War3RenderEditorModelSceneV2 scene{sizeof(scene),0,nullptr,nullptr,&fixture.scene,&mask};
    Require(api->submitEditorModelScene(renderer.handle, &scene) == 0, "roster baseline rejected");
    const auto before = CaptureParticles(renderer,1,1);
    War3RenderInstanceRosterResultV2 base{}; base.structSize = sizeof(base);
    Require(api->getEditorInstanceRosterResult(renderer.handle,0,&base) == 0 && base.state == WAR3_RENDER_ROSTER_TARGET,
        "roster base identity missing");
    std::vector<War3RenderSkinnedModelInstanceV2> instances(20, fixture.skin.instances[0]);
    std::vector<War3RenderGroupMatrixV2> groups(20, fixture.skin.groups[0]);
    std::vector<float> alphas(20,1);
    std::vector<War3RenderMeshColorV2> colors(20, {{0,0,1}});
    std::vector<std::uint8_t> instancesVisible(20,0); instancesVisible[0] = 1;
    for (std::uint32_t i = 0; i < instances.size(); ++i) {
        instances[i].firstGroup = instances[i].firstAlpha = i;
        groups[i].values[9] += static_cast<float>(i * 100);
    }
    auto pose = fixture.pose; pose.base.sceneVersion = 2;
    pose.base.instanceCount = pose.base.groupCount = pose.base.alphaCount = pose.colorCount = 20;
    pose.base.instances = instances.data(); pose.base.groups = groups.data(); pose.base.alphas = alphas.data(); pose.colors = colors.data();
    auto particles = fixture.frame; particles.sceneVersion = 2; particles.modelPose = &pose.base;
    std::uint8_t hiddenParticle = 0;
    mask = {sizeof(mask),20,1,0,base.target,2,instancesVisible.data(),&hiddenParticle,nullptr,nullptr,nullptr};
    War3RenderEditorInstanceRosterV2 request{}; request.structSize = sizeof(request); request.expected = base.target;
    request.expectedPresentationRevision = base.presentationRevision; request.expectedAssetIdentity = base.assetIdentity;
    request.expectedLayersRevision = base.layersRevision; request.candidatePose = &pose.base;
    request.candidateParticles = &particles; request.visibility = &mask;
    std::uint64_t ticket = 0;
    auto bad = request; bad.flags = 0x80000000u;
    Require(api->submitEditorInstanceRoster(renderer.handle,&bad,&ticket) != 0 && !ticket, "unknown asset flag accepted");
    bad = request; ++bad.expected.contentVersion;
    Require(api->submitEditorInstanceRoster(renderer.handle,&bad,&ticket) != 0 && !ticket, "stale roster base accepted");
    Require(api->updateSkinMatrices(renderer.handle,&pose.base) != 0, "ordinary update accepted changed roster");
    if (fault) {
        for (const auto stage : {1u,2u}) {
            Require(fault(renderer.handle,stage) == 0, "roster fault arm failed");
            Require(api->submitEditorInstanceRoster(renderer.handle,&request,&ticket) == 0 && ticket, "fault roster was not queued");
            const auto rejected = WaitInstanceRoster(renderer,ticket);
            Require(rejected.state == WAR3_RENDER_ROSTER_REJECTED && rejected.result < 0 && !rejected.completed.view.frameCount,
                "failed prepare became completed");
            const auto old = CaptureParticles(renderer,1,1);
            Require(old.pixels == before.pixels && old.receipt.models.skin.staticUploadCount == before.receipt.models.skin.staticUploadCount
                && old.receipt.models.skin.matrixUploadCount == before.receipt.models.skin.matrixUploadCount
                && old.receipt.particles.textureUploadCount == before.receipt.particles.textureUploadCount
                && old.receipt.particles.dynamicUploadCount == before.receipt.particles.dynamicUploadCount,
                "roster failure published a partial pair or advanced upload counts");
        }
        Require(gate && assets, "prePublish cancellation evidence controls unavailable");
        std::array<std::uint64_t, 7> oldResources{}, afterResources{};
        Require(assets(renderer.handle, oldResources.data(), static_cast<std::uint32_t>(oldResources.size())) == 0,
            "baseline resource identities missing");
        struct ReleaseGate {
            InstanceRosterSetFault fault; War3RendererHandle handle;
            ~ReleaseGate() { (void)fault(handle, 0); }
        } release{fault, renderer.handle};
        Require(fault(renderer.handle, 6) == 0, "prePublish gate arm failed");
        Require(api->submitEditorInstanceRoster(renderer.handle, &request, &ticket) == 0 && ticket,
            "cancellation roster not queued");
        const auto deadline = GetTickCount64() + 10000;
        while (gate(renderer.handle) != 1 && GetTickCount64() < deadline) Sleep(1);
        Require(gate(renderer.handle) == 1, "prePublish cancellation gate not reached");
        War3RenderInstanceRosterResultV2 cancelled{}; cancelled.structSize = sizeof(cancelled);
        Require(api->cancelEditorInstanceRoster(renderer.handle, ticket, &cancelled) == 0
            && cancelled.ticket == ticket && cancelled.state == WAR3_RENDER_ROSTER_SUPERSEDED
            && !cancelled.completed.view.frameCount, "queued cancellation did not return exact superseded ticket");
        Require(fault(renderer.handle, 0) == 0, "prePublish gate release failed");
        Require(WaitInstanceRoster(renderer, ticket).state == WAR3_RENDER_ROSTER_SUPERSEDED,
            "cancelled prepared GPU roster was published later");
        const auto old = CaptureParticles(renderer, 1, 1);
        Require(assets(renderer.handle, afterResources.data(), static_cast<std::uint32_t>(afterResources.size())) == 0
            && oldResources == afterResources && old.pixels == before.pixels
            && old.receipt.models.skin.staticUploadCount == before.receipt.models.skin.staticUploadCount
            && old.receipt.models.skin.matrixUploadCount == before.receipt.models.skin.matrixUploadCount
            && old.receipt.particles.textureUploadCount == before.receipt.particles.textureUploadCount
            && old.receipt.particles.dynamicUploadCount == before.receipt.particles.dynamicUploadCount,
            "cancelled roster changed resident VB/IB/textures, old completed frame or upload counts");
    }
    Require(api->submitEditorInstanceRoster(renderer.handle,&request,&ticket) == 0 && ticket, "twenty roster not queued");
    colors[0] = {{1,0,0}}; groups[0].values[9] = 900; // 返回后宿主内存不再被借用。
    const auto completed = WaitInstanceRoster(renderer,ticket);
    War3RenderInstanceRosterResultV2 alreadyCompleted{}; alreadyCompleted.structSize = sizeof(alreadyCompleted);
    Require(api->cancelEditorInstanceRoster(renderer.handle, ticket, &alreadyCompleted) == 0
        && alreadyCompleted.state == WAR3_RENDER_ROSTER_COMPLETED && alreadyCompleted.ticket == ticket
        && alreadyCompleted.completed.view.frameCount == completed.completed.view.frameCount,
        "completed cancellation falsely rolled back or changed completion identity");
    Require(completed.state == WAR3_RENDER_ROSTER_COMPLETED && completed.result == 0
        && completed.assetIdentity == base.assetIdentity && completed.target.modelEpoch == base.target.modelEpoch + 1
        && completed.completed.skin.frameCount == completed.completed.view.frameCount
        && completed.completed.presentation.instanceCount == 20 && completed.completed.presentation.visibleInstanceCount == 1
        && completed.completed.presentation.visibleParticleDrawCount == 0
        && completed.completed.skin.staticUploadCount == before.receipt.models.skin.staticUploadCount
        && completed.completed.skin.matrixUploadCount == before.receipt.models.skin.matrixUploadCount + 1
        && completed.completed.particles.textureUploadCount == before.receipt.particles.textureUploadCount,
        "roster completion identity, visibility or immutable upload counts wrong");
    const auto accepted = CaptureParticles(renderer,2,1);
    RequireWorldColor(accepted.pixels,accepted.receipt.models.frame.view,-500,0,{0,0,255},1);
    // 真正读取显隐/附加层组合RGBA；单独粒子Capture不会经过显隐名册数量校验。
    // 当前名册20项、驻留资产首次上传仅1项，正好覆盖复用模型后持续被误判过期的问题。
    const auto captureRoster = [&](const War3RenderInstanceRosterResultV2& current,
        const std::vector<std::uint8_t>& expectedPixels) {
        std::vector<std::uint8_t> pixels(static_cast<std::size_t>(info.width) * info.height * 4, 0xCD);
        War3RenderEditorCapturedFrameV2 presented{sizeof(presented)};
        Require(api->captureEditorPresentationFrameRgba(renderer.handle, current.target.modelEpoch,
            current.presentationRevision, pixels.data(), pixels.size(), &presented) == 0,
            "resident roster presentation RGBA capture failed");
        Require(presented.presentation.instanceCount == current.target.instanceCount
            && presented.presentation.modelEpoch == current.target.modelEpoch
            && presented.presentation.presentationRevision == current.presentationRevision
            && presented.skin.sceneVersion == current.target.sceneVersion
            && presented.skin.contentVersion == current.target.contentVersion
            && presented.skin.frameCount == presented.frame.capture.frameCount
            && presented.frame.view.frameCount == presented.frame.capture.frameCount
            && presented.frame.capture.ticket && pixels == expectedPixels,
            "resident roster presentation pixels or completed identity changed");
        War3RenderModelLayersTargetV2 layers{sizeof(layers)};
        Require(api->getModelLayersTarget(renderer.handle, &layers) == 0
            && layers.parentModelEpoch == current.target.modelEpoch && layers.layersRevision == current.layersRevision,
            "resident roster layered target identity changed");
        War3RenderLayeredCapturedFrameV2 layered{sizeof(layered)};
        Require(api->captureLayeredFrameRgba(renderer.handle, layers.layersRevision,
            pixels.data(), pixels.size(), &layered) == 0,
            "resident roster layered RGBA capture failed");
        Require(layered.layerCount == layers.layerCount && layered.layersRevision == layers.layersRevision
            && layered.world.presentation.instanceCount == current.target.instanceCount
            && layered.world.presentation.modelEpoch == current.target.modelEpoch
            && layered.world.presentation.presentationRevision == current.presentationRevision
            && layered.world.skin.sceneVersion == current.target.sceneVersion
            && layered.world.skin.contentVersion == current.target.contentVersion
            && layered.world.skin.frameCount == layered.world.frame.capture.frameCount
            && layered.world.frame.view.frameCount == layered.world.frame.capture.frameCount
            && layered.world.frame.capture.ticket && pixels == expectedPixels,
            "resident roster layered pixels or completed identity changed");
        std::fill(pixels.begin(), pixels.end(), static_cast<std::uint8_t>(0xCD));
        Require(api->captureEditorPresentationFrameRgba(renderer.handle, current.target.modelEpoch - 1,
            current.presentationRevision, pixels.data(), pixels.size(), &presented) == 2
            && api->captureLayeredFrameRgba(renderer.handle, layers.layersRevision - 1,
                pixels.data(), pixels.size(), &layered) == 2
            && std::all_of(pixels.begin(), pixels.end(), [](auto pixel) { return pixel == 0xCD; }),
            "stale roster capture identity accepted or wrote pixels");
    };
    captureRoster(completed, accepted.pixels);
    ++pose.base.contentVersion; particles.contentVersion = pose.base.contentVersion;
    Require(api->updateParticleModelFrame(renderer.handle,&particles) == 0, "ordinary update rejects new roster baseline");
    (void)CaptureParticles(renderer,2,2);
    base.structSize = sizeof(base); Require(api->getEditorInstanceRosterResult(renderer.handle,0,&base) == 0, "zero roster base missing");
    pose.base.sceneVersion = 3; pose.base.contentVersion = 1;
    pose.base.instanceCount = pose.base.groupCount = pose.base.alphaCount = pose.colorCount = 0;
    pose.base.instances = nullptr; pose.base.groups = nullptr; pose.base.alphas = nullptr; pose.colors = nullptr;
    particles = {sizeof(particles),0,0,0,3,1,nullptr,nullptr,&pose.base};
    mask = {sizeof(mask),0,0,0,base.target,3,nullptr,nullptr,nullptr,nullptr,nullptr};
    request.expected = base.target; request.expectedPresentationRevision = base.presentationRevision;
    request.expectedLayersRevision = base.layersRevision;
    Require(api->submitEditorInstanceRoster(renderer.handle,&request,&ticket) == 0, "zero roster not queued");
    const auto zero = WaitInstanceRoster(renderer,ticket);
    Require(zero.state == WAR3_RENDER_ROSTER_COMPLETED && zero.completed.presentation.instanceCount == 0
        && zero.completed.presentation.particleDrawCount == 0 && zero.completed.particles.sceneVersion == 3
        && zero.completed.skin.staticUploadCount == before.receipt.models.skin.staticUploadCount
        && zero.completed.skin.matrixUploadCount == accepted.receipt.models.skin.matrixUploadCount + 1,
        "empty roster lost particle mode or reuploaded static assets");
    const auto emptied = CaptureParticles(renderer,3,1);
    captureRoster(zero, emptied.pixels);
    std::cout << "roster WARP: atomic one->twenty->zero, owned inputs, strict ordinary baseline, immutable geometry/textures"
        << (fault ? ", fault1/2 rollback, gated prePublish cancel preserves resources\n" : "\n");
}
void TestInstanceRosterFaults(const std::filesystem::path& directory, const War3RenderCreateInfoV2& info) {
    const auto module = LoadLibraryExW((directory / L"war3-render-d3d11-test.dll").c_str(),nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    Require(module != nullptr,"roster fault DLL missing");
    struct ModuleGuard { HMODULE value; ~ModuleGuard() { FreeLibrary(value); } } guard{module};
    const auto getApi = reinterpret_cast<War3GetRenderPluginApiV2>(GetProcAddress(module,"War3MapEditor_GetRenderPluginApiV2"));
    const auto fault = reinterpret_cast<InstanceRosterSetFault>(GetProcAddress(module,"War3RenderTest_SetFault"));
    const auto gate = reinterpret_cast<InstanceRosterGateRead>(GetProcAddress(module,"War3RenderTest_RosterGateReached"));
    const auto assets = reinterpret_cast<InstanceRosterAssetRead>(GetProcAddress(module,"War3RenderTest_GetAssetResources"));
    Require(getApi && fault && gate && assets,"roster fault controls unavailable");
    TestInstanceRoster(getApi(WAR3_RENDER_PLUGIN_API_V2),info,fault,gate,assets);
}
