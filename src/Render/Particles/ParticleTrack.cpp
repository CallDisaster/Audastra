#include "ParticleMath.h"
#include "War3RenderCore/GameKeyRanges.h"

namespace War3Map::Render::Particles {
Result<std::uint64_t> GlobalTimeAt(const Timeline& timeline, std::uint64_t elapsed) try {
    return Result<std::uint64_t>::Success(CheckedGlobalTime(timeline,elapsed));
} catch (const Error& e) {
    return Result<std::uint64_t>::Failure("Render.Particles",e.code,e.what());
}
Vec3 Sample(const Track& t,Vec3 fallback,const Asset& asset,const Timeline& timeline,std::uint64_t ms) {
    if(t.keys.empty())return fallback;
    std::uint64_t frame=0;std::uint32_t lo=timeline.start,hi=timeline.end;
    if(t.globalSequence!=UINT32_MAX) {
        Require(t.globalSequence<asset.globalSequences.size(),
            "PARTICLE_GLOBAL_INVALID","粒子全局序列不存在。");
        const auto globalTime=CheckedGlobalTime(timeline,ms);
        hi=asset.globalSequences[t.globalSequence];lo=0;frame=hi ? globalTime%hi : 0;
    } else {
        Require(hi>=lo,"PARTICLE_SEQUENCE_INVALID","粒子动作区间无效。");const auto length=std::uint64_t{hi}-lo;
        frame=lo+((timeline.loop&&length)?ms%length:std::min(ms,length));
    }
    const Key* left=nullptr;const Key* right=nullptr;double u=0;
    if(!t.ranges.empty()) {
        // 1.27a：本动作的原版区间（可含逆序键）内顺序查找；首键前与末键后接回另一端，与节点轨迹共用实现。
        Require(timeline.sequenceIndex<t.ranges.size(),"PARTICLE_SEQUENCE_INVALID","粒子动作序号超出 SEQS。");
        const auto range=t.ranges[timeline.sequenceIndex];
        const auto window=War3RenderCore::SelectGameKeyWindow(t.keys,range.first,range.count,lo,hi,
            static_cast<std::int64_t>(frame),range.ascending);
        if(window.kind==War3RenderCore::SourceOrderWindowKind::Empty)return fallback; // 空区间取默认值。
        left=&t.keys[window.left];
        if(window.kind==War3RenderCore::SourceOrderWindowKind::Value||window.span==0||t.interpolation==0)return left->value;
        right=&t.keys[window.right];u=static_cast<double>(window.offset)/static_cast<double>(window.span);
    } else {
        const auto first=std::lower_bound(t.keys.begin(),t.keys.end(),static_cast<std::int64_t>(lo),[](const Key& k,std::int64_t f){return k.frame<f;});
        const auto last=std::upper_bound(first,t.keys.end(),static_cast<std::int64_t>(hi),[](std::int64_t f,const Key& k){return f<k.frame;});
        if(first==last)return fallback; // 粒子轨迹只取当前动作；Death单键不污染Stand。
        auto after=std::upper_bound(first,last,static_cast<std::int64_t>(frame),[](std::int64_t f,const Key& k){return f<k.frame;});
        if(after==first)return first->value;
        left=&*std::prev(after);if(after==last||t.interpolation==0)return left->value;
        right=&*after;u=(static_cast<double>(frame)-left->frame)/(static_cast<double>(right->frame)-left->frame);
    }
    const auto& a=*left;const auto& b=*right;
    Vec3 out{};
    for(std::uint32_t c=0;c<t.width;++c){
        const double x=a.value[c],y=b.value[c],v=a.out[c],w=b.in[c],u2=u*u,u3=u2*u;
        double f=x+(y-x)*u;
        if(t.interpolation==2)f=(2*u3-3*u2+1)*x+(u3-2*u2+u)*v+(-2*u3+3*u2)*y+(u3-u2)*w;
        if(t.interpolation==3){const auto r=1-u;f=r*r*r*x+3*r*r*u*v+3*r*u2*w+u3*y;}
        out[c]=static_cast<float>(f);Require(std::isfinite(out[c]),"PARTICLE_TRACK_NUMBER","粒子轨迹结果非有限。");
    }
    return out;
}
} // namespace War3Map::Render::Particles
