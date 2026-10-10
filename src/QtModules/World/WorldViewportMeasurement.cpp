#include "WorldViewport.h"
#include <algorithm>
#include <cstddef>

namespace War3Map::Modules {
Result<WorldViewport::FrameMetricsPage> WorldViewport::ReadFrameMetrics(std::uint64_t afterFrame) const {
    using Output = Result<FrameMetricsPage>;
    if (!renderer_ || failed_)
        return Output::Failure("World.Measurement", "WORLD_MEASUREMENT_NOT_READY", "完成帧测量等待渲染器就绪。");
    if (!api_ || api_->structSize < offsetof(War3RenderPluginApiV2, getFrameMetrics) + sizeof(api_->getFrameMetrics)
        || !api_->getFrameMetrics)
        return Output::Failure("World.Measurement", "WORLD_MEASUREMENT_UNSUPPORTED", "插件没有完成帧测量尾部，不能用请求帧代替。");
    FrameMetricsPage output;
    output.page.structSize = sizeof(output.page);
    output.records.resize(WAR3_RENDER_FRAME_METRICS_PAGE_LIMIT);
    const auto result = api_->getFrameMetrics(renderer_, afterFrame, output.records.data(),
        static_cast<std::uint32_t>(output.records.size()), &output.page);
    if (result != 0) ReportPluginFailure("getFrameMetrics", result);
    if (result != 0 || output.page.structSize < sizeof(output.page) || output.page.count > output.records.size())
        return Output::Failure("World.Measurement", "WORLD_MEASUREMENT_READ_FAILED", "插件测量分页失败，未补造完成帧。");
    output.records.resize(output.page.count);
    return Output::Success(std::move(output));
}
std::optional<WorldViewport::AnimationReceiveTiming> WorldViewport::ReceiveTiming(std::uint64_t scene,
    std::uint64_t content) const {
    const auto found = std::find_if(animationReceiveTimings_.rbegin(), animationReceiveTimings_.rend(),
        [&](const auto& item) { return item.sceneVersion == scene && item.contentVersion == content; });
    return found == animationReceiveTimings_.rend() ? std::nullopt : std::optional(*found);
}
} // namespace War3Map::Modules
