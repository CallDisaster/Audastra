#pragma once

#include "War3MapCore/Trigger/TriggerCodeEditing.h"
#include "War3MapCore/Trigger/GuiConversionSnapshots.h"

#include <QString>
#include <QUndoCommand>
#include <string>
#include <utility>

namespace War3Map::QtProject {
class EditorProject;

// 代码正文、新建与 GUI 转换都只重放共用服务签发的单一收据。
class TriggerCodeEditCommand final : public QUndoCommand {
public:
    TriggerCodeEditCommand(EditorProject& project, Trigger::TriggerCodeEditReceipt receipt, QString label);
    void SetReceipt(Trigger::TriggerCodeEditReceipt receipt) noexcept { receipt_ = std::move(receipt); }
    // 首次应用前已观察到的触发器指纹（修改前状态）；撤销时直接提供给修订观察。
    void SetBeforeFingerprint(std::string fingerprint) { beforeFingerprint_ = std::move(fingerprint); }
    void SetSnapshot(Trigger::GuiConversionSnapshots::PreparedSnapshot snapshot) noexcept { snapshot_ = std::move(snapshot); }
    void PublishSnapshot() noexcept;
    void undo() override { Apply(false); }
    void redo() override;

private:
    void Apply(bool forward);
    EditorProject& project_;
    Trigger::TriggerCodeEditReceipt receipt_;
    Trigger::GuiConversionSnapshots::PreparedSnapshot snapshot_;
    bool firstRedo_ = true;
    std::string beforeFingerprint_, afterFingerprint_;
};

} // namespace War3Map::QtProject
