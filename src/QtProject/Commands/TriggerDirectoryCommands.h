#pragma once

#include "War3MapCore/Trigger/TriggerDirectory.h"

#include <QUndoCommand>

// 触发器目录布局修改（新建 / 重命名 / 删除空类别、移动类别、移动触发器）。
// 推入时修改已经应用；undo 恢复 before 布局，redo 恢复 after 布局。
class TriggerDirectoryUndoCommand final : public QUndoCommand {
public:
    TriggerDirectoryUndoCommand(
        War3Map::Trigger::TriggerDocument* document,
        War3Map::Trigger::TriggerDirectoryLayout before,
        War3Map::Trigger::TriggerDirectoryLayout after,
        QString text,
        QUndoCommand* parent = nullptr);

    void undo() override;
    void redo() override;

private:
    War3Map::Trigger::TriggerDocument* document_ = nullptr;
    War3Map::Trigger::TriggerDirectoryLayout before_;
    War3Map::Trigger::TriggerDirectoryLayout after_;
    bool firstRedo_ = true;
};

// 语句移动（重排 / 跨分支）。推入时移动已经应用；undo / redo 按回执改回或重做挂载关系。
class MoveEcaUndoCommand final : public QUndoCommand {
public:
    MoveEcaUndoCommand(
        War3Map::Trigger::TriggerDocument* document,
        War3Map::Trigger::TriggerDocumentEditor::EcaMoveReceipt receipt,
        QString text,
        QUndoCommand* parent = nullptr);

    void undo() override;
    void redo() override;

private:
    War3Map::Trigger::TriggerDocument* document_ = nullptr;
    War3Map::Trigger::TriggerDocumentEditor::EcaMoveReceipt receipt_;
    bool firstRedo_ = true;
};
