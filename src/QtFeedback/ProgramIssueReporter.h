#pragma once

#include "War3MapCore/Feedback/Queue.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>

namespace War3Map::QtFeedback {

enum class ExceptionBoundary { Startup, Event, SaveWorker };

// 可信宿主的程序问题入口；不接受地图、脚本、工具结果或异常 what() 正文。
// 只对既有队列同步起草，退出时也不需要新的 Qt 事件循环；不发送、不改变同意项。
class ProgramIssueReporter final {
public:
    using Clock = std::function<std::chrono::steady_clock::time_point()>;
    ProgramIssueReporter(std::shared_ptr<Feedback::FeedbackQueue> queue, Feedback::ApplicationInfo application,
        Clock clock = {});
    [[nodiscard]] std::optional<Feedback::DraftResult> UnhandledException(ExceptionBoundary boundary,
        bool standardException) noexcept;
    [[nodiscard]] std::optional<Feedback::DraftResult> SaveDefect(std::string_view code) noexcept;
    [[nodiscard]] std::optional<Feedback::DraftResult> RenderFailure(std::string_view operation,
        std::int32_t result, bool statusAvailable, std::int32_t statusCallResult,
        std::int32_t statusResult, std::string_view boundedMessage) noexcept;
    [[nodiscard]] static bool IsSaveDefect(std::string_view code) noexcept;

private:
    [[nodiscard]] std::optional<Feedback::DraftResult> Draft(Feedback::AgentIssue issue,
        std::string_view code, std::string_view callSite, std::string_view diagnostic) noexcept;
    std::shared_ptr<Feedback::FeedbackQueue> queue_;
    Feedback::ApplicationInfo application_;
    Clock clock_;
    std::mutex renderMutex_;
    std::array<std::optional<std::chrono::steady_clock::time_point>, 21> renderLastDraft_{};
};

} // namespace War3Map::QtFeedback
