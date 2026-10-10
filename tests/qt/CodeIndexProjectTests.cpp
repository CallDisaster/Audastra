#include "EditorProject.h"
#include "Widgets/CodeView.h"
#include "TriggerCodeChecking.h"
#include "War3MapCore/Services/CodeCheck.h"
#include "War3MapCore/Services/CodeCheckProject.h"

#include <QApplication>
#include <QTest>
#include <QTextBlock>
#include <QTreeWidget>
#include <QSignalSpy>
#include <QDialog>
#include <QUndoStack>
#include <QSettings>
#include <QScopeGuard>
#include <QTemporaryDir>

#include <algorithm>
#include <utility>

using namespace War3Map;

class CodeIndexProjectTests final : public QObject {
    Q_OBJECT
private slots:
    void ActualSemanticNavigationSeparatesSameNameLocalsAndRejectsStaleIndex() {
        const QString body = QStringLiteral("globals\ninteger g = 0\nendglobals\n"
            "function First takes nothing returns integer\nlocal integer g = 1\nset g = g + 1\nreturn g\nendfunction\n"
            "function Second takes nothing returns integer\nlocal integer g = 2\nreturn g\nendfunction\n");
        UiKit::CodeView view; view.SetSourceName("scoped.j"); view.SetEditable(true);
        Services::CodeCheckContext context; context.complete = true; context.binding = "owned-semantic-scope";
        view.SetPrepareCheck([context](const QString& text, std::uint64_t generation) {
            return Modules::PrepareTriggerCodeCheck(text, generation, "scoped.j", UiKit::CodeLanguage::Jass, context);
        });
        view.SetText(body); view.RequestCheck();
        QTRY_VERIFY_WITH_TIMEOUT(!view.IsChecking() && view.CheckResult().completed, 5000);
        QVERIFY(view.CheckResult().semanticIndexComplete);
        QVERIFY(!view.CheckResult().semanticDefinitions.isEmpty());
        QTextCursor cursor(view.document()); cursor.setPosition(body.indexOf("return g") + 7); view.setTextCursor(cursor);
        QSignalSpy jumped(&view, &UiKit::CodeView::JumpRequested); QTest::keyClick(&view, Qt::Key_F12);
        QCOMPARE(jumped.size(), 1); QCOMPARE(jumped[0][1].toString(), QStringLiteral("scoped.j"));
        QCOMPARE(view.textCursor().position(), body.indexOf("local integer g = 1") + 14);
        QTest::keyClick(&view, Qt::Key_F12, Qt::ShiftModifier);
        auto* dialog = view.findChild<QDialog*>("codeReferencesDialog"); QVERIFY(dialog);
        auto* rows = dialog->findChild<QTreeWidget*>(); QVERIFY(rows); QCOMPARE(rows->topLevelItemCount(), 3);
        for (int i = 0; i < rows->topLevelItemCount(); ++i) {
            const auto line = rows->topLevelItem(i)->data(1, Qt::UserRole).toInt(); QVERIFY(line == 6 || line == 7);
        }
        auto* oldRow = rows->topLevelItem(0);
        const auto firstDeclaration = view.textCursor().position();
        QTextCursor temporary = view.textCursor(); temporary.movePosition(QTextCursor::End); temporary.insertText("// changed\n");
        view.setTextCursor(temporary); const auto changedForOldDialog = view.textCursor().position();
        QMetaObject::invokeMethod(rows, "itemActivated", Qt::DirectConnection, Q_ARG(QTreeWidgetItem*, oldRow), Q_ARG(int, 0));
        QCOMPARE(view.textCursor().position(), changedForOldDialog); QVERIFY(changedForOldDialog != firstDeclaration);
        dialog->close();
        view.SetText(body); view.RequestCheck();
        QTRY_VERIFY_WITH_TIMEOUT(!view.IsChecking() && view.CheckResult().completed, 5000);
        cursor = QTextCursor(view.document()); // SetText 已替换 document，不能复用旧正文的 cursor。
        cursor.setPosition(body.indexOf("return g") + 7); view.setTextCursor(cursor);
        bool bindingCurrent = true;
        view.SetCheckBindingValidator([&bindingCurrent](const QString&) { return bindingCurrent; });
        QTest::keyClick(&view, Qt::Key_F12, Qt::ShiftModifier);
        auto* bindingDialog = view.findChildren<QDialog*>("codeReferencesDialog").last(); QVERIFY(bindingDialog);
        auto* bindingRows = bindingDialog->findChild<QTreeWidget*>(); QVERIFY(bindingRows);
        QVERIFY(bindingRows->topLevelItemCount() > 0);
        auto* bindingRow = bindingRows->topLevelItem(0);
        const auto bindingPosition = view.textCursor().position();
        bindingCurrent = false;
        QMetaObject::invokeMethod(bindingRows, "itemActivated", Qt::DirectConnection,
            Q_ARG(QTreeWidgetItem*, bindingRow), Q_ARG(int, 0));
        QCOMPARE(view.textCursor().position(), bindingPosition);
        bindingCurrent = true; bindingDialog->close();
        cursor.setPosition(body.indexOf("return g", body.indexOf("function Second")) + 7); view.setTextCursor(cursor);
        QTest::keyClick(&view, Qt::Key_F12);
        QCOMPARE(view.textCursor().position(), body.indexOf("local integer g = 2") + 14);
        // 输入变化后的下一次检查尚未完成，不允许用旧索引跳回旧正文。
        cursor = view.textCursor(); cursor.insertText("x"); view.setTextCursor(cursor); const auto changedPosition = view.textCursor().position();
        QTest::keyClick(&view, Qt::Key_F12); QCOMPARE(view.textCursor().position(), changedPosition);
        QCOMPARE(jumped.last()[2].toInt(), 0);
    }
    void CoreCRLineIdentityAndUTF8ColumnsUseSamePresentationTable() {
        Services::CodeCheckRequest request; request.source = {"WCT/header.j",
            "function F takes nothing returns string\nlocal string result = \"中文\" + Value(\"x\")\nreturn result\nendfunction\n"};
        request.context.complete = true; request.context.binding = "actual-mixed-newline-origin";
        request.context.declarations = {{"Save/common.j", "// original comment\rnative Value takes string text returns string\r"}};
        const auto checked = Services::CheckCode(request); QVERIFY(checked.Succeeded());
        const auto body = QString::fromUtf8(request.source.text);
        const auto presented = Modules::PresentCodeCheckResult(checked, body, "WCT/header.j");
        const auto native = std::find_if(presented.semanticDefinitions.begin(), presented.semanticDefinitions.end(), [](const auto& definition) {
            return definition.name == "Value";
        });
        QVERIFY(native != presented.semanticDefinitions.end()); QVERIFY(native->exact);
        QCOMPARE(native->source, QStringLiteral("Save/common.j")); QCOMPARE(native->line, 2); QCOMPARE(native->column, 8);
        const auto call = std::find_if(presented.semanticReferences.begin(), presented.semanticReferences.end(), [&](const auto& reference) {
            return reference.targetKnown && reference.target == native->identity;
        });
        QVERIFY(call != presented.semanticReferences.end()); QVERIFY(call->exact); QCOMPARE(call->line, 2);
        const auto line = body.split('\n')[1]; QCOMPARE(call->column, line.indexOf("Value") + 1); QCOMPARE(call->length, 5);
        // 原版字节注释的未知编码不抹掉同一行前方已经证实的UTF8 token边界。
        request.context.declarations[0].text = "// original comment\rnative Value takes string text returns string // \xD6\xD0\xCE\xC4\r";
        const auto legacyChecked = Services::CheckCode(request); QVERIFY(legacyChecked.Succeeded());
        const auto legacyPresented = Modules::PresentCodeCheckResult(legacyChecked, body, "WCT/header.j");
        const auto legacyNative = std::find_if(legacyPresented.semanticDefinitions.begin(), legacyPresented.semanticDefinitions.end(), [](const auto& definition) {
            return definition.name == "Value";
        });
        QVERIFY(legacyNative != legacyPresented.semanticDefinitions.end()); QVERIFY(legacyNative->exact);
        QCOMPARE(legacyNative->line, 2); QCOMPARE(legacyNative->column, 8);
    }
    void PublishedIndexRequiresCurrentOwnedBinding() {
        QtProject::EditorProject project;
        project.triggerDocument_ = std::make_unique<Trigger::TriggerDocument>();
        project.triggerUndoStack_ = new QUndoStack(&project);
        const auto snapshot = project.CaptureCodeCheckSnapshot();
        QVERIFY(snapshot && !snapshot->binding.empty());
        QVERIFY(project.IsCodeCheckBindingCurrent(snapshot->binding));
        auto result = std::make_shared<Services::CodeCheckResult>();
        result->contextBinding = snapshot->binding; result->completed = true;
        // 语法降级的真实来源仍可导航，完整性由索引自己的状态明确报告。
        project.AdoptCodeCheckIndex(result);
        QCOMPARE(project.CurrentCodeCheckIndex().get(), result.get());
        auto cancelled = std::make_shared<Services::CodeCheckResult>(*result);
        cancelled->cancelled = true; project.AdoptCodeCheckIndex(cancelled);
        QCOMPARE(project.CurrentCodeCheckIndex().get(), result.get());
        auto unfinished = std::make_shared<Services::CodeCheckResult>(*result);
        unfinished->completed = false; project.AdoptCodeCheckIndex(unfinished);
        QCOMPARE(project.CurrentCodeCheckIndex().get(), result.get());
        project.triggerUndoStack_->push(new QUndoCommand("owned source revision"));
        QVERIFY(!project.IsCodeCheckBindingCurrent(snapshot->binding));
        QVERIFY(!project.CurrentCodeCheckIndex());
        const auto next = project.CaptureCodeCheckSnapshot();
        QVERIFY(next->binding != snapshot->binding);
        project.AdoptCodeCheckIndex(result); QVERIFY(!project.CurrentCodeCheckIndex());
        project.triggerUndoStack_->undo();
        project.triggerUndoStack_->push(new QUndoCommand("another owned branch"));
        QVERIFY(!project.IsCodeCheckBindingCurrent(next->binding));
        const auto branch = project.CaptureCodeCheckSnapshot();
        QVERIFY(branch->binding != next->binding);
        QCOMPARE(project.triggerUndoStack_->index(), 1);
        QCOMPARE(project.triggerUndoStack_->count(), 1);
    }
    void F9RuntimeSettingsHaveIndependentRootsAndInvalidateOwnedBindings() {
        QTemporaryDir fixture; QVERIFY(fixture.isValid());
        QSettings settings;
        const auto originalFlat = settings.value(QStringLiteral("mapTest/runtimeRoot"));
        const auto originalYdwe = settings.value(QStringLiteral("ui/dataRoot"));
        const auto restore = qScopeGuard([&] {
            for (const auto& entry : {std::pair{QStringLiteral("mapTest/runtimeRoot"), originalFlat},
                    std::pair{QStringLiteral("ui/dataRoot"), originalYdwe}}) {
                if (entry.second.isValid()) settings.setValue(entry.first, entry.second);
                else settings.remove(entry.first);
            }
            settings.sync();
        });
        const auto path = [](const QString& name) { return std::filesystem::path(name.toStdWString()); };
        const auto jassTools = fixture.filePath(QStringLiteral("independent-jass-tools"));
        const auto tree = fixture.filePath(QStringLiteral("runtime-tree/ui"));
        const auto flat = fixture.filePath(QStringLiteral("explicit-flat"));
        const auto alternateTree = fixture.filePath(QStringLiteral("other-runtime/share/mpq"));
        settings.remove(QStringLiteral("mapTest/runtimeRoot"));
        settings.setValue(QStringLiteral("ui/dataRoot"), QStringLiteral("  ") + tree + QStringLiteral("  "));
        settings.sync(); QCOMPARE(settings.status(), QSettings::NoError);
        QtProject::EditorProject project;
        project.uiDataRoot_ = path(jassTools); // 不需要创建伪造catalog或读取机器安装。
        const auto first = project.CaptureCodeCheckSnapshot();
        QVERIFY(first && !first->binding.empty());
        QCOMPARE(first->toolRoot, path(jassTools));
        QVERIFY(first->luaRuntimeRoot.empty()); QCOMPARE(first->luaRuntimeYdweRoot, path(tree));
        QCOMPARE(project.CaptureCodeCheckSnapshot().get(), first.get());
        auto index = std::make_shared<Services::CodeCheckResult>();
        index->completed = true; index->contextBinding = first->binding;
        project.AdoptCodeCheckIndex(index); QCOMPARE(project.CurrentCodeCheckIndex().get(), index.get());

        settings.setValue(QStringLiteral("mapTest/runtimeRoot"), QStringLiteral(" ") + flat + QStringLiteral(" "));
        settings.sync(); QCOMPARE(settings.status(), QSettings::NoError);
        QVERIFY(!project.IsCodeCheckBindingCurrent(first->binding)); QVERIFY(!project.CurrentCodeCheckIndex());
        const auto selectedFlat = project.CaptureCodeCheckSnapshot();
        QVERIFY(selectedFlat->binding != first->binding);
        QCOMPARE(selectedFlat->toolRoot, first->toolRoot);
        QCOMPARE(selectedFlat->luaRuntimeRoot, path(flat)); QCOMPARE(selectedFlat->luaRuntimeYdweRoot, path(tree));

        settings.setValue(QStringLiteral("ui/dataRoot"), alternateTree);
        settings.sync(); QCOMPARE(settings.status(), QSettings::NoError);
        QVERIFY(!project.IsCodeCheckBindingCurrent(selectedFlat->binding));
        const auto changedFallback = project.CaptureCodeCheckSnapshot();
        QVERIFY(changedFallback->binding != selectedFlat->binding);
        QCOMPARE(changedFallback->toolRoot, first->toolRoot);
        QCOMPARE(changedFallback->luaRuntimeRoot, path(flat)); QCOMPARE(changedFallback->luaRuntimeYdweRoot, path(alternateTree));

        settings.remove(QStringLiteral("mapTest/runtimeRoot")); settings.sync();
        QVERIFY(!project.IsCodeCheckBindingCurrent(changedFallback->binding));
        const auto automatic = project.CaptureCodeCheckSnapshot();
        QVERIFY(automatic->luaRuntimeRoot.empty()); QCOMPARE(automatic->luaRuntimeYdweRoot, path(alternateTree));
        QCOMPARE(automatic->toolRoot, first->toolRoot);
    }
    void UTF8ByteColumnsAndFrozenMixedNewlinesStayReadonly() {
        UiKit::CodeView view;
        view.SetText(QStringLiteral("中文 Print\nsecond"));
        view.GoToUtf8ByteColumn(1, 8);
        QCOMPARE(view.textCursor().positionInBlock(), 3);
        view.SetLanguage(UiKit::CodeLanguage::Lua);
        view.SetDecoder(Modules::DecodeMapCodeForView);
        const auto body = QStringLiteral("first\r\n中文 Print\rthird").toUtf8();
        QVERIFY(view.ShowFrozenSource({"actual-source.j", {}, {}, body}, 2, 8, "owned-test-binding", true));
        auto* frozen = view.findChild<UiKit::CodeView*>(); QVERIFY(frozen);
        QTRY_VERIFY_WITH_TIMEOUT(!frozen->IsLoading() && !frozen->IsChecking(), 5000);
        QVERIFY(frozen->isReadOnly());
        QCOMPARE(frozen->Language(), UiKit::CodeLanguage::VJass);
        QCOMPARE(frozen->textCursor().blockNumber(), 1);
        QCOMPARE(frozen->textCursor().positionInBlock(), 3);
        QCOMPARE(frozen->toPlainText(), QStringLiteral("first\n中文 Print\nthird"));
        QVERIFY(view.ShowFrozenSource({"actual-source.lua", {}, {}, "local value = 3\n"}, 1, 1, "owned-test-binding", true));
        QTRY_VERIFY_WITH_TIMEOUT(view.findChildren<UiKit::CodeView*>().size() == 1, 5000);
        frozen = view.findChild<UiKit::CodeView*>(); QVERIFY(frozen);
        QTRY_VERIFY_WITH_TIMEOUT(!frozen->IsLoading() && !frozen->IsChecking(), 5000);
        QCOMPARE(frozen->Language(), UiKit::CodeLanguage::Lua);
    }
};

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    QApplication application(argc, argv);
    QTemporaryDir settingsRoot;
    if (!settingsRoot.isValid()) return 1;
    const auto oldOrganization = QCoreApplication::organizationName();
    const auto oldApplication = QCoreApplication::applicationName();
    const auto oldFormat = QSettings::defaultFormat();
    QCoreApplication::setOrganizationName(QStringLiteral("Audastra-CodeIndexProject-Tests"));
    QCoreApplication::setApplicationName(QStringLiteral("CodeIndexProject"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    // User/System路径只在此测试进程生效，全部写入自有临时目录。
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsRoot.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settingsRoot.path());
    const auto restoreSettingsIdentity = qScopeGuard([&] {
        QSettings::setDefaultFormat(oldFormat);
        QCoreApplication::setOrganizationName(oldOrganization);
        QCoreApplication::setApplicationName(oldApplication);
    });
    CodeIndexProjectTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "CodeIndexProjectTests.moc"
