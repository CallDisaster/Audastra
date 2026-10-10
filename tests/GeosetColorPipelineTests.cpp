#include "AnimationTestModel.h"
#include "War3MapCore/Render/ModelAnimation.h"
#include "War3RenderCore/PoseBatch.h"
#include "../src/Render/Tools/MdxScan/ScanTypes.h"
#include "../src/Render/Tools/MdxScan/ScanOracle.h"
#include <array>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using namespace War3Map::Render;
using namespace War3RenderCore;
void Check(bool v,const char* text) { if (!v) throw std::runtime_error(text); }
std::vector<std::byte> Bytes(std::uint32_t flags=2) {
    auto bytes=AnimationTest::Bytes();
    for (std::size_t at=4;at+8<=bytes.size();) {
        std::uint32_t size=0; std::memcpy(&size,bytes.data()+at+4,4);
        if (std::memcmp(bytes.data()+at,"GEOA",4)==0) {
            const std::array<float,3> staticRgb{1.0f,0.211765f,0.0f};
            std::memcpy(bytes.data()+at+16,&flags,4);
            std::memcpy(bytes.data()+at+20,staticRgb.data(),12);
            return bytes;
        }
        at+=8+size;
    }
    throw std::runtime_error("缺少合成 GEOA");
}
void Pipeline() {
    const auto bytes=Bytes(); const auto parsed=ParseMdx(bytes); Check(parsed.Succeeded(),"解析失败");
    const auto model=*parsed.value;
    const auto compiled=CompileMdxModel(model,{1,7},{"color.mdx","synthetic",std::string(64,'c')});
    Check(compiled.Succeeded(),"静态颜色编译失败");
    const auto& asset=*compiled.value; const std::array assets{asset};
    auto made=PoseBatchEvaluator::Create(assets,1); Check(made.Succeeded(),"求值器失败");
    auto evaluator=std::move(*made.value);
    for (const auto time:{0u,250u,500u,999u}) {
        PoseRequest request; request.instance={1,7}; request.asset={1,7}; request.sequenceIndex=1; request.sequenceElapsedMs=time;
        Check(evaluator.Evaluate(std::span(&request,1)).failedInstances==0,"flags2 仍被求值门拒绝");
        for (std::uint32_t g=0;g<asset.GeosetCount();++g) {
            const auto alpha=SampleMdxGeosetAlpha(model,{g,1,time,0});
            const auto color=SampleMdxGeosetColor(model,{g,1,time,0});
            Check(alpha.Succeeded() && color.Succeeded(),"参考拒绝 flags2");
            Check(std::memcmp(&*alpha.value,&evaluator.Alphas(0)[g],4)==0 &&
                std::memcmp(color.value->data(),asset.GeosetColors()[g].rgb.data(),12)==0,"alpha/RGB 逐位不一致");
        }
    }
    for (const auto invalid:{1u,3u,4u,0x80000000u}) {
        auto bad=model; bad.geosets[1].animation->flags=invalid;
        Scan::Record report; report.path="bad.mdx"; report.source="synthetic"; report.digest="flags";
        Scan::Compare(bad,report,{},[]{});
        Check(report.mismatchSamples==0 && report.numericSamples==0 && report.matchedRejections==report.samples,
            "未知位没有保持同码拒绝或被算作数值通过");
    }
    auto unknown=model; unknown.geosets[1].animation->hasUninterpretedData=true;
    Scan::Record blocked; blocked.path="kgac.mdx"; blocked.source="synthetic"; blocked.digest="unknown";
    Scan::Compare(unknown,blocked,{},[]{});
    Check(blocked.mismatchSamples==0 && blocked.numericSamples==0,"未知尾部被忽略");
    auto nan=model; nan.geosets[1].animation->staticColor[0]=std::numeric_limits<float>::quiet_NaN();
    Scan::Record invalid; invalid.path="nan.mdx"; invalid.source="synthetic"; invalid.digest="nan";
    Scan::Compare(nan,invalid,{},[]{});
    Check(invalid.mismatchSamples==0 && invalid.numericSamples==0 && invalid.matchedRejections>0,"活动 NaN 颜色未严格拒绝");
    Scan::Record good; good.path="good.mdx"; good.source="synthetic"; good.digest="good";
    Scan::Compare(model,good,{},[]{});
    Check(good.mismatchSamples==0 && good.numericSamples>0 && good.colors>0,"扫描没有真实比较 RGB");
    auto changed=model; changed.geosets[1].animation->staticColor[0]=0.25f;
    Scan::Record positive;
    Scan::CompareSample(changed,asset,evaluator,{1,500,0,false},positive,[]{});
    Check(positive.colorMismatch==1 && positive.mismatchSamples==1 && positive.nodeMismatch==0,
        "RGB 差分阳性未检出，不能只看零差异输出");
    std::array<AnimationInstanceSpec,2> specs;
    for (std::size_t i=0;i<specs.size();++i) {
        specs[i].identity={i+1,7}; specs[i].hostKey="row"+std::to_string(i); specs[i].model.modelPath="color.mdx";
        specs[i].model.teamColor=static_cast<std::uint32_t>(i); specs[i].sequenceIndex=1; specs[i].elapsedMs=500;
    }
    auto built=ModelAnimationService::Create({7,11,"source"},specs,[&](std::string_view path,std::uint64_t) {
        if (path!="color.mdx") return War3Map::Result<AnimationResource>::Failure("Test","MISSING","合成模型没有纹理");
        return War3Map::Result<AnimationResource>::Success({bytes,"actual-layer",std::string(64,'a')});
    });
    Check(built.Succeeded(),"颜色模型服务创建失败");
    auto service=*built.value; const auto scene=service->Scene();
    Check(scene->meshColors.size()==scene->meshes.size() && scene->geometry.models.size()==2,"变体颜色表不完整");
    const std::array<float,3> staticRgb{1.0f,0.211765f,0.0f}, white{1,1,1};
    for (std::size_t i=0;i<scene->meshColors.size();++i) {
        const auto& expected=scene->geometry.meshGeosets[i]==1 ? staticRgb : white;
        Check(std::memcmp(scene->meshColors[i].rgb,expected.data(),12)==0,"geoset 静态文件 RGB 到变体网格映射错误");
    }
    const auto first=service->Prepare(nullptr); Check(first.Succeeded(),"颜色实例首帧失败");
    const auto second=service->Prepare(first.value->get()); Check(second.Succeeded(),"颜色实例重复帧失败");
    Check(scene==service->Scene() && (*second.value)->contentVersion>(*first.value)->contentVersion,
        "姿势更新重新建立静态场景或没有新版本");
    std::cout<<"PASS geoa-color-pipeline rgb_bitwise=1 static_rgb_mapping=1 variants=2 strict_flags_kgac_nan=1 numeric_samples="
        <<good.numericSamples<<" colors_compared="<<good.colors<<" mismatch_samples="<<good.mismatchSamples<<'\n';
}
}
int main() { try { Pipeline(); return 0; } catch(const std::exception& e) { std::cerr<<"FAIL "<<e.what()<<'\n';return 1; } }
