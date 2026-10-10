#include "GeosetChannelFixture.h"
#include "AnimationTestModel.h"
#include "War3MapCore/Render/AnimationRenderer.h"
#include "../src/Render/Scene/ModelSkinInput.h"
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using namespace War3Map::Render;
void Check(bool value,const char* message) { if (!value) throw std::runtime_error(message); }
struct Backend {
    bool dynamic=false;
    unsigned uploads=0,updates=0;
    ModelScene scene;
    SkinPose pose;
};
std::int32_t WAR3_RENDER_CALL Submit(War3RendererHandle h,const War3RenderSkinnedModelSceneV2* input) {
    auto& b=*static_cast<Backend*>(h); ModelScene next; std::uint64_t size=0;
    if (!CopySkinnedModelInput(*input,next,size)) return -1;
    b.pose=*next.initialSkin;b.scene=std::move(next);++b.uploads;return 0;
}
std::int32_t WAR3_RENDER_CALL Update(War3RendererHandle h,const War3RenderSkinUpdateV2* input) {
    auto& b=*static_cast<Backend*>(h); SkinPose next;
    if (!CopySkinUpdate(*input,b.scene,next)) return -1;
    b.pose=std::move(next);++b.updates;return 0;
}
std::int32_t WAR3_RENDER_CALL Status(War3RendererHandle h,War3RenderSkinStatusV2* status) {
    auto& b=*static_cast<Backend*>(h);const auto capacity=status->structSize;
    *status={};status->structSize=sizeof(*status);
    if(capacity>=sizeof(War3RenderSkinColorCapabilitiesV2))
        reinterpret_cast<War3RenderSkinColorCapabilitiesV2*>(status)->colorFormat=WAR3_RENDER_STATIC_MESH_RGB_V1;
    if(capacity>=sizeof(War3RenderSkinDynamicColorCapabilitiesV2) && b.dynamic)
        reinterpret_cast<War3RenderSkinDynamicColorCapabilitiesV2*>(status)->dynamicColorFormat=WAR3_RENDER_DYNAMIC_MESH_RGB_V1;
    return 0;
}
std::int32_t WAR3_RENDER_CALL Capture(War3RendererHandle,void*,std::uint64_t,War3RenderSkinnedCapturedFrameV2*) { return -1; }
void Run() {
    // 保留原KGAC/KGAO、颜色和骨架；无关合成KMTF不参与旧动态RGB能力门禁。
    const auto bytes=AnimationTest::WithoutSyntheticConstantKmtf(GeosetChannelFixture::Bytes(1,false));
    std::array<AnimationInstanceSpec,2> inputs;
    for (std::size_t i=0;i<2;++i) {
        inputs[i].identity={i+1,7};inputs[i].hostKey="i"+std::to_string(i);inputs[i].model.modelPath="color.mdx";
        inputs[i].sequenceIndex=0;inputs[i].elapsedMs=i*500; // GeosetChannelFixture 按时间顺序：Stand=0、Death=1
    }
    auto created=ModelAnimationService::Create({7,1,"synthetic"},inputs,[&](std::string_view path,std::uint64_t) {
        if(path!="color.mdx")return War3Map::Result<AnimationResource>::Failure("Test","MISSING","合成模型没有纹理");
        return War3Map::Result<AnimationResource>::Success({bytes,"layer",std::string(64,'c')});
    });
    Check(created.Succeeded(),"动态颜色场景失败");auto service=*created.value;const auto scene=service->Scene();
    Check(scene->dynamicColors,"KGAC 没标记动态能力");
    Check(!scene->dynamicMaterialAlphas && !scene->dynamicMaterialStates,"旧KGAC夹具混入材质动画能力");
    const auto prepared=service->Prepare(nullptr);Check(prepared.Succeeded(),"动态颜色首帧失败");const auto first=*prepared.value;
    Check(first->poses[0].colorHash!=first->poses[1].colorHash,"两实例颜色串帧");
    War3RenderPluginApiV2 api{};api.structSize=sizeof(api);api.apiVersion=WAR3_RENDER_PLUGIN_API_V2;
    api.submitSkinnedModelScene=Submit;api.updateSkinMatrices=Update;api.getSkinStatus=Status;api.captureSkinnedFrameRgba=Capture;
    Backend backend;AnimationRenderer renderer(&api,&backend);
    auto rejected=renderer.Submit(scene,first);
    std::cout<<"dynamic_color_backend_result succeeded="<<rejected.Succeeded()<<" uploads="<<backend.uploads<<'\n';
    for (const auto& diagnostic:rejected.diagnostics)
        std::cout<<"dynamic_color_backend_diagnostic code="<<diagnostic.code<<" message="<<diagnostic.message<<'\n';
    Check(!rejected.Succeeded() && !rejected.diagnostics.empty() && rejected.diagnostics[0].code=="ANIMATION_DYNAMIC_COLOR_BACKEND_UNSUPPORTED" && backend.uploads==0,
        "只有静态RGB能力的插件误接受KGAC");
    backend.dynamic=true;Check(renderer.Submit(scene,first).Succeeded() && backend.uploads==1,"初始动态RGB未提交");
    Check(backend.pose.colors.size()==first->alphas.size() && std::memcmp(backend.pose.colors.data(),first->colors.data(),first->colors.size()*12)==0,"初始RGB未按原值深拷贝");
    AnimationAdvance advance;AnimationEdit edit;edit.identity={2,7};edit.elapsedMs=533;advance.edits.push_back(edit);
    auto updated=service->Prepare(first.get(),advance);Check(updated.Succeeded(),"33ms 颜色更新失败");const auto next=*updated.value;
    Check(next->poses[0].colorHash==first->poses[0].colorHash && next->poses[1].colorHash!=first->poses[1].colorHash,"单实例颜色推进错误");
    Check(renderer.Update(next).Succeeded() && backend.uploads==1 && backend.updates==1,"颜色更新重交静态资产");
    const auto repeat=service->Prepare(next.get());Check(repeat.Succeeded() && renderer.Update(*repeat.value).Succeeded(),"重复颜色帧失败");
    Check((*repeat.value)->poses[1].colorHash==next->poses[1].colorHash && (*repeat.value)->contentVersion>next->contentVersion,"颜色重复帧数值/版本错误");
    const auto stable=backend.pose.colors;const auto target=renderer.Target();
    auto bad=std::make_shared<AnimationFrame>(*target);bad->contentVersion+=100;bad->colors[0].rgb[0]=std::numeric_limits<float>::quiet_NaN();
    Check(!renderer.Update(bad).Succeeded() && renderer.Target()==target && backend.pose.colors.size()==stable.size() &&
        std::memcmp(backend.pose.colors.data(),stable.data(),stable.size()*12)==0,"坏颜色污染目标");
    auto desc=target->DescribeColored();SkinPose scratch;desc.colorCount-=1;
    Check(!CopySkinUpdate(desc.base,backend.scene,scratch),"短颜色表未拒绝");
    auto legacy=target->Describe();Check(!CopySkinUpdate(legacy,backend.scene,scratch),"动态场景接受缺颜色的旧描述符");
    std::cout<<"PASS dynamic-color-service mapping=1 explicit_capability=1 colors_copied=1 same_geometry=1 per_instance=1 repeats=1 bad_input_preserved=1\n";
}
}
int main() { try { Run();return 0; } catch (const std::exception& e) { std::cerr<<"FAIL "<<e.what()<<'\n';return 1; } }
