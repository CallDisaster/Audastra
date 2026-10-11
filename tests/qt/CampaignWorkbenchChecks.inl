#include "CampaignModule.h"
#include <QCloseEvent>

// 使用已有主窗夹具验证真实动作路由。所有归档和备份都在本用例的临时目录里。
void NativeWorkbenchTests::CampaignModuleMainWindowRouting() {
    QTemporaryDir fixture;
    QVERIFY(fixture.isValid());
    const auto map = NativePath(fixture.filePath(QStringLiteral("original-map.w3x")));
    QVERIFY(MakeArchive(map, 420).Succeeded());
    const auto originalMap = ReadFile(map);
    const ScopedMapBackupPreferences backupPreferences(fixture.filePath(QStringLiteral("backups")));

    auto campaign = War3Map::Services::CampaignSession::Create();
    War3Map::Services::CampaignCommand settings;
    settings.operation = War3Map::Services::CampaignOperation::SetInfo;
    settings.info = campaign->Info();
    settings.info.name = "Original Campaign";
    QVERIFY(campaign->Apply(campaign->Revision(), settings).Succeeded());
    const auto path = NativePath(fixture.filePath(QStringLiteral("original-campaign.w3n")));
    QVERIFY(campaign->Save(campaign->Revision(), path, false).Succeeded());
    const auto originalCampaign = ReadFile(path);

    MainWindow window(MainWindow::StartupMode::HeadlessProbe);
    QVERIFY(window.OpenMapArchivePath(map));
    const auto mapIdentity = window.project_->sourceArchivePath_;
    QVERIFY(window.OpenCampaignPath(path));
    QCOMPARE(window.CurrentModule(), QStringLiteral("campaign"));
    QVERIFY(window.windowTitle().contains(QStringLiteral("战役")));
    QVERIFY(!window.saveProjectAction_->isEnabled());
    QVERIFY(!window.testMapAction_->isEnabled());
    QVERIFY(window.saveMapInPlaceAction_->isEnabled());

    settings.info = window.campaignModule_->Session()->Info();
    settings.info.description = "Changed through the campaign module";
    QVERIFY(window.campaignModule_->ApplyCommand(settings));
    QVERIFY(window.campaignModule_->Dirty());
    // 此 QAction 是 Ctrl+S 的同一入口，必须写战役并使用共用备份规则。
    window.saveMapInPlaceAction_->trigger();
    QVERIFY(!window.campaignModule_->Dirty());
    auto reopened = War3Map::Services::CampaignSession::Open(path);
    QVERIFY(reopened.Succeeded() && reopened.value);
    QCOMPARE((*reopened.value)->Info().description, settings.info.description);
    QCOMPARE(window.project_->sourceArchivePath_, mapIdentity);
    QCOMPARE(ReadFile(map), originalMap);
    const auto backups = QDir(fixture.filePath(QStringLiteral("backups"))).entryInfoList({QStringLiteral("*.w3n")}, QDir::Files);
    QCOMPARE(backups.size(), qsizetype{1});
    QCOMPARE(ReadFile(NativePath(backups.front().absoluteFilePath())), originalCampaign);

    const auto savedCampaign = ReadFile(path);
    settings.info = window.campaignModule_->Session()->Info();
    settings.info.description = "Unsaved campaign change";
    QVERIFY(window.campaignModule_->ApplyCommand(settings));
    bool campaignPrompt = false;
    QTimer::singleShot(0, &window, [&] {
        auto* prompt = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        if (!prompt) return;
        campaignPrompt = prompt->windowTitle() == QStringLiteral("战役尚未保存");
        prompt->reject();
    });
    QCloseEvent close;
    QApplication::sendEvent(&window, &close);
    QVERIFY(campaignPrompt);
    QVERIFY(!close.isAccepted());
    QVERIFY(window.campaignModule_->Dirty());
    QCOMPARE(ReadFile(path), savedCampaign);
    QCOMPARE(ReadFile(map), originalMap);
}
