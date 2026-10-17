#include "War3MapCore/Render/ModelAnimation.h"
#include "War3MapCore/Render/AnimationRenderer.h"
#include "../src/Render/Scene/ModelSkinInput.h"
#include "../src/Render/Particles/ParticleInput.h"
#include "AnimationTestModel.h"
#include "AnimationParticleContinuityTests.h"
#include "AnimationParticleVisibilityPreviewTests.h"
#include "AnimationRibbonMaterialTests.h"
#include "AnimationRibbonRendererTests.h"
#include "AnimationInstanceDeltaTests.h"
#include "ModelAnimationMaterialTextures.h"
#include "ModelPreviewReplacementTests.h"
#include "War3MapCore/Render/ModelAnimationCapture.h"
#include <fstream>
#include <algorithm>
#include <bit>
#include <cstring>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
using namespace War3Map;
using namespace War3Map::Render;
void Require(bool x,const char* message) { if (!x) throw std::runtime_error(message); }
template<class T> const T& Good(const Result<T>& r) {
    if (!r.Succeeded()) throw std::runtime_error(r.diagnostics.empty()?"empty failure":r.diagnostics.front().code+": "+r.diagnostics.front().message);
    return *r.value;
}
struct Input {
    std::vector<std::byte> bytes;
    unsigned reads=0;
    std::array<AnimationInstanceSpec,2> specs;
    Input() {
        bytes=AnimationTest::Bytes();
        for (std::size_t i=0;i<2;++i) {
            specs[i].identity={i+1,7}; specs[i].hostKey="row"+std::to_string(i);
            specs[i].model.modelPath="same.mdx"; specs[i].sequenceIndex=static_cast<std::uint32_t>(i==0?0:1); // Stand=0、Walk=1
            specs[i].elapsedMs=100+static_cast<std::uint64_t>(i)*300; specs[i].model.position={static_cast<float>(i)*200,0,0};
        }
    }
    Result<AnimationResource> Read(std::string_view path,std::uint64_t) {
        if (path!="same.mdx") return Result<AnimationResource>::Failure("Test","missing","合成模型没有贴图文件。");
        ++reads; return Result<AnimationResource>::Success({bytes,"synthetic",std::string(64,'a')});
    }
    std::shared_ptr<ModelAnimationService> Build(std::span<const BakedModelMesh> baked={}) {
        return Good(ModelAnimationService::Create({7,33,"synthetic-source"},specs,
            [&](auto p,auto n){return Read(p,n);},{},baked));
    }
};
void LargeScene() {
    Input in;
    constexpr std::size_t count = 100000;
    std::vector<AnimationInstanceSpec> specs(count, in.specs[0]);
    for (std::size_t i = 0; i < count; ++i) {
        specs[i].identity.value = i + 1; specs[i].hostKey = "large:" + std::to_string(i);
        specs[i].model.position[0] = static_cast<float>(i) * 20;
    }
    AnimationLimits limits; limits.retainUnsupportedStatic = true;
    limits.maxFrameBytes = count * (sizeof(War3RenderSkinnedModelInstanceV2) + sizeof(AnimationPoseLink)
        + sizeof(AnimationInstanceState) + sizeof(AnimationPickBounds)) + 1024ull * 1024;
    limits.maxActivePoseBytes = 128 * 1024;
    auto service = Good(ModelAnimationService::Create({7,33,"large-synthetic"}, specs,
        [&](auto p,auto n) { return in.Read(p,n); }, limits));
    AnimationAdvance advance; advance.worldCamera = War3RenderCore::LocalCamera{{0,0,100},{0,0,1}};
    auto nearFirst = Good(service->Prepare(nullptr, advance));
    Require(nearFirst->instances.size() == count && !nearFirst->poses.front().staticForBudget
        && nearFirst->poses.back().staticForBudget && nearFirst->budgetStaticInstances > 0
        && nearFirst->hiddenInstances == 0, "large scene lost instances or did not retain real Stand");
    Require(nearFirst->Describe().matrixFormat == WAR3_RENDER_GROUP_LOCAL_COLUMN_3X4_SHARED_RANGES,
        "shared Stand did not declare explicit plugin format");
    Require(nearFirst->timings.candidateInstances==count && nearFirst->timings.eligiblePoseInstances==count &&
        nearFirst->timings.poseEvaluationRequests==count-nearFirst->budgetStaticInstances,
        "timing counted shared Stand reuse as a fresh pose evaluation");
    advance.worldCamera->eye[0] = specs.back().model.position[0];
    auto nearLast = Good(service->Prepare(nearFirst.get(), advance));
    Require(nearLast->instances.size() == count && nearLast->poses.front().staticForBudget
        && !nearLast->poses.back().staticForBudget && nearLast->groups.size() == nearFirst->groups.size()
        && nearLast->alphas.size() == nearFirst->alphas.size() && in.reads == 1,
        "camera scheduling changed static assets or pool capacity");
    std::cout << "large_scene_instances=" << count << " retained_stand=" << nearLast->budgetStaticInstances << '\n';
}
void Instances() {
    Input in; auto service=in.Build(); auto first=Good(service->Prepare(nullptr));
    Require(first->poses.size()==2 && first->poses[0].matrixHash!=first->poses[1].matrixHash,"instances mixed");
    AnimationAdvance command; AnimationEdit edit; edit.identity={2,7}; edit.elapsedMs=433; command.edits.push_back(edit);
    auto second=Good(service->Prepare(first.get(),command)); auto repeat=Good(service->Prepare(second.get()));
    Require(second->poses[0].matrixHash==first->poses[0].matrixHash && second->poses[1].matrixHash!=first->poses[1].matrixHash,"time leaked");
    Require(std::memcmp(second->groups.data(),repeat->groups.data(),second->groups.size()*sizeof(second->groups[0]))==0 &&
        second->alphas==repeat->alphas && second->contentVersion<repeat->contentVersion &&
        second->poses[1].poseVersion<repeat->poses[1].poseVersion,"determinism or version");
    Require(second->timings.outputMotionHash==repeat->timings.outputMotionHash &&
        second->timings.outputMotionHash!=first->timings.outputMotionHash && repeat->timings.poseEvaluationRequests==2,
        "motion fingerprint used content/pose versions instead of evaluated numerical output");
    Require(repeat->timings.prepareNs>=repeat->timings.stateAdvanceNs+repeat->timings.poseEvaluationNs+
        repeat->timings.posePackingNs+repeat->timings.particlesTotalNs+repeat->timings.pickBoundsNs+repeat->timings.fingerprintNs,
        "CPU timing phases overlapped or exceeded Prepare");
    command={}; command.deltaMs=33;
    auto paused=Good(service->Prepare(repeat.get(),command));
    Require(paused->states[1].elapsedMs==repeat->states[1].elapsedMs,"pause advanced");
    Require(paused->timings.outputMotionHash==repeat->timings.outputMotionHash,"paused redraw counted as motion");
    command.stepPaused=true; auto step=Good(service->Prepare(paused.get(),command));
    Require(step->states[1].elapsedMs==paused->states[1].elapsedMs+33,"step missing");
    Require(in.reads==1,"pose re-read static asset");
    command.deltaMs=1001; Require(!service->Prepare(step.get(),command).Succeeded(),"unbounded dt");
    auto recovered=Good(service->Prepare(repeat.get())); Require(recovered->groups.size()==repeat->groups.size(),"cancel recovery");
    // 分数速度累积、瞬时位移、无效动作与身份；都从明确已接受帧推进。
    command={}; command.deltaMs=33;
    AnimationEdit slow; slow.identity={2,7}; slow.speed=0.5; slow.playing=true; command.edits.push_back(slow);
    auto half=Good(service->Prepare(repeat.get(),command)); command.edits.clear();
    auto halfAgain=Good(service->Prepare(half.get(),command));
    Require(half->states[1].elapsedMs==repeat->states[1].elapsedMs+16 &&
        halfAgain->states[1].elapsedMs==repeat->states[1].elapsedMs+33 && halfAgain->states[0].elapsedMs==repeat->states[0].elapsedMs,"speed/pause mixed");
    command={}; slow={}; slow.identity={1,7}; slow.transform=repeat->states[0].transform; (*slow.transform)[3]+=50;
    command.edits.push_back(slow); auto moved=Good(service->Prepare(repeat.get(),command));
    Require(moved->poses[0].matrixHash==repeat->poses[0].matrixHash && moved->instances[0].instance.transform[3]!=repeat->instances[0].instance.transform[3],"world transform baked into groups");
    Require(moved->timings.outputMotionHash!=repeat->timings.outputMotionHash,"motion fingerprint omitted instance transforms");
    command.edits[0].identity.generation=8; Require(!service->Prepare(repeat.get(),command).Succeeded(),"stale identity accepted");
    command.edits[0].identity.generation=7; command.edits[0].sequenceIndex=999;
    Require(!service->Prepare(repeat.get(),command).Succeeded(),"bad sequence accepted");
    Require(FindAnimationSequence(service->Scene()->sequences[0],"Stand")==0u && FindAnimationSequence(service->Scene()->sequences[1],"Walk")==1u,"actual SEQS resolution");
    Require(first->states[0].sequenceIndex!=first->states[1].sequenceIndex,"fixture did not use independent actions");
    std::cout<<"instances=2 actions=Stand,Walk independent=1 deterministic=1 pause_step=1 same_source_reads="<<in.reads<<" cancelled_frame_not_advanced=1\n";
}
void ViewAdmission() {
    Input input;
    for (auto& spec:input.specs) {
        spec.sequenceIndex=0; spec.elapsedMs=0; spec.globalMs=0; spec.playing=true;
    }
    input.specs[1].model.position[0]=100000;
    auto service=input.Build();
    AnimationAdvance command;
    AnimationView view; view.physicalWidth=1200; view.physicalHeight=800;
    view.camera={sizeof(War3RenderCameraV2),0,0,0,500,0,0.8f};
    command.worldView=view; command.worldCamera=AnimationWorldCamera(view.camera);
    const auto first=Good(service->Prepare(nullptr,command));
    Require(first->requestVisibility.size()==2 && first->requestVisibility[0]==1 && first->requestVisibility[1]==0 &&
        first->offscreenInstances==1 && first->offscreenPoseInstances==1 && first->timings.poseEvaluationRequests==1 &&
        first->budgetStaticInstances==0,"frustum did not exclude far pose before evaluation/budget");
    const auto poseFor=[](const AnimationFrame& frame,std::uint32_t request)->const AnimationPoseLink& {
        return *std::find_if(frame.poses.begin(),frame.poses.end(),[&](const auto& p){return p.requestIndex==request;});
    };
    Require(poseFor(*first,1).outsideView && !poseFor(*first,1).animated,
        "outside pose reported a fresh successful evaluation");
    command.deltaMs=33; command.worldView->camera.targetX=-100000;
    command.worldCamera=AnimationWorldCamera(command.worldView->camera);
    const auto empty=Good(service->Prepare(first.get(),command));
    Require(empty->offscreenInstances==2 && empty->timings.poseEvaluationRequests==0 &&
        empty->budgetStaticInstances==0 && empty->states[0].elapsedMs==33 && empty->states[1].elapsedMs==33,
        "ground-only frustum evaluated poses, spent budget or stopped offscreen clocks");
    command.worldView->camera.targetX=100000;
    command.worldCamera=AnimationWorldCamera(command.worldView->camera);
    const auto reentered=Good(service->Prepare(empty.get(),command));
    Require(reentered->requestVisibility[0]==0 && reentered->requestVisibility[1]==1 &&
        reentered->states[1].elapsedMs==66 && poseFor(*reentered,1).animated &&
        reentered->timings.poseEvaluationRequests==1 && input.reads==1,
        "reentry replayed old clock, rebuilt assets or evaluated outside pose");
    command={};
    const auto legacy=Good(service->Prepare(nullptr,command));
    Require(legacy->requestVisibility.empty() && legacy->timings.poseEvaluationRequests==2,
        "legacy unfiltered service acquired hidden view semantics");
    std::cout<<"view_admission=1 outside_pose_work=0 ground_pose_work=0 reentry_current_time=66 legacy_unfiltered=1\n";
}
void VariantActions() {
    std::vector<MdxSequence> names(5);
    names[0].name="Stand Upgrade Second"; names[1].name="Stand Upgrade Firstly";
    names[2].name="Stand Upgrade First - 2"; names[3].name="Stand Upgrade First"; names[4].name="Stand Alternate";
    Require(FindAnimationSequence(names,"Stand","first,UPGRADE")==3u &&
        FindAnimationSequence(names,"Stand","upgrade,second")==0u &&
        FindAnimationSequence(names,"Stand","alternate")==4u &&
        !FindAnimationSequence(names,"Stand","upgrade,third"),"required animation properties must match complete words");
    std::vector<MdxSequence> plain(3);
    plain[0].name="Birth"; plain[1].name="Stand Alternate"; plain[2].name="Stand";
    Require(FindAnimationSequence(plain,"Stand","upgrade,first")==2u,"missing property variants fall back to the plain Stand, not sequence 0");
    Input in;
    const std::string_view bytes(reinterpret_cast<const char*>(in.bytes.data()),in.bytes.size());
    const auto sequences=bytes.find("SEQS")+8;
    for (std::size_t i=0;i<2;++i) {
        const char* name=i==0 ? "Stand Upgrade Second" : "Stand Upgrade First";
        std::fill_n(in.bytes.begin()+static_cast<std::ptrdiff_t>(sequences+i*132),80,std::byte{});
        std::memcpy(in.bytes.data()+sequences+i*132,name,std::strlen(name));
        in.specs[i].sequenceIndex.reset(); in.specs[i].elapsedMs.reset();
        in.specs[i].model.animationProperties=i==0 ? "upgrade,first" : "upgrade,second";
    }
    constexpr std::size_t count=4096; std::vector<AnimationInstanceSpec> many(count);
    for (std::size_t i=0;i<count;++i) { many[i]=in.specs[i%2]; many[i].identity.value=i+1; many[i].hostKey="variant:"+std::to_string(i); }
    AnimationLimits limits; limits.retainUnsupportedStatic=true; limits.maxActivePoseBytes=0;
    limits.maxFrameBytes=count*(sizeof(War3RenderSkinnedModelInstanceV2)+sizeof(AnimationPoseLink)+
        sizeof(AnimationInstanceState)+sizeof(AnimationPickBounds))+128*1024;
    auto shared=Good(ModelAnimationService::Create({7,33,"variant-cache"},many,[&](auto p,auto n){return in.Read(p,n);},limits));
    auto cached=Good(shared->Prepare(nullptr));
    Require(cached->sharedPoseRanges && cached->budgetStaticInstances==count && shared->Scene()->geometry.models.size()==1 &&
        in.reads==1 && cached->states[0].sequenceIndex==1 && cached->states[1].sequenceIndex==0 &&
        cached->poses[0].staticSequenceIndex==1 && cached->poses[1].staticSequenceIndex==0 &&
        cached->poses[0].matrixHash!=cached->poses[1].matrixHash && cached->poses[0].matrixHash==cached->poses[2].matrixHash &&
        cached->pickBounds[0].available && cached->pickBounds[1].available,"budget Stand cache mixed upgrade variants");
    in.specs[0].model.animationProperties="upgrade,third";
    const auto missing=ModelAnimationService::Create({7,33,"missing-variant"},in.specs,[&](auto p,auto n){return in.Read(p,n);});
    Require(!missing.Succeeded() && missing.diagnostics.front().code=="ANIMATION_REQUIRED_SEQUENCE_MISSING","missing variant was silently accepted");
}
void RequestGeometryBudget() {
    // 原创owned静态几何隔离计费边界：无MDX求值器、粒子或材质workspace。
    const auto asset=Good(CreateCheckerBlockerAsset(std::nullopt));
    std::array<AnimationInstanceSpec,2> specs;
    for (std::size_t i=0;i<specs.size();++i) {
        specs[i].identity={i+1,71}; specs[i].hostKey="geometry-budget:"+std::to_string(i);
        specs[i].model.modelPath="original-budget-owned.mdx"; specs[i].model.ownedStaticAsset=asset;
    }
    unsigned reads=0;
    const auto read=[&](std::string_view,std::uint64_t) {
        ++reads; return Result<AnimationResource>::Failure("Test","UNEXPECTED_READ","owned预算夹具不读取MDX。");
    };
    const auto probe=Good(ModelAnimationService::Create({71,1,"geometry-budget"},specs,read));
    const auto frame=Good(probe->Prepare(nullptr));
    Require(!probe->Scene()->dynamicMaterialAlphas && !probe->Scene()->dynamicMaterialStates &&
        frame->geosetIssues.empty() && frame->materialIssues.empty() && frame->collisionMatrices.empty(),
        "owned geometry budget fixture unexpectedly acquired another charged workspace");
    const auto payloadBytes=std::uint64_t(frame->instances.size())*(sizeof(War3RenderSkinnedModelInstanceV2)+
        sizeof(AnimationPoseLink)+sizeof(AnimationPickBounds))+std::uint64_t(frame->states.size())*sizeof(AnimationInstanceState)+
        std::uint64_t(frame->groups.size())*sizeof(War3RenderGroupMatrixV2)+
        std::uint64_t(frame->alphas.size())*(sizeof(float)+sizeof(War3RenderMeshColorV2));
    const auto geometryBytes=std::uint64_t(probe->Scene()->requestGeometryRequired.capacity())*sizeof(std::uint8_t);
    // 本原创夹具只建立一个asset槽；空粒子owner本体也进入共用固定账。
    AnimationLimits exact; exact.maxFrameBytes=payloadBytes+geometryBytes+AnimationParticleMaterialOwnerBytes(1);
    auto narrow=exact; --narrow.maxFrameBytes;
    const auto refused=ModelAnimationService::Create({71,1,"geometry-budget"},specs,read,narrow);
    Require(!refused.Succeeded() && refused.diagnostics.front().code=="ANIMATION_FRAME_BUDGET",
        "Create admitted geometry provenance capacity one byte above the original frame budget");
    const auto accepted=Good(ModelAnimationService::Create({71,1,"geometry-budget"},specs,read,exact));
    Require(Good(accepted->Prepare(nullptr))->instances.size()==2,"exact owned geometry budget was rejected");
    const auto one=std::span(specs).first(1);
    const auto bounded=Good(ModelAnimationService::Create({71,1,"geometry-budget"},one,read,narrow));
    const auto before=Good(bounded->Prepare(nullptr)); const auto original=bounded->Scene();
    AnimationInstanceDelta delta; delta.session={71,2,"geometry-budget"}; delta.additions={specs[1]};
    const auto over=bounded->PrepareInstanceCatalog(delta,read);
    Require(!over.Succeeded() && over.diagnostics.front().code=="ANIMATION_FRAME_BUDGET" &&
        bounded->Scene()==original && Good(bounded->Prepare(before.get()))->states.size()==1,
        "Delta bypassed the shared one-byte budget gate or changed the resident catalog");
    const auto source=Good(ModelAnimationService::Create({71,1,"geometry-budget"},one,read,exact));
    const auto previous=Good(source->Prepare(nullptr)); const auto plan=Good(source->PrepareInstanceCatalog(delta,read));
    const auto result=Good(source->PrepareInstanceDelta(*previous,*plan));
    Require(result.frame->states.size()==2 && result.service->Scene()->requestGeometryRequired.size()==2 &&
        source->Scene()->requestGeometryRequired.size()==1 && !reads,
        "exact Delta/rebase budget changed assets, lost coverage or double charged the shared source table");
    // 显式合成容量输入：size仍为1，capacity超预算，检验旧新共存门读取capacity而非size。
    auto oversized=std::const_pointer_cast<AnimatedModelScene>(source->Scene());
    oversized->requestGeometryRequired.reserve(static_cast<std::size_t>(exact.maxFrameBytes));
    Require(oversized->requestGeometryRequired.capacity()>oversized->requestGeometryRequired.size(),
        "synthetic capacity fixture did not retain spare capacity");
    const auto capacityRefused=source->PrepareInstanceCatalog(delta,read);
    Require(!capacityRefused.Succeeded() && capacityRefused.diagnostics.front().code=="ANIMATION_DELTA_BUDGET",
        "Delta ignored the actual spare capacity of the retained provenance table");
    std::cout<<"geometry_provenance_budget exact="<<exact.maxFrameBytes<<" create_delta_one_byte_under_refused=1 capacity_not_size=1\n";
}
void Geometry() {
    Input in; BakedModelMesh cliff;
    cliff.vertices={{-10,0,0,0,0,1,0,0},{10,0,0,0,0,1,1,0},{0,10,0,0,0,1,0,1}}; cliff.indices={0,1,2};
    auto service=in.Build(std::span(&cliff,1)); auto scene=service->Scene(); auto frame=Good(service->Prepare(nullptr));
    Require(scene->retainedStaticInstances==1 && scene->animatedInstances==2 && scene->models.size()==2,"cliff disappeared");
    Require(frame->instances.front().firstGroup==0 && frame->groups[0].values[0]==1 && frame->groups[0].values[9]==0,"cliff identity");
    Require(scene->geometry.models.back().meshCount==2,"hidden geoset missing from immutable geometry");
    Require(frame->alphas[frame->instances[1].firstAlpha+1]==0,"geoset to mesh alpha");
    AnimatedSceneDescriptor descriptor; descriptor.Refresh(*scene,*frame);
    ModelScene copy; std::uint64_t size=0;
    Require(CopySkinnedModelInput(descriptor.scene,copy,size),"production plugin input refused service packet");
    auto pick=AnimationPickingScene(*scene,*frame); Require(pick->vertices.empty() && pick->instances.size()==3,"picking copied geometry");
    Require(scene->geometry.vertices.data()==service->Scene()->geometry.vertices.data(),"static storage moved");
    auto changed=in.specs; changed[0].identity.generation=8; changed[1].identity.generation=8;
    auto newSession=Good(ModelAnimationService::Create({8,0,"new-map"},changed,[&](auto p,auto n){return in.Read(p,n);}));
    Require(!newSession->Prepare(frame.get()).Succeeded(),"map epoch reused old pose");
    // 真实服务一次打包2000个实例；跨64实例的批界不能借走上一批输出。
    Input world; std::vector<AnimationInstanceSpec> many(2000);
    for (std::size_t i=0;i<many.size();++i) { many[i]=world.specs[i%2]; many[i].identity={i+1,7}; many[i].hostKey=std::to_string(i+1); }
    auto worldService=Good(ModelAnimationService::Create({7,33,"world-synthetic"},many,[&](auto p,auto n){return world.Read(p,n);}));
    auto packed=Good(worldService->Prepare(nullptr)); auto packedAgain=Good(worldService->Prepare(packed.get()));
    Require(packed->poses.size()==2000 && world.reads==1 && packed->groups.size()==4000,"world batch dropped instances or re-read asset");
    for (std::size_t i=0;i<packed->poses.size();++i) {
        const auto& p=packed->poses[i];
        Require(p.identity.value==i+1 && p.matrixHash==packed->poses[i%2].matrixHash && p.matrixHash==packedAgain->poses[i].matrixHash,"world batch overwritten");
    }
    std::cout<<"world_instances=2000 batches=32 groups=4000 owned_packets=1 source_reads=1\n";
    std::cout<<"baked_cliff=1 animated=2 all_geosets=1 mesh_alpha_mapped=1 plugin_input=accepted map_epoch_reject=1\n";
}
struct MockBackend { unsigned staticUploads=0,updates=0; ModelScene model; SkinPose pose; };
std::int32_t WAR3_RENDER_CALL Submit(War3RendererHandle h,const War3RenderSkinnedModelSceneV2* p) {
    auto& mock=*static_cast<MockBackend*>(h); ModelScene temp; std::uint64_t bytes=0;
    if (!CopySkinnedModelInput(*p,temp,bytes)) return 1;
    mock.pose=*temp.initialSkin; mock.model=std::move(temp); ++mock.staticUploads; ++mock.updates; return 0;
}
std::int32_t WAR3_RENDER_CALL Update(War3RendererHandle h,const War3RenderSkinUpdateV2* p) {
    auto& mock=*static_cast<MockBackend*>(h); SkinPose temp;
    if (!CopySkinUpdate(*p,mock.model,temp)) return 1;
    mock.pose=std::move(temp); ++mock.updates; return 0;
}
std::int32_t WAR3_RENDER_CALL StatusMock(War3RendererHandle,War3RenderSkinStatusV2*) { return 0; }
std::int32_t WAR3_RENDER_CALL CaptureMock(War3RendererHandle,void*,std::uint64_t,War3RenderSkinnedCapturedFrameV2*) { return 1; }
void Patch(std::vector<std::byte>& bytes,std::string_view tag,std::size_t offset,std::uint32_t value) {
    const auto at=std::string_view(reinterpret_cast<const char*>(bytes.data()),bytes.size()).find(tag);
    Require(at!=std::string_view::npos && at+offset+4<=bytes.size(),"patch target");
    std::memcpy(bytes.data()+at+offset,&value,4);
}
std::vector<std::byte> MaterialModel() {
    auto bytes=AnimationTest::Bytes(); MdxTest::Writer material;
    const auto start=material.Begin(); material.U32(0); material.U32(0); material.Tag("LAYS"); material.U32(2);
    for (unsigned layerIndex=0;layerIndex<2;++layerIndex) {
        const auto layer=material.Begin(); material.U32(2); material.U32(0); material.U32(0);
        material.U32(UINT32_MAX); material.U32(0); material.F32(layerIndex ? 0.6f : 0.2f);
        material.Tag("KMTA"); material.U32(4); material.U32(1); material.U32(UINT32_MAX);
        for (const auto frame:{0u,1000u,2000u,3000u}) {
            material.U32(frame); const bool end=frame==1000 || frame==3000;
            material.F32(layerIndex ? (end ? 1.0f : 0.0f) : (end ? 0.0f : 1.0f));
        }
        material.EndInclusive(layer);
    }
    material.EndInclusive(start);
    const auto at=std::string_view(reinterpret_cast<const char*>(bytes.data()),bytes.size()).find("MTLS");
    Require(at!=std::string_view::npos,"material fixture chunk"); std::uint32_t oldSize=0;
    std::memcpy(&oldSize,bytes.data()+at+4,4);
    bytes.erase(bytes.begin()+at+8,bytes.begin()+at+8+oldSize);
    bytes.insert(bytes.begin()+at+8,material.bytes.begin(),material.bytes.end());
    const auto size=static_cast<std::uint32_t>(material.bytes.size()); std::memcpy(bytes.data()+at+4,&size,4); return bytes;
}
void MaterialLayers() {
    Input input; input.bytes=MaterialModel();
    for (std::size_t i=0;i<input.specs.size();++i) { input.specs[i].sequenceIndex=0; input.specs[i].elapsedMs=i*500; }
    auto service=input.Build(); const auto scene=service->Scene(); auto first=Good(service->Prepare(nullptr));
    Require(scene->dynamicMaterialAlphas && !scene->dynamicColors && first->layerSlices.size()==2 &&
        first->layerSlices[0].count==4 && first->layerSlices[1].count==4,"draw-layer mapping used mesh count or lost mode");
    const auto a=first->layerSlices[0].first,b=first->layerSlices[1].first;
    Require(first->layerAlphas[a]==1.0f && first->layerAlphas[a+1]==0.0f && first->layerAlphas[b]==0.5f &&
        first->layerAlphas[b+1]==0.5f && first->alphas[1]==0.0f && first->poses[0].materialAlphaHash!=first->poses[1].materialAlphaHash,
        "independent layers/clocks mixed, alpha premultiplied static or GEOA overridden");
    const auto parsed=Good(ParseMdx(input.bytes)); const auto reference=Good(SampleMdxLayerAlphas(parsed,{0,0,500,0}));
    Require(reference.size()==2 && std::bit_cast<std::uint32_t>(reference[0])==std::bit_cast<std::uint32_t>(first->layerAlphas[b]) &&
        std::bit_cast<std::uint32_t>(reference[1])==std::bit_cast<std::uint32_t>(first->layerAlphas[b+1]),"reference layer bits differ");
    const ModelSceneRigidPoseRequest posed{0,500,0,{}};
    const auto referenceScene=Good(BuildWorldModelScene(std::span(&input.specs[1].model,1),
        [&](auto) { return Result<std::vector<std::byte>>::Success(input.bytes); },{}, {},posed));
    Require(referenceScene.layers.size()==2 && referenceScene.layers[0].alpha==0.5f && referenceScene.layers[1].alpha==0.5f,
        "static reference baked a different material clock");
    const auto descriptor=first->DescribeMaterial();
    Require(descriptor.base.base.structSize==sizeof(descriptor) && descriptor.base.colorFormat==0 && descriptor.base.colorCount==0 &&
        descriptor.base.colors==nullptr && descriptor.layerAlphas==first->layerAlphas.data() && descriptor.instanceSlices==first->layerSlices.data(),
        "material descriptor borrowed scratch or forced dynamic RGB");
    MockBackend backend; War3RenderPluginApiV2 api{}; api.structSize=sizeof(api);api.apiVersion=WAR3_RENDER_PLUGIN_API_V2;
    api.submitSkinnedModelScene=Submit;api.updateSkinMatrices=Update;api.getSkinStatus=StatusMock;api.captureSkinnedFrameRgba=CaptureMock;
    AnimationRenderer oldRenderer(&api,&backend);const auto unsupported=oldRenderer.Submit(scene,first);
    Require(!unsupported.Succeeded() && unsupported.diagnostics[0].code=="ANIMATION_MATERIAL_ALPHA_BACKEND_UNSUPPORTED" && !backend.staticUploads,
        "old plugin silently displayed static KMTA");
    api.getSkinStatus=[](War3RendererHandle,War3RenderSkinStatusV2* status)->std::int32_t {
        if (status->structSize>=sizeof(War3RenderSkinMaterialCapabilitiesV2))
            reinterpret_cast<War3RenderSkinMaterialCapabilitiesV2*>(status)->layerAlphaFormat=WAR3_RENDER_DYNAMIC_LAYER_ALPHA_V1;
        return 0;
    };
    AnimationRenderer renderer(&api,&backend);Require(renderer.Submit(scene,first).Succeeded(),"material initial descriptor rejected");
    AnimationAdvance change;AnimationEdit edit;edit.identity=input.specs[0].identity;edit.elapsedMs=250;change.edits={edit};
    const auto next=Good(service->Prepare(first.get(),change)),repeat=Good(service->Prepare(next.get()));
    Require(next->layerAlphas[a]==0.75f && next->layerAlphas[a+1]==0.25f && next->layerAlphas[b]==0.5f &&
        first->layerAlphas[a]==1.0f && next->layerAlphas==repeat->layerAlphas && next->poses[0].materialAlphaHash==repeat->poses[0].materialAlphaHash &&
        next->timings.outputMotionHash==repeat->timings.outputMotionHash,"owned material output mutated old frame or nondeterministic");
    Require(renderer.Update(next).Succeeded() && renderer.Update(repeat).Succeeded() && backend.staticUploads==1 &&
        backend.pose.dynamicMaterialAlphas && backend.pose.layerAlphas==repeat->layerAlphas && input.reads==1,
        "material update reuploaded assets or lost owned material channels");
    const auto receipt=AnimationReceiptJson(*scene,*next,{},"material",std::string(64,'a'));
    Require(receipt.find("materialAlphaHashFNV1a")!=std::string::npos && receipt.find("materialAlphaSlice")!=std::string::npos,
        "capture omitted material pose identity");
    AnimationLimits limits;limits.retainUnsupportedStatic=true;limits.maxActivePoseBytes=512;
    std::vector<AnimationInstanceSpec> many(256,input.specs[0]);
    for (std::size_t i=0;i<many.size();++i) {many[i].identity.value=i+1;many[i].hostKey=std::to_string(i);}
    limits.maxFrameBytes=many.size()*(sizeof(War3RenderSkinnedModelInstanceV2)+sizeof(AnimationPoseLink)+sizeof(AnimationInstanceState)+
        sizeof(AnimationPickBounds)+sizeof(War3RenderMaterialLayerSliceV2))+32768;
    const auto shared=Good(ModelAnimationService::Create({7,33,"shared-material"},many,[&](auto p,auto n){return input.Read(p,n);},limits));
    const auto stand=Good(shared->Prepare(nullptr));
    Require(stand->sharedPoseRanges && stand->budgetStaticInstances>0 && stand->layerSlices.size()==many.size(),"shared material fixture did not use Stand pool");
    const auto found=std::find_if(stand->poses.begin(),stand->poses.end(),[](const auto& pose){return pose.staticForBudget;});
    Require(found!=stand->poses.end() && stand->layerAlphas[found->materialAlphas.offset]==0.5f &&
        found->materialAlphaHash!=0 && found->staticElapsedMs==500,"budget reused a static/default layer instead of true Stand sample");
    auto ghost=input.specs[0];ghost.model.previewGhost=true;
    const auto ghostService=Good(ModelAnimationService::Create({7,33,"ghost"},std::span(&ghost,1),[&](auto p,auto n){return input.Read(p,n);}));
    const auto ghostFrame=Good(ghostService->Prepare(nullptr));
    Require(ghostFrame->layerAlphas.front()==0.45f,"ghost alpha scale lost or multiplied the static alpha again");
    Input plain;plain.specs[0].elapsedMs=0;const auto ordinary=plain.Build();const auto before=Good(ordinary->Prepare(nullptr));
    AnimationInstanceDelta delta;delta.session={7,34,"synthetic-source"};auto addition=input.specs[0];addition.identity={3,7};
    addition.hostKey="new-material";addition.model.modelPath="alpha.mdx";addition.elapsedMs=500;delta.additions={addition};
    const auto plan=Good(ordinary->PrepareInstanceCatalog(delta,[&](auto,auto){return Result<AnimationResource>::Success({input.bytes,"synthetic",std::string(64,'b')});}));
    const auto promoted=Good(ordinary->PrepareInstanceDelta(*before,*plan));
    Require(promoted.assetsChanged && promoted.service->Scene()->dynamicMaterialAlphas && promoted.frame->dynamicMaterialAlphas &&
        promoted.frame->layerSlices.size()==3 && promoted.frame->layerAlphas.front()==1.0f && promoted.frame->layerAlphas[1]==0.5f &&
        promoted.frame->states[0].elapsedMs==before->states[0].elapsedMs,"asset suffix did not atomically promote old static material values");
    delta={};delta.session={7,35,"synthetic-source"};delta.removals={addition.identity};
    const auto remove=Good(promoted.service->PrepareInstanceCatalog(delta,[&](auto p,auto n){return plain.Read(p,n);}));
    const auto removed=Good(promoted.service->PrepareInstanceDelta(*promoted.frame,*remove));
    Require(!removed.assetsChanged && removed.frame->dynamicMaterialAlphas && removed.frame->layerSlices.size()==2 &&
        removed.service->Scene()->vertices.data()==promoted.service->Scene()->vertices.data(),"undo removed material mode or reuploaded resident geometry");
    delta={};delta.session={7,36,"synthetic-source"};delta.removals={plain.specs[0].identity,plain.specs[1].identity};
    const auto emptyPlan=Good(removed.service->PrepareInstanceCatalog(delta,[&](auto p,auto n){return plain.Read(p,n);}));
    const auto empty=Good(removed.service->PrepareInstanceDelta(*removed.frame,*emptyPlan));
    const auto emptyDescriptor=empty.frame->DescribeMaterial();
    Require(empty.frame->instances.empty() && empty.frame->dynamicMaterialAlphas && emptyDescriptor.instanceSliceCount==0 &&
        emptyDescriptor.layerAlphaFormat==WAR3_RENDER_DYNAMIC_LAYER_ALPHA_V1,"empty roster discarded negotiated material mode");
    auto invalid=input.bytes;Patch(invalid,"KMTA",20,0x7FC00000u);AnimationLimits tolerant;tolerant.retainUnsupportedStatic=true;
    const auto invalidService=Good(ModelAnimationService::Create({7,33,"invalid-material"},input.specs,
        [&](auto,auto){return Result<AnimationResource>::Success({invalid,"synthetic",std::string(64,'c')});},tolerant));
    const auto hidden=Good(invalidService->Prepare(nullptr));
    Require(hidden->hiddenInstances==2 && hidden->poses[0].materialIssues.count==1 && hidden->poses[0].poseVersion==0 &&
        hidden->poses[0].materialAlphaHash==0 && hidden->materialIssues.front().source.materialIndex==0 &&
        hidden->materialIssues.front().source.layerIndex==0 && hidden->materialIssues.front().source.sourceByteOffset>0,
        "invalid material layer silently retained static or lost exact source");
    std::cout<<"material_alpha_independent_layers=1 owned_clock_identity=1 old_plugin_refused=1 same_static_upload=1 stand_budget=1 append_promote_undo=1\n";
}
void SingularScaleSegment(std::vector<std::byte>& bytes) {
    // 有限合法源数据：首段父缩放为零，子节点去除缩放时才发生局部求值失败。
    Patch(bytes,"KGTR",8,0); Patch(bytes,"KGTR",32,200); Patch(bytes,"KGTR",48,1000);
    Patch(bytes,"KGTR",20,0);
    for (const auto offset:{24u,28u,36u,40u,44u,52u,56u,60u,68u,72u,76u})
        Patch(bytes,"KGTR",offset,0x3F800000u);
    Patch(bytes,"BONE",100,2); Patch(bytes,"KGTR",0,0x4353474Bu);
}
void WorldIsolation() {
    Input in; SingularScaleSegment(in.bytes);
    std::vector<AnimationInstanceSpec> specs(65,in.specs[0]);
    for (std::size_t i=0;i<specs.size();++i) { specs[i].identity={i+1,7}; specs[i].elapsedMs=(i==0 || i==64)?100:500; }
    const auto read=[&](auto p,auto n){return in.Read(p,n);};
    auto strict=Good(ModelAnimationService::Create({7,33,"world"},specs,read));
    Require(!strict->Prepare(nullptr).Succeeded(),"strict sampling became tolerant");
    AnimationLimits limits; limits.retainUnsupportedStatic=true;
    auto service=Good(ModelAnimationService::Create({7,33,"world"},specs,read,limits));
    auto first=Good(service->Prepare(nullptr)); auto repeat=Good(service->Prepare(first.get()));
    Require(first->hiddenInstances==2 && first->animatedInstances==63 && first->retainedStaticInstances==0,"local failures stopped world");
    for (const auto i:{0u,64u}) {
        const auto& p=first->poses[i]; Require(p.hidden && !p.animated && p.poseVersion==0 && p.matrixHash==0 &&
            p.identity.value==i+1 && p.asset.value==1 && p.rejection.code==War3RenderCore::Code::PoseInheritanceSingular,"rejection identity lost");
        Require(std::all_of(first->alphas.begin()+p.firstAlpha,first->alphas.begin()+p.firstAlpha+p.alphaCount,[](float a){return a==0;}),"partial model hidden");
    }
    Require(first->poses[1].matrixHash==repeat->poses[1].matrixHash && first->groups.size()==repeat->groups.size(),"cross-batch pose changed");
    auto pick=AnimationPickingScene(*service->Scene(),*first);
    Require(pick->instances.size()==63 && pick->instanceRequests.front()==1 && pick->instanceRequests.back()==63,"hidden instance picked");
    MockBackend mock; War3RenderPluginApiV2 api{}; api.structSize=sizeof(api); api.apiVersion=WAR3_RENDER_PLUGIN_API_V2;
    api.submitSkinnedModelScene=Submit; api.updateSkinMatrices=Update; api.getSkinStatus=StatusMock; api.captureSkinnedFrameRgba=CaptureMock;
    AnimationRenderer renderer(&api,&mock); Require(renderer.Submit(service->Scene(),first).Succeeded(),"partial frame rejected by ABI");
    AnimationAdvance advance; AnimationEdit edit; edit.identity={1,7}; edit.elapsedMs=500; advance.edits.push_back(edit);
    auto recovered=Good(service->Prepare(first.get(),advance)); Require(renderer.Update(recovered).Succeeded(),"recovery upload failed");
    Require(!recovered->poses[0].hidden && recovered->poses[0].animated && recovered->hiddenInstances==1 && recovered->animatedInstances==64 &&
        recovered->poses[1].matrixHash==first->poses[1].matrixHash && mock.staticUploads==1 && mock.updates==2,"recovery rebuilt or mixed poses");
    const auto json=AnimationReceiptJson(*service->Scene(),*first,{},"isolated",std::string(64,'a'));
    Require(json.find("\"hiddenInstances\":2")!=std::string::npos && json.find("MDX_POSE_INHERITANCE_SINGULAR")!=std::string::npos &&
        json.find("\"animatedInstances\":63")!=std::string::npos,"receipt claimed hidden poses succeeded");
    advance.edits[0].identity.generation=8; Require(!service->Prepare(first.get(),advance).Succeeded(),"stale edit isolated");
    limits.maxFrameBytes=1; Require(!ModelAnimationService::Create({7,33,"world"},specs,read,limits).Succeeded(),"frame budget isolated");
    std::cout<<"world_isolation=65 hidden=2 cross_batch=1 recovery=1 strict_reject=1 static_uploads=1\n";
}
void AssetIsolation() {
    // 此处验证静态保留/局部GEOA/空模型；真实KMTF另由材质夹具覆盖。
    Input in; auto fixed=AnimationTest::WithoutSyntheticConstantKmtf(MdxTest::BuildTestMdx()), bad=fixed, empty=fixed;
    const auto remove=[](std::vector<std::byte>& bytes,std::string_view tag) {
        for (std::size_t at=4;at+8<=bytes.size();) { std::uint32_t size=0; std::memcpy(&size,bytes.data()+at+4,4);
            if (std::memcmp(bytes.data()+at,tag.data(),4)==0) { bytes.erase(bytes.begin()+at,bytes.begin()+at+8+size); return; } at+=8+size; }
    };
    remove(fixed,"SEQS"); Patch(fixed,"GEOA",16,2); Patch(fixed,"GEOA",24,std::bit_cast<std::uint32_t>(0.5f));
    Patch(fixed,"GEOA",28,std::bit_cast<std::uint32_t>(0.25f)); Patch(fixed,"KGAO",20,std::bit_cast<std::uint32_t>(1.0f));
    Patch(bad,"GEOA",16,1); remove(empty,"GEOS"); remove(empty,"GEOA");
    std::array<AnimationInstanceSpec,4> specs;
    const std::array paths{"same.mdx","static.mdx","bad.mdx","empty.mdx"};
    for (std::size_t i=0;i<specs.size();++i) { specs[i]=in.specs[0]; specs[i].identity={i+1,7}; specs[i].model.modelPath=paths[i]; specs[i].sequenceIndex.reset(); }
    const auto read=[&](std::string_view p,std::uint64_t n) { if (p=="same.mdx") return in.Read(p,n);
        if (p=="static.mdx" || p=="bad.mdx" || p=="empty.mdx") return Result<AnimationResource>::Success(
            {p=="static.mdx"?fixed:p=="bad.mdx"?bad:empty,"synthetic",std::string(64,'b')});
        return Result<AnimationResource>::Failure("Test","RESOURCE_NOT_FOUND","没有合成贴图"); };
    AnimationLimits limits; limits.retainUnsupportedStatic=true;
    auto service=Good(ModelAnimationService::Create({7,33,"assets"},specs,read,limits)); auto frame=Good(service->Prepare(nullptr));
    const auto scene=service->Scene();
    std::cout<<"asset_isolation_actual animated="<<frame->animatedInstances<<" retained_static="<<frame->retainedStaticInstances
        <<" skipped="<<scene->geometry.skippedInstances<<" unavailable="<<scene->unavailableInstances.size()<<'\n';
    for (const auto& unavailable:scene->unavailableInstances)
        std::cout<<"asset_isolation_unavailable id="<<unavailable.identity.value<<" code="<<unavailable.code<<'\n';
    Require(frame->animatedInstances==2 && frame->retainedStaticInstances==1 &&
        scene->geometry.skippedInstances==1 && scene->unavailableInstances.size()==1,"asset isolation counts");
    Require(scene->unavailableInstances[0].identity.value==4 && scene->unavailableInstances[0].code=="ANIMATION_EMPTY_MODEL","asset error lost");
    const auto& partialPose=frame->poses[2];
    Require(partialPose.identity.value==3 && partialPose.animated && !partialPose.hidden && partialPose.partial &&
        partialPose.geosetIssues.count==1 && frame->geosetIssues[partialPose.geosetIssues.offset].geosetIndex==1 &&
        frame->geosetIssues[partialPose.geosetIssues.offset].diagnostic.code==War3RenderCore::Code::GeoaUninterpreted &&
        frame->alphas[partialPose.firstAlpha]==1 && frame->alphas[partialPose.firstAlpha+1]==0,"local GEOA lost its cause or hid the good mesh");
    const auto& pose=frame->poses[1]; Require(!pose.animated && !pose.hidden && pose.asset.value==2,"static identity lost");
    // GEOA 的静态文件字段 +20/+24/+28 是 RGB；只有动态 KGAC 使用 BGR。
    const auto& color=frame->colors[pose.firstAlpha+1]; Require(color.rgb[0]==1.0f && color.rgb[1]==0.5f && color.rgb[2]==0.25f,"static geoset file RGB order lost");
    auto invalidStatic=specs[1]; invalidStatic.sequenceIndex=9999;
    const auto invalid=ModelAnimationService::Create({7,33,"assets"},std::span(&invalidStatic,1),read,limits);
    Require(!invalid.Succeeded() && invalid.diagnostics.front().code=="ANIMATION_SEQUENCE_INVALID","static retention swallowed invalid sequence input");
    Require(!ModelAnimationService::Create({7,33,"assets"},std::span(specs).subspan(2,1),read).Succeeded(),"strict unknown GEOA accepted");
    for (const auto code:{"WORLD_ASSET_SOURCE_CHANGED","WORLD_ANIMATION_RESOURCE_BUDGET"}) for (const bool texture:{false,true}) {
        const auto failed=ModelAnimationService::Create({7,33,"assets"},std::span(specs).first(1),[&](auto path,auto count) {
            if (texture && path=="same.mdx") return in.Read(path,count);
            return Result<AnimationResource>::Failure("Test",code,"不可降级的读取失败"); },limits);
        Require(!failed.Succeeded() && failed.diagnostics.front().code==code,"source/budget swallowed");
    }
    const auto json=AnimationReceiptJson(*scene,*frame,{},"assets",std::string(64,'a'));
    Require(json.find("\"unavailableInstances\":[{\"id\":\"4\"")!=std::string::npos &&
        json.find("\"geosetIndex\":1,\"code\":\"MDX_GEOA_UNINTERPRETED\"")!=std::string::npos,"asset and local GEOA receipt missing");
    // 原创1×1红色BLP1：156字节头+1024字节调色板+1字节索引，不给独立目标引入编码器。
    std::vector<std::byte> blp(1181); std::memcpy(blp.data(),"BLP1",4); blp[158]=std::byte{255};
    for (const auto offset:{4u,12u,16u,92u}) Patch(blp,"BLP1",offset,1);
    Patch(blp,"BLP1",28,1180);
    // 此局部门只验证解码像素/纹理数量预算；来源sampler的独立内存门由专用target覆盖。
    auto textureBudgetModel=in.bytes;
    Patch(textureBudgetModel,"TEXS",8+264,3); Patch(textureBudgetModel,"TEXS",8+268+264,3);
    const auto textureBudgetParsed=Good(ParseMdx(textureBudgetModel));
    Require(textureBudgetParsed.textures.size()==2 &&
        std::all_of(textureBudgetParsed.textures.begin(),textureBudgetParsed.textures.end(),[](const auto& texture) { return texture.flags==3; }),
        "pure decoded budget fixture must explicitly keep source legacy3 samplers");
    const auto textures=[&](std::string_view p,std::uint64_t n) {
        if (p=="same.mdx") {
            auto resource=in.Read(p,n);
            if (resource.Succeeded()) { resource.value->bytes=textureBudgetModel; resource.value->digest=Good(Sha256(textureBudgetModel)); }
            return resource;
        }
        return Result<AnimationResource>::Success({blp,"synthetic-blp","80A9D0025CFBB0DA12A0A026FBC73938B57DBF41D00190F8768201AB4C231D32"});
    };
    Require(Good(ModelAnimationService::Create({7,33,"texture-budget"},std::span(specs).first(1),textures,limits))->Scene()->geometry.textures.size()==2,
        "budget fixture did not decode real BLP textures");
    bool textureBudgetCasesAccepted=true;
    for (const bool count:{false,true}) {
        auto bounded=limits; if (count) bounded.geometry.maxTextures=0; else bounded.geometry.maxTotalTextureBytes=1;
        const auto rejected=ModelAnimationService::Create({7,33,"texture-budget"},std::span(specs).first(1),textures,bounded);
        std::cout<<"texture_budget_case count="<<count<<" max_textures="<<bounded.geometry.maxTextures
            <<" max_total_texture_bytes="<<bounded.geometry.maxTotalTextureBytes
            <<" succeeded="<<rejected.Succeeded()<<" value_present="<<rejected.value.has_value()<<'\n';
        for (const auto& diagnostic:rejected.diagnostics)
            std::cout<<"texture_budget_diagnostic count="<<count<<" code="<<diagnostic.code<<" message="<<diagnostic.message<<'\n';
        textureBudgetCasesAccepted &= !rejected.Succeeded() && rejected.diagnostics.front().code=="WORLD_MODEL_TEXTURE_BUDGET";
    }
    Require(textureBudgetCasesAccepted,"decoded texture budget became white fallback");
    std::cout<<"world_assets=4 animated=2 retained_static=1 unavailable=1 partial_geoset=1 original_rgb=1 source_budget_fail_closed=1\n";
}
std::vector<std::byte> ParticleBytes(bool pure,bool invalidSibling=false,bool invalidBindings=false) {
    const auto source=AnimationTest::Bytes(); MdxTest::Writer w; w.Tag("MDLX");
    const auto emitterCount=invalidBindings?3u:invalidSibling?2u:1u;
    const auto u32=[&](std::size_t p) { std::uint32_t n=0; std::memcpy(&n,source.data()+p,4); return n; };
    for(std::size_t p=4;p<source.size();) {
        const auto n=u32(p+4); const std::string_view tag(reinterpret_cast<const char*>(source.data()+p),4);
        if(tag=="PIVT") { w.Tag("PIVT"); w.U32(n+12*(emitterCount+(invalidBindings?1u:0u))); w.Raw(source.data()+p+8,n);
            for(std::uint32_t i=0;i<emitterCount+(invalidBindings?1u:0u);++i) { w.F32(3.0f+static_cast<float>(i)); w.F32(2); w.F32(1); } }
        else if(!pure || (tag!="GEOS" && tag!="GEOA")) w.Raw(source.data()+p,n+8);
        p+=n+8;
    }
    w.Tag("PRE2"); const auto chunk=w.Begin();
    for(std::uint32_t emitter=0;emitter<emitterCount;++emitter) {
        const auto object=w.Begin(),node=w.Begin();
        w.Fixed("Hosted emitter",80); w.U32(2+emitter); w.U32(0); w.U32(0x1000); w.EndInclusive(node);
        for(const float v:{0.0f,0.0f,0.0f,0.0f,0.2f,100.0f,0.0f,0.0f}) w.F32(v);
        w.U32(invalidSibling&&emitter?9u:1u); w.U32(1); w.U32(1); w.U32(0); w.F32(0); w.F32(0.5f);
        for(int i=0;i<9;++i) w.F32(1);
        const unsigned char alpha[]{255,255,255}; w.Raw(alpha,3); for(int i=0;i<3;++i) w.F32(1);
        for(int i=0;i<4;++i) { w.U32(0); w.U32(0); w.U32(1); }
        w.U32(invalidBindings&&emitter==1?99u:0u); w.U32(0); w.U32(0); w.U32(0);
        w.Tag("KP2V"); w.U32(3); w.U32(0); w.U32(UINT32_MAX);
        w.U32(0); w.F32(1); w.U32(600); w.F32(0); w.U32(1000); w.F32(0);
        w.EndInclusive(object);
    }
    w.EndChunk(chunk);
    if(invalidBindings) {
        w.Tag("RIBB"); const auto ribbon=w.Begin(),object=w.Begin(),node=w.Begin();
        w.Fixed("Material rejection",80); w.U32(5); w.U32(0); w.U32(0x4000); w.EndInclusive(node);
        for(const float v:{3.0f,2.0f,0.5f,0.2f,0.4f,0.8f,2.0f}) w.F32(v);
        w.U32(0); w.U32(20); w.U32(1); w.U32(1); w.U32(0); w.F32(1);
        w.EndInclusive(object); w.EndChunk(ribbon);
    }
    return std::move(w.bytes);
}
void ParticleService() {
    AnimationRibbonMaterialTest::Run();
    AnimationRibbonRendererTest::Run();
    AnimationParticleVisibilityPreviewTest::Run();
    Input input; const auto mixed=ParticleBytes(false),pure=ParticleBytes(true),partial=ParticleBytes(false,true),bindings=ParticleBytes(false,false,true);
    auto badFilter=mixed,history=mixed,geoset=mixed; Patch(badFilter,"PRE2",140,9); SingularScaleSegment(history);
    // 真实地图模型的PRE2分段时间可为1.2：粒子到不了中点，按前半段播放，不能整组跳过。
    auto lateMiddle=mixed; Patch(lateMiddle,"PRE2",160,std::bit_cast<std::uint32_t>(1.2f));
    Patch(geoset,"GEOA",16,2); Patch(geoset,"GEOA",20,std::bit_cast<std::uint32_t>(1.0f));
    Patch(geoset,"GEOA",24,std::bit_cast<std::uint32_t>(0.5f)); Patch(geoset,"GEOA",28,std::bit_cast<std::uint32_t>(0.25f));
    Patch(geoset,"KGAO",20,std::bit_cast<std::uint32_t>(1.0f));
    MdxTest::Writer badGeoset; badGeoset.Tag("GEOA"); const auto geoChunk=badGeoset.Begin(),geoRecord=badGeoset.Begin();
    badGeoset.F32(1); badGeoset.U32(1); for(int c=0;c<3;++c) badGeoset.F32(1); badGeoset.U32(0);
    badGeoset.EndInclusive(geoRecord); badGeoset.EndChunk(geoChunk); geoset.insert(geoset.end(),badGeoset.bytes.begin(),badGeoset.bytes.end());
    std::vector<std::byte> blp(1181); std::memcpy(blp.data(),"BLP1",4); blp[158]=std::byte{255};
    for(const auto offset:{4u,12u,16u,92u}) Patch(blp,"BLP1",offset,1); Patch(blp,"BLP1",28,1180);
    const auto read=[&](std::string_view path,std::uint64_t) { return Result<AnimationResource>::Success(
        {path=="same.mdx"?mixed:path=="pure.mdx"?pure:path=="filter.mdx"?badFilter:path=="middle.mdx"?lateMiddle:path=="history.mdx"?history:path=="partial.mdx"?partial:path=="binding.mdx"?bindings:path=="geoset.mdx"?geoset:blp,
            "original-fixture",std::string(64,'a')}); };
    for(auto& spec:input.specs) { spec.sequenceIndex=0; spec.elapsedMs=0; spec.globalMs=0; }
    AnimationLimits limits; limits.enableParticles=true;
    auto service=Good(ModelAnimationService::Create({7,33,"particles"},input.specs,read,limits));
    AnimationAdvance command; command.worldCamera=War3RenderCore::LocalCamera{{0,-100,100},{0,0,1}};
    auto first=Good(service->Prepare(nullptr,command)); Require(first->particleVertices.empty() && first->particleLinks.size()==2,"particle zero frame");
    AnimationEdit a; a.identity={1,7}; a.elapsedMs=150; a.globalMs=650;
    AnimationEdit b; b.identity={2,7}; b.elapsedMs=200; b.globalMs=700; command.edits={a,b};
    auto frame=Good(service->Prepare(first.get(),command)); auto repeated=Good(service->Prepare(frame.get()));
    Require(frame->particleLinks[0].simulated && frame->particleLinks[1].simulated && !frame->particleVertices.empty(),"particle host omitted instances");
    Require(frame->particleLinks[0].stateHash==repeated->particleLinks[0].stateHash &&
        frame->particleLinks[0].geometryHash==repeated->particleLinks[0].geometryHash,"paused particle state changed");
    Require(frame->timings.outputMotionHash==repeated->timings.outputMotionHash &&
        frame->timings.particleSimulationRequests==2 && frame->timings.particleAnchorRequests>0 &&
        frame->timings.particlesTotalNs>=frame->timings.particleSimulationNs+frame->timings.particleGeometryNs,
        "particle CPU timing omitted real work or treated paused numerical output as animation");
    a.elapsedMs=200; a.globalMs=700; command.edits={a}; auto next=Good(service->Prepare(frame.get(),command));
    Require(next->particleLinks[1].stateHash==frame->particleLinks[1].stateHash &&
        next->particleLinks[1].geometryHash==frame->particleLinks[1].geometryHash,"particle instance coupling");
    a.elapsedMs=150; a.globalMs=650; command.edits={a}; auto rewind=Good(service->Prepare(next.get(),command));
    Require(rewind->particleLinks[0].resetReason=="rewind" && rewind->particleLinks[0].stateHash==frame->particleLinks[0].stateHash,"replay changed particle state");
    a.globalMs=0; command.edits={a}; const auto bad=service->Prepare(frame.get(),command);
    Require(!bad.Succeeded() && bad.diagnostics.front().code=="PARTICLE_GLOBAL_REPLAY_UNSUPPORTED","negative global history manufactured");
    Require(Good(service->Prepare(frame.get()))->particleLinks[0].stateHash==frame->particleLinks[0].stateHash,"failed candidate changed accepted state");
    auto bounded=limits; bounded.retainUnsupportedStatic=true; bounded.particles.maxAnchorSamples=1;
    auto limited=Good(ModelAnimationService::Create({7,33,"budget"},std::span(input.specs).first(1),read,bounded));
    a.elapsedMs=20; a.globalMs=20; command.edits={a}; const auto exhausted=limited->Prepare(nullptr,command);
    Require(!exhausted.Succeeded() && exhausted.diagnostics.front().code=="PARTICLE_REPLAY_BUDGET","replay step budget was swallowed");
    std::cout<<"particle_replay_budget="<<exhausted.diagnostics.front().code<<'\n';
    bounded.particles.maxAnchorSamples=2; limited=Good(ModelAnimationService::Create({7,33,"budget"},input.specs,read,bounded));
    a.elapsedMs=b.elapsedMs=10; a.globalMs=b.globalMs=10; command.edits={a,b}; const auto sharedBudget=Good(limited->Prepare(nullptr,command));
    Require(sharedBudget->particleLinks.size()==2 && sharedBudget->particleLinks[0].simulated &&
        !sharedBudget->particleLinks[1].simulated && !sharedBudget->particleLinks[1].hidden &&
        sharedBudget->particleLinks[1].budgetLimited &&
        sharedBudget->particleLinks[1].rejectionCode=="ANIMATION_PARTICLE_ANCHOR_BUDGET" &&
        sharedBudget->budgetParticleInstances==1 && sharedBudget->hiddenInstances==0 &&
        std::none_of(sharedBudget->poses.begin(),sharedBudget->poses.end(),[](const auto& pose){ return pose.hidden; }),
        "preview particle budget was per instance, lost diagnostic or hid model geometry");
    bounded.retainUnsupportedStatic=false;
    const auto strictBudget=Good(ModelAnimationService::Create({7,33,"strict-budget"},input.specs,read,bounded))->Prepare(nullptr,command);
    Require(!strictBudget.Succeeded() && strictBudget.diagnostics.front().code=="ANIMATION_PARTICLE_ANCHOR_BUDGET",
        "strict shared anchor budget was relaxed");
    std::cout<<"particle_shared_anchor_budget=ANIMATION_PARTICLE_ANCHOR_BUDGET preview_keeps_models=1 strict_reject=1\n";
    auto spec=input.specs[0]; spec.model.modelPath="pure.mdx";
    auto only=Good(ModelAnimationService::Create({7,33,"pure"},std::span(&spec,1),read,limits)); command.edits.clear();
    auto zero=Good(only->Prepare(nullptr,command)); a.elapsedMs=150; a.globalMs=150; command.edits={a};
    auto visible=Good(only->Prepare(zero.get(),command));
    Require(only->Scene()->models.empty() && visible->instances.empty() && only->Scene()->particleInstanceCount==1 &&
        visible->particleLinks[0].particleCount>0,"pure particles require fake mesh");
    const auto& vertices=visible->particleVertices;
    Require(std::abs((vertices[0].position[0]+vertices[2].position[0])*0.5f-3.1f)<0.0001f,"parent chain/pivot transposed or doubled");
    AnimatedSceneDescriptor descriptor; descriptor.Refresh(*only->Scene(),*visible); ModelScene model; std::uint64_t bytes=0;
    Require(CopySkinnedModelInput(descriptor.scene,model,bytes),"empty model prefix refused");
    auto update=visible->Describe(); auto particles=visible->DescribeParticles(&update); ParticleFrameData packet;
    Require(CopyParticleInput(particles,static_cast<std::uint32_t>(only->Scene()->particleTextures.size()),packet),"host particle packet invalid");
    a.elapsedMs=800; a.globalMs=800; command.edits={a}; auto expired=Good(only->Prepare(visible.get(),command));
    Require(expired->particleVertices.empty() && expired->particleDraws.empty() && expired->particleLinks[0].simulated,"pure zero frame stale geometry");
    Require(!ModelAnimationService::Create({7,33,"disabled"},std::span(&spec,1),read).Succeeded(),"disabled particles faked empty success");
    auto filtered=input.specs; filtered[0].model.modelPath="filter.mdx";
    const auto strictFilter=ModelAnimationService::Create({7,33,"filter"},filtered,read,limits);
    Require(!strictFilter.Succeeded() && strictFilter.diagnostics.front().code=="PARTICLE_FILTER_UNSUPPORTED","strict invalid filter accepted");
    auto lateSpecs=input.specs; lateSpecs[0].model.modelPath="middle.mdx";
    auto late=Good(ModelAnimationService::Create({7,33,"middle"},lateSpecs,read,limits));
    AnimationEdit lateEdit; lateEdit.identity=lateSpecs[0].identity; lateEdit.elapsedMs=150; lateEdit.globalMs=650;
    AnimationAdvance lateCommand=command; lateCommand.edits={lateEdit};
    auto lateZero=Good(late->Prepare(nullptr,lateCommand)); auto lateFrame=Good(late->Prepare(lateZero.get(),lateCommand));
    Require(late->Scene()->particleCapabilities[0].available && !late->Scene()->particleCapabilities[0].skippedEmitters &&
        lateFrame->particleLinks[0].simulated && !lateFrame->particleLinks[0].partial,"PRE2 middle above 1 skipped emitter");
    auto tolerant=limits; tolerant.retainUnsupportedStatic=true;
    auto isolated=Good(ModelAnimationService::Create({7,33,"filter"},filtered,read,tolerant)); command.edits.clear();
    auto isolatedFrame=Good(isolated->Prepare(nullptr,command));
    Require(!isolatedFrame->particleLinks[0].hidden && isolatedFrame->particleLinks[0].partial &&
        !isolatedFrame->particleLinks[0].simulated && isolatedFrame->particleLinks[0].rejectionCode=="PARTICLE_FILTER_UNSUPPORTED" &&
        !isolated->Scene()->particleCapabilities[0].available && isolatedFrame->particleLinks[1].simulated &&
        isolatedFrame->animatedInstances==2,"all skipped emitters hid model geometry or lost rejection");
    auto shared=input.specs; for(auto& item:shared) item.model.modelPath="partial.mdx";
    const auto strictMixed=ModelAnimationService::Create({7,33,"partial"},shared,read,limits);
    Require(!strictMixed.Succeeded() && strictMixed.diagnostics.front().code=="PARTICLE_FILTER_UNSUPPORTED","strict mixed emitters accepted");
    auto surviving=Good(ModelAnimationService::Create({7,33,"partial"},shared,read,tolerant));
    const auto& capabilities=surviving->Scene()->particleCapabilities;
    const auto issues=capabilities[0].skippedEmitters;
    Require(capabilities[0].available && issues && issues==capabilities[1].skippedEmitters && issues->size()==1 &&
        issues->front().kind=="PRE2" && issues->front().sourceIndex==1 && issues->front().nodeId==3u &&
        issues->front().code=="PARTICLE_FILTER_UNSUPPORTED","partial emitter source identity or shared issue list lost");
    a.elapsedMs=b.elapsedMs=150; a.globalMs=b.globalMs=650; command.edits={a,b};
    auto valid=Good(surviving->Prepare(nullptr,command)),paused=Good(surviving->Prepare(valid.get()));
    Require(valid->animatedInstances==2 && !valid->instances.empty() && !valid->particleVertices.empty() &&
        valid->particleLinks[0].simulated && valid->particleLinks[0].partial && !valid->particleLinks[0].hidden &&
        valid->particleLinks[0].simulationTimeMs==150 && valid->states[0].globalMs==650 &&
        valid->particleLinks[0].stateHash==paused->particleLinks[0].stateHash,"valid emitter/frame clock was lost after sibling skip");
    auto diagnostics=Good(AnimationDiagnosticQuery::Create(surviving->Scene()));
    const auto source=Good(diagnostics->Source(0,shared[0].identity));
    Require(source.has_value() && !source->contentDigest.empty() &&
        !diagnostics->Source(0,shared[1].identity).Succeeded(),"source query lost identity or actual digest");
    auto binding=Good(diagnostics->Binding(0,shared[0].identity));
    auto pending=Good(diagnostics->Query(binding));
    Require(!pending.hasFrame && pending.partial && pending.source &&
        pending.issues.front().code=="ANIMATION_FRAME_UNAVAILABLE","pending diagnostics fabricated a frame/source");
    Require(!diagnostics->Binding(0,shared[1].identity).Succeeded() &&
        !diagnostics->SetFrame(isolatedFrame).Succeeded(),"wrong instance or scene accepted by diagnostic query");
    Require(diagnostics->SetFrame(valid).Succeeded(),"accepted service frame unavailable for diagnostics");
    binding=Good(diagnostics->Binding(0,shared[0].identity));
    const auto firstPage=Good(diagnostics->Query(binding,0,1)),lastPage=Good(diagnostics->Query(binding,1,1));
    Require(firstPage.hasFrame && firstPage.animated && !firstPage.hidden && firstPage.partial &&
        firstPage.particlesSimulated && firstPage.elapsedMs==150 && firstPage.globalMs==650 &&
        firstPage.total==2 && firstPage.truncated && !lastPage.truncated && lastPage.issues.size()==1 &&
        lastPage.issues[0].stage=="skipped-emitter" && lastPage.issues[0].kind=="PRE2" &&
        lastPage.issues[0].sourceIndex==1u && lastPage.issues[0].nodeId==3u &&
        lastPage.issues[0].code=="PARTICLE_FILTER_UNSUPPORTED","bounded instance diagnostics lost original emitter identity");
    auto wrongPose=binding; ++wrongPose.poseVersion;
    Require(!diagnostics->Query(wrongPose).Succeeded() && !diagnostics->Query(binding,0,65).Succeeded() &&
        !diagnostics->Query(binding,3,1).Succeeded(),"stale pose or unbounded diagnostic page accepted");
    Require(diagnostics->SetFrame(paused).Succeeded() && !diagnostics->Query(binding).Succeeded(),"old frame diagnostic binding survived replacement");
    for(auto& item:shared) item.model.modelPath="binding.mdx";
    const auto strictBindings=ModelAnimationService::Create({7,33,"bindings"},shared,read,limits);
    Require(!strictBindings.Succeeded() && strictBindings.diagnostics.front().code=="PARTICLE_TEXTURE_INVALID","strict binding failure changed");
    auto bindingService=Good(ModelAnimationService::Create({7,33,"bindings"},shared,read,tolerant));
    const auto bindingIssues=bindingService->Scene()->particleCapabilities[0].skippedEmitters;
    auto bindingFrame=Good(bindingService->Prepare(nullptr,command));
    Require(bindingIssues && bindingIssues==bindingService->Scene()->particleCapabilities[1].skippedEmitters && bindingIssues->size()==2 &&
        (*bindingIssues)[0].kind=="PRE2" && (*bindingIssues)[0].sourceIndex==1u && (*bindingIssues)[0].nodeId==3u &&
        (*bindingIssues)[0].code=="PARTICLE_TEXTURE_INVALID" && (*bindingIssues)[1].kind=="RIBB" &&
        (*bindingIssues)[1].sourceIndex==0u && (*bindingIssues)[1].nodeId==5u && (*bindingIssues)[1].code=="PARTICLE_MATERIAL_UNSUPPORTED" &&
        bindingFrame->animatedInstances==2 && bindingFrame->particleLinks[0].simulated && bindingFrame->particleLinks[0].partial &&
        !bindingFrame->particleLinks[0].hidden && !bindingFrame->particleVertices.empty(),"binding skip lost later emitters or model geometry");
    // 粒子贴图在所有来源都不存在：严格模式照旧拒绝；预览只停这个实例的粒子，模型和其他实例照常求值。
    const auto missingTexture=[&](std::string_view path,std::uint64_t budget) -> Result<AnimationResource> {
        if(path.ends_with(".mdx")) return read(path,budget);
        return Result<AnimationResource>::Failure("Resources","RESOURCE_NOT_FOUND","所有已注册来源均未找到资源："+std::string(path));
    };
    const auto strictMissing=ModelAnimationService::Create({7,33,"missing-texture"},input.specs,missingTexture,limits);
    Require(!strictMissing.Succeeded() && strictMissing.diagnostics.front().code=="RESOURCE_NOT_FOUND","strict missing particle texture accepted");
    auto missingService=Good(ModelAnimationService::Create({7,33,"missing-texture"},input.specs,missingTexture,tolerant));
    const auto& missingCapabilities=missingService->Scene()->particleCapabilities;
    a.elapsedMs=b.elapsedMs=150; a.globalMs=b.globalMs=650; command.edits={a,b};
    auto missingFrame=Good(missingService->Prepare(nullptr,command));
    Require(!missingCapabilities[0].available && missingCapabilities[0].rejectionCode=="PARTICLE_TEXTURE_MISSING" &&
        !missingCapabilities[1].available && missingCapabilities[1].rejectionCode=="PARTICLE_TEXTURE_MISSING" &&
        !missingFrame->instances.empty() && missingFrame->animatedInstances==2 &&
        !missingFrame->particleLinks[0].simulated && !missingFrame->particleLinks[0].hidden &&
        missingFrame->particleLinks[0].rejectionCode=="PARTICLE_TEXTURE_MISSING","missing particle texture failed whole scene or lost reason");
    std::cout<<"particle_texture_missing=PARTICLE_TEXTURE_MISSING preview_keeps_models=1 strict_reject=1\n";
    for(auto& item:shared) item.model.modelPath="geoset.mdx";
    const auto strictGeoset=ModelAnimationService::Create({7,33,"geosets"},shared,read,limits);
    Require(!strictGeoset.Succeeded() && strictGeoset.diagnostics.front().code=="MDX_GEOA_UNINTERPRETED","strict GEOA gate changed");
    auto geosetService=Good(ModelAnimationService::Create({7,33,"geosets"},shared,read,tolerant));
    const auto geoScene=geosetService->Scene(); auto geoFirst=Good(geosetService->Prepare(nullptr,command));
    a.elapsedMs=250; b.elapsedMs=400; a.globalMs=750; b.globalMs=900; command.edits={a,b};
    auto geoNext=Good(geosetService->Prepare(geoFirst.get(),command)),geoRepeat=Good(geosetService->Prepare(geoNext.get()));
    Require(geoFirst->geosetIssues.size()==2 && geoNext->animatedInstances==2 && !geoNext->particleVertices.empty() &&
        geoNext->poses[0].matrixHash!=geoFirst->poses[0].matrixHash && geoNext->poses[0].matrixHash!=geoNext->poses[1].matrixHash &&
        geoNext->poses[0].matrixHash==geoRepeat->poses[0].matrixHash,"GEOA isolation froze or coupled real bones/particles");
    for(std::size_t i=0;i<geoNext->poses.size();++i) {
        const auto& pose=geoNext->poses[i]; const auto& issue=geoNext->geosetIssues[pose.geosetIssues.offset];
        Require(pose.identity==shared[i].identity && pose.asset.value==1 && pose.asset.generation==7 && pose.evaluationEpoch &&
            pose.poseVersion && pose.partial && !pose.hidden && !pose.noVisibleGeometry && pose.geosetIssues.count==1 &&
            issue.geosetIndex==0 && issue.diagnostic.code==War3RenderCore::Code::GeoaUninterpreted &&
            geoNext->particleLinks[i].simulated && !geoNext->particleLinks[i].hidden,"GEOA source or valid anchor lost");
        const auto& geosetModel=geoScene->geometry.models[geoNext->instances[i].instance.model];
        for(std::uint32_t mesh=0;mesh<geosetModel.meshCount;++mesh) {
            const bool badMesh=geoScene->geometry.meshGeosets[geosetModel.firstMesh+mesh]==0;
            const auto at=pose.firstAlpha+mesh; const auto& rgb=geoNext->colors[at].rgb;
            Require(geoNext->alphas[at]==(badMesh?0.0f:1.0f) && rgb[0]==(badMesh?0.0f:1.0f) &&
                rgb[1]==(badMesh?0.0f:0.5f) && rgb[2]==(badMesh?0.0f:0.25f),"bad GEOA mesh or good original static RGB not mapped");
        }
    }
    auto geoQuery=Good(AnimationDiagnosticQuery::Create(geoScene)); Require(geoQuery->SetFrame(geoNext).Succeeded(),"GEOA query frame");
    const auto geoPage=Good(geoQuery->Query(Good(geoQuery->Binding(0,shared[0].identity)),0,1));
    Require(geoPage.partial && geoPage.total==1 && geoPage.issues[0].geosetIndex==0u && !geoPage.issues[0].sourceIndex &&
        !geoPage.issues[0].nodeId && geoPage.issues[0].code=="MDX_GEOA_UNINTERPRETED","GEOA index became node/emitter identity");
    MockBackend geoMock; War3RenderPluginApiV2 geoApi{}; geoApi.structSize=sizeof(geoApi); geoApi.apiVersion=WAR3_RENDER_PLUGIN_API_V2;
    geoApi.submitSkinnedModelScene=Submit; geoApi.updateSkinMatrices=Update; geoApi.captureSkinnedFrameRgba=CaptureMock;
    geoApi.getSkinStatus=[](War3RendererHandle,War3RenderSkinStatusV2* status)->std::int32_t {
        const auto capacity=status->structSize; *status={}; status->structSize=sizeof(*status);
        if(capacity>=sizeof(War3RenderSkinColorCapabilitiesV2))
            reinterpret_cast<War3RenderSkinColorCapabilitiesV2*>(status)->colorFormat=WAR3_RENDER_STATIC_MESH_RGB_V1;
        if(capacity>=sizeof(War3RenderSkinMaterialCapabilitiesV2))
            reinterpret_cast<War3RenderSkinMaterialCapabilitiesV2*>(status)->layerAlphaFormat=WAR3_RENDER_DYNAMIC_LAYER_ALPHA_V1;
        if(capacity>=sizeof(War3RenderSkinMaterialStateCapabilitiesV2))
            reinterpret_cast<War3RenderSkinMaterialStateCapabilitiesV2*>(status)->materialStateFormat=WAR3_RENDER_DYNAMIC_MATERIAL_STATE_V1;
        return 0; };
    geoApi.submitParticleModelScene=[](War3RendererHandle h,const War3RenderParticleSceneV2* packet)->std::int32_t { return Submit(h,packet->models); };
    geoApi.updateParticleModelFrame=[](War3RendererHandle h,const War3RenderParticleUpdateV2* packet)->std::int32_t { return Update(h,packet->modelPose); };
    geoApi.captureParticleFrameRgba=[](War3RendererHandle,void*,std::uint64_t,War3RenderParticleCapturedFrameV2*)->std::int32_t { return 1; };
    AnimationRenderer geoRenderer(&geoApi,&geoMock);
    std::cout<<"geoa_material_mode scene_states="<<geoScene->dynamicMaterialStates<<" scene_alphas="<<geoScene->dynamicMaterialAlphas
        <<" frame_states="<<geoFirst->dynamicMaterialStates<<" frame_alphas="<<geoFirst->dynamicMaterialAlphas<<'\n';
    const auto reportGeoStatus=[&](const char* label,const Status& status) {
        std::cout<<"geoa_backend_step step="<<label<<" attempted=1 succeeded="<<status.Succeeded()
            <<" static_uploads="<<geoMock.staticUploads<<" updates="<<geoMock.updates<<'\n';
        for (const auto& diagnostic:status.diagnostics)
            std::cout<<"geoa_backend_diagnostic step="<<label<<" code="<<diagnostic.code<<" message="<<diagnostic.message<<'\n';
    };
    const auto checkGeoCopiedMaterials=[&](const AnimationFrame& expectedFrame) {
        Require(geoMock.pose.dynamicMaterialStates && !geoMock.pose.materialStates.empty() &&
            geoMock.pose.materialStates.size()==expectedFrame.materialStates.size(),
            "GEOA backend declared material states without copying a real payload");
        bool copiedSourceClamp=false;
        for (std::size_t instance=0;instance<expectedFrame.instances.size();++instance) {
            const auto modelIndex=expectedFrame.instances[instance].instance.model;
            const auto& source=*geoScene->pickModels[modelIndex].source;
            const auto& geometry=geoScene->geometry;
            const auto model=geometry.models[modelIndex];
            for (std::uint32_t meshOffset=0;meshOffset<model.meshCount;++meshOffset) {
                const auto meshIndex=model.firstMesh+meshOffset;
                const auto& mesh=geometry.meshes[meshIndex];
                const auto& material=source.materials[source.geosets[geometry.meshGeosets[meshIndex]].materialId];
                for (std::uint32_t layer=0;layer<mesh.layerCount;++layer) {
                    const auto at=expectedFrame.poses[instance].materialStates.offset+mesh.firstLayer-geoScene->modelLayerRanges[modelIndex].offset+layer;
                    const auto& actual=geoMock.pose.materialStates.at(at);
                    const auto& expectedState=expectedFrame.materialStates.at(at);
                    const auto sourceFlags=source.textures[material.layers[layer].textureId].flags;
                    Require(actual.samplerFlags==sourceFlags && actual.textureIndex==expectedState.textureIndex &&
                        actual.textureIndex!=WAR3_RENDER_MODEL_NO_TEXTURE,
                        "GEOA shared Copy validators lost the actual resident layer source sampler");
                    copiedSourceClamp |= sourceFlags==0;
                }
            }
        }
        Require(copiedSourceClamp,"GEOA source clamp layer was removed instead of transported");
    };
    const auto geoSubmitted=geoRenderer.Submit(geoScene,geoFirst); reportGeoStatus("submit",geoSubmitted);
    bool geoNextAccepted=false,geoRepeatAccepted=false;
    if (geoSubmitted.Succeeded()) {
        checkGeoCopiedMaterials(*geoFirst);
        const auto updated=geoRenderer.Update(geoNext); reportGeoStatus("next",updated); geoNextAccepted=updated.Succeeded();
        if (geoNextAccepted) {
            checkGeoCopiedMaterials(*geoNext);
            const auto geoRepeatedStatus=geoRenderer.Update(geoRepeat); reportGeoStatus("repeat",geoRepeatedStatus); geoRepeatAccepted=geoRepeatedStatus.Succeeded();
            if (geoRepeatAccepted) checkGeoCopiedMaterials(*geoRepeat);
        } else std::cout<<"geoa_backend_step step=repeat attempted=0\n";
    } else std::cout<<"geoa_backend_step step=next attempted=0\ngeoa_backend_step step=repeat attempted=0\n";
    Require(geoSubmitted.Succeeded() && geoNextAccepted && geoRepeatAccepted && geoMock.staticUploads==1 && geoMock.updates==3,
        "GEOA partial rebuilt static assets");
    auto allGeosets=geoset; Patch(allGeosets,"GEOA",16,1);
    auto allService=Good(ModelAnimationService::Create({7,33,"all-geosets"},shared,[&](auto path,auto count) {
        return path=="geoset.mdx"?Result<AnimationResource>::Success({allGeosets,"original-fixture",std::string(64,'a')}):read(path,count); },tolerant));
    auto allFrame=Good(allService->Prepare(nullptr,command));
    Require(allFrame->poses[0].partial && allFrame->poses[0].noVisibleGeometry && !allFrame->poses[0].hidden &&
        allFrame->particleLinks[0].simulated && AnimationPickingScene(*allService->Scene(),*allFrame)->instances.size()==2,"all GEOA failure hid valid particles");
    a.elapsedMs=b.elapsedMs=800; a.globalMs=b.globalMs=1300; command.edits={a,b}; allFrame=Good(allService->Prepare(allFrame.get(),command));
    Require(allFrame->particleDraws.empty() && AnimationPickingScene(*allService->Scene(),*allFrame)->instances.empty(),"invisible geometry and absent particles stayed pickable");
    command.edits.clear();
    spec=input.specs[0]; spec.model.modelPath="history.mdx"; spec.elapsedMs=500; spec.globalMs=500;
    auto strictHistory=Good(ModelAnimationService::Create({7,33,"history"},std::span(&spec,1),read,limits));
    Require(!strictHistory->Prepare(nullptr,command).Succeeded(),"invalid historical anchor accepted");
    auto local=Good(ModelAnimationService::Create({7,33,"history"},std::span(&spec,1),read,tolerant));
    auto hidden=Good(local->Prepare(nullptr,command));
    Require(hidden->particleLinks[0].hidden && hidden->poses[0].hidden && hidden->poses[0].poseVersion==0 &&
        hidden->poses[0].rejectionCode=="MDX_POSE_INHERITANCE_SINGULAR" && AnimationPickingScene(*local->Scene(),*hidden)->instances.empty(),
        "particle historical failure remained visible/pickable or lost diagnostic");
    a.sequenceIndex=1; a.elapsedMs=20; a.globalMs=20; command.edits={a}; auto recovered=Good(local->Prepare(hidden.get(),command));
    Require(!recovered->poses[0].hidden && recovered->poses[0].poseVersion && recovered->particleLinks[0].simulated &&
        recovered->particleLinks[0].resetReason=="recover","particle hidden checkpoint poisoned recovery");
    std::cout<<"particle_service=two_instances explicit_clocks replay_cancel_anchor_budget=passed pure_zero_nonzero_zero=passed\n";
}
void Submission() {
    Input in; auto service=in.Build(); auto frame=Good(service->Prepare(nullptr)); MockBackend mock;
    War3RenderPluginApiV2 api{}; api.structSize=sizeof(api); api.apiVersion=WAR3_RENDER_PLUGIN_API_V2; api.submitSkinnedModelScene=Submit; api.updateSkinMatrices=Update;
    api.getSkinStatus=StatusMock; api.captureSkinnedFrameRgba=CaptureMock;
    AnimationRenderer renderer(&api,&mock); Require(renderer.Submit(service->Scene(),frame).Succeeded(),"submit bridge");
    for (unsigned i=0;i<30;++i) {
        auto next=Good(service->Prepare(frame.get())); Require(renderer.Update(next).Succeeded(),"update bridge"); frame=next;
    }
    Require(mock.staticUploads==1 && mock.updates==31 && in.reads==1,"static asset re-uploaded");
    Require(!renderer.Update(frame).Succeeded(),"stale version accepted");
    std::array<std::byte,4> pixels{}; Require(!renderer.Capture(pixels,0).Succeeded(),"fake GPU success");
    const auto planText=std::string("\xef\xbb\xbf")+
        "instance\t1\tsame.mdx\tStand\t100\t0\t0\t0\t0\t1\t1\t1\t0\n"
        "instance\t2\tsame.mdx\tWalk\t400\t200\t0\t0\t0\t1\t1\t1\t0\n"
        "capture\tfirst\ntime\t2\t433\ncapture\tstep\ncapture\trepeat\n";
    const auto plan=Good(ParseAnimationCapturePlan(planText));
    Require(plan.instances.size()==2 && plan.captures.size()==3 && plan.captures[1].edits.size()==1 && plan.captures[2].edits.empty(),"capture plan commands mixed");
    Require(!ParseAnimationCapturePlan(planText+"capture\t../escape\n").Succeeded(),"unsafe capture label");
    Require(!ParseAnimationCapturePlan(planText+"time\t999\t1\ncapture\tbad\n").Succeeded(),"unknown identity");
    Require(!ParseAnimationCapturePlan(planText+"time\t2\t500\n").Succeeded(),"unconsumed edit");
    Require(!ParseAnimationCapturePlan(planText+"capture\tfirst\n").Succeeded(),"duplicate output");
    const auto particlePlan=Good(ParseAnimationCapturePlan("particles\ttrue\n"+planText.substr(3)+"time\t1\t20\t40\ncapture\tclocks\n"));
    Require(particlePlan.particles && particlePlan.captures.back().edits[0].elapsedMs==20 && particlePlan.captures.back().edits[0].globalMs==40,"particle plan changed explicit clocks");
    Require(!Good(ParseAnimationCapturePlan("particles\tfalse\n"+planText.substr(3))).particles &&
        !ParseAnimationCapturePlan("particles\ttrue\nparticles\tfalse\n"+planText.substr(3)).Succeeded() &&
        !ParseAnimationCapturePlan(planText+"particles\ttrue\n").Succeeded(),"particle plan duplicate/late policy");
    std::cout<<"capture_plan=3 unsafe_label_unknown_id_duplicate_dangling_edit=rejected\n";
    std::cout<<"CPU_mock_static_submits="<<mock.staticUploads<<" matrix_submits="<<mock.updates<<" no_fake_capture=1 GPU_unverified=1\n";
}
}
int main(int argc,char** argv) try {
    const std::string which=argc==2?argv[1]:"";
    if(which=="instances") { Instances(); WorldIsolation(); ParticleService(); LargeScene(); VariantActions(); ViewAdmission(); MaterialLayers(); AnimationMaterialTextureTest::Run(); } else if(which=="geometry") { Geometry(); AssetIsolation(); RequestGeometryBudget(); ModelPreviewReplacementTest::Run(); }
    else if(which=="submission") Submission();
    else if(which=="particle-continuity") AnimationParticleContinuityTest::Run();
    else if(which=="instance-delta") AnimationInstanceDeltaTest::Run();
    else return 2;
    std::cout<<"PASS "<<which<<'\n'; return 0;
} catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
