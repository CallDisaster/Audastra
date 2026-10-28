#pragma once
#include "War3MapCore/MapData/MapSettings.h"
#include <QDialog>
#include <QString>
#include <functional>
#include <optional>
#include <vector>
class QComboBox;
class QLabel;
class QLineEdit;
class QSpinBox;
class QTableWidget;
class QTreeWidget;
class QHBoxLayout;

namespace War3Map::Modules {
// 随机单位组/随机物品表的整表编辑表单。对象格只在编辑时弹出可搜索的选择框（含“无”）；
// 表单只收集整表，校验与提交由调用方经共用自定义设置服务完成，失败时保留表单并显示原因。
struct RandomObjectChoice {
    QString raw;
    QString display;
};
struct RandomObjectChoices {
    std::vector<RandomObjectChoice> units; // 单位列与建筑列共用；类别由服务按实际物编核对
    std::vector<RandomObjectChoice> items;
};
using RandomObjectName = std::function<QString(const MapData::SettingsRawcode& id)>;

// 四字节代码与显示文本互转（科技页与随机表表单共用）；只接受可打印 ASCII。
[[nodiscard]] QString SettingsRawText(const MapData::SettingsRawcode& id);
[[nodiscard]] std::optional<MapData::SettingsRawcode> SettingsRawFromText(const QString& text);
// 随机单位组列类型：0 单位、1 建筑、2 物品。
[[nodiscard]] QString RandomColumnTypeName(std::int32_t type);

class RandomUnitTableDialog final : public QDialog {
public:
    // keepName 为真表示名称未改，沿用原编码（可能是字符串表引用）。
    using Submit = std::function<QString(const MapData::MapRandomUnitTable& table, bool keepName)>;
    // current 为空时新建（number 为建议编号）；否则修改该表，编号与列结构不在此修改。
    RandomUnitTableDialog(const std::optional<MapData::MapRandomUnitTable>& current, const QString& shownName,
        std::int32_t number, const RandomObjectChoices& choices, RandomObjectName nameOf, Submit submit,
        QWidget* parent = nullptr);

private:
    void AddColumn(std::int32_t type);
    void AddRow(const MapData::MapRandomUnitRow& row);
    void SyncHeaders();
    void UpdateTotal();
    void Accept();

    bool creating_ = true;
    QString initialName_;
    RandomObjectName nameOf_;
    Submit submit_;
    QLineEdit* name_ = nullptr;
    QSpinBox* number_ = nullptr;
    QHBoxLayout* columnTypes_ = nullptr;
    std::vector<QComboBox*> typeBoxes_;
    QTableWidget* rows_ = nullptr;
    QLabel* total_ = nullptr;
    QLabel* problem_ = nullptr;
};

class RandomItemTableDialog final : public QDialog {
public:
    using Submit = std::function<QString(const MapData::MapRandomItemTable& table, bool keepName)>;
    RandomItemTableDialog(const std::optional<MapData::MapRandomItemTable>& current, const QString& shownName,
        std::int32_t number, const RandomObjectChoices& choices, RandomObjectName nameOf, Submit submit,
        QWidget* parent = nullptr);

private:
    void AddSet();
    void AddItem(const MapData::MapRandomItem& item);
    void Accept();

    bool creating_ = true;
    QString initialName_;
    RandomObjectName nameOf_;
    Submit submit_;
    QLineEdit* name_ = nullptr;
    QSpinBox* number_ = nullptr;
    QTreeWidget* sets_ = nullptr;
    QLabel* problem_ = nullptr;
};
} // namespace War3Map::Modules
