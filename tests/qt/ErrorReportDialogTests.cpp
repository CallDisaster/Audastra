#include "ErrorReportDialog.h"
#include "ErrorReportEnvironment.h"

#include "War3MapCore/IO/FileBytes.h"

#include <QApplication>
#include <QPlainTextEdit>
#include <QTemporaryDir>
#include <QtTest>

#include <miniz.h>
#include <windows.h>

#include <algorithm>
#include <set>
#include <span>

using namespace War3Map;

namespace {
std::string ZipMember(const QString& path, const char* name) {
    const auto bytes = IO::ReadFileBytes(std::filesystem::path(path.toStdWString()), 64ull * 1024 * 1024);
    if (!bytes.Succeeded()) return "<unreadable>";
    mz_zip_archive archive{};
    if (!mz_zip_reader_init_mem(&archive, bytes.value->data(), bytes.value->size(), 0)) return "<not zip>";
    std::size_t size = 0;
    void* heap = mz_zip_reader_extract_file_to_heap(&archive, name, &size, 0);
    std::string result = heap ? std::string(static_cast<const char*>(heap), size) : std::string("<missing>");
    if (heap) mz_free(heap);
    mz_zip_reader_end(&archive);
    return result;
}
} // namespace

class ErrorReportDialogTests final : public QObject {
    Q_OBJECT
private slots:
    void EnvironmentNamesHardwareAndSystem() {
        const auto sections = QtFeedback::CollectErrorReportEnvironment(QStringLiteral("fixture-stamp"));
        std::set<std::string> titles;
        for (const auto& section : sections) {
            titles.insert(section.title);
            QVERIFY2(!section.rows.empty(), section.title.c_str());
            for (const auto& [name, value] : section.rows) QVERIFY2(!value.empty(), name.c_str());
        }
        for (const auto* title : {"程序", "系统", "处理器", "内存", "显卡", "显示器", "磁盘"})
            QVERIFY2(titles.contains(title), title);
        const auto& program = sections.front().rows;
        QVERIFY(std::any_of(program.begin(), program.end(), [](const auto& row) { return row.first == "构建" && row.second == "fixture-stamp"; }));
        QVERIFY(std::any_of(program.begin(), program.end(), [](const auto& row) { return row.first == "已运行"; }));
        wchar_t system[MAX_PATH]{};
        QVERIFY(GetSystemDirectoryW(system, MAX_PATH) > 0);
        QVERIFY(!QtFeedback::FileVersion(std::filesystem::path(system) / L"kernel32.dll").isEmpty());
        QVERIFY(QtFeedback::FileVersion(std::filesystem::path(system) / L"absent-audastra-fixture.dll").isEmpty());
    }

    void CrashDumpsNewestFirst() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const std::filesystem::path root(directory.path().toStdWString());
        for (const auto* name : {L"crash-20260101-000000-1.dmp", L"crash-20260301-000000-1.dmp", L"crash-20260201-000000-1.dmp", L"other.dmp"})
            QVERIFY(IO::WriteFileBytes(root / name, std::vector<std::byte>(4)).Succeeded());
        const auto dumps = QtFeedback::RecentCrashDumps(root, 2);
        QCOMPARE(dumps.size(), std::size_t{2});
        QCOMPARE(QString::fromStdWString(dumps[0].filename().wstring()), QStringLiteral("crash-20260301-000000-1.dmp"));
        QCOMPARE(QString::fromStdWString(dumps[1].filename().wstring()), QStringLiteral("crash-20260201-000000-1.dmp"));
    }

    void PreviewAndExportCarryDescriptionAndLogs() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const std::filesystem::path root(directory.path().toStdWString());
        const auto log = root / "fixture.log";
        const std::string text = "保存 开始\r\n保存 失败：写入被拒绝\r\n";
        QVERIFY(IO::WriteFileBytes(log, std::span(reinterpret_cast<const std::byte*>(text.data()), text.size())).Succeeded());
        int collected = 0;
        QtFeedback::ErrorReportDialog dialog([&]() {
            ++collected;
            Feedback::ErrorReportInput input;
            input.sections.push_back({"程序", {{"版本", "fixture"}}});
            input.diagnostics.push_back("错误 · 地图 · SAVE_FAILED · 写入被拒绝");
            input.files.push_back({"logs/current.log", log, true});
            return input;
        }, directory.path());
        QVERIFY(collected == 1);
        auto* description = dialog.findChild<QPlainTextEdit*>(QStringLiteral("errorReportDescription"));
        auto* preview = dialog.findChild<QPlainTextEdit*>(QStringLiteral("errorReportPreview"));
        QVERIFY(description && preview);
        description->setPlainText(QStringLiteral("点保存后提示写入被拒绝"));
        QVERIFY(preview->toPlainText().contains(QStringLiteral("点保存后提示写入被拒绝")));
        QVERIFY(preview->toPlainText().contains(QStringLiteral("版本：fixture")));
        QVERIFY(preview->toPlainText().contains(QStringLiteral("SAVE_FAILED")));
        QVERIFY(preview->toPlainText().contains(QStringLiteral("logs/current.log")));

        const auto target = QDir(directory.path()).filePath(QStringLiteral("报告.zip"));
        QString error;
        QVERIFY2(dialog.ExportTo(QDir::toNativeSeparators(target), &error), qPrintable(error));
        QVERIFY(collected == 2);
        QVERIFY(QString::fromStdString(ZipMember(target, "report.txt")).contains(QStringLiteral("点保存后提示写入被拒绝")));
        QCOMPARE(ZipMember(target, "logs/current.log"), text);
        QVERIFY(!dialog.ExportTo(QStringLiteral("relative.zip"), &error));
        QVERIFY(!error.isEmpty());
    }
};

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    ErrorReportDialogTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "ErrorReportDialogTests.moc"
