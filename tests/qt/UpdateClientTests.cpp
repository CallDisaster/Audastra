#include "UpdateClient.h"
#include "UpdateFixtures.h"

#include <QDate>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

using namespace War3Map::QtUpdate;
using namespace UpdateFixtures;

class UpdateClientTests final : public QObject {
    Q_OBJECT
private slots:
    void SignedLoopbackDownloadKeepsExactInputsAndOnlyPreparesInstallation();
    void HttpAndParseFailuresNeverReportLatest_data();
    void HttpAndParseFailuresNeverReportLatest();
    void StartupCheckIsDelayedDailyAndSkippedVersionOnlySuppressesAutomaticPrompt();
    void StableAndBetaUseDifferentEndpointsAndSemver();
    void SignatureAndPackageFailuresNeverPrepareInstallation_data();
    void SignatureAndPackageFailuresNeverPrepareInstallation();
    void CancellingARealPartialDownloadRemovesItsOwnedTemporaryFiles();
    void InterruptedPackageDownloadResumesFromReceivedBytes_data();
    void InterruptedPackageDownloadResumesFromReceivedBytes();
    void RedirectsStayOnExplicitLoopbackAndNeverCarryCredentials();
    void ConfigurationWithoutTrustedKeyCanReadVersionButCannotDownload();
    void ResponseBudgetTimeoutAndRedirectLoopAreBounded();
    void CommittedPreferencesNeverWriteAndStagedChecksKeepTheSavedChannel();
    void ReadyFilesAreRemovedUnlessTheInstallerRetainsTheExactReceipt();
    void InvalidReleaseAssetsNeverDownload_data();
    void InvalidReleaseAssetsNeverDownload();
    void InstalledBaselineFailuresKeepTheReleaseVisibleAndBlockDownload_data();
    void InstalledBaselineFailuresKeepTheReleaseVisibleAndBlockDownload();
    void RecheckingDoesNotReuseAnEarlierTrustedInstallation();
    void CancellingInstalledBaselineCheckRejectsReentryAndDiscardsItsResult();
    void SwitchingChannelsDuringInstalledBaselineCheckDiscardsTheOldRelease();
    void DestroyingClientFromInstalledBaselineStatusDoesNotStartAnOrphanWorker();
    void DestroyingClientFromChannelCancellationStopsApplyingPreferences();
    void DestroyingClientFromCommittedChannelIdleStopsPreferencesNotification();
};

void UpdateClientTests::SignedLoopbackDownloadKeepsExactInputsAndOnlyPreparesInstallation() {
    QTemporaryDir directory; QVERIFY(directory.isValid()); Bundle bundle; Server server; QVERIFY(server.Start()); server.Publish(bundle);
    QSettings settings(directory.filePath("preferences.ini"), QSettings::IniFormat);
    UpdateClient client(server.Source(bundle, directory.filePath("downloads"), CreateSignedInstallation(directory.filePath("install"))), settings);
    QSignalSpy ready(&client, &UpdateClient::PackagePrepared); QSignalSpy progress(&client, &UpdateClient::DownloadProgress);
    QVERIFY(client.Check()); QTRY_VERIFY_WITH_TIMEOUT(client.Status().state == UpdateState::Available, 3000);
    QVERIFY(client.Status().canDownload); QCOMPARE(server.requests.size(), 1);
    // 检查只取版本JSON；作者点击下载前未请求任何清单、签名或ZIP。
    QVERIFY(client.Download()); QTRY_VERIFY_WITH_TIMEOUT(client.Status().state == UpdateState::Ready, 5000);
    QCOMPARE(ready.count(), 1); QVERIFY(!client.IsBusy()); QVERIFY(client.Prepared().has_value());
    const auto prepared = *client.Prepared(); QCOMPARE(prepared.version, QStringLiteral("0.2.0"));
    QCOMPARE(Read(prepared.manifestPath), bundle.manifest); QCOMPARE(Read(prepared.signaturePath), bundle.signature); QCOMPARE(Read(prepared.packagePath), bundle.package);
    QCOMPARE(prepared.manifestSha256.toLatin1(), Hash(bundle.manifest)); QCOMPARE(prepared.packageSha256.toLatin1(), Hash(bundle.package));
    QCOMPARE(server.requests.size(), 4); QVERIFY(progress.count() >= 2);
    const auto final = progress.last(); QCOMPARE(final[0].toLongLong(), qint64(bundle.package.size())); QCOMPARE(final[1].toLongLong(), qint64(bundle.package.size()));
    for (const auto& request : server.requests) {
        QVERIFY(!request.toLower().contains("authorization:")); QVERIFY(!request.toLower().contains("cookie:"));
        QVERIFY(!request.contains("0.1.0")); QVERIFY(!request.contains("preferences.ini"));
    }
    // 夹具文件不是可执行程序；这里只验证准备回执，没有启动或安装任何程序。
    QVERIFY(QFileInfo(prepared.packagePath).absoluteFilePath().contains(QStringLiteral("downloads")));
}

void UpdateClientTests::CommittedPreferencesNeverWriteAndStagedChecksKeepTheSavedChannel() {
    QTemporaryDir directory; QVERIFY(directory.isValid()); Bundle bundle; Server server; QVERIFY(server.Start()); server.Publish(bundle);
    server.routes["/repos/fixture/editor/releases?per_page=100"] = QJsonDocument(QJsonArray{
        QJsonDocument::fromJson(server.Release(bundle)).object()}).toJson();
    QSettings settings(directory.filePath("preferences.ini"), QSettings::IniFormat);
    settings.setValue("unrelated/value", QStringLiteral("保留")); settings.sync(); const auto before = Read(settings.fileName());
    UpdateClient client(server.Source(bundle, directory.path(), CreateSignedInstallation(directory.filePath("install"))), settings);
    UpdatePreferences draft{false, War3Map::Update::UpdateChannel::Beta, QStringLiteral("0.2.0")};
    QVERIFY(client.CheckWithPreferences(draft)); QTRY_VERIFY_WITH_TIMEOUT(client.Status().state == UpdateState::Available, 3000);
    QCOMPARE(client.Status().channel, War3Map::Update::UpdateChannel::Beta); QCOMPARE(client.Channel(), War3Map::Update::UpdateChannel::Stable);
    QVERIFY(client.AutomaticCheckEnabled()); QVERIFY(client.SkippedVersion().isEmpty());
    QVERIFY(client.SkipThisVersionInSession()); settings.sync(); QCOMPARE(Read(settings.fileName()), before);
    // Apply只更新运行时；它本身永远不能成为另一个写盘路径。
    QString error; QVERIFY(client.ApplyCommittedPreferences(draft, &error)); settings.sync(); QCOMPARE(Read(settings.fileName()), before);
    QVERIFY(client.Preferences() == draft);
    auto invalid = draft; invalid.channel = static_cast<War3Map::Update::UpdateChannel>(77);
    QVERIFY(!client.ApplyCommittedPreferences(invalid, &error)); QVERIFY(!error.isEmpty()); QVERIFY(client.Preferences() == draft);
    invalid = draft; invalid.skippedVersion = QStringLiteral("TRIGSTR_123");
    QVERIFY(!UpdateClient::WritePreferences(settings, invalid, &error)); settings.sync(); QCOMPARE(Read(settings.fileName()), before);
    QVERIFY(UpdateClient::WritePreferences(settings, draft, &error)); settings.sync(); QCOMPARE(settings.status(), QSettings::NoError);
    QVERIFY(UpdateClient::ReadPreferences(settings) == draft); QCOMPARE(settings.value("unrelated/value").toString(), QStringLiteral("保留"));
    QCOMPARE(server.requests.size(), 1);
}

void UpdateClientTests::ReadyFilesAreRemovedUnlessTheInstallerRetainsTheExactReceipt() {
    QTemporaryDir directory; QVERIFY(directory.isValid()); Bundle bundle; Server server; QVERIFY(server.Start()); server.Publish(bundle);
    for (const bool retain : {false, true}) {
        QSettings settings(directory.filePath(retain ? "retained.ini" : "unretained.ini"), QSettings::IniFormat);
        PreparedUpdate receipt;
        {
            UpdateClient client(server.Source(bundle, directory.filePath(retain ? "retained" : "unretained"), CreateSignedInstallation(directory.filePath("install"))), settings);
            QString error; QVERIFY(!client.RetainPreparedForInstaller(receipt, &error)); QVERIFY(!error.isEmpty());
            QVERIFY(client.Check()); QTRY_VERIFY_WITH_TIMEOUT(client.Status().state == UpdateState::Available, 3000);
            QVERIFY(client.Download()); QTRY_VERIFY_WITH_TIMEOUT(client.Status().state == UpdateState::Ready, 5000);
            QVERIFY(client.Prepared()); receipt = *client.Prepared();
            auto wrong = receipt; wrong.packageSha256 = QString(64, QLatin1Char('0'));
            QVERIFY(!client.RetainPreparedForInstaller(wrong, &error)); QVERIFY(!error.isEmpty());
            wrong = receipt; wrong.packagePath = directory.filePath("unverified.zip");
            QVERIFY(!client.RetainPreparedForInstaller(wrong, &error));
            if (retain) QVERIFY(client.RetainPreparedForInstaller(receipt, &error));
            QCOMPARE(Read(receipt.packagePath), bundle.package);
        }
        QCOMPARE(QFileInfo::exists(receipt.packagePath), retain);
        QCOMPARE(QFileInfo::exists(receipt.manifestPath), retain);
        QCOMPARE(QFileInfo::exists(receipt.signaturePath), retain);
    }
}

void UpdateClientTests::InvalidReleaseAssetsNeverDownload_data() {
    QTest::addColumn<QString>("mutation");
    QTest::newRow("duplicate-name") << QStringLiteral("duplicate");
    QTest::newRow("fractional-size") << QStringLiteral("fractional");
    QTest::newRow("negative-size") << QStringLiteral("negative");
    QTest::newRow("excessive-size") << QStringLiteral("excessive");
}
void UpdateClientTests::InvalidReleaseAssetsNeverDownload() {
    QFETCH(QString, mutation);
    QTemporaryDir directory; QVERIFY(directory.isValid()); Bundle bundle; Server server; QVERIFY(server.Start()); server.Publish(bundle);
    auto release = QJsonDocument::fromJson(server.Release(bundle)).object(); auto assets = release.value("assets").toArray();
    if (mutation == "duplicate") assets.append(assets.at(0));
    else {
        auto asset = assets[0].toObject();
        asset["size"] = mutation == "fractional" ? 64.5 : mutation == "negative" ? -1.0 : 2147483649.0;
        assets[0] = asset;
    }
    release["assets"] = assets; server.routes["/repos/fixture/editor/releases/latest"] = QJsonDocument(release).toJson();
    QSettings settings(directory.filePath("preferences.ini"), QSettings::IniFormat); UpdateClient client(server.Source(bundle, directory.path(), CreateSignedInstallation(directory.filePath("install"))), settings);
    QSignalSpy prepared(&client, &UpdateClient::PackagePrepared); QVERIFY(client.Check());
    QTRY_VERIFY_WITH_TIMEOUT(client.Status().state == UpdateState::Failed, 3000);
    QCOMPARE(client.Status().errorCode, QStringLiteral("UPDATE_RELEASE_ASSET")); QVERIFY(!client.Download());
    QCOMPARE(server.requests.size(), 1); QCOMPARE(prepared.count(), 0);
}

void UpdateClientTests::HttpAndParseFailuresNeverReportLatest_data() {
    QTest::addColumn<int>("http"); QTest::addColumn<QByteArray>("body"); QTest::addColumn<QString>("code");
    QTest::newRow("private404") << 404 << QByteArray("private repo") << QStringLiteral("UPDATE_HTTP_404");
    QTest::newRow("forbidden403") << 403 << QByteArray("rate limited") << QStringLiteral("UPDATE_RATE_LIMIT");
    QTest::newRow("limited429") << 429 << QByteArray("rate limited") << QStringLiteral("UPDATE_RATE_LIMIT");
    QTest::newRow("server500") << 500 << QByteArray("failure") << QStringLiteral("UPDATE_HTTP_ERROR");
    QTest::newRow("invalid-json") << 200 << QByteArray("{broken") << QStringLiteral("UPDATE_RELEASE_JSON");
    QTest::newRow("wrong-shape") << 200 << QByteArray("[]") << QStringLiteral("UPDATE_RELEASE_JSON");
    QTest::newRow("missing-fields") << 200 << QByteArray("{}") << QStringLiteral("UPDATE_RELEASE_INVALID");
}
void UpdateClientTests::HttpAndParseFailuresNeverReportLatest() {
    QFETCH(int, http); QFETCH(QByteArray, body); QFETCH(QString, code);
    QTemporaryDir directory; QVERIFY(directory.isValid()); Bundle bundle; Server server; QVERIFY(server.Start());
    server.responder = [http, body](QTcpSocket* socket, const QByteArray&) { socket->write(Http(body, http)); socket->disconnectFromHost(); };
    QSettings settings(directory.filePath("preferences.ini"), QSettings::IniFormat); UpdateClient client(server.Source(bundle, directory.path(), CreateSignedInstallation(directory.filePath("install"))), settings);
    QSignalSpy ready(&client, &UpdateClient::PackagePrepared);
    QVERIFY(client.Check()); QTRY_VERIFY_WITH_TIMEOUT(client.Status().state == UpdateState::Failed, 3000);
    QCOMPARE(client.Status().errorCode, code); QVERIFY(!client.Status().message.isEmpty()); QVERIFY(!client.Status().message.contains(QStringLiteral("已是")));
    QVERIFY(!client.Status().canDownload); QVERIFY(!client.Download()); QCOMPARE(ready.count(), 0); QVERIFY(!client.Prepared());
}

void UpdateClientTests::StartupCheckIsDelayedDailyAndSkippedVersionOnlySuppressesAutomaticPrompt() {
    QTemporaryDir directory; QVERIFY(directory.isValid()); Bundle bundle; Server server; QVERIFY(server.Start()); server.Publish(bundle);
    QSettings settings(directory.filePath("preferences.ini"), QSettings::IniFormat); UpdateClient client(server.Source(bundle, directory.path(), CreateSignedInstallation(directory.filePath("install"))), settings);
    QVERIFY(client.AutomaticCheckEnabled()); QCOMPARE(client.Channel(), War3Map::Update::UpdateChannel::Stable);
    client.ScheduleAutomaticCheck(40); QCOMPARE(server.requests.size(), 0);
    QTRY_VERIFY_WITH_TIMEOUT(client.Status().state == UpdateState::Available, 3000); QVERIFY(!client.Status().manual);
    QCOMPARE(settings.value("update/lastAutomaticCheckDate").toString(), QDate::currentDate().toString(Qt::ISODate));
    QCOMPARE(server.requests.size(), 1); QVERIFY(!client.Check(false)); QCOMPARE(server.requests.size(), 1);
    QVERIFY(client.SkipThisVersion()); QCOMPARE(client.SkippedVersion(), QStringLiteral("0.2.0"));
    // 原创设置模拟下一日，不改系统时间或正式应用设置。
    settings.setValue("update/lastAutomaticCheckDate", QDate::currentDate().addDays(-1).toString(Qt::ISODate)); settings.sync();
    QVERIFY(client.Check(false)); QTRY_VERIFY_WITH_TIMEOUT(client.Status().state == UpdateState::Skipped, 3000); QCOMPARE(server.requests.size(), 2);
    QVERIFY(client.Check(true)); QTRY_VERIFY_WITH_TIMEOUT(client.Status().state == UpdateState::Available, 3000); QCOMPARE(server.requests.size(), 3);
    QVERIFY(client.SetAutomaticCheckEnabled(false)); client.ScheduleAutomaticCheck(0); QVERIFY(!client.Check(false)); QCOMPARE(server.requests.size(), 3);
    server.Publish(bundle, QStringLiteral("0.3.0")); QVERIFY(client.Check());
    QTRY_VERIFY_WITH_TIMEOUT(client.Status().state == UpdateState::Available, 3000);
    QCOMPARE(client.Status().availableVersion, QStringLiteral("0.3.0")); QVERIFY(client.Status().canDownload);
}

void UpdateClientTests::StableAndBetaUseDifferentEndpointsAndSemver() {
    QTemporaryDir directory; QVERIFY(directory.isValid()); Bundle stable; Bundle beta(QStringLiteral("0.3.0-beta.2")); Server server; QVERIFY(server.Start()); server.Publish(stable);
    QSettings settings(directory.filePath("preferences.ini"), QSettings::IniFormat);
    UpdateClient client(server.Source(stable, directory.path(), CreateSignedInstallation(directory.filePath("install"), QStringLiteral("0.2.0")), QStringLiteral("0.2.0")), settings);
    QVERIFY(client.Check()); QTRY_VERIFY_WITH_TIMEOUT(client.Status().state == UpdateState::UpToDate, 3000);
    QVERIFY(server.requests.back().startsWith("GET /repos/fixture/editor/releases/latest "));
    QJsonArray releases{QJsonDocument::fromJson(server.Release(beta, QStringLiteral("0.3.0-beta.2"), true)).object(),
        QJsonDocument::fromJson(server.Release(beta, QStringLiteral("0.3.0-beta.10"), true)).object(), QJsonDocument::fromJson(server.Release(stable)).object()};
    server.routes.insert("/repos/fixture/editor/releases?per_page=100", QJsonDocument(releases).toJson());
    QVERIFY(client.SetChannel(War3Map::Update::UpdateChannel::Beta)); QVERIFY(client.Check());
    QTRY_VERIFY_WITH_TIMEOUT(client.Status().state == UpdateState::Available, 3000); QCOMPARE(client.Status().availableVersion, QStringLiteral("0.3.0-beta.10"));
    QVERIFY(server.requests.back().startsWith("GET /repos/fixture/editor/releases?per_page=100 "));
    QVERIFY(client.SetChannel(War3Map::Update::UpdateChannel::Stable)); server.Publish(beta, QStringLiteral("0.3.0-beta.2"), true);
    QVERIFY(client.Check()); QTRY_VERIFY_WITH_TIMEOUT(client.Status().state == UpdateState::Failed, 3000);
    QCOMPARE(client.Status().errorCode, QStringLiteral("UPDATE_RELEASE_UNAVAILABLE"));
}

void UpdateClientTests::SignatureAndPackageFailuresNeverPrepareInstallation_data() {
    QTest::addColumn<QString>("mutation"); QTest::addColumn<QString>("code");
    QTest::newRow("wrong-signature") << QStringLiteral("signature") << QStringLiteral("UPDATE_SIGNATURE_INVALID");
    QTest::newRow("missing-signature") << QStringLiteral("missing") << QStringLiteral("UPDATE_SIGNATURE_MISSING_OR_INVALID");
    QTest::newRow("raw-manifest-change") << QStringLiteral("raw") << QStringLiteral("UPDATE_SIGNATURE_INVALID");
    QTest::newRow("wrong-package-sha") << QStringLiteral("sha") << QStringLiteral("UPDATE_PACKAGE_SHA_MISMATCH");
    QTest::newRow("short-package") << QStringLiteral("short") << QStringLiteral("UPDATE_PACKAGE_SIZE");
    QTest::newRow("long-package") << QStringLiteral("long") << QStringLiteral("UPDATE_PACKAGE_SIZE");
    QTest::newRow("new-version-race") << QStringLiteral("version") << QStringLiteral("UPDATE_RELEASE_MANIFEST_VERSION");
    QTest::newRow("upgrade-too-old") << QStringLiteral("minimum") << QStringLiteral("UPDATE_DIRECT_UPGRADE");
    QTest::newRow("extra-zip-file") << QStringLiteral("extra") << QStringLiteral("UPDATE_ZIP_EXTRA_MEMBER");
}
void UpdateClientTests::SignatureAndPackageFailuresNeverPrepareInstallation() {
    QFETCH(QString, mutation); QFETCH(QString, code);
    QTemporaryDir directory; QVERIFY(directory.isValid()); Bundle bundle; Server server; QVERIFY(server.Start());
    if (mutation == "version") bundle = Bundle(QStringLiteral("0.3.0"));
    if (mutation == "minimum") bundle = Bundle(QStringLiteral("0.2.0"), QStringLiteral("0.1.5"));
    if (mutation == "extra") {
        bundle.package = Zip({{QStringLiteral("war3-map-editor.exe"), QByteArray("original fixture executable data")},
            {QStringLiteral("plugins/fixture.txt"), QByteArray("original fixture plugin data")}, {QStringLiteral("extra.bin"), QByteArray("not allowed")}});
        auto json = QJsonDocument::fromJson(bundle.manifest).object(); auto zip = json.value("zip").toObject();
        zip.insert("bytes", qint64(bundle.package.size())); zip.insert("sha256", QString::fromLatin1(Hash(bundle.package))); json.insert("zip", zip);
        bundle.manifest = QJsonDocument(json).toJson(); bundle.signature = bundle.signer.Sign(bundle.manifest);
    }
    server.Publish(bundle);
    if (mutation == "signature") { auto bad = bundle.signature; bad[0] = char(bad.at(0) ^ 1); server.routes["/assets/update-manifest.json.sig"] = bad; }
    if (mutation == "missing") server.routes["/assets/update-manifest.json.sig"].clear();
    if (mutation == "raw") server.routes["/assets/update-manifest.json"] += " ";
    if (mutation == "sha") { auto bad = bundle.package; bad[0] = char(bad.at(0) ^ 1); server.routes["/assets/fixture.zip"] = bad; }
    if (mutation == "short") server.routes["/assets/fixture.zip"].chop(1);
    if (mutation == "long") server.routes["/assets/fixture.zip"] += "x";
    QSettings settings(directory.filePath("preferences.ini"), QSettings::IniFormat); UpdateClient client(server.Source(bundle, directory.filePath("downloads"), CreateSignedInstallation(directory.filePath("install"))), settings);
    QSignalSpy ready(&client, &UpdateClient::PackagePrepared); QVERIFY(client.Check()); QTRY_VERIFY_WITH_TIMEOUT(client.Status().state == UpdateState::Available, 3000);
    QVERIFY(client.Download()); QTRY_VERIFY_WITH_TIMEOUT(client.Status().state == UpdateState::Failed, 5000);
    QCOMPARE(client.Status().errorCode, code); QCOMPARE(ready.count(), 0); QVERIFY(!client.Prepared());
    QCOMPARE(QDir(directory.filePath("downloads")).entryList(QDir::Dirs | QDir::NoDotAndDotDot).size(), 0);
}

void UpdateClientTests::CancellingARealPartialDownloadRemovesItsOwnedTemporaryFiles() {
    QTemporaryDir directory; QVERIFY(directory.isValid()); Bundle bundle; Server server; QVERIFY(server.Start()); server.Publish(bundle);
    server.responder = [&server, &bundle](QTcpSocket* socket, const QByteArray& path) {
        if (path == "/assets/fixture.zip") {
            socket->write(QByteArray("HTTP/1.1 200 OK\r\nContent-Length: ") + QByteArray::number(bundle.package.size()) + "\r\n\r\n" + bundle.package.left(32));
            return; // 保持真实TCP连接，等待客户端取消；不是伪造的完成回执。
        }
        socket->write(Http(server.routes.value(path))); socket->disconnectFromHost();
    };
    QSettings settings(directory.filePath("preferences.ini"), QSettings::IniFormat); UpdateClient client(server.Source(bundle, directory.filePath("downloads"), CreateSignedInstallation(directory.filePath("install"))), settings);
    QSignalSpy ready(&client, &UpdateClient::PackagePrepared); QSignalSpy progress(&client, &UpdateClient::DownloadProgress);
    QVERIFY(client.Check()); QTRY_VERIFY_WITH_TIMEOUT(client.Status().state == UpdateState::Available, 3000); QVERIFY(client.Download());
    QTRY_VERIFY_WITH_TIMEOUT(!progress.empty() && progress.last()[0].toLongLong() == 32, 3000);
    client.Cancel(); QCOMPARE(client.Status().state, UpdateState::Cancelled); QVERIFY(!client.IsBusy()); QCOMPARE(ready.count(), 0);
    QCOMPARE(QDir(directory.filePath("downloads")).entryList(QDir::Dirs | QDir::NoDotAndDotDot).size(), 0);
    QVERIFY(client.Check()); QTRY_VERIFY_WITH_TIMEOUT(client.Status().state == UpdateState::Available, 3000);
}

void UpdateClientTests::InterruptedPackageDownloadResumesFromReceivedBytes_data() {
    QTest::addColumn<QString>("interruption"); QTest::addColumn<QString>("resume");
    QTest::newRow("closed-range") << QStringLiteral("close") << QStringLiteral("range");
    QTest::newRow("stalled-range") << QStringLiteral("stall") << QStringLiteral("range");
    QTest::newRow("closed-full") << QStringLiteral("close") << QStringLiteral("full");
    QTest::newRow("wrong-range") << QStringLiteral("close") << QStringLiteral("wrong");
}
void UpdateClientTests::InterruptedPackageDownloadResumesFromReceivedBytes() {
    QFETCH(QString, interruption); QFETCH(QString, resume);
    QTemporaryDir directory; QVERIFY(directory.isValid()); Bundle bundle; Server server; QVERIFY(server.Start()); server.Publish(bundle);
    int packageRequests = 0; QByteArray resumedRequest;
    server.responder = [&](QTcpSocket* socket, const QByteArray& path) {
        if (path != "/assets/fixture.zip") { socket->write(Http(server.routes.value(path))); socket->disconnectFromHost(); return; }
        const auto& package = bundle.package; const auto total = QByteArray::number(package.size());
        if (++packageRequests == 1) {
            // 真实发出声明完整长度的头和前32字节后断开或停住，模拟 CDN 中途停顿。
            socket->write(QByteArray("HTTP/1.1 200 OK\r\nContent-Length: ") + total + "\r\n\r\n" + package.left(32));
            if (interruption == QStringLiteral("close")) { socket->flush(); socket->abort(); }
            return;
        }
        resumedRequest = server.requests.last();
        if (resume == QStringLiteral("full")) socket->write(Http(package));
        else {
            const qsizetype from = resume == QStringLiteral("wrong") ? 16 : 32;
            const auto rest = package.mid(from);
            socket->write(QByteArray("HTTP/1.1 206 Partial Content\r\nContent-Length: ") + QByteArray::number(rest.size())
                + "\r\nContent-Range: bytes " + QByteArray::number(from) + '-' + QByteArray::number(package.size() - 1) + '/' + total
                + "\r\nConnection: close\r\n\r\n" + rest);
        }
        socket->disconnectFromHost();
    };
    QSettings settings(directory.filePath("preferences.ini"), QSettings::IniFormat);
    auto source = server.Source(bundle, directory.filePath("downloads"), CreateSignedInstallation(directory.filePath("install"))); source.requestTimeoutMs = 300;
    UpdateClient client(source, settings); QSignalSpy ready(&client, &UpdateClient::PackagePrepared);
    QVERIFY(client.Check()); QTRY_VERIFY_WITH_TIMEOUT(client.Status().state == UpdateState::Available, 3000);
    QVERIFY(client.Download());
    if (resume == QStringLiteral("wrong")) {
        QTRY_VERIFY_WITH_TIMEOUT(client.Status().state == UpdateState::Failed, 5000);
        QCOMPARE(client.Status().errorCode, QStringLiteral("UPDATE_PACKAGE_RANGE")); QCOMPARE(ready.count(), 0);
        QCOMPARE(QDir(directory.filePath("downloads")).entryList(QDir::Dirs | QDir::NoDotAndDotDot).size(), 0);
        return;
    }
    QTRY_VERIFY_WITH_TIMEOUT(client.Status().state == UpdateState::Ready || client.Status().state == UpdateState::Failed, 5000);
    QCOMPARE(client.Status().errorCode, QString()); QCOMPARE(client.Status().state, UpdateState::Ready);
    // Qt 6.7 起请求头名按小写发送。
    QCOMPARE(packageRequests, 2); QVERIFY2(resumedRequest.toLower().contains("\r\nrange: bytes=32-\r\n"), resumedRequest.constData());
    QVERIFY(client.Prepared()); QCOMPARE(Read(client.Prepared()->packagePath), bundle.package);
}

void UpdateClientTests::RedirectsStayOnExplicitLoopbackAndNeverCarryCredentials() {
    QTemporaryDir directory; QVERIFY(directory.isValid()); Bundle bundle; Server server; Server escaped; QVERIFY(server.Start()); QVERIFY(escaped.Start()); server.Publish(bundle);
    QSettings settings(directory.filePath("preferences.ini"), QSettings::IniFormat); UpdateClient client(server.Source(bundle, directory.path(), CreateSignedInstallation(directory.filePath("install"))), settings);
    server.responder = [&server, &escaped](QTcpSocket* socket, const QByteArray& path) {
        const auto body = path.endsWith("latest") ? Http({}, 302, "Location: " + escaped.Url("/escape").toEncoded() + "\r\n") : Http(server.routes.value(path));
        socket->write(body); socket->disconnectFromHost();
    };
    QVERIFY(client.Check()); QTRY_VERIFY_WITH_TIMEOUT(client.Status().state == UpdateState::Failed, 3000);
    QCOMPARE(client.Status().errorCode, QStringLiteral("UPDATE_SOURCE_URL")); QCOMPARE(escaped.requests.size(), 0);
    server.responder = [&server](QTcpSocket* socket, const QByteArray&) {
        socket->write(Http({}, 302, "Location: " + server.Url("/credential?access_token=fixture-secret").toEncoded() + "\r\n")); socket->disconnectFromHost();
    };
    QVERIFY(client.Check()); QTRY_VERIFY_WITH_TIMEOUT(client.Status().state == UpdateState::Failed, 3000);
    QCOMPARE(client.Status().errorCode, QStringLiteral("UPDATE_SOURCE_URL")); QCOMPARE(server.requests.size(), 2);
    QVERIFY(!client.Status().message.contains(QStringLiteral("fixture-secret")));
    server.responder = [&server](QTcpSocket* socket, const QByteArray& path) {
        socket->write(path.endsWith("latest") ? Http({}, 302, "Location: /same-origin\r\n") : Http(server.routes.value("/repos/fixture/editor/releases/latest"))); socket->disconnectFromHost();
    };
    QVERIFY(client.Check()); QTRY_VERIFY_WITH_TIMEOUT(client.Status().state == UpdateState::Available, 3000); QCOMPARE(server.requests.size(), 4);
}

void UpdateClientTests::ConfigurationWithoutTrustedKeyCanReadVersionButCannotDownload() {
    QTemporaryDir directory; QVERIFY(directory.isValid()); Bundle bundle; Server server; QVERIFY(server.Start()); server.Publish(bundle);
    QSettings settings(directory.filePath("preferences.ini"), QSettings::IniFormat); auto source = server.Source(bundle, directory.path(), CreateSignedInstallation(directory.filePath("install"))); source.trustedPublicKeyHex.clear();
    UpdateClient client(source, settings); QVERIFY(client.Check()); QTRY_VERIFY_WITH_TIMEOUT(client.Status().state == UpdateState::Available, 3000);
    QVERIFY(!client.Status().canDownload); QVERIFY(!client.Download()); QCOMPARE(server.requests.size(), 1); QVERIFY(!client.Prepared());
    source.apiRoot = QUrl(QStringLiteral("http://192.0.2.1/repos/fixture/editor"));
    UpdateClient rejected(source, settings); QVERIFY(!rejected.Check()); QCOMPARE(rejected.Status().state, UpdateState::Failed);
    QCOMPARE(rejected.Status().errorCode, QStringLiteral("UPDATE_CONFIGURATION")); QCOMPARE(server.requests.size(), 1);
}

void UpdateClientTests::ResponseBudgetTimeoutAndRedirectLoopAreBounded() {
    QTemporaryDir directory; QVERIFY(directory.isValid()); Bundle bundle; Server server; QVERIFY(server.Start());
    QSettings settings(directory.filePath("preferences.ini"), QSettings::IniFormat); auto source = server.Source(bundle, directory.path(), CreateSignedInstallation(directory.filePath("install"))); source.requestTimeoutMs = 80;
    UpdateClient client(source, settings);
    server.responder = [](QTcpSocket*, const QByteArray&) {};
    QVERIFY(client.Check()); QTRY_VERIFY_WITH_TIMEOUT(client.Status().state == UpdateState::Failed, 3000);
    QCOMPARE(client.Status().errorCode, QStringLiteral("UPDATE_NETWORK_TIMEOUT"));
    server.responder = [](QTcpSocket* socket, const QByteArray&) { socket->write(Http({}, 302, "Location: /loop\r\n")); socket->disconnectFromHost(); };
    QVERIFY(client.Check()); QTRY_VERIFY_WITH_TIMEOUT(client.Status().state == UpdateState::Failed, 3000);
    // 超时检查共 4 次请求（首次 + 3 次重试），重定向循环 6 次（首次 + 5 次跳转），重定向上限不重试。
    QCOMPARE(client.Status().errorCode, QStringLiteral("UPDATE_REDIRECT_LIMIT")); QCOMPARE(server.requests.size(), 10);
    server.responder = [](QTcpSocket* socket, const QByteArray&) { socket->write(Http(QByteArray(2 * 1024 * 1024 + 1, 'x'))); socket->disconnectFromHost(); };
    // 单响应预算不依赖短测试超时，换用常规测试源验证真正的字节拒绝。
    source.requestTimeoutMs = 1000; UpdateClient bounded(source, settings);
    QVERIFY(bounded.Check()); QTRY_VERIFY_WITH_TIMEOUT(bounded.Status().state == UpdateState::Failed, 3000);
    QCOMPARE(bounded.Status().errorCode, QStringLiteral("UPDATE_RESPONSE_LIMIT")); QVERIFY(!bounded.Prepared());
}

void UpdateClientTests::InstalledBaselineFailuresKeepTheReleaseVisibleAndBlockDownload_data() {
    QTest::addColumn<QString>("mutation"); QTest::addColumn<QString>("code");
    QTest::newRow("missing-baseline") << QStringLiteral("missing") << QStringLiteral("UPDATE_BASELINE_NOT_FOUND");
    QTest::newRow("mixed-version") << QStringLiteral("version") << QStringLiteral("UPDATE_BASELINE_IDENTITY_MISMATCH");
    QTest::newRow("mixed-program-file") << QStringLiteral("file") << QStringLiteral("UPDATE_STAGE_PROGRAM_MISMATCH");
    QTest::newRow("untrusted-signature") << QStringLiteral("signature") << QStringLiteral("UPDATE_SIGNATURE_INVALID");
    QTest::newRow("explicit-empty-test-root") << QStringLiteral("empty") << QStringLiteral("UPDATE_BASELINE_NOT_FOUND");
}

void UpdateClientTests::InstalledBaselineFailuresKeepTheReleaseVisibleAndBlockDownload() {
    QFETCH(QString, mutation); QFETCH(QString, code);
    QTemporaryDir directory; QVERIFY(directory.isValid()); Bundle bundle; Server server; QVERIFY(server.Start()); server.Publish(bundle);
    const auto install = CreateSignedInstallation(directory.filePath("install"), mutation == QStringLiteral("version")
        ? QStringLiteral("0.0.9") : QStringLiteral("0.1.0"));
    if (mutation == QStringLiteral("missing")) {
        QVERIFY(QFile::remove(QDir(install).filePath("update-manifest.json")));
        QVERIFY(QFile::remove(QDir(install).filePath("update-manifest.json.sig")));
    } else if (mutation == QStringLiteral("file")) {
        auto bytes = Read(QDir(install).filePath("plugins/fixture.txt")); QVERIFY(!bytes.isEmpty());
        bytes[0] = 'X'; QVERIFY(WriteFixtureFile(QDir(install).filePath("plugins/fixture.txt"), bytes));
    } else if (mutation == QStringLiteral("signature")) {
        QVERIFY(WriteFixtureFile(QDir(install).filePath("update-manifest.json.sig"), QByteArray(64, 'x')));
    }
    const auto manifestBefore = Read(QDir(install).filePath("update-manifest.json"));
    const auto programBefore = Read(QDir(install).filePath("plugins/fixture.txt"));
    auto source = server.Source(bundle, directory.filePath("downloads"), mutation == QStringLiteral("empty") ? QString{} : install);
    QSettings settings(directory.filePath("preferences.ini"), QSettings::IniFormat);
    UpdateClient client(source, settings); QSignalSpy ready(&client, &UpdateClient::PackagePrepared);
    QVERIFY(client.Check()); QTRY_VERIFY_WITH_TIMEOUT(client.Status().state == UpdateState::Available, 3000);
    QCOMPARE(client.Status().availableVersion, QStringLiteral("0.2.0"));
    QCOMPARE(client.Status().errorCode, code); QVERIFY(!client.Status().canDownload); QVERIFY(!client.IsBusy());
    QVERIFY(client.Status().message.contains(QStringLiteral("完整包")));
    QVERIFY(client.Status().message.contains(QStringLiteral("新的文件夹")));
    QVERIFY(!client.Download()); QVERIFY(!client.Prepared()); QCOMPARE(ready.count(), 0); QCOMPARE(server.requests.size(), 1);
    QVERIFY(!QFileInfo::exists(directory.filePath("downloads")));
    QCOMPARE(Read(QDir(install).filePath("update-manifest.json")), manifestBefore);
    QCOMPARE(Read(QDir(install).filePath("plugins/fixture.txt")), programBefore);
}

void UpdateClientTests::RecheckingDoesNotReuseAnEarlierTrustedInstallation() {
    QTemporaryDir directory; QVERIFY(directory.isValid()); Bundle bundle; Server server; QVERIFY(server.Start()); server.Publish(bundle);
    const auto install = CreateSignedInstallation(directory.filePath("install"));
    QSettings settings(directory.filePath("preferences.ini"), QSettings::IniFormat);
    UpdateClient client(server.Source(bundle, directory.filePath("downloads"), install), settings);
    QVERIFY(client.Check()); QTRY_VERIFY_WITH_TIMEOUT(client.Status().state == UpdateState::Available, 3000);
    QVERIFY(client.Status().canDownload);
    const auto path = QDir(install).filePath("war3-map-editor.exe"); const auto original = Read(path); QVERIFY(!original.isEmpty());
    auto changed = original; changed[0] = 'X'; QVERIFY(WriteFixtureFile(path, changed));
    QVERIFY(client.Check()); QTRY_VERIFY_WITH_TIMEOUT(client.Status().state == UpdateState::Available, 3000);
    QVERIFY(!client.Status().canDownload); QVERIFY(!client.Download());
    QCOMPARE(client.Status().errorCode, QStringLiteral("UPDATE_STAGE_PROGRAM_MISMATCH"));
    QVERIFY(WriteFixtureFile(path, original));
    QVERIFY(client.Check()); QTRY_VERIFY_WITH_TIMEOUT(client.Status().state == UpdateState::Available, 3000);
    QVERIFY(client.Status().canDownload); QVERIFY(client.Status().errorCode.isEmpty()); QCOMPARE(server.requests.size(), 3);
}

void UpdateClientTests::CancellingInstalledBaselineCheckRejectsReentryAndDiscardsItsResult() {
    QTemporaryDir directory; QVERIFY(directory.isValid()); Bundle bundle; Server server; QVERIFY(server.Start()); server.Publish(bundle);
    const auto install = CreateSignedInstallation(directory.filePath("install"));
    QSettings settings(directory.filePath("preferences.ini"), QSettings::IniFormat);
    UpdateClient client(server.Source(bundle, directory.filePath("downloads"), install), settings);
    bool cancelled = false, eventLoopContinued = false;
    int available = 0;
    connect(&client, &UpdateClient::StatusChanged, this, [&](const UpdateStatus& status) {
        if (status.state == UpdateState::Available) ++available;
        if (status.state != UpdateState::Checking || status.availableVersion.isEmpty() || cancelled) return;
        cancelled = true;
        QVERIFY(client.IsBusy()); QVERIFY(!client.Status().canDownload); QVERIFY(!client.Check()); QVERIFY(!client.Download());
        client.Cancel();
        QVERIFY(client.IsBusy()); QVERIFY(!client.Check());
        QTimer::singleShot(0, &client, [&] { eventLoopContinued = true; });
    });
    QVERIFY(client.Check()); QTRY_VERIFY_WITH_TIMEOUT(client.Status().state == UpdateState::Cancelled && !client.IsBusy(), 3000);
    QVERIFY(cancelled); QTRY_VERIFY(eventLoopContinued); QCOMPARE(available, 0); QCOMPARE(server.requests.size(), 1);
    QVERIFY(!client.Prepared()); QVERIFY(!client.Download());
    QVERIFY(client.Check()); QTRY_VERIFY_WITH_TIMEOUT(client.Status().state == UpdateState::Available, 3000);
    QVERIFY(client.Status().canDownload); QCOMPARE(available, 1); QCOMPARE(server.requests.size(), 2);
}

void UpdateClientTests::SwitchingChannelsDuringInstalledBaselineCheckDiscardsTheOldRelease() {
    QTemporaryDir directory; QVERIFY(directory.isValid()); Bundle stable, beta(QStringLiteral("0.3.0-beta.1"));
    Server server; QVERIFY(server.Start()); server.Publish(stable);
    server.routes["/repos/fixture/editor/releases?per_page=100"] = QJsonDocument(QJsonArray{
        QJsonDocument::fromJson(server.Release(beta, QStringLiteral("0.3.0-beta.1"), true)).object()}).toJson();
    const auto install = CreateSignedInstallation(directory.filePath("install"));
    QSettings settings(directory.filePath("preferences.ini"), QSettings::IniFormat);
    UpdateClient client(server.Source(stable, directory.filePath("downloads"), install), settings);
    bool switched = false; int available = 0;
    connect(&client, &UpdateClient::StatusChanged, this, [&](const UpdateStatus& status) {
        if (status.state == UpdateState::Available) ++available;
        if (status.state != UpdateState::Checking || status.availableVersion.isEmpty() || switched) return;
        switched = true;
        QVERIFY(!client.SetChannel(War3Map::Update::UpdateChannel::Beta)); // 独立渠道按钮在忙时禁用。
        // 设置页已提交的事务仍可在检查期间变更渠道，旧结果必须丢弃。
        auto preferences = client.Preferences(); preferences.channel = War3Map::Update::UpdateChannel::Beta;
        QVERIFY(UpdateClient::WritePreferences(settings, preferences)); settings.sync();
        QCOMPARE(settings.status(), QSettings::NoError);
        QVERIFY(client.ApplyCommittedPreferences(preferences));
        QVERIFY(!client.Check()); QVERIFY(!client.Download());
    });
    QVERIFY(client.Check()); QTRY_VERIFY_WITH_TIMEOUT(switched && !client.IsBusy(), 3000);
    QCOMPARE(client.Status().state, UpdateState::Idle); QCOMPARE(available, 0);
    QCOMPARE(client.Status().channel, War3Map::Update::UpdateChannel::Beta); QVERIFY(client.Status().availableVersion.isEmpty());
    QVERIFY(!client.Status().canDownload); QVERIFY(!client.Prepared());
    QVERIFY(client.Check()); QTRY_VERIFY_WITH_TIMEOUT(client.Status().state == UpdateState::Available, 3000);
    QCOMPARE(client.Status().availableVersion, QStringLiteral("0.3.0-beta.1"));
    QCOMPARE(client.Status().channel, War3Map::Update::UpdateChannel::Beta); QVERIFY(client.Status().canDownload);
    QCOMPARE(available, 1); QCOMPARE(server.requests.size(), 2);
}

void UpdateClientTests::DestroyingClientFromInstalledBaselineStatusDoesNotStartAnOrphanWorker() {
    QTemporaryDir directory; QVERIFY(directory.isValid()); Bundle bundle; Server server; QVERIFY(server.Start()); server.Publish(bundle);
    const auto install = CreateSignedInstallation(directory.filePath("install"));
    QSettings settings(directory.filePath("preferences.ini"), QSettings::IniFormat);
    auto client = std::make_unique<UpdateClient>(server.Source(bundle, directory.filePath("downloads"), install), settings);
    bool destroyedAtBaseline = false; int available = 0;
    connect(client.get(), &UpdateClient::StatusChanged, this, [&](const UpdateStatus& status) {
        if (status.state == UpdateState::Available) ++available;
        if (status.state != UpdateState::Checking || status.availableVersion.isEmpty()) return;
        destroyedAtBaseline = true; client.reset();
    });
    QVERIFY(client->Check()); QTRY_VERIFY_WITH_TIMEOUT(destroyedAtBaseline, 3000);
    QVERIFY(!client); QCOMPARE(available, 0); QCOMPARE(server.requests.size(), 1);
    // 同一安装目录还能重新验证；上一个客户端没有残留的状态回调或安装操作。
    UpdateClient next(server.Source(bundle, directory.filePath("downloads"), install), settings);
    QVERIFY(next.Check()); QTRY_VERIFY_WITH_TIMEOUT(next.Status().state == UpdateState::Available, 3000);
    QVERIFY(next.Status().canDownload); QCOMPARE(server.requests.size(), 2);
}

void UpdateClientTests::DestroyingClientFromChannelCancellationStopsApplyingPreferences() {
    QTemporaryDir directory; QVERIFY(directory.isValid()); Bundle bundle; Server server; QVERIFY(server.Start()); server.Publish(bundle);
    const auto install = CreateSignedInstallation(directory.filePath("install"));
    QSettings settings(directory.filePath("preferences.ini"), QSettings::IniFormat);
    auto client = std::make_unique<UpdateClient>(server.Source(bundle, directory.filePath("downloads"), install), settings);
    bool switching = false, destroyedFromCancel = false, preferenceCallReturned = false;
    int available = 0;
    connect(client.get(), &UpdateClient::StatusChanged, this, [&](const UpdateStatus& status) {
        if (status.state == UpdateState::Available) ++available;
        if (status.state != UpdateState::Checking || status.availableVersion.isEmpty()) return;
        if (switching) {
            // ApplyCommittedPreferences → Cancel → 同步取消提示；外层调用仍在栈上。
            destroyedFromCancel = true; client.reset();
            return;
        }
        switching = true;
        auto preferences = client->Preferences(); preferences.channel = War3Map::Update::UpdateChannel::Beta;
        QVERIFY(UpdateClient::WritePreferences(settings, preferences)); settings.sync();
        QCOMPARE(settings.status(), QSettings::NoError);
        QVERIFY(client->ApplyCommittedPreferences(preferences));
        preferenceCallReturned = true;
    });
    QVERIFY(client->Check()); QTRY_VERIFY_WITH_TIMEOUT(destroyedFromCancel && preferenceCallReturned, 3000);
    QVERIFY(!client); QCOMPARE(available, 0); QCOMPARE(server.requests.size(), 1);
    // 用独立设置重新核对同一原创安装树，旧客户端不能留下待启动线程或迟到状态结果。
    QSettings nextSettings(directory.filePath("next-preferences.ini"), QSettings::IniFormat);
    UpdateClient next(server.Source(bundle, directory.filePath("downloads"), install), nextSettings);
    QVERIFY(next.Check()); QTRY_VERIFY_WITH_TIMEOUT(next.Status().state == UpdateState::Available, 3000);
    QVERIFY(next.Status().canDownload); QCOMPARE(server.requests.size(), 2);
}

void UpdateClientTests::DestroyingClientFromCommittedChannelIdleStopsPreferencesNotification() {
    QTemporaryDir directory; QVERIFY(directory.isValid()); Bundle bundle; Server server; QVERIFY(server.Start());
    QSettings settings(directory.filePath("preferences.ini"), QSettings::IniFormat);
    auto client = std::make_unique<UpdateClient>(server.Source(bundle, directory.filePath("downloads"), CreateSignedInstallation(directory.filePath("install"))), settings);
    QSignalSpy changes(client.get(), &UpdateClient::PreferencesChanged);
    bool destroyedAtIdle = false;
    connect(client.get(), &UpdateClient::StatusChanged, this, [&](const UpdateStatus& status) {
        if (status.state != UpdateState::Idle || status.channel != War3Map::Update::UpdateChannel::Beta) return;
        destroyedAtIdle = true; client.reset();
    });
    auto preferences = client->Preferences(); preferences.channel = War3Map::Update::UpdateChannel::Beta;
    QVERIFY(UpdateClient::WritePreferences(settings, preferences)); settings.sync(); QCOMPARE(settings.status(), QSettings::NoError);
    QVERIFY(client->ApplyCommittedPreferences(preferences));
    QVERIFY(destroyedAtIdle); QVERIFY(!client); QCOMPARE(changes.count(), 0); QVERIFY(server.requests.empty());
}

QTEST_GUILESS_MAIN(UpdateClientTests)
#include "UpdateClientTests.moc"
