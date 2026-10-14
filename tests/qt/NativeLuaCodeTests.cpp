#include "EditorProject.h"
#include "TriggerModule.h"
#include "Widgets/CodeView.h"
#include "Widgets/CodeAssistPopup.h"
#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/Script/LuaAuthorCode.h"
#include "War3MapCore/Trigger/TriggerCodec.h"
#include "War3MapCore/Workspace.h"

#include <QApplication>
#include <QComboBox>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTest>
#include <QUndoStack>

#include <fstream>
#include <algorithm>
#include <stdexcept>

using namespace War3Map;
namespace {
constexpr std::string_view kHeader = "-- 原生 Lua 头部\nNativeCounter = 41\nNativeProbe = CreateTrigger()\n";

template<class T> T Take(Result<T> result) {
    if (!result.Succeeded() || !result.value) {
        throw std::runtime_error(result.diagnostics.empty() ? "fixture operation failed" : result.diagnostics.front().message);
    }
    return std::move(*result.value);
}
void Write(const std::filesystem::path& path, std::span<const std::byte> bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!file) throw std::runtime_error("original synthetic fixture write failed");
}
void Write(const std::filesystem::path& path, std::string_view text) { Write(path, std::as_bytes(std::span(text.data(), text.size()))); }
std::vector<std::byte> Settings(int language, std::string name = "Original native Lua UI fixture") {
    MapData::MapSettings value;
    value.formatVersion = 31; value.scriptLanguage = language; value.custom.emplace(); value.tileset = 'L';
    value.gameVersion = {1, 32, 0, 0}; value.graphicsModes = 3; value.gameDataVersion = 1; value.mapName = std::move(name);
    return Take(value.Serialize());
}
void Install(QtProject::EditorProject& project, const std::filesystem::path& root, bool archive, int language = 1, bool largeCode = false) {
    Write(root / "catalog/ui/config", "base\n");
    Write(root / "catalog/ui/base/define.txt", "[TriggerTypes]\ninteger=0,1,1,整数,integer\n");
    for (const auto* name : {"event.txt", "condition.txt", "action.txt", "call.txt"}) Write(root / "catalog/ui/base" / name, "");
    auto catalog = Take(Trigger::UiCatalog::Load(root / "catalog"));
    auto document = Take(Trigger::WtgWctCodec{}.CreateEmpty(catalog));
    const auto settings = Settings(language);
    auto header = language == 1
        ? Script::LuaAuthorCodeEditor::PrepareText(settings, document, catalog, {Trigger::CodeBlockKind::GlobalHeader, 0}, kHeader)
        : Trigger::TriggerCodeEditor::PrepareText(document, catalog, {Trigger::CodeBlockKind::GlobalHeader, 0}, "// Original JASS header\n");
    (void)Take(Trigger::TriggerCodeEditor::Apply(document, catalog, Take(std::move(header))));
    auto plan = language == 1 ? Script::LuaAuthorCodeEditor::PrepareNewCustom(settings, document, catalog, -1, "OriginalLua", {},
        Trigger::TriggerCodeEditor::kDefaultMaxEditedBytes, "Original")
        : Trigger::TriggerCodeEditor::PrepareNewCustom(document, catalog, -1, "OriginalJass", {},
            Trigger::TriggerCodeEditor::kDefaultMaxEditedBytes, "Original");
    (void)Take(Trigger::TriggerCodeEditor::Apply(document, catalog, Take(std::move(plan))));
    if (largeCode) {
        // 原创大块来源验证异步展示；不扩大作者编辑或热重载预算。
        document.triggers.front().customText = Trigger::RawText{
            "--" + std::string(2 * 1024 * 1024 + 128, 'x')
                + "\nfunction InitTrig_OriginalLua()\n    NativeCounter = 42\nend\n"};
    }
    const auto pair = Take(Trigger::WtgWctCodec{}.Serialize(document, catalog));
    document = Take(Trigger::WtgWctCodec{}.Parse(pair.wtg, pair.wct, catalog));
    const std::vector<MapArchiveMember> members{{"war3map.w3i", settings}, {"war3map.wtg", pair.wtg}, {"war3map.wct", pair.wct}};
    if (archive) {
        project.sourceArchivePath_ = root / "original.w3x";
        (void)Take(WriteVerifiedArchive(project.sourceArchivePath_, members));
        project.archiveSession_ = std::make_unique<MapArchiveSession>(Take(MapArchiveSession::Open(project.sourceArchivePath_)));
    } else {
        project.projectRoot_ = root / "original-directory";
        for (const auto& member : members) Write(project.projectRoot_ / "map" / member.name, member.data);
        project.directorySnapshot_ = std::make_unique<DirectoryWorkspace>(Take(DirectoryWorkspace::Open(project.projectRoot_)));
    }
    project.uiCatalog_ = std::make_unique<Trigger::UiCatalog>(std::move(catalog));
    project.triggerDocument_ = std::make_unique<Trigger::TriggerDocument>(std::move(document));
    project.triggerUndoStack_ = new QUndoStack(&project);
    project.LoadMapDocument();
    QObject::connect(project.triggerUndoStack_, &QUndoStack::indexChanged, &project,
        [&project] { project.ObserveTriggerEdit(); });
    project.ResetEditTracking();
    if (!project.editTrackingReady_ || project.editTrackingFailed_) throw std::runtime_error("original fixture edit tracking failed");
}
UiKit::CodeView* Code(Modules::TriggerModule& module) { return qobject_cast<UiKit::CodeView*>(module.CodeEditor()); }
Shell::NavigationTarget RuntimeLine(const QtProject::EditorProject& project, StableId triggerId, int line) {
    return {"triggers", "runtime-code", QString::number(triggerId),
        {{"generation", QString::number(project.editRevision_.Generation())},
            {"revision", QString::number(project.editRevision_.Revision())}, {"line", line}}};
}
}

// 全程 offscreen / 原创微型 fixture；语法检查编译不执行，未启动游戏或外部编译器。
class NativeLuaCodeTests final : public QObject {
    Q_OBJECT
private slots:
    void SourceArchiveAndDirectorySelectLuaAndUndo();
    void LuaCheckingDoesNotExecuteAndDraftFailuresPreserveText();
    void OriginalSourceSettingsIdentityBlocksReboundDraft();
    void EditedLanguageOrManualComboDoesNotGrantNativeAuthority();
    void RuntimeLineNavigationPreservesDraftsAndIdentity();
    void RuntimeLineNavigationWaitsForTextAndCancels();
    void RuntimeLineNavigationRejectsDisplayLineChanges();
    void RuntimeLineNavigationPreservesPropertyDrafts();
    void CodeBlockCompletionSeesOtherBlocks();
};

void NativeLuaCodeTests::SourceArchiveAndDirectorySelectLuaAndUndo() {
    for (const bool archive : {false, true}) {
        QTemporaryDir root; QVERIFY(root.isValid());
        QtProject::EditorProject project; Install(project, root.path().toStdWString(), archive);
        const auto original = Take(Trigger::WtgWctCodec{}.Serialize(*project.triggerDocument_, *project.uiCatalog_));
        Modules::TriggerModule module(project); auto* page = module.Widget();
        QVERIFY(module.ShowGlobalCustomCode());
        auto* code = Code(module); auto* language = page->findChild<QComboBox*>("triggerCodeLanguage");
        QVERIFY(code && language); QVERIFY(!code->isReadOnly()); QCOMPARE(code->Language(), UiKit::CodeLanguage::Lua);
        QCOMPARE(code->SourceName(), QStringLiteral("WCT/header.lua")); QCOMPARE(language->currentIndex(), 2); QVERIFY(!language->isEnabled());
        language->setCurrentIndex(0); QCOMPARE(language->currentIndex(), 2); QCOMPARE(code->Language(), UiKit::CodeLanguage::Lua);
        code->appendPlainText(QStringLiteral("-- 新增中文注释")); QVERIFY(module.CommitPendingEdits());
        QCOMPARE(project.triggerUndoStack_->count(), 1);
        project.triggerUndoStack_->undo();
        auto undone = Take(Trigger::WtgWctCodec{}.Serialize(*project.triggerDocument_, *project.uiCatalog_));
        QVERIFY(undone.wtg == original.wtg && undone.wct == original.wct);
        project.triggerUndoStack_->redo(); QVERIFY(project.triggerDocument_->wct.customCode.text.find("新增中文注释") != std::string::npos);
        QVERIFY(module.CreateCustomTrigger(QStringLiteral("NewLua")));
        const auto found = std::find_if(project.triggerDocument_->triggers.begin(), project.triggerDocument_->triggers.end(),
            [](const auto& row) { return row.name.text == "NewLua"; });
        QVERIFY(found != project.triggerDocument_->triggers.end());
        QCOMPARE(found->customText.text, std::string("function InitTrig_NewLua()\n    gg_trg_NewLua = CreateTrigger()\nend\n"));
        QCOMPARE(Code(module)->SourceName(), QString::fromStdString("WCT/trigger-" + std::to_string(found->id) + ".lua"));
        QVERIFY(!module.ConvertCurrentTriggerToCustomCode());
        project.triggerUndoStack_->undo(); QCOMPARE(project.triggerDocument_->triggers.size(), std::size_t{1});
        project.triggerUndoStack_->redo(); QCOMPARE(project.triggerDocument_->triggers.size(), std::size_t{2});
    }
}

void NativeLuaCodeTests::LuaCheckingDoesNotExecuteAndDraftFailuresPreserveText() {
    QTemporaryDir root; QtProject::EditorProject project; Install(project, root.path().toStdWString(), true);
    Modules::TriggerModule module(project); auto* page = module.Widget(); QVERIFY(module.ShowGlobalCustomCode());
    auto* code = Code(module); QVERIFY(code);
    code->RequestCheck(); QTRY_VERIFY_WITH_TIMEOUT(!code->IsChecking(), 8000);
    QVERIFY(code->CheckResult().completed && code->CheckResult().syntaxChecked); QVERIFY(!code->CheckResult().semanticChecked);
    QVERIFY(std::none_of(code->CheckResult().diagnostics.begin(), code->CheckResult().diagnostics.end(),
        [](const auto& row) { return row.severity == UiKit::CodeSeverity::Error; }));
    // CreateTrigger 在检查进程中不存在；检查通过证明这段原生调用没有被执行。
    code->setPlainText(QStringLiteral("function unfinished("));
    code->RequestCheck(); QTRY_VERIFY_WITH_TIMEOUT(!code->IsChecking(), 8000);
    QVERIFY(std::any_of(code->CheckResult().diagnostics.begin(), code->CheckResult().diagnostics.end(),
        [](const auto& row) { return row.severity == UiKit::CodeSeverity::Error; }));
    QVERIFY(module.CommitPendingEdits()); // 作者可应用未完成正文；完整保存仍编译拒绝。
    project.triggerUndoStack_->undo();
    const auto baseline = code->toPlainText();
    code->setPlainText(baseline + QChar(0)); QVERIFY(!module.CommitPendingEdits());
    QVERIFY(module.HasPendingCodeDraft()); QVERIFY(code->toPlainText().endsWith(QChar(0)));
    project.sourceChangeSuspected_ = true; project.SourceStateChanged();
    QVERIFY(code->isReadOnly()); QVERIFY(!page->findChild<QPushButton*>("triggerApplyCode")->isEnabled());
    QVERIFY(!module.CommitPendingEdits()); QVERIFY(code->toPlainText().endsWith(QChar(0)));
    project.sourceChangeSuspected_ = false; project.SourceStateChanged();
    module.DiscardPendingCodeDraft(); QVERIFY(!module.HasPendingCodeDraft()); QVERIFY(!code->isReadOnly()); QCOMPARE(code->toPlainText(), baseline);
}

void NativeLuaCodeTests::OriginalSourceSettingsIdentityBlocksReboundDraft() {
    QTemporaryDir root; QtProject::EditorProject project; Install(project, root.path().toStdWString(), false);
    Modules::TriggerModule module(project); (void)module.Widget(); QVERIFY(module.ShowGlobalCustomCode());
    auto* code = Code(module); code->appendPlainText(QStringLiteral("-- 保留草稿")); const auto draft = code->toPlainText();
    Write(project.projectRoot_ / "map/war3map.w3i", Settings(1, "Another actual native Lua source"));
    project.directorySnapshot_ = std::make_unique<DirectoryWorkspace>(Take(DirectoryWorkspace::Open(project.projectRoot_)));
    QVERIFY(!module.CommitPendingEdits()); QVERIFY(module.HasPendingCodeDraft()); QCOMPARE(code->toPlainText(), draft);
    QCOMPARE(project.triggerUndoStack_->count(), 0);
}

void NativeLuaCodeTests::EditedLanguageOrManualComboDoesNotGrantNativeAuthority() {
    QTemporaryDir root; QtProject::EditorProject project; Install(project, root.path().toStdWString(), true, 0);
    Modules::TriggerModule module(project); auto* page = module.Widget(); QVERIFY(module.ShowGlobalCustomCode());
    auto* language = page->findChild<QComboBox*>("triggerCodeLanguage"); QVERIFY(language && language->isEnabled());
    language->setCurrentIndex(2); QVERIFY(module.CreateCustomTrigger(QStringLiteral("StillJass")));
    const auto& created = project.triggerDocument_->triggers.back();
    QVERIFY(created.customText.text.find("takes nothing returns nothing") != std::string::npos);
    QVERIFY(created.customText.text.find("endfunction") != std::string::npos);
    auto settings = *project.mapEdits_->Snapshot().Settings(); settings.scriptLanguage = 1;
    QVERIFY(project.mapEdits_->EditSettings(settings).Succeeded());
    const auto count = project.triggerUndoStack_->count();
    QVERIFY(!module.CreateCustomTrigger(QStringLiteral("ForbiddenNativeLua")));
    project.luaMapTarget_.emplace(); QVERIFY(!module.CreateCustomTrigger(QStringLiteral("TargetOnlyLua")));
    QCOMPARE(project.triggerUndoStack_->count(), count);
}

void NativeLuaCodeTests::RuntimeLineNavigationPreservesDraftsAndIdentity() {
    QTemporaryDir root; QtProject::EditorProject project; Install(project, root.path().toStdWString(), true);
    Modules::TriggerModule module(project); (void)module.Widget(); QVERIFY(module.ShowGlobalCustomCode());
    const auto triggerId = project.triggerDocument_->triggers.front().id;
    const auto target = RuntimeLine(project, triggerId, 2);
    QVERIFY(!project.CurrentCodeCheckIndex()); // 真实测试回执无需先建立检查索引。
    QVERIFY(module.Reveal(target));
    auto* code = Code(module); QVERIFY(code);
    QCOMPARE(code->SourceName(), QString::fromStdString("WCT/trigger-" + std::to_string(triggerId) + ".lua"));
    QCOMPARE(code->textCursor().blockNumber(), 1);
    QCOMPARE(project.triggerUndoStack_->count(), 0);
    const auto original = code->toPlainText();
    code->appendPlainText(QStringLiteral("-- 作者尚未应用的草稿"));
    const auto draft = code->toPlainText(); const auto draftCursor = code->textCursor().position();
    QVERIFY(module.HasPendingCodeDraft()); QVERIFY(!module.Reveal(target));
    QCOMPARE(code->toPlainText(), draft); QCOMPARE(code->textCursor().position(), draftCursor);
    QCOMPARE(project.triggerUndoStack_->count(), 0);
    module.DiscardPendingCodeDraft(); QCOMPARE(code->toPlainText(), original);
    for (const auto* key : {"generation", "revision"}) {
        auto stale = target;
        stale.extra[key] = QString::number(target.extra.value(key).toString().toULongLong() + 1);
        QVERIFY(!module.Reveal(stale));
    }
    for (const auto line : {0, -1, 1000}) QVERIFY(!module.Reveal(RuntimeLine(project, triggerId, line)));
    auto malformed = target; malformed.extra["line"] = QStringLiteral("2"); QVERIFY(!module.Reveal(malformed));
    malformed = target; malformed.id = QStringLiteral("00") + QString::number(triggerId); QVERIFY(!module.Reveal(malformed));
    auto& row = project.triggerDocument_->triggers.front();
    row.customTextFlag = 0;
    QVERIFY(!module.Reveal(RuntimeLine(project, triggerId, 2))); // 运行时位置不能将 GUI 转为代码。
    QCOMPARE(row.customTextFlag, 0); row.customTextFlag = 1;
    code->appendPlainText(QStringLiteral("-- 已应用的新正文")); QVERIFY(module.CommitPendingEdits());
    QVERIFY(!module.Reveal(target));
    const auto fresh = RuntimeLine(project, triggerId, 2); QVERIFY(module.Reveal(fresh));
    QCOMPARE(code->textCursor().blockNumber(), 1);
    project.sourceChangeSuspected_ = true;
    QVERIFY(!module.Reveal(fresh));
    project.sourceChangeSuspected_ = false;
    QVERIFY(project.editRevision_.Reset(project.editRevision_.Current()).Succeeded());
    QVERIFY(!module.Reveal(fresh));
}

void NativeLuaCodeTests::RuntimeLineNavigationWaitsForTextAndCancels() {
    QTemporaryDir root; QtProject::EditorProject project; Install(project, root.path().toStdWString(), true, 1, true);
    Modules::TriggerModule module(project); (void)module.Widget(); QVERIFY(module.ShowGlobalCustomCode());
    const auto target = RuntimeLine(project, project.triggerDocument_->triggers.front().id, 3);
    QVERIFY(module.Reveal(target));
    auto* code = Code(module); QVERIFY(code && code->IsLoading());
    QTRY_VERIFY_WITH_TIMEOUT(!code->IsLoading(), 15000);
    QCOMPARE(code->textCursor().blockNumber(), 2);
    QVERIFY(module.ShowGlobalCustomCode());
    QVERIFY(module.Reveal(target)); QVERIFY(code->IsLoading());
    module.CancelNavigationRestoration();
    QTRY_VERIFY_WITH_TIMEOUT(!code->IsLoading(), 15000);
    QCOMPARE(code->textCursor().position(), 0);
    QCOMPARE(project.triggerUndoStack_->count(), 0);
}

void NativeLuaCodeTests::RuntimeLineNavigationRejectsDisplayLineChanges() {
    QTemporaryDir root; QtProject::EditorProject project; Install(project, root.path().toStdWString(), true);
    auto& trigger = project.triggerDocument_->triggers.front();
    // Lua把U+2028保留在这一行注释中；Qt会改变其展示分行，不能声称定位到原行。
    trigger.customText = Trigger::RawText{"-- source" "\xE2\x80\xA8" "comment\nfunction InitTrig_OriginalLua()\nend\n"};
    const auto original = trigger.customText.text;
    Modules::TriggerModule module(project); (void)module.Widget(); QVERIFY(module.ShowGlobalCustomCode());
    QVERIFY(!module.Reveal(RuntimeLine(project, trigger.id, 2)));
    QVERIFY(Code(module)->isReadOnly());
    QCOMPARE(Code(module)->textCursor().position(), 0);
    QCOMPARE(trigger.customText.text, original);
    QCOMPARE(project.triggerUndoStack_->count(), 0);
}

void NativeLuaCodeTests::RuntimeLineNavigationPreservesPropertyDrafts() {
    QTemporaryDir root; QtProject::EditorProject project; Install(project, root.path().toStdWString(), true);
    Modules::TriggerModule module(project); auto* page = module.Widget();
    const auto triggerId = project.triggerDocument_->triggers.front().id;
    const auto target = RuntimeLine(project, triggerId, 2); QVERIFY(module.Reveal(target));
    auto* comment = page->findChild<QPlainTextEdit*>("triggerCommentEdit"); QVERIFY(comment);
    const auto before = project.triggerDocument_->triggers.front().description.text;
    const auto revision = project.editRevision_.Revision();
    const auto undo = project.triggerUndoStack_->count();
    comment->setPlainText(QStringLiteral("作者仍在编辑的触发器说明"));
    const auto draft = comment->toPlainText(); const auto cursor = Code(module)->textCursor().position();
    QVERIFY(!module.HasPendingCodeDraft()); QVERIFY(module.HasPendingNavigationDraft());
    QVERIFY(!module.Reveal(target));
    QCOMPARE(comment->toPlainText(), draft); QCOMPARE(Code(module)->textCursor().position(), cursor);
    QCOMPARE(project.triggerDocument_->triggers.front().description.text, before);
    QCOMPARE(project.triggerUndoStack_->count(), undo); QCOMPARE(project.editRevision_.Revision(), revision);
    comment->setPlainText(QString::fromUtf8(before));
    QVERIFY(!module.HasPendingNavigationDraft()); QVERIFY(module.Reveal(target));
}

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    QApplication application(argc, argv);
    NativeLuaCodeTests tests;
    return QTest::qExec(&tests, argc, argv);
}
void NativeLuaCodeTests::CodeBlockCompletionSeesOtherBlocks() {
    QTemporaryDir root; QtProject::EditorProject project; Install(project, root.path().toStdWString(), true);
    Modules::TriggerModule module(project); module.Widget()->resize(900, 600); module.Widget()->show();
    QVERIFY(module.CreateCustomTrigger(QStringLiteral("Probe")));
    auto* code = Code(module); QVERIFY(code && !code->isReadOnly()); QCOMPARE(code->Language(), UiKit::CodeLanguage::Lua);
    auto* popup = code->findChild<UiKit::CodeCompletionPopup*>(); QVERIFY(popup);
    code->setFocus(); code->moveCursor(QTextCursor::End);
    // 头部定义的全局与其它代码块里的函数都在候选里。
    QTest::keyClicks(code, "NativeC");
    QTRY_VERIFY(popup->isVisible()); QCOMPARE(popup->Current()->label, QStringLiteral("NativeCounter"));
    QTest::keyClick(code, Qt::Key_Escape);
    QTest::keyClick(code, Qt::Key_Return); QTest::keyClicks(code, "InitTrig_Ori");
    QTRY_VERIFY(popup->isVisible()); QCOMPARE(popup->Current()->label, QStringLiteral("InitTrig_OriginalLua"));
    QTest::keyClick(code, Qt::Key_Escape);
    // 换成 JASS 地图的代码块后不再用 Lua 分析。
    QTemporaryDir jassRoot; QtProject::EditorProject jassProject; Install(jassProject, jassRoot.path().toStdWString(), true, 0);
    Modules::TriggerModule jassModule(jassProject); QVERIFY(jassModule.ShowGlobalCustomCode());
    QVERIFY(Code(jassModule)->Language() != UiKit::CodeLanguage::Lua);
}

#include "NativeLuaCodeTests.moc"
