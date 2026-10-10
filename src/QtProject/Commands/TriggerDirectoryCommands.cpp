#include "TriggerDirectoryCommands.h"

#include <QDebug>

TriggerDirectoryUndoCommand::TriggerDirectoryUndoCommand(
    War3Map::Trigger::TriggerDocument* document,
    War3Map::Trigger::TriggerDirectoryLayout before,
    War3Map::Trigger::TriggerDirectoryLayout after,
    QString text,
    QUndoCommand* parent)
    : QUndoCommand(std::move(text), parent),
      document_(document),
      before_(std::move(before)),
      after_(std::move(after)) {
}

void TriggerDirectoryUndoCommand::undo() {
    War3Map::Trigger::TriggerDirectoryEditor::ApplyLayout(*document_, before_);
}

void TriggerDirectoryUndoCommand::redo() {
    if (firstRedo_) {
        firstRedo_ = false;
        return;
    }
    War3Map::Trigger::TriggerDirectoryEditor::ApplyLayout(*document_, after_);
}

MoveEcaUndoCommand::MoveEcaUndoCommand(
    War3Map::Trigger::TriggerDocument* document,
    War3Map::Trigger::TriggerDocumentEditor::EcaMoveReceipt receipt,
    QString text,
    QUndoCommand* parent)
    : QUndoCommand(std::move(text), parent),
      document_(document),
      receipt_(std::move(receipt)) {
}

void MoveEcaUndoCommand::undo() {
    const War3Map::Status status = War3Map::Trigger::TriggerDocumentEditor::UndoMoveEca(*document_, receipt_);
    if (!status.Succeeded()) {
        qCritical() << "撤销移动语句失败" << QString::fromUtf8(status.diagnostics.front().message);
    }
}

void MoveEcaUndoCommand::redo() {
    if (firstRedo_) {
        firstRedo_ = false;
        return;
    }
    const War3Map::Status status = War3Map::Trigger::TriggerDocumentEditor::RedoMoveEca(*document_, receipt_);
    if (!status.Succeeded()) {
        qCritical() << "重做移动语句失败" << QString::fromUtf8(status.diagnostics.front().message);
    }
}
