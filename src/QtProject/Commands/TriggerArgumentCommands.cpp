#include "TriggerArgumentCommands.h"

#include <QDebug>

namespace {

void Report(const char* action, const War3Map::Status& status) {
    if (!status.Succeeded()) {
        qCritical() << action
                    << (status.diagnostics.empty() ? QStringLiteral("未知错误")
                                                   : QString::fromUtf8(status.diagnostics.front().message));
    }
}

} // namespace

DetachArgumentCallUndoCommand::DetachArgumentCallUndoCommand(
    War3Map::Trigger::TriggerDocument* document,
    War3Map::Trigger::TriggerDocumentEditor::ArgumentCallDetachReceipt receipt,
    QString text,
    QUndoCommand* parent)
    : QUndoCommand(std::move(text), parent),
      document_(document),
      receipt_(std::move(receipt)) {
}

void DetachArgumentCallUndoCommand::undo() {
    if (document_ != nullptr) {
        Report("函数参数摘除撤销失败", War3Map::Trigger::TriggerDocumentEditor::UndoDetachArgumentCall(*document_, receipt_));
    }
}

void DetachArgumentCallUndoCommand::redo() {
    if (firstRedo_) {
        firstRedo_ = false;
        return;
    }
    if (document_ != nullptr) {
        Report("函数参数摘除重做失败", War3Map::Trigger::TriggerDocumentEditor::RedoDetachArgumentCall(*document_, receipt_));
    }
}
