#pragma once
#include "War3MapCore/Trigger/TriggerDocument.h"
#include "War3MapCore/Services/NameResolution.h"
#include <QDialog>
#include <functional>
#include <optional>
class QUndoStack;
class QTableWidget;
class QPushButton;

// 变量管理对话框：列出全局变量，支持新建/重命名/删除（经撤销栈）。
class VariablesDialog final : public QDialog {
public:
    VariablesDialog(
        War3Map::Trigger::TriggerDocument* document,
        QUndoStack* undoStack,
        QWidget* parent = nullptr,
        bool embedded = false);

    void accept() override;
    void reject() override;
    using NameResolver = std::function<War3Map::Services::NameResolution(std::string_view, std::string_view)>;
    void SetNameResolver(NameResolver resolver);
    void RefreshNames() { RebuildList(); }

private:
    void RebuildList(std::optional<War3Map::StableId> preferredId = std::nullopt);
    void AddVariable();
    void RenameSelected();
    void RemoveSelected();
    [[nodiscard]] std::optional<std::size_t> SelectedRow() const;

    War3Map::Trigger::TriggerDocument* document_ = nullptr;
    QUndoStack* undoStack_ = nullptr;
    QTableWidget* table_ = nullptr;
    QPushButton* renameButton_ = nullptr;
    QPushButton* removeButton_ = nullptr;
    bool embedded_ = false;
    bool showRawValues_ = false;
    NameResolver nameResolver_;
};
