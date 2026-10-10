#pragma once
#include "AnimationTestModel.h"
#include "../src/Render/Animation/AnimationParticles.h"
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace AnimationParticleContinuityTest {
using namespace War3Map;
using namespace War3Map::Render;
inline void Check(bool ok,const char* message) { if (!ok) throw std::runtime_error(message); }
template<class T> inline T Good(Result<T> result) {
    if (!result.Succeeded()) throw std::runtime_error(result.diagnostics.empty() ? "empty particle continuity failure" :
        result.diagnostics.front().code+": "+result.diagnostics.front().message);
    return std::move(*result.value);
}
inline std::vector<std::byte> Model(bool cameraDependent=false) {
    MdxTest::Writer writer;writer.bytes=AnimationTest::Bytes();
    const auto word=[&](std::size_t at) {
        std::uint32_t value=0;for (unsigned i=0;i<4;++i)
            value|=std::to_integer<std::uint32_t>(writer.bytes[at+i])<<(i*8);
        return value;
    };
    for (std::size_t at=4;at+8<=writer.bytes.size();) {
        const auto size=word(at+4);
        if (std::memcmp(writer.bytes.data()+at,"PIVT",4)==0) {
            for (unsigned i=0;i<4;++i) writer.bytes[at+4+i]=static_cast<std::byte>(((size+12)>>(i*8))&255u);
            writer.bytes.insert(writer.bytes.begin()+static_cast<std::ptrdiff_t>(at+8+size),12,std::byte{});break;
        }
        at+=8+size;
    }
    writer.Tag("PRE2");const auto chunk=writer.Begin(),object=writer.Begin(),node=writer.Begin();
    writer.Fixed("Continuity emitter",80);writer.U32(2);writer.U32(0);writer.U32(0x1000|(cameraDependent?8u:0u));writer.EndInclusive(node);
    for (const float value:{0.0f,0.0f,0.0f,0.0f,2.0f,100.0f,0.0f,0.0f}) writer.F32(value);
    writer.U32(1);writer.U32(1);writer.U32(1);writer.U32(0);writer.F32(0);writer.F32(0.5f);
    for (int i=0;i<9;++i) writer.F32(1);
    const unsigned char alpha[]{255,255,255};writer.Raw(alpha,3);for (int i=0;i<3;++i) writer.F32(1);
    for (int i=0;i<4;++i) { writer.U32(0);writer.U32(0);writer.U32(1); }
    writer.U32(0);writer.U32(0);writer.U32(0);writer.U32(0);
    writer.EndInclusive(object);writer.EndChunk(chunk);return std::move(writer.bytes);
}
inline std::vector<std::byte> Texture() {
    std::vector<std::byte> bytes(1181);std::memcpy(bytes.data(),"BLP1",4);bytes[158]=std::byte{255};
    const auto write=[&](std::size_t at,std::uint32_t value) {
        for (unsigned i=0;i<4;++i) bytes[at+i]=static_cast<std::byte>((value>>(i*8))&255u);
    };
    for (const auto offset:{4u,12u,16u,92u}) write(offset,1);write(28,1180);return bytes;
}
inline AnimationAdvance View(float targetX=1024) {
    AnimationAdvance advance;AnimationView view;
    view.camera={sizeof(War3RenderCameraV2),targetX,0,0,6000,1.57079632679f,1.3f};
    view.physicalWidth=4000;view.physicalHeight=1000;view.conservativeMargin=64;
    advance.worldView=view;advance.playAll=true;return advance;
}
inline void ExplicitPlaybackPolicy() {
    const auto model=Model(),texture=Texture();
    const auto read=[&](std::string_view path,std::uint64_t) {
        return Result<AnimationResource>::Success({path=="continuity.mdx" ? model : texture,
            "explicit-playback-fixture",std::string(64,'b')});
    };
    AnimationInstanceSpec spec; spec.identity={1,7};spec.hostKey="preview:1";spec.model.modelPath="continuity.mdx";
    spec.sequenceIndex=0;spec.elapsedMs=500;spec.globalMs=0;spec.playing=true;
    AnimationLimits limits;limits.enableParticles=true;limits.retainUnsupportedStatic=true;
    auto strict=Good(ModelAnimationService::Create({7,1,"explicit-policy"},std::span(&spec,1),read,limits));
    auto view=View(0);
    const auto rejected=strict->Prepare(nullptr,view);
    Check(!rejected.Succeeded() && rejected.diagnostics.front().code=="PARTICLE_GLOBAL_REPLAY_UNSUPPORTED",
        "visibility mask silently enabled approximate particle replay");
    limits.playbackPolicy=AnimationPlaybackPolicy::InteractivePreview;
    auto preview=Good(ModelAnimationService::Create({7,1,"explicit-policy"},std::span(&spec,1),read,limits));
    AnimationAdvance first; first.worldCamera=AnimationWorldCamera(view.worldView->camera);
    auto frame=Good(preview->Prepare(nullptr,first));
    Check(frame->requestVisibility.empty() && frame->animatedInstances==1 && !frame->poses[0].hidden &&
        frame->states[0].elapsedMs==500 && frame->states[0].globalMs==0 &&
        frame->particleLinks[0].simulated && frame->timings.particleAnchorRequests==1,
        "explicit unculled preview lost geometry, fabricated clocks or replayed history");
    first.deltaMs=33;
    frame=Good(preview->Prepare(frame.get(),first));
    Check(frame->particleLinks[0].resetReason.empty() && !frame->particleVertices.empty() &&
        frame->states[0].elapsedMs==533 && frame->states[0].globalMs==33,
        "interactive preview could not continue from its accepted first state");
    limits.playbackPolicy=static_cast<AnimationPlaybackPolicy>(77);
    Check(!ModelAnimationService::Create({7,1,"explicit-policy"},std::span(&spec,1),read,limits).Succeeded(),
        "unknown playback policy accepted");
}
inline void Run() {
    ExplicitPlaybackPolicy();
    const auto model=Model(),texture=Texture();
    const auto read=[&](std::string_view path,std::uint64_t) {
        return Result<AnimationResource>::Success({path=="continuity.mdx" ? model : texture,
            "original-continuity-fixture",std::string(64,'a')});
    };
    std::array<AnimationInstanceSpec,2> specs;
    for (std::size_t i=0;i<specs.size();++i) {
        specs[i].identity={2-i,7};specs[i].model.modelPath="continuity.mdx";
        specs[i].hostKey="continuity:"+std::to_string(i);
        specs[i].model.position={static_cast<float>(i)*2048,0,0};
        specs[i].sequenceIndex=0;specs[i].elapsedMs=60000;specs[i].globalMs=60000;specs[i].playing=true;
    }
    AnimationLimits limits;limits.enableParticles=true;limits.retainUnsupportedStatic=true;
    limits.playbackPolicy=AnimationPlaybackPolicy::InteractivePreview;
    limits.particles.maxAnchorSamples=9;
    auto service=Good(ModelAnimationService::Create({7,1,"continuity-fixture"},specs,read,limits));
    auto advance=View(-2048);auto frame=Good(service->Prepare(nullptr,advance));
    Check(frame->particleLinks.size()==2&&frame->timings.particleAnchorRequests==1&&frame->budgetParticleInstances==1&&
        frame->particleLinks[0].resetReason=="initial"&&frame->particleLinks[0].skippedTimelineMs==60000&&
        frame->particleVertices.empty(),"initial long-elapsed world state replayed or lost binding");
    const auto originalCheckpoint=frame->particleCheckpoint->instances[0];
    advance=View();
    advance.deltaMs=33;
    frame=Good(service->Prepare(frame.get(),advance));
    Check(frame->particleLinks[1].simulated&&frame->particleLinks[1].resetReason=="initial"&&
        frame->timings.particleAnchorRequests==1,"newly admitted world state warmed up after budget omission");
    frame=Good(service->Prepare(frame.get(),advance));
    for (unsigned iteration=0;iteration<120;++iteration) {
        advance.worldView->camera.targetX=1024+((iteration&1u) ? 0.25f : -0.25f);
        advance.deltaMs=(iteration&1u) ? 100 : 33;
        auto next=Good(service->Prepare(frame.get(),advance));
        Check(next->requestVisibility==std::vector<std::uint8_t>{1,1}&&next->budgetParticleInstances==1&&
            next->timings.particleSimulationRequests==1&&next->timings.particleAnchorRequests<=9&&
            !next->particleLinks[0].simulated&&next->particleLinks[0].frozen&&next->particleLinks[0].resetReason.empty()&&
            next->particleLinks[0].rejectionCode=="ANIMATION_PARTICLE_ANCHOR_BUDGET"&&
            next->particleLinks[1].simulated&&next->particleLinks[1].resetReason.empty()&&
            next->particleLinks[1].particleCount>0&&next->particleCheckpoint->instances[0]==originalCheckpoint,
            "world priority oscillated, lost frozen pointer, warmed up or reset selected particle state");
        Check(advance.deltaMs==33 || (next->particleLinks[1].skippedTimelineMs>0&&
            next->timings.particleSkippedTimelineMs==next->particleLinks[1].skippedTimelineMs),
            "bounded slow-frame simulation did not report skipped timeline");
        frame=std::move(next);
    }
    advance.deltaMs=0;
    for (const auto& state:frame->states) {
        AnimationEdit edit;edit.identity=state.identity;edit.elapsedMs=state.elapsedMs+5000;
        edit.globalMs=state.globalMs+5000;advance.edits.push_back(edit);
    }
    auto slow=Good(service->Prepare(frame.get(),advance));
    Check(slow->particleLinks[1].simulated&&!slow->particleLinks[0].simulated&&
        slow->timings.particleAnchorRequests==9&&slow->particleLinks[1].resetReason.empty()&&
        slow->particleLinks[1].skippedTimelineMs>=4900&&
        slow->timings.particleSkippedTimelineMs==slow->particleLinks[1].skippedTimelineMs&&
        slow->particleCheckpoint->instances[0]==originalCheckpoint,
        "five-second slow frame changed admission/reset state, exceeded fixed work or hid skipped time");
    frame=std::move(slow);advance.edits.clear();advance.deltaMs=33;
    const auto selectedCheckpoint=frame->particleCheckpoint->instances[1];
    advance.worldView->camera.targetX=-2048;
    auto switched=Good(service->Prepare(frame.get(),advance));
    Check(switched->requestVisibility==std::vector<std::uint8_t>{1,1}&&switched->particleLinks[0].simulated&&
        switched->particleLinks[0].resetReason.empty()&&switched->particleLinks[0].simulationTimeMs==switched->states[0].elapsedMs&&
        switched->particleLinks[0].particleCount==0&&switched->timings.particleAnchorRequests==1&&
        switched->particleLinks[1].frozen&&switched->particleCheckpoint->instances[1]==selectedCheckpoint&&
        switched->particleCheckpoint->instances[0]->state.emitters[0].random==originalCheckpoint->state.emitters[0].random,
        "budget-frozen resume recovered, warmed up, reset RNG or did not clear expired particles");
    frame=std::move(switched);
    advance.worldView->camera.targetX=1000000;advance.deltaMs=1000;
    for (unsigned iteration=0;iteration<3;++iteration) {
        auto next=Good(service->Prepare(frame.get(),advance));
        Check(next->requestVisibility==std::vector<std::uint8_t>{0,0}&&next->offscreenParticleInstances==2&&
            next->timings.particleSimulationRequests==0&&next->timings.particleAnchorRequests==0&&
            next->timings.particleAnchorNodeVisits==0&&next->budgetParticleInstances==0&&next->particleVertices.empty()&&
            next->particleDraws.empty()&&next->particleLinks[0].outsideView&&next->particleLinks[1].outsideView&&
            next->particleCheckpoint->instances==frame->particleCheckpoint->instances,
            "out-of-view particles consumed budget/work or copied/dropped checkpoints");
        frame=std::move(next);
    }
    advance=View();advance.deltaMs=33;
    auto resumed=Good(service->Prepare(frame.get(),advance));
    Check(resumed->particleLinks[1].simulated&&resumed->particleLinks[1].resetReason.empty()&&
        resumed->timings.particleAnchorRequests==1&&resumed->particleLinks[1].particleCount==0&&
        resumed->particleLinks[1].spawned==selectedCheckpoint->counts.spawned&&
        resumed->particleCheckpoint->instances[1]->state.emitters[0].random==selectedCheckpoint->state.emitters[0].random,
        "out-of-view resume recreated, warmed up, back-emitted or preserved expired particles");
    auto flowing=Good(service->Prepare(resumed.get(),advance));
    Check(flowing->particleLinks[1].simulated&&flowing->particleLinks[1].particleCount>0&&
        flowing->particleLinks[1].resetReason.empty(),"resumed particles did not continue normal emission");
    // 相机依赖节点与随机动作相位同时存在：相机只改变锚点/几何，不能重置已发粒子。
    const auto cameraModel=Model(true);
    const auto cameraRead=[&](std::string_view path,std::uint64_t) {
        return Result<AnimationResource>::Success({path=="continuity.mdx"?cameraModel:texture,
            "camera-continuity-fixture",std::string(64,'b')});
    };
    auto cameraSpec=specs[0];cameraSpec.globalMs=0;
    auto cameraService=Good(ModelAnimationService::Create({7,5,"camera-phase"},std::span(&cameraSpec,1),cameraRead,limits));
    auto cameraAdvance=View(0);auto cameraFrame=Good(cameraService->Prepare(nullptr,cameraAdvance));
    cameraAdvance.deltaMs=33;cameraFrame=Good(cameraService->Prepare(cameraFrame.get(),cameraAdvance));
    const auto counts=cameraFrame->particleLinks[0];
    Check(counts.particleCount>0&&cameraFrame->particleCheckpoint->instances[0]->state.globalStartMs==-60000,
        "world phase required negative historical replay or dropped current emission");
    cameraAdvance.deltaMs=0;cameraAdvance.worldView->camera.yawRadians+=0.5f;
    auto cameraChanged=Good(cameraService->Prepare(cameraFrame.get(),cameraAdvance));
    Check(cameraChanged->particleLinks[0].resetReason.empty()&&cameraChanged->particleLinks[0].stateHash==counts.stateHash&&
        cameraChanged->particleLinks[0].spawned==counts.spawned&&cameraChanged->particleLinks[0].geometryHash!=counts.geometryHash,
        "camera change reset world checkpoint/RNG or failed to update billboard geometry");
    cameraAdvance.deltaMs=33;auto cameraFlow=Good(cameraService->Prepare(cameraChanged.get(),cameraAdvance));
    Check(cameraFlow->particleLinks[0].resetReason.empty()&&cameraFlow->particleLinks[0].spawned>counts.spawned,
        "camera-dependent emitter did not continue current emission");
    // 几何不足也须在任何锚点工作前整实例省略，并复用上一状态。
    auto small=limits;small.particles.maxVertices=0;
    auto noGeometry=Good(ModelAnimationService::Create({7,2,"no-geometry"},specs,read,small));
    auto omitted=Good(noGeometry->Prepare(nullptr,View()));
    Check(omitted->timings.particleAnchorRequests==0&&omitted->timings.particleSimulationRequests==0&&
        omitted->budgetParticleInstances==2&&omitted->particleVertices.empty(),"geometry budget admitted partial particle work");
    // 旧全图16384发射器闸门必须移除；画外绑定只有身份存储，不瓜分活跃模拟池。
    constexpr std::size_t mapBindings=17000;
    std::vector<AnimationInstanceSpec> many(mapBindings,specs[0]);
    for (std::size_t i=0;i<many.size();++i) {
        many[i].identity.value=i+1;many[i].hostKey="continuity-map:"+std::to_string(i);
    }
    auto sparse=limits;sparse.maxActivePoseBytes=128*1024;
    auto large=Good(ModelAnimationService::Create({7,3,"large-particle-bindings"},many,read,sparse));
    Check(large->Scene()->particleInstanceCount==mapBindings&&
        std::all_of(large->Scene()->particleCapabilities.begin(),large->Scene()->particleCapabilities.end(),
            [](const auto& capability) { return capability.available; }),"map bindings hit arbitrary emitter gate");
    auto empty=Good(large->Prepare(nullptr,View(1000000)));
    Check(empty->offscreenParticleInstances==mapBindings&&empty->budgetParticleInstances==0&&
        empty->timings.particleAnchorRequests==0&&empty->particleCheckpoint->stateBytes==0,
        "outside map bindings allocated simulator states or spent active particle budget");
    for (const auto steps:{0u,129u}) {
        auto invalid=limits;invalid.particles.maxWorldFrameSteps=steps;
        const auto rejected=ModelAnimationService::Create({7,4,"invalid-world-steps"},specs,read,invalid);
        Check(!rejected.Succeeded()&&rejected.diagnostics.front().code=="ANIMATION_PARTICLE_BUDGET",
            "world frame step limit outside 1..128 accepted");
    }
    std::cout<<"world_particles_initial_no_warmup=1 stable_identity_bucket_alternating33_100_120frames=1 "
        "five_second_frame_bounded_reported=1 frozen_pointer_retained=1 budget_resume_one_anchor=1 "
        "outside_zero_work=1 expired_aged_without_rng_reset=1 map_bindings_17000=1\n";
}
} // namespace AnimationParticleContinuityTest
