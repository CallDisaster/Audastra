#pragma once
#include "War3MapCore/Render/AnimationRenderer.h"
#include "War3MapCore/Render/ModelSceneLayerRenderer.h"
#include "War3MapCore/RenderPluginMaterialState.h"
#include "AnimationParticleContinuityTests.h"
#include "../src/Render/Scene/ModelSkinInput.h"

namespace AnimationRibbonRendererTest {
using namespace War3Map;
using namespace War3Map::Render;
using AnimationParticleContinuityTest::Check;
struct Backend {
    bool supported=false, reject=false;
    std::vector<std::uint32_t> sizes, flags;
    std::uint64_t presentationRevision=0;
    War3RenderEditorPresentationTargetV2 target{sizeof(target),WAR3_EDITOR_PRESENTATION_SKINNED,0,0,1,0,0};
    void Receive(const War3RenderParticleUpdateV2* input, const War3RenderSkinUpdateV2* pose) {
        Check(input && pose && input->modelPose==pose && input->sceneVersion==pose->sceneVersion
            && input->contentVersion==pose->contentVersion,"particle route separated flags from the same pose");
        sizes.push_back(input->structSize); flags.assign(input->drawCount,0);
        if (supported) {
            Check(input->structSize==sizeof(War3RenderParticleMaterialUpdateV2),"capable route lost particle material tail");
            const auto& material=*reinterpret_cast<const War3RenderParticleMaterialUpdateV2*>(input);
            Check(material.materialFlagsFormat==WAR3_RENDER_PARTICLE_MATERIAL_FLAGS_V1
                && material.materialFlagsCount==input->drawCount
                && (input->drawCount ? material.materialFlags!=nullptr : material.materialFlags==nullptr),
                "particle route sent invalid flags shape");
            if (input->drawCount) flags.assign(material.materialFlags,material.materialFlags+input->drawCount);
            Check(flags==std::vector<std::uint32_t>({WAR3_PARTICLE_UNFOGGED,0}),"particle route ORed mixed ribbon flags");
        } else Check(input->structSize==sizeof(War3RenderParticleUpdateV2),"legacy provider received new particle input size");
    }
    void Advance(const War3RenderParticleUpdateV2& input, bool full) {
        if (full) ++target.modelEpoch;
        target.mode=WAR3_EDITOR_PRESENTATION_PARTICLES; target.sceneVersion=input.sceneVersion;
        target.contentVersion=input.contentVersion; target.instanceCount=input.modelPose->instanceCount;
        target.particleDrawCount=input.drawCount;
    }
};
inline War3RenderPluginApiV2 Api() {
    War3RenderPluginApiV2 api{}; api.structSize=sizeof(api); api.apiVersion=WAR3_RENDER_PLUGIN_API_V2;
    api.submitSkinnedModelScene=[](War3RendererHandle,const War3RenderSkinnedModelSceneV2*)->std::int32_t { return 0; };
    api.updateSkinMatrices=[](War3RendererHandle,const War3RenderSkinUpdateV2*)->std::int32_t { return 0; };
    api.captureSkinnedFrameRgba=[](War3RendererHandle,void*,std::uint64_t,War3RenderSkinnedCapturedFrameV2*)->std::int32_t { return 2; };
    api.captureParticleFrameRgba=[](War3RendererHandle,void*,std::uint64_t,War3RenderParticleCapturedFrameV2*)->std::int32_t { return 2; };
    api.getSkinStatus=[](War3RendererHandle h,War3RenderSkinStatusV2* status)->std::int32_t {
        if (static_cast<Backend*>(h)->supported && status->structSize>=sizeof(War3RenderParticleMaterialCapabilitiesV2)) {
            auto& caps=*reinterpret_cast<War3RenderParticleMaterialCapabilitiesV2*>(status);
            caps.particleMaterialFlagsFormat=WAR3_RENDER_PARTICLE_MATERIAL_FLAGS_V1;
            caps.particleMaterialFlagsMask=WAR3_PARTICLE_UNFOGGED;
        }
        return 0; // 旧provider保持未知尾部不写。
    };
    api.submitParticleModelScene=[](War3RendererHandle h,const War3RenderParticleSceneV2* scene)->std::int32_t {
        auto& backend=*static_cast<Backend*>(h); backend.Receive(scene->initialFrame,scene->models->initialPose);
        if (backend.reject) return 1;
        backend.Advance(*scene->initialFrame,true); backend.presentationRevision=0; return 0;
    };
    api.updateParticleModelFrame=[](War3RendererHandle h,const War3RenderParticleUpdateV2* frame)->std::int32_t {
        auto& backend=*static_cast<Backend*>(h); backend.Receive(frame,frame->modelPose);
        if (backend.reject) return 1;
        backend.Advance(*frame,false); return 0;
    };
    api.getEditorPresentationTarget=[](War3RendererHandle h,War3RenderEditorPresentationTargetV2* target)->std::int32_t {
        *target=static_cast<Backend*>(h)->target; return 0;
    };
    api.submitEditorVisibility=[](War3RendererHandle h,const War3RenderEditorVisibilityUpdateV2* input)->std::int32_t {
        auto& backend=*static_cast<Backend*>(h);
        if (input->particleFrame) backend.Receive(input->particleFrame,input->particleFrame->modelPose);
        if (backend.reject) return 1;
        if (input->particleFrame) backend.Advance(*input->particleFrame,false);
        backend.presentationRevision=input->presentationRevision; return 0;
    };
    api.captureEditorPresentationFrameRgba=[](War3RendererHandle,std::uint64_t,std::uint64_t,void*,std::uint64_t,
        War3RenderEditorCapturedFrameV2*)->std::int32_t { return 2; };
    api.submitEditorModelScene=[](War3RendererHandle h,const War3RenderEditorModelSceneV2* input)->std::int32_t {
        auto& backend=*static_cast<Backend*>(h); Check(input->particles && input->visibility,"presented initial particle route missing");
        backend.Receive(input->particles->initialFrame,input->particles->models->initialPose);
        if (backend.reject) return 1;
        backend.Advance(*input->particles->initialFrame,true); backend.presentationRevision=input->visibility->presentationRevision; return 0;
    };
    api.getEditorRosterCapabilities=[](War3RendererHandle,std::uint32_t* flags)->std::int32_t { *flags=WAR3_RENDER_ROSTER_APPEND_ASSETS; return 0; };
    api.getEditorInstanceRosterResult=[](War3RendererHandle h,std::uint64_t ticket,War3RenderInstanceRosterResultV2* result)->std::int32_t {
        Check(ticket==0,"route mock only supplies the current CPU roster target");
        auto& backend=*static_cast<Backend*>(h); *result={}; result->structSize=sizeof(*result);
        result->state=WAR3_RENDER_ROSTER_TARGET; result->target=backend.target;
        result->assetIdentity=1; result->layersRevision=1; result->presentationRevision=backend.presentationRevision; return 0;
    };
    api.submitEditorInstanceRoster=[](War3RendererHandle h,const War3RenderEditorInstanceRosterV2* input,std::uint64_t* ticket)->std::int32_t {
        auto& backend=*static_cast<Backend*>(h); backend.Receive(input->candidateParticles,input->candidatePose);
        if (input->flags&WAR3_RENDER_ROSTER_APPEND_ASSETS) {
            const auto& append=*reinterpret_cast<const War3RenderEditorAssetRosterV2*>(input);
            Check(input->structSize==sizeof(append) && append.assets,"asset append route lost its extended transaction");
        }
        if (backend.reject) return 1;
        *ticket=17; return 0;
    };
    api.getModelLayersTarget=[](War3RendererHandle h,War3RenderModelLayersTargetV2* target)->std::int32_t {
        *target={sizeof(*target),0,static_cast<Backend*>(h)->target.modelEpoch,1}; return 0;
    };
    api.submitModelSceneLayer=[](War3RendererHandle h,const War3RenderModelSceneLayerV2* input)->std::int32_t {
        auto& backend=*static_cast<Backend*>(h); Check(input->particles,"attached particle scene missing");
        backend.Receive(input->particles->initialFrame,input->particles->models->initialPose); return backend.reject ? 1 : 0;
    };
    api.updateModelSceneLayer=[](War3RendererHandle h,const War3RenderModelLayerUpdateV2* input)->std::int32_t {
        auto& backend=*static_cast<Backend*>(h); Check(input->particles,"attached particle update missing");
        backend.Receive(input->particles,input->particles->modelPose); return backend.reject ? 1 : 0;
    };
    api.removeModelSceneLayer=[](War3RendererHandle,std::uint64_t,std::uint64_t,std::uint64_t)->std::int32_t { return 0; };
    api.captureLayeredFrameRgba=[](War3RendererHandle,std::uint64_t,void*,std::uint64_t,War3RenderLayeredCapturedFrameV2*)->std::int32_t { return 2; };
    return api;
}
inline void Warning(const Status& status, bool supported) {
    Check(status.Succeeded(),"CPU route unexpectedly rejected a valid frame");
    const auto count=std::count_if(status.diagnostics.begin(),status.diagnostics.end(),[](const auto& issue) {
        return issue.code=="PARTICLE_UNFOGGED_PREVIEW" && issue.severity==DiagnosticSeverity::Warning;
    });
    Check(count==(supported ? 0 : 1),"old-provider ribbon approximation warning lost or reported for supported input");
}

inline void BakedSamplerLayer() {
    // 原创baked-only non3层，走真实CPU输入/更新验证，不让mock凭返回0掩盖描述符尾部丢失。
    BakedModelMesh baked; baked.texturePath="layer-sampler.tga"; baked.samplerFlags=0;
    baked.vertices={{0,0,0,0,0,1,-.25f,-.25f},{10,0,0,0,0,1,1.25f,-.25f},{0,10,0,0,0,1,-.25f,1.25f}};
    baked.indices={0,1,2};
    const auto image=AnimationParticleContinuityTest::Texture();
    const auto service=AnimationParticleContinuityTest::Good(ModelAnimationService::Create({7,50,"layer-sampler"},{},
        [&](std::string_view path,std::uint64_t) {
            Check(path=="layer-sampler.tga","baked layer unexpectedly requested a synthetic MDX");
            return Result<AnimationResource>::Success({image,"original-layer-sampler",std::string(64,'a')});
        },{},std::span(&baked,1)));
    const auto scene=service->Scene(); const auto first=AnimationParticleContinuityTest::Good(service->Prepare(nullptr));
    const auto second=AnimationParticleContinuityTest::Good(service->Prepare(first.get()));
    const auto third=AnimationParticleContinuityTest::Good(service->Prepare(second.get()));
    Check(scene->dynamicMaterialStates && first->materialStates.size()==1 && first->materialStates[0].samplerFlags==0,
        "layer regression fixture must contain an actual nonlegacy material state");
    struct MaterialBackend {
        bool alpha=true, state=false, reject=false;
        unsigned submitCalls=0, updateCalls=0, uploads=0, updates=0;
        ModelScene model;
        SkinPose pose;
    } backend;
    auto api=Api();
    api.getSkinStatus=[](War3RendererHandle handle,War3RenderSkinStatusV2* output)->std::int32_t {
        const auto& state=*static_cast<MaterialBackend*>(handle); const auto capacity=output->structSize;
        *output={}; output->structSize=sizeof(*output);
        if (capacity>=sizeof(War3RenderSkinMaterialCapabilitiesV2) && state.alpha)
            reinterpret_cast<War3RenderSkinMaterialCapabilitiesV2*>(output)->layerAlphaFormat=WAR3_RENDER_DYNAMIC_LAYER_ALPHA_V1;
        if (capacity>=sizeof(War3RenderSkinMaterialStateCapabilitiesV2) && state.state)
            reinterpret_cast<War3RenderSkinMaterialStateCapabilitiesV2*>(output)->materialStateFormat=WAR3_RENDER_DYNAMIC_MATERIAL_STATE_V1;
        return 0;
    };
    api.getModelLayersTarget=[](War3RendererHandle,War3RenderModelLayersTargetV2* output)->std::int32_t {
        *output={sizeof(*output),0,7,1}; return 0;
    };
    api.submitModelSceneLayer=[](War3RendererHandle handle,const War3RenderModelSceneLayerV2* input)->std::int32_t {
        auto& state=*static_cast<MaterialBackend*>(handle); ++state.submitCalls;
        ModelScene candidate; std::uint64_t bytes=0;
        if (!input->models || input->particles || !CopySkinnedModelInput(*input->models,candidate,bytes) || state.reject) return 1;
        state.pose=*candidate.initialSkin; state.model=std::move(candidate); ++state.uploads; return 0;
    };
    api.updateModelSceneLayer=[](War3RendererHandle handle,const War3RenderModelLayerUpdateV2* input)->std::int32_t {
        auto& state=*static_cast<MaterialBackend*>(handle); ++state.updateCalls;
        SkinPose candidate;
        if (!input->pose || input->particles || !CopySkinUpdate(*input->pose,state.model,candidate) || state.reject) return 1;
        state.pose=std::move(candidate); ++state.updates; return 0;
    };
    ModelSceneLayerRenderer layer(&api,&backend,9);
    const auto noState=layer.Submit(scene,first);
    Check(!noState.Succeeded() && !noState.diagnostics.empty() &&
        noState.diagnostics.front().code=="ANIMATION_MATERIAL_STATE_BACKEND_UNSUPPORTED" && backend.submitCalls==0 &&
        !layer.Target() && layer.Revision()==0,"non3 layer bypassed its material-state capability gate");
    backend.state=true; backend.alpha=false;
    const auto noAlpha=layer.Submit(scene,first);
    Check(!noAlpha.Succeeded() && !noAlpha.diagnostics.empty() &&
        noAlpha.diagnostics.front().code=="ANIMATION_MATERIAL_ALPHA_BACKEND_UNSUPPORTED" && backend.submitCalls==0,
        "material-state capability must not imply the required absolute alpha capability");
    backend.alpha=true;
    Check(layer.Submit(scene,first).Succeeded() && backend.uploads==1 && layer.Target()==first && layer.Revision()==1 &&
        backend.pose.dynamicMaterialStates && backend.pose.materialStates[0].samplerFlags==0,
        "capable layer submission lost the real sampler through the shared CPU validator");
    backend.state=false;
    const auto rejected=layer.Update(second);
    Check(!rejected.Succeeded() && !rejected.diagnostics.empty() &&
        rejected.diagnostics.front().code=="ANIMATION_MATERIAL_STATE_BACKEND_UNSUPPORTED" && backend.updateCalls==0 &&
        layer.Target()==first && layer.Revision()==1,"layer update omitted capability negotiation or replaced the accepted target");
    backend.state=true;
    auto malformed=std::make_shared<AnimationFrame>(*second); malformed->dynamicMaterialStates=false;
    Check(!layer.Update(malformed).Succeeded() && backend.updateCalls==0 && layer.Target()==first && layer.Revision()==1,
        "bad owned material mode reached the backend or advanced the layer revision");
    malformed=std::make_shared<AnimationFrame>(*second); malformed->materialStates[0].samplerFlags=4;
    Check(!layer.Update(malformed).Succeeded() && backend.updates==0 && layer.Target()==first && layer.Revision()==1,
        "shared CPU update validation accepted unknown sampler flags or lost the old frame");
    Check(layer.Update(second).Succeeded() && backend.updates==1 && layer.Target()==second && layer.Revision()==2 &&
        backend.pose.materialStates.size()==1 && backend.pose.materialStates[0].samplerFlags==0 &&
        backend.pose.contentVersion==second->contentVersion && first->materialStates[0].samplerFlags==0,
        "non3 layer update lost DescribeMaterialState, copied old contents, or mutated its previous frame");
    backend.reject=true;
    Check(!layer.Update(third).Succeeded() && backend.updates==1 && layer.Target()==second && layer.Revision()==2 &&
        backend.pose.contentVersion==second->contentVersion,"backend refusal changed the accepted layer revision or material owner");
    std::cout<<"non3_baked_layer_cpu_submit_update=1 alpha_state_capability_gates=1 rejected_target_preserved=1\n";
}

inline void Run() {
    BakedSamplerLayer();
    auto scene=std::make_shared<AnimatedModelScene>(); scene->sceneVersion=7; scene->generation=2; scene->documentRevision=3;
    scene->particlesEnabled=true; scene->particleInstanceCount=1; scene->particleUnfogged=true;
    AnimationParticleCapability capability; capability.available=true; capability.ribbonCount=2;
    scene->particleCapabilities.push_back(capability);
    auto first=std::make_shared<AnimationFrame>(); first->sceneVersion=7; first->contentVersion=1; first->documentRevision=3;
    first->particleVertices.resize(12); first->particleDraws={{0,6,0,2},{6,6,0,2}};
    first->particleMaterialFlags={WAR3_PARTICLE_UNFOGGED,0};
    auto second=std::make_shared<AnimationFrame>(*first); second->contentVersion=2;
    auto third=std::make_shared<AnimationFrame>(*second); third->contentVersion=3;
    const std::array<std::uint8_t,2> drawMask{1,1};
    EditorVisibilityMasks masks{{},drawMask,1};
    for (const bool supported:{false,true}) {
        Backend backend; backend.supported=supported; auto api=Api(); AnimationRenderer renderer(&api,&backend);
        Warning(renderer.Submit(scene,first),supported); Warning(renderer.Update(second),supported);
        masks.presentationRevision=1; Warning(renderer.Update(third,&masks),supported);
        Check(backend.sizes.size()==3,"initial/update/visibility routes were not all visited");
        auto invalid=std::make_shared<AnimationFrame>(*third); invalid->contentVersion=4; invalid->particleMaterialFlags.pop_back();
        Check(!renderer.Update(invalid).Succeeded() && renderer.Target()==third && backend.sizes.size()==3,
            "host accepted malformed owned flags or changed the old frame");
        invalid->particleMaterialFlags={2,0};
        Check(!renderer.Update(invalid).Succeeded() && renderer.Target()==third && backend.sizes.size()==3,
            "host accepted unknown particle material flags");
        auto fourth=std::make_shared<AnimationFrame>(*third); fourth->contentVersion=4; backend.reject=true;
        Check(!renderer.Update(fourth).Succeeded() && renderer.Target()==third,"rejected particle update replaced owned target");
        backend.reject=false;
        ModelSceneLayerRenderer layer(&api,&backend,4);
        Warning(layer.Submit(scene,first),supported); Warning(layer.Update(second),supported);
        Check(layer.Target()==second,"attached layer failed to retain its same-frame flags owner");
        for (const bool append:{false,true}) {
            Backend rosterBackend; rosterBackend.supported=supported; AnimationRenderer roster(&api,&rosterBackend);
            masks.presentationRevision=1; Warning(roster.Submit(scene,first,&masks),supported);
            auto candidate=std::make_shared<AnimatedModelScene>(*scene); candidate->sceneVersion=8;
            auto candidateFrame=std::make_shared<AnimationFrame>(*first); candidateFrame->sceneVersion=8;
            masks.presentationRevision=2;
            const auto queued=roster.QueueRoster(candidate,candidateFrame,masks,append);
            Check(queued.Succeeded() && *queued.value==17 && roster.Target()==first && rosterBackend.sizes.size()==2,
                "roster/append did not send flags with candidate pose or published before completion");
            Warning({queued.diagnostics},supported);
        }
    }
    std::cout<<"ribbon_host_routes=initial,update,visibility,roster,append,layer legacy_and_material=1 rejected_target_retained=1 CPU_mock_only=1\n";
}
} // namespace AnimationRibbonRendererTest
