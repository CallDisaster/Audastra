#include "MainWindow.h"
#include "../QtCommon/ApplicationPaths.h"
#include "FeedbackClient.h"
#include "FeedbackDialog.h"
#include "ProgramIssueApplication.h"
#include "WorldModule.h"
#include "WorldViewport.h"
#include "UpdateClient.h"
#include "UpdateDialog.h"
#include "McpProductTools.h"
#include "AudastraVersion.generated.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/IO/FileBytes.h"
#include "War3MapCore/VerifiedFile.h"
#include "War3MapCore/Update/UpdateInstaller.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QMessageBox>
#include <QPointer>
#include <QProcess>
#include <QSettings>
#include <QStandardItemModel>
#include <QStandardPaths>
#include <QSysInfo>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#include <algorithm>

namespace {

QString Utf8(std::string_view value) {
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}
QString DiagnosticText(const std::vector<War3Map::Diagnostic>& diagnostics) {
    QStringList lines;
    for (const auto& diagnostic : diagnostics) lines.push_back(Utf8(diagnostic.code) + QStringLiteral(": ") + Utf8(diagnostic.message));
    return lines.join(QLatin1Char('\n'));
}
QString StateName(War3Map::QtUpdate::UpdateState state) {
    using State = War3Map::QtUpdate::UpdateState;
    switch (state) {
    case State::Idle: return QStringLiteral("not_checked");
    case State::Checking: return QStringLiteral("checking");
    case State::UpToDate: return QStringLiteral("up_to_date");
    case State::Available: return QStringLiteral("available");
    case State::Skipped: return QStringLiteral("skipped");
    case State::Downloading: return QStringLiteral("downloading");
    case State::Verifying: return QStringLiteral("verifying");
    case State::Ready: return QStringLiteral("ready");
    case State::Failed: return QStringLiteral("failed");
    case State::Cancelled: return QStringLiteral("cancelled");
    }
    return QStringLiteral("unknown");
}
QString InstallationRoot() {
    return War3Map::QtCommon::InstallationRoot();
}
struct UpdateLaunchData {
    std::unique_ptr<QTemporaryDir> directory;
    QString updater;
    QStringList arguments;
    QString error;
};
bool WriteNew(const QString& path, const std::vector<std::byte>& bytes) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::NewOnly)
        && file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<qint64>(bytes.size())) == static_cast<qint64>(bytes.size())
        && file.flush() && file.error() == QFileDevice::NoError;
}

} // namespace

void MainWindow::SetupProductServices() {
    if (startupMode_ != StartupMode::Normal || updateClient_) return;
    QSettings preferences;
    const auto queue = War3Map::Mcp::ApplicationFeedbackQueue();
    if (queue.Succeeded()) {
        auto* application = dynamic_cast<War3Map::QtFeedback::ProgramIssueApplication*>(QCoreApplication::instance());
        programIssueReporter_ = application ? application->Reporter() : nullptr;
        if (!programIssueReporter_) {
            programIssueReporter_ = std::make_shared<War3Map::QtFeedback::ProgramIssueReporter>(*queue.value,
                War3Map::Feedback::ApplicationInfo{std::string(War3Map::BuildInfo::Version),
                    std::string(War3Map::BuildInfo::Commit), {}, {}});
            if (application) application->SetProgramIssueReporter(programIssueReporter_);
        }
        feedbackClient_ = new War3Map::QtFeedback::FeedbackClient(*queue.value,
            War3Map::QtFeedback::FeedbackEndpointConfig{Utf8(War3Map::BuildInfo::FeedbackEndpoint)},
            War3Map::QtFeedback::FeedbackClient::ReadPreferences(preferences), this);
        connect(feedbackClient_, &War3Map::QtFeedback::FeedbackClient::LastSentChanged, this, [this](qint64 time) {
            // 仅写时间，不用旧快照覆回同意开关或端点。
            QSettings current;
            current.setValue(QStringLiteral("feedback/lastSentUnixSeconds"), time);
            current.sync();
            if (current.status() != QSettings::NoError)
                AddDiagnostic(tr("错误"), tr("反馈"), QStringLiteral("FEEDBACK_SETTINGS_IO"), tr("最近上报时间未能写入本机设置。"));
        });
        connect(feedbackClient_, &War3Map::QtFeedback::FeedbackClient::OperationFinished, this,
            [this](quint64, bool success, const QString& message) {
                if (!success) AddDiagnostic(tr("错误"), tr("反馈"), QStringLiteral("FEEDBACK_OPERATION_FAILED"), message);
            });
        // WorldModule 转发其延迟创建视口的结构化失败；连接的 QObject context 随主窗销毁。
        connect(worldModule_, &War3Map::Modules::WorldModule::RenderPluginFailed, this,
            [this](const War3Map::Modules::RenderPluginFailure& failure) {
                if (!programIssueReporter_) return;
                const auto message = failure.message.toUtf8();
                const auto draft = programIssueReporter_->RenderFailure(failure.operation.toStdString(), failure.result,
                    failure.statusAvailable, failure.statusCallResult, failure.statusResult,
                    std::string_view(message.constData(), static_cast<std::size_t>(message.size())));
                if (draft) {
                    feedbackClient_->RefreshQueue();
                    AddDiagnostic(tr("信息"), tr("反馈"), QStringLiteral("PROGRAM_ISSUE_DRAFTED"),
                        tr("程序问题已加入反馈队列；查看完整内容后确认发送。"));
                }
            });
    } else AddDiagnostic(tr("错误"), tr("反馈"), QStringLiteral("FEEDBACK_QUEUE_UNAVAILABLE"), DiagnosticText(queue.diagnostics));

    updateClient_ = new War3Map::QtUpdate::UpdateClient(this);
    const auto publish = [](const War3Map::QtUpdate::UpdateStatus& status) {
        War3Map::Mcp::PublishApplicationUpdateStatus({
            {QStringLiteral("check_state"), StateName(status.state)},
            {QStringLiteral("current_version"), status.currentVersion},
            {QStringLiteral("available_version"), status.availableVersion},
            {QStringLiteral("message"), status.message}, {QStringLiteral("error_code"), status.errorCode},
            {QStringLiteral("installation_available_to_agent"), false}});
    };
    connect(updateClient_, &War3Map::QtUpdate::UpdateClient::StatusChanged, this, publish);
    connect(updateClient_, &War3Map::QtUpdate::UpdateClient::StatusChanged, this,
        [this](const War3Map::QtUpdate::UpdateStatus& status) {
            if (status.manual || status.state != War3Map::QtUpdate::UpdateState::Available || productUpdateNotice_) return;
            auto* notice = new War3Map::QtUpdate::UpdateDialog(*updateClient_, this);
            notice->setAttribute(Qt::WA_DeleteOnClose);
            productUpdateNotice_ = notice;
            connect(notice, &War3Map::QtUpdate::UpdateDialog::InstallationRequested, this, &MainWindow::BeginProductUpdate);
            notice->show(); // 模型提示只展示实际自动检查结果，不重复请求。
        });
    publish(updateClient_->Status());
    updateClient_->ScheduleAutomaticCheck();

    // 真实后续启动才清理旧备份。刚被更新器启动的健康应答实例不抢同一事务。
    if (!QCoreApplication::arguments().contains(QStringLiteral("--update-health-transaction"))) {
        QTimer::singleShot(1000, this, [this] {
            if (productWorker_ || productUpdateExit_) return;
            const auto key = War3Map::Update::ParseTrustedUpdateKeyHex(War3Map::BuildInfo::UpdatePublicKeyHex);
            if (!key.Succeeded() || !key.value->configured) return;
            const auto root = std::filesystem::path(InstallationRoot().toStdWString());
            auto result = std::make_shared<War3Map::Result<War3Map::Update::UpdateBackupCleanupReceipt>>();
            productWorker_ = QThread::create([root, key = *key.value, result] {
                try {
                    *result = War3Map::Update::CleanupUpdateBackups(root, key);
                } catch (...) {
                    *result = War3Map::Result<War3Map::Update::UpdateBackupCleanupReceipt>::Failure(
                        "Update", "UPDATE_CLEANUP_EXCEPTION", "清理工作线程未能完成，保留现有备份。");
                }
            });
            productWorker_->setParent(this);
            connect(productWorker_, &QThread::finished, this, [this, result] {
                auto* worker = productWorker_;
                productWorker_ = nullptr;
                worker->deleteLater();
                if (!result->Succeeded()) {
                    auto message = DiagnosticText(result->diagnostics);
                    if (result->value && result->value->partiallyCleanedBackups) {
                        message += tr("\n有 %1 份旧备份只清理了一部分。剩余内容保留在：")
                            .arg(static_cast<qulonglong>(result->value->partiallyCleanedBackups));
                        for (const auto& directory : result->value->partialBackupDirectories)
                            message += QChar('\n') + QString::fromStdWString(directory.wstring());
                    }
                    AddDiagnostic(tr("警告"), tr("更新"), QStringLiteral("UPDATE_CLEANUP_DEFERRED"), message);
                }
            });
            productWorker_->start(QThread::LowPriority);
        });
    }
}

void MainWindow::ReportProgramSaveFailures(int firstDiagnostic) {
    if (!programIssueReporter_ || startupMode_ != StartupMode::Normal) return;
    // 只读取可信宿主内部错误码。诊断正文可能含用户脚本，不能自动附带。
    for (int row = firstDiagnostic; row < project_->diagnosticsModel_->rowCount(); ++row) {
        const auto code = project_->diagnosticsModel_->index(row, 2).data().toString().toStdString();
        if (programIssueReporter_->SaveDefect(code)) {
            feedbackClient_->RefreshQueue();
            AddDiagnostic(tr("信息"), tr("反馈"), QStringLiteral("PROGRAM_ISSUE_DRAFTED"),
                tr("程序问题已加入反馈队列；查看完整内容后确认发送。"));
            break; // 一次保存失败只起草首个明确内部故障，不放大派生门禁。
        }
    }
}

void MainWindow::ShowFeedback() {
    SetupProductServices();
    if (!feedbackClient_) {
        QMessageBox::warning(this, tr("反馈不可用"), tr("本机反馈队列无法打开，请查看问题面板。"));
        return;
    }
    War3Map::QtFeedback::FeedbackFormContext context;
    context.application.version = std::string(War3Map::BuildInfo::Version);
    context.application.commit = std::string(War3Map::BuildInfo::Commit);
    context.application.operatingSystem = QSysInfo::prettyProductName().toUtf8().toStdString();
    context.screenshotTarget = this;
    context.conversationSnippet = [this] { return FeedbackConversationSnippet().toUtf8().toStdString(); };
    context.recentLogs = [this] {
        // 只附诊断身份，不附地图错误正文、触发器文本或工具请求内容。
        QStringList lines;
        const auto* model = project_->diagnosticsModel_;
        if (model) for (int row = std::max(0, model->rowCount() - 100); row < model->rowCount(); ++row) {
            QStringList fields;
            for (int column = 0; column < std::min(3, model->columnCount()); ++column)
                fields.push_back(model->index(row, column).data().toString());
            lines.push_back(fields.join(QStringLiteral(" · ")));
        }
        return lines.join(QLatin1Char('\n')).toUtf8().toStdString();
    };
    War3Map::QtFeedback::FeedbackDialog dialog(*feedbackClient_, std::move(context), this);
    dialog.exec();
}

void MainWindow::ShowUpdates() {
    SetupProductServices();
    if (!updateClient_) return;
    War3Map::QtUpdate::UpdateDialog dialog(*updateClient_, this);
    connect(&dialog, &War3Map::QtUpdate::UpdateDialog::InstallationRequested, this, &MainWindow::BeginProductUpdate);
    dialog.CheckNow();
    dialog.exec();
}

void MainWindow::BeginProductUpdate(const War3Map::QtUpdate::PreparedUpdate& prepared) {
    if (productWorker_ || productUpdateExit_ || project_->savingMap_ || changesChecking_) {
        QMessageBox::information(this, tr("暂时无法更新"), tr("请等待当前保存或更新准备完成。"));
        return;
    }
    // 更新只提供保存或取消，不能走平时关闭窗口的放弃修改分支。
    if (!ConfirmDiscardPendingEdits(tr("更新"), false)) return;
    if (!updateClient_ || !updateClient_->Prepared() || updateClient_->Prepared()->manifestSha256 != prepared.manifestSha256) return;
    const auto root = InstallationRoot();
    const auto executable = QDir(root).relativeFilePath(QCoreApplication::applicationFilePath());
    const auto appDirectory = QCoreApplication::applicationDirPath();
    const auto stagingRoot = QFileInfo(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)).canonicalFilePath();
    const auto launchData = std::make_shared<UpdateLaunchData>();
    productWorker_ = QThread::create([root, executable, appDirectory, stagingRoot, prepared, launchData] {
        try {
            using namespace War3Map;
            using namespace War3Map::Update;
            const auto key = ParseTrustedUpdateKeyHex(BuildInfo::UpdatePublicKeyHex);
            if (!key.Succeeded() || !key.value->configured) { launchData->error = QStringLiteral("尚未配置可信发布公钥，无法安装更新。"); return; }
            const auto baseline = DiscoverInstalledUpdateBaseline(std::filesystem::path(root.toStdWString()), BuildInfo::Version, *key.value);
            if (!baseline.Succeeded()) { launchData->error = DiagnosticText(baseline.diagnostics); return; }
            const auto raw = ReadVerifiedFile(baseline.value->manifestPath, baseline.value->manifestSha256, kMaxUpdateManifestBytes);
            const auto signature = IO::ReadFileBytes(baseline.value->signaturePath, 64);
            if (!raw.Succeeded() || !signature.Succeeded()) { launchData->error = QStringLiteral("当前版本签名基线读取失败。"); return; }
            const auto verified = VerifyAndParseUpdateManifest(*raw.value, *signature.value, *key.value);
            if (!verified.Succeeded()) { launchData->error = DiagnosticText(verified.diagnostics); return; }
            if (stagingRoot.isEmpty()) { launchData->error = QStringLiteral("无法定位独立更新临时目录。"); return; }
            launchData->directory = std::make_unique<QTemporaryDir>(QDir(stagingRoot).filePath(QStringLiteral("audastra-installer-XXXXXX")));
            if (!launchData->directory->isValid()) { launchData->error = QStringLiteral("独立更新临时目录创建失败。"); return; }
            const auto write = [&](const QString& name, const std::vector<std::byte>& bytes) {
                return WriteNew(launchData->directory->filePath(name), bytes);
            };
            if (!write(QStringLiteral("baseline.json"), *raw.value) || !write(QStringLiteral("baseline.sig"), *signature.value)) {
                launchData->error = QStringLiteral("当前版本签名基线暂存失败。"); return;
            }
            // 在安装树外执行更新器；复制其签名清单内的字节和 app-local VC 运行库。
            for (const auto* name : {"audastra-updater.exe", "msvcp140.dll", "msvcp140_1.dll", "msvcp140_2.dll",
                                    "msvcp140_atomic_wait.dll", "vcruntime140.dll", "vcruntime140_1.dll", "concrt140.dll"}) {
                const auto source = QDir(appDirectory).filePath(QString::fromLatin1(name));
                if (!QFileInfo::exists(source) && std::string_view(name) != "audastra-updater.exe") continue;
                const auto relative = QDir(root).relativeFilePath(source).toUtf8().toStdString();
                const auto& files = verified.value->Manifest().files;
                const auto found = std::find_if(files.begin(), files.end(), [&](const auto& file) { return file.path == relative; });
                if (found == files.end()) { launchData->error = QStringLiteral("更新器或运行库不在当前签名清单内。"); return; }
                const auto bytes = ReadVerifiedFile(std::filesystem::path(source.toStdWString()), found->sha256, 64 * 1024 * 1024);
                if (!bytes.Succeeded() || !write(QString::fromLatin1(name), *bytes.value)) {
                    launchData->error = QStringLiteral("已签名更新器或运行库暂存失败。"); return;
                }
            }
            const auto parent = ReadUpdateProcessIdentity();
            if (!parent.Succeeded()) { launchData->error = DiagnosticText(parent.diagnostics); return; }
            launchData->updater = launchData->directory->filePath(QStringLiteral("audastra-updater.exe"));
            launchData->arguments = {QStringLiteral("--show-result"), QStringLiteral("--install-dir"), root,
                QStringLiteral("--package"), prepared.packagePath, QStringLiteral("--manifest"), prepared.manifestPath,
                QStringLiteral("--signature"), prepared.signaturePath, QStringLiteral("--current-version"), Utf8(BuildInfo::Version),
                QStringLiteral("--baseline-manifest"), launchData->directory->filePath(QStringLiteral("baseline.json")),
                QStringLiteral("--baseline-signature"), launchData->directory->filePath(QStringLiteral("baseline.sig")),
                QStringLiteral("--parent-pid"), QString::number(parent.value->processId),
                QStringLiteral("--parent-created"), QString::number(parent.value->creationTime100ns),
                QStringLiteral("--exe"), executable};
        } catch (...) {
            launchData->error = QStringLiteral("更新准备未能完成，当前程序和编辑保持开放。");
        }
    });
    productWorker_->setParent(this);
    connect(productWorker_, &QThread::finished, this, [this, launchData, prepared] {
        auto* worker = productWorker_;
        productWorker_ = nullptr;
        worker->deleteLater();
        if (!launchData->error.isEmpty()) { QMessageBox::warning(this, tr("无法安装更新"), launchData->error); return; }
        productUpdateExit_ = [this, launchData, prepared] {
            QString error;
            // 此时关闭门禁已再次检查所有未保存编辑；启动失败则保持窗口打开。
            if (!updateClient_->Prepared() || *updateClient_->Prepared() != prepared
                || updateClient_->Status().state != War3Map::QtUpdate::UpdateState::Ready) return false;
            QProcess updater;
            updater.setProgram(launchData->updater);
            updater.setArguments(launchData->arguments);
            updater.setWorkingDirectory(launchData->directory->path());
            updater.setStandardOutputFile(launchData->directory->filePath(QStringLiteral("updater-result.json")));
            updater.setStandardErrorFile(launchData->directory->filePath(QStringLiteral("updater-error.log")));
            qint64 pid = 0;
            if (!updater.startDetached(&pid)) {
                QMessageBox::warning(this, tr("无法启动更新程序"), updater.errorString()); return false;
            }
            launchData->directory->setAutoRemove(false);
            if (!updateClient_->RetainPreparedForInstaller(prepared, &error)) {
                // 已启动helper只能等待本进程；保持本窗口开放，helper等待超时后自然放弃。
                QMessageBox::warning(this, tr("更新准备已变化"), error); return false;
            }
            return true;
        };
        if (!close()) productUpdateExit_ = {};
        else QCoreApplication::quit(); // 结束本应用事件循环，让模态产品窗口也正常返回。
    });
    productWorker_->start(QThread::LowPriority);
}

bool MainWindow::CommitProductUpdateExit() {
    return productUpdateExit_ && productUpdateExit_();
}
