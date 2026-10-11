#include "../src/Render/Tools/MdxScan/ScanPreview.h"
#include "../src/Render/Tools/MdxScan/ScanPreviewRead.h"
#include "AnimationPreviewFixture.h"
#include "AnimationParticleContinuityTests.h"
#include "War3MapCore/Hash.h"
#include <iostream>
#include <set>
#include <stdexcept>

namespace {
using namespace War3Map;
using namespace War3Map::Render;
using namespace War3Map::Render::Scan;
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
std::vector<std::byte> PureParticleModel() {
    const auto source = AnimationParticleContinuityTest::Model();
    MdxTest::Writer out; out.Tag("MDLX");
    for (std::size_t at = 4; at + 8 <= source.size();) {
        std::uint32_t size = 0; std::memcpy(&size, source.data() + at + 4, 4);
        Check(size <= source.size() - at - 8, "original particle fixture chunk truncated");
        const std::string_view tag(reinterpret_cast<const char*>(source.data() + at), 4);
        // 无网格不等于无骨架：PRE2 2 -> BONE 0 -> HELP 1 的父链必须保留。
        // 原创 BONE 的两个 geoset 引用均为 UINT32_MAX，本身不引入任何几何。
        if (tag != "GEOS" && tag != "GEOA") out.Raw(source.data() + at, size + 8);
        at += size + 8;
    }
    return std::move(out.bytes);
}
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
void ReadFailures() {
    const std::vector<Diagnostic> original{
        {DiagnosticSeverity::Error,"OriginalArchive","SOURCE_CHECKSUM_FAILED","原始扇区校验失败。",
            OriginSpan{"original.mpq",32,8,std::string(64,'b')}},
        {DiagnosticSeverity::Warning,"OriginalArchive","SOURCE_BLOCK_DETAIL","保留原始块诊断。",
            OriginSpan{"original-member.blp",64,4,std::string(64,'c')}}};
    const auto path=std::string_view("Textures/Requested.blp");
    const auto rejected=PreviewRead::Resource(path,[&]() -> AnimationResource {
        return PreviewRead::Take(Result<AnimationResource>{std::nullopt,original});
    });
    Check(!rejected.Succeeded() && !rejected.value && rejected.diagnostics.size()==original.size(),
        "source failure became success or discarded additional diagnostics");
    for(std::size_t i=0;i<original.size();++i) {
        const auto& before=original[i];const auto& after=rejected.diagnostics[i];
        Check(after.code==before.code && after.subsystem==before.subsystem && after.severity==before.severity &&
            after.message.starts_with(before.message) && after.message.find(path)!=std::string::npos &&
            after.origin && before.origin && after.origin->source==before.origin->source &&
            after.origin->offset==before.origin->offset && after.origin->length==before.origin->length &&
            after.origin->fingerprint==before.origin->fingerprint,
            "source diagnostic lost its code, message, origin, fingerprint or requested child path");
    }
    Check(original[0].message.find(path)==std::string::npos,"resource decoration changed original diagnostics");
    const auto guarded=PreviewRead::Resource(path,[&]() -> AnimationResource {
        PreviewRead::Take(Status{original});return {};
    });
    Check(!guarded.Succeeded() && guarded.diagnostics.size()==2 && guarded.diagnostics[0].origin,
        "source guard status lost its full diagnostic set");
    const auto budget=PreviewRead::Resource(path,[]() -> AnimationResource { throw std::runtime_error("SCAN_RESOURCE_LIMIT"); });
    Check(!budget.Succeeded() && budget.diagnostics.size()==1 && budget.diagnostics[0].code=="SCAN_RESOURCE_LIMIT" &&
        budget.diagnostics[0].message.find(path)!=std::string::npos,"plain budget failure lost code or child path");
    const AnimationResource good{{std::byte{0x11}},"original-layer",std::string(64,'a')};
    const auto accepted=PreviewRead::Resource(path,[&] { return good; });
    Check(accepted.Succeeded() && accepted.diagnostics.empty() && accepted.value->bytes==good.bytes &&
        accepted.value->sourceLayer==good.sourceLayer && accepted.value->digest==good.digest,"resource helper changed valid input");
    bool contextual=false;
    try { static_cast<void>(PreviewRead::Take(Result<AnimationResource>{std::nullopt,original},"SCAN_SOURCE_TERRAIN_")); }
    catch(const PreviewRead::Failure& failure) {
        contextual=std::string_view(failure.what())=="SCAN_SOURCE_TERRAIN_SOURCE_CHECKSUM_FAILED" &&
            failure.diagnostics.size()==2 && failure.diagnostics[0].code=="SOURCE_CHECKSUM_FAILED";
    }
    Check(contextual,"enumeration exception changed its contextual code or original diagnostic code");
}
void Run(){
    ReadFailures();
    const auto original=Evaluate(AnimationTest::Bytes());
    Check(original.policies.size()==2&&!original.sha256.empty(),"scanner lost policy or identity");
    for(const auto& policy:original.policies){
        Check(policy.planned==6&&policy.sampled==6&&policy.state=="complete","two sequence sample coverage incomplete");
        Check(policy.animated==6&&policy.moving>0,"animated fixture did not reach production pose samples");
    }
    const auto inverse=Evaluate(AnimationPreviewFixture::InverseKeys());
    Check(inverse.policies.size()==2,"inverse fixture lost a scan policy");
    const auto& strict=inverse.policies[0];
    const auto& interactive=inverse.policies[1];
    // 逆序边只属于 Stand；严格求值逐动作拒绝，不能把仍合法的 Walk 一起算成拒绝。
    // scanner 保留局部失败并继续采样，complete 表示六次观察结束，不表示没有模型错误。
    Check(strict.policy=="strict"&&strict.state=="complete"&&strict.planned==6&&strict.sampled==6,
        "strict scan did not observe both sequence ranges");
    Check(strict.animated==3&&strict.visible==3&&strict.moving==1,
        "strict scan did not isolate rejected Stand from valid Walk");
    Check(strict.issues.contains("MDX_POSE_KEYS_INVALID"),"strict scan lost descending-key rejection");
    Check(strict.issues.at("MDX_POSE_KEYS_INVALID").firstSequence==0,
        "strict descending-key rejection was attributed to the wrong sequence");
    Check(!strict.issues.contains("MDX_POSE_SOURCE_ORDER_PREVIEW"),"strict scan enabled source-order preview");
    Check(interactive.policy=="interactive"&&interactive.state=="complete"&&interactive.planned==6&&
        interactive.sampled==6&&interactive.animated==6&&interactive.visible==6&&interactive.moving==2,
        "interactive scan lost source-order sequence evaluation");
    Check(!interactive.issues.contains("MDX_POSE_KEYS_INVALID"),"interactive scan still rejected the source-order range");
    Check(interactive.issues.contains("MDX_POSE_SOURCE_ORDER_PREVIEW"),"interactive scan lost source-order provenance");
    Check(interactive.issues.at("MDX_POSE_SOURCE_ORDER_PREVIEW").firstSequence==0,
        "interactive source-order notice was attributed to the wrong sequence");
    Check(AnimationLimits{}.playbackPolicy==AnimationPlaybackPolicy::StrictReplay,"scan changed strict defaults");
    Check(!AnimationLimits{}.allowSourceOrderNodePreview,"scan changed default node-key policy");
    const auto pureParticle = PureParticleModel();
    const auto particleParsed = ParseMdx(pureParticle);
    Check(particleParsed.Succeeded() && particleParsed.value->geosets.empty(), "particle-only fixture has geometry");
    const auto& particleModel = *particleParsed.value;
    Check(particleModel.bones.size() == 1 && particleModel.bones[0].objectId == 0 &&
        particleModel.bones[0].parentId == 1 && particleModel.bones[0].geosetId == UINT32_MAX &&
        particleModel.bones[0].geosetAnimationId == UINT32_MAX && particleModel.helpers.size() == 1 &&
        particleModel.helpers[0].objectId == 1 && particleModel.helpers[0].parentId == -1 &&
        particleModel.otherNodes.size() == 1 && particleModel.otherNodes[0].objectId == 2 &&
        particleModel.otherNodes[0].parentId == 0, "particle-only fixture lost its original emitter parent chain");
    std::set<std::uint32_t> nodeIds;
    ForEachMdxNode(particleModel, [&](const auto& node) {
        Check(nodeIds.insert(node.objectId).second, "particle-only fixture has duplicate node IDs");
    });
    ForEachMdxNode(particleModel, [&](const auto& node) {
        Check(node.parentId == -1 || (node.parentId >= 0 && nodeIds.contains(static_cast<std::uint32_t>(node.parentId))),
            "particle-only fixture has a dangling parent");
    });
    const auto particleScan = Evaluate(pureParticle);
    Check(particleScan.sha256 == *Sha256(pureParticle).value && particleScan.source == "original-fixture",
        "particle-only scan discarded the verified model identity");
    Check(particleScan.policies.size() == 2, "particle-only scan lost a policy");
    for (const auto& policy : particleScan.policies) {
        if (policy.sampled != 6 || policy.planned != 6) {
            std::cerr << "particle-only policy=" << policy.policy << " state=" << policy.state
                << " planned=" << policy.planned << " sampled=" << policy.sampled << '\n';
            for (const auto& [code, issue] : policy.issues) std::cerr << code << ": " << issue.message << '\n';
        }
        Check(policy.sampled == 6 && policy.planned == 6, "particle-only scan lost declared samples");
        Check(policy.issues.contains("SCAN_EXPLICIT_CAMERA_NO_GEOMETRY"), "particle-only scan did not declare its camera");
        Check(!policy.issues.contains("ANIMATION_CAMERA_EMPTY_SCENE"), "particle-only scan still required a mesh camera fit");
    }
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
    Check(PreviewDiagnosticCategory("MDX_POSE_SOURCE_ORDER_PREVIEW")=="compatibility_notice","source-order notice treated as source error");
    Check(PreviewDiagnosticCategory("PARTICLE_REPLAY_BUDGET")=="budget_uncovered","budget treated as bad model");
    Check(PreviewDiagnosticCategory("SCAN_SOURCE_TERRAIN_W3E_INVALID")=="resource_source","invalid terrain context treated as bad model");
    Check(PreviewDiagnosticCategory("SCAN_SOURCE_TERRAIN_MPQ_READ_LIMIT")=="budget_uncovered","terrain read budget treated as bad model");
    for(const auto* missing:{"file-not-found","WORLD_ASSET_MISSING","RESOURCE_NOT_FOUND"})
        Check(PreviewDiagnosticCategory(missing)=="resource_source","known missing resource lost source classification");
    Check(PreviewDiagnosticCategory("SCAN_READ_RESOURCE_FAILED")=="resource_source","resource I/O failure treated as bad model");
    Check(PreviewDiagnosticCategory("MDX_UNKNOWN_NEW_CODE")=="needs_investigation","unknown treated as bad model");
}
}
int main(){try{Run();std::cout<<"PASS preview scan classification and sequence coverage\n";return 0;}
catch(const std::exception& error){std::cerr<<"FAIL "<<error.what()<<'\n';return 1;}}
