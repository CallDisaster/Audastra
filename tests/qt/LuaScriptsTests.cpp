#include "TriggerLuaScripts.h"
#include "TriggerModule.h"
#include "AssetsModule.h"
#include "AssetOwnedPayload.h"
#include "EditorProject.h"
#include "MapSaveJob.h"
#include "Theme/ThemeManager.h"
#include "Widgets/CodeView.h"
#include "Widgets/CodeProblemsView.h"
#include "Modules/ModuleHost.h"

#include "War3MapCore/Hash.h"
#include "War3MapCore/Workspace.h"
#include "War3MapCore/Lua/LuaSyntax.h"
#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/Mpq/MpqWriter.h"

#include <QApplication>
#include "Fonts/FontLoader.h"
#include <QCryptographicHash>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTextBlock>
#include <QTreeWidget>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QPushButton>
#include <QTabWidget>
#include <QStandardItemModel>
#include <QTemporaryDir>
#include <QTest>
#include <QTextCharFormat>
#include <QFontInfo>
#include <QFontMetricsF>
#include <QUndoStack>

#include <algorithm>

using namespace War3Map;
namespace {
std::vector<std::byte> Bytes(std::string_view text) {
    const auto* start = reinterpret_cast<const std::byte*>(text.data()); return {start, start + text.size()};
}
std::shared_ptr<const std::vector<std::byte>> Payload(std::string_view text) { return std::make_shared<const std::vector<std::byte>>(Bytes(text)); }
bool OpenMap(const QTemporaryDir& dir, QtProject::EditorProject& project) {
    const auto path = std::filesystem::path(dir.filePath("lua-assets.w3x").toStdWString());
    const std::vector<Mpq::MpqBuildFile> files{
        {"war3map.w3i", Bytes("synthetic"), false},
        {"Scripts\\Existing.lua", Bytes("local count = 1\r\nreturn count\r\n"), false},
        {"Scripts\\Legacy.lua", {std::byte{'-'}, std::byte{'-'}, std::byte{' '}, std::byte{0xD6}, std::byte{0xD0}, std::byte{'\n'}}, false},
        {"Scripts\\BomCrLf.lua", Bytes("\xEF\xBB\xBF" "local count = 1\r\nreturn count\r\n"), false},
        {"Scripts\\Unicode.lua", Bytes("local text = \"\xC2\xA0\xE2\x80\xA8\xE2\x80\xA9\"\r\nreturn text\r\n"), false},
    };
    if (!Mpq::WriteMpqArchive(path, files).Succeeded()) return false;
    auto archive = MapArchiveSession::Open(path); if (!archive.Succeeded()) return false;
    project.archiveSession_ = std::make_unique<MapArchiveSession>(std::move(*archive.value));
    project.sourceArchivePath_ = path; project.projectRoot_ = dir.path().toStdWString(); project.ResetEditTracking();
    return true;
}
std::string OwnedSourceText(const QtProject::OwnedAssetSource& source) { return {reinterpret_cast<const char*>(source.bytes->data()), source.bytes->size()}; }
bool HasDiagnostic(const QtProject::EditorProject& project, const QString& code) {
    for (int row = 0; row < project.diagnosticsModel_->rowCount(); ++row)
        if (project.diagnosticsModel_->index(row, 2).data().toString() == code) return true;
    return false;
}
class LuaHost final : public Shell::ModuleHost {
public:
    Shell::NavigationTarget lastTarget;
    int undoChanges = 0;
    void Navigate(const Shell::NavigationTarget& target) override { lastTarget = target; }
    void ActivateModule(const Shell::ModuleId&) override {}
    void ShowStatusMessage(const QString&, int) override {}
    void UndoStackChanged(const Shell::ModuleId&) override { ++undoChanges; }
};
}

class LuaScriptsTests final : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { UiKit::ThemeManager theme; theme.ApplyTheme("graphite"); }
    void OriginalLuaDiagnosticVisualAndProblemNavigation() {
        const auto directory = qEnvironmentVariable("WAR3_CODE_ERRORS_CAPTURE_DIR");
        if (directory.isEmpty()) QSKIP("Opt-in original Qt diagnostic capture; compile-only, no game execution.");
        QCOMPARE(QApplication::platformName(), QStringLiteral("offscreen"));
        bool dprValid = false;
        const auto expectedDpr = qEnvironmentVariable("WAR3_CODE_ERRORS_EXPECTED_DPR").toInt(&dprValid);
        QVERIFY(dprValid && (expectedDpr == 1 || expectedDpr == 2));
        const QFileInfo output(directory);
        QVERIFY(output.isAbsolute()); QVERIFY(!output.exists());
        QVERIFY(QFileInfo(output.absolutePath()).isDir());
        const auto fonts = UiKit::LoadFonts(UiKit::DefaultFontDirectory());
        QVERIFY(fonts.bundledUi); QVERIFY(fonts.bundledCode); QVERIFY(fonts.failedFiles.isEmpty());
        UiKit::ApplyApplicationFont(fonts);

        QTemporaryDir source; QVERIFY(source.isValid());
        QtProject::EditorProject project; QVERIFY(OpenMap(source, project));
        const auto sourceSha = Sha256File(project.sourceArchivePath_); QVERIFY(sourceSha.Succeeded());
        const auto assetBefore = project.CaptureAssets();
        const auto original = QtProject::ReadOwnedAsset(project, "Scripts\\Existing.lua"); QVERIFY(original.Succeeded());
        Modules::TriggerLuaScripts scripts(project); scripts.resize(1280, 780); scripts.show();
        QVERIFY(scripts.SelectScript("Scripts\\Existing.lua"));
        auto* editor = scripts.Editor(); QVERIFY(editor); QVERIFY(!editor->isReadOnly());
        const QFontInfo codeFont(editor->font());
        const QFontMetricsF codeMetrics(editor->font());
        QVERIFY(codeFont.fixedPitch());
        QVERIFY(fonts.codeFamilies.contains(codeFont.family()));
        QVERIFY(qAbs(codeMetrics.horizontalAdvance(QStringLiteral("iiiiiiii"))
            - codeMetrics.horizontalAdvance(QStringLiteral("WWWWWWWW"))) < 0.1);
        QVERIFY2(qAbs(editor->tabStopDistance() - codeMetrics.horizontalAdvance(QLatin1Char(' ')) * 4) < 0.1,
            qPrintable(QStringLiteral("actual=%1 expected=%2 resolved=%3").arg(editor->tabStopDistance())
                .arg(codeMetrics.horizontalAdvance(QLatin1Char(' ')) * 4).arg(codeFont.family())));
        const QString fixture = QStringLiteral(
            "-- 原创检查夹具：仅编译检查，不执行代码\n"
            "local function calculate(value)\n"
            "    local total = value + 1\n"
            ") -- 原创：此行故意含语法错误（不执行）\n"
            "    return total\n"
            "end\n"
            "\n"
            "return calculate(3)\n");
        editor->SetText(fixture); editor->RequestCheck();
        QTRY_VERIFY_WITH_TIMEOUT(!editor->IsLoading() && !editor->IsChecking() && editor->CheckResult().completed, 8000);
        const auto& checked = editor->CheckResult();
        QVERIFY(checked.syntaxChecked); QVERIFY(!checked.semanticChecked); QVERIFY(!checked.cancelled);
        QCOMPARE(checked.generation, editor->Generation());
        QCOMPARE(editor->toPlainText(), fixture); QVERIFY(scripts.HasPendingDraft());
        QCOMPARE(project.AssetUndoStack()->count(), 0);
        auto* problems = scripts.findChild<UiKit::CodeProblemsView*>("luaCodeProblems"); QVERIFY(problems);
        auto* issues = problems->findChild<QTreeWidget*>("codeIssues"); QVERIFY(issues);
        auto* gutter = editor->findChild<QWidget*>("codeGutter"); QVERIFY(gutter);
        QVERIFY(editor->GutterWidth() > 30); QVERIFY(gutter->isVisible());
        QVERIFY(problems->IssueCount() > 0); QVERIFY(problems->StatusText().contains(QStringLiteral("错误")));

        const auto error = std::find_if(checked.diagnostics.cbegin(), checked.diagnostics.cend(), [editor](const auto& item) {
            return item.severity == UiKit::CodeSeverity::Error && item.source == editor->SourceName() && item.line == 4;
        });
        QVERIFY(error != checked.diagnostics.cend());
        auto* row = static_cast<QTreeWidgetItem*>(nullptr);
        for (int index = 0; index < issues->topLevelItemCount(); ++index) {
            auto* item = issues->topLevelItem(index);
            if (item->data(0, Qt::UserRole).toString() == error->source
                && item->data(1, Qt::UserRole).toInt() == error->line
                && item->data(2, Qt::UserRole).toInt() == error->column) { row = item; break; }
        }
        QVERIFY(row);
        const auto markers = editor->extraSelections();
        QVERIFY(std::any_of(markers.cbegin(), markers.cend(), [](const auto& marker) {
            return marker.cursor.blockNumber() == 3 && marker.cursor.hasSelection()
                && marker.format.underlineStyle() == QTextCharFormat::WaveUnderline
                && marker.format.underlineColor().red() > marker.format.underlineColor().green();
        }));
        editor->GoToLine(1); issues->setCurrentItem(row); issues->scrollToItem(row);
        QCoreApplication::processEvents();
        QVERIFY(issues->isVisible()); QVERIFY(!issues->visualItemRect(row).isEmpty());
        const auto rowPoint = issues->visualItemRect(row).center();
        QTest::mouseClick(issues->viewport(), Qt::LeftButton, Qt::NoModifier, rowPoint);
        QSignalSpy navigated(problems, &UiKit::CodeProblemsView::LocationRequested);
        QTest::mouseDClick(issues->viewport(), Qt::LeftButton, Qt::NoModifier, rowPoint);
        QTest::mouseRelease(issues->viewport(), Qt::LeftButton, Qt::NoModifier, rowPoint);
        QTRY_COMPARE_WITH_TIMEOUT(navigated.count(), 1, 2000);
        QCOMPARE(navigated.front().at(0).toString(), error->source);
        QCOMPARE(navigated.front().at(1).toInt(), 4);
        QCOMPARE(editor->textCursor().blockNumber(), 3);
        QCoreApplication::processEvents();
        QVERIFY(editor->height() > 350); QVERIFY(problems->height() > 60);
        QVERIFY(qAbs(scripts.devicePixelRatioF() - expectedDpr) < 0.01);
        const auto screenshot = scripts.grab();
        QVERIFY(!screenshot.isNull()); QVERIFY(qAbs(screenshot.devicePixelRatio() - expectedDpr) < 0.01);
        QCOMPARE(screenshot.size(), QSize(scripts.width() * expectedDpr, scripts.height() * expectedDpr));
        QVERIFY(project.CaptureAssets() == assetBefore);
        const auto afterSourceSha = Sha256File(project.sourceArchivePath_); QVERIFY(afterSourceSha.Succeeded());
        QCOMPARE(*afterSourceSha.value, *sourceSha.value);
        const auto afterMember = QtProject::ReadOwnedAsset(project, "Scripts\\Existing.lua"); QVERIFY(afterMember.Succeeded());
        QCOMPARE(*afterMember.value->bytes, *original.value->bytes);

        QVERIFY(QDir().mkdir(directory));
        const auto name = QStringLiteral("c2-lua-errors-dpr%1.png").arg(expectedDpr);
        QFile image(QDir(directory).filePath(name)); QVERIFY(image.open(QIODevice::WriteOnly | QIODevice::NewOnly));
        QVERIFY(screenshot.save(&image, "PNG")); image.close();
        QJsonArray diagnostics;
        for (const auto& issue : checked.diagnostics) diagnostics.append(QJsonObject{
            {"source", issue.source}, {"code", issue.code}, {"message", issue.message},
            {"line", issue.line}, {"utf16_column", issue.column}, {"length", issue.length},
            {"error", issue.severity == UiKit::CodeSeverity::Error}});
        const QJsonObject receipt{
            {"evidence", "production Qt widget + actual Lua 5.3 compile-only diagnostic; not game/semantic proof"},
            {"qt_version", QString::fromLatin1(qVersion())}, {"platform", QApplication::platformName()},
            {"bundled_ui_font", fonts.bundledUi}, {"bundled_code_font", fonts.bundledCode},
            {"loaded_font_files", QJsonArray::fromStringList(fonts.loadedFiles)},
            {"ui_font_families", QJsonArray::fromStringList(fonts.uiFamilies)},
            {"code_font_families", QJsonArray::fromStringList(fonts.codeFamilies)},
            {"editor_font_requested_families", QJsonArray::fromStringList(editor->font().families())},
            {"editor_font_resolved_family", codeFont.family()}, {"editor_font_fixed_pitch", codeFont.fixedPitch()},
            {"editor_font_pixel_size", codeFont.pixelSize()}, {"tab_stop_spaces", 4},
            {"dpr", expectedDpr}, {"logical_width", scripts.width()}, {"logical_height", scripts.height()},
            {"pixel_width", screenshot.width()}, {"pixel_height", screenshot.height()},
            {"fixture_utf8_sha256", QString::fromLatin1(QCryptographicHash::hash(fixture.toUtf8(), QCryptographicHash::Sha256).toHex())},
            {"source_archive_sha256_before", QString::fromStdString(*sourceSha.value)},
            {"source_archive_sha256_after", QString::fromStdString(*afterSourceSha.value)},
            {"source_member_bytes_unchanged", true}, {"asset_state_unchanged", true}, {"asset_undo_count", 0},
            {"syntax_checked", checked.syntaxChecked}, {"semantic_checked", checked.semanticChecked},
            {"diagnostics", diagnostics}, {"problem_navigation_signals", static_cast<int>(navigated.count())},
            {"lua_location_precision", "line-only; marker begins at first character, not a compiler token column"},
            {"navigated_line", editor->textCursor().blockNumber() + 1}, {"screenshot", name}};
        QFile metadata(QDir(directory).filePath("capture.json")); QVERIFY(metadata.open(QIODevice::WriteOnly | QIODevice::NewOnly));
        const auto encoded = QJsonDocument(receipt).toJson(QJsonDocument::Indented);
        QCOMPARE(metadata.write(encoded), static_cast<qint64>(encoded.size())); metadata.close();
        scripts.DiscardPendingDraft(); QVERIFY(!scripts.HasPendingDraft());
        QCOMPARE(project.AssetUndoStack()->count(), 0); QVERIFY(project.CaptureAssets() == assetBefore);
    }

    void OwnedCreationUsesSameResourceQueueAndKeepsMapSaveClosed() {
        QTemporaryDir dir; QVERIFY(dir.isValid()); QtProject::EditorProject project; QVERIFY(OpenMap(dir, project));
        Modules::AssetsModule assets(project); assets.Widget();
        Modules::TriggerLuaScripts scripts(project); scripts.resize(1120, 700); scripts.show();
        const auto sourceSha = Sha256File(project.sourceArchivePath_); QVERIFY(sourceSha.Succeeded());
        const auto files = QDir(dir.path()).entryList(QDir::Files);
        QVERIFY(scripts.CreateScript("Lua\\demo.lua", "local value = 3\nreturn value\n"));
        QVERIFY(scripts.ScriptPaths().contains("Lua\\demo.lua")); QCOMPARE(scripts.SelectedPath(), QStringLiteral("Lua\\demo.lua"));
        QCOMPARE(project.pendingImports_.size(), std::size_t{1});
        const auto& pending = project.pendingImports_.front(); QVERIFY(pending.ownedPayload); QVERIFY(pending.diskPath.isEmpty()); QVERIFY(pending.runtimeLuaDraft);
        QCOMPARE(pending.size, static_cast<qint64>(pending.ownedPayload->size()));
        const auto source = QtProject::ReadOwnedAsset(project, "lua/demo.lua"); QVERIFY(source.Succeeded());
        QCOMPARE(OwnedSourceText(*source.value), std::string("local value = 3\nreturn value\n"));
        assets.OpenFolder("Lua"); QVERIFY(assets.ListedRows().join('\n').contains("demo.lua"));
        QCOMPARE(assets.UndoStack(), project.AssetUndoStack()); QCOMPARE(assets.UndoStack()->count(), 1);
        auto* entry = scripts.findChild<QPushButton*>("luaSetEntryScript"); QVERIFY(entry); QVERIFY(!entry->isEnabled());
        const auto output = std::filesystem::path(dir.filePath("runtime-must-not-save.w3x").toStdWString());
        project.ClearDiagnostics(); QVERIFY(!project.PrepareMapSave(output, false));
        QVERIFY(HasDiagnostic(project, "MAP_RUNTIME_LUA_PROFILE_UNCONFIRMED")); QVERIFY(!QFile::exists(QString::fromStdWString(output.wstring())));
        QCOMPARE(QDir(dir.path()).entryList(QDir::Files), files);
        QCOMPARE(Sha256File(project.sourceArchivePath_).value.value_or(""), *sourceSha.value);
        const auto snapshots = qEnvironmentVariable("WAR3_TEST_SCREENSHOT_DIR");
        if (!snapshots.isEmpty()) { QApplication::processEvents(); QVERIFY(scripts.grab().save(QDir(snapshots).filePath("c3-lua-owned-runtime-gate.png"))); }
    }
    void SyntaxChecksAndUndoPreserveOriginalBytes() {
        QTemporaryDir dir; QVERIFY(dir.isValid()); QtProject::EditorProject project; QVERIFY(OpenMap(dir, project));
        Modules::TriggerLuaScripts scripts(project); scripts.show(); QVERIFY(scripts.SelectScript("Scripts\\Existing.lua"));
        const auto original = QtProject::ReadOwnedAsset(project, scripts.SelectedPath()); QVERIFY(original.Succeeded());
        QCOMPARE(OwnedSourceText(*original.value), std::string("local count = 1\r\nreturn count\r\n"));
        QVERIFY(!scripts.HasPendingDraft()); QVERIFY(scripts.CommitPendingDraft()); QCOMPARE(project.AssetUndoStack()->count(), 0);
        scripts.Editor()->SetText("local count =\n");
        QVERIFY(scripts.Editor()->IsChecking() || scripts.Editor()->CheckResult().generation != scripts.Editor()->Generation());
        auto* apply = scripts.findChild<QPushButton*>("luaApplyDraft"); QVERIFY(apply); QVERIFY(apply->isEnabled()); apply->click();
        QVERIFY(!scripts.HasPendingDraft()); QCOMPARE(project.AssetUndoStack()->count(), 1);
        const auto unfinished = QtProject::ReadOwnedAsset(project, "Scripts\\Existing.lua"); QVERIFY(unfinished.Succeeded());
        QVERIFY(unfinished.value->runtimeLuaDraft); QVERIFY(project.pendingImports_.front().diskPath.isEmpty());
        QCOMPARE(OwnedSourceText(*unfinished.value), std::string("local count =\r\n"));
        QTRY_VERIFY_WITH_TIMEOUT(!scripts.Editor()->IsChecking() && scripts.Editor()->CheckResult().completed, 5000);
        QVERIFY(std::any_of(scripts.Editor()->CheckResult().diagnostics.begin(), scripts.Editor()->CheckResult().diagnostics.end(),
            [](const auto& issue) { return issue.severity == UiKit::CodeSeverity::Error; }));
        auto* problems = scripts.findChild<UiKit::CodeProblemsView*>("luaCodeProblems"); QVERIFY(problems); QVERIFY(problems->IssueCount() > 0);
        QVERIFY(problems->StatusText().contains(QStringLiteral("错误")));
        const auto markers = scripts.Editor()->extraSelections();
        QVERIFY(std::any_of(markers.begin(), markers.end(), [](const auto& marker) {
            return marker.format.underlineStyle() == QTextCharFormat::WaveUnderline
                && marker.format.underlineColor().red() > marker.format.underlineColor().green();
        }));
        scripts.Editor()->SetText("local count = 2\nreturn count\n");
        QVERIFY(scripts.CommitPendingDraft()); QVERIFY(!scripts.HasPendingDraft()); QCOMPARE(project.AssetUndoStack()->count(), 2);
        const auto edited = QtProject::ReadOwnedAsset(project, "Scripts\\Existing.lua"); QVERIFY(edited.Succeeded());
        QVERIFY(edited.value->runtimeLuaDraft); QCOMPARE(OwnedSourceText(*edited.value), std::string("local count = 2\r\nreturn count\r\n"));
        project.AssetUndoStack()->undo(); const auto undoneIncomplete = QtProject::ReadOwnedAsset(project, "Scripts\\Existing.lua"); QVERIFY(undoneIncomplete.Succeeded());
        QCOMPARE(*undoneIncomplete.value->bytes, *unfinished.value->bytes);
        project.AssetUndoStack()->undo(); const auto undone = QtProject::ReadOwnedAsset(project, "Scripts\\Existing.lua"); QVERIFY(undone.Succeeded());
        QCOMPARE(undone.value->sha256, original.value->sha256); QCOMPARE(OwnedSourceText(*undone.value), OwnedSourceText(*original.value));
        project.AssetUndoStack()->redo(); project.AssetUndoStack()->redo(); const auto redone = QtProject::ReadOwnedAsset(project, "Scripts\\Existing.lua"); QVERIFY(redone.Succeeded());
        QCOMPARE(redone.value->sha256, edited.value->sha256);
        const auto archive = project.archiveSession_->ReadMember("Scripts\\Existing.lua"); QVERIFY(archive.Succeeded());
        QCOMPARE(*archive.value, *original.value->bytes);
    }
    void ResourceRenameAndDeleteSynchronizeTheSourceView() {
        QTemporaryDir dir; QVERIFY(dir.isValid()); QtProject::EditorProject project; QVERIFY(OpenMap(dir, project));
        Modules::AssetsModule assets(project); assets.Widget(); Modules::TriggerLuaScripts scripts(project); scripts.show();
        QVERIFY(scripts.CreateScript("Lua\\one.lua", "return 7\n"));
        const auto payload = project.pendingImports_.front().ownedPayload;
        QVERIFY(assets.RenameAsset("Lua\\one.lua", "Lua\\two.lua").isEmpty());
        QCOMPARE(scripts.SelectedPath(), QStringLiteral("Lua\\two.lua"));
        QVERIFY(scripts.ScriptPaths().contains("Lua\\two.lua")); QVERIFY(!scripts.ScriptPaths().contains("Lua\\one.lua"));
        QCOMPARE(project.pendingImports_.front().ownedPayload, payload); QVERIFY(project.pendingImports_.front().runtimeLuaDraft);
        QVERIFY(assets.DeleteAssets({"Lua\\two.lua"}).isEmpty()); QVERIFY(!scripts.ScriptPaths().contains("Lua\\two.lua"));
        QVERIFY(scripts.SelectedPath().isEmpty()); QVERIFY(scripts.Editor()->isReadOnly());
        assets.UndoStack()->undo(); QVERIFY(scripts.ScriptPaths().contains("Lua\\two.lua"));
        QVERIFY(scripts.SelectScript("Lua\\two.lua")); QCOMPARE(scripts.Editor()->toPlainText(), QStringLiteral("return 7\n"));
        assets.UndoStack()->undo(); QCOMPARE(scripts.SelectedPath(), QStringLiteral("Lua\\one.lua"));
        QVERIFY(!scripts.CreateScript("lua/ONE.lua")); QVERIFY(!scripts.RenameScript("Lua\\one.lua", "Lua\\bad.txt"));
        QCOMPARE(project.pendingImports_.front().ownedPayload, payload);
    }
    void EmptySelectionResetsSourceNoteAndDisablesFileActions() {
        QTemporaryDir dir; QVERIFY(dir.isValid()); QtProject::EditorProject project; QVERIFY(OpenMap(dir, project));
        const auto sourceSha = Sha256File(project.sourceArchivePath_); QVERIFY(sourceSha.Succeeded());
        Modules::AssetsModule assets(project); assets.Widget(); Modules::TriggerLuaScripts scripts(project); scripts.show();
        auto* note = scripts.findChild<QLabel*>("luaSourceNote"); QVERIFY(note);
        const auto empty = [&] {
            if (!scripts.SelectedPath().isEmpty() || !scripts.Editor()->isReadOnly() || !scripts.Editor()->toPlainText().isEmpty()
                || note->text() != QStringLiteral("选择一个导入的 .lua 文件。")) return false;
            for (const auto* name : {"luaRenameScript", "luaDeleteScript", "luaRevealAsset", "luaCheckSyntax"}) {
                auto* button = scripts.findChild<QPushButton*>(name);
                if (!button || button->isEnabled()) return false;
            }
            return true;
        };
        QVERIFY(empty());
        QVERIFY(scripts.SelectScript("Scripts\\Existing.lua"));
        for (const auto* name : {"luaRenameScript", "luaDeleteScript", "luaRevealAsset", "luaCheckSyntax"}) {
            auto* button = scripts.findChild<QPushButton*>(name); QVERIFY(button); QVERIFY(button->isEnabled());
        }
        QVERIFY(note->text().contains(QStringLiteral("Scripts\\Existing.lua")));
        QVERIFY(scripts.DeleteScript("Scripts\\Existing.lua")); QVERIFY(empty());
        project.AssetUndoStack()->undo(); QVERIFY(scripts.SelectScript("Scripts\\Existing.lua"));
        QVERIFY(assets.DeleteAssets({"Scripts\\Existing.lua"}).isEmpty()); QVERIFY(empty());
        project.AssetUndoStack()->undo(); QVERIFY(scripts.SelectScript("Scripts\\Existing.lua"));
        scripts.Editor()->SetText("local value =\n"); QVERIFY(scripts.HasPendingDraft());
        QVERIFY(assets.DeleteAssets({"Scripts\\Existing.lua"}).isEmpty()); QVERIFY(scripts.HasPendingDraft());
        scripts.DiscardPendingDraft(); QVERIFY(empty()); QVERIFY(!scripts.HasPendingDraft());
        QCOMPARE(Sha256File(project.sourceArchivePath_).value.value_or(""), *sourceSha.value);
    }
    void StaleDraftSurvivesExternalEditButUnrelatedEditDoesNotInvalidateIt() {
        QTemporaryDir dir; QVERIFY(dir.isValid()); QtProject::EditorProject project; QVERIFY(OpenMap(dir, project));
        Modules::TriggerLuaScripts scripts(project); scripts.show(); QVERIFY(scripts.SelectScript("Scripts\\Existing.lua"));
        scripts.Editor()->SetText("local count = 2\nreturn count\n");
        QTRY_VERIFY_WITH_TIMEOUT(!scripts.Editor()->IsChecking() && scripts.Editor()->CheckResult().completed, 5000);
        QVERIFY(project.RecordAssetEdit("unrelated Lua draft", [&](QtProject::AssetState& candidate) { return QtProject::PutOwnedAsset(project, candidate, "Lua\\other.lua", Payload("return 3\n")); }).isEmpty());
        QVERIFY(scripts.CommitPendingDraft());
        scripts.Editor()->SetText("local count = 4\nreturn count\n");
        QTRY_VERIFY_WITH_TIMEOUT(!scripts.Editor()->IsChecking() && scripts.Editor()->CheckResult().completed, 5000);
        QVERIFY(project.RecordAssetEdit("external edit", [&](QtProject::AssetState& candidate) { return QtProject::PutOwnedAsset(project, candidate, "Scripts\\Existing.lua", Payload("return 9\n")); }).isEmpty());
        const auto undoCount = project.AssetUndoStack()->count();
        QVERIFY(!scripts.CommitPendingDraft()); QVERIFY(scripts.HasPendingDraft()); QCOMPARE(project.AssetUndoStack()->count(), undoCount);
        QCOMPARE(scripts.Editor()->toPlainText(), QStringLiteral("local count = 4\nreturn count\n"));
        const auto live = QtProject::ReadOwnedAsset(project, "Scripts\\Existing.lua"); QVERIFY(live.Succeeded()); QCOMPARE(OwnedSourceText(*live.value), std::string("return 9\n"));
        scripts.DiscardPendingDraft(); QVERIFY(!scripts.HasPendingDraft()); QCOMPARE(scripts.Editor()->toPlainText(), QStringLiteral("return 9\n"));
    }
    void UnknownEncodingRemainsReadonlyAndUnmodified() {
        QTemporaryDir dir; QVERIFY(dir.isValid()); QtProject::EditorProject project; QVERIFY(OpenMap(dir, project));
        Modules::TriggerLuaScripts scripts(project); scripts.show(); QVERIFY(scripts.SelectScript("Scripts\\Legacy.lua"));
        QTRY_VERIFY_WITH_TIMEOUT(!scripts.Editor()->IsLoading() && !scripts.Editor()->IsChecking(), 5000);
        QVERIFY(scripts.Editor()->isReadOnly()); QVERIFY(!scripts.HasPendingDraft()); QVERIFY(scripts.CommitPendingDraft());
        QCOMPARE(project.AssetUndoStack()->count(), 0); QVERIFY(!scripts.Editor()->CheckResult().syntaxChecked);
        QVERIFY(scripts.Editor()->DecodeNotice().contains(QStringLiteral("推测")));
        const auto source = QtProject::ReadOwnedAsset(project, "Scripts\\Legacy.lua"); QVERIFY(source.Succeeded());
        QCOMPARE(*source.value->bytes, (std::vector<std::byte>{std::byte{'-'}, std::byte{'-'}, std::byte{' '}, std::byte{0xD6}, std::byte{0xD0}, std::byte{'\n'}}));
    }
    void SpecialWhitespaceIsReadonlyAndCannotPublishAlteredDisplay() {
        QTemporaryDir dir; QVERIFY(dir.isValid()); QtProject::EditorProject project; QVERIFY(OpenMap(dir, project));
        const auto sourceSha = Sha256File(project.sourceArchivePath_); QVERIFY(sourceSha.Succeeded());
        const auto original = QtProject::ReadOwnedAsset(project, "Scripts\\Unicode.lua"); QVERIFY(original.Succeeded());
        // 原字节是合法 Lua；只读来自显示投影能力，不可报告成 Lua 语法错误。
        QVERIFY(Lua::CheckSyntax(OwnedSourceText(*original.value), "Unicode.lua").Succeeded());
        Modules::TriggerLuaScripts scripts(project); scripts.show(); QVERIFY(scripts.SelectScript("Scripts\\Unicode.lua"));
        QTRY_VERIFY_WITH_TIMEOUT(!scripts.Editor()->IsLoading() && !scripts.Editor()->IsChecking(), 5000);
        QVERIFY(scripts.Editor()->isReadOnly()); QVERIFY(!scripts.HasPendingDraft());
        auto* note = scripts.findChild<QLabel*>("luaSourceNote"); QVERIFY(note && note->text().contains(QStringLiteral("特殊空白字符")));
        auto* apply = scripts.findChild<QPushButton*>("luaApplyDraft"); QVERIFY(apply && !apply->isEnabled());
        QVERIFY(!scripts.Editor()->CheckResult().syntaxChecked);
        const auto displayed = scripts.Editor()->toPlainText(); QTest::keyClicks(scripts.Editor(), "changed");
        QCOMPARE(scripts.Editor()->toPlainText(), displayed);
        // 即使测试代码绕过只读控件改了显示，来源能力也不能生成 owned 草稿。
        scripts.Editor()->SetText("return 9\n"); QVERIFY(!scripts.HasPendingDraft()); QVERIFY(scripts.CommitPendingDraft());
        QCOMPARE(project.AssetUndoStack()->count(), 0); QVERIFY(project.pendingImports_.empty());
        const auto unchanged = QtProject::ReadOwnedAsset(project, "Scripts\\Unicode.lua"); QVERIFY(unchanged.Succeeded());
        QCOMPARE(*unchanged.value->bytes, *original.value->bytes); QCOMPARE(unchanged.value->sha256, original.value->sha256);
        scripts.DiscardPendingDraft(); QTRY_VERIFY_WITH_TIMEOUT(!scripts.Editor()->IsLoading(), 5000);
        QVERIFY(scripts.Editor()->isReadOnly()); QCOMPARE(scripts.Editor()->toPlainText(), displayed);
        QCOMPARE(Sha256File(project.sourceArchivePath_).value, sourceSha.value);
    }
    void ArchiveIdentityIsAuthoritativeWithExtractedDirectoryProjection() {
        QTemporaryDir dir; QVERIFY(dir.isValid()); QtProject::EditorProject project; QVERIFY(OpenMap(dir, project));
        auto projection = DirectoryWorkspace::Open(std::filesystem::path(dir.path().toStdWString()));
        QVERIFY(projection.Succeeded()); project.directorySnapshot_ = std::make_unique<DirectoryWorkspace>(std::move(*projection.value));
        const auto source = QtProject::ReadOwnedAsset(project, "Scripts\\Existing.lua"); QVERIFY(source.Succeeded());
        QVERIFY(source.value->archive && source.value->directory);
        QVERIFY(source.value->sourceSha256 == source.value->archive->SourceSha256());
        QVERIFY(source.value->sourceSha256 != source.value->directory->Snapshot().aggregateSha256);
        QVERIFY(QtProject::VerifyOwnedAsset(project, *source.value).Succeeded());
        Modules::TriggerLuaScripts scripts(project); QVERIFY(scripts.SelectScript("Scripts\\Existing.lua"));
        scripts.Editor()->SetText("local count =\n"); QVERIFY(scripts.CommitPendingDraft());
        QCOMPARE(OwnedSourceText(*QtProject::ReadOwnedAsset(project, "Scripts\\Existing.lua").value), std::string("local count =\r\n"));
        // 目录会话指针仍是身份一部分，不能让另一投影借用旧能力。
        auto replacement = DirectoryWorkspace::Open(std::filesystem::path(dir.path().toStdWString())); QVERIFY(replacement.Succeeded());
        project.directorySnapshot_ = std::make_unique<DirectoryWorkspace>(std::move(*replacement.value));
        QVERIFY(!QtProject::VerifyOwnedAsset(project, *source.value).Succeeded());
    }
    void FinalProjectIdentityRefreshesCleanLuaButNeverRebindsPendingDraft() {
        QTemporaryDir dir; QVERIFY(dir.isValid()); QtProject::EditorProject project; QVERIFY(OpenMap(dir, project));
        Modules::TriggerLuaScripts scripts(project); QVERIFY(scripts.SelectScript("Scripts\\Existing.lua"));
        const auto previous = QtProject::ReadOwnedAsset(project, "Scripts\\Existing.lua"); QVERIFY(previous.Succeeded());
        project.projectFilePath_ = dir.filePath("active.forge").toStdWString();
        project.ResetEditTracking(); project.RefreshProjectWatch();
        QVERIFY(!QtProject::VerifyOwnedAsset(project, *previous.value).Succeeded());
        scripts.Editor()->SetText("local count =\n"); QVERIFY(scripts.CommitPendingDraft());
        const auto applied = QtProject::ReadOwnedAsset(project, "Scripts\\Existing.lua"); QVERIFY(applied.Succeeded());
        QCOMPARE(OwnedSourceText(*applied.value), std::string("local count =\r\n"));
        scripts.Editor()->SetText("return 7\n"); QVERIFY(scripts.HasPendingDraft());
        const int undoCount = project.AssetUndoStack()->count();
        project.ResetEditTracking(); project.RefreshProjectWatch();
        QVERIFY(!scripts.CommitPendingDraft()); QVERIFY(scripts.HasPendingDraft());
        QCOMPARE(scripts.Editor()->toPlainText(), QStringLiteral("return 7\n"));
        QCOMPARE(project.AssetUndoStack()->count(), undoCount);
        scripts.DiscardPendingDraft(); QVERIFY(!scripts.HasPendingDraft());
        scripts.Editor()->SetText("return 9\n"); project.sourceChangeSuspected_ = true; project.RefreshProjectWatch();
        QVERIFY(!scripts.CommitPendingDraft()); QVERIFY(scripts.HasPendingDraft());
        QCOMPARE(project.AssetUndoStack()->count(), undoCount);
        QCOMPARE(OwnedSourceText(*QtProject::ReadOwnedAsset(project, "Scripts\\Existing.lua").value), std::string("local count =\r\n"));
    }
    void BomAndCrLfUneditedCommitAndDiscardPreserveOriginalBytes() {
        QTemporaryDir dir; QVERIFY(dir.isValid()); QtProject::EditorProject project; QVERIFY(OpenMap(dir, project));
        const auto sourceSha = Sha256File(project.sourceArchivePath_); QVERIFY(sourceSha.Succeeded());
        Modules::TriggerLuaScripts scripts(project); scripts.show(); QVERIFY(scripts.SelectScript("Scripts\\BomCrLf.lua"));
        QVERIFY(!scripts.Editor()->isReadOnly()); QVERIFY(!scripts.HasPendingDraft());
        const auto original = QtProject::ReadOwnedAsset(project, "Scripts\\BomCrLf.lua"); QVERIFY(original.Succeeded());
        QCOMPARE(OwnedSourceText(*original.value), std::string("\xEF\xBB\xBF" "local count = 1\r\nreturn count\r\n"));
        QVERIFY(scripts.CommitPendingDraft()); scripts.DiscardPendingDraft(); QVERIFY(scripts.CommitPendingDraft());
        QVERIFY(!scripts.HasPendingDraft()); QCOMPARE(project.AssetUndoStack()->count(), 0); QVERIFY(project.pendingImports_.empty());
        const auto unchanged = QtProject::ReadOwnedAsset(project, "Scripts\\BomCrLf.lua"); QVERIFY(unchanged.Succeeded());
        QCOMPARE(*unchanged.value->bytes, *original.value->bytes); QCOMPARE(unchanged.value->sha256, original.value->sha256);
        QCOMPARE(Sha256File(project.sourceArchivePath_).value.value_or(""), *sourceSha.value);
    }
    void MutableCallerAliasCannotChangePublishedOrUndoBytes() {
        QTemporaryDir dir; QVERIFY(dir.isValid()); QtProject::EditorProject project; QVERIFY(OpenMap(dir, project));
        auto mutableBytes = std::make_shared<std::vector<std::byte>>(Bytes("return 1\n"));
        const std::shared_ptr<const std::vector<std::byte>> input = mutableBytes;
        QVERIFY(project.RecordAssetEdit("owned Lua", [&](QtProject::AssetState& candidate) {
            return QtProject::PutOwnedAsset(project, candidate, "Lua\\sealed.lua", input);
        }).isEmpty());
        const auto original = QtProject::ReadOwnedAsset(project, "Lua\\sealed.lua"); QVERIFY(original.Succeeded());
        QVERIFY(original.value->bytes != input); *mutableBytes = Bytes("return 9\n");
        const auto current = QtProject::ReadOwnedAsset(project, "Lua\\sealed.lua"); QVERIFY(current.Succeeded());
        QCOMPARE(OwnedSourceText(*current.value), std::string("return 1\n")); QCOMPARE(current.value->sha256, original.value->sha256);
        project.AssetUndoStack()->undo(); QVERIFY(!QtProject::ReadOwnedAsset(project, "Lua\\sealed.lua").Succeeded());
        project.AssetUndoStack()->redo(); const auto redone = QtProject::ReadOwnedAsset(project, "Lua\\sealed.lua"); QVERIFY(redone.Succeeded());
        QCOMPARE(OwnedSourceText(*redone.value), std::string("return 1\n")); QCOMPARE(redone.value->sha256, original.value->sha256);
    }
    void RuntimeDraftRoleCannotBeDroppedByRenameOrDiskReplacement() {
        QTemporaryDir dir; QVERIFY(dir.isValid()); QtProject::EditorProject project; QVERIFY(OpenMap(dir, project));
        Modules::TriggerLuaScripts scripts(project); QVERIFY(scripts.CreateScript("Lua\\sealed.lua", "return 1\n"));
        Modules::AssetsModule assets(project); assets.Widget();
        QVERIFY(assets.RenameAsset("Lua\\sealed.lua", "Lua\\sealed.txt").isEmpty());
        QVERIFY(project.pendingImports_.front().runtimeLuaDraft); QVERIFY(project.pendingImports_.front().ownedPayload);
        QFile replacement(dir.filePath("sealed.txt")); QVERIFY(replacement.open(QIODevice::WriteOnly)); QCOMPARE(replacement.write("return 9\n"), qint64{9}); replacement.close();
        assets.AddFiles({replacement.fileName()}, "Lua"); QCOMPARE(project.pendingImports_.size(), std::size_t{1});
        QVERIFY(project.pendingImports_.front().runtimeLuaDraft); QVERIFY(!project.pendingImports_.front().ownedPayload);
        const auto output = std::filesystem::path(dir.filePath("role-must-not-save.w3x").toStdWString());
        project.ClearDiagnostics(); QVERIFY(!project.PrepareMapSave(output, false));
        QVERIFY(HasDiagnostic(project, "MAP_RUNTIME_LUA_PROFILE_UNCONFIRMED")); QVERIFY(!QFile::exists(QString::fromStdWString(output.wstring())));
        assets.UndoStack()->undo(); QVERIFY(project.pendingImports_.front().ownedPayload); QVERIFY(project.pendingImports_.front().runtimeLuaDraft);
        assets.UndoStack()->undo(); QCOMPARE(project.pendingImports_.front().archivePath, QStringLiteral("Lua\\sealed.lua"));
        QVERIFY(project.pendingImports_.front().runtimeLuaDraft);
    }
    void TriggerWorkspaceRoutesAssetUndoAndRetainsInvalidDraftOnTabSwitch() {
        QTemporaryDir dir; QVERIFY(dir.isValid()); QtProject::EditorProject project; QVERIFY(OpenMap(dir, project));
        LuaHost host; Modules::TriggerModule module(project); module.AttachHost(&host);
        auto* page = module.Widget(); page->resize(1200, 780); page->show();
        auto* tabs = page->findChild<QTabWidget*>("triggerWorkspaceTabs"); QVERIFY(tabs); QCOMPARE(tabs->count(), 2);
        auto* scripts = module.LuaScripts(); QVERIFY(scripts); QVERIFY(scripts->CreateScript("Lua\\workspace.lua", "return 7\n"));
        tabs->setCurrentIndex(1); QCOMPARE(module.UndoStack(), project.AssetUndoStack()); QCOMPARE(host.undoChanges, 1);
        module.SetSearchText("return"); QCOMPARE(scripts->Editor()->textCursor().selectedText(), QStringLiteral("return"));
        const auto nulDraft = QStringLiteral("return 8\n") + QChar{u'\0'};
        scripts->Editor()->SetText(nulDraft);
        QVERIFY(module.HasPendingCodeDraft()); QVERIFY(!module.CommitPendingEdits());
        QVERIFY(scripts->StatusText().contains(QStringLiteral("NUL")));
        tabs->setCurrentIndex(0); QCOMPARE(tabs->currentIndex(), 1); QCOMPARE(host.undoChanges, 1);
        QCOMPARE(scripts->Editor()->toPlainText(), nulDraft); QCOMPARE(project.AssetUndoStack()->count(), 1);
        module.DiscardPendingCodeDraft(); QVERIFY(!module.HasPendingCodeDraft());
        tabs->setCurrentIndex(0); QCOMPARE(module.UndoStack(), project.triggerUndoStack_); QCOMPARE(host.undoChanges, 2);
        QVERIFY(module.Reveal({"triggers", "lua-file", "Lua\\workspace.lua"})); QCOMPARE(tabs->currentIndex(), 1);
        auto* reveal = scripts->findChild<QPushButton*>("luaRevealAsset"); QVERIFY(reveal); reveal->click();
        QCOMPARE(host.lastTarget.module, QStringLiteral("assets")); QCOMPARE(host.lastTarget.kind, QStringLiteral("asset")); QCOMPARE(host.lastTarget.id, scripts->SelectedPath());
    }
    void InvalidPathsBudgetsAndAmbiguousSourcesPublishNothing() {
        QTemporaryDir dir; QVERIFY(dir.isValid()); QtProject::EditorProject project; QVERIFY(OpenMap(dir, project));
        Modules::TriggerLuaScripts scripts(project); const auto before = project.CaptureAssets();
        QVERIFY(!scripts.CreateScript("Lua\\..\\unsafe.lua")); QVERIFY(!scripts.CreateScript("Scripts\\war3map.lua"));
        QVERIFY(!scripts.CreateScript("Lua\\large.lua", QString(static_cast<qsizetype>(QtProject::kOwnedLuaAssetBytes) + 1, QChar('x'))));
        QVERIFY(project.CaptureAssets() == before); QCOMPARE(project.AssetUndoStack()->count(), 0);
        QVERIFY(project.RecordAssetEdit("ambiguous source", [&](QtProject::AssetState& candidate) {
            candidate.imports.push_back({"Lua\\ambiguous.lua", {}, 9, Payload("return 1\n"), true});
            candidate.imports.push_back({"lua/AMBIGUOUS.lua", {}, 9, Payload("return 2\n"), true});
            return QString{};
        }).isEmpty());
        const auto ambiguous = project.CaptureAssets(); QVERIFY(!QtProject::ReadOwnedAsset(project, "Lua\\ambiguous.lua").Succeeded());
        QVERIFY(!project.RecordAssetEdit("must not pick source", [&](QtProject::AssetState& candidate) {
            return QtProject::PutOwnedAsset(project, candidate, "Lua\\ambiguous.lua", Payload("return 3\n"));
        }).isEmpty());
        QVERIFY(project.CaptureAssets() == ambiguous); QCOMPARE(project.AssetUndoStack()->count(), 1);
    }
    void InvalidUtf8AndNulCannotPublishOwnedDrafts() {
        QTemporaryDir dir; QVERIFY(dir.isValid()); QtProject::EditorProject project; QVERIFY(OpenMap(dir, project));
        const auto before = project.CaptureAssets();
        const std::vector<std::string> invalidUtf8{
            std::string("-- \xD6\xD0\n"), std::string("\xC0\xAF"), std::string("\xED\xA0\x80"),
            std::string("\xF4\x90\x80\x80"), std::string("\xE2\x82"), std::string("\x80")
        };
        for (const auto& raw : invalidUtf8) {
            const auto problem = project.RecordAssetEdit("refuse invalid UTF-8", [&](QtProject::AssetState& candidate) {
                return QtProject::PutOwnedAsset(project, candidate, "Lua\\invalid.lua", Payload(raw));
            });
            QVERIFY(problem.contains(QStringLiteral("UTF-8"))); QVERIFY(project.CaptureAssets() == before);
        }
        const std::string nul("return 1\n\0", 10);
        const auto problem = project.RecordAssetEdit("refuse NUL", [&](QtProject::AssetState& candidate) {
            return QtProject::PutOwnedAsset(project, candidate, "Lua\\invalid.lua", Payload(nul));
        });
        QVERIFY(problem.contains(QStringLiteral("NUL"))); QVERIFY(project.CaptureAssets() == before); QCOMPARE(project.AssetUndoStack()->count(), 0);
    }
};

QTEST_MAIN(LuaScriptsTests)
#include "LuaScriptsTests.moc"
