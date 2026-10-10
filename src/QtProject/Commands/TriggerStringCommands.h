#pragma once

#include "War3MapCore/Services/TriggerStringEditing.h"

#include <QString>
#include <QUndoCommand>

namespace War3Map::QtProject {
class EditorProject;

// WTG 引用与 WTS 正文共用一个严格回执，归入触发器撤销栈的一次修改。
class TriggerStringEditCommand final : public QUndoCommand {
public:
    TriggerStringEditCommand(EditorProject& project, Services::TriggerStringEditReceipt receipt, QString text);
    void undo() override { Apply(false); }
    void redo() override;

private:
    void Apply(bool forward);
    EditorProject& project_;
    Services::TriggerStringEditReceipt receipt_;
    bool firstRedo_ = true;
};

} // namespace War3Map::QtProject
