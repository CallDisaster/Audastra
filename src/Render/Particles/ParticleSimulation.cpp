#include "ParticleMath.h"
#include <numbers>

namespace War3Map::Render::Particles {
Result<std::int64_t> GlobalPhase(const Timeline& t) try {
    if (!t.boundedWorldPreview)
        return Result<std::int64_t>::Success(static_cast<std::int64_t>(CheckedGlobalTime(t,0)));
    // 验证两个显式参考时钟的范围，保留有符号差值；没有构造不存在的历史节点时间。
    (void)CheckedGlobalTime(t,t.elapsedReferenceMs);
    return Result<std::int64_t>::Success(static_cast<std::int64_t>(t.globalReferenceMs)-
        static_cast<std::int64_t>(t.elapsedReferenceMs));
} catch(const Error& e) { return Result<std::int64_t>::Failure("Render.Particles",e.code,e.what()); }
Result<State> Create(const Asset& a,std::uint64_t id,std::uint64_t generation,std::uint32_t sequence,
    std::span<const std::uint32_t> quotas,const Limits& limits) try {
    Require(id&&generation&&quotas.size()==a.emitters.size()&&a.emitters.size()<=256,
        "PARTICLE_IDENTITY","粒子实例身份或配额表无效。");
    State state;state.instance=id;state.generation=generation;state.sequenceIndex=sequence;
    state.emitters.resize(a.emitters.size());std::uint64_t particles=0,segments=0;
    for(std::size_t i=0;i<a.emitters.size();++i){
        auto& s=state.emitters[i];s.random=Mix(id)^Mix(generation)^Mix(i+1);s.quota=quotas[i];
        Require(s.quota<=limits.perEmitter&&limits.perEmitter<=4096,"PARTICLE_QUOTA","单发射器配额超限。");
        if(a.emitters[i].kind==Kind::Particle2){particles+=s.quota;s.particles.reserve(s.quota);}
        else{segments+=s.quota;s.segments.reserve(s.quota);}
    }
    Require(particles<=limits.particles&&segments<=limits.ribbonSegments&&limits.particles<=65536&&limits.ribbonSegments<=32768,
        "PARTICLE_SCENE_BUDGET","粒子或条带段场景配额超限。");
    return Result<State>::Success(std::move(state));
}catch(const Error& e){return Result<State>::Failure("Render.Particles",e.code,e.what());}
catch(const std::bad_alloc&){return Result<State>::Failure("Render.Particles","PARTICLE_ALLOCATION","粒子预热分配失败。");}
namespace {
std::uint64_t Bursts(const Emitter& e,const Asset& a,const Timeline& t,std::uint64_t begin,std::uint64_t end) {
    const auto& track=e.tracks[static_cast<std::size_t>(Parameter::Rate)];
    if(track.keys.empty())return begin==0 ? static_cast<std::uint64_t>(std::max(0.0f,e.rate)) : 0;
    std::uint64_t count=0;
    const bool global=track.globalSequence!=UINT32_MAX;
    const auto lo=global?0u:t.start;
    const auto hi=global?a.globalSequences.at(track.globalSequence):t.end;
    const std::uint64_t period=std::uint64_t{hi}-lo;
    const bool loops=global||t.loop;
    const auto offset=global&&period&&!t.boundedWorldPreview?CheckedGlobalTime(t,0):0;
    // 逐键算跨越次数，不用随机/浮点触发burst；包括t=0，后续左开右闭。
    // 非全局轨迹只数本动作的原版区间内的键（与取值同一切分）；区间外或不在动作时间内的键不触发。
    std::span<const Key> keys(track.keys);
    if(!global&&!track.ranges.empty()) {
        Require(t.sequenceIndex<track.ranges.size(),"PARTICLE_SEQUENCE_INVALID","粒子动作序号超出 SEQS。");
        const auto range=track.ranges[t.sequenceIndex];keys=keys.subspan(range.first,range.count);
    }
    for(const auto& k:keys) {
        if(k.frame<static_cast<std::int64_t>(lo)||k.frame>static_cast<std::int64_t>(hi))continue;
        const auto key=static_cast<std::uint64_t>(k.frame-lo);
        const auto through=[&](std::uint64_t at)->std::uint64_t {
            if(at<key)return 0;
            return loops&&period ? (at-key)/period+1 : 1;
        };
        std::uint64_t n=0;
        if (global&&period&&t.boundedWorldPreview) {
            // 只计本次实际跨越的global键；负的历史起点不参与模拟。
            const auto earliest=t.elapsedReferenceMs>t.globalReferenceMs ? t.elapsedReferenceMs-t.globalReferenceMs : 0;
            const auto finish=CheckedGlobalTime(t,end);
            const bool startsBeforeGlobalZero=begin<earliest || (begin==0&&earliest==0&&t.globalReferenceMs==t.elapsedReferenceMs);
            n=through(finish)-(startsBeforeGlobalZero ? 0 : through(CheckedGlobalTime(t,begin)));
        } else n=through(end+offset)-(begin==0?through(offset==0?0:offset-1)-(offset==0&&key==0?1u:0u):through(begin+offset));
        if (!n) continue;
        Require(n<=1000001 && std::isfinite(k.value[0]) && k.value[0]>=0 && k.value[0]<=1000000,
            "PARTICLE_BURST_BUDGET","squirt原始键值或触发次数超出预算，未截小发射量。");
        // 预览仍按向下取整解释有限非负数量；先验证原值，禁止夹值后少记suppressed。
        const auto amount=static_cast<std::uint64_t>(k.value[0]);count+=n*amount;
    }
    return count;
}
}
Status Advance(const Asset& a,State& s,const Timeline& t,std::uint64_t target,const AnchorSampler& sample,const Limits& limits) try {
    Require(sample&&s.emitters.size()==a.emitters.size()&&s.sequenceIndex==t.sequenceIndex&&target>=s.timeMs&&target<=86400000,
        "PARTICLE_TIME_INVALID","粒子时钟、动作或发射器身份无效；倒退须显式重放。");
    const auto phase=GlobalPhase(t);if (!phase.Succeeded()) return {phase.diagnostics};
    const auto globalStart=*phase.value;
    (void)CheckedGlobalTime(t,target);
    Require(!s.globalStartMs || (*s.globalStartMs==globalStart&&s.boundedWorldPreview==t.boundedWorldPreview),"PARTICLE_TIMELINE_CHANGED",
        "粒子全局相位映射已改变，须重新创建并显式回放。");
    const auto stop=target-target%FixedStepMs;
    Require(s.tickMs<=s.timeMs && s.tickMs<=stop,"PARTICLE_TIME_INVALID","粒子已提交时间网格无效。");
    Require((stop-s.tickMs)/FixedStepMs<=limits.maxReplaySteps&&limits.maxReplaySteps<=6000,
        "PARTICLE_REPLAY_BUDGET","单次粒子重放超过固定步预算，未截断时间冒充完成。");
    std::vector<Anchor> anchors(a.emitters.size());
    s.globalStartMs=globalStart;s.boundedWorldPreview=t.boundedWorldPreview;
    while(s.tickMs<stop){
        const auto next=s.tickMs+FixedStepMs;const auto status=sample(next,anchors);if(!status.Succeeded())return status;
        constexpr float dt=static_cast<float>(FixedStepMs)*0.001f;
        for(std::size_t i=0;i<a.emitters.size();++i){
            const auto& e=a.emitters[i];auto& q=s.emitters[i];const auto& anchor=anchors[i];
            for(const auto f:anchor.model)Require(std::isfinite(f)&&std::abs(f)<=1e7f,"PARTICLE_ANCHOR_INVALID","粒子节点矩阵无效。");
            for(auto& p:q.particles){p.position=Add(p.position,Mul(p.velocity,dt));p.position[2]-=0.5f*p.gravity*dt*dt;p.velocity[2]-=p.gravity*dt;p.age+=dt;}
            std::erase_if(q.particles,[](const Particle& p){return p.age+1e-6f>=p.life;});
            for(auto& p:q.segments){const float old=p.age;p.age+=dt;const auto dz=0.5f*e.gravity*(p.age*p.age-old*old);p.above[2]-=dz;p.below[2]-=dz;}
            std::erase_if(q.segments,[&](const Segment& p){return p.age+1e-6f>=e.life;});
            const float visible=Value(e,Parameter::Visibility,1,a,t,next);
            if(visible<=0||e.life<=0){q.accumulator=0;continue;}
            const auto rate=Value(e,Parameter::Rate,e.rate,a,t,next);
            Require(rate>=0&&rate<=1000000,"PARTICLE_RATE_BUDGET","粒子发射率超出预算。");
            std::uint64_t desired=0;
            if(e.kind==Kind::Particle2&&e.squirt)desired=Bursts(e,a,t,s.tickMs,next);
            else {q.accumulator+=static_cast<double>(rate)*static_cast<double>(FixedStepMs)/1000.0;
                desired=static_cast<std::uint64_t>(q.accumulator+1e-9);q.accumulator=std::max(0.0,q.accumulator-static_cast<double>(desired));}
            const auto active=e.kind==Kind::Particle2?q.particles.size():q.segments.size();
            const auto allowed=std::min<std::uint64_t>(desired,q.quota-std::min<std::size_t>(active,q.quota));
            q.dropped+=desired-allowed;
            const auto worldPoint=[&](const Vec3& p){return Transform(t.world,Transform(anchor.model,p));};
            if(e.kind==Kind::Particle2){
                const float speed=Value(e,Parameter::Speed,e.speed,a,t,next),variation=Value(e,Parameter::Variation,e.variation,a,t,next);
                const float latitude=Value(e,Parameter::Latitude,e.latitude,a,t,next),gravity=Value(e,Parameter::Gravity,e.gravity,a,t,next);
                const float length=Value(e,Parameter::Length,e.length,a,t,next),width=Value(e,Parameter::Width,e.width,a,t,next);
                for(std::uint64_t n=0;n<allowed;++n){
                    Particle p;p.life=e.life;p.gravity=gravity;
                    // 1.27a：6F77DB20 的 PRE2 分支先把节点矩阵左乘绕本地 Z 的 +90°（6F1AAE00(π/2,(0,0,1))），
                    // 6F19F640 在此坐标系取 X=宽(KP2W)×随机、Y=长(KP2N)×随机；折回节点本地为 (-y, x, 0)，即长度沿节点 X。
                    const auto x=(Random(q.random)-0.5f)*width,y=(Random(q.random)-0.5f)*length;
                    const auto along=(e.flags&0x20000u)?0.0f:y;
                    // 1.27a 6F13E283 将 MDX 0x80000 映射到运行时 0x200。
                    // 6F19F640 对该标志保留发射器局部状态；6F19DFF0 绘制时才组合当前矩阵。
                    // 此处已折入 +90°，但不折入 pivot/骨骼；它们由 Build 在当前帧各应用一次。
                    const bool modelSpace=(e.flags&0x80000u)!=0;
                    p.position=modelSpace?Vec3{-along,x,0}:worldPoint(Add(anchor.pivot,{-along,x,0}));
                    const auto azimuth=Random(q.random)*2*std::numbers::pi_v<float>;
                    const auto angle=Random(q.random)*latitude*std::numbers::pi_v<float>/180.0f;
                    const auto v=speed*(1+(Random(q.random)*2-1)*variation);
                    // 速度与位置同属该旋转后的发射器坐标系：(a,b,c) -> 节点本地 (-b,a,c)。
                    p.velocity={-v*std::sin(angle)*std::sin(azimuth),v*std::sin(angle)*std::cos(azimuth),v*std::cos(angle)};
                    if(!modelSpace)p.velocity=Transform(t.world,Transform(anchor.model,p.velocity,false),false);
                    Require(Finite(p.position)&&Finite(p.velocity),"PARTICLE_NUMBER_BUDGET","粒子发射位置/速度超限。");q.particles.push_back(p);++q.spawned;
                }
            }else{
                const auto above=Value(e,Parameter::Above,e.above,a,t,next),below=Value(e,Parameter::Below,e.below,a,t,next);
                const auto color=Sample(e.tracks[static_cast<std::size_t>(Parameter::Color)],e.ribbonColor,a,t,next);
                const auto alpha=Value(e,Parameter::Alpha,e.alpha,a,t,next);
                const auto slot=Value(e,Parameter::TextureSlot,static_cast<float>(e.textureSlot),a,t,next);
                Require(slot>=0&&slot<e.rows*e.columns,"PARTICLE_SLOT_INVALID","条带贴图槽越界。");
                for(std::uint64_t n=0;n<allowed;++n){Segment p;
                    p.above=worldPoint(Add(anchor.pivot,{0,0,above}));p.below=worldPoint(Add(anchor.pivot,{0,0,-below}));
                    p.color={color[0],color[1],color[2],alpha};p.cell=static_cast<std::uint32_t>(slot);q.segments.push_back(p);++q.spawned;}
            }
        }
        s.tickMs=next;
    }
    s.timeMs=target;return {};
}catch(const Error& e){return Failure(e);}
catch(const std::exception&){return Status::Failure("Render.Particles","PARTICLE_SIMULATION_FAILED","粒子模拟失败，候选状态不可发布。");}
Status FastForwardFrozen(const Asset& a,State& s,const Timeline& t,std::uint64_t target) try {
    Require(s.emitters.size()==a.emitters.size()&&s.sequenceIndex==t.sequenceIndex&&target>=s.timeMs&&target<=86400000,
        "PARTICLE_TIME_INVALID","冻结粒子时钟、动作或发射器身份无效。");
    const auto phase=GlobalPhase(t);if (!phase.Succeeded()) return {phase.diagnostics};
    const auto globalStart=*phase.value;
    (void)CheckedGlobalTime(t,target);
    Require(!s.globalStartMs || (*s.globalStartMs==globalStart&&s.boundedWorldPreview==t.boundedWorldPreview),"PARTICLE_TIMELINE_CHANGED",
        "冻结粒子全局相位已改变，须显式重新创建。");
    const auto stop=target-target%FixedStepMs;
    Require(s.tickMs<=s.timeMs&&s.tickMs<=stop,"PARTICLE_TIME_INVALID","冻结粒子时间网格无效。");
    const float dt=static_cast<float>(stop-s.tickMs)*0.001f;
    for(std::size_t i=0;i<a.emitters.size();++i) {
        auto& q=s.emitters[i];const auto& e=a.emitters[i];
        // 先剔除寿命已尽对象，避免对长期冻结的无效坐标作巨大外推。
        std::erase_if(q.particles,[&](const Particle& p){return p.age+dt+1e-6f>=p.life;});
        for(auto& p:q.particles) {
            p.position=Add(p.position,Mul(p.velocity,dt));p.position[2]-=0.5f*p.gravity*dt*dt;
            p.velocity[2]-=p.gravity*dt;p.age+=dt;
            Require(Finite(p.position)&&Finite(p.velocity),"PARTICLE_NUMBER_BUDGET","冻结粒子快进后的数值超限。");
        }
        std::erase_if(q.segments,[&](const Segment& p){return p.age+dt+1e-6f>=e.life;});
        for(auto& p:q.segments) {
            const float old=p.age;p.age+=dt;const auto dz=0.5f*e.gravity*(p.age*p.age-old*old);
            p.above[2]-=dz;p.below[2]-=dz;
            Require(Finite(p.above)&&Finite(p.below),"PARTICLE_NUMBER_BUDGET","冻结条带快进后的数值超限。");
        }
        // 保留小数发射累计及随机身份；跳过区间不构造历史发射。
    }
    s.globalStartMs=globalStart;s.boundedWorldPreview=t.boundedWorldPreview;s.tickMs=stop;s.timeMs=target;return {};
}catch(const Error& e){return Failure(e);}
catch(const std::exception&){return Status::Failure("Render.Particles","PARTICLE_SIMULATION_FAILED","冻结粒子快进失败，候选状态不可发布。");}
Counts Count(const State& s) noexcept {
    Counts c;for(const auto& e:s.emitters){c.particles+=static_cast<std::uint32_t>(e.particles.size());c.ribbonSegments+=static_cast<std::uint32_t>(e.segments.size());c.spawned+=e.spawned;c.suppressed+=e.dropped;c.budgetLimited|=e.dropped!=0;}return c;
}
} // namespace War3Map::Render::Particles
