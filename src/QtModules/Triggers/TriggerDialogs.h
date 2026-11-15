#pragma once

#include "TriggerFunctionList.h"
#include "Widgets/ObjectResourcePicker.h"

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

// 传统参数对话框（WE 式）：每个参数把可用的来源（预设、变量、函数、值）同时列出，单选决定使用哪一个；
// 函数行只列返回类型匹配的函数，可直接换函数。类型选择器（typename 等）改了，后面跟随它的参数立即换类型。
class TriggerParameterDialog final : public QDialog {
public:
    TriggerParameterDialog(
        const War3Map::Trigger::TriggerDocument& document,
        const War3Map::Trigger::UiCatalog& catalog,
        War3Map::StableId ecaId,
        QWidget* parent = nullptr);
    ~TriggerParameterDialog() override;

    // 来源编号与 ArgumentNode::type 相同。
    enum Source { Preset = 0, Variable = 1, Function = 2, Value = 3 };

    [[nodiscard]] std::vector<TriggerArgumentEditRequest> PendingEdits() const;
    // 函数列表与选择器的分类图标来源。
    void SetCategoryIcons(TriggerFunctionList::CategoryIcon icons);
    // 模型、图像参数的“值”旁显示“选择…”，打开物编的资源选择器；未设置时只能手填路径。
    void SetResourcePickerFactory(War3Map::Modules::ObjectResourcePickerFactory factory);
    [[nodiscard]] std::optional<War3Map::StableId> RequestedNestedEca() const noexcept;

protected:
    void accept() override;

private:
    struct Row;
    void UpdateNaturalPreview();
    void FlashRow(Row* row);
    void AddArgumentRow(
        const War3Map::Trigger::UiArgumentDefinition* definition,
        std::string label,
        const War3Map::Trigger::ArgumentNode& argument);
    // 按当前期望类型重建预设、变量、函数候选与可见的来源行；initial 时选中参数原有的值。
    void ConfigureRow(Row* row, bool initial);
    [[nodiscard]] int DefaultSource(const Row* row) const;
    // 跟随类型选择器或前一个参数的 Null 参数按对话框里当前的选择重新定类型；unresolvedOnly 时只补还没解析出的。
    void RecomputeTypes(bool unresolvedOnly = false);
    [[nodiscard]] std::string PendingExpectedType(std::size_t index) const;
    [[nodiscard]] std::string PendingValueType(const Row* row) const;
    void PopulateVariables(Row* row);
    void PopulateFunctions(Row* row);
    void BrowseFunction(Row* row);
    void BrowseResource(Row* row);
    void UpdateFunctionLine(Row* row);
    [[nodiscard]] TriggerFunctionList& Functions();
    [[nodiscard]] static int CurrentSource(const Row* row);
    static void SelectSource(Row* row, int source);
    [[nodiscard]] static QString PresetValue(const Row* row);
    [[nodiscard]] static QString VariableName(const Row* row);
    [[nodiscard]] static QString VariableValue(const Row* row);
    [[nodiscard]] static std::optional<std::string> FunctionName(const Row* row);

    const War3Map::Trigger::TriggerDocument& document_;
    const War3Map::Trigger::UiCatalog& catalog_;
    War3Map::StableId ecaId_ = 0;
    War3Map::Trigger::TriggerTypeSystem types_;
    std::vector<War3Map::Trigger::LocalVariable> locals_;
    QLabel* naturalPreview_ = nullptr;
    QScrollArea* scroll_ = nullptr;
    std::vector<Row*> rows_;
    std::optional<War3Map::StableId> requestedNestedEca_;
    TriggerFunctionList::CategoryIcon categoryIcons_;
    std::optional<TriggerFunctionList> functions_;
    War3Map::Modules::ObjectResourcePickerFactory resourcePickerFactory_;
};

// 兼容旧调用入口；变量编辑器由独立模块提供。
#include "VariablesDialog.h"
