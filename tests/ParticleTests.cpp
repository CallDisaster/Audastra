#include "Particles/ParticleTypes.h"
#include "Particles/ParticleInput.h"
#include "War3MapCore/RenderPluginMaterialState.h"
#include <limits>
#include "MdxTestModel.h"
#include "ParticleVisibilityPreviewTests.h"
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>

using namespace War3Map;
using namespace War3Map::Render::Particles;
namespace {
void Check(bool ok,const char* s){if(!ok)throw std::runtime_error(s);}
Asset Fixture(){Asset a;Emitter e;e.rate=20;e.life=2;e.speed=10;e.variation=0.5f;e.latitude=20;e.width=5;e.length=5;a.emitters.push_back(e);return a;}
State Make(const Asset& a,std::uint32_t cap=100){std::vector<std::uint32_t> quotas(a.emitters.size(),cap);auto r=Create(a,17,3,0,quotas);Check(r.Succeeded(),"create");return std::move(*r.value);}
Status Anchors(std::uint64_t ms,std::span<Anchor> anchors){for(auto& a:anchors){a=Anchor{};a.model[3]=static_cast<float>(ms)*0.1f;}return {};}
void Deterministic(){
    ParticleVisibilityPreviewTest::SamplesAndSimulation();
    const auto a=Fixture();auto s=Make(a),q=Make(a);Timeline t;t.end=10000;
    Check(Advance(a,s,t,1500,Anchors).Succeeded(),"replay");
    for(std::uint64_t at=33;at<1500;at+=33)Check(Advance(a,q,t,at,Anchors).Succeeded(),"step");
    Check(Advance(a,q,t,1500,Anchors).Succeeded(),"end");
    auto g=Build(a,s,t,{0,-100,100},{0,0,1}),h=Build(a,q,t,{0,-100,100},{0,0,1});
    Check(g.Succeeded()&&h.Succeeded()&&g.value->vertices.size()==h.value->vertices.size(),"geometry");
    Check(std::memcmp(g.value->vertices.data(),h.value->vertices.data(),g.value->vertices.size()*sizeof(Vertex))==0,"partition changes state");
    const auto before=q.emitters[0].random;Check(Advance(a,q,t,1500,Anchors).Succeeded()&&q.emitters[0].random==before,"pause random");
    Check(!Advance(a,q,t,1499,Anchors).Succeeded(),"rewind accepted");
    t.elapsedReferenceMs=500;t.globalReferenceMs=500;Check(Advance(a,q,t,1500,Anchors).Succeeded(),"equivalent clock reference");
    t.globalReferenceMs=1000;Check(!Advance(a,q,t,1533,Anchors).Succeeded()&&q.timeMs==1500,"clock change continued stale state");
    t.globalReferenceMs=0;auto unsupported=Make(a);const auto replay=Advance(a,unsupported,t,500,Anchors);
    Check(!replay.Succeeded()&&replay.diagnostics.front().code=="PARTICLE_GLOBAL_REPLAY_UNSUPPORTED"&&unsupported.timeMs==0,"negative global history accepted");
    std::cout<<"fixed_grid=10 partition33_bitwise_equal=1 paused_rng_unchanged=1 vertices="<<g.value->vertices.size()<<'\n';
}
void FrozenResume() {
    auto asset=Fixture();auto state=Make(asset);Timeline clock;clock.end=10000;
    unsigned anchors=0;
    const auto counted=[&](std::uint64_t at,std::span<Anchor> out) { ++anchors;return Anchors(at,out); };
    Check(Advance(asset,state,clock,500,counted).Succeeded()&&Count(state).particles==10,"freeze seed");
    const auto random=state.emitters[0].random,spawned=Count(state).spawned,dropped=Count(state).suppressed;
    const auto accumulator=state.emitters[0].accumulator;
    const auto first=state.emitters[0].particles.front();anchors=0;
    Check(FastForwardFrozen(asset,state,clock,1753).Succeeded()&&state.tickMs==1750&&state.timeMs==1753&&anchors==0,
        "frozen resume sampled history or kept old grid");
    const auto& aged=state.emitters[0].particles.front();
    Check(std::abs(aged.age-first.age-1.25f)<0.00001f&&
        std::abs(aged.position[0]-first.position[0]-first.velocity[0]*1.25f)<0.0001f&&
        state.emitters[0].random==random&&Count(state).spawned==spawned&&Count(state).suppressed==dropped&&
        state.emitters[0].accumulator==accumulator,"freeze changed RNG/emission identity or did not age live state");
    Check(FastForwardFrozen(asset,state,clock,60003).Succeeded()&&Count(state).particles==0&&anchors==0&&
        state.emitters[0].random==random,"long freeze replayed or retained expired particles");
    Check(Advance(asset,state,clock,60013,counted).Succeeded()&&anchors==1&&Count(state).spawned==spawned,
        "resume replayed backlog or re-created random stream");
    const auto at=state.timeMs;clock.globalReferenceMs=10;
    Check(!FastForwardFrozen(asset,state,clock,60023).Succeeded()&&state.timeMs==at,"freeze silently rebased global phase");
    clock={};clock.end=10000;auto initial=Make(asset);anchors=0;
    Check(FastForwardFrozen(asset,initial,clock,600003).Succeeded()&&Count(initial).spawned==0&&anchors==0&&
        Advance(asset,initial,clock,600013,counted).Succeeded()&&anchors==1,"initial world jump performed warmup");
    auto ribbon=Fixture();ribbon.emitters[0].kind=Kind::Ribbon;ribbon.emitters[0].gravity=2;
    auto strip=Make(ribbon);Check(Advance(ribbon,strip,clock,500,counted).Succeeded(),"freeze ribbon seed");
    const auto edge=strip.emitters[0].segments.front();anchors=0;
    Check(FastForwardFrozen(ribbon,strip,clock,1000).Succeeded()&&anchors==0&&
        std::abs(strip.emitters[0].segments.front().age-edge.age-0.5f)<0.00001f&&
        strip.emitters[0].segments.front().above[2]<edge.above[2],"frozen ribbon gravity/age lost");
    Check(FastForwardFrozen(ribbon,strip,clock,65003).Succeeded()&&Count(strip).ribbonSegments==0&&anchors==0,
        "expired frozen ribbon retained");
    auto bounded=Fixture();bounded.emitters[0].squirt=1;bounded.globalSequences={100};
    auto& burst=bounded.emitters[0].tracks[static_cast<std::size_t>(Parameter::Rate)];
    burst.globalSequence=0;burst.keys={{0,{5,0,0},{},{}}};
    auto now=Make(bounded);clock={};clock.end=10000;clock.elapsedReferenceMs=600003;
    clock.boundedWorldPreview=true;anchors=0;
    Check(FastForwardFrozen(bounded,now,clock,600003).Succeeded()&&now.globalStartMs==-600003&&anchors==0,
        "current-only phase requested negative global history");
    clock.elapsedReferenceMs+=110;clock.globalReferenceMs=110;
    Check(Advance(bounded,now,clock,clock.elapsedReferenceMs,counted).Succeeded()&&Count(now).spawned==10&&anchors==11,
        "bounded global squirt lost zero crossing/current period or sampled invalid history");
    const auto saved=now.timeMs;clock.boundedWorldPreview=false;
    const auto strict=FastForwardFrozen(bounded,now,clock,saved);
    Check(!strict.Succeeded()&&strict.diagnostics.front().code=="PARTICLE_GLOBAL_REPLAY_UNSUPPORTED"&&now.timeMs==saved,
        "world phase policy weakened default strict replay");
    std::cout<<"frozen_resume_no_anchor=1 rng_emission_retained=1 expired_cleared=1 next_step_only=1 initial_no_warmup=1\n";
}
void Emission(){auto a=Fixture();auto s=Make(a);Timeline t;t.end=10000;
    FrozenResume();
    Check(Advance(a,s,t,1000,Anchors).Succeeded(),"emit");Check(Count(s).particles==20&&Count(s).spawned==20,"rate*time");
    {   // 1.27a 发射器坐标系绕节点 Z 转 +90°：长度（KP2N）沿节点 X，宽度（KP2W）沿节点 Y。
        auto axes=Fixture();axes.emitters[0].width=0;axes.emitters[0].length=10;axes.emitters[0].speed=0;axes.emitters[0].variation=0;
        auto spread=Make(axes);Check(Advance(axes,spread,t,1000,[](std::uint64_t,std::span<Anchor> out){for(auto& o:out)o=Anchor{};return Status{};}).Succeeded(),"axes emit");
        float minX=1e9f,maxX=-1e9f,maxY=0;
        for(const auto& p:spread.emitters[0].particles){minX=std::min(minX,p.position[0]);maxX=std::max(maxX,p.position[0]);maxY=std::max(maxY,std::abs(p.position[1]));}
        Check(spread.emitters[0].particles.size()==20&&maxY==0&&maxX-minX>5&&minX>=-5&&maxX<=5,"PRE2 length must spread along node X");
    }
    a.emitters[0].rate=0;Check(Advance(a,s,t,3100,Anchors).Succeeded()&&Count(s).particles==0,"expiry");
    a=Fixture();s=Make(a,3);Check(Advance(a,s,t,500,Anchors).Succeeded()&&Count(s).particles==3&&Count(s).suppressed==7,"budget");
    a.emitters[0].kind=Kind::Ribbon;s=Make(a);Check(Advance(a,s,t,500,Anchors).Succeeded()&&Count(s).ribbonSegments==10,"ribbon count");
    a.emitters[0].above=1;a.emitters[0].below=1;auto g=Build(a,s,t,{0,-100,100},{0,0,1});Check(g.Succeeded()&&g.value->vertices.size()==54,"ribbon connected");
    a=Fixture();a.emitters[0].squirt=1;a.emitters[0].rate=5;s=Make(a);Check(Advance(a,s,t,500,Anchors).Succeeded()&&Count(s).spawned==5,"squirt once");
    auto& burst=a.emitters[0].tracks[static_cast<std::size_t>(Parameter::Rate)];
    burst.keys={{0,{2000000,0,0},{},{}},{10,{0,0,0},{},{}}};s=Make(a);
    const auto over=Advance(a,s,t,10,Anchors);
    Check(!over.Succeeded()&&over.diagnostics.front().code=="PARTICLE_BURST_BUDGET"&&Count(s).spawned==0,"oversized burst silently clamped");
    burst.keys[0].value[0]=1000000;s=Make(a);Check(Advance(a,s,t,10,Anchors).Succeeded()&&Count(s).spawned==100&&Count(s).suppressed==999900,"burst suppression accounting");
    a.globalSequences={0};burst.globalSequence=0;burst.keys={{0,{5,0,0},{},{}},{10,{0,0,0},{},{}}};
    t.globalReferenceMs=700;s=Make(a);Check(Advance(a,s,t,500,Anchors).Succeeded()&&Count(s).spawned==5,"zero global squirt skipped or repeated");
    Check(Sample(burst,{1,0,0},a,t,900)[0]==5,"zero global particle track used action clock");
    burst.keys.erase(burst.keys.begin());Check(Sample(burst,{1,0,0},a,t,900)[0]==1,"zero global missing zero key changed fallback");
    burst.globalSequence=1;bool rejected=false;try{(void)Sample(burst,{1,0,0},a,t,900);}catch(const std::exception&){rejected=true;}Check(rejected,"bad global index accepted");
    std::cout<<"rate20_at1000=20 expired=1 quota3_suppressed7=1 ribbon10_edges=1 squirt5_once=1\n";
}
std::vector<std::byte> Binary(std::uint32_t side=2,std::uint32_t last=3,std::uint32_t first=0,bool mixed=false){
    MdxTest::Writer w;w.Tag("MDLX");w.Tag("VERS");w.U32(4);w.U32(800);
    for(int record=0;record<(mixed?4:2);++record) {
        const bool ribbon=record==(mixed?3:1);
        w.Tag(ribbon?"RIBB":"PRE2");const auto chunk=w.Begin(),object=w.Begin(),node=w.Begin();
        w.Fixed("Emitter",80);w.U32(static_cast<std::uint32_t>(record));w.U32(UINT32_MAX);w.U32(ribbon?0x4000u:0x1000u);w.EndInclusive(node);
        if(!ribbon){
            for(const float v:{10.0f,0.2f,20.0f,1.0f,2.0f,20.0f,5.0f,5.0f})w.F32(v);
            w.U32(mixed&&record==1?99u:1u);w.U32(side);w.U32(side);w.U32(2);w.F32(0.5f);w.F32(0.5f);
            for(int i=0;i<3;++i){w.F32(0.2f);w.F32(0.4f);w.F32(0.8f);}
            const unsigned char a[3]{255,128,0};w.Raw(a,3);for(int i=0;i<3;++i)w.F32(2);
            for(int i=0;i<4;++i){w.U32(first);w.U32(last);w.U32(1);}w.U32(0);w.U32(0);w.U32(0);w.U32(0);
            w.Tag("KP2V");w.U32(2);w.U32(1);w.U32(UINT32_MAX);w.U32(0);w.F32(1);w.U32(1000);w.F32(0);
        }else{
            for(const float v:{3.0f,2.0f,0.5f,0.2f,0.4f,0.8f,2.0f})w.F32(v);
            w.U32(0);w.U32(20);w.U32(1);w.U32(1);w.U32(0);w.F32(1);
            w.Tag("KRCO");w.U32(1);w.U32(2);w.U32(UINT32_MAX);w.U32(0);
            for(const float v:{0.1f,0.2f,0.3f,0.4f,0.5f,0.6f,0.7f,0.8f,0.9f})w.F32(v);
        }
        w.EndInclusive(object);w.EndChunk(chunk);
    }
    return std::move(w.bytes);
}
void PreviewEmitterIsolation(){
    const auto bytes=Binary(2,3,0,true),original=bytes;
    const auto strict=Read(bytes),preview=Read(bytes,{},ReadMode::PreviewSkipInvalidEmitters);
    Check(!strict.Succeeded()&&strict.diagnostics.front().code=="PARTICLE_FILTER_UNSUPPORTED","strict emitter rejection changed");
    Check(preview.Succeeded()&&preview.value->emitters.size()==3&&preview.value->skippedEmitters.size()==1,"single bad emitter isolated");
    const auto& emitters=preview.value->emitters;const auto& skipped=preview.value->skippedEmitters.front();
    Check(emitters[0].nodeId==0&&emitters[1].nodeId==2&&emitters[1].sourceIndex==2&&emitters[2].kind==Kind::Ribbon,
        "later emitter source order or cross-chunk identity lost");
    Check(skipped.kind==Kind::Particle2&&skipped.sourceIndex==1&&skipped.nodeId==1u&&
        skipped.code==strict.diagnostics.front().code&&skipped.message==strict.diagnostics.front().message&&bytes==original,
        "skipped source diagnostic or source bytes changed");
}
void PreviewIsolationKeepsHardLimits(){
    auto bytes=Binary(2,3,0,true);ReadLimits limits;limits.maxEmitters=3;
    const auto count=Read(bytes,limits,ReadMode::PreviewSkipInvalidEmitters);limits=ReadLimits{};limits.maxKeys=6;
    const auto keys=Read(bytes,limits,ReadMode::PreviewSkipInvalidEmitters);limits=ReadLimits{};limits.maxBytes=bytes.size()-1;
    const auto size=Read(bytes,limits,ReadMode::PreviewSkipInvalidEmitters);bytes.pop_back();
    const auto truncated=Read(bytes,{},ReadMode::PreviewSkipInvalidEmitters);
    Check(!count.Succeeded()&&count.diagnostics.front().code=="PARTICLE_EMITTER_BUDGET"&&
        !keys.Succeeded()&&keys.diagnostics.front().code=="PARTICLE_KEY_BUDGET"&&
        !size.Succeeded()&&size.diagnostics.front().code=="PARTICLE_READ_BUDGET"&&
        !truncated.Succeeded()&&truncated.diagnostics.front().code=="PARTICLE_TRUNCATED","preview refunded skipped budgets or ignored truncation");
}
void MaterialStateInputContract(){
    using namespace War3Map::Render;
    War3RenderMaterialStateSkinUpdateV2 state{};
    auto& material=state.base;auto& colored=material.base;auto& skin=colored.base;
    skin.structSize=sizeof(state);skin.sceneVersion=1;skin.contentVersion=7;skin.instanceCount=1;skin.alphaCount=1;
    War3RenderSkinnedModelInstanceV2 instance{};float alpha=1;skin.instances=&instance;skin.alphas=&alpha;
    float layerAlpha=1.25f;War3RenderMaterialLayerSliceV2 alphaSlice{0,1},stateSlice{0,1};
    material.layerAlphaFormat=WAR3_RENDER_DYNAMIC_LAYER_ALPHA_V1;
    material.layerAlphaCount=material.instanceSliceCount=1;material.layerAlphas=&layerAlpha;material.instanceSlices=&alphaSlice;
    // 模型贴图17不是粒子池的索引；此处只验尾部，真实模型范围由CopySkinUpdate负责。
    War3RenderMaterialStateV2 value{17,3,{2,0,-1,0,.5f,3}};
    state.materialStateFormat=WAR3_RENDER_DYNAMIC_MATERIAL_STATE_V1;
    state.materialStateCount=state.materialInstanceSliceCount=1;state.materialStates=&value;state.materialInstanceSlices=&stateSlice;
    std::array<War3RenderParticleVertexV2,3> vertices{};for(auto& v:vertices)for(auto& c:v.color)c=1;
    War3RenderParticleDrawV2 draw{0,3,0,WAR3_PARTICLE_BLEND};
    War3RenderParticleUpdateV2 in{sizeof(in),3,1,0,1,7,vertices.data(),&draw,&skin};
    ParticleFrameData out;Check(CopyParticleInput(in,1,out),"known state tail or distinct model texture pool rejected");
    const auto accepted=out;
    vertices[0].position[0]=42;
    const auto reject=[&](const char* message){
        Check(!CopyParticleInput(in,1,out)&&out.sceneVersion==accepted.sceneVersion&&out.contentVersion==accepted.contentVersion
            &&out.vertices.size()==accepted.vertices.size()&&out.draws.size()==accepted.draws.size()
            &&std::memcmp(out.vertices.data(),accepted.vertices.data(),accepted.vertices.size()*sizeof(War3RenderParticleVertexV2))==0
            &&std::memcmp(out.draws.data(),accepted.draws.data(),accepted.draws.size()*sizeof(War3RenderParticleDrawV2))==0,message);
    };
    for(const auto size:{sizeof(state)-1,sizeof(state)+1}){
        skin.structSize=static_cast<std::uint32_t>(size);reject("unknown material-state header size accepted");
    }
    skin.structSize=sizeof(state);state.materialStateFormat=2;reject("unknown material-state format accepted");
    state.materialStateFormat=WAR3_RENDER_DYNAMIC_MATERIAL_STATE_V1;state.reserved=1;reject("reserved material-state bits accepted");state.reserved=0;
    state.materialStateCount=1048577;reject("material-state element budget bypassed");state.materialStateCount=1;
    state.materialStates=nullptr;reject("missing material-state pool accepted");state.materialStates=&value;
    state.materialInstanceSlices=nullptr;reject("missing material-state slices accepted");state.materialInstanceSlices=&stateSlice;
    state.materialInstanceSliceCount=0;reject("material-state instance count mismatch accepted");state.materialInstanceSliceCount=1;
    stateSlice.first=UINT32_MAX;reject("material-state first overflow accepted");stateSlice.first=0;
    stateSlice.count=2;reject("material-state range overflow accepted");stateSlice.count=1;
    value.samplerFlags=4;reject("unknown material-state sampler flags accepted");value.samplerFlags=3;
    value.uvTransform[4]=std::numeric_limits<float>::quiet_NaN();reject("material-state NaN UV accepted");value.uvTransform[4]=.5f;
    value.uvTransform[5]=std::numeric_limits<float>::infinity();reject("material-state infinite UV accepted");value.uvTransform[5]=3;
    // 在任何伪长数组解引用之前拒绝总工作量，不靠单个count的上限代替64 MiB组合预算。
    skin.groupCount=1048576;state.materialStateCount=1048576;reject("combined material-state pose budget bypassed");
    skin.groupCount=0;state.materialStateCount=1;
    skin.instanceCount=material.instanceSliceCount=state.materialInstanceSliceCount=UINT32_MAX;
    reject("unbounded state slice walk bypassed work budget");
    skin.instanceCount=material.instanceSliceCount=state.materialInstanceSliceCount=1;
    material.reserved=1;reject("state tail hid bad KMTA reserved bits");material.reserved=0;
    material.layerAlphaFormat=2;reject("state tail hid unknown KMTA format");material.layerAlphaFormat=WAR3_RENDER_DYNAMIC_LAYER_ALPHA_V1;
    material.layerAlphas=nullptr;reject("state tail hid missing KMTA pool");material.layerAlphas=&layerAlpha;
    material.instanceSliceCount=0;reject("state tail hid bad KMTA slice count");material.instanceSliceCount=1;
    alphaSlice.count=2;reject("state tail hid bad KMTA range");alphaSlice.count=1;
    layerAlpha=std::numeric_limits<float>::quiet_NaN();reject("state tail hid nonfinite KMTA");layerAlpha=1.25f;
    War3RenderMeshColorV2 rgb{{.2f,.4f,.8f}};
    colored.colors=&rgb;reject("state format0 hid stray RGB pointer");
    colored.colorCount=1;reject("state format0 hid stray RGB count");
    colored.colorFormat=WAR3_RENDER_DYNAMIC_MESH_RGB_V1;
    Check(CopyParticleInput(in,1,out)&&skin.structSize==sizeof(state),"state/KMTA/RGB known tail combination rejected or truncated");
    out=accepted;
    colored.colorCount=0;reject("state tail hid RGB count mismatch");colored.colorCount=1;
    rgb.rgb[0]=-1;reject("state tail hid RGB range violation");rgb.rgb[0]=.2f;
    rgb.rgb[1]=std::numeric_limits<float>::quiet_NaN();reject("state tail hid RGB NaN");rgb.rgb[1]=.4f;
    colored.colors=nullptr;reject("state tail hid missing RGB pool");colored.colors=&rgb;
    Check(CopyParticleInput(in,1,out)&&out.vertices[0].position[0]==42,"material-state recovery failed");
    std::cout<<"particle_material_state_header=1 distinct_model_texture_pool=1 known_tails_validated=1 bad_tail_atomic=1 pose_work_budget=1\n";
}
void InputContract(){
    using namespace War3Map::Render;
    War3RenderPluginApiV2 old{};old.apiVersion=WAR3_RENDER_PLUGIN_API_V2;
    old.structSize=static_cast<std::uint32_t>(offsetof(War3RenderPluginApiV2,submitParticleModelScene));
    Check(!ParticleApiSupported(&old),"old tail accepted");
    old.structSize=sizeof(old);Check(!ParticleApiSupported(&old),"null tail accepted");
    War3RenderSkinUpdateV2 skin{};skin.structSize=sizeof(skin);skin.sceneVersion=1;skin.contentVersion=7;
    std::array<War3RenderParticleVertexV2,3> vertices{};
    for(auto& v:vertices)for(auto& c:v.color)c=1;
    War3RenderParticleDrawV2 draw{0,3,0,WAR3_PARTICLE_BLEND};
    War3RenderParticleUpdateV2 in{sizeof(in),3,1,0,1,7,vertices.data(),&draw,&skin};
    ParticleFrameData out;Check(CopyParticleInput(in,1,out),"valid input rejected");
    vertices[0].position[0]=42;Check(out.vertices[0].position[0]==0,"borrowed host memory");
    const auto saved=out.contentVersion;draw.texture=1;Check(!CopyParticleInput(in,1,out)&&out.contentVersion==saved,"invalid texture overwrote output");
    draw.texture=0;draw.filter=7;Check(!CopyParticleInput(in,1,out),"unknown blend");
    draw.filter=0;vertices[0].color[0]=std::numeric_limits<float>::quiet_NaN();Check(!CopyParticleInput(in,1,out),"NaN accepted");
    vertices[0].color[0]=1;skin.contentVersion=8;Check(!CopyParticleInput(in,1,out),"model/particle mismatch");
    skin.contentVersion=7;Check(CopyParticleInput(in,1,out),"recovery failed");
    War3RenderMeshColorV2 rgb{{0.2f,0.4f,0.8f}};
    War3RenderDynamicColoredSkinUpdateV2 colored{};colored.base=skin;colored.base.structSize=sizeof(colored);
    colored.base.alphaCount=1;colored.colorFormat=WAR3_RENDER_DYNAMIC_MESH_RGB_V1;colored.colorCount=1;colored.colors=&rgb;
    in.modelPose=&colored.base;Check(CopyParticleInput(in,1,out),"dynamic color pair rejected");
    colored.base.structSize=sizeof(colored)-1;Check(!CopyParticleInput(in,1,out),"unknown pose size accepted");
    colored.base.structSize=sizeof(colored);colored.colorCount=0;Check(!CopyParticleInput(in,1,out),"color count mismatch");
    colored.colorCount=1;rgb.rgb[0]=std::numeric_limits<float>::quiet_NaN();Check(!CopyParticleInput(in,1,out),"NaN paired color accepted");
    rgb.rgb[0]=0.2f;Check(CopyParticleInput(in,1,out)&&colored.base.structSize==sizeof(colored),"color tail truncated");
    War3RenderMaterialSkinUpdateV2 material{};material.base.base=skin;material.base.base.structSize=sizeof(material);
    material.layerAlphaFormat=WAR3_RENDER_DYNAMIC_LAYER_ALPHA_V1;in.modelPose=&material.base.base;
    Check(CopyParticleInput(in,1,out),"known material header without dynamic RGB rejected");
    material.base.colorCount=1;Check(!CopyParticleInput(in,1,out),"material format0 hid stray RGB");material.base.colorCount=0;
    for(const auto size:{sizeof(material)-1,sizeof(material)+1}){
        material.base.base.structSize=static_cast<std::uint32_t>(size);Check(!CopyParticleInput(in,1,out),"unknown material size accepted");
    }
    material.base.base.structSize=sizeof(material);material.layerAlphaFormat=2;
    Check(!CopyParticleInput(in,1,out)&&out.contentVersion==saved,"bad material format changed accepted particle frame");
    static_assert(offsetof(War3RenderPluginApiV2,submitParticleModelScene)==
        offsetof(War3RenderPluginApiV2,captureSkinnedFrameRgba)+sizeof(old.captureSkinnedFrameRgba));
    MaterialStateInputContract();
    std::cout<<"old_plugin_rejected=1 prefix_unchanged=1 paired_versions=1 dynamic_color_header=1 deep_copy=1 invalid_preserves=1 recovery=1\n";
}
void Parsing(){
    ParticleVisibilityPreviewTest::ReadContracts();
    InputContract();
    PreviewEmitterIsolation();PreviewIsolationKeepsHardLimits();
    auto binary=Binary();const auto read=Read(binary);
    Check(read.Succeeded()&&read.value->emitters.size()==2,"PRE2/RIBB read");
    Check(read.value->emitters[0].colors[0]==Vec3{0.2f,0.4f,0.8f}&&read.value->emitters[0].headOrTail==2,"PRE2 RGB or head/tail");
    Check(read.value->emitters[0].tracks[static_cast<std::size_t>(Parameter::Visibility)].keys.size()==2,"visibility track");
    for(const std::array<std::uint32_t,4> range: {std::array<std::uint32_t,4>{64,0,0,64},
        {65,0,0,0},{65,32,0,0},{64,65,0,0},{UINT32_MAX,UINT32_MAX,0,0},{64,64,64,64}}) {
        const auto parsed=Read(Binary(8,range[0],range[1]));Check(parsed.Succeeded(),"native atlas endpoint read");
        for(const auto& interval:parsed.value->emitters[0].intervals)
            Check(interval.first==range[2]&&interval.last==range[3]&&interval.repeat==1,"four atlas intervals or repeat changed");
    }
    const auto reversed=Read(Binary(8,31,32));Check(!reversed.Succeeded()&&
        reversed.diagnostics.front().code=="PARTICLE_ATLAS_RANGE","normalized negative atlas span accepted");
    auto atlas=Fixture();atlas.emitters[0].rows=atlas.emitters[0].columns=8;atlas.emitters[0].middle=0;
    atlas.emitters[0].colors=read.value->emitters[0].colors;atlas.emitters[0].alphas={1,1,1};
    atlas.emitters[0].intervals[1]={32,64,1};auto state=Make(atlas);Particle terminal;terminal.life=1;terminal.age=0.999f;
    state.emitters[0].particles.push_back(terminal);const auto geometry=Build(atlas,state,Timeline{}, {0,-100,100},{0,0,1});
    Check(geometry.Succeeded()&&geometry.value->vertices[0].uv[0]==0&&geometry.value->vertices[0].uv[1]==1,
        "inclusive terminal atlas cell was silently clamped");
    Check(geometry.value->vertices[0].color==std::array<float,4>{0.2f,0.4f,0.8f,1},"PRE2 asymmetric RGB did not reach vertex");
    const auto& ribbon=read.value->emitters[1];const auto& color=ribbon.tracks[static_cast<std::size_t>(Parameter::Color)].keys[0];
    Check(ribbon.ribbonColor==Vec3{0.2f,0.4f,0.8f}&&color.value==Vec3{0.3f,0.2f,0.1f}&&
        color.in==Vec3{0.6f,0.5f,0.4f}&&color.out==Vec3{0.9f,0.8f,0.7f},"RIBB static RGB/KRCO BGR distinction lost");
    Asset ribbons;ribbons.emitters.push_back(ribbon);auto strip=Make(ribbons);Timeline clock;clock.end=1000;
    Check(Advance(ribbons,strip,clock,100,Anchors).Succeeded(),"RIBB color simulation failed");
    const auto rendered=Build(ribbons,strip,clock,{0,-100,100},{0,0,1});
    Check(rendered.Succeeded()&&!rendered.value->vertices.empty()&&
        rendered.value->vertices[0].color==std::array<float,4>{0.3f,0.2f,0.1f,0.5f},"KRCO RGB did not reach ribbon vertex");
    binary.pop_back();Check(!Read(binary).Succeeded(),"tail truncation");

    const std::array<std::byte,16> bad{};Check(!Read(bad).Succeeded(),"bad magic");
    Track track;track.interpolation=1;track.keys={{100,{2,0,0},{},{}},{200,{4,0,0},{},{}}};Asset a;Timeline t;t.end=300;
    Check(Sample(track,{1,0,0},a,t,150)[0]==3,"linear");t.start=500;t.end=1000;
    Check(Sample(track,{1,0,0},a,t,150)[0]==1,"other sequence contamination");
    a.globalSequences={300};track.globalSequence=0;Check(Sample(track,{1,0,0},a,t,450)[0]==3,"global");
    t.elapsedReferenceMs=500;t.globalReferenceMs=1000;const auto mapped=GlobalTimeAt(t,550);
    Check(mapped.Succeeded()&&*mapped.value==1050&&Sample(track,{1,0,0},a,t,550)[0]==3,"independent clock mapping");
    t.globalReferenceMs=0;const auto zero=GlobalTimeAt(t,500);Check(zero.Succeeded()&&*zero.value==0&&!GlobalTimeAt(t,499).Succeeded(),"global clock underflow");
    t.elapsedReferenceMs=0;t.globalReferenceMs=MaxClockMs;Check(!GlobalTimeAt(t,1).Succeeded(),"global clock overflow");
    std::cout<<"bad_magic_rejected=1 linear=1 sequence_isolation=1 global=1\n";
}
}
int main(int argc,char** argv){try{Check(argc==2,"group");const std::string group=argv[1];
    if(group=="deterministic")Deterministic();else if(group=="emission")Emission();else if(group=="parsing")Parsing();else Check(false,"unknown");
    std::cout<<"PASS "<<group<<'\n';return 0;}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
