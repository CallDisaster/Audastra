#pragma once
#include "War3MapCore/Render/AnimationRenderer.h"
#include <filesystem>

namespace War3Map::Render {
struct AnimationCaptureStep {
    std::string label;
    std::vector<AnimationEdit> edits;
};
struct AnimationCapturePlan {
    std::vector<AnimationInstanceSpec> instances;
    std::vector<AnimationCaptureStep> captures;
    std::optional<War3RenderCameraV2> camera;
    bool particles = false; // 显式选择粒子回放；关闭时保持既有模型捕获行为。
};
// UTF-8 TSV，instance/time/camera/particles/capture；particles 仅在开头给一次 true/false。
// 不接受任意脚本/路径命令；显式时间（包括 globalMs）绝不自动纠正。
// 动作名称原样交给服务按真实 SEQS 解析。最大 256 实例、16 帧、1 MiB 文本。
[[nodiscard]] Result<AnimationCapturePlan> ParseAnimationCapturePlan(std::string_view text);
struct AnimationCaptureRequest {
    std::filesystem::path gameRoot;
    std::filesystem::path outputDirectory; // 必须不存在；父目录须存在
    AnimationCapturePlan plan;
    std::uint32_t width=1024,height=768;
    std::uint64_t documentRevision=0;
    bool software=true;
    bool worldPreview=false; // 显式复用世界页局部恢复；默认严格，结果仍仅 PreviewApproximate。
};
struct AnimationCaptureOutput {
    std::filesystem::path report;
    std::string summaryJson;
};
// Windows 无窗口：固定使用可执行文件旁的 D3D11 插件，不接受请求指定 DLL。
// 创建一次服务、一次 renderer/静态上传，再连续更新/截图；CLI 与 MCP 共用此函数。
[[nodiscard]] Result<AnimationCaptureOutput> RunModelAnimationCapture(const AnimationCaptureRequest& request);
// 同帧 receipt 经过 AnimationRenderer 验证后才交此序列化入口；大整数用 JSON 字符串，避免 JS 丢精度。
[[nodiscard]] std::string AnimationReceiptJson(const AnimatedModelScene& scene,const AnimationFrame& frame,
    const AnimationCaptureReceipt& actual,std::string_view label,std::string_view imageSha);
[[nodiscard]] std::string AnimationJsonString(std::string_view value);
} // namespace War3Map::Render
