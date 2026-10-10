#include "RecoveryExitDialog.h"

#include <QApplication>
#include <QDialog>
#include <QLabel>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QScopeGuard>
#include <QSemaphore>
#include <QTest>
#include <QThread>
#include <QTimer>
#include <QTemporaryDir>
#include <memory>

class RecoveryExitDialogTests final : public QObject {
    Q_OBJECT
private slots:
    void CompletesWithoutBlockingEvents();
    void AcceptedCancellationWaitsForWorker();
    void PublicationRejectsLateCancellation();
    void ReturnToEditorLeavesWorkerRunning();
    void QuitFallbackCannotDismissActiveWrite();
    void RepeatedApplicationExitKeepsOneCancellableDialog();
    void DestroyedParentStillSettlesOwnedWorker();
};

namespace {
QDialog* ActiveRecoveryDialog() {
    auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
    return dialog && dialog->objectName() == QStringLiteral("recoveryExitDialog") ? dialog : nullptr;
}
}

void RecoveryExitDialogTests::CompletesWithoutBlockingEvents() {
    QSemaphore release;
    std::unique_ptr<QThread> worker(QThread::create([&] { release.acquire(); }));
    worker->start();
    const auto join = qScopeGuard([&] { release.release(); worker->wait(); });
    int beats = 0;
    bool sawProgress = false;
    QTimer heartbeat;
    heartbeat.setInterval(5);
    connect(&heartbeat, &QTimer::timeout, this, [&] {
        ++beats;
        if (const auto* dialog = ActiveRecoveryDialog()) sawProgress = dialog->windowTitle() == QStringLiteral("正在写入恢复点");
        if (beats == 6) release.release();
    });
    heartbeat.start();
    QVERIFY(War3Map::QtApp::WaitForRecoveryExit(worker.get(), [] { return false; }));
    QVERIFY(sawProgress && beats >= 6);
}

void RecoveryExitDialogTests::AcceptedCancellationWaitsForWorker() {
    QObject timers;
    QSemaphore release;
    std::unique_ptr<QThread> worker(QThread::create([&] { release.acquire(); }));
    worker->start();
    const auto join = qScopeGuard([&] { release.release(); worker->wait(); });
    bool accepted = false, sawPreservation = false;
    QTimer::singleShot(0, &timers, [&] {
        auto* dialog = ActiveRecoveryDialog();
        if (!dialog) { release.release(); return; }
        auto* cancel = dialog->findChild<QPushButton*>(QStringLiteral("cancelRecoveryWrite"));
        cancel->click();
        const auto* label = dialog->findChild<QLabel*>(QStringLiteral("recoveryExitStatus"));
        sawPreservation = label->text().contains(QStringLiteral("上一份恢复点将保留")) && !cancel->isEnabled();
        QTimer::singleShot(30, &timers, [&] { release.release(); });
    });
    QVERIFY(War3Map::QtApp::WaitForRecoveryExit(worker.get(), [&] { accepted = true; return true; }));
    QVERIFY(accepted && sawPreservation && worker->isFinished());
}

void RecoveryExitDialogTests::PublicationRejectsLateCancellation() {
    QObject timers;
    QSemaphore release;
    std::unique_ptr<QThread> worker(QThread::create([&] { release.acquire(); }));
    worker->start();
    const auto join = qScopeGuard([&] { release.release(); worker->wait(); });
    bool rejectedTruthfully = false;
    QTimer::singleShot(0, &timers, [&] {
        auto* dialog = ActiveRecoveryDialog();
        if (!dialog) { release.release(); return; }
        dialog->findChild<QPushButton*>(QStringLiteral("cancelRecoveryWrite"))->click();
        rejectedTruthfully = dialog->findChild<QLabel*>(QStringLiteral("recoveryExitStatus"))->text()
            .contains(QStringLiteral("已无法取消"));
        release.release();
    });
    QVERIFY(War3Map::QtApp::WaitForRecoveryExit(worker.get(), [] { return false; }));
    QVERIFY(rejectedTruthfully);
}

void RecoveryExitDialogTests::ReturnToEditorLeavesWorkerRunning() {
    QObject timers;
    QSemaphore release;
    std::unique_ptr<QThread> worker(QThread::create([&] { release.acquire(); }));
    worker->start();
    const auto join = qScopeGuard([&] { release.release(); worker->wait(); });
    bool cancellationRequested = false;
    QTimer::singleShot(0, &timers, [&] {
        if (auto* dialog = ActiveRecoveryDialog()) dialog->findChild<QPushButton*>(QStringLiteral("returnToEditor"))->click();
        else release.release();
    });
    QVERIFY(!War3Map::QtApp::WaitForRecoveryExit(worker.get(), [&] { cancellationRequested = true; return true; }));
    QVERIFY(!cancellationRequested && worker->isRunning());
}

void RecoveryExitDialogTests::QuitFallbackCannotDismissActiveWrite() {
    QObject timers;
    QSemaphore release;
    std::unique_ptr<QThread> worker(QThread::create([&] { release.acquire(); }));
    worker->start();
    const auto join = qScopeGuard([&] { release.release(); worker->wait(); });
    bool cannotDismiss = false;
    QTimer::singleShot(0, &timers, [&] {
        auto* dialog = ActiveRecoveryDialog();
        if (!dialog) { release.release(); return; }
        dialog->reject();
        cannotDismiss = dialog->isVisible()
            && !dialog->findChild<QPushButton*>(QStringLiteral("returnToEditor"));
        release.release();
    });
    QVERIFY(War3Map::QtApp::WaitForRecoveryExit(worker.get(), [] { return false; }, nullptr, false));
    QVERIFY(cannotDismiss);
}

void RecoveryExitDialogTests::RepeatedApplicationExitKeepsOneCancellableDialog() {
    if (qEnvironmentVariable("FORGE_RECOVERY_REPEAT_EXIT_CHILD") != QStringLiteral("1")) {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        QProcess child;
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("FORGE_RECOVERY_REPEAT_EXIT_CHILD"), QStringLiteral("1"));
        environment.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
        environment.insert(QStringLiteral("TMP"), temporary.path());
        environment.insert(QStringLiteral("TEMP"), temporary.path());
        child.setProcessEnvironment(environment);
        child.setWorkingDirectory(temporary.path());
        const auto stopChild = qScopeGuard([&] {
            if (child.state() != QProcess::NotRunning) { child.kill(); (void)child.waitForFinished(3000); }
        });
        child.start(QCoreApplication::applicationFilePath(),
            {QStringLiteral("RepeatedApplicationExitKeepsOneCancellableDialog"), QStringLiteral("-silent")});
        QVERIFY2(child.waitForStarted(3000), qPrintable(child.errorString()));
        const bool completed = child.waitForFinished(10000);
        const auto output = child.readAllStandardOutput().right(8192) + child.readAllStandardError().right(8192);
        QVERIFY2(completed && child.exitStatus() == QProcess::NormalExit && child.exitCode() == 0, output.constData());
        return;
    }
    QObject timers;
    QSemaphore release;
    std::unique_ptr<QThread> worker(QThread::create([&] { release.acquire(); }));
    worker->start();
    const auto join = qScopeGuard([&] { release.release(); worker->wait(); });
    const bool previousQuit = QApplication::quitOnLastWindowClosed();
    QApplication::setQuitOnLastWindowClosed(false);
    const auto restoreQuit = qScopeGuard([&] { QApplication::setQuitOnLastWindowClosed(previousQuit); });
    bool waiting = false, timedOut = false, cancelled = false, preservedNotice = false, waited = false;
    int calls = 0, beats = 0, maxDialogs = 0;
    const auto connection = connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, &timers, [&] {
        ++calls;
        if (waiting) return; // 与生产job退出连接相同，重复exit不会再创建模态。
        waiting = true;
        const auto settle = qScopeGuard([&] { waiting = false; });
        waited = War3Map::QtApp::WaitForRecoveryExit(worker.get(), [&] {
            cancelled = true; release.release(); return true;
        }, nullptr, false);
        worker->wait();
    });
    const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
    QTimer heartbeat;
    heartbeat.setInterval(5);
    connect(&heartbeat, &QTimer::timeout, &timers, [&] {
        ++beats;
        int count = 0;
        for (const auto* widget : QApplication::topLevelWidgets())
            if (widget->isVisible() && widget->objectName() == QStringLiteral("recoveryExitDialog")) ++count;
        maxDialogs = qMax(maxDialogs, count);
        if (beats >= 6 && !cancelled) {
            if (auto* dialog = ActiveRecoveryDialog()) {
                auto* cancel = dialog->findChild<QPushButton*>(QStringLiteral("cancelRecoveryWrite"));
                cancel->click();
                preservedNotice = dialog->findChild<QLabel*>(QStringLiteral("recoveryExitStatus"))->text()
                    .contains(QStringLiteral("上一份恢复点将保留"));
            }
        }
    });
    heartbeat.start();
    QTimer::singleShot(3000, &timers, [&] { timedOut = true; release.release(); });
    QTimer::singleShot(0, &timers, [] { QCoreApplication::exit(0); });
    QTimer::singleShot(0, &timers, [] { QCoreApplication::exit(0); });
    QCOMPARE(QCoreApplication::exec(), 0);
    QVERIFY(!timedOut && cancelled && preservedNotice && waited && worker->isFinished());
    QVERIFY(calls >= 2 && beats >= 6 && maxDialogs == 1);
}

void RecoveryExitDialogTests::DestroyedParentStillSettlesOwnedWorker() {
    QObject timers;
    QSemaphore release;
    std::unique_ptr<QThread> worker(QThread::create([&] { release.acquire(); }));
    worker->start();
    const auto join = qScopeGuard([&] { release.release(); worker->wait(); });
    auto parent = std::make_unique<QWidget>();
    bool requested = false;
    QTimer::singleShot(0, &timers, [&] { parent.reset(); });
    QTimer::singleShot(1000, &timers, [&] { release.release(); });
    QVERIFY(War3Map::QtApp::WaitForRecoveryExit(worker.get(), [&] {
        requested = true; release.release(); return true;
    }, parent.get(), false));
    QVERIFY(requested && worker->isFinished() && !parent);
}

QTEST_MAIN(RecoveryExitDialogTests)
#include "RecoveryExitDialogTests.moc"
