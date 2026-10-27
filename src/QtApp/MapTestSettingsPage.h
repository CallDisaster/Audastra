#pragma once

#include "ApplicationSettingsCommit.h"

#include <QString>
#include <QWidget>

#include <functional>

class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;

namespace War3Map::QtApp {

// 只读取本机偏好并编辑草稿；持久化由应用设置的统一提交事务完成。
class MapTestSettingsPage final : public QWidget {
public:
    explicit MapTestSettingsPage(const QString& installationRoot, QWidget* parent = nullptr);
    MapTestSettingsPage(const QString& installationRoot, const QString& genericDataRoot, QWidget* parent = nullptr);
    [[nodiscard]] bool Validate(QString* error = nullptr) const;
    [[nodiscard]] Agent::ApplicationServicePreferences Preferences() const;
    // 后台检查由应用服务负责；本页不枚举、散列、搬移或写入备份。
    void SetLegacyBackupInspectionText(const QString& text);
    void SetLegacyBackupInspectionBusy(bool busy);
    void SetLegacyBackupDirectoryRequestedHandler(std::function<void(QString)> handler);

private:
    QString legacyBackupInspectionText_;
    QString backupMigrationText_;
    bool backupMigrationBusy_ = false;
    QString genericDataRoot_;
    QLineEdit* backupDirectory_ = nullptr;
    QSpinBox* backupLimit_ = nullptr;
    QLineEdit* runtimeRoot_ = nullptr;
    QCheckBox* slkEnabled_ = nullptr;
    QLabel* legacyBackupInspection_ = nullptr;
    QPushButton* legacyBackupBrowse_ = nullptr;
    bool legacyBackupInspectionBusy_ = false;
    std::function<void(QString)> legacyBackupDirectoryRequested_;
};

} // namespace War3Map::QtApp
