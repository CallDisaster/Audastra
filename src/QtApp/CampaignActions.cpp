#include "MainWindow.h"
#include "CampaignModule.h"
#include <QAction>
#include <QStatusBar>

void MainWindow::SetupCampaignActions() {
    connect(campaignModule_, &War3Map::Modules::CampaignModule::ChapterOpenRequested, this, [this](const QString& path) {
        // 章节始终打开为受管临时副本。战役会话保持活动，由“更新已打开章节”显式收回修改。
        if (OpenMapArchivePath(std::filesystem::path(path.toStdWString()))) ActivateModule("triggers");
    });
    connect(campaignModule_, &War3Map::Modules::CampaignModule::SessionChanged, this, [this] {
        if (campaignModule_->Session()) { DismissStartupPage(); ActivateModule("campaign"); }
        UpdateExportAvailability(); UpdateWindowTitle();
    });
    connect(campaignModule_, &War3Map::Modules::CampaignModule::Changed, this, [this] {
        UpdateExportAvailability(); UpdateWindowTitle();
    });
    connect(this, &ShellWindow::CurrentModuleChanged, this, [this] {
        UpdateExportAvailability(); UpdateWindowTitle();
    });
}

bool MainWindow::OpenCampaignPath(const std::filesystem::path& path) {
    if (project_->savingMap_ || changesChecking_) return false;
    (void)campaignModule_->Widget();
    if (!campaignModule_->Open(path)) return false;
    DismissStartupPage(); ActivateModule("campaign");
    statusBar()->showMessage(tr("已打开战役。章节、菜单和共享数据分别编辑；保存战役写入 .w3n。"), 7000);
    return true;
}
