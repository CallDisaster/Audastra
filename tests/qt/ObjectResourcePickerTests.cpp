#include "Widgets/ObjectResourcePicker.h"
#include "ObjectModels.h"
#include "ObjectResourcePickerAdapter.h"
#include "EditorProject.h"
#include "ObjectModule.h"
#include "Fonts/FontLoader.h"
#include "War3MapCore/ObjectData/StandardObjectCatalog.h"
#include "War3MapCore/ObjectData/NativeObjectCodec.h"
#include "War3MapCore/ObjectData/ProfileList.h"
#include <QApplication>
#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QTabWidget>
#include <QTableView>
#include <QTemporaryDir>
#include <QTreeWidget>
#include <QUndoStack>
#include <QtTest>
#include <map>

using namespace War3Map;
using namespace War3Map::Modules;
namespace {
ObjectResourcePickerServices ServicesFor(bool* current, QStringList* previews = nullptr) {
    ObjectResourcePickerServices result; result.revision = "picker:1";
    result.isCurrent = [current] { return *current; };
    result.load = [](Services::ObjectResourceOrigin origin, const auto&) {
        auto catalog = std::make_shared<Services::ObjectResourceCatalog>();
        catalog->revision = "picker:1"; catalog->identity = std::to_string(static_cast<int>(origin)); catalog->origin = origin;
        Services::ObjectResourceCandidate candidate;
        candidate.id = "fixture-model"; candidate.title = "合成模型"; candidate.value = "Basic.mdx";
        candidate.sourceId = "synthetic"; candidate.kind = Services::ObjectResourceKind::Model; candidate.origin = origin;
        catalog->rows.push_back(candidate);
        return Result<std::shared_ptr<const Services::ObjectResourceCatalog>>::Success(std::move(catalog));
    };
    if (previews) result.preview = [previews](QWidget* parent) {
        return ObjectResourcePreviewHandle{new QLabel(QStringLiteral("合成预览记录器"), parent),
            [previews](const QString& value, auto) { previews->push_back(value); }};
    };
    return result;
}
QPushButton* Apply(QWidget& panel) { return panel.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Apply); }
std::vector<std::byte> Bytes(std::string_view value) {
    return {reinterpret_cast<const std::byte*>(value.data()), reinterpret_cast<const std::byte*>(value.data() + value.size())};
}
bool InstallModelDefinitions(QtProject::EditorProject& project) {
    using namespace ObjectData;
    const std::map<std::string, std::string> files{
        {"units.slk", "ID;PWXL\nC;X1;Y1;K\"alias\"\nC;X2;K\"file\"\nC;X1;Y2;K\"u001\"\nC;X2;K\"Basic.mdx\"\nE\n"},
        {"fields.slk", "ID;PWXL\nC;X1;Y1;K\"ID\"\nC;X2;K\"field\"\nC;X3;K\"slk\"\nC;X4;K\"type\"\n"
            "C;X1;Y2;K\"umdl\"\nC;X2;K\"file\"\nC;X3;K\"UnitUI\"\nC;X4;K\"model\"\nE\n"}};
    const auto reader = [&](const std::string& path) {
        const auto found = files.find(path);
        return found != files.end() ? Result<std::vector<std::byte>>::Success(Bytes(found->second))
            : Result<std::vector<std::byte>>::Failure("fixture", "missing", path);
    };
    ObjectFieldSchemaOptions options; options.sources = {{ObjectType::Unit, "fields.slk"}};
    options.stringFiles.clear(); options.worldEditDataFile.clear(); options.unitEditorDataFile.clear();
    auto schema = ObjectFieldSchema::Load(reader, options);
    CatalogLoadOptions catalogOptions; catalogOptions.sources = {{ObjectType::Unit, {"units.slk"}, {}}}; catalogOptions.stringFiles.clear();
    auto catalog = StandardObjectCatalog::Load(reader, catalogOptions);
    if (!schema.Succeeded() || !catalog.Succeeded()) return false;
    project.standardCatalog_ = std::make_shared<const StandardObjectCatalog>(std::move(*catalog.value));
    project.objectFieldSchema_ = std::make_shared<const ObjectFieldSchema>(std::move(*schema.value));
    return true;
}
}
class ObjectResourcePickerTests final : public QObject {
    Q_OBJECT
private slots:
    void SourcesHoverAndKeyboard();
    void OrderedValuesAndUnchangedOriginal();
    void StaleDraftDoesNotCommit();
    void FieldDelegateUsesSameUndoAndSerialization();
    void StandardFieldUsesSameCommand();
    void CommitMayCloseOwner();
    void AdapterOwnsSnapshotAndRejectsOversizedSource();
};
void ObjectResourcePickerTests::SourcesHoverAndKeyboard() {
    bool current = true; QStringList previews;
    auto* picker = new ObjectResourcePicker({Services::ObjectResourceKind::Model, false, true}, "Missing.mdx", ServicesFor(&current, &previews));
    QPointer<ObjectResourcePicker> guard(picker); QString submitted;
    picker->SetCommit([&](const QString& value) { submitted = value; return true; }); picker->show();
    auto* list = picker->findChild<QTreeWidget*>("resourceCandidates");
    auto* tabs = picker->findChild<QTabWidget*>("resourceSourceTabs");
    QTRY_COMPARE(list->topLevelItemCount(), 1);
    auto* item = list->topLevelItem(0);
    QTest::mouseMove(list->viewport(), list->visualItemRect(item).center());
    QTRY_VERIFY(previews.contains(QStringLiteral("Basic.mdx")));
    tabs->setCurrentIndex(1); QTRY_COMPARE(list->topLevelItemCount(), 1);
    list->setCurrentItem(list->topLevelItem(0)); QTest::keyClick(list, Qt::Key_Return);
    QCOMPARE(picker->findChild<QLineEdit*>("resourcePath")->text(), QStringLiteral("Basic.mdx"));
    tabs->setCurrentIndex(2); auto* path = picker->findChild<QLineEdit*>("resourcePath");
    path->setText(QStringLiteral("Custom.mdx")); QTest::keyClick(path, Qt::Key_Return);
    QCOMPARE(submitted, QStringLiteral("Custom.mdx")); QTRY_VERIFY(!guard);
}
void ObjectResourcePickerTests::OrderedValuesAndUnchangedOriginal() {
    bool current = true;
    auto* picker = new ObjectResourcePicker({Services::ObjectResourceKind::Model, true, true},
        "  First.mdx ,\"Second,Part.mdx\"", ServicesFor(&current));
    QPointer<ObjectResourcePicker> guard(picker); int commits = 0;
    picker->SetCommit([&](const QString&) { ++commits; return true; }); picker->show();
    QCOMPARE(picker->Value(), QStringLiteral("  First.mdx ,\"Second,Part.mdx\""));
    QTest::mouseClick(Apply(*picker), Qt::LeftButton); QTRY_VERIFY(!guard); QCOMPARE(commits, 0);
    picker = new ObjectResourcePicker({Services::ObjectResourceKind::Model, true, true}, "First.mdx,Second.mdx", ServicesFor(&current));
    guard = picker;
    picker->show(); auto* list = picker->findChild<QListWidget*>("resourceOrderedValues");
    auto* path = picker->findChild<QLineEdit*>("resourcePath"); path->setText(QStringLiteral("Third.mdx"));
    QTest::mouseClick(picker->findChild<QPushButton*>("resourceAdd"), Qt::LeftButton);
    QTest::mouseClick(picker->findChild<QPushButton*>("resourceMoveUp"), Qt::LeftButton);
    QCOMPARE(picker->Value(), QStringLiteral("First.mdx,Third.mdx,Second.mdx"));
    list->setCurrentRow(0); QTest::mouseClick(picker->findChild<QPushButton*>("resourceRemove"), Qt::LeftButton);
    QCOMPARE(picker->Value(), QStringLiteral("Third.mdx,Second.mdx")); picker->close(); QTRY_VERIFY(!guard);
}
void ObjectResourcePickerTests::StaleDraftDoesNotCommit() {
    bool current = true;
    auto* picker = new ObjectResourcePicker({Services::ObjectResourceKind::Model, false, true}, "Missing.mdx", ServicesFor(&current));
    const QPointer<ObjectResourcePicker> guard(picker);
    int commits = 0; picker->SetCommit([&](const QString&) { ++commits; return true; }); picker->show();
    picker->findChild<QLineEdit*>("resourcePath")->setText(QStringLiteral("Changed.mdx")); current = false;
    QTest::mouseClick(Apply(*picker), Qt::LeftButton); QCOMPARE(commits, 0); QVERIFY(picker->isVisible());
    QVERIFY(picker->findChild<QLabel*>("resourceStatus")->text().contains(QStringLiteral("工程已变化"))); picker->close(); QTRY_VERIFY(!guard);
}
void ObjectResourcePickerTests::FieldDelegateUsesSameUndoAndSerialization() {
    using namespace ObjectData;
    QtProject::EditorProject project; QVERIFY(InstallModelDefinitions(project));
    auto parsed = LniObjectCodec(ObjectType::Unit).Parse(Bytes("[u001]\n_parent = \"u001\"\nfile = \"Missing.mdx\"\n"));
    QVERIFY(parsed.Succeeded()); auto* document = dynamic_cast<LniObjectDocument*>(parsed.document.get()); QVERIFY(document);
    const ObjectKey object{ObjectType::Unit, "u001"}; QUndoStack undo; ObjectFieldModel model;
    model.SetUndoStack(&undo); model.SetContext(project.standardCatalog_.get(), project.objectFieldSchema_.get()); model.SetDocument(document, &object);
    int row = -1; for (int i = 0; i < model.rowCount(); ++i) if (model.DefinitionAt(i) && model.DefinitionAt(i)->fieldId == "umdl") row = i;
    QVERIFY(row >= 0); const auto index = model.index(row, 3); QVERIFY(index.flags() & Qt::ItemIsEditable);
    QTableView view; view.setModel(&model); auto* delegate = new ObjectFieldDelegate(&view); view.setItemDelegate(delegate);
    bool current = true; delegate->SetResourcePickerFactory([&] { return ServicesFor(&current); });
    view.resize(760, 480); view.show(); view.setCurrentIndex(index); QTest::keyClick(&view, Qt::Key_Return);
    QTRY_VERIFY(view.findChild<ObjectResourcePicker*>());
    auto* picker = view.findChild<ObjectResourcePicker*>(); QPointer<ObjectResourcePicker> guard(picker);
    picker->findChild<QLineEdit*>("resourcePath")->setText(QStringLiteral("Changed.mdx")); QTest::mouseClick(Apply(*picker), Qt::LeftButton);
    QTRY_VERIFY(!guard); QCOMPARE(undo.count(), 1);
    QCOMPARE(model.data(model.index(row, 3), Qt::EditRole).toString(), QStringLiteral("Changed.mdx"));
    auto saved = LniObjectCodec(ObjectType::Unit).Serialize(*document); QVERIFY(saved.Succeeded());
    auto readback = LniObjectCodec(ObjectType::Unit).Parse(*saved.value); QVERIFY(readback.Succeeded());
    const auto* recovered = dynamic_cast<LniObjectDocument*>(readback.document.get()); QVERIFY(recovered);
    QCOMPARE(QString::fromStdString(ValueToDisplayString(recovered->Objects().front().modifications.front().value)), QStringLiteral("Changed.mdx"));
    undo.undo(); QCOMPARE(model.data(model.index(row, 3), Qt::EditRole).toString(), QStringLiteral("Missing.mdx"));
    undo.redo(); QCOMPARE(model.data(model.index(row, 3), Qt::EditRole).toString(), QStringLiteral("Changed.mdx"));
}
void ObjectResourcePickerTests::StandardFieldUsesSameCommand() {
    using namespace ObjectData;
    QTemporaryDir workspace; QVERIFY(workspace.isValid());
    QtProject::EditorProject project; QVERIFY(InstallModelDefinitions(project));
    project.projectRoot_ = workspace.path().toStdWString(); project.sourceArchivePath_ = project.projectRoot_ / "synthetic.w3x";
    auto empty = LniObjectCodec(ObjectType::Unit).Parse({}); QVERIFY(empty.Succeeded());
    project.objectDocuments_[0].reset(dynamic_cast<LniObjectDocument*>(empty.document.release()));
    auto native = NativeObjectDocument::CreateEmpty(ObjectType::Unit, 2); QVERIFY(native.Succeeded());
    project.nativeObjectDocuments_[0] = std::make_unique<NativeObjectDocument>(std::move(*native.value));
    for (auto& stack : project.objectUndoStacks_) stack = new QUndoStack(&project);
    bool current = true; ObjectModule module(project);
    module.SetResourcePickerFactory([&] { return ServicesFor(&current); });
    auto* page = module.Widget(); page->resize(1200, 780); page->show(); module.UpdateObjectReferenceEntries();
    Shell::NavigationTarget target; target.module = "objects"; target.kind = "object"; target.id = "unit:u001";
    target.extra.insert("source", "standard"); QVERIFY(module.Reveal(target));
    auto* view = page->findChild<QTableView*>("standardObjectFields"); QVERIFY(view);
    int row = -1;
    for (int i = 0; i < view->model()->rowCount(); ++i)
        if (view->model()->index(i, 0).data(Qt::UserRole).toString() == "file") row = i;
    QVERIFY(row >= 0); const auto index = view->model()->index(row, 1);
    view->setCurrentIndex(index); page->activateWindow(); view->setFocus();
    QTRY_VERIFY(view->hasFocus()); QTest::keyClick(view, Qt::Key_Return);
    QTRY_VERIFY(page->findChild<ObjectResourcePicker*>());
    auto* picker = page->findChild<ObjectResourcePicker*>(); QPointer<ObjectResourcePicker> guard(picker);
    picker->findChild<QLineEdit*>("resourcePath")->setText(QStringLiteral("StandardOverride.mdx"));
    QTest::mouseClick(Apply(*picker), Qt::LeftButton); QTRY_VERIFY(!guard);
    QCOMPARE(project.objectUndoStacks_[0]->count(), 1);
    const auto* object = project.objectDocuments_[0]->FindObject({ObjectType::Unit, "u001"}); QVERIFY(object);
    QCOMPARE(ValueToDisplayString(object->modifications.front().value), std::string("StandardOverride.mdx"));
    QCOMPARE(project.standardCatalog_->Find(ObjectType::Unit, "u001")->Field("file"), std::string_view("Basic.mdx"));
    project.objectUndoStacks_[0]->undo(); QVERIFY(!project.objectDocuments_[0]->FindObject({ObjectType::Unit, "u001"}));
    project.objectUndoStacks_[0]->redo(); QVERIFY(project.objectDocuments_[0]->FindObject({ObjectType::Unit, "u001"}));
    auto saved = LniObjectCodec(ObjectType::Unit).Serialize(*project.objectDocuments_[0]); QVERIFY(saved.Succeeded());
    auto readback = LniObjectCodec(ObjectType::Unit).Parse(*saved.value); QVERIFY(readback.Succeeded());
    const auto* recovered = dynamic_cast<LniObjectDocument*>(readback.document.get()); QVERIFY(recovered);
    QCOMPARE(ValueToDisplayString(recovered->Objects().front().modifications.front().value), std::string("StandardOverride.mdx"));
}
void ObjectResourcePickerTests::CommitMayCloseOwner() {
    bool current = true; auto* owner = new QWidget;
    auto* picker = new ObjectResourcePicker({Services::ObjectResourceKind::Model, false, true}, "Before.mdx", ServicesFor(&current), owner);
    const QPointer<ObjectResourcePicker> guard(picker);
    picker->SetCommit([owner](const QString&) { delete owner; return true; }); picker->show();
    picker->findChild<QLineEdit*>("resourcePath")->setText(QStringLiteral("After.mdx"));
    QTest::mouseClick(Apply(*picker), Qt::LeftButton); QVERIFY(!guard);
}
void ObjectResourcePickerTests::AdapterOwnsSnapshotAndRejectsOversizedSource() {
    using namespace ObjectData;
    auto project = std::make_unique<QtProject::EditorProject>(); QVERIFY(InstallModelDefinitions(*project));
    auto parsed = LniObjectCodec(ObjectType::Unit).Parse(Bytes("[u001]\nfile = \"Frozen.mdx\"\n")); QVERIFY(parsed.Succeeded());
    project->objectDocuments_[0].reset(dynamic_cast<LniObjectDocument*>(parsed.document.release()));
    auto factory = QtApp::MakeObjectResourcePickerFactory(*project); auto services = factory();
    QVERIFY(services.isCurrent()); project.reset(); QVERIFY(!services.isCurrent());
    auto catalog = services.load(Services::ObjectResourceOrigin::Object, [](std::size_t) { return true; }); QVERIFY(catalog.Succeeded());
    QVERIFY(std::any_of((*catalog.value)->rows.begin(), (*catalog.value)->rows.end(), [](const auto& row) { return row.value == "Frozen.mdx"; }));
    project = std::make_unique<QtProject::EditorProject>(); QVERIFY(InstallModelDefinitions(*project));
    std::string excessive;
    for (int i = 0; i < 32769; ++i) excessive += '[' + QString::number(i, 16).rightJustified(4, '0').toStdString() + "]\n";
    parsed = LniObjectCodec(ObjectType::Unit).Parse(Bytes(excessive)); QVERIFY(parsed.Succeeded());
    project->objectDocuments_[0].reset(dynamic_cast<LniObjectDocument*>(parsed.document.release()));
    factory = QtApp::MakeObjectResourcePickerFactory(*project); services = factory();
    catalog = services.load(Services::ObjectResourceOrigin::Object, [](std::size_t) { return true; }); QVERIFY(!catalog.Succeeded());
    QCOMPARE(catalog.diagnostics.front().code, std::string("OBJECT_RESOURCE_SNAPSHOT_LIMIT"));
}
int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen"); QApplication app(argc, argv);
    UiKit::ApplyApplicationFont(UiKit::LoadFonts(UiKit::DefaultFontDirectory()));
    ObjectResourcePickerTests tests; return QTest::qExec(&tests, argc, argv);
}
#include "ObjectResourcePickerTests.moc"
