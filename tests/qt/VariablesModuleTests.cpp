#include "VariablesModule.h"
#include "EditorProject.h"
#include "TriggerCommands.h"
#include "Shell/ShellWindow.h"
#include "Modules/ModuleRegistry.h"
#include "War3MapCore/Trigger/TriggerCodec.h"
#include "War3MapCore/Trigger/TriggerEditing.h"
#include "War3MapCore/Trigger/UiCatalog.h"
#include "War3MapCore/ObjectData/LniObjectCodec.h"
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>
#include <QDir>
#include <QFile>
#include <QScrollBar>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QUndoStack>
#include <algorithm>
#include <functional>
#include <memory>

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

// 依次处理之后弹出的模态表单（同一表单被重新打开也算下一步）；每步拿到当时可见的表单。
// 步骤用完后还出现的表单记为失败并关掉（附上表单里的诊断），测试报告失败而不是卡在模态循环里。
// 返回的处理器只在调用方持有期间有效，各次点击互不干扰。
[[nodiscard]] std::unique_ptr<QTimer> OnForms(std::vector<std::function<void(QDialog&)>> steps) {
    auto timer = std::make_unique<QTimer>();
    auto queue = std::make_shared<std::vector<std::function<void(QDialog&)>>>(std::move(steps));
    auto next = std::make_shared<std::size_t>(0);
    QObject::connect(timer.get(), &QTimer::timeout, timer.get(), [queue, next] {
        auto* form = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (form == nullptr || !form->isVisible()) return;
        const QPointer<QDialog> guard(form);
        if (*next >= queue->size()) {
            const auto* problem = form->findChild<QLabel*>("formProblem");
            QTest::qFail(qPrintable(QStringLiteral("未预期的表单：%1").arg(problem ? problem->text() : form->windowTitle())),
                __FILE__, __LINE__);
            form->reject();
            return;
        }
        (*queue)[(*next)++](*form);
        if (QTest::currentTestFailed() && guard && guard->isVisible()) guard->reject();
    });
    timer->start(5);
    return timer;
}

void Choose(QDialog& form, const char* name, const QString& value) {
    auto* box = form.findChild<QComboBox*>(QString::fromLatin1(name));
    QVERIFY(box != nullptr);
    const int index = box->findData(value);
    QVERIFY2(index >= 0, qPrintable(value));
    box->setCurrentIndex(index);
}

void Confirm(QDialog& form) {
    auto* buttons = form.findChild<QDialogButtonBox*>();
    QVERIFY(buttons != nullptr && buttons->button(QDialogButtonBox::Ok) != nullptr);
    buttons->button(QDialogButtonBox::Ok)->click();
}
}

class VariablesModuleTests final : public QObject {
    Q_OBJECT
private slots:
    void RealModuleMovesWithoutRebuildingAndRoutesUndo();
    void DocumentReplacementDropsOldBindings();
    void ViewStateRestoresSelection();
    void InitialObjectNamesRefreshAfterObjectUndoAndReplacement();
    void PropertiesFormPublishesTypedChangesAsOneUndo();
};

void VariablesModuleTests::PropertiesFormPublishesTypedChangesAsOneUndo() {
    QTemporaryDir ui;
    QVERIFY(ui.isValid());
    QVERIFY(QDir{}.mkpath(ui.filePath("ui/base")));
    const auto write = [&](const char* path, const char* text) {
        QFile file(ui.filePath(QString::fromLatin1(path)));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(text);
    };
    write("ui/config", "base\n");
    write("ui/base/define.txt", "[TriggerTypes]\ninteger=0,1,1,整数,integer\nboolean=0,1,1,布尔值,boolean\n"
        "player=0,1,1,玩家,handle\nunitcode=0,1,1,单位类型,integer\ncode=0,0,0,代码,code\n"
        "[TriggerParams]\nPlayer00=0,player,Player(0),玩家1（红色）\n");
    auto loaded = Trigger::UiCatalog::Load(ui.path().toStdWString());
    QVERIFY(loaded.Succeeded());
    QtProject::EditorProject project;
    project.triggerUndoStack_ = new QUndoStack(&project);
    project.uiCatalog_ = std::make_unique<Trigger::UiCatalog>(std::move(*loaded.value));
    // 共用图编辑只接受经真实编解码器建立来源的文档：原创空文档加一个变量后写出再读回。
    const Trigger::WtgWctCodec codec;
    auto empty = codec.CreateEmpty(*project.uiCatalog_);
    QVERIFY(empty.Succeeded());
    QVERIFY(Trigger::TriggerDocumentEditor::AddVariable(*empty.value, "Count", "integer", false).Succeeded());
    const auto encoded = codec.Serialize(*empty.value, *project.uiCatalog_);
    QVERIFY(encoded.Succeeded());
    auto sealed = codec.Parse(encoded.value->wtg, encoded.value->wct, *project.uiCatalog_);
    QVERIFY(sealed.Succeeded());
    project.ReplaceTriggerDocument(std::make_unique<Trigger::TriggerDocument>(std::move(*sealed.value)));
    Modules::VariablesModule module(project);
    auto* page = module.Widget();
    auto* table = page->findChild<QTableWidget*>("globalVariablesTable");
    auto* properties = page->findChild<QPushButton*>("variablePropertiesButton");
    auto* add = page->findChild<QPushButton*>("addVariableButton");
    QVERIFY(table && properties && add);
    table->selectRow(0);
    QVERIFY(properties->isEnabled());

    // 改类型并设初值：一次发布、一条撤销；类型只列可声明为全局变量的类型。
    bool listedCode = true;
    auto forms = OnForms({[&](QDialog& form) {
        auto* type = form.findChild<QComboBox*>("variableType");
        QVERIFY(type != nullptr);
        listedCode = type->findData(QStringLiteral("code")) >= 0;
        QVERIFY(form.findChild<QLineEdit*>("variableName")->isReadOnly());
        Choose(form, "variableType", QStringLiteral("boolean"));
        form.findChild<QCheckBox*>("variableInitialized")->setChecked(true);
        Choose(form, "variableInitialValue", QStringLiteral("true"));
        Confirm(form);
    }});
    properties->click();
    QVERIFY(!listedCode);
    const auto& count = project.triggerDocument_->variables.front();
    QCOMPARE(count.typeName.text, std::string("boolean"));
    QCOMPARE(count.hasDefault, 1);
    QCOMPARE(count.value.text, std::string("true"));
    QCOMPARE(project.triggerUndoStack_->count(), 1);
    QCOMPARE(table->item(0, 1)->text(), QStringLiteral("布尔值"));
    page->findChild<QCheckBox*>("variableRawValues")->setChecked(true);
    QCOMPARE(table->item(0, 1)->text(), QStringLiteral("boolean"));
    page->findChild<QCheckBox*>("variableRawValues")->setChecked(false);

    // 新建：数组大小与预设初值同一个表单。
    forms = OnForms({[&](QDialog& form) {
        form.findChild<QLineEdit*>("variableName")->setText(QStringLiteral("Owner"));
        Choose(form, "variableType", QStringLiteral("player"));
        form.findChild<QCheckBox*>("variableArray")->setChecked(true);
        form.findChild<QSpinBox*>("variableArraySize")->setValue(12);
        form.findChild<QCheckBox*>("variableInitialized")->setChecked(true);
        Choose(form, "variableInitialValue", QStringLiteral("Player00"));
        Confirm(form);
    }});
    add->click();
    QCOMPARE(project.triggerDocument_->variables.size(), std::size_t{2});
    const auto& owner = project.triggerDocument_->variables.back();
    QCOMPARE(owner.name.text, std::string("Owner"));
    QCOMPARE(owner.typeName.text, std::string("player"));
    QCOMPARE(owner.isArray, 1);
    QCOMPARE(owner.size, 12);
    QCOMPARE(owner.value.text, std::string("Player00"));
    QCOMPARE(project.triggerUndoStack_->count(), 2);

    // 不合法的字面值由共用批次拒绝：表单留着并显示原因，文档与撤销栈不变。
    table->selectRow(0);
    bool problemShown = false;
    forms = OnForms({[&](QDialog& form) {
        Choose(form, "variableType", QStringLiteral("integer"));
        form.findChild<QCheckBox*>("variableInitialized")->setChecked(true);
        form.findChild<QComboBox*>("variableInitialValue")->setEditText(QStringLiteral("abc"));
        Confirm(form);
    }, [&](QDialog& form) {
        auto* problem = form.findChild<QLabel*>("formProblem");
        problemShown = problem && problem->isVisible() && !problem->text().isEmpty();
        form.reject();
    }});
    properties->click();
    QVERIFY(problemShown);
    QCOMPARE(project.triggerDocument_->variables.front().typeName.text, std::string("boolean"));
    QCOMPARE(project.triggerUndoStack_->count(), 2);

    project.triggerUndoStack_->undo();
    project.triggerUndoStack_->undo();
    QCOMPARE(project.triggerDocument_->variables.size(), std::size_t{1});
    QCOMPARE(project.triggerDocument_->variables.front().typeName.text, std::string("integer"));
    QCOMPARE(project.triggerDocument_->variables.front().hasDefault, 0);
}

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
