#include "EditorProject.h"
#include "TriggerModule.h"
#include "Widgets/CodeView.h"
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
void Install(QtProject::EditorProject& project, const std::filesystem::path& root, bool archive, int language = 1) {
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
}
UiKit::CodeView* Code(Modules::TriggerModule& module) { return qobject_cast<UiKit::CodeView*>(module.CodeEditor()); }
}

// 全程 offscreen / 原创微型 fixture；语法检查编译不执行，未启动游戏或外部编译器。
class NativeLuaCodeTests final : public QObject {
    Q_OBJECT
private slots:
    void SourceArchiveAndDirectorySelectLuaAndUndo();
    void LuaCheckingDoesNotExecuteAndDraftFailuresPreserveText();
    void OriginalSourceSettingsIdentityBlocksReboundDraft();
    void EditedLanguageOrManualComboDoesNotGrantNativeAuthority();
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

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    QApplication application(argc, argv);
    NativeLuaCodeTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "NativeLuaCodeTests.moc"
