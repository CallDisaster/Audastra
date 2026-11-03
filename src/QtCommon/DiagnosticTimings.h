#pragma once
#include <QTimer>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <chrono>
#include <iostream>
#include <string>
#include <string_view>
#include <syncstream>
#include <thread>

namespace War3Map::QtCommon {
namespace TimingDetail {
inline std::atomic<bool> enabled{false};
inline std::thread::id uiThread;
using Clock = std::chrono::steady_clock;
inline Clock::time_point Origin() { static const auto origin = Clock::now(); return origin; }
inline auto Elapsed() { return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - Origin()).count(); }
// 只在启动线程访问；worker仅比较线程身份，不访问心跳状态或Qt对象。
struct HeartbeatState {
    std::string_view operation;
    Clock::time_point last;
    std::int64_t longest = 0;
};
inline HeartbeatState heartbeat;
inline bool IsUiThread() { return std::this_thread::get_id() == uiThread; }
inline void Tick() {
    if (!IsUiThread() || heartbeat.operation.empty()) return;
    const auto now = Clock::now();
    heartbeat.longest = (std::max)(heartbeat.longest,
        std::chrono::duration_cast<std::chrono::microseconds>(now - heartbeat.last).count());
    heartbeat.last = now;
}
}
// 启动时解析并发布一次；创建worker前调用。默认关闭，不从worker读取应用参数。
inline void ConfigureDiagnosticTimings(bool enabled) noexcept {
    TimingDetail::uiThread = std::this_thread::get_id();
    TimingDetail::enabled.store(enabled, std::memory_order_release);
}
inline bool DiagnosticTimingsEnabled() noexcept {
    return TimingDetail::enabled.load(std::memory_order_acquire);
}
inline void StartDiagnosticHeartbeat(QObject* owner) {
    if (!DiagnosticTimingsEnabled()) return;
    auto* timer = new QTimer(owner);
    timer->setTimerType(Qt::PreciseTimer); timer->setInterval(20);
    QObject::connect(timer, &QTimer::timeout, owner, [] { TimingDetail::Tick(); });
    timer->start();
}
// 显式诊断才向stderr写事件；不改变事务、进度文案或异常结果。
inline void DiagnosticTimingEvent(std::string_view event) noexcept {
    if (!DiagnosticTimingsEnabled()) return;
    try {
        const auto elapsed = TimingDetail::Elapsed();
        std::osyncstream(std::cerr) << "editor_timing_event=" << elapsed << '|' << event << '\n';
        std::osyncstream(std::cerr) << "editor_timing_thread=" << elapsed << '|'
            << (TimingDetail::IsUiThread() ? "ui" : "worker") << '|' << event << '\n';
    } catch (...) {}
}
class DiagnosticTimingScope {
public:
    explicit DiagnosticTimingScope(std::string_view name) noexcept
        : name_(name), enabled_(DiagnosticTimingsEnabled()) {
        if (enabled_ && TimingDetail::IsUiThread() && TimingDetail::heartbeat.operation.empty()
            && (name == "open-total" || name == "save-total" || name == "minimap-total")) {
            TimingDetail::heartbeat = {name, TimingDetail::Clock::now(), 0}; heartbeatOwner_ = true;
        }
        Notify("begin/");
    }
    ~DiagnosticTimingScope() { Finish(); }
    DiagnosticTimingScope(const DiagnosticTimingScope&) = delete;
    DiagnosticTimingScope& operator=(const DiagnosticTimingScope&) = delete;
    void Finish() noexcept {
        if (finished_) return;
        if (heartbeatOwner_) {
            TimingDetail::Tick();
            try { std::osyncstream(std::cerr) << "editor_ui_responsiveness=" << TimingDetail::Elapsed()
                << "|operation=" << name_ << "|max_gap_us=" << TimingDetail::heartbeat.longest << "|timer_us=20000\n"; } catch (...) {}
            TimingDetail::heartbeat.operation = {};
        }
        Notify("end/"); finished_ = true;
    }
private:
    void Notify(std::string_view prefix) noexcept {
        if (enabled_) try { DiagnosticTimingEvent(std::string(prefix) + std::string(name_)); } catch (...) {}
    }
    std::string_view name_;
    bool enabled_, finished_ = false, heartbeatOwner_ = false;
};
} // namespace War3Map::QtCommon
