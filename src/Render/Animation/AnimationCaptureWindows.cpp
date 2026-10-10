#include "War3MapCore/Render/ModelAnimationCapture.h"
#include "War3MapCore/Game/GameDataSource.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/Render/AnimationSourceGuard.h"
#include "War3MapCore/RenderPluginFrameMetrics.h"
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>

namespace War3Map::Render {
namespace {
namespace fs=std::filesystem;
void Require(bool condition,const char* text) { if(!condition) throw std::runtime_error(text); }
std::string Utf8(const fs::path& path) {
    const auto text=path.generic_u8string(); return {reinterpret_cast<const char*>(text.data()),text.size()};
}
struct Plugin {
    HMODULE module=nullptr;
    const War3RenderPluginApiV2* api=nullptr;
    War3RendererHandle renderer=nullptr;
    ~Plugin(){if(renderer)api->destroyRenderer(renderer);if(module)FreeLibrary(module);}
    void Open(std::uint32_t width,std::uint32_t height,bool software) {
        std::wstring executable(32768,L'\0');
        const auto size=GetModuleFileNameW(nullptr,executable.data(),static_cast<DWORD>(executable.size()));
        Require(size>0 && size<executable.size(),"无法定位宿主可执行文件。"); executable.resize(size);
        const auto dll=fs::path(executable).parent_path()/L"war3-render-d3d11.dll";
        module=LoadLibraryExW(dll.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        Require(module!=nullptr,"无法加载宿主旁的 D3D11 插件。");
        const auto get=reinterpret_cast<War3GetRenderPluginApiV2>(GetProcAddress(module,"War3MapEditor_GetRenderPluginApiV2"));
        Require(get!=nullptr,"插件缺少 V2 入口。"); api=get(WAR3_RENDER_PLUGIN_API_V2);
        Require(AnimationRenderer::Supported(api) && api->createRenderer && api->destroyRenderer && api->setCamera && api->submitMesh,
            "插件没有完整的动画与完成帧接口。");
        const War3RenderCreateInfoV2 info{sizeof(info),width,height,software?1u:0u};
        renderer=api->createRenderer(&info); Require(renderer!=nullptr,"无法创建无窗口渲染器。");
    }
};
std::string CaptureRendererMetrics(const Plugin& plugin,const AnimationCaptureReceipt& receipt) {
    const auto* api=plugin.api;
    if (api->structSize<offsetof(War3RenderPluginApiV2,getFrameMetrics)+sizeof(api->getFrameMetrics) || !api->getFrameMetrics)
        return "\"rendererDevice\":null,\"rendererTiming\":null";
    const auto frameCount=receipt.frame.capture.frameCount;
    Require(frameCount!=0,"实际截图没有完成帧编号，未发布设备测量。 ");
    War3RenderFrameMetricsV2 metrics{}; metrics.structSize=sizeof(metrics);
    War3RenderFrameMetricsPageV2 page{}; page.structSize=sizeof(page);
    Require(api->getFrameMetrics(plugin.renderer,frameCount-1,&metrics,1,&page)==0 && page.structSize>=sizeof(page) &&
        page.count==1 && page.software<=1 && metrics.structSize>=sizeof(metrics) && metrics.result==0 &&
        (metrics.flags & WAR3_RENDER_FRAME_METRICS_DRAW_COMPLETED)!=0 &&
        metrics.completed.view.frameCount==frameCount && metrics.completed.skin.frameCount==receipt.skin.frameCount &&
        metrics.completed.skin.sceneVersion==receipt.skin.sceneVersion && metrics.completed.skin.contentVersion==receipt.skin.contentVersion &&
        metrics.completed.view.documentRevision==receipt.frame.capture.documentRevision &&
        metrics.completed.view.surfaceEpoch==receipt.frame.view.surfaceEpoch && metrics.completed.view.viewEpoch==receipt.frame.view.viewEpoch &&
        metrics.completed.view.sceneEpoch==receipt.frame.view.sceneEpoch,
        "设备测量与实际捕获的完成帧身份不同，未发布截图。 ");
    const auto adapterEnd=std::find(std::begin(page.adapterName),std::end(page.adapterName),'\0');
    std::ostringstream out;
    out<<"\"rendererDevice\":{\"software\":"<<(page.software ? "true" : "false")<<",\"featureLevel\":"<<page.featureLevel
        <<",\"adapterName\":"<<AnimationJsonString({page.adapterName,static_cast<std::size_t>(adapterEnd-page.adapterName)})<<'}';
    out<<",\"rendererTiming\":{\"frameCount\":\""<<frameCount<<"\",\"flags\":"<<metrics.flags
        <<",\"windowAttached\":"<<((metrics.flags & WAR3_RENDER_FRAME_METRICS_WINDOW_ATTACHED)!=0 ? "true" : "false")
        <<",\"uploadCpuNs\":\""<<metrics.uploadCpuNs<<"\",\"drawCpuNs\":\""<<metrics.drawCpuNs
        <<"\",\"presentCpuNs\":\""<<metrics.presentCpuNs<<"\",\"readbackCpuNs\":\""<<metrics.readbackCpuNs
        <<"\",\"gpuTimeAvailable\":"<<((metrics.flags & WAR3_RENDER_FRAME_METRICS_GPU_TIME_AVAILABLE)!=0 ? "true" : "false")
        <<",\"gpuDrawNs\":";
    if ((metrics.flags & WAR3_RENDER_FRAME_METRICS_GPU_TIME_AVAILABLE)!=0) out<<'"'<<metrics.gpuDrawNs<<'"';
    else out<<"null";
    out<<'}'; return out.str();
}
void WriteNew(const fs::path& path,std::span<const std::byte> bytes) {
    HANDLE file=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    Require(file!=INVALID_HANDLE_VALUE,"输出文件已存在或不可写，未覆盖。");
    bool success=true;
    for(std::size_t at=0;at<bytes.size();) {
        const auto count=static_cast<DWORD>(std::min<std::size_t>(bytes.size()-at,1024*1024)); DWORD written=0;
        if(!WriteFile(file,bytes.data()+at,count,&written,nullptr) || written!=count) {success=false;break;}
        at+=written;
    }
    if(success)success=FlushFileBuffers(file)!=0;
    CloseHandle(file);
    if(!success) { DeleteFileW(path.c_str()); throw std::runtime_error("输出写入失败，没有发布成功文件。"); }
}
void WriteText(const fs::path& path,std::string_view text) {
    WriteNew(path,{reinterpret_cast<const std::byte*>(text.data()),text.size()});
}
std::vector<std::byte> Bitmap(std::span<const std::byte> rgba,std::uint32_t width,std::uint32_t height) {
    std::vector<std::byte> result(54+rgba.size());
    const auto put=[&](std::size_t at,std::uint32_t n){for(unsigned i=0;i<4;++i)result[at+i]=static_cast<std::byte>((n>>(i*8))&255u);};
    result[0]=std::byte{'B'};result[1]=std::byte{'M'};put(2,static_cast<std::uint32_t>(result.size()));put(10,54);put(14,40);
    put(18,width);put(22,static_cast<std::uint32_t>(-static_cast<std::int32_t>(height)));
    result[26]=std::byte{1};result[28]=std::byte{32};put(34,static_cast<std::uint32_t>(rgba.size()));
    for(std::size_t p=0;p<rgba.size();p+=4){result[54+p]=rgba[p+2];result[55+p]=rgba[p+1];result[56+p]=rgba[p];result[57+p]=rgba[p+3];}
    return result;
}
}
Result<AnimationCaptureOutput> RunModelAnimationCapture(const AnimationCaptureRequest& input) try {
    using Output=Result<AnimationCaptureOutput>;
    if(input.width<64 || input.height<64 || input.width>2048 || input.height>2048 ||
        input.plan.instances.empty() || input.plan.instances.size()>256 || input.plan.captures.empty() || input.plan.captures.size()>16)
        return Output::Failure("Render.Animation","ANIMATION_CAPTURE_BUDGET","截取尺寸或实例/帧数超出预算。");
    std::set<std::string> names;
    for(const auto& step:input.plan.captures) {
        Require(!step.label.empty() && step.label.size()<=64 && std::all_of(step.label.begin(),step.label.end(),[](unsigned char c) {
            return (c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') || c=='-' || c=='_';
        }) && names.insert(step.label).second,"输出帧标签不安全或重复。");
    }
    std::error_code error;
    Require(input.outputDirectory.is_absolute() && !fs::exists(input.outputDirectory,error) && !error &&
        fs::is_directory(input.outputDirectory.parent_path()),"输出目录必须为不存在的绝对路径，且父目录已存在。");
    AnimationSourceGuard sourceGuard;
    auto guarded = sourceGuard.WatchGameRoot(input.gameRoot);
    if (!guarded.Succeeded()) return {std::nullopt, std::move(guarded.diagnostics)};
    auto mounted=Game::GameDataSource::MountStrict(input.gameRoot);
    if(!mounted.Succeeded())return {std::nullopt,std::move(mounted.diagnostics)};
    auto plan=input.plan; const auto epoch=NextAnimationSerial();
    for(auto& spec:plan.instances)spec.identity.generation=epoch;
    for(auto& step:plan.captures)for(auto& e:step.edits)e.identity.generation=epoch;
    AnimationLimits limits; limits.geometry.maxInstances=256; limits.geometry.maxTotalTextureBytes=64ull*1024*1024;
    limits.geometry.maxVertices=1048576; limits.geometry.maxIndices=3145728;
    limits.enableParticles=plan.particles;
    limits.retainUnsupportedStatic=input.worldPreview;
    limits.playbackPolicy=input.worldPreview ? AnimationPlaybackPolicy::InteractivePreview : AnimationPlaybackPolicy::StrictReplay;
    limits.allowRigidStaticPreview=input.worldPreview;
    Plugin plugin; plugin.Open(input.width,input.height,input.software);
    if (plan.particles && !AnimationRenderer::ParticleSupported(plugin.api))
        return Output::Failure("Render.Animation","ANIMATION_PARTICLE_BACKEND_UNSUPPORTED","显式粒子捕获要求完整粒子插件尾部；未改走模型截图。");
    auto service=ModelAnimationService::Create({epoch,input.documentRevision,"game-session:"+std::to_string(epoch)},plan.instances,
        [&](std::string_view path,std::uint64_t maximum)->Result<AnimationResource> {
            auto read=mounted.value->ReadFileBoundedWithSource(std::string(path),static_cast<std::size_t>(maximum));
            if(!read.Succeeded())return {std::nullopt,std::move(read.diagnostics)};
            auto digest=Sha256(read.value->bytes); if(!digest.Succeeded())return {std::nullopt,std::move(digest.diagnostics)};
            return Result<AnimationResource>::Success({std::move(read.value->bytes),"game:"+Utf8(input.gameRoot)+"|"+read.value->archiveName,std::move(*digest.value)});
        },limits);
    if(!service.Succeeded())return {std::nullopt,std::move(service.diagnostics)};
    auto scene=(*service.value)->Scene();
    if (!plan.camera && scene->geometry.instances.empty())
        return Output::Failure("Render.Animation","ANIMATION_CAMERA_REQUIRED","纯粒子场景须在清单中显式指定 camera，不能用空模型包围盒自动取景。");
    const auto camera=plan.camera?*plan.camera:FitAnimationSceneCamera(*scene);
    Require(std::isfinite(camera.distance) && camera.distance>=16 && camera.distance<=400000 &&
        std::isfinite(camera.pitchRadians) && camera.pitchRadians>=0.05f && camera.pitchRadians<=1.5f,"相机距离或俯仰越界。");
    Require(plugin.api->setCamera(plugin.renderer,&camera)==0,"插件拒绝相机。");
    const War3RenderMeshV2 empty{sizeof(empty),0,0,input.documentRevision,nullptr,nullptr};
    Require(plugin.api->submitMesh(plugin.renderer,&empty)==0,"插件拒绝视图修订。");
    AnimationRenderer bridge(plugin.api,plugin.renderer); std::shared_ptr<const AnimationFrame> previous;
    std::vector<std::byte> rgba(static_cast<std::size_t>(input.width)*input.height*4);
    const auto policyFields=std::string("\"capturePolicy\":")+AnimationJsonString(input.worldPreview?"WorldPreview":"Strict")+
        ",\"worldPreview\":"+(input.worldPreview?"true":"false");
    std::string summary="{\"quality\":\"PreviewApproximate\","+policyFields+",\"captures\":[";
    std::uint64_t staticUploads=0,particleTextureUploads=0,particleDynamicUploads=0;
    Require(fs::create_directory(input.outputDirectory),"无法独占创建输出目录。");
    for(std::size_t index=0;index<plan.captures.size();++index) {
        const auto& step=plan.captures[index]; AnimationAdvance command; command.worldCamera=AnimationWorldCamera(camera); command.edits=step.edits;
        auto evaluated=(*service.value)->Prepare(previous.get(),command);
        if(!evaluated.Succeeded())return {std::nullopt,std::move(evaluated.diagnostics)};
        auto frame=std::move(*evaluated.value);
        const auto submitted=previous?bridge.Update(frame):bridge.Submit(scene,frame);
        if(!submitted.Succeeded())return {std::nullopt,submitted.diagnostics};
        auto actual=bridge.Capture(rgba,input.documentRevision);
        if(!actual.Succeeded())return {std::nullopt,std::move(actual.diagnostics)};
        Require(actual.value->frame.capture.width==input.width && actual.value->frame.capture.height==input.height,"捕获尺寸与缓冲不符。");
        if(!index)staticUploads=actual.value->skin.staticUploadCount;
        Require(actual.value->skin.staticUploadCount==staticUploads,"姿势更新触发静态上传，验收拒绝。");
        if (actual.value->particles) {
            const auto& particles=*actual.value->particles;
            if (!index) {
                particleTextureUploads=particles.textureUploadCount;
                Require(particleTextureUploads==(scene->particleTextures.empty()?0u:1u),"粒子纹理首次上传计数无效。");
            }
            Require(particles.textureUploadCount==particleTextureUploads,"粒子更新触发静态纹理重传，验收拒绝。");
            Require(particles.dynamicUploadCount==particleDynamicUploads+1,"粒子组合帧上传计数不连续，验收拒绝。");
            particleDynamicUploads=particles.dynamicUploadCount;
        }
        auto current = sourceGuard.Verify();
        if (!current.Succeeded()) return {std::nullopt, std::move(current.diagnostics)};
        const auto& drawnCamera = actual.value->frame.camera;
        Require(drawnCamera.targetX == camera.targetX && drawnCamera.targetY == camera.targetY && drawnCamera.targetZ == camera.targetZ &&
            drawnCamera.distance == camera.distance && drawnCamera.yawRadians == camera.yawRadians && drawnCamera.pitchRadians == camera.pitchRadians,
            "实际绘制相机与请求不同，未发布截图。 ");
        const auto bitmap=Bitmap(rgba,input.width,input.height);
        auto digest=Sha256(bitmap); if(!digest.Succeeded())return {std::nullopt,std::move(digest.diagnostics)};
        auto receipt=AnimationReceiptJson(*scene,*frame,*actual.value,step.label,*digest.value);
        // 单帧可独立审查：保留既有原诊断/未显示 geoset，并附显式请求策略。
        receipt.insert(1,policyFields+','+CaptureRendererMetrics(plugin,*actual.value)+',');
        const auto name=fs::path(step.label);
        WriteNew(input.outputDirectory/(name.wstring()+L".bmp"),bitmap);
        WriteText(input.outputDirectory/(name.wstring()+L".json"),receipt);
        if (index) summary += ',';
        summary += receipt;
        previous = std::move(frame);
    }
    summary+="]}"; const auto report=input.outputDirectory/L"report.json";
    WriteText(report,summary);
    return Output::Success({report,std::move(summary)});
} catch(const std::exception& e) {
    return Result<AnimationCaptureOutput>::Failure("Render.Animation","ANIMATION_CAPTURE_FAILED",e.what());
}
} // namespace War3Map::Render
