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
    void MultiSelectRemoveIsOneUndo();
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

void VariablesDialogTests::MultiSelectRemoveIsOneUndo() {
    TriggerDocument document;
    for (const char* name : {"Alpha", "Beta", "Gamma", "Delta"})
        QVERIFY(TriggerDocumentEditor::AddVariable(document, name, "integer", false).Succeeded());
    const auto betaId = document.variables[1].id;
    QUndoStack stack;
    VariablesDialog editor(&document, &stack, nullptr, true);
    editor.show();
    auto* table = Table(editor);
    QVERIFY(table != nullptr);
    QCOMPARE(table->selectionMode(), QAbstractItemView::ExtendedSelection);
    // Ctrl 多选第 1、3 行；当前行驱动单项按钮。
    auto* selection = table->selectionModel();
    selection->setCurrentIndex(table->model()->index(0, 0), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    selection->setCurrentIndex(table->model()->index(2, 0), QItemSelectionModel::Select | QItemSelectionModel::Rows);
    QCOMPARE(selection->selectedRows().size(), qsizetype{2});
    QVERIFY(Button(editor, "renameVariableButton")->isEnabled());
    QVERIFY(Button(editor, "removeVariableButton")->isEnabled());
    QString question;
    QTimer::singleShot(0, qApp, [&question] {
        auto* confirm = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        QVERIFY(confirm != nullptr);
        question = confirm->text();
        confirm->button(QMessageBox::Yes)->click();
    });
    Button(editor, "removeVariableButton")->click();
    QVERIFY(question.contains(QStringLiteral("2 个变量")));
    QCOMPARE(stack.count(), 1);
    QCOMPARE(document.variables.size(), std::size_t{2});
    QCOMPARE(document.variables[0].name.text, std::string("Beta"));
    QCOMPARE(document.variables[1].name.text, std::string("Delta"));
    stack.undo();
    QCOMPARE(document.variables.size(), std::size_t{4});
    QCOMPARE(document.variables[0].name.text, std::string("Alpha"));
    QCOMPARE(document.variables[2].name.text, std::string("Gamma"));
    QCOMPARE(table->rowCount(), 4);
    stack.redo();
    QCOMPARE(document.variables.size(), std::size_t{2});
    stack.undo();
    // 刷新后按稳定 ID 保留多选与当前行。
    selection->setCurrentIndex(table->model()->index(1, 0), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    selection->setCurrentIndex(table->model()->index(3, 0), QItemSelectionModel::Select | QItemSelectionModel::Rows);
    QVERIFY(TriggerDocumentEditor::AddVariable(document, "Epsilon", "integer", false).Succeeded());
    stack.push(new QUndoCommand(QStringLiteral("刷新")));
    QCOMPARE(table->rowCount(), 5);
    QCOMPARE(table->selectionModel()->selectedRows().size(), qsizetype{2});
    QCOMPARE(table->item(table->currentRow(), 0)->text(), QStringLiteral("Delta"));
    QVERIFY(table->selectionModel()->isRowSelected(1, {}));
    QCOMPARE(table->item(1, 0)->data(Qt::UserRole).toULongLong(), betaId);
}

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    QApplication application(argc, argv);
    VariablesDialogTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "VariablesDialogTests.moc"
