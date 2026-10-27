#pragma once
#include "War3MapCore/Trigger/TriggerGraphEditing.h"
#include "War3MapCore/Trigger/TriggerTypes.h"
#include <QDialog>
#include <QString>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QSpinBox;

// 变量属性表单（新建与“属性…”共用）：类型只列函数库里可声明为全局变量的类型；
// 初值按类型给预设、物体或字面值。表单只收集值，提交由调用方经共用的触发器图编辑发布，与 Agent 走同一检查。
class VariablePropertiesDialog final : public QDialog {
public:
    struct Choice {
        QString value;
        QString display;
    };
    // 物体代码类型（unitcode 等）的固定值候选；其余类型不调用。
    using ObjectChoices = std::function<std::vector<Choice>(std::string_view type)>;

    // current 为空时是新建；否则编辑该变量（名称只读，改名走重命名以同步引用）。
    VariablePropertiesDialog(const War3Map::Trigger::UiCatalog& catalog, ObjectChoices objects, bool showRawValues,
        const std::optional<War3Map::Trigger::GraphVariableValue>& current, QWidget* parent = nullptr);

    [[nodiscard]] War3Map::Trigger::GraphVariableValue Value() const;
    // 发布失败时显示诊断，保留已填内容，作者改完可再次确定。
    void ShowProblem(const QString& message);

private:
    // original 为真时是打开表单时的已有初值：不在候选里也原样保留。
    void RebuildInitialChoices(const std::string& keep, bool original);
    void UpdateEnabled();
    void Submit();
    [[nodiscard]] std::string CurrentType() const;
    [[nodiscard]] std::string CurrentInitialValue() const;

    War3Map::Trigger::TriggerTypeSystem types_;
    ObjectChoices objects_;
    bool showRaw_ = false;
    bool creating_ = true;
    std::optional<War3Map::Trigger::GraphVariableValue> original_;
    bool initialAvailable_ = false;
    QLineEdit* name_ = nullptr;
    QComboBox* type_ = nullptr;
    QCheckBox* array_ = nullptr;
    QSpinBox* size_ = nullptr;
    QCheckBox* initialized_ = nullptr;
    QComboBox* initial_ = nullptr;
    QLabel* problem_ = nullptr;
};

// 已有变量改成 after 所需的字段设置（名称不在此修改），只含有变化的字段，前值取自 before 供批次核对。
// 标量的长度按 1 写；未设初值时保留原初值文本，不顺带清空。
[[nodiscard]] std::vector<War3Map::Trigger::GraphEditOperation> VariablePropertyOperations(
    const War3Map::Trigger::VariableRecord& before, const War3Map::Trigger::GraphVariableValue& after);
