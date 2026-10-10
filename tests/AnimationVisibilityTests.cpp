#include "../src/Render/Animation/AnimationVisibility.h"
#include "AnimationTestModel.h"
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <numbers>
#include <stdexcept>

namespace {
using namespace War3Map;
using namespace War3Map::Render;
constexpr std::uint64_t Budget=64ull*1024*1024;
void Require(bool value,const char* message) { if (!value) throw std::runtime_error(message); }
void Good(const Status& status) {
    if (!status.Succeeded()) throw std::runtime_error(status.diagnostics.empty() ? "empty failure" : status.diagnostics.front().code);
}
struct Fixture {
    AnimatedModelScene scene;
    std::vector<AnimationAsset> assets{1};
    std::vector<std::uint32_t> requests;
    std::shared_ptr<AnimationVisibilityIndex> index;
    AnimationFrame frame;
    explicit Fixture(std::size_t count=1) {
        scene.sceneVersion=31; scene.generation=7; scene.specs.resize(count);
        frame.sceneVersion=31; frame.states.resize(count); requests.resize(count);
        auto model=std::make_shared<MdxModel>(); model->version=800;
        assets[0].parsed=model; assets[0].visibilityBounds={{-1,-1,-1,1,1,1},0,true};
        for (std::size_t i=0;i<count;++i) {
            scene.specs[i].identity={i+1,7}; frame.states[i].identity=scene.specs[i].identity;
            frame.states[i].transform={1,0,0,0,0,1,0,0,0,0,1,0};
        }
        frame.worldView=AnimationView{{sizeof(War3RenderCameraV2),0,0,0,100,0,0},1000,1000,0};
    }
    void Position(std::size_t i,float x,float y,float z) {
        frame.states[i].transform[3]=x; frame.states[i].transform[7]=y; frame.states[i].transform[11]=z;
    }
    void Prepare(std::span<const AnimationEdit> edits={}) {
        frame.requestVisibility.clear(); frame.offscreenInstances=frame.visibilityUnknownInstances=0;
        frame.timings.visibilityNodesVisited=frame.timings.visibilityBoundsTests=0;
        Good(PrepareAnimationVisibility(index,scene,assets,requests,frame,edits,Budget));
    }
};

void PerspectiveAndPadding() {
    Fixture f(7);
    f.Position(1,0,55,0); f.Position(2,0,0,55); f.Position(3,150,0,0);
    f.Position(4,-1100200,0,0); f.Position(5,99.5f,0,0); f.Position(6,0,40.5f,0);
    f.Prepare();
    Require(f.frame.requestVisibility==std::vector<std::uint8_t>({1,0,0,0,0,1,1}),"perspective/near/far/intersecting bounds mask");
    Require(f.frame.offscreenInstances==4 && f.frame.visibilityUnknownInstances==0,"outside and unknown counts");
    f.frame.worldView->physicalWidth=2000; f.Prepare();
    Require(f.frame.requestVisibility[1]==1 && f.frame.requestVisibility[2]==0,"physical aspect did not change horizontal frustum");
    f.Position(4,-1000200,0,0); AnimationEdit distant; distant.identity=f.frame.states[4].identity;
    distant.transform=f.frame.states[4].transform; f.Prepare(std::span(&distant,1));
    Require(f.frame.requestVisibility[4]==1,"nominal far plane clipped renderer float depth range");
    f.frame.worldView->physicalWidth=1000; f.frame.worldView->conservativeMargin=64; f.Prepare();
    Require(f.frame.requestVisibility[1]==1 && f.frame.requestVisibility[2]==1,"world-plane padding rejected edge envelope");
    f.frame.worldView->conservativeMargin=0; f.frame.worldView->camera.yawRadians=std::numbers::pi_v<float>/2; f.Prepare();
    Require(f.frame.requestVisibility[1]==1,"yaw did not rotate true view frustum");
    f.frame.worldView->camera.pitchRadians=std::numbers::pi_v<float>/2;
    Require(!CheckAnimationView(*f.frame.worldView).Succeeded(),"singular camera accepted");
    f.frame.worldView->camera.pitchRadians=0; f.frame.worldView->physicalHeight=0;
    Require(!CheckAnimationView(*f.frame.worldView).Succeeded(),"zero physical viewport accepted");
    f.frame.worldView->physicalHeight=1000; f.frame.worldView->conservativeMargin=std::numeric_limits<float>::quiet_NaN();
    Require(!CheckAnimationView(*f.frame.worldView).Succeeded(),"nonfinite padding accepted");
}
void BoundsAndUnknowns() {
    Fixture f(1); f.Position(0,0,150,0);
    f.assets[0].visibilityBounds.available=false; f.Prepare();
    Require(f.frame.requestVisibility[0]==1 && f.frame.visibilityUnknownInstances==1 && f.frame.offscreenInstances==0,
        "unknown bounds must explicitly admit far/outside instance");
    f.index.reset(); f.assets[0].visibilityBounds={{-1,-200,-1,1,200,1},0,true}; f.Prepare();
    Require(f.frame.requestVisibility[0]==1 && f.frame.visibilityUnknownInstances==0,"intersecting broad envelope rejected by center");
    f.index.reset(); f.assets[0].visibilityBounds={{-1,-1,-1,1,1,1},0,true}; f.Position(0,0,45,0);
    f.frame.states[0].transform[0]=-4; f.frame.states[0].transform[5]=4; f.Prepare();
    Require(f.frame.requestVisibility[0]==1,"negative/nonuniform scale clipped conservative corners");
    auto model=std::make_shared<MdxModel>(); model->version=800; model->pivots={{{0,0,0}}};
    model->sequences.push_back({"Stand",0,1000,0});
    MdxNode node; node.objectId=0; MdxNodeTrack<3> track; track.interpolation=MdxInterpolation::Linear;
    track.keys.push_back({100,{0,0,0},{},{}}); track.keys.push_back({900,{10,0,0},{},{}}); node.translation=track;
    model->helpers.push_back(node); f.assets[0].parsed=model; f.Position(0,5000,0,0); f.index.reset(); f.Prepare();
    Require(f.frame.requestVisibility[0]==1 && f.frame.visibilityUnknownInstances==1,"unclosed TRS seam trusted SEQS extent");
    model->helpers[0].translation.reset(); model->helpers[0].scaling=track;
    model->helpers[0].scaling->keys[0].frame=0; model->helpers[0].scaling->keys[1].frame=1000;
    model->helpers[0].scaling->keys[1].value={1e10f,1,1}; f.index.reset(); f.Prepare();
    Require(f.frame.visibilityUnknownInstances==1,"extreme node scale fabricated finite envelope");
}
void DeclaredSequenceExtent() {
    auto bytes=AnimationTest::Bytes();
    std::size_t sequence=0;
    for (std::size_t at=4;at+8<=bytes.size();) {
        std::uint32_t size=0; std::memcpy(&size,bytes.data()+at+4,4);
        if (std::memcmp(bytes.data()+at,"SEQS",4)==0) { sequence=at+8; break; }
        at+=8+size;
    }
    Require(sequence!=0,"fixture sequence missing");
    const float declared=750; std::memcpy(bytes.data()+sequence+124,&declared,4);
    auto parsed=ParseMdx(bytes); Require(parsed.Succeeded(),"fixture MDX parse");
    AnimationAsset asset; asset.parsed=std::make_shared<const MdxModel>(std::move(*parsed.value));
    ReadAnimationVisibilityBounds(asset,bytes);
    Require(asset.visibilityBounds.available && asset.visibilityBounds.box[4]>=750,"SEQS declared extent dropped");
    Fixture f; f.assets[0]=asset; f.Position(0,0,600,0); f.Prepare();
    // 原夹具只向+Y声明750，而实例也移到+Y600；世界盒最小Y仍大于500，确实离屏。
    // 先固定这一正确剔除，再补齐负向extent制造真正相交的声明盒，保持原放行断言。
    Require(f.frame.requestVisibility[0]==0 && f.frame.visibilityUnknownInstances==0,"separate positive-only extent incorrectly admitted");
    const float negative=-750; std::memcpy(bytes.data()+sequence+112,&negative,4);
    ReadAnimationVisibilityBounds(asset,bytes);
    Require(asset.visibilityBounds.available && asset.visibilityBounds.box[1]<=-750 && asset.visibilityBounds.box[4]>=750,
        "SEQS negative or positive declared extent dropped");
    f.assets[0]=asset; f.index.reset(); f.Prepare();
    Require(f.frame.requestVisibility[0]==1 && f.frame.visibilityUnknownInstances==0,"large sequence extent did not admit overlap");
    const float bad=std::numeric_limits<float>::quiet_NaN(); std::memcpy(bytes.data()+sequence+124,&bad,4);
    ReadAnimationVisibilityBounds(asset,bytes); Require(!asset.visibilityBounds.available,"nonfinite declared extent accepted");
}
void ParticleEnvelope() {
    Fixture f; f.scene.particlesEnabled=true;
    auto model=std::make_shared<MdxModel>(); model->version=800; model->pivots={{{0,0,0}}};
    MdxNode node; node.kind=MdxNodeKind::ParticleEmitter2; model->otherNodes.push_back(node); f.assets[0].parsed=model;
    auto particles=std::make_shared<Particles::Asset>(); Particles::Emitter emitter;
    emitter.nodeId=0; emitter.speed=100; emitter.life=2; emitter.width=20; emitter.scales={1,1,1}; particles->emitters.push_back(emitter);
    f.assets[0].particles=particles; f.assets[0].particleCapability.available=true; f.Position(0,0,150,0); f.Prepare();
    Require(f.frame.requestVisibility[0]==1 && f.frame.visibilityUnknownInstances==0,"lifetime speed envelope clipped visible particle reach");
    f.Position(0,0,600,0); AnimationEdit edit; edit.identity=f.frame.states[0].identity; edit.transform=f.frame.states[0].transform;
    f.Prepare(std::span(&edit,1)); Require(f.frame.requestVisibility[0]==0,"bounded emitter admitted arbitrarily distant source");
    particles->emitters[0].gravity=500; f.Position(0,0,0,600);
    f.frame.states[0].transform[0]=f.frame.states[0].transform[5]=f.frame.states[0].transform[10]=0.01f;
    f.index.reset(); f.Prepare(); Require(f.frame.requestVisibility[0]==1,"world-space gravity shrunk with instance scale");
    particles->emitters[0].gravity=0; particles->emitters[0].scales={200,200,200}; f.Position(0,0,150,0);
    f.index.reset(); f.Prepare(); Require(f.frame.requestVisibility[0]==1,"billboard world size shrunk with instance scale");
}
void CachedTreeAndCancelledEdits() {
    Fixture f(4096);
    for (std::size_t i=0;i<f.frame.states.size();++i) f.Position(i,0,10000+static_cast<float>(i)*10,0);
    f.Prepare(); Require(f.frame.offscreenInstances==4096 && f.frame.timings.visibilityNodesVisited==1 &&
        f.frame.timings.visibilityBoundsTests==0,"outside cluster scanned every leaf instead of BVH pruning");
    const auto original=f.frame.states[0].transform;
    f.Position(0,0,0,0); AnimationEdit edit; edit.identity=f.frame.states[0].identity; edit.transform=f.frame.states[0].transform;
    f.Prepare(std::span(&edit,1));
    Require(f.frame.requestVisibility[0]==1 && f.frame.offscreenInstances==4095 && f.frame.timings.visibilityNodesVisited<100,
        "single transform edit did not refit cached tree");
    f.frame.states[0].transform=original; f.Prepare();
    Require(f.frame.requestVisibility[0]==0,"cancelled transformed candidate polluted accepted visibility");
    f.index.reset(); f.Position(0,0,0,0); edit.transform=f.frame.states[0].transform; f.Prepare(std::span(&edit,1));
    f.frame.states[0].transform=original; f.Prepare(); Require(f.frame.requestVisibility[0]==0,"first filtered candidate cancellation left stale leaf");
    auto small=PrepareAnimationVisibility(f.index,f.scene,f.assets,f.requests,f.frame,{},1024);
    Require(!small.Succeeded() && small.diagnostics.front().code=="ANIMATION_VISIBILITY_BUDGET","capacity failure silently disabled filtering");
    f.frame.worldView.reset(); f.Prepare();
    Require(f.frame.requestVisibility.empty() && !f.index,"legacy no-view call unexpectedly filtered or kept stale transform cache");
}
} // namespace
int main() try {
    PerspectiveAndPadding(); BoundsAndUnknowns(); DeclaredSequenceExtent(); ParticleEnvelope(); CachedTreeAndCancelledEdits();
    std::cout << "PASS animation visibility: perspective/aspect/padding, declared/dynamic/unknown bounds, particle envelope, BVH/refit/cancellation/budget\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n'; return 1;
}
