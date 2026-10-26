// NQ020 原创 GUI 触发器引用同步；不使用作者地图，不启动游戏。
#include "EditorProject.h"
#include "TriggerCommands.h"
#include "TriggerModule.h"
#include "TriggerModels.h"
#include "MapSaveJob.h"
#include "LiveEditorTools.h"
#include "ProjectAuxiliaryEdits.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/MapData/MapSettings.h"
#include "War3MapCore/Mpq/MpqWriter.h"
#include "War3MapCore/Services/ClassicLuaMapSources.h"
#include "War3MapCore/Services/MapSaveService.h"
#include "War3MapCore/Services/ScriptSavePreparation.h"
#include "War3MapCore/Script/MapScript.h"
#include "War3MapCore/Script/ScriptSaveBundle.h"
#include "War3MapCore/Trigger/TriggerCodec.h"
#include "War3MapCore/Trigger/TriggerGraphEditing.h"
#include "War3MapCore/Workspace.h"
#include "../MapDataFixtures.h"
#include "../AuxiliarySaveFixtures.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSortFilterProxyModel>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeView>
#include <QUndoStack>

#include <algorithm>
#include <bit>
#include <filesystem>
#include <initializer_list>
#include <stdexcept>

using namespace War3Map;
using namespace War3Map::Trigger;
using War3Map::QtProject::EditorProject;

namespace {
std::vector<std::byte> Bytes(std::string_view text) {
    const auto* p = reinterpret_cast<const std::byte*>(text.data()); return {p, p + text.size()};
}
struct Wire {
    std::vector<std::byte> data;
    void Int(std::int32_t n) { for (int bit = 0; bit < 32; bit += 8) data.push_back(static_cast<std::byte>((static_cast<std::uint32_t>(n) >> bit) & 255)); }
    void Text(std::string_view text) { auto b = Bytes(text); data.insert(data.end(), b.begin(), b.end()); data.push_back(std::byte{}); }
};
std::vector<std::byte> Wtg(bool custom, bool opaque, std::string_view functionName = "RunTrigger") {
    Wire w; w.data = Bytes("WTG!"); w.Int(7); w.Int(1); w.Int(1); w.Text("Original GUI"); w.Int(0);
    w.Int(2); w.Int(0); w.Int(3);
    const auto trigger = [&](std::string_view name, bool code, int roots) {
        w.Text(name); w.Text("original description\r\nunchanged"); w.Int(0); w.Int(1); w.Int(code); w.Int(0); w.Int(0); w.Int(1); w.Int(roots);
    };
    trigger("Init", custom, 0); trigger("Other", false, 0); trigger("Caller", false, opaque ? 3 : 2);
    for (int i = 0; i < 2; ++i) {
        w.Int(2); w.Text(functionName); w.Int(1);
        w.Int(1); w.Text("gg_trg_Init"); w.Int(0); w.Int(0); w.Int(0);
    }
    if (opaque) {
        w.Int(2); w.Text("CustomScriptCode"); w.Int(1);
        w.Int(3); w.Text("call DoNothing()"); w.Int(0); w.Int(0); w.Int(0);
    }
    return std::move(w.data);
}
std::vector<std::byte> Wct(std::string_view header, bool custom) {
    Wire w; w.Int(1); w.Text("");
    const auto code = [&](std::string_view text) { w.Int(text.empty() ? 0 : static_cast<std::int32_t>(text.size() + 1)); if (!text.empty()) w.Text(text); };
    code(header); w.Int(3);
    code(custom ? "function InitTrig_Init takes nothing returns nothing\nendfunction\n" : ""); code(""); code("");
    return std::move(w.data);
}
void Require(bool value, const std::string& why) { if (!value) throw std::runtime_error(why); }
std::string Explain(const auto& result) {
    std::string text;
    for (const auto& d : result.diagnostics) text += d.code + ": " + d.message + "\n";
    return text;
}
template<class T> T Take(Result<T>&& result) { Require(result.Succeeded() && result.value.has_value(), Explain(result)); return std::move(*result.value); }
bool Write(const std::filesystem::path& path, std::span<const std::byte> bytes) {
    QDir().mkpath(QString::fromStdWString(path.parent_path().wstring()));
    QFile file(QString::fromStdWString(path.wstring()));
    return file.open(QIODevice::WriteOnly) && file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<qint64>(bytes.size())) == static_cast<qint64>(bytes.size());
}
bool Write(const std::filesystem::path& path, std::string_view text) { return Write(path, Bytes(text)); }
const TriggerRecord* Named(const EditorProject& project, std::string_view name) {
    const auto& rows = project.triggerDocument_->triggers;
    const auto it = std::find_if(rows.begin(), rows.end(), [&](const auto& row) { return row.name.text == name; });
    return it == rows.end() ? nullptr : &*it;
}
struct Fixture {
    QTemporaryDir temp;
    EditorProject project;
    std::unique_ptr<Modules::TriggerModule> module;
    std::unique_ptr<QtApp::LiveEditorTools> tools;
    std::filesystem::path root, ui;
    EncodedTriggerDocument original;
    StableId target = 0;
    QString error;
    bool Initialize(std::string_view header = {}, bool custom = false, bool opaque = false,
        const std::filesystem::path& readonlyUiRoot = {}) {
        try {
            Require(temp.isValid(), "owned test directory"); root = std::filesystem::path(temp.path().toStdWString());
            if (readonlyUiRoot.empty()) {
                ui = root / "catalog";
                Require(Write(ui / "ui/config", "base\n") && Write(ui / "ui/base/define.txt", "[TriggerTypes]\ntrigger=0,1,1,Trigger\nstring=0,1,1,String,string\nscriptcode=0,1,1,Code,string\n")
                    && Write(ui / "ui/base/action.txt", "[RunTrigger]\nscript_name=TriggerExecute\ndescription=Run ${target}\n[[.args]]\ntype=trigger\n[CustomScriptCode]\ndescription=Code ${code}\n[[.args]]\ntype=scriptcode\n"), "owned UI metadata");
            } else {
                Require(readonlyUiRoot.is_absolute(), "explicit readonly UI root");
                ui = UiCatalog::ResolveUiRoot(readonlyUiRoot); // 不写 config/元数据；实际层序从最初的 seal 建立。
            }
            auto catalog = Take(UiCatalog::Load(ui));
            auto document = Take(WtgWctCodec{}.Parse(Wtg(custom, opaque, readonlyUiRoot.empty() ? "RunTrigger" : "TriggerExecute"), Wct(header, custom), catalog));
            original = Take(WtgWctCodec{}.Serialize(document, catalog));
            Require(Write(root / "source/map/war3map.wtg", original.wtg) && Write(root / "source/map/war3map.wct", original.wct), "owned source bytes");
            auto source = Take(DirectoryWorkspace::Open(root / "source"));
            project.projectRoot_ = root / "source";
            project.directorySnapshot_ = std::make_unique<DirectoryWorkspace>(std::move(source));
            project.uiDataRoot_ = project.activeUiDataRoot_ = ui;
            project.uiCatalog_ = std::make_unique<UiCatalog>(std::move(catalog));
            project.triggerDocument_ = std::make_unique<TriggerDocument>(std::move(document));
            target = Named(project, "Init")->id;
            project.triggerUndoStack_ = new QUndoStack(&project);
            QObject::connect(project.triggerUndoStack_, &QUndoStack::indexChanged, &project, [&] { project.ObserveTriggerEdit(); });
            project.ResetEditTracking(); Require(project.editTrackingReady_ && !project.editTrackingFailed_, "owned edit baseline");
            module = std::make_unique<Modules::TriggerModule>(project); (void)module->Widget();
            tools = std::make_unique<QtApp::LiveEditorTools>(project); return true;
        } catch (const std::exception& e) { error = QString::fromUtf8(e.what()); return false; }
    }
    EncodedTriggerDocument Pair() const { return Take(WtgWctCodec{}.Serialize(*project.triggerDocument_, *project.uiCatalog_)); }
    bool OriginalBytes() const { const auto p = Pair(); return p.wtg == original.wtg && p.wct == original.wct; }
    bool SourceBytes() const {
        const auto read = [](const auto& path) { QFile f(QString::fromStdWString(path.wstring())); return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray{}; };
        return read(root / "source/map/war3map.wtg") == QByteArray(reinterpret_cast<const char*>(original.wtg.data()), static_cast<qsizetype>(original.wtg.size()))
            && read(root / "source/map/war3map.wct") == QByteArray(reinterpret_cast<const char*>(original.wct.data()), static_cast<qsizetype>(original.wct.size()));
    }
    bool RenameThroughModel(const QString& name) {
        auto* view = module->Widget()->findChild<QTreeView*>("triggerExplorer");
        auto* proxy = view ? qobject_cast<QSortFilterProxyModel*>(view->model()) : nullptr;
        auto* model = proxy ? dynamic_cast<TriggerExplorerModel*>(proxy->sourceModel()) : nullptr;
        return model && proxy->setData(proxy->mapFromSource(model->IndexForTriggerId(target)), name, Qt::EditRole);
    }
    bool ReferencesEqual(std::string_view name) const {
        const auto& args = project.triggerDocument_->argumentNodes;
        // 引用存 WE 写法：按名字换算后的脚本全局名。
        const auto symbol = "gg_trg_" + TriggerDirectoryEditor::ScriptName(name);
        return std::count_if(args.begin(), args.end(), [&](const auto& a) { return a.type == 1 && a.value.text == symbol; }) == 2;
    }
    QJsonObject RenameRequest(QString name) const {
        const auto state = tools->Invoke("editor_state", {});
        const auto b = Take(CaptureTriggerGraphBinding(*project.triggerDocument_, *project.uiCatalog_));
        const auto& raw = TriggerDocumentEditor::FindTrigger(*project.triggerDocument_, target)->name;
        return {{"project_identity", state.value("project_identity")}, {"expected_revision", state.value("revision")},
            {"expected", QJsonObject{{"source_wtg_sha256", QString::fromStdString(b.sourceWtgSha256)},
                {"source_wct_sha256", QString::fromStdString(b.sourceWctSha256)}, {"ui_catalog_sha256", QString::fromStdString(b.uiCatalogSha256)},
                {"current_wtg_sha256", QString::fromStdString(b.currentWtgSha256)}, {"current_wct_sha256", QString::fromStdString(b.currentWctSha256)}}},
            {"operations", QJsonArray{QJsonObject{{"op", "rename"}, {"target", QJsonObject{{"kind", "trigger"}, {"id", QString::number(target)}}},
                {"expected_name", QJsonObject{{"text", QString::fromStdString(raw.text)}, {"encoding", "utf8"},
                    {"raw_bytes_hex", QString::fromLatin1(QByteArray(reinterpret_cast<const char*>(raw.originalBytes.data()), static_cast<qsizetype>(raw.originalBytes.size())).toHex())}}},
                {"name", name}}}}};
    }
};
} // namespace

class TriggerRenameTests final : public QObject {
    Q_OBJECT
    QTemporaryDir settings_;
private slots:
    void initTestCase() {
        QVERIFY(settings_.isValid());
        QCoreApplication::setOrganizationName(QStringLiteral("Audastra-NQ020-Tests"));
        QCoreApplication::setApplicationName(QStringLiteral("TriggerRenameTests"));
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_.path());
    }
    void GuiRenameSynchronizesReferencesAndWholeUndo();
    void GuiAndMcpShareOneUndoTransaction();
    void UnsafeCodeAndDuplicateNameRejectWithoutMutation();
    void QueuedStaleSessionAndUndoReject();
    void FrozenGraphIdentityAndCatalogSourceGuardsReject();
    void UnboundIdentityAndRejectedFirstPublicationExposeFailure();
    void ActualSmallMapSaveAndReopen();
    void AuxiliaryCrudClassicLuaSaveAndReopen();
    void AuxiliaryCrudClassicJassSaveAndReopen();
};

void TriggerRenameTests::GuiRenameSynchronizesReferencesAndWholeUndo() {
    Fixture f; QVERIFY2(f.Initialize(), qPrintable(f.error));
    auto* identity = f.project.triggerDocument_.get();
    QVERIFY(f.RenameThroughModel(QStringLiteral("新的 波次")));
    QTRY_VERIFY(Named(f.project, "新的 波次")); QCOMPARE(f.project.triggerUndoStack_->count(), 1);
    QVERIFY(f.ReferencesEqual("新的 波次")); QCOMPARE(f.project.triggerDocument_.get(), identity);
    const auto edited = f.Pair(); QVERIFY(edited.wct == f.original.wct); QVERIFY(f.SourceBytes());
    Script::MapScriptInputs inputs; inputs.triggers = f.project.triggerDocument_.get(); inputs.catalog = f.project.uiCatalog_.get();
    const auto script = Script::GenerateMapScript(inputs); QVERIFY2(script.Succeeded(), Explain(script).c_str());
    const auto symbol = "gg_trg_" + TriggerDirectoryEditor::ScriptName("新的 波次");
    QVERIFY(script.text.find("call TriggerExecute(" + symbol + ")") != std::string::npos);
    QVERIFY(script.text.find("gg_trg_Init") == std::string::npos);
    f.project.triggerUndoStack_->undo(); QVERIFY(f.OriginalBytes()); QVERIFY(f.SourceBytes()); QVERIFY(f.ReferencesEqual("Init"));
    f.project.triggerUndoStack_->redo(); const auto again = f.Pair(); QVERIFY(again.wtg == edited.wtg && again.wct == edited.wct);
    QVERIFY(f.SourceBytes()); QCOMPARE(f.project.triggerDocument_.get(), identity);
}

void TriggerRenameTests::GuiAndMcpShareOneUndoTransaction() {
    Fixture f; QVERIFY2(f.Initialize(), qPrintable(f.error));
    QVERIFY(f.RenameThroughModel(QStringLiteral("GuiName"))); QTRY_VERIFY(Named(f.project, "GuiName"));
    const auto gui = f.Pair();
    const auto response = f.tools->Invoke("editor_edit_trigger_graph", f.RenameRequest(QStringLiteral("McpName")));
    QVERIFY2(response.value("ok").toBool(), qPrintable(QString::fromUtf8(QJsonDocument(response).toJson(QJsonDocument::Compact))));
    QCOMPARE(f.project.triggerUndoStack_->count(), 2); QVERIFY(f.ReferencesEqual("McpName"));
    f.project.triggerUndoStack_->undo(); const auto first = f.Pair(); QVERIFY(first.wtg == gui.wtg && first.wct == gui.wct);
    f.project.triggerUndoStack_->undo(); QVERIFY(f.OriginalBytes());
    f.project.triggerUndoStack_->redo(); f.project.triggerUndoStack_->redo(); QVERIFY(f.ReferencesEqual("McpName")); QVERIFY(f.SourceBytes());
}

void TriggerRenameTests::UnsafeCodeAndDuplicateNameRejectWithoutMutation() {
    for (int kind = 0; kind < 4; ++kind) {
        Fixture f; QVERIFY2(f.Initialize(kind == 1 ? "// opaque author header\r\n" : "", kind == 2, kind == 3), qPrintable(f.error));
        const auto fingerprint = f.project.ProjectFingerprint();
        const bool accepted = f.RenameThroughModel(kind == 0 ? QStringLiteral("Other") : QStringLiteral("Changed"));
        if (kind == 0 || kind == 2) QVERIFY(!accepted);
        else { QVERIFY(accepted); QCoreApplication::processEvents(); }
        QCOMPARE(f.project.triggerUndoStack_->count(), 0); QVERIFY(f.OriginalBytes()); QVERIFY(f.ReferencesEqual("Init"));
        QCOMPARE(f.project.ProjectFingerprint(), fingerprint); QVERIFY(f.SourceBytes());
        if (kind == 1 || kind == 3) {
            const auto response = f.tools->Invoke("editor_edit_trigger_graph", f.RenameRequest(QStringLiteral("Changed")));
            QVERIFY(!response.value("ok").toBool());
            QVERIFY(QString::fromUtf8(QJsonDocument(response).toJson()).contains(QStringLiteral("TRIGGER_RENAME_CODE_AUDIT_REQUIRED")));
            QCOMPARE(f.project.triggerUndoStack_->count(), 0); QVERIFY(f.OriginalBytes());
        }
    }
}

void TriggerRenameTests::QueuedStaleSessionAndUndoReject() {
    Fixture f; QVERIFY2(f.Initialize(), qPrintable(f.error));
    QVERIFY(f.RenameThroughModel(QStringLiteral("StaleName")));
    f.project.ResetEditTracking(); QCoreApplication::processEvents();
    QVERIFY(f.OriginalBytes()); QCOMPARE(f.project.triggerUndoStack_->count(), 0);
    QVERIFY(f.RenameThroughModel(QStringLiteral("GoodName"))); QTRY_VERIFY(Named(f.project, "GoodName"));
    const auto saved = f.Pair();
    f.project.ResetEditTracking(); f.project.triggerUndoStack_->undo();
    const auto untouched = f.Pair(); QVERIFY(saved.wtg == untouched.wtg && saved.wct == untouched.wct); QVERIFY(f.SourceBytes());
}

void TriggerRenameTests::FrozenGraphIdentityAndCatalogSourceGuardsReject() {
    for (int kind = 0; kind < 7; ++kind) {
        Fixture f; QVERIFY2(f.Initialize(), qPrintable(f.error));
        QVERIFY(f.RenameThroughModel(QStringLiteral("GoodName"))); QTRY_VERIFY(Named(f.project, "GoodName"));
        const UiCatalog originalCatalog(*f.project.uiCatalog_);
        const auto identity = Take(CaptureTriggerGraphReplayIdentity(*f.project.triggerDocument_, originalCatalog));
        const auto originalPayload = f.Pair();
        if (kind == 0) std::swap(f.project.triggerDocument_->argumentNodes[0].id, f.project.triggerDocument_->argumentNodes[1].id);
        if (kind == 1) f.project.triggerDocument_->argumentNodes[0].value.encoding = TextEncoding::Latin1;
        if (kind == 2) ++f.project.triggerDocument_->argumentNodes[0].origin.offset;
        if (kind == 3) {
            QVERIFY(Write(f.ui / "ui/base/action.txt", "[RunTrigger]\nscript_name=TriggerExecute\ntitle=New presentation\ndescription=Run ${target}\n[[.args]]\ntype=trigger\n[CustomScriptCode]\ndescription=Code ${code}\n[[.args]]\ntype=scriptcode\n"));
            *f.project.uiCatalog_ = Take(UiCatalog::Load(f.ui)); // 指针不变、真实元数据 SHA 改变。
        }
        if (kind == 4) f.project.uiCatalog_ = std::make_unique<UiCatalog>(originalCatalog); // 内容相同、会话指针不同。
        if (kind == 5) f.project.sourceChangeSuspected_ = true;
        if (kind == 6) f.project.sourceChangeConfirmed_ = true;
        const auto mutated = Take(WtgWctCodec{}.Serialize(*f.project.triggerDocument_, originalCatalog));
        QVERIFY(mutated.wtg == originalPayload.wtg && mutated.wct == originalPayload.wct);
        if (kind <= 2) QVERIFY(!VerifyTriggerGraphReplayIdentity(*f.project.triggerDocument_, originalCatalog, identity).Succeeded());
        const auto id0 = f.project.triggerDocument_->argumentNodes[0].id;
        const auto encoding0 = f.project.triggerDocument_->argumentNodes[0].value.encoding;
        const auto origin0 = f.project.triggerDocument_->argumentNodes[0].origin.offset;
        f.project.triggerUndoStack_->undo(); // 拒绝旧事务；不能覆盖同载荷的 IR 元信息。
        const auto after = Take(WtgWctCodec{}.Serialize(*f.project.triggerDocument_, originalCatalog));
        QVERIFY(after.wtg == mutated.wtg && after.wct == mutated.wct);
        QCOMPARE(f.project.triggerDocument_->argumentNodes[0].id, id0);
        QCOMPARE(f.project.triggerDocument_->argumentNodes[0].value.encoding, encoding0);
        QCOMPARE(f.project.triggerDocument_->argumentNodes[0].origin.offset, origin0);
        QVERIFY(f.SourceBytes()); QVERIFY(Named(f.project, "GoodName"));
    }
    Fixture f; QVERIFY2(f.Initialize(), qPrintable(f.error));
    f.project.sourceChangeSuspected_ = true;
    QVERIFY(!f.RenameThroughModel(QStringLiteral("Denied")));
    QCOMPARE(f.project.triggerUndoStack_->count(), 0); QVERIFY(f.OriginalBytes()); QVERIFY(f.SourceBytes());

    // MCP 写入会真实核验来源：只有磁盘确实改变，不能把人工疑似标志当成永久拒绝。
    const auto request = f.RenameRequest(QStringLiteral("Denied"));
    const auto sourcePath = f.root / "source/map/war3map.wct";
    const auto originalSourceSha = Take(Sha256File(sourcePath));
    auto externalBytes = f.original.wct; externalBytes.push_back(std::byte{0x23});
    QVERIFY(Write(sourcePath, externalBytes));
    const auto changedSourceSha = Take(Sha256File(sourcePath));
    QVERIFY(changedSourceSha != originalSourceSha);
    const auto fingerprint = f.project.ProjectFingerprint();
    const auto response = f.tools->Invoke("editor_edit_trigger_graph", request);
    QVERIFY(!response.value("ok").toBool());
    const auto diagnostics = response.value("diagnostics").toArray();
    QVERIFY(std::any_of(diagnostics.begin(), diagnostics.end(), [](const auto& d) {
        return d.toObject().value("code").toString() == QStringLiteral("SOURCE_CHANGED");
    }));
    QVERIFY(f.project.sourceChangeSuspected_ && f.project.sourceChangeConfirmed_);
    QCOMPARE(f.project.triggerUndoStack_->count(), 0); QVERIFY(f.OriginalBytes()); QVERIFY(f.ReferencesEqual("Init"));
    QCOMPARE(f.project.ProjectFingerprint(), fingerprint);
    QCOMPARE(Take(Sha256File(sourcePath)), changedSourceSha); // 不覆盖外部改变后的 owned 源。

    Fixture clean; QVERIFY2(clean.Initialize(), qPrintable(clean.error));
    clean.project.sourceChangeSuspected_ = true;
    const auto accepted = clean.tools->Invoke("editor_edit_trigger_graph", clean.RenameRequest(QStringLiteral("VerifiedName")));
    QVERIFY2(accepted.value("ok").toBool(), qPrintable(QString::fromUtf8(QJsonDocument(accepted).toJson(QJsonDocument::Compact))));
    QVERIFY(!clean.project.sourceChangeSuspected_ && !clean.project.sourceChangeConfirmed_);
    QCOMPARE(clean.project.triggerUndoStack_->count(), 1); QVERIFY(clean.ReferencesEqual("VerifiedName")); QVERIFY(clean.SourceBytes());
    clean.project.triggerUndoStack_->undo(); QVERIFY(clean.OriginalBytes()); QVERIFY(clean.ReferencesEqual("Init")); QVERIFY(clean.SourceBytes());
}

void TriggerRenameTests::UnboundIdentityAndRejectedFirstPublicationExposeFailure() {
    Fixture f; QVERIFY2(f.Initialize(), qPrintable(f.error));
    QVERIFY(!VerifyTriggerGraphReplayIdentity(*f.project.triggerDocument_, *f.project.uiCatalog_, {}).Succeeded());
    TriggerDocument before(*f.project.triggerDocument_), invalid(before);
    invalid.ecaNodes.front().arguments.clear(); // 原函数要求一个参数；实际 codec 拒绝此候选。
    auto command = std::make_unique<QtProject::TriggerGraphSnapshotUndoCommand>(f.project, std::move(before), std::move(invalid), QStringLiteral("Denied test candidate"));
    QVERIFY(!command->Ready()); const auto published = command->FirstPublication();
    QVERIFY(!*published);
    f.project.triggerUndoStack_->push(command.release()); // obsolete 命令可被删，Outcome 独立保活。
    QVERIFY(!*published); QCOMPARE(f.project.triggerUndoStack_->count(), 0); QVERIFY(f.OriginalBytes()); QVERIFY(f.SourceBytes());
}

void TriggerRenameTests::ActualSmallMapSaveAndReopen() {
    const auto outputText = qEnvironmentVariable("FORGE_TRIGGER_RENAME_REAL_ROOT");
    if (outputText.isEmpty()) QSKIP("Set FORGE_TRIGGER_RENAME_REAL_ROOT for a single owned tiny-map save; no game executed.");
    try {
        const auto output = std::filesystem::path(outputText.toStdWString());
        const auto toolRoot = std::filesystem::path(qEnvironmentVariable("FORGE_YDWE_ROOT").toStdWString());
        const auto gameRoot = std::filesystem::path(qEnvironmentVariable("FORGE_WAR3_ROOT").toStdWString());
        Require(output.is_absolute() && toolRoot.is_absolute() && gameRoot.is_absolute(), "explicit absolute output/tool/game roots");
        Require(std::filesystem::create_directory(output), "unique output must not already exist; refusing prior receipt overwrite");
        const auto readonlyUi = UiCatalog::ResolveUiRoot(toolRoot);
        const auto configSha = Take(Sha256File(readonlyUi / "config"));
        const auto macroSha = Take(Sha256File(readonlyUi / "ydwe/jass/AntiBJLeak/MainMacro.h"));
        Fixture f; const bool initialized = f.Initialize({}, false, false, toolRoot);
        Require(initialized, f.error.toStdString());
        Require(f.ui == readonlyUi && std::find(f.project.uiCatalog_->Layers().begin(), f.project.uiCatalog_->Layers().end(), "ydwe")
            != f.project.uiCatalog_->Layers().end(), "actual readonly UI catalog and enabled ydwe include layer are bound from initial parse");
        MapData::MapSettings settings; settings.formatVersion = 25; settings.tileset = 'L'; settings.mapName = "Original NQ020 tiny fixture"; settings.custom.emplace();
        const auto settingBytes = Take(settings.Serialize());
        const auto source = output / "rename-source.w3x", candidate = output / "rename-candidate.w3x";
        const std::vector<MapArchiveMember> members{{"war3map.w3i", settingBytes}, {"war3map.wtg", f.original.wtg}, {"war3map.wct", f.original.wct},
            {"war3mapUnits.doo", MapDataFixtures::EmptyUnitsV8Sub11()}, {"war3map.doo", MapDataFixtures::EmptyDoodadsV8Sub11()},
            {"war3map.j", Bytes("function config takes nothing returns nothing\nendfunction\nfunction main takes nothing returns nothing\nendfunction\n")},
            {"unchanged.bin", Bytes("Original opaque unrelated member")}};
        (void)Take(WriteVerifiedArchive(source, members)); auto archive = Take(MapArchiveSession::Open(source));
        const auto beforeSha = Take(Sha256File(source));
        f.project.archiveSession_ = std::make_unique<MapArchiveSession>(archive);
        f.project.sourceArchivePath_ = source; f.project.compilerDataRoot_ = toolRoot; f.project.war3GameRoot_ = gameRoot;
        f.project.ResetEditTracking();
        Require(f.RenameThroughModel(QStringLiteral("RenamedInit")), "actual UI rename queued");
        QTRY_VERIFY(Named(f.project, "RenamedInit")); Require(f.ReferencesEqual("RenamedInit"), "both actual references renamed");
        const auto edited = f.Pair();
        auto job = f.project.PrepareMapSave(candidate, false);
        Require(job && job->script && !job->scriptSkipped, "production project save preparation did not bind script rebuild");
        auto& task = *job;
        struct Evidence {
            Services::MapSaveTask& task;
            ~Evidence() noexcept { try { qInfo().noquote() << "script_workspace=" << QString::fromStdWString(task.scriptWorkspace.wstring()); } catch (...) {} }
        } evidence{task};
        task.beforePublish = [&, previous = std::move(task.beforePublish)] {
            if (previous) { const auto status = previous(); if (!status.Succeeded()) return status; }
            return archive.VerifySourceUnchanged();
        };
        EditorProject::RunMapSave(task);
        Require(task.executed && task.succeeded && task.scriptRebuilt && !task.scriptSkipped && task.nextSession, Explain(task));
        auto reopened = Take(MapArchiveSession::Open(candidate));
        const auto wtg = Take(reopened.ReadMember("war3map.wtg")), wct = Take(reopened.ReadMember("war3map.wct"));
        Require(wtg == edited.wtg && wct == edited.wct && wct == f.original.wct, "exact saved edited WTG and original WCT");
        const auto doc = Take(WtgWctCodec{}.Parse(wtg, wct, *f.project.uiCatalog_));
        Require(doc.triggers[0].name.text == "RenamedInit" && std::count_if(doc.argumentNodes.begin(), doc.argumentNodes.end(),
            [](const auto& a) { return a.type == 1 && a.value.text == "gg_trg_RenamedInit"; }) == 2, "independent reopened name and both references");
        const auto script = Take(reopened.ReadMember("war3map.j")); const std::string text(reinterpret_cast<const char*>(script.data()), script.size());
        Require(text.find("gg_trg_Init") == std::string::npos && text.find("call TriggerExecute(gg_trg_RenamedInit)") != std::string::npos,
            "actual compiled script uses exactly the renamed symbol");
        Require(Take(reopened.ReadMember("unchanged.bin")) == members.back().data && Take(Sha256File(source)) == beforeSha
            && archive.VerifySourceUnchanged().Succeeded() && f.SourceBytes(), "actual original sources and unrelated member unchanged");
        const auto committed = f.project.CommitMapSave(task);
        Require(committed && committed->scriptRebuilt && f.project.triggerUndoStack_->isClean(), "production own-save commits same graph and clean point");
        const auto candidateSha = Take(Sha256File(candidate));
        f.project.triggerUndoStack_->undo(); Require(f.OriginalBytes() && f.ReferencesEqual("Init"), "normal own-save preserves graph seal for whole transaction Undo");
        f.project.triggerUndoStack_->redo(); const auto replayed = f.Pair();
        Require(replayed.wtg == edited.wtg && replayed.wct == edited.wct && Take(Sha256File(candidate)) == candidateSha,
            "normal own-save Undo/Redo changes only current overlay, not saved candidate");
        Require(Take(Sha256File(readonlyUi / "config")) == configSha
            && Take(Sha256File(readonlyUi / "ydwe/jass/AntiBJLeak/MainMacro.h")) == macroSha,
            "real UI config and compiler include dependency remained readonly");
        Services::FinalizeScriptWorkspace(task);
        qInfo().noquote() << "source_sha256=" << QString::fromStdString(beforeSha)
            << "candidate_sha256=" << QString::fromStdString(reopened.SourceSha256()) << "script_sha256=" << QString::fromStdString(Take(Sha256(script)))
            << "actual_save_reopen=true script_rebuilt=true source_unchanged=true own_save_undo_redo=true game_executed=false";
    } catch (const std::exception& e) { QFAIL(e.what()); }
}

void TriggerRenameTests::AuxiliaryCrudClassicLuaSaveAndReopen() {
    try {
        // 默认执行的原创专项。经典 Lua 档案只用内嵌 Lua/JASS 检查，不进入 YDWE、模板或外部回退。
        QTemporaryDir evidence(QDir::tempPath() + "/audastra-s6-classic-lua-XXXXXX");
        Require(evidence.isValid(), "owned S6 evidence directory");
        const auto root = std::filesystem::path(evidence.path().toStdWString());
        qInfo().noquote() << "s6_evidence_root=" << evidence.path();
        const auto ui = root / "ui", workspace = root / "workspace";
        Require(Write(ui / "config", "base\n")
            && Write(ui / "base/define.txt", "[TriggerTypes]\ninteger=0,1,1,Integer,integer\n[TriggerParams]\n"), "owned S6 UI metadata");
        for (const auto* name : {"action.txt", "call.txt", "condition.txt", "event.txt"})
            Require(Write(ui / "base" / name, ""), "owned empty S6 UI definitions");
        auto catalog = Take(UiCatalog::Load(ui));
        const auto empty = Take(WtgWctCodec{}.CreateEmpty(catalog));
        const auto pair = Take(WtgWctCodec{}.Serialize(empty, catalog));
        auto triggers = Take(WtgWctCodec{}.Parse(pair.wtg, pair.wct, catalog));
        Require(triggers.HasTrustedSourceSeal(), "original empty T has a real codec source seal");

        // 沿用 services-classic-lua-map-save 的原创声明方式；不复制游戏基础库，也不执行空函数。
        const auto declarations = AuxiliarySaveFixtures::OriginalDeclarations();
        const auto& profile = declarations.profile;
        // Original game-data source remains separate from the map.
        const auto gameRoot = root / "game", gameArchive = gameRoot / "war3.mpq";
        Require(std::filesystem::create_directory(gameRoot), "owned original game-data directory");
        const auto gameFiles = AuxiliarySaveFixtures::GameFiles(declarations);
        const auto gameWritten = Mpq::WriteMpqArchive(gameArchive, gameFiles);
        Require(gameWritten.Succeeded(), Explain(gameWritten));
        const auto gameSha = Take(Sha256File(gameArchive));
        const auto profilePath = root / "classic-profile.tsv", configuration = root / "classic-save.tsv";
        const auto profileUtf8 = profilePath.u8string();
        Require(Write(profilePath, profile) && Write(configuration, "schema\tforge-lua-map-save-v1\nprofile\t"
            + std::string(profileUtf8.begin(), profileUtf8.end()) + '\t' + Take(Sha256Text(profile))
            + "\nplayer_layout\tclassic12\nlanguage\tpreserve\ndialect\tclassic-127-ydwe\n"), "owned explicit classic Lua configuration");

        const auto source = root / "auxiliary-source.w3x", candidate = root / "auxiliary-candidate.w3x";
        const auto members = AuxiliarySaveFixtures::SourceMembers(pair);
        (void)Take(WriteVerifiedArchive(source, members)); auto archive = Take(MapArchiveSession::Open(source));
        const auto sourceSha = Take(Sha256File(source)), profileSha = Take(Sha256File(profilePath)), configSha = Take(Sha256File(configuration));
        Require(!archive.HasMember("war3map.w3r") && !archive.HasMember("war3map.w3c"), "S6 members originally absent");
        Require(Write(workspace / "map/war3map.wtg", pair.wtg) && Write(workspace / "map/war3map.wct", pair.wct), "owned original T directory");
        EditorProject project;
        project.projectRoot_ = workspace; project.sourceArchivePath_ = source;
        project.directorySnapshot_ = std::make_unique<DirectoryWorkspace>(Take(DirectoryWorkspace::Open(workspace)));
        project.archiveSession_ = std::make_unique<MapArchiveSession>(archive);
        project.uiDataRoot_ = project.activeUiDataRoot_ = ui;
        project.war3GameRoot_ = gameRoot;
        project.uiCatalog_ = std::make_unique<UiCatalog>(std::move(catalog));
        project.triggerDocument_ = std::make_unique<TriggerDocument>(std::move(triggers));
        project.triggerUndoStack_ = new QUndoStack(&project);
        project.LoadMapDocument(); project.ResetEditTracking();
        Require(project.mapEdits_ && project.mapDataUndoStack_ && project.editTrackingReady_ && !project.editTrackingFailed_, "normal original S6 map session");
        Require(project.ConfigureLuaMapTarget(configuration).Succeeded(), "normal explicit classic Lua target selection");
        Require(project.luaMapTarget_ && project.luaMapTarget_->dialect == Script::LuaMapSaveDialect::Classic127YDWE
            && !project.luaMapTarget_->checker.Identity() && project.compilerDataRoot_.empty() && project.war3GameRoot_ == gameRoot, "no external checker or tool; game data is the original owned fixture");
        const auto before = project.mapEdits_->Snapshot();
        const auto request = [&](MapDataKind kind, Services::AuxiliaryEditAction action, std::size_t row = 0) {
            const auto current = project.mapEdits_->Snapshot();
            Services::AuxiliaryEditRequest value; value.kind = kind; value.action = action;
            value.expectedMemberSha256 = Take(Services::AuxiliaryEditing::MemberIdentity(current, kind));
            if (action != Services::AuxiliaryEditAction::Create) {
                value.sourceIndex = row; value.expectedRowKey = Take(MapData::MapFieldRowKey(current, kind, row));
            }
            return value;
        };
        const auto apply = [&](Services::AuxiliaryEditRequest value) {
            const auto binding = Take(QtProject::CaptureAuxiliaryEditBinding(project));
            const auto result = Take(QtProject::ApplyAuxiliaryEditWithResult(project, binding, std::move(value), QStringLiteral("S6原创结构命令")));
            Require(result.changed && result.receipt.CommandIdentity() != 0, "real project auxiliary command and receipt");
        };
        MapData::RegionRecord region{-128, -64, 256, 512, "DraftRegion", 10, {}, "", {1, 2, 3, 177}};
        auto create = request(MapDataKind::Regions, Services::AuxiliaryEditAction::Create); create.record = region; apply(create);
        region.name = "KeptRegion"; region.top = 768;
        auto update = request(MapDataKind::Regions, Services::AuxiliaryEditAction::Update); update.record = region; apply(update);
        auto copiedRegion = region; copiedRegion.name = "RemovedRegion"; copiedRegion.creationNumber = 11;
        create = request(MapDataKind::Regions, Services::AuxiliaryEditAction::Create); create.record = copiedRegion; apply(create);
        apply(request(MapDataKind::Regions, Services::AuxiliaryEditAction::Delete, 1));
        MapData::CameraRecord camera{}; camera.name = "DraftCamera"; camera.targetX = 16; camera.targetY = 32;
        camera.zOffset = 30; camera.rotation = 90; camera.angleOfAttack = 300; camera.targetDistance = 1500;
        camera.fieldOfView = 70; camera.farClippingPlane = 5000; camera.nearClippingPlane = -0.0f;
        create = request(MapDataKind::Cameras, Services::AuxiliaryEditAction::Create); create.record = camera; apply(create);
        camera.name = "KeptCamera"; camera.rotation = 45; camera.targetY = 96;
        update = request(MapDataKind::Cameras, Services::AuxiliaryEditAction::Update); update.record = camera; apply(update);
        auto copiedCamera = camera; copiedCamera.name = "RemovedCamera";
        create = request(MapDataKind::Cameras, Services::AuxiliaryEditAction::Create); create.record = copiedCamera; apply(create);
        apply(request(MapDataKind::Cameras, Services::AuxiliaryEditAction::Delete, 1));
        const auto edited = project.mapEdits_->Snapshot();
        Require(project.mapDataUndoStack_->count() == 8 && project.triggerUndoStack_->count() == 0, "eight shared structural commands, original T unchanged");
        for (int index = 0; index < 8; ++index) project.mapDataUndoStack_->undo();
        Require(Services::AuxiliaryEditing::AreEquivalentMapSources(project.mapEdits_->Snapshot(), before), "whole Undo restores both absent members");
        for (int index = 0; index < 8; ++index) project.mapDataUndoStack_->redo();
        Require(Services::AuxiliaryEditing::AreEquivalentMapSources(project.mapEdits_->Snapshot(), edited), "whole Redo restores exact final native bytes");

        // 直接合并原创模板成员后走正常保存服务，验证最终guard拒绝覆盖且不提交作者草稿。
        auto alternateRegions = *edited.Regions(); alternateRegions.entries[0].top += 128;
        auto alternateCameras = *edited.Cameras(); alternateCameras.entries[0].rotation += 45;
        for (const auto& replacement : std::array{
            MapArchiveMember{"WAR3MAP.W3R", Take(alternateRegions.Serialize())},
            MapArchiveMember{"WAR3MAP.W3C", Take(alternateCameras.Serialize())}}) {
            const auto rejectedPath = root / (replacement.name + ".rejected.w3x");
            auto rejected = project.PrepareMapSave(rejectedPath, false);
            Require(rejected && rejected->script && rejected->beforePublish, "actual Qt S6 save freezes a final output guard");
            std::vector<MemberReplacement> imports{replacement};
            Require(Script::MergeTemplateImportMembers(rejected->replacements, rejected->removals, imports).Succeeded(),
                "normal template merge can still name S6 members");
            EditorProject::RunMapSave(*rejected);
            Require(rejected->executed && !rejected->succeeded && !rejected->nextSession
                && std::any_of(rejected->diagnostics.begin(), rejected->diagnostics.end(), [](const auto& diagnostic) {
                    return diagnostic.code == "SAVE_AUXILIARY_OUTPUT_MISMATCH";
                }) && !std::filesystem::exists(rejectedPath), "template replacement must fail before candidate publication");
            Require(!project.CommitMapSave(*rejected) && project.sourceArchivePath_ == source && project.mapEdits_->IsDirty()
                && project.mapDataUndoStack_->count() == 8 && project.mapDataUndoStack_->index() == 8
                && Services::AuxiliaryEditing::AreEquivalentMapSources(project.mapEdits_->Snapshot(), edited)
                && Take(Sha256File(source)) == sourceSha && archive.VerifySourceUnchanged().Succeeded(),
                "rejected template save preserves source, dirty bytes and original Undo history");
        }
        auto job = project.PrepareMapSave(candidate, false);
        Require(job && job->script && !job->scriptSkipped && job->script->luaTarget
            && job->script->luaTarget->Profile()->dialect == Script::LuaMapSaveDialect::Classic127YDWE
            && !job->script->luaTarget->Profile()->checker.Identity() && job->script->toolchain.toolRoot.empty()
            && !job->script->toolchain.layout && !job->script->toolchain.frameworkInputs && job->script->game
            && job->script->game->Root() == gameRoot, "normal complete save is frozen to embedded checks and the original game-data source");
        QStringList stages; job->progress = [&](const QString& stage) { stages.append(stage); };
        job->beforePublish = [&, previous = std::move(job->beforePublish)] {
            if (previous) { const auto status = previous(); if (!status.Succeeded()) return status; }
            return archive.VerifySourceUnchanged();
        };
        EditorProject::RunMapSave(*job);
        Require(job->executed && job->succeeded && job->scriptRebuilt && !job->scriptSkipped && job->nextSession, Explain(*job));
        const auto checked = std::find_if(job->diagnostics.begin(), job->diagnostics.end(), [](const auto& diagnostic) {
            return diagnostic.code == "SCRIPT_REBUILT"
                && diagnostic.message.find("内置经典 Lua 原生入口 JASS 语法与类型检查") != std::string::npos;
        });
        Require(checked != job->diagnostics.end(), "actual save receipt must identify the embedded classic Lua JASS checker");
        const auto checkReceipt = checked->message;
        auto reopened = Take(MapArchiveSession::Open(candidate));
        const auto w3r = Take(reopened.ReadMember("war3map.w3r")), w3c = Take(reopened.ReadMember("war3map.w3c"));
        Require(w3r == edited.Member(MapDataKind::Regions).sources.front().data
            && w3c == edited.Member(MapDataKind::Cameras).sources.front().data, "independent MPQ reopen matches both edited native members exactly");
        const auto regions = Take(MapData::ParseRegions(w3r));
        const auto cameras = Take(MapData::ParseCameras(w3c));
        Require(regions.entries.size() == 1 && regions.entries[0].name == "KeptRegion" && regions.entries[0].top == 768
            && regions.entries[0].creationNumber == 10 && regions.entries[0].colorBgra[3] == 177, "independent region parser preserves final fields");
        Require(cameras.entries.size() == 1 && cameras.entries[0].name == "KeptCamera" && cameras.entries[0].rotation == 45
            && cameras.entries[0].targetY == 96 && std::bit_cast<std::uint32_t>(cameras.entries[0].nearClippingPlane) == 0x80000000u, "independent camera parser preserves final fields and reserved negative zero");
        const auto lua = Take(reopened.ReadMember("audastra\\map.lua"));
        const std::string text(reinterpret_cast<const char*>(lua.data()), lua.size());
        for (const auto* expected : {"function CreateRegions()", "gg_rct_KeptRegion = Rect(-128.0, -64.0, 256.0, 768.0)",
                "function CreateCameras()", "gg_cam_KeptCamera = CreateCameraSetup()",
                "CameraSetupSetField(gg_cam_KeptCamera, CAMERA_FIELD_ROTATION, 45.0, 0.0)",
                "CameraSetupSetDestPosition(gg_cam_KeptCamera, 16.0, 96.0, 0.0)", "    CreateRegions()", "    CreateCameras()"})
            Require(text.find(expected) != std::string::npos, std::string("complete rebuilt initialization lacks: ") + expected);
        for (const auto* absent : {"DraftRegion", "RemovedRegion", "DraftCamera", "RemovedCamera"})
            Require(text.find(absent) == std::string::npos, "deleted and renamed symbols absent from rebuilt initialization");
        const auto candidateSha = Take(Sha256File(candidate));
        Require(Services::VerifyClassicLuaGeneratedSources(reopened, candidateSha).Succeeded(), "all seven actual generated script members match the strict manifest");
        const auto manifest = Take(reopened.ReadMember("audastra\\generated.manifest")), entry = Take(reopened.ReadMember("war3map.j"));
        Require(!manifest.empty() && entry != members[members.size() - 2].data, "new manifest and real rebuilt JASS entry");
        for (const auto& member : members) if (member.name != "war3map.j")
            Require(Take(reopened.ReadMember(member.name)) == member.data, "original unrelated member or T changed: " + member.name);
        Require(Take(Sha256File(source)) == sourceSha && archive.VerifySourceUnchanged().Succeeded()
            && Take(Sha256File(gameArchive)) == gameSha
            && Take(Sha256File(profilePath)) == profileSha && Take(Sha256File(configuration)) == configSha, "source archive and explicit profile/config remain unchanged");
        const auto committed = project.CommitMapSave(*job);
        Require(committed && committed->scriptRebuilt && project.sourceArchivePath_ == candidate
            && !project.mapEdits_->IsDirty(), "normal CommitMapSave establishes saved S6 native baseline");
        Require(Services::AuxiliaryEditing::AreEquivalentMapSources(project.mapEdits_->Snapshot(), edited)
            && Take(Sha256File(candidate)) == candidateSha && Take(Sha256File(source)) == sourceSha, "commit preserves final auxiliary bytes and both archive identities");
        for (int index = 0; index < 8; ++index) project.mapDataUndoStack_->undo();
        Require(Services::AuxiliaryEditing::AreEquivalentMapSources(project.mapEdits_->Snapshot(), before)
            && project.mapEdits_->IsDirty(), "saved S6 history still restores the original absent members");
        for (int index = 0; index < 8; ++index) project.mapDataUndoStack_->redo();
        Require(Services::AuxiliaryEditing::AreEquivalentMapSources(project.mapEdits_->Snapshot(), edited)
            && !project.mapEdits_->IsDirty() && Take(Sha256File(candidate)) == candidateSha
            && Take(Sha256File(source)) == sourceSha && Take(Sha256File(gameArchive)) == gameSha, "saved S6 Undo/Redo returns to the saved baseline without touching either archive or game data");
        qInfo().noquote() << "s6_source_sha256=" << QString::fromStdString(sourceSha)
            << "candidate_sha256=" << QString::fromStdString(candidateSha) << "game_fixture_sha256=" << QString::fromStdString(gameSha)
            << "w3r_sha256=" << QString::fromStdString(Take(Sha256(w3r)))
            << "w3c_sha256=" << QString::fromStdString(Take(Sha256(w3c))) << "map_lua_sha256=" << QString::fromStdString(Take(Sha256(lua)))
            << "manifest_sha256=" << QString::fromStdString(Take(Sha256(manifest)))
            << "check_receipt=" << QString::fromStdString(checkReceipt) << "stages=" << stages.join(" | ")
            << "classic_lua_save_reopen=true embedded_lua_jass_checks=true source_unchanged=true ydwe_executed=false classic_jass_pipeline_verified=false game_executed=false";
    } catch (const std::exception& e) { QFAIL(e.what()); }
}

void TriggerRenameTests::AuxiliaryCrudClassicJassSaveAndReopen() {
    try {
        QTemporaryDir temporary(QDir::tempPath() + "/audastra-s6-classic-jass-XXXXXX");
        Require(temporary.isValid(), "owned S6 JASS directory");
        auto root = std::filesystem::path(temporary.path().toStdWString());
        const auto retained = qEnvironmentVariable("FORGE_S6_QT_SAVE_ROOT");
        if (!retained.isEmpty()) {
            root = std::filesystem::path(retained.toStdWString()).lexically_normal();
            Require(root.is_absolute() && std::filesystem::create_directory(root), "retained S6 root must be new and absolute");
        }
        const auto input = AuxiliarySaveFixtures::CreateOriginalInputs(root);
        auto archive = Take(MapArchiveSession::Open(input.source));
        const auto sourceSha = Take(Sha256File(input.source)), gameSha = Take(Sha256File(input.game / "war3.mpq"));
        const auto commonPath = input.tool / "compiler/jass/24/common.j";
        const auto commonBytes = AuxiliarySaveFixtures::JassDeclarations().common;
        const auto commonSha = Take(Sha256File(commonPath));
        const auto configSha = Take(Sha256File(input.ui / "config"));
        auto catalog = Take(UiCatalog::Load(input.ui));
        auto triggers = Take(WtgWctCodec{}.Parse(input.pair.wtg, input.pair.wct, catalog));
        Require(triggers.HasTrustedSourceSeal() && triggers.triggers.size() == 1
            && triggers.triggers.front().customTextFlag == 0 && triggers.wct.customCode.text.empty(), "real editable T source with no custom code");
        EditorProject project;
        project.projectRoot_ = input.workspace; project.sourceArchivePath_ = input.source;
        project.directorySnapshot_ = std::make_unique<DirectoryWorkspace>(Take(DirectoryWorkspace::Open(input.workspace)));
        project.archiveSession_ = std::make_unique<MapArchiveSession>(archive);
        project.uiDataRoot_ = project.activeUiDataRoot_ = input.ui;
        project.compilerDataRoot_ = input.tool; project.war3GameRoot_ = input.game;
        project.uiCatalog_ = std::make_unique<UiCatalog>(std::move(catalog));
        project.triggerDocument_ = std::make_unique<TriggerDocument>(std::move(triggers));
        project.triggerUndoStack_ = new QUndoStack(&project);
        project.LoadMapDocument(); project.ResetEditTracking();
        Require(project.mapEdits_ && project.mapDataUndoStack_ && project.editTrackingReady_ && !project.editTrackingFailed_, "original S6 JASS map session");
        const auto original = project.mapEdits_->Snapshot();
        const auto request = [&](MapDataKind kind, Services::AuxiliaryEditAction action, std::size_t row = 0) {
            const auto current = project.mapEdits_->Snapshot();
            Services::AuxiliaryEditRequest value; value.kind = kind; value.action = action;
            value.expectedMemberSha256 = Take(Services::AuxiliaryEditing::MemberIdentity(current, kind));
            if (action != Services::AuxiliaryEditAction::Create) {
                value.sourceIndex = row; value.expectedRowKey = Take(MapData::MapFieldRowKey(current, kind, row));
            }
            return value;
        };
        const auto apply = [&](Services::AuxiliaryEditRequest value) {
            const auto binding = Take(QtProject::CaptureAuxiliaryEditBinding(project));
            const auto changed = Take(QtProject::ApplyAuxiliaryEditWithResult(project, binding, std::move(value), QStringLiteral("S6原创JASS结构命令")));
            Require(changed.changed && changed.receipt.CommandIdentity() != 0, "real JASS project command receipt");
        };
        const auto staleBinding = Take(QtProject::CaptureAuxiliaryEditBinding(project));
        MapData::RegionRecord region{-128, -64, 256, 512, "DraftRegion", 10, {}, "", {1, 2, 3, 177}};
        auto command = request(MapDataKind::Regions, Services::AuxiliaryEditAction::Create); command.record = region; apply(command);
        auto staleRequest = request(MapDataKind::Regions, Services::AuxiliaryEditAction::Update); staleRequest.record = region;
        const auto stale = QtProject::ApplyAuxiliaryEditWithResult(project, staleBinding, staleRequest, QStringLiteral("Stale revision"));
        Require(!stale.Succeeded() && std::any_of(stale.diagnostics.begin(), stale.diagnostics.end(), [](const auto& row) { return row.code == "AUX_PROJECT_STALE"; })
            && project.mapDataUndoStack_->count() == 1 && project.mapDataUndoStack_->index() == 1 && project.mapEdits_->IsDirty(), "stale revision adds no Undo command");
        auto wrongSource = Take(QtProject::CaptureAuxiliaryEditBinding(project)); wrongSource.sourceArchive = root / "another-source.w3x";
        const auto staleSource = QtProject::ApplyAuxiliaryEditWithResult(project, wrongSource, staleRequest, QStringLiteral("Stale source"));
        Require(!staleSource.Succeeded() && project.mapDataUndoStack_->count() == 1
            && project.mapEdits_->Snapshot().Regions()->entries.front().name == "DraftRegion", "another source binding cannot mutate the original command");
        region.name = "KeptRegion"; region.top = 768;
        command = request(MapDataKind::Regions, Services::AuxiliaryEditAction::Update); command.record = region; apply(command);
        auto removedRegion = region; removedRegion.name = "RemovedRegion"; removedRegion.creationNumber = 11;
        command = request(MapDataKind::Regions, Services::AuxiliaryEditAction::Create); command.record = removedRegion; apply(command);
        apply(request(MapDataKind::Regions, Services::AuxiliaryEditAction::Delete, 1));
        MapData::CameraRecord camera{}; camera.name = "DraftCamera"; camera.targetX = 16; camera.targetY = 32;
        camera.zOffset = 30; camera.rotation = 90; camera.angleOfAttack = 300; camera.targetDistance = 1500;
        camera.fieldOfView = 70; camera.farClippingPlane = 5000; camera.nearClippingPlane = -0.0f;
        command = request(MapDataKind::Cameras, Services::AuxiliaryEditAction::Create); command.record = camera; apply(command);
        camera.name = "KeptCamera"; camera.rotation = 45; camera.targetY = 96;
        command = request(MapDataKind::Cameras, Services::AuxiliaryEditAction::Update); command.record = camera; apply(command);
        auto removedCamera = camera; removedCamera.name = "RemovedCamera";
        command = request(MapDataKind::Cameras, Services::AuxiliaryEditAction::Create); command.record = removedCamera; apply(command);
        apply(request(MapDataKind::Cameras, Services::AuxiliaryEditAction::Delete, 1));
        const auto edited = project.mapEdits_->Snapshot();
        const auto stable = [&] {
            return project.sourceArchivePath_ == input.source && project.mapEdits_->IsDirty()
                && project.mapDataUndoStack_->count() == 8 && project.mapDataUndoStack_->index() == 8
                && project.triggerUndoStack_->count() == 0
                && Services::AuxiliaryEditing::AreEquivalentMapSources(project.mapEdits_->Snapshot(), edited)
                && Take(Sha256File(input.source)) == sourceSha && Take(Sha256File(input.game / "war3.mpq")) == gameSha;
        };
        Require(stable(), "eight shared JASS structural commands preserve T/source/game");
        const auto deniedBinding = Take(QtProject::CaptureAuxiliaryEditBinding(project));
        command = request(MapDataKind::Cameras, Services::AuxiliaryEditAction::Update); auto deniedCamera = camera; deniedCamera.roll = 99; command.record = deniedCamera;
        const auto denied = QtProject::ApplyAuxiliaryEditWithResult(project, deniedBinding, command, QStringLiteral("Denied publication"),
            [](const auto&, const auto&, const auto&) { return Status::Failure("Test", "S6_PUBLICATION_DENIED", "原创发布拒绝"); });
        Require(!denied.Succeeded() && stable(), "failed public edit preserves dirty/native bytes and the eight Undo commands");
        for (int index = 0; index < 8; ++index) project.mapDataUndoStack_->undo();
        Require(Services::AuxiliaryEditing::AreEquivalentMapSources(project.mapEdits_->Snapshot(), original), "JASS whole Undo restores missing members");
        for (int index = 0; index < 8; ++index) project.mapDataUndoStack_->redo();
        Require(stable(), "JASS whole Redo preserves the real save snapshot");

        const auto rejectedPath = root / "dependency-stale.w3x";
        auto rejected = project.PrepareMapSave(rejectedPath, false);
        Require(rejected && rejected->script && rejected->script->toolchain.layout && !rejected->scriptSkipped, "normal JASS preparation freezes complete tool layout");
        Require(Write(commonPath, commonBytes + "// changed original dependency\n"), "change only the owned tool fixture after preparation");
        EditorProject::RunMapSave(*rejected);
        Require(rejected->executed && !rejected->succeeded && !rejected->nextSession && !std::filesystem::exists(rejectedPath)
            && std::any_of(rejected->diagnostics.begin(), rejected->diagnostics.end(), [](const auto& row) { return row.code == "SCRIPT_LAYOUT_STALE"; })
            && !project.CommitMapSave(*rejected) && stable(), "dependency-stale save leaves source, dirty bytes and Undo unchanged");
        Require(Write(commonPath, commonBytes) && Take(Sha256File(commonPath)) == commonSha, "restore only the original test dependency for a fresh save");
        const auto candidate = root / "candidate.w3x";
        auto job = project.PrepareMapSave(candidate, false);
        Require(job && job->script && !job->scriptSkipped && !job->script->luaTarget && job->script->toolchain.layout
            && job->script->toolchain.preprocessor == Script::ScriptEngine::Builtin
            && job->script->toolchain.vjass == Script::ScriptEngine::Builtin && job->script->toolchain.checker == Script::ScriptEngine::Builtin
            && !job->script->toolchain.crossCheck && job->script->toolchain.layout->waveExecutable.empty()
            && job->script->toolchain.layout->jassHelperExecutable.empty() && job->script->toolchain.layout->pjassExecutable.empty(), "normal builtin-only original toolchain has no external executable");
        QStringList stages; job->progress = [&](const QString& stage) { stages.append(stage); };
        EditorProject::RunMapSave(*job);
        Require(job->executed && job->succeeded && job->scriptRebuilt && !job->scriptSkipped && job->nextSession, Explain(*job));
        const auto checked = std::find_if(job->diagnostics.begin(), job->diagnostics.end(), [](const auto& row) {
            return row.code == "SCRIPT_REBUILT" && row.message.find("内置 vJass 翻译与语法检查") != std::string::npos;
        });
        Require(checked != job->diagnostics.end(), "actual JASS save receipt records builtin translation and checking");
        const auto checkReceipt = checked->message;
        auto reopened = Take(MapArchiveSession::Open(candidate));
        AuxiliarySaveFixtures::VerifyNativeMembers(reopened, edited);
        const auto jass = AuxiliarySaveFixtures::VerifyJassInitialization(reopened);
        for (const auto& member : input.members) if (member.name != "war3map.j")
            Require(Take(reopened.ReadMember(member.name)) == member.data, "JASS save changed original unrelated member/T: " + member.name);
        const auto candidateSha = Take(Sha256File(candidate));
        Require(stable() && archive.VerifySourceUnchanged().Succeeded() && Take(Sha256File(input.ui / "config")) == configSha, "JASS save preserves original inputs before commit");
        Require(project.CommitMapSave(*job).has_value() && !project.mapEdits_->IsDirty()
            && project.sourceArchivePath_ == candidate && project.mapDataUndoStack_->count() == 8, "normal JASS commit sets the real saved native baseline");
        for (int index = 0; index < 8; ++index) project.mapDataUndoStack_->undo();
        Require(project.mapEdits_->IsDirty() && Services::AuxiliaryEditing::AreEquivalentMapSources(project.mapEdits_->Snapshot(), original), "post-save JASS Undo preserves whole history");
        for (int index = 0; index < 8; ++index) project.mapDataUndoStack_->redo();
        Require(!project.mapEdits_->IsDirty() && Services::AuxiliaryEditing::AreEquivalentMapSources(project.mapEdits_->Snapshot(), edited)
            && Take(Sha256File(candidate)) == candidateSha && Take(Sha256File(input.source)) == sourceSha
            && Take(Sha256File(input.game / "war3.mpq")) == gameSha, "post-save JASS Redo changes no archive or game fixture");
        Services::FinalizeScriptWorkspace(*job);
        qInfo().noquote() << "s6_jass_root=" << QString::fromStdWString(root.wstring())
            << "source_sha256=" << QString::fromStdString(sourceSha) << "candidate_sha256=" << QString::fromStdString(candidateSha)
            << "game_fixture_sha256=" << QString::fromStdString(gameSha) << "script_sha256=" << QString::fromStdString(Take(Sha256Text(jass)))
            << "check_receipt=" << QString::fromStdString(checkReceipt) << "stages=" << stages.join(" | ")
            << "original_jass_template_save_reopen=true real_ydwe_environment_verified=false external_tools_executed=false game_executed=false";
    } catch (const std::exception& error) { QFAIL(error.what()); }
}

QTEST_MAIN(TriggerRenameTests)
#include "TriggerRenameTests.moc"
