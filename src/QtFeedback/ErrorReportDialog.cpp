#include "ErrorReportDialog.h"

#include "War3MapCore/Feedback/RunLog.h"

#include <QDateTime>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QLabel>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QStandardPaths>
#include <QUrl>
#include <QVBoxLayout>

namespace War3Map::QtFeedback {

ErrorReportDialog::ErrorReportDialog(Collect collect, QString logDirectory, QWidget* parent)
    : QDialog(parent), collect_(std::move(collect)), logDirectory_(std::move(logDirectory)) {
    setObjectName(QStringLiteral("errorReportDialog"));
    setWindowTitle(tr("导出错误报告"));
    resize(760, 620);
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(new QLabel(tr("经过"), this));
    description_ = new QPlainTextEdit(this);
    description_->setObjectName(QStringLiteral("errorReportDescription"));
    description_->setMaximumHeight(120);
    layout->addWidget(description_);
    layout->addWidget(new QLabel(tr("报告内容"), this));
    preview_ = new QPlainTextEdit(this);
    preview_->setObjectName(QStringLiteral("errorReportPreview"));
    preview_->setReadOnly(true);
    preview_->setLineWrapMode(QPlainTextEdit::NoWrap);
    layout->addWidget(preview_, 1);
    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("errorReportStatus"));
    status_->setWordWrap(true);
    status_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(status_);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    auto* folder = buttons->addButton(tr("打开日志文件夹"), QDialogButtonBox::ActionRole);
    folder->setObjectName(QStringLiteral("errorReportOpenLogs"));
    auto* exportButton = buttons->addButton(tr("导出…"), QDialogButtonBox::AcceptRole);
    exportButton->setObjectName(QStringLiteral("errorReportExport"));
    exportButton->setDefault(true);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(exportButton, &QPushButton::clicked, this, &ErrorReportDialog::Export);
    connect(folder, &QPushButton::clicked, this, [this]() {
        if (!logDirectory_.isEmpty()) QDesktopServices::openUrl(QUrl::fromLocalFile(logDirectory_));
    });
    connect(description_, &QPlainTextEdit::textChanged, this, [this]() {
        input_.description = description_->toPlainText().toStdString();
        preview_->setPlainText(QString::fromStdString(Feedback::RenderErrorReportText(input_)));
    });
    Refresh();
}

void ErrorReportDialog::Refresh() {
    input_ = collect_ ? collect_() : Feedback::ErrorReportInput{};
    input_.description = description_->toPlainText().toStdString();
    preview_->setPlainText(QString::fromStdString(Feedback::RenderErrorReportText(input_)));
}

bool ErrorReportDialog::ExportTo(const QString& target, QString* error) {
    // 导出时重新收集，带上打开对话框之后的日志与诊断。
    Refresh();
    RunLog::Info("错误报告", "导出到 " + target.toStdString());
    const auto status = Feedback::WriteErrorReport(std::filesystem::path(target.toStdWString()), input_);
    if (!status.Succeeded()) {
        const auto message = status.diagnostics.empty() ? tr("导出失败。") : QString::fromStdString(status.diagnostics.front().message);
        RunLog::Error("错误报告", message.toStdString());
        if (error) *error = message;
        return false;
    }
    return true;
}

void ErrorReportDialog::Export() {
    const auto desktop = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
    const auto name = tr("Audastra-错误报告-%1.zip").arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")));
    const auto target = QFileDialog::getSaveFileName(this, tr("导出错误报告"), QDir(desktop).filePath(name), tr("ZIP 文件 (*.zip)"));
    if (target.isEmpty()) return;
    QString error;
    if (!ExportTo(QDir::toNativeSeparators(target), &error)) {
        status_->setText(error);
        return;
    }
    status_->setText(tr("已导出：%1").arg(QDir::toNativeSeparators(target)));
    QProcess::startDetached(QStringLiteral("explorer.exe"), {QStringLiteral("/select,"), QDir::toNativeSeparators(target)});
}

} // namespace War3Map::QtFeedback
