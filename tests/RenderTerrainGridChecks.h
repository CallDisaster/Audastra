#pragma once

// 由 RenderD3D11Tests.cpp 在通用读回助手定义后包含；所有像素 oracle 均走原展开入口。
struct TerrainGridFixture {
    std::array<War3RenderTerrainGridGroundVertexV2, 9> ground{};
    std::array<War3RenderTerrainGridWaterVertexV2, 9> water{};
    std::array<War3RenderTerrainGridBaseQuadV2, 3> base{{
        {0, 0.5f / 64, 15.5f / 64, 0.5f / 64, 15.5f / 64},
        {2, 16.5f / 64, 31.5f / 64, 32.5f / 64, 47.5f / 64}, {3, 0, 0, 0, 0}}};
    // 基础quad按源tile排序；indices按材质稳定分组，故意与quad顺序不同。
    std::array<std::uint32_t, 18> baseIndices{8,9,10,9,11,10, 0,1,2,1,3,2, 4,5,6,5,7,6};
    std::array<War3RenderTerrainBatchV2, 2> baseBatches{{{0,6,WAR3_RENDER_HEIGHT_COLOR_TEXTURE},{6,12,0}}};
    std::array<War3RenderTerrainVertexV2, 8> transitions{};
    std::array<std::uint32_t, 12> transitionIndices{4,5,6,5,7,6, 0,1,2,1,3,2};
    std::array<War3RenderTerrainBatchV2, 2> transitionBatches{{
        {0,6,1u | WAR3_RENDER_TERRAIN_ALPHA_BLEND},{6,6,1u | WAR3_RENDER_TERRAIN_ALPHA_BLEND}}};
    std::array<std::uint32_t, 6> blightIndices{4,5,6,5,7,6};
    std::array<War3RenderTerrainBlightBatchV2, 1> blightBatches{{{0,6,0.5f/64,7.5f/64,0.5f/32,7.5f/32}}};
    // tile 1没有基础面但仍保留水；水序不得按alpha重排。
    std::array<War3RenderTerrainGridWaterQuadV2, 3> waterQuads{{{1,15},{2,0},{3,5}}};
    std::array<std::uint32_t, 18> waterIndices{0,1,2,1,3,2, 4,5,6,5,7,6, 8,9,10,9,11,10};
    std::array<std::vector<std::vector<std::uint8_t>>, 3> rgba;
    std::array<std::vector<War3RenderMipRgbaV2>, 3> mips;
    std::vector<std::uint8_t> waterRgba;
    War3RenderMipRgbaV2 waterMip{};
    std::array<War3RenderTextureMipChainV2, 2> groundTextures{};
    War3RenderTextureMipChainV2 blightTexture{}, waterTexture{};

    TerrainGridFixture() {
        for (std::uint32_t y = 0; y < 3; ++y) for (std::uint32_t x = 0; x < 3; ++x) {
            const auto i = y * 3 + x;
            ground[i] = {float(x) * 128 - 128, float(y) * 128 - 128, float(x * 9 + y * 17),
                0.6f + float(x) * 0.15f, 0.5f + float(y) * 0.2f, 0.75f,
                0.2f + float(x) * 0.1f, 0.7f, 0.4f + float(y) * 0.1f};
            water[i] = {75.f + float(x * 6 + y * 8), 0.4f + float(x) * 0.2f, 0.8f, 0.6f + float(y) * 0.15f};
        }
        for (std::uint32_t texture = 0; texture < 3; ++texture) {
            for (std::uint32_t level = 0; level < 7; ++level) {
                const auto width = std::max(1u, 64u >> level);
                const auto height = std::max(1u, (texture == 2 ? 32u : 64u) >> level);
                auto& pixels = rgba[texture].emplace_back(std::size_t(width) * height * 4);
                for (std::uint32_t y = 0; y < height; ++y) for (std::uint32_t x = 0; x < width; ++x) {
                    const auto at = (std::size_t(y) * width + x) * 4;
                    pixels[at] = std::uint8_t(40 + (x * 5 + y * 3 + level * 23 + texture * 61) % 190);
                    pixels[at + 1] = std::uint8_t(30 + (x * 2 + y * 7 + level * 31) % 200);
                    pixels[at + 2] = std::uint8_t(20 + (x * 11 + y * 3 + texture * 53) % 220);
                    pixels[at + 3] = texture == 0 ? 255 : std::uint8_t(50 + (x + y * 9) % 150);
                }
            }
            for (std::uint32_t level = 0; level < 7; ++level) mips[texture].push_back({sizeof(War3RenderMipRgbaV2),
                std::max(1u, 64u >> level), std::max(1u, (texture == 2 ? 32u : 64u) >> level),
                rgba[texture][level].size(), rgba[texture][level].data()});
        }
        groundTextures = {{{sizeof(War3RenderTextureMipChainV2),7,2,4,4,mips[0].data()},
            {sizeof(War3RenderTextureMipChainV2),7,2,4,4,mips[1].data()}}};
        blightTexture = {sizeof(blightTexture),7,3,8,4,mips[2].data()};
        waterRgba.resize(128 * 128 * 4);
        for (std::uint32_t y = 0; y < 128; ++y) for (std::uint32_t x = 0; x < 128; ++x) {
            const auto at = (std::size_t(y) * 128 + x) * 4;
            const auto level = (y / 32) * 4 + x / 32;
            waterRgba[at] = std::uint8_t(25 + x % 32 * 5); waterRgba[at + 1] = std::uint8_t(80 + y % 32 * 4);
            waterRgba[at + 2] = 220; waterRgba[at + 3] = std::uint8_t(16 + level * 14);
        }
        waterMip = {sizeof(waterMip),128,128,waterRgba.size(),waterRgba.data()};
        waterTexture = {sizeof(waterTexture),1,0,4,4,&waterMip};
        for (std::uint32_t q = 0; q < 2; ++q) for (std::uint32_t corner = 0; corner < 4; ++corner) {
            const auto tile = q == 0 ? 0u : 2u, index = (tile / 2 + corner / 2) * 3 + tile % 2 + corner % 2;
            const auto& v = ground[index];
            transitions[q * 4 + corner] = {v.x,v.y,v.groundZ,v.textureR,v.textureG,v.textureB,
                corner % 2 ? 31.5f/64 : 16.5f/64, corner < 2 ? 31.5f/64 : 16.5f/64};
        }
    }
    War3RenderTerrainGridFrameV2 Frame(std::uint64_t revision = 984) const {
        return {sizeof(War3RenderTerrainGridFrameV2),WAR3_RENDER_TERRAIN_GRID_FETCH_V1,3,3,9,3,18,2,6,1,3,18,
            {sizeof(War3RenderTerrainMipSceneV2),8,12,2,2,revision,transitions.data(),transitionIndices.data(),
                groundTextures.data(),transitionBatches.data()}, ground.data(),water.data(),base.data(),baseIndices.data(),
            baseBatches.data(),blightIndices.data(),blightBatches.data(),&blightTexture,waterQuads.data(),waterIndices.data(),&waterTexture};
    }
};

void TestTerrainGrid(const RendererOwner& renderer) {
    const auto* api = renderer.api;
    Require(api->structSize >= offsetof(War3RenderPluginApiV2,getTerrainGridCaptureFacts)
        + sizeof(api->getTerrainGridCaptureFacts) && api->getTerrainGridCapabilities && api->submitTerrainGridFrame
        && api->getTerrainGridCaptureFacts, "terrain grid optional ABI absent");
    struct FutureCaps { War3RenderTerrainGridCapabilitiesV2 prefix; std::uint64_t tail; } caps{{sizeof(FutureCaps)},0x987654321ull};
    Require(api->getTerrainGridCapabilities(nullptr,&caps.prefix) == 0 && caps.prefix.structSize == sizeof(caps.prefix)
        && caps.prefix.gridFormat == 1 && caps.prefix.maxGroundTextures == 16 && caps.prefix.maxBlightBatches == 32
        && caps.prefix.maxWaterTextures == 1 && !caps.prefix.reserved && caps.tail == 0x987654321ull,
        "grid caps changed prefix or future tail");
    auto shortCaps = caps.prefix; --shortCaps.structSize; const auto savedCaps = shortCaps;
    Require(api->getTerrainGridCapabilities(nullptr,&shortCaps) != 0 && std::memcmp(&shortCaps,&savedCaps,sizeof(shortCaps)) == 0,
        "short grid caps output changed");
    TerrainGridFixture fixture;
    std::vector<War3RenderTerrainVertexV2> expanded;
    std::vector<std::uint32_t> indices;
    std::vector<War3RenderTerrainBatchV2> batches;
    // 独立固定布局展开：不调用生产取角/CellUv函数，也不从grid shader反推期望像素。
    const auto append = [&](std::uint32_t tile, bool fallback, bool water, float left,float right,float top,float bottom) {
        const auto first = static_cast<std::uint32_t>(expanded.size());
        for (std::uint32_t corner = 0; corner < 4; ++corner) {
            const auto source = (tile / 2 + corner / 2) * 3 + tile % 2 + corner % 2;
            const auto& g = fixture.ground[source]; const auto& w = fixture.water[source];
            expanded.push_back({g.x,g.y,water ? w.waterZ : g.groundZ,
                water ? w.red : fallback ? g.fallbackR : g.textureR,
                water ? w.green : fallback ? g.fallbackG : g.textureG,
                water ? w.blue : fallback ? g.fallbackB : g.textureB,
                corner % 2 ? right : left,corner < 2 ? bottom : top});
        }
        indices.insert(indices.end(),{first,first+1,first+2,first+1,first+3,first+2});
    };
    append(3,true,false,0,0,0,0); batches.push_back({0,6,WAR3_RENDER_HEIGHT_COLOR_TEXTURE});
    for (const auto q : {0u,1u}) { const auto& v = fixture.base[q]; append(v.sourceTile,false,false,v.uvLeft,v.uvRight,v.uvTop,v.uvBottom); }
    batches.push_back({6,12,0});
    for (const auto q : {1u,0u}) {
        const auto first = static_cast<std::uint32_t>(expanded.size()), offset = static_cast<std::uint32_t>(indices.size());
        expanded.insert(expanded.end(),fixture.transitions.begin()+q*4,fixture.transitions.begin()+q*4+4);
        indices.insert(indices.end(),{first,first+1,first+2,first+1,first+3,first+2});
        batches.push_back({offset,6,1u | WAR3_RENDER_TERRAIN_ALPHA_BLEND});
    }
    const auto& blight = fixture.blightBatches[0];
    append(2,false,false,blight.uvLeft,blight.uvRight,blight.uvTop,blight.uvBottom);
    batches.push_back({30,6,2u | WAR3_RENDER_TERRAIN_ALPHA_BLEND});
    for (const auto& q : fixture.waterQuads) {
        const auto col = q.alphaLevel % 4, row = q.alphaLevel / 4;
        append(q.sourceTile,false,true,(float(col*32)+0.5f)/128,(float(col*32+32)-0.5f)/128,
            (float(row*32)+0.5f)/128,(float(row*32+32)-0.5f)/128);
    }
    batches.push_back({36,18,3u | WAR3_RENDER_TERRAIN_TRANSLUCENT});
    const std::array<War3RenderTextureMipChainV2,4> textures{fixture.groundTextures[0],fixture.groundTextures[1],fixture.blightTexture,fixture.waterTexture};
    const War3RenderTerrainMipSceneV2 reference{sizeof(reference),static_cast<std::uint32_t>(expanded.size()),
        static_cast<std::uint32_t>(indices.size()),4,static_cast<std::uint32_t>(batches.size()),984,
        expanded.data(),indices.data(),textures.data(),batches.data()};
    Require(api->resize(renderer.handle,320,240) == 0,"grid resize rejected");
    const std::array<War3RenderCameraV2,3> cameras{{{sizeof(War3RenderCameraV2),0,0,35,650,-1.5707963f,1.4f},
        {sizeof(War3RenderCameraV2),0,0,35,3500,-1.5707963f,1.4f},
        {sizeof(War3RenderCameraV2),0,0,35,650,-0.45f,0.6f}}};
    std::vector<std::uint8_t> accepted;
    War3RenderViewInfoV2 acceptedView{};
    for (const auto& camera : cameras) {
        Require(api->setCamera(renderer.handle,&camera) == 0 && api->submitTerrainMipScene(renderer.handle,&reference) == 0,
            "clean expanded grid oracle rejected");
        const auto expected = CaptureFrame(renderer,320,240,984,camera);
        auto frame = fixture.Frame();
        Require(api->submitTerrainGridFrame(renderer.handle,&frame) == 0,"valid shared-grid frame rejected");
        auto actual = CaptureFrame(renderer,320,240,984,camera);
        Require(actual.pixels == expected.pixels && Colors(actual.pixels) > 50,"grid pixels differ from independent expanded oracle");
        struct FutureFacts { War3RenderTerrainGridCaptureFactsV2 prefix; std::array<std::uint8_t,17> tail; } facts{};
        facts.prefix.structSize = sizeof(facts); facts.tail.fill(0xb3);
        Require(api->getTerrainGridCaptureFacts(renderer.handle,&actual.receipt.view,&facts.prefix) == 0
            && facts.prefix.structSize == sizeof(facts.prefix) && facts.prefix.gridFormat == 1
            && SameView(facts.prefix.view,actual.receipt.view) && facts.prefix.gridVertexCount == 9
            && facts.prefix.baseQuadCount == 3 && facts.prefix.transitionVertexCount == 8 && facts.prefix.blightIndexCount == 6
            && facts.prefix.blightBatchCount == 1 && facts.prefix.waterQuadCount == 3 && facts.prefix.groundTextureCount == 2
            && facts.prefix.passBits == 3 && std::all_of(facts.tail.begin(),facts.tail.end(),[](auto value){ return value == 0xb3; }),
            "GPU accepted facts differ from captured scene or changed future tail");
        accepted = std::move(actual.pixels); acceptedView = actual.receipt.view;
    }
    auto unchangedFacts = War3RenderTerrainGridCaptureFactsV2{sizeof(War3RenderTerrainGridCaptureFactsV2)};
    const auto reject = [&](const War3RenderTerrainGridFrameV2& invalid) {
        const auto before = ReadView(renderer);
        Require(api->submitTerrainGridFrame(renderer.handle,&invalid) != 0,"invalid grid accepted");
        const auto after = ReadView(renderer);
        Require(before.sceneEpoch == after.sceneEpoch && Capture(renderer,320,240,984) == accepted,
            "rejected grid changed epoch or previous pixels");
    };
    auto bad = fixture.Frame(); bad.baseQuadCount = 262145; reject(bad);
    bad = fixture.Frame(); bad.waterGrid = nullptr; reject(bad);
    bad = fixture.Frame(); ++bad.groundScene.structSize; reject(bad);
    fixture.baseIndices[6] = 4; bad = fixture.Frame(); reject(bad); fixture.baseIndices[6] = 0;
    fixture.transitionIndices[6] = 4; bad = fixture.Frame(); reject(bad); fixture.transitionIndices[6] = 0;
    fixture.waterQuads[0].alphaLevel = 16; bad = fixture.Frame(); reject(bad); fixture.waterQuads[0].alphaLevel = 15;
    fixture.blightIndices[0] = 8; bad = fixture.Frame(); reject(bad); fixture.blightIndices[0] = 4;
    fixture.ground[4].x += 0.25f; bad = fixture.Frame(); reject(bad); fixture.ground[4].x -= 0.25f;
    fixture.water[4].waterZ = std::numeric_limits<float>::quiet_NaN(); bad = fixture.Frame(); reject(bad); fixture.water[4].waterZ = 89;
    auto stale = acceptedView; ++stale.sceneEpoch;
    auto preserved = unchangedFacts;
    Require(api->getTerrainGridCaptureFacts(renderer.handle,&stale,&unchangedFacts) == 2
        && std::memcmp(&unchangedFacts,&preserved,sizeof(preserved)) == 0,"stale grid facts overwrote caller output");
    // 借入所有池在submit返回后均可变化，GPU仍须使用owned副本。
    const auto borrowed = fixture.Frame();
    Require(api->submitTerrainGridFrame(renderer.handle,&borrowed) == 0,"grid resubmit rejected");
    fixture.ground.fill({}); fixture.water.fill({}); fixture.base.fill({}); fixture.waterQuads.fill({});
    std::fill(fixture.waterRgba.begin(),fixture.waterRgba.end(),std::uint8_t{0});
    Require(Capture(renderer,320,240,984) == accepted,"grid borrowed geometry or pixels were retained");
    // 空grid是明确的空场景，仍有可查询的零统计；旧入口则明确清掉grid统计。
    War3RenderTerrainGridFrameV2 empty{}; empty.structSize = sizeof(empty); empty.gridFormat = 1;
    empty.groundScene.structSize = sizeof(empty.groundScene); empty.groundScene.documentRevision = 985;
    Require(api->submitTerrainGridFrame(renderer.handle,&empty) == 0,"explicit empty grid rejected");
    const auto emptyPixels = Capture(renderer,320,240,985); const auto emptyView = ReadView(renderer);
    Require(api->getTerrainGridCaptureFacts(renderer.handle,&emptyView,&unchangedFacts) == 0
        && !unchangedFacts.gridVertexCount && !unchangedFacts.waterQuadCount && !unchangedFacts.passBits
        && emptyPixels != accepted,"empty grid inherited geometry or lost explicit mode");
    const War3RenderMeshV2 old{sizeof(old),0,0,986,nullptr,nullptr};
    Require(api->submitMesh(renderer.handle,&old) == 0,"old empty mesh rejected after grid");
    (void)Capture(renderer,320,240,986); const auto legacyView = ReadView(renderer); preserved = unchangedFacts;
    Require(api->getTerrainGridCaptureFacts(renderer.handle,&legacyView,&unchangedFacts) == 2
        && std::memcmp(&unchangedFacts,&preserved,sizeof(preserved)) == 0,"legacy mesh inherited grid facts");
    std::cout << "terrain grid: independent expanded pixels near/far/oblique, hidden water, reordered materials, original mips,"
        " exact completed facts, invalid atomicity, borrowed lifetime, empty/legacy reset passed\n";
}

void TestTerrainGridFault(const std::filesystem::path& directory, const War3RenderCreateInfoV2& info) {
    const auto module = LoadLibraryExW((directory / L"war3-render-d3d11-test.dll").c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    Require(module != nullptr,"grid fault provider absent");
    struct ModuleOwner { HMODULE value; ~ModuleOwner() { FreeLibrary(value); } } library{module};
    const auto getApi = reinterpret_cast<War3GetRenderPluginApiV2>(GetProcAddress(module,"War3MapEditor_GetRenderPluginApiV2"));
    using Fault = std::int32_t (WAR3_RENDER_CALL *)(War3RendererHandle,std::uint32_t);
    using Resources = std::int32_t (WAR3_RENDER_CALL *)(War3RendererHandle,std::uint64_t*,std::uint32_t,War3RenderTerrainGridCaptureFactsV2*);
    const auto fault = reinterpret_cast<Fault>(GetProcAddress(module,"War3RenderTest_SetFault"));
    const auto resources = reinterpret_cast<Resources>(GetProcAddress(module,"War3RenderTest_GetTerrainGridResources"));
    Require(getApi && fault && resources,"grid fault controls absent");
    const auto* api = getApi(WAR3_RENDER_PLUGIN_API_V2);
    RendererOwner renderer{api,api->createRenderer(&info)};
    Require(renderer.handle != nullptr,"grid fault renderer unavailable");
    TerrainGridFixture fixture;
    const auto input = fixture.Frame();
    const War3RenderCameraV2 camera{sizeof(camera),0,0,35,650,-1.5707963f,1.4f};
    Require(api->resize(renderer.handle,320,240) == 0 && api->setCamera(renderer.handle,&camera) == 0
        && api->submitTerrainGridFrame(renderer.handle,&input) == 0,"grid fault baseline rejected");
    const auto baseline = CaptureFrame(renderer,320,240,984,camera);
    std::array<std::uint64_t,16> previous{}, failed{};
    War3RenderTerrainGridCaptureFactsV2 previousFacts{sizeof(previousFacts)}, failedFacts{sizeof(failedFacts)};
    Require(resources(renderer.handle,previous.data(),16,&previousFacts) == 0
        && std::all_of(previous.begin(),previous.end(),[](auto id){return id != 0;})
        && previousFacts.gridFormat == 1 && previousFacts.waterQuadCount == 3,"accepted GPU grid owners absent");
    auto changed = fixture.Frame(985); fixture.ground[4].groundZ += 45;
    Require(fault(renderer.handle,9) == 0 && api->submitTerrainGridFrame(renderer.handle,&changed) == 0,
        "grid pre-swap failure could not be armed");
    std::vector<std::uint8_t> pixels(320 * 240 * 4,0xa7);
    War3RenderCapturedFrameV2 receipt{sizeof(receipt)}; const auto before = receipt;
    Require(api->captureFrameRgba(renderer.handle,pixels.data(),pixels.size(),&receipt) != 0
        && std::memcmp(&receipt,&before,sizeof(receipt)) == 0
        && std::all_of(pixels.begin(),pixels.end(),[](auto value){return value == 0xa7;}),
        "failed grid upload certified or overwrote a new frame");
    Require(resources(renderer.handle,failed.data(),16,&failedFacts) == 0 && failed == previous
        && failedFacts.gridFormat == previousFacts.gridFormat && failedFacts.gridVertexCount == previousFacts.gridVertexCount
        && failedFacts.waterQuadCount == previousFacts.waterQuadCount && failedFacts.passBits == previousFacts.passBits,
        "pre-swap grid failure replaced previous GPU owners or accepted facts");
    auto facts = previousFacts; const auto frozen = facts;
    Require(api->getTerrainGridCaptureFacts(renderer.handle,&baseline.receipt.view,&facts) != 0
        && std::memcmp(&facts,&frozen,sizeof(facts)) == 0,"upload failure signed pending grid facts");
    Require(fault(renderer.handle,0) == 0 && api->submitTerrainGridFrame(renderer.handle,&changed) == 0,
        "valid grid recovery rejected");
    const auto recovered = CaptureFrame(renderer,320,240,985,camera);
    Require(recovered.pixels != baseline.pixels && recovered.receipt.view.sceneEpoch > baseline.receipt.view.sceneEpoch
        && api->getTerrainGridCaptureFacts(renderer.handle,&recovered.receipt.view,&facts) == 0,
        "grid did not recover a distinct completed scene after failed upload");
    std::cout << "terrain grid GPU fault: sixteen old owner identities retained, outputs untouched, no false completion, valid retry passed\n";
}
