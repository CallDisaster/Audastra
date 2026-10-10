#pragma once

#include "War3MapCore/Trigger/TriggerEditing.h"

#include <QUndoCommand>

// 把函数提供的参数换成其他来源时，先摘除原函数子树。推入时摘除已经应用；
// undo 按原索引精确补回，redo 再次摘除（同一宏里之后的新值 / 新函数由各自的命令负责）。
class DetachArgumentCallUndoCommand final : public QUndoCommand {
public:
    DetachArgumentCallUndoCommand(
        War3Map::Trigger::TriggerDocument* document,
        War3Map::Trigger::TriggerDocumentEditor::ArgumentCallDetachReceipt receipt,
        QString text,
        QUndoCommand* parent = nullptr);

    void undo() override;
    void redo() override;

private:
    War3Map::Trigger::TriggerDocument* document_ = nullptr;
    War3Map::Trigger::TriggerDocumentEditor::ArgumentCallDetachReceipt receipt_;
    bool firstRedo_ = true;
};
