#include "War3MapCore/Render/AnimationRenderer.h"
#include <array>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using namespace War3Map::Render;
void Require(bool value,const char* message) { if (!value) throw std::runtime_error(message); }
struct Backend {
    bool color=false, reject=false;
    std::uint32_t submissions=0, updates=0;
    std::vector<War3RenderMeshColorV2> colors;
};
std::int32_t WAR3_RENDER_CALL Submit(War3RendererHandle handle,const War3RenderSkinnedModelSceneV2* scene) {
    auto& backend=*static_cast<Backend*>(handle);
    if (backend.reject) return -1;
    ++backend.submissions;
    backend.colors.clear();
    if (scene->structSize==sizeof(War3RenderColoredSkinnedSceneV2)) {
        const auto& colored=*reinterpret_cast<const War3RenderColoredSkinnedSceneV2*>(scene);
        Require(backend.color && colored.colorFormat==WAR3_RENDER_STATIC_MESH_RGB_V1,"错误能力或格式");
        backend.colors.assign(colored.colors,colored.colors+colored.colorCount);
    }
    return 0;
}
std::int32_t WAR3_RENDER_CALL Update(War3RendererHandle handle,const War3RenderSkinUpdateV2*) {
    ++static_cast<Backend*>(handle)->updates; return 0;
}
std::int32_t WAR3_RENDER_CALL Status(War3RendererHandle handle,War3RenderSkinStatusV2* status) {
    const auto size=status->structSize;
    *status={}; status->structSize=sizeof(*status);
    if (size>=sizeof(War3RenderSkinColorCapabilitiesV2) && static_cast<Backend*>(handle)->color)
        reinterpret_cast<War3RenderSkinColorCapabilitiesV2*>(status)->colorFormat=WAR3_RENDER_STATIC_MESH_RGB_V1;
    return 0;
}
std::int32_t WAR3_RENDER_CALL Capture(War3RendererHandle,void*,std::uint64_t,War3RenderSkinnedCapturedFrameV2*) { return -1; }
}
int main() {
    try {
        War3RenderPluginApiV2 api{}; api.structSize=sizeof(api); api.apiVersion=WAR3_RENDER_PLUGIN_API_V2;
        api.submitSkinnedModelScene=Submit; api.updateSkinMatrices=Update; api.getSkinStatus=Status; api.captureSkinnedFrameRgba=Capture;
        Backend backend; AnimationRenderer renderer(&api,&backend);
        auto scene=std::make_shared<AnimatedModelScene>(); scene->sceneVersion=1; scene->documentRevision=7;
        scene->meshes.resize(2);
        auto frame=std::make_shared<AnimationFrame>(); frame->sceneVersion=1; frame->contentVersion=1; frame->documentRevision=7;
        Require(renderer.Submit(scene,frame).Succeeded() && backend.submissions==1,"旧白色场景不兼容");
        auto colored=std::make_shared<AnimatedModelScene>(*scene);
        colored->meshColors={{{1.0f,0.211765f,0.0f}},{{0.0f,0.25f,1.0f}}};
        const auto unsupported=renderer.Submit(colored,frame);
        Require(!unsupported.Succeeded() && unsupported.diagnostics[0].code=="ANIMATION_COLOR_BACKEND_UNSUPPORTED"
            && backend.submissions==1 && renderer.Target()==frame,"旧插件丢弃颜色或污染目标");
        backend.color=true;
        Require(renderer.Submit(colored,frame).Succeeded() && backend.colors.size()==2 &&
            std::memcmp(backend.colors.data(),colored->meshColors.data(),2*sizeof(War3RenderMeshColorV2))==0,
            "非对称 RGB 在宿主提交中被改写");
        auto next=std::make_shared<AnimationFrame>(*frame); next->contentVersion=2;
        Require(renderer.Update(next).Succeeded() && backend.updates==1 && backend.submissions==2 &&
            std::memcmp(backend.colors.data(),colored->meshColors.data(),2*sizeof(War3RenderMeshColorV2))==0,
            "矩阵更新重交或改写静态颜色");
        auto invalid=std::make_shared<AnimatedModelScene>(*colored); invalid->meshColors.pop_back();
        Require(!renderer.Submit(invalid,frame).Succeeded() && renderer.Target()==next,"坏颜色数量污染目标");
        invalid=std::make_shared<AnimatedModelScene>(*colored);
        invalid->meshColors[0].rgb[0]=std::numeric_limits<float>::quiet_NaN();
        Require(!renderer.Submit(invalid,frame).Succeeded() && backend.submissions==2,"NaN 颜色未拒绝");
        backend.reject=true;
        Require(!renderer.Submit(colored,frame).Succeeded() && renderer.Target()==next,"后端拒绝污染目标");
        std::cout<<"PASS animation-color-transport rgb_bits=1 old_plugin_rejected=1 dynamic_preserves_colors=1 rejected_target_preserved=1\n";
        return 0;
    } catch (const std::exception& error) { std::cerr<<"FAIL "<<error.what()<<'\n'; return 1; }
}
