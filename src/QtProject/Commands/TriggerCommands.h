#pragma once

#include "War3MapCore/Trigger/TriggerEditing.h"

#include <QUndoCommand>
#include <QPointer>
#include <memory>
#include "War3MapCore/Trigger/TriggerCodeEditing.h"
#include "War3MapCore/Trigger/GuiConversionSnapshots.h"
#include "War3MapCore/Trigger/TriggerGraphEditing.h"

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

// Core 回执类型的本地别名（嵌套于 TriggerDocumentEditor）。
using EcaRemoveReceipt = War3Map::Trigger::TriggerDocumentEditor::EcaRemoveReceipt;
using TriggerRemoveReceipt =
    War3Map::Trigger::TriggerDocumentEditor::TriggerRemoveReceipt;
using EcaSubtreeSnapshot =
    War3Map::Trigger::TriggerDocumentEditor::EcaSubtreeSnapshot;

// 粘贴 ECA 子树（推入时 Core PasteEcaSubtree 已应用）。
// undo=删除粘贴体，redo=按快照重新粘贴。
class PasteEcaUndoCommand final : public QUndoCommand {
public:
    PasteEcaUndoCommand(
        War3Map::Trigger::TriggerDocument* document,
        const War3Map::Trigger::UiCatalog* catalog,
        EcaSubtreeSnapshot snapshot,
        War3Map::StableId parentTriggerId,
        War3Map::StableId parentEcaId,
        std::size_t row,
        std::optional<std::int32_t> rootChildId,
        War3Map::StableId rootEcaId,
        QString text,
        QUndoCommand* parent = nullptr);

    void undo() override;
    void redo() override;

private:
    War3Map::Trigger::TriggerDocument* document_ = nullptr;
    const War3Map::Trigger::UiCatalog* catalog_ = nullptr;
    EcaSubtreeSnapshot snapshot_;
    War3Map::StableId parentTriggerId_ = 0;
    War3Map::StableId parentEcaId_ = 0;
    std::size_t row_ = 0;
    std::optional<std::int32_t> rootChildId_;
    War3Map::StableId rootEcaId_ = 0;
    bool firstRedo_ = true;
};

// 新建触发器（推入时 Core AddTrigger 已应用）。undo=删除整个触发器，
// redo=按删除回执恢复。
class AddTriggerUndoCommand final : public QUndoCommand {
public:
    AddTriggerUndoCommand(
        War3Map::Trigger::TriggerDocument* document,
        War3Map::StableId triggerId,
        QString text,
        QUndoCommand* parent = nullptr,
        War3Map::Trigger::GuiConversionSnapshots* snapshots = nullptr);

    void undo() override;
    void redo() override;

private:
    War3Map::Trigger::TriggerDocument* document_ = nullptr;
    War3Map::StableId triggerId_ = 0;
    std::optional<TriggerRemoveReceipt> removal_;
    War3Map::Trigger::GuiConversionSnapshots* snapshots_ = nullptr;
    War3Map::Trigger::GuiConversionSnapshots::PreparedSnapshot snapshot_;
    bool firstRedo_ = true;
};

class RenameTriggerUndoCommand final : public QUndoCommand {
public:
    RenameTriggerUndoCommand(
        War3Map::Trigger::TriggerDocument* document,
        War3Map::StableId triggerId,
        std::string before,
        std::string after,
        QString text,
        QUndoCommand* parent = nullptr);

    void undo() override;
    void redo() override;

private:
    void Apply(const std::string& name);

    War3Map::Trigger::TriggerDocument* document_ = nullptr;
    War3Map::StableId triggerId_ = 0;
    std::string before_;
    std::string after_;
    bool firstRedo_ = true;
};

class RemoveTriggerUndoCommand final : public QUndoCommand {
public:
    RemoveTriggerUndoCommand(
        War3Map::Trigger::TriggerDocument* document,
        TriggerRemoveReceipt receipt,
        QString text,
        QUndoCommand* parent = nullptr,
        War3Map::Trigger::GuiConversionSnapshots* snapshots = nullptr);

    void undo() override;
    void redo() override;

private:
    War3Map::Trigger::TriggerDocument* document_ = nullptr;
    TriggerRemoveReceipt receipt_;
    War3Map::Trigger::GuiConversionSnapshots* snapshots_ = nullptr;
    War3Map::Trigger::GuiConversionSnapshots::PreparedSnapshot snapshot_;
    bool firstRedo_ = true;
};

class AddVariableUndoCommand final : public QUndoCommand {
public:
    AddVariableUndoCommand(
        War3Map::Trigger::TriggerDocument* document,
        std::string name,
        std::string typeName,
        bool isArray,
        QString text,
        QUndoCommand* parent = nullptr);

    void undo() override;
    void redo() override;

private:
    War3Map::Trigger::TriggerDocument* document_ = nullptr;
    std::string name_;
    std::string typeName_;
    bool isArray_ = false;
    bool firstRedo_ = true;
};

class RenameVariableUndoCommand final : public QUndoCommand {
public:
    RenameVariableUndoCommand(
        War3Map::Trigger::TriggerDocument* document,
        std::string before,
        std::string after,
        QString text,
        QUndoCommand* parent = nullptr);

    void undo() override;
    void redo() override;

private:
    void Apply(const std::string& from, const std::string& to);

    War3Map::Trigger::TriggerDocument* document_ = nullptr;
    std::string before_;
    std::string after_;
    bool firstRedo_ = true;
};

class RemoveVariableUndoCommand final : public QUndoCommand {
public:
    RemoveVariableUndoCommand(
        War3Map::Trigger::TriggerDocument* document,
        War3Map::Trigger::VariableRecord record,
        std::size_t row,
        QString text,
        QUndoCommand* parent = nullptr);

    void undo() override;
    void redo() override;

private:
    War3Map::Trigger::TriggerDocument* document_ = nullptr;
    War3Map::Trigger::VariableRecord record_;
    std::size_t row_ = 0;
    bool firstRedo_ = true;
};

struct TriggerArgumentChange {
    War3Map::StableId argumentId = 0;
    War3Map::Trigger::ArgumentNode before;
    War3Map::Trigger::ArgumentNode after;
};

// 触发器属性快照（启用/初始关闭/初始化运行/注释）。
struct TriggerPropertyState {
    std::int32_t enabled = 1;
    std::int32_t initiallyClosed = 0;
    std::int32_t runOnInit = 0;
    std::string comment;
};

class AppendEcaUndoCommand final : public QUndoCommand {
public:
    AppendEcaUndoCommand(
        War3Map::Trigger::TriggerDocument* document,
        War3Map::Trigger::EcaAppendReceipt receipt,
        QString text,
        QUndoCommand* parent = nullptr);

    void undo() override;
    void redo() override;

private:
    War3Map::Trigger::TriggerDocument* document_ = nullptr;
    War3Map::Trigger::EcaAppendReceipt receipt_;
    bool firstRedo_ = true;
};

class EditTriggerArgumentsUndoCommand final : public QUndoCommand {
public:
    EditTriggerArgumentsUndoCommand(
        War3Map::Trigger::TriggerDocument* document,
        std::vector<TriggerArgumentChange> changes,
        QString text,
        QUndoCommand* parent = nullptr);

    void undo() override;
    void redo() override;

private:
    void Apply(bool after);

    War3Map::Trigger::TriggerDocument* document_ = nullptr;
    std::vector<TriggerArgumentChange> changes_;
    bool firstRedo_ = true;
};

class EditTriggerPropertiesUndoCommand final : public QUndoCommand {
public:
    EditTriggerPropertiesUndoCommand(
        War3Map::Trigger::TriggerDocument* document,
        War3Map::StableId triggerId,
        TriggerPropertyState before,
        TriggerPropertyState after,
        QString text,
        QUndoCommand* parent = nullptr);

    void undo() override;
    void redo() override;

private:
    void Apply(const TriggerPropertyState& state);

    War3Map::Trigger::TriggerDocument* document_ = nullptr;
    War3Map::StableId triggerId_ = 0;
    TriggerPropertyState before_;
    TriggerPropertyState after_;
};

class SetEcaEnabledUndoCommand final : public QUndoCommand {
public:
    SetEcaEnabledUndoCommand(
        War3Map::Trigger::TriggerDocument* document,
        War3Map::StableId ecaId,
        bool before,
        bool after,
        QString text,
        QUndoCommand* parent = nullptr);

    void undo() override;
    void redo() override;

private:
    void Apply(bool enabled);

    War3Map::Trigger::TriggerDocument* document_ = nullptr;
    War3Map::StableId ecaId_ = 0;
    bool before_ = true;
    bool after_ = true;
};

class RemoveEcaUndoCommand final : public QUndoCommand {
public:
    RemoveEcaUndoCommand(
        War3Map::Trigger::TriggerDocument* document,
        EcaRemoveReceipt receipt,
        QString text,
        QUndoCommand* parent = nullptr);

    void undo() override;
    void redo() override;

private:
    War3Map::Trigger::TriggerDocument* document_ = nullptr;
    EcaRemoveReceipt receipt_;
    bool firstRedo_ = true;
};

namespace War3Map::QtProject {
class EditorProject;

// GUI 与 MCP 共用事务撤销；一般图保持整图快照，标量批量持有 Core 密封收据。
// 两种存储都在首次 redo 真正发布；标量 Ready 只做会话门，Core 在 Apply 严核整图。
class TriggerGraphSnapshotUndoCommand final : public QUndoCommand {
public:
    TriggerGraphSnapshotUndoCommand(EditorProject& project,
        Trigger::TriggerDocument before, Trigger::TriggerDocument after,
        QString label, QUndoCommand* parent = nullptr);
    TriggerGraphSnapshotUndoCommand(EditorProject& project,
        Trigger::TriggerGraphScalarEditReceipt receipt,
        QString label, QUndoCommand* parent = nullptr);
    [[nodiscard]] bool Ready() const;
    [[nodiscard]] std::shared_ptr<const bool> FirstPublication() const { return firstPublication_; }
    void undo() override { Apply(false); }
    void redo() override { Apply(true); }

private:
    void Apply(bool forward);
    QPointer<EditorProject> project_;
    Trigger::TriggerDocument* document_ = nullptr;
    const Trigger::UiCatalog* catalog_ = nullptr;
    std::uint64_t generation_ = 0;
    struct SnapshotStorage {
        SnapshotStorage(Trigger::TriggerDocument beforeValue, Trigger::TriggerDocument afterValue)
            : before(std::move(beforeValue)), after(std::move(afterValue)) {}
        Trigger::TriggerDocument before, after;
        std::optional<Trigger::TriggerGraphReplayIdentity> beforeBinding, afterBinding;
        std::optional<Trigger::TriggerDocument> preparedFirst;
        std::vector<Trigger::GuiConversionSnapshots::PreparedSnapshot> changedSnapshots;
    };
    // 标量模式不构造 SnapshotStorage，也不拥有任何完整图候选。
    std::optional<SnapshotStorage> snapshot_;
    std::optional<Trigger::TriggerGraphScalarEditReceipt> scalarReceipt_;
    std::shared_ptr<bool> firstPublication_ = std::make_shared<bool>(false);
    [[nodiscard]] bool CanApply(bool forward) const;
};
} // namespace War3Map::QtProject
