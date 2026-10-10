#include "UpdateDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace War3Map::QtUpdate {
namespace {
QString SizeText(qint64 bytes) {
    if (bytes < 1024) return QObject::tr("%1 字节").arg(bytes);
    if (bytes < 1024 * 1024) return QObject::tr("%1 KiB").arg(double(bytes) / 1024, 0, 'f', 1);
    if (bytes < 1024LL * 1024 * 1024) return QObject::tr("%1 MiB").arg(double(bytes) / (1024 * 1024), 0, 'f', 1);
    return QObject::tr("%1 GiB").arg(double(bytes) / (1024LL * 1024 * 1024), 0, 'f', 2);
}
QLabel* PlainLabel(QWidget* parent, const QString& name, const QString& text = {}) {
    auto* label = new QLabel(text, parent); label->setObjectName(name); label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true); label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto sizePolicy = label->sizePolicy(); sizePolicy.setHorizontalPolicy(QSizePolicy::Ignored);
    label->setSizePolicy(sizePolicy); return label;
}
struct PreferenceControls {
    QWidget* widget;
    QCheckBox* automatic;
    QComboBox* channel;
    QLabel* problem;
};
PreferenceControls CreatePreferences(const UpdatePreferences& preferences, QWidget* parent) {
    auto* widget = new QWidget(parent); widget->setObjectName(QStringLiteral("updatePreferences"));
    auto* form = new QFormLayout(widget); form->setContentsMargins(0, 0, 0, 0); form->setSpacing(10);
    form->setFormAlignment(Qt::AlignLeft | Qt::AlignTop);
    form->setFieldGrowthPolicy(QFormLayout::FieldsStayAtSizeHint);
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    auto* automatic = new QCheckBox(QObject::tr("启动后自动检查，每天最多一次"), widget); automatic->setObjectName(QStringLiteral("updateAutomaticCheck"));
    automatic->setChecked(preferences.automaticCheckEnabled); form->addRow(automatic);
    auto* channel = new QComboBox(widget); channel->setObjectName(QStringLiteral("updateChannel"));
    channel->setMinimumContentsLength(14);
    channel->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    channel->setMaximumWidth(280);
    channel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    channel->addItem(QObject::tr("稳定版")); channel->addItem(QObject::tr("测试版"));
    channel->setCurrentIndex(preferences.channel == Update::UpdateChannel::Beta ? 1 : 0); form->addRow(QObject::tr("更新渠道："), channel);
    auto* problem = PlainLabel(widget, QStringLiteral("updatePreferenceProblem")); problem->hide(); form->addRow(problem);
    return {widget, automatic, channel, problem};
}
} // namespace

UpdateDialog::UpdateDialog(UpdateClient& client, QWidget* parent) : UpdateDialog(client, std::nullopt, parent) {}
UpdateDialog::UpdateDialog(UpdateClient& client, const UpdatePreferences& preferences, QWidget* parent)
    : UpdateDialog(client, std::optional<UpdatePreferences>{preferences}, parent) {}
UpdateDialog::UpdateDialog(UpdateClient& client, std::optional<UpdatePreferences> preferences, QWidget* parent)
    : QDialog(parent), client_(client), draftPreferences_(std::move(preferences)) {
    setObjectName(QStringLiteral("updateDialog")); setWindowTitle(tr("检查更新")); resize(620, 510); setMinimumSize(420, 380);
    auto* layout = new QVBoxLayout(this); layout->setContentsMargins(20, 18, 20, 18); layout->setSpacing(12);
    auto* title = PlainLabel(this, QStringLiteral("dialogTitle"), tr("检查更新")); layout->addWidget(title);
    auto* versions = new QFormLayout; versions->setContentsMargins(0, 0, 0, 0); versions->setSpacing(8);
    versions->setRowWrapPolicy(QFormLayout::WrapLongRows);
    current_ = PlainLabel(this, QStringLiteral("updateCurrentVersion")); available_ = PlainLabel(this, QStringLiteral("updateAvailableVersion"));
    versions->addRow(tr("当前版本："), current_); versions->addRow(tr("可用版本："), available_); layout->addLayout(versions);
    message_ = PlainLabel(this, QStringLiteral("updateStatus")); layout->addWidget(message_);
    auto* notesCaption = PlainLabel(this, QStringLiteral("paneHint"), tr("更新说明")); layout->addWidget(notesCaption);
    notes_ = new QPlainTextEdit(this); notes_->setObjectName(QStringLiteral("updateReleaseNotes")); notes_->setReadOnly(true);
    notes_->setPlaceholderText(tr("检查更新后显示发布说明")); notes_->setMinimumHeight(100); layout->addWidget(notes_, 1);
    progress_ = new QProgressBar(this); progress_->setObjectName(QStringLiteral("updateDownloadProgress")); progress_->setTextVisible(false); layout->addWidget(progress_);
    progressText_ = PlainLabel(this, QStringLiteral("updateDownloadBytes")); layout->addWidget(progressText_);
    const auto controls = CreatePreferences(draftPreferences_ ? *draftPreferences_ : client_.Preferences(), this);
    automatic_ = controls.automatic; channel_ = controls.channel; preferenceProblem_ = controls.problem;
    layout->addWidget(controls.widget);
    connect(automatic_, &QCheckBox::toggled, this, [this](bool enabled) {
        QString error;
        if (draftPreferences_) {
            draftPreferences_->automaticCheckEnabled = enabled;
            emit PreferencesEdited(*draftPreferences_);
        } else if (!client_.SetAutomaticCheckEnabled(enabled, &error)) RefreshPreferences();
        preferenceProblem_->setText(error); preferenceProblem_->setVisible(!error.isEmpty());
    });
    connect(channel_, &QComboBox::currentIndexChanged, this, [this](int index) {
        const auto channel = index == 1 ? Update::UpdateChannel::Beta : Update::UpdateChannel::Stable;
        QString error;
        if (draftPreferences_) {
            draftPreferences_->channel = channel;
            emit PreferencesEdited(*draftPreferences_);
            Refresh(client_.Status());
        } else if (!client_.SetChannel(channel, &error)) RefreshPreferences();
        preferenceProblem_->setText(error); preferenceProblem_->setVisible(!error.isEmpty());
    });
    connect(&client_, &UpdateClient::PreferencesChanged, this, [this] { if (!draftPreferences_) RefreshPreferences(); });
    auto* buttons = new QHBoxLayout; buttons->setSpacing(8);
    check_ = new QPushButton(tr("检查更新"), this); check_->setObjectName(QStringLiteral("updateCheckButton")); buttons->addWidget(check_);
    skip_ = new QPushButton(tr("跳过此版本"), this); skip_->setObjectName(QStringLiteral("updateSkipButton")); buttons->addWidget(skip_); buttons->addStretch();
    primary_ = new QPushButton(tr("更新"), this); primary_->setObjectName(QStringLiteral("primaryButton")); buttons->addWidget(primary_);
    close_ = new QPushButton(tr("关闭"), this); close_->setObjectName(QStringLiteral("updateCloseButton")); buttons->addWidget(close_); layout->addLayout(buttons);
    connect(check_, &QPushButton::clicked, this, &UpdateDialog::CheckNow);
    connect(skip_, &QPushButton::clicked, this, [this] {
        QString error;
        if (draftPreferences_) {
            const auto version = client_.Status().availableVersion;
            if (client_.SkipThisVersionInSession(&error)) {
                draftPreferences_->skippedVersion = version;
                emit PreferencesEdited(*draftPreferences_);
            }
        } else (void)client_.SkipThisVersion(&error);
        if (!error.isEmpty()) message_->setText(error);
    });
    connect(primary_, &QPushButton::clicked, this, [this] {
        if (client_.Status().state == UpdateState::Ready && client_.Prepared()) emit InstallationRequested(*client_.Prepared());
        else (void)client_.Download();
    });
    connect(close_, &QPushButton::clicked, this, [this] {
        if (client_.IsBusy()) client_.Cancel(); else QDialog::reject();
    });
    connect(&client_, &UpdateClient::StatusChanged, this, &UpdateDialog::Refresh);
    connect(&client_, &UpdateClient::DownloadProgress, this, [this](qint64 received, qint64 total) {
        if (total > 0) { progress_->setRange(0, 1000); progress_->setValue(static_cast<int>(std::clamp<qint64>(received * 1000 / total, 0, 1000))); }
        else progress_->setRange(0, 0);
        progressText_->setText(total > 0 ? tr("%1 / %2").arg(SizeText(received), SizeText(total)) : tr("正在取得签名更新清单…"));
    });
    Refresh(client_.Status());
}

void UpdateDialog::CheckNow() {
    if (draftPreferences_) (void)client_.CheckWithPreferences(*draftPreferences_);
    else (void)client_.Check(true);
}
void UpdateDialog::SetDraftPreferences(const UpdatePreferences& preferences) {
    if (!draftPreferences_ || *draftPreferences_ == preferences || !UpdateClient::ValidatePreferences(preferences)) return;
    draftPreferences_ = preferences; RefreshPreferences(); Refresh(client_.Status());
}
void UpdateDialog::RefreshPreferences() {
    const auto& preferences = draftPreferences_ ? *draftPreferences_ : client_.Preferences();
    const QSignalBlocker automaticBlock(automatic_), channelBlock(channel_);
    automatic_->setChecked(preferences.automaticCheckEnabled);
    channel_->setCurrentIndex(preferences.channel == Update::UpdateChannel::Beta ? 1 : 0);
}
void UpdateDialog::reject() { if (client_.IsBusy()) client_.Cancel(); QDialog::reject(); }
void UpdateDialog::Refresh(const UpdateStatus& status) {
    channel_->setEnabled(!client_.IsBusy());
    const bool channelMatches = !draftPreferences_ || draftPreferences_->channel == status.channel;
    current_->setText(status.currentVersion); available_->setText(status.availableVersion.isEmpty() ? tr("尚未检查") : status.availableVersion);
    message_->setText(status.message.isEmpty() ? tr("检查公开发布的版本信息。") : status.message);
    if (notes_->toPlainText() != status.releaseNotes) notes_->setPlainText(status.releaseNotes);
    if (!channelMatches) {
        available_->setText(tr("尚未检查")); notes_->clear();
        message_->setText(tr("渠道已暂存为 %1，请重新检查更新。").arg(draftPreferences_->channel == Update::UpdateChannel::Beta ? tr("测试版") : tr("稳定版")));
    } else if (draftPreferences_ && status.state == UpdateState::Skipped) {
        message_->setText(tr("已在本页暂存跳过版本 %1；保存设置后生效。").arg(status.availableVersion));
    }
    const bool busy = client_.IsBusy(); const bool progress = status.state == UpdateState::Checking || status.state == UpdateState::Downloading || status.state == UpdateState::Verifying;
    check_->setEnabled(!busy); check_->setText(status.state == UpdateState::Failed ? tr("重试检查") : tr("检查更新"));
    skip_->setVisible(channelMatches && status.state == UpdateState::Available); skip_->setEnabled(!busy);
    primary_->setVisible(channelMatches && (status.state == UpdateState::Available || status.state == UpdateState::Ready));
    primary_->setText(status.state == UpdateState::Ready ? tr("安装并重启") : tr("更新"));
    primary_->setEnabled(!busy && (status.canDownload || (status.state == UpdateState::Ready && client_.Prepared().has_value())));
    close_->setText(busy ? tr("取消") : tr("关闭"));
    progress_->setVisible(progress); progressText_->setVisible(status.state == UpdateState::Downloading || status.state == UpdateState::Verifying);
    if (status.state == UpdateState::Checking || status.state == UpdateState::Verifying) progress_->setRange(0, 0);
    if (status.state == UpdateState::Verifying) progressText_->setText(tr("校验完成后交给安装准备流程。"));
}

UpdateSettingsWidget::UpdateSettingsWidget(UpdateClient& client, QWidget* parent)
    : QWidget(parent), client_(client), draftPreferences_(client.Preferences()) {
    setObjectName(QStringLiteral("updateSettingsPage"));
    auto* layout = new QVBoxLayout(this); layout->setContentsMargins(0, 0, 0, 0); layout->setSpacing(14);
    layout->addWidget(PlainLabel(this, QStringLiteral("sectionTitle"), tr("程序更新")));
    layout->addWidget(PlainLabel(this, QStringLiteral("updateSettingsVersion"), tr("当前版本 %1").arg(client_.Status().currentVersion)));
    const auto controls = CreatePreferences(draftPreferences_, this);
    automatic_ = controls.automatic; channel_ = controls.channel; layout->addWidget(controls.widget);
    connect(automatic_, &QCheckBox::toggled, this, [this](bool enabled) {
        draftPreferences_.automaticCheckEnabled = enabled; emit PreferencesEdited(draftPreferences_);
    });
    connect(channel_, &QComboBox::currentIndexChanged, this, [this](int index) {
        draftPreferences_.channel = index == 1 ? Update::UpdateChannel::Beta : Update::UpdateChannel::Stable;
        emit PreferencesEdited(draftPreferences_);
    });
    auto* button = new QPushButton(tr("检查更新"), this); button->setObjectName(QStringLiteral("updateSettingsCheck")); layout->addWidget(button, 0, Qt::AlignLeft);
    status_ = PlainLabel(this, QStringLiteral("updateSettingsStatus"), client_.Status().message); layout->addWidget(status_); layout->addStretch();
    channel_->setEnabled(!client_.IsBusy());
    connect(&client_, &UpdateClient::StatusChanged, this, [this](const auto& status) {
        channel_->setEnabled(!client_.IsBusy()); status_->setText(status.message);
    });
    connect(button, &QPushButton::clicked, this, [this] {
        if (dialog_) { dialog_->show(); dialog_->raise(); dialog_->activateWindow(); return; }
        auto* dialog = new UpdateDialog(client_, draftPreferences_, this); dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog_ = dialog;
        connect(dialog, &UpdateDialog::InstallationRequested, this, &UpdateSettingsWidget::InstallationRequested);
        connect(this, &UpdateSettingsWidget::PreferencesEdited, dialog, &UpdateDialog::SetDraftPreferences);
        connect(dialog, &UpdateDialog::PreferencesEdited, this, [this](const auto& preferences) {
            draftPreferences_ = preferences; RefreshPreferences(); emit PreferencesEdited(draftPreferences_);
        });
        dialog->show(); dialog->CheckNow();
    });
}

bool UpdateSettingsWidget::Validate(QString* error) const { return UpdateClient::ValidatePreferences(draftPreferences_, error); }
void UpdateSettingsWidget::ApplyCommitted(const UpdatePreferences& preferences) {
    if (!UpdateClient::ValidatePreferences(preferences)) return;
    draftPreferences_ = preferences; RefreshPreferences(); emit PreferencesEdited(draftPreferences_);
}
void UpdateSettingsWidget::RefreshPreferences() {
    const QSignalBlocker automaticBlock(automatic_), channelBlock(channel_);
    automatic_->setChecked(draftPreferences_.automaticCheckEnabled);
    channel_->setCurrentIndex(draftPreferences_.channel == Update::UpdateChannel::Beta ? 1 : 0);
}

} // namespace War3Map::QtUpdate
