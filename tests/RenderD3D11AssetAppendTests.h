#pragma once
// 复用已有原创平面、粒子夹具和等待器；新断言针对资源身份、页追加及整组回滚。
using AssetResourceQuery = std::int32_t (WAR3_RENDER_CALL*)(War3RendererHandle, std::uint64_t*, std::uint32_t);
void TestAssetAppend(const War3RenderPluginApiV2* api, const War3RenderCreateInfoV2& info,
    InstanceRosterSetFault fault = nullptr, AssetResourceQuery query = nullptr, bool promotion = false) {
    Require(api->structSize >= offsetof(War3RenderPluginApiV2, getEditorRosterCapabilities) + sizeof(api->getEditorRosterCapabilities)
        && api->getEditorRosterCapabilities, "asset append capability tail missing");
    RendererOwner renderer{api,api->createRenderer(&info)}; Require(renderer.handle != nullptr,"asset append renderer unavailable");
    std::uint32_t capabilities = 0;
    Require(api->getEditorRosterCapabilities(renderer.handle,&capabilities) == 0
        && (capabilities & WAR3_RENDER_ROSTER_APPEND_ASSETS), "asset append not advertised");
    const War3RenderCameraV2 camera{sizeof(camera),0,0,0,3500,-1.5707963f,1.5f};
    Require(api->setCamera(renderer.handle,&camera) == 0,"asset append camera rejected");
    ParticleGpuFixture fixture; fixture.Bind(1,1);
    fixture.pose.base.instanceCount = fixture.pose.base.groupCount = fixture.pose.base.alphaCount = fixture.pose.colorCount = 1;
    auto plain = fixture.pose.base; plain.structSize = sizeof(plain);
    if (promotion) fixture.models.initialPose = &plain;
    War3RenderEditorPresentationTargetV2 target{sizeof(target)};
    Require(api->getEditorPresentationTarget(renderer.handle,&target) == 0,"asset append initial target missing");
    std::uint8_t visible = 1, hidden = 0;
    War3RenderEditorVisibilityUpdateV2 mask{sizeof(mask),1,promotion ? 0u : 1u,0,target,1,&visible,&hidden,nullptr,nullptr,nullptr};
    const War3RenderEditorModelSceneV2 initial{sizeof(initial),0,nullptr,promotion ? &fixture.models : nullptr,
        promotion ? nullptr : &fixture.scene,&mask};
    Require(api->submitEditorModelScene(renderer.handle,&initial) == 0,"asset append baseline rejected");
    ParticleGpuCapture before;
    if (promotion) {
        before.pixels.resize(320*240*4); before.receipt.models.structSize = sizeof(before.receipt.models);
        Require(api->captureSkinnedFrameRgba(renderer.handle,before.pixels.data(),before.pixels.size(),&before.receipt.models) == 0,
            "plain baseline capture failed");
    } else before = CaptureParticles(renderer,1,1);
    std::array<std::uint64_t,7> identities{};
    if (query) Require(query(renderer.handle,identities.data(),7) == 0 && identities[0] == 1,"baseline GPU identities unavailable");
    War3RenderInstanceRosterResultV2 base{sizeof(base)};
    Require(api->getEditorInstanceRosterResult(renderer.handle,0,&base) == 0,"asset append base missing");
    auto mesh = fixture.skin.mesh; mesh.mesh.baseVertex = 4; mesh.mesh.firstIndex = 6; mesh.mesh.firstLayer = 1;
    auto model = fixture.skin.model; model.model.firstMesh = 1;
    auto layer = fixture.skin.layer; layer.textureIndex = 0;
    fixture.skin.instances[1].instance.model = 1;
    auto pose = fixture.pose; pose.base.sceneVersion = 2;
    pose.base.instanceCount = pose.base.groupCount = pose.base.alphaCount = pose.colorCount = 2;
    fixture.colors[1] = {{0,0,1}};
    auto frame = fixture.frame; frame.sceneVersion = 2; frame.modelPose = &pose.base;
    auto draw = fixture.draw; draw.texture = promotion ? 0u : 1u; frame.draws = &draw;
    std::array<std::uint8_t,2> both{1,1};
    mask = {sizeof(mask),2,1,0,base.target,2,both.data(),&hidden,nullptr,nullptr,nullptr};
    War3RenderAssetAppendV2 assets{}; assets.structSize = sizeof(assets); assets.prefix = {4,6,1,1,1,0,promotion ? 0u : 1u};
    assets.vertexCount = 4; assets.indexCount = 6; assets.layerCount = assets.meshCount = assets.modelCount = 1;
    assets.modelTextureCount = assets.particleTextureCount = 1;
    assets.vertices = fixture.skin.vertices.data(); assets.indices = fixture.skin.indices.data(); assets.layers = &layer;
    assets.meshes = &mesh; assets.models = &model; assets.modelTextures = assets.particleTextures = &fixture.texture;
    War3RenderEditorAssetRosterV2 extended{}; auto& request = extended.base;
    request.structSize = sizeof(extended); request.flags = WAR3_RENDER_ROSTER_APPEND_ASSETS; request.expected = base.target;
    request.expectedPresentationRevision = base.presentationRevision; request.expectedAssetIdentity = base.assetIdentity;
    request.expectedLayersRevision = base.layersRevision; request.candidatePose = &pose.base;
    request.candidateParticles = &frame; request.visibility = &mask; extended.assets = &assets;
    std::uint64_t ticket = 0;
    ++assets.prefix.vertexCount;
    Require(api->submitEditorInstanceRoster(renderer.handle,&request,&ticket) != 0 && !ticket,"stale asset prefix accepted");
    --assets.prefix.vertexCount;
    if (fault) for (const auto stage : {1u,2u,4u,5u}) {
        Require(fault(renderer.handle,stage) == 0,"append fault arm failed");
        Require(api->submitEditorInstanceRoster(renderer.handle,&request,&ticket) == 0,"append fault not queued");
        const auto rejected = WaitInstanceRoster(renderer,ticket);
        Require(rejected.state == WAR3_RENDER_ROSTER_REJECTED && rejected.result < 0,"append fault became completed");
        const auto old = CaptureParticles(renderer,1,1);
        Require(old.pixels == before.pixels && old.receipt.models.skin.staticUploadCount == before.receipt.models.skin.staticUploadCount
            && old.receipt.models.skin.matrixUploadCount == before.receipt.models.skin.matrixUploadCount
            && old.receipt.particles.textureUploadCount == before.receipt.particles.textureUploadCount
            && old.receipt.particles.dynamicUploadCount == before.receipt.particles.dynamicUploadCount,
            "append failure changed old frame or upload counts");
        std::array<std::uint64_t,7> unchanged{};
        Require(query && query(renderer.handle,unchanged.data(),7) == 0 && unchanged == identities,"failed append replaced resident GPU owners");
    }
    Require(api->submitEditorInstanceRoster(renderer.handle,&request,&ticket) == 0,"asset append not queued");
    fixture.white.fill(0); // 返回后不得再借调用方纹理。
    const auto result = WaitInstanceRoster(renderer,ticket);
    Require(result.state == WAR3_RENDER_ROSTER_COMPLETED && result.result == 0 && result.assetIdentity == base.assetIdentity + 1
        && result.target.mode == WAR3_EDITOR_PRESENTATION_PARTICLES && result.completed.presentation.visibleInstanceCount == 2
        && result.completed.skin.staticUploadCount == before.receipt.models.skin.staticUploadCount + 1
        && result.completed.particles.textureUploadCount == before.receipt.particles.textureUploadCount + 1,
        "asset append ticket, promotion or new-only upload counts wrong");
    const auto accepted = CaptureParticles(renderer,2,1); fixture.white.fill(255);
    RequireWorldColor(accepted.pixels,accepted.receipt.models.frame.view,-500,0,{255,0,0},1);
    RequireWorldColor(accepted.pixels,accepted.receipt.models.frame.view,500,0,{0,0,255},1);
    if (query) {
        std::array<std::uint64_t,7> appended{};
        Require(query(renderer.handle,appended.data(),7) == 0 && appended[0] == 2 && appended[1] == 1 && appended[2] == 2
            && appended[3] == identities[3] && appended[4] == identities[4] && appended[6] == identities[6],
            "append reuploaded old VB, IB or particle texture");
    }
    // 再增20只B、删回A、恢复B：都只重绑动态roster，不再次上传新页/纹理。
    std::uint64_t sceneVersion = 2;
    for (const auto count : {21u,1u,2u}) {
        Require(api->getEditorInstanceRosterResult(renderer.handle,0,&base) == 0,"cached append target missing");
        std::vector<War3RenderSkinnedModelInstanceV2> instances(count,fixture.skin.instances[1]); instances[0] = fixture.skin.instances[0];
        std::vector<War3RenderGroupMatrixV2> groups(count,fixture.skin.groups[1]); groups[0] = fixture.skin.groups[0];
        std::vector<float> alphas(count,1); std::vector<War3RenderMeshColorV2> colors(count,{{0,0,1}}); colors[0] = {{1,0,0}};
        std::vector<std::uint8_t> visibility(count,0); visibility[0] = 1; if (count > 1) visibility[1] = 1;
        for (std::uint32_t i = 0; i < count; ++i) instances[i].firstGroup = instances[i].firstAlpha = i;
        pose.base.sceneVersion = ++sceneVersion; pose.base.instanceCount = pose.base.groupCount = pose.base.alphaCount = pose.colorCount = count;
        pose.base.instances = instances.data(); pose.base.groups = groups.data(); pose.base.alphas = alphas.data(); pose.colors = colors.data();
        frame.sceneVersion = sceneVersion;
        mask = {sizeof(mask),count,1,0,base.target,sceneVersion,visibility.data(),&hidden,nullptr,nullptr,nullptr};
        request.structSize = sizeof(request); request.flags = 0; request.expected = base.target;
        request.expectedPresentationRevision = base.presentationRevision; request.expectedAssetIdentity = base.assetIdentity;
        request.expectedLayersRevision = base.layersRevision;
        Require(api->submitEditorInstanceRoster(renderer.handle,&request,&ticket) == 0,"cached new asset roster rejected");
        const auto cached = WaitInstanceRoster(renderer,ticket);
        Require(cached.state == WAR3_RENDER_ROSTER_COMPLETED && cached.assetIdentity == result.assetIdentity
            && cached.completed.skin.staticUploadCount == result.completed.skin.staticUploadCount
            && cached.completed.particles.textureUploadCount == result.completed.particles.textureUploadCount,
            "cached new asset roster reuploaded static pages");
        (void)CaptureParticles(renderer,sceneVersion,1);
    }
    std::cout << "asset append WARP: A->A+B->20B->A->A+B, immutable prefix" << (promotion ? ", RGB/particle promotion\n" : "\n");
}
void TestAssetAppendFaults(const std::filesystem::path& directory, const War3RenderCreateInfoV2& info) {
    const auto module = LoadLibraryExW((directory / L"war3-render-d3d11-test.dll").c_str(),nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    Require(module != nullptr,"asset append fault DLL missing");
    struct Guard { HMODULE module; ~Guard() { FreeLibrary(module); } } guard{module};
    const auto api = reinterpret_cast<War3GetRenderPluginApiV2>(GetProcAddress(module,"War3MapEditor_GetRenderPluginApiV2"));
    const auto fault = reinterpret_cast<InstanceRosterSetFault>(GetProcAddress(module,"War3RenderTest_SetFault"));
    const auto query = reinterpret_cast<AssetResourceQuery>(GetProcAddress(module,"War3RenderTest_GetAssetResources"));
    Require(api && fault && query,"asset append test controls missing");
    TestAssetAppend(api(WAR3_RENDER_PLUGIN_API_V2),info,fault,query);
}
