#pragma once
#include "ParticleTypes.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace War3Map::Render::Particles {
struct Error : std::runtime_error {
    std::string code;
    Error(std::string id, std::string text) : std::runtime_error(std::move(text)), code(std::move(id)) {}
};
inline void Require(bool yes, const char* id, const char* text) { if (!yes) throw Error(id,text); }
inline std::uint64_t CheckedGlobalTime(const Timeline& t, std::uint64_t elapsed) {
    Require(elapsed<=MaxClockMs && t.elapsedReferenceMs<=MaxClockMs && t.globalReferenceMs<=MaxClockMs,
        "PARTICLE_TIME_INVALID","粒子显式时钟超过精确整数范围。");
    if (elapsed >= t.elapsedReferenceMs) {
        const auto delta=elapsed-t.elapsedReferenceMs;
        Require(delta<=MaxClockMs-t.globalReferenceMs,"PARTICLE_TIME_INVALID","粒子全局时钟推进超限。");
        return t.globalReferenceMs+delta;
    }
    const auto delta=t.elapsedReferenceMs-elapsed;
    Require(delta<=t.globalReferenceMs,"PARTICLE_GLOBAL_REPLAY_UNSUPPORTED",
        "粒子从零回放需要负的历史全局时间，当前不支持；未改写模型时钟。");
    return t.globalReferenceMs-delta;
}
inline Vec3 Add(const Vec3& a,const Vec3& b) { return {a[0]+b[0],a[1]+b[1],a[2]+b[2]}; }
inline Vec3 Sub(const Vec3& a,const Vec3& b) { return {a[0]-b[0],a[1]-b[1],a[2]-b[2]}; }
inline Vec3 Mul(const Vec3& a,float b) { return {a[0]*b,a[1]*b,a[2]*b}; }
inline float Dot(const Vec3& a,const Vec3& b) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }
inline Vec3 Cross(const Vec3& a,const Vec3& b) { return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]}; }
inline Vec3 Unit(const Vec3& a,const Vec3& fallback) {
    const auto q=Dot(a,a); return std::isfinite(q)&&q>1e-20f ? Mul(a,1.0f/std::sqrt(q)) : fallback;
}
inline Vec3 Transform(const Matrix& m,const Vec3& v,bool point=true) {
    Vec3 out{};
    for(std::size_t r=0;r<3;++r) out[r]=m[r*4]*v[0]+m[r*4+1]*v[1]+m[r*4+2]*v[2]+(point?m[r*4+3]:0.0f);
    return out;
}
inline bool Finite(const Vec3& v) { return std::all_of(v.begin(),v.end(),[](float f){return std::isfinite(f)&&std::abs(f)<=1e7f;}); }
inline float Value(const Emitter& e,Parameter p,float fallback,const Asset& a,const Timeline& t,std::uint64_t ms) {
    const auto v=Sample(e.tracks[static_cast<std::size_t>(p)],{fallback,0,0},a,t,ms)[0];
    Require(std::isfinite(v)&&std::abs(v)<=1e7f,"PARTICLE_TRACK_NUMBER","粒子轨迹采样超过有限值预算。"); return v;
}
inline std::uint64_t Mix(std::uint64_t x) {
    x=(x^(x>>30))*0xbf58476d1ce4e5b9ull; x=(x^(x>>27))*0x94d049bb133111ebull; return x^(x>>31);
}
inline float Random(std::uint64_t& s) { s+=0x9e3779b97f4a7c15ull; return static_cast<float>(Mix(s)>>40)/16777216.0f; }
inline Status Failure(const Error& e) { return Status::Failure("Render.Particles",e.code,e.what()); }
} // namespace War3Map::Render::Particles
