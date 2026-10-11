#pragma once

#include <QDialog>

#include <functional>

class QCheckBox;
class QDialogButtonBox;
class QLabel;
class QLineEdit;

namespace War3Map::QtProject { struct AssetEntry; }

namespace War3Map::Modules {

// 导入文件属性（对应原版 WE 的“导入的文件属性”）：显示文件名、来源与大小，
// “使用自定义路径”未勾选时路径为 war3mapImported\文件名，勾选后可以编辑完整路径。
// 路径实时检查（非法字符、非 ASCII、编辑器管理的文件、重名），有问题时不能确定。
class AssetPropertiesDialog final : public QDialog {
    Q_OBJECT

public:
    // check 返回新路径的问题（空 = 可用）；disabledIconState 为按钮图标的暂停图标说明（非按钮图标为空）。
    AssetPropertiesDialog(const QtProject::AssetEntry& entry, std::function<QString(const QString&)> check,
        const QString& disabledIconState, QWidget* parent = nullptr, bool directorySource = false, bool campaignSource = false);

    [[nodiscard]] QString Path() const;

private:
    void Validate();

    QString original_;
    QString fileName_;
    QString standardFolder_;
    std::function<QString(const QString&)> check_;
    QCheckBox* custom_ = nullptr;
    QLineEdit* path_ = nullptr;
    QLabel* problem_ = nullptr;
    QDialogButtonBox* buttons_ = nullptr;
};

} // namespace War3Map::Modules
