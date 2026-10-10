#include "RecoverySettingsPage.h"
#include "RecoveryPreferences.h"

#include <QCheckBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QSettings>
#include <QSpinBox>
#include <QVBoxLayout>

namespace War3Map::QtApp {

RecoverySettingsPage::RecoverySettingsPage(QWidget* parent) : RecoverySettingsPage(QSettings{}, parent) {}

RecoverySettingsPage::RecoverySettingsPage(const QSettings& stored, QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("recoverySettingsPage"));
    const auto preferences = Agent::LoadRecoveryPreferences(stored);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(18);
    auto* group = new QGroupBox(tr("自动恢复"), this);
    auto* form = new QFormLayout(group);
    form->setContentsMargins(16, 18, 16, 16);
    form->setSpacing(12);
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    enabled_ = new QCheckBox(tr("定期保存未保存的编辑到恢复工程"), group);
    enabled_->setObjectName(QStringLiteral("recoveryEnabled"));
    enabled_->setChecked(preferences.enabled);
    form->addRow(enabled_);
    interval_ = new QSpinBox(group);
    interval_->setObjectName(QStringLiteral("recoveryIntervalMinutes"));
    interval_->setRange(Agent::kMinimumRecoveryIntervalMinutes, Agent::kMaximumRecoveryIntervalMinutes);
    interval_->setSuffix(tr(" 分钟"));
    interval_->setMaximumWidth(180);
    interval_->setValue(preferences.intervalMinutes);
    interval_->setEnabled(preferences.enabled);
    connect(enabled_, &QCheckBox::toggled, interval_, &QSpinBox::setEnabled);
    form->addRow(tr("保存间隔"), interval_);
    auto* note = new QLabel(tr("恢复工程（.forge）保留编辑状态，不会写回原地图，也不会把编辑标记为已保存。异常退出后可使用“恢复编辑…”打开已有恢复工程。"), group);
    note->setObjectName(QStringLiteral("autoRecoveryNote"));
    note->setTextFormat(Qt::PlainText);
    note->setWordWrap(true);
    form->addRow(note);
    auto* scope = new QLabel(tr("多个编辑窗口的恢复工程需要分别选择；启动时只提示最近一次恢复。"), group);
    scope->setObjectName(QStringLiteral("autoRecoveryScopeNote"));
    scope->setTextFormat(Qt::PlainText);
    scope->setWordWrap(true);
    form->addRow(scope);
    layout->addWidget(group);
    layout->addStretch(1);
}

bool RecoverySettingsPage::Validate(QString* error) const {
    if (error) error->clear();
    if (Agent::IsRecoveryIntervalValid(interval_->value())) return true;
    if (error) *error = tr("自动恢复间隔须为1到120分钟。");
    return false;
}

Agent::ApplicationServicePreferences RecoverySettingsPage::Preferences() const {
    Agent::ApplicationServicePreferences preferences;
    preferences.recoveryEnabled = enabled_->isChecked();
    preferences.recoveryIntervalMinutes = interval_->value();
    return preferences;
}

} // namespace War3Map::QtApp
