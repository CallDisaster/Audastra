#include "../src/Render/Tools/MdxScan/ScanPreview.h"
#include "AnimationPreviewFixture.h"
#include "AnimationParticleContinuityTests.h"
#include "War3MapCore/Hash.h"
#include <iostream>
#include <stdexcept>

namespace {
using namespace War3Map;
using namespace War3Map::Render;
using namespace War3Map::Render::Scan;
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
PreviewModelReport Evaluate(std::vector<std::byte> bytes,Limits limits={}) {
    const auto sha=Sha256(bytes);Check(sha.Succeeded(),"fixture digest failed");
    AnimationResource model{std::move(bytes),"original-fixture",*sha.value};
    const auto reader=[&](std::string_view path,std::uint64_t maximum){
        auto resource=path=="sample.mdx"?model:AnimationResource{AnimationParticleContinuityTest::Texture(),"original-fixture",std::string(64,'a')};
        if(resource.bytes.size()>maximum)return Result<AnimationResource>::Failure("Test","SCAN_RESOURCE_LIMIT","fixture budget");
        return Result<AnimationResource>::Success(std::move(resource));
    };
    return EvaluatePreviewModel("sample.mdx",model,reader,limits,[]{});
}
void Run(){
    const auto original=Evaluate(AnimationTest::Bytes());
    Check(original.policies.size()==2&&!original.sha256.empty(),"scanner lost policy or identity");
    for(const auto& policy:original.policies){
        Check(policy.planned==6&&policy.sampled==6&&policy.state=="complete","two sequence sample coverage incomplete");
        Check(policy.animated==6&&policy.moving>0,"animated fixture did not reach production pose samples");
    }
    const auto inverse=Evaluate(AnimationPreviewFixture::InverseKeys());
    Check(inverse.policies[0].animated==0&&!inverse.policies[0].issues.empty(),"strict scan accepted descending node keys");
    Check(inverse.policies[1].sampled==6&&inverse.policies[1].animated==6&&inverse.policies[1].moving>0,
        "interactive scan lost source-order sequence evaluation");
    Check(AnimationLimits{}.playbackPolicy==AnimationPlaybackPolicy::StrictReplay,"scan changed strict defaults");
    Limits bounded;bounded.maxSamples=1;
    const auto partial=Evaluate(AnimationTest::Bytes(),bounded);
    for(const auto& policy:partial.policies)
        Check(policy.state=="uncovered"&&policy.sampled==1&&policy.planned==6&&policy.issues.contains("SCAN_SAMPLE_LIMIT"),
            "scan silently counted incomplete sample budget as complete");
    bounded=Limits{};bounded.maxNodes=1;
    const auto complex=Evaluate(AnimationTest::Bytes(),bounded);
    for(const auto& policy:complex.policies)
        Check(policy.state=="uncovered"&&policy.issues.contains("SCAN_MODEL_COMPLEXITY_LIMIT"),"complexity budget lost");
    const auto invalid=Evaluate(std::vector<std::byte>(8));
    for(const auto& policy:invalid.policies)Check(policy.state=="parse_rejected"&&!policy.issues.empty(),"bad magic accepted");
    Check(PreviewDiagnosticCategory("MDX_NODE_SOURCE_ORDER_PREVIEW")=="compatibility_notice","source-order notice treated as source error");
    Check(PreviewDiagnosticCategory("PARTICLE_REPLAY_BUDGET")=="budget_uncovered","budget treated as bad model");
    Check(PreviewDiagnosticCategory("SCAN_SOURCE_TERRAIN_W3E_INVALID")=="resource_source","invalid terrain context treated as bad model");
    Check(PreviewDiagnosticCategory("SCAN_SOURCE_TERRAIN_MPQ_READ_LIMIT")=="budget_uncovered","terrain read budget treated as bad model");
    Check(PreviewDiagnosticCategory("MDX_UNKNOWN_NEW_CODE")=="needs_investigation","unknown treated as bad model");
}
}
int main(){try{Run();std::cout<<"PASS preview scan classification and sequence coverage\n";return 0;}
catch(const std::exception& error){std::cerr<<"FAIL "<<error.what()<<'\n';return 1;}}
