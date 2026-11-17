#pragma once

#include "ProgramIssueReporter.h"
#include "War3MapCore/Feedback/RunLog.h"

#include <QApplication>
#include <QEvent>
#include <QMetaObject>
#include <QThread>

#include <chrono>
#include <exception>
#include <atomic>
#include <memory>
#include <string>
#include <utility>

namespace War3Map::QtFeedback {

// 仅覆盖安全可捕获的 C++ 启动/事件异常；不在 terminate/SEH 中取得 Qt 或队列锁。
class ProgramIssueApplication final : public QApplication {
public:
    static constexpr int kUnhandledExceptionExitCode = 70;
    using QApplication::QApplication;
    void SetProgramIssueReporter(std::shared_ptr<ProgramIssueReporter> reporter) {
        reporter_.store(std::move(reporter));
    }
    [[nodiscard]] std::shared_ptr<ProgramIssueReporter> Reporter() const { return reporter_.load(); }
    void ReportStartupException(bool standardException) noexcept {
        RunLog::Error("异常", standardException ? "启动时抛出异常。" : "启动时抛出未知异常。");
        if (const auto reporter = Reporter()) (void)reporter->UnhandledException(ExceptionBoundary::Startup, standardException);
    }
    [[nodiscard]] bool HadUnhandledException() const noexcept { return failed_.load(); }

    bool notify(QObject* receiver, QEvent* event) override {
        // 界面线程上单个事件处理过久时记进运行日志（含接收者与事件类型）；期间开过嵌套事件循环（对话框等）的不算。
        thread_local int depth = 0;
        thread_local int entryLoopLevel = 0;
        thread_local bool nestedLoop = false;
        QThread* const current = QThread::currentThread();
        const bool timed = depth == 0 && current == thread();
        if (timed) { entryLoopLevel = current->loopLevel(); nestedLoop = false; }
        else if (depth > 0 && current == thread() && current->loopLevel() > entryLoopLevel) nestedLoop = true;
        const auto started = timed ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        struct Depth {
            int& value; bool timed; std::chrono::steady_clock::time_point started; const char* className; int type;
            ~Depth() {
                --value;
                if (!timed || nestedLoop) return;
                const auto elapsed = std::chrono::steady_clock::now() - started;
                if (elapsed < RunLog::SlowThreshold()) return;
                try {
                    // receiver 可能已在处理中销毁，这里只用进入前记下的类名（元对象里的静态字符串）。
                    RunLog::Info("耗时", "界面线程处理一个事件用了 " + std::to_string(
                        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()) + " 毫秒："
                        + (className ? className : "未知") + "，事件 " + std::to_string(type));
                } catch (...) {}
            }
        } guard{++depth, timed, started, timed && receiver ? receiver->metaObject()->className() : nullptr,
            event ? static_cast<int>(event->type()) : 0};
        try { return QApplication::notify(receiver, event); }
        catch (const std::exception& error) {
            // 本机运行日志记下异常文字与接收者，便于错误报告定位；反馈队列仍只收固定代码。
            RunLog::Error("异常", std::string("事件处理抛出异常：") + error.what() + "；接收者 "
                + (receiver ? receiver->metaObject()->className() : "未知") + "，事件 " + std::to_string(event ? static_cast<int>(event->type()) : 0));
            FailEvent(true);
        }
        catch (...) {
            RunLog::Error("异常", std::string("事件处理抛出未知异常；接收者 ") + (receiver ? receiver->metaObject()->className() : "未知"));
            FailEvent(false);
        }
        return false;
    }

private:
    void FailEvent(bool standardException) noexcept {
        failed_ = true;
        if (const auto reporter = Reporter()) (void)reporter->UnhandledException(ExceptionBoundary::Event, standardException);
        // 接收者可能在异常展开中销毁。退出只绑定应用对象，不捕获 receiver 或主窗口。
        if (QThread::currentThread() == thread()) QCoreApplication::exit(kUnhandledExceptionExitCode);
        try {
            // 即使异常发生在 exec 之前，也让后续事件循环以失败退出。
            QMetaObject::invokeMethod(this, [] { QCoreApplication::exit(kUnhandledExceptionExitCode); }, Qt::QueuedConnection);
        } catch (...) {
        }
    }
    std::atomic<std::shared_ptr<ProgramIssueReporter>> reporter_;
    std::atomic_bool failed_ = false;
};

} // namespace War3Map::QtFeedback
