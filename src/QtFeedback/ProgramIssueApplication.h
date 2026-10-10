#pragma once

#include "ProgramIssueReporter.h"

#include <QApplication>
#include <QMetaObject>
#include <QThread>

#include <exception>
#include <atomic>
#include <memory>
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
        if (const auto reporter = Reporter()) (void)reporter->UnhandledException(ExceptionBoundary::Startup, standardException);
    }
    [[nodiscard]] bool HadUnhandledException() const noexcept { return failed_.load(); }

    bool notify(QObject* receiver, QEvent* event) override {
        try { return QApplication::notify(receiver, event); }
        catch (const std::exception&) { FailEvent(true); }
        catch (...) { FailEvent(false); }
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
