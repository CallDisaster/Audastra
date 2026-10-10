#pragma once
#include "AnimationTestModel.h"
#include "AnimationParticleContinuityTests.h"
#include "GeosetChannelFixture.h"
#include "War3MapCore/Render/AnimationRenderer.h"
#include "War3MapCore/Render/ModelAnimationCapture.h"
#include "War3MapCore/RenderPluginMaterialState.h"
#include "../src/Render/Scene/ModelSkinInput.h"
#include <cmath>
#include <iostream>
#include <map>

namespace AnimationMaterialTextureTest {
using namespace War3Map;
using namespace War3Map::Render;
using AnimationParticleContinuityTest::Check;
using AnimationParticleContinuityTest::Good;
inline std::vector<std::byte> Model(std::uint32_t mode=0,bool combinedChannels=false) {
    auto bytes=combinedChannels ? GeosetChannelFixture::Bytes(1,false) : AnimationTest::Bytes();
    const auto replace=[&](const char* tag,const std::vector<std::byte>& content) {
        for (std::size_t at=4;at+8<=bytes.size();) {
            std::uint32_t size=0; std::memcpy(&size,bytes.data()+at+4,4);
            if (!std::memcmp(bytes.data()+at,tag,4)) {
                bytes.erase(bytes.begin()+at+8,bytes.begin()+at+8+size);
                bytes.insert(bytes.begin()+at+8,content.begin(),content.end());
                size=static_cast<std::uint32_t>(content.size()); std::memcpy(bytes.data()+at+4,&size,4); return;
            }
            at+=size+8;
        }
        throw std::runtime_error("original KMTF fixture chunk missing");
    };
    MdxTest::Writer textures;
    for (unsigned i=0;i<2;++i) { textures.U32(0); textures.Fixed(i ? "second.tga" : "first.tga",260); textures.U32(i ? 2 : 1); }
    replace("TEXS",textures.bytes);
    MdxTest::Writer material;
    const auto begin=material.Begin(); material.U32(0); material.U32(0); material.Tag("LAYS"); material.U32(1);
    const auto layer=material.Begin(); material.U32(2); material.U32(0); material.U32(0);
    material.U32(UINT32_MAX); material.U32(0); material.F32(0.75f);
    material.Tag("KMTF"); material.U32(4); material.U32(mode); material.U32(UINT32_MAX);
    for (const auto frame:{0u,500u,2000u,2500u}) {
        material.U32(frame); material.U32(frame==500 || frame==2500 ? 1 : 0);
        if (mode>1) { material.U32(0); material.U32(0); } // 带切线的插值编号按原格式写入切线
    }
    if (combinedChannels) {
        material.Tag("KMTA"); material.U32(2); material.U32(1); material.U32(UINT32_MAX);
        material.U32(0); material.F32(0.25f); material.U32(1000); material.F32(0.75f);
    }
    material.EndInclusive(layer); material.EndInclusive(begin); replace("MTLS",material.bytes); return bytes;
}
struct CapabilityBackend {
    bool dynamicRgb=true,materialAlpha=false,materialState=false;
    unsigned uploads=0,updates=0;
    ModelScene scene;
    SkinPose pose;
};
inline std::int32_t WAR3_RENDER_CALL CapabilityStatus(War3RendererHandle handle,War3RenderSkinStatusV2* status) {
    const auto& backend=*static_cast<CapabilityBackend*>(handle); const auto capacity=status->structSize;
    *status={}; status->structSize=sizeof(*status);
    if (capacity>=sizeof(War3RenderSkinColorCapabilitiesV2))
        reinterpret_cast<War3RenderSkinColorCapabilitiesV2*>(status)->colorFormat=WAR3_RENDER_STATIC_MESH_RGB_V1;
    if (capacity>=sizeof(War3RenderSkinDynamicColorCapabilitiesV2) && backend.dynamicRgb)
        reinterpret_cast<War3RenderSkinDynamicColorCapabilitiesV2*>(status)->dynamicColorFormat=WAR3_RENDER_DYNAMIC_MESH_RGB_V1;
    if (capacity>=sizeof(War3RenderSkinMaterialCapabilitiesV2) && backend.materialAlpha)
        reinterpret_cast<War3RenderSkinMaterialCapabilitiesV2*>(status)->layerAlphaFormat=WAR3_RENDER_DYNAMIC_LAYER_ALPHA_V1;
    if (capacity>=sizeof(War3RenderSkinMaterialStateCapabilitiesV2) && backend.materialState)
        reinterpret_cast<War3RenderSkinMaterialStateCapabilitiesV2*>(status)->materialStateFormat=WAR3_RENDER_DYNAMIC_MATERIAL_STATE_V1;
    return 0;
}
inline std::int32_t WAR3_RENDER_CALL CapabilitySubmit(War3RendererHandle handle,const War3RenderSkinnedModelSceneV2* input) {
    auto& backend=*static_cast<CapabilityBackend*>(handle); ModelScene scene; std::uint64_t bytes=0;
    if (!CopySkinnedModelInput(*input,scene,bytes)) return -1;
    backend.pose=*scene.initialSkin; backend.scene=std::move(scene); ++backend.uploads; return 0;
}
inline std::int32_t WAR3_RENDER_CALL CapabilityUpdate(War3RendererHandle handle,const War3RenderSkinUpdateV2* input) {
    auto& backend=*static_cast<CapabilityBackend*>(handle); SkinPose pose;
    if (!CopySkinUpdate(*input,backend.scene,pose)) return -1;
    backend.pose=std::move(pose); ++backend.updates; return 0;
}
inline void CapabilityMatrix() {
    // 输入真正含KGAC、KMTA和变化KMTF；桩走共用descriptor深拷贝校验，不能只报能力并假接受。
    const auto model=Model(0,true),image=AnimationParticleContinuityTest::Texture();
    std::array<AnimationInstanceSpec,2> specs;
    for (std::size_t i=0;i<specs.size();++i) {
        specs[i].identity={i+1,7}; specs[i].hostKey="combined:"+std::to_string(i); specs[i].model.modelPath="combined.mdx";
        specs[i].sequenceIndex=0; specs[i].elapsedMs=i*750; // GeosetChannelFixture 按时间顺序：Stand=0、Death=1
    }
    const auto service=Good(ModelAnimationService::Create({7,33,"combined-channel-fixture"},specs,
        [&](std::string_view path,std::uint64_t) { return Result<AnimationResource>::Success({path.ends_with(".mdx") ? model : image,
            "combined-channel-fixture",std::string(64,path.ends_with(".mdx") ? 'd' : path=="first.tga" ? 'a' : 'b')}); }));
    const auto scene=service->Scene(); const auto first=Good(service->Prepare(nullptr));
    Check(scene->dynamicColors && scene->dynamicMaterialAlphas && scene->dynamicMaterialStates,
        "combined capability fixture omitted a real animated channel");
    CapabilityBackend backend; War3RenderPluginApiV2 api{}; api.structSize=sizeof(api); api.apiVersion=WAR3_RENDER_PLUGIN_API_V2;
    api.submitSkinnedModelScene=CapabilitySubmit; api.updateSkinMatrices=CapabilityUpdate; api.getSkinStatus=CapabilityStatus;
    api.captureSkinnedFrameRgba=[](War3RendererHandle,void*,std::uint64_t,War3RenderSkinnedCapturedFrameV2*)->std::int32_t { return -1; };
    AnimationRenderer renderer(&api,&backend);
    const auto expectRejected=[&](std::string_view expected) {
        const auto status=renderer.Submit(scene,first);
        std::cout<<"combined_capabilities rgb="<<backend.dynamicRgb<<" alpha="<<backend.materialAlpha<<" state="<<backend.materialState
            <<" succeeded="<<status.Succeeded()<<" uploads="<<backend.uploads<<'\n';
        for (const auto& diagnostic:status.diagnostics)
            std::cout<<"combined_capability_diagnostic code="<<diagnostic.code<<" message="<<diagnostic.message<<'\n';
        Check(!status.Succeeded() && !status.diagnostics.empty() && status.diagnostics.front().code==expected && backend.uploads==0 && !renderer.Target(),
            "combined capability refusal lost its exact diagnostic or changed the target");
    };
    expectRejected("ANIMATION_MATERIAL_ALPHA_BACKEND_UNSUPPORTED");
    backend.materialAlpha=true; expectRejected("ANIMATION_MATERIAL_STATE_BACKEND_UNSUPPORTED");
    backend.materialState=true; backend.dynamicRgb=false; expectRejected("ANIMATION_DYNAMIC_COLOR_BACKEND_UNSUPPORTED");
    backend.dynamicRgb=true; const auto submitted=renderer.Submit(scene,first);
    for (const auto& diagnostic:submitted.diagnostics)
        std::cout<<"combined_submit_diagnostic code="<<diagnostic.code<<" message="<<diagnostic.message<<'\n';
    Check(submitted.Succeeded() && backend.uploads==1,"complete material/RGB capabilities did not accept the real combined frame");
    const auto copied=[&](const AnimationFrame& frame) {
        return backend.pose.colors.size()==frame.colors.size() && backend.pose.layerAlphas==frame.layerAlphas &&
            backend.pose.materialStates.size()==frame.materialStates.size() &&
            std::memcmp(backend.pose.colors.data(),frame.colors.data(),frame.colors.size()*sizeof(War3RenderMeshColorV2))==0 &&
            std::memcmp(backend.pose.materialStates.data(),frame.materialStates.data(),frame.materialStates.size()*sizeof(War3RenderMaterialStateV2))==0;
    };
    Check(copied(*first),"combined descriptor did not deep-copy the actual RGB/alpha/texture values");
    AnimationAdvance advance; AnimationEdit edit; edit.identity=specs[0].identity; edit.elapsedMs=750; advance.edits.push_back(edit);
    const auto next=Good(service->Prepare(first.get(),advance)); const auto updated=renderer.Update(next);
    for (const auto& diagnostic:updated.diagnostics)
        std::cout<<"combined_update_diagnostic code="<<diagnostic.code<<" message="<<diagnostic.message<<'\n';
    Check(updated.Succeeded() && backend.uploads==1 && backend.updates==1 && copied(*next),
        "combined frame update lost channels or repeated static upload");
    Check(first->poses[0].colorHash!=next->poses[0].colorHash && first->poses[0].materialAlphaHash!=next->poses[0].materialAlphaHash &&
        first->poses[0].materialStateHash!=next->poses[0].materialStateHash && first->poses[1].colorHash==next->poses[1].colorHash &&
        first->poses[1].materialAlphaHash==next->poses[1].materialAlphaHash && first->poses[1].materialStateHash==next->poses[1].materialStateHash,
        "combined animated values did not advance independently per instance");
    std::cout<<"combined_capability_matrix=1 real_channels=3 static_uploads=1 CPU_descriptor_only=1\n";
}
// 无KMTF、只有TXAN平移的层：材质状态逐帧发布坐标仿射，贴图仍为静态绑定；实例时钟各自独立。
inline std::vector<std::byte> UvModel() {
    auto bytes=Model();
    for (std::size_t at=4;at+8<=bytes.size();) {
        std::uint32_t size=0; std::memcpy(&size,bytes.data()+at+4,4);
        if (!std::memcmp(bytes.data()+at,"MTLS",4)) {
            MdxTest::Writer material;
            const auto begin=material.Begin(); material.U32(0); material.U32(0); material.Tag("LAYS"); material.U32(1);
            const auto layer=material.Begin(); material.U32(0); material.U32(0); material.U32(0);
            material.U32(0); material.U32(0); material.F32(1);
            material.EndInclusive(layer); material.EndInclusive(begin);
            bytes.erase(bytes.begin()+at+8,bytes.begin()+at+8+size);
            bytes.insert(bytes.begin()+at+8,material.bytes.begin(),material.bytes.end());
            size=static_cast<std::uint32_t>(material.bytes.size()); std::memcpy(bytes.data()+at+4,&size,4); break;
        }
        at+=size+8;
    }
    MdxTest::Writer out; out.bytes=std::move(bytes);
    out.Tag("TXAN"); const auto chunk=out.Begin(); const auto entry=out.Begin();
    out.Tag("KTAT"); out.U32(2); out.U32(1); out.U32(UINT32_MAX);
    out.U32(0); out.F32(0); out.F32(0); out.F32(0); out.U32(1000); out.F32(1); out.F32(0); out.F32(0);
    out.EndInclusive(entry); out.EndChunk(chunk);
    return std::move(out.bytes);
}
inline void UvAnimation() {
    const auto model=UvModel(),image=AnimationParticleContinuityTest::Texture();
    std::array<AnimationInstanceSpec,2> specs;
    for (std::size_t i=0;i<specs.size();++i) {
        specs[i].identity={i+1,7}; specs[i].hostKey="uv:"+std::to_string(i); specs[i].model.modelPath="uv.mdx";
        specs[i].sequenceIndex=0; specs[i].elapsedMs=500+i*250; specs[i].playing=false;
    }
    const auto service=Good(ModelAnimationService::Create({7,35,"uv-fixture"},specs,
        [&](std::string_view path,std::uint64_t) { return Result<AnimationResource>::Success({path.ends_with(".mdx") ? model : image,
            "uv-fixture",std::string(64,path.ends_with(".mdx") ? 'e' : 'a')}); }));
    const auto scene=service->Scene(); const auto first=Good(service->Prepare(nullptr));
    Check(scene->dynamicMaterialStates && first->poses.size()==2,"TXAN-only scene did not enable per-frame material state");
    const auto uvAt=[](const AnimationFrame& frame,std::size_t pose) {
        return frame.materialStates[frame.poses[pose].materialStates.offset].uvTransform;
    };
    const auto near=[](const float* actual,std::array<float,6> expected) {
        for (std::size_t i=0;i<6;++i) if (std::fabs(actual[i]-expected[i])>1e-5f) return false;
        return true;
    };
    Check(near(uvAt(*first,0),{1,0,0.5f,0,1,0}) && near(uvAt(*first,1),{1,0,0.75f,0,1,0}) &&
        first->materialStates[first->poses[0].materialStates.offset].textureIndex==
        first->materialStates[first->poses[1].materialStates.offset].textureIndex,
        "TXAN translation not published per instance or texture binding changed");
    AnimationAdvance advance; AnimationEdit edit; edit.identity=specs[0].identity; edit.elapsedMs=250; advance.edits.push_back(edit);
    const auto next=Good(service->Prepare(first.get(),advance));
    Check(near(uvAt(*next,0),{1,0,0.25f,0,1,0}) && near(uvAt(*next,1),{1,0,0.75f,0,1,0}) && near(uvAt(*first,0),{1,0,0.5f,0,1,0}) &&
        first->poses[0].materialStateHash!=next->poses[0].materialStateHash,
        "TXAN clock shared between instances, old frame mutated, or hash unchanged");
    std::cout<<"txan_uv_material_state=1 per_instance_clock=1 static_texture_binding=1\n";
}
inline void Run() {
    CapabilityMatrix();
    UvAnimation();
    const auto model=Model(),image=AnimationParticleContinuityTest::Texture();
    std::map<std::string,unsigned> reads;
    const auto reader=[&](std::string_view path,std::uint64_t) {
        ++reads[std::string(path)]; return Result<AnimationResource>::Success({path.ends_with(".mdx") ? model : image,
            "original-texture-fixture",std::string(64,path.ends_with(".mdx") ? 'd' : path=="first.tga" ? 'a' : 'b')});
    };
    std::array<AnimationInstanceSpec,2> specs;
    for (std::size_t i=0;i<2;++i) {
        specs[i].identity={i+1,7}; specs[i].hostKey="texture:"+std::to_string(i); specs[i].model.modelPath="texture.mdx";
        specs[i].sequenceIndex=0; specs[i].elapsedMs=i*750; specs[i].playing=false;
    }
    BakedModelMesh baked; baked.texturePath="prefix.tga";
    baked.vertices={{0,0,0,0,0,1,0,0},{10,0,0,0,0,1,1,0},{0,10,0,0,0,1,0,1}}; baked.indices={0,1,2};
    const auto service=Good(ModelAnimationService::Create({7,33,"texture-source"},specs,reader,{},std::span(&baked,1)));
    const auto scene=service->Scene(); const auto first=Good(service->Prepare(nullptr));
    Check(scene->dynamicMaterialStates && scene->dynamicMaterialAlphas && first->materialStateSlices.size()==3,
        "KMTF-only scene omitted its explicit alpha/state mode");
    const auto a=first->poses[1].materialStates.offset,b=first->poses[2].materialStates.offset;
    Check(first->materialStates[a].textureIndex==1 && first->materialStates[b].textureIndex==2 &&
        first->materialStates[a].samplerFlags==1 && first->materialStates[b].samplerFlags==2 &&
        first->materialStates[0].samplerFlags==3 && first->layerAlphas[a]==0.75f,
        "TEXS ID used as resident index, clocks shared, flags guessed, or static alpha premultiplied");
    const auto descriptor=first->DescribeMaterialState();
    Check(descriptor.base.base.base.structSize==sizeof(descriptor) && descriptor.materialStates==first->materialStates.data() &&
        descriptor.materialInstanceSlices==first->materialStateSlices.data(),"material descriptor borrowed scratch or lost outer size");
    AnimationAdvance advance; AnimationEdit edit; edit.identity=specs[0].identity; edit.elapsedMs=750; advance.edits.push_back(edit);
    const auto next=Good(service->Prepare(first.get(),advance)),repeat=Good(service->Prepare(next.get()));
    Check(next->materialStates[a].textureIndex==2 && first->materialStates[a].textureIndex==1 &&
        next->poses[1].materialStateHash==repeat->poses[1].materialStateHash &&
        next->timings.outputMotionHash==repeat->timings.outputMotionHash && reads["first.tga"]==1 && reads["second.tga"]==1,
        "KMTF mutated old frame, nondeterministic numeric hash, or repeated resource IO");
    const auto receipt=AnimationReceiptJson(*scene,*next,{},"texture",std::string(64,'a'));
    Check(receipt.find("residentTextureIndex")!=std::string::npos && receipt.find("materialStateHashFNV1a")!=std::string::npos,
        "completed-frame receipt omitted real texture state");
    AnimationInstanceDelta delta; delta.session={7,34,"texture-source"};
    auto added=specs[0]; added.identity={3,7}; added.hostKey="texture:added"; added.elapsedMs=0; delta.additions.push_back(added);
    const auto plan=Good(service->PrepareInstanceCatalog(delta,reader)); const auto rebound=Good(service->PrepareInstanceDelta(*next,*plan));
    Check(!rebound.assetsChanged && rebound.service->Scene()->geometry.textures.data()==scene->geometry.textures.data() &&
        rebound.frame->poses[1].materialStateHash==next->poses[1].materialStateHash && reads["first.tga"]==1,
        "same-asset roster changed resident textures or surviving material clock");
    std::vector<AnimationInstanceSpec> many(512,specs[0]);
    for (std::size_t i=0;i<many.size();++i) { many[i].identity.value=i+1; many[i].hostKey="shared-texture:"+std::to_string(i); }
    AnimationLimits sharedLimits; sharedLimits.retainUnsupportedStatic=true; sharedLimits.maxActivePoseBytes=4096;
    sharedLimits.maxFrameBytes=many.size()*(sizeof(War3RenderSkinnedModelInstanceV2)+sizeof(AnimationPoseLink)+
        sizeof(AnimationInstanceState)+sizeof(AnimationPickBounds)+2*sizeof(War3RenderMaterialLayerSliceV2))+32768;
    const auto originalBudget=ModelAnimationService::Create({7,33,"shared-texture"},many,reader,sharedLimits);
    Check(!originalBudget.Succeeded() && !originalBudget.diagnostics.empty() &&
        originalBudget.diagnostics.front().code=="ANIMATION_FRAME_BUDGET",
        "original narrow shared Stand budget no longer refused actual cache metadata");
    // 一个Stand变体拥有三份Link元数据，缓存与候选双份计费；只补新增typed记录的实际成本。
    const auto typedStandBytes=2ull*3*AnimationPoseWarnings::Capacity*sizeof(War3RenderCore::MaterialAlphaPreviewWarning);
    // 核心 0.1.19 起贴图渠道求值器为每个层槽另存 TXAN 坐标仿射（64 实例批宽 × 2 层），同样只补这部分实际成本。
    const auto uvWorkspaceBytes=64ull*2*sizeof(War3RenderCore::UvTransform);
    sharedLimits.maxFrameBytes+=typedStandBytes+uvWorkspaceBytes;
    const auto shared=Good(ModelAnimationService::Create({7,33,"shared-texture"},many,reader,sharedLimits));
    const auto stand=Good(shared->Prepare(nullptr));
    Check(stand->sharedPoseRanges && stand->budgetStaticInstances>0 && stand->materialStateSlices.size()==many.size() &&
        stand->poses.back().materialStates.count==2 && stand->poses.back().materialStateHash!=0,
        "shared Stand lost its owned texture slice or silently fell back to static texture mode");
    std::cout<<"shared_texture_original_budget_refused=ANIMATION_FRAME_BUDGET typed_stand_cache_bytes="<<typedStandBytes<<'\n';
    AnimationLimits limits; limits.retainUnsupportedStatic=true;
    // Linear/Hermite/Bezier 标记的整数贴图轨迹按阶梯播放；只有未知插值编号仍整模型拒绝。
    const auto invalid=Model(4); const auto bad=Good(ModelAnimationService::Create({7,33,"invalid-texture"},specs,
        [&](std::string_view path,std::uint64_t) { return Result<AnimationResource>::Success({path.ends_with(".mdx") ? invalid : image,
            "original-invalid-texture",std::string(64,'c')}); },limits));
    const auto hidden=Good(bad->Prepare(nullptr));
    Check(hidden->hiddenInstances==2 && hidden->animatedInstances==0 && !hidden->materialIssues.empty() &&
        hidden->poses[0].materialStateHash==0 && hidden->materialIssues[0].source.sourceByteOffset!=0,
        "unknown KMTF interpolator silently displayed its static texture");
}
} // namespace AnimationMaterialTextureTest
