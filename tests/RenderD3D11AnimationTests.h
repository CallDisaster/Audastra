#pragma once
// 在既有 WARP 入口验证真实服务 -> 插件 -> 完成帧，不代替真实 Footman 与界面验收。
void TestAnimationServiceWARP(const War3RenderPluginApiV2* api,const War3RenderCreateInfoV2& info) {
    using namespace War3Map::Render;
    const auto bytes=AnimationTest::Bytes();
    std::array<AnimationInstanceSpec,2> specs;
    for(std::size_t i=0;i<specs.size();++i) {
        specs[i].identity={i+1,700}; specs[i].hostKey=std::to_string(i+1); specs[i].model.modelPath="synthetic.mdx";
        specs[i].sequenceIndex=static_cast<std::uint32_t>(i==0?1:0); specs[i].elapsedMs=100+i*300;
        specs[i].model.position={static_cast<float>(i)*600-300,-100,0}; specs[i].model.scale={20,20,20};
        specs[i].model.tint=i?std::array<float,3>{0,1,0}:std::array<float,3>{1,0,0};
    }
    BakedModelMesh cliff;
    cliff.vertices={{-500,350,0,0,0,1,0,0},{500,350,0,0,0,1,1,0},{0,500,150,0,0,1,0,1}};cliff.indices={0,1,2};
    auto created=ModelAnimationService::Create({700,991,"WARP-synthetic"},specs,[&](auto path,auto)->War3Map::Result<AnimationResource> {
        if(path!="synthetic.mdx")return War3Map::Result<AnimationResource>::Failure("Test","RESOURCE_NOT_FOUND","原创测试无游戏贴图。");
        return War3Map::Result<AnimationResource>::Success({bytes,"synthetic",std::string(64,'a')});
    },{},std::span(&cliff,1));
    if (!created.Succeeded()) for (const auto& diagnostic : created.diagnostics)
        std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    Require(created.Succeeded(),"animation service WARP setup failed"); auto service=*created.value;
    const War3RenderCameraV2 camera{sizeof(camera),0,100,0,2200,-1.5707963f,1.2f};
    AnimationAdvance command; command.worldCamera=AnimationWorldCamera(camera);
    auto first=service->Prepare(nullptr,command); Require(first.Succeeded(),"animation initial pose failed");
    RendererOwner renderer{api,api->createRenderer(&info)}; Require(renderer.handle!=nullptr,"animation renderer failed");
    const War3RenderMeshV2 empty{sizeof(empty),0,0,991,nullptr,nullptr};
    Require(api->submitMesh(renderer.handle,&empty)==0 && api->setCamera(renderer.handle,&camera)==0,"animation view setup failed");
    AnimationRenderer bridge(api,renderer.handle);
    Require(bridge.Submit(service->Scene(),*first.value).Succeeded(),"animation scene rejected");
    std::vector<std::byte> pixels(static_cast<std::size_t>(info.width)*info.height*4);
    auto capture=bridge.Capture(pixels,991);Require(capture.Succeeded(),"animation same-frame capture failed");
    const auto initialPixels=pixels; const auto uploads=capture.value->skin.staticUploadCount;
    Require(uploads==1 && service->Scene()->models.size()==2,"animation lost baked model or duplicated static upload");
    command={}; AnimationEdit edit; edit.identity={2,700}; edit.elapsedMs=700; command.edits.push_back(edit);
    auto second=service->Prepare(first.value->get(),command);Require(second.Succeeded(),"animation next pose failed");
    Require(bridge.Update(*second.value).Succeeded(),"animation next upload failed");
    capture=bridge.Capture(pixels,991);Require(capture.Succeeded() && pixels!=initialPixels,"different per-instance pose did not change image");
    Require((*first.value)->poses[1].matrixHash==(*second.value)->poses[1].matrixHash &&
        (*first.value)->poses[2].matrixHash!=(*second.value)->poses[2].matrixHash,"independent poses mixed");
    const auto secondPixels=pixels;
    auto repeat=service->Prepare(second.value->get());Require(repeat.Succeeded() && bridge.Update(*repeat.value).Succeeded(),"repeat rejected");
    capture=bridge.Capture(pixels,991);
    Require(capture.Succeeded() && pixels==secondPixels && capture.value->skin.staticUploadCount==uploads &&
        capture.value->skin.matrixUploadCount==3 && capture.value->skin.contentVersion==(*repeat.value)->contentVersion,
        "same-time image/counters/version mismatch");
    RequireExactMatrix(capture.value->frame.view,camera);
    std::cout<<"animation WARP: service batches / baked geometry retained / independent poses / same-time pixels / version / static uploads passed\n";
}
