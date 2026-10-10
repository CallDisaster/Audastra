#pragma once

void NativeWorkbenchTests::MapFileDropUsesOpenTransactionAndKeepsPendingEdits() {
    QTemporaryDir fixture;
    QVERIFY(fixture.isValid());
    const auto first = NativePath(fixture.filePath(QStringLiteral("地图 一.W3X")));
    const auto second = NativePath(fixture.filePath(QStringLiteral("地图 二.w3m")));
    QVERIFY(MakeArchive(first, 420).Succeeded());
    QVERIFY(MakeArchive(second, 731).Succeeded());
    const auto sourceBytes = ReadFile(first);
    MainWindow window(MainWindow::StartupMode::HeadlessProbe);
    window.ExecuteCommand("module.world");
    auto* world = window.findChild<War3Map::Modules::WorldViewport*>(QStringLiteral("worldViewport"));
    QVERIFY(world); // 拖到WA_NativeWindow世界子窗也须经同一主窗口入口。
    const auto drag = [](QWidget& target, const QList<QUrl>& urls, bool finish = true) {
        QMimeData mime;
        mime.setUrls(urls);
        QDragEnterEvent enter(QPoint(8, 8), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&target, &enter);
        if (!enter.isAccepted() || !finish) return enter.isAccepted();
        QDropEvent drop(QPointF(8, 8), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&target, &drop);
        return drop.isAccepted();
    };
    const auto url = [](const auto& path) { return QUrl::fromLocalFile(QString::fromStdWString(path.wstring())); };
    QVERIFY(!drag(*world, {url(first), url(second)}));
    QVERIFY(!drag(*world, {QUrl(QStringLiteral("https://example.invalid/map.w3x"))}));
    QVERIFY(!drag(*world, {QUrl::fromLocalFile(fixture.path())}));
    QVERIFY(drag(*world, {url(first)}));
    QTRY_VERIFY(!window.mapDropQueued_);
    QVERIFY(window.project_->sourceArchivePath_ == first);
    QCOMPARE(Health(*window.project_->objectDocuments_[0]).value_or(-1), 420);
    QVERIFY(ChangeHealth(window, 640));
    const auto currentRoot = window.project_->projectRoot_;
    window.project_->savingMap_ = true;
    QVERIFY(!drag(*world, {url(second)}));
    QVERIFY(!window.OpenMapArchivePath(second));
    window.project_->savingMap_ = false;
    window.changesChecking_ = true;
    QVERIFY(!drag(window, {url(second)}));
    window.changesChecking_ = false;
    bool sawPrompt = false;
    QTimer closePrompt;
    closePrompt.setInterval(5);
    connect(&closePrompt, &QTimer::timeout, &window, [&] {
        for (auto* top : QApplication::topLevelWidgets()) {
            auto* prompt = qobject_cast<QMessageBox*>(top);
            if (!prompt || prompt->parentWidget() != &window) continue;
            sawPrompt = true;
            prompt->reject(); // 继续编辑；不能因拖放丢失未保存数据。
        }
    });
    closePrompt.start();
    QVERIFY(drag(*world, {url(second)}));
    QTRY_VERIFY(!window.mapDropQueued_);
    closePrompt.stop();
    QVERIFY(sawPrompt);
    QVERIFY(window.project_->sourceArchivePath_ == first);
    QVERIFY(window.project_->projectRoot_ == currentRoot);
    QCOMPARE(Health(*window.project_->objectDocuments_[0]).value_or(-1), 640);
    const auto invalid = fixture.filePath(QStringLiteral("损坏.w3x"));
    QVERIFY(WriteText(invalid, "not a map"));
    QVERIFY(drag(window, {QUrl::fromLocalFile(invalid)}));
    QTRY_VERIFY(!window.mapDropQueued_);
    QVERIFY(window.project_->sourceArchivePath_ == first);
    QCOMPARE(Health(*window.project_->objectDocuments_[0]).value_or(-1), 640);
    QCOMPARE(ReadFile(first), sourceBytes);
}
