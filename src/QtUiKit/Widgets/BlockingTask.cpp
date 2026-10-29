#include "BlockingTask.h"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QEventLoop>
#include <QProgressDialog>
#include <QThread>

#include <memory>

namespace War3Map::UiKit {

bool RunBlockingTask(QWidget* parent, const QString& label, const QString& cancelText,
    const std::function<void(const BlockingTaskControl&)>& work) {
    std::atomic_bool stop = false;
    QProgressDialog dialog(label, cancelText, 0, 0, parent);
    dialog.setWindowModality(Qt::WindowModal);
    if (cancelText.isEmpty()) dialog.setCancelButton(nullptr);
    dialog.setAutoClose(false);
    dialog.setAutoReset(false);
    dialog.setMinimumWidth(420);
    dialog.setWindowFlag(Qt::WindowCloseButtonHint, false);
    // 进度回调在工作线程调用：排队到界面线程。对话框在工作线程结束后才销毁，未处理的排队事件随之丢弃。
    BlockingTaskControl control;
    control.stop = &stop;
    control.progress = [target = &dialog](const QString& text) {
        QMetaObject::invokeMethod(target, [target, text] { target->setLabelText(text); }, Qt::QueuedConnection);
    };
    std::unique_ptr<QThread> worker(QThread::create([&work, &control] { work(control); }));
    QEventLoop loop;
    QObject::connect(worker.get(), &QThread::finished, &loop, &QEventLoop::quit);
    // 点取消后对话框会自己隐藏；任务停下之前重新显示，等待期间界面仍不可操作。
    QObject::connect(&dialog, &QProgressDialog::canceled, &dialog, [&stop, &dialog, cancellable = !cancelText.isEmpty()] {
        if (cancellable) {
            stop.store(true);
            dialog.setLabelText(QCoreApplication::translate("BlockingTask", "正在停止…"));
        }
        dialog.setCancelButton(nullptr);
        dialog.show();
    });
    worker->start(QThread::LowPriority);
    // 很快就结束的任务直接等完：不闪进度框，这期间也不处理别的输入。
    if (!worker->wait(QDeadlineTimer(400))) {
        dialog.show();
        if (!worker->isFinished()) loop.exec();
        worker->wait();
    }
    return !stop.load();
}

} // namespace War3Map::UiKit
