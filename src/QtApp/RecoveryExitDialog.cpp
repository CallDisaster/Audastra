#include "RecoveryExitDialog.h"

#include <QDialog>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QEventLoop>
#include <QLabel>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QScopeGuard>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>
#include <memory>

namespace War3Map::QtApp {
namespace {
class RecoveryExitDialog final : public QDialog {
public:
    explicit RecoveryExitDialog(QWidget* parent, bool allowReturn, std::shared_ptr<bool> returned)
        : QDialog(parent), allowReturn_(allowReturn), returned_(std::move(returned)) {}
    void reject() override {
        if (allowReturn_) { *returned_ = true; QDialog::reject(); }
    }
private:
    bool allowReturn_;
    std::shared_ptr<bool> returned_;
};
}

bool WaitForRecoveryExit(QThread* worker, const std::function<bool()>& cancelWrite,
    QWidget* parent, bool allowReturnToEditor) {
    if (!worker || worker->isFinished()) return true;
    const QPointer<QThread> ownedWorker(worker);
    // 父窗口可能在嵌套事件处理中释放；用QPointer跟踪堆对象，不能让父析构delete栈上dialog。
    const auto returned = std::make_shared<bool>(false); // 只在GUI线程使用，父窗口删除后仍保留明确选择。
    const QPointer<RecoveryExitDialog> dialog(new RecoveryExitDialog(parent, allowReturnToEditor, returned));
    const auto dispose = qScopeGuard([&] { if (dialog) delete dialog.data(); });
    dialog->setObjectName(QStringLiteral("recoveryExitDialog"));
    dialog->setWindowTitle(QObject::tr("正在写入恢复点"));
    dialog->setMinimumWidth(420);
    dialog->setWindowModality(Qt::ApplicationModal);
    auto* layout = new QVBoxLayout(dialog);
    auto* label = new QLabel(QObject::tr("正在写入恢复点…"), dialog);
    label->setObjectName(QStringLiteral("recoveryExitStatus"));
    label->setWordWrap(true);
    layout->addWidget(label);
    auto* progress = new QProgressBar(dialog);
    progress->setRange(0, 0);
    progress->setTextVisible(false);
    layout->addWidget(progress);
    auto* buttons = new QDialogButtonBox(dialog);
    auto* cancel = buttons->addButton(QObject::tr("取消本次写入"), QDialogButtonBox::ActionRole);
    cancel->setObjectName(QStringLiteral("cancelRecoveryWrite"));
    cancel->setEnabled(static_cast<bool>(cancelWrite));
    if (allowReturnToEditor) {
        auto* back = buttons->addButton(QObject::tr("返回编辑"), QDialogButtonBox::RejectRole);
        back->setObjectName(QStringLiteral("returnToEditor"));
        QObject::connect(back, &QPushButton::clicked, dialog, &QDialog::reject);
    }
    layout->addWidget(buttons);
    QObject::connect(cancel, &QPushButton::clicked, dialog, [&] {
        const bool accepted = cancelWrite && cancelWrite();
        label->setText(accepted
            ? QObject::tr("正在取消写入，请稍候。\n上一份恢复点将保留。")
            : QObject::tr("本次写入已无法取消，正在完成最后步骤。\n请稍候。"));
        cancel->setEnabled(false);
    });
    QObject::connect(worker, &QThread::finished, dialog, &QDialog::accept);
    // 同时覆盖 finished 已发射而排队回调尚未送达的窗口，以及已释放的线程对象。
    QTimer poll;
    poll.setInterval(25);
    QObject::connect(&poll, &QTimer::timeout, dialog, [&] {
        if (!ownedWorker || ownedWorker->isFinished()) dialog->accept();
    });
    poll.start();
    (void)dialog->exec();
    if (*returned) return false;
    // 直接重复exit可退出所有嵌套exec；仅在此退出兜底继续处理有界事件批次，保留取消按钮。
    // 正常完成与用户返回编辑均不走此轮询，不擅自终止拥有文件载荷的线程。
    if (dialog && ownedWorker && !ownedWorker->isFinished()) dialog->show();
    bool requestedAfterParentDestruction = false;
    while (ownedWorker && !ownedWorker->isFinished()) {
        if (!dialog && !requestedAfterParentDestruction) {
            requestedAfterParentDestruction = true;
            if (cancelWrite) (void)cancelWrite();
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        if (*returned) return false;
        QThread::msleep(1);
    }
    return true;
}

} // namespace War3Map::QtApp
