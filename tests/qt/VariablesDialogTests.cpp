#include "TriggerDialogs.h"

#include "War3MapCore/Trigger/TriggerEditing.h"

#include <QApplication>
#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QTest>
#include <QTimer>
#include <QUndoStack>

using namespace War3Map::Trigger;

namespace {

QTableWidget* Table(VariablesDialog& editor) {
    return editor.findChild<QTableWidget*>(QStringLiteral("globalVariablesTable"));
}

QPushButton* Button(VariablesDialog& editor, const char* name) {
    return editor.findChild<QPushButton*>(QString::fromLatin1(name));
}

void QueueRename(const QString& name, bool expectRejection = false) {
    QTimer::singleShot(0, qApp, [name, expectRejection] {
        auto* input = qobject_cast<QInputDialog*>(QApplication::activeModalWidget());
        QVERIFY(input != nullptr);
        input->setTextValue(name);
        if (expectRejection) {
            QTimer::singleShot(0, qApp, [] {
                auto* warning = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
                QVERIFY(warning != nullptr);
                warning->accept();
            });
        }
        input->accept();
    });
}

}

class VariablesDialogTests final : public QObject {
    Q_OBJECT

private slots:
    void EmbeddedEscapePreservesWorkspace();
    void RenameReferencesIsOneUndoTransaction();
    void UnsupportedReferenceRename();
    void AddRemoveAndUndoRefreshSelection();
};

void VariablesDialogTests::EmbeddedEscapePreservesWorkspace() {
    TriggerDocument document;
    QVERIFY(TriggerDocumentEditor::AddVariable(document, "Count", "integer", false).Succeeded());
    QUndoStack stack;
    VariablesDialog embedded(&document, &stack, nullptr, true);
    embedded.show();
    QApplication::processEvents();
    QVERIFY(Table(embedded) != nullptr);
    QTest::keyClick(Table(embedded), Qt::Key_Escape);
    QVERIFY(embedded.isVisible());
    embedded.accept();
    QVERIFY(embedded.isVisible());
    QCOMPARE(Table(embedded)->rowCount(), 1);

    VariablesDialog modal(&document, &stack);
    modal.show();
    QTest::keyClick(Table(modal), Qt::Key_Escape);
    QVERIFY(!modal.isVisible());
}

void VariablesDialogTests::RenameReferencesIsOneUndoTransaction() {
    TriggerDocument document;
    QVERIFY(TriggerDocumentEditor::AddVariable(document, "Count", "integer", false).Succeeded());
    const auto variableId = document.variables.front().id;
    ArgumentNode bare;
    bare.id = 101;
    bare.type = 1;
    bare.value.text = "Count";
    ArgumentNode prefixed = bare;
    prefixed.id = 102;
    prefixed.value.text = "udg_Count";
    document.argumentNodes = {bare, prefixed};
    QUndoStack stack;
    VariablesDialog editor(&document, &stack, nullptr, true);
    editor.show();
    QueueRename(QStringLiteral("Total"));
    Button(editor, "renameVariableButton")->click();
    QCOMPARE(stack.count(), 1);
    QCOMPARE(document.variables.front().name.text, std::string("Total"));
    QCOMPARE(document.argumentNodes[0].value.text, std::string("Total"));
    QCOMPARE(document.argumentNodes[1].value.text, std::string("udg_Total"));
    QCOMPARE(Table(editor)->item(Table(editor)->currentRow(), 0)->data(Qt::UserRole).toULongLong(), variableId);
    QCOMPARE(Table(editor)->item(0, 0)->text(), QStringLiteral("Total"));

    stack.undo();
    QCOMPARE(document.variables.front().name.text, std::string("Count"));
    QCOMPARE(document.argumentNodes[0].value.text, std::string("Count"));
    QCOMPARE(document.argumentNodes[1].value.text, std::string("udg_Count"));
    QCOMPARE(Table(editor)->item(0, 0)->text(), QStringLiteral("Count"));
    stack.redo();
    QCOMPARE(document.argumentNodes[1].value.text, std::string("udg_Total"));
    QCOMPARE(Table(editor)->item(0, 0)->text(), QStringLiteral("Total"));
    QVERIFY(Button(editor, "renameVariableButton")->isEnabled());
}

void VariablesDialogTests::UnsupportedReferenceRename() {
    // 带数组下标的引用暂不支持同步改名；整个重命名必须保持原子。
    TriggerDocument document;
    QVERIFY(TriggerDocumentEditor::AddVariable(document, "Count", "integer", true).Succeeded());
    ArgumentNode argument;
    argument.id = 201;
    argument.type = 1;
    argument.value.text = "Count";
    argument.insertIndex = 1;
    document.argumentNodes.push_back(argument);
    QUndoStack stack;
    VariablesDialog editor(&document, &stack, nullptr, true);
    editor.show();
    QueueRename(QStringLiteral("Total"), true);
    Button(editor, "renameVariableButton")->click();
    QCOMPARE(stack.count(), 0);
    QCOMPARE(document.variables.front().name.text, std::string("Count"));
    QCOMPARE(document.argumentNodes.front().value.text, std::string("Count"));
    QCOMPARE(document.argumentNodes.front().insertIndex, argument.insertIndex);
    QCOMPARE(Table(editor)->item(0, 0)->text(), QStringLiteral("Count"));
}

void VariablesDialogTests::AddRemoveAndUndoRefreshSelection() {
    TriggerDocument document;
    QVERIFY(TriggerDocumentEditor::AddVariable(document, "Count", "integer", false).Succeeded());
    QUndoStack stack;
    VariablesDialog editor(&document, &stack, nullptr, true);
    editor.show();
    QTimer::singleShot(0, qApp, [] {
        auto* form = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        QVERIFY(form != nullptr);
        const auto inputs = form->findChildren<QLineEdit*>();
        QVERIFY(!inputs.isEmpty());
        inputs.front()->setText(QStringLiteral("Health"));
        form->accept();
    });
    Button(editor, "addVariableButton")->click();
    QCOMPARE(document.variables.size(), std::size_t{2});
    QCOMPARE(Table(editor)->item(Table(editor)->currentRow(), 0)->text(), QStringLiteral("Health"));
    QTimer::singleShot(0, qApp, [] {
        auto* confirm = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        QVERIFY(confirm != nullptr);
        confirm->button(QMessageBox::Yes)->click();
    });
    Button(editor, "removeVariableButton")->click();
    QCOMPARE(document.variables.size(), std::size_t{1});
    QCOMPARE(Table(editor)->rowCount(), 1);
    QCOMPARE(Table(editor)->item(Table(editor)->currentRow(), 0)->text(), QStringLiteral("Count"));
    stack.undo();
    QCOMPARE(document.variables.size(), std::size_t{2});
    QCOMPARE(Table(editor)->rowCount(), 2);
    stack.undo();
    QCOMPARE(document.variables.size(), std::size_t{1});
    QCOMPARE(Table(editor)->rowCount(), 1);
    stack.redo();
    QCOMPARE(Table(editor)->rowCount(), 2);
    QVERIFY(Button(editor, "removeVariableButton")->isEnabled());
}

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    QApplication application(argc, argv);
    VariablesDialogTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "VariablesDialogTests.moc"
