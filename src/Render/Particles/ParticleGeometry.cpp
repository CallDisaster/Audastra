#include "ParticleMath.h"

namespace War3Map::Render::Particles {
namespace {
std::array<float,4> Tint(const Emitter& e,float life,float& scale,float& phase,bool& decay) {
    const float f=std::clamp(life,0.0f,1.0f);decay=f>=e.middle;
    const std::size_t a=decay?1:0,b=a+1;
    phase=decay?(e.middle>=1?1:(f-e.middle)/(1-e.middle)):(e.middle<=0?1:f/e.middle);
    phase=std::clamp(phase,0.0f,1.0f);scale=e.scales[a]+(e.scales[b]-e.scales[a])*phase;
    std::array<float,4> out{};for(std::size_t i=0;i<3;++i)out[i]=e.colors[a][i]+(e.colors[b][i]-e.colors[a][i])*phase;
    out[3]=e.alphas[a]+(e.alphas[b]-e.alphas[a])*phase;return out;
}
std::uint32_t Cell(const Interval& i,float phase) {
    const float q=phase*static_cast<float>(i.repeat);
    const float f=phase>=1?1:q-std::floor(q);
    return i.first+std::min(i.last-i.first,static_cast<std::uint32_t>(f*static_cast<float>(i.last-i.first+1)));
}
void Quad(Geometry& g,const std::array<Vec3,4>& p,const std::array<std::array<float,4>,4>& c,
    std::uint32_t cell,const Emitter& e,std::uint32_t budget,float left=0,float right=1) {
    Require(g.vertices.size()<=budget&&6<=budget-g.vertices.size(),"PARTICLE_VERTEX_BUDGET","粒子动态顶点超过预算。");
    const float u=static_cast<float>(cell%e.columns),v=static_cast<float>(cell/e.columns);
    const auto uv=std::array<std::array<float,2>,4>{{{(u+left)/e.columns,v/e.rows},{(u+right)/e.columns,v/e.rows},
        {(u+right)/e.columns,(v+1)/e.rows},{(u+left)/e.columns,(v+1)/e.rows}}};
    for(const auto i:{0u,1u,2u,0u,2u,3u}){
        Require(Finite(p[i]),"PARTICLE_NUMBER_BUDGET","粒子绘制坐标非有限或超限。");auto color=c[i];
        for(auto& f:color){Require(std::isfinite(f),"PARTICLE_COLOR_INVALID","粒子颜色非有限。");f=std::clamp(f,0.0f,1.0f);}
        g.vertices.push_back({p[i],uv[i],color});
    }
}
}
Result<Geometry> Build(const Asset& a,const State& s,const Timeline& t,const Vec3& eye,const Vec3& up,std::uint32_t maxVertices) try {
    Require(s.emitters.size()==a.emitters.size()&&maxVertices<=1048576&&Finite(eye)&&Finite(up),"PARTICLE_GEOMETRY_INPUT","粒子绘制输入无效。");
    Geometry g;const float remainder=static_cast<float>(s.timeMs-s.tickMs)*0.001f;
    std::uint64_t capacity=0;
    for(std::size_t i=0;i<s.emitters.size();++i) capacity+=s.emitters[i].particles.size()*(a.emitters[i].headOrTail==2?12ull:6ull)
        +(s.emitters[i].segments.empty()?0ull:static_cast<std::uint64_t>(s.emitters[i].segments.size()-1)*6);
    g.vertices.reserve(static_cast<std::size_t>(std::min<std::uint64_t>(capacity,maxVertices)));
    g.draws.reserve(a.emitters.size());
    for(std::uint32_t index=0;index<a.emitters.size();++index){
        const auto& e=a.emitters[index];const auto& q=s.emitters[index];const auto first=static_cast<std::uint32_t>(g.vertices.size());
        for(const auto& p:q.particles){
            const float age=p.age+remainder;if(age>=p.life||p.life<=0)continue;
            auto center=Add(p.position,Mul(p.velocity,remainder));center[2]-=0.5f*p.gravity*remainder*remainder;
            auto velocity=p.velocity;velocity[2]-=p.gravity*remainder;
            if(e.flags&0x80000u){center=Transform(t.world,center);velocity=Transform(t.world,velocity,false);}
            float scale=1,phase=0;bool decay=false;const auto c=Tint(e,age/p.life,scale,phase,decay);
            Vec3 z=Unit(Sub(eye,center),{0,0,1}),x=Unit(Cross(up,z),{1,0,0}),y=Unit(Cross(z,x),{0,1,0});
            if(e.flags&0x100000u){x={1,0,0};y={0,1,0};if(e.flags&0x80000u){x=Unit(Transform(t.world,x,false),{1,0,0});y=Unit(Transform(t.world,y,false),{0,1,0});}}
            x=Mul(x,scale);y=Mul(y,scale);
            if(e.headOrTail!=1)Quad(g,{{Sub(Sub(center,x),y),Add(Sub(center,y),x),Add(Add(center,x),y),Add(Sub(center,x),y)}},
                {{c,c,c,c}},Cell(e.intervals[decay?1:0],phase),e,maxVertices);
            if(e.headOrTail!=0){const auto end=Sub(center,Mul(velocity,e.tailLength));
                const auto side=Mul(Unit(Cross(Sub(center,end),z),Unit(x,{1,0,0})),scale);
                Quad(g,{{Sub(center,side),Sub(end,side),Add(end,side),Add(center,side)}},{{c,c,c,c}},
                    Cell(e.intervals[decay?3:2],phase),e,maxVertices);}
        }
        if(q.segments.size()>1)for(std::size_t i=1;i<q.segments.size();++i){
            const auto& p=q.segments[i-1];const auto& n=q.segments[i];if(p.age+remainder>=e.life)continue;
            const float dp=0.5f*e.gravity*((p.age+remainder)*(p.age+remainder)-p.age*p.age);
            const float dn=0.5f*e.gravity*((n.age+remainder)*(n.age+remainder)-n.age*n.age);
            Quad(g,{{Sub(p.above,{0,0,dp}),Sub(n.above,{0,0,dn}),Sub(n.below,{0,0,dn}),Sub(p.below,{0,0,dp})}},
                {{p.color,n.color,n.color,p.color}},n.cell,e,maxVertices,
                std::clamp((p.age+remainder)/e.life,0.0f,1.0f),std::clamp((n.age+remainder)/e.life,0.0f,1.0f));
        }
        const auto count=static_cast<std::uint32_t>(g.vertices.size())-first;if(count)g.draws.push_back({first,count,index,e.filter});
    }
    return Result<Geometry>::Success(std::move(g));
}catch(const Error& e){return Result<Geometry>::Failure("Render.Particles",e.code,e.what());}
catch(const std::bad_alloc&){return Result<Geometry>::Failure("Render.Particles","PARTICLE_ALLOCATION","粒子绘制缓冲分配失败。");}
} // namespace War3Map::Render::Particles
