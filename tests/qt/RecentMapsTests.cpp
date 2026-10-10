#include "RecentMapsStore.h"
#include "RecentMapsList.h"

#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLockFile>
#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>

#include <algorithm>
#include <iostream>
#include <string>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using War3Map::QtProject::RecentMapsStore;
using War3Map::QtProject::RecentMapEntry;
using War3Map::Shell::RecentMapsList;
using War3Map::Shell::RecentMapListEntry;

namespace {
bool Write(const QString& path, const QByteArray& bytes = QByteArrayLiteral("original MRU fixture")) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
QByteArray Read(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}
QDateTime Time(int seconds) {
    return QDateTime::fromString(QStringLiteral("2026-10-06T00:00:00.000Z"), Qt::ISODateWithMs).addSecs(seconds);
}
bool Contains(const QVector<RecentMapEntry>& entries, const QString& path) {
    const auto normalized = QFileInfo(path).canonicalFilePath();
    const auto expected = normalized.isEmpty() ? QDir::cleanPath(path) : normalized;
    return std::any_of(entries.begin(), entries.end(), [&](const auto& entry) { return entry.path == expected; });
}
QVector<RecentMapListEntry> ViewEntries(const QVector<RecentMapEntry>& entries) {
    QVector<RecentMapListEntry> result;
    for (const auto& entry : entries) result.push_back({entry.path, entry.lastOpenedUtc, entry.pinned, entry.available});
    return result;
}
void HiddenProcess(QProcess& process) {
#ifdef Q_OS_WIN
    process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* arguments) {
        arguments->flags |= CREATE_NO_WINDOW;
    });
#else
    Q_UNUSED(process);
#endif
}
struct ChildLifetime {
    QProcess& process;
    ~ChildLifetime() {
        if (process.state() == QProcess::NotRunning) return;
        process.write("release\n");
        process.closeWriteChannel();
        if (!process.waitForFinished(2000)) { process.kill(); process.waitForFinished(2000); }
    }
};
} // namespace

class RecentMapsTests final : public QObject {
    Q_OBJECT
private slots:
    void RoundTripAndReadDoesNotCreateStorage();
    void TenTotalPinnedEvictionAndAllPinned();
    void ActualSecondProcessPreservesPinnedHistory();
    void ActualLockContentionPreservesFile();
    void InvalidOrDuplicateJsonIsNotOverwritten();
    void CanonicalAndWindowsCaseIdentity();
    void MissingRowsAllowPinAndRemoveWithoutOpen();
    void PlainLabelsRefreshAndCompactLayout();
};

void RecentMapsTests::RoundTripAndReadDoesNotCreateStorage() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto directory = temporary.path() + QStringLiteral("/private-user-data");
    RecentMapsStore store(directory, 0);
    const auto absent = store.Read();
    QVERIFY(absent.succeeded);
    QVERIFY(absent.entries.isEmpty());
    QVERIFY(!QFileInfo::exists(directory));
    const auto source = temporary.path() + QStringLiteral("/中文地图 & original.w3x");
    QVERIFY(Write(source));
    const auto original = Read(source);
    const auto opened = store.RecordOpened(source, Time(1));
    QVERIFY2(opened.succeeded, qPrintable(opened.message));
    QVERIFY(opened.recorded && opened.changed);
    QCOMPARE(opened.entries.size(), 1);
    const auto result = RecentMapsStore(directory).Read();
    QVERIFY(result.succeeded);
    QCOMPARE(result.entries.size(), 1);
    QCOMPARE(result.entries.front().path, QFileInfo(source).canonicalFilePath());
    QCOMPARE(result.entries.front().lastOpenedUtc, Time(1));
    QVERIFY(!result.entries.front().pinned);
    QVERIFY(result.entries.front().available);
    QCOMPARE(Read(source), original);
    const auto maps = QJsonDocument::fromJson(Read(store.FilePath())).object().value(QStringLiteral("maps")).toArray();
    QVERIFY(!maps.first().toObject().contains(QStringLiteral("available")));
    QVERIFY(!store.SetPinned(temporary.path() + QStringLiteral("/unknown.w3x"), true).succeeded);
    QVERIFY(!store.Remove(temporary.path() + QStringLiteral("/unknown.w3x")).succeeded);
    QVERIFY(!RecentMapsStore(QStringLiteral("relative/path")).Read().succeeded);
    QVERIFY(!store.RecordOpened(QStringLiteral("relative.w3x")).succeeded);
}

void RecentMapsTests::TenTotalPinnedEvictionAndAllPinned() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    RecentMapsStore store(temporary.path() + QStringLiteral("/data"), 0);
    QVector<QString> paths;
    for (int index = 0; index < 12; ++index) {
        paths.push_back(temporary.path() + QStringLiteral("/map-%1.w3x").arg(index));
        QVERIFY(Write(paths.back()));
    }
    QVERIFY(store.RecordOpened(paths[0], Time(0)).succeeded);
    QVERIFY(store.SetPinned(paths[0], true).succeeded);
    for (int index = 1; index <= 10; ++index) QVERIFY(store.RecordOpened(paths[index], Time(index)).succeeded);
    auto result = store.Read();
    QCOMPARE(result.entries.size(), 10);
    QVERIFY(result.entries.front().pinned);
    QVERIFY(Contains(result.entries, paths[0]));
    QVERIFY(!Contains(result.entries, paths[1]));
    QCOMPARE(result.entries[1].path, QFileInfo(paths[10]).canonicalFilePath());
    for (const auto& entry : result.entries) QVERIFY(store.SetPinned(entry.path, true).succeeded);
    const auto before = Read(store.FilePath());
    const auto blocked = store.RecordOpened(paths[11], Time(30));
    QVERIFY(blocked.succeeded);
    QVERIFY(!blocked.changed && !blocked.recorded);
    QCOMPARE(blocked.entries.size(), 10);
    QVERIFY(!blocked.message.isEmpty());
    QCOMPARE(Read(store.FilePath()), before);
    QVERIFY(store.Remove(paths[0]).succeeded);
    const auto admitted = store.RecordOpened(paths[11], Time(31));
    QVERIFY(admitted.succeeded && admitted.recorded && admitted.changed);
    QCOMPARE(admitted.entries.size(), 10);
    QVERIFY(store.SetPinned(paths[2], false).succeeded);
    const auto unpinned = store.Read();
    const auto found = std::find_if(unpinned.entries.begin(), unpinned.entries.end(), [&](const auto& entry) { return entry.path == QFileInfo(paths[2]).canonicalFilePath(); });
    QVERIFY(found != unpinned.entries.end());
    QVERIFY(!found->pinned);
}

void RecentMapsTests::ActualSecondProcessPreservesPinnedHistory() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto directory = temporary.path() + QStringLiteral("/data");
    const auto a = temporary.path() + QStringLiteral("/a.w3x");
    const auto b = temporary.path() + QStringLiteral("/b.w3x");
    QVERIFY(Write(a) && Write(b));
    RecentMapsStore first(directory, 0);
    QVERIFY(first.Read().entries.isEmpty());
    QProcess child;
    HiddenProcess(child);
    ChildLifetime lifetime{child};
    child.start(QCoreApplication::applicationFilePath(), {QStringLiteral("--record-and-pin"), directory, b});
    QVERIFY(child.waitForStarted(3000));
    QVERIFY(child.waitForFinished(5000));
    QCOMPARE(child.exitStatus(), QProcess::NormalExit);
    QCOMPARE(child.exitCode(), 0);
    QVERIFY(child.readAllStandardOutput().contains("RECORDED"));
    const auto result = first.RecordOpened(a, Time(1));
    QVERIFY2(result.succeeded, qPrintable(result.message));
    QCOMPARE(result.entries.size(), 2);
    QCOMPARE(result.entries.front().path, QFileInfo(b).canonicalFilePath());
    QVERIFY(result.entries.front().pinned);
    RecentMapsStore second(directory, 0);
    QVERIFY(second.Remove(a).succeeded);
    QVERIFY(first.SetPinned(b, false).succeeded);
    const auto reopened = second.Read();
    QCOMPARE(reopened.entries.size(), 1);
    QVERIFY(!reopened.entries.front().pinned);
    QCOMPARE(reopened.entries.front().path, QFileInfo(b).canonicalFilePath());
}

void RecentMapsTests::ActualLockContentionPreservesFile() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto source = temporary.path() + QStringLiteral("/a.w3x");
    QVERIFY(Write(source));
    RecentMapsStore store(temporary.path() + QStringLiteral("/data"), 0);
    QVERIFY(store.RecordOpened(source, Time(1)).succeeded);
    const auto before = Read(store.FilePath());
    QProcess child;
    HiddenProcess(child);
    ChildLifetime lifetime{child};
    child.start(QCoreApplication::applicationFilePath(), {QStringLiteral("--hold-lock"), store.LockPath()});
    QVERIFY(child.waitForStarted(3000));
    QVERIFY(child.waitForReadyRead(3000));
    QVERIFY(child.readAllStandardOutput().contains("LOCKED"));
    const auto locked = store.SetPinned(source, true);
    QVERIFY(!locked.succeeded);
    QVERIFY(!locked.message.isEmpty());
    QCOMPARE(Read(store.FilePath()), before);
    child.write("release\n");
    child.closeWriteChannel();
    QVERIFY(child.waitForFinished(3000));
    QCOMPARE(child.exitCode(), 0);
    QVERIFY(store.SetPinned(source, true).succeeded);
    QVERIFY(store.Read().entries.front().pinned);
}

void RecentMapsTests::InvalidOrDuplicateJsonIsNotOverwritten() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto source = temporary.path() + QStringLiteral("/a.w3x");
    QVERIFY(Write(source));
    RecentMapsStore store(temporary.path() + QStringLiteral("/data"), 0);
    QVERIFY(store.RecordOpened(source, Time(1)).succeeded);
    const QJsonObject entry{{QStringLiteral("path"), source}, {QStringLiteral("lastOpenedUtc"), Time(1).toString(Qt::ISODateWithMs)}, {QStringLiteral("pinned"), false}};
    const auto duplicate = QJsonDocument(QJsonObject{{QStringLiteral("schema"), 1}, {QStringLiteral("maps"), QJsonArray{entry, entry}}}).toJson();
    const QVector<QByteArray> invalid{QByteArrayLiteral("{invalid"), QByteArrayLiteral("{}"),
        QByteArrayLiteral("{\"schema\":2,\"maps\":[]}"), duplicate,
        QByteArray(int(RecentMapsStore::kMaximumJsonBytes + 1), ' ')};
    for (const auto& bytes : invalid) {
        QVERIFY(Write(store.FilePath(), bytes));
        const auto hash = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
        const auto read = store.Read();
        QVERIFY(!read.succeeded);
        QVERIFY(read.entries.isEmpty());
        QVERIFY(!store.RecordOpened(source, Time(2)).succeeded);
        QVERIFY(!store.SetPinned(source, true).succeeded);
        QVERIFY(!store.Remove(source).succeeded);
        QCOMPARE(QCryptographicHash::hash(Read(store.FilePath()), QCryptographicHash::Sha256), hash);
    }
}

void RecentMapsTests::CanonicalAndWindowsCaseIdentity() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QVERIFY(QDir(temporary.path()).mkdir(QStringLiteral("maps")));
    const auto path = temporary.path() + QStringLiteral("/maps/Map.w3x");
    QVERIFY(Write(path));
    RecentMapsStore store(temporary.path() + QStringLiteral("/data"), 0);
    QVERIFY(store.RecordOpened(path, Time(1)).succeeded);
    const auto alias = temporary.path() + QStringLiteral("/maps/../maps/Map.w3x");
    QVERIFY(store.RecordOpened(alias, Time(2)).succeeded);
#ifdef Q_OS_WIN
    QVERIFY(store.RecordOpened(alias.toUpper(), Time(3)).succeeded);
#endif
    auto result = store.Read();
    QCOMPARE(result.entries.size(), 1);
    QCOMPARE(result.entries.front().path, QFileInfo(path).canonicalFilePath());
    QVERIFY(store.SetPinned(alias, true).succeeded);
    QVERIFY(store.Read().entries.front().pinned);
}

void RecentMapsTests::MissingRowsAllowPinAndRemoveWithoutOpen() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto source = temporary.path() + QStringLiteral("/missing.w3x");
    QVERIFY(Write(source));
    RecentMapsStore store(temporary.path() + QStringLiteral("/data"), 0);
    QVERIFY(store.RecordOpened(source, Time(1)).succeeded);
    QVERIFY(QFile::remove(source));
    const auto result = store.Read();
    QVERIFY(result.succeeded && !result.entries.front().available);
    RecentMapsList list;
    QVERIFY(list.SetEntries(ViewEntries(result.entries)));
    list.resize(320, 160); list.show(); QApplication::processEvents();
    auto* open = list.findChild<QToolButton*>(QStringLiteral("recentMapOpen"));
    auto* pin = list.findChild<QToolButton*>(QStringLiteral("recentMapPin"));
    auto* remove = list.findChild<QToolButton*>(QStringLiteral("recentMapRemove"));
    QVERIFY(open && pin && remove);
    QVERIFY(!open->isEnabled());
    QVERIFY(pin->isEnabled() && remove->isEnabled());
    QSignalSpy opened(&list, &RecentMapsList::OpenRequested);
    QSignalSpy pinned(&list, &RecentMapsList::PinRequested);
    QSignalSpy removed(&list, &RecentMapsList::RemoveRequested);
    connect(&list, &RecentMapsList::PinRequested, &list, [&](const QString& path, bool value) { QVERIFY(store.SetPinned(path, value).succeeded); });
    connect(&list, &RecentMapsList::RemoveRequested, &list, [&](const QString& path) { QVERIFY(store.Remove(path).succeeded); });
    open->click(); QTest::keyClick(open, Qt::Key_Space);
    QCOMPARE(opened.count(), 0);
    QTest::mouseClick(pin, Qt::LeftButton);
    QCOMPARE(pinned.count(), 1);
    QCOMPARE(pinned.front()[0].toString(), result.entries.front().path);
    QCOMPARE(pinned.front()[1].toBool(), true);
    QVERIFY(store.Read().entries.front().pinned);
    QTest::mouseClick(remove, Qt::LeftButton);
    QCOMPARE(removed.count(), 1);
    QVERIFY(store.Read().entries.isEmpty());
}

void RecentMapsTests::PlainLabelsRefreshAndCompactLayout() {
    RecentMapsList list;
    const QString path = QStringLiteral("C:/maps/<b>&map.w3x");
    QVERIFY(list.SetEntries({{path, Time(1), true, true}}));
    list.SetStatus(QStringLiteral("<b>status</b>"));
    list.resize(240, 180); list.show(); QApplication::processEvents();
    auto* open = list.findChild<QToolButton*>(QStringLiteral("recentMapOpen"));
    auto* label = list.findChild<QLabel*>(QStringLiteral("recentMapPath"));
    auto* status = list.findChild<QLabel*>(QStringLiteral("recentMapsStatus"));
    QVERIFY(open && label && status);
    QCOMPARE(open->accessibleName(), QStringLiteral("<b>&map.w3x"));
    QVERIFY(open->toolTip().contains(QStringLiteral("&lt;b&gt;")));
    QCOMPARE(label->textFormat(), Qt::PlainText);
    QCOMPARE(status->textFormat(), Qt::PlainText);
    QCOMPARE(status->text(), QStringLiteral("<b>status</b>"));
    QSignalSpy opened(&list, &RecentMapsList::OpenRequested);
    QTest::mouseClick(open, Qt::LeftButton);
    QCOMPARE(opened.count(), 1);
    QCOMPARE(opened.front()[0].toString(), path);
    connect(&list, &RecentMapsList::RemoveRequested, &list, [&](const QString&) { QVERIFY(list.SetEntries({})); });
    auto* remove = list.findChild<QToolButton*>(QStringLiteral("recentMapRemove"));
    QVERIFY(remove);
    QTest::mouseClick(remove, Qt::LeftButton); // 同步刷新不会销毁正在发送 clicked 的按钮。
    QVERIFY(list.findChildren<QToolButton*>(QStringLiteral("recentMapOpen")).isEmpty());
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QVector<RecentMapListEntry> tooMany(11, {path, Time(1), false, false});
    QVERIFY(!list.SetEntries(tooMany));
    QVERIFY(list.findChildren<QToolButton*>(QStringLiteral("recentMapOpen")).isEmpty());
    QVERIFY(list.width() <= 240);
}

int main(int argc, char** argv) {
    if (argc == 3 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("--hold-lock")) {
        QCoreApplication application(argc, argv);
        QLockFile lock(QString::fromLocal8Bit(argv[2]));
        lock.setStaleLockTime(0);
        if (!lock.tryLock(0)) return 2;
        std::cout << "LOCKED\n" << std::flush;
        std::string line;
        std::getline(std::cin, line);
        return 0;
    }
    if (argc == 4 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("--record-and-pin")) {
        QCoreApplication application(argc, argv);
        RecentMapsStore store(QString::fromLocal8Bit(argv[2]), 1000);
        const auto path = QString::fromLocal8Bit(argv[3]);
        if (!store.RecordOpened(path).succeeded || !store.SetPinned(path, true).succeeded) return 3;
        std::cout << "RECORDED\n" << std::flush;
        return 0;
    }
    QApplication application(argc, argv);
    RecentMapsTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "RecentMapsTests.moc"
