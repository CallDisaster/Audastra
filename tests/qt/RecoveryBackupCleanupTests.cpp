#include "RecoveryBackupCleanup.h"
#include "War3MapCore/Hash.h"

#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include <Windows.h>

namespace {
std::filesystem::path Path(const QString& value) { return value.toStdWString(); }
bool Write(const QString& name, const QByteArray& bytes) {
    QFile file(name);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
std::string Sha(const QString& name) {
    const auto sha = War3Map::Sha256File(Path(name));
    return sha.Succeeded() ? *sha.value : std::string{};
}
} // namespace

class RecoveryBackupCleanupTests : public QObject {
    Q_OBJECT
private slots:
    void ConfirmedOldCopyIsDeletedWhileCurrentStays();
    void ChangedCurrentOrOldCopyIsKept();
    void MissingCurrentAndForeignBackupAreKept();
    void HardLinkedBackupIsKept();
};

void RecoveryBackupCleanupTests::ConfirmedOldCopyIsDeletedWhileCurrentStays() {
    QTemporaryDir directory; QVERIFY(directory.isValid());
    const auto target = directory.filePath("recovery.forge");
    const auto backup = directory.filePath("recovery.forge.backup-1-2-3.forge");
    QVERIFY(Write(target, "new recovery")); QVERIFY(Write(backup, "old recovery"));
    const auto currentSha = Sha(target); const auto previousSha = Sha(backup);
    War3Map::ProjectSaveResult result{Path(target), currentSha, 1, Path(backup)};
    const auto cleanup = War3Map::QtProject::CleanupConfirmedRecoveryBackup(result, previousSha);
    QVERIFY(cleanup.diagnostics.empty()); QVERIFY(!QFile::exists(backup)); QCOMPARE(Sha(target), currentSha);
}

void RecoveryBackupCleanupTests::ChangedCurrentOrOldCopyIsKept() {
    QTemporaryDir directory; QVERIFY(directory.isValid());
    const auto target = directory.filePath("recovery.forge");
    const auto backup = directory.filePath("recovery.forge.backup-1-2-3.forge");
    QVERIFY(Write(target, "new recovery")); QVERIFY(Write(backup, "old recovery"));
    const auto currentSha = Sha(target); const auto previousSha = Sha(backup);
    War3Map::ProjectSaveResult result{Path(target), currentSha, 1, Path(backup)};
    QVERIFY(Write(target, "external changed current"));
    QVERIFY(!War3Map::QtProject::CleanupConfirmedRecoveryBackup(result, previousSha).diagnostics.empty());
    QVERIFY(QFile::exists(backup)); QCOMPARE(Sha(backup), previousSha);
    QVERIFY(Write(target, "new recovery")); QVERIFY(Write(backup, "external changed old"));
    const auto changedSha = Sha(backup);
    QVERIFY(!War3Map::QtProject::CleanupConfirmedRecoveryBackup(result, previousSha).diagnostics.empty());
    QCOMPARE(Sha(backup), changedSha); QCOMPARE(Sha(target), currentSha);
}

void RecoveryBackupCleanupTests::MissingCurrentAndForeignBackupAreKept() {
    QTemporaryDir directory; QVERIFY(directory.isValid());
    const auto target = directory.filePath("recovery.forge");
    const auto backup = directory.filePath("recovery.forge.backup-1-2-3.forge");
    QVERIFY(Write(backup, "old recovery")); const auto previousSha = Sha(backup);
    War3Map::ProjectSaveResult result{Path(target), previousSha, 1, Path(backup)};
    QVERIFY(!War3Map::QtProject::CleanupConfirmedRecoveryBackup(result, previousSha).diagnostics.empty());
    QCOMPARE(Sha(backup), previousSha);
    QVERIFY(Write(target, "new recovery")); result.sha256 = Sha(target);
    const auto foreign = directory.filePath("manual.forge.backup-1-2-3.forge");
    QVERIFY(Write(foreign, "old recovery")); result.backupPath = Path(foreign);
    QVERIFY(!War3Map::QtProject::CleanupConfirmedRecoveryBackup(result, previousSha).diagnostics.empty());
    QCOMPARE(Sha(foreign), previousSha); QCOMPARE(Sha(backup), previousSha);
}

void RecoveryBackupCleanupTests::HardLinkedBackupIsKept() {
    QTemporaryDir directory; QVERIFY(directory.isValid());
    const auto target = directory.filePath("recovery.forge");
    const auto backup = directory.filePath("recovery.forge.backup-1-2-3.forge");
    const auto alias = directory.filePath("author-copy.forge");
    QVERIFY(Write(target, "new recovery")); QVERIFY(Write(backup, "old recovery"));
    QVERIFY(CreateHardLinkW(Path(alias).c_str(), Path(backup).c_str(), nullptr));
    const auto previousSha = Sha(backup);
    War3Map::ProjectSaveResult result{Path(target), Sha(target), 1, Path(backup)};
    QVERIFY(!War3Map::QtProject::CleanupConfirmedRecoveryBackup(result, previousSha).diagnostics.empty());
    QCOMPARE(Sha(backup), previousSha); QCOMPARE(Sha(alias), previousSha);
}

QTEST_GUILESS_MAIN(RecoveryBackupCleanupTests)
#include "RecoveryBackupCleanupTests.moc"
