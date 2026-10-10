#pragma once

#include "War3MapCore/Render/FrameMeasurement.h"
#include "War3MapCore/Render/FrameCapture.h"

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <cstddef>
#include <cstdint>
#include <future>
#include <optional>
#include <vector>

namespace War3Map::Modules {

// 读回及完成帧身份由UI冻结。worker只拥有该份像素/元数据，不借用视口、日志或State。
struct WorldScreenshotEncodeInput {
    Render::FrameScreenshotSample sample;
    QJsonObject metadata;
    std::vector<std::byte> rgba;
    std::uint64_t captureIndex = 0, comparisonEpoch = 0;
};
// 只接受 CaptureCompletedFrame 已校验成功的拥有型结果；身份全部来自读回回执，
// 不再查询视口。探针、测量和服务层共用，调用方可追加测量上下文。
// document_revision 兼容字段属于 viewport_presentation；来源修订单列，缺少主来源时为 null。
[[nodiscard]] WorldScreenshotEncodeInput MakeWorldScreenshotEncodeInput(Render::CapturedFrameRgba frame);
struct WorldScreenshotEncodeResult {
    Render::FrameScreenshotSample sample;
    QJsonObject metadata;
    QByteArray png;
    QString diagnostic;
    std::uint64_t captureIndex = 0, comparisonEpoch = 0;
    std::uint64_t queuedNs = 0, workerStartedNs = 0, workerFinishedNs = 0;
    std::uint64_t pixelHashNs = 0, pngEncodeNs = 0;
    [[nodiscard]] bool Succeeded() const noexcept { return diagnostic.isEmpty() && !png.isEmpty(); }
};

// 只有一个在途/待取回结果；没有第二份待编码队列。结果未取回时也返回busy，
// 使宿主在读回之前就能跳过并记录背压。Stop/析构宿主须Finish后再关闭其日志。
class WorldScreenshotEncoder final {
public:
    [[nodiscard]] bool Busy() const noexcept;
    [[nodiscard]] bool Submit(WorldScreenshotEncodeInput input);
    [[nodiscard]] std::optional<WorldScreenshotEncodeResult> Poll();
    [[nodiscard]] std::optional<WorldScreenshotEncodeResult> Finish();
private:
    std::future<WorldScreenshotEncodeResult> active_;
    std::optional<WorldScreenshotEncodeResult> launchFailure_;
};

} // namespace War3Map::Modules
