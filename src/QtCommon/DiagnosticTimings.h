#pragma once
#include <QCoreApplication>
#include <chrono>
#include <iostream>
#include <string>
#include <string_view>
#include <syncstream>

namespace War3Map::QtCommon {
inline bool DiagnosticTimingsEnabled() noexcept {
    try {
        return QCoreApplication::instance()
            && QCoreApplication::arguments().contains(QStringLiteral("--diagnostic-timings"));
    } catch (...) { return false; }
}
// 显式诊断才向stderr写事件；不改变事务、进度文案或异常结果。
inline void DiagnosticTimingEvent(std::string_view event) noexcept {
    if (!DiagnosticTimingsEnabled()) return;
    try {
        using Clock = std::chrono::steady_clock;
        static const auto origin = Clock::now();
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - origin).count();
        std::osyncstream(std::cerr) << "editor_timing_event=" << elapsed << '|' << event << '\n';
    } catch (...) {}
}
class DiagnosticTimingScope {
public:
    explicit DiagnosticTimingScope(std::string_view name) noexcept
        : name_(name), enabled_(DiagnosticTimingsEnabled()) { Notify("begin/"); }
    ~DiagnosticTimingScope() { Finish(); }
    DiagnosticTimingScope(const DiagnosticTimingScope&) = delete;
    DiagnosticTimingScope& operator=(const DiagnosticTimingScope&) = delete;
    void Finish() noexcept { if (!finished_) { Notify("end/"); finished_ = true; } }
private:
    void Notify(std::string_view prefix) noexcept {
        if (enabled_) try { DiagnosticTimingEvent(std::string(prefix) + std::string(name_)); } catch (...) {}
    }
    std::string_view name_;
    bool enabled_, finished_ = false;
};
} // namespace War3Map::QtCommon
