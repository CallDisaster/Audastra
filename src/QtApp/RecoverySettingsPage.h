#pragma once

#include "ApplicationSettingsCommit.h"

#include <QWidget>

class QCheckBox;
class QSettings;
class QSpinBox;

namespace War3Map::QtApp {

// 只编辑草稿；保存由 ApplicationSettingsCommit 的共同事务发布。
class RecoverySettingsPage final : public QWidget {
public:
    explicit RecoverySettingsPage(QWidget* parent = nullptr);
    explicit RecoverySettingsPage(const QSettings& stored, QWidget* parent = nullptr);
    [[nodiscard]] bool Validate(QString* error = nullptr) const;
    [[nodiscard]] Agent::ApplicationServicePreferences Preferences() const;

private:
    QCheckBox* enabled_ = nullptr;
    QSpinBox* interval_ = nullptr;
};

} // namespace War3Map::QtApp
