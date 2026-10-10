#include "VariablesModule.h"
#include "EditorProject.h"
#include "TriggerCommands.h"
#include "Shell/ShellWindow.h"
#include "Modules/ModuleRegistry.h"
#include "War3MapCore/Trigger/TriggerEditing.h"
#include "War3MapCore/Trigger/UiCatalog.h"
#include "War3MapCore/ObjectData/LniObjectCodec.h"
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QDir>
#include <QFile>
#include <QScrollBar>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QUndoStack>

using namespace War3Map;

namespace {
std::unique_ptr<Trigger::TriggerDocument> Document(const std::string& prefix, int count) {
    auto document = std::make_unique<Trigger::TriggerDocument>();
    for (int i = 0; i < count; ++i) {
        if (!Trigger::TriggerDocumentEditor::AddVariable(*document,
            prefix + std::to_string(i), "integer", false).Succeeded()) return {};
    }
    return document;
}
}

class VariablesModuleTests final : public QObject {
    Q_OBJECT
private slots:
    void RealModuleMovesWithoutRebuildingAndRoutesUndo();
    void DocumentReplacementDropsOldBindings();
    void ViewStateRestoresSelection();
    void InitialObjectNamesRefreshAfterObjectUndoAndReplacement();
};

void VariablesModuleTests::InitialObjectNamesRefreshAfterObjectUndoAndReplacement() {
    QtProject::EditorProject project;
    project.triggerUndoStack_ = new QUndoStack(&project);
    project.ReplaceTriggerDocument(Document("Value", 1));
    auto& variable = project.triggerDocument_->variables.front();
    variable.typeName.text = "unitcode";
    variable.hasDefault = 1;
    variable.value.text = "H001";
    const auto units = [](std::string_view name) {
        const auto text = "[H001]\n_parent=\"hfoo\"\nunam=\"" + std::string(name) + "\"\n";
        auto parsed = ObjectData::LniObjectCodec(ObjectData::ObjectType::Unit).Parse(std::as_bytes(std::span(text.data(), text.size())));
        return parsed.Succeeded() ? std::unique_ptr<ObjectData::LniObjectDocument>(
            dynamic_cast<ObjectData::LniObjectDocument*>(parsed.document.release())) : nullptr;
    };
    project.objectDocuments_[0] = units("旧名字");
    QVERIFY(project.objectDocuments_[0]);
    if (!project.objectUndoStacks_[0]) project.objectUndoStacks_[0] = new QUndoStack(&project);
    Modules::VariablesModule module(project);
    auto* page = module.Widget();
    auto* table = page->findChild<QTableWidget*>("globalVariablesTable");
    auto* raw = page->findChild<QCheckBox*>("variableRawValues");
    QVERIFY(table && raw);
    QCOMPARE(table->item(0, 4)->text(), QStringLiteral("旧名字"));

    class Rename final : public QUndoCommand {
    public:
        explicit Rename(ObjectData::LniObjectDocument& document) : document_(document) {}
        void redo() override { succeeded = document_.SetModification({ObjectData::ObjectType::Unit, "H001"}, {"unam"}, std::string("新名字")).Succeeded(); }
        void undo() override { succeeded = document_.SetModification({ObjectData::ObjectType::Unit, "H001"}, {"unam"}, std::string("旧名字")).Succeeded(); }
        bool succeeded = false;
    private:
        ObjectData::LniObjectDocument& document_;
    };
    auto* rename = new Rename(*project.objectDocuments_[0]);
    project.objectUndoStacks_[0]->push(rename);
    QVERIFY(rename->succeeded);
    QCOMPARE(table->item(0, 4)->text(), QStringLiteral("新名字"));
    project.objectUndoStacks_[0]->undo();
    QVERIFY(rename->succeeded);
    QCOMPARE(table->item(0, 4)->text(), QStringLiteral("旧名字"));
    project.objectUndoStacks_[0]->redo();
    QCOMPARE(table->item(0, 4)->text(), QStringLiteral("新名字"));
    raw->setChecked(true);
    QCOMPARE(table->item(0, 4)->text(), QStringLiteral("H001"));
    raw->setChecked(false);
    QCOMPARE(table->item(0, 4)->text(), QStringLiteral("新名字"));
    project.objectUndoStacks_[0]->clear();
    project.objectDocuments_[0] = units("替换后的名字");
    emit project.ObjectDocumentsReplaced();
    QCOMPARE(table->item(0, 4)->text(), QStringLiteral("替换后的名字"));
    QTemporaryDir ui;
    QVERIFY(ui.isValid());
    QVERIFY(QDir{}.mkpath(ui.filePath("ui/base")));
    QFile config(ui.filePath("ui/config"));
    QVERIFY(config.open(QIODevice::WriteOnly));
    config.write("base\n");
    config.close();
    const auto aliasCatalog = [&](const QString& base) -> std::unique_ptr<Trigger::UiCatalog> {
        QFile definitions(ui.filePath("ui/base/define.txt"));
        if (!definitions.open(QIODevice::WriteOnly)) return {};
        definitions.write(QStringLiteral("[TriggerTypes]\nunitcode=0,1,1,单位类型,integer\nUnitAlias=0,1,1,自定义单位类型,%1\n").arg(base).toUtf8());
        definitions.close();
        auto loaded = Trigger::UiCatalog::Load(ui.path().toStdWString());
        return loaded.Succeeded() ? std::make_unique<Trigger::UiCatalog>(std::move(*loaded.value)) : nullptr;
    };
    variable.typeName.text = "UnitAlias";
    project.uiCatalog_ = aliasCatalog("unitcode");
    QVERIFY(project.uiCatalog_);
    emit project.UiLibrariesChanged({"base"}, true);
    QCOMPARE(table->item(0, 4)->text(), QStringLiteral("替换后的名字"));
    project.uiCatalog_ = aliasCatalog("integer");
    QVERIFY(project.uiCatalog_);
    emit project.UiLibrariesChanged({"base"}, true);
    QCOMPARE(table->item(0, 4)->text(), QStringLiteral("H001"));
    project.uiCatalog_ = aliasCatalog("unitcode");
    QVERIFY(project.uiCatalog_);
    emit project.UiLibrariesChanged({"base"}, true);
    QCOMPARE(table->item(0, 4)->text(), QStringLiteral("替换后的名字"));
    QCOMPARE(variable.value.text, std::string("H001"));
    QCOMPARE(project.triggerUndoStack_->count(), 0);
}

void VariablesModuleTests::RealModuleMovesWithoutRebuildingAndRoutesUndo() {
    QtProject::EditorProject project;
    QUndoStack stack;
    project.triggerUndoStack_ = &stack;
    project.ReplaceTriggerDocument(Document("Count", 80));
    QVERIFY(project.triggerDocument_ != nullptr);
    Shell::ModuleRegistry registry;
    auto module = std::make_unique<Modules::VariablesModule>(project);
    auto* variables = module.get();
    QVERIFY(registry.Register(std::move(module)));
    QPointer<QWidget> widget;
    {
        Shell::ShellWindow shell(registry, nullptr);
        shell.resize(1100, 750);
        shell.show();
        shell.ActivateModule("variables");
        widget = variables->Widget();
        auto* table = widget->findChild<QTableWidget*>("globalVariablesTable");
        QVERIFY(table != nullptr);
        QVERIFY(variables->Reveal({"variables", "variable", "Count45", {}}));
        const auto selected = variables->SaveViewState().value("selected");
        const auto originalScroll = table->verticalScrollBar()->value();
        variables->Refresh();
        QCOMPARE(widget->findChild<QTableWidget*>("globalVariablesTable"), table);
        QCOMPARE(table->verticalScrollBar()->value(), originalScroll);
        shell.PlaceModule("variables", Shell::Placement::Float(30, 30));
        shell.PlaceModule("variables", Shell::Placement::Side());
        shell.PlaceModule("variables", Shell::Placement::Main());
        QCOMPARE(variables->Widget(), widget.data());
        QCOMPARE(widget->findChild<QTableWidget*>("globalVariablesTable"), table);
        QCOMPARE(variables->SaveViewState().value("selected"), selected);
        QVERIFY(Trigger::TriggerDocumentEditor::RenameVariable(*project.triggerDocument_, "Count45", "Renamed").Succeeded());
        stack.push(new RenameVariableUndoCommand(project.triggerDocument_.get(), "Count45", "Renamed", "Rename"));
        shell.ActivateModule("variables");
        QVERIFY(shell.UndoAction()->isEnabled());
        shell.UndoAction()->trigger();
        QCOMPARE(project.triggerDocument_->variables[45].name.text, std::string("Count45"));
        shell.RedoAction()->trigger();
        QCOMPARE(project.triggerDocument_->variables[45].name.text, std::string("Renamed"));
    }
    QVERIFY(widget); // Shell 只交还控件，实际模块仍持有它。
    QCOMPARE(widget->parentWidget(), nullptr);
    variables->Refresh(); // 已解绑宿主，可以独立刷新。
    stack.clear();
}

void VariablesModuleTests::DocumentReplacementDropsOldBindings() {
    QtProject::EditorProject project;
    QUndoStack stack;
    project.triggerUndoStack_ = &stack;
    project.ReplaceTriggerDocument(Document("Old", 3));
    Modules::VariablesModule module(project);
    auto* page = module.Widget();
    QPointer<QTableWidget> old = page->findChild<QTableWidget*>("globalVariablesTable");
    QVERIFY(old);
    project.ReplaceTriggerDocument(Document("New", 2));
    QVERIFY(old.isNull());
    auto* table = page->findChild<QTableWidget*>("globalVariablesTable");
    QCOMPARE(table->rowCount(), 2);
    QCOMPARE(table->item(0, 0)->text(), QStringLiteral("New0"));
    QVERIFY(!module.Reveal({"variables", "variable", "Old0", {}}));
    QVERIFY(module.Reveal({"variables", "variable", "New1", {}}));
    project.ReplaceTriggerDocument(nullptr);
    QVERIFY(page->findChild<QTableWidget*>("globalVariablesTable") == nullptr);
    QCOMPARE(module.Widget(), page);
}

void VariablesModuleTests::ViewStateRestoresSelection() {
    QtProject::EditorProject project;
    QUndoStack stack;
    project.triggerUndoStack_ = &stack;
    project.ReplaceTriggerDocument(Document("Value", 8));
    Modules::VariablesModule first(project);
    const auto stableTarget = QString::number(project.triggerDocument_->variables[1].id);
    project.triggerDocument_->variables.front().name.text = stableTarget.toStdString();
    first.Refresh();
    QVERIFY(first.Reveal({"variables", "variable", stableTarget, {{"stableIdentity", true}}}));
    QCOMPARE(first.SaveViewState().value("selected").toString(), stableTarget);
    QVERIFY(!first.Reveal({"variables", "variable", stableTarget, {{"stableIdentity", 1}}}));
    project.triggerDocument_->variables.front().name.text = "Value0";
    first.Refresh();
    QVERIFY(first.Reveal({"variables", "variable", "Value6", {}}));
    const auto saved = first.SaveViewState();
    Modules::VariablesModule second(project);
    second.RestoreViewState(saved);
    (void)second.Widget();
    QCOMPARE(second.SaveViewState().value("selected"), saved.value("selected"));
    QCOMPARE(second.UndoStack(), &stack);
    QVERIFY(!second.Reveal({"objects", "variable", "Value6", {}}));
}

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    VariablesModuleTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "VariablesModuleTests.moc"
