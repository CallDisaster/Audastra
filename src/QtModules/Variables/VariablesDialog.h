#pragma once
#include "War3MapCore/Trigger/TriggerDocument.h"
#include "War3MapCore/Services/NameResolution.h"
#include "VariablePropertiesDialog.h"
#include <QDialog>
#include <QStringList>
#include <functional>
#include <optional>
class QUndoStack;
class QTableWidget;
class QPushButton;

// 变量管理对话框：列出全局变量，支持新建/属性/重命名/删除（经撤销栈）。
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
    // 新建与“属性…”改用类型化表单，经共用触发器图编辑整批发布（与 Agent 同一检查与撤销）。
    // 未接通时新建退回旧的简单表单，属性不可用。
    struct GraphEditing {
        std::function<const War3Map::Trigger::UiCatalog*()> catalog;
        VariablePropertiesDialog::ObjectChoices objects;
        // 发布失败返回诊断且不改文档。
        std::function<War3Map::Status(std::vector<War3Map::Trigger::GraphEditOperation>, const QString&)> publish;
    };
    void SetGraphEditing(GraphEditing editing);
    // 删除前查代码里的引用：返回要列在确认框里的行，返回空表示不删（取消或期间地图有变化）。未设置时只确认一次。
    struct RemovalReview { QStringList lines; std::size_t references = 0; QString incomplete; };
    using RemovalCheck = std::function<std::optional<RemovalReview>(const War3Map::Trigger::VariableRecord&)>;
    void SetRemovalCheck(RemovalCheck check) { removalCheck_ = std::move(check); }

private:
    void RebuildList(std::optional<War3Map::StableId> preferredId = std::nullopt);
    void AddVariable();
    void EditSelected();
    [[nodiscard]] bool GraphEditingReady() const;
    void RenameSelected();
    void RemoveSelected();
    [[nodiscard]] std::optional<std::size_t> SelectedRow() const;

    War3Map::Trigger::TriggerDocument* document_ = nullptr;
    QUndoStack* undoStack_ = nullptr;
    QTableWidget* table_ = nullptr;
    QPushButton* propertiesButton_ = nullptr;
    QPushButton* renameButton_ = nullptr;
    QPushButton* removeButton_ = nullptr;
    bool embedded_ = false;
    bool showRawValues_ = false;
    NameResolver nameResolver_;
    GraphEditing graphEditing_;
    RemovalCheck removalCheck_;
};
