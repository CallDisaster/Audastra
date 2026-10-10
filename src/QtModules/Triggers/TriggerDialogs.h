#pragma once

#include "TriggerFunctionList.h"

#include "War3MapCore/Trigger/TriggerDocument.h"
#include "War3MapCore/Trigger/TriggerTypes.h"

#include <QDialog>
#include <QString>

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QScrollArea;
class QSplitter;
class QStandardItem;
class QStandardItemModel;
class QTableWidget;
class QTextBrowser;
class QTreeView;
class QUndoStack;

namespace War3Map::Trigger {
class UiCatalog;
struct UiDefinition;
}

// WE 式函数选择：分类筛选 + “分类 - 函数名”平铺列表（带分类图标）+ 详情。
// 可调整大小，尺寸与分栏比例记在本机设置里。
class UiDefinitionPickerDialog final : public QDialog {
public:
    UiDefinitionPickerDialog(
        const War3Map::Trigger::UiCatalog& catalog,
        War3Map::Trigger::UiDefinitionKind kind,
        std::string expectedReturnType,
        TriggerFunctionList::CategoryIcon categoryIcon = {},
        QWidget* parent = nullptr);

    [[nodiscard]] const War3Map::Trigger::UiDefinition* SelectedDefinition() const;

    void done(int result) override;

private:
    void BuildCategoryFilter();
    void RebuildList();
    void ShowCurrentDefinition();
    [[nodiscard]] QStandardItem* MakeDefinitionItem(
        const War3Map::Trigger::UiDefinition* definition) const;

    const War3Map::Trigger::UiCatalog& catalog_;
    War3Map::Trigger::UiDefinitionKind kind_;
    std::string expectedReturnType_;
    TriggerFunctionList functions_;
    // 按返回类型筛过、按列表标题排好序的候选。
    std::vector<const War3Map::Trigger::UiDefinition*> candidates_;
    QComboBox* category_ = nullptr;
    QLineEdit* search_ = nullptr;
    QTreeView* definitions_ = nullptr;
    QStandardItemModel* definitionModel_ = nullptr;
    QTextBrowser* details_ = nullptr;
    QSplitter* splitter_ = nullptr;
};

struct TriggerArgumentEditRequest {
    War3Map::StableId argumentId = 0;
    std::int32_t valueKind = 3;
    std::string value;
    std::string expectedType;
    std::optional<std::string> callFunction;
    // 新挂接函数自身的参数（按位置；valueKind 与值），例如局部变量读取的变量名。
    std::vector<std::pair<std::int32_t, std::string>> callArguments;
};

class TriggerParameterDialog final : public QDialog {
public:
    TriggerParameterDialog(
        const War3Map::Trigger::TriggerDocument& document,
        const War3Map::Trigger::UiCatalog& catalog,
        War3Map::StableId ecaId,
        QWidget* parent = nullptr);
    ~TriggerParameterDialog() override;

    [[nodiscard]] std::vector<TriggerArgumentEditRequest> PendingEdits() const;
    // 嵌套函数选择器的分类图标来源。
    void SetCategoryIcons(TriggerFunctionList::CategoryIcon icons) { categoryIcons_ = std::move(icons); }
    [[nodiscard]] std::optional<War3Map::StableId> RequestedNestedEca() const noexcept;

protected:
    void accept() override;

private:
    struct Row;
    void UpdateNaturalPreview();
    void FlashRow(Row* row);
    void RefreshPresets(Row* row, const War3Map::Trigger::ArgumentNode& argument);
    void RestrictKinds(Row* row, const War3Map::Trigger::TriggerTypeSystem& types);
    void AddArgumentRow(
        std::size_t position,
        const War3Map::Trigger::UiArgumentDefinition* definition,
        std::string label,
        const War3Map::Trigger::ArgumentNode& argument);

    const War3Map::Trigger::TriggerDocument& document_;
    const War3Map::Trigger::UiCatalog& catalog_;
    War3Map::StableId ecaId_ = 0;
    QLabel* naturalPreview_ = nullptr;
    QScrollArea* scroll_ = nullptr;
    std::vector<Row*> rows_;
    std::optional<War3Map::StableId> requestedNestedEca_;
    TriggerFunctionList::CategoryIcon categoryIcons_;
};

// 兼容旧调用入口；变量编辑器由独立模块提供。
#include "VariablesDialog.h"
