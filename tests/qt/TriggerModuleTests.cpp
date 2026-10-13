#include "EditorProject.h"
#include "Fonts/FontLoader.h"
#include "Theme/ThemeManager.h"
#include "Widgets/ResponsiveToolBar.h"
#include "TriggerBlueprint.h"
#include "TriggerDialogs.h"
#include "TriggerModels.h"
#include "TriggerModule.h"
#include "TriggerCommands.h"
#include "TriggerValuePanel.h"
#include "TriggerSentenceDelegate.h"
#include "ObjectNames.h"
#include "War3MapCore/MapDocument.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/ObjectData/LniObjectCodec.h"
#include "War3MapCore/ObjectData/StandardObjectCatalog.h"
#include "War3MapCore/ObjectData/NativeObjectCodec.h"
#include "War3MapCore/Game/GameDataSource.h"
#include "War3MapCore/Script/MapScript.h"
#include "Widgets/CodeView.h"
#include "Widgets/CodeProblemsView.h"
#include "Widgets/ProblemList.h"
#include "TriggerCodeChecking.h"
#include "Icons/SvgIcons.h"
#include "PopupCapture.h"

#include "War3MapCore/Trigger/TriggerCodec.h"
#include "War3MapCore/Trigger/TriggerEditing.h"
#include "War3MapCore/Trigger/TriggerGraphEditing.h"
#include "War3MapCore/Trigger/UiCatalog.h"
#include "War3MapCore/Workspace.h"

#include <QApplication>
#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QSettings>
#include <QStandardItemModel>
#include <QContextMenuEvent>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonArray>
#include <QKeyEvent>
#include <QLabel>
#include <QTreeView>
#include <QLineEdit>
#include <QLayout>
#include <QMessageBox>
#include <QMenu>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPersistentModelIndex>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalSpy>
#include <QSortFilterProxyModel>
#include <QTabBar>
#include <QTemporaryDir>
#include <QTest>
#include <QTextCursor>
#include <QTimer>
#include <QTreeView>
#include <QToolButton>
#include <QUndoStack>

#include <fstream>

using namespace War3Map;

namespace {

// 原创合成 WTG v7 / WCT v1：类别 1 System（Init 含 PlainAction，Spawn 为空），类别 2 Combat（Caller 运行 Init）。
class Bytes {
public:
    void I32(std::int32_t value) {
        for (int shift = 0; shift < 32; shift += 8) data.push_back(static_cast<std::byte>((static_cast<std::uint32_t>(value) >> shift) & 0xffU));
    }
    void Text(std::string_view value) {
        for (const char c : value) data.push_back(static_cast<std::byte>(c));
        data.push_back(std::byte{0});
    }
    std::vector<std::byte> data;
};

std::vector<std::byte> DirectoryWtg(std::string_view initialName = "Init", std::string_view spawnName = "Spawn") {
    Bytes w;
    for (const char c : std::string_view("WTG!")) w.data.push_back(static_cast<std::byte>(c));
    w.I32(7);
    w.I32(2);
    w.I32(1); w.Text("System"); w.I32(0);
    w.I32(2); w.Text("Combat"); w.I32(0);
    w.I32(2);
    w.I32(0);
    w.I32(3);
    const auto trigger = [&w](std::string_view name, std::int32_t category, std::string_view action, std::string_view argument) {
        w.Text(name); w.Text(""); w.I32(0); w.I32(1); w.I32(0); w.I32(0); w.I32(0); w.I32(category);
        if (action.empty()) {
            w.I32(0);
            return;
        }
        w.I32(1);
        w.I32(2); w.Text(action); w.I32(1);
        if (!argument.empty()) {
            w.I32(1); w.Text(argument); w.I32(0); w.I32(0);
        }
        w.I32(0);
    };
    trigger(initialName, 1, "PlainAction", "");
    trigger(spawnName, 1, "", "");
    trigger("Caller", 2, "RunTrigger", "gg_trg_" + Trigger::TriggerDirectoryEditor::ScriptName(initialName));
    return std::move(w.data);
}

std::vector<std::byte> DirectoryWct() {
    Bytes w;
    w.I32(1);
    w.Text("");
    w.I32(0);
    w.I32(3);
    for (int index = 0; index < 3; ++index) w.I32(0);
    return std::move(w.data);
}

std::vector<std::byte> CodeWct(std::string_view header, int slotCount = 3) {
    Bytes w;
    w.I32(1); w.Text("");
    w.I32(header.empty() ? 0 : static_cast<std::int32_t>(header.size() + 1));
    if (!header.empty()) w.Text(header);
    w.I32(slotCount);
    for (int index = 0; index < slotCount; ++index) w.I32(0);
    return std::move(w.data);
}

std::vector<std::byte> EmptyCodeWtg() {
    Bytes w;
    for (const char c : std::string_view("WTG!")) w.data.push_back(static_cast<std::byte>(c));
    w.I32(7); w.I32(0); w.I32(2); w.I32(0); w.I32(0);
    return std::move(w.data);
}

void WriteUtf8(const std::filesystem::path& path, std::string_view text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
}

bool InstallCodeProject(QtProject::EditorProject& project, const std::filesystem::path& root,
    bool empty = false, std::string_view globalBody = {}) {
    WriteUtf8(root / "ui/config", "base\n");
    WriteUtf8(root / "ui/base/define.txt", "[TriggerTypes]\ntrigger=0,1,1,触发器\n");
    WriteUtf8(root / "ui/base/action.txt", "[PlainAction]\ndescription=动作\n[RunTrigger]\ndescription=运行 ${触发器}\n[[.args]]\ntype=trigger\n");
    WriteUtf8(root / "ui/base/event.txt", "[MapInitializationEvent]\ndescription=地图初始化\n");
    auto catalog = Trigger::UiCatalog::Load(root);
    if (!catalog.Succeeded() || !catalog.value) return false;
    auto parsed = Trigger::WtgWctCodec{}.Parse(empty ? EmptyCodeWtg() : DirectoryWtg(),
        CodeWct(globalBody, empty ? 0 : 3), *catalog.value);
    if (!parsed.Succeeded() || !parsed.value) return false;
    project.uiCatalog_ = std::make_unique<Trigger::UiCatalog>(std::move(*catalog.value));
    project.triggerDocument_ = std::make_unique<Trigger::TriggerDocument>(std::move(*parsed.value));
    project.triggerUndoStack_ = new QUndoStack(&project);
    return true;
}

bool InstallScalarUiProject(QtProject::EditorProject& project, const std::filesystem::path& root, int callerRows = 2) {
    WriteUtf8(root / "ui/config", "base\n");
    WriteUtf8(root / "ui/base/define.txt", "[TriggerTypes]\nstring=0,1,1,String,string\n");
    WriteUtf8(root / "ui/base/action.txt", "[PlainAction]\ndescription=Plain\n[UseText]\ndescription=Use ${value}\n[[.args]]\ntype=string\n");
    auto catalog = Trigger::UiCatalog::Load(root);
    if (!catalog.Succeeded() || !catalog.value) return false;
    Bytes w;
    for (const char c : std::string_view("WTG!")) w.data.push_back(static_cast<std::byte>(c));
    w.I32(7); w.I32(2); w.I32(1); w.Text("System"); w.I32(0); w.I32(2); w.Text("Combat"); w.I32(0);
    w.I32(2); w.I32(2);
    for (const auto name : {"OldText", "NextText"}) {
        w.Text(name); w.Text("string"); w.I32(1); w.I32(0); w.I32(1); w.I32(0); w.Text("");
    }
    w.I32(3);
    for (const auto name : {"Init", "Spawn", "Caller"}) {
        const bool caller = std::string_view(name) == "Caller";
        w.Text(name); w.Text(""); w.I32(0); w.I32(1); w.I32(0); w.I32(0); w.I32(0); w.I32(caller ? 2 : 1);
        w.I32(caller ? callerRows : 1);
        for (int row = 0; row < (caller ? callerRows : 1); ++row) {
            w.I32(2); w.Text(caller ? "UseText" : "PlainAction"); w.I32(1);
            if (caller) { w.I32(3); w.Text(row == 0 ? "Before A" : "Before B"); w.I32(0); w.I32(0); }
            w.I32(0);
        }
    }
    auto parsed = Trigger::WtgWctCodec{}.Parse(w.data, DirectoryWct(), *catalog.value);
    if (!parsed.Succeeded() || !parsed.value) return false;
    const auto encoded = Trigger::WtgWctCodec{}.Serialize(*parsed.value, *catalog.value);
    if (!encoded.Succeeded() || !encoded.value) return false;
    const auto source = root / "source";
    std::filesystem::create_directories(source / "map");
    for (const auto& entry : {std::pair{"war3map.wtg", &encoded.value->wtg}, std::pair{"war3map.wct", &encoded.value->wct}}) {
        std::ofstream stream(source / "map" / entry.first, std::ios::binary);
        stream.write(reinterpret_cast<const char*>(entry.second->data()), static_cast<std::streamsize>(entry.second->size()));
        if (!stream) return false;
    }
    auto workspace = DirectoryWorkspace::Open(source);
    if (!workspace.Succeeded() || !workspace.value) return false;
    project.projectRoot_ = source; project.directorySnapshot_ = std::make_unique<DirectoryWorkspace>(std::move(*workspace.value));
    project.uiDataRoot_ = project.activeUiDataRoot_ = root;
    project.uiCatalog_ = std::make_unique<Trigger::UiCatalog>(std::move(*catalog.value));
    project.triggerDocument_ = std::make_unique<Trigger::TriggerDocument>(std::move(*parsed.value));
    project.triggerUndoStack_ = new QUndoStack(&project); project.ResetEditTracking();
    return project.editTrackingReady_ && !project.editTrackingFailed_;
}

Result<Trigger::TriggerGraphScalarEditReceipt> PrepareScalarUiEdit(QtProject::EditorProject& project) {
    const auto binding = Trigger::CaptureTriggerGraphBinding(*project.triggerDocument_, *project.uiCatalog_);
    if (!binding.Succeeded() || !binding.value) {
        Result<Trigger::TriggerGraphScalarEditReceipt> failed; failed.diagnostics = binding.diagnostics; return failed;
    }
    Trigger::TriggerGraphEditBatch batch; batch.expected = *binding.value;
    for (const auto& argument : project.triggerDocument_->argumentNodes) {
        if (batch.operations.size() == 2) break;
        batch.operations.emplace_back(Trigger::GraphSet{{Trigger::GraphRecordKind::Argument, argument.id, {}},
            Trigger::GraphField::ArgumentScalar, Trigger::GraphScalarValue{argument.type, argument.value.text},
            Trigger::GraphScalarValue{1, "NextText"}});
    }
    return Trigger::PrepareTriggerGraphScalarEditBatch(*project.triggerDocument_, *project.uiCatalog_, batch);
}

const Trigger::TriggerRecord* Named(const QtProject::EditorProject& project, std::string_view name) {
    for (const auto& trigger : project.triggerDocument_->triggers) {
        if (trigger.name.text == name) return &trigger;
    }
    return nullptr;
}

bool InstallParsedCustomCodeBody(QtProject::EditorProject& project, const QString& body) {
    const auto* trigger = Named(project, "Spawn");
    if (!trigger) return false;
    const Trigger::RawText source{body.toStdString(), Trigger::TextEncoding::Utf8, {}};
    const auto plan = Trigger::TriggerCodeEditor::PrepareConversion(*project.triggerDocument_, *project.uiCatalog_,
        trigger->id, Trigger::TriggerCodeEditor::NormalizedText(source), false);
    if (!plan.Succeeded() || !plan.value) return false;
    const auto applied = Trigger::TriggerCodeEditor::Apply(*project.triggerDocument_, *project.uiCatalog_, *plan.value);
    if (!applied.Succeeded() || !applied.value) return false;
    const auto serialized = Trigger::WtgWctCodec{}.Serialize(*project.triggerDocument_, *project.uiCatalog_);
    if (!serialized.Succeeded() || !serialized.value) return false;
    auto parsed = Trigger::WtgWctCodec{}.Parse(serialized.value->wtg, serialized.value->wct, *project.uiCatalog_);
    if (!parsed.Succeeded() || !parsed.value) return false;
    project.triggerDocument_ = std::make_unique<Trigger::TriggerDocument>(std::move(*parsed.value));
    return true;
}

QStringList Names(const QtProject::EditorProject& project) {
    QStringList names;
    for (const auto& trigger : project.triggerDocument_->triggers) names << QString::fromStdString(trigger.name.text);
    return names;
}

// 设置 WAR3_TEST_SCREENSHOT_DIR 时保存界面截图，供人工检查（不作为断言）。
void Snapshot(QWidget* widget, const char* name) {
    const QByteArray dir = qgetenv("WAR3_TEST_SCREENSHOT_DIR");
    if (!dir.isEmpty()) widget->grab().save(QDir(QString::fromLocal8Bit(dir)).filePath(QString::fromLatin1(name) + ".png"));
}

void SendKey(QWidget* target, int key, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    QKeyEvent press(QEvent::KeyPress, key, modifiers);
    QApplication::sendEvent(target, &press);
}


// 触发器 Logic：动作 A（PlainAction）与 If（IfThenElseMultiple：条件 RawCond，则 PlainAction，否则为空）。
std::vector<std::byte> StructureWtg() {
    Bytes w;
    for (const char c : std::string_view("WTG!")) w.data.push_back(static_cast<std::byte>(c));
    w.I32(7);
    w.I32(1);
    w.I32(1); w.Text("Main"); w.I32(0);
    w.I32(2);
    w.I32(0);
    w.I32(1);
    w.Text("Logic"); w.Text(""); w.I32(0); w.I32(1); w.I32(0); w.I32(0); w.I32(0); w.I32(1);
    w.I32(2);
    w.I32(2); w.Text("PlainAction"); w.I32(1); w.I32(0);
    w.I32(2); w.Text("IfThenElseMultiple"); w.I32(1);
    w.I32(2);
    w.I32(1); w.I32(0); w.Text("RawCond"); w.I32(1); w.I32(0);
    w.I32(2); w.I32(1); w.Text("PlainAction"); w.I32(1); w.I32(0);
    return std::move(w.data);
}

std::vector<std::byte> StructureWct() {
    Bytes w;
    w.I32(1);
    w.Text("");
    w.I32(0);
    w.I32(1);
    w.I32(0);
    return std::move(w.data);
}


// 触发器 Values：SetValue(变量 Hero, 预设 PresetTen)；全局变量 Hero(unit)、Count(integer)。
std::vector<std::byte> ValueWtg() {
    Bytes w;
    for (const char c : std::string_view("WTG!")) w.data.push_back(static_cast<std::byte>(c));
    w.I32(7);
    w.I32(1);
    w.I32(1); w.Text("Main"); w.I32(0);
    w.I32(2);
    w.I32(2);
    w.Text("Hero"); w.Text("unit"); w.I32(1); w.I32(0); w.I32(1); w.I32(0); w.Text("");
    w.Text("Count"); w.Text("integer"); w.I32(1); w.I32(0); w.I32(1); w.I32(0); w.Text("");
    w.I32(1);
    w.Text("Values"); w.Text(""); w.I32(0); w.I32(1); w.I32(0); w.I32(0); w.I32(0); w.I32(1);
    w.I32(1);
    w.I32(2); w.Text("SetValue"); w.I32(1);
    w.I32(1); w.Text("Hero"); w.I32(0); w.I32(0);
    w.I32(0); w.Text("PresetTen"); w.I32(0); w.I32(0);
    w.I32(0);
    return std::move(w.data);
}

// 触发器 Gate：事件 RawEvent、触发条件 RawCond、动作 PlainAction；注释条目 Readme（isComment = 1，只有说明文字）。
std::vector<std::byte> CommentWtg() {
    Bytes w;
    for (const char c : std::string_view("WTG!")) w.data.push_back(static_cast<std::byte>(c));
    w.I32(7);
    w.I32(1);
    w.I32(1); w.Text("Main"); w.I32(0);
    w.I32(2);
    w.I32(0);
    w.I32(2);
    w.Text("Gate"); w.Text(""); w.I32(0); w.I32(1); w.I32(0); w.I32(0); w.I32(0); w.I32(1);
    w.I32(3);
    w.I32(0); w.Text("RawEvent"); w.I32(1); w.I32(0);
    w.I32(1); w.Text("RawCond"); w.I32(1); w.I32(0);
    w.I32(2); w.Text("PlainAction"); w.I32(1); w.I32(0);
    w.Text("Readme"); w.Text("地图说明"); w.I32(1); w.I32(1); w.I32(0); w.I32(0); w.I32(0); w.I32(1);
    w.I32(0);
    return std::move(w.data);
}

std::vector<std::byte> CommentWct() {
    Bytes w;
    w.I32(1);
    w.Text("");
    w.I32(0);
    w.I32(2);
    w.I32(0);
    w.I32(0);
    return std::move(w.data);
}

} // namespace

class TriggerModuleTests final : public QObject {
    Q_OBJECT
private slots:
    void CommandOverflowKeepsFindReplaceKeyboardAndMouseReachable();
    void AuthorCodeEntrypointsAt1080p();
    void MapScriptOpensReadOnlyAndSearches();
    void MissingMapScriptKeepsLogicView();
    void DirectoryEditsUndoAndReferences();
    void DirectoryReferenceConfirmationUsesSharedSymbols();
    void StatementsMoveAndFillEmptyBranches();
    void CallbackActionShowsActionBranch();
    void ValuePanelEditsParameters();
    void BlueprintLaysOutAndSyncs();
    void CommentEntriesAndConditionNode();
    void UneditedTriggerCommentPreservesOriginalNewlines();
    void FunctionPickerShowsCategoryPrefixes();
    void DynamicReferencesAndCurrentStringOverlayPreserveRawValues();
    void ReferenceTokensMeasureExactlyWhatTheyPaint();
    void SharedMapStringsUseOneUndoAndPreserveOriginalBody();
    void RealAudastraNameSamplesRenderReadOnlyProjection();
    void CustomCodeHeaderEncodingAndOneUndo();
    void CodeUnicodeDisplayBaselinePreservesOriginalBytes_data();
    void CodeUnicodeDisplayBaselinePreservesOriginalBytes();
    void OrdinaryChineseCodeRemainsEditableAndUsesOneUndo_data();
    void OrdinaryChineseCodeRemainsEditableAndUsesOneUndo();
    void LargeCodeLanguageReloadCapturesCurrentDisplayBaseline();
    void CustomCodeDraftRejectsNavigationAndProperties();
    void CustomCodeDraftStaysBoundThroughUndo();
    void GuiConversionUsesSealedConfirmationAndClosureUndo();
    void FirstCustomCodeCreatesCategoryTogetherAndRejectsLua();
    void CodeCheckingColumnsMixedEncodingAndReadonlyBytes();
    void CodeUiUsesSharedProjectCheckerAndKeepsWctUntouched();
    void ScalarReplayKeepsSelectionCollapseClipboardAndRefreshesPresentations();
    void ScalarUiNestedOrSourceChangeFallsBackToFullModels_data();
    void ScalarUiNestedOrSourceChangeFallsBackToFullModels();
    void ScalarPresentationCacheHasFiniteAdmissionBoundary();
    void ScalarUiFirstApplyAfterLateRepeatedBaselineReset();
    void BaselineResetRebuildsUnnotifiedSameOwnerTopology();
    void BaselineUiSnapshotRejectsEarlySourceAndNestedReset_data();
    void BaselineUiSnapshotRejectsEarlySourceAndNestedReset();
};

void TriggerModuleTests::ScalarReplayKeepsSelectionCollapseClipboardAndRefreshesPresentations() {
    QTemporaryDir root; QVERIFY(root.isValid()); QtProject::EditorProject project;
    QVERIFY(InstallScalarUiProject(project, root.path().toStdWString()));
    const auto original = Trigger::WtgWctCodec{}.Serialize(*project.triggerDocument_, *project.uiCatalog_);
    QVERIFY(original.Succeeded() && original.value);
    Modules::TriggerModule module(project); auto* page = module.Widget();
    QVERIFY(module.Reveal({"triggers", "trigger", "Caller", {}}));
    auto* logic = page->findChild<QTreeView*>("triggerLogicTree"); auto* tree = page->findChild<QTreeView*>("triggerExplorer");
    QVERIFY(logic && tree); auto* model = dynamic_cast<TriggerEcaModel*>(logic->model()); QVERIFY(model);
    const auto actions = model->index(2, 0); const auto selected = model->index(1, 0, actions);
    QVERIFY(selected.isValid()); logic->setCurrentIndex(selected);
    const QPersistentModelIndex persistent(selected); const auto selectedId = selected.data(TriggerEcaModel::StableIdRole);
    const auto display = selected.data(Qt::DisplayRole).toString(); QVERIFY(display.contains("Before B"));
    (void)selected.data(Qt::ToolTipRole); (void)selected.data(TriggerEcaModel::ArgumentSpansRole);
    const auto system = tree->model()->index(0, 0); tree->collapse(system); QVERIFY(!tree->isExpanded(system));
    module.CopyCurrentTrigger(); // 正常业务剪贴板，最后真实 Paste 验证未被同次标量通知清空。
    QSignalSpy logicResets(model, &QAbstractItemModel::modelReset), directoryResets(tree->model(), &QAbstractItemModel::modelReset);
    QSignalSpy changed(model, &QAbstractItemModel::dataChanged), documentChanges(&project, &QtProject::EditorProject::TriggerDocumentChanged);
    auto receipt = PrepareScalarUiEdit(project); QVERIFY(receipt.Succeeded() && receipt.value);
    auto command = std::make_unique<QtProject::TriggerGraphSnapshotUndoCommand>(project, std::move(*receipt.value), QStringLiteral("Scalar UI actual"));
    const auto published = command->FirstPublication(); project.triggerUndoStack_->push(command.release());
    QVERIFY(*published); QCOMPARE(project.triggerUndoStack_->count(), 1); QCOMPARE(logicResets.count(), 0); QCOMPARE(directoryResets.count(), 0);
    QVERIFY(changed.count() > 0); QVERIFY(persistent.isValid()); QCOMPARE(logic->currentIndex().data(TriggerEcaModel::StableIdRole), selectedId);
    QVERIFY(!tree->isExpanded(system)); QVERIFY(persistent.data(Qt::DisplayRole).toString().contains("NextText"));
    QVERIFY(persistent.data(Qt::ToolTipRole).toString().contains("NextText"));
    const auto spans = persistent.data(TriggerEcaModel::ArgumentSpansRole).toList(); QVERIFY(!spans.isEmpty());
    QCOMPARE(spans.front().toMap().value("raw").toString(), QStringLiteral("NextText"));
    project.triggerUndoStack_->undo(); QVERIFY(persistent.isValid());
    QCOMPARE(persistent.data(Qt::DisplayRole).toString(), display);
    project.triggerUndoStack_->redo(); QVERIFY(persistent.data(Qt::DisplayRole).toString().contains("NextText"));
    project.triggerUndoStack_->undo(); QCOMPARE(documentChanges.count(), 4); QCOMPARE(logicResets.count(), 0); QCOMPARE(directoryResets.count(), 0);
    const auto restored = Trigger::WtgWctCodec{}.Serialize(*project.triggerDocument_, *project.uiCatalog_);
    QVERIFY(restored.Succeeded() && restored.value); QVERIFY(restored.value->wtg == original.value->wtg && restored.value->wct == original.value->wct);
    module.PasteTrigger(); QCOMPARE(project.triggerDocument_->triggers.size(), std::size_t{4});
    project.triggerUndoStack_->undo(); QCOMPARE(project.triggerDocument_->triggers.size(), std::size_t{3});
}

void TriggerModuleTests::ScalarUiNestedOrSourceChangeFallsBackToFullModels_data() {
    QTest::addColumn<int>("kind"); QTest::newRow("generic-reentry") << 0; QTest::newRow("world-string-reentry") << 1;
    QTest::newRow("source-suspected") << 2; QTest::newRow("outer-macro") << 3;
    QTest::newRow("datachanged-world-string-reentry") << 4;
    QTest::newRow("null-public-stack-after-notice") << 5;
    QTest::newRow("foreign-public-stack-after-notice") << 6;
    QTest::newRow("same-owner-model-reset") << 7;
    QTest::newRow("different-bound-trigger") << 8;
    QTest::newRow("same-owner-same-trigger-reset") << 9;
    QTest::newRow("null-stack-datachanged") << 10;
    QTest::newRow("foreign-stack-datachanged") << 11;
}

void TriggerModuleTests::ScalarUiNestedOrSourceChangeFallsBackToFullModels() {
    QFETCH(int, kind); QTemporaryDir root; QVERIFY(root.isValid()); QtProject::EditorProject project;
    QVERIFY(InstallScalarUiProject(project, root.path().toStdWString())); Modules::TriggerModule module(project);
    auto* page = module.Widget(); QVERIFY(module.Reveal({"triggers", "trigger", "Caller", {}}));
    auto* logic = page->findChild<QTreeView*>("triggerLogicTree"); QVERIFY(logic);
    auto* model = dynamic_cast<TriggerEcaModel*>(logic->model()); QVERIFY(model);
    auto* const actualStack = project.triggerUndoStack_; QVERIFY(actualStack);
    QUndoStack foreignStack;
    struct RestoreStack {
        QtProject::EditorProject& project; QUndoStack* actual;
        ~RestoreStack() { project.triggerUndoStack_ = actual; }
    } restoreStack{project, actualStack}; // 测试局部 Project 必定比 guard 晚析构，断言提前返回也恢复。
    const auto* caller = Named(project, "Caller"); const auto* other = Named(project, "Init"); QVERIFY(caller && other);
    (void)model->index(0, 0, model->index(2, 0)).data(Qt::DisplayRole);
    QSignalSpy resets(model, &QAbstractItemModel::modelReset);
    bool nested = false;
    connect(&project, &QtProject::EditorProject::TriggerDocumentChanged, &module, [&] {
        if (nested || kind == 3) return; nested = true;
        if (kind == 0) emit project.TriggerDocumentChanged();
        if (kind == 1) emit project.WorldEditStringsChanged();
        if (kind == 2) { project.sourceChangeSuspected_ = true; emit project.SourceStateChanged(); }
        // 保留实际执行 push 的栈活着；只改公开指针，重现旧 null-deref 而不删除 Qt 正在执行的对象。
        if (kind == 5) project.triggerUndoStack_ = nullptr;
        if (kind == 6) project.triggerUndoStack_ = &foreignStack;
        if (kind == 7) model->SetDocument(project.triggerDocument_.get(), project.uiCatalog_.get());
        if (kind == 8) model->SetTrigger(other->id);
        if (kind == 9) model->SetTrigger(caller->id);
    });
    bool presentationReentry = false;
    connect(model, &QAbstractItemModel::dataChanged, &module, [&] {
        if (kind == 4 && !presentationReentry) { presentationReentry = true; emit project.WorldEditStringsChanged(); }
        if ((kind == 10 || kind == 11) && !presentationReentry) {
            presentationReentry = true; project.triggerUndoStack_ = kind == 10 ? nullptr : &foreignStack;
        }
    });
    auto receipt = PrepareScalarUiEdit(project); QVERIFY(receipt.Succeeded() && receipt.value);
    if (kind == 3) actualStack->beginMacro(QStringLiteral("Real macro"));
    auto command = std::make_unique<QtProject::TriggerGraphSnapshotUndoCommand>(project, std::move(*receipt.value), QStringLiteral("Fallback actual"));
    const auto published = command->FirstPublication(); actualStack->push(command.release());
    if (kind == 3) actualStack->endMacro();
    QVERIFY(*published); QVERIFY(resets.count() > 0); QCOMPARE(actualStack->count(), 1);
    if (kind == 4 || kind == 10 || kind == 11) QVERIFY(presentationReentry);
    if (kind == 7 || kind == 8 || kind == 9) QVERIFY(resets.count() >= 2);
    if (kind >= 5) QCOMPARE(model->BoundTriggerId(), caller->id);
    // 完成负例后恢复原公开指针；foreignStack 仍由本测试独立拥有，不污染后续 QObject 析构。
    project.triggerUndoStack_ = actualStack;
    QVERIFY(project.triggerDocument_->argumentNodes[0].value.text == "NextText");
}

void TriggerModuleTests::ScalarPresentationCacheHasFiniteAdmissionBoundary() {
    QTemporaryDir root; QVERIFY(root.isValid()); QtProject::EditorProject project;
    QVERIFY(InstallScalarUiProject(project, root.path().toStdWString(), 4097));
    const auto* caller = Named(project, "Caller"); QVERIFY(caller);
    TriggerEcaModel model; model.SetDocument(project.triggerDocument_.get(), project.uiCatalog_.get()); model.SetTrigger(caller->id);
    const auto actions = model.index(2, 0); QCOMPARE(model.rowCount(actions), 4097);
    for (int row = 0; row < 4096; ++row) (void)model.index(row, 0, actions).data(Qt::DisplayRole);
    QSignalSpy changes(&model, &QAbstractItemModel::dataChanged), resets(&model, &QAbstractItemModel::modelReset);
    QVERIFY(model.InvalidateScalarPresentations()); QCOMPARE(changes.count(), 4096); QCOMPARE(resets.count(), 0);
    changes.clear();
    for (int row = 0; row < 4097; ++row) (void)model.index(row, 0, actions).data(Qt::DisplayRole);
    const auto retained = model.index(4096, 0, actions).data(Qt::DisplayRole);
    QVERIFY(!model.InvalidateScalarPresentations()); QCOMPARE(changes.count(), 0); QCOMPARE(resets.count(), 0);
    QCOMPARE(model.index(4096, 0, actions).data(Qt::DisplayRole), retained);
    // 超帽仍可走原完整 SetDocument/SetTrigger；不销毁地图、不截为假完整窗口。
    model.SetDocument(project.triggerDocument_.get(), project.uiCatalog_.get()); model.SetTrigger(caller->id);
    QCOMPARE(resets.count(), 2); QCOMPARE(model.rowCount(model.index(2, 0)), 4097);
}

void TriggerModuleTests::ScalarUiFirstApplyAfterLateRepeatedBaselineReset() {
    QTemporaryDir root; QVERIFY(root.isValid()); QtProject::EditorProject project;
    QVERIFY(InstallScalarUiProject(project, root.path().toStdWString())); Modules::TriggerModule module(project);
    auto* page = module.Widget(); auto* logic = page->findChild<QTreeView*>("triggerLogicTree"); QVERIFY(logic);
    auto* model = dynamic_cast<TriggerEcaModel*>(logic->model()); QVERIFY(model);
    const auto initialGeneration = project.editRevision_.Generation();
    QSignalSpy baselines(&project, &QtProject::EditorProject::EditTrackingBaselineChanged);
    QSignalSpy documentChanges(&project, &QtProject::EditorProject::TriggerDocumentChanged), resets(model, &QAbstractItemModel::modelReset);
    // 实际 Main 顺序：先接收 watcher 配置的来源失效，再建立 workspace/archive 冷基线。
    QSignalSpy sourceChanges(&project, &QtProject::EditorProject::SourceStateChanged);
    project.RefreshProjectWatch(); project.ResetEditTracking();
    project.RefreshProjectWatch(); project.ResetEditTracking();
    QCOMPARE(sourceChanges.count(), 2);
    QCOMPARE(baselines.count(), 2); QVERIFY(resets.count() >= 2); QCOMPARE(documentChanges.count(), 0);
    QVERIFY(project.editRevision_.Generation() > initialGeneration);
    QVERIFY(project.IsEditTrackingUiBaselineCurrent(project.editRevision_.Generation(), project.editRevision_.Revision()));
    // 与真实 bench 一样，在 Open 的冷阶段结束后选择当前测试目标。
    QVERIFY(module.Reveal({"triggers", "trigger", "Caller", {}}));
    const auto selected = model->index(1, 0, model->index(2, 0)); QVERIFY(selected.isValid()); logic->setCurrentIndex(selected);
    const QPersistentModelIndex persistent(selected); (void)selected.data(Qt::DisplayRole);
    resets.clear(); QSignalSpy changes(model, &QAbstractItemModel::dataChanged);
    const auto original = Trigger::WtgWctCodec{}.Serialize(*project.triggerDocument_, *project.uiCatalog_); QVERIFY(original.Succeeded() && original.value);
    auto receipt = PrepareScalarUiEdit(project); QVERIFY(receipt.Succeeded() && receipt.value);
    auto command = std::make_unique<QtProject::TriggerGraphSnapshotUndoCommand>(project, std::move(*receipt.value), QStringLiteral("Late baseline scalar"));
    const auto published = command->FirstPublication(); project.triggerUndoStack_->push(command.release());
    QVERIFY(*published); QCOMPARE(resets.count(), 0); QVERIFY(changes.count() > 0); QVERIFY(persistent.isValid());
    QVERIFY(persistent.data(Qt::DisplayRole).toString().contains("NextText")); QCOMPARE(documentChanges.count(), 1);
    project.triggerUndoStack_->undo(); QCOMPARE(resets.count(), 0); QVERIFY(persistent.isValid()); QCOMPARE(documentChanges.count(), 2);
    const auto restored = Trigger::WtgWctCodec{}.Serialize(*project.triggerDocument_, *project.uiCatalog_);
    QVERIFY(restored.Succeeded() && restored.value); QVERIFY(restored.value->wtg == original.value->wtg && restored.value->wct == original.value->wct);
}

void TriggerModuleTests::BaselineResetRebuildsUnnotifiedSameOwnerTopology() {
    QTemporaryDir root; QVERIFY(root.isValid()); QtProject::EditorProject project;
    QVERIFY(InstallScalarUiProject(project, root.path().toStdWString())); Modules::TriggerModule module(project);
    auto* page = module.Widget(); QVERIFY(module.Reveal({"triggers", "trigger", "Caller", {}}));
    auto* logic = page->findChild<QTreeView*>("triggerLogicTree"); QVERIFY(logic);
    auto* model = dynamic_cast<TriggerEcaModel*>(logic->model()); QVERIFY(model);
    auto* caller = const_cast<Trigger::TriggerRecord*>(Named(project, "Caller")); QVERIFY(caller && caller->ecas.size() == 2);
    const auto epoch = model->PresentationEpoch(); const auto oldFirst = model->index(0, 0, model->index(2, 0)).data(TriggerEcaModel::StableIdRole);
    // 合法的原创 IR 拓扑变化，却故意漏掉业务 DocChanged，重现仅重记 stamp 无法察觉的旧树。
    std::reverse(caller->ecas.begin(), caller->ecas.end()); QCOMPARE(model->PresentationEpoch(), epoch);
    const auto wanted = project.triggerDocument_->ecaNodes[caller->ecas.front()].id; QVERIFY(oldFirst.toULongLong() != wanted);
    QSignalSpy resets(model, &QAbstractItemModel::modelReset); project.ResetEditTracking();
    QVERIFY(project.editTrackingReady_ && !project.editTrackingFailed_); QVERIFY(resets.count() > 0);
    QVERIFY(module.Reveal({"triggers", "trigger", "Caller", {}}));
    QCOMPARE(model->index(0, 0, model->index(2, 0)).data(TriggerEcaModel::StableIdRole).toULongLong(), wanted);
}

void TriggerModuleTests::BaselineUiSnapshotRejectsEarlySourceAndNestedReset_data() {
    QTest::addColumn<int>("kind"); QTest::newRow("early-catalog-same-owner-change") << 0;
    QTest::newRow("early-source-suspected") << 1; QTest::newRow("early-nested-reset") << 2;
}

void TriggerModuleTests::BaselineUiSnapshotRejectsEarlySourceAndNestedReset() {
    QFETCH(int, kind); QTemporaryDir root; QVERIFY(root.isValid()); QtProject::EditorProject project;
    const auto path = std::filesystem::path(root.path().toStdWString()); QVERIFY(InstallScalarUiProject(project, path));
    bool entered = false, outerRejected = false, replacementLoaded = true;
    // 早于 Module 注册的真实 direct 接收器：外层旧 payload 不得采纳重入后的新来源/代次。
    connect(&project, &QtProject::EditorProject::EditTrackingBaselineChanged, &project, [&](quint64 generation, quint64 revision) {
        if (entered) return; entered = true;
        if (kind == 0) {
            WriteUtf8(path / "ui/base/action.txt", "[PlainAction]\ndescription=Plain\n[UseText]\ntitle=Changed\ndescription=Use ${value}\n[[.args]]\ntype=string\n");
            auto replacement = Trigger::UiCatalog::Load(path); replacementLoaded = replacement.Succeeded() && replacement.value.has_value();
            if (replacementLoaded) *project.uiCatalog_ = std::move(*replacement.value);
        }
        if (kind == 1) project.sourceChangeSuspected_ = true;
        if (kind == 2) project.ResetEditTracking();
        outerRejected = !project.IsEditTrackingUiBaselineCurrent(generation, revision);
    });
    Modules::TriggerModule module(project); auto* page = module.Widget();
    auto* logic = page->findChild<QTreeView*>("triggerLogicTree"); QVERIFY(logic);
    auto* model = dynamic_cast<TriggerEcaModel*>(logic->model()); QVERIFY(model);
    QSignalSpy resets(model, &QAbstractItemModel::modelReset), baselines(&project, &QtProject::EditorProject::EditTrackingBaselineChanged);
    project.ResetEditTracking(); QVERIFY(entered); QVERIFY(replacementLoaded); QVERIFY(outerRejected);
    if (kind == 2) { QCOMPARE(baselines.count(), 2); QVERIFY(resets.count() > 0); }
    else { QCOMPARE(baselines.count(), 1); QCOMPARE(resets.count(), 0); }
}

void TriggerModuleTests::CommandOverflowKeepsFindReplaceKeyboardAndMouseReachable() {
    QTemporaryDir root; QVERIFY(root.isValid());
    QtProject::EditorProject project;
    QVERIFY(InstallCodeProject(project, root.path().toStdWString()));
    Modules::TriggerModule module(project);
    auto* page = module.Widget();
    auto* toolbar = page->findChild<UiKit::ResponsiveToolBar*>("triggerCommandBar");
    QPushButton* replace = nullptr;
    QPushButton* variables = nullptr;
    for (auto* button : page->findChildren<QPushButton*>()) {
        if (button->property("triggerFindReplaceButton").toBool()) replace = button;
        if (button->text() == QStringLiteral("变量…")) variables = button;
    }
    QVERIFY(toolbar && replace && variables);
    const auto before = Trigger::WtgWctCodec{}.Serialize(*project.triggerDocument_, *project.uiCatalog_);
    QVERIFY(before.Succeeded() && before.value);
    // 真 Undo 文案进入原命令按钮；长作者动作不能撑宽宿主或让后续命令失去入口。
    project.triggerUndoStack_->push(new QUndoCommand(QStringLiteral("作者动作 · %1").arg(QString(160, QChar(0x957f)))));
    for (const int width : {1080, 1920}) {
        page->resize(width, 900); page->show(); QApplication::processEvents();
        QCOMPARE(page->width(), width);
        toolbar->Refresh(); QApplication::processEvents();
        QVERIFY(toolbar->width() <= page->width());
        // 离屏插件不会在关闭工具对话框后自动激活父窗口；先恢复真实键盘输入宿主。
        QApplication::setActiveWindow(page);
        if (!variables->isHidden() && !replace->isHidden()) {
            variables->setFocus();
            QTRY_COMPARE(QApplication::focusWidget(), static_cast<QWidget*>(variables));
            QTest::keyClick(variables, Qt::Key_Tab);
            QTRY_COMPARE(QApplication::focusWidget(), static_cast<QWidget*>(replace));
            QVERIFY(toolbar->rect().contains(QRect(replace->mapTo(toolbar, QPoint()), replace->size())));
            QVERIFY(replace->width() >= replace->sizeHint().width());
            QTest::mouseClick(replace, Qt::LeftButton);
        } else {
            auto* more = toolbar->findChild<QToolButton*>("responsiveToolbarMore"); QVERIFY(more && more->isVisible());
            QAction* action = nullptr;
            for (auto* item : toolbar->OverflowMenu()->actions()) if (item->text() == QStringLiteral("查找/替换…")) action = item;
            QVERIFY(action && action->isEnabled());
            more->setFocus(); QTRY_COMPARE(QApplication::focusWidget(), static_cast<QWidget*>(more));
            action->trigger();
        }
        auto* dialog = page->findChild<QDialog*>("triggerReplaceDialog");
        QTRY_VERIFY(dialog && dialog->isVisible());
        dialog->reject();
        QTRY_VERIFY(!dialog->isVisible());
        QCOMPARE(project.triggerUndoStack_->count(), 1);
        QCOMPARE(project.triggerUndoStack_->index(), 1);
        const auto after = Trigger::WtgWctCodec{}.Serialize(*project.triggerDocument_, *project.uiCatalog_);
        QVERIFY(after.Succeeded() && after.value);
        QVERIFY(after.value->wtg == before.value->wtg && after.value->wct == before.value->wct);
    }
}

void TriggerModuleTests::AuthorCodeEntrypointsAt1080p() {
    QTemporaryDir root; QVERIFY(root.isValid());
    QtProject::EditorProject project;
    QVERIFY(InstallCodeProject(project, root.path().toStdWString()));
    Modules::TriggerModule module(project);
    auto* page = module.Widget(); page->resize(1920, 1080); page->show();
    QVERIFY(module.Reveal({"triggers", "trigger", "Init", {}}));
    QCoreApplication::processEvents(); page->layout()->activate();
    auto* create = page->findChild<QToolButton*>("triggerNewTrigger");
    auto* custom = page->findChild<QToolButton*>("triggerNewCustomCode");
    auto* convert = page->findChild<QPushButton*>("triggerConvertCode");
    auto* global = page->findChild<QPushButton*>("triggerGlobalCodeButton");
    auto* tree = page->findChild<QTreeView*>("triggerExplorer");
    auto* logic = page->findChild<QTreeView*>("triggerLogicTree");
    QVERIFY(create && custom && convert && global && tree && logic);
    QVERIFY(create->isVisible() && custom->isVisible() && convert->isVisible());
    QVERIFY(!custom->icon().isNull()); QCOMPARE(custom->icon().name(), QStringLiteral("lucide/file-code-2"));
    QCOMPARE(create->toolButtonStyle(), Qt::ToolButtonTextBesideIcon);
    QCOMPARE(create->popupMode(), QToolButton::MenuButtonPopup);
    QCOMPARE(convert->text(), QStringLiteral("转换为自定义代码…"));
    QVERIFY(convert->mapTo(page, QPoint{}).y() < logic->mapTo(page, QPoint{}).y());
    for (auto* button : {static_cast<QWidget*>(create), static_cast<QWidget*>(custom), static_cast<QWidget*>(convert)})
        QVERIFY(page->rect().contains(QRect(button->mapTo(page, QPoint{}), button->size())));
    QVERIFY(global->toolTip().contains(QStringLiteral("地图头部")));
    QVERIFY(global->toolTip().contains(QStringLiteral("JASS / vJass")));
    auto* dropdown = create->menu(); QVERIFY(dropdown);
    QStringList names;
    for (auto* action : dropdown->actions()) names << action->text();
    QCOMPARE(names, QStringList({QStringLiteral("新建触发器"), QStringLiteral("新建自定义代码"),
        QStringLiteral("新建注释"), QStringLiteral("新建类别")}));
    QVERIFY(!dropdown->actions()[1]->icon().isNull());
    Snapshot(page, "author-code-entries-1080");
    const auto captureDirectory = QString::fromLocal8Bit(qgetenv("WAR3_TEST_SCREENSHOT_DIR"));
    dropdown->popup(create->mapToGlobal(QPoint(0, create->height())));
    QTRY_VERIFY_WITH_TIMEOUT(dropdown->isVisible(), 1000);
    const auto dropdownError = PopupCapture::Save(*page, *dropdown, create, captureDirectory, "author-code-dropdown-1080");
    dropdown->close(); QVERIFY2(dropdownError.isEmpty(), qPrintable(dropdownError));

    int confirmations = 0;
    module.SetConfirmHandler([&](const auto&, const auto&, const auto&) { ++confirmations; return false; });
    QTest::mouseClick(convert, Qt::LeftButton);
    QCOMPARE(confirmations, 1); QCOMPARE(Named(project, "Init")->customTextFlag, 0);
    QCOMPARE(project.triggerUndoStack_->count(), 0);
    bool contextSeen = false, contextHasCreate = false, contextHasConvert = false;
    QString contextCaptureError;
    QObject timerOwner;
    QTimer::singleShot(0, &timerOwner, [&] {
        auto* menu = page->findChild<QMenu*>("triggerDirectoryContextMenu");
        if (!menu || !menu->isVisible()) return;
        contextSeen = true;
        QAction* convertAction = nullptr;
        for (auto* action : menu->actions()) {
            contextHasCreate |= action->objectName() == "triggerNewCodeAction" && !action->icon().isNull();
            if (action->objectName() == "triggerConvertCodeAction") {
                convertAction = action; contextHasConvert = action->isEnabled();
            }
        }
        contextCaptureError = PopupCapture::Save(*page, *menu, tree, captureDirectory, "author-code-context-1080");
        if (convertAction) QTest::mouseClick(menu, Qt::LeftButton, Qt::NoModifier, menu->actionGeometry(convertAction).center());
        else menu->close();
    });
    // 异常情况下仍能退出nested menu；回调随本次test owner销毁，不污染后续测试。
    QTimer::singleShot(1000, &timerOwner, [&] {
        if (auto* menu = page->findChild<QMenu*>("triggerDirectoryContextMenu")) menu->close();
    });
    const auto point = tree->visualRect(tree->currentIndex()).center();
    QVERIFY(QMetaObject::invokeMethod(tree, "customContextMenuRequested", Qt::DirectConnection, Q_ARG(QPoint, point)));
    QVERIFY(contextSeen && contextHasCreate && contextHasConvert);
    QVERIFY2(contextCaptureError.isEmpty(), qPrintable(contextCaptureError));
    QCOMPARE(confirmations, 2); QCOMPARE(project.triggerUndoStack_->count(), 0);

    bool nameDialogSeen = false;
    QTimer responder;
    connect(&responder, &QTimer::timeout, &timerOwner, [&] {
        if (auto* dialog = qobject_cast<QInputDialog*>(QApplication::activeModalWidget())) {
            nameDialogSeen = true; dialog->setTextValue(QStringLiteral("MenuCustom")); dialog->accept(); responder.stop();
        }
    });
    responder.start(5);
    QTimer::singleShot(1000, &timerOwner, [&] {
        if (auto* dialog = qobject_cast<QInputDialog*>(QApplication::activeModalWidget())) dialog->reject();
    });
    dropdown->popup(create->mapToGlobal(QPoint(0, create->height())));
    QTRY_VERIFY_WITH_TIMEOUT(dropdown->isVisible(), 1000);
    QTest::mouseClick(dropdown, Qt::LeftButton, Qt::NoModifier, dropdown->actionGeometry(dropdown->actions()[1]).center());
    responder.stop();
    QVERIFY(nameDialogSeen); QVERIFY(Named(project, "MenuCustom"));
    QCOMPARE(Named(project, "MenuCustom")->customTextFlag, 1);
    QCOMPARE(project.triggerUndoStack_->count(), 1);
    QVERIFY(!convert->isVisible());
    project.triggerUndoStack_->undo(); QVERIFY(!Named(project, "MenuCustom"));
}

void TriggerModuleTests::CodeCheckingColumnsMixedEncodingAndReadonlyBytes() {
    QByteArray mixed = QStringLiteral("// UTF8 中文；GBK ").toUtf8();
    mixed += QByteArray::fromHex("d6d0cec4"); mixed += QStringLiteral("；UTF8 尾声😀\n").toUtf8();
    const auto original = mixed;
    const auto decoded = Modules::DecodeMapCodeForView(mixed);
    QCOMPARE(decoded.text, QStringLiteral("// UTF8 中文；GBK 中文；UTF8 尾声😀\n")); QVERIFY(!decoded.complete); QVERIFY(decoded.reason.contains(QStringLiteral("推测"))); QCOMPARE(mixed, original);
    const auto lowTrail = Modules::DecodeMapCodeForView(QByteArray::fromHex("8140")); QCOMPARE(lowTrail.text, QString(QChar(0x4e02))); QVERIFY(!lowTrail.complete);
    const auto raw = Modules::ShowMapCodeRawBytes(mixed); QVERIFY(raw.text.contains(QStringLiteral("\\xD6\\xD0\\xCE\\xC4"))); QCOMPARE(mixed, original);
    std::stop_source stop; stop.request_stop(); QVERIFY(!Modules::DecodeMapCodeForView(mixed, stop.get_token()).complete);
    Services::CodeCheckResult checked; checked.completed = true; checked.syntaxChecked = true; checked.generation = 12;
    checked.diagnostics.push_back({DiagnosticSeverity::Error, "current.j", "C1", "Unicode token", 1, 11, 4, true});
    checked.diagnostics.push_back({DiagnosticSeverity::Error, "external.j", "C2", "Other source", 1, 11, 4, true});
    // ASCII(1) + 中文(6 bytes) + emoji(4 bytes): 1基byte11 落在emoji后半，使用边界byte8。
    checked.diagnostics.front().byteColumn = 8;
    const auto presented = Modules::PresentCodeCheckResult(checked, QStringLiteral("a中文😀x"), "current.j");
    QCOMPARE(presented.diagnostics.front().column, 4); QCOMPARE(presented.diagnostics.front().length, 2);
    // 没有冻结外部正文时只能可靠定位到行，不能把 byte column 冒充 UTF-16 column。
    QCOMPARE(presented.diagnostics.back().column, 1); QCOMPARE(presented.diagnostics.back().length, 0);
    QVERIFY(!presented.diagnostics.back().byteRangeExact); QVERIFY(!presented.semanticChecked);
    checked.externalSources.push_back({"external.j", "abcdefghijklmno\n"});
    const auto withExternal = Modules::PresentCodeCheckResult(checked, QStringLiteral("a中文😀x"), "current.j");
    QCOMPARE(withExternal.diagnostics.back().column, 11);
    checked.externalSources.push_back({"large-source.j", std::string(8 * 1024 * 1024, 'x')});
    const auto stoppedPresentation = Modules::PresentCodeCheckResult(checked, QStringLiteral("a中文😀x"), "current.j", stop.get_token());
    QVERIFY(stoppedPresentation.cancelled); QVERIFY(!stoppedPresentation.completed); QVERIFY(stoppedPresentation.externalSources.isEmpty());
    const auto declarations = Modules::CodeDeclarationsForView(QStringLiteral("// function Fake takes nothing returns nothing\nfunction Real takes nothing returns nothing\nendfunction"), "current.j", UiKit::CodeLanguage::Jass);
    QCOMPARE(declarations.size(), 1); QCOMPARE(declarations.front().name, QStringLiteral("Real")); QCOMPARE(declarations.front().line, 2);

    // F4：声明的说明取紧邻其上的连续注释块与行尾注释；空行打断注释块。
    Services::CodeCheckResult documented; documented.completed = true; documented.generation = 13;
    documented.externalSources.push_back({"common.j", "// unrelated\n\n// Creates a unit.\r\n/// Second line\nnative CreateUnit takes player id returns unit // trailing\nnative Plain takes nothing returns nothing\n"});
    Jass::DeclarationSymbol create; create.kind = Jass::DeclarationKind::Native; create.name = "CreateUnit"; create.type = "unit";
    create.source = "common.j"; create.line = 5; create.parameters.push_back({"player", "id"});
    Jass::DeclarationSymbol plain = create; plain.name = "Plain"; plain.line = 6; plain.parameters.clear(); plain.type = "nothing";
    documented.declarations.symbols = {create, plain};
    const auto withDocs = Modules::PresentCodeCheckResult(documented, QStringLiteral("x"), "current.j");
    QCOMPARE(withDocs.declarations.size(), 2);
    QCOMPARE(withDocs.declarations[0].documentation, QStringLiteral("Creates a unit.\nSecond line\ntrailing"));
    QCOMPARE(withDocs.declarations[0].signature, QStringLiteral("CreateUnit takes player id returns unit"));
    QVERIFY(withDocs.declarations[1].documentation.isEmpty());
}

void TriggerModuleTests::CodeUiUsesSharedProjectCheckerAndKeepsWctUntouched() {
    QTemporaryDir root; QVERIFY(root.isValid());
    QtProject::EditorProject project; QVERIFY(InstallCodeProject(project, root.path().toStdWString(), false));
    Modules::TriggerModule module(project); auto* page = module.Widget(); page->resize(1100, 800); page->show();
    QVERIFY(module.ShowGlobalCustomCode());
    auto* code = qobject_cast<UiKit::CodeView*>(module.CodeEditor()); QVERIFY(code);
    auto* problems = code->parentWidget()->findChild<UiKit::CodeProblemsView*>(); QVERIFY(problems);
    const auto before = project.triggerDocument_->wct.customCode.ToBytes(); QVERIFY(before.value);
    const auto undo = project.triggerUndoStack_->count();
    code->setPlainText(QStringLiteral("function Broken takes nothing returns nothing\n    set\nendfunction"));
    module.SetSearchText(QStringLiteral("Broken")); QCOMPARE(code->textCursor().selectedText(), QStringLiteral("Broken"));
    QVERIFY(page->findChild<QLineEdit*>("triggerSearch")->text().isEmpty());
    code->RequestCheck(); QTRY_VERIFY_WITH_TIMEOUT(!code->IsChecking(), 8000);
    QVERIFY(code->CheckResult().completed); QVERIFY(code->CheckResult().syntaxChecked); QVERIFY(!code->CheckResult().semanticChecked);
    QVERIFY(!code->CheckResult().diagnostics.isEmpty());
    QVERIFY(problems->IssueCount() > 0); QVERIFY(module.HasPendingCodeDraft());
    QCoreApplication::processEvents(); Snapshot(page, "c2-shared-code-check-errors");
    QCOMPARE(project.triggerUndoStack_->count(), undo); QCOMPARE(*project.triggerDocument_->wct.customCode.ToBytes().value, *before.value);
    module.DiscardPendingCodeDraft(); QVERIFY(!module.HasPendingCodeDraft());
}

void TriggerModuleTests::CustomCodeHeaderEncodingAndOneUndo() {
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QtProject::EditorProject project;
    const std::string header = "// \xd6\xd0\xce\xc4\r\nfunction Header takes nothing returns nothing\r\nendfunction\r\n";
    QVERIFY(InstallCodeProject(project, root.path().toStdWString(), false, header));
    QCOMPARE(project.triggerDocument_->wct.customCode.encoding, Trigger::TextEncoding::Gb18030);
    const auto original = Trigger::WtgWctCodec{}.Serialize(*project.triggerDocument_, *project.uiCatalog_);
    QVERIFY(original.Succeeded() && original.value);
    const auto originalCode = project.triggerDocument_->wct.customCode;
    Modules::TriggerModule module(project);
    auto* page = module.Widget(); page->resize(1450, 900); page->show();
    QVERIFY(module.ShowGlobalCustomCode());
    for (const auto* name : {"triggerApplyCode", "triggerDiscardCode", "triggerCodeDraftNote"}) {
        const auto* control = page->findChild<QWidget*>(name);
        QVERIFY(control && !control->isHidden());
    }
    QVERIFY(!page->findChild<QToolButton*>("triggerViewTraditional")->isEnabled());
    QVERIFY(!page->findChild<QToolButton*>("triggerViewBlueprint")->isEnabled());
    auto* editor = module.CodeEditor();
    QVERIFY(editor && !editor->isReadOnly());
    QVERIFY(editor->toPlainText().contains(QStringLiteral("中文")));
    QVERIFY(!editor->toPlainText().contains('\r'));
    QSignalSpy drafts(&module, &Modules::TriggerModule::DraftEdited);
    const auto clean = editor->toPlainText();
    editor->setPlainText(clean + QStringLiteral("// invalid ") + QChar(0));
    QVERIFY(!module.CommitPendingEdits());
    auto* discard = page->findChild<QPushButton*>("triggerDiscardCode");
    QVERIFY(discard && !discard->isHidden() && discard->isEnabled());
    discard->click();
    QCOMPARE(editor->toPlainText(), clean);
    QVERIFY(!module.HasPendingCodeDraft());
    editor->setPlainText(clean + QStringLiteral("// temporary"));
    const auto signalCount = drafts.count();
    editor->setPlainText(clean);
    QVERIFY(!module.HasPendingCodeDraft());
    QVERIFY(drafts.count() > signalCount);
    QVERIFY(module.CommitPendingEdits()); QVERIFY(module.CommitPendingEdits());
    QCOMPARE(project.triggerUndoStack_->count(), 0);
    const auto untouched = Trigger::WtgWctCodec{}.Serialize(*project.triggerDocument_, *project.uiCatalog_);
    QVERIFY(untouched.Succeeded() && untouched.value);
    QVERIFY(untouched.value->wct == original.value->wct);
    editor->appendPlainText(QStringLiteral("// 新注释"));
    QVERIFY(module.HasPendingCodeDraft());
    QVERIFY(module.CommitPendingEdits());
    QCOMPARE(project.triggerUndoStack_->count(), 1);
    QCOMPARE(project.triggerDocument_->wct.customCode.encoding, originalCode.encoding);
    QVERIFY(project.triggerDocument_->wct.customCode.text.find("新注释") != std::string::npos);
    QVERIFY(project.triggerDocument_->wct.customCode.text.find("\r\n") != std::string::npos);
    const auto bytes = project.triggerDocument_->wct.customCode.ToBytes();
    QVERIFY(bytes.Succeeded() && bytes.value);
    const std::string encoded(reinterpret_cast<const char*>(bytes.value->data()), bytes.value->size());
    QVERIFY(encoded.find("\xd0\xc2\xd7\xa2\xca\xcd") != std::string::npos);
    QApplication::processEvents(); page->layout()->activate();
    QVERIFY(page->findChild<QTreeView*>("triggerLogicTree")->isHidden());
    QVERIFY(editor->height() > page->height() / 2);
    Snapshot(page, "c1-global-code-applied");
    project.triggerUndoStack_->undo();
    QVERIFY(project.triggerDocument_->wct.customCode.originalBytes == originalCode.originalBytes);
    QCOMPARE(project.triggerDocument_->wct.customCode.text, originalCode.text);
    QVERIFY(module.CommitPendingEdits());
    QVERIFY(module.CreateCustomTrigger(QStringLiteral("AuthorCode")));
    const auto* created = Named(project, "AuthorCode");
    QVERIFY(created && created->customTextFlag == 1 && created->ecas.empty());
    const auto id = created->id;
    QVERIFY(created->customText.text.find("function InitTrig_AuthorCode takes nothing returns nothing") != std::string::npos);
    QVERIFY(created->customText.text.find("set gg_trg_AuthorCode = CreateTrigger()") != std::string::npos);
    const auto count = project.triggerUndoStack_->count();
    const auto names = Names(project);
    const auto dirty = project.triggerDirty_;
    QVERIFY(!module.RenameDirectoryItem({false, id, created->categoryId}, QStringLiteral("RenamedCode")));
    module.DuplicateCurrentTrigger(); module.CopyCurrentTrigger(); module.PasteTrigger();
    QCOMPARE(Names(project), names);
    QCOMPARE(project.triggerUndoStack_->count(), count);
    QCOMPARE(project.triggerDirty_, dirty);
    const auto originalText = Named(project, "AuthorCode")->customText.text;
    editor->appendPlainText(QStringLiteral("// 同时编辑属性"));
    auto* enabled = page->findChild<QCheckBox*>("triggerEnabledCheck");
    QVERIFY(enabled && enabled->isChecked());
    enabled->setChecked(false);
    QVERIFY(!module.HasPendingCodeDraft());
    QCOMPARE(Named(project, "AuthorCode")->enabled, 0);
    QVERIFY(Named(project, "AuthorCode")->customText.text != originalText);
    QCOMPARE(project.triggerUndoStack_->count(), count + 2);
    project.triggerUndoStack_->undo();
    QCOMPARE(Named(project, "AuthorCode")->enabled, 1);
    project.triggerUndoStack_->undo();
    QCOMPARE(Named(project, "AuthorCode")->customText.text, originalText);
}

void TriggerModuleTests::CodeUnicodeDisplayBaselinePreservesOriginalBytes_data() {
    QTest::addColumn<bool>("customTrigger");
    QTest::newRow("global-header") << false;
    QTest::newRow("custom-trigger") << true;
}

void TriggerModuleTests::CodeUnicodeDisplayBaselinePreservesOriginalBytes() {
    QFETCH(bool, customTrigger);
    QTemporaryDir root; QVERIFY(root.isValid());
    const auto originalText = QStringLiteral("// NBSP:a\u00a0b\r\n// LS:a\u2028// b\r\n// PS:a\u2029// b\r\nfunction InitTrig_Spawn takes nothing returns nothing\r\nendfunction\r\n");
    QtProject::EditorProject project;
    QVERIFY(InstallCodeProject(project, root.path().toStdWString(), false, originalText.toStdString()));
    // 回读原创来源，让自定义槽与全局头部都具有实际原字节与新的可信来源身份。
    if (customTrigger) QVERIFY(InstallParsedCustomCodeBody(project, originalText));
    const auto original = Trigger::WtgWctCodec{}.Serialize(*project.triggerDocument_, *project.uiCatalog_);
    QVERIFY(original.Succeeded() && original.value);
    const auto before = customTrigger ? Named(project, "Spawn")->customText : project.triggerDocument_->wct.customCode;
    QVERIFY(!before.originalBytes.empty());
    QVERIFY(before.text.find("\xc2\xa0") != std::string::npos);
    QVERIFY(before.text.find("\xe2\x80\xa8") != std::string::npos);
    QVERIFY(before.text.find("\xe2\x80\xa9") != std::string::npos);
    Modules::TriggerModule module(project); auto* page = module.Widget(); page->resize(1100, 800); page->show();
    if (customTrigger) QVERIFY(module.Reveal({"triggers", "trigger", QStringLiteral("Spawn"), {}}));
    else QVERIFY(module.ShowGlobalCustomCode());
    auto* editor = qobject_cast<UiKit::CodeView*>(module.CodeEditor()); QVERIFY(editor);
    QTRY_VERIFY_WITH_TIMEOUT(!editor->IsLoading(), 5000);
    QVERIFY(editor->isReadOnly());
    const auto readonlyNotice = QStringLiteral("只读：含特殊空白字符");
    QCOMPARE(page->findChild<QLabel*>("triggerCodeDraftNote")->text(), readonlyNotice);
    const auto displayBaseline = editor->toPlainText();
    QVERIFY(displayBaseline.contains(QStringLiteral("NBSP:a b")));
    QVERIFY(!displayBaseline.contains(QChar(0x00a0)));
    QVERIFY(!displayBaseline.contains(QChar(0x2028)));
    QVERIFY(!displayBaseline.contains(QChar(0x2029)));
    QVERIFY(!displayBaseline.contains('\r'));
    QVERIFY(!module.HasPendingCodeDraft()); QVERIFY(module.HasLoadedContent());
    editor->setFocus(); QTest::keyClicks(editor, "blocked");
    QCOMPARE(editor->toPlainText(), displayBaseline); QVERIFY(!module.HasPendingCodeDraft());
    QVERIFY(module.CommitPendingEdits());
    QCOMPARE(project.triggerUndoStack_->count(), 0);
    QCOMPARE(project.triggerUndoStack_->index(), 0);
    auto unchanged = Trigger::WtgWctCodec{}.Serialize(*project.triggerDocument_, *project.uiCatalog_);
    QVERIFY(unchanged.Succeeded() && unchanged.value);
    QVERIFY(unchanged.value->wtg == original.value->wtg && unchanged.value->wct == original.value->wct);
    editor->RequestCheck();
    QTRY_VERIFY_WITH_TIMEOUT(!editor->IsChecking(), 5000);
    QCOMPARE(editor->CheckResult().generation, editor->Generation());
    QVERIFY(!editor->CheckResult().completed); QVERIFY(!editor->CheckResult().syntaxChecked);
    QVERIFY(!editor->CheckResult().semanticChecked); QVERIFY(editor->CheckResult().diagnostics.isEmpty());
    QVERIFY(!editor->CheckResult().ownedProjectIndex);
    auto* problems = editor->parentWidget()->findChild<UiKit::CodeProblemsView*>(); QVERIFY(problems);
    QVERIFY(problems->StatusText().contains(QStringLiteral("未检查")));
    {
        // 编码说明作为“消息”条目列出。
        const auto* rows = problems->List()->View()->model(); bool listed = false;
        for (int row = 0; row < rows->rowCount(); ++row)
            listed = listed || rows->index(row, 2).data().toString().contains(QStringLiteral("特殊空白字符"));
        QVERIFY(listed); QCOMPARE(problems->IssueCount(), 0);
    }
    editor->setPlainText(displayBaseline + QStringLiteral("\n// programmatic change"));
    QVERIFY(module.HasPendingCodeDraft()); QVERIFY(!module.HasLoadedContent());
    QVERIFY(!module.CommitPendingEdits());
    QCOMPARE(project.triggerUndoStack_->count(), 0);
    unchanged = Trigger::WtgWctCodec{}.Serialize(*project.triggerDocument_, *project.uiCatalog_);
    QVERIFY(unchanged.Succeeded() && unchanged.value);
    QVERIFY(unchanged.value->wtg == original.value->wtg && unchanged.value->wct == original.value->wct);
    module.DiscardPendingCodeDraft();
    QCOMPARE(editor->toPlainText(), displayBaseline);
    QVERIFY(editor->isReadOnly()); QVERIFY(!module.HasPendingCodeDraft()); QVERIFY(module.HasLoadedContent());
    QVERIFY(module.CommitPendingEdits()); module.DiscardPendingCodeDraft();
    QVERIFY(module.CommitPendingEdits());
    QCOMPARE(project.triggerUndoStack_->count(), 0);
    unchanged = Trigger::WtgWctCodec{}.Serialize(*project.triggerDocument_, *project.uiCatalog_);
    QVERIFY(unchanged.Succeeded() && unchanged.value);
    QVERIFY(unchanged.value->wtg == original.value->wtg && unchanged.value->wct == original.value->wct);
    const auto& unedited = customTrigger ? Named(project, "Spawn")->customText : project.triggerDocument_->wct.customCode;
    QVERIFY(unedited.originalBytes == before.originalBytes); QCOMPARE(unedited.encoding, before.encoding);
    QCOMPARE(unedited.text, before.text);
}

void TriggerModuleTests::OrdinaryChineseCodeRemainsEditableAndUsesOneUndo_data() {
    QTest::addColumn<bool>("customTrigger");
    QTest::newRow("global-header") << false;
    QTest::newRow("custom-trigger") << true;
}

void TriggerModuleTests::OrdinaryChineseCodeRemainsEditableAndUsesOneUndo() {
    QFETCH(bool, customTrigger);
    QTemporaryDir root; QVERIFY(root.isValid());
    const auto body = QStringLiteral("// 普通中文正文\r\nfunction InitTrig_Spawn takes nothing returns nothing\r\nendfunction\r\n");
    QtProject::EditorProject project;
    QVERIFY(InstallCodeProject(project, root.path().toStdWString(), false, body.toStdString()));
    if (customTrigger) QVERIFY(InstallParsedCustomCodeBody(project, body));
    const auto original = Trigger::WtgWctCodec{}.Serialize(*project.triggerDocument_, *project.uiCatalog_);
    QVERIFY(original.Succeeded() && original.value);
    const auto before = customTrigger ? Named(project, "Spawn")->customText : project.triggerDocument_->wct.customCode;
    Modules::TriggerModule module(project); auto* page = module.Widget(); page->resize(1100, 800); page->show();
    if (customTrigger) QVERIFY(module.Reveal({"triggers", "trigger", QStringLiteral("Spawn"), {}}));
    else QVERIFY(module.ShowGlobalCustomCode());
    auto* editor = qobject_cast<UiKit::CodeView*>(module.CodeEditor()); QVERIFY(editor);
    QTRY_VERIFY_WITH_TIMEOUT(!editor->IsLoading(), 5000);
    QVERIFY(!editor->isReadOnly()); QVERIFY(module.HasLoadedContent());
    QVERIFY(!page->findChild<QLabel*>("triggerCodeDraftNote")->text().contains(QStringLiteral("特殊空白字符")));
    const auto displayBaseline = editor->toPlainText();
    QVERIFY(displayBaseline.contains(QStringLiteral("普通中文正文")));
    QVERIFY(!module.HasPendingCodeDraft()); QVERIFY(module.CommitPendingEdits());
    QCOMPARE(project.triggerUndoStack_->count(), 0);
    editor->moveCursor(QTextCursor::End); editor->insertPlainText(QStringLiteral("// temporary"));
    QVERIFY(module.HasPendingCodeDraft()); QVERIFY(!module.HasLoadedContent());
    editor->setFocus(); QTest::keyClick(editor, Qt::Key_Z, Qt::ControlModifier);
    QCOMPARE(editor->toPlainText(), displayBaseline);
    QVERIFY(!module.HasPendingCodeDraft()); QVERIFY(module.HasLoadedContent());
    QVERIFY(module.CommitPendingEdits()); module.DiscardPendingCodeDraft(); QVERIFY(module.CommitPendingEdits());
    QCOMPARE(project.triggerUndoStack_->count(), 0);
    const auto untouched = Trigger::WtgWctCodec{}.Serialize(*project.triggerDocument_, *project.uiCatalog_);
    QVERIFY(untouched.Succeeded() && untouched.value);
    QVERIFY(untouched.value->wtg == original.value->wtg && untouched.value->wct == original.value->wct);
    editor->appendPlainText(QStringLiteral("// 新的中文修改"));
    QVERIFY(module.HasPendingCodeDraft()); QVERIFY(module.CommitPendingEdits());
    QCOMPARE(project.triggerUndoStack_->count(), 1); QCOMPARE(project.triggerUndoStack_->index(), 1);
    QVERIFY(!module.HasPendingCodeDraft()); QVERIFY(module.HasLoadedContent());
    const auto& appliedText = customTrigger ? Named(project, "Spawn")->customText : project.triggerDocument_->wct.customCode;
    QVERIFY(appliedText.text.find("新的中文修改") != std::string::npos);
    project.triggerUndoStack_->undo();
    QCOMPARE(project.triggerUndoStack_->index(), 0);
    QVERIFY(!module.HasPendingCodeDraft()); QVERIFY(module.HasLoadedContent());
    const auto reverted = Trigger::WtgWctCodec{}.Serialize(*project.triggerDocument_, *project.uiCatalog_);
    QVERIFY(reverted.Succeeded() && reverted.value);
    QVERIFY(reverted.value->wtg == original.value->wtg && reverted.value->wct == original.value->wct);
    const auto& restored = customTrigger ? Named(project, "Spawn")->customText : project.triggerDocument_->wct.customCode;
    QVERIFY(restored.originalBytes == before.originalBytes); QCOMPARE(restored.encoding, before.encoding); QCOMPARE(restored.text, before.text);
    QVERIFY(module.CommitPendingEdits()); QCOMPARE(project.triggerUndoStack_->count(), 1);
}

void TriggerModuleTests::LargeCodeLanguageReloadCapturesCurrentDisplayBaseline() {
    QTemporaryDir root; QVERIFY(root.isValid());
    const std::string body = "//" + std::string(2 * 1024 * 1024 + 32, ' ')
        + "\r\nfunction Header takes nothing returns nothing\r\nendfunction\r\n";
    QtProject::EditorProject project;
    QVERIFY(InstallCodeProject(project, root.path().toStdWString(), false, body));
    const auto before = project.triggerDocument_->wct.customCode;
    const auto normalized = Trigger::TriggerCodeEditor::NormalizedText(before);
    QVERIFY(QString::fromUtf8(normalized).size() > 2 * 1024 * 1024);
    const auto noOp = Trigger::TriggerCodeEditor::PrepareText(*project.triggerDocument_, *project.uiCatalog_,
        {Trigger::CodeBlockKind::GlobalHeader, 0}, normalized);
    QVERIFY(noOp.Succeeded() && noOp.value && !noOp.value->Changed());
    const auto original = Trigger::WtgWctCodec{}.Serialize(*project.triggerDocument_, *project.uiCatalog_);
    QVERIFY(original.Succeeded() && original.value);
    Modules::TriggerModule module(project); auto* page = module.Widget(); page->resize(1100, 800); page->show();
    QVERIFY(module.ShowGlobalCustomCode());
    auto* editor = qobject_cast<UiKit::CodeView*>(module.CodeEditor()); QVERIFY(editor);
    QVERIFY(editor->IsLoading()); QVERIFY(editor->isReadOnly()); QVERIFY(!module.HasLoadedContent());
    const auto firstGeneration = editor->Generation();
    auto* language = page->findChild<QComboBox*>("triggerCodeLanguage"); QVERIFY(language && language->isEnabled());
    language->setCurrentIndex(0); language->setCurrentIndex(1);
    QVERIFY(editor->Generation() > firstGeneration); QVERIFY(editor->IsLoading()); QVERIFY(editor->isReadOnly());
    QTRY_VERIFY_WITH_TIMEOUT(!editor->IsLoading(), 10000);
    QVERIFY(!editor->isReadOnly()); QCOMPARE(editor->Language(), UiKit::CodeLanguage::VJass);
    QCOMPARE(editor->toPlainText(), QString::fromUtf8(normalized));
    QVERIFY(!module.HasPendingCodeDraft()); QVERIFY(module.HasLoadedContent()); QVERIFY(module.CommitPendingEdits());
    QCOMPARE(project.triggerUndoStack_->count(), 0); QCOMPARE(project.triggerUndoStack_->index(), 0);
    const auto unchanged = Trigger::WtgWctCodec{}.Serialize(*project.triggerDocument_, *project.uiCatalog_);
    QVERIFY(unchanged.Succeeded() && unchanged.value);
    QVERIFY(unchanged.value->wtg == original.value->wtg && unchanged.value->wct == original.value->wct);
    QVERIFY(project.triggerDocument_->wct.customCode.originalBytes == before.originalBytes);
    QCOMPARE(project.triggerDocument_->wct.customCode.encoding, before.encoding);
    QCOMPARE(project.triggerDocument_->wct.customCode.text, before.text);
}

void TriggerModuleTests::CustomCodeDraftRejectsNavigationAndProperties() {
    QTemporaryDir root;
    QtProject::EditorProject project;
    QVERIFY(InstallCodeProject(project, root.path().toStdWString()));
    project.projectRoot_ = root.path().toStdWString();
    WriteUtf8(project.projectRoot_ / "map/war3map.j", "function main takes nothing returns nothing\nendfunction\n");
    Modules::TriggerModule module(project);
    auto* page = module.Widget(); page->resize(1450, 900); page->show();
    QVERIFY(module.CreateCustomTrigger(QStringLiteral("DraftCode")));
    const auto id = Named(project, "DraftCode")->id;
    const auto originalText = Named(project, "DraftCode")->customText.text;
    const auto count = project.triggerUndoStack_->count();
    auto* editor = module.CodeEditor();
    editor->setPlainText(editor->toPlainText() + QStringLiteral("\n// invalid ") + QChar(0));
    QVERIFY(editor->toPlainText().contains(QChar(0)));
    const auto draft = editor->toPlainText();
    QVERIFY(module.HasPendingCodeDraft());
    QVERIFY(!module.CommitPendingEdits());
    QVERIFY(!module.Reveal({"triggers", "trigger", "Init", {}}));
    QCOMPARE(editor->toPlainText(), draft);
    auto* explorer = page->findChild<QTreeView*>("triggerExplorer");
    auto* filter = dynamic_cast<QSortFilterProxyModel*>(explorer->model());
    auto* model = dynamic_cast<TriggerExplorerModel*>(filter->sourceModel());
    explorer->setCurrentIndex(filter->mapFromSource(model->IndexForTriggerId(Named(project, "Init")->id)));
    QCOMPARE(explorer->currentIndex().data(TriggerExplorerModel::StableIdRole).toULongLong(), id);
    auto* enabled = page->findChild<QCheckBox*>("triggerEnabledCheck");
    enabled->setChecked(false);
    QVERIFY(enabled->isChecked());
    QCOMPARE(Named(project, "DraftCode")->enabled, 1);
    page->findChild<QPushButton*>("triggerMapScriptButton")->click();
    QVERIFY(page->findChild<UiKit::CodeView*>("triggerMapScriptView")->isHidden());
    module.NewTrigger();
    QCOMPARE(project.triggerUndoStack_->count(), count);
    QCOMPARE(Named(project, "DraftCode")->customText.text, originalText);
    QVERIFY(module.HasPendingCodeDraft());
    QCOMPARE(editor->toPlainText(), draft);
    Snapshot(page, "c1-invalid-code-draft-preserved");
    module.DiscardPendingCodeDraft();
    QVERIFY(!module.HasPendingCodeDraft());
    QCOMPARE(editor->toPlainText(), QString::fromUtf8(Trigger::TriggerCodeEditor::NormalizedText(Named(project, "DraftCode")->customText)));
    QVERIFY(module.CommitPendingEdits());
    QVERIFY(module.Reveal({"triggers", "trigger", "Init", {}}));
}

void TriggerModuleTests::CustomCodeDraftStaysBoundThroughUndo() {
    QTemporaryDir root;
    QtProject::EditorProject project;
    QVERIFY(InstallCodeProject(project, root.path().toStdWString()));
    Modules::TriggerModule module(project);
    (void)module.Widget();
    QVERIFY(module.CreateCustomTrigger(QStringLiteral("UndoCode")));
    const auto initial = Named(project, "UndoCode")->customText.text;
    auto* editor = module.CodeEditor();
    editor->appendPlainText(QStringLiteral("// applied"));
    QVERIFY(module.CommitPendingEdits());
    editor->appendPlainText(QStringLiteral("// unsaved draft"));
    const auto draft = editor->toPlainText();
    project.triggerUndoStack_->undo();
    QCOMPARE(Named(project, "UndoCode")->customText.text, initial);
    QVERIFY(module.HasPendingCodeDraft());
    QCOMPARE(editor->toPlainText(), draft);
    QVERIFY(!module.CommitPendingEdits());
    QCOMPARE(Named(project, "UndoCode")->customText.text, initial);
    QCOMPARE(project.triggerUndoStack_->count(), 2);
    QCOMPARE(project.triggerUndoStack_->index(), 1);
    module.DiscardPendingCodeDraft();
    editor->appendPlainText(QStringLiteral("// fresh draft"));
    QVERIFY(module.CommitPendingEdits());
    QCOMPARE(project.triggerUndoStack_->count(), 2);
    QVERIFY(Named(project, "UndoCode")->customText.text.find("fresh draft") != std::string::npos);
}

void TriggerModuleTests::GuiConversionUsesSealedConfirmationAndClosureUndo() {
    QTemporaryDir root;
    QtProject::EditorProject project;
    QVERIFY(InstallCodeProject(project, root.path().toStdWString()));
    const auto id = Named(project, "Init")->id;
    QVERIFY(Trigger::TriggerDocumentEditor::AppendRootEca(*project.triggerDocument_, *project.uiCatalog_, id,
        Trigger::UiDefinitionKind::Event, "MapInitializationEvent").Succeeded());
    Script::MapScriptInputs inputs; inputs.triggers = project.triggerDocument_.get(); inputs.catalog = project.uiCatalog_.get();
    const auto generated = Script::GenerateGuiTriggerSource(inputs, id);
    QVERIFY(generated.Succeeded() && generated.value && generated.value->effectiveRunOnInit);
    const auto original = Trigger::WtgWctCodec{}.Serialize(*project.triggerDocument_, *project.uiCatalog_);
    QVERIFY(original.Succeeded() && original.value);
    const auto ecas = project.triggerDocument_->ecaNodes.size();
    Modules::TriggerModule module(project);
    auto* page = module.Widget(); page->resize(1450, 900); page->show();
    QVERIFY(module.Reveal({"triggers", "trigger", "Init", {}}));
    module.SetConfirmHandler([](const auto&, const auto&, const auto&) { return false; });
    QVERIFY(!module.ConvertCurrentTriggerToCustomCode());
    QCOMPARE(project.triggerUndoStack_->count(), 0);
    module.SetConfirmHandler([&](const auto&, const auto&, const auto&) {
        Trigger::TriggerDocumentEditor::FindTrigger(*project.triggerDocument_, id)->enabled = 0;
        return true;
    });
    QVERIFY(!module.ConvertCurrentTriggerToCustomCode());
    QCOMPARE(Named(project, "Init")->customTextFlag, 0);
    QCOMPARE(project.triggerDocument_->ecaNodes.size(), ecas);
    QCOMPARE(project.triggerUndoStack_->count(), 0);
    Trigger::TriggerDocumentEditor::FindTrigger(*project.triggerDocument_, id)->enabled = 1;
    module.RefreshEditor();
    module.SetConfirmHandler([](const auto&, const auto&, const auto&) { return true; });
    QVERIFY(module.ConvertCurrentTriggerToCustomCode());
    QCOMPARE(Named(project, "Init")->customTextFlag, 1);
    QCOMPARE(Named(project, "Init")->runOnInit, 1);
    QVERIFY(Named(project, "Init")->ecas.empty());
    QCOMPARE(Named(project, "Init")->customText.text, generated.value->text);
    QCOMPARE(project.triggerUndoStack_->count(), 1);
    QVERIFY(!module.CodeEditor()->isHidden());
    QApplication::processEvents(); page->layout()->activate();
    QVERIFY(page->findChild<QTreeView*>("triggerLogicTree")->isHidden());
    QVERIFY(page->findChild<UiKit::CodeView*>("triggerMapScriptView")->isHidden());
    QVERIFY(module.CodeEditor()->height() > page->height() / 2);
    Snapshot(page, "c1-gui-converted-code");
    project.triggerUndoStack_->undo();
    QCOMPARE(Named(project, "Init")->customTextFlag, 0);
    QCOMPARE(Named(project, "Init")->runOnInit, 0);
    QVERIFY(module.CodeEditor()->isHidden());
    QCOMPARE(project.triggerDocument_->ecaNodes.size(), ecas);
    const auto undone = Trigger::WtgWctCodec{}.Serialize(*project.triggerDocument_, *project.uiCatalog_);
    QVERIFY(undone.Succeeded() && undone.value);
    QVERIFY(undone.value->wtg == original.value->wtg && undone.value->wct == original.value->wct);
    project.triggerUndoStack_->redo();
    QCOMPARE(Named(project, "Init")->customText.text, generated.value->text);
    QVERIFY(!module.CodeEditor()->isHidden());
    // 代码未改：转回 T 按转换前快照原样恢复，WTG/WCT 与转换前逐字节一致；撤销回到代码。
    QVERIFY(!page->findChild<QPushButton*>("triggerRecoverCode")->isHidden());
    QVERIFY(module.ConvertCurrentCodeToTrigger());
    QCOMPARE(Named(project, "Init")->customTextFlag, 0);
    QVERIFY(module.CodeEditor()->isHidden());
    const auto recovered = Trigger::WtgWctCodec{}.Serialize(*project.triggerDocument_, *project.uiCatalog_);
    QVERIFY(recovered.Succeeded() && recovered.value);
    QVERIFY(recovered.value->wtg == original.value->wtg && recovered.value->wct == original.value->wct);
    QCOMPARE(project.triggerUndoStack_->count(), 2);
    project.triggerUndoStack_->undo();
    QCOMPARE(Named(project, "Init")->customTextFlag, 1);
    QCOMPARE(Named(project, "Init")->customText.text, generated.value->text);
    // 改成 T 写法以外的代码：拒绝并保持代码不变。
    module.RefreshEditor();
    module.CodeEditor()->appendPlainText(QStringLiteral("function Extra takes nothing returns nothing\nlocal integer x = 1\nendfunction"));
    QVERIFY(!module.ConvertCurrentCodeToTrigger());
    QCOMPARE(Named(project, "Init")->customTextFlag, 1);
}

void TriggerModuleTests::FirstCustomCodeCreatesCategoryTogetherAndRejectsLua() {
    QTemporaryDir root;
    QtProject::EditorProject project;
    QVERIFY(InstallCodeProject(project, root.path().toStdWString(), true));
    Modules::TriggerModule module(project);
    (void)module.Widget();
    QVERIFY(module.CreateCustomTrigger(QStringLiteral("FirstCode")));
    QCOMPARE(project.triggerDocument_->categories.size(), std::size_t{1});
    QCOMPARE(project.triggerDocument_->triggers.size(), std::size_t{1});
    QCOMPARE(project.triggerUndoStack_->count(), 1);
    const auto id = Named(project, "FirstCode")->id;
    project.triggerUndoStack_->undo();
    QVERIFY(project.triggerDocument_->categories.empty()); QVERIFY(project.triggerDocument_->triggers.empty());
    project.triggerUndoStack_->redo();
    QCOMPARE(Named(project, "FirstCode")->id, id);
    project.luaMapTarget_.emplace();
    const auto count = project.triggerUndoStack_->count();
    QVERIFY(!module.CreateCustomTrigger(QStringLiteral("WrongJassTemplate")));
    QVERIFY(!module.ConvertCurrentTriggerToCustomCode());
    QCOMPARE(project.triggerUndoStack_->count(), count);
    QCOMPARE(project.triggerDocument_->triggers.size(), std::size_t{1});
    project.luaMapTarget_.reset();
    const std::string luaBody = "function main() end\n";
    const auto luaBytes = std::as_bytes(std::span(luaBody.data(), luaBody.size()));
    const std::vector<MapArchiveMember> members{{"Scripts/war3map.lua", {luaBytes.begin(), luaBytes.end()}}};
    const auto archivePath = std::filesystem::path(root.path().toStdWString()) / "nested-lua-synthetic.w3x";
    QVERIFY(WriteVerifiedArchive(archivePath, members).Succeeded());
    auto archive = MapArchiveSession::Open(archivePath);
    QVERIFY(archive.Succeeded() && archive.value);
    project.archiveSession_ = std::make_unique<MapArchiveSession>(std::move(*archive.value));
    QVERIFY(!module.CreateCustomTrigger(QStringLiteral("WrongNestedLuaJass")));
    QCOMPARE(project.triggerUndoStack_->count(), count);
    QCOMPARE(project.triggerDocument_->triggers.size(), std::size_t{1});
}

void TriggerModuleTests::RealAudastraNameSamplesRenderReadOnlyProjection() {
    const QString sampleRoot = qEnvironmentVariable("WAR3_NAME_SAMPLE_DIR");
    const QString gameRoot = qEnvironmentVariable("WAR3_NAME_GAME_ROOT");
    const QString toolRoot = qEnvironmentVariable("WAR3_NAME_UI_ROOT");
    const QString sourceMap = qEnvironmentVariable("WAR3_NAME_SOURCE_MAP");
    if (sampleRoot.isEmpty() || gameRoot.isEmpty() || toolRoot.isEmpty())
        QSKIP("真实来源离屏 UI 验收需明确 WAR3_NAME_SAMPLE_DIR / GAME_ROOT / UI_ROOT；默认不读取作者文件。");
    const auto read = [&](const QString& name) {
        QFile file(QDir(sampleRoot).filePath(name));
        if (!file.open(QIODevice::ReadOnly)) return std::vector<std::byte>{};
        const auto bytes = file.readAll();
        const auto* begin = reinterpret_cast<const std::byte*>(bytes.constData());
        return std::vector<std::byte>(begin, begin + bytes.size());
    };
    const auto wtg = read("war3map.wtg"), wct = read("war3map.wct"), wts = read("war3map.wts"), w3u = read("war3map.w3u");
    QVERIFY(!wtg.empty() && !wct.empty() && !wts.empty() && !w3u.empty());
    auto catalog = Trigger::UiCatalog::Load(toolRoot.toStdWString());
    QVERIFY(catalog.Succeeded() && catalog.value);
    auto parsed = Trigger::WtgWctCodec{}.Parse(wtg, wct, *catalog.value);
    QVERIFY(parsed.Succeeded() && parsed.value);
    auto game = Game::GameDataSource::MountStrict(gameRoot.toStdWString());
    QVERIFY(game.Succeeded() && game.value);
    auto standards = ObjectData::StandardObjectCatalog::Load(*game.value);
    QVERIFY(standards.Succeeded() && standards.value);
    auto objects = ObjectData::NativeObjectCodec(ObjectData::ObjectType::Unit).Parse(w3u);
    QVERIFY(objects.Succeeded() && objects.document);
    QtProject::EditorProject project;
    project.uiCatalog_ = std::make_unique<Trigger::UiCatalog>(std::move(*catalog.value));
    project.triggerDocument_ = std::make_unique<Trigger::TriggerDocument>(std::move(*parsed.value));
    project.standardCatalog_ = std::make_unique<ObjectData::StandardObjectCatalog>(std::move(*standards.value));
    project.nativeObjectDocuments_[0].reset(dynamic_cast<ObjectData::NativeObjectDocument*>(objects.document.release()));
    QVERIFY(project.nativeObjectDocuments_[0]);
    project.mapDocument_ = std::make_unique<MapDocument>(MapDocument::FromMembers(
        std::vector<MapArchiveMember>{{"war3map.wts", wts}, {"war3map.w3u", w3u}}));
    project.mapEdits_ = std::make_unique<MapData::MapDataEditSession>(*project.mapDocument_);
    QVERIFY(project.iconProvider_->SetGameRoot(gameRoot.toStdWString()));
    project.iconProvider_->SetUiToolRoot(toolRoot.toStdWString());
    project.iconProvider_->MergeCatalogCategories(*project.uiCatalog_);
    if (!sourceMap.isEmpty()) QVERIFY(project.iconProvider_->SetMapArchive(sourceMap.toStdWString()));
    project.triggerUndoStack_ = new QUndoStack(&project);

    // 给四条真实单位语句及一条实际 Debug 文本建立可见投影；原触发器引用保留完整 wire 原值。
    // 原输入成员不修改、不保存；图片上的“原值”是当前程序复现旧 Null 展示缺口，非旧程序截图。
    const std::array<std::string, 4> wanted{"Hpal", "Hamg", "U043", "H01W"};
    std::array<std::uint32_t, 4> chosen{};
    std::array<StableId, 4> argumentIds{};
    for (std::size_t sample = 0; sample < wanted.size(); ++sample) {
        bool found = false;
        for (std::size_t i = 0; i < project.triggerDocument_->ecaNodes.size(); ++i) {
            const auto& eca = project.triggerDocument_->ecaNodes[i];
            if (eca.name.text != "YDWESaveAnyTypeDataByUserData" || eca.arguments.size() < 2) continue;
            const auto& argument = project.triggerDocument_->argumentNodes[eca.arguments[1]];
            if (argument.type != 3 || argument.value.text != wanted[sample]) continue;
            chosen[sample] = static_cast<std::uint32_t>(i);
            argumentIds[sample] = argument.id;
            found = true;
            break;
        }
        QVERIFY2(found, wanted[sample].c_str());
        const auto name = ResolveProjectName(project, wanted[sample], "unitcode");
        QCOMPARE(name.state, Services::NameResolutionState::Resolved);
        QVERIFY(name.displayName != wanted[sample]);
        if (!sourceMap.isEmpty())
            QVERIFY2(!project.iconProvider_->IconForTexture(QString::fromStdString(name.iconPath)).isNull(), name.iconPath.c_str());
        qInfo() << "real-source-name" << QString::fromStdString(wanted[sample]) << QString::fromStdString(name.displayName)
                << QString::fromStdString(name.reference) << QString::fromStdString(name.iconPath);
    }
    std::optional<std::uint32_t> scalarEca;
    StableId scalarArgumentId = 0;
    for (std::size_t index = 0; index < project.triggerDocument_->ecaNodes.size(); ++index) {
        const auto& eca = project.triggerDocument_->ecaNodes[index];
        if (eca.name.text != "BJDebugMsg" || eca.arguments.size() != 1) continue;
        const auto& argument = project.triggerDocument_->argumentNodes[eca.arguments.front()];
        if (argument.type != 3 || argument.callEca || argument.value.text != "TRIGSTR_015") continue;
        scalarEca = static_cast<std::uint32_t>(index);
        scalarArgumentId = argument.id;
        break;
    }
    QVERIFY(scalarEca);
    const auto scalarName = ResolveProjectName(project, "TRIGSTR_015", "StringExt");
    QCOMPARE(scalarName.state, Services::NameResolutionState::Resolved);
    QCOMPARE(scalarName.displayName, std::string("显示减伤"));
    const auto& triggerArgument = project.triggerDocument_->argumentNodes[
        project.triggerDocument_->ecaNodes[chosen[0]].arguments[4]];
    const auto triggerArgumentId = triggerArgument.id;
    const auto triggerRaw = triggerArgument.value.text;
    QCOMPARE(triggerRaw, std::string("gg_trg_9_0_2_________________u"));
    const auto triggerName = ResolveProjectName(project, triggerRaw, "trigger");
    QCOMPARE(triggerName.state, Services::NameResolutionState::Resolved);
    QCOMPARE(triggerName.displayName, std::string("9-0-2 提里奥.弗丁"));
    QVERIFY(triggerName.candidates.empty());
    qInfo() << "real-source-scalar" << QString::fromStdString(scalarName.rawValue) << QString::fromStdString(scalarName.displayName);
    qInfo() << "real-source-trigger-reference" << QString::fromStdString(triggerRaw) << QString::fromStdString(triggerName.displayName);
    Trigger::TriggerRecord projection;
    for (const auto& trigger : project.triggerDocument_->triggers) projection.id = std::max(projection.id, trigger.id);
    ++projection.id;
    projection.name.text = "真实来源名称样本（离屏视图投影）";
    projection.categoryId = project.triggerDocument_->triggers.front().categoryId;
    projection.ecas.assign(chosen.begin(), chosen.end());
    projection.ecas.push_back(*scalarEca);
    const auto projectionId = projection.id;
    project.triggerDocument_->triggers.push_back(std::move(projection));
    for (const auto index : chosen) project.triggerDocument_->ecaNodes[index].sourceTriggerId = projectionId;
    project.triggerDocument_->ecaNodes[*scalarEca].sourceTriggerId = projectionId;
    Modules::TriggerModule module(project);
    auto* page = module.Widget();
    page->resize(1450, 900); page->show();
    auto* provenance = new QLabel(page);
    provenance->setGeometry(250, 0, 1180, 28);
    provenance->setStyleSheet(QStringLiteral("background:#202725; color:#cce1d8; padding:4px;"));
    provenance->show(); provenance->raise();
    QVERIFY(module.Reveal({"triggers", "trigger", "真实来源名称样本（离屏视图投影）", {}}));
    auto* raw = page->findChild<QCheckBox*>("triggerRawValues");
    auto* view = page->findChild<QTreeView*>("triggerLogicTree");
    auto* model = dynamic_cast<TriggerEcaModel*>(view->model());
    QVERIFY(raw && model);
    provenance->setText(QStringLiteral("当前程序原始值对照：真实单位 / WTS 文本 / 触发器引用 · 离屏投影 · 非旧程序截图"));
    raw->setChecked(true); view->expandAll(); QApplication::processEvents();
    Snapshot(page, "n1-real-before-null-raw-projection");
    provenance->setText(QStringLiteral("当前程序 N1 统一解析 · 真实 WTG/W3U/WTS 与本机只读游戏目录 · 离屏投影，未运行 YDWE"));
    raw->setChecked(false); view->expandAll(); QApplication::processEvents();
    for (const auto index : chosen) {
        const auto display = model->IndexForStableId(project.triggerDocument_->ecaNodes[index].id);
        QVERIFY(display.isValid());
        QCOMPARE(display.data(TriggerEcaModel::ArgumentSpansRole).toList()[1].toMap()["type"].toString(), QStringLiteral("unitcode"));
        QVERIFY(!display.data().toString().contains(QString::fromStdString(project.triggerDocument_->argumentNodes[project.triggerDocument_->ecaNodes[index].arguments[1]].value.text)));
    }
    const auto scalarRow = model->IndexForStableId(project.triggerDocument_->ecaNodes[*scalarEca].id);
    QVERIFY(scalarRow.isValid());
    QVERIFY(scalarRow.data().toString().contains(QStringLiteral("显示减伤")));
    QVERIFY(!scalarRow.data().toString().contains(QStringLiteral("TRIGSTR_015")));
    const auto scalarSpan = scalarRow.data(TriggerEcaModel::ArgumentSpansRole).toList().front().toMap();
    QCOMPARE(scalarSpan["raw"].toString(), QStringLiteral("TRIGSTR_015"));
    QCOMPARE(scalarSpan["reference"].toString(), QStringLiteral("TRIGSTR_015"));
    QCOMPARE(scalarSpan["state"].toString(), QStringLiteral("resolved"));
    QVERIFY(scalarSpan["badge"].toBool());
    const auto referenceRow = model->IndexForStableId(project.triggerDocument_->ecaNodes[chosen[0]].id);
    const auto referenceSpan = referenceRow.data(TriggerEcaModel::ArgumentSpansRole).toList()[4].toMap();
    QCOMPARE(referenceSpan["raw"].toString(), QString::fromStdString(triggerRaw));
    QCOMPARE(referenceSpan["type"].toString(), QStringLiteral("trigger"));
    QCOMPARE(referenceSpan["state"].toString(), QStringLiteral("resolved"));
    QVERIFY(referenceRow.data().toString().contains(QString::fromStdString(triggerName.displayName)));
    QVERIFY(!referenceRow.data().toString().contains(QString::fromStdString(triggerRaw)));
    Snapshot(page, "n1-real-after-resolved-projection");
    auto* blueprintButton = page->findChild<QToolButton*>("triggerViewBlueprint");
    QVERIFY(blueprintButton);
    blueprintButton->click(); QApplication::processEvents();
    module.Blueprint()->Fit(); QApplication::processEvents();
    Snapshot(page, "n1-real-after-blueprint-projection");
    for (std::size_t sample = 0; sample < wanted.size(); ++sample)
        QCOMPARE(Trigger::TriggerDocumentEditor::FindArgument(*project.triggerDocument_, argumentIds[sample])->value.text, wanted[sample]);
    QCOMPARE(Trigger::TriggerDocumentEditor::FindArgument(*project.triggerDocument_, scalarArgumentId)->value.text, std::string("TRIGSTR_015"));
    QCOMPARE(Trigger::TriggerDocumentEditor::FindArgument(*project.triggerDocument_, triggerArgumentId)->value.text, triggerRaw);
    QCOMPARE(project.triggerUndoStack_->count(), 0);
}

void TriggerModuleTests::SharedMapStringsUseOneUndoAndPreserveOriginalBody() {
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const std::filesystem::path ui = root.path().toStdWString();
    WriteUtf8(ui / "ui/config", "base\n");
    WriteUtf8(ui / "ui/base/define.txt", "[TriggerTypes]\nstring=0,1,1,字符串\ntrigger=0,1,1,触发器\n");
    WriteUtf8(ui / "ui/base/action.txt",
        "[PlainAction]\ndescription=Plain\n[RunTrigger]\ndescription=运行 ${触发器}\n[[.args]]\ntype=trigger\n"
        "[TextPair]\ndescription=文字 ${第一处} 和 ${第二处}\n[[.args]]\ntype=string\n[[.args]]\ntype=string\n");
    auto catalog = Trigger::UiCatalog::Load(ui);
    QVERIFY(catalog.Succeeded());
    auto parsed = Trigger::WtgWctCodec{}.Parse(DirectoryWtg(), DirectoryWct(), *catalog.value);
    QVERIFY(parsed.Succeeded());
    QtProject::EditorProject project;
    project.triggerUndoStack_ = new QUndoStack(&project);
    project.uiCatalog_ = std::make_unique<Trigger::UiCatalog>(std::move(*catalog.value));
    project.triggerDocument_ = std::make_unique<Trigger::TriggerDocument>(std::move(*parsed.value));
    const auto triggerId = Named(project, "Caller")->id;
    const auto appended = Trigger::TriggerDocumentEditor::AppendRootEca(*project.triggerDocument_, *project.uiCatalog_,
        triggerId, Trigger::UiDefinitionKind::Action, "TextPair");
    QVERIFY(appended.Succeeded());
    QVERIFY(Trigger::TriggerDocumentEditor::SetScalarArgument(*project.triggerDocument_, appended.value->appendedArguments[0].id,
        3, "TRIGSTR_008").Succeeded());
    QVERIFY(Trigger::TriggerDocumentEditor::SetScalarArgument(*project.triggerDocument_, appended.value->appendedArguments[1].id,
        3, "\"TRIGSTR_8\"").Succeeded());
    // 真实经典地图也含 GB18030：正文按源编码编辑，未编辑必须保留 GBK 字节与 CRLF。
    const std::string originalBody = "|cffff0000\xba\xec\xc9\xab|r\r\n\xb5\xda\xb6\xfe\xd0\xd0";
    const std::string wts = "STRING 8\r\n{\r\n" + originalBody + "\r\n}\r\n";
    const auto bytes = std::as_bytes(std::span(wts.data(), wts.size()));
    project.mapDocument_ = std::make_unique<MapDocument>(MapDocument::FromMembers(
        std::vector<MapArchiveMember>{{"war3map.wts", {bytes.begin(), bytes.end()}}}));
    project.mapEdits_ = std::make_unique<MapData::MapDataEditSession>(*project.mapDocument_);
    const auto currentText = [&](std::string_view reference) {
        const auto body = project.mapEdits_->Snapshot().Strings()->Resolve(reference);
        return body.Succeeded() && body.value
            ? Trigger::DecodeSourceText(std::as_bytes(std::span(body.value->data(), body.value->size()))).text : std::string{};
    };
    QVERIFY(ResolveProjectName(project, "TRIGSTR_8", "string").displayName.find("红色") != std::string::npos);
    const auto original = Trigger::WtgWctCodec{}.Serialize(*project.triggerDocument_, *project.uiCatalog_);
    QVERIFY(original.Succeeded());
    Modules::TriggerModule module(project);
    auto* page = module.Widget();
    QVERIFY(module.Reveal({"triggers", "trigger", "Caller", {}}));
    const auto ecaId = appended.value->rootEcaId;
    module.OpenValuePanel(ecaId, 0, QRect(0, 0, 30, 20));
    auto* panel = module.ValuePanel();
    QVERIFY(panel);
    auto* body = panel->findChild<QPlainTextEdit*>("valuePanelStringBody");
    QVERIFY(body);
    QVERIFY(body->toPlainText().contains(QStringLiteral("|cffff0000红色|r\n第二行")));
    panel->Apply(); // 未改正文：QPlainTextEdit 换行规范化也不能产生一次地图编辑。
    QApplication::processEvents();
    QCOMPARE(project.triggerUndoStack_->count(), 0);
    QCOMPARE(project.mapEdits_->Snapshot().Strings()->Resolve("TRIGSTR_8").value, std::optional<std::string>(originalBody));

    // 直接参数对话框传回同一个解码后的正文，也不能弹共享确认或创建编辑。
    bool uneditedPrompt = false;
    QTimer::singleShot(0, page, [&] {
        for (auto* widget : QApplication::topLevelWidgets()) {
            auto* dialog = qobject_cast<QMessageBox*>(widget);
            if (dialog && dialog->objectName() == "triggerStringEditScope") {
                uneditedPrompt = true;
                dialog->reject();
            }
        }
    });
    QVERIFY(module.ApplyValueChoice(ecaId, 0, TriggerValueChoice{3, currentText("TRIGSTR_8")}));
    QApplication::processEvents();
    QVERIFY(!uneditedPrompt);
    QCOMPARE(project.triggerUndoStack_->count(), 0);
    QCOMPARE(project.mapEdits_->Snapshot().Strings()->Resolve("TRIGSTR_8").value, std::optional<std::string>(originalBody));

    bool sawPrompt = false;
    bool promptCountCorrect = false;
    const auto chooseScope = [&](const QString& buttonText) {
        QTimer::singleShot(0, page, [&, buttonText] {
            for (auto* widget : QApplication::topLevelWidgets()) {
                auto* dialog = qobject_cast<QMessageBox*>(widget);
                if (!dialog || dialog->objectName() != "triggerStringEditScope") continue;
                sawPrompt = true;
                promptCountCorrect = dialog->text().contains(QStringLiteral("2 处"));
                for (auto* button : dialog->buttons()) if (button->text() == buttonText) { button->click(); return; }
                dialog->reject();
            }
        });
    };
    chooseScope(QStringLiteral("全部一起改"));
    QVERIFY(module.ApplyValueChoice(ecaId, 0, TriggerValueChoice{3, "共同修改"}));
    QVERIFY(sawPrompt);
    QVERIFY(promptCountCorrect);
    QCOMPARE(project.triggerUndoStack_->count(), 1);
    QCOMPARE(currentText("TRIGSTR_8"), std::string("共同修改"));
    QCOMPARE(ResolveProjectName(project, "TRIGSTR_8", "string").displayName, std::string("共同修改"));
    project.triggerUndoStack_->undo();
    QCOMPARE(project.mapEdits_->Snapshot().Strings()->Resolve("TRIGSTR_8").value, std::optional<std::string>(originalBody));
    project.triggerUndoStack_->redo();
    QCOMPARE(currentText("TRIGSTR_8"), std::string("共同修改"));
    project.triggerUndoStack_->undo();

    sawPrompt = false;
    promptCountCorrect = false;
    chooseScope(QStringLiteral("只改这一处"));
    QVERIFY(module.ApplyValueChoice(ecaId, 0, TriggerValueChoice{3, "单独正文"}));
    QVERIFY(sawPrompt);
    QVERIFY(promptCountCorrect);
    QCOMPARE(project.triggerUndoStack_->count(), 1);
    const auto* eca = Trigger::TriggerDocumentEditor::FindEca(*project.triggerDocument_, ecaId);
    const auto changedReference = project.triggerDocument_->argumentNodes[eca->arguments[0]].value.text;
    QVERIFY(changedReference != "TRIGSTR_8");
    QCOMPARE(project.triggerDocument_->argumentNodes[eca->arguments[1]].value.text, std::string("\"TRIGSTR_8\""));
    QCOMPARE(currentText(changedReference), std::string("单独正文"));
    QCOMPARE(project.mapEdits_->Snapshot().Strings()->Resolve("TRIGSTR_8").value, std::optional<std::string>(originalBody));
    project.triggerUndoStack_->undo();
    const auto reverted = Trigger::WtgWctCodec{}.Serialize(*project.triggerDocument_, *project.uiCatalog_);
    QVERIFY(reverted.Succeeded());
    QCOMPARE(reverted.value->wtg, original.value->wtg);
    QCOMPARE(reverted.value->wct, original.value->wct);
    QCOMPARE(project.mapEdits_->Snapshot().Strings()->Resolve("TRIGSTR_8").value, std::optional<std::string>(originalBody));
    project.triggerUndoStack_->redo();
    QCOMPARE(currentText(changedReference), std::string("单独正文"));
}

void TriggerModuleTests::DynamicReferencesAndCurrentStringOverlayPreserveRawValues() {
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const std::filesystem::path ui = root.path().toStdWString();
    WriteUtf8(ui / "ui/config", "base\n");
    const std::string definitions = "[TriggerTypes]\ntypename=1,0,0,类型,string\nunitcode=0,1,1,单位类型,integer\nUnitAlias=0,1,1,自定义单位类型,unitcode\ntrigger=0,1,1,触发器\nstring=0,1,1,字符串\n[TriggerParams]\ntypename08_unitcode=0,typename,8,单位类型\ntypename06_trigger=0,typename,6,触发器\n";
    WriteUtf8(ui / "ui/base/define.txt", definitions);
    WriteUtf8(ui / "ui/base/action.txt",
        "[PlainAction]\ndescription=Plain\n[RunTrigger]\ndescription=运行 ${触发器}\n[[.args]]\ntype=trigger\n"
        "[DynamicReference]\ndescription=设置 [${类型}] ${对象} 的 [${类型}] ${触发器}；显示 ${文字}\n"
        "[[.args]]\ntype=typename\n[[.args]]\ntype=Null\n[[.args]]\ntype=typename\n[[.args]]\ntype=Null\n[[.args]]\ntype=string\n");
    auto catalog = Trigger::UiCatalog::Load(ui);
    QVERIFY(catalog.Succeeded());
    auto parsed = Trigger::WtgWctCodec{}.Parse(DirectoryWtg(), DirectoryWct(), *catalog.value);
    QVERIFY(parsed.Succeeded());
    QtProject::EditorProject project;
    project.triggerUndoStack_ = new QUndoStack(&project);
    project.uiCatalog_ = std::make_unique<Trigger::UiCatalog>(std::move(*catalog.value));
    project.triggerDocument_ = std::make_unique<Trigger::TriggerDocument>(std::move(*parsed.value));
    const auto triggerId = Named(project, "Caller")->id;
    auto appended = Trigger::TriggerDocumentEditor::AppendRootEca(*project.triggerDocument_, *project.uiCatalog_, triggerId,
        Trigger::UiDefinitionKind::Action, "DynamicReference");
    QVERIFY(appended.Succeeded() && appended.value->appendedArguments.size() == 5);
    const std::array<std::pair<int, std::string>, 5> values{{{0, "typename08_unitcode"}, {3, "H001"}, {0, "typename06_trigger"}, {1, "gg_trg_Init"}, {3, "TRIGSTR_8"}}};
    for (std::size_t i = 0; i < values.size(); ++i)
        QVERIFY(Trigger::TriggerDocumentEditor::SetScalarArgument(*project.triggerDocument_, appended.value->appendedArguments[i].id, values[i].first, values[i].second).Succeeded());
    const std::string wts = "STRING 7\n{\n地图单位名称\n}\nSTRING 8\n{\nTest1\n}\n";
    const auto wtsBytes = std::as_bytes(std::span(wts.data(), wts.size()));
    const std::vector<MapArchiveMember> members{{"war3map.wts", {wtsBytes.begin(), wtsBytes.end()}}};
    project.mapDocument_ = std::make_unique<MapDocument>(MapDocument::FromMembers(members));
    project.mapEdits_ = std::make_unique<MapData::MapDataEditSession>(*project.mapDocument_);
    const std::string lni = "[H001]\n_parent=\"hfoo\"\nunam=\"TRIGSTR_7\"\n";
    auto objects = ObjectData::LniObjectCodec(ObjectData::ObjectType::Unit).Parse(std::as_bytes(std::span(lni.data(), lni.size())));
    QVERIFY(objects.Succeeded());
    project.objectDocuments_[0].reset(dynamic_cast<ObjectData::LniObjectDocument*>(objects.document.release()));
    const auto alias = ResolveProjectName(project, "H001", "UnitAlias");
    QCOMPARE(alias.state, Services::NameResolutionState::Resolved);
    QCOMPARE(alias.displayName, std::string("地图单位名称"));
    QCOMPARE(alias.rawValue, std::string("H001"));
    // Catalog 热切换须废弃已解析名字和旧 TypeSystem，恢复后原数据身份仍保持。
    auto changedDefinitions = definitions;
    changedDefinitions.replace(changedDefinitions.find("自定义单位类型,unitcode"), std::string("自定义单位类型,unitcode").size(), "自定义单位类型,integer");
    WriteUtf8(ui / "ui/base/define.txt", changedDefinitions);
    auto replacementCatalog = Trigger::UiCatalog::Load(ui);
    QVERIFY(replacementCatalog.Succeeded());
    auto originalCatalog = std::move(project.uiCatalog_);
    project.uiCatalog_ = std::make_unique<Trigger::UiCatalog>(std::move(*replacementCatalog.value));
    QCOMPARE(ResolveProjectName(project, "H001", "UnitAlias").displayName, std::string("H001"));
    project.uiCatalog_ = std::move(originalCatalog);
    WriteUtf8(ui / "ui/base/define.txt", definitions);
    QCOMPARE(ResolveProjectName(project, "H001", "UnitAlias").displayName, std::string("地图单位名称"));
    const auto before = Trigger::WtgWctCodec{}.Serialize(*project.triggerDocument_, *project.uiCatalog_);
    QVERIFY(before.Succeeded());
    const auto objectBefore = project.objectDocuments_[0]->DocumentFingerprint();
    Modules::TriggerModule module(project);
    auto* page = module.Widget();
    QVERIFY(module.Reveal({"triggers", "trigger", "Caller", {}}));
    auto* view = page->findChild<QTreeView*>("triggerLogicTree");
    auto* model = dynamic_cast<TriggerEcaModel*>(view->model());
    auto* raw = page->findChild<QCheckBox*>("triggerRawValues");
    QVERIFY(model && raw);
    const auto id = appended.value->rootEcaId;
    const auto index = model->IndexForStableId(id);
    QVERIFY(index.isValid());
    const auto text = index.data().toString();
    QVERIFY2(text.contains(QStringLiteral("地图单位名称")), qPrintable(text));
    QVERIFY(text.contains(QStringLiteral("Init")) && text.contains(QStringLiteral("Test1")));
    QVERIFY(!text.contains(QStringLiteral("H001")) && !text.contains(QStringLiteral("TRIGSTR_8")));
    const auto spans = index.data(TriggerEcaModel::ArgumentSpansRole).toList();
    QCOMPARE(spans[1].toMap()["type"].toString(), QStringLiteral("unitcode"));
    QCOMPARE(spans[3].toMap()["type"].toString(), QStringLiteral("trigger"));
    QCOMPARE(spans[4].toMap()["raw"].toString(), QStringLiteral("TRIGSTR_8"));
    QVERIFY(spans[4].toMap()["badge"].toBool());
    page->resize(1250, 680); page->show(); QApplication::processEvents();
    Snapshot(page, "n1-synthetic-resolved");
    raw->setChecked(true);
    const auto rawText = model->IndexForStableId(id).data().toString();
    QVERIFY(rawText.contains(QStringLiteral("H001")) && rawText.contains(QStringLiteral("TRIGSTR_8")) && rawText.contains(QStringLiteral("gg_trg_Init")));
    Snapshot(page, "n1-synthetic-raw");
    raw->setChecked(false);
    auto strings = *project.mapEdits_->Snapshot().Strings();
    strings.entries[0].value = "当前未保存名称";
    QVERIFY(project.mapEdits_->EditStrings(strings).Succeeded());
    module.RefreshAfterUndo();
    QVERIFY(model->IndexForStableId(id).data().toString().contains(QStringLiteral("当前未保存名称")));
    QVERIFY(project.mapEdits_->Undo());
    module.RefreshAfterUndo();
    QVERIFY(model->IndexForStableId(id).data().toString().contains(QStringLiteral("地图单位名称")));
    QVERIFY(project.mapEdits_->Redo());
    module.RefreshAfterUndo();
    QVERIFY(model->IndexForStableId(id).data().toString().contains(QStringLiteral("当前未保存名称")));
    const auto missing = ResolveProjectName(project, "TRIGSTR_99999", "string");
    QCOMPARE(missing.state, Services::NameResolutionState::MissingString);
    QCOMPARE(missing.displayName, std::string("TRIGSTR_99999"));
    QVERIFY(!missing.reason.empty());
    const auto after = Trigger::WtgWctCodec{}.Serialize(*project.triggerDocument_, *project.uiCatalog_);
    QVERIFY(after.Succeeded() && before.value->wtg == after.value->wtg && before.value->wct == after.value->wct);
    QCOMPARE(project.objectDocuments_[0]->DocumentFingerprint(), objectBefore);
    QCOMPARE(project.mapDocument_->ResolveString("TRIGSTR_7").value, std::optional<std::string>("地图单位名称"));
}

void TriggerModuleTests::ReferenceTokensMeasureExactlyWhatTheyPaint() {
    QStandardItemModel model(1, 1);
    const auto index = model.index(0, 0);
    model.setData(index, QStringLiteral("空  有名  WTS文字"));
    QPixmap avatar(16, 16); avatar.fill(Qt::green);
    const QVariantList spans{
        QVariantMap{{"begin", 1}, {"length", 0}, {"position", 0}, {"empty", true}},
        QVariantMap{{"begin", 3}, {"length", 2}, {"position", 1}, {"icon", QIcon(avatar)}, {"raw", "H001"}},
        QVariantMap{{"begin", 7}, {"length", 5}, {"position", 2}, {"badge", true}, {"reference", "TRIGSTR_8"}}};
    model.setData(index, spans, TriggerEcaModel::ArgumentSpansRole);
    TriggerSentenceDelegate delegate;
    QStyleOptionViewItem option; option.rect = QRect(0, 0, 600, 34); option.font = QApplication::font();
    const auto first = delegate.TokenByOrder(option, index, 0);
    const auto named = delegate.TokenByOrder(option, index, 1);
    const auto wts = delegate.TokenByOrder(option, index, 2);
    QVERIFY(first && named && wts);
    const QFontMetrics metrics(option.font);
    QCOMPARE(named->rect.left() - first->rect.left(), metrics.horizontalAdvance(QStringLiteral("  ")) * 2);
    QCOMPARE(wts->rect.left() - named->rect.left(), 20 + metrics.horizontalAdvance(QStringLiteral("有名")) + metrics.horizontalAdvance(QStringLiteral("  ")));
    const auto namedHit = delegate.TokenAt(option, index, named->rect.center());
    const auto wtsHit = delegate.TokenAt(option, index, wts->rect.center());
    QVERIFY(namedHit && wtsHit);
    QCOMPARE(namedHit->position, std::size_t{1});
    QCOMPARE(wtsHit->position, std::size_t{2});
    QStyledItemDelegate base;
    QVERIFY(delegate.sizeHint(option, index).width() >= base.sizeHint(option, index).width() + 36);
}

void TriggerModuleTests::MapScriptOpensReadOnlyAndSearches() {
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QVERIFY(QDir(root.path()).mkpath(QStringLiteral("map")));
    QFile script(root.filePath(QStringLiteral("map/war3map.j")));
    QVERIFY(script.open(QIODevice::WriteOnly));
    // GB18030 编码的注释（非 UTF-8）需要按回退编码显示。
    script.write("function main takes nothing returns nothing\n    // \xD6\xD0\xCE\xC4\n    call InitBlizzard()\nendfunction\n");
    script.close();

    QtProject::EditorProject project;
    project.projectRoot_ = root.path().toStdWString();
    Modules::TriggerModule module(project);
    QWidget* page = module.Widget();
    auto* button = page->findChild<QPushButton*>(QStringLiteral("triggerMapScriptButton"));
    auto* view = page->findChild<UiKit::CodeView*>(QStringLiteral("triggerMapScriptView"));
    QVERIFY(button != nullptr && view != nullptr);
    QVERIFY(view->isHidden());

    QSignalSpy loaded(view, &UiKit::CodeView::TextLoaded);
    button->click();
    QVERIFY(!view->isHidden());
    QVERIFY(loaded.wait(5000));
    QVERIFY(view->isReadOnly());
    QVERIFY(view->toPlainText().contains(QStringLiteral("中文")));
    QCOMPARE(view->blockCount(), 5);

    // 脚本模式下顶栏搜索在脚本内查找，不过滤触发器目录。
    module.SetSearchText(QStringLiteral("InitBlizzard"));
    QCOMPARE(view->textCursor().selectedText(), QStringLiteral("InitBlizzard"));

    button->click();
    QVERIFY(view->isHidden());
    QCOMPARE(view->blockCount(), 1);
}

void TriggerModuleTests::MissingMapScriptKeepsLogicView() {
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QtProject::EditorProject project;
    project.projectRoot_ = root.path().toStdWString();
    Modules::TriggerModule module(project);
    QWidget* page = module.Widget();
    auto* button = page->findChild<QPushButton*>(QStringLiteral("triggerMapScriptButton"));
    auto* view = page->findChild<UiKit::CodeView*>(QStringLiteral("triggerMapScriptView"));
    QVERIFY(button != nullptr && view != nullptr);
    button->click();
    QVERIFY(view->isHidden());
    QCOMPARE(button->text(), QStringLiteral("地图脚本"));
}

void TriggerModuleTests::DirectoryReferenceConfirmationUsesSharedSymbols() {
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const std::filesystem::path ui = root.path().toStdWString();
    WriteUtf8(ui / "ui" / "config", "base\n");
    WriteUtf8(ui / "ui" / "base" / "action.txt",
        "[PlainAction]\ntitle=\"Plain\"\ndescription=\"Plain action\"\n"
        "[RunTrigger]\ntitle=\"Run\"\ndescription=\"Run ${trigger}\"\n[[.args]]\ntype=trigger\n");
    auto catalog = Trigger::UiCatalog::Load(ui);
    QVERIFY(catalog.Succeeded());
    // 旧图中可能存在不同中文/标点名称映射为同一符号，显示不能任选其中一个。
    const std::string first = "中文 A!", second = "中文 A?";
    const auto symbol = "gg_trg_" + Trigger::TriggerDirectoryEditor::ScriptName(first);
    QCOMPARE(Trigger::TriggerDirectoryEditor::ScriptName(first), Trigger::TriggerDirectoryEditor::ScriptName(second));
    auto parsed = Trigger::WtgWctCodec{}.Parse(DirectoryWtg(first, second), DirectoryWct(), *catalog.value);
    QVERIFY(parsed.Succeeded());
    QtProject::EditorProject project;
    project.triggerUndoStack_ = new QUndoStack(&project);
    project.uiCatalog_ = std::make_unique<Trigger::UiCatalog>(std::move(*catalog.value));
    project.triggerDocument_ = std::make_unique<Trigger::TriggerDocument>(std::move(*parsed.value));
    const auto shown = ResolveProjectName(project, symbol, "trigger");
    QCOMPARE(shown.state, Services::NameResolutionState::Ambiguous);
    QCOMPARE(shown.candidates.size(), std::size_t{2});
    QCOMPARE(shown.displayName, symbol);
    Modules::TriggerModule module(project);
    (void)module.Widget();
    QStringList confirmations;
    module.SetConfirmHandler([&](const QString&, const QString& message, const QStringList& references) {
        confirmations << message + QLatin1Char('\n') + references.join(QLatin1Char('\n'));
        return false;
    });
    for (const auto& name : {first, second}) {
        QVERIFY(module.Reveal({"triggers", "trigger", QString::fromStdString(name), {}}));
        module.DeleteCurrentItem();
        QVERIFY(Named(project, name));
    }
    QCOMPARE(confirmations.size(), 2);
    for (const auto& message : confirmations) {
        QVERIFY2(message.contains(QStringLiteral("1 处")), qPrintable(message));
        QVERIFY2(message.contains(QStringLiteral("Caller")), qPrintable(message));
    }
    QCOMPARE(project.triggerUndoStack_->count(), 0);
    QCOMPARE(project.triggerDocument_->argumentNodes.front().value.text, symbol);
}

void TriggerModuleTests::DirectoryEditsUndoAndReferences() {
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const std::filesystem::path ui = root.path().toStdWString();
    WriteUtf8(ui / "ui" / "config", "base\n");
    WriteUtf8(ui / "ui" / "base" / "action.txt",
        "[PlainAction]\ntitle=\"Plain\"\ndescription=\"Plain action\"\n"
        "[RunTrigger]\ntitle=\"Run\"\ndescription=\"Run ${trigger}\"\n[[.args]]\ntype=trigger\n");
    auto catalog = Trigger::UiCatalog::Load(ui);
    QVERIFY(catalog.Succeeded());
    auto parsed = Trigger::WtgWctCodec{}.Parse(DirectoryWtg(), DirectoryWct(), *catalog.value);
    QVERIFY2(parsed.Succeeded(), parsed.diagnostics.empty() ? "" : parsed.diagnostics.front().message.c_str());

    QtProject::EditorProject project;
    project.triggerUndoStack_ = new QUndoStack(&project);
    project.uiCatalog_ = std::make_unique<Trigger::UiCatalog>(std::move(*catalog.value));
    QStringList confirms;
    bool approve = false;
    Modules::TriggerModule module(project);
    module.RestoreViewState({{"logicColumnWidths", QJsonArray{450, 96}}});
    module.SetConfirmHandler([&](const QString& title, const QString&, const QStringList& references) {
        confirms << title + QStringLiteral(": ") + references.join(QStringLiteral(" | "));
        return approve;
    });
    QWidget* page = module.Widget();
    auto* logicHeader = page->findChild<QTreeView*>("triggerLogicTree")->header();
    QCOMPARE(logicHeader->sectionSize(0), 450);
    logicHeader->resizeSection(0, 510);
    project.triggerDocument_ = std::make_unique<Trigger::TriggerDocument>(std::move(*parsed.value));
    emit project.TriggerDocumentChanged();
    QCOMPARE(logicHeader->sectionSize(0), 510);
    page->resize(1200, 700);
    auto* view = page->findChild<QTreeView*>(QStringLiteral("triggerExplorer"));
    QVERIFY(view != nullptr);
    auto* proxy = qobject_cast<QSortFilterProxyModel*>(view->model());
    auto* model = dynamic_cast<TriggerExplorerModel*>(proxy->sourceModel());
    QVERIFY(model != nullptr);
    QUndoStack* stack = project.triggerUndoStack_;
    const auto stableTarget = QString::number(project.triggerDocument_->triggers[1].id);
    project.triggerDocument_->triggers.front().name.text = stableTarget.toStdString();
    emit project.TriggerDocumentChanged();
    QVERIFY(module.Reveal({"triggers", "trigger", stableTarget, {{"stableIdentity", true}}}));
    QCOMPARE(module.SaveViewState().value("trigger").toString(), stableTarget);
    QVERIFY(!module.Reveal({"triggers", "trigger", stableTarget, {{"stableIdentity", QStringLiteral("true")}}}));
    project.triggerDocument_->triggers.front().name.text = "Init";
    emit project.TriggerDocumentChanged();
    const auto indexOf = [&](std::string_view name) {
        return proxy->mapFromSource(model->IndexForTriggerId(Named(project, name)->id));
    };

    // 新建触发器：原版命名，插在当前触发器之后；撤销 / 重做。
    QVERIFY(module.Reveal({"triggers", "trigger", "Init", {}}));
    module.NewTrigger();
    QCOMPARE(Names(project), (QStringList{"Init", "未命名触发器 001", "Spawn", "Caller"}));
    stack->undo();
    QCOMPARE(Names(project), (QStringList{"Init", "Spawn", "Caller"}));
    stack->redo();
    QCOMPARE(Names(project).size(), 4);

    // 就地重命名：重名被拒绝；有效名称排队提交，可撤销。
    QVERIFY(!proxy->setData(indexOf("未命名触发器 001"), QStringLiteral("Init"), Qt::EditRole));
    QVERIFY(proxy->setData(indexOf("未命名触发器 001"), QStringLiteral(" Wave "), Qt::EditRole));
    QTRY_VERIFY(Named(project, "Wave") != nullptr);
    stack->undo();
    QVERIFY(Named(project, "Wave") == nullptr && Named(project, "未命名触发器 001") != nullptr);

    // 新类别插在当前类别之后；拖放语义：触发器放到类别上 = 移到末尾，可撤销。
    module.NewCategory();
    QCOMPARE(project.triggerDocument_->categories.size(), std::size_t{3});
    QCOMPARE(project.triggerDocument_->categories[1].name.text, std::string("未命名类别 001"));
    const std::int32_t fresh = project.triggerDocument_->categories[1].categoryId;
    const TriggerDirectoryItem spawn{false, Named(project, "Spawn")->id, 1};
    const auto move = model->ResolveDrop(spawn, -1, model->IndexForCategoryId(fresh));
    QVERIFY(move && move->targetCategoryId == fresh && move->beforeTriggerId == 0);
    QVERIFY(!model->ResolveDrop(spawn, 0, QModelIndex{}).has_value()); // 触发器不能放在类别之间
    const auto categoryMove = model->ResolveDrop({true, 0, 2}, 0, QModelIndex{});
    QVERIFY(categoryMove && categoryMove->beforeCategoryId == 1);
    module.ApplyDirectoryMove(*move);
    QCOMPARE(Named(project, "Spawn")->categoryId, fresh);
    stack->undo();
    QCOMPARE(Named(project, "Spawn")->categoryId, 1);

    // Ctrl+D 创建副本并选中副本（目录键盘）；Ctrl+C / Ctrl+V 把副本粘贴到另一个类别。
    QVERIFY(module.Reveal({"triggers", "trigger", "Init", {}}));
    SendKey(view, Qt::Key_D, Qt::ControlModifier);
    QVERIFY(Named(project, "Init 副本") != nullptr);
    QCOMPARE(Names(project).indexOf("Init 副本"), Names(project).indexOf("Init") + 1);
    QCOMPARE(Named(project, "Init 副本")->ecas.size(), std::size_t{1});
    SendKey(view, Qt::Key_C, Qt::ControlModifier);
    QVERIFY(module.Reveal({"triggers", "trigger", "Caller", {}}));
    SendKey(view, Qt::Key_V, Qt::ControlModifier);
    QVERIFY(Named(project, "Init 副本 副本") != nullptr);
    QCOMPARE(Named(project, "Init 副本 副本")->categoryId, 2);

    // 删除被引用的触发器先确认并列出引用位置；取消则不删，确认后删除，可撤销。
    QVERIFY(module.Reveal({"triggers", "trigger", "Init", {}}));
    SendKey(view, Qt::Key_Delete);
    QCOMPARE(confirms.size(), 1);
    QVERIFY2(confirms.front().contains(QStringLiteral("Caller")), qPrintable(confirms.front()));
    QVERIFY(Named(project, "Init") != nullptr);
    approve = true;
    module.DeleteCurrentItem();
    QVERIFY(Named(project, "Init") == nullptr);
    stack->undo();
    QVERIFY(Named(project, "Init") != nullptr);

    // 删除非空类别：一次确认、一次撤销恢复全部触发器与原顺序。
    const QStringList before = Names(project);
    QVERIFY(module.Reveal({"triggers", "trigger", "Caller", {}}));
    view->setCurrentIndex(proxy->mapFromSource(model->IndexForCategoryId(2)));
    module.DeleteCurrentItem();
    QVERIFY(Named(project, "Caller") == nullptr && Named(project, "Init 副本 副本") == nullptr);
    QCOMPARE(project.triggerDocument_->categories.size(), std::size_t{2});
    stack->undo();
    QCOMPARE(Names(project), before);
    QCOMPARE(project.triggerDocument_->categories.size(), std::size_t{3});
    page->show();
    Snapshot(page, "trigger-directory");

    // 信息条“允许”：目录显示删除线与“禁用”，“初始打开”随之不可用。
    QVERIFY(module.Reveal({"triggers", "trigger", "Spawn", {}}));
    auto* allow = page->findChild<QCheckBox*>(QStringLiteral("triggerEnabledCheck"));
    auto* initiallyOn = page->findChild<QCheckBox*>(QStringLiteral("triggerInitiallyOnCheck"));
    QVERIFY(allow != nullptr && initiallyOn != nullptr);
    // 同一能力查询仅提示限制；旧工具链不会锁住正常编辑。
    const auto legacy = std::filesystem::path(root.filePath("legacy").toStdWString());
    for (const auto* path : {"plugin/wave/include/fixture.j", "jass/system/ht/common.j", "jass/system/ht/blizzard.j"})
        WriteUtf8(legacy / path, "// fixture\n");
    project.activeUiDataRoot_ = legacy; project.compilerDataRoot_ = legacy;
    project.ClearDiagnostics(); module.RefreshAfterUndo();
    // 不能保存的原因写入错误列表（警告），不在属性区放说明文字。
    const auto rebuildWarning = [&project] {
        const auto* model = project.diagnosticsModel_;
        for (int row = 0; row < model->rowCount(); ++row)
            if (model->index(row, 2).data().toString() == QStringLiteral("SCRIPT_REBUILD_UNAVAILABLE"))
                return model->index(row, 3).data().toString();
        return QString{};
    };
    // 框架自身现在可进入冻结输入准备；入口就绪不等于此夹具能够完成保存。
    QVERIFY(rebuildWarning().isEmpty());
    const auto independentCompiler = std::filesystem::path(root.filePath("modern-compiler").toStdWString());
    for (const auto* path : {"compiler/include/fixture.j", "compiler/jass/24/common.j", "compiler/jass/24/blizzard.j"})
        WriteUtf8(independentCompiler / path, "// fixture\n");
    // 旧UI搭配独立现代编译器仍不支持；保留提示和正常编辑不被锁住的断言。
    project.compilerDataRoot_ = independentCompiler;
    module.RefreshAfterUndo();
    QVERIFY(rebuildWarning().contains(QStringLiteral("旧版图暂时无法保存")));
    QVERIFY(allow->isEnabled() && allow->toolTip().contains(QStringLiteral("旧版图暂时无法保存")));
    allow->setChecked(false);
    QCOMPARE(Named(project, "Spawn")->enabled, 0);
    QCOMPARE(indexOf("Spawn").siblingAtColumn(1).data().toString(), QStringLiteral("禁用"));
    QVERIFY(indexOf("Spawn").data(Qt::FontRole).value<QFont>().strikeOut());
    QVERIFY(!initiallyOn->isEnabled());

    // 视图状态：折叠的类别往返。
    view->collapse(proxy->mapFromSource(model->IndexForCategoryId(1)));
    const QJsonObject state = module.SaveViewState();
    Modules::TriggerModule restored(project);
    restored.RestoreViewState(state);
    auto* restoredView = restored.Widget()->findChild<QTreeView*>(QStringLiteral("triggerExplorer"));
    auto* restoredProxy = qobject_cast<QSortFilterProxyModel*>(restoredView->model());
    auto* restoredModel = dynamic_cast<TriggerExplorerModel*>(restoredProxy->sourceModel());
    QVERIFY(!restoredView->isExpanded(restoredProxy->mapFromSource(restoredModel->IndexForCategoryId(1))));
    QVERIFY(restoredView->isExpanded(restoredProxy->mapFromSource(restoredModel->IndexForCategoryId(2))));
}


void TriggerModuleTests::CallbackActionShowsActionBranch() {
    // 函数库声明比 UI 定义多一个末尾 code 参数：树里出现可直接添加动作的“动作”分支。
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const std::filesystem::path ui = root.path().toStdWString();
    WriteUtf8(ui / "ui" / "config", "base\n");
    WriteUtf8(ui / "ui" / "base" / "action.txt",
        "[PlainAction]\ntitle=\"Plain\"\ndescription=\"Plain action\"\n"
        "[IfThenElseMultiple]\ntitle=\"If\"\ndescription=\"If / Then / Else\"\n");
    WriteUtf8(ui / "ui" / "base" / "condition.txt", "[RawCond]\ntitle=\"Cond\"\ndescription=\"Cond holds\"\n");
    WriteUtf8(ui / "ui" / "base" / "jass" / "api.j", "function PlainAction takes code callback returns nothing\nendfunction\n");
    auto catalog = Trigger::UiCatalog::Load(ui);
    QVERIFY(catalog.Succeeded());
    QVERIFY(catalog.value->CallbackActions().Find("PlainAction") != nullptr);
    auto parsed = Trigger::WtgWctCodec{}.Parse(StructureWtg(), StructureWct(), *catalog.value);
    QVERIFY(parsed.Succeeded());

    QtProject::EditorProject project;
    project.triggerUndoStack_ = new QUndoStack(&project);
    project.uiCatalog_ = std::make_unique<Trigger::UiCatalog>(std::move(*catalog.value));
    project.triggerDocument_ = std::make_unique<Trigger::TriggerDocument>(std::move(*parsed.value));
    Modules::TriggerModule module(project);
    QWidget* page = module.Widget();
    QVERIFY(module.Reveal({"triggers", "trigger", "Logic", {}}));
    auto* model = dynamic_cast<TriggerEcaModel*>(page->findChild<QTreeView*>(QStringLiteral("triggerLogicTree"))->model());
    QVERIFY(model != nullptr);
    auto& document = *project.triggerDocument_;
    const StableId plain = document.ecaNodes[document.triggers.front().ecas[0]].id;
    const auto branches = model->BranchesOf(plain);
    QCOMPARE(branches.size(), std::size_t{1});
    QCOMPARE(branches[0].label, QStringLiteral("动作"));
    QCOMPARE(branches[0].type, 2);
    QCOMPARE(model->rowCount(model->IndexForStableId(plain)), 1);
    const StableId added = module.InsertEca(branches[0], 0, "PlainAction");
    QVERIFY(added != 0);
    QCOMPARE(Trigger::TriggerDocumentEditor::FindEca(document, added)->childId, std::optional<std::int32_t>(0));
    QCOMPARE(model->index(0, 0, model->IndexForStableId(plain)).data().toString(), QStringLiteral("动作 (1)"));
    // 内建块语句的分支名不受回调识别影响。
    const StableId block = document.ecaNodes[document.triggers.front().ecas[1]].id;
    QCOMPARE(model->BranchesOf(block)[2].label, QStringLiteral("否则 - 动作"));
}

void TriggerModuleTests::StatementsMoveAndFillEmptyBranches() {
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const std::filesystem::path ui = root.path().toStdWString();
    WriteUtf8(ui / "ui" / "config", "base\n");
    WriteUtf8(ui / "ui" / "base" / "action.txt",
        "[PlainAction]\ntitle=\"Plain\"\ndescription=\"Plain action\"\n"
        "[IfThenElseMultiple]\ntitle=\"If\"\ndescription=\"If / Then / Else\"\n");
    WriteUtf8(ui / "ui" / "base" / "condition.txt", "[RawCond]\ntitle=\"Cond\"\ndescription=\"Cond holds\"\n");
    auto catalog = Trigger::UiCatalog::Load(ui);
    QVERIFY(catalog.Succeeded());
    auto parsed = Trigger::WtgWctCodec{}.Parse(StructureWtg(), StructureWct(), *catalog.value);
    QVERIFY2(parsed.Succeeded(), parsed.diagnostics.empty() ? "" : parsed.diagnostics.front().message.c_str());

    QtProject::EditorProject project;
    project.triggerUndoStack_ = new QUndoStack(&project);
    project.uiCatalog_ = std::make_unique<Trigger::UiCatalog>(std::move(*catalog.value));
    project.triggerDocument_ = std::make_unique<Trigger::TriggerDocument>(std::move(*parsed.value));
    Modules::TriggerModule module(project);
    QWidget* page = module.Widget();
    QVERIFY(module.Reveal({"triggers", "trigger", "Logic", {}}));
    auto* view = page->findChild<QTreeView*>(QStringLiteral("triggerLogicTree"));
    auto* model = dynamic_cast<TriggerEcaModel*>(view->model());
    QVERIFY(model != nullptr);
    auto& document = *project.triggerDocument_;
    const auto roots = [&document]() {
        std::vector<std::string> names;
        for (const std::uint32_t root : document.triggers.front().ecas) names.push_back(document.ecaNodes[root].name.text);
        return names;
    };
    const StableId plain = document.ecaNodes[document.triggers.front().ecas[0]].id;
    const StableId block = document.ecaNodes[document.triggers.front().ecas[1]].id;

    // 块语句显示全部分支，空的“否则”也在。
    const auto branches = model->BranchesOf(block);
    QCOMPARE(branches.size(), std::size_t{3});
    QCOMPARE(branches[0].label, QStringLiteral("如果 - 条件"));
    QCOMPARE(branches[2].label, QStringLiteral("否则 - 动作"));
    QCOMPARE(branches[0].type, 1);
    const QModelIndex blockIndex = model->IndexForStableId(block);
    QCOMPARE(model->rowCount(blockIndex), 3);
    QCOMPARE(model->index(2, 0, blockIndex).data().toString(), QStringLiteral("否则 - 动作"));

    // 往空的“否则”添加动作：一次撤销整体撤回。
    const StableId added = module.InsertEca(branches[2], 0, "PlainAction");
    QVERIFY(added != 0);
    QCOMPARE(Trigger::TriggerDocumentEditor::FindEca(document, added)->childId, std::optional<std::int32_t>(2));
    QCOMPARE(model->index(2, 0, model->IndexForStableId(block)).data().toString(), QStringLiteral("否则 - 动作 (1)"));
    project.triggerUndoStack_->undo();
    QVERIFY(Trigger::TriggerDocumentEditor::FindEca(document, added) == nullptr);
    QCOMPARE(roots(), (std::vector<std::string>{"PlainAction", "IfThenElseMultiple"}));

    // 拖放规则：条件不能进动作组，块语句不能移入自己的分支。
    const StableId condition = document.ecaNodes[document.ecaNodes[document.triggers.front().ecas[1]].children[0]].id;
    QVERIFY(!model->ResolveDrop(condition, -1, model->index(2, 0)).has_value());
    QVERIFY(!model->ResolveDrop(block, -1, model->index(1, 0, model->IndexForStableId(block))).has_value());
    const auto intoThen = model->ResolveDrop(plain, -1, model->index(1, 0, model->IndexForStableId(block)));
    QVERIFY(intoThen && intoThen->slot.childId == std::optional<std::int32_t>(1));

    // 移入“则”分支，可撤销。
    module.ApplyEcaMove(*intoThen);
    QCOMPARE(roots(), std::vector<std::string>{"IfThenElseMultiple"});
    QCOMPARE(Trigger::TriggerDocumentEditor::FindEca(document, plain)->childId, std::optional<std::int32_t>(1));
    project.triggerUndoStack_->undo();
    QCOMPARE(roots(), (std::vector<std::string>{"PlainAction", "IfThenElseMultiple"}));

    // Alt+↓：同组下移；到末尾后不再移动。
    view->setCurrentIndex(model->IndexForStableId(plain));
    module.MoveSelectedEca(1);
    QCOMPARE(roots(), (std::vector<std::string>{"IfThenElseMultiple", "PlainAction"}));
    module.MoveSelectedEca(1);
    QCOMPARE(roots(), (std::vector<std::string>{"IfThenElseMultiple", "PlainAction"}));
    QCOMPARE(project.triggerUndoStack_->index(), 1); // 到末尾的第二次下移没有入栈
}

void TriggerModuleTests::ValuePanelEditsParameters() {
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const std::filesystem::path ui = root.path().toStdWString();
    WriteUtf8(ui / "ui" / "config", "base\n");
    WriteUtf8(ui / "ui" / "base" / "define.txt",
        "[TriggerParams]\nPresetTen=0,integer,10,十\nPresetTwenty=0,integer,20,二十\n");
    WriteUtf8(ui / "ui" / "base" / "action.txt",
        "[SetValue]\ntitle=\"Set\"\ndescription=\"设置 ${单位} 的数值为 ${数值}\"\n"
        "[[.args]]\ntype=unit\n[[.args]]\ntype=integer\n");
    WriteUtf8(ui / "ui" / "base" / "call.txt",
        "[GetTriggerUnit]\ntitle=\"触发单位\"\ndescription=\"触发单位\"\nreturns=unit\n");
    auto catalog = Trigger::UiCatalog::Load(ui);
    QVERIFY(catalog.Succeeded());
    auto parsed = Trigger::WtgWctCodec{}.Parse(ValueWtg(), StructureWct(), *catalog.value);
    QVERIFY2(parsed.Succeeded(), parsed.diagnostics.empty() ? "" : parsed.diagnostics.front().message.c_str());

    QtProject::EditorProject project;
    project.triggerUndoStack_ = new QUndoStack(&project);
    project.uiCatalog_ = std::make_unique<Trigger::UiCatalog>(std::move(*catalog.value));
    project.triggerDocument_ = std::make_unique<Trigger::TriggerDocument>(std::move(*parsed.value));
    Modules::TriggerModule module(project);
    QWidget* page = module.Widget();
    page->resize(1200, 700);
    page->show();
    QVERIFY(module.Reveal({"triggers", "trigger", "Values", {}}));
    auto* view = page->findChild<QTreeView*>(QStringLiteral("triggerLogicTree"));
    auto* model = dynamic_cast<TriggerEcaModel*>(view->model());
    auto& document = *project.triggerDocument_;
    const StableId eca = document.ecaNodes[document.triggers.front().ecas.front()].id;
    const auto argument = [&document](std::size_t position) -> const Trigger::ArgumentNode& {
        return document.argumentNodes[document.ecaNodes[document.triggers.front().ecas.front()].arguments[position]];
    };

    // 语句中的参数记号：两个参数都有位置，文字覆盖“Hero”和“十”。
    const QModelIndex row = model->IndexForStableId(eca);
    const QString text = row.data().toString();
    const QVariantList spans = row.data(TriggerEcaModel::ArgumentSpansRole).toList();
    QCOMPARE(spans.size(), 2);
    QCOMPARE(text.mid(spans[0].toMap()["begin"].toInt(), spans[0].toMap()["length"].toInt()), QStringLiteral("Hero"));
    QCOMPARE(text.mid(spans[1].toMap()["begin"].toInt(), spans[1].toMap()["length"].toInt()), QStringLiteral("十"));

    QApplication::processEvents();
    QTest::mouseDClick(view->header()->viewport(), Qt::LeftButton, {}, QPoint(view->header()->sectionSize(0) - 1, 8));
    auto columnState = module.SaveViewState();
    QVERIFY(columnState.contains("logicColumnWidths"));
    page->resize(1080, 700);
    module.RestoreViewState(columnState);
    QCOMPARE(module.SaveViewState().value("logicColumnWidths"), columnState.value("logicColumnWidths"));
    columnState["logicColumnWidths"] = QJsonArray{1e20, 96};
    module.RestoreViewState(columnState);
    QVERIFY(!module.SaveViewState().contains("logicColumnWidths"));
    view->setCurrentIndex(model->IndexForStableId(eca));
    view->setFocus();
    QTest::keyClick(view, Qt::Key_Return);
    QTRY_VERIFY(module.ValuePanel() != nullptr);
    QCOMPARE(module.ValuePanel()->ExpectedType(), std::string("unit"));
    module.ValuePanel()->close();
    QCOMPARE(project.triggerUndoStack_->index(), 0);
    Snapshot(page, "trigger-columns-auto");

    // 面板：整数参数默认在“预设”页，列出同类型预设，Enter 使用选中项。
    module.OpenValuePanel(eca, 1, QRect(page->mapToGlobal(QPoint(40, 40)), QSize(30, 16)));
    TriggerValuePanel* panel = module.ValuePanel();
    QVERIFY(panel != nullptr);
    QCOMPARE(panel->ExpectedType(), std::string("integer"));
    auto* tabs = panel->findChild<QTabBar*>(QStringLiteral("valuePanelTabs"));
    auto* list = panel->findChild<QListWidget*>(QStringLiteral("valuePanelList"));
    auto* search = panel->findChild<QLineEdit*>(QStringLiteral("valuePanelSearch"));
    QCOMPARE(tabs->tabText(tabs->currentIndex()), QStringLiteral("预设"));
    QCOMPARE(list->count(), 2);
    Snapshot(panel, "trigger-value-panel");
    list->setCurrentRow(1);
    panel->Apply();
    QTRY_COMPARE(argument(1).value.text, std::string("PresetTwenty"));
    QCOMPARE(argument(1).type, 0);

    // “值”页输入非法整数不给出可用项；合法整数写成直接值。
    module.OpenValuePanel(eca, 1, QRect(page->mapToGlobal(QPoint(40, 40)), QSize(30, 16)));
    panel = module.ValuePanel();
    tabs = panel->findChild<QTabBar*>(QStringLiteral("valuePanelTabs"));
    list = panel->findChild<QListWidget*>(QStringLiteral("valuePanelList"));
    search = panel->findChild<QLineEdit*>(QStringLiteral("valuePanelSearch"));
    tabs->setCurrentIndex(3);
    search->setText(QStringLiteral("1.5"));
    QCOMPARE(list->count(), 0);
    search->setText(QStringLiteral("42"));
    QCOMPARE(list->count(), 1);
    panel->Apply();
    QTRY_COMPARE(argument(1).value.text, std::string("42"));
    QCOMPARE(argument(1).type, 3);

    // 蓝图节点内编辑同一整数：非法文本不改变原值，合法文本写成直接值。
    module.SetBlueprintVisible(true);
    auto* blueprint = module.Blueprint();
    QVERIFY(blueprint != nullptr && blueprint->isVisible());
    const QPoint scalarPoint = blueprint->ArgumentViewPos(eca, 1);
    QVERIFY(scalarPoint.x() >= 0);
    QTest::mouseClick(blueprint->viewport(), Qt::LeftButton, {}, scalarPoint);
    auto* scalar = blueprint->viewport()->findChild<QLineEdit*>(QStringLiteral("blueprintScalarEditor"));
    QVERIFY(scalar != nullptr);
    scalar->setText(QStringLiteral("abc"));
    QTest::keyClick(scalar, Qt::Key_Return);
    QCoreApplication::processEvents();
    QCOMPARE(argument(1).value.text, std::string("42"));
    scalar = blueprint->viewport()->findChild<QLineEdit*>(QStringLiteral("blueprintScalarEditor"));
    QVERIFY(scalar != nullptr);
    scalar->setText(QStringLiteral("64"));
    QTest::keyClick(scalar, Qt::Key_Return);
    QCoreApplication::processEvents();
    QTRY_COMPARE(argument(1).value.text, std::string("64"));

    // 单位参数的“变量”页只列 unit 变量；输入新名称可“新建并使用”，变量与绑定一次撤销。
    module.OpenValuePanel(eca, 0, QRect(page->mapToGlobal(QPoint(40, 40)), QSize(30, 16)));
    panel = module.ValuePanel();
    tabs = panel->findChild<QTabBar*>(QStringLiteral("valuePanelTabs"));
    list = panel->findChild<QListWidget*>(QStringLiteral("valuePanelList"));
    search = panel->findChild<QLineEdit*>(QStringLiteral("valuePanelSearch"));
    auto* create = panel->findChild<QPushButton*>(QStringLiteral("valuePanelCreate"));
    QCOMPARE(tabs->tabText(tabs->currentIndex()), QStringLiteral("变量"));
    QCOMPARE(list->count(), 1); // Hero；Count 是 integer
    search->setText(QStringLiteral("Boss"));
    QCOMPARE(list->count(), 0);
    QVERIFY(create->isVisibleTo(panel));
    const std::size_t before = document.variables.size();
    const int undoIndex = project.triggerUndoStack_->index();
    create->click();
    QTRY_COMPARE(argument(0).value.text, std::string("Boss"));
    QCOMPARE(document.variables.size(), before + 1);
    QCOMPARE(document.variables.back().typeName.text, std::string("unit"));
    QCOMPARE(project.triggerUndoStack_->index(), undoIndex + 1);
    project.triggerUndoStack_->undo();
    QCOMPARE(document.variables.size(), before);
    QCOMPARE(argument(0).value.text, std::string("Hero"));

    // “函数”页列出返回 unit 的函数，选择后参数由函数提供。
    module.OpenValuePanel(eca, 0, QRect(page->mapToGlobal(QPoint(40, 40)), QSize(30, 16)));
    panel = module.ValuePanel();
    tabs = panel->findChild<QTabBar*>(QStringLiteral("valuePanelTabs"));
    list = panel->findChild<QListWidget*>(QStringLiteral("valuePanelList"));
    tabs->setCurrentIndex(1);
    QCOMPARE(list->count(), 1);
    panel->Apply();
    QTRY_VERIFY(argument(0).callEca.has_value());
    QVERIFY(model->IndexForStableId(eca).data().toString().contains(QStringLiteral("触发单位")));
}

void TriggerModuleTests::BlueprintLaysOutAndSyncs() {
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const std::filesystem::path ui = root.path().toStdWString();
    WriteUtf8(ui / "ui" / "config", "base\n");
    WriteUtf8(ui / "ui" / "base" / "action.txt",
        "[PlainAction]\ntitle=\"Plain\"\ndescription=\"Plain action\"\n"
        "[IfThenElseMultiple]\ntitle=\"If\"\ndescription=\"If / Then / Else\"\n");
    WriteUtf8(ui / "ui" / "base" / "condition.txt", "[RawCond]\ntitle=\"Cond\"\ndescription=\"Cond holds\"\n");
    auto catalog = Trigger::UiCatalog::Load(ui);
    QVERIFY(catalog.Succeeded());
    auto parsed = Trigger::WtgWctCodec{}.Parse(StructureWtg(), StructureWct(), *catalog.value);
    QVERIFY(parsed.Succeeded());
    QtProject::EditorProject project;
    project.triggerUndoStack_ = new QUndoStack(&project);
    project.uiCatalog_ = std::make_unique<Trigger::UiCatalog>(std::move(*catalog.value));
    project.triggerDocument_ = std::make_unique<Trigger::TriggerDocument>(std::move(*parsed.value));
    Modules::TriggerModule module(project);
    QWidget* page = module.Widget();
    page->resize(1200, 700);
    page->show();
    QVERIFY(module.Reveal({"triggers", "trigger", "Logic", {}}));
    auto& document = *project.triggerDocument_;
    const auto& roots = document.triggers.front().ecas;
    const StableId plain = document.ecaNodes[roots[0]].id;
    const StableId block = document.ecaNodes[roots[1]].id;
    const StableId cond = document.ecaNodes[document.ecaNodes[roots[1]].children[0]].id;
    const StableId then = document.ecaNodes[document.ecaNodes[roots[1]].children[1]].id;

    // 切到蓝图：开始节点 → 动作链纵向；If 的条件 / 则 / 否则是节点右侧的输出端口，分支语句向右展开。
    module.SetBlueprintVisible(true);
    TriggerBlueprint* blueprint = module.Blueprint();
    QVERIFY(blueprint != nullptr && blueprint->isVisible());
    QVERIFY(!page->findChild<QTreeView*>(QStringLiteral("triggerLogicTree"))->isVisible());
    QCOMPARE(blueprint->NodeCount(), std::size_t{4});
    QCOMPARE(blueprint->WireCount(), std::size_t{4}); // 开始→A、A→If、If.条件→Cond、If.则→Then
    QVERIFY(blueprint->NodeRect(block).top() > blueprint->NodeRect(plain).bottom());
    QVERIFY(blueprint->NodeRect(then).left() > blueprint->NodeRect(block).right());
    QVERIFY(blueprint->NodeText(cond).contains(QStringLiteral("Cond holds")));
    const QStringList blockPins = blueprint->PinLabels(block);
    for (const char* label : {"输入", "下一步", "如果 - 条件", "则 - 动作", "否则 - 动作"}) {
        QVERIFY2(blockPins.contains(QString::fromUtf8(label)), qPrintable(blockPins.join('|')));
    }
    QCOMPARE(blueprint->PinLabels(0), (QStringList{QStringLiteral("事件"), QStringLiteral("触发条件"), QStringLiteral("动作")}));

    // 拉线：从 A 的输入端口拉到 If 的“否则”端口 → A 移入空的否则分支；一次撤销还原。
    QWidget* canvas = blueprint->viewport();
    const auto drag = [&](const QPoint& from, const QPoint& to) {
        QVERIFY(from.x() >= 0 && to.x() >= 0);
        QTest::mousePress(canvas, Qt::LeftButton, {}, from);
        QTest::mouseMove(canvas, (from + to) / 2);
        QTest::mouseMove(canvas, to);
        QTest::mouseRelease(canvas, Qt::LeftButton, {}, to);
    };
    const auto parentOf = [&](StableId id) { return Trigger::TriggerDocumentEditor::LocateEcaPlacement(document, id).parentEcaId; };
    const int undoBase = project.triggerUndoStack_->index();
    drag(blueprint->PinViewPos(plain, QStringLiteral("输入")), blueprint->PinViewPos(block, QStringLiteral("否则 - 动作")));
    QCOMPARE(parentOf(plain), block);
    QCOMPARE(Trigger::TriggerDocumentEditor::FindEca(document, plain)->childId.value_or(-1), 2);
    QCOMPARE(project.triggerUndoStack_->index(), undoBase + 1);
    QVERIFY(blueprint->PinLabels(0).size() == 3 && blueprint->NodeRect(plain).left() > blueprint->NodeRect(block).right());
    project.triggerUndoStack_->undo();
    QCOMPARE(parentOf(plain), StableId{0});

    // 反方向：从“则”端口拉到 A → A 插到则分支最前；条件语句不能接到动作分支（类型不符，不改文档）。
    drag(blueprint->PinViewPos(block, QStringLiteral("则 - 动作")), blueprint->mapFromScene(blueprint->NodeRect(plain).center()));
    QCOMPARE(parentOf(plain), block);
    QCOMPARE(document.ecaNodes[document.ecaNodes[roots.front()].children[1]].id, plain);
    project.triggerUndoStack_->undo();
    QCOMPARE(parentOf(plain), StableId{0});
    const int beforeMismatch = project.triggerUndoStack_->index();
    drag(blueprint->PinViewPos(cond, QStringLiteral("输入")), blueprint->PinViewPos(block, QStringLiteral("则 - 动作")));
    QCOMPARE(parentOf(cond), block);
    QCOMPARE(Trigger::TriggerDocumentEditor::FindEca(document, cond)->childId.value_or(-1), 0);
    QCOMPARE(project.triggerUndoStack_->index(), beforeMismatch);

    // 拉到空白处 = 在端口位置新建语句：打开语句选择器（这里直接取消）。
    QString pickerTitle;
    QTimer::singleShot(100, [&pickerTitle]() {
        if (auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget())) {
            pickerTitle = dialog->windowTitle();
            dialog->reject();
        }
    });
    const QPoint thenNext = blueprint->PinViewPos(then, QStringLiteral("下一步"));
    drag(thenNext, thenNext + QPoint(0, 160));
    QVERIFY2(!pickerTitle.isEmpty(), "拉到空白处应打开语句选择器");
    QCOMPARE(blueprint->NodeCount(), std::size_t{4});

    // 右键 If 节点：直接列出“添加到则 / 否则 / 条件”和语句命令；空白处可添加事件 / 条件 / 动作。
    const auto rightClick = [&](const QPoint& at) {
        QTimer::singleShot(100, []() {
            if (QWidget* popup = QApplication::activePopupWidget()) popup->close();
        });
        QContextMenuEvent event(QContextMenuEvent::Mouse, at, canvas->mapToGlobal(at));
        QApplication::sendEvent(canvas, &event);
        return module.LastBlueprintMenu();
    };
    const QRectF blockRect = blueprint->NodeRect(block);
    const QStringList blockMenu = rightClick(blueprint->mapFromScene(blockRect.topLeft() + QPointF(40, 8)));
    for (const char* entry : {"添加动作到“否则 - 动作”…", "添加条件到“如果 - 条件”…", "在之后添加动作…", "删除"}) {
        QVERIFY2(blockMenu.join('|').contains(QString::fromUtf8(entry)), qPrintable(blockMenu.join('|')));
    }
    const QStringList emptyMenu = rightClick(blueprint->mapFromScene(blockRect.bottomLeft() + QPointF(-200, 200)));
    QVERIFY2(emptyMenu.join('|').contains(QStringLiteral("添加事件到“事件”…")), qPrintable(emptyMenu.join('|')));

    // 选中同步：语句树的当前项在蓝图中选中。
    auto* view = page->findChild<QTreeView*>(QStringLiteral("triggerLogicTree"));
    auto* model = dynamic_cast<TriggerEcaModel*>(view->model());
    view->setCurrentIndex(model->IndexForStableId(then));
    QCOMPARE(blueprint->SelectedEca(), then);

    // 编辑后蓝图重建：往“否则”添加语句后多一个节点。
    QVERIFY(module.InsertEca(model->BranchesOf(block)[2], 0, "PlainAction") != 0);
    QCOMPARE(blueprint->NodeCount(), std::size_t{5});
    Snapshot(page, "trigger-blueprint");

    // 视图状态记住蓝图。
    QCOMPARE(module.SaveViewState()["view"].toString(), QStringLiteral("blueprint"));
    module.SetBlueprintVisible(false);
    QVERIFY(view->isVisible() && !blueprint->isVisible());
    view->expandAll();
    Snapshot(page, "trigger-traditional");
}

void TriggerModuleTests::CommentEntriesAndConditionNode() {
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const std::filesystem::path ui = root.path().toStdWString();
    WriteUtf8(ui / "ui" / "config", "base\n");
    WriteUtf8(ui / "ui" / "base" / "event.txt", "[RawEvent]\ntitle=\"Event\"\ndescription=\"Map starts\"\n");
    WriteUtf8(ui / "ui" / "base" / "condition.txt", "[RawCond]\ntitle=\"Cond\"\ndescription=\"Cond holds\"\n");
    WriteUtf8(ui / "ui" / "base" / "action.txt", "[PlainAction]\ntitle=\"Plain\"\ndescription=\"Plain action\"\n");
    auto catalog = Trigger::UiCatalog::Load(ui);
    QVERIFY(catalog.Succeeded());
    auto parsed = Trigger::WtgWctCodec{}.Parse(CommentWtg(), CommentWct(), *catalog.value);
    QVERIFY2(parsed.Succeeded(), parsed.diagnostics.empty() ? "" : parsed.diagnostics.front().message.c_str());

    QtProject::EditorProject project;
    project.triggerUndoStack_ = new QUndoStack(&project);
    project.uiCatalog_ = std::make_unique<Trigger::UiCatalog>(std::move(*catalog.value));
    project.triggerDocument_ = std::make_unique<Trigger::TriggerDocument>(std::move(*parsed.value));
    Modules::TriggerModule module(project);
    QWidget* page = module.Widget();
    page->resize(1200, 700);
    page->show();
    auto* explorer = page->findChild<QTreeView*>(QStringLiteral("triggerExplorer"));
    auto* proxy = qobject_cast<QSortFilterProxyModel*>(explorer->model());
    auto* model = dynamic_cast<TriggerExplorerModel*>(proxy->sourceModel());
    auto* logic = page->findChild<QTreeView*>(QStringLiteral("triggerLogicTree"));
    auto* allow = page->findChild<QCheckBox*>(QStringLiteral("triggerEnabledCheck"));
    auto* comment = page->findChild<QWidget*>(QStringLiteral("triggerCommentEdit"));
    auto* addAction = page->findChild<QPushButton*>(QStringLiteral("primaryButton"));
    QVERIFY(logic != nullptr && allow != nullptr && comment != nullptr && addAction != nullptr);
    const auto flagOf = [&](std::string_view name) {
        return proxy->mapFromSource(model->IndexForTriggerId(Named(project, name)->id)).siblingAtColumn(1).data().toString();
    };

    // 注释条目与触发器分开显示：目录标“注释”，只显示注释编辑框，不显示语句与允许开关。
    QCOMPARE(flagOf("Readme"), QStringLiteral("注释"));
    QVERIFY(flagOf("Gate") != QStringLiteral("注释"));
    QVERIFY(module.Reveal({"triggers", "trigger", "Readme", {}}));
    QVERIFY(!logic->isVisible());
    QVERIFY(comment->isVisible());
    QVERIFY(!allow->isVisible());
    QVERIFY(!addAction->isEnabled());
    module.SetBlueprintVisible(true);
    QVERIFY(!module.Blueprint()->isVisible());
    Snapshot(page, "trigger-comment-entry");
    module.SetBlueprintVisible(false);

    // 普通触发器恢复语句区与开关。
    QVERIFY(module.Reveal({"triggers", "trigger", "Gate", {}}));
    QVERIFY(logic->isVisible() && allow->isVisible() && addAction->isEnabled());

    // 新建注释：isComment = 1，原版命名，可撤销。
    module.NewComment();
    const auto* created = Named(project, "未命名注释 001");
    QVERIFY(created != nullptr);
    QCOMPARE(created->type, 1);
    QCOMPARE(flagOf("未命名注释 001"), QStringLiteral("注释"));
    project.triggerUndoStack_->undo();
    QVERIFY(Named(project, "未命名注释 001") == nullptr);

    // 蓝图：触发条件是事件与动作之间的独立节点（事件 → 触发条件 → 动作），不再是浮在角落的标记。
    QVERIFY(module.Reveal({"triggers", "trigger", "Gate", {}}));
    module.SetBlueprintVisible(true);
    TriggerBlueprint* blueprint = module.Blueprint();
    QVERIFY(blueprint->isVisible());
    QVERIFY(blueprint->ConditionText().contains(QStringLiteral("Cond holds")));
    QCOMPARE(blueprint->NodeCount(), std::size_t{2}); // 事件与动作；触发条件节点单独计
    QCOMPARE(blueprint->WireCount(), std::size_t{2});
    const auto& document = *project.triggerDocument_;
    const auto& roots = Named(project, "Gate")->ecas;
    const StableId event = document.ecaNodes[roots[0]].id;
    const StableId action = document.ecaNodes[roots[2]].id;
    QVERIFY(blueprint->NodeRect(action).left() > blueprint->NodeRect(event).right());
    Snapshot(page, "trigger-blueprint-conditions");
}

void TriggerModuleTests::UneditedTriggerCommentPreservesOriginalNewlines() {
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const std::filesystem::path ui = root.path().toStdWString();
    WriteUtf8(ui / "ui" / "config", "base\n");
    WriteUtf8(ui / "ui" / "base" / "action.txt",
        "[PlainAction]\ntitle=\"Plain\"\ndescription=\"Plain action\"\n"
        "[RunTrigger]\ntitle=\"Run\"\ndescription=\"Run ${trigger}\"\n[[.args]]\ntype=trigger\n");
    auto catalog = Trigger::UiCatalog::Load(ui);
    QVERIFY(catalog.Succeeded());
    Trigger::WtgWctCodec codec;
    auto parsed = codec.Parse(DirectoryWtg(), DirectoryWct(), *catalog.value);
    QVERIFY(parsed.Succeeded());
    const std::string originalComment = "第一行\r\n第二行\r\n";
    auto& initial = parsed.value->triggers.front();
    initial.enabled = 0;
    initial.initiallyClosed = 1;
    initial.description.text = originalComment;
    const auto original = codec.Serialize(*parsed.value, *catalog.value);
    QVERIFY(original.Succeeded());
    // 从实际 WTG/WCT 字节重读，使测试同时覆盖原始字节保留。
    parsed = codec.Parse(original.value->wtg, original.value->wct, *catalog.value);
    QVERIFY(parsed.Succeeded());

    QtProject::EditorProject project;
    project.triggerUndoStack_ = new QUndoStack(&project);
    project.uiCatalog_ = std::make_unique<Trigger::UiCatalog>(std::move(*catalog.value));
    project.triggerDocument_ = std::make_unique<Trigger::TriggerDocument>(std::move(*parsed.value));
    const std::string originalFingerprint = project.ProjectFingerprint();
    Modules::TriggerModule module(project);
    QWidget* page = module.Widget();
    auto* comment = module.CommentEditor();
    auto* allow = page->findChild<QCheckBox*>(QStringLiteral("triggerEnabledCheck"));
    QVERIFY(comment != nullptr && allow != nullptr);
    QUndoStack* stack = project.triggerUndoStack_;
    const auto unchangedBytes = [&]() {
        const auto encoded = codec.Serialize(*project.triggerDocument_, *project.uiCatalog_);
        return encoded.Succeeded() && encoded.value->wtg == original.value->wtg &&
            encoded.value->wct == original.value->wct;
    };

    QCOMPARE(comment->toPlainText(), QStringLiteral("第一行\n第二行\n"));
    // 只读搜索、切换选中项、隐藏模块后，关闭流程会再次提交当前属性。
    module.SetSearchText(QStringLiteral("Init"));
    module.SetSearchText({});
    QVERIFY(module.Reveal({"triggers", "trigger", "Spawn", {}}));
    QVERIFY(module.Reveal({"triggers", "trigger", "Init", {}}));
    page->hide();
    module.CommitPendingEdits();
    module.CommitPendingEdits();
    QCOMPARE(stack->count(), 0);
    QCOMPARE(project.ProjectFingerprint(), originalFingerprint);
    QCOMPARE(Named(project, "Init")->description.text, originalComment);
    QVERIFY(unchangedBytes());

    // 只改开关仍须保留注释中的 CRLF，撤销后恢复完整原始地图字节。
    allow->setChecked(true);
    QCOMPARE(stack->count(), 1);
    QCOMPARE(Named(project, "Init")->enabled, 1);
    QCOMPARE(Named(project, "Init")->description.text, originalComment);
    stack->undo();
    module.CommitPendingEdits();
    QCOMPARE(stack->index(), 0);
    QVERIFY(unchangedBytes());
    stack->clear();

    // 真正编辑注释仍入栈；保存前重复提交无额外命令，撤销可恢复 CRLF。
    comment->setPlainText(QStringLiteral("第一行\n已修改\n"));
    module.CommitPendingEdits();
    QCOMPARE(stack->count(), 1);
    QCOMPARE(Named(project, "Init")->description.text, std::string("第一行\n已修改\n"));
    QVERIFY(project.ProjectFingerprint() != originalFingerprint);
    module.CommitPendingEdits();
    QCOMPARE(stack->count(), 1);
    stack->undo();
    module.CommitPendingEdits();
    QCOMPARE(stack->index(), 0);
    QCOMPARE(project.ProjectFingerprint(), originalFingerprint);
    QVERIFY(unchangedBytes());
}

// 函数选择器：WE 式“分类 - 函数名”、标志 1 不加前缀、分类筛选、按前缀搜索、记住尺寸。原创合成定义。
void TriggerModuleTests::FunctionPickerShowsCategoryPrefixes() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const std::filesystem::path root = temporary.path().toStdWString();
    WriteUtf8(root / "ui" / "config", "base\n");
    WriteUtf8(root / "ui" / "base" / "define.txt",
        "[TriggerCategories]\n"
        "TC_UNIT=单位,ReplaceableTextures\\WorldEditUI\\Actions-Unit\n"
        "TC_LOGIC=逻辑,ReplaceableTextures\\WorldEditUI\\Actions-Logical,1\n");
    WriteUtf8(root / "ui" / "base" / "action.txt",
        "[CreateThing]\ntitle=\"创建物件\"\ndescription=\"创建物件\"\ncategory=TC_UNIT\n"
        "[KillThing]\ntitle=\"杀死物件\"\ndescription=\"杀死物件\"\ncategory = \"TC_UNIT\"\n"
        "[IfThing]\ntitle=\"如果\"\ndescription=\"如果\"\ncategory=TC_LOGIC\n"
        "[LooseThing]\ntitle=\"无分类\"\ndescription=\"无分类\"\n");
    auto catalog = Trigger::UiCatalog::Load(root);
    QVERIFY(catalog.Succeeded() && catalog.value);
    // 尺寸记在本机设置：测试进程用独立的组织名，不碰编辑器自己的设置。
    const QString organization = QCoreApplication::organizationName();
    QCoreApplication::setOrganizationName(QStringLiteral("War3MapEditor64-Tests"));
    QSettings().remove(QStringLiteral("triggers/functionPicker"));

    QStringList iconRequests;
    const auto icons = [&iconRequests](const QString& id) {
        iconRequests << id;
        QPixmap pixmap(4, 4);
        pixmap.fill(Qt::red);
        return QIcon(pixmap);
    };
    QSize remembered;
    {
        UiDefinitionPickerDialog picker(*catalog.value, Trigger::UiDefinitionKind::Action, {}, icons);
        auto* list = picker.findChild<QTreeView*>("definitionList");
        auto* category = picker.findChild<QComboBox*>("definitionCategory");
        auto* search = picker.findChild<QLineEdit*>("dialogSearch");
        QVERIFY(list && category && search);
        QVERIFY(picker.width() >= 1000 && picker.isSizeGripEnabled());
        const auto* model = list->model();
        QStringList titles;
        for (int row = 0; row < model->rowCount(); ++row) titles << model->index(row, 0).data().toString();
        QVERIFY2(titles.contains(QStringLiteral("单位 - 创建物件")), qPrintable(titles.join('|')));
        QVERIFY(titles.contains(QStringLiteral("单位 - 杀死物件")));
        QVERIFY(titles.contains(QStringLiteral("如果")));
        QVERIFY(titles.contains(QStringLiteral("无分类")));
        QVERIFY(!model->index(titles.indexOf(QStringLiteral("单位 - 创建物件")), 0).data(Qt::DecorationRole).value<QIcon>().isNull());
        QVERIFY(iconRequests.contains(QStringLiteral("TC_UNIT")));
        // 分类筛选：全部 + 实际出现的三类（含未分类）。
        QCOMPARE(category->count(), 4);
        category->setCurrentIndex(category->findData(QStringLiteral("TC_UNIT")));
        QCOMPARE(model->rowCount(), 2);
        category->setCurrentIndex(0);
        search->setText(QStringLiteral("单位 - 杀"));
        QCOMPARE(model->rowCount(), 1);
        QCOMPARE(picker.SelectedDefinition()->name, std::string("KillThing"));
        picker.resize(700, 480); // 小于 offscreen 屏幕，恢复时不会被收进屏幕
        remembered = picker.size();
        picker.reject();
        qWarning() << "saved" << QSettings().value("triggers/functionPicker/size") << QSettings().status() << QSettings().fileName();
    }
    {
        UiDefinitionPickerDialog picker(*catalog.value, Trigger::UiDefinitionKind::Action, {});
        QCOMPARE(picker.size(), remembered);
    }
    QSettings().remove(QStringLiteral("triggers/functionPicker"));
    QCoreApplication::setOrganizationName(organization);
}

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    QApplication application(argc, argv);
    UiKit::ApplyApplicationFont(UiKit::LoadFonts(UiKit::DefaultFontDirectory()));
    UiKit::ThemeManager theme;
    theme.ApplyTheme(QStringLiteral("graphite"));
    TriggerModuleTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "TriggerModuleTests.moc"
