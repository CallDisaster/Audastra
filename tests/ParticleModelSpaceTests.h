#pragma once
#include "Particles/ParticleTypes.h"
#include <cmath>
#include <limits>
#include <stdexcept>

namespace ParticleModelSpaceTest {
using namespace War3Map::Render::Particles;
inline void Check(bool value,const char* text){if(!value)throw std::runtime_error(text);}
inline void Near(const Vec3& actual,const Vec3& expected,const char* text){
    for(std::size_t i=0;i<3;++i)Check(std::abs(actual[i]-expected[i])<0.0002f,text);
}
inline Vec3 Center(const Geometry& geometry,std::size_t first=0){
    const auto& a=geometry.vertices.at(first).position;const auto& b=geometry.vertices.at(first+2).position;
    return {(a[0]+b[0])*0.5f,(a[1]+b[1])*0.5f,(a[2]+b[2])*0.5f};
}
inline void Run(){
    Asset asset;Emitter emitter;emitter.flags=0x80000u|0x100000u;emitter.rate=1;emitter.squirt=1;
    emitter.life=2;emitter.speed=20;emitter.gravity=10;emitter.headOrTail=2;emitter.tailLength=1;
    asset.emitters.push_back(emitter);
    Timeline timeline;timeline.end=1000;timeline.world[3]=100;timeline.world[7]=200;timeline.world[11]=300;
    const std::array<std::uint32_t,1> quotas{8};
    auto created=Create(asset,1,1,0,quotas);Check(created.Succeeded(),"model-space create failed");
    auto state=*created.value;
    const auto birth=[](std::uint64_t,std::span<Anchor> anchors){
        anchors[0].model=Identity;anchors[0].model[3]=100;anchors[0].pivot={2,3,4};return War3Map::Status{};
    };
    Check(Advance(asset,state,timeline,25,birth).Succeeded(),"model-space simulation failed");
    Check(state.emitters[0].particles.size()==1,"squirt must leave exactly one live particle");
    Near(state.emitters[0].particles[0].position,{0,0,0.1995f},"model-space state baked birth bone/pivot/world");
    const auto savedPosition=state.emitters[0].particles[0].position;
    // 当前骨骼绕 X +90°，相对出生节点既平移又旋转；几何时刻还包含 5ms 余量。
    std::array<Anchor,1> current{};
    current[0].model={1,0,0,10,0,0,-1,20,0,1,0,30};current[0].pivot={2,3,4};
    auto geometry=Build(asset,state,timeline,{0,-100,100},{0,0,1},100,current);
    Check(geometry.Succeeded()&&geometry.value->vertices.size()==12,"model-space head/tail not built");
    Near(Center(*geometry.value),{112,215.701125f,333},"current bone/pivot/world/gravity not applied once");
    const auto& vertices=geometry.value->vertices;
    Vec3 head{},tail{};
    for(std::size_t i=0;i<3;++i){
        head[i]=(vertices[6].position[i]+vertices[11].position[i])*0.5f;
        tail[i]=(vertices[7].position[i]+vertices[8].position[i])*0.5f;
    }
    Near(head,{112,215.701125f,333},"tail head disconnected from particle center");
    Near(tail,{112,235.551125f,333},"tail velocity did not use current emitter rotation and local gravity");
    Vec3 axisX{},axisY{};
    for(std::size_t i=0;i<3;++i){axisX[i]=vertices[1].position[i]-vertices[0].position[i];axisY[i]=vertices[2].position[i]-vertices[1].position[i];}
    Near(axisX,{0,0,0},"zero XY velocity must keep a degenerate XYQuad X axis");
    Near(axisY,{0,0,0},"zero XY velocity must keep a degenerate XYQuad Y axis");
    current[0].model[3]+=10;
    const auto moved=Build(asset,state,timeline,{0,-100,100},{0,0,1},100,current);
    Check(moved.Succeeded(),"current-anchor redraw failed");
    Near(Center(*moved.value),{122,215.701125f,333},"existing particles did not follow latest bone");
    Near(state.emitters[0].particles[0].position,savedPosition,"drawing mutated particle state");
    Check(!Build(asset,state,timeline,{0,-100,100},{0,0,1}).Succeeded(),"missing current model-space anchor accepted");
    current[0].model[0]=std::numeric_limits<float>::quiet_NaN();
    Check(!Build(asset,state,timeline,{0,-100,100},{0,0,1},100,current).Succeeded(),"invalid current anchor accepted");

    // XYQuad 的轴来自不对称速度的 XY 分量，而非固定轴或再次 +90°。
    current[0].model={1,0,0,10,0,0,-1,20,0,1,0,30};
    state.timeMs=state.tickMs=0;state.emitters[0].particles[0].position={0,0,0};
    state.emitters[0].particles[0].velocity={3,4,5};
    geometry=Build(asset,state,timeline,{0,-100,100},{0,0,1},100,current);
    Check(geometry.Succeeded(),"velocity XYQuad failed");
    for(std::size_t i=0;i<3;++i){
        axisX[i]=geometry.value->vertices[1].position[i]-geometry.value->vertices[0].position[i];
        axisY[i]=geometry.value->vertices[2].position[i]-geometry.value->vertices[1].position[i];
    }
    Near(axisX,{1.2f,0,1.6f},"XYQuad velocity axis did not follow current bone");
    Near(axisY,{1.6f,0,-1.2f},"XYQuad perpendicular axis has wrong rotation or normalization");
    asset.emitters[0].flags=0x100000u;
    geometry=Build(asset,state,timeline,{0,-100,100},{0,0,1});
    Check(geometry.Succeeded(),"world XYQuad failed");
    for(std::size_t i=0;i<3;++i){
        axisX[i]=geometry.value->vertices[1].position[i]-geometry.value->vertices[0].position[i];
        axisY[i]=geometry.value->vertices[2].position[i]-geometry.value->vertices[1].position[i];
    }
    Near(axisX,{1.2f,1.6f,0},"world XYQuad ignored world velocity XY");
    Near(axisY,{1.6f,-1.2f,0},"world XYQuad perpendicular axis wrong");
    // 第一轴小于 2^-22 时两轴都保留原值；不是各自独立套一个 epsilon。
    state.emitters[0].particles[0].velocity={0x1p-12f,0,0};
    geometry=Build(asset,state,timeline,{0,-100,100},{0,0,1});
    Check(geometry.Succeeded(),"small XY velocity rejected");
    Check(std::abs(geometry.value->vertices[1].position[0]-geometry.value->vertices[0].position[0]-0x1p-11f)<1e-7f,
        "small XY first axis incorrectly normalized");
    Check(std::abs(geometry.value->vertices[2].position[1]-geometry.value->vertices[1].position[1]+0x1p-11f)<1e-7f,
        "small XY second axis incorrectly normalized");
    // 非退化第一轴与退化第二轴没有原版 fallback；沿有限值门禁拒绝，不能塞单位轴。
    asset.emitters[0].flags=0x80000u|0x100000u;state.emitters[0].particles[0].velocity={1,0,0};
    current[0].model={1,0,0,0,0,0,0,0,0,0,1,0};
    const auto degenerate=Build(asset,state,timeline,{0,-100,100},{0,0,1},100,current);
    Check(!degenerate.Succeeded()&&degenerate.diagnostics.front().code=="PARTICLE_NUMBER_BUDGET",
        "degenerate second XYQuad axis silently replaced with a fallback");

    // 普通粒子仍留在出生时的世界轨迹，后续骨骼旋转不移动它，也不旋转世界重力。
    asset.emitters[0].flags=0;
    created=Create(asset,1,1,0,quotas);Check(created.Succeeded(),"world particle create failed");state=*created.value;
    Check(Advance(asset,state,timeline,25,birth).Succeeded(),"world particle simulation failed");
    geometry=Build(asset,state,timeline,{0,-100,100},{0,0,1},100,current);
    Check(geometry.Succeeded(),"world particle incorrectly required a current anchor");
    Near(Center(*geometry.value),{202,203,304.298875f},"non-model-space birth trajectory changed");

    // LineEmitter 压掉长度轴，仍使用既有 +90° 发射坐标，且不烘焙出生骨骼。
    asset.emitters[0].flags=0x80000u|0x20000u;asset.emitters[0].length=100;
    asset.emitters[0].width=0;asset.emitters[0].speed=0;
    created=Create(asset,1,1,0,quotas);Check(created.Succeeded(),"line emitter create failed");state=*created.value;
    Check(Advance(asset,state,timeline,10,birth).Succeeded(),"line emitter simulation failed");
    Near(state.emitters[0].particles[0].position,{0,0,0},"line emitter length leaked into local position");
}
}
