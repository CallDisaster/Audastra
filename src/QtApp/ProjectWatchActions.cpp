#include "MainWindow.h"

#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QSizePolicy>
#include <QStatusBar>
#include <QToolBar>

namespace {
QString PathText(const std::filesystem::path& path) {
    return QString::fromStdWString(path.wstring());
}

QString Utf8Text(const std::string& text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}
}

void MainWindow::SetupProjectNotices() {
    if (projectNoticeBar_) return;

    projectNoticeBar_ = new QToolBar(tr("工程通知"), this);
    projectNoticeBar_->setObjectName("projectNoticeBar");
    projectNoticeBar_->setMovable(false);
    projectNoticeBar_->setFloatable(false);
    projectNoticeBar_->setContextMenuPolicy(Qt::PreventContextMenu);
    auto* content = new QWidget(projectNoticeBar_);
    content->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    auto* layout = new QHBoxLayout(content);
    layout->setContentsMargins(12, 3, 10, 3);
    layout->setSpacing(8);
    projectNoticeText_ = new QLabel(content);
    projectNoticeText_->setObjectName("projectNoticeText");
    projectNoticeText_->setMinimumWidth(0);
    projectNoticeText_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    projectNoticeText_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    projectNoticeAction_ = new QPushButton(content);
    projectNoticeAction_->setObjectName("projectNoticeAction");
    projectNoticeDismiss_ = new QPushButton(tr("稍后"), content);
    projectNoticeDismiss_->setObjectName("projectNoticeDismiss");
    layout->addWidget(projectNoticeText_, 1);
    layout->addWidget(projectNoticeAction_);
    layout->addWidget(projectNoticeDismiss_);
    projectNoticeBar_->addWidget(content);
    addToolBarBreak(Qt::TopToolBarArea);
    addToolBar(Qt::TopToolBarArea, projectNoticeBar_);
    projectNoticeBar_->hide();

    connect(project_.get(), &War3Map::QtProject::EditorProject::SourceStateChanged,
        this, &MainWindow::UpdateProjectNotice);
    connect(projectNoticeAction_, &QPushButton::clicked, this, [this] {
        if (project_->sourceChangeSuspected_ || project_->recoveryNoticePath_.empty()) {
            CheckProjectSources();
            return;
        }
        // 打开入口先完成全部格式检查，再询问已有修改的处理方式。
        // 不自动重载，不删除恢复文件，也不绕过取消/保存保护。
        const auto path = project_->recoveryNoticePath_;
        if (OpenEditorProjectPath(path)) {
            project_->recoveryNoticePath_.clear();
            UpdateProjectNotice();
        }
    });
    connect(projectNoticeDismiss_, &QPushButton::clicked, this, [this] {
        if (!project_->sourceChangeSuspected_ && !project_->recoveryNoticePath_.empty())
            project_->recoveryNoticePath_.clear();
        project_->projectNoticeDismissed_ = true;
        UpdateProjectNotice();
    });
}

void MainWindow::ResetProjectWatch() {
    project_->ResetProjectWatch();
}

void MainWindow::RefreshProjectWatch() {
    project_->RefreshProjectWatch();
}

bool MainWindow::CheckProjectSources() {
    const auto result = project_->CheckProjectSources();
    for (const auto& diagnostic : result.diagnostics)
        AddDiagnostic(tr("警告"), tr("来源"), Utf8Text(diagnostic.code), Utf8Text(diagnostic.message));
    statusBar()->showMessage(result.Succeeded() ? tr("来源检查通过")
        : tr("来源检查未通过 · 当前编辑已保留"), 5000);
    return result.Succeeded();
}

void MainWindow::ShowRecoveryNotice() {
    // 无界面验证不能读取或修改使用者的持久恢复状态。
    if (startupMode_ != StartupMode::Normal || !projectNoticeBar_) return;
    const auto latest = QSettings{}.value("project/latestRecovery").toString();
    const QFileInfo file(latest);
    if (latest.isEmpty() || !file.isFile() || file.suffix().compare("forge", Qt::CaseInsensitive) != 0)
        return;
    project_->recoveryNoticePath_ = std::filesystem::path(file.absoluteFilePath().toStdWString());
    project_->projectNoticeDismissed_ = false;
    UpdateProjectNotice();
}

void MainWindow::UpdateProjectNotice() {
    if (!projectNoticeBar_) return;
    if (project_->projectNoticeDismissed_) {
        projectNoticeBar_->hide();
        return;
    }
    QString text;
    QString detail;
    if (project_->sourceChangeSuspected_) {
        text = project_->sourceChangeConfirmed_ ? tr("来源检查未通过 · 当前编辑已保留")
                                     : tr("源文件可能已变化 · 当前编辑已保留");
        detail = tr("检查地图、工作区及工程文件是否仍与打开时的快照一致。检查不会重载或丢弃编辑。");
        projectNoticeAction_->setText(tr("检查来源"));
    } else if (!project_->recoveryNoticePath_.empty()) {
        text = tr("可恢复工程 · %1").arg(PathText(project_->recoveryNoticePath_.filename()));
        detail = PathText(project_->recoveryNoticePath_);
        projectNoticeAction_->setText(tr("恢复编辑"));
    } else if (project_->projectWatchLimited_) {
        text = tr("部分路径未实时监测 · 保存前仍会完整检查");
        detail = tr("实时观察最多 2048 个路径；部分路径可能不存在或系统未接受观察请求。保存前仍检查完整来源。");
        projectNoticeAction_->setText(tr("检查来源"));
    } else {
        projectNoticeBar_->hide();
        return;
    }
    projectNoticeText_->setText(text);
    projectNoticeText_->setToolTip(detail);
    projectNoticeText_->setAccessibleName(text);
    projectNoticeAction_->setToolTip(detail);
    projectNoticeBar_->show();
}
