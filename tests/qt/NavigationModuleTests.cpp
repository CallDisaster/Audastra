#include "TriggerModule.h"
#include "TriggerLuaScripts.h"
#include "TriggerBlueprint.h"
#include "VariablesModule.h"
#include "ObjectModule.h"
#include "EditorProject.h"
#include "Modules/ModuleHost.h"
#include "Widgets/CodeView.h"
#include "Widgets/ObjectTreeBrowser.h"
#include "War3MapCore/Trigger/TriggerCodec.h"
#include "War3MapCore/Trigger/TriggerEditing.h"
#include "War3MapCore/Trigger/UiCatalog.h"
#include "War3MapCore/ObjectData/LniObjectCodec.h"
#include "War3MapCore/ObjectData/StandardObjectCatalog.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/Workspace.h"

#include <QApplication>
#include <QCheckBox>
#include <QDir>
#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRectF>
#include <QScrollBar>
#include <QSettings>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTabWidget>
#include <QTest>
#include <QTextCursor>
#include <QUndoStack>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

using namespace War3Map;
namespace {
using RestoreResult = Shell::NavigationRestoreResult;
class Host final : public Shell::ModuleHost {
public:
    std::uint64_t epoch = 1;
    void Navigate(const Shell::NavigationTarget&) override {}
    void ActivateModule(const Shell::ModuleId&) override {}
    void ShowStatusMessage(const QString&, int) override {}
    void UndoStackChanged(const Shell::ModuleId&) override {}
    std::uint64_t NavigationEpoch() const override { return epoch; }
};
struct Bytes {
    std::vector<std::byte> data;
    void I32(std::int32_t value) {
        for (int shift = 0; shift < 32; shift += 8)
            data.push_back(static_cast<std::byte>((static_cast<std::uint32_t>(value) >> shift) & 0xffu));
    }
    void Text(std::string_view text) {
        for (const char c : text) data.push_back(static_cast<std::byte>(c));
        data.push_back(std::byte{});
    }
};
bool Write(const QString& path, const QByteArray& bytes) {
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
    QFile file(path); return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
bool InstallTriggers(QtProject::EditorProject& project, const QTemporaryDir& root, bool large = false) {
    if (!Write(root.filePath("ui/config"), "base\n")
        || !Write(root.filePath("ui/base/define.txt"), "[TriggerTypes]\ninteger=0,1,1,整数\n")
        || !Write(root.filePath("ui/base/action.txt"), "[PlainAction]\ndescription=原创动作\n")) return false;
    auto catalog = Trigger::UiCatalog::Load(root.path().toStdWString());
    if (!catalog.Succeeded() || !catalog.value) return false;
    Bytes w;
    for (const char c : std::string_view("WTG!")) w.data.push_back(static_cast<std::byte>(c));
    w.I32(7); w.I32(1); w.I32(37); w.Text("Owned category"); w.I32(0);
    w.I32(2); w.I32(0); w.I32(2);
    for (int row = 0; row < 2; ++row) {
        w.Text(row == 0 ? "First" : "Second"); w.Text("");
        w.I32(0); w.I32(1); w.I32(0); w.I32(0); w.I32(0); w.I32(37);
        w.I32(row == 0 ? 1 : 0);
        if (row == 0) { w.I32(2); w.Text("PlainAction"); w.I32(1); w.I32(0); }
    }
    const auto header = large ? "//" + std::string(2 * 1024 * 1024 + 100, 'x') + "\n"
        : std::string("// owned navigation fixture\n// another line\n");
    Bytes c; c.I32(1); c.Text(""); c.I32(static_cast<std::int32_t>(header.size() + 1));
    c.Text(header); c.I32(2); c.I32(0); c.I32(0);
    auto parsed = Trigger::WtgWctCodec{}.Parse(w.data, c.data, *catalog.value);
    if (!parsed.Succeeded() || !parsed.value) return false;
    project.uiCatalog_ = std::make_unique<Trigger::UiCatalog>(std::move(*catalog.value));
    project.triggerDocument_ = std::make_unique<Trigger::TriggerDocument>(std::move(*parsed.value));
    project.triggerUndoStack_ = new QUndoStack(&project);
    return true;
}
bool InstallObjects(QtProject::EditorProject& project) {
    const std::string slk = "ID;PWXL\nC;X1;Y1;K\"alias\"\nC;X2;K\"Name\"\n"
        "C;X1;Y2;K\"hfoo\"\nC;X2;K\"Owned footman\"\nE\n";
    const auto reader = [slk](const std::string& path) {
        if (path != "owned-unit.slk") return Result<std::vector<std::byte>>::Failure("test", "missing", path);
        const auto bytes = std::as_bytes(std::span(slk.data(), slk.size()));
        return Result<std::vector<std::byte>>::Success({bytes.begin(), bytes.end()});
    };
    ObjectData::CatalogLoadOptions options;
    options.sources = {{ObjectData::ObjectType::Unit, {"owned-unit.slk"}, {}}}; options.stringFiles.clear();
    auto catalog = ObjectData::StandardObjectCatalog::Load(reader, options);
    if (!catalog.Succeeded() || !catalog.value) return false;
    project.standardCatalog_ = std::make_unique<ObjectData::StandardObjectCatalog>(std::move(*catalog.value));
    const std::string lni = "[hfoo]\nhp=250\n";
    auto parsed = ObjectData::LniObjectCodec(ObjectData::ObjectType::Unit).Parse(std::as_bytes(std::span(lni.data(), lni.size())));
    if (!parsed.Succeeded() || !parsed.document) return false;
    project.objectDocuments_[0].reset(dynamic_cast<ObjectData::LniObjectDocument*>(parsed.document.release()));
    return project.objectDocuments_[0] != nullptr;
}
void Cursor(UiKit::CodeView* view, int position, int anchor) {
    auto cursor = view->textCursor(); cursor.setPosition(anchor); cursor.setPosition(position, QTextCursor::KeepAnchor);
    view->setTextCursor(cursor);
}
} // namespace

class NavigationModuleTests final : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void cleanupTestCase();
    void TriggerStableAnchorsAndModes();
    void TriggerMissingAnchorAndBlockedDraft();
    void TriggerFindReplaceViewGatePreservesSelectionAndDraft();
    void TriggerCodeCursorAndAsynchronousCancellation();
    void TriggerWorkspaceEpochRejectsLateRestore();
    void TriggerMapAndLuaSources();
    void TriggerLuaDiskTargetFailuresPreserveView();
    void TriggerLuaSamePathDraftKeepsNewOwnedPayload();
    void LuaSourceWithoutTriggerDocument();
    void VariableStableIdRawViewAndMissingAnchor();
    void ObjectSourceNeverFallsBack();
private:
    QTemporaryDir settings_;
    QString oldOrganization_, oldApplication_;
    QSettings::Format oldFormat_ = QSettings::NativeFormat;
};

void NavigationModuleTests::initTestCase() {
    QVERIFY(settings_.isValid());
    oldOrganization_ = QCoreApplication::organizationName(); oldApplication_ = QCoreApplication::applicationName();
    oldFormat_ = QSettings::defaultFormat(); QSettings::setDefaultFormat(QSettings::IniFormat);
    QCoreApplication::setOrganizationName("AudastraNavigationTest"); QCoreApplication::setApplicationName("OwnedFixture");
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settings_.path());
}
void NavigationModuleTests::cleanupTestCase() {
    QSettings::setDefaultFormat(oldFormat_);
    QCoreApplication::setOrganizationName(oldOrganization_); QCoreApplication::setApplicationName(oldApplication_);
}

void NavigationModuleTests::TriggerStableAnchorsAndModes() {
    QTemporaryDir root; QtProject::EditorProject project; QVERIFY(InstallTriggers(project, root));
    Modules::TriggerModule module(project); auto* page = module.Widget(); page->resize(900, 600); page->show();
    const auto first = project.triggerDocument_->triggers[0].id, second = project.triggerDocument_->triggers[1].id;
    const auto eca = project.triggerDocument_->ecaNodes[project.triggerDocument_->triggers[0].ecas[0]].id;
    QVERIFY(module.Reveal({"triggers", "eca", QString::number(eca), {{"stableIdentity", true}}}));
    const auto original = module.CaptureNavigationBookmark(); QVERIFY(original);
    QCOMPARE(original->value("trigger").toString(), QString::number(first));
    QCOMPARE(original->value("eca").toString(), QString::number(eca));
    QVERIFY(module.Reveal({"triggers", "trigger", QString::number(second), {{"stableIdentity", true}}}));
    QCOMPARE(module.RestoreNavigationBookmark(*original), RestoreResult::Restored);
    QCOMPARE(module.CaptureNavigationBookmark()->value("eca"), original->value("eca"));
    module.SetBlueprintVisible(true); module.Blueprint()->ZoomBy(1.5);
    const auto blueprint = module.CaptureNavigationBookmark(); QVERIFY(blueprint);
    QCOMPARE(blueprint->value("mode").toString(), QStringLiteral("blueprint"));
    QCOMPARE(blueprint->value("trigger").toString(), QString::number(first));
    QCOMPARE(blueprint->value("eca").toString(), QString::number(eca));
    qInfo() << "U5_BLUEPRINT_CAPTURE" << "zoom" << blueprint->value("blueprintZoom").toDouble()
        << "horizontal" << blueprint->value("blueprintHorizontal").toInt()
        << "vertical" << blueprint->value("blueprintScroll").toInt();
    QVERIFY(module.Reveal({"triggers", "trigger", QString::number(second), {{"stableIdentity", true}}}));
    QCOMPARE(module.RestoreNavigationBookmark(*blueprint), RestoreResult::Restored); QVERIFY(module.BlueprintVisible());
    QCOMPARE(module.Blueprint()->SelectedEca(), eca);
    QCOMPARE(module.Blueprint()->transform().m11(), blueprint->value("blueprintZoom").toDouble());
    // 同一真实场景保持稳定 ECA，向负坐标区域平移。两个轴的值来自真实 QGraphicsView 范围。
    module.Blueprint()->setSceneRect(module.Blueprint()->sceneRect().united(QRectF(-1800, -1600, 3600, 3200)));
    auto* blueprintHorizontal = module.Blueprint()->horizontalScrollBar();
    auto* blueprintVertical = module.Blueprint()->verticalScrollBar();
    QVERIFY(blueprintHorizontal->minimum() <= -137 && blueprintHorizontal->maximum() >= -137);
    QVERIFY(blueprintVertical->minimum() <= -91 && blueprintVertical->maximum() >= -91);
    blueprintHorizontal->setValue(-137); blueprintVertical->setValue(-91);
    const auto panned = module.CaptureNavigationBookmark(); QVERIFY(panned);
    QCOMPARE(panned->value("blueprintHorizontal").toInt(), -137);
    QCOMPARE(panned->value("blueprintScroll").toInt(), -91);
    QCOMPARE(panned->value("eca").toString(), QString::number(eca));
    QVERIFY(module.Reveal({"triggers", "trigger", QString::number(second), {{"stableIdentity", true}}}));
    QCOMPARE(module.RestoreNavigationBookmark(*panned), RestoreResult::Restored);
    QVERIFY(module.BlueprintVisible()); QCOMPARE(module.Blueprint()->SelectedEca(), eca);
    QCOMPARE(blueprintHorizontal->value(), -137); QCOMPARE(blueprintVertical->value(), -91);
    const auto pannedBeforeInvalid = module.CaptureNavigationBookmark(); QVERIFY(pannedBeforeInvalid);
    auto fractionalPan = *panned; fractionalPan.insert("blueprintScroll", -91.5);
    QCOMPARE(module.RestoreNavigationBookmark(fractionalPan), RestoreResult::Invalid);
    const auto afterFractional = module.CaptureNavigationBookmark();
    QVERIFY(afterFractional && *afterFractional == *pannedBeforeInvalid);
    auto negativeTree = *panned; negativeTree.insert("treeScroll", -1);
    QCOMPARE(module.RestoreNavigationBookmark(negativeTree), RestoreResult::Invalid);
    const auto afterNegativeTree = module.CaptureNavigationBookmark();
    QVERIFY(afterNegativeTree && *afterNegativeTree == *pannedBeforeInvalid);
    auto bad = *original; bad.insert("eca", QString::number(999999));
    const auto before = module.CaptureNavigationBookmark();
    QCOMPARE(module.RestoreNavigationBookmark(bad), RestoreResult::Invalid);
    const auto unchanged = module.CaptureNavigationBookmark(); QVERIFY(unchanged && before && *unchanged == *before);
    auto category = *original; category.insert("mode", "category"); category.insert("trigger", "0");
    category.insert("eca", "0"); category.insert("category", 37);
    QCOMPARE(module.RestoreNavigationBookmark(category), RestoreResult::Restored);
    QCOMPARE(module.CaptureNavigationBookmark()->value("category").toInt(), 37); // categoryId != stable category.id
    QCOMPARE(project.triggerUndoStack_->count(), 0);
}

void NavigationModuleTests::TriggerMissingAnchorAndBlockedDraft() {
    QTemporaryDir root; QtProject::EditorProject project; QVERIFY(InstallTriggers(project, root));
    Modules::TriggerModule module(project); (void)module.Widget();
    const auto target = module.CaptureNavigationBookmark(); QVERIFY(target);
    QVERIFY(module.ShowGlobalCustomCode());
    auto* view = qobject_cast<UiKit::CodeView*>(module.CodeEditor()); QVERIFY(view);
    view->moveCursor(QTextCursor::End); view->insertPlainText("// author pending draft\n");
    QVERIFY(module.HasPendingCodeDraft()); const auto draft = view->toPlainText();
    const auto sourceBody = project.triggerDocument_->wct.customCode.text;
    auto missing = *target; missing.insert("trigger", "999999");
    QCOMPARE(module.RestoreNavigationBookmark(missing), RestoreResult::Invalid);
    QCOMPARE(view->toPlainText(), draft); QCOMPARE(project.triggerDocument_->wct.customCode.text, sourceBody);
    project.sourceChangeSuspected_ = true; // 正式来源门拒绝发布，不能吞历史或草稿。
    QVERIFY(!module.PrepareNavigationLeave());
    QCOMPARE(module.RestoreNavigationBookmark(*target), RestoreResult::Blocked);
    QCOMPARE(view->toPlainText(), draft); QVERIFY(module.HasPendingCodeDraft());
    QCOMPARE(project.triggerUndoStack_->count(), 0);
}

void NavigationModuleTests::TriggerFindReplaceViewGatePreservesSelectionAndDraft() {
    const Shell::NavigationTarget target{"triggers", "find-replace", {}, {}};
    {
        QtProject::EditorProject absent;
        Modules::TriggerModule module(absent);
        QVERIFY(!module.Reveal(target));
        QVERIFY(!module.CodeEditor()); QVERIFY(!module.CaptureNavigationBookmark());
    }
    QTemporaryDir root; QtProject::EditorProject project; QVERIFY(InstallTriggers(project, root));
    Modules::TriggerModule module(project);
    QVERIFY(!module.Reveal({"triggers", "find-replace", "unexpected-id", {}}));
    QVERIFY(!module.CodeEditor()); QVERIFY(!module.CaptureNavigationBookmark());
    QVERIFY(!module.Reveal({"triggers", "find-replace", {}, {{"unexpected", true}}}));
    QVERIFY(!module.CodeEditor()); QVERIFY(!module.CaptureNavigationBookmark());
    auto heldCatalog = std::move(project.uiCatalog_);
    QVERIFY(!module.Reveal(target)); QVERIFY(!module.CodeEditor());
    project.uiCatalog_ = std::move(heldCatalog);
    auto heldDocument = std::move(project.triggerDocument_);
    QVERIFY(!module.Reveal(target)); QVERIFY(!module.CodeEditor());
    project.triggerDocument_ = std::move(heldDocument);
    auto* page = module.Widget(); QVERIFY(page);
    const auto triggerId = project.triggerDocument_->triggers[0].id;
    const auto ecaId = project.triggerDocument_->ecaNodes[0].id;
    QVERIFY(module.Reveal({"triggers", "eca", QString::number(ecaId), {{"stableIdentity", true}}}));
    const auto before = module.CaptureNavigationBookmark(); QVERIFY(before);
    QCOMPARE(before->value("trigger").toString(), QString::number(triggerId));
    QCOMPARE(before->value("eca").toString(), QString::number(ecaId));
    QVERIFY(module.Reveal(target));
    const auto after = module.CaptureNavigationBookmark(); QVERIFY(after && *after == *before);
    QVERIFY(!page->findChild<QWidget*>(QStringLiteral("triggerReplaceDialog")));
    QCOMPARE(project.triggerUndoStack_->count(), 0); QCOMPARE(project.triggerUndoStack_->index(), 0);
    const auto viewBeforeInvalid = module.SaveViewState();
    QVERIFY(!module.Reveal({"triggers", "find-replace", {}, {{"stableIdentity", true}}}));
    QCOMPARE(module.SaveViewState(), viewBeforeInvalid);
    const auto invalidAfter = module.CaptureNavigationBookmark(); QVERIFY(invalidAfter && *invalidAfter == *before);
    QVERIFY(module.ShowGlobalCustomCode());
    auto* code = qobject_cast<UiKit::CodeView*>(module.CodeEditor()); QVERIFY(code);
    code->moveCursor(QTextCursor::End); code->insertPlainText("// owned pending draft\n");
    QVERIFY(module.HasPendingCodeDraft());
    const auto draft = code->toPlainText(), body = QString::fromUtf8(project.triggerDocument_->wct.customCode.text);
    const auto draftView = module.SaveViewState();
    project.sourceChangeSuspected_ = true;
    QVERIFY(!module.CommitPendingEdits()); // 真实既有提交入口拒绝旧来源，不能用假回调模拟拒绝。
    QVERIFY(!module.Reveal(target));
    QCOMPARE(code->toPlainText(), draft); QCOMPARE(QString::fromUtf8(project.triggerDocument_->wct.customCode.text), body);
    QCOMPARE(module.SaveViewState(), draftView); QVERIFY(module.HasPendingCodeDraft());
    QCOMPARE(project.triggerUndoStack_->count(), 0); QCOMPARE(project.triggerUndoStack_->index(), 0);
    QVERIFY(!page->findChild<QWidget*>(QStringLiteral("triggerReplaceDialog")));
}

void NavigationModuleTests::TriggerCodeCursorAndAsynchronousCancellation() {
    QTemporaryDir root; QtProject::EditorProject project; QVERIFY(InstallTriggers(project, root, true));
    Modules::TriggerModule module(project); (void)module.Widget(); QVERIFY(module.ShowGlobalCustomCode());
    auto* view = qobject_cast<UiKit::CodeView*>(module.CodeEditor()); QVERIFY(view);
    QTRY_VERIFY_WITH_TIMEOUT(!view->IsLoading(), 15000);
    Cursor(view, 180, 120); const auto state = module.CaptureNavigationBookmark(); QVERIFY(state);
    const auto second = project.triggerDocument_->triggers[1].id;
    QVERIFY(module.Reveal({"triggers", "trigger", QString::number(second), {{"stableIdentity", true}}}));
    QCOMPARE(module.RestoreNavigationBookmark(*state), RestoreResult::Restored); QVERIFY(view->IsLoading());
    QTRY_VERIFY_WITH_TIMEOUT(!view->IsLoading(), 15000);
    QCOMPARE(view->textCursor().position(), 180); QCOMPARE(view->textCursor().anchor(), 120);
    QVERIFY(module.Reveal({"triggers", "trigger", QString::number(second), {{"stableIdentity", true}}}));
    QCOMPARE(module.RestoreNavigationBookmark(*state), RestoreResult::Restored); QVERIFY(view->IsLoading());
    module.CancelNavigationRestoration();
    QTRY_VERIFY_WITH_TIMEOUT(!view->IsLoading(), 15000);
    QCOMPARE(view->textCursor().position(), 0); QCOMPARE(view->textCursor().anchor(), 0);
    auto malformed = *state; malformed.insert("cursor", -1);
    QCOMPARE(module.RestoreNavigationBookmark(malformed), RestoreResult::Invalid);
    QCOMPARE(project.triggerUndoStack_->count(), 0);
}

void NavigationModuleTests::TriggerWorkspaceEpochRejectsLateRestore() {
    QTemporaryDir root; QtProject::EditorProject project; QVERIFY(InstallTriggers(project, root, true));
    Host host; Modules::TriggerModule module(project); module.AttachHost(&host); (void)module.Widget();
    QVERIFY(module.ShowGlobalCustomCode()); auto* view = qobject_cast<UiKit::CodeView*>(module.CodeEditor()); QVERIFY(view);
    QTRY_VERIFY_WITH_TIMEOUT(!view->IsLoading(), 15000); Cursor(view, 77, 77);
    const auto state = module.CaptureNavigationBookmark(); QVERIFY(state);
    QCOMPARE(module.RestoreNavigationBookmark(*state), RestoreResult::Restored); QVERIFY(view->IsLoading());
    ++host.epoch;
    QTRY_VERIFY_WITH_TIMEOUT(!view->IsLoading(), 15000); QCOMPARE(view->textCursor().position(), 0);
    QCOMPARE(module.RestoreNavigationBookmark(*state), RestoreResult::Restored); QVERIFY(view->IsLoading());
    module.AttachHost(nullptr);
    QTRY_VERIFY_WITH_TIMEOUT(!view->IsLoading(), 15000); QCOMPARE(view->textCursor().position(), 0);
}

void NavigationModuleTests::TriggerMapAndLuaSources() {
    QTemporaryDir root; QtProject::EditorProject project; QVERIFY(InstallTriggers(project, root));
    project.projectRoot_ = root.path().toStdWString();
    QVERIFY(Write(root.filePath("map/war3map.j"), "// original owned map fixture\n// second line\n"));
    Modules::TriggerModule module(project); auto* page = module.Widget();
    auto* button = page->findChild<QPushButton*>("triggerMapScriptButton"); QVERIFY(button); button->click();
    auto* mapView = page->findChild<UiKit::CodeView*>("triggerMapScriptView"); QVERIFY(mapView);
    QTRY_VERIFY_WITH_TIMEOUT(!mapView->IsLoading(), 5000); Cursor(mapView, 19, 19);
    const auto map = module.CaptureNavigationBookmark(); QVERIFY(map); QCOMPARE(map->value("mode").toString(), QString("map-script"));
    QVERIFY(module.LuaScripts()->CreateScript("Lua\\owned.lua", "-- authored fixture\nreturn 1\n"));
    QVERIFY(module.Reveal({"triggers", "lua-file", "Lua\\owned.lua", {}}));
    const auto lua = module.CaptureNavigationBookmark(); QVERIFY(lua);
    QCOMPARE(module.RestoreNavigationBookmark(*map), RestoreResult::Restored);
    QTRY_VERIFY_WITH_TIMEOUT(!mapView->IsLoading(), 5000); QCOMPARE(mapView->textCursor().position(), 19);
    QCOMPARE(module.RestoreNavigationBookmark(*lua), RestoreResult::Restored);
    auto missing = *lua; missing.insert("path", "Lua\\missing.lua");
    const auto before = module.CaptureNavigationBookmark();
    QCOMPARE(module.RestoreNavigationBookmark(missing), RestoreResult::Invalid);
    const auto unchanged = module.CaptureNavigationBookmark(); QVERIFY(unchanged && before && *unchanged == *before);
    QFile file(root.filePath("map/war3map.j")); QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), QByteArray("// original owned map fixture\n// second line\n"));
}

void NavigationModuleTests::TriggerLuaDiskTargetFailuresPreserveView() {
    for (const bool remove : {true, false}) {
        QTemporaryDir root; QtProject::EditorProject project; QVERIFY(InstallTriggers(project, root));
        const auto targetPath = root.filePath("scripts/target.lua");
        const QByteArray targetBytes("-- owned disk target\nreturn 1\n");
        QVERIFY(Write(targetPath, targetBytes));
        auto opened = DirectoryWorkspace::Open(std::filesystem::path(root.path().toStdWString())); QVERIFY(opened.Succeeded());
        project.directorySnapshot_ = std::make_unique<DirectoryWorkspace>(std::move(*opened.value));
        project.projectRoot_ = project.directorySnapshot_->Snapshot().root; project.ResetEditTracking();
        Modules::TriggerModule module(project); auto* page = module.Widget(); page->resize(900, 600); page->show();
        QVERIFY(module.Reveal({"triggers", "lua-file", "scripts\\target.lua", {}}));
        const auto target = module.CaptureNavigationBookmark(); QVERIFY(target);
        auto* tabs = page->findChild<QTabWidget*>("triggerWorkspaceTabs"); QVERIFY(tabs); tabs->setCurrentIndex(0);
        auto* search = page->findChild<QLineEdit*>("triggerSearch"); QVERIFY(search); search->setText("First");
        QVERIFY(module.ShowGlobalCustomCode());
        auto* view = page->findChild<UiKit::CodeView*>("triggerScriptView"); QVERIFY(view);
        const QString draft = "// current author draft\n" + QString(2000, 'x');
        view->SetText(draft); Cursor(view, 38, 21);
        view->verticalScrollBar()->setValue(7); view->horizontalScrollBar()->setValue(3);
        const auto before = module.CaptureNavigationBookmark(); QVERIFY(before);
        const auto baseline = project.triggerDocument_->wct.customCode.text;
        const auto generation = project.editRevision_.Generation(), revision = project.editRevision_.Revision();
        const int undo = project.triggerUndoStack_->count(), assetUndo = project.AssetUndoStack()->count(), tab = tabs->currentIndex();
        const auto selectedPath = module.LuaScripts()->SelectedPath(), luaText = module.LuaScripts()->Editor()->toPlainText();
        const auto source = project.directorySnapshot_->Snapshot().aggregateSha256;
        if (remove) QVERIFY(QFile::remove(targetPath));
        else { auto changed = targetBytes; changed[changed.size() - 2] = '2'; QVERIFY(Write(targetPath, changed)); }
        QCOMPARE(module.RestoreNavigationBookmark(*target), RestoreResult::Invalid);
        const auto after = module.CaptureNavigationBookmark(); QVERIFY(after && *after == *before);
        QCOMPARE(tabs->currentIndex(), tab); QCOMPARE(search->text(), QString("First")); QCOMPARE(view->toPlainText(), draft);
        QCOMPARE(module.LuaScripts()->SelectedPath(), selectedPath); QCOMPARE(module.LuaScripts()->Editor()->toPlainText(), luaText);
        QCOMPARE(project.triggerDocument_->wct.customCode.text, baseline);
        QCOMPARE(project.editRevision_.Generation(), generation); QCOMPARE(project.editRevision_.Revision(), revision);
        QCOMPARE(project.triggerUndoStack_->count(), undo); QCOMPARE(project.AssetUndoStack()->count(), assetUndo);
        QCOMPARE(project.directorySnapshot_->Snapshot().aggregateSha256, source);
        QVERIFY(module.HasPendingCodeDraft());
    }
}

void NavigationModuleTests::TriggerLuaSamePathDraftKeepsNewOwnedPayload() {
    QtProject::EditorProject project; Modules::TriggerModule module(project); (void)module.Widget();
    QVERIFY(module.LuaScripts()->CreateScript("Lua\\same.lua", "-- owned file\nreturn 1\n"));
    QVERIFY(module.Reveal({"triggers", "lua-file", "Lua\\same.lua", {}}));
    auto* view = module.LuaScripts()->Editor(); Cursor(view, 12, 4);
    const auto state = module.CaptureNavigationBookmark(); QVERIFY(state);
    const QString draft("-- edited owned file\nreturn 2\n"); view->SetText(draft);
    QVERIFY(module.LuaScripts()->HasPendingDraft());
    const int before = project.AssetUndoStack()->count();
    QCOMPARE(module.RestoreNavigationBookmark(*state), RestoreResult::Restored);
    QCOMPARE(view->toPlainText(), draft); QVERIFY(!module.LuaScripts()->HasPendingDraft());
    QCOMPARE(project.AssetUndoStack()->count(), before + 1);
    const auto current = QtProject::ReadOwnedAsset(project, "Lua\\same.lua"); QVERIFY(current.Succeeded());
    const QByteArray accepted(reinterpret_cast<const char*>(current.value->bytes->data()), static_cast<qsizetype>(current.value->bytes->size()));
    QCOMPARE(accepted, draft.toUtf8()); QCOMPARE(view->textCursor().position(), 12); QCOMPARE(view->textCursor().anchor(), 4);
}

void NavigationModuleTests::VariableStableIdRawViewAndMissingAnchor() {
    QtProject::EditorProject project; project.triggerUndoStack_ = new QUndoStack(&project);
    auto document = std::make_unique<Trigger::TriggerDocument>();
    for (int i = 0; i < 80; ++i) QVERIFY(Trigger::TriggerDocumentEditor::AddVariable(*document,
        "Value" + std::to_string(i), "integer", false).Succeeded());
    project.ReplaceTriggerDocument(std::move(document));
    Modules::VariablesModule module(project); auto* page = module.Widget(); page->resize(700, 300); page->show();
    auto* table = page->findChild<QTableWidget*>("globalVariablesTable"); QVERIFY(table);
    const auto id = project.triggerDocument_->variables[50].id;
    QVERIFY(module.Reveal({"variables", "variable", QString::number(id), {{"stableIdentity", true}}}));
    auto* raw = page->findChild<QCheckBox*>("variableRawValues"); QVERIFY(raw); raw->setChecked(true);
    table->verticalScrollBar()->setValue(15);
    const auto state = module.CaptureNavigationBookmark(); QVERIFY(state);
    const auto selected = table->currentRow(); const auto recordedScroll = state->value("scroll").toInt();
    raw->setChecked(false); table->setCurrentCell(0, 0);
    project.triggerDocument_->variables[50].name.text = "Renamed"; emit project.WorldEditStringsChanged();
    QCOMPARE(module.RestoreNavigationBookmark(*state), RestoreResult::Restored);
    QCOMPARE(table->currentRow(), selected); QVERIFY(raw->isChecked()); QCOMPARE(table->verticalScrollBar()->value(), recordedScroll);
    project.triggerDocument_->variables.erase(project.triggerDocument_->variables.begin() + 50);
    const int before = table->currentRow();
    QCOMPARE(module.RestoreNavigationBookmark(*state), RestoreResult::Invalid); QCOMPARE(table->currentRow(), before);
    auto bad = *state; bad.insert("selected", "Value50"); QCOMPARE(module.RestoreNavigationBookmark(bad), RestoreResult::Invalid);
    QCOMPARE(project.triggerUndoStack_->count(), 0);
}

void NavigationModuleTests::LuaSourceWithoutTriggerDocument() {
    QtProject::EditorProject project; QVERIFY(!project.triggerDocument_);
    Modules::TriggerModule module(project); (void)module.Widget();
    QVERIFY(module.LuaScripts()->CreateScript("Lua\\independent.lua", "-- owned file\nreturn 1\n"));
    QVERIFY(module.Reveal({"triggers", "lua-file", "Lua\\independent.lua", {}}));
    auto* view = module.LuaScripts()->Editor(); Cursor(view, 12, 4);
    const auto state = module.CaptureNavigationBookmark(); QVERIFY(state);
    Cursor(view, 0, 0);
    QCOMPARE(module.RestoreNavigationBookmark(*state), RestoreResult::Restored);
    QCOMPARE(view->textCursor().position(), 12); QCOMPARE(view->textCursor().anchor(), 4);
    QVERIFY(!project.triggerDocument_);
}

void NavigationModuleTests::ObjectSourceNeverFallsBack() {
    QtProject::EditorProject project; QVERIFY(InstallObjects(project)); Modules::ObjectModule module(project);
    (void)module.Widget(); module.UpdateObjectReferenceEntries(); module.SelectFirstType();
    QVERIFY(module.Reveal({"objects", "object", "unit:hfoo", {}}));
    const auto map = module.CaptureNavigationBookmark(); QVERIFY(map); QCOMPARE(map->value("source").toString(), QString("map"));
    const auto originalFingerprint = project.objectDocuments_[0]->DocumentFingerprint();
    auto standard = *map; standard.insert("source", "standard");
    QCOMPARE(module.RestoreNavigationBookmark(standard), RestoreResult::Restored);
    QVERIFY(!module.EditingMapObjects());
    QCOMPARE(module.CaptureNavigationBookmark()->value("source").toString(), QString("standard"));
    QCOMPARE(module.RestoreNavigationBookmark(*map), RestoreResult::Restored); QVERIFY(module.EditingMapObjects());
    QCOMPARE(project.objectDocuments_[0]->DocumentFingerprint(), originalFingerprint);
    module.DetachDocuments(); project.objectDocuments_[0].reset();
    QCOMPARE(module.RestoreNavigationBookmark(standard), RestoreResult::Restored); QVERIFY(!module.EditingMapObjects());
    const auto before = module.CaptureNavigationBookmark();
    QCOMPARE(module.RestoreNavigationBookmark(*map), RestoreResult::Invalid); QVERIFY(!module.EditingMapObjects());
    const auto unchanged = module.CaptureNavigationBookmark(); QVERIFY(unchanged && before && *unchanged == *before);
    auto wrong = standard; wrong.insert("type", 1);
    QCOMPARE(module.RestoreNavigationBookmark(wrong), RestoreResult::Invalid); QCOMPARE(module.CurrentObjectType(), 0);
    wrong = standard; wrong.insert("source", "fallback"); QCOMPARE(module.RestoreNavigationBookmark(wrong), RestoreResult::Invalid);
    wrong = standard; wrong.insert("type", 0.5); QCOMPARE(module.RestoreNavigationBookmark(wrong), RestoreResult::Invalid);
}
QTEST_MAIN(NavigationModuleTests)
#include "NavigationModuleTests.moc"
