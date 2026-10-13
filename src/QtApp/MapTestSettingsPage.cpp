#include "MapTestSettingsPage.h"
#include "../QtCommon/LocalConsentPreference.h"
#include "../QtCommon/MapBackupPaths.h"

#include <QCheckBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QPointer>
#include <QSettings>
#include <QSizePolicy>
#include <QSpinBox>
#include <QStandardPaths>
#include <QVBoxLayout>
#include <QScopeGuard>

#include <algorithm>
#include <utility>

namespace War3Map::QtApp {
namespace {

constexpr qsizetype MaximumDirectoryLength = 4096;

QLabel* Note(const QString& text, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    auto sizePolicy = label->sizePolicy();
    sizePolicy.setHorizontalPolicy(QSizePolicy::Ignored);
    label->setSizePolicy(sizePolicy);
    auto palette = label->palette();
    palette.setColor(QPalette::WindowText, palette.color(QPalette::PlaceholderText));
    label->setPalette(palette);
    return label;
}

QWidget* DirectoryRow(QLineEdit* edit, const QString& buttonName, const QString& caption,
    const QString& fallback, QWidget* parent) {
    auto* row = new QWidget(parent);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);
    layout->addWidget(edit, 1);
    auto* browse = new QPushButton(QObject::tr("选择…"), row);
    browse->setObjectName(buttonName);
    browse->setAutoDefault(false);
    layout->addWidget(browse);
    QObject::connect(browse, &QPushButton::clicked, row,
        [edit = QPointer<QLineEdit>(edit), caption, fallback, row = QPointer<QWidget>(row)] {
        if (!edit || !row) return;
        const auto current = edit->text().trimmed();
        const auto directory = QFileDialog::getExistingDirectory(row, caption,
            current.isEmpty() ? fallback : current);
        if (edit && !directory.isEmpty()) edit->setText(QDir::toNativeSeparators(directory));
    });
    return row;
}

bool ValidateDirectory(const QString& value, const QString& label, QString* error) {
    const auto fail = [error](const QString& message) {
        if (error) *error = message;
        return false;
    };
    if (value.size() > MaximumDirectoryLength)
        return fail(QObject::tr("%1不能超过 %2 个字符。").arg(label).arg(MaximumDirectoryLength));
    for (const auto character : value) {
        if (character.unicode() < 0x20)
            return fail(QObject::tr("%1不能包含控制字符。").arg(label));
    }
    const auto directory = QDir::fromNativeSeparators(value.trimmed());
    if (!directory.isEmpty() && !QDir::isAbsolutePath(directory))
        return fail(QObject::tr("%1需填写绝对目录，或留空使用默认位置。").arg(label));
    return true;
}

} // namespace

MapTestSettingsPage::MapTestSettingsPage(const QString& installationRoot, QWidget* parent)
    : MapTestSettingsPage(installationRoot,
        QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation), parent) {}

MapTestSettingsPage::MapTestSettingsPage(
    const QString& installationRoot, const QString& genericDataRoot, QWidget* parent)
    : QWidget(parent), genericDataRoot_(genericDataRoot) {
    setObjectName(QStringLiteral("mapTestSettingsPage"));
    QSettings stored;
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(18);

    auto* testGroup = new QGroupBox(tr("地图测试"), this);
    auto* testForm = new QFormLayout(testGroup);
    testForm->setContentsMargins(16, 18, 16, 16);
    testForm->setSpacing(12);
    testForm->setRowWrapPolicy(QFormLayout::WrapLongRows);
    testForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    runtimeRoot_ = new QLineEdit(stored.value(QStringLiteral("mapTest/runtimeRoot"), QString()).toString(), testGroup);
    runtimeRoot_->setObjectName(QStringLiteral("mapTestRuntimeRoot"));
    runtimeRoot_->setPlaceholderText(tr("留空使用已配置的本机 YDWE 目录"));
    testForm->addRow(tr("扩展运行时目录（可选）"), DirectoryRow(runtimeRoot_, QStringLiteral("mapTestRuntimeRootBrowse"),
        tr("选择扩展运行时目录"), installationRoot, testGroup));
    slkEnabled_ = new QCheckBox(tr("测试时启用 SLK 优化"), testGroup);
    slkEnabled_->setObjectName(QStringLiteral("mapTestSlkEnabled"));
    slkEnabled_->setChecked(QtCommon::ReadExplicitLocalConsent(stored, QStringLiteral("mapTest/slkEnabled")));
    testForm->addRow(slkEnabled_);
    layout->addWidget(testGroup);

    auto* backupGroup = new QGroupBox(tr("地图备份"), this);
    auto* backupForm = new QFormLayout(backupGroup);
    backupForm->setContentsMargins(16, 18, 16, 16);
    backupForm->setSpacing(12);
    backupForm->setRowWrapPolicy(QFormLayout::WrapLongRows);
    backupForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    const auto resolvedDefault = QtCommon::DefaultMapBackupDirectory(genericDataRoot_);
    const auto defaultBackup = resolvedDefault.Succeeded() ? *resolvedDefault.value : QString{};
    backupDirectory_ = new QLineEdit(stored.value(QStringLiteral("mapSave/backupDirectory"), QString()).toString(), backupGroup);
    backupDirectory_->setObjectName(QStringLiteral("mapBackupDirectory"));
    backupDirectory_->setPlaceholderText(resolvedDefault.Succeeded()
        ? QDir::toNativeSeparators(defaultBackup) : tr("默认备份目录不可用；请设置绝对目录"));
    backupForm->addRow(tr("备份目录"), DirectoryRow(backupDirectory_, QStringLiteral("mapBackupDirectoryBrowse"),
        tr("选择地图备份目录"), defaultBackup, backupGroup));
    backupLimit_ = new QSpinBox(backupGroup);
    backupLimit_->setObjectName(QStringLiteral("mapBackupLimit"));
    backupLimit_->setRange(0, 1000);
    backupLimit_->setSpecialValueText(tr("不限数量"));
    backupLimit_->setSuffix(tr(" 份"));
    backupLimit_->setMaximumWidth(180);
    bool limitValid = false;
    const auto limit = stored.value(QStringLiteral("mapSave/backupLimit"), 10).toInt(&limitValid);
    backupLimit_->setValue(limitValid && limit >= 0 && limit <= 1000 ? limit : 10);
    backupForm->addRow(tr("保留数量"), backupLimit_);
    legacyBackupInspection_ = Note(QString(), backupGroup);
    legacyBackupInspection_->setObjectName(QStringLiteral("mapLegacyBackupInspection"));
    backupForm->addRow(legacyBackupInspection_);
    legacyBackupBrowse_ = new QPushButton(tr("检查旧版目录…"), backupGroup);
    legacyBackupBrowse_->setObjectName(QStringLiteral("mapLegacyBackupDirectoryBrowse"));
    legacyBackupBrowse_->setAutoDefault(false);
    legacyBackupBrowse_->setEnabled(false);
    backupForm->addRow(legacyBackupBrowse_);
    connect(legacyBackupBrowse_, &QPushButton::clicked, this, [this, installationRoot] {
        const QPointer<MapTestSettingsPage> page(this);
        QPointer<QFileDialog> dialog(new QFileDialog(this, tr("检查旧版备份目录"), installationRoot));
        const auto cleanup = qScopeGuard([&dialog] { if (dialog) delete dialog.data(); });
        dialog->setObjectName(QStringLiteral("mapLegacyBackupDirectoryDialog"));
        dialog->setOption(QFileDialog::DontUseNativeDialog);
        dialog->setOption(QFileDialog::ShowDirsOnly);
        dialog->setOption(QFileDialog::ReadOnly);
        dialog->setFileMode(QFileDialog::Directory);
        dialog->setAcceptMode(QFileDialog::AcceptOpen);
        if (dialog->exec() != QDialog::Accepted || !dialog || !page) return;
        const auto directories = dialog->selectedFiles();
        if (directories.size() != 1 || directories.front().isEmpty()) return;
        // 复制回调后不再借用本页；回调允许立即切换/关闭设置窗口。
        const auto callback = page->legacyBackupDirectoryRequested_;
        if (callback) callback(QDir::cleanPath(directories.front()));
    });
    layout->addWidget(backupGroup);
    layout->addStretch(1);
}

bool MapTestSettingsPage::Validate(QString* error) const {
    if (error) error->clear();
    if (!ValidateDirectory(runtimeRoot_->text(), tr("扩展运行时目录"), error)) return false;
    const auto directory = QtCommon::ResolveMapBackupDirectory(backupDirectory_->text(), genericDataRoot_);
    if (!directory.Succeeded()) {
        if (error) *error = QString::fromUtf8(directory.diagnostics.front().message);
        return false;
    }
    return true;
}

void MapTestSettingsPage::SetLegacyBackupInspectionText(const QString& text) {
    legacyBackupInspection_->setText(text);
}

void MapTestSettingsPage::SetLegacyBackupInspectionBusy(bool busy) {
    legacyBackupInspectionBusy_ = busy;
    legacyBackupBrowse_->setEnabled(!busy && static_cast<bool>(legacyBackupDirectoryRequested_));
}

void MapTestSettingsPage::SetLegacyBackupDirectoryRequestedHandler(std::function<void(QString)> handler) {
    legacyBackupDirectoryRequested_ = std::move(handler);
    legacyBackupBrowse_->setEnabled(!legacyBackupInspectionBusy_ && static_cast<bool>(legacyBackupDirectoryRequested_));
}

Agent::ApplicationServicePreferences MapTestSettingsPage::Preferences() const {
    Agent::ApplicationServicePreferences preferences;
    preferences.mapBackupDirectory = backupDirectory_->text().trimmed();
    preferences.mapBackupLimit = backupLimit_->value();
    preferences.mapTestRuntimeRoot = runtimeRoot_->text().trimmed();
    preferences.mapTestSlk = slkEnabled_->isChecked();
    return preferences;
}

} // namespace War3Map::QtApp
