#pragma once
#include "WorldScreenshotEncoder.h"
#include <QPointer>
#include <functional>
#include <optional>

namespace War3Map::Modules {
class WorldViewport;

// UI与MCP共用真实视口，不拥有桌面窗口、不写用户指定路径。
// 一个在途读回/编码和一个结果；PNG编码在已有worker中执行。
class WorldViewportToolService final {
public:
    using ViewportProvider = std::function<WorldViewport*()>;
    explicit WorldViewportToolService(ViewportProvider provider);
    [[nodiscard]] QJsonObject Invoke(const QString& name, const QJsonObject& arguments);
private:
    struct CaptureRequest {
        QString id, projectIdentity, editRevision;
        QPointer<WorldViewport> viewport;
        std::uint64_t viewportRevision = 0;
    };
    void Collect();
    ViewportProvider provider_;
    WorldScreenshotEncoder encoder_;
    std::optional<CaptureRequest> request_;
    std::optional<WorldScreenshotEncodeResult> result_;
};
} // namespace War3Map::Modules
