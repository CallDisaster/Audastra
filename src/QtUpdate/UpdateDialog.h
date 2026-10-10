#pragma once

#include "UpdateClient.h"

#include <QDialog>
#include <QPointer>
#include <QWidget>

class QLabel;
class QCheckBox;
class QComboBox;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;

namespace War3Map::QtUpdate {

// 主题、字体和焦点反馈继承现有原生Qt工作台；不访问网络之外的地图或窗口状态。
class UpdateDialog final : public QDialog {
    Q_OBJECT
public:
    explicit UpdateDialog(UpdateClient& client, QWidget* parent = nullptr);
    UpdateDialog(UpdateClient& client, const UpdatePreferences& draftPreferences, QWidget* parent = nullptr);
    void CheckNow();
    void SetDraftPreferences(const UpdatePreferences& preferences);
    void reject() override;

signals:
    // 只发安装请求；未保存内容、独立updater和正常退出由主窗口统一处理。
    void InstallationRequested(const War3Map::QtUpdate::PreparedUpdate& update);
    void PreferencesEdited(const War3Map::QtUpdate::UpdatePreferences& preferences);

private:
    UpdateDialog(UpdateClient& client, std::optional<UpdatePreferences> draftPreferences, QWidget* parent);
    void Refresh(const UpdateStatus& status);
    void RefreshPreferences();
    UpdateClient& client_;
    std::optional<UpdatePreferences> draftPreferences_;
    QCheckBox* automatic_ = nullptr;
    QComboBox* channel_ = nullptr;
    QLabel* preferenceProblem_ = nullptr;
    QLabel* current_ = nullptr;
    QLabel* available_ = nullptr;
    QLabel* message_ = nullptr;
    QPlainTextEdit* notes_ = nullptr;
    QProgressBar* progress_ = nullptr;
    QLabel* progressText_ = nullptr;
    QPushButton* check_ = nullptr;
    QPushButton* skip_ = nullptr;
    QPushButton* primary_ = nullptr;
    QPushButton* close_ = nullptr;
};

// root可通过AgentSettingsDialog::AddApplicationPage接入；不会在构造时请求网络。
class UpdateSettingsWidget final : public QWidget {
    Q_OBJECT
public:
    explicit UpdateSettingsWidget(UpdateClient& client, QWidget* parent = nullptr);
    [[nodiscard]] UpdatePreferences Preferences() const { return draftPreferences_; }
    [[nodiscard]] bool Validate(QString* error = nullptr) const;
    // 统一提交成功后可重置本页草稿；不持久化，也不修改客户端运行时偏好。
    void ApplyCommitted(const UpdatePreferences& preferences);

signals:
    void InstallationRequested(const War3Map::QtUpdate::PreparedUpdate& update);
    void PreferencesEdited(const War3Map::QtUpdate::UpdatePreferences& preferences);

private:
    void RefreshPreferences();
    UpdateClient& client_;
    UpdatePreferences draftPreferences_;
    QCheckBox* automatic_ = nullptr;
    QComboBox* channel_ = nullptr;
    QLabel* status_ = nullptr;
    QPointer<UpdateDialog> dialog_;
};

} // namespace War3Map::QtUpdate
