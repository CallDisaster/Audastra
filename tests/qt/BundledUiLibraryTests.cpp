#include "BundledUiLibrary.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

using namespace War3Map::QtProject;

// 自带函数库的安装：只写包目录，不碰 config；内容不同才写，写后逐字节一致。
class BundledUiLibraryTests final : public QObject {
    Q_OBJECT
private slots:
    void InstallsIntoDataRootWithoutTouchingConfig();
};

void BundledUiLibraryTests::InstallsIntoDataRootWithoutTouchingConfig() {
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const std::filesystem::path dataRoot(root.path().toStdWString());
    QCOMPARE(CheckBundledUiLibrary(dataRoot).error.isEmpty(), false);  // 没有 ui\config
    QVERIFY(!InstallBundledUiLibrary(dataRoot).isEmpty());

    QVERIFY(QDir(root.path()).mkpath(QStringLiteral("ui")));
    QFile config(root.filePath(QStringLiteral("ui/config")));
    QVERIFY(config.open(QIODevice::WriteOnly));
    config.write("base\r\n");
    config.close();

    auto check = CheckBundledUiLibrary(dataRoot);
    QVERIFY2(check.error.isEmpty(), qPrintable(check.error));
    QCOMPARE(check.state, BundledUiLibraryState::Missing);
    QCOMPARE(check.target, dataRoot / "ui" / "AudastraUI");

    const QString installed = InstallBundledUiLibrary(dataRoot);
    QVERIFY2(installed.isEmpty(), qPrintable(installed));
    QCOMPARE(CheckBundledUiLibrary(dataRoot).state, BundledUiLibraryState::Same);
    for (const auto* name : {"ui/AudastraUI/define.txt", "ui/AudastraUI/action.txt", "ui/AudastraUI/call.txt", "ui/AudastraUI/event.txt",
             "ui/AudastraUI/jass/AudastraUI.cfg", "ui/AudastraUI/jass/AudastraUI.j"}) {
        QVERIFY2(QFile::exists(root.filePath(QString::fromLatin1(name))), name);
    }
    QVERIFY(config.open(QIODevice::ReadOnly));
    QCOMPARE(config.readAll(), QByteArray("base\r\n"));
    config.close();

    // 作者改过的文件算作不同；再次安装恢复成自带内容，包目录里的其他文件不动。
    QFile action(root.filePath(QStringLiteral("ui/AudastraUI/action.txt")));
    QVERIFY(action.open(QIODevice::Append));
    action.write("\r\n[Extra]\r\n");
    action.close();
    QFile extra(root.filePath(QStringLiteral("ui/AudastraUI/notes.txt")));
    QVERIFY(extra.open(QIODevice::WriteOnly));
    extra.write("keep");
    extra.close();
    QCOMPARE(CheckBundledUiLibrary(dataRoot).state, BundledUiLibraryState::Different);
    QVERIFY(InstallBundledUiLibrary(dataRoot).isEmpty());
    QCOMPARE(CheckBundledUiLibrary(dataRoot).state, BundledUiLibraryState::Same);
    QVERIFY(QFile::exists(extra.fileName()));
}

QTEST_GUILESS_MAIN(BundledUiLibraryTests)
#include "BundledUiLibraryTests.moc"
